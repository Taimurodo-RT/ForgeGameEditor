#include "forge/core/file.h"
#include "forge/core/path.h"
#include "forge/editor/ui_design.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace d = forge::editor::design;
using forge::u32;

namespace {

constexpr forge::usize kOnTop = std::numeric_limits<forge::usize>::max();

std::string read_text(const std::filesystem::path& p) {
    std::vector<forge::u8> bytes;
    if (!forge::read_file(p, bytes)) return {};
    std::string out(bytes.begin(), bytes.end());
    std::erase(out, '\r'); // a Windows checkout may turn line ends into CRLF
    return out;
}

d::Node layer(d::NodeType type, const char* name, forge::f32 x, forge::f32 y, forge::f32 w, forge::f32 h, d::Color fill) {
    d::Node n;
    n.type = type;
    n.name = name;
    n.x = x;
    n.y = y;
    n.w = w;
    n.h = h;
    n.fills.push_back({});
    n.fills[0].color = fill;
    return n;
}

// games/examples/layer-move: two frames; in «Рамка А» a button (it counts its presses) and a picture, in «Рамка Б»
// a caption, a row (auto layout) of two cells and a frame of its own, «Окошко».
d::Screen move_screen() {
    d::Screen s = d::make_screen("Перенос: пример", 1920, 1080);
    d::Node title;
    title.type = d::NodeType::Text;
    title.name = "Заголовок";
    title.text = "Перенос между рамками";
    title.x = 560;
    title.y = 60;
    title.w = 800;
    title.h = 60;
    title.text_style.size = 48;
    title.text_style.align = d::TextAlign::Center;

    d::Node a = layer(d::NodeType::Frame, "Рамка А", 160, 200, 640, 640, {30, 42, 58, 255});
    a.clip = true;
    a.radius = {16, 16, 16, 16};
    d::Node button = layer(d::NodeType::Frame, "Кнопка", 60, 80, 240, 80, {232, 176, 74, 255});
    button.radius = {12, 12, 12, 12};
    button.on_click = {{d::ActionKind::Change, "demo.presses += 1"}};
    d::Node label;
    label.type = d::NodeType::Text;
    label.name = "Надпись";
    label.text = "Нажми";
    label.w = 240;
    label.h = 80;
    label.text_style.size = 32;
    label.text_style.align = d::TextAlign::Center;
    label.text_style.color = {27, 33, 39, 255};
    button.children.push_back(label);
    d::Node picture = layer(d::NodeType::Rectangle, "Картинка", 60, 240, 200, 200, {255, 255, 255, 255});
    picture.fills[0].kind = d::PaintKind::Image;
    picture.fills[0].image = "pictures/интерфейс/камень.png";
    a.children = {button, picture};

    d::Node b = layer(d::NodeType::Frame, "Рамка Б", 1000, 200, 760, 640, {36, 48, 63, 255});
    b.clip = true;
    b.radius = {16, 16, 16, 16};
    d::Node caption;
    caption.type = d::NodeType::Text;
    caption.name = "Подпись";
    caption.text = "Рамка Б";
    caption.x = 40;
    caption.y = 24;
    caption.w = 400;
    caption.h = 48;
    d::Node row = layer(d::NodeType::Frame, "Ряд", 40, 380, 680, 220, {44, 58, 77, 255});
    row.layout.mode = d::LayoutMode::Row;
    row.layout.gap = 16;
    row.layout.padding = {20, 20, 20, 20};
    row.children = {layer(d::NodeType::Rectangle, "Ячейка 1", 0, 0, 140, 140, {74, 106, 138, 255}),
                    layer(d::NodeType::Rectangle, "Ячейка 2", 0, 0, 140, 140, {90, 122, 154, 255})};
    d::Node window = layer(d::NodeType::Frame, "Окошко", 420, 100, 300, 220, {58, 74, 96, 255});
    window.clip = true;
    window.radius = {12, 12, 12, 12};
    b.children = {caption, row, window};
    s.root.children = {title, a, b};
    for (d::Node& n : s.root.children) d::renumber(s, n);
    return s;
}

// The example after the beginner's route: the button in «Рамка Б» at 1100, 300 of the screen, the picture last in the row.
d::Screen moved_screen() {
    d::Screen s = move_screen();
    REQUIRE(d::move_layers(s, {4}, 7, kOnTop, {{100, 100}}));
    REQUIRE(d::move_layers(s, {6}, 9, kOnTop));
    s.title = "Перенос: готово";
    s.root.name = s.title;
    return s;
}

std::vector<u32> kids(const d::Screen& s, u32 id) {
    std::vector<u32> out;
    if (const d::Node* n = d::find(s.root, id))
        for (const d::Node& c : n->children) out.push_back(c.id);
    return out;
}

void write_example(const std::filesystem::path& dir, const char* name, const d::Screen& s) {
    const std::string json = d::save_screen(s), html = d::screen_html(s);
    const std::filesystem::path json_file = dir / forge::utf8_path(std::string(name) + ".json");
    const std::filesystem::path html_file = dir / forge::utf8_path(std::string(name) + ".html");
    if (std::getenv("FORGE_WRITE_EXAMPLES")) {
        std::filesystem::create_directories(dir);
        REQUIRE(forge::write_file_atomic(json_file, std::span(reinterpret_cast<const forge::u8*>(json.data()), json.size())));
        REQUIRE(forge::write_file_atomic(html_file, std::span(reinterpret_cast<const forge::u8*>(html.data()), html.size())));
    }
    d::Screen back;
    REQUIRE(d::load_screen(read_text(json_file), back));
    CHECK(d::save_screen(back) == json);
    CHECK(read_text(html_file) == html);
}

} // namespace

TEST_CASE("layer move: a button goes into another frame and back, all of it kept") {
    d::Screen s = move_screen();
    REQUIRE(kids(s, 3) == std::vector<u32>{4, 6});
    d::Node* button = d::find(s.root, 4);
    REQUIRE(button);
    // What it has goes with it: links to the game's colours and styles, its movement, a hidden layer inside.
    button->fills[0].style = "gold";
    button->text_style.style = "title";
    button->motion.kind = d::MotionKind::Pulse;
    button->show_if = "inv.coins > 0";
    button->children[0].visible = false;
    const std::string before = d::save_screen(s);
    const d::Node was = *button;

    CHECK(d::move_refusal(s, {4}, 7).empty());
    REQUIRE(d::move_layers(s, {4}, 7, kOnTop, {{100, 100}}));
    CHECK(kids(s, 3) == std::vector<u32>{6});
    CHECK(kids(s, 7) == std::vector<u32>{8, 9, 12, 4}); // on top of what was there
    const d::Node* moved = d::find(s.root, 4);
    REQUIRE(moved);
    CHECK(d::parent_of(s.root, 4)->id == 7);
    CHECK(moved->x == 100);
    CHECK(moved->y == 100);
    CHECK(moved->w == was.w);
    CHECK(moved->h == was.h);
    CHECK(moved->on_click == was.on_click);
    CHECK(moved->fills == was.fills);
    CHECK(moved->motion == was.motion);
    CHECK(moved->show_if == was.show_if);
    REQUIRE(moved->children.size() == 1);
    CHECK(moved->children[0].id == 5);
    CHECK(!moved->children[0].visible);
    CHECK(s.next_id == 13); // no new ids

    // And back where it was: the screen exactly as before.
    d::Screen t;
    REQUIRE(d::load_screen(before, t));
    REQUIRE(d::move_layers(t, {4}, 7, kOnTop, {{100, 100}}));
    REQUIRE(d::move_layers(t, {4}, 3, 0, {{60, 80}}));
    CHECK(d::save_screen(t) == before);
}

TEST_CASE("layer move: a frame goes with all inside it; what moves with another is not moved twice") {
    d::Screen s = move_screen();
    CHECK(d::movable_order(s, {6, 3, 4, 1}) == std::vector<u32>{3}); // the root left out, the button and picture go with A
    REQUIRE(d::move_layers(s, {6, 3, 4}, 7, 1, {{20, 120}}));
    CHECK(kids(s, 1) == std::vector<u32>{2, 7});
    CHECK(kids(s, 7) == std::vector<u32>{8, 3, 9, 12}); // before the second of those that stay
    CHECK(kids(s, 3) == std::vector<u32>{4, 6});
    CHECK(kids(s, 4) == std::vector<u32>{5});
    CHECK(d::find(s.root, 3)->x == 20);
}

TEST_CASE("layer move: into a row it joins the flow at its place; several keep their order") {
    d::Screen s = move_screen();
    d::find(s.root, 6)->absolute = true;
    REQUIRE(d::move_layers(s, {6}, 9, 1));
    CHECK(kids(s, 9) == std::vector<u32>{10, 6, 11});
    CHECK(!d::find(s.root, 6)->absolute);
    REQUIRE(d::move_layers(s, {11, 2}, 9, 0)); // in the drawing's order: the title first
    CHECK(kids(s, 9) == std::vector<u32>{2, 11, 10, 6});
    // Its own frame, another place: the order there changes, nothing else.
    REQUIRE(d::move_layers(s, {2}, 9, kOnTop));
    CHECK(kids(s, 9) == std::vector<u32>{11, 10, 6, 2});
}

TEST_CASE("layer move: let go where it already is, nothing changes, «Вне раскладки» too") {
    d::Screen s = move_screen();
    d::find(s.root, 10)->absolute = true; // «Ячейка 1» out of the row's flow, first in it
    const std::string before = d::save_screen(s);
    // Before «Ячейка 2» (its row in the list just under it): where it is.
    CHECK(d::move_stays(s, {10}, 9, 0));
    REQUIRE(d::move_layers(s, {10}, 9, 0, {{0, 0}}));
    CHECK(d::save_screen(s) == before);
    // An ordinary layer in a free frame: the button before the picture, as it is; on top of «Рамка А» is not.
    CHECK(d::move_stays(s, {4}, 3, 0));
    REQUIRE(d::move_layers(s, {4}, 3, 0, {{5, 5}}));
    CHECK(d::save_screen(s) == before);
    CHECK(!d::move_stays(s, {4}, 3, kOnTop));
    // Several, in their order: the same; the other way round they are moved.
    CHECK(d::move_stays(s, {10, 11}, 9, 0));
    CHECK(!d::move_stays(s, {4}, 7, kOnTop));
    // Truly moved after «Ячейка 2»: it joins the flow there (as the author meant).
    REQUIRE(d::move_layers(s, {10}, 9, 1));
    CHECK(kids(s, 9) == std::vector<u32>{11, 10});
    CHECK(!d::find(s.root, 10)->absolute);
}

TEST_CASE("layer move: a screen with a Russian name is written and read under that name") {
    // On Windows a path from a narrow string goes through the code page: «перенос_пример» came out garbled.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "forge_layer_move_names";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    const d::Screen screen = move_screen();
    const std::string json = d::save_screen(screen);
    const std::filesystem::path file = d::screen_file(dir, "перенос_пример", ".json");
    CHECK(forge::path_to_utf8(file.filename()) == "перенос_пример.json");
    REQUIRE(forge::write_file_atomic(file, std::span(reinterpret_cast<const forge::u8*>(json.data()), json.size())));
    std::vector<std::string> names;
    for (const auto& e : std::filesystem::directory_iterator(dir)) names.push_back(forge::path_to_utf8(e.path().filename()));
    CHECK(names == std::vector<std::string>{"перенос_пример.json"});
    d::Screen back;
    REQUIRE(d::load_screen(read_text(d::screen_file(dir, "перенос_пример", ".json")), back));
    CHECK(d::save_screen(back) == json);
    CHECK(d::screen_file(dir, "перенос_пример", ".html") == dir / forge::utf8_path("перенос_пример.html"));
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("layer move: refused moves change nothing and say why") {
    d::Screen s = move_screen();
    const std::string before = d::save_screen(s);
    auto refused = [&](const std::vector<u32>& ids, u32 parent, const char* words) {
        const std::string why = d::move_refusal(s, ids, parent);
        CHECK_MESSAGE(why.find(words) != std::string::npos, why);
        CHECK(!d::move_layers(s, ids, parent, 0));
        CHECK(d::save_screen(s) == before);
    };
    refused({3}, 3, "в саму себя");
    refused({7}, 9, "в саму себя");    // into what is inside it
    refused({4}, 6, "только в рамку"); // a picture is not a frame
    refused({4}, 99, "только в рамку");
    refused({1}, 3, "Экран целиком");
    refused({}, 3, "Экран целиком");

    // A list: nothing goes in, its layers do not go out.
    d::Node* row = d::find(s.root, 9);
    row->layout = {};
    row->list = d::ListSource::Items;
    const std::string listed = d::save_screen(s);
    CHECK(d::move_refusal(s, {4}, 9).find("В список") != std::string::npos);
    CHECK(d::move_refusal(s, {10}, 7).find("Слой списка") != std::string::npos);
    CHECK(!d::move_layers(s, {10}, 7, 0));
    CHECK(d::save_screen(s) == listed);
}

TEST_CASE("layer move: a copy of a component moves whole, its layers stay in it") {
    d::Screen lib = d::make_screen("Компоненты", 1920, 1080);
    lib.library = true;
    d::Node v = layer(d::NodeType::Frame, "Кнопка", 0, 0, 200, 60, {40, 40, 40, 255});
    v.component = "Кнопка";
    v.variant = {{"Состояние", "Обычная"}};
    d::Node text;
    text.type = d::NodeType::Text;
    text.name = "Надпись";
    text.text = "Играть";
    v.children.push_back(text);
    d::renumber(lib, v);
    lib.root.children.push_back(v);

    d::Screen s = move_screen();
    std::optional<d::Node> copy = d::make_instance(s, lib, "Кнопка");
    REQUIRE(copy);
    copy->x = 300;
    copy->y = 500;
    copy->children[0].text = "Своя надпись";
    copy->children[0].overrides.push_back("text");
    const u32 id = copy->id, inner = copy->children[0].id;
    d::find(s.root, 3)->children.push_back(*copy);

    CHECK(d::move_refusal(s, {inner}, 7).find("копии компонента") != std::string::npos);
    CHECK(d::move_refusal(s, {4}, id).find("копии компонента") != std::string::npos);
    REQUIRE(d::move_layers(s, {id}, 7, kOnTop, {{50, 60}}));
    CHECK(d::parent_of(s.root, id)->id == 7);
    // The component changing later keeps the copy's place and its own text.
    CHECK(!d::sync_instances(s, lib));
    const d::Node* moved = d::find(s.root, id);
    CHECK(moved->x == 50);
    CHECK(moved->children[0].text == "Своя надпись");

    // In the library the variants stay at its top, and nothing else goes there.
    d::Node extra = layer(d::NodeType::Rectangle, "Подложка", 0, 0, 10, 10, {0, 0, 0, 255});
    d::renumber(lib, extra);
    lib.root.children[0].children.push_back(extra);
    CHECK(d::move_refusal(lib, {lib.root.children[0].id}, lib.root.children[0].id).find("в саму себя") != std::string::npos);
    CHECK(d::move_refusal(lib, {extra.id}, lib.root.id).find("Наверху библиотеки") != std::string::npos);
}

TEST_CASE("layer move: the example screens are as the editor writes them") {
    const std::filesystem::path dir = std::filesystem::path(FORGE_SOURCE_DIR) / "games" / "examples" / "layer-move";
    write_example(dir, "перенос_пример", move_screen());
    const d::Screen done = moved_screen();
    write_example(dir, "перенос_готово", done);
    CHECK(kids(done, 7) == std::vector<u32>{8, 9, 12, 4});
    CHECK(kids(done, 9) == std::vector<u32>{10, 11, 6});
    const std::string html = d::screen_html(done);
    CHECK(html.find("id=\"n4\"") != std::string::npos);
}
