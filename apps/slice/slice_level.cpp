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

bool load_objects(objects::Library& library, const std::filesystem::path& game_dir, std::string* error) {
    const bool ok = library.load(game_dir / "kinds.json", game_dir / "objects", error);
    if (ok) FORGE_INFO("Объекты: видов %zu, шаблонов %zu", library.kinds().size(), library.templates().size());
    return ok;
}

void attach_objects(objects::Library& library, scene::Scene& scene) {
    library.attach(scene);
    // Saves from before templates: which template each object is.
    library.set_adopt([&library](flecs::entity e) -> const objects::Template* {
        if (e.has<Hero>()) return nullptr;
        if (const Npc* n = e.try_get<Npc>()) return library.find(n->who == 0 ? "miner" : "smith");
        if (e.has<Critter>()) return library.find("critter");
        if (const Item* i = e.try_get<Item>()) {
            static const char* ids[] = {"pickaxe", "coins", "copper", "wood", "torches"};
            return i->kind < 5 ? library.find(ids[i->kind]) : nullptr;
        }
        if (e.has<RigidBody>()) return library.find("crate");
        return nullptr;
    });
}

flecs::entity spawn_object(const objects::Library& library, scene::Scene& scene, std::string_view id, f64 x,
                           f64 feet_y, u32 seed) {
    const objects::Template* t = library.find(id);
    if (!t) return {};
    flecs::entity e = library.spawn(scene, *t, x, feet_y);
    if (!e.is_valid()) return e;
    if (Npc* n = e.try_get_mut<Npc>()) {
        n->home_x = static_cast<f32>(x);
        n->seed = seed;
    }
    if (Critter* c = e.try_get_mut<Critter>()) {
        const u32 s = hash32(seed, 100);
        c->dir = (s & 1) ? 1.0f : -1.0f;
        c->seed = s;
    }
    return e;
}

void populate(const SliceGenerator& gen, const objects::Library& library, ChunkCoord coord, scene::Scene& scene) {
    const i32 x0 = coord.x * kChunkSize, y0 = coord.y * kChunkSize;
    auto here = [&](f64 x, f64 y) {
        const i32 tx = static_cast<i32>(std::floor(x)), ty = static_cast<i32>(std::floor(y));
        return tx >= x0 && tx < x0 + kChunkSize && ty >= y0 && ty < y0 + kChunkSize;
    };
    // Each object stands with its feet at (x, y).
    auto put = [&](std::string_view id, f64 x, f64 y, u32 seed) {
        return here(x, y - 0.5) ? spawn_object(library, scene, id, x, y, seed) : flecs::entity();
    };
    const f64 v = gen.village_y();
    put("miner", (gen.miner_house().x0 + gen.miner_house().x1) * 0.5 + 0.5, v, 13u);
    put("smith", (gen.smith_house().x0 + gen.smith_house().x1) * 0.5 + 0.5, v, 7919u + 13u);
    put("pickaxe", gen.pickaxe_x(), gen.pickaxe_y() + 0.1, 1);
    // A bigger pile than the template's by the pickaxe: this copy's own count.
    if (flecs::entity coins = put("coins", gen.pickaxe_x() + 3.0, gen.pickaxe_y() + 0.1, 1); coins.is_valid())
        if (const objects::Template* t = library.find("coins"))
            if (const objects::PropDef* count = library.prop_of(*t, "count")) library.set_value(scene, coins, *count, "12");
    put("torches", gen.gallery_x0() - 3.5, gen.gallery_y() - 0.1, 1);
    const f64 sx = gen.smith_house().x1 + 4.0;
    put("crate", sx, v, 1);
    put("crate", sx + 1.0, v, 1);
    put("crate", sx + 0.5, v - 1.0, 1);
    put("crate", gen.gallery_x0() - 16.5, gen.gallery_y() + 1.0, 1);
    put("crate", gen.gallery_x0() - 17.5, gen.gallery_y() + 1.0, 1);

    // A few critters on the surface outside the village.
    const u32 seed = hash32(static_cast<u32>(coord.x) * 92821u, static_cast<u32>(coord.y));
    for (u32 k = 0; k < seed % 4; ++k) {
        const i32 x = x0 + static_cast<i32>(hash32(seed, k) % kChunkSize);
        if (std::abs(x) < 40) continue;
        const f64 y = gen.surface(x) - 0.6;
        put("critter", x + 0.5, y, hash32(seed, k + 100));
    }
}

void Objects::init(flecs::world& ecs) {
    // An object made of several blocks is drawn once: as a villager, else as
    // a critter, else as an item.
    npcs = ecs.query<Position, Body, Npc>();
    critters = ecs.query_builder<Position, Body, Critter>().without<Npc>().build();
    items = ecs.query_builder<Position, Body, Item>().without<Npc>().without<Critter>().build();
    crates = ecs.query<Position, RigidBody>();
    bodies = ecs.query_builder<Position, Body>().without<Npc>().without<Critter>().without<Item>().without<Hero>().build();
}

void push_objects(render::SpriteBatch& batch, Objects& objects, const SliceGenerator& gen, f64 cam_x, f64 cam_y,
                  f32 alpha, u64 tick, const Pictures* pictures) {
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
    // The template's own picture, if the object is a copy of one that has it.
    const bool any_picture = pictures && !pictures->empty();
    auto picture = [&](flecs::entity e) -> const Pictures::Picture* {
        if (!any_picture) return nullptr;
        const objects::ObjectRef* ref = e.try_get<objects::ObjectRef>();
        return ref ? pictures->of(ref->key) : nullptr;
    };
    objects.critters.each([&](flecs::entity e, const Position& p, const Body& b, const Critter& c) {
        f64 x, y;
        sim::draw_position(p, b, alpha, x, y);
        const f32 side = c.dir < 0 ? -1.0f : 1.0f;
        if (const Pictures::Picture* pic = picture(e)) {
            at(x, y, side * pic->aspect, 1.0f, pic->frame, 1);
            return;
        }
        Sprite* s = batch.push(1);
        if (!s) return;
        s->x = static_cast<f32>(x - cam_x);
        s->y = static_cast<f32>(y - cam_y);
        s->w = side;
        s->h = 1.0f;
        s->angle = 0;
        s->frame = (c.seed % demo::kCritterKinds) * 2 + (((b.contacts & sim::OnGround) ? phase + c.seed : 0) & 1u);
        s->color = 0xffffffffu;
        s->order = 1;
    });
    objects.npcs.each([&](flecs::entity e, const Position& p, const Body& b, const Npc& n) {
        f64 x, y;
        sim::draw_position(p, b, alpha, x, y);
        const f32 side = n.facing < 0 ? -1.0f : 1.0f;
        if (const Pictures::Picture* pic = picture(e)) {
            at(x, y, side * 2.0f * pic->aspect, 2.0f, pic->frame, 2);
            return;
        }
        const u32 base = n.who == 0 ? FrameMiner : FrameSmith;
        const u32 frame = base + (n.dir != 0 ? (phase & 1u) : 0u);
        at(x, y, side, 2.0f, frame, 2);
    });
    objects.items.each([&](flecs::entity e, const Position& p, const Body& b, const Item& i) {
        f64 x, y;
        sim::draw_position(p, b, alpha, x, y);
        const f64 bob = std::sin(static_cast<f64>(tick) * 0.08 + p.tile_x()) * 0.08;
        if (const Pictures::Picture* pic = picture(e))
            at(x, y + bob, 0.8f * pic->aspect, 0.8f, pic->frame, 3);
        else
            at(x, y + bob, 0.8f, 0.8f, item_frame(static_cast<ItemKind>(i.kind)), 3);
        if (i.kind == static_cast<u8>(ItemKind::Pickaxe))
            at(x, y, 2.4f, 2.4f, demo::kFrameGlow, 0, render::pack_color(255, 220, 120, 90));
    });
    objects.crates.each([&](flecs::entity e, const Position& p, const RigidBody& rb) {
        const Pictures::Picture* pic = picture(e);
        at(p.tile_x(), p.tile_y(), rb.half_w * 2.0f, rb.half_h * 2.0f, pic ? pic->frame : demo::kFrameCrate, 1,
           0xffffffffu, rb.angle);
    });
    // Objects that only have a body: their picture, else a crate.
    objects.bodies.each([&](flecs::entity e, const Position& p, const Body& b) {
        if (!e.has<objects::ObjectRef>()) return;
        f64 x, y;
        sim::draw_position(p, b, alpha, x, y);
        const Pictures::Picture* pic = picture(e);
        const f32 h = b.half_h * 2.0f;
        at(x, y, pic ? h * pic->aspect : b.half_w * 2.0f, h, pic ? pic->frame : demo::kFrameCrate, 1);
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
    base_sheet_ = make_sheet();
    sheet_ = base_sheet_;
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

bool SliceLevel::load_objects(const std::filesystem::path& game_dir, std::string* error) {
    return slice::load_objects(library_, game_dir, error);
}

const std::vector<level::ObjectDef>& SliceLevel::objects() const {
    if (defs_version_ != library_.version()) {
        defs_version_ = library_.version();
        object_defs_.clear();
        for (const objects::Template& t : library_.templates()) {
            const objects::KindDef* k = library_.kind_of(t);
            if (!k) continue;
            object_defs_.push_back({t.id, t.name, k->group, t.about, t.key});
        }
    }
    return object_defs_;
}

void SliceLevel::refresh_pictures() const {
    if (pictures_version_ == library_.version()) return;
    pictures_version_ = library_.version();
    if (pictures_.update(library_, base_sheet_, sheet_)) sheet_changed_ = true;
}

void SliceLevel::object_icon(const level::ObjectDef& def, u32 size, std::vector<u8>& rgba) const {
    rgba.assign(static_cast<usize>(size) * size * 4, 0);
    refresh_pictures();
    const objects::Template* t = library_.find(def.key);
    const objects::KindDef* k = t ? library_.kind_of(*t) : nullptr;
    if (!k) return;
    // The picture the game draws for such an object: by its blocks.
    auto choice = [&](std::string_view prop) {
        const objects::PropDef* p = library_.prop_of(*t, prop);
        return p ? library_.value(*t, *p) : std::string();
    };
    u32 frame = demo::kFrameCrate;
    if (library_.has_block(*t, "villager")) frame = choice("who") == "\"smith\"" ? FrameSmith : FrameMiner;
    else if (library_.has_block(*t, "critter")) frame = 2;
    else if (library_.has_block(*t, "pickup")) {
        static const char* ids[] = {"\"pickaxe\"", "\"coins\"", "\"copper\"", "\"wood\"", "\"torch\""};
        const std::string what = choice("what");
        for (u8 i = 0; i < 5; ++i)
            if (what == ids[i]) frame = item_frame(static_cast<ItemKind>(i));
    }
    if (const Pictures::Picture* pic = pictures_.of(t->key)) frame = pic->frame;
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
    const auto& defs = objects();
    if (index >= defs.size()) return {};
    return spawn_object(library_, level.scene(), defs[index].id, x, y, hash32(static_cast<u32>(level.new_id()), 7));
}

i32 SliceLevel::object_kind(flecs::entity e) const {
    if (!e.is_alive() || e.has<Hero>()) return -1;
    const objects::ObjectRef* ref = e.try_get<objects::ObjectRef>();
    if (!ref) return -1;
    const auto& defs = objects();
    for (usize i = 0; i < defs.size(); ++i)
        if (defs[i].key == ref->key) return static_cast<i32>(i);
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
    attach_objects(library_, scene);
    scene.set_populator([this](ChunkCoord c, scene::Scene& s) { populate(*gen_, library_, c, s); });
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
    refresh_pictures();
    if (!sprites_.init(device, format, sheet_.sheet(), 1u << 17)) return false;
    sheet_changed_ = false;
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
    refresh_pictures();
    if (sheet_changed_ && sprites_.set_sheet(sheet_.sheet())) sheet_changed_ = false;
    const u64 tick = static_cast<u64>(time * 60.0);
    batch_.begin(camera.snapped_x(), camera.snapped_y(), sprites_.max_sprites());
    push_objects(batch_, objects_, *gen_, batch_.origin_x(), batch_.origin_y(), 1.0f, tick, &pictures_);
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
