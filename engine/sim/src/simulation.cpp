#include "forge/sim/simulation.h"

#include "forge/sim/rigid.h"

#include "forge/core/profile.h"
#include "forge/core/time.h"

#include <cmath>

FORGE_REFLECT(forge::sim::KeepAwake, 1) { t.field("radius", &forge::sim::KeepAwake::radius); }
FORGE_REFLECT(forge::sim::Trigger, 1) { t.field("radius", &forge::sim::Trigger::radius); }

namespace forge::sim {

void register_components(scene::Scene& scene) {
    scene.register_component<Body>();
    scene.register_component<KeepAwake>();
    scene.register_component<Trigger>();
    scene.register_component<GravitySource>();
    scene.register_component<TimeBubble>();
    scene.register_component<OutsideTime>();
    scene.register_component<RigidBody>();
}

Simulation::Simulation(world::World& world, scene::Scene& scene, const SimDesc& desc)
    : world_(world),
      scene_(scene),
      desc_(desc),
      clock_(desc.ticks_per_second, desc.max_ticks_per_frame),
      zones_(desc.zones),
      collision_(desc.collision_layer) {
    register_components(scene_);
    gravity_.set_world(desc.gravity_x, desc.gravity_y);
    if (desc.liquid_layer < world_.layer_count()) cells_ = std::make_unique<CellSim>(desc.collision_layer, desc.liquid_layer);
    rigid_ = std::make_unique<RigidWorld>(scene_, collision_);
    bodies_ = scene_.ecs().query<scene::Position, Body>();
    triggers_ = scene_.ecs().query<scene::Position, Trigger>();
    keep_awake_ = scene_.ecs().query<scene::Position, KeepAwake>();
    gravity_sources_ = scene_.ecs().query<scene::Position, GravitySource>();
    time_bubbles_ = scene_.ecs().query<scene::Position, TimeBubble>();
    rewind_ = std::make_unique<Rewind>(world_, scene_, desc.rewind, clock_.step());
    world_.add_listener(this);
}

Simulation::~Simulation() {
    world_.remove_listener(this);
    time_bubbles_ = {};
    rewind_.reset();
}

void Simulation::on_chunk_loaded(world::Chunk& chunk) {
    tiles_dirty_ = true;
    rewind_->on_chunk_loaded(chunk.coord);
}
void Simulation::on_chunk_unloading(world::Chunk& chunk) {
    tiles_dirty_ = true;
    rewind_->on_chunk_unloading(chunk.coord);
}

u32 Simulation::update(f64 frame_seconds, std::span<const world::Rect> focus) {
    FORGE_ZONE_N("Simulation update");
    const SimStats prev = stats_;
    stats_ = {};
    // Counts describe the last tick: keep them through frames that run none
    // (a 144 Hz screen sees two of those for every tick).
    stats_.bodies_moved = prev.bodies_moved;
    stats_.triggers = prev.triggers;
    stats_.cells = prev.cells;
    stats_.rigid = prev.rigid;
    stats_.factory = prev.factory;

    u64 t0 = time_now_ns();
    focus_.assign(focus.begin(), focus.end());
    keep_awake_.each([&](const scene::Position& p, const KeepAwake& k) {
        const f64 x = p.tile_x(), y = p.tile_y();
        focus_.push_back({static_cast<i32>(std::floor(x - k.radius)), static_cast<i32>(std::floor(y - k.radius)),
                          static_cast<i32>(std::floor(x + k.radius)) + 1, static_cast<i32>(std::floor(y + k.radius)) + 1});
    });
    world_.update(focus_);
    if (tiles_dirty_) {
        tiles_.rebuild(world_, collision_, cells_ ? desc_.liquid_layer : ~0u);
        if (cells_) cells_->rebuild(world_, collision_);
        tiles_dirty_ = false;
    }
    const u32 ticks = clock_.advance(frame_seconds);
    zones_.update(world_, focus_, current_tick_);
    stats_.zones = zones_.stats();
    stats_.world_ms = ns_to_ms(time_now_ns() - t0);

    frame_events_.clear();
    stop_ticks_ = false;
    u32 ran = 0;
    for (; ran < ticks && !stop_ticks_; ++ran) tick();
    stop_ticks_ = false;
    stats_.ticks = ran;

    t0 = time_now_ns();
    scene_.update();
    stats_.scene_ms = ns_to_ms(time_now_ns() - t0);

    // Triggers look at where everything ended up this frame, through the
    // fresh spatial index.
    if (ran > 0) {
        t0 = time_now_ns();
        step_triggers();
        stats_.triggers_ms = ns_to_ms(time_now_ns() - t0);
    }
    return ran;
}

void Simulation::tick() {
    std::swap(prev_events_, tick_events_);
    tick_events_.clear();
    gather_gravity();
    gather_time();
    const bool rewinding = rewind_->playing();
    const f32 scale = rewinding ? 0.0f : time_scale_;
    const TickContext ctx{current_tick_++, clock_.step(), zones_,   tiles_,    gravity_,
                          prev_events_,    scene_,        scale,    time_,     rewinding,
                          scene_.ecs().component<OutsideTime>().id()};

    u64 t0 = time_now_ns();
    for (const SystemFn& system : systems_) system(ctx);
    const u64 tc = time_now_ns();
    // Liquids and sand move a whole step at a time: in slow motion they step
    // less often, faster than real time more often.
    const u32 down = cells_down(gravity_.world_x(), gravity_.world_y());
    if (cells_ && down != ~0u && scale > 0) {
        cells_due_ = std::min(cells_due_ + scale, 4.0f);
        while (cells_due_ >= 1.0f) {
            cells_->step(zones_, ctx.tick, down);
            cells_due_ -= 1.0f;
        }
        stats_.cells = cells_->stats();
    }
    const u64 t1 = time_now_ns();
    stats_.cells_ms += ns_to_ms(t1 - tc);
    step_bodies(ctx);
    const u64 tr = time_now_ns();
    if (scale > 0) {
        // Box2D takes steps no longer than a tick.
        const u32 steps = static_cast<u32>(std::ceil(scale));
        for (u32 i = 0; i < steps; ++i)
            rigid_->step(world_, zones_, tiles_, gravity_, cells_.get(), ctx.tick, ctx.dt * scale / static_cast<f32>(steps),
                         tick_events_.rigid);
        stats_.rigid = rigid_->stats();
    }
    const u64 t2 = time_now_ns();
    stats_.rigid_ms += ns_to_ms(t2 - tr);
    if (scale > 0) {
        factory_.tick(ctx.dt * scale);
        stats_.factory = factory_.stats();
    }
    stats_.factory_ms += ns_to_ms(time_now_ns() - t2);
    stats_.systems_ms += ns_to_ms(tc - t0);
    stats_.bodies_ms += ns_to_ms(tr - t1);

    // Time recording, or going back through it.
    const u64 tw = time_now_ns();
    if (rewinding) {
        if (rewind_->play_tick()) {
            rigid_->reset();
            if (cells_)
                for (world::ChunkCoord c : rewind_->restored_chunks())
                    cells_->wake({c.x * world::kChunkSize, c.y * world::kChunkSize, (c.x + 1) * world::kChunkSize,
                                  (c.y + 1) * world::kChunkSize});
        }
    } else {
        rewind_->after_tick(scale, zones_);
    }
    stats_.rewind_ms += ns_to_ms(time_now_ns() - tw);

    frame_events_.contacts.insert(frame_events_.contacts.end(), tick_events_.contacts.begin(), tick_events_.contacts.end());
    frame_events_.triggers.insert(frame_events_.triggers.end(), tick_events_.triggers.begin(), tick_events_.triggers.end());
    frame_events_.rigid.insert(frame_events_.rigid.end(), tick_events_.rigid.begin(), tick_events_.rigid.end());
}

void Simulation::gather_time() {
    placed_bubbles_.clear();
    time_bubbles_.each([&](const scene::Position& p, const TimeBubble& b) {
        if (zones_.zone_of(p.chunk()) != Zone::Asleep) placed_bubbles_.push_back({p.tile_x(), p.tile_y(), b});
    });
    time_.build(placed_bubbles_);
}

void Simulation::gather_gravity() {
    placed_.clear();
    gravity_sources_.each([&](const scene::Position& p, const GravitySource& g) {
        placed_.push_back({p.tile_x(), p.tile_y(), g});
    });
    gravity_.build(placed_);
}

void Simulation::step_bodies(const TickContext& ctx) {
    FORGE_ZONE_N("Bodies");
    // Contact changes are collected per thread and merged in thread order.
    struct alignas(64) PerThread {
        std::vector<ContactEvent> found;
        u32 moved = 0;
    };
    const u32 threads = jobs::thread_count() + 1;
    std::vector<PerThread> per(threads);
    const f32 max_fall = desc_.max_fall;
    const bool uniform = gravity_.source_count() == 0;
    const CellSim* cells = cells_.get();
    each_due(ctx, bodies_, [&](flecs::entity_t e, f32 dt, scene::Position& p, Body& b) {
        f32 gx = gravity_.world_x(), gy = gravity_.world_y();
        if (!uniform) gravity_.at(p.tile_x(), p.tile_y(), gx, gy);
        f32 scale = b.gravity;
        b.liquid = 0;
        if (cells) {
            // In a liquid (at least half a tile of it at the centre): held up and slowed.
            const i32 tx = p.cx * world::kChunkSize + static_cast<i32>(std::floor(p.x));
            const i32 ty = p.cy * world::kChunkSize + static_cast<i32>(std::floor(p.y));
            const u16 cell = tiles_.liquid(tx, ty);
            if (cell != 0 && liquid_amount(cell) >= kFull / 2 && liquid_kind(cell) <= cells->liquid_count()) {
                const LiquidKind& lk = cells->liquid(liquid_kind(cell));
                b.liquid = liquid_kind(cell);
                scale *= 1.0f - lk.buoyancy;
                const f32 keep = std::max(0.0f, 1.0f - lk.drag * dt);
                b.vx *= keep;
                b.vy *= keep;
            }
        }
        const u8 before = b.contacts;
        const u8 after = move_body(p, b, dt, gx * scale, gy * scale, max_fall, tiles_);
        PerThread& mine = per[std::min(jobs::this_thread_index(), threads - 1)];
        ++mine.moved;
        if (after != before) mine.found.push_back({e, static_cast<u8>(after & ~before), static_cast<u8>(before & ~after)});
    });
    u32 total = 0;
    for (PerThread& t : per) {
        total += t.moved;
        tick_events_.contacts.insert(tick_events_.contacts.end(), t.found.begin(), t.found.end());
    }
    stats_.bodies_moved = total;
}

void Simulation::step_triggers() {
    FORGE_ZONE_N("Triggers");
    struct Item {
        flecs::entity_t entity;
        f64 x, y;
        f32 radius;
        bool due;
    };
    std::vector<Item> items;
    triggers_.each([&](flecs::entity e, const scene::Position& p, const Trigger& t) {
        items.push_back({e.id(), p.tile_x(), p.tile_y(), t.radius, zones_.zone_of(p.chunk()) != Zone::Asleep});
    });
    stats_.triggers = static_cast<u32>(items.size());
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.entity < b.entity; });

    // Who is inside each awake trigger now: anything with a position, the
    // trigger itself aside.
    using Pair = std::pair<flecs::entity_t, flecs::entity_t>;
    std::vector<std::vector<Pair>> inside(items.size());
    jobs::parallel_for(static_cast<u32>(items.size()), 16, [&](u32 begin, u32 end) {
        std::vector<flecs::entity_t> found;
        for (u32 i = begin; i < end; ++i) {
            const Item& t = items[i];
            if (!t.due) continue;
            found.clear();
            scene_.query_radius(t.x, t.y, t.radius, found);
            std::sort(found.begin(), found.end());
            for (flecs::entity_t e : found)
                if (e != t.entity) inside[i].push_back({t.entity, e});
        }
    });

    // Asleep triggers keep who was inside them.
    inside_next_.clear();
    usize old = 0;
    for (usize i = 0; i < items.size(); ++i) {
        while (old < inside_.size() && inside_[old].first < items[i].entity) ++old;
        if (items[i].due) {
            inside_next_.insert(inside_next_.end(), inside[i].begin(), inside[i].end());
        } else {
            for (usize k = old; k < inside_.size() && inside_[k].first == items[i].entity; ++k)
                inside_next_.push_back(inside_[k]);
        }
    }

    // Compare with the last tick: new pairs entered, missing pairs left.
    usize a = 0, b = 0;
    while (a < inside_.size() || b < inside_next_.size()) {
        if (b == inside_next_.size() || (a < inside_.size() && inside_[a] < inside_next_[b])) {
            frame_events_.triggers.push_back({inside_[a].first, inside_[a].second, false});
            ++a;
        } else if (a == inside_.size() || inside_next_[b] < inside_[a]) {
            frame_events_.triggers.push_back({inside_next_[b].first, inside_next_[b].second, true});
            ++b;
        } else {
            ++a;
            ++b;
        }
    }
    std::swap(inside_, inside_next_);
    // The next tick's systems see them with the last tick's events.
    tick_events_.triggers = frame_events_.triggers;
}

} // namespace forge::sim
