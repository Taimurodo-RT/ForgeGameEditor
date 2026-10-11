#include "match3_testgame.h"

#include "match3_level.h"

#include "forge/core/file.h"
#include "forge/core/path.h"
#include "forge/editor/project.h"
#include "forge/editor/ui_design.h"
#include "forge/level/levels.h"
#include "forge/level/own_tiles.h"
#include "forge/logic/logic.h"
#include "forge/render/offscreen.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace match3 {

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

// A gem's picture: frames of 64 px side by side, each a rounded square of its colour (smaller frame by frame: it
// vanishes), a darker edge; the middle of each frame is its colour exactly (what the test's pixels look for).
struct Paint {
    const char* file;
    std::vector<std::array<u8, 3>> frames;
    bool diamond = false;
};

bool write_picture(const fs::path& file, const Paint& p, std::string& why) {
    constexpr u32 kSide = 64;
    const u32 n = static_cast<u32>(p.frames.size()), w = kSide * n;
    std::vector<u8> rgba(static_cast<usize>(w) * kSide * 4, 0);
    for (u32 f = 0; f < n; ++f) {
        const f64 half = 26.0 - f * 6.0; // 26, 20, 14
        for (u32 y = 0; y < kSide; ++y)
            for (u32 x = 0; x < kSide; ++x) {
                const f64 dx = std::fabs(x + 0.5 - 32), dy = std::fabs(y + 0.5 - 32);
                const f64 r = p.diamond ? dx + dy : std::max(dx, dy);
                const f64 edge = p.diamond ? half * 1.35 : half;
                if (r > edge) continue;
                const bool rim = r > edge - 3;
                u8* px = &rgba[(static_cast<usize>(y) * w + f * kSide + x) * 4];
                for (int i = 0; i < 3; ++i) px[i] = rim ? static_cast<u8>(p.frames[f][i] * 0.6) : p.frames[f][i];
                px[3] = 255;
            }
    }
    if (render::write_png(path_to_utf8(file).c_str(), w, kSide, rgba)) return true;
    why = "не записана картинка " + path_to_utf8(file);
    return false;
}

// --- the levels ---------------------------------------------------------------------------------------------------

struct Put {
    const char* id;
    i32 x, y;
};
struct LevelSpec {
    const char* id;
    const char* name;
    i32 w, h;
    std::vector<std::pair<i32, i32>> holes;
    std::vector<Put> gems;
    u32 moves, target, seed;
};

std::vector<LevelSpec> levels() {
    return {
        {"level", "Первый", 6, 6, {}, {}, 10, 150, 1001},
        {"second", "Второй", 7, 7, {{0, 0}, {6, 0}, {0, 6}, {6, 6}, {3, 3}}, {{"ruby", 1, 1}, {"ruby", 2, 1}, {"sapphire", 5, 5}}, 10, 200, 2002},
        {"third", "Третий", 8, 6, {{3, 0}, {4, 0}, {3, 1}, {4, 1}}, {}, 10, 200, 3003},
    };
}
// The field of the scene match3_lose: two moves for an aim it cannot reach.
LevelSpec short_level() { return {"short", "Короткий", 5, 5, {}, {}, 2, 5000, 4004}; }

u64 object_id(usize level, usize n) { return 0x143b000000000000ull | (static_cast<u64>(level + 1) << 16) | (n + 1); }

bool make_level(const fs::path& game, const fs::path& dir, const LevelSpec& s, usize index, std::string& why) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!level::save_world(dir, level::LevelWorld{true}, &why)) return false;
    Match3Level module;
    if (!module.load_objects(game, &why)) return false;
    level::Level lv(module);
    if (!lv.open(dir, &why)) return false;
    lv.ensure_loaded(world::Rect{-8, -8, 16, 16});
    for (i32 y = 0; y < s.h; ++y)
        for (i32 x = 0; x < s.w; ++x) {
            if (std::find(s.holes.begin(), s.holes.end(), std::pair{x, y}) != s.holes.end()) continue;
            if (!lv.set_tile(kFieldLayer, x, y, (x + y) % 2 ? kCellDark : kCell)) {
                why = std::string(s.name) + ": клетка не нарисована";
                return false;
            }
        }
    const objects::Library& library = module.library_ref();
    usize n = 0;
    auto spawn = [&](const char* id, i32 x, i32 y) -> flecs::entity {
        const objects::Template* t = library.find(std::string_view(id));
        flecs::entity e = t ? library.spawn(lv.scene(), *t, x + 0.5, y + 1.0) : flecs::entity{};
        if (e.is_valid()) e.set<level::LevelId>({object_id(index, n++)});
        return e;
    };
    for (const Put& p : s.gems)
        if (!spawn(p.id, p.x, p.y).is_valid()) {
            why = std::string(s.name) + ": не поставлена фишка " + p.id;
            return false;
        }
    // The level's rules: a copy of «Поле» left of the field, its values the level's.
    flecs::entity rules = spawn("board", -2, 0);
    const objects::Template* board = library.find(std::string_view("board"));
    if (!rules.is_valid() || !board) {
        why = std::string(s.name) + ": не поставлены правила уровня";
        return false;
    }
    for (auto [prop, value] : {std::pair{"moves", s.moves}, {"target", s.target}, {"seed", s.seed}}) {
        const objects::PropDef* def = library.prop_of(*board, prop);
        if (!def || !library.set_value(lv.scene(), rules, *def, std::to_string(value))) {
            why = std::string(s.name) + ": правило " + prop + " не задано";
            return false;
        }
    }
    lv.touch_objects();
    const level::Level::SaveReport r = lv.save();
    if (!r.ok) why = std::string(s.name) + ": " + r.error;
    return r.ok;
}

// --- the screens ------------------------------------------------------------------------------------------------------

constexpr d::Color kInk{30, 26, 46, 255}, kPanel{30, 26, 46, 210}, kWhite{255, 255, 255, 255}, kGold{255, 213, 74, 255},
                   kSoft{226, 222, 240, 255}, kButton{242, 179, 61, 255}, kButtonDark{80, 72, 110, 255};

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
    d::Node line = row(id, "Счёт",
                       {text(id, "Уровень", "{board.level_number}. {board.level}", 34, 700, kWhite),
                        text(id, "Очки", "Очки: {board.score} из {board.target}", 34, 800, kGold),
                        text(id, "Ходы", "Ходов: {board.moves}", 34, 700, kSoft)},
                       40, {10, 32, 10, 32});
    line.width_sizing = d::Sizing::Fixed;
    line.w = 1000;
    line.h = 64;
    line.x = (1920 - line.w) / 2;
    line.y = 12;
    line.horizontal = d::Constraint::Center;
    line.radius = {20, 20, 20, 20};
    line.fills = {solid(kPanel)};
    s.root.children = {line};
    s.next_id = id;
    return s;
}

// A window the game shows itself: kind "level" (a level passed), "win", "lose".
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
    const char* title = k == "level" ? "Уровень пройден!" : k == "win" ? "Победа!" : "Поражение";
    const char* words = k == "level" ? "«{board.level}»: {board.score} очков." : k == "win" ? "Все поля пройдены: {board.score} очков."
                                                                                            : "{board.reason}. Очки: {board.score} из {board.target}.";
    std::vector<d::Node> buttons;
    if (k == "level") buttons.push_back(button(id, "Дальше", d::ActionKind::NextLevel, kButton, kInk));
    if (k == "lose") buttons.push_back(button(id, "Ещё раз", d::ActionKind::Retry, kButton, kInk));
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
                    "{\n  \"title\": \"Три в ряд: проба\",\n  \"org\": \"Forge\",\n  \"theme\": \"fantasy\",\n"
                    "  \"autosave_minutes\": 5,\n  \"module\": \"match3\"\n}\n",
                    why))
        return false;
    for (const char* file : editor::project::kModuleFiles) {
        fs::copy_file(games / "modules" / "match3" / file, game / file, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            why = std::string("не скопирован ") + file + ": " + ec.message();
            return false;
        }
    }
    // The gems: ruby and sapphire of three frames, emerald of one, topaz with no picture, amethyst off the board.
    const std::vector<Paint> paints = {
        {"рубин.png", {{{200, 30, 50}}, {{240, 120, 130}}, {{250, 200, 205}}}},
        {"сапфир.png", {{{30, 70, 210}}, {{120, 150, 240}}, {{200, 215, 250}}}},
        {"изумруд.png", {{{30, 170, 70}}}, true},
        {"аметист.png", {{{150, 60, 190}}}, true},
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
            {"ruby", "Рубин", "gem", "рубин.png", 3, {{"points", "10"}}},
            {"sapphire", "Сапфир", "gem", "сапфир.png", 3, {{"points", "10"}}},
            {"emerald", "Изумруд", "gem", "изумруд.png", 1, {{"points", "10"}}},
            {"topaz", "Топаз", "gem", "", 1, {{"points", "20"}}},
            {"amethyst", "Аметист", "gem", "аметист.png", 1, {{"points", "50"}, {"on_board", "false"}}},
            {"board", "Поле", "board_rules", "", 1, {}},
        };
        for (const Spec& s : specs) {
            objects::Template t;
            t.id = s.id;
            t.name = s.name;
            t.kind = s.kind;
            t.genre = "Три в ряд";
            t.picture = s.picture;
            t.frames = s.frames;
            t.values = s.values;
            t.file = game / "objects" / (std::string(s.id) + ".object.json");
            if (!library.put(t, &why)) return false;
        }
    }
    level::LevelList list;
    const std::vector<LevelSpec> specs = levels();
    for (const LevelSpec& s : specs) list.levels.push_back({s.id, s.name});
    list.start = specs.front().id;
    if (!level::write_levels(game, list, &why)) return false;
    for (usize i = 0; i < specs.size(); ++i)
        if (!make_level(game, level::level_folder(game, specs[i].id), specs[i], i, why)) return false;
    if (!make_level(game, out / "short", short_level(), specs.size(), why)) return false;
    // The links: +2 moves for a row of topaz, the target passes the level, no moves left loses it.
    logic::Logic links;
    for (auto [verb, b] : {std::pair{"plus_moves", "topaz"}, {"reaches_target", "board"}, {"out_of_moves", "board"}}) {
        logic::Link l;
        l.a = "board";
        l.verb = verb;
        l.b = b;
        links.add(l);
    }
    links.set_spot("board", 60, 200);
    links.set_spot("topaz", 300, 200);
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

} // namespace match3
