#include "rpg_testgame.h"

#include "rpg_level.h"

#include "forge/core/file.h"
#include "forge/core/path.h"
#include "forge/editor/project.h"
#include "forge/editor/ui_design.h"
#include "forge/level/levels.h"
#include "forge/logic/logic.h"
#include "forge/render/offscreen.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace rpg {

namespace fs = std::filesystem;
namespace d = forge::editor::design;
using namespace forge;

namespace {

bool write_text(const fs::path& file, std::string_view text, std::string& why) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (write_file_atomic(file, {reinterpret_cast<const u8*>(text.data()), text.size()})) return true;
    why = "не записан " + path_to_utf8(file);
    return false;
}

// --- the pictures -------------------------------------------------------------------------------------------------

// A picture of frames side by side, each a shape of its colour with a darker rim; the middle of each frame is its
// colour exactly (what the test's pixels look for). Placeholders, not art.
enum class Shape : u8 { Figure, Box, Disc };
struct Paint {
    const char* file;
    u32 w, h; // of a frame
    Shape shape;
    std::vector<std::array<u8, 3>> frames;
};

bool write_picture(const fs::path& file, const Paint& p, std::string& why) {
    const u32 n = static_cast<u32>(p.frames.size()), w = p.w * n;
    std::vector<u8> rgba(static_cast<usize>(w) * p.h * 4, 0);
    for (u32 f = 0; f < n; ++f)
        for (u32 y = 0; y < p.h; ++y)
            for (u32 x = 0; x < p.w; ++x) {
                const f64 u = (x + 0.5) / p.w, v = (y + 0.5) / p.h;
                bool in = false, rim = false;
                switch (p.shape) {
                case Shape::Figure: {
                    const f64 head = std::hypot((u - 0.5) * p.w / p.h, v - 0.2), body = std::hypot((u - 0.5) * 1.25, (v - 0.64) * 0.9);
                    in = head < 0.14 || body < 0.34;
                    rim = in && head >= 0.11 && body >= 0.3;
                    break;
                }
                case Shape::Box: {
                    const f64 r = std::max(std::fabs(u - 0.5) * 2, std::fabs(v - 0.55) * 2.4);
                    in = r < 0.9;
                    rim = r > 0.75;
                    break;
                }
                case Shape::Disc: {
                    const f64 r = std::hypot(u - 0.5, v - 0.55);
                    in = r < 0.33;
                    rim = r > 0.27;
                    break;
                }
                }
                if (!in) continue;
                u8* px = &rgba[(static_cast<usize>(y) * w + f * p.w + x) * 4];
                for (int i = 0; i < 3; ++i) px[i] = rim ? static_cast<u8>(p.frames[f][i] * 0.55) : p.frames[f][i];
                px[3] = 255;
            }
    if (render::write_png(path_to_utf8(file).c_str(), w, p.h, rgba)) return true;
    why = "не записана картинка " + path_to_utf8(file);
    return false;
}

// --- the maps -----------------------------------------------------------------------------------------------------

// Rows of a map from its top left at (0, 0): '.' grass, '=' a path, 's' sand, '~' water; '#' a wall, 'T' a tree,
// 'F' a fence, 'R' a rock, each on grass.
struct Put {
    const char* id;
    i32 x, y;
};
struct MapSpec {
    const char* id;
    const char* name;
    std::vector<const char*> rows;
    std::vector<Put> things;
};

std::vector<MapSpec> maps() {
    return {
        {"village",
         "Деревня",
         {"TTTTTTTTTTTTTTTTTTTTTTTT", "T.........s~~~~s.......T", "T.........s~~~~s.......T", "T.........ss~~ss.......T",
          "T.####....FFF..FFF.....T", "T.####.................T", "T.####.................T", "T......................T",
          "T=======================", "T......................T", "T......................T", "T......................T",
          "T......................T", "TTTTTTTTTTTTTTTTTTTTTTTT"},
         {{"hero", 4, 10}, {"elder", 7, 6}, {"guard", 21, 7}, {"dog", 18, 11}, {"key", 15, 2}, {"coins", 9, 11}, {"gate", 23, 8}}},
        {"glade",
         "Поляна",
         {"TTTTTTTTTTTT", "T..........T", "T..RR......T", "T..........T", "T....~~....T", "T....~~....T", "T..........T",
          "T..........T", "TTTTTTTTTTTT"},
         {{"hero", 2, 6}, {"chest", 9, 2}}},
    };
}

u64 object_id(usize level, usize n) { return 0x144a000000000000ull | (static_cast<u64>(level + 1) << 16) | (n + 1); }

bool make_map(const fs::path& game, const fs::path& dir, const MapSpec& s, usize index, std::string& why) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!level::save_world(dir, level::LevelWorld{true}, &why)) return false;
    RpgLevel module;
    if (!module.load_objects(game, &why)) return false;
    level::Level lv(module);
    if (!lv.open(dir, &why)) return false;
    lv.ensure_loaded(world::Rect{-8, -8, 40, 40});
    for (usize y = 0; y < s.rows.size(); ++y)
        for (usize x = 0; s.rows[y][x]; ++x) {
            const char c = s.rows[y][x];
            const world::TileId ground = c == '=' ? kPath : c == 's' ? kSand : c == '~' ? kWater : kGrass;
            const world::TileId top = c == '#' ? kWall : c == 'T' ? kTree : c == 'F' ? kFence : c == 'R' ? kRock : 0;
            const i32 tx = static_cast<i32>(x), ty = static_cast<i32>(y);
            if (!lv.set_tile(kGround, tx, ty, ground) || (top && !lv.set_tile(kObstacles, tx, ty, top))) {
                why = std::string(s.name) + ": клетка не нарисована";
                return false;
            }
        }
    const objects::Library& library = module.library_ref();
    usize n = 0;
    for (const Put& p : s.things) {
        const objects::Template* t = library.find(std::string_view(p.id));
        flecs::entity e = t ? library.spawn(lv.scene(), *t, p.x + 0.5, p.y + 1.0) : flecs::entity{};
        if (!e.is_valid()) {
            why = std::string(s.name) + ": не поставлен " + p.id;
            return false;
        }
        e.set<level::LevelId>({object_id(index, n++)});
    }
    lv.touch_objects();
    const level::Level::SaveReport r = lv.save();
    if (!r.ok) why = std::string(s.name) + ": " + r.error;
    return r.ok;
}

// --- the story ------------------------------------------------------------------------------------------------------

const char* const kQuests = R"json([
  {
    "id": "key",
    "title": "Ключ от ворот",
    "var": "quest.key",
    "stages": [
      {"at": 1, "name": "ищет ключ", "text": "Найти ключ старосты у пруда за забором и принести ему."}
    ],
    "done_at": 2, "done_name": "выполнено",
    "done_text": "Ключ у старосты. Ворота на востоке открыты."
  }
]
)json";

const char* const kElder = R"json({
  "id": "elder",
  "speakers": {"elder": {"name": "Староста", "color": "#e8b04a"}},
  "start": [
    {"if": "quest.key >= 2", "goto": "thanks"},
    {"if": "quest.key >= 1", "goto": "waiting"},
    {"goto": "hello"}
  ],
  "nodes": [
    {"id": "hello", "speaker": "elder",
     "text": "Здравствуй, путник! Я обронил ключ от ворот у пруда, за забором. Найдёшь?",
     "choices": [
       {"text": "Найду.", "do": "quest.key = 1", "goto": "go"},
       {"text": "Не сейчас.", "goto": "end"}
     ]},
    {"id": "go", "speaker": "elder", "text": "Пруд к северу. В заборе есть проход."},
    {"id": "waiting", "branches": [{"if": "has(\"key\")", "goto": "give_back"}], "next": "not_yet"},
    {"id": "not_yet", "speaker": "elder", "text": "Ключ лежит у пруда, к северу."},
    {"id": "give_back", "speaker": "elder", "text": "Мой ключ! Держи тридцать монет. Ворота на востоке, ступай.",
     "do": "quest.key = 2; take(\"key\"); give(\"coins\", 30)"},
    {"id": "thanks", "speaker": "elder", "text": "Ворота на востоке, ступай."}
  ]
}
)json";

const char* const kGuard = R"json({
  "id": "guard",
  "speakers": {"guard": {"name": "Стражник", "color": "#8ab4f8"}},
  "start": [{"goto": "hello"}],
  "nodes": [
    {"id": "hello", "speaker": "guard", "text": "Стой! Ворота за мной. Без дела не ходи.",
     "choices": [
       {"text": "Понял.", "goto": "end"},
       {"text": "Сдаюсь, иду домой.", "goto": "give_up"}
     ]},
    {"id": "give_up", "speaker": "guard", "text": "Вот и правильно. Ступай домой.", "do": "lose()"}
  ]
}
)json";

// --- the screens ------------------------------------------------------------------------------------------------------

constexpr d::Color kInk{24, 30, 26, 255}, kPanel{24, 30, 26, 200}, kWhite{255, 255, 255, 255}, kGold{255, 213, 74, 255},
                   kSoft{222, 232, 220, 255}, kButton{242, 179, 61, 255}, kButtonDark{70, 86, 72, 255};

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
    // Top left: the map's name, and what the hero carries.
    // The list puts its cells in a grid of the cell's own size: two to a line.
    d::Node cell = row(id, "Вещь", {text(id, "Название", "{item.name} ×{item.count}", 26, 600, kSoft)}, 0, {6, 12, 6, 12});
    cell.width_sizing = cell.height_sizing = d::Sizing::Fixed;
    cell.w = 198;
    cell.h = 48;
    cell.radius = {12, 12, 12, 12};
    cell.fills = {solid(kButtonDark)};
    d::Node list;
    list.id = id++;
    list.type = d::NodeType::Frame;
    list.name = "Инвентарь";
    list.list = d::ListSource::Items;
    list.list_gap = 8;
    list.w = 420;
    list.h = 160;
    d::Node empty = text(id, "Пока пусто", "Сумка пуста", 24, 500, kSoft);
    empty.x = 8;
    empty.y = 8;
    cell.x = cell.y = 8;
    list.children = {cell, empty};
    d::Node panel;
    panel.id = id++;
    panel.name = "Сумка";
    panel.x = 24;
    panel.y = 24;
    panel.w = 460;
    panel.h = 240;
    panel.radius = {20, 20, 20, 20};
    panel.fills = {solid(kPanel)};
    panel.layout.mode = d::LayoutMode::Column;
    panel.layout.gap = 10;
    panel.layout.padding = {16, 20, 16, 20};
    panel.children = {text(id, "Карта", "{rpg.map}", 34, 800, kGold), list};
    // Bottom middle, near a villager: how to talk.
    d::Node hint = row(id, "Подсказка", {text(id, "Слова", "E — поговорить", 32, 700, kWhite)}, 0, {12, 28, 12, 28});
    hint.width_sizing = d::Sizing::Fixed;
    hint.w = 400;
    hint.h = 64;
    hint.x = (1920 - hint.w) / 2;
    hint.y = 1080 - 64 - 40;
    hint.horizontal = d::Constraint::Center;
    hint.vertical = d::Constraint::End;
    hint.radius = {18, 18, 18, 18};
    hint.fills = {solid(kPanel)};
    hint.show_if = "rpg.near > 0";
    s.root.children = {panel, hint};
    s.next_id = id;
    return s;
}

// A window the game shows itself: kind "level" (a map passed), "win", "lose".
d::Screen window(const char* kind) {
    const std::string k = kind;
    d::Screen s = d::make_screen(k == "level" ? "Уровень пройден" : k == "win" ? "Победа" : "Поражение", 1920, 1080);
    s.show = d::ScreenShow::Command;
    s.ending = k == "level" ? d::WindowEnding::Level : k == "win" ? d::WindowEnding::Win : d::WindowEnding::Lose;
    s.over = d::WindowOver::Game;
    s.pauses = true;
    s.esc_closes = false;
    s.dim = true;
    u32 id = 2;
    d::Node panel;
    panel.id = id++;
    panel.name = "Окно";
    panel.w = 760;
    panel.h = 400;
    panel.x = (1920 - panel.w) / 2;
    panel.y = (1080 - panel.h) / 2;
    panel.horizontal = panel.vertical = d::Constraint::Center;
    panel.radius = {28, 28, 28, 28};
    panel.fills = {solid(kInk)};
    panel.layout.mode = d::LayoutMode::Column;
    panel.layout.gap = 22;
    panel.layout.padding = {44, 40, 44, 40};
    panel.layout.align = 1;
    const char* title = k == "level" ? "Карта пройдена!" : k == "win" ? "Победа!" : "Поражение";
    const char* words = k == "level" ? "«{rpg.map}» позади. Дальше — новая карта." : k == "win" ? "Сундук найден. Монет: {inv.coins}."
                                                                                       : "Герой ушёл домой.";
    std::vector<d::Node> buttons;
    if (k == "level") buttons.push_back(button(id, "Дальше", d::ActionKind::NextLevel, kButton, kInk));
    buttons.push_back(button(id, "В меню", d::ActionKind::Menu, kButtonDark, kWhite));
    panel.children = {text(id, "Заголовок", title, 72, 800, k == "lose" ? kWhite : kGold), text(id, "Итог", words, 32, 500, kSoft),
                      row(id, "Кнопки", std::move(buttons), 28, {18, 0, 0, 0})};
    s.root.children = {panel};
    s.next_id = id;
    return s;
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

bool make_in(const fs::path& games, const fs::path& out, std::string& why) {
    const fs::path game = out / "game";
    std::error_code ec;
    fs::create_directories(game / "objects", ec);
    if (ec) {
        why = "не создана папка " + path_to_utf8(game) + ": " + ec.message();
        return false;
    }
    if (!write_text(game / "game.json",
                    "{\n  \"title\": \"RPG сверху: проба\",\n  \"org\": \"Forge\",\n  \"theme\": \"fantasy\",\n"
                    "  \"autosave_minutes\": 5,\n  \"module\": \"rpg\"\n}\n",
                    why))
        return false;
    for (const char* file : editor::project::kModuleFiles) {
        fs::copy_file(games / "modules" / "rpg" / file, game / file, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            why = std::string("не скопирован ") + file + ": " + ec.message();
            return false;
        }
    }
    if (!write_text(game / "quests.json", kQuests, why) || !write_text(game / "dialogues" / "elder.json", kElder, why) ||
        !write_text(game / "dialogues" / "guard.json", kGuard, why))
        return false;
    // The hero of three frames (stands, two steps), the elder, the dog of two, the key, the chest; the guard, the coins
    // and the gate have none (they are figures).
    const std::vector<Paint> paints = {
        {"герой.png", 48, 64, Shape::Figure, {{{52, 96, 220}}, {{80, 130, 240}}, {{30, 70, 180}}}},
        {"староста.png", 48, 64, Shape::Figure, {{{200, 120, 60}}}},
        {"пёс.png", 48, 40, Shape::Box, {{{140, 110, 80}}, {{160, 128, 92}}}},
        {"ключ.png", 32, 32, Shape::Disc, {{{236, 196, 40}}}},
        {"сундук.png", 48, 40, Shape::Box, {{{150, 90, 40}}}},
    };
    for (const Paint& p : paints)
        if (!write_picture(game / "pictures" / utf8_path(p.file), p, why)) return false;
    {
        objects::Library library;
        if (!library.load(game / "kinds.json", game / "objects", &why)) return false;
        struct Spec {
            const char* id;
            const char* name;
            const char* kind;
            const char* picture;
            u32 frames;
            std::vector<std::pair<std::string, std::string>> values;
        };
        const Spec specs[] = {
            {"hero", "Герой", "hero", "герой.png", 3, {{"speed", "4"}}},
            {"elder", "Староста", "villager", "староста.png", 1, {{"talk", "\"elder\""}}},
            {"guard", "Стражник", "villager", "", 1, {{"talk", "\"guard\""}}},
            {"dog", "Пёс", "villager", "пёс.png", 2, {}},
            {"key", "Ключ", "pickup", "ключ.png", 1, {{"what", "\"key\""}, {"count", "1"}}},
            {"coins", "Монеты", "pickup", "", 1, {{"what", "\"coins\""}, {"count", "5"}}},
            {"chest", "Сундук", "pickup", "сундук.png", 1, {{"what", "\"treasure\""}, {"count", "1"}}},
            {"gate", "Ворота", "place", "", 1, {{"shown", "true"}}},
        };
        for (const Spec& s : specs) {
            objects::Template t;
            t.id = s.id;
            t.name = s.name;
            t.kind = s.kind;
            t.genre = "RPG";
            t.picture = s.picture;
            t.frames = s.frames;
            t.values = s.values;
            t.file = game / "objects" / (std::string(s.id) + ".object.json");
            if (!library.put(t, &why)) return false;
        }
    }
    level::LevelList list;
    const std::vector<MapSpec> specs = maps();
    for (const MapSpec& s : specs) list.levels.push_back({s.id, s.name});
    list.start = specs.front().id;
    if (!level::write_levels(game, list, &why)) return false;
    for (usize i = 0; i < specs.size(); ++i)
        if (!make_map(game, level::level_folder(game, specs[i].id), specs[i], i, why)) return false;
    // The links: the gate passes the village, the chest the glade (the last map: the game is won).
    logic::Logic links;
    for (auto [verb, b] : {std::pair{"reaches", "gate"}, {"finds", "chest"}}) {
        logic::Link l;
        l.a = "hero";
        l.verb = verb;
        l.b = b;
        links.add(l);
    }
    links.set_spot("hero", 60, 200);
    links.set_spot("gate", 300, 120);
    links.set_spot("chest", 300, 280);
    if (!links.save(game / "logic.json", &why)) return false;
    return write_screen(game, "hud", hud(), why) && write_screen(game, "level", window("level"), why) &&
           write_screen(game, "win", window("win"), why) && write_screen(game, "lose", window("lose"), why);
}

} // namespace

bool make_test_game(const fs::path& games, const fs::path& out, std::string& why) {
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

} // namespace rpg
