#include "match3_level.h"

#include "forge/assets/image.h"
#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/world/generators.h"

#include <yyjson.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <map>

FORGE_REFLECT(match3::Gem, 1) {
    t.field("points", &match3::Gem::points).label("Очки").range(0, 10000);
    t.field("sound", &match3::Gem::sound).label("Звук сбора");
    t.field("on_board", &match3::Gem::on_board).label("На поле");
}
FORGE_REFLECT(match3::BoardRules, 1) {
    t.field("moves", &match3::BoardRules::moves).label("Ходов").range(1, 999);
    t.field("target", &match3::BoardRules::target).label("Цель, очков").range(1, 1000000);
    t.field("seed", &match3::BoardRules::seed).label("Зерно");
    t.field("four", &match3::BoardRules::four).label("За 4 подряд").range(1, 10);
    t.field("five", &match3::BoardRules::five).label("За 5 подряд").range(1, 10);
    t.field("chain", &match3::BoardRules::chain).label("Множитель цепочки").range(0, 5);
}

namespace match3 {

namespace fs = std::filesystem;
using namespace forge;

void register_components(scene::Scene& scene) {
    scene.register_component<Gem>();
    scene.register_component<BoardRules>();
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

// A template's value of a property (its own, else its kind's), JSON; "" without the property.
std::string template_value(const objects::Library& library, const objects::Template& t, std::string_view prop) {
    const objects::PropDef* p = library.prop_of(t, prop);
    return p ? library.value(t, *p) : std::string();
}

// The colours of gems without a picture (RGB), one per gem number.
constexpr u8 kColors[kMaxGems][3] = {{220, 50, 60}, {50, 110, 230}, {50, 190, 80}, {240, 200, 40},
                                     {160, 70, 210}, {245, 130, 30}, {40, 200, 210}, {240, 110, 180}};

// A disc of a colour in a square of size px, transparent around it.
void disc(u32 size, const u8 rgb[3], std::vector<u8>& out, u32 stride, u32 ox, u32 oy) {
    const f64 r = size * 0.42, c = size * 0.5;
    for (u32 y = 0; y < size; ++y)
        for (u32 x = 0; x < size; ++x) {
            const f64 dx = x + 0.5 - c, dy = y + 0.5 - c;
            const f64 d = std::sqrt(dx * dx + dy * dy);
            u8* p = &out[(static_cast<usize>(oy + y) * stride + ox + x) * 4];
            if (d > r) {
                p[0] = p[1] = p[2] = p[3] = 0;
                continue;
            }
            // A lighter spot up left, so it reads as a gem, not a dot.
            const bool shine = dx < -r * 0.2 && dy < -r * 0.2 && dx > -r * 0.6 && dy > -r * 0.6;
            for (int i = 0; i < 3; ++i) p[i] = shine ? static_cast<u8>(std::min(255, rgb[i] + 70)) : rgb[i];
            p[3] = 255;
        }
}

// A picture cut into its frames, each scaled to fit side × side (nearest pixel): frames side by side.
struct Cut {
    u32 frames = 0, w = 0, h = 0; // one frame's size
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

// The cell tile's picture, size px: light or dark, with a thin edge.
void cell_picture(bool dark, u32 size, std::vector<u8>& out, u32 stride, u32 ox, u32 oy) {
    const u8 fill = dark ? 70 : 92, edge = dark ? 52 : 70;
    for (u32 y = 0; y < size; ++y)
        for (u32 x = 0; x < size; ++x) {
            const bool border = x == 0 || y == 0 || x == size - 1 || y == size - 1;
            u8* p = &out[(static_cast<usize>(oy + y) * stride + ox + x) * 4];
            const u8 v = border ? edge : fill;
            p[0] = v;
            p[1] = static_cast<u8>(v + 8);
            p[2] = static_cast<u8>(v + 30);
            p[3] = 255;
        }
}

constexpr u32 kGemPx = 64;  // the largest side a gem's frame keeps in the sheet
constexpr u32 kCellPx = 32; // a cell's picture
constexpr u32 kSheetW = 1024;

} // namespace

// --- the set ---------------------------------------------------------------------------------------------------------

i32 GemSet::index(std::string_view template_id) const {
    for (usize i = 0; i < gems.size(); ++i)
        if (gems[i]->id == template_id) return static_cast<i32>(i);
    return -1;
}

GemSet gem_set(const objects::Library& library) {
    GemSet set;
    for (const objects::Template& t : library.templates()) {
        if (!library.has_block(t, kGemBlock)) continue;
        if (!json_bool(template_value(library, t, "on_board"), true)) continue;
        set.gems.push_back(&t);
    }
    std::sort(set.gems.begin(), set.gems.end(), [](const objects::Template* a, const objects::Template* b) { return a->id < b->id; });
    for (const objects::Template* t : set.gems) {
        set.scoring.points.push_back(static_cast<u32>(std::max(0.0, json_number(template_value(library, *t, "points"), 10))));
        set.sounds.push_back(json_string(template_value(library, *t, "gem_sound")));
    }
    return set;
}

// --- the field ---------------------------------------------------------------------------------------------------------

Spot spot_of(world::TileId t, const level::LevelTiles& own) {
    if (is_cell(t)) return Spot::Cell;
    if (t < level::kFirstOwnTile) return Spot::None;
    const level::OwnTile* o = own.find(t);
    if (!o || o->layer != kFieldLayer) return Spot::None;
    return o->solid ? Spot::Stone : Spot::Cell;
}

bool read_field(level::Level& level, const objects::Library& library, const GemSet& set, const std::string& name, Field& out,
                std::string& why) {
    const std::string field = "Поле «" + name + "»: ";
    level.ensure_loaded(world::Rect{-kReach, -kReach, kReach, kReach});
    level.load_objects();
    const level::LevelTiles& own = level.own_tiles();
    i32 x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
    for (i32 y = -kReach; y < kReach; ++y)
        for (i32 x = -kReach; x < kReach; ++x)
            if (spot_of(level.tile(kFieldLayer, x, y), own) == Spot::Cell) {
                x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
            }
    if (x0 == INT_MAX) {
        why = field + "на слое «Поле» нет ни одной клетки";
        return false;
    }
    const i32 w = x1 - x0 + 1, h = y1 - y0 + 1;
    if (w > kMaxSide || h > kMaxSide) {
        why = field + "клеток " + std::to_string(w) + " × " + std::to_string(h) + ", а можно не больше " +
              std::to_string(kMaxSide) + " × " + std::to_string(kMaxSide);
        return false;
    }
    Field f;
    f.x0 = x0;
    f.y0 = y0;
    f.shape = Board(w, h, kHole);
    f.dark.assign(static_cast<usize>(w) * static_cast<usize>(h), false);
    f.look.assign(static_cast<usize>(w) * static_cast<usize>(h), 0);
    f.own = own;
    for (i32 y = 0; y < h; ++y)
        for (i32 x = 0; x < w; ++x) {
            const world::TileId tile = level.tile(kFieldLayer, x0 + x, y0 + y);
            const Spot spot = spot_of(tile, own);
            if (spot == Spot::Cell) f.shape.set(x, y, kNone);
            const usize i = static_cast<usize>(f.shape.index(x, y));
            f.dark[i] = tile == kCellDark;
            if (spot != Spot::None) f.look[i] = tile;
        }
    // The copies: gems put before, and the rules.
    std::string off, foreign, twice;
    std::map<i32, std::string> taken;
    std::map<u64, std::vector<std::pair<i32, i32>>> gone; // copies of objects the game has not (deleted after they were put)
    BoardRules kind_rules;
    level.scene().ecs().each([&](flecs::entity e, const objects::ObjectRef& ref, const scene::Position& p) {
        const objects::Template* t = library.find(ref.key);
        if (!t) {
            // Neither left out (the field would start other than the author left it) nor taken for another object.
            gone[ref.key].emplace_back(static_cast<i32>(std::floor(p.tile_y())) - y0, static_cast<i32>(std::floor(p.tile_x())) - x0);
            return;
        }
        if (library.has_block(*t, kRulesBlock)) {
            ++f.rules_copies;
            auto num = [&](std::string_view prop, f64 fallback) {
                const objects::PropDef* d = library.prop_of(*t, prop);
                return d ? json_number(library.value(level.scene(), e, *d), fallback) : fallback;
            };
            f.rules.moves = static_cast<u32>(std::max(1.0, num("moves", kind_rules.moves)));
            f.rules.target = static_cast<u64>(std::max(1.0, num("target", kind_rules.target)));
            f.rules.seed = static_cast<u64>(std::max(0.0, num("seed", kind_rules.seed)));
            f.rules.four = num("four", kind_rules.four);
            f.rules.five = num("five", kind_rules.five);
            f.rules.chain = num("chain", kind_rules.chain);
            return;
        }
        if (!library.has_block(*t, kGemBlock)) return;
        const i32 cx = static_cast<i32>(std::floor(p.tile_x())) - x0, cy = static_cast<i32>(std::floor(p.tile_y())) - y0;
        const std::string at = std::to_string(cx + 1) + ":" + std::to_string(cy + 1);
        if (!f.shape.cell(cx, cy)) {
            off += (off.empty() ? "" : ", ") + t->name + " в " + at;
            return;
        }
        const i32 gem = set.index(t->id);
        if (gem < 0) {
            foreign += (foreign.empty() ? "«" : ", «") + t->name + "» в " + at;
            return;
        }
        const i32 i = f.shape.index(cx, cy);
        if (taken.count(i)) twice += (twice.empty() ? "" : ", ") + at;
        taken[i] = t->id;
        f.shape.set(cx, cy, static_cast<u8>(gem));
    });
    if (!gone.empty()) {
        // Where, and the key the copies keep of their template (its id is not in the game any more).
        std::string where;
        for (auto& [key, cells] : gone) {
            std::sort(cells.begin(), cells.end());
            char hex[24];
            std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(key));
            std::string at;
            for (const auto& [y, x] : cells) at += (at.empty() ? "" : ", ") + std::to_string(x + 1) + ":" + std::to_string(y + 1);
            where += (where.empty() ? "" : "; ") + std::string(cells.size() == 1 ? "клетка " : "клетки ") + at + " (ключ шаблона " + hex + ")";
        }
        why = field + "копии объекта, которого больше нет в игре: " + where;
        return false;
    }
    if (!off.empty()) {
        why = field + "фишки стоят не на клетках поля (" + off + ")";
        return false;
    }
    if (!foreign.empty()) {
        why = field + "фишек нет в наборе поля, у них выключено «На поле» (" + foreign + ")";
        return false;
    }
    if (!twice.empty()) {
        why = field + "на клетке две фишки (" + twice + ")";
        return false;
    }
    if (f.rules_copies > 1) {
        why = field + "копий «Правил уровня» на уровне " + std::to_string(f.rules_copies) + ", а нужна одна";
        return false;
    }
    out = std::move(f);
    return true;
}

// --- drawing -------------------------------------------------------------------------------------------------------------

u32 Painter::placeholder(u32 gem) {
    const u8* c = kColors[gem % kMaxGems];
    return render::pack_color(c[0], c[1], c[2]);
}

bool Painter::init(SDL_GPUDevice* device, SDL_GPUTextureFormat format) {
    objects::Library none;
    make_sheet(none, GemSet{}, nullptr);
    render::SpriteSheet sheet{rgba_.data(), width_, height_, frames_.data(), static_cast<u32>(frames_.size()), false};
    ready_ = sprites_.init(device, format, sheet, 4096);
    sheet_changed_ = false;
    return ready_;
}

void Painter::shutdown() {
    if (ready_) sprites_.shutdown();
    ready_ = false;
}

void Painter::update(const objects::Library& library, const GemSet& set, const level::LevelTiles* own) {
    // What the sheet is made of: each gem's template, picture, its file's time and frames; the level's own tiles.
    const bool tiles_changed = own && !(*own == own_tiles_);
    std::string sig;
    for (const objects::Template* t : set.gems) {
        sig += t->id + "|" + t->picture + "|" + std::to_string(t->frames) + "|";
        std::error_code ec;
        const fs::path file = library.picture_file(*t);
        if (!file.empty()) sig += std::to_string(fs::last_write_time(file, ec).time_since_epoch().count());
        sig += ";";
    }
    if (sig == signature_ && !tiles_changed) return;
    signature_ = sig;
    make_sheet(library, set, own ? own : &own_tiles_);
    sheet_changed_ = true;
}

u32 Painter::own_frame(world::TileId id) const {
    const usize slot = id >= level::kFirstOwnTile ? id - level::kFirstOwnTile : own_.size();
    return slot < own_.size() ? own_[slot] : white();
}

void Painter::make_sheet(const objects::Library& library, const GemSet& set, const level::LevelTiles* own) {
    struct Piece {
        u32 w, h;
        std::vector<u8> rgba;
    };
    std::vector<Piece> pieces;
    {
        Piece white{4, 4, std::vector<u8>(64, 255)};
        pieces.push_back(std::move(white));
        for (bool dark : {false, true}) {
            Piece c{kCellPx, kCellPx, std::vector<u8>(static_cast<usize>(kCellPx) * kCellPx * 4)};
            cell_picture(dark, kCellPx, c.rgba, kCellPx, 0, 0);
            pieces.push_back(std::move(c));
        }
    }
    first_.clear();
    count_.clear();
    for (u32 g = 0; g < set.size(); ++g) {
        first_.push_back(static_cast<u32>(pieces.size()));
        Cut cut;
        const objects::Template& t = *set.gems[g];
        const fs::path file = library.picture_file(t);
        if (!file.empty() && cut_picture(file, t.frames, kGemPx, cut)) {
            for (std::vector<u8>& px : cut.rgba) pieces.push_back({cut.w, cut.h, std::move(px)});
            count_.push_back(cut.frames);
        } else {
            if (!file.empty()) FORGE_WARN("Три в ряд: картинка «%s» фишки «%s» не читается, фишка нарисована кругом", t.picture.c_str(), t.name.c_str());
            Piece d{kGemPx, kGemPx, std::vector<u8>(static_cast<usize>(kGemPx) * kGemPx * 4)};
            disc(kGemPx, kColors[g % kMaxGems], d.rgba, kGemPx, 0, 0);
            pieces.push_back(std::move(d));
            count_.push_back(1);
        }
    }
    // The level's own tiles: each its picture.
    own_.clear();
    if (own) {
        if (&own_tiles_ != own) own_tiles_ = *own;
        for (usize i = 0; i < own_tiles_.tiles.size(); ++i) {
            const usize slot = own_tiles_.tiles[i].id - level::kFirstOwnTile;
            if (own_.size() <= slot) own_.resize(slot + 1, white());
            own_[slot] = static_cast<u32>(pieces.size());
            const u32 px = own_tiles_.px;
            const u8* from = own_tiles_.picture(i);
            pieces.push_back({px, px, std::vector<u8>(from, from + static_cast<usize>(px) * px * 4)});
        }
    } else {
        own_tiles_ = {};
    }
    // Shelves of pieces, a pixel apart.
    frames_.clear();
    u32 x = 1, y = 1, shelf = 0;
    for (const Piece& p : pieces) {
        if (x + p.w + 1 > kSheetW) x = 1, y += shelf + 1, shelf = 0;
        frames_.push_back({x, y, p.w, p.h});
        x += p.w + 1;
        shelf = std::max(shelf, p.h);
    }
    width_ = kSheetW;
    height_ = y + shelf + 1;
    rgba_.assign(static_cast<usize>(width_) * height_ * 4, 0);
    for (usize i = 0; i < pieces.size(); ++i)
        for (u32 py = 0; py < pieces[i].h; ++py)
            std::copy_n(&pieces[i].rgba[static_cast<usize>(py) * pieces[i].w * 4], pieces[i].w * 4,
                        &rgba_[(static_cast<usize>(frames_[i].y + py) * width_ + frames_[i].x) * 4]);
}

u32 Painter::gem_frame(u32 gem, u32 frame) const {
    if (gem >= first_.size()) return white();
    return first_[gem] + std::min(frame, count_[gem] - 1);
}

u32 Painter::gem_frames(u32 gem) const { return gem < count_.size() ? count_[gem] : 1; }

std::vector<u8> Painter::gem_pixels(u32 gem, u32 frame, u32& w, u32& h) const {
    w = h = 0;
    const u32 at = gem_frame(gem, frame);
    if (gem >= first_.size() || at >= frames_.size()) return {};
    const render::SpriteRect& r = frames_[at];
    std::vector<u8> out;
    out.reserve(static_cast<usize>(r.w) * r.h * 4);
    for (u32 y = 0; y < r.h; ++y) {
        const u8* row = &rgba_[(static_cast<usize>(r.y + y) * width_ + r.x) * 4];
        out.insert(out.end(), row, row + static_cast<usize>(r.w) * 4);
    }
    w = r.w;
    h = r.h;
    return out;
}

void Painter::begin(const render::Camera2D& camera) { batch_.begin(camera.snapped_x(), camera.snapped_y(), 4096); }

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

void Painter::put_tile(world::TileId t, f64 x, f64 y, const level::LevelTiles& own) {
    const Spot spot = spot_of(t, own);
    if (spot == Spot::None) return;
    // A stone over what hides gems in holes (order 4), a cell under the gems.
    if (t >= level::kFirstOwnTile) put(own_frame(t), x, y, 1, 1, 0xffffffffu, spot == Spot::Stone ? 5 : 0);
    else put(cell(t == kCellDark), x, y, 1, 1, 0xffffffffu, 0);
}

void Painter::put_cells(const Field& field) {
    const Board& shape = field.shape;
    const i32 x0 = field.x0, y0 = field.y0;
    for (i32 y = 0; y < shape.h; ++y)
        for (i32 x = 0; x < shape.w; ++x) {
            const usize i = static_cast<usize>(shape.index(x, y));
            const world::TileId t = i < field.look.size() ? field.look[i] : 0;
            if (t >= level::kFirstOwnTile) {
                put_tile(t, x0 + x + 0.5, y0 + y + 0.5, field.own);
                continue;
            }
            if (!shape.cell(x, y)) continue;
            put(cell(i < field.dark.size() && field.dark[i]), x0 + x + 0.5, y0 + y + 0.5, 1, 1, 0xffffffffu, 0);
        }
    // The frame round the field.
    const u32 edge = render::pack_color(230, 200, 120, 255);
    const f32 t = 0.08f;
    const f64 cx = x0 + shape.w * 0.5, cy = y0 + shape.h * 0.5;
    put(white(), cx, y0 - t * 0.5, static_cast<f32>(shape.w) + 2 * t, t, edge, 5);
    put(white(), cx, y0 + shape.h + t * 0.5, static_cast<f32>(shape.w) + 2 * t, t, edge, 5);
    put(white(), x0 - t * 0.5, cy, t, static_cast<f32>(shape.h), edge, 5);
    put(white(), x0 + shape.w + t * 0.5, cy, t, static_cast<f32>(shape.h), edge, 5);
}

void Painter::put_covers(const Field& field, u32 color) {
    const Board& shape = field.shape;
    const i32 x0 = field.x0, y0 = field.y0;
    for (i32 y = 0; y < shape.h; ++y)
        for (i32 x = 0; x < shape.w; ++x)
            if (!shape.cell(x, y)) put(white(), x0 + x + 0.5, y0 + y + 0.5, 1, 1, color, 4);
    // Above the field, as high as a gem ever starts (a whole column of new ones).
    const f32 high = static_cast<f32>(shape.h) + 1;
    put(white(), x0 + shape.w * 0.5, y0 - high * 0.5, static_cast<f32>(shape.w) + 1, high, color, 4);
}

void Painter::prepare(SDL_GPUCommandBuffer* cmd, const render::Camera2D& camera, u32 width, u32 height) {
    if (!ready_) return;
    if (sheet_changed_) {
        render::SpriteSheet sheet{rgba_.data(), width_, height_, frames_.data(), static_cast<u32>(frames_.size()), false};
        if (sprites_.set_sheet(sheet)) sheet_changed_ = false;
    }
    sprites_.prepare(cmd, batch_, camera, width, height);
}

void Painter::draw(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass) {
    if (ready_) sprites_.draw(cmd, pass);
}

// --- the level module ------------------------------------------------------------------------------------------------------

Match3Level::Match3Level() : gen_(std::make_shared<world::EmptyGenerator>()) {
    tiles_.push_back({"cell", "Клетка", "Поле", "клетка поля: на ней стоят фишки; пустое место — дыра", kFieldLayer, kCell, "1"});
    tiles_.push_back({"cell_dark", "Клетка тёмная", "Поле", "такая же клетка, для шахматного узора", kFieldLayer, kCellDark, "2"});
}

Match3Level::~Match3Level() = default;

bool Match3Level::load_objects(const fs::path& game_dir, std::string* error) {
    u32 limit = objects::kDefaultFrameLimit;
    if (!objects::read_frame_limit(game_dir, limit, error)) return false;
    library_.set_frame_limit(limit);
    return library_.load(game_dir / "kinds.json", game_dir / "objects", error);
}

world::WorldDesc Match3Level::world_desc() const {
    world::WorldDesc d;
    d.layer_count = 1;
    return d;
}

void Match3Level::setup_scene(scene::Scene& scene) {
    register_components(scene);
    library_.attach(scene);
}

void Match3Level::tile_icon(const level::TileDef& tile, u32 size, std::vector<u8>& rgba) const {
    rgba.assign(static_cast<usize>(size) * size * 4, 0);
    if (size) cell_picture(tile.value == kCellDark, size, rgba, size, 0, 0);
}

void Match3Level::start(f64& x, f64& y) const {
    x = start_x_;
    y = start_y_;
}

u32 Match3Level::map_color(u32 layer, world::TileId value) const {
    if (layer != kFieldLayer || !is_cell(value)) return 0;
    return value == kCellDark ? render::pack_color(70, 78, 100) : render::pack_color(92, 100, 122);
}

Color Match3Level::background() const { return {0.11f, 0.10f, 0.16f, 1.0f}; }

bool Match3Level::init_view(SDL_GPUDevice* device, SDL_GPUTextureFormat format) { return painter_.init(device, format); }

void Match3Level::shutdown_view() { painter_.shutdown(); }

void Match3Level::prepare_view(SDL_GPUCommandBuffer* cmd, level::Level& level, const render::Camera2D& camera, u32 width,
                               u32 height, const level::ViewOptions&, f64) {
    if (!painter_.ready()) return;
    const GemSet set = gem_set(library_);
    const level::LevelTiles& own = level.own_tiles();
    painter_.update(library_, set, &own);
    painter_.begin(camera);
    // The cells in sight, as drawn (a field still being drawn may be bigger than any board), and the stones.
    const world::Rect view = camera.visible_tiles(width, height);
    i32 x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
    for (i32 y = view.y0; y < view.y1; ++y)
        for (i32 x = view.x0; x < view.x1; ++x) {
            const world::TileId t = level.tile(kFieldLayer, x, y);
            const Spot spot = spot_of(t, own);
            if (spot == Spot::None) continue;
            painter_.put_tile(t, x + 0.5, y + 0.5, own);
            if (spot != Spot::Cell) continue;
            x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
        }
    if (x0 != INT_MAX) {
        start_x_ = (x0 + x1 + 1) * 0.5;
        start_y_ = (y0 + y1 + 1) * 0.5;
        const u32 edge = render::pack_color(230, 200, 120, 255);
        const f32 t = 0.08f;
        const f64 w = x1 - x0 + 1, h = y1 - y0 + 1, cx = x0 + w * 0.5, cy = y0 + h * 0.5;
        painter_.put(painter_.white(), cx, y0 - t * 0.5, static_cast<f32>(w) + 2 * t, t, edge, 1);
        painter_.put(painter_.white(), cx, y1 + 1 + t * 0.5, static_cast<f32>(w) + 2 * t, t, edge, 1);
        painter_.put(painter_.white(), x0 - t * 0.5, cy, t, static_cast<f32>(h), edge, 1);
        painter_.put(painter_.white(), x1 + 1 + t * 0.5, cy, t, static_cast<f32>(h), edge, 1);
    }
    // The copies: gems by their pictures, the rules as a gold square.
    level.scene().ecs().each([&](flecs::entity, const objects::ObjectRef& ref, const scene::Position& p) {
        const objects::Template* t = library_.find(ref.key);
        if (!t) return;
        const f64 cx = std::floor(p.tile_x()) + 0.5, cy = std::floor(p.tile_y()) + 0.5;
        if (library_.has_block(*t, kRulesBlock)) {
            painter_.put(painter_.white(), cx, cy, 0.6f, 0.6f, render::pack_color(240, 190, 60, 230), 2);
            return;
        }
        const i32 gem = set.index(t->id);
        if (gem >= 0) painter_.put(painter_.gem_frame(static_cast<u32>(gem), 0), cx, cy, 0.86f, 0.86f, 0xffffffffu, 2);
        else if (library_.has_block(*t, kGemBlock)) // not on the board: drawn dim, so the author sees it is out
            painter_.put(painter_.white(), cx, cy, 0.5f, 0.5f, render::pack_color(150, 150, 150, 160), 2);
    });
    painter_.prepare(cmd, camera, width, height);
}

void Match3Level::draw_view(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass) { painter_.draw(cmd, pass); }

const std::vector<level::ObjectDef>& Match3Level::objects() const {
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

void Match3Level::object_icon(const level::ObjectDef& def, u32 size, std::vector<u8>& rgba) const {
    rgba.assign(static_cast<usize>(size) * size * 4, 0);
    const objects::Template* t = def.tmpl ? def.tmpl : library_.find(def.key);
    if (!t || !size) return;
    if (library_.has_block(*t, kRulesBlock)) {
        for (u32 i = 0; i < size * size; ++i) {
            const u32 x = i % size, y = i / size;
            if (x < size / 8 || y < size / 8 || x >= size - size / 8 || y >= size - size / 8) continue;
            rgba[i * 4] = 240, rgba[i * 4 + 1] = 190, rgba[i * 4 + 2] = 60, rgba[i * 4 + 3] = 255;
        }
        return;
    }
    Cut cut;
    const fs::path file = library_.picture_file(*t);
    if (!file.empty() && cut_picture(file, t->frames, size, cut)) {
        const u32 ox = (size - cut.w) / 2, oy = (size - cut.h) / 2;
        for (u32 y = 0; y < cut.h; ++y)
            std::copy_n(&cut.rgba[0][static_cast<usize>(y) * cut.w * 4], cut.w * 4, &rgba[(static_cast<usize>(oy + y) * size + ox) * 4]);
        return;
    }
    const GemSet set = gem_set(library_);
    const i32 gem = set.index(t->id);
    disc(size, kColors[(gem < 0 ? 0 : gem) % kMaxGems], rgba, size, 0, 0);
}

flecs::entity Match3Level::place_object(level::Level& level, usize index, f64 x, f64 y) {
    const std::vector<level::ObjectDef>& defs = objects();
    if (index >= defs.size()) return {};
    const objects::Template* t = library_.find(defs[index].key);
    if (!t) return {};
    // (x, y) are the feet (LevelModule::place_object): the gem stands in the middle of the cell they are in, a point on
    // a cell's bottom line in the cell above it (the editor gives the line under the cell clicked).
    const f64 row = std::ceil(y - 1e-6) - 1;
    return library_.spawn(level.scene(), *t, std::floor(x) + 0.5, row + 1.0);
}

i32 Match3Level::object_kind(flecs::entity e) const {
    const objects::Template* t = library_.template_of(e);
    if (!t) return -1;
    const std::vector<level::ObjectDef>& defs = objects();
    for (usize i = 0; i < defs.size(); ++i)
        if (defs[i].key == t->key) return static_cast<i32>(i);
    return -1;
}

bool Match3Level::object_box(flecs::entity e, f64& x0, f64& y0, f64& x1, f64& y1) const {
    const scene::Position* p = e.try_get<scene::Position>();
    if (!p || !library_.template_of(e)) return false;
    x0 = std::floor(p->tile_x()) + 0.07;
    y0 = std::floor(p->tile_y()) + 0.07;
    x1 = x0 + 0.86;
    y1 = y0 + 0.86;
    return true;
}

std::string Match3Level::field_problem(level::Level& level, const std::string& name) const {
    const GemSet set = gem_set(library_);
    Field f;
    std::string why;
    if (!read_field(level, library_, set, name, f, why)) return why;
    Board board = f.shape;
    Rng rng(f.rules.seed ? f.rules.seed : 1);
    if (!fill_start(board, rng, set.size(), name, why)) return why;
    return {};
}

} // namespace match3
