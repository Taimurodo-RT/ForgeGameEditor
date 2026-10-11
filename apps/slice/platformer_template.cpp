#include "platformer_template.h"

#include "slice_level.h"
#include "slice_world.h"

#include "forge/assets/image.h"
#include "forge/core/file.h"
#include "forge/core/path.h"
#include "forge/editor/project.h"
#include "forge/editor/sources.h"
#include "forge/editor/ui_design.h"
#include "forge/level/areas.h"
#include "forge/level/level.h"
#include "forge/level/levels.h"
#include "forge/logic/logic.h"
#include "forge/objects/library.h"
#include "forge/sim/bodies.h"

#include <yyjson.h>

#include <algorithm>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace platformer_template {

namespace fs = std::filesystem;
namespace d = forge::editor::design;
using namespace forge;

namespace {

// --- the levels ---------------------------------------------------------------

// The own tiles, in the order of tiles.png (ids from 256): 32 px each, as the brief has them.
constexpr u32 kTilePx = 32;
enum : world::TileId {
    Grass = 256, GrassLeft, GrassRight, Dirt, DirtLeft, DirtRight, BridgeLeft, Bridge, BridgeRight, Stone,
    Shade, Bush, Flowers, CloudLeft, CloudRight, FarRock
};
struct TileName {
    const char* name;
    bool solid;
};
constexpr TileName kTiles[] = {
    {"Трава", true},          {"Трава, левый край", true},  {"Трава, правый край", true}, {"Земля", true},
    {"Земля, левая стенка", true}, {"Земля, правая стенка", true}, {"Мостки, левый край", true}, {"Мостки", true},
    {"Мостки, правый край", true}, {"Камень", true},     {"Земля в тени", false},     {"Куст", false},
    {"Цветы", false},         {"Облако, слева", false},     {"Облако, справа", false},  {"Камень вдали", false},
};

// Every level: the ground's top is row 0 where nothing else is said (y grows down), the ground goes down to kDeep; a
// pit is a column with no ground, its floor kDeep - 1 .. kDeep of stone, and falling into it is the area «Пропасть»
// (rows 2 .. kDeep - 1). A cliff kCliff rows high closes each end, and the meadow on top of it goes on for kOutside
// columns, so the camera at an end shows ground, not the empty world.
constexpr i32 kDeep = 20, kCliff = 6, kOutside = 34;

struct Ground {
    i32 x0, x1, top; // columns x0..x1
};
struct Row {
    i32 x0, x1, y; // a bridge of columns x0..x1 on row y
};
struct Cell {
    i32 x, y;
    world::TileId tile;
};
struct Put {
    const char* id; // the template's
    f64 x;          // its middle
    i32 feet;       // the row its feet stand on (the top of that row)
};
struct Zone {
    const char* name;
    i32 x0, y0, x1, y1;
};
struct LevelSpec {
    const char* id;
    const char* name;
    i32 width; // columns 0..width-1 inside; stone at -1, 0 and width, width + 1
    std::vector<Ground> ground;
    std::vector<Row> bridges;
    std::vector<Cell> cells; // stones of the blocks and pictures of the walls (bushes, flowers, clouds), as they are
    std::vector<Put> objects;
    std::vector<Zone> zones; // [x0, x1) × [y0, y1); «Пропасть» is added
};

// The three levels. Each goes left to right; a sign stands near the start, coins show the way.
std::vector<LevelSpec> levels() {
    std::vector<LevelSpec> out;
    {
        LevelSpec l{"level", "Луг", 96, {}, {}, {}, {}, {}};
        l.ground = {{1, 19, 0}, {22, 45, 0}, {50, 69, 0}, {70, 70, -1}, {71, 77, -2}, {78, 78, -1}, {79, 95, 0}};
        l.bridges = {{36, 40, -3}};
        l.cells = {{34, -1, Stone}};
        for (i32 x : {4, 12, 15, 24, 31, 52, 58, 66, 81, 90}) l.cells.push_back({x, -1, Flowers});
        for (i32 x : {8, 27, 55, 84}) l.cells.push_back({x, -1, Bush});
        for (i32 x : {17, 42, 64}) l.cells.push_back({x, -1, FarRock});
        for (auto [x, y] : {std::pair{5, -7}, {24, -8}, {47, -6}, {68, -8}, {86, -7}})
            l.cells.push_back({x, y, CloudLeft}), l.cells.push_back({x + 1, y, CloudRight});
        l.objects = {{"sign", 6.5, 0},     {"coin", 9.5, 0},  {"coin", 11.5, 0},  {"coin", 13.5, 0},  {"beetle", 30.5, 0},
                     {"coin", 37.5, -3},   {"coin", 38.5, -3}, {"coin", 39.5, -3}, {"beetle", 60.5, 0}, {"coin", 73.5, -2},
                     {"coin", 75.5, -2},   {"sign", 88.5, 0}};
        l.zones = {{"Выход на Холмы", 92, -4, 96, 0}};
        out.push_back(std::move(l));
    }
    {
        LevelSpec l{"hills", "Холмы", 81, {}, {}, {}, {}, {}};
        l.ground = {{1, 12, 0}, {13, 13, -1}, {14, 24, -2}, {29, 36, -2}, {37, 37, -1}, {38, 52, 0}, {63, 80, 0}};
        l.bridges = {{53, 56, 0}, {59, 62, 0}};
        for (i32 x : {3, 9, 17, 22, 31, 41, 49, 65, 72, 79}) {
            const i32 top = x >= 14 && x <= 36 ? -2 : 0;
            l.cells.push_back({x, top - 1, Flowers});
        }
        for (auto [x, top] : {std::pair{11, 0}, {20, -2}, {44, 0}, {69, 0}}) l.cells.push_back({x, top - 1, Bush});
        for (auto [x, top] : {std::pair{7, 0}, {35, -2}, {76, 0}}) l.cells.push_back({x, top - 1, FarRock});
        for (auto [x, y] : {std::pair{8, -8}, {26, -9}, {45, -7}, {58, -9}, {70, -8}})
            l.cells.push_back({x, y, CloudLeft}), l.cells.push_back({x + 1, y, CloudRight});
        l.objects = {{"sign", 6.5, 0},  {"coin", 10.5, 0}, {"coin", 18.5, -2}, {"coin", 20.5, -2},
                     {"spikes", 33.5, -2}, {"hedgehog", 46.5, 0}, {"coin", 54.5, 0}, {"coin", 61.5, 0},
                     {"coin", 66.5, 0}, {"coin", 68.5, 0}, {"coin", 70.5, 0}, {"sign", 74.5, 0}};
        l.zones = {{"Вход", 1, -3, 5, 0}, {"Выход на Вершину", 77, -4, 81, 0}};
        out.push_back(std::move(l));
    }
    {
        LevelSpec l{"summit", "Вершина", 81, {}, {}, {}, {}, {}};
        l.ground = {{1, 22, 0}, {45, 60, 0}, {61, 61, -1}, {62, 62, -2}, {63, 80, -2}};
        l.bridges = {{26, 28, -1}, {32, 34, -2}, {38, 40, -1}};
        for (auto [x, top] : {std::pair{3, 0}, {9, 0}, {17, 0}, {47, 0}, {56, 0}, {66, -2}, {73, -2}})
            l.cells.push_back({x, top - 1, Flowers});
        for (auto [x, top] : {std::pair{10, 0}, {58, 0}, {68, -2}}) l.cells.push_back({x, top - 1, Bush});
        for (auto [x, top] : {std::pair{20, 0}, {51, 0}, {79, -2}}) l.cells.push_back({x, top - 1, FarRock});
        for (auto [x, y] : {std::pair{6, -8}, {24, -7}, {36, -9}, {52, -8}, {72, -9}})
            l.cells.push_back({x, y, CloudLeft}), l.cells.push_back({x + 1, y, CloudRight});
        l.objects = {{"sign", 6.5, 0},  {"spikes_row", 13.5, 0}, {"beetle", 19.5, 0}, {"coin", 27.5, -1},
                     {"coin", 33.5, -2}, {"coin", 39.5, -1},     {"coin", 48.5, 0},   {"coin", 50.5, 0},
                     {"hedgehog", 53.5, 0}, {"spikes", 70.5, -2},  {"flag", 77.5, -2}};
        l.zones = {{"Вход", 1, -3, 5, 0}};
        out.push_back(std::move(l));
    }
    return out;
}

// Ids that stay the same each time the template is made: of the level's objects (LevelId), of its areas, and the
// little randomness of each copy (a critter's).
u64 object_id(usize level, usize n) { return 0x142d000000000000ull | (static_cast<u64>(level + 1) << 16) | (n + 1); }
u64 area_id(usize level, usize n) { return 0x142d00a000000000ull | (static_cast<u64>(level + 1) << 8) | (n + 1); }
std::string area_thing(u64 id) { return std::string(logic::kAreaPrefix) + level::area_id_text(id); }

class Painter {
public:
    explicit Painter(const LevelSpec& spec) : spec_(spec) {
        for (const Ground& g : spec.ground)
            for (i32 x = g.x0; x <= g.x1; ++x) top_[x] = g.top;
        const i32 left = top_of(1).value_or(0) - kCliff, right = top_of(spec.width - 1).value_or(0) - kCliff;
        for (i32 i = 0; i <= kOutside; ++i) top_[-i] = left, top_[spec.width + i] = right;
    }

    // Every cell of the level, to level.set_tile.
    template <class Set>
    void paint(Set&& set) const {
        for (i32 x = -kOutside; x <= spec_.width + kOutside; ++x) {
            const std::optional<i32> top = top_of(x);
            if (!top) { // a pit: in shade from below its edges, its floor of stone
                for (i32 y = pit_edge(x) + 1; y < kDeep - 1; ++y) set(slice::kWalls, x, y, Shade);
                for (i32 y = kDeep - 1; y <= kDeep; ++y) set(slice::kBlocks, x, y, Stone);
                continue;
            }
            for (i32 y = *top; y <= kDeep; ++y) {
                const bool left = open(x - 1, y), right = open(x + 1, y);
                world::TileId t;
                if (y == *top) t = left && !right ? GrassLeft : right && !left ? GrassRight : Grass;
                else if (left && right) t = Stone;
                else t = left ? DirtLeft : right ? DirtRight : Dirt;
                set(slice::kBlocks, x, y, t);
            }
        }
        for (const Row& r : spec_.bridges)
            for (i32 x = r.x0; x <= r.x1; ++x) set(slice::kBlocks, x, r.y, x == r.x0 ? BridgeLeft : x == r.x1 ? BridgeRight : Bridge);
        for (const Cell& c : spec_.cells) set(c.tile == Stone ? slice::kBlocks : slice::kWalls, c.x, c.y, c.tile);
        for (i32 i = 2; i <= kOutside; ++i)
            for (i32 x : {-i, spec_.width + i})
                if (const world::TileId t = i % 5 == 2 ? Flowers : i % 11 == 4 ? Bush : 0) set(slice::kWalls, x, *top_of(x) - 1, t);
    }

private:
    std::optional<i32> top_of(i32 x) const {
        auto it = top_.find(x);
        return it == top_.end() ? std::nullopt : std::optional<i32>(it->second);
    }
    // No ground at (x, y): a pit above its floor, or above the ground's top. Past the ends: ground.
    bool open(i32 x, i32 y) const {
        if (x < -kOutside || x > spec_.width + kOutside) return false;
        const std::optional<i32> top = top_of(x);
        return top ? y < *top : y < kDeep - 1;
    }
    // The lower of the tops either side of the pit x is in.
    i32 pit_edge(i32 x) const {
        i32 l = x, r = x;
        while (!top_of(l)) --l;
        while (!top_of(r)) ++r;
        return std::max(*top_of(l), *top_of(r));
    }

    const LevelSpec& spec_;
    std::map<i32, i32> top_;
};

bool make_level(const fs::path& game, const LevelSpec& spec, usize index, const level::LevelTiles& tiles, std::string& why) {
    const fs::path dir = level::level_folder(game, spec.id);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!level::save_world(dir, level::LevelWorld{true}, &why)) return false;
    slice::SliceLevel module;
    if (!module.load_objects(game, &why)) return false;
    level::Level level(module);
    if (!level.open(dir, &why)) return false;
    if (!level.set_own_tiles(tiles)) {
        why = std::string(spec.name) + ": тайлы не ложатся в уровень";
        return false;
    }
    level.ensure_loaded(world::Rect{-4 - kOutside, -kCliff - 16, spec.width + 4 + kOutside, kDeep + 4});
    bool painted = true;
    Painter(spec).paint([&](u32 layer, i32 x, i32 y, world::TileId t) { painted &= level.set_tile(layer, x, y, t); });
    if (!painted) {
        why = std::string(spec.name) + ": не все клетки нарисованы";
        return false;
    }
    level::LevelAreas areas;
    std::vector<Zone> zones = spec.zones;
    zones.push_back({"Пропасть", 0, 2, spec.width, kDeep - 1});
    for (usize i = 0; i < zones.size(); ++i)
        areas.areas.push_back({area_id(index, i), zones[i].name, zones[i].x0, zones[i].y0, zones[i].x1, zones[i].y1, {}});
    areas.spawn = true;
    areas.spawn_x = 3.5;
    areas.spawn_y = 0;
    level.set_areas(areas);
    const objects::Library& library = *module.library();
    for (usize i = 0; i < spec.objects.size(); ++i) {
        const Put& p = spec.objects[i];
        flecs::entity e = slice::spawn_object(library, level.scene(), p.id, p.x, p.feet, slice::hash32(0x142d, static_cast<u32>(index * 100 + i)));
        if (!e.is_valid()) {
            why = std::string(spec.name) + ": не поставлен «" + p.id + "»";
            return false;
        }
        // What does not fall stands on the floor exactly (the kind's foot is a guess for what falls).
        const objects::Template* t = library.template_of(e);
        if (const sim::Body* b = e.try_get<sim::Body>(); b && b->gravity == 0 && t)
            e.set<scene::Position>(scene::Position::at_tile(p.x, p.feet - static_cast<f64>(b->half_h)));
        e.set<level::LevelId>({object_id(index, i)});
    }
    level.touch_objects();
    const level::Level::SaveReport r = level.save();
    if (!r.ok) why = std::string(spec.name) + ": " + r.error;
    return r.ok;
}

// The atlas of the levels' tiles: 16 pictures of 32 px in a row.
bool read_tiles(const fs::path& file, level::LevelTiles& out, std::string& why) {
    std::vector<u8> bytes;
    assets::CookedTexture img;
    std::string error;
    if (!read_file(file, bytes) || !assets::decode_image(bytes, img, &error)) {
        why = "не читается " + path_to_utf8(file) + " " + error;
        return false;
    }
    constexpr u32 n = std::size(kTiles);
    if (img.width != n * kTilePx || img.height != kTilePx) {
        why = path_to_utf8(file) + ": нужен " + std::to_string(n * kTilePx) + " × " + std::to_string(kTilePx) + ", а он " +
              std::to_string(img.width) + " × " + std::to_string(img.height);
        return false;
    }
    out.px = kTilePx;
    for (u32 i = 0; i < n; ++i) {
        out.tiles.push_back({static_cast<world::TileId>(level::kFirstOwnTile + i), kTiles[i].name,
                             kTiles[i].solid ? slice::kBlocks : slice::kWalls, kTiles[i].solid,
                             "шаблон «Платформер», tiles.png, клетка " + std::to_string(i + 1)});
        for (u32 y = 0; y < kTilePx; ++y) {
            const u8* row = &img.rgba8[(static_cast<usize>(y) * img.width + i * kTilePx) * 4];
            out.rgba.insert(out.rgba.end(), row, row + kTilePx * 4);
        }
    }
    return true;
}

// --- the objects ----------------------------------------------------------------

struct TemplateSpec {
    const char* file; // in objects/
    const char* id;
    const char* name;
    const char* kind;
    const char* about;
    const char* picture;
    u32 frames;
    std::vector<std::pair<std::string, std::string>> values;
};

std::vector<TemplateSpec> templates() {
    return {
        {"hero", "hero_look", "Герой", "hero", "чем игра рисует героя: стоит, шаг, шаг, в воздухе", "герой.png", 4, {}},
        {"beetle", "beetle", "Жук", "enemy", "бродит, побеждается прыжком сверху, 100 очков", "жук.png", 2, {{"speed", "1.5"}}},
        {"hedgehog", "hedgehog", "Ёж", "enemy", "колючий: прыжком не победить, только перепрыгнуть", "еж.png", 2,
         {{"stomp", "false"}, {"speed", "1"}}},
        {"coin", "coin", "Монетка", "pickup", "одна монетка и 10 очков в счёт", "монета.png", 1,
         {{"what", "\"coins\""}, {"count", "1"}, {"score", "10"}}},
        {"spikes", "spikes", "Шипы", "trap", "шипы в одну клетку", "шипы.png", 1, {{"half_height", "0.5"}}},
        {"spikes_row", "spikes_row", "Ряд шипов", "trap", "шипы в три клетки шириной", "ряд_шипов.png", 1,
         {{"half_height", "0.5"}, {"half_width", "1.5"}}},
        {"flag", "flag", "Флаг", "picture", "цель игры: герой доходит до флага, и это победа", "флаг.png", 1, {{"half_height", "1"}}},
        {"sign", "sign", "Указатель", "picture", "показывает, куда идти", "указатель.png", 1, {}},
    };
}

// --- the screens ------------------------------------------------------------------

constexpr d::Color kInk{43, 29, 46, 255}, kPanel{43, 29, 46, 200}, kWhite{255, 255, 255, 255}, kGold{255, 213, 74, 255},
                   kSoft{238, 228, 210, 255}, kButton{242, 179, 61, 255}, kButtonDark{88, 66, 92, 255};

d::Paint solid(d::Color c) {
    d::Paint p;
    p.color = c;
    return p;
}

d::Node text(u32& id, std::string name, std::string words, f32 size, u16 weight, d::Color color) {
    d::Node n;
    n.id = id++;
    n.name = std::move(name);
    n.type = d::NodeType::Text;
    n.text = std::move(words);
    n.text_style.family = "";
    n.text_style.size = size;
    n.text_style.weight = weight;
    n.text_style.color = color;
    n.width_sizing = d::Sizing::Hug;
    n.height_sizing = d::Sizing::Hug;
    n.w = size * 0.6f * static_cast<f32>(n.text.size() / 2 + 1);
    n.h = size * 1.3f;
    return n;
}

d::Node picture(u32& id, std::string name, std::string file, f32 side) {
    d::Node n;
    n.id = id++;
    n.name = std::move(name);
    n.type = d::NodeType::Image;
    n.w = n.h = side;
    d::Paint p;
    p.kind = d::PaintKind::Image;
    p.image = "pictures/" + file;
    p.fit = d::ImageFit::Fit;
    n.fills = {p};
    return n;
}

d::Node row(u32& id, std::string name, std::vector<d::Node> children, f32 gap, std::array<f32, 4> padding) {
    d::Node n;
    n.id = id++;
    n.name = std::move(name);
    n.layout.mode = d::LayoutMode::Row;
    n.layout.gap = gap;
    n.layout.padding = padding;
    n.layout.align = 4;
    n.width_sizing = d::Sizing::Hug;
    n.height_sizing = d::Sizing::Hug;
    n.children = std::move(children);
    return n;
}

d::Node button(u32& id, std::string words, d::ActionKind action, d::Color fill, d::Color ink) {
    d::Node t = text(id, words, words, 34, 700, ink);
    d::Node b = row(id, "Кнопка «" + words + "»", {}, 0, {14, 36, 14, 36});
    b.width_sizing = b.height_sizing = d::Sizing::Fixed;
    b.w = 280;
    b.h = 76;
    b.radius = {18, 18, 18, 18};
    b.fills = {solid(fill)};
    b.on_click = {{action, {}}};
    b.children = {t};
    return b;
}

d::Screen hud() {
    d::Screen s = d::make_screen("HUD", 1920, 1080);
    s.show = d::ScreenShow::Playing;
    u32 id = 2;
    d::Node counts = row(id, "Счёт",
                         {picture(id, "Сердце", "сердце.png", 48), text(id, "Сердца", "{hero.hearts}", 40, 800, kWhite),
                          picture(id, "Монета", "монета.png", 48), text(id, "Монеты", "{inv.coins}", 40, 800, kWhite),
                          text(id, "Очки", "Очки: {hero.score}", 34, 700, kGold)},
                         14, {10, 28, 10, 18});
    counts.x = 32;
    counts.y = 96; // under the «F2 Связи» badge the editor's «Играть» shows (its size does not scale)
    counts.w = 520;
    counts.h = 68;
    counts.radius = {20, 20, 20, 20};
    counts.fills = {solid(kPanel)};
    d::Node hint = row(id, "Подсказка",
                       {text(id, "Управление", "Бег: стрелки или A и D. Прыжок: пробел. Цель: флаг в конце «Вершины».", 26, 500,
                             kSoft)},
                       0, {10, 24, 10, 24});
    hint.width_sizing = d::Sizing::Fixed;
    hint.w = 1000;
    hint.h = 54;
    hint.x = 1920 - 32 - hint.w;
    hint.y = 32; // the module's own HUD (hotbar, coins, place) is along the bottom
    hint.horizontal = d::Constraint::End;
    hint.radius = {16, 16, 16, 16};
    hint.fills = {solid(kPanel)};
    s.root.children = {counts, hint};
    s.next_id = id;
    return s;
}

d::Screen ending(bool win) {
    d::Screen s = d::make_screen(win ? "Победа" : "Поражение", 1920, 1080);
    s.show = d::ScreenShow::Command;
    s.ending = win ? d::WindowEnding::Win : d::WindowEnding::Lose;
    s.over = d::WindowOver::Game;
    s.pauses = true;
    s.esc_closes = false;
    s.dim = true;
    s.appear = d::Appear::Zoom;
    u32 id = 2;
    d::Node panel;
    panel.id = id++;
    panel.name = "Окно";
    panel.w = 760;
    panel.h = 420;
    panel.x = (1920 - panel.w) / 2;
    panel.y = (1080 - panel.h) / 2;
    panel.horizontal = panel.vertical = d::Constraint::Center;
    panel.radius = {28, 28, 28, 28};
    panel.fills = {solid(kInk)};
    d::Stroke edge;
    edge.color = win ? kGold : d::Color{214, 92, 92, 255};
    edge.width = 4;
    panel.strokes = {edge};
    panel.layout.mode = d::LayoutMode::Column;
    panel.layout.gap = 22;
    panel.layout.padding = {44, 40, 44, 40};
    panel.layout.align = 1; // top centre
    d::Node title = text(id, "Заголовок", win ? "Победа!" : "Сердца кончились", 72, 800, win ? kGold : kWhite);
    d::Node line = text(id, "Итог", win ? "Флаг взят. Очки: {hero.score}, монет: {inv.coins}." : "Очки: {hero.score}. Попробуйте ещё раз.",
                        32, 500, kSoft);
    d::Node buttons = row(id, "Кнопки",
                          {button(id, "Ещё раз", d::ActionKind::NewGame, kButton, kInk),
                           button(id, "В меню", d::ActionKind::Menu, kButtonDark, kWhite)},
                          28, {18, 0, 0, 0});
    panel.children = {title, line, buttons};
    s.root.children = {panel};
    s.next_id = id;
    return s;
}

bool write_text(const fs::path& file, std::string_view text, std::string& why) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (write_file_atomic(file, {reinterpret_cast<const u8*>(text.data()), text.size()})) return true;
    why = "не записан " + path_to_utf8(file);
    return false;
}

bool write_screen(const fs::path& game, const std::string& name, const d::Screen& s, std::string& why) {
    const d::Screen library = [] {
        d::Screen l;
        l.library = true;
        return l;
    }();
    d::HtmlOptions options;
    options.library = &library;
    return write_text(game / "ui" / utf8_path(name + ".json"), d::save_screen(s), why) &&
           write_text(game / "ui" / utf8_path(name + ".html"), d::screen_html(s, options), why);
}

// The Guid a .meta of «Ресурсы» keeps.
std::optional<Guid> meta_id(const fs::path& meta) {
    std::vector<u8> bytes;
    if (!read_file(meta, bytes)) return std::nullopt;
    yyjson_doc* doc = yyjson_read(reinterpret_cast<const char*>(bytes.data()), bytes.size(), 0);
    const char* id = doc ? yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "id")) : nullptr;
    std::optional<Guid> out = id ? Guid::parse(id) : std::nullopt;
    yyjson_doc_free(doc);
    return out;
}

// The game made in out, a folder of its own (made anew).
bool make_in(const fs::path& games, const fs::path& out, std::string& why) {
    const fs::path module = games / "modules" / "slice", from = games / "templates" / "platformer";
    std::error_code ec;
    fs::create_directories(out / "objects", ec);
    if (ec) {
        why = "не создана папка " + path_to_utf8(out) + ": " + ec.message();
        return false;
    }
    // The game, its camera at 32 px of the screen a tile: the pictures' own pixels. Its module, named (step 14.3a).
    if (!write_text(out / "game.json",
                    "{\n  \"title\": \"Платформер\",\n  \"org\": \"Forge\",\n  \"theme\": \"fantasy\",\n  \"autosave_minutes\": 5,\n"
                    "  \"zoom\": 32,\n  \"module\": \"slice\"\n}\n",
                    why))
        return false;
    for (const char* file : editor::project::kModuleFiles) {
        fs::copy_file(module / file, out / file, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            why = std::string("не скопирован ") + file + ": " + ec.message();
            return false;
        }
    }

    // The pictures: copies of the template's «Ресурсы», as the editor makes them (sources.json).
    const fs::path pictures = from / "assets" / utf8_path("картинки");
    std::vector<fs::path> files;
    for (fs::directory_iterator it(pictures, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".png") files.push_back(it->path());
    std::sort(files.begin(), files.end());
    editor::sources::Sources sources;
    if (files.empty() || !sources.load(out, &why)) {
        if (files.empty()) why = "нет картинок в " + path_to_utf8(pictures);
        return false;
    }
    for (const fs::path& file : files) {
        std::vector<u8> bytes;
        fs::path meta = file;
        meta += ".meta";
        const std::optional<Guid> id = meta_id(meta);
        if (!id || !read_file(file, bytes)) {
            why = "нет .meta или не читается " + path_to_utf8(file);
            return false;
        }
        const editor::sources::Asset asset{*id, "картинки/" + path_to_utf8(file.filename()), file, editor::sources::hash_of(bytes)};
        if (sources.copy_in(asset, "pictures", &why) != path_to_utf8(file.filename())) {
            why = "картинка " + path_to_utf8(file.filename()) + " не скопирована: " + why;
            return false;
        }
    }

    // The templates.
    {
        objects::Library library;
        if (!library.load(out / "kinds.json", out / "objects", &why)) return false;
        for (const TemplateSpec& s : templates()) {
            objects::Template t;
            t.id = s.id;
            t.name = s.name;
            t.kind = s.kind;
            if (std::string_view(s.kind) != "hero" && std::string_view(s.kind) != "picture") t.genre = "Платформер";
            t.about = s.about;
            t.picture = s.picture;
            t.frames = s.frames;
            t.values = s.values;
            t.file = out / "objects" / (std::string(s.file) + ".object.json");
            if (!library.kind(t.kind)) {
                why = std::string("в модуле нет вида ") + s.kind;
                return false;
            }
            if (!library.put(t, &why)) return false;
        }
    }

    // The levels.
    level::LevelList list;
    const std::vector<LevelSpec> specs = levels();
    for (const LevelSpec& s : specs) list.levels.push_back({s.id, s.name});
    list.start = specs.front().id;
    if (!level::write_levels(out, list, &why)) return false;
    level::LevelTiles tiles;
    if (!read_tiles(from / "tiles.png", tiles, why)) return false;
    for (usize i = 0; i < specs.size(); ++i)
        if (!make_level(out, specs[i], i, tiles, why)) return false;

    // The links: from level to level through the exits, a fall into a pit costs a heart, the flag wins.
    logic::Logic links;
    auto zone = [&](usize level, const char* name) {
        const std::vector<Zone>& zones = specs[level].zones;
        for (usize i = 0; i < zones.size(); ++i)
            if (std::string_view(zones[i].name) == name) return area_thing(area_id(level, i));
        return area_thing(area_id(level, zones.size())); // «Пропасть», added last
    };
    auto link = [&](const char* verb, std::string b) -> logic::Link& {
        logic::Link l;
        l.a = std::string(logic::kHero);
        l.verb = verb;
        l.b = std::move(b);
        links.add(l);
        return links.links.back();
    };
    {
        logic::Link& l = link("go", zone(0, "Выход на Холмы"));
        l.level = specs[1].id;
        l.arrive = zone(1, "Вход");
    }
    {
        logic::Link& l = link("go", zone(1, "Выход на Вершину"));
        l.level = specs[2].id;
        l.arrive = zone(2, "Вход");
    }
    for (usize i = 0; i < specs.size(); ++i) link("fall", zone(i, "Пропасть"));
    link("win", "flag");
    // The board: the hero on the left, what it reaches in two columns.
    links.set_spot(logic::kHero, 60, 252);
    const std::string things[] = {zone(0, "Выход на Холмы"), zone(1, "Выход на Вершину"), "flag",
                                  zone(0, "Пропасть"),       zone(1, "Пропасть"),         zone(2, "Пропасть")};
    for (usize i = 0; i < std::size(things); ++i)
        links.set_spot(things[i], 292 + static_cast<f32>(i / 3) * 232, 60 + static_cast<f32>(i % 3) * 192);
    if (!links.save(out / "logic.json", &why)) return false;

    // The screens: the HUD, the windows of victory and defeat.
    return write_screen(out, "hud", hud(), why) && write_screen(out, "win", ending(true), why) &&
           write_screen(out, "lose", ending(false), why);
}

} // namespace

bool make(const fs::path& games, const fs::path& out, std::string& why) {
    // Made beside it first: what is in out stays as it was till the whole game is made.
    const fs::path target = out.has_filename() ? out : out.parent_path();
    const fs::path made = target.parent_path() / utf8_path(path_to_utf8(target.filename()) + ".сборка");
    std::error_code ec;
    fs::remove_all(made, ec);
    if (!make_in(games, made, why)) {
        fs::remove_all(made, ec);
        return false;
    }
    fs::remove_all(target, ec);
    if (!ec) fs::rename(made, target, ec);
    if (ec) {
        why = "игра собрана в " + path_to_utf8(made) + ", но не перенесена в " + path_to_utf8(target) + ": " + ec.message();
        return false;
    }
    return true;
}

} // namespace platformer_template
