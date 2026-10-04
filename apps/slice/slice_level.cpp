#include "slice_level.h"

#include "slice_art.h"

#include "forge/core/log.h"
#include "forge/sim/cells.h"
#include "forge/sim/simulation.h"

#include <algorithm>
#include <cmath>

FORGE_REFLECT(slice::Hero, 1) { t.field("facing", &slice::Hero::facing); }
FORGE_REFLECT(slice::Npc, 1) {
    t.field("who", &slice::Npc::who).hidden();
    t.field("home_x", &slice::Npc::home_x).label("Дом (x)");
    t.field("dir", &slice::Npc::dir).hidden();
    t.field("facing", &slice::Npc::facing).label("Смотрит (−1 влево, 1 вправо)").range(-1, 1);
    t.field("timer", &slice::Npc::timer).hidden();
    t.field("seed", &slice::Npc::seed).hidden();
}
FORGE_REFLECT(slice::Item, 1) {
    t.field("kind", &slice::Item::kind).hidden();
    t.field("count", &slice::Item::count).label("Сколько").range(1, 999);
}
FORGE_REFLECT(slice::Critter, 1) {
    t.field("speed", &slice::Critter::speed).label("Скорость").range(0.2, 6);
    t.field("dir", &slice::Critter::dir).hidden();
    t.field("seed", &slice::Critter::seed).label("Вид и характер");
}

namespace slice {

using namespace forge;
using namespace forge::world;
using forge::render::Sprite;
using forge::scene::Position;
using forge::sim::Body;
using forge::sim::RigidBody;

namespace {
constexpr u64 kSeed = 1;
}

u32 hash32(u32 a, u32 b) {
    u32 h = a * 374761393u + b * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

u32 item_frame(ItemKind k) {
    switch (k) {
    case ItemKind::Pickaxe: return FramePickaxe;
    case ItemKind::Coins: return FrameCoins;
    case ItemKind::Copper: return demo::kFrameOre;
    case ItemKind::Wood: return FrameWood;
    case ItemKind::Torch: return FrameTorch;
    }
    return FrameDust;
}

WorldDesc world_desc() {
    WorldDesc wd;
    wd.layer_count = 3;                 // walls, blocks, liquids
    wd.bounds = {-512, -128, 512, 896}; // 1024 × 1024 chunks: 65 536 tiles each way
    return wd;
}

void register_components(scene::Scene& scene) {
    scene.register_component<Hero>();
    scene.register_component<Npc>();
    scene.register_component<Item>();
    scene.register_component<Critter>();
}

void populate(const SliceGenerator& gen, ChunkCoord coord, scene::Scene& scene) {
    const i32 x0 = coord.x * kChunkSize, y0 = coord.y * kChunkSize;
    auto here = [&](f64 x, f64 y) {
        const i32 tx = static_cast<i32>(std::floor(x)), ty = static_cast<i32>(std::floor(y));
        return tx >= x0 && tx < x0 + kChunkSize && ty >= y0 && ty < y0 + kChunkSize;
    };
    const f64 v = gen.village_y();
    auto person = [&](u8 who, f64 x) {
        if (!here(x, v - 1.0)) return;
        flecs::entity e = scene.spawn(Position::at_tile(x, v - kHeroHalfH - 0.02));
        if (!e.is_valid()) return;
        Body b;
        b.half_w = kHeroHalfW;
        b.half_h = kHeroHalfH;
        e.set<Body>(b);
        Npc n;
        n.who = who;
        n.home_x = static_cast<f32>(x);
        n.seed = who * 7919u + 13u;
        n.timer = 1.0f;
        n.facing = who == 0 ? -1.0f : 1.0f;
        e.set<Npc>(n);
    };
    person(0, (gen.miner_house().x0 + gen.miner_house().x1) * 0.5 + 0.5);
    person(1, (gen.smith_house().x0 + gen.smith_house().x1) * 0.5 + 0.5);

    auto item = [&](ItemKind kind, u16 count, f64 x, f64 y) {
        if (!here(x, y)) return;
        flecs::entity e = scene.spawn(Position::at_tile(x, y));
        if (!e.is_valid()) return;
        Body b;
        b.half_w = b.half_h = 0.3f;
        e.set<Body>(b);
        e.set<Item>({static_cast<u8>(kind), count});
    };
    item(ItemKind::Pickaxe, 1, gen.pickaxe_x(), gen.pickaxe_y() - 0.2);
    item(ItemKind::Coins, 12, gen.pickaxe_x() + 3.0, gen.pickaxe_y() - 0.2);
    item(ItemKind::Torch, 3, gen.gallery_x0() - 3.5, gen.gallery_y() - 0.4);

    auto crate = [&](f64 x, f64 y) {
        if (!here(x, y)) return;
        flecs::entity e = scene.spawn(Position::at_tile(x, y));
        if (!e.is_valid()) return;
        RigidBody rb;
        rb.half_w = rb.half_h = 0.48f;
        rb.density = 0.6f;
        e.set<RigidBody>(rb);
    };
    const f64 sx = gen.smith_house().x1 + 4.0;
    crate(sx, v - 0.5);
    crate(sx + 1.0, v - 0.5);
    crate(sx + 0.5, v - 1.5);
    crate(gen.gallery_x0() - 16.5, gen.gallery_y() + 0.5);
    crate(gen.gallery_x0() - 17.5, gen.gallery_y() + 0.5);

    // A few critters on the surface outside the village.
    const u32 seed = hash32(static_cast<u32>(coord.x) * 92821u, static_cast<u32>(coord.y));
    for (u32 k = 0; k < seed % 4; ++k) {
        const i32 x = x0 + static_cast<i32>(hash32(seed, k) % kChunkSize);
        if (std::abs(x) < 40) continue;
        const f64 y = gen.surface(x) - 1.0;
        if (!here(x + 0.5, y)) continue;
        flecs::entity e = scene.spawn(Position::at_tile(x + 0.5, y));
        if (!e.is_valid()) continue;
        Body b;
        b.half_w = 0.35f;
        b.half_h = 0.4f;
        e.set<Body>(b);
        const u32 s = hash32(seed, k + 100);
        e.set<Critter>({1.2f + static_cast<f32>(s % 200) / 100.0f, (s & 1) ? 1.0f : -1.0f, s});
    }
}

void Objects::init(flecs::world& ecs) {
    npcs = ecs.query<Position, Body, Npc>();
    critters = ecs.query<Position, Body, Critter>();
    items = ecs.query<Position, Body, Item>();
    crates = ecs.query<Position, RigidBody>();
}

void push_objects(render::SpriteBatch& batch, Objects& objects, const SliceGenerator& gen, f64 cam_x, f64 cam_y,
                  f32 alpha, u64 tick) {
    const u32 phase = static_cast<u32>(tick / 8);
    auto at = [&](f64 x, f64 y, f32 w, f32 h, u32 frame, u32 order, u32 color = 0xffffffffu, f32 angle = 0) {
        Sprite s;
        s.x = static_cast<f32>(x - cam_x);
        s.y = static_cast<f32>(y - cam_y);
        s.w = w;
        s.h = h;
        s.frame = frame;
        s.order = order;
        s.color = color;
        s.angle = angle;
        batch.push(s);
    };
    objects.critters.each([&](const Position& p, const Body& b, const Critter& c) {
        Sprite* s = batch.push(1);
        if (!s) return;
        f64 x, y;
        sim::draw_position(p, b, alpha, x, y);
        s->x = static_cast<f32>(x - cam_x);
        s->y = static_cast<f32>(y - cam_y);
        s->w = c.dir < 0 ? -1.0f : 1.0f;
        s->h = 1.0f;
        s->angle = 0;
        s->frame = (c.seed % demo::kCritterKinds) * 2 + (((b.contacts & sim::OnGround) ? phase + c.seed : 0) & 1u);
        s->color = 0xffffffffu;
        s->order = 1;
    });
    objects.npcs.each([&](const Position& p, const Body& b, const Npc& n) {
        f64 x, y;
        sim::draw_position(p, b, alpha, x, y);
        const u32 base = n.who == 0 ? FrameMiner : FrameSmith;
        const u32 frame = base + (n.dir != 0 ? (phase & 1u) : 0u);
        at(x, y, n.facing < 0 ? -1.0f : 1.0f, 2.0f, frame, 2);
    });
    objects.items.each([&](const Position& p, const Body& b, const Item& i) {
        f64 x, y;
        sim::draw_position(p, b, alpha, x, y);
        const f64 bob = std::sin(static_cast<f64>(tick) * 0.08 + p.tile_x()) * 0.08;
        at(x, y + bob, 0.8f, 0.8f, item_frame(static_cast<ItemKind>(i.kind)), 3);
        if (i.kind == static_cast<u8>(ItemKind::Pickaxe))
            at(x, y, 2.4f, 2.4f, demo::kFrameGlow, 0, render::pack_color(255, 220, 120, 90));
    });
    objects.crates.each([&](const Position& p, const RigidBody& rb) {
        at(p.tile_x(), p.tile_y(), rb.half_w * 2.0f, rb.half_h * 2.0f, demo::kFrameCrate, 1, 0xffffffffu, rb.angle);
    });
    // The smith's anvil by his door.
    const House h = gen.smith_house();
    at(h.x1 - 3.5, gen.village_y() - 0.5, 1.4f, 1.0f, FrameAnvil, 1);
}

void push_torches(render::SpriteBatch& batch, const World& world, const Rect& view, f64 cam_x, f64 cam_y, u64 tick,
                  std::vector<std::pair<f64, f64>>& torches) {
    torches.clear();
    for (i32 y = view.y0 - 4; y < view.y1 + 4; ++y)
        for (i32 x = view.x0 - 4; x < view.x1 + 4; ++x)
            if (world.tile(kWalls, x, y) == TileTorch && world.tile(kBlocks, x, y) == TileAir) {
                torches.push_back({x + 0.5, y + 0.25});
                const f32 flicker = 0.9f + 0.1f * std::sin(static_cast<f32>(tick) * 0.3f + static_cast<f32>(x * 7 + y));
                Sprite s;
                s.x = static_cast<f32>(x + 0.5 - cam_x);
                s.y = static_cast<f32>(y + 0.2 - cam_y);
                s.w = 0.5f * flicker;
                s.h = 0.6f * flicker;
                s.frame = FrameFlame;
                s.order = 3;
                batch.push(s);
            }
}

bool init_tiles(render::TilemapRenderer& tiles, SDL_GPUDevice* device, SDL_GPUTextureFormat format, World& world,
                const std::vector<u8>& atlas) {
    if (!tiles.init(device, format, world, {atlas.data(), demo::kTileCellPx, demo::kTileCells})) return false;
    const Color liquid_colors[2] = {{}, {0.20f, 0.45f, 0.95f, 0.72f}};
    tiles.set_layer_liquid(kLiquids, liquid_colors, 2, sim::kFull);
    tiles.set_layer_tint(kWalls, {0.58f, 0.58f, 0.64f, 1.0f});
    return true;
}

// --- the level module ---------------------------------------------------------

SliceLevel::SliceLevel() : gen_(std::make_shared<SliceGenerator>(kSeed)) {
    layers_ = {"Стены", "Блоки", "Жидкости"};
    using level::TileDef;
    const std::string earth = "Земля и камень", ore = "Руда", mine = "Шахта", build = "Постройки", liquid = "Жидкости";
    tiles_ = {
        {"grass", "Трава", earth, "копается руками", kBlocks, TileGrass, "1"},
        {"dirt", "Земля", earth, "копается руками", kBlocks, TileDirt, "2"},
        {"stone", "Камень", earth, "копается киркой", kBlocks, TileStone, "3"},
        {"sand", "Песок", earth, "падает, если под ним пусто", kBlocks, TileSand, "4"},
        {"dirt_wall", "Стена земли", earth, "фон за блоками", kWalls, TileDirtWall, ""},
        {"stone_wall", "Стена камня", earth, "фон за блоками", kWalls, TileStoneWall, ""},
        {"copper", "Медь", ore, "кузнец ждёт десять", kBlocks, TileCopper, ""},
        {"iron", "Железо", ore, "киркой", kBlocks, TileIron, ""},
        {"gold", "Золото", ore, "киркой", kBlocks, TileGold, ""},
        {"planks", "Доски", mine, "держат песок, копаются руками", kBlocks, TilePlanks, "5"},
        {"plank_wall", "Стена из досок", mine, "фон", kWalls, TilePlankWall, ""},
        {"beam", "Балка", mine, "фон, опора штольни", kWalls, TileBeam, ""},
        {"torch", "Факел", mine, "светит в темноте", kWalls, TileTorch, "6"},
        {"brick", "Кирпич", build, "стены домов", kBlocks, TileBrick, ""},
        {"roof", "Крыша", build, "", kBlocks, TileRoof, ""},
        {"window", "Окно", build, "фон", kWalls, TileWindow, ""},
        {"water", "Вода", liquid, "растекается в игре", kLiquids, sim::make_liquid(kWater, sim::kFull), "7"},
    };
    object_defs_ = {
        {"miner", "Шахтёр Борис", "Жители", "просит найти кирку"},
        {"smith", "Кузнец", "Жители", "ждёт десять меди"},
        {"critter", "Зверёк", "Животные", "бегает по поверхности"},
        {"pickaxe", "Кирка", "Предметы", "та самая, Бориса"},
        {"coins", "Монеты", "Предметы", "лежат и ждут героя"},
        {"copper", "Медь", "Предметы", "руда для кузнеца"},
        {"wood", "Дерево", "Предметы", ""},
        {"torches", "Факелы", "Предметы", "можно поставить на стену"},
        {"crate", "Ящик", "Разное", "падает, плавает, разбивается"},
    };
    sheet_ = make_sheet();
    atlas_ = make_atlas();
    // Minimap colours: the average of each atlas cell.
    const u32 cell = demo::kTileCellPx, row = cell * demo::kTileCells;
    map_colors_.assign(TileSliceCount, 0);
    for (u32 t = 1; t < TileSliceCount; ++t) {
        u64 sum[4] = {};
        const u32 cx = (t % demo::kTileCells) * cell, cy = (t / demo::kTileCells) * cell;
        for (u32 y = 0; y < cell; ++y)
            for (u32 x = 0; x < cell; ++x)
                for (u32 c = 0; c < 4; ++c) sum[c] += atlas_[(static_cast<usize>(cy + y) * row + cx + x) * 4 + c];
        const u32 n = cell * cell;
        if (sum[3] == 0) continue;
        map_colors_[t] = render::pack_color(static_cast<u8>(sum[0] / n), static_cast<u8>(sum[1] / n),
                                            static_cast<u8>(sum[2] / n), 255);
    }
}

// --- objects ---

namespace {
constexpr i32 kFirstItem = 3; // objects_[3..7] are items, in ItemKind order
constexpr i32 kCrate = 8;
}

void SliceLevel::object_icon(const level::ObjectDef& def, u32 size, std::vector<u8>& rgba) const {
    rgba.assign(static_cast<usize>(size) * size * 4, 0);
    const auto it = std::find_if(object_defs_.begin(), object_defs_.end(), [&](const level::ObjectDef& d) { return d.id == def.id; });
    const i32 kind = static_cast<i32>(it - object_defs_.begin());
    u32 frame = demo::kFrameCrate;
    if (kind == 0) frame = FrameMiner;
    else if (kind == 1) frame = FrameSmith;
    else if (kind == 2) frame = 2; // a critter
    else if (kind >= kFirstItem && kind < kCrate) frame = item_frame(static_cast<ItemKind>(kind - kFirstItem));
    if (frame >= sheet_.frames.size()) return;
    const render::SpriteRect r = sheet_.frames[frame];
    // Fit the frame, keeping its shape (people are twice as tall).
    const f32 scale = static_cast<f32>(size) / static_cast<f32>(std::max(r.w, r.h));
    const u32 w = static_cast<u32>(static_cast<f32>(r.w) * scale), h = static_cast<u32>(static_cast<f32>(r.h) * scale);
    const u32 ox = (size - w) / 2, oy = (size - h) / 2;
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            const u32 sx = r.x + std::min(r.w - 1, static_cast<u32>(static_cast<f32>(x) / scale));
            const u32 sy = r.y + std::min(r.h - 1, static_cast<u32>(static_cast<f32>(y) / scale));
            const u8* src = &sheet_.rgba[(static_cast<usize>(sy) * sheet_.width + sx) * 4];
            u8* dst = &rgba[(static_cast<usize>(oy + y) * size + ox + x) * 4];
            std::copy(src, src + 4, dst);
        }
}

flecs::entity SliceLevel::place_object(level::Level& level, usize index, f64 x, f64 y) {
    scene::Scene& scene = level.scene();
    const i32 kind = static_cast<i32>(index);
    auto spawn = [&](f64 half_h) { return scene.spawn(Position::at_tile(x, y - half_h - 0.02)); };
    flecs::entity e;
    if (kind == 0 || kind == 1) {
        e = spawn(kHeroHalfH);
        if (!e.is_valid()) return e;
        Body b;
        b.half_w = kHeroHalfW;
        b.half_h = kHeroHalfH;
        e.set<Body>(b);
        Npc n;
        n.who = static_cast<u8>(kind);
        n.home_x = static_cast<f32>(x);
        n.seed = hash32(static_cast<u32>(level.new_id()), 7);
        n.timer = 1.0f;
        n.facing = 1.0f;
        e.set<Npc>(n);
    } else if (kind == 2) {
        e = spawn(0.4);
        if (!e.is_valid()) return e;
        Body b;
        b.half_w = 0.35f;
        b.half_h = 0.4f;
        e.set<Body>(b);
        const u32 s = hash32(static_cast<u32>(level.new_id()), 100);
        e.set<Critter>({1.2f + static_cast<f32>(s % 200) / 100.0f, (s & 1) ? 1.0f : -1.0f, s});
    } else if (kind >= kFirstItem && kind < kCrate) {
        e = spawn(0.3);
        if (!e.is_valid()) return e;
        Body b;
        b.half_w = b.half_h = 0.3f;
        e.set<Body>(b);
        const ItemKind ik = static_cast<ItemKind>(kind - kFirstItem);
        const u16 count = ik == ItemKind::Coins ? 10 : ik == ItemKind::Torch ? 3 : 1;
        e.set<Item>({static_cast<u8>(ik), count});
    } else if (kind == kCrate) {
        e = spawn(0.48);
        if (!e.is_valid()) return e;
        RigidBody rb;
        rb.half_w = rb.half_h = 0.48f;
        rb.density = 0.6f;
        e.set<RigidBody>(rb);
    }
    return e;
}

i32 SliceLevel::object_kind(flecs::entity e) const {
    if (!e.is_alive() || e.has<Hero>()) return -1;
    if (const Npc* n = e.try_get<Npc>()) return n->who == 0 ? 0 : 1;
    if (e.has<Critter>()) return 2;
    if (const Item* i = e.try_get<Item>()) return std::min<i32>(kFirstItem + i->kind, kCrate - 1);
    if (e.has<RigidBody>()) return kCrate;
    return -1;
}

bool SliceLevel::object_box(flecs::entity e, f64& x0, f64& y0, f64& x1, f64& y1) const {
    const Position* p = e.try_get<Position>();
    if (!p) return false;
    f64 hw = 0.4, hh = 0.4;
    if (const Body* b = e.try_get<Body>()) {
        hw = std::max(0.4, static_cast<f64>(b->half_w));
        hh = std::max(0.4, static_cast<f64>(b->half_h));
        if (e.has<Npc>()) hw = 0.5, hh = 1.0; // drawn 1 × 2
    } else if (const RigidBody* rb = e.try_get<RigidBody>()) {
        hw = rb->half_w;
        hh = rb->half_h;
    }
    x0 = p->tile_x() - hw;
    x1 = p->tile_x() + hw;
    y0 = p->tile_y() - hh;
    y1 = p->tile_y() + hh;
    return true;
}

void SliceLevel::object_moved(flecs::entity e) {
    if (Body* b = e.try_get_mut<Body>()) b->vx = b->vy = 0;
    if (RigidBody* rb = e.try_get_mut<RigidBody>()) rb->vx = rb->vy = rb->spin = 0;
    if (Npc* n = e.try_get_mut<Npc>())
        if (const Position* p = e.try_get<Position>()) n->home_x = static_cast<f32>(p->tile_x());
}

bool SliceLevel::object_component_shown(const reflect::TypeInfo* type) const {
    return type == reflect::type_of<Npc>() || type == reflect::type_of<Item>() || type == reflect::type_of<Critter>();
}

u32 SliceLevel::map_color(u32 layer, TileId value) const {
    if (value == 0) return 0;
    if (layer == kLiquids) return render::pack_color(51, 115, 242, 255);
    if (value >= map_colors_.size()) return 0;
    const u32 c = map_colors_[value];
    if (layer != kWalls || c == 0) return c;
    // Walls are behind: darker, as in the game.
    auto dim = [&](u32 shift) { return static_cast<u8>(((c >> shift) & 0xff) * 6 / 10); };
    return render::pack_color(dim(0), dim(8), dim(16), 255);
}

bool SliceLevel::play_spot(level::Level& level, f64 x, f64 y, f64& out_x, f64& out_y) const {
    const i32 tx = static_cast<i32>(std::floor(x)), ty = static_cast<i32>(std::floor(y));
    constexpr i32 kReach = 96;
    level.ensure_loaded({tx - 1, ty - kReach - 3, tx + 2, ty + kReach + 1});
    const World& w = level.world();
    auto free = [&](i32 cy) { return !is_solid(w.tile(kBlocks, tx, cy)); };
    // Ground under two free cells, nearest to y first.
    for (i32 d = 0; d <= kReach; ++d)
        for (const i32 cy : {ty + d, ty - d})
            if (!free(cy) && free(cy - 1) && free(cy - 2)) {
                out_x = tx + 0.5;
                out_y = cy;
                return true;
            }
    return false;
}

SliceLevel::~SliceLevel() { shutdown_view(); }

WorldDesc SliceLevel::world_desc() const {
    WorldDesc wd = slice::world_desc();
    // The editor flies over the world fast: keep a wider ring loaded.
    wd.load_margin = 3;
    return wd;
}

void SliceLevel::setup_scene(scene::Scene& scene) {
    // The game's bodies too: villagers and crates keep them in the saved level.
    sim::register_components(scene);
    register_components(scene);
    scene.set_populator([gen = gen_](ChunkCoord c, scene::Scene& s) { populate(*gen, c, s); });
}

void SliceLevel::tile_icon(const level::TileDef& tile, u32 size, std::vector<u8>& rgba) const {
    rgba.assign(static_cast<usize>(size) * size * 4, 0);
    if (tile.layer == kLiquids) {
        for (u32 y = 0; y < size; ++y)
            for (u32 x = 0; x < size; ++x) {
                u8* p = &rgba[(static_cast<usize>(y) * size + x) * 4];
                const bool surface = y < size / 8;
                p[0] = surface ? 120 : 51;
                p[1] = surface ? 170 : 115;
                p[2] = surface ? 250 : 242;
                p[3] = 255;
            }
        return;
    }
    const u32 cell = demo::kTileCellPx, row = demo::kTileCellPx * demo::kTileCells;
    const u32 cx = (tile.value % demo::kTileCells) * cell, cy = (tile.value / demo::kTileCells) * cell;
    for (u32 y = 0; y < size; ++y)
        for (u32 x = 0; x < size; ++x) {
            const u32 sx = cx + x * cell / size, sy = cy + y * cell / size;
            const u8* s = &atlas_[(static_cast<usize>(sy) * row + sx) * 4];
            u8* p = &rgba[(static_cast<usize>(y) * size + x) * 4];
            p[0] = s[0];
            p[1] = s[1];
            p[2] = s[2];
            p[3] = s[3];
        }
    // A torch is a stick on the atlas; the flame is a sprite. Add it.
    if (tile.value == TileTorch)
        for (u32 y = size / 8; y < size * 3 / 8; ++y)
            for (u32 x = size * 3 / 8; x < size * 5 / 8; ++x) {
                u8* p = &rgba[(static_cast<usize>(y) * size + x) * 4];
                p[0] = 255;
                p[1] = static_cast<u8>(y < size / 4 ? 220 : 150);
                p[2] = 60;
                p[3] = 255;
            }
}

void SliceLevel::start(f64& x, f64& y) const {
    x = gen_->spawn_x();
    y = gen_->spawn_y() - 2;
}

bool SliceLevel::init_view(SDL_GPUDevice* device, SDL_GPUTextureFormat format) {
    device_ = device;
    format_ = format;
    if (!sprites_.init(device, format, sheet_.sheet(), 1u << 17)) return false;
    if (!lights_.init(device, format)) return false;
    lights_.set_rules(light_rules());
    view_ready_ = true;
    return true;
}

void SliceLevel::shutdown_view() {
    if (!view_ready_) return;
    objects_ = {};
    objects_ecs_ = nullptr;
    tilemap_.shutdown();
    tilemap_world_ = nullptr;
    lights_.shutdown();
    sprites_.shutdown();
    view_ready_ = false;
}

void SliceLevel::level_closing(level::Level& level) {
    if (objects_ecs_ == &level.scene().ecs()) {
        objects_ = {};
        objects_ecs_ = nullptr;
    }
    if (tilemap_world_ == &level.world()) {
        tilemap_.shutdown();
        tilemap_world_ = nullptr;
    }
}

void SliceLevel::prepare_view(SDL_GPUCommandBuffer* cmd, level::Level& level, const render::Camera2D& camera, u32 width,
                              u32 height, const level::ViewOptions& options, f64 time) {
    if (!view_ready_) return;
    if (tilemap_world_ != &level.world()) {
        tilemap_.shutdown();
        tilemap_world_ = nullptr;
        if (!init_tiles(tilemap_, device_, format_, level.world(), atlas_)) {
            FORGE_ERROR("slice: the tile renderer did not start");
            return;
        }
        tilemap_world_ = &level.world();
    }
    if (objects_ecs_ != &level.scene().ecs()) {
        objects_ = {};
        objects_.init(level.scene().ecs());
        objects_ecs_ = &level.scene().ecs();
    }
    const u64 tick = static_cast<u64>(time * 60.0);
    batch_.begin(camera.snapped_x(), camera.snapped_y(), sprites_.max_sprites());
    push_objects(batch_, objects_, *gen_, batch_.origin_x(), batch_.origin_y(), 1.0f, tick);
    push_torches(batch_, level.world(), camera.visible_tiles(width, height), batch_.origin_x(), batch_.origin_y(), tick,
                 torches_);
    tilemap_.prepare(cmd, camera, width, height);
    sprites_.prepare(cmd, batch_, camera, width, height);
    game_light_ = options.game_light;
    if (game_light_) {
        for (const auto& [x, y] : torches_) lights_.add({x, y, kTorchLight.r, kTorchLight.g, kTorchLight.b});
        lights_.prepare(cmd, level.world(), camera, width, height);
    }
}

void SliceLevel::draw_view(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass) {
    if (!view_ready_ || !tilemap_world_) return;
    tilemap_.draw_layers(cmd, pass, 0, 2);
    sprites_.draw(cmd, pass);
    tilemap_.draw_layers(cmd, pass, kLiquids, 1);
    if (game_light_) lights_.draw(cmd, pass);
}

} // namespace slice
