// The template library's own templates (13.12): ui/templates/, the constructions and screens the «Интерфейс»
// tab offers. They are built here, so they always load and are written as the editor writes screens;
// FORGE_WRITE_TEMPLATES=1 writes them again.

#include "forge/core/file.h"
#include "forge/core/path.h"
#include "forge/editor/ui_design.h"
#include "forge/editor/ui_templates.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <tuple>

using namespace forge;
using namespace forge::editor::design;

namespace {

const std::string kArt = "pictures/интерфейс/";

// --- the looks: one per variant ---

Paint solid(Color c) {
    Paint p;
    p.color = c;
    return p;
}
Paint tiled(const char* picture) {
    Paint p;
    p.kind = PaintKind::Image;
    p.image = kArt + picture;
    p.fit = ImageFit::Tile;
    return p;
}
Paint gradient(Color from, Color to, f32 angle) {
    Paint p;
    p.kind = PaintKind::Linear;
    p.angle = angle;
    p.stops = {{from, 0}, {to, 1}};
    return p;
}
FrameArt art(const char* picture, f32 cut, bool fill) {
    FrameArt a;
    a.image = kArt + picture;
    a.slice = {cut, cut, cut, cut};
    a.fill = fill;
    return a;
}
Stroke line(Color c, f32 width) {
    Stroke s;
    s.color = c;
    s.width = width;
    return s;
}
Effect shadow(Color c, f32 y, f32 blur) {
    Effect e;
    e.color = c;
    e.y = y;
    e.blur = blur;
    return e;
}

struct Look {
    std::string name; // the variant
    std::string title_font;
    u16 title_weight = 700;
    Color title;
    std::string text_font;
    Color text, muted, accent;
    // Panels (windows, cards).
    std::vector<Paint> panel_fills;
    FrameArt panel_art;
    f32 panel_radius = 0;
    std::vector<Stroke> panel_strokes;
    std::vector<Effect> panel_effects;
    f32 panel_inset = 0; // how far in from the panel's edge its content starts (a drawn frame is thick)
    // The main buttons and the others.
    std::vector<Paint> button_fills;
    FrameArt button_art;
    f32 button_radius = 0;
    Color button_text;
    std::vector<Paint> button2_fills;
    f32 button2_radius = 0;
    Color button2_text;
    // Cells: a thing's place in a grid, a row of a list.
    std::vector<Paint> cell_fills;
    std::vector<Stroke> cell_strokes;
    f32 cell_radius = 0;
    Color bar_back;
    // A whole screen's background (a main menu's).
    std::vector<Paint> background;
};

Look dark() {
    Look l;
    l.name = "Тёмная";
    l.title_font = "Onest";
    l.title = {242, 244, 247, 255};
    l.text_font = "Onest";
    l.text = {215, 220, 226, 255};
    l.muted = {138, 148, 163, 255};
    l.accent = {232, 176, 74, 255};
    l.panel_fills = {solid({27, 33, 41, 250})};
    l.panel_radius = 20;
    l.panel_strokes = {line({44, 53, 66, 255}, 1)};
    l.panel_effects = {shadow({0, 0, 0, 128}, 12, 40)};
    l.button_fills = {solid({232, 176, 74, 255})};
    l.button_radius = 14;
    l.button_text = {27, 33, 41, 255};
    l.button2_fills = {solid({44, 53, 66, 255})};
    l.button2_radius = 14;
    l.button2_text = {230, 234, 240, 255};
    l.cell_fills = {solid({37, 45, 55, 255})};
    l.cell_strokes = {line({50, 60, 73, 255}, 1)};
    l.cell_radius = 12;
    l.bar_back = {17, 22, 28, 255};
    l.background = {gradient({26, 36, 48, 255}, {13, 11, 9, 255}, 160)};
    return l;
}

Look wood() {
    Look l;
    l.name = "Дерево";
    l.title_font = "Cormorant SC";
    l.title = {59, 36, 18, 255};
    l.text_font = "Alegreya Sans";
    l.text = {59, 42, 26, 255};
    l.muted = {107, 84, 64, 255};
    l.accent = {179, 58, 46, 255};
    l.panel_fills = {tiled("пергамент.png")};
    l.panel_art = art("рамка_дерево.png", 32, false);
    l.panel_effects = {shadow({0, 0, 0, 140}, 10, 30)};
    l.panel_inset = 24;
    l.button_art = art("кнопка_золото.png", 16, true);
    l.button_art.scale = 1.5f; // the picture is 48 px high: a 72 px button shows its rim where it is drawn
    l.button_text = {59, 36, 18, 255};
    l.button2_fills = {solid({107, 74, 43, 255})};
    l.button2_radius = 10;
    l.button2_text = {245, 230, 200, 255};
    l.cell_fills = {solid({139, 106, 68, 70})};
    l.cell_strokes = {line({107, 74, 43, 255}, 2)};
    l.cell_radius = 8;
    l.bar_back = {59, 36, 18, 200};
    l.background = {gradient({42, 29, 18, 255}, {14, 9, 5, 255}, 160)};
    return l;
}

Look stone() {
    Look l;
    l.name = "Камень";
    l.title_font = "Cormorant SC";
    l.title = {233, 223, 199, 255};
    l.text_font = "Alegreya Sans";
    l.text = {230, 225, 214, 255};
    l.muted = {169, 163, 154, 255};
    l.accent = {212, 169, 60, 255};
    l.panel_fills = {solid({34, 38, 44, 255})};
    l.panel_art = art("рамка_камень.png", 28, false);
    l.panel_effects = {shadow({0, 0, 0, 160}, 10, 30)};
    l.panel_inset = 20;
    l.button_art = art("кнопка_синяя.png", 16, true);
    l.button_art.scale = 1.5f;
    l.button_text = {255, 255, 255, 255};
    l.button2_fills = {solid({58, 64, 72, 255})};
    l.button2_radius = 8;
    l.button2_text = {230, 225, 214, 255};
    l.cell_fills = {solid({26, 29, 34, 255})};
    l.cell_strokes = {line({90, 96, 104, 255}, 2)};
    l.cell_radius = 6;
    l.bar_back = {20, 22, 26, 255};
    l.background = {tiled("камень.png"), solid({0, 0, 0, 150})};
    return l;
}

// --- layers ---

// The icons' font: a text of one of its characters is the icon (Material Symbols, ui/fonts).
const char* const kIcons = "Material Symbols Rounded";
const char* const kCoin = "\xEE\x89\xA3";   // monetization_on U+E263
const char* const kHeart = "\xEE\xA1\xBE";  // favorite U+E87E
const char* const kKey = "\xEE\x9C\xBC";    // key U+E73C
const char* const kSwords = "\xEF\xA2\x89"; // swords U+F889
const char* const kFlask = "\xEE\xA9\x8B";  // science U+EA4B
const char* const kFlag = "\xEF\x83\x86";   // flag U+F0C6
const char* const kCheck = "\xEF\x82\xBE";  // check_circle U+F0BE
const char* const kPerson = "\xEF\x83\x93"; // person U+F0D3
const char* const kPause = "\xEE\x80\xB4";  // pause U+E034
const char* const kClose = "\xEE\x97\x8D";  // close U+E5CD
const char* const kLock = "\xEE\xA2\x99";   // lock U+E899
const char* const kStar = "\xEF\x82\x9A";   // star U+F09A

Node frame(Screen& s, std::string name, f32 x, f32 y, f32 w, f32 h) {
    Node n;
    n.id = s.next_id++;
    n.name = std::move(name);
    n.type = NodeType::Frame;
    n.x = x;
    n.y = y;
    n.w = w;
    n.h = h;
    return n;
}

Node words(Screen& s, std::string name, f32 x, f32 y, f32 w, f32 h, std::string text, f32 size, TextAlign align,
           const std::string& font, u16 weight, Color color) {
    Node n = frame(s, std::move(name), x, y, w, h);
    n.type = NodeType::Text;
    n.text = std::move(text);
    n.text_style.family = font;
    n.text_style.size = size;
    n.text_style.weight = weight;
    n.text_style.align = align;
    n.text_style.color = color;
    return n;
}
Node heading(Screen& s, const Look& l, std::string text, f32 x, f32 y, f32 w, f32 size, TextAlign align = TextAlign::Center) {
    return words(s, "Заголовок", x, y, w, std::round(size * 1.3f), std::move(text), size, align, l.title_font, l.title_weight, l.title);
}
Node body(Screen& s, const Look& l, std::string name, std::string text, f32 x, f32 y, f32 w, f32 h, f32 size,
          TextAlign align = TextAlign::Left, bool muted = false) {
    return words(s, std::move(name), x, y, w, h, std::move(text), size, align, l.text_font, 400, muted ? l.muted : l.text);
}
Node icon(Screen& s, std::string name, const char* glyph, f32 x, f32 y, f32 size, Color color) {
    return words(s, std::move(name), x, y, size, size, glyph, size, TextAlign::Center, kIcons, 500, color);
}

Node panel(Screen& s, const Look& l, std::string name, f32 x, f32 y, f32 w, f32 h) {
    Node p = frame(s, std::move(name), x, y, w, h);
    p.fills = l.panel_fills;
    p.frame = l.panel_art;
    p.radius = {l.panel_radius, l.panel_radius, l.panel_radius, l.panel_radius};
    p.strokes = l.panel_strokes;
    p.effects = l.panel_effects;
    return p;
}

Node button(Screen& s, const Look& l, std::string label, f32 x, f32 y, f32 w, f32 h, std::vector<Action> actions,
            bool primary = true, f32 size = 32) {
    Node b = frame(s, "Кнопка «" + label + "»", x, y, w, h);
    b.fills = primary ? l.button_fills : l.button2_fills;
    if (primary) b.frame = l.button_art;
    const f32 r = primary ? l.button_radius : l.button2_radius;
    b.radius = {r, r, r, r};
    b.on_click = std::move(actions);
    const f32 th = std::round(size * 1.3f);
    b.children = {words(s, "Надпись", 0, std::round((h - th) * 0.5f), w, th, label, size, TextAlign::Center, l.text_font, 700,
                        primary ? l.button_text : l.button2_text)};
    return b;
}

Node close_button(Screen& s, const Look& l, f32 x, f32 y) {
    Node b = frame(s, "Закрыть", x, y, 64, 64);
    b.fills = l.button2_fills;
    b.radius = {32, 32, 32, 32};
    b.on_click = {{ActionKind::Close, ""}};
    b.children = {icon(s, "Крестик", kClose, 12, 12, 40, l.button2_text)};
    return b;
}

Node cell(Screen& s, const Look& l, std::string name, f32 x, f32 y, f32 w, f32 h) {
    Node c = frame(s, std::move(name), x, y, w, h);
    c.fills = l.cell_fills;
    c.strokes = l.cell_strokes;
    c.radius = {l.cell_radius, l.cell_radius, l.cell_radius, l.cell_radius};
    return c;
}

// A bar of the game's data on its dark track, the numbers over it.
Node meter(Screen& s, const Look& l, std::string name, f32 x, f32 y, f32 w, f32 h, Color color, std::string value,
           std::string max, std::string label) {
    Node track = frame(s, std::move(name), x, y, w, h);
    track.fills = {solid(l.bar_back)};
    track.radius = {h * 0.5f, h * 0.5f, h * 0.5f, h * 0.5f};
    track.clip = true;
    Node fill = frame(s, "Полоска", 0, 0, w, h);
    fill.type = NodeType::Rectangle;
    fill.fills = {solid(color)};
    fill.radius = track.radius;
    fill.bar.value = std::move(value);
    fill.bar.max = std::move(max);
    track.children = {fill};
    if (!label.empty())
        track.children.push_back(words(s, "Числа", 0, std::round((h - h * 0.8f) * 0.5f), w, std::round(h * 0.8f), std::move(label),
                                       std::round(h * 0.62f), TextAlign::Center, l.text_font, 700, {255, 255, 255, 255}));
    return track;
}

// --- constructions: each a screen of its own size with the one layer ---

Screen construction(const std::string& title, Node layer) {
    Screen s = make_screen(title, layer.w, layer.h);
    s.next_id = std::max(s.next_id, layer.id + 1);
    std::function<void(const Node&)> top = [&](const Node& n) {
        s.next_id = std::max(s.next_id, n.id + 1);
        for (const Node& c : n.children) top(c);
    };
    top(layer);
    layer.x = 0;
    layer.y = 0;
    s.root.children = {std::move(layer)};
    return s;
}

// The hero's things, a cell each: its picture and how many.
Node items_grid(Screen& s, const Look& l, f32 x, f32 y, int columns, int rows, f32 size, f32 gap) {
    Node list = frame(s, "Вещи героя", x, y, columns * size + (columns - 1) * gap, rows * size + (rows - 1) * gap);
    list.list = ListSource::Items;
    list.list_gap = gap;
    Node c = cell(s, l, "Ячейка", 0, 0, size, size);
    c.on_click = {{ActionKind::Message, "предмет {item.id}"}};
    Node picture = frame(s, "Картинка предмета", std::round(size * 0.14f), std::round(size * 0.1f), std::round(size * 0.72f),
                         std::round(size * 0.72f));
    picture.type = NodeType::Rectangle;
    picture.picture_from = "item.icon";
    Node count = words(s, "Сколько", 8, size - 38, size - 18, 32, "{item.count}", 24, TextAlign::Right, l.text_font, 700, l.text);
    c.children = {picture, count};
    // Shown while the list is empty: in its middle (on the canvas, under the cell's row).
    list.children = {c, body(s, l, "Пусто", "Пусто", 0, std::round(list.h * 0.5f) - 20, list.w, 40, 28, TextAlign::Center, true)};
    return list;
}

Node menu_column(Screen& s, const Look& l, f32 x, f32 y, f32 w) {
    Node col = frame(s, "Меню", x, y, w, 5 * 80 + 4 * 16);
    col.layout.mode = LayoutMode::Column;
    col.layout.gap = 16;
    const std::pair<const char*, ActionKind> rows[] = {{"Новая игра", ActionKind::NewGame}, {"Продолжить", ActionKind::Continue},
                                                       {"Загрузить", ActionKind::Load},     {"Настройки", ActionKind::Settings},
                                                       {"Выход", ActionKind::Quit}};
    f32 at = 0;
    for (const auto& [label, kind] : rows) {
        col.children.push_back(button(s, l, label, 0, at, w, 80, {{kind, ""}}, kind != ActionKind::Quit));
        at += 96;
    }
    return col;
}

Node confirm(Screen& s, const Look& l) {
    const f32 in = l.panel_inset;
    Node p = panel(s, l, "Подтверждение", 0, 0, 720 + 2 * in, 340 + 2 * in);
    p.children = {heading(s, l, "Вы уверены?", in, in + 32, 720, 48),
                  body(s, l, "Текст", "Прогресс после последнего сохранения пропадёт.", in + 60, in + 118, 600, 80, 28, TextAlign::Center,
                       true),
                  button(s, l, "Да", in + 60, in + 232, 280, 72, {{ActionKind::Message, "подтвердил"}}),
                  button(s, l, "Нет", in + 380, in + 232, 280, 72, {{ActionKind::Close, ""}}, false)};
    return p;
}

// «Музыка  − ▬▬▬▬ +»: a value of the game's (0..100) changed by ten.
Node volume_row(Screen& s, const Look& l, std::string label, const std::string& var, f32 x, f32 y) {
    Node row = frame(s, "Строка «" + label + "»", x, y, 720, 96);
    row.children = {body(s, l, "Название", label, 0, 28, 220, 40, 30),
                    button(s, l, "−", 236, 16, 64, 64, {{ActionKind::Change, var + " -= 10"}}, false, 36),
                    meter(s, l, "Шкала", 316, 30, 300, 36, l.accent, var, "100", "{" + var + "}"),
                    button(s, l, "+", 632, 16, 64, 64, {{ActionKind::Change, var + " += 10"}}, false, 36)};
    return row;
}

Node item_card(Screen& s, const Look& l, const char* name, const char* about, const char* glyph, std::vector<Action> take,
               const std::string& take_label) {
    const f32 in = l.panel_inset + 32; // the content's edge
    Node p = panel(s, l, "Карточка «" + std::string(name) + "»", 0, 0, 336 + 2 * in, 472 + 2 * in);
    Node picture = cell(s, l, "Картинка", in, in, 336, 200);
    picture.children = {icon(s, "Значок", glyph, 108, 40, 120, l.accent)};
    p.children = {picture,
                  words(s, "Название", in, in + 224, 336, 44, name, 34, TextAlign::Left, l.title_font, 700, l.title),
                  body(s, l, "Описание", about, in, in + 274, 336, 100, 24, TextAlign::Left, true),
                  button(s, l, take_label, in, in + 400, 336, 72, std::move(take))};
    return p;
}

Node quest_card(Screen& s, const Look& l) {
    const f32 in = l.panel_inset;
    Node p = panel(s, l, "Карточка задания", 0, 0, 640 + 2 * in, 200 + 2 * in);
    Node badge = frame(s, "Значок", in + 28, in + 52, 96, 96);
    badge.fills = {solid(l.accent)};
    badge.radius = {48, 48, 48, 48};
    badge.children = {icon(s, "Флаг", kFlag, 24, 24, 48, l.button_text)};
    p.children = {badge, words(s, "Название", in + 148, in + 28, 460, 40, "Найти кирку", 30, TextAlign::Left, l.title_font, 700, l.title),
                  body(s, l, "Текст", "Старый шахтёр потерял кирку где-то в пещерах.", in + 148, in + 74, 460, 64, 24, TextAlign::Left, true),
                  words(s, "Награда", in + 148, in + 146, 460, 32, "Награда: 50 монет", 24, TextAlign::Left, l.text_font, 700, l.accent)};
    return p;
}

Node portrait(Screen& s, const Look& l, f32 x, f32 y) {
    const f32 in = l.panel_inset;
    Node p = panel(s, l, "Портрет", x, y, 520 + 2 * in, 150 + 2 * in);
    p.clip = true; // on the canvas the numbers are their {names}: longer than the numbers, cut at the edge
    Node face = cell(s, l, "Лицо", in + 15, in + 15, 120, 120);
    face.radius = {60, 60, 60, 60};
    face.children = {icon(s, "Значок", kPerson, 20, 20, 80, l.muted)};
    // The hearts' numbers beside their bar: in a thin bar they would not read.
    p.children = {face, words(s, "Имя", in + 152, in + 14, 340, 40, "Герой", 30, TextAlign::Left, l.title_font, 700, l.title),
                  icon(s, "Сердце", kHeart, in + 152, in + 60, 30, {205, 60, 70, 255}),
                  meter(s, l, "Здоровье", in + 190, in + 64, 200, 22, {205, 60, 70, 255}, "hero.hearts", "hero.hearts_max", ""),
                  words(s, "Сердца", in + 400, in + 58, 104, 34, "{hero.hearts}/{hero.hearts_max}", 22, TextAlign::Left, l.text_font, 700,
                        l.text),
                  icon(s, "Звезда", kStar, in + 152, in + 102, 30, {90, 150, 230, 255}),
                  meter(s, l, "Опыт", in + 190, in + 108, 200, 18, {90, 150, 230, 255}, "hero.xp", "hero.xp_max", ""),
                  words(s, "Опыт числом", in + 400, in + 100, 104, 34, "{hero.xp}/{hero.xp_max}", 22, TextAlign::Left, l.text_font, 700,
                        l.muted)};
    return p;
}

Node quests_list(Screen& s, const Look& l, f32 x, f32 y, f32 w, f32 h) {
    Node list = frame(s, "Задания", x, y, w, h);
    list.list = ListSource::Quests;
    list.list_gap = 12;
    Node c = cell(s, l, "Строка", 0, 0, w, 100);
    Node done = icon(s, "Сделано", kCheck, 20, 30, 40, l.accent);
    done.show_if = "item.done";
    Node open = icon(s, "Не сделано", kFlag, 20, 30, 40, l.muted);
    open.show_if = "!item.done";
    c.children = {done, open, words(s, "Название", 76, 14, w - 96, 38, "{item.title}", 28, TextAlign::Left, l.title_font, 700, l.title),
                  body(s, l, "Текст", "{item.text}", 76, 54, w - 96, 36, 22, TextAlign::Left, true)};
    list.children = {c, body(s, l, "Пусто", "Заданий пока нет", 0, std::round(h * 0.5f) - 20, w, 40, 28, TextAlign::Center, true)};
    return list;
}

Node level_grid(Screen& s, const Look& l) {
    Node grid = frame(s, "Выбор уровня", 0, 0, 3 * 200 + 2 * 24, 2 * 200 + 24);
    grid.layout.mode = LayoutMode::Wrap;
    grid.layout.gap = 24;
    for (int i = 0; i < 6; ++i) {
        const std::string n = std::to_string(i + 1);
        Node tile = cell(s, l, "Уровень " + n, (i % 3) * 224.0f, (i / 3) * 224.0f, 200, 200);
        tile.on_click = {{ActionKind::Message, "уровень " + n}};
        tile.children = {words(s, "Номер", 0, 36, 200, 96, n, 80, TextAlign::Center, l.title_font, 700, l.title),
                         body(s, l, "Подпись", "Уровень", 0, 132, 200, 36, 24, TextAlign::Center, true)};
        grid.children.push_back(tile);
    }
    return grid;
}

// --- screens ---

Screen window_screen(const std::string& title) {
    Screen s = make_screen(title, 1920, 1080);
    s.show = ScreenShow::Command;
    s.pauses = true;
    s.dim = true;
    s.appear = Appear::Zoom;
    s.appear_time = 0.2f;
    return s;
}

Screen main_menu(const Look& l) {
    Screen s = make_screen("Главное меню", 1920, 1080);
    s.show = ScreenShow::Menu;
    s.root.fills = l.background;
    Node name = words(s, "Название игры", 160, 170, 1100, 150, "Название игры", 110, TextAlign::Left, l.title_font, 700, l.accent);
    Node motto = body(s, l, "Девиз", "Короткий девиз или подзаголовок", 166, 320, 1000, 44, 32, TextAlign::Left, true);
    if (l.name == "Дерево") motto.text_style.color = {214, 196, 160, 255};
    Node version = body(s, l, "Версия", "версия 1.0", 1640, 1010, 240, 30, 20, TextAlign::Right, true);
    version.horizontal = Constraint::End;
    version.vertical = Constraint::End;
    s.root.children = {name, motto, menu_column(s, l, 160, 440, 440), version};
    if (l.name == "Дерево") {
        Node news = panel(s, l, "Новости", 1180, 440, 600, 380);
        news.children = {heading(s, l, "Новости", 48, 44, 504, 44, TextAlign::Left),
                         body(s, l, "Текст", "Здесь можно рассказать игроку, что нового в игре.", 48, 110, 504, 200, 26)};
        s.root.children.insert(s.root.children.begin() + 3, news);
    }
    return s;
}

Screen hud(const Look& l) {
    Screen s = make_screen("Над игрой", 1920, 1080);
    s.show = ScreenShow::Playing;
    s.safe = 40;
    const f32 in = l.panel_inset;
    Node coins = panel(s, l, "Монеты", 1620 - 2 * in, 40, 260 + 2 * in, 72 + 2 * in);
    coins.horizontal = Constraint::End;
    coins.children = {icon(s, "Монета", kCoin, in + 16, in + 12, 48, l.accent),
                      words(s, "Сколько", in + 76, in + 14, 168, 44, "{inv.coins}", 36, TextAlign::Left, l.text_font, 700, l.text)};
    Node pause = button(s, l, "", 1808, 140 + 2 * in, 72, 72, {{ActionKind::Pause, ""}}, false);
    pause.name = "Кнопка «Пауза»";
    pause.horizontal = Constraint::End;
    pause.children = {icon(s, "Значок", kPause, 12, 12, 48, l.button2_text)};
    Node bar = frame(s, "Ячейки", 642, 944, 6 * 96 + 5 * 12, 96);
    bar.horizontal = Constraint::Center;
    bar.vertical = Constraint::End;
    for (int i = 0; i < 6; ++i) {
        Node c = cell(s, l, "Ячейка " + std::to_string(i + 1), i * 108.0f, 0, 96, 96);
        c.children = {words(s, "Клавиша", 8, 4, 30, 26, std::to_string(i + 1), 20, TextAlign::Left, l.text_font, 700, l.muted)};
        bar.children.push_back(c);
    }
    s.root.children = {portrait(s, l, 40, 40), coins, pause, bar};
    return s;
}

Screen pause_screen(const Look& l) {
    Screen s = window_screen("Пауза");
    const f32 in = l.panel_inset;
    Node p = panel(s, l, "Окно", 680 - in, 190 - in, 560 + 2 * in, 700 + 2 * in);
    p.horizontal = Constraint::Center;
    p.vertical = Constraint::Center;
    p.children = {heading(s, l, "Пауза", in, in + 40, 560, 56)};
    // «Продолжить» closes this window (it stops the game while it is up): Resume is the game's own pause menu's.
    const std::tuple<const char*, ActionKind, bool> rows[] = {{"Продолжить", ActionKind::Close, true},
                                                              {"Сохранить", ActionKind::Save, true},
                                                              {"Загрузить", ActionKind::Load, true},
                                                              {"Настройки", ActionKind::Settings, true},
                                                              {"В главное меню", ActionKind::Menu, false}};
    f32 y = in + 150;
    for (const auto& [label, kind, primary] : rows) {
        p.children.push_back(button(s, l, label, in + 80, y, 400, 80, {{kind, ""}}, primary));
        y += 100;
    }
    s.root.children = {p};
    return s;
}

Screen inventory(const Look& l) {
    Screen s = window_screen("Инвентарь");
    const f32 in = l.panel_inset;
    Node p = panel(s, l, "Окно", 580 - in, 160 - in, 760 + 2 * in, 760 + 2 * in);
    p.horizontal = Constraint::Center;
    p.vertical = Constraint::Center;
    p.children = {heading(s, l, "Инвентарь", in + 50, in + 36, 340, 52, TextAlign::Left),
                  icon(s, "Монета", kCoin, in + 412, in + 48, 40, l.accent),
                  words(s, "Монеты", in + 458, in + 46, 196, 44, "{inv.coins}", 32, TextAlign::Left, l.text_font, 700, l.text),
                  close_button(s, l, in + 672, in + 30), items_grid(s, l, in + 50, in + 130, 4, 3, 156, 12),
                  body(s, l, "Подсказка", "Нажмите на предмет, чтобы использовать его", in + 50, in + 660, 660, 40, 24,
                       TextAlign::Center, true)};
    s.root.children = {p};
    return s;
}

Screen shop(const Look& l) {
    Screen s = window_screen("Магазин");
    const f32 in = l.panel_inset;
    Node p = panel(s, l, "Окно", 300 - in, 140 - in, 1320 + 2 * in, 800 + 2 * in);
    p.horizontal = Constraint::Center;
    p.vertical = Constraint::Center;
    p.children = {heading(s, l, "Лавка", in + 48, in + 36, 600, 56, TextAlign::Left),
                  icon(s, "Монета", kCoin, in + 960, in + 48, 44, l.accent),
                  words(s, "Монеты", in + 1012, in + 46, 200, 48, "{inv.coins}", 36, TextAlign::Left, l.text_font, 700, l.text),
                  close_button(s, l, in + 1232, in + 30)};
    struct Ware {
        const char* name;
        const char* about;
        const char* glyph;
        const char* id;
        int price;
    };
    const Ware wares[] = {{"Зелье лечения", "Возвращает три сердца.", kFlask, "potion", 5},
                          {"Ключ", "Открывает запертую дверь.", kKey, "key", 10},
                          {"Меч", "Бьёт вдвое сильнее.", kSwords, "sword", 25}};
    f32 x = in + 48;
    for (const Ware& w : wares) {
        const std::string price = std::to_string(w.price);
        Node card = cell(s, l, std::string("Товар «") + w.name + "»", x, in + 140, 384, 600);
        Node picture = frame(s, "Картинка", 32, 32, 320, 200);
        picture.children = {icon(s, "Значок", w.glyph, 100, 40, 120, l.accent)};
        Node buy = button(s, l, "Купить за " + price, 32, 500, 320, 72,
                          {{ActionKind::Change, "inv.coins -= " + price + "; inv." + w.id + " += 1"}});
        buy.show_if = "inv.coins >= " + price;
        // Not enough coins: no button, a word beside the price (on the canvas both show, side by side).
        Node poor = body(s, l, "Не хватает монет", "не хватает монет", 152, 404, 200, 36, 22, TextAlign::Right, true);
        poor.show_if = "inv.coins < " + price;
        card.children = {picture, words(s, "Название", 32, 252, 320, 44, w.name, 34, TextAlign::Left, l.title_font, 700, l.title),
                         body(s, l, "Описание", w.about, 32, 302, 320, 70, 24, TextAlign::Left, true),
                         words(s, "Цена", 32, 400, 140, 40, "Цена: " + price, 28, TextAlign::Left, l.text_font, 700, l.accent), poor, buy};
        p.children.push_back(card);
        x += 420;
    }
    s.root.children = {p};
    return s;
}

Screen saves(const Look& l) {
    Screen s = window_screen("Сохранения");
    const f32 in = l.panel_inset;
    Node p = panel(s, l, "Окно", 640 - in, 230 - in, 640 + 2 * in, 620 + 2 * in);
    p.horizontal = Constraint::Center;
    p.vertical = Constraint::Center;
    p.children = {heading(s, l, "Сохранения", in, in + 40, 640, 52),
                  button(s, l, "Сохранить игру", in + 100, in + 150, 440, 80, {{ActionKind::Save, ""}}),
                  button(s, l, "Загрузить игру", in + 100, in + 250, 440, 80, {{ActionKind::Load, ""}}),
                  button(s, l, "Продолжить с последнего", in + 100, in + 350, 440, 80, {{ActionKind::Continue, ""}}, true, 28),
                  button(s, l, "Закрыть", in + 100, in + 450, 440, 80, {{ActionKind::Close, ""}}, false),
                  body(s, l, "Подсказка", "Быстрое сохранение — F5, загрузка — F9", in, in + 556, 640, 36, 22, TextAlign::Center, true)};
    s.root.children = {p};
    return s;
}

Screen settings(const Look& l) {
    Screen s = window_screen("Настройки");
    const f32 in = l.panel_inset;
    Node p = panel(s, l, "Окно", 510 - in, 160 - in, 900 + 2 * in, 760 + 2 * in);
    p.horizontal = Constraint::Center;
    p.vertical = Constraint::Center;
    // The game's own volumes (Shell::sync_settings_vars): heard at once, kept in its settings.
    p.children = {heading(s, l, "Настройки", in, in + 36, 900, 56), volume_row(s, l, "Общая", "settings.master", in + 90, in + 130),
                  volume_row(s, l, "Музыка", "settings.music", in + 90, in + 236), volume_row(s, l, "Звуки", "settings.sound", in + 90, in + 342)};
    p.children.push_back(button(s, l, "Экран и управление", in + 230, in + 480, 440, 76, {{ActionKind::Settings, ""}}, false, 28));
    p.children.push_back(button(s, l, "Готово", in + 230, in + 600, 440, 84, {{ActionKind::Close, ""}}));
    s.root.children = {p};
    return s;
}

struct Built {
    TemplateInfo info;
    Screen screen;
};

std::vector<Built> builtin_templates() {
    std::vector<Built> out;
    auto add = [&](std::string file, TemplateKind kind, std::string group, std::string title, const Look* look, std::string about,
                   std::vector<std::string> words, Screen screen) {
        TemplateInfo info;
        info.file = std::move(file);
        info.kind = kind;
        info.group = std::move(group);
        info.title = std::move(title);
        info.variant = look ? look->name : std::string();
        info.about = std::move(about);
        info.words = std::move(words);
        screen.title = info.title;
        screen.root.name = info.title;
        out.push_back({std::move(info), std::move(screen)});
    };
    const Look looks[] = {dark(), wood(), stone()};
    const Look& d = looks[0];
    const Look& w = looks[1];
    const Look& st = looks[2];
    const TemplateKind C = TemplateKind::Construction, S = TemplateKind::Screen;

    // Constructions.
    for (const Look* l : {&d, &w}) {
        Screen s = make_screen("", 1, 1);
        Node grid = items_grid(s, *l, 0, 0, 4, 3, 138, 12);
        // On a panel of its look: the cells read as one block on any screen.
        Node p = panel(s, *l, "Сетка предметов", 0, 0, grid.w + 2 * l->panel_inset + 48, grid.h + 2 * l->panel_inset + 48);
        grid.x = grid.y = l->panel_inset + 24;
        p.children = {grid};
        grid = p;
        add(std::string("grid_items_") + (l == &d ? "dark" : "wood"), C, "Сетки", "Сетка предметов", l,
            "Вещи героя ячейками по четыре в ряд: картинка и сколько. Щелчок по ячейке шлёт сообщение «предмет {id}» в «Логику».",
            {"инвентарь", "сумка", "вещи", "предметы", "ячейки", "список"}, construction("Сетка предметов", grid));
    }
    {
        Screen s = make_screen("", 1, 1);
        add("grid_levels_dark", C, "Сетки", "Выбор уровня", &d,
            "Шесть плиток в сетке с автораскладкой. Щелчок шлёт сообщение «уровень N» в «Логику».", {"уровни", "карта", "плитки"},
            construction("Выбор уровня", level_grid(s, d)));
    }
    for (const Look* l : {&d, &w}) {
        Screen s = make_screen("", 1, 1);
        add(std::string("form_confirm_") + (l == &d ? "dark" : "wood"), C, "Формы", "Подтверждение", l,
            "Вопрос и две кнопки: «Да» шлёт сообщение «подтвердил», «Нет» закрывает окно.", {"вопрос", "да", "нет", "диалог", "окно"},
            construction("Подтверждение", confirm(s, *l)));
    }
    {
        Screen s = make_screen("", 1, 1);
        add("form_volume_dark", C, "Формы", "Громкость со шкалой", &d,
            "Название, шкала и кнопки − и +: громкость музыки игры на 10 тише или громче (settings.music, от 0 до 100). "
            "Игра сразу меняет звук и запоминает его в своих настройках.",
            {"громкость", "музыка", "звук", "ползунок", "настройка", "шкала"},
            construction("Громкость со шкалой", volume_row(s, d, "Музыка", "settings.music", 0, 0)));
    }
    for (const Look* l : {&d, &w}) {
        Screen s = make_screen("", 1, 1);
        add(std::string("card_item_") + (l == &d ? "dark" : "wood"), C, "Карточки", "Карточка предмета", l,
            "Картинка, название, описание и кнопка «Взять»: она прибавляет предмет sword в инвентарь (inv.sword).",
            {"предмет", "вещь", "товар", "описание"},
            construction("Карточка предмета", item_card(s, *l, "Меч", "Старый, но острый. Бьёт вдвое сильнее.", kSwords,
                                                        {{ActionKind::Change, "inv.sword += 1"}}, "Взять")));
    }
    {
        Screen s = make_screen("", 1, 1);
        add("card_quest_dark", C, "Карточки", "Карточка задания", &d, "Значок, название, описание и награда задания.",
            {"задание", "квест", "награда", "журнал"}, construction("Карточка задания", quest_card(s, d)));
    }
    for (const Look* l : {&d, &w}) {
        Screen s = make_screen("", 1, 1);
        add(std::string("card_portrait_") + (l == &d ? "dark" : "wood"), C, "Карточки", "Портрет с полосками", l,
            "Лицо героя, имя, здоровье (hero.hearts из hero.hearts_max) и опыт (hero.xp из hero.xp_max).",
            {"герой", "здоровье", "опыт", "полоска", "hud", "сердца"}, construction("Портрет с полосками", portrait(s, *l, 0, 0)));
    }
    {
        Screen s = make_screen("", 1, 1);
        add("list_quests_dark", C, "Списки", "Список заданий", &d,
            "Начатые задания игры: значок «сделано», название и текст. Пока заданий нет — надпись «Заданий пока нет».",
            {"задания", "квесты", "журнал"}, construction("Список заданий", quests_list(s, d, 0, 0, 640, 480)));
    }
    for (const Look* l : {&d, &w, &st}) {
        Screen s = make_screen("", 1, 1);
        add(std::string("list_menu_") + (l == &d ? "dark" : l == &w ? "wood" : "stone"), C, "Списки", "Меню из кнопок", l,
            "Новая игра, продолжить, загрузить, настройки и выход — столбцом с автораскладкой.",
            {"меню", "кнопки", "главное меню", "столбец"}, construction("Меню из кнопок", menu_column(s, *l, 0, 0, 440)));
    }

    // Screens.
    for (const Look* l : {&d, &w})
        add(std::string("screen_menu_") + (l == &d ? "dark" : "wood"), S, "Главное меню", "Главное меню", l,
            "Показывается вместо меню игры: название, девиз и кнопки новой игры, продолжения, загрузки, настроек и выхода.",
            {"меню", "старт", "начало", "заставка"}, main_menu(*l));
    for (const Look* l : {&d, &w})
        add(std::string("screen_hud_") + (l == &d ? "dark" : "wood"), S, "Над игрой", "Над игрой", l,
            "Поверх мира, пока идёт игра: портрет со здоровьем и опытом, монеты (inv.coins), кнопка паузы и шесть ячеек.",
            {"hud", "интерфейс", "здоровье", "монеты", "ячейки"}, hud(*l));
    for (const Look* l : {&d, &w, &st})
        add(std::string("screen_pause_") + (l == &d ? "dark" : l == &w ? "wood" : "stone"), S, "Пауза", "Пауза", l,
            "Окно с паузой и затемнением, закрывается по Esc: «Продолжить» закрывает его, «Сохранить», «Загрузить» и "
            "«Настройки» открывают экраны самой игры, «В главное меню».",
            {"пауза", "меню паузы", "стоп"}, pause_screen(*l));
    for (const Look* l : {&d, &w})
        add(std::string("screen_inventory_") + (l == &d ? "dark" : "wood"), S, "Инвентарь", "Инвентарь", l,
            "Окно с паузой: вещи героя ячейками, монеты и кнопка «Закрыть». Щелчок по вещи шлёт «предмет {id}» в «Логику».",
            {"инвентарь", "сумка", "вещи", "предметы"}, inventory(*l));
    for (const Look* l : {&d, &w})
        add(std::string("screen_shop_") + (l == &d ? "dark" : "wood"), S, "Магазин", "Магазин", l,
            "Три товара за монеты (inv.coins): «Купить» забирает цену и прибавляет вещь (inv.potion, inv.key, inv.sword); "
            "когда монет мало, кнопки нет, а у цены написано «не хватает монет».",
            {"магазин", "лавка", "торговец", "купить", "монеты"}, shop(*l));
    add("screen_saves_dark", S, "Сохранения", "Сохранения", &d,
        "Кнопки к сохранениям игры: сохранить, загрузить, продолжить с последнего. Списки сохранений показывает сама игра.",
        {"сохранить", "загрузить", "слоты"}, saves(d));
    for (const Look* l : {&d, &w})
        add(std::string("screen_settings_") + (l == &d ? "dark" : "wood"), S, "Настройки", "Настройки", l,
            "Громкость общая, музыки и звуков (settings.master, settings.music, settings.sound, от 0 до 100): игра сразу "
            "меняет звук и запоминает его. «Экран и управление» открывает настройки самой игры, «Готово» закрывает окно.",
            {"настройки", "громкость", "звук", "музыка", "опции"}, settings(*l));
    return out;
}

std::string read_text(const std::filesystem::path& p) {
    std::vector<u8> bytes;
    if (!read_file(p, bytes)) return {};
    std::string out(bytes.begin(), bytes.end());
    std::erase(out, '\r'); // a Windows checkout may turn line ends into CRLF
    return out;
}

bool write_text(const std::filesystem::path& p, const std::string& text) {
    return write_file_atomic(p, std::span(reinterpret_cast<const u8*>(text.data()), text.size()));
}

void each_node(const Node& n, const std::function<void(const Node&)>& f) {
    f(n);
    for (const Node& c : n.children) each_node(c, f);
}

const std::filesystem::path kDir = std::filesystem::path(FORGE_SOURCE_DIR) / "ui" / "templates";

} // namespace

TEST_CASE("ui templates: the library's own templates are as the editor writes them") {
    const bool write = std::getenv("FORGE_WRITE_TEMPLATES") != nullptr;
    const std::vector<Built> built = builtin_templates();
    std::vector<TemplateInfo> index;
    for (const Built& b : built) index.push_back(b.info);
    if (write) {
        std::filesystem::create_directories(kDir);
        REQUIRE(write_text(kDir / "templates.json", save_template_index(index)));
        for (const Built& b : built) REQUIRE(write_text(screen_file(kDir, b.info.file, ".json"), save_screen(b.screen)));
    }
    CHECK(read_text(kDir / "templates.json") == save_template_index(index));
    for (const Built& b : built) {
        CAPTURE(b.info.file);
        CHECK(read_text(screen_file(kDir, b.info.file, ".json")) == save_screen(b.screen));
    }
    // Nothing else lies in the folder.
    std::set<std::string> files{"templates.json"};
    for (const Built& b : built) files.insert(b.info.file + ".json");
    for (const auto& e : std::filesystem::directory_iterator(kDir)) CHECK(files.count(path_to_utf8(e.path().filename())) == 1);

    std::vector<std::string> errors;
    const std::vector<Template> all = load_templates(kDir, &errors);
    CHECK(errors.empty());
    REQUIRE(all.size() == built.size());
    std::set<std::string> art;
    for (const auto& e : std::filesystem::directory_iterator(std::filesystem::path(FORGE_SOURCE_DIR) / "ui" / "art"))
        art.insert("pictures/интерфейс/" + path_to_utf8(e.path().filename()));
    for (const Template& t : all) {
        CAPTURE(t.info.file);
        // In a group of its kind, named, said what it is.
        const auto& groups = template_groups(t.info.kind);
        CHECK(std::find(groups.begin(), groups.end(), t.info.group) != groups.end());
        CHECK_FALSE(t.info.title.empty());
        CHECK_FALSE(t.info.about.empty());
        if (t.info.kind == TemplateKind::Construction) CHECK(t.screen.root.children.size() == 1);
        // Self-contained: no components, no game styles, no screens of its own to show, pictures from the editor's art,
        // every text with its own font, size and colour (a screen's defaults are not the template's).
        std::set<u32> ids;
        each_node(t.screen.root, [&](const Node& n) {
            CHECK(ids.insert(n.id).second);
            CHECK(n.id < t.screen.next_id);
            CHECK(n.component.empty());
            CHECK(n.master == 0);
            for (const Paint& p : n.fills) {
                CHECK(p.style.empty());
                if (p.kind == PaintKind::Image) CHECK(art.count(p.image) == 1);
            }
            if (!n.frame.image.empty()) CHECK(art.count(n.frame.image) == 1);
            for (const Action& a : n.on_click) CHECK((a.kind != ActionKind::Show && a.kind != ActionKind::Hide && a.kind != ActionKind::Toggle));
            if (n.type == NodeType::Text) {
                CHECK_FALSE(n.text_style.family.empty());
                CHECK(n.text_style.size > 0);
                CHECK(n.text_style.style.empty());
            }
        });
    }
    // Both kinds, every group has at least one, and a group's variants look different.
    for (const TemplateKind kind : {TemplateKind::Construction, TemplateKind::Screen})
        for (const std::string& g : template_groups(kind))
            if (g != kOwnGroup) // the author's own
                CHECK(std::any_of(all.begin(), all.end(), [&](const Template& t) { return t.info.kind == kind && t.info.group == g; }));
    std::set<std::pair<std::string, std::string>> named;
    for (const Template& t : all) CHECK(named.insert({t.info.title, t.info.variant}).second);
}

TEST_CASE("ui templates: a template is copied onto a screen or into a new one") {
    const std::vector<Template> all = load_templates(kDir);
    auto find = [&](const char* file) {
        for (const Template& t : all)
            if (t.info.file == file) return &t;
        return static_cast<const Template*>(nullptr);
    };
    const Template* card = find("card_item_dark");
    const Template* pause = find("screen_pause_wood");
    REQUIRE(card);
    REQUIRE(pause);

    // A construction: its one layer, with fresh ids after the screen's own.
    Screen s = make_screen("Экран", 1920, 1080);
    s.root.children.push_back(Node{});
    s.root.children.back().id = s.next_id++;
    const u32 first = s.next_id;
    const Node layer = template_layer(*card, s);
    CHECK(layer.name == card->screen.root.children.front().name);
    CHECK(layer.w == card->screen.root.children.front().w);
    std::set<u32> ids;
    each_node(layer, [&](const Node& n) {
        CHECK(n.id >= first);
        CHECK(n.id < s.next_id);
        CHECK(ids.insert(n.id).second);
    });
    CHECK(s.next_id - first == ids.size());
    // Taken twice: two copies with their own ids.
    const Node again = template_layer(*card, s);
    CHECK(again.id != layer.id);

    // A screen into the screen open now: one frame of its size, its background and layers.
    const Node whole = template_layer(*pause, s);
    CHECK(whole.name == "Пауза");
    CHECK(whole.type == NodeType::Frame);
    CHECK(whole.w == 1920);
    CHECK(whole.h == 1080);
    CHECK(whole.children.size() == pause->screen.root.children.size());
    CHECK(whole.clip);

    // A new screen: the template's settings and layers.
    const Screen fresh = template_screen(*pause, "Пауза 2");
    CHECK(fresh.title == "Пауза 2");
    CHECK(fresh.root.name == "Пауза 2");
    CHECK(fresh.show == ScreenShow::Command);
    CHECK(fresh.pauses);
    CHECK(fresh.dim);
    CHECK(fresh.esc_closes);
    CHECK(save_screen(fresh).find("\"pauses\": true") != std::string::npos);
    CHECK(fresh.root.children.size() == pause->screen.root.children.size());

    // The copy is the author's: the template changed afterwards (as its file read again) does not change it.
    Template changed = *card;
    changed.screen.root.children.front().name = "Другое";
    changed.screen.root.children.front().fills.clear();
    CHECK(layer.name != "Другое");
    CHECK_FALSE(layer.fills.empty());

    // Copies of components and links to game styles come as plain layers and colours.
    Template linked = *card;
    Node& top = linked.screen.root.children.front();
    top.component = "Карточка";
    top.master = 5;
    top.overrides = {"text"};
    top.fills.front().style = "c1";
    top.children.front().text_style.style = "t1";
    const Node plain = template_layer(linked, s);
    CHECK(plain.component.empty());
    CHECK(plain.master == 0);
    CHECK(plain.overrides.empty());
    CHECK(plain.fills.front().style.empty());
    CHECK(plain.fills.front().color == top.fills.front().color);

    // The index reads back as written.
    std::vector<TemplateInfo> index;
    for (const Template& t : all) index.push_back(t.info);
    std::vector<TemplateInfo> back;
    REQUIRE(load_template_index(save_template_index(index), back));
    CHECK(back == index);
    // A template the index names but whose file is missing is left out, and said.
    const std::filesystem::path tmp = std::filesystem::temp_directory_path() / "forge_templates_test";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    TemplateInfo ghost = index.front();
    ghost.file = "нет_такого";
    REQUIRE(write_text(tmp / "templates.json", save_template_index({index.front(), ghost})));
    REQUIRE(write_text(screen_file(tmp, index.front().file, ".json"), save_screen(all.front().screen)));
    std::vector<std::string> errors;
    CHECK(load_templates(tmp, &errors).size() == 1);
    CHECK(errors.size() == 1);
    std::filesystem::remove_all(tmp);
}

TEST_CASE("ui templates: what the library's preview draws") {
    const std::vector<Template> all = load_templates(kDir);
    auto find = [&](const char* file) {
        for (const Template& t : all)
            if (t.info.file == file) return &t;
        return static_cast<const Template*>(nullptr);
    };
    const Color ground{0x23, 0x27, 0x2e, 255};
    const Template* card = find("card_item_dark");
    const Template* hud = find("screen_hud_dark");
    const Template* menu = find("screen_menu_dark");
    const Template* pause = find("screen_pause_wood");
    REQUIRE(card);
    REQUIRE(hud);
    REQUIRE(menu);
    REQUIRE(pause);

    // A construction: alone in the middle of a 16:9 screen around it, a margin on every side, on the ground.
    const Screen c = template_preview(*card, ground);
    REQUIRE(c.root.children.size() == 1);
    const Node& n = c.root.children.front();
    CHECK(std::abs(c.width * 9 - c.height * 16) <= 16);
    CHECK(c.root.w == c.width);
    CHECK(c.root.h == c.height);
    CHECK(n.w == card->screen.root.children.front().w);
    CHECK(n.x >= 48);
    CHECK(n.y >= 48);
    CHECK(c.width - n.x - n.w >= 48);
    CHECK(c.height - n.y - n.h >= 48);
    CHECK(std::abs(n.x - (c.width - n.x - n.w)) <= 1);
    CHECK(std::abs(n.y - (c.height - n.y - n.h)) <= 1);
    REQUIRE(c.root.fills.size() == 1);
    CHECK(c.root.fills.front().color == ground);

    // Every preview is up at once and whole: a window by command, no veil, no appearing, no pause, no sound, fitted.
    for (const Template* t : {card, hud, menu, pause}) {
        const Screen p = template_preview(*t, ground);
        CHECK(p.show == ScreenShow::Command);
        CHECK(p.over == WindowOver::Any);
        CHECK_FALSE(p.dim);
        CHECK_FALSE(p.pauses);
        CHECK_FALSE(p.esc_closes);
        CHECK(p.appear == Appear::None);
        CHECK(p.fit == ScreenFit::Fit);
        CHECK(p.bars == ground);
        CHECK(p.safe == 0);
        CHECK(p.music.empty());
        CHECK(p.button_sound.empty());
    }
    // A screen as it is: its size and layers where they are; what is over the game (no background) on the ground,
    // a screen with its own background keeps it.
    const Screen h = template_preview(*hud, ground);
    CHECK(h.width == hud->screen.width);
    CHECK(h.height == hud->screen.height);
    REQUIRE(h.root.children.size() == hud->screen.root.children.size());
    CHECK(h.root.children.front().x == hud->screen.root.children.front().x);
    CHECK(hud->screen.root.fills.empty());
    REQUIRE(h.root.fills.size() == 1);
    CHECK(h.root.fills.front().color == ground);
    const Screen m = template_preview(*menu, ground);
    REQUIRE_FALSE(menu->screen.root.fills.empty());
    CHECK(m.root.fills == menu->screen.root.fills);
    // The template itself is as it was.
    CHECK(hud->screen.show == ScreenShow::Playing);
    CHECK(pause->screen.dim);
}

TEST_CASE("ui templates: the author's own, saved and taken back") {
    // A game with a component «Кнопка» and a colour and a text style of its own.
    Screen library = make_screen("Компоненты", 1920, 1080);
    library.library = true;
    Node variant;
    variant.id = library.next_id++;
    variant.name = "Кнопка";
    variant.component = "Кнопка";
    variant.w = 200;
    variant.h = 60;
    Node label;
    label.id = library.next_id++;
    label.type = NodeType::Text;
    label.text = "Кнопка";
    variant.children = {label};
    library.root.children = {variant};
    library.colors = {{"c1", "Акцент", Color{200, 150, 40, 255}}};
    library.text_styles = {{"t1", "Заголовок", TextStyle{}}};

    // A screen with a frame: a copy of «Кнопка» (its label changed), a text with the screen's font, a linked fill.
    Screen from = make_screen("Магазин", 1920, 1080);
    from.text.family = "Lora";
    from.text.size = 30;
    Node box;
    box.id = from.next_id++;
    box.name = "Витрина";
    box.x = 300;
    box.y = 200;
    box.w = 400;
    box.h = 300;
    box.horizontal = Constraint::Center;
    Paint fill;
    fill.color = Color{200, 150, 40, 255};
    fill.style = "c1";
    box.fills = {fill};
    std::optional<Node> buy = make_instance(from, library, "Кнопка");
    REQUIRE(buy);
    buy->children.front().text = "Купить";
    buy->overrides = {"text"};
    Node caption;
    caption.id = from.next_id++;
    caption.type = NodeType::Text;
    caption.text = "Товары";
    caption.text_style.family.clear(); // the screen's font
    caption.text_style.size = 0;       // and size
    caption.text_style.style = "t1";
    box.children = {*buy, caption};
    from.root.children = {box};

    const Template mine = own_template(from, &from.root.children.front(), "Витрина магазина", kOwnGroup);
    CHECK(mine.info.own);
    CHECK(mine.info.kind == TemplateKind::Construction);
    CHECK(mine.info.group == kOwnGroup);
    REQUIRE(mine.screen.root.children.size() == 1);
    const Node& top = mine.screen.root.children.front();
    CHECK(top.x == 0);
    CHECK(top.y == 0);
    CHECK(top.horizontal == Constraint::Start);
    CHECK(mine.screen.width == 400);
    CHECK(top.children.front().component == "Кнопка"); // the links stay in the template
    CHECK(top.children.back().text_style.family == "Lora"); // the screen's font written in
    CHECK(top.children.back().text_style.size == 30);
    CHECK(top.fills.front().style == "c1");

    // Into the folder: the index and a file; read back the same.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "forge_own_templates_test";
    std::filesystem::remove_all(dir);
    std::string error;
    const std::string file = save_own_template(dir, mine, "", &error);
    CHECK(error.empty());
    CHECK(file == "витрина_магазина");
    // The same title again: a file of its own, the first is not touched.
    const std::string second = save_own_template(dir, mine, "", &error);
    CHECK(second == "витрина_магазина_2");
    // A title a file system refuses: what it refuses left out, never outside the folder.
    Template odd = mine;
    odd.info.title = "../Пауза: «A/B»?";
    const std::string odd_file = save_own_template(dir, odd, "", &error);
    CHECK(odd_file == "пауза_«ab»");
    CHECK(odd_file.find('/') == std::string::npos);
    CHECK(std::filesystem::exists(screen_file(dir, odd_file, ".json")));
    CHECK(template_file_name("") == "шаблон");
    CHECK(template_file_name("templates") != "templates");
    // Replacing: the same file, the new content.
    Template changed = mine;
    changed.info.about = "другая";
    CHECK(save_own_template(dir, changed, file, &error) == file);
    std::vector<std::string> errors;
    std::vector<Template> back = load_templates(dir, &errors);
    CHECK(errors.empty());
    REQUIRE(back.size() == 3);
    CHECK(back[0].info.file == file);
    CHECK(back[0].info.about == "другая");
    CHECK(back[0].info.title == "Витрина магазина");
    CHECK(back[0].info.group == kOwnGroup);
    CHECK(save_screen(back[0].screen) == save_screen(mine.screen));
    // A broken index (written by hand) is not written over: nothing saved, said why, the file as it was.
    const std::filesystem::path broken = std::filesystem::temp_directory_path() / "forge_own_templates_broken";
    std::filesystem::remove_all(broken);
    std::filesystem::create_directories(broken);
    const std::string garbled = "{\"templates\": [ {\"file\": \"моё\"";
    REQUIRE(write_text(broken / "templates.json", garbled));
    error.clear();
    CHECK(save_own_template(broken, mine, "", &error).empty());
    CHECK_FALSE(error.empty());
    CHECK(read_text(broken / "templates.json") == garbled);
    CHECK_FALSE(std::filesystem::exists(screen_file(broken, "витрина_магазина", ".json")));
    std::filesystem::remove_all(broken);

    // Taken back in the same game: two copies, every layer its own id, the links to the game kept.
    Screen into = make_screen("Экран", 1920, 1080);
    std::vector<std::string> dropped;
    Node a = template_layer(back[0], into, &library, &dropped);
    Node b = template_layer(back[0], into, &library, &dropped);
    CHECK(dropped.empty());
    std::set<u32> ids;
    bool unique = true;
    for (const Node* n : {&a, &b})
        each_node(*n, [&](const Node& m) { unique = unique && ids.insert(m.id).second && m.id < into.next_id; });
    CHECK(unique);
    CHECK(a.children.front().component == "Кнопка");
    CHECK(a.children.front().master == buy->master); // the library's layer, not an id of this screen
    CHECK(a.children.front().overrides == std::vector<std::string>{"text"});
    CHECK(a.children.front().children.front().text == "Купить");
    CHECK(a.fills.front().style == "c1");
    CHECK(a.children.back().text_style.style == "t1");
    // One copy changed: the other and the template are as they were.
    a.children.front().children.front().text = "Продать";
    CHECK(b.children.front().children.front().text == "Купить");
    CHECK(back[0].screen.root.children.front().children.front().children.front().text == "Купить");

    // In a game without them: plain layers that look the same, and what was let go is said once each.
    Screen other = make_screen("Компоненты", 1920, 1080);
    other.library = true;
    dropped.clear();
    Node plain = template_layer(back[0], into, &other, &dropped);
    CHECK(plain.children.front().component.empty());
    CHECK(plain.children.front().master == 0);
    CHECK(plain.children.front().children.front().text == "Купить");
    CHECK(plain.fills.front().style.empty());
    CHECK(plain.fills.front().color == (Color{200, 150, 40, 255}));
    CHECK(plain.children.back().text_style.style.empty());
    CHECK(dropped == std::vector<std::string>{"цвета игры", "компонента «Кнопка»", "стиля текста игры"});

    // A whole screen: its settings and layers, its links kept the same way.
    from.show = ScreenShow::Command;
    from.pauses = true;
    const Template whole = own_template(from, nullptr, "Мой магазин", "Магазин");
    CHECK(whole.info.kind == TemplateKind::Screen);
    const Screen made = template_screen(whole, "Мой магазин", &library);
    CHECK(made.pauses);
    CHECK(made.root.children.front().children.front().component == "Кнопка");

    // A layer from inside a copy of a component, saved alone: plain, it follows no component.
    const Node& inner = from.root.children.front().children.front().children.front();
    REQUIRE(inner.master != 0);
    const Template part = own_template(from, &inner, "Надпись кнопки", kOwnGroup);
    CHECK(part.screen.root.children.front().master == 0);
    CHECK(part.screen.root.children.front().text == "Купить");
    Template stale = part;
    stale.screen.root.children.front().master = inner.master; // written by hand, or by an older editor
    CHECK(template_layer(stale, into, &library).master == 0);

    // Taken out: its file and its line; the others stay.
    CHECK(remove_own_template(dir, second));
    CHECK_FALSE(std::filesystem::exists(screen_file(dir, second, ".json")));
    CHECK(load_templates(dir).size() == 2);
    CHECK_FALSE(remove_own_template(dir, "нет_такого"));
    std::filesystem::remove_all(dir);
}

// --- games/examples/templates: a small game made of templates ---------------------------------------------------
//
// As the «Шаблоны» window makes it, with the same calls: the main menu, the screen over the game, the pause and
// the settings are new screens from the library's templates (template_screen); a card from the library is changed
// into «Ключ» and saved as the game's own template «Находка» (own_template, save_own_template), which is put twice
// on the screen over the game (template_layer), the second copy changed into «Факел». By hand after that: the
// buttons that open the author's pause and settings, the pause's click sound, the picture-less XP the game does
// not keep taken away. The pages are written as the editor writes them; the editor's self-test (step 154) and
// the game's (forge_slice --test --scene templates, also from the package) read these very files.
// FORGE_WRITE_EXAMPLES=1 writes them again.

namespace {

const std::filesystem::path kExample = std::filesystem::path(FORGE_SOURCE_DIR) / "games" / "examples" / "templates";

Node* named(Node& n, const std::string& name) {
    if (n.name == name) return &n;
    for (Node& c : n.children)
        if (Node* found = named(c, name)) return found;
    return nullptr;
}

const Template& builtin(const std::vector<Template>& all, const char* file) {
    for (const Template& t : all)
        if (t.info.file == file) return t;
    FAIL("no template " << file);
    return all.front();
}

struct TemplatesExample {
    std::vector<std::pair<std::string, Screen>> pages;
    Template own;
    Screen library;
};

TemplatesExample templates_example() {
    TemplatesExample ex;
    const std::vector<Template> all = load_templates(kDir);
    // The game's components, as a new game's: none.
    ex.library = make_screen("Компоненты", 1920, 1080);
    ex.library.library = true;
    const Screen* lib = &ex.library;

    Screen menu = template_screen(builtin(all, "screen_menu_wood"), "Шаблоны: меню", lib);
    named(menu.root, "Название игры")->text = "Шаблоны";
    named(menu.root, "Девиз")->text = "Всё здесь взято из библиотеки шаблонов";
    named(menu.root, "Кнопка «Настройки»")->on_click = {{ActionKind::Show, "шаблоны_настройки"}};

    Screen game = template_screen(builtin(all, "screen_hud_wood"), "Шаблоны: игра", lib);
    // The XP the game does not keep (the library says so beside the template): taken away.
    Node& portrait = *named(game.root, "Портрет");
    std::erase_if(portrait.children, [](const Node& n) { return n.name == "Звезда" || n.name == "Опыт" || n.name == "Опыт числом"; });
    named(game.root, "Кнопка «Пауза»")->on_click = {{ActionKind::Show, "шаблоны_пауза"}};

    // A card of the library made «Ключ» on the screen, saved as the game's own, then taken away from the screen.
    Node card = template_layer(builtin(all, "card_item_wood"), game, lib);
    card.name = "Карточка «Ключ»";
    named(card, "Значок")->text = "\xee\x9c\xbc"; // key
    named(card, "Название")->text = "Ключ";
    named(card, "Описание")->text = "Открывает старые двери в шахте.";
    named(card, "Кнопка «Взять»")->on_click = {{ActionKind::Change, "inv.key += 1"}};
    game.root.children.push_back(card);
    ex.own = own_template(game, &game.root.children.back(), "Находка", kOwnGroup);
    game.root.children.pop_back();
    // Put twice: the first as saved, the second made «Факел».
    Node key = template_layer(ex.own, game, lib);
    key.x = 496;
    key.y = 248;
    Node torch = template_layer(ex.own, game, lib);
    torch.x = 976;
    torch.y = 248;
    torch.name = "Карточка «Факел»";
    named(torch, "Значок")->text = "\xee\xbd\x95"; // local_fire_department
    named(torch, "Название")->text = "Факел";
    named(torch, "Описание")->text = "Светит в тёмной галерее.";
    named(torch, "Кнопка «Взять»")->on_click = {{ActionKind::Change, "inv.torch += 1"}};
    game.root.children.push_back(std::move(key));
    game.root.children.push_back(std::move(torch));

    Screen pause = template_screen(builtin(all, "screen_pause_wood"), "Шаблоны: пауза", lib);
    named(pause.root, "Кнопка «Настройки»")->on_click = {{ActionKind::Show, "шаблоны_настройки"}};
    pause.button_sound = "щелчок.wav";

    Screen settings = template_screen(builtin(all, "screen_settings_wood"), "Шаблоны: настройки", lib);

    ex.pages = {{"шаблоны_меню", menu}, {"шаблоны_игра", game}, {"шаблоны_пауза", pause}, {"шаблоны_настройки", settings}};
    return ex;
}

} // namespace

TEST_CASE("ui templates: the example game is as the editor makes it") {
    const bool write = std::getenv("FORGE_WRITE_EXAMPLES") != nullptr;
    const TemplatesExample ex = templates_example();
    HtmlOptions options;
    options.library = &ex.library;
    const std::filesystem::path ui = kExample / "ui";
    for (const auto& [name, screen] : ex.pages) {
        CAPTURE(name);
        const std::string json = save_screen(screen), html = screen_html(screen, options);
        const std::filesystem::path json_file = ui / utf8_path(name + ".json"), html_file = ui / utf8_path(name + ".html");
        if (write) {
            std::filesystem::create_directories(ui);
            REQUIRE(write_text(json_file, json));
            REQUIRE(write_text(html_file, html));
        }
        CHECK(read_text(json_file) == json);
        CHECK(read_text(html_file) == html);
    }
    // The game's own template, as «Сохранить как шаблон» writes it.
    const std::filesystem::path made = std::filesystem::temp_directory_path() / "forge_templates_example_own";
    std::filesystem::remove_all(made);
    std::string error;
    REQUIRE(save_own_template(made, ex.own, "", &error) == "находка");
    if (write) {
        std::filesystem::remove_all(ui / "templates");
        REQUIRE(save_own_template(ui / "templates", ex.own, "", &error) == "находка");
    }
    for (const char* file : {"templates.json", "находка.json"})
        CHECK(read_text(ui / "templates" / utf8_path(file)) == read_text(made / utf8_path(file)));
    std::filesystem::remove_all(made);

    // What the pages name is in the example: its pictures, its sound, the screens its buttons open.
    std::set<std::string> pictures, sounds, opened;
    std::set<std::string> pages;
    for (const auto& [name, screen] : ex.pages) {
        pages.insert(name);
        if (!screen.button_sound.empty()) sounds.insert(screen.button_sound);
        each_node(screen.root, [&](const Node& n) {
            for (const Paint& p : n.fills)
                if (p.kind == PaintKind::Image) pictures.insert(p.image);
            if (!n.frame.image.empty()) pictures.insert(n.frame.image);
            if (!n.click_sound.empty()) sounds.insert(n.click_sound);
            for (const Action& a : n.on_click)
                if (a.kind == ActionKind::Show || a.kind == ActionKind::Toggle) opened.insert(a.target);
        });
    }
    CHECK_FALSE(pictures.empty());
    for (const std::string& p : pictures) CHECK_MESSAGE(std::filesystem::is_regular_file(kExample / utf8_path(p)), p);
    CHECK(sounds == std::set<std::string>{"щелчок.wav"});
    for (const std::string& s : sounds) CHECK(std::filesystem::is_regular_file(kExample / "sounds" / utf8_path(s)));
    CHECK(opened == std::set<std::string>{"шаблоны_пауза", "шаблоны_настройки"});
    for (const std::string& o : opened) CHECK(pages.count(o) == 1);

    // Two copies of «Находка», apart: every id their own; the second changed, the first and the template not.
    const Screen& game = ex.pages[1].second;
    std::set<u32> ids;
    bool unique = true;
    each_node(game.root, [&](const Node& n) { unique = unique && ids.insert(n.id).second && n.id < game.next_id; });
    CHECK(unique);
    const Node& key = game.root.children[game.root.children.size() - 2];
    const Node& torch = game.root.children.back();
    CHECK(key.children[1].text == "Ключ");
    CHECK(torch.children[1].text == "Факел");
    CHECK(ex.own.screen.root.children.front().children[1].text == "Ключ");
    CHECK(key.children.back().on_click == std::vector<Action>{{ActionKind::Change, "inv.key += 1"}});
    CHECK(torch.children.back().on_click == std::vector<Action>{{ActionKind::Change, "inv.torch += 1"}});
    // The windows: a pause that stops the game, darkens, closes by Esc and grows in; the settings over it.
    const Screen& pause = ex.pages[2].second;
    CHECK(pause.show == ScreenShow::Command);
    CHECK(pause.pauses);
    CHECK(pause.dim);
    CHECK(pause.esc_closes);
    CHECK(pause.appear == Appear::Zoom);
    CHECK(screen_html(pause, options).find("forge-button-sound=\"щелчок.wav\"") != std::string::npos);
}
