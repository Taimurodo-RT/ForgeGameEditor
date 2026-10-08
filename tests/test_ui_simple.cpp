#include "forge/core/file.h"
#include "forge/core/path.h"
#include "forge/editor/ui_design.h"
#include "forge/editor/ui_simple.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <span>

using namespace forge;
using namespace forge::editor::design;

namespace {

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void ids_of(const Node& n, std::vector<u32>& out) {
    out.push_back(n.id);
    for (const Node& c : n.children) ids_of(c, out);
}

std::string read_text(const std::filesystem::path& p) {
    std::vector<u8> bytes;
    if (!read_file(p, bytes)) return {};
    std::string out(bytes.begin(), bytes.end());
    std::erase(out, '\r'); // a Windows checkout may turn line ends into CRLF
    return out;
}

const Block kBlocks[] = {Block::Button, Block::Text, Block::Picture, Block::Bar, Block::List};

} // namespace

TEST_CASE("ui simple: every block is made of ordinary layers and reads as itself") {
    for (Block b : kBlocks) {
        CAPTURE(block_word(b));
        Screen s = make_screen("Экран", 1920, 1080);
        const u32 first = s.next_id;
        BlockOptions o;
        o.picture = "pictures/интерфейс/камень.png";
        const Node n = make_block(s, b, 100, 200, o);
        CHECK(block_of(n) == b);
        CHECK(n.x == 100);
        CHECK(n.y == 200);
        CHECK(simple_hidden(n).empty()); // a new block has nothing the simple panel hides
        CHECK(simple_action_editable(n));
        // Fresh ids, all different, all from the screen's counter.
        std::vector<u32> ids;
        ids_of(n, ids);
        CHECK(std::set<u32>(ids.begin(), ids.end()).size() == ids.size());
        for (u32 id : ids) CHECK((id >= first && id < s.next_id));
        // Kept and drawn like any layer.
        s.root.children.push_back(n);
        Screen back;
        REQUIRE(load_screen(save_screen(s), back));
        CHECK(save_screen(back) == save_screen(s));
        const std::string html = screen_html(s);
        CHECK(contains(html, "id=\"n" + std::to_string(n.id) + "\""));
        switch (b) {
        case Block::Button:
            REQUIRE(block_label(n));
            CHECK(block_label(n)->text == "Кнопка");
            CHECK(n.on_click.empty()); // the author gives it its action
            break;
        case Block::Text: CHECK(n.text == "Текст"); break;
        case Block::Picture: CHECK(contains(html, "камень.png")); break;
        case Block::Bar:
            CHECK(contains(html, "forge-bar-value=\"hero.hearts\""));
            CHECK(contains(html, "forge-bar-max=\"hero.hearts_max\""));
            break;
        case Block::List:
            CHECK(contains(html, "forge-list=\"items\""));
            CHECK(contains(html, "forge-picture=\"item.icon\""));
            CHECK(contains(html, "{item.name} {item.count}"));
            REQUIRE(n.children.size() == 2); // the cell, and what shows while it is empty
            CHECK(block_of(n.children[0]) == Block::Button);
            break;
        default: break;
        }
    }
}

TEST_CASE("ui simple: blocks take the game's first colour and follow it") {
    Screen library = make_screen("Компоненты", 1920, 1080);
    library.library = true;
    library.colors.push_back({"c1", "Золото", {232, 176, 74, 255}});
    library.colors.push_back({"c2", "Кровь", {200, 40, 40, 255}});
    Screen s = make_screen("Экран", 1920, 1080);
    BlockOptions o;
    o.library = &library;
    for (Block b : {Block::Button, Block::Bar}) {
        const Node n = make_block(s, b, 0, 0, o);
        REQUIRE(!n.fills.empty());
        CHECK(n.fills[0].style == "c1");
        CHECK(n.fills[0].color == Color{232, 176, 74, 255});
    }
    // Without a theme: colours of their own.
    const Node plain = make_block(s, Block::Button, 0, 0);
    CHECK(plain.fills[0].style.empty());
}

TEST_CASE("ui simple: names keep counting the screen's own blocks") {
    Screen s = make_screen("Экран", 1920, 1080);
    s.root.children.push_back(make_block(s, Block::Button, 0, 0));
    s.root.children.push_back(make_block(s, Block::Button, 0, 0));
    CHECK(s.root.children[0].name == "Кнопка 1");
    CHECK(s.root.children[1].name == "Кнопка 2");
    s.root.children[0].name = "Кнопка 7";
    CHECK(make_block(s, Block::Button, 0, 0).name == "Кнопка 8");
}

TEST_CASE("ui simple: what layers drawn in «Полный» read as") {
    Screen s = make_screen("Экран", 1920, 1080);
    CHECK(block_of(s.root, true) == Block::Screen);
    Node rect;
    rect.type = NodeType::Rectangle;
    CHECK(block_of(rect) == Block::Shape);
    Node ellipse;
    ellipse.type = NodeType::Ellipse;
    CHECK(block_of(ellipse) == Block::Shape);
    rect.on_click.push_back({ActionKind::Close, ""});
    CHECK(block_of(rect) == Block::Button); // anything with an action is pressed
    Node bar;
    bar.type = NodeType::Rectangle;
    bar.bar.value = "hero.hearts";
    CHECK(block_of(bar) == Block::Bar);
    Node pic;
    pic.type = NodeType::Rectangle;
    Paint image;
    image.kind = PaintKind::Image;
    image.image = "pictures/a.png";
    pic.fills.push_back(image);
    CHECK(block_of(pic) == Block::Picture);
    // A frame with a text: a button (the menu's buttons); with more: a group.
    Node label;
    label.type = NodeType::Text;
    label.text = "Новая игра";
    Node button;
    button.type = NodeType::Frame;
    button.children.push_back(label);
    CHECK(block_of(button) == Block::Button);
    REQUIRE(block_label(button));
    CHECK(block_label(button)->text == "Новая игра");
    Node group;
    group.type = NodeType::Frame;
    group.children = {label, rect};
    CHECK(block_of(group) == Block::Group);
    Node empty_frame;
    empty_frame.type = NodeType::Frame;
    CHECK(block_of(empty_frame) == Block::Shape);
    // A copy of a component reads as a button; its label is inside.
    Node copy = group;
    copy.component = "Кнопка меню";
    copy.master = 5;
    CHECK(block_of(copy) == Block::Button);
}

TEST_CASE("ui simple: a layer from «Полный» names what the simple panel does not show") {
    Node n;
    n.type = NodeType::Frame;
    Node label;
    label.type = NodeType::Text;
    label.text = "Купить";
    n.children.push_back(label);
    n.fills.push_back(Paint{});
    CHECK(block_of(n) == Block::Button);
    CHECK(simple_hidden(n).empty());

    n.effects.push_back(Effect{});
    n.strokes.push_back(Stroke{});
    n.motion.kind = MotionKind::Pulse;
    n.show_if = "inv.coins > 4";
    n.rotation = 5;
    Paint grad;
    grad.kind = PaintKind::Linear;
    n.fills.push_back(grad);
    n.on_click = {{ActionKind::Change, "inv.coins -= 5"}, {ActionKind::Show, "лавка"}};
    n.frame.image = "pictures/интерфейс/рамка.png";
    const std::vector<std::string> hidden = simple_hidden(n);
    for (const char* word : {"тени и размытие", "обводка", "движение", "условие показа", "поворот", "сложная заливка",
                             "несколько или особые действия при нажатии", "рисованная рамка"}) {
        CAPTURE(word);
        CHECK(std::find(hidden.begin(), hidden.end(), word) != hidden.end());
    }
    CHECK(!simple_action_editable(n));
    // One simple action fits; a change of data does not.
    n.on_click = {{ActionKind::Show, "лавка"}};
    CHECK(simple_action_editable(n));
    n.on_click = {{ActionKind::Change, "inv.coins -= 5"}};
    CHECK(!simple_action_editable(n));
    // Asking changes nothing.
    const Node copy = n;
    (void)simple_hidden(n);
    (void)block_of(n);
    CHECK(save_screen([&] {
              Screen s = make_screen("a", 10, 10);
              s.root.children.push_back(n);
              return s;
          }()) == save_screen([&] {
              Screen s = make_screen("a", 10, 10);
              s.root.children.push_back(copy);
              return s;
          }()));
    // Every simple action is one of the game's; hide, toggle, change and talk are «Полный»'s.
    for (ActionKind k : {ActionKind::Show, ActionKind::Close, ActionKind::Message, ActionKind::NewGame, ActionKind::Continue,
                         ActionKind::Load, ActionKind::Save, ActionKind::Settings, ActionKind::Pause, ActionKind::Resume,
                         ActionKind::Menu, ActionKind::Quit})
        CHECK(simple_action(k));
    for (ActionKind k : {ActionKind::Hide, ActionKind::Toggle, ActionKind::Change, ActionKind::Talk}) CHECK(!simple_action(k));
}

// The example of «Простой» (games/examples/simple-mode): a HUD and a bag made
// of simple blocks only, as a beginner would make them. The test makes them
// and compares with the files; FORGE_WRITE_EXAMPLES=1 writes them again.
namespace {

std::pair<Screen, Screen> simple_screens() {
    Screen hud = make_screen("Простой: HUD", 1920, 1080);
    hud.show = ScreenShow::Playing;
    Node coins = make_block(hud, Block::Text, 1500, 40);
    coins.text = "Монеты: {inv.coins}";
    coins.horizontal = Constraint::End;
    Node hearts = make_block(hud, Block::Bar, 40, 1000);
    hearts.vertical = Constraint::End;
    Node bag = make_block(hud, Block::Button, 1560, 960);
    block_label(bag)->text = "Сумка";
    bag.horizontal = Constraint::End;
    bag.vertical = Constraint::End;
    bag.on_click = {{ActionKind::Show, "простой_сумка"}};
    hud.root.children = {coins, hearts, bag};

    Screen window = make_screen("Простой: сумка", 1920, 1080);
    window.show = ScreenShow::Command;
    window.root.fills.push_back(Paint{PaintKind::Solid, Color{0, 0, 0, 0x99}});
    Node title = make_block(window, Block::Text, 652, 240);
    title.text = "Сумка";
    title.horizontal = Constraint::Center;
    title.vertical = Constraint::Center;
    Node list = make_block(window, Block::List, 652, 320);
    list.horizontal = Constraint::Center;
    list.vertical = Constraint::Center;
    Node close = make_block(window, Block::Button, 800, 720);
    block_label(close)->text = "Закрыть";
    close.horizontal = Constraint::Center;
    close.vertical = Constraint::Center;
    close.on_click = {{ActionKind::Close, ""}};
    window.root.children = {title, list, close};
    return {hud, window};
}

} // namespace

TEST_CASE("ui simple: the example screens are as the simple blocks make them") {
    const std::filesystem::path dir = std::filesystem::path(FORGE_SOURCE_DIR) / "games" / "examples" / "simple-mode";
    const auto [hud, bag] = simple_screens();
    const std::pair<const char*, const Screen*> files[] = {{"простой_hud", &hud}, {"простой_сумка", &bag}};
    const bool write = std::getenv("FORGE_WRITE_EXAMPLES") != nullptr;
    for (const auto& [name, screen] : files) {
        CAPTURE(name);
        const std::string json = save_screen(*screen), html = screen_html(*screen);
        const std::filesystem::path json_file = dir / utf8_path(std::string(name) + ".json"), html_file = dir / utf8_path(std::string(name) + ".html");
        if (write) {
            std::filesystem::create_directories(dir);
            REQUIRE(write_file_atomic(json_file, std::span(reinterpret_cast<const u8*>(json.data()), json.size())));
            REQUIRE(write_file_atomic(html_file, std::span(reinterpret_cast<const u8*>(html.data()), html.size())));
        }
        Screen back;
        REQUIRE(load_screen(read_text(json_file), back));
        CHECK(save_screen(back) == json);
        CHECK(read_text(html_file) == html);
        // Nothing in them the simple panel would hide.
        for (const Node& n : screen->root.children) {
            CAPTURE(n.name);
            CHECK(simple_hidden(n).empty());
        }
    }
    CHECK(contains(screen_html(hud), "forge-click=\"[[&quot;show&quot;,&quot;простой_сумка&quot;]]\""));
    CHECK(contains(screen_html(bag), "forge-click=\"[[&quot;close&quot;,&quot;&quot;]]\""));
    CHECK(contains(screen_html(bag), "forge-list=\"items\""));
}
