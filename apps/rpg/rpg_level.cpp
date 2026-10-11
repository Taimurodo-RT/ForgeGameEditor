#include "rpg_level.h"

#include "forge/assets/image.h"
#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/world/generators.h"

#include <yyjson.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <map>

FORGE_REFLECT(rpg::Hero, 1) { t.field("speed", &rpg::Hero::speed).label("Скорость").range(1, 20); }
FORGE_REFLECT(rpg::Villager, 1) { t.field("talk", &rpg::Villager::talk).label("Разговор"); }
FORGE_REFLECT(rpg::Pickup, 1) {
    t.field("what", &rpg::Pickup::what).label("Что даёт");
    t.field("count", &rpg::Pickup::count).label("Сколько").range(1, 999);
}
FORGE_REFLECT(rpg::Place, 1) { t.field("shown", &rpg::Place::shown).label("Видно в игре"); }

namespace rpg {

namespace fs = std::filesystem;
using namespace forge;

void register_components(scene::Scene& scene) {
    scene.register_component<Hero>();
    scene.register_component<Villager>();
    scene.register_component<Pickup>();
    scene.register_component<Place>();
}

namespace {

// A property's JSON value as a number / a bool / a string; fallback when it is not one.
f64 json_number(const std::string& json, f64 fallback) {
    yyjson_doc* doc = yyjson_read(json.data(), json.size(), 0);
    yyjson_val* v = doc ? yyjson_doc_get_root(doc) : nullptr;
    const f64 out = v && yyjson_is_num(v) ? yyjson_get_num(v) : fallback;
    yyjson_doc_free(doc);
    return out;
}
bool json_bool(const std::string& json, bool fallback) {
    if (json == "true") return true;
    if (json == "false") return false;
    return fallback;
}
std::string json_string(const std::string& json) {
    yyjson_doc* doc = yyjson_read(json.data(), json.size(), 0);
    yyjson_val* v = doc ? yyjson_doc_get_root(doc) : nullptr;
    std::string out = v && yyjson_is_str(v) ? std::string(yyjson_get_str(v), yyjson_get_len(v)) : std::string();
    yyjson_doc_free(doc);
    return out;
}

// --- the pictures the code makes ---------------------------------------------------------------------------------------

using Rgb = std::array<u8, 3>;

// The middle colour of each tile (what the tests look for), by layer and value.
Rgb tile_rgb(u32 layer, world::TileId v) {
    if (layer == kGround) switch (v) {
        case kGrass: return {86, 158, 74};
        case kPath: return {198, 170, 118};
        case kSand: return {232, 212, 150};
        case kWater: return {58, 118, 204};
        default: break;
        }
    else switch (v) {
        case kWall: return {128, 126, 138};
        case kTree: return {38, 112, 52};
        case kFence: return {150, 104, 60};
        case kRock: return {150, 150, 156};
        default: break;
        }
    return {255, 0, 255};
}

Rgb shade(Rgb c, f64 k) {
    for (u8& x : c) x = static_cast<u8>(std::clamp(x * k, 0.0, 255.0));
    return c;
}

// A tile's picture, size px. The middle quarter is its colour exactly; the patterns stay out of it.
void tile_picture(u32 layer, world::TileId v, u32 size, std::vector<u8>& out, u32 stride, u32 ox, u32 oy) {
    const Rgb base = tile_rgb(layer, v);
    for (u32 y = 0; y < size; ++y)
        for (u32 x = 0; x < size; ++x) {
            u8* p = &out[(static_cast<usize>(oy + y) * stride + ox + x) * 4];
            const f64 u = (x + 0.5) / size, w = (y + 0.5) / size; // 0 … 1 across
            const bool middle = std::fabs(u - 0.5) < 0.2 && std::fabs(w - 0.5) < 0.2;
            Rgb c = base;
            u8 a = 255;
            if (layer == kGround) {
                const u32 hash = (x * 73856093u) ^ (y * 19349663u) ^ (v * 83492791u);
                if (v == kWater && !middle && std::fmod(w * 4 + std::sin(u * 6.283) * 0.3, 1.0) < 0.12) c = shade(base, 1.35);
                else if (v != kWater && !middle && hash % 11 == 0) c = shade(base, v == kGrass ? 0.75 : 0.85);
            } else if (v == kWall) {
                // Bricks: rows of four, every other one shifted by half a brick.
                const u32 row = static_cast<u32>(w * 4);
                const f64 bu = u * 2 + (row % 2 ? 0.5 : 0);
                if (!middle && (std::fmod(w * 4, 1.0) < 0.12 || std::fmod(bu, 1.0) < 0.06)) c = shade(base, 0.65);
            } else if (v == kTree) {
                // A crown: a disc with a darker rim, nothing round it.
                const f64 d = std::hypot(u - 0.5, w - 0.48);
                if (d > 0.47) a = 0;
                else if (d > 0.4) c = shade(base, 0.7);
            } else if (v == kFence) {
                // Two rails and posts; between them the ground shows.
                const bool rail = std::fabs(w - 0.35) < 0.07 || std::fabs(w - 0.62) < 0.07;
                const bool post = std::fabs(u - 0.15) < 0.07 || std::fabs(u - 0.85) < 0.07 || std::fabs(u - 0.5) < 0.2;
                if (!(rail || (post && w > 0.15 && w < 0.85))) a = 0;
                else if (!middle && post) c = shade(base, 0.8);
            } else if (v == kRock) {
                const f64 d = std::hypot((u - 0.5) * 1.1, w - 0.55);
                if (d > 0.45) a = 0;
                else if (d > 0.38) c = shade(base, 0.7);
            }
            p[0] = c[0], p[1] = c[1], p[2] = c[2], p[3] = a;
        }
}

// The colours of things without a picture, by Thing::Kind.
constexpr Rgb kFigure[] = {{52, 96, 220}, {230, 140, 40}, {250, 214, 60}, {120, 220, 140}};

// A thing's figure without a picture, size px: the hero and villagers a body with a head, a pickup a diamond, a place
// a square. Its middle is its colour exactly.
void figure_picture(Thing::Kind kind, u32 size, std::vector<u8>& out, u32 stride, u32 ox, u32 oy) {
    const Rgb c = kFigure[static_cast<usize>(kind)];
    for (u32 y = 0; y < size; ++y)
        for (u32 x = 0; x < size; ++x) {
            u8* p = &out[(static_cast<usize>(oy + y) * stride + ox + x) * 4];
            const f64 u = (x + 0.5) / size, w = (y + 0.5) / size;
            bool in = false, rim = false;
            switch (kind) {
            case Thing::Kind::Hero:
            case Thing::Kind::Villager: {
                const f64 head = std::hypot(u - 0.5, w - 0.24), body = std::hypot((u - 0.5) * 1.3, w - 0.66);
                in = head < 0.17 || body < 0.32;
                rim = in && head >= 0.14 && body >= 0.28;
                break;
            }
            case Thing::Kind::Pickup: {
                const f64 r = std::fabs(u - 0.5) + std::fabs(w - 0.55);
                in = r < 0.32;
                rim = r > 0.27;
                break;
            }
            case Thing::Kind::Place: {
                const f64 r = std::max(std::fabs(u - 0.5), std::fabs(w - 0.5));
                in = r < 0.46;
                rim = r > 0.4;
                break;
            }
            }
            if (!in) {
                p[0] = p[1] = p[2] = p[3] = 0;
                continue;
            }
            const Rgb k = rim ? shade(c, 0.6) : c;
            p[0] = k[0], p[1] = k[1], p[2] = k[2];
            p[3] = kind == Thing::Kind::Place ? 150 : 255;
        }
}

// A picture cut into its frames, each scaled to fit side × side (nearest pixel).
struct Cut {
    u32 frames = 0, w = 0, h = 0;
    std::vector<std::vector<u8>> rgba;
};
bool cut_picture(const fs::path& file, u32 frames, u32 side, Cut& out) {
    std::vector<u8> bytes;
    assets::CookedTexture img;
    if (!read_file(file, bytes) || !assets::decode_image(bytes, img) || img.width == 0 || img.height == 0) return false;
    if (frames < 1 || img.width % frames != 0) frames = 1;
    const u32 fw = img.width / frames, fh = img.height;
    const f64 scale = std::min(1.0, static_cast<f64>(side) / std::max(fw, fh));
    out.frames = frames;
    out.w = std::max(1u, static_cast<u32>(fw * scale));
    out.h = std::max(1u, static_cast<u32>(fh * scale));
    for (u32 f = 0; f < frames; ++f) {
        std::vector<u8> px(static_cast<usize>(out.w) * out.h * 4);
        for (u32 y = 0; y < out.h; ++y)
            for (u32 x = 0; x < out.w; ++x) {
                const u32 sx = std::min(fw - 1, static_cast<u32>(x / scale)), sy = std::min(fh - 1, static_cast<u32>(y / scale));
                const u8* s = &img.rgba8[(static_cast<usize>(sy) * img.width + f * fw + sx) * 4];
                std::copy(s, s + 4, &px[(static_cast<usize>(y) * out.w + x) * 4]);
            }
        out.rgba.push_back(std::move(px));
    }
    return true;
}

constexpr u32 kTilePx = 32;  // a tile's picture
constexpr u32 kThingPx = 96; // the largest side a thing's frame keeps in the sheet
constexpr u32 kSheetW = 1024;

Thing::Kind kind_of(const objects::Library& library, const objects::Template& t, bool& known) {
    known = true;
    if (library.has_block(t, kHeroBlock)) return Thing::Kind::Hero;
    if (library.has_block(t, kVillagerBlock)) return Thing::Kind::Villager;
    if (library.has_block(t, kPickupBlock)) return Thing::Kind::Pickup;
    if (library.has_block(t, kPlaceBlock)) return Thing::Kind::Place;
    known = false;
    return Thing::Kind::Pickup;
}

std::string cell_words(i32 x, i32 y) { return std::to_string(x) + ":" + std::to_string(y); }

} // namespace

// --- the map ------------------------------------------------------------------------------------------------------------

world::TileId MapData::ground_at(i32 x, i32 y) const {
    x -= map.x0(), y -= map.y0();
    if (x < 0 || y < 0 || x >= map.w() || y >= map.h()) return 0;
    return ground[static_cast<usize>(y) * static_cast<usize>(map.w()) + static_cast<usize>(x)];
}

world::TileId MapData::obstacle_at(i32 x, i32 y) const {
    x -= map.x0(), y -= map.y0();
    if (x < 0 || y < 0 || x >= map.w() || y >= map.h()) return 0;
    return obstacles[static_cast<usize>(y) * static_cast<usize>(map.w()) + static_cast<usize>(x)];
}

bool read_map(level::Level& level, const objects::Library& library, const std::string& name, MapData& out, std::string& why) {
    const std::string head = "Карта «" + name + "»: ";
    level.ensure_loaded(world::Rect{-kReach, -kReach, kReach, kReach});
    level.load_objects();
    i32 x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
    for (i32 y = -kReach; y < kReach; ++y)
        for (i32 x = -kReach; x < kReach; ++x)
            if (level.tile(kGround, x, y)) x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
    if (x0 == INT_MAX) {
        why = head + "на слое «Земля» нет ни одной клетки";
        return false;
    }
    MapData m;
    const i32 w = x1 - x0 + 1, h = y1 - y0 + 1;
    m.map = Map(x0, y0, w, h);
    m.ground.assign(static_cast<usize>(w) * static_cast<usize>(h), 0);
    m.obstacles.assign(m.ground.size(), 0);
    for (i32 y = 0; y < h; ++y)
        for (i32 x = 0; x < w; ++x) {
            const usize i = static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x);
            m.ground[i] = level.tile(kGround, x0 + x, y0 + y);
            m.obstacles[i] = level.tile(kObstacles, x0 + x, y0 + y);
            m.map.set(x0 + x, y0 + y, walkable(m.ground[i], m.obstacles[i]));
        }
    // The copies, then in the order of the map (the scene's order is not the file's).
    std::vector<Thing> things, heroes;
    std::string empty_pickups;
    level.scene().ecs().each([&](flecs::entity e, const objects::ObjectRef& ref, const scene::Position& p) {
        const objects::Template* t = library.find(ref.key);
        if (!t) return;
        bool known = false;
        Thing th;
        th.kind = kind_of(library, *t, known);
        if (!known) return;
        th.tmpl = t;
        // A copy's place is a little above the middle of its cell (Library::spawn: the feet less the kind's foot).
        th.cx = static_cast<i32>(std::floor(p.tile_x()));
        th.cy = static_cast<i32>(std::floor(p.tile_y()));
        th.x = th.cx + 0.5;
        th.y = th.cy + 1.0;
        auto prop = [&](std::string_view id) {
            const objects::PropDef* d = library.prop_of(*t, id);
            return d ? library.value(level.scene(), e, *d) : std::string();
        };
        switch (th.kind) {
        case Thing::Kind::Hero: th.speed = static_cast<f32>(std::clamp(json_number(prop("speed"), 4), 1.0, 20.0)); break;
        case Thing::Kind::Villager: th.talk = json_string(prop("talk")); break;
        case Thing::Kind::Pickup:
            th.what = json_string(prop("what"));
            th.count = static_cast<u32>(std::clamp(json_number(prop("count"), 1), 1.0, 999.0));
            if (th.what.empty()) empty_pickups += (empty_pickups.empty() ? "«" : ", «") + t->name + "» в " + cell_words(th.cx, th.cy);
            break;
        case Thing::Kind::Place: th.shown = json_bool(prop("shown"), false); break;
        }
        (th.kind == Thing::Kind::Hero ? heroes : things).push_back(std::move(th));
    });
    auto by_place = [](const Thing& a, const Thing& b) {
        if (a.cy != b.cy) return a.cy < b.cy;
        if (a.cx != b.cx) return a.cx < b.cx;
        return a.tmpl->id < b.tmpl->id;
    };
    std::stable_sort(things.begin(), things.end(), by_place);
    std::stable_sort(heroes.begin(), heroes.end(), by_place);
    if (heroes.empty()) {
        why = head + "нет героя, поставьте копию вида «Герой»";
        return false;
    }
    if (heroes.size() > 1) {
        why = head + "героев " + std::to_string(heroes.size()) + ", нужен один";
        return false;
    }
    if (!m.map.free(heroes[0].cx, heroes[0].cy)) {
        why = head + "герой стоит не там, где можно ходить (клетка " + cell_words(heroes[0].cx, heroes[0].cy) + ")";
        return false;
    }
    if (!empty_pickups.empty()) {
        why = head + "у предметов не задано «Что даёт» (" + empty_pickups + ")";
        return false;
    }
    // Names for the saves: a template on a cell, and a number for more of one on one cell.
    std::map<std::string, u32> seen;
    for (Thing& t : things) {
        t.id = t.tmpl->id + "@" + std::to_string(t.cx) + "," + std::to_string(t.cy);
        if (const u32 n = seen[t.id]++; n > 0) t.id += "#" + std::to_string(n + 1);
        switch (t.kind) {
        case Thing::Kind::Villager: m.villagers.push_back(t); break;
        case Thing::Kind::Pickup: m.pickups.push_back(t); break;
        case Thing::Kind::Place: m.places.push_back(t); break;
        case Thing::Kind::Hero: break;
        }
    }
    m.hero = heroes[0];
    m.hero.id = m.hero.tmpl->id;
    out = std::move(m);
    return true;
}

Box pickup_box(const Thing& t) { return {t.cx + 0.15, t.cy + 0.3, t.cx + 0.85, t.cy + 1.0}; }
Box place_box(const Thing& t) { return {static_cast<f64>(t.cx), static_cast<f64>(t.cy), t.cx + 1.0, t.cy + 1.0}; }

// --- animation ----------------------------------------------------------------------------------------------------------

std::vector<level::LevelModule::AnimState> anim_states_of(const objects::Library& library, const objects::Template& t) {
    std::vector<level::LevelModule::AnimState> out;
    if (t.picture.empty()) return out;
    const u32 n = std::max(1u, t.frames);
    objects::Clip all{{}, 5, true};
    for (u32 f = 0; f < n; ++f) all.frames.push_back(f);
    if (library.has_block(t, kHeroBlock)) {
        objects::Clip walk{{}, 10, true};
        for (u32 f = n > 1 ? 1 : 0; f < n; ++f) walk.frames.push_back(f);
        out.push_back({"stand", "Стоит", {{0}, 10, true}});
        out.push_back({"walk", "Идёт", walk});
    } else if (library.has_block(t, kVillagerBlock) || library.has_block(t, kPickupBlock) || library.has_block(t, kPlaceBlock)) {
        out.push_back({"stand", "Стоит", all});
    }
    return out;
}

objects::Clip clip_of(const objects::Library& library, const objects::Template& t, std::string_view state) {
    if (auto it = t.animations.find(std::string(state)); it != t.animations.end() && objects::clip_problem(it->second, t.frames).empty())
        return it->second;
    for (const level::LevelModule::AnimState& s : anim_states_of(library, t))
        if (s.id == state) return s.rule;
    return {{0}, 10, true};
}

// --- drawing -------------------------------------------------------------------------------------------------------------

u32 Painter::figure_colour(Thing::Kind kind) {
    const Rgb& c = kFigure[static_cast<usize>(kind)];
    return render::pack_color(c[0], c[1], c[2]);
}

u32 Painter::tile_colour(u32 layer, world::TileId value) {
    const Rgb c = tile_rgb(layer, value);
    return render::pack_color(c[0], c[1], c[2]);
}

bool Painter::init(SDL_GPUDevice* device, SDL_GPUTextureFormat format) {
    objects::Library none;
    make_sheet(none);
    render::SpriteSheet sheet{rgba_.data(), width_, height_, rects_.data(), static_cast<u32>(rects_.size()), false};
    ready_ = sprites_.init(device, format, sheet, 16384);
    sheet_changed_ = false;
    return ready_;
}

void Painter::shutdown() {
    if (ready_) sprites_.shutdown();
    ready_ = false;
}

void Painter::update(const objects::Library& library) {
    // What the sheet is made of: each template's picture, its file's time and frames.
    std::string sig;
    for (const objects::Template& t : library.templates()) {
        if (t.picture.empty()) continue;
        sig += t.id + "|" + t.picture + "|" + std::to_string(t.frames) + "|" + std::to_string(t.picture_stamp) + "|";
        std::error_code ec;
        const fs::path file = library.picture_file(t);
        if (!file.empty()) sig += std::to_string(fs::last_write_time(file, ec).time_since_epoch().count());
        sig += ";";
    }
    if (sig == signature_) return;
    signature_ = sig;
    make_sheet(library);
    sheet_changed_ = true;
}

void Painter::make_sheet(const objects::Library& library) {
    struct Piece {
        u32 w, h;
        std::vector<u8> rgba;
    };
    std::vector<Piece> pieces;
    pieces.push_back({4, 4, std::vector<u8>(64, 255)});
    // The tiles: ground 1…4, then obstacles 1…4.
    for (u32 layer : {kGround, kObstacles})
        for (world::TileId v = 1; v <= 4; ++v) {
            Piece p{kTilePx, kTilePx, std::vector<u8>(static_cast<usize>(kTilePx) * kTilePx * 4)};
            tile_picture(layer, v, kTilePx, p.rgba, kTilePx, 0, 0);
            pieces.push_back(std::move(p));
        }
    figures_ = static_cast<u32>(pieces.size());
    for (Thing::Kind k : {Thing::Kind::Hero, Thing::Kind::Villager, Thing::Kind::Pickup, Thing::Kind::Place}) {
        Piece p{kThingPx / 2, kThingPx / 2, std::vector<u8>(static_cast<usize>(kThingPx / 2) * (kThingPx / 2) * 4)};
        figure_picture(k, kThingPx / 2, p.rgba, kThingPx / 2, 0, 0);
        pieces.push_back(std::move(p));
    }
    pics_.clear();
    for (const objects::Template& t : library.templates()) {
        const fs::path file = library.picture_file(t);
        if (file.empty()) continue;
        Cut cut;
        if (!cut_picture(file, t.frames, kThingPx, cut)) {
            FORGE_WARN("RPG: картинка «%s» шаблона «%s» не читается, он нарисован фигурой", t.picture.c_str(), t.name.c_str());
            continue;
        }
        Pic pic;
        pic.first = static_cast<u32>(pieces.size());
        pic.count = cut.frames;
        pic.aspect = static_cast<f32>(cut.h) / static_cast<f32>(cut.w);
        for (std::vector<u8>& px : cut.rgba) pieces.push_back({cut.w, cut.h, std::move(px)});
        pics_[t.key] = pic;
    }
    // Shelves of pieces, a pixel apart.
    rects_.clear();
    u32 x = 1, y = 1, shelf = 0;
    for (const Piece& p : pieces) {
        if (x + p.w + 1 > kSheetW) x = 1, y += shelf + 1, shelf = 0;
        rects_.push_back({x, y, p.w, p.h});
        x += p.w + 1;
        shelf = std::max(shelf, p.h);
    }
    width_ = kSheetW;
    height_ = y + shelf + 1;
    rgba_.assign(static_cast<usize>(width_) * height_ * 4, 0);
    for (usize i = 0; i < pieces.size(); ++i)
        for (u32 py = 0; py < pieces[i].h; ++py)
            std::copy_n(&pieces[i].rgba[static_cast<usize>(py) * pieces[i].w * 4], pieces[i].w * 4,
                        &rgba_[(static_cast<usize>(rects_[i].y + py) * width_ + rects_[i].x) * 4]);
}

u32 Painter::tile(u32 layer, world::TileId value) const {
    if (value < 1 || value > 4) return white();
    return 1 + (layer == kGround ? 0 : 4) + (value - 1);
}

u32 Painter::frame(const objects::Template& t, const objects::Library& library, u32 n) const {
    if (auto it = pics_.find(t.key); it != pics_.end()) return it->second.first + std::min(n, it->second.count - 1);
    bool known = false;
    const Thing::Kind k = kind_of(library, t, known);
    return figures_ + static_cast<u32>(k);
}

u32 Painter::frames(const objects::Template& t) const {
    auto it = pics_.find(t.key);
    return it == pics_.end() ? 1 : it->second.count;
}

f32 Painter::aspect(const objects::Template& t) const {
    auto it = pics_.find(t.key);
    return it == pics_.end() ? 1.0f : it->second.aspect;
}

void Painter::begin(const render::Camera2D& camera) { batch_.begin(camera.snapped_x(), camera.snapped_y(), 16384); }

void Painter::put(u32 frame, f64 x, f64 y, f32 w, f32 h, u32 color, u32 order) {
    render::Sprite s;
    s.x = static_cast<f32>(x - batch_.origin_x());
    s.y = static_cast<f32>(y - batch_.origin_y());
    s.w = w;
    s.h = h;
    s.frame = frame;
    s.color = color;
    s.order = order;
    batch_.push(s);
}

void Painter::put_thing(const objects::Template& t, const objects::Library& library, u32 frame_no, f64 x, f64 y,
                        bool mirrored, u32 order, u32 color) {
    const f32 h = aspect(t);
    put(frame(t, library, frame_no), x, y - h * 0.5, mirrored ? -1.0f : 1.0f, h, color, order);
}

void Painter::prepare(SDL_GPUCommandBuffer* cmd, const render::Camera2D& camera, u32 width, u32 height) {
    if (!ready_) return;
    if (sheet_changed_) {
        render::SpriteSheet sheet{rgba_.data(), width_, height_, rects_.data(), static_cast<u32>(rects_.size()), false};
        if (sprites_.set_sheet(sheet)) sheet_changed_ = false;
    }
    sprites_.prepare(cmd, batch_, camera, width, height);
}

void Painter::draw(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass) {
    if (ready_) sprites_.draw(cmd, pass);
}

// --- the level module ------------------------------------------------------------------------------------------------------

RpgLevel::RpgLevel() : gen_(std::make_shared<world::EmptyGenerator>()) {
    tiles_.push_back({"grass", "Трава", "Земля", "по ней ходят", kGround, kGrass, "1"});
    tiles_.push_back({"path", "Дорога", "Земля", "по ней ходят", kGround, kPath, "2"});
    tiles_.push_back({"sand", "Песок", "Земля", "по нему ходят", kGround, kSand, "3"});
    tiles_.push_back({"water", "Вода", "Земля", "по ней не пройти; где земли нет совсем — край карты", kGround, kWater, "4"});
    tiles_.push_back({"wall", "Стена", "Препятствия", "сквозь неё не пройти", kObstacles, kWall, "5"});
    tiles_.push_back({"tree", "Дерево", "Препятствия", "сквозь него не пройти", kObstacles, kTree, "6"});
    tiles_.push_back({"fence", "Забор", "Препятствия", "сквозь него не пройти", kObstacles, kFence, "7"});
    tiles_.push_back({"rock", "Камень", "Препятствия", "сквозь него не пройти", kObstacles, kRock, "8"});
}

RpgLevel::~RpgLevel() = default;

bool RpgLevel::load_objects(const fs::path& game_dir, std::string* error) {
    return library_.load(game_dir / "kinds.json", game_dir / "objects", error);
}

world::WorldDesc RpgLevel::world_desc() const {
    world::WorldDesc d;
    d.layer_count = 2;
    return d;
}

void RpgLevel::setup_scene(scene::Scene& scene) {
    register_components(scene);
    library_.attach(scene);
}

void RpgLevel::tile_icon(const level::TileDef& tile, u32 size, std::vector<u8>& rgba) const {
    rgba.assign(static_cast<usize>(size) * size * 4, 0);
    if (!size) return;
    // An obstacle on grass, as it stands on the map.
    if (tile.layer == kObstacles) tile_picture(kGround, kGrass, size, rgba, size, 0, 0);
    std::vector<u8> top(rgba.size(), 0);
    tile_picture(tile.layer, tile.value, size, top, size, 0, 0);
    for (usize i = 0; i < rgba.size(); i += 4)
        if (top[i + 3]) std::copy_n(&top[i], 4, &rgba[i]);
}

void RpgLevel::start(f64& x, f64& y) const {
    x = start_x_;
    y = start_y_;
}

u32 RpgLevel::map_color(u32 layer, world::TileId value) const {
    if (value < 1 || value > 4) return 0;
    const Rgb c = tile_rgb(layer, value);
    return render::pack_color(c[0], c[1], c[2]);
}

Color RpgLevel::background() const { return {0.07f, 0.08f, 0.10f, 1.0f}; }

bool RpgLevel::init_view(SDL_GPUDevice* device, SDL_GPUTextureFormat format) { return painter_.init(device, format); }

void RpgLevel::shutdown_view() { painter_.shutdown(); }

void RpgLevel::prepare_view(SDL_GPUCommandBuffer* cmd, level::Level& level, const render::Camera2D& camera, u32 width,
                            u32 height, const level::ViewOptions&, f64) {
    if (!painter_.ready()) return;
    painter_.update(library_);
    painter_.begin(camera);
    const world::Rect view = camera.visible_tiles(width, height);
    painter_.put_tiles(view, [&](u32 layer, i32 x, i32 y) { return level.tile(layer, x, y); });
    // The copies by their pictures, lower ones in front; places as squares under them.
    const f32 view_h = static_cast<f32>(view.y1 - view.y0);
    bool hero = false;
    level.scene().ecs().each([&](flecs::entity, const objects::ObjectRef& ref, const scene::Position& p) {
        const objects::Template* t = library_.find(ref.key);
        if (!t) return;
        bool known = false;
        const Thing::Kind k = kind_of(library_, *t, known);
        if (!known) return;
        const f64 cx = std::floor(p.tile_x()) + 0.5, feet = std::floor(p.tile_y()) + 1.0;
        if (k == Thing::Kind::Place) {
            painter_.put(painter_.frame(*t, library_, 0), cx, feet - 0.5, 0.9f, 0.9f, render::pack_color(255, 255, 255, 200), 2);
            return;
        }
        if (k == Thing::Kind::Hero && !hero) {
            hero = true;
            start_x_ = cx;
            start_y_ = feet - 0.5;
        }
        painter_.put_thing(*t, library_, 0, cx, feet, false, render::draw_order(1, static_cast<f32>(feet - view.y0), view_h));
    });
    painter_.prepare(cmd, camera, width, height);
}

void RpgLevel::draw_view(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass) { painter_.draw(cmd, pass); }

const std::vector<level::ObjectDef>& RpgLevel::objects() const {
    if (defs_version_ != library_.version()) {
        defs_version_ = library_.version();
        defs_.clear();
        for (const objects::Template& t : library_.templates()) {
            const objects::KindDef* k = library_.kind_of(t);
            if (!k || !k->placed) continue;
            defs_.push_back({t.id, t.name, k->group, t.about, t.key});
        }
    }
    return defs_;
}

void RpgLevel::object_icon(const level::ObjectDef& def, u32 size, std::vector<u8>& rgba) const {
    rgba.assign(static_cast<usize>(size) * size * 4, 0);
    const objects::Template* t = def.tmpl ? def.tmpl : library_.find(def.key);
    if (!t || !size) return;
    Cut cut;
    const fs::path file = library_.picture_file(*t);
    if (!file.empty() && cut_picture(file, t->frames, size, cut)) {
        const u32 ox = (size - cut.w) / 2, oy = (size - cut.h) / 2;
        for (u32 y = 0; y < cut.h; ++y)
            std::copy_n(&cut.rgba[0][static_cast<usize>(y) * cut.w * 4], cut.w * 4, &rgba[(static_cast<usize>(oy + y) * size + ox) * 4]);
        return;
    }
    bool known = false;
    figure_picture(kind_of(library_, *t, known), size, rgba, size, 0, 0);
}

void RpgLevel::hero_icon(u32 size, std::vector<u8>& rgba) const {
    rgba.assign(static_cast<usize>(size) * size * 4, 0);
    if (size) figure_picture(Thing::Kind::Hero, size, rgba, size, 0, 0);
}

flecs::entity RpgLevel::place_object(level::Level& level, usize index, f64 x, f64 y) {
    const std::vector<level::ObjectDef>& defs = objects();
    if (index >= defs.size()) return {};
    const objects::Template* t = library_.find(defs[index].key);
    if (!t) return {};
    return library_.spawn(level.scene(), *t, std::floor(x) + 0.5, std::floor(y) + 1.0);
}

i32 RpgLevel::object_kind(flecs::entity e) const {
    const objects::Template* t = library_.template_of(e);
    if (!t) return -1;
    const std::vector<level::ObjectDef>& defs = objects();
    for (usize i = 0; i < defs.size(); ++i)
        if (defs[i].key == t->key) return static_cast<i32>(i);
    return -1;
}

bool RpgLevel::object_box(flecs::entity e, f64& x0, f64& y0, f64& x1, f64& y1) const {
    const scene::Position* p = e.try_get<scene::Position>();
    if (!p || !library_.template_of(e)) return false;
    x0 = std::floor(p->tile_x()) + 0.05;
    y0 = std::floor(p->tile_y()) + 0.05;
    x1 = x0 + 0.9;
    y1 = y0 + 0.9;
    return true;
}

std::vector<level::LevelModule::AnimState> RpgLevel::anim_states(const objects::Library& library, const objects::Template& t) const {
    return anim_states_of(library, t);
}

std::string RpgLevel::map_problem(level::Level& level, const std::string& name) const {
    MapData m;
    std::string why;
    return read_map(level, library_, name, m, why) ? std::string() : why;
}

} // namespace rpg
