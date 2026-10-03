#include "forge/sim/rigid.h"

#include "forge/core/jobs.h"
#include "forge/core/log.h"
#include "forge/core/profile.h"
#include "forge/core/time.h"
#include "forge/sim/cells.h"
#include "forge/sim/gravity.h"
#include "forge/sim/tiles.h"
#include "forge/sim/zones.h"

#include <box2d/box2d.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

FORGE_REFLECT(forge::sim::RigidBody, 1) {
    t.field("shape", &forge::sim::RigidBody::shape);
    t.field("half_w", &forge::sim::RigidBody::half_w);
    t.field("half_h", &forge::sim::RigidBody::half_h);
    t.field("density", &forge::sim::RigidBody::density);
    t.field("friction", &forge::sim::RigidBody::friction);
    t.field("bounce", &forge::sim::RigidBody::bounce);
    t.field("gravity", &forge::sim::RigidBody::gravity);
    t.field("fixed_rotation", &forge::sim::RigidBody::fixed_rotation);
    t.field("angle", &forge::sim::RigidBody::angle);
    t.field("vx", &forge::sim::RigidBody::vx);
    t.field("vy", &forge::sim::RigidBody::vy);
    t.field("spin", &forge::sim::RigidBody::spin);
}

namespace forge::sim {

using world::ChunkCoord;
using world::kChunkSize;

namespace {

// Box2D tasks on the engine's job system.
struct B2Task {
    JobCounter counter;
    b2TaskCallback* fn;
    void* context;
    i32 count;
    i32 per;
};

void run_b2_range(void* data, u32 index) {
    auto* t = static_cast<B2Task*>(data);
    const i32 begin = static_cast<i32>(index) * t->per;
    const i32 end = std::min(t->count, begin + t->per);
    t->fn(begin, end, jobs::this_thread_index(), t->context);
}

void* enqueue_b2(b2TaskCallback* fn, int count, int min_range, void* context, void*) {
    const i32 threads = static_cast<i32>(jobs::thread_count());
    const i32 per = std::max(min_range, (count + threads * 2 - 1) / (threads * 2));
    const i32 n = per > 0 ? (count + per - 1) / per : 0;
    if (n <= 1) {
        // Small: run it right here; Box2D then does not call finish.
        fn(0, count, jobs::this_thread_index(), context);
        return nullptr;
    }
    auto* t = new B2Task{{}, fn, context, count, per};
    std::vector<Job> list(static_cast<usize>(n));
    for (i32 i = 0; i < n; ++i) list[static_cast<usize>(i)] = {&run_b2_range, t, static_cast<u32>(i)};
    jobs::submit(list.data(), static_cast<u32>(n), &t->counter);
    return t;
}

void finish_b2(void* task, void*) {
    if (!task) return;
    auto* t = static_cast<B2Task*>(task);
    jobs::wait(t->counter);
    delete t;
}

// Loaded area this far from the origin moves the origin (and rebuilds Box2D).
constexpr f64 kRecenterTiles = 8192;

} // namespace

struct RigidWorld::Impl {
    struct TileChunk {
        b2BodyId body = b2_nullBodyId;
        u64 rows[kChunkSize] = {}; // solid tiles, a bit per tile
        u32 revision = ~0u;
        u32 boxes = 0;
        u64 seen = 0;
        bool built = false;
    };

    scene::Scene& scene;
    const CollisionRules& rules;
    b2WorldId world = b2_nullWorldId;
    i64 origin_x = 0, origin_y = 0; // tiles
    bool has_origin = false;
    struct Live {
        b2BodyId id;
        f32 half_w, half_h, area;
    };
    std::unordered_map<flecs::entity_t, Live> live;
    std::unordered_map<ChunkCoord, TileChunk, world::ChunkCoordHash> tile_chunks;
    std::unordered_set<ChunkCoord, world::ChunkCoordHash> under, need;
    flecs::query<scene::Position, RigidBody> query;
    flecs::observer removed;
    u64 frame = 0;

    Impl(scene::Scene& s, const CollisionRules& r) : scene(s), rules(r) {
        query = scene.ecs().query<scene::Position, RigidBody>();
        // Deleted or unloaded entities (Scene packs them first, with the state
        // written back at the end of the last tick) leave Box2D too.
        removed = scene.ecs().observer<RigidBody>().event(flecs::OnRemove).each([this](flecs::entity e, RigidBody&) {
            auto it = live.find(e.id());
            if (it == live.end()) return;
            b2DestroyBody(it->second.id);
            live.erase(it);
        });
    }

    ~Impl() {
        removed.destruct();
        query = {};
        destroy_world();
    }

    void create_world() {
        b2WorldDef def = b2DefaultWorldDef();
        def.gravity = {0, 0};
        def.workerCount = static_cast<int>(jobs::thread_count());
        def.enqueueTask = &enqueue_b2;
        def.finishTask = &finish_b2;
        world = b2CreateWorld(&def);
    }

    void destroy_world() {
        if (B2_IS_NULL(world)) return;
        b2DestroyWorld(world);
        world = b2_nullWorldId;
        live.clear();
        tile_chunks.clear();
    }

    b2Vec2 local(f64 tile_x, f64 tile_y) const {
        return {static_cast<f32>(tile_x - static_cast<f64>(origin_x)), static_cast<f32>(tile_y - static_cast<f64>(origin_y))};
    }

    void write_back(flecs::entity_t e, b2BodyId id) {
        flecs::entity ent = scene.ecs().entity(e);
        RigidBody* rb = ent.try_get_mut<RigidBody>();
        scene::Position* p = ent.try_get_mut<scene::Position>();
        if (!rb || !p) return;
        const b2Vec2 pos = b2Body_GetPosition(id);
        const b2Vec2 v = b2Body_GetLinearVelocity(id);
        *p = scene::Position::at_tile(static_cast<f64>(origin_x) + pos.x, static_cast<f64>(origin_y) + pos.y);
        rb->angle = b2Rot_GetAngle(b2Body_GetRotation(id));
        rb->vx = v.x;
        rb->vy = v.y;
        rb->spin = b2Body_GetAngularVelocity(id);
    }

    void create_body(flecs::entity_t e, const scene::Position& p, const RigidBody& rb) {
        b2BodyDef bd = b2DefaultBodyDef();
        bd.type = b2_dynamicBody;
        bd.position = local(p.tile_x(), p.tile_y());
        bd.rotation = b2MakeRot(rb.angle);
        bd.linearVelocity = {rb.vx, rb.vy};
        bd.angularVelocity = rb.spin;
        bd.gravityScale = rb.gravity;
        bd.fixedRotation = rb.fixed_rotation;
        bd.userData = reinterpret_cast<void*>(static_cast<uintptr_t>(e));
        const b2BodyId id = b2CreateBody(world, &bd);
        b2ShapeDef sd = b2DefaultShapeDef();
        sd.density = rb.density;
        sd.material.friction = rb.friction;
        sd.material.restitution = rb.bounce;
        // Balls slow down on their own instead of rolling forever (and keep
        // a resting pile awake).
        if (rb.shape == static_cast<u8>(RigidShape::Circle)) sd.material.rollingResistance = 0.1f;
        sd.enableContactEvents = true;
        if (rb.shape == static_cast<u8>(RigidShape::Circle)) {
            const b2Circle circle{{0, 0}, std::max(rb.half_w, 0.05f)};
            b2CreateCircleShape(id, &sd, &circle);
        } else {
            const b2Polygon box = b2MakeBox(std::max(rb.half_w, 0.05f), std::max(rb.half_h, 0.05f));
            b2CreatePolygonShape(id, &sd, &box);
        }
        const bool circle = rb.shape == static_cast<u8>(RigidShape::Circle);
        const f32 hw = std::max(rb.half_w, 0.05f), hh = circle ? hw : std::max(rb.half_h, 0.05f);
        live[e] = {id, hw, hh, circle ? 3.14159265f * hw * hw : 4.0f * hw * hh};
    }

    // Solid tiles of one chunk as boxes: runs along each row, merged with
    // the same run in the rows below.
    void build_tiles(ChunkCoord c, TileChunk& tc) {
        if (B2_IS_NON_NULL(tc.body)) b2DestroyBody(tc.body);
        tc.body = b2_nullBodyId;
        tc.boxes = 0;
        b2BodyDef bd = b2DefaultBodyDef();
        bd.type = b2_staticBody;
        bd.position = local(static_cast<f64>(c.x) * kChunkSize, static_cast<f64>(c.y) * kChunkSize);
        b2ShapeDef sd = b2DefaultShapeDef();
        sd.material.friction = 0.7f;
        u64 left[kChunkSize];
        std::memcpy(left, tc.rows, sizeof(left));
        for (i32 y = 0; y < kChunkSize; ++y) {
            while (left[y] != 0) {
                // The first run in this row.
                const i32 x0 = std::countr_zero(left[y]);
                const u64 from = left[y] >> x0;
                const i32 len = from == ~0ull ? kChunkSize - x0 : std::countr_one(from);
                const u64 run = (len == 64 ? ~0ull : ((1ull << len) - 1)) << x0;
                i32 y1 = y + 1;
                while (y1 < kChunkSize && (left[y1] & run) == run) {
                    left[y1] &= ~run;
                    ++y1;
                }
                left[y] &= ~run;
                if (B2_IS_NULL(tc.body)) tc.body = b2CreateBody(world, &bd);
                const f32 hw = static_cast<f32>(len) * 0.5f, hh = static_cast<f32>(y1 - y) * 0.5f;
                const b2Polygon box = b2MakeOffsetBox(hw, hh, {static_cast<f32>(x0) + hw, static_cast<f32>(y) + hh}, b2Rot_identity);
                b2CreatePolygonShape(tc.body, &sd, &box);
                ++tc.boxes;
            }
        }
    }

    // Tiles only where bodies are: the chunks under them and their
    // neighbours (a body moves less than a chunk per tick).
    void sync_tiles(world::World& w) {
        FORGE_ZONE_N("Rigid tiles");
        ++frame;
        const u32 layer = std::min(rules.layer(), w.layer_count() - 1);
        const u8* shapes = rules.table();
        under.clear();
        ChunkCoord last{INT32_MIN, INT32_MIN};
        for (const auto& [e, body] : live) {
            const b2Vec2 lp = b2Body_GetPosition(body.id);
            const ChunkCoord c = world::chunk_of(static_cast<i32>(std::floor(static_cast<f64>(origin_x) + lp.x)),
                                                 static_cast<i32>(std::floor(static_cast<f64>(origin_y) + lp.y)));
            if (c != last) under.insert(c); // neighbours often share a chunk
            last = c;
        }
        need.clear();
        for (const ChunkCoord& c : under)
            for (i32 dy = -1; dy <= 1; ++dy)
                for (i32 dx = -1; dx <= 1; ++dx) need.insert({c.x + dx, c.y + dy});
        for (const ChunkCoord& coord : need) {
            world::Chunk* chunk = w.find_chunk(coord);
            if (!chunk || !chunk->tiles) continue;
            TileChunk& tc = tile_chunks[coord];
            tc.seen = frame;
            if (tc.revision == chunk->revision) continue;
            tc.revision = chunk->revision;
            // Liquids change the revision too: rebuild only if solids changed.
            u64 rows[kChunkSize] = {};
            const world::TileId* tiles = chunk->layer(layer);
            for (i32 y = 0; y < kChunkSize; ++y)
                for (i32 x = 0; x < kChunkSize; ++x)
                    if (shapes[tiles[y * kChunkSize + x]] == static_cast<u8>(TileShape::Solid)) rows[y] |= 1ull << x;
            if (tc.built && std::memcmp(rows, tc.rows, sizeof(rows)) == 0) continue;
            std::memcpy(tc.rows, rows, sizeof(rows));
            build_tiles(coord, tc);
            tc.built = true;
        }
        for (auto it = tile_chunks.begin(); it != tile_chunks.end();) {
            if (it->second.seen == frame) {
                ++it;
                continue;
            }
            if (B2_IS_NON_NULL(it->second.body)) b2DestroyBody(it->second.body);
            it = tile_chunks.erase(it);
        }
    }
};

RigidWorld::RigidWorld(scene::Scene& scene, const CollisionRules& rules) : impl_(new Impl(scene, rules)) {
    scene.register_component<RigidBody>();
}

RigidWorld::~RigidWorld() { delete impl_; }

bool RigidWorld::apply_impulse(flecs::entity_t e, f32 ix, f32 iy) {
    auto it = impl_->live.find(e);
    if (it == impl_->live.end()) return false;
    b2Body_ApplyLinearImpulseToCenter(it->second.id, {ix, iy}, true);
    return true;
}

void RigidWorld::step(world::World& world, const Zones& zones, const TileView& tiles, const GravityField& gravity,
                      const CellSim* cells, u64 tick, f32 dt, std::vector<RigidContact>& contacts) {
    FORGE_ZONE_N("Rigid bodies");
    (void)tick;
    Impl& m = *impl_;

    // Gather who should be awake: bodies in Active chunks.
    struct Want {
        flecs::entity_t e;
        scene::Position p;
        RigidBody rb;
        bool active;
    };
    std::vector<Want> want;
    m.query.each([&](flecs::entity e, const scene::Position& p, const RigidBody& rb) {
        want.push_back({e.id(), p, rb, zones.zone_of(p.chunk()) == Zone::Active});
    });
    if (want.empty() && m.live.empty()) {
        stats_ = {};
        return;
    }

    // The origin: near the loaded area, moved (Box2D rebuilt) if it drifts far.
    const world::Rect area = tiles.chunk_rect();
    const f64 cx = (static_cast<f64>(area.x0) + area.x1) * 0.5 * kChunkSize;
    const f64 cy = (static_cast<f64>(area.y0) + area.y1) * 0.5 * kChunkSize;
    if (!m.has_origin || std::fabs(cx - static_cast<f64>(m.origin_x)) > kRecenterTiles ||
        std::fabs(cy - static_cast<f64>(m.origin_y)) > kRecenterTiles) {
        for (const auto& [e, body] : m.live) m.write_back(e, body.id);
        m.destroy_world();
        m.origin_x = static_cast<i64>(std::floor(cx / kChunkSize)) * kChunkSize;
        m.origin_y = static_cast<i64>(std::floor(cy / kChunkSize)) * kChunkSize;
        m.has_origin = true;
        // Positions changed by the write-back: gather again.
        want.clear();
        m.query.each([&](flecs::entity e, const scene::Position& p, const RigidBody& rb) {
            want.push_back({e.id(), p, rb, zones.zone_of(p.chunk()) == Zone::Active});
        });
    }
    if (B2_IS_NULL(m.world)) m.create_world();

    for (const Want& w : want) {
        auto it = m.live.find(w.e);
        if (w.active && it == m.live.end()) {
            m.create_body(w.e, w.p, w.rb);
        } else if (!w.active && it != m.live.end()) {
            // Leaving the awake area: the state stays in the component.
            m.write_back(w.e, it->second.id);
            b2DestroyBody(it->second.id);
            m.live.erase(it);
        }
    }

    m.sync_tiles(world);

    // Gravity: the world's in Box2D, local sources and liquids as forces.
    b2World_SetGravity(m.world, {gravity.world_x(), gravity.world_y()});
    const bool sources = gravity.source_count() > 0;
    for (const auto& [e, body] : m.live) {
        const b2BodyId id = body.id;
        const b2Vec2 lp = b2Body_GetPosition(id);
        const f64 tx = static_cast<f64>(m.origin_x) + lp.x, ty = static_cast<f64>(m.origin_y) + lp.y;
        f32 gx = gravity.world_x(), gy = gravity.world_y();
        if (sources) gravity.at(tx, ty, gx, gy);
        const f32 mass = b2Body_GetMass(id);
        const f32 scale = b2Body_GetGravityScale(id);
        f32 fx = (gx - gravity.world_x()) * mass * scale, fy = (gy - gravity.world_y()) * mass * scale;
        const f32 g = std::sqrt(gx * gx + gy * gy);
        if (cells && g > 0) {
            // How much of it is under liquid: four samples along the pull.
            const f32 nx = gx / g, ny = gy / g;
            const f32 reach = std::fabs(nx) * body.half_w + std::fabs(ny) * body.half_h;
            f32 under = 0;
            u8 kind = 0;
            for (f32 t : {-0.75f, -0.25f, 0.25f, 0.75f}) {
                const u16 cell = tiles.liquid(static_cast<i32>(std::floor(tx + nx * reach * t)),
                                              static_cast<i32>(std::floor(ty + ny * reach * t)));
                if (cell == 0 || liquid_kind(cell) > cells->liquid_count()) continue;
                under += std::min(1.0f, static_cast<f32>(liquid_amount(cell)) / kFull) * 0.25f;
                kind = liquid_kind(cell);
            }
            if (kind != 0) {
                const LiquidKind& lk = cells->liquid(kind);
                // Archimedes: the weight of the liquid pushed aside, against the pull.
                const f32 lift = lk.density * body.area * under * scale;
                fx -= gx * lift;
                fy -= gy * lift;
                const f32 keep = std::max(0.0f, 1.0f - lk.drag * under * dt);
                const b2Vec2 v = b2Body_GetLinearVelocity(id);
                b2Body_SetLinearVelocity(id, {v.x * keep, v.y * keep});
                b2Body_SetAngularVelocity(id, b2Body_GetAngularVelocity(id) * keep);
            }
        }
        if (fx != 0 || fy != 0) b2Body_ApplyForceToCenter(id, {fx, fy}, true);
    }

    const u64 t0 = time_now_ns();
    b2World_Step(m.world, dt, 4);
    stats_.solver_ms = ns_to_ms(time_now_ns() - t0);

    for (const auto& [e, body] : m.live)
        if (b2Body_IsAwake(body.id)) m.write_back(e, body.id);

    const b2ContactEvents events = b2World_GetContactEvents(m.world);
    for (int i = 0; i < events.beginCount; ++i) {
        const b2ContactBeginTouchEvent& ev = events.beginEvents[i];
        if (!b2Shape_IsValid(ev.shapeIdA) || !b2Shape_IsValid(ev.shapeIdB)) continue;
        const auto a = static_cast<flecs::entity_t>(reinterpret_cast<uintptr_t>(b2Body_GetUserData(b2Shape_GetBody(ev.shapeIdA))));
        const auto b = static_cast<flecs::entity_t>(reinterpret_cast<uintptr_t>(b2Body_GetUserData(b2Shape_GetBody(ev.shapeIdB))));
        if (a == 0 && b == 0) continue;
        contacts.push_back(a == 0 ? RigidContact{b, 0} : RigidContact{a, b});
    }

    stats_.live = static_cast<u32>(m.live.size());
    stats_.tile_chunks = static_cast<u32>(m.tile_chunks.size());
    stats_.tile_boxes = 0;
    stats_.awake = static_cast<u32>(b2World_GetAwakeBodyCount(m.world));
    for (const auto& [c, tc] : m.tile_chunks) stats_.tile_boxes += tc.boxes;
}

} // namespace forge::sim
