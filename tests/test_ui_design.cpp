#include "forge/core/file.h"
#include "forge/core/path.h"
#include "forge/editor/ui_design.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <span>

using namespace forge;
using namespace forge::editor::design;

namespace {

Node rect_node(u32 id, f32 x, f32 y, f32 w, f32 h) {
    Node n;
    n.id = id;
    n.name = "Прямоугольник " + std::to_string(id);
    n.type = NodeType::Rectangle;
    n.x = x;
    n.y = y;
    n.w = w;
    n.h = h;
    return n;
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

} // namespace

TEST_CASE("ui design: colours read and write as hex") {
    CHECK(parse_color("#fff") == Color{255, 255, 255, 255});
    CHECK(parse_color("#12345680") == Color{0x12, 0x34, 0x56, 0x80});
    CHECK(parse_color(" 0a0b0c ") == Color{10, 11, 12, 255});
    CHECK_FALSE(parse_color("#12345"));
    CHECK_FALSE(parse_color("red"));
    CHECK(color_hex(Color{255, 0, 16, 255}) == "#ff0010");
    CHECK(color_hex(Color{255, 0, 16, 128}) == "#ff001080");
}

TEST_CASE("ui design: a screen survives saving and loading") {
    Screen s = make_screen("Главное меню", 1920, 1080);
    s.root.fills.push_back(Paint{});
    Node panel = rect_node(s.next_id++, 100, 200, 640, 480);
    panel.type = NodeType::Frame;
    panel.layout.mode = LayoutMode::Column;
    panel.layout.gap = 12;
    panel.layout.padding = {24, 24, 24, 24};
    panel.layout.align = 4;
    panel.horizontal = Constraint::Center;
    panel.vertical = Constraint::Scale;
    panel.radius = {16, 16, 16, 16};
    Paint g;
    g.kind = PaintKind::Linear;
    g.angle = 90;
    g.stops = {{Color{255, 0, 0, 255}, 0}, {Color{0, 0, 255, 128}, 1}};
    panel.fills.push_back(g);
    panel.strokes.push_back(Stroke{Color{255, 255, 255, 64}, 2, StrokeAlign::Outside, StrokeStyle::Dashed, true});
    panel.effects.push_back(Effect{EffectKind::DropShadow, Color{0, 0, 0, 100}, 0, 8, 24, 2, true});
    panel.blend = Blend::Multiply;
    panel.opacity = 0.75f;
    Node title;
    title.id = s.next_id++;
    title.name = "Заголовок";
    title.type = NodeType::Text;
    title.text = "Пауза\nвторая строка";
    title.text_style.size = 40;
    title.text_style.weight = 700;
    title.width_sizing = Sizing::Fill;
    title.height_sizing = Sizing::Hug;
    panel.children.push_back(title);
    s.root.children.push_back(panel);
    s.guides.push_back({true, 960});

    const std::string json = save_screen(s);
    Screen back;
    std::string error;
    REQUIRE(load_screen(json, back, &error));
    CHECK(back.title == "Главное меню");
    CHECK(back.guides == s.guides);
    REQUIRE(back.root.children.size() == 1);
    const Node& p = back.root.children[0];
    CHECK(p.layout == panel.layout);
    CHECK(p.horizontal == Constraint::Center);
    CHECK(p.vertical == Constraint::Scale);
    CHECK(p.fills == panel.fills);
    CHECK(p.strokes == panel.strokes);
    CHECK(p.effects == panel.effects);
    CHECK(p.blend == Blend::Multiply);
    CHECK(p.opacity == doctest::Approx(0.75));
    REQUIRE(p.children.size() == 1);
    CHECK(p.children[0].text == "Пауза\nвторая строка");
    CHECK(p.children[0].text_style == title.text_style);
    CHECK(p.children[0].width_sizing == Sizing::Fill);
    CHECK(back.next_id == s.next_id);
    CHECK(save_screen(back) == json);

    CHECK_FALSE(load_screen("{ broken", back, &error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("ui design: drawn art and screen settings survive saving and become CSS") {
    Screen s = make_screen("Окно", 1280, 720);
    s.text.family = "Exo 2";
    s.text.size = 30;
    s.text.color = Color{10, 20, 30, 255};
    s.fit = ScreenFit::Fit;
    s.bars = Color{1, 2, 3, 255};
    s.safe = 48;
    Node panel = rect_node(s.next_id++, 100, 100, 400, 300);
    Paint tile;
    tile.kind = PaintKind::Image;
    tile.image = "pictures/камень.png";
    tile.fit = ImageFit::Tile;
    tile.tile = 64;
    tile.offset_x = 5;
    tile.offset_y = -3;
    panel.fills.push_back(tile);
    panel.frame.image = "pictures/рамка.png";
    panel.frame.slice = {20, 24, 20, 24};
    panel.frame.scale = 2;
    panel.frame.repeat = ArtRepeat::Round;
    panel.mask.image = "pictures/клякса.png";
    panel.mask.fit = ImageFit::Fit;
    s.root.children.push_back(panel);
    Node label = rect_node(s.next_id++, 10, 10, 100, 40);
    label.type = NodeType::Text;
    label.text = "Привет";
    label.text_style.family.clear(); // the screen's font
    label.text_style.size = 0;
    s.root.children.push_back(label);

    Screen back;
    REQUIRE(load_screen(save_screen(s), back));
    CHECK(back.text.family == "Exo 2");
    CHECK(back.text.size == 30);
    CHECK(back.text.color == s.text.color);
    CHECK(back.fit == ScreenFit::Fit);
    CHECK(back.bars == s.bars);
    CHECK(back.safe == 48);
    REQUIRE(back.root.children.size() == 2);
    CHECK(back.root.children[0].fills == panel.fills);
    CHECK(back.root.children[0].frame == panel.frame);
    CHECK(back.root.children[0].mask == panel.mask);
    CHECK(back.root.children[1].text_style.family.empty());
    CHECK(back.root.children[1].text_style.size == 0);

    const std::string css = node_css(panel, &s.root, 1280, 720);
    CHECK(contains(css, "background-size: 64px auto"));
    CHECK(contains(css, "background-position: 5px -3px"));
    CHECK(contains(css, "background-repeat: repeat"));
    CHECK(contains(css, "border-image-slice: 20 24 20 24 fill"));
    CHECK(contains(css, "border-image-width: 40px 48px 40px 48px"));
    CHECK(contains(css, "border-image-repeat: round"));
    CHECK(contains(css, "mask-size: contain"));
    const std::string text_css = node_css(label, &s.root, 1280, 720);
    CHECK_FALSE(contains(text_css, "font-family"));
    CHECK_FALSE(contains(text_css, "font-size"));
    const std::string html = screen_html(s);
    CHECK(contains(html, "font-family: \"Exo 2\""));
    CHECK(contains(html, "font-size: 30px"));
}

TEST_CASE("ui design: a picture fill without a file draws nothing and names no folder") {
    Screen s = make_screen("Окно", 1280, 720);
    Node picture = rect_node(s.next_id++, 100, 100, 160, 160);
    Paint empty;
    empty.kind = PaintKind::Image;
    picture.fills.push_back(empty);
    const std::string alone = node_css(picture, &s.root, 1280, 720);
    CHECK_FALSE(contains(alone, "url("));
    CHECK_FALSE(contains(alone, "background-image"));
    // Under it a colour: only the colour.
    Paint solid;
    solid.kind = PaintKind::Solid;
    solid.color = Color{200, 10, 10, 255};
    picture.fills.insert(picture.fills.begin(), solid);
    const std::string under = node_css(picture, &s.root, 1280, 720);
    CHECK_FALSE(contains(under, "url("));
    CHECK(contains(under, "background-image: linear-gradient("));
    // Read as a file, a folder fails instead of asking for endless memory.
    std::vector<u8> bytes;
    CHECK_FALSE(forge::read_file(std::filesystem::temp_directory_path(), bytes));
    CHECK(bytes.empty());
}

TEST_CASE("ui design: constraints become CSS that keeps the layer in place") {
    Node parent;
    Node n = rect_node(5, 100, 50, 200, 80);
    CHECK(contains(node_css(n, &parent, 1920, 1080), "left: 100px;"));
    CHECK(contains(node_css(n, &parent, 1920, 1080), "width: 200px;"));
    n.horizontal = Constraint::End;
    CHECK(contains(node_css(n, &parent, 1920, 1080), "right: 1620px;"));
    n.horizontal = Constraint::Both;
    const std::string both = node_css(n, &parent, 1920, 1080);
    CHECK(contains(both, "left: 100px;"));
    CHECK(contains(both, "right: 1620px;"));
    CHECK_FALSE(contains(both, "width:"));
    n.horizontal = Constraint::Center;
    CHECK(contains(node_css(n, &parent, 1920, 1080), "left: calc(50% + -860px);"));
    n.vertical = Constraint::Scale;
    CHECK(contains(node_css(n, &parent, 1920, 1080), "top: 4.63%;"));
}

TEST_CASE("ui design: auto layout becomes flex") {
    Node frame;
    frame.id = 2;
    frame.layout.mode = LayoutMode::Row;
    frame.layout.gap = 8;
    frame.layout.align = 5; // middle right
    const std::string css = node_css(frame, nullptr, 0, 0);
    CHECK(contains(css, "display: flex;"));
    CHECK(contains(css, "gap: 8px;"));
    CHECK(contains(css, "justify-content: flex-end;"));
    CHECK(contains(css, "align-items: center;"));
    Node child = rect_node(3, 0, 0, 50, 20);
    child.width_sizing = Sizing::Fill;
    const std::string c = node_css(child, &frame, 400, 100);
    CHECK(contains(c, "position: relative;"));
    CHECK(contains(c, "flex: 1 1 0;"));
    CHECK(contains(c, "height: 20px;"));
}

TEST_CASE("ui design: the page holds every layer") {
    Screen s = make_screen("HUD", 1280, 720);
    Node t;
    t.id = s.next_id++;
    t.type = NodeType::Text;
    t.text = "<Монеты> & очки";
    s.root.children.push_back(t);
    const std::string html = screen_html(s, {{"../fonts.css"}});
    CHECK(contains(html, "<!DOCTYPE html>"));
    CHECK(contains(html, "<link rel=\"stylesheet\" href=\"../fonts.css\">"));
    CHECK(contains(html, "id=\"n1\""));
    CHECK(contains(html, "&lt;Монеты&gt; &amp; очки"));
    CHECK(contains(html, "#n2 {"));
}

TEST_CASE("ui design: tree helpers") {
    Screen s = make_screen("Экран", 100, 100);
    Node a = rect_node(s.next_id++, 0, 0, 10, 10);
    a.type = NodeType::Frame;
    a.children.push_back(rect_node(s.next_id++, 0, 0, 5, 5));
    const u32 inner = a.children[0].id;
    s.root.children.push_back(a);
    CHECK(path_to(s.root, inner) == std::vector<u32>{1, 2, 3});
    CHECK(parent_of(s.root, inner)->id == 2);
    CHECK(fresh_name(s, NodeType::Rectangle) == "Прямоугольник 4");
    std::optional<Node> taken = remove(s.root, 2);
    REQUIRE(taken);
    CHECK(s.root.children.empty());
    renumber(s, *taken);
    CHECK(taken->id == 4);
    CHECK(taken->children[0].id == 5);
}

TEST_CASE("ui design: snapping to edges, middles and equal gaps") {
    SnapTargets t;
    t.frame = {0, 0, 1000, 600};
    t.boxes = {{100, 100, 100, 50}};
    // Left edge 3 px from the other box's right edge.
    SnapResult r = snap_box({203, 310, 80, 40}, t, 6);
    CHECK(r.snapped_x);
    CHECK(r.dx == doctest::Approx(-3));
    CHECK_FALSE(r.snapped_y);
    CHECK_FALSE(r.lines.empty());
    // The middle of the screen.
    r = snap_box({458, 278, 80, 40}, t, 6);
    CHECK(r.dx == doctest::Approx(2));
    CHECK(r.dy == doctest::Approx(2));
    // Equal gaps between two boxes in a row.
    t.boxes = {{0, 400, 100, 50}, {400, 400, 100, 50}};
    r = snap_box({198, 410, 100, 30}, t, 6);
    CHECK(r.dx == doctest::Approx(2));
    CHECK(r.gaps.size() == 2);
    // A grid when nothing is near.
    t.boxes.clear();
    t.frame = {};
    t.grid = 8;
    r = snap_box({13, 21, 10, 10}, t, 2);
    CHECK(r.dx == doctest::Approx(3));
    CHECK(r.dy == doctest::Approx(3));
    // Resizing moves only the dragged edge.
    t = {};
    t.boxes = {{0, 0, 100, 100}};
    r = snap_edges({200, 0, 98, 50}, false, true, false, false, t, 6);
    CHECK(r.dx == doctest::Approx(0));
    r = snap_edges({20, 200, 78, 50}, false, true, false, false, t, 6);
    CHECK(r.dx == doctest::Approx(2));
    CHECK(r.dy == 0);
}

namespace {

// A button component with three states in a library.
Screen button_library() {
    Screen lib = make_screen("Компоненты", 1920, 1080);
    lib.library = true;
    const char* states[] = {"Обычная", "Наведение", "Выключена"};
    const Color colors[] = {{40, 40, 40, 255}, {200, 150, 50, 255}, {90, 90, 90, 255}};
    for (int i = 0; i < 3; ++i) {
        Node b;
        b.type = NodeType::Frame;
        b.name = "Кнопка";
        b.w = 200;
        b.h = 60;
        b.component = "Кнопка";
        b.variant = {{"Состояние", states[i]}};
        b.fills.push_back({});
        b.fills[0].color = colors[i];
        Node label;
        label.type = NodeType::Text;
        label.name = "Надпись";
        label.text = "Играть";
        b.children.push_back(label);
        if (i == 2) b.children[0].visible = false;
        renumber(lib, b);
        lib.root.children.push_back(b);
    }
    return lib;
}

} // namespace

TEST_CASE("ui design: components, variants and instances") {
    Screen lib = button_library();
    const std::vector<Component> list = components(lib);
    REQUIRE(list.size() == 1);
    CHECK(list[0].name == "Кнопка");
    CHECK(list[0].variants.size() == 3);
    REQUIRE(list[0].properties.size() == 1);
    CHECK(list[0].properties[0].values == std::vector<std::string>{"Обычная", "Наведение", "Выключена"});
    CHECK(state_property(list[0], lib) == "Состояние");
    CHECK(state_selector("Наведение") == ":hover");
    CHECK(state_selector("нажата") == ":active");
    CHECK(state_selector("Выключена") == ".disabled");
    CHECK(state_selector("Обычная").empty());

    // The library survives saving.
    Screen lib2;
    REQUIRE(load_screen(save_screen(lib), lib2));
    CHECK(lib2.library);
    CHECK(lib2.root.children[1].variant == lib.root.children[1].variant);
    CHECK(lib2.root.children[1].component == "Кнопка");

    Screen screen = make_screen("Меню", 1920, 1080);
    std::optional<Node> inst = make_instance(screen, lib, "Кнопка");
    REQUIRE(inst);
    CHECK(inst->master == lib.root.children[0].id);
    CHECK(inst->children[0].master == lib.root.children[0].children[0].id);
    inst->x = 100;
    inst->y = 300;
    screen.root.children.push_back(*inst);
    Node& placed = screen.root.children.back();
    CHECK(instance_of(screen.root, placed.children[0].id) == &placed);
    CHECK(instance_of(screen.root, screen.root.id) == nullptr);

    // The author changes the label here; then the component changes.
    placed.children[0].text = "Новая игра";
    placed.children[0].overrides.push_back(override_of("text"));
    CHECK(override_of("fill.0.color") == "fills");
    CHECK(override_of("text_color") == "text_style");
    CHECK(override_of("w") == "size");
    lib.root.children[0].fills[0].color = {10, 20, 30, 255};
    lib.root.children[0].children[0].text_style.size = 40;
    CHECK(sync_instances(screen, lib));
    const Node& synced = screen.root.children.back();
    CHECK(synced.x == 100);
    CHECK(synced.y == 300);
    CHECK(synced.fills[0].color == Color{10, 20, 30, 255});
    CHECK(synced.children[0].text == "Новая игра");
    CHECK(synced.children[0].text_style.size == 40);
    CHECK_FALSE(sync_instances(screen, lib));

    // Another variant.
    Node& again = screen.root.children.back();
    set_variant_value(again.variant, "Состояние", "Выключена");
    CHECK(sync_instance(screen, again, lib));
    CHECK(again.fills[0].color == Color{90, 90, 90, 255});
    CHECK_FALSE(again.children[0].visible);
    CHECK(again.children[0].text == "Новая игра");
    set_variant_value(again.variant, "Состояние", "Обычная");
    sync_instance(screen, again, lib);

    // States on the page: the hover look applies on hover, the disabled one with .disabled.
    HtmlOptions options;
    options.library = &lib;
    const std::string html = screen_html(screen, options);
    const std::string id = std::to_string(again.id);
    const std::string label = std::to_string(again.children[0].id);
    CHECK(html.find("#n" + id + ":hover {") != std::string::npos);
    CHECK(html.find("#c89632") != std::string::npos);
    CHECK(html.find("#n" + id + ".disabled #n" + label + " {\n  display: none;") != std::string::npos);
    CHECK(screen_html(screen).find(":hover") == std::string::npos);

    // A renamed component keeps its copies.
    for (Node& v : lib.root.children) v.component = "Большая кнопка";
    CHECK(sync_instances(screen, lib));
    CHECK(screen.root.children.back().component == "Большая кнопка");

    // Detached, it is plain layers.
    detach(again);
    CHECK(again.component.empty());
    CHECK(again.master == 0);
    CHECK(again.children[0].master == 0);
    CHECK(instance_of(screen.root, again.children[0].id) == nullptr);
}

TEST_CASE("ui design: game colours and text styles follow the library") {
    Screen lib = make_screen("Компоненты", 1920, 1080);
    lib.library = true;
    lib.colors.push_back({"c1", "Золото", {232, 176, 74, 255}});
    NamedTextStyle heading;
    heading.key = "t1";
    heading.name = "Заголовок";
    heading.style.family = "Exo 2";
    heading.style.size = 64;
    heading.style.weight = 800;
    heading.style.color = {255, 240, 200, 255};
    lib.text_styles.push_back(heading);

    Screen lib2;
    REQUIRE(load_screen(save_screen(lib), lib2));
    CHECK(lib2.colors == lib.colors);
    CHECK(lib2.text_styles == lib.text_styles);

    Screen screen = make_screen("Меню", 1920, 1080);
    Node box;
    box.id = screen.next_id++;
    box.type = NodeType::Rectangle;
    box.fills.push_back({});
    box.fills[0].style = "c1";
    Node title;
    title.id = screen.next_id++;
    title.type = NodeType::Text;
    title.text = "Старая шахта";
    title.text_style.style = "t1";
    title.text_style.align = TextAlign::Center;
    screen.root.children = {box, title};
    CHECK(apply_styles(screen, lib));
    CHECK(screen.root.children[0].fills[0].color == Color{232, 176, 74, 255});
    CHECK(screen.root.children[1].text_style.size == 64);
    CHECK(screen.root.children[1].text_style.family == "Exo 2");
    CHECK(screen.root.children[1].text_style.align == TextAlign::Center);
    CHECK(screen.root.children[1].text_style.style == "t1");
    CHECK_FALSE(apply_styles(screen, lib));

    // Changing the game colour changes every layer using it; saved, the link stays.
    lib.colors[0].color = {200, 60, 60, 255};
    CHECK(apply_styles(screen, lib));
    CHECK(screen.root.children[0].fills[0].color == Color{200, 60, 60, 255});
    Screen again;
    REQUIRE(load_screen(save_screen(screen), again));
    CHECK(again.root.children[0].fills[0].style == "c1");
    CHECK(again.root.children[1].text_style.style == "t1");
}

TEST_CASE("ui design: deleted component and style identities stay reserved after reload") {
    Screen lib = make_screen("Library", 1920, 1080);
    lib.library = true;
    const std::string color = fresh_style_key(lib, true);
    const std::string text = fresh_style_key(lib, false);
    lib.colors.push_back({color, "Gold", {200, 150, 50, 255}});
    lib.text_styles.push_back({text, "Heading", {}});
    Node component;
    component.id = lib.next_id++;
    component.component = "Old button";
    const u32 old_master = component.id;
    lib.root.children.push_back(component);
    Screen screen = make_screen("Menu", 1920, 1080);
    auto copy = make_instance(screen, lib, component.component);
    REQUIRE(copy);
    screen.root.children.push_back(*copy);
    lib.root.children.clear();
    lib.colors.clear();
    lib.text_styles.clear();
    Screen reopened;
    REQUIRE(load_screen(save_screen(lib), reopened));
    CHECK(fresh_style_key(reopened, true) != color);
    CHECK(fresh_style_key(reopened, false) != text);
    CHECK(reopened.next_id > old_master);
    component.id = reopened.next_id++;
    component.component = "Unrelated button";
    reopened.root.children.push_back(component);
    CHECK_FALSE(sync_instances(screen, reopened));
    CHECK(screen.root.children[0].component == "Old button");

    // Compatibility with libraries written before identity counters were saved.
    REQUIRE(load_screen(R"({"library":true,"root":{"id":7},"colors":[{"key":"c20"}],"text_styles":[{"key":"t30"}]})", reopened));
    CHECK(reopened.next_id == 8);
    CHECK(fresh_style_key(reopened, true) == "c31");
}

TEST_CASE("ui design: the link to the game survives saving and goes onto the page") {
    Screen screen = make_screen("Лавка", 1280, 720);
    screen.show = ScreenShow::Command;
    screen.pauses = true;
    screen.esc_closes = false;
    screen.fit = ScreenFit::Fit;
    screen.root.fills.clear();
    Node buy = rect_node(screen.next_id++, 10, 10, 200, 60);
    buy.name = "Купить";
    buy.on_click = {{ActionKind::Change, "inv.coins -= 5; shop.bought = 1"}, {ActionKind::Message, "купил"}, {ActionKind::Close, ""}};
    Node bar = rect_node(screen.next_id++, 10, 100, 300, 20);
    bar.bar = {"hero.hearts", "hero.hearts_max", BarFrom::Bottom};
    bar.show_if = "inv.key > 0";
    bar.fills.push_back(Paint{}); // white: it shows something
    Node label;
    label.id = screen.next_id++;
    label.type = NodeType::Text;
    label.text = "Монеты: {inv.coins} & \"всё\"";
    Node empty_frame;
    empty_frame.id = screen.next_id++;
    screen.root.children = {buy, bar, label, empty_frame};

    Screen back;
    REQUIRE(load_screen(save_screen(screen), back));
    CHECK(back.show == ScreenShow::Command);
    CHECK(back.pauses);
    CHECK_FALSE(back.esc_closes);
    CHECK(back.root.children[0].on_click == buy.on_click);
    CHECK(back.root.children[1].bar == bar.bar);
    CHECK(back.root.children[1].show_if == "inv.key > 0");
    CHECK(parse_action("toggle") == ActionKind::Toggle);
    CHECK_FALSE(parse_action("fly"));

    // Screens from before the link stay off the game until shown.
    Screen old;
    REQUIRE(load_screen(R"({"title": "Старый", "settings": {"fit": "expand"}, "root": {"id": 1, "type": "frame"}})", old));
    CHECK(old.show == ScreenShow::Command);

    const std::string html = screen_html(screen);
    CHECK(contains(html, "forge-screen=\"command\" forge-fit=\"fit\" forge-size=\"1280 720\" forge-bars=\"#000000\" forge-pauses=\"1\" forge-esc=\"0\""));
    CHECK(contains(html, "forge-click=\"[[&quot;change&quot;,&quot;inv.coins -= 5; shop.bought = 1&quot;],[&quot;message&quot;,&quot;купил&quot;],[&quot;close&quot;,&quot;&quot;]]\""));
    CHECK(contains(html, "forge-bar-value=\"hero.hearts\" forge-bar-max=\"hero.hearts_max\" forge-bar-from=\"bottom\""));
    CHECK(contains(html, "forge-show-if=\"inv.key &gt; 0\""));
    CHECK(contains(html, "forge-text=\"Монеты: {inv.coins} &amp; &quot;всё&quot;\""));
    // Only what shows something takes clicks: the empty frame lets them through to the world.
    const std::string root = "#n" + std::to_string(screen.root.id);
    CHECK(contains(html, "html, body, " + root + " {\n  pointer-events: none;"));
    const std::string mouse = "#n" + std::to_string(buy.id) + ", #n" + std::to_string(bar.id) + ", #n" + std::to_string(label.id) + " {\n  pointer-events: auto;";
    CHECK(contains(html, mouse));
    CHECK_FALSE(contains(html, "#n" + std::to_string(empty_frame.id) + ", "));

    // On a component's copy, what the copy does in the game is its own.
    CHECK(override_of("click.0.kind") == "game");
    CHECK(override_of("bar.value") == "game");
    CHECK(override_of("show_if") == "game");
}

TEST_CASE("ui design: a window's place and veil survive saving and go onto the page") {
    Screen screen = make_screen("Сундук", 1280, 720);
    screen.show = ScreenShow::Command;
    screen.dim = true;
    screen.over = WindowOver::Game;
    Screen back;
    REQUIRE(load_screen(save_screen(screen), back));
    CHECK(back.dim);
    CHECK(back.over == WindowOver::Game);
    screen.over = WindowOver::Menu;
    REQUIRE(load_screen(save_screen(screen), back));
    CHECK(back.over == WindowOver::Menu);
    CHECK(std::string(window_over_word(WindowOver::Any)) == "any");

    // Windows from before: no veil, anywhere; «везде» is not written.
    Screen old;
    REQUIRE(load_screen(R"({"title": "Старое", "settings": {"show": "command"}, "root": {"id": 1, "type": "frame"}})", old));
    CHECK_FALSE(old.dim);
    CHECK(old.over == WindowOver::Any);
    CHECK_FALSE(contains(save_screen(old), "\"over\""));
    CHECK_FALSE(contains(save_screen(old), "\"dim\""));

    // On the page: the root says where it comes up, and the veil lies under the window over the whole screen.
    const std::string html = screen_html(screen);
    const std::string root = "<div id=\"n" + std::to_string(screen.root.id) + "\"";
    CHECK(contains(html, "forge-dim=\"1\" forge-over=\"menu\""));
    CHECK(contains(html, "#forge-dim {\n  position: absolute;\n  left: 0px;\n  top: 0px;\n  width: 100%;\n  height: 100%;\n"
                         "  background-color: rgba(0, 0, 0, 0.5);\n  pointer-events: auto;\n}"));
    REQUIRE(contains(html, "<div id=\"forge-dim\"></div>"));
    CHECK(html.find("<div id=\"forge-dim\"></div>") < html.find(root));
    // Without the switch, or on a screen that is not a window, there is neither.
    for (const auto& [show, dim] : {std::pair{ScreenShow::Command, false}, {ScreenShow::Playing, true}, {ScreenShow::Menu, true}}) {
        Screen other = screen;
        other.show = show;
        other.dim = dim;
        const std::string page = screen_html(other);
        CHECK_FALSE(contains(page, "forge-dim"));
        CHECK(contains(page, "forge-over") == (show == ScreenShow::Command));
    }
}

TEST_CASE("ui design: a list of the game's survives saving and goes onto the page") {
    Screen screen = make_screen("Сумка", 1280, 720);
    Node list;
    list.id = screen.next_id++;
    list.name = "Список";
    list.w = 400;
    list.h = 300;
    list.list = ListSource::Items;
    list.list_gap = 6;
    Node cell = rect_node(screen.next_id++, 10, 12, 120, 48);
    cell.name = "Ячейка";
    Node name;
    name.id = screen.next_id++;
    name.type = NodeType::Text;
    name.text = "{item.name} ×{item.count}";
    Node icon = rect_node(screen.next_id++, 0, 0, 40, 40);
    icon.picture_from = "item.icon";
    cell.on_click = {{ActionKind::Message, "взять {item.id}"}};
    cell.children = {name, icon};
    Node empty;
    empty.id = screen.next_id++;
    empty.type = NodeType::Text;
    empty.text = "Пусто";
    list.children = {cell, empty};
    screen.root.children = {list};

    Screen back;
    REQUIRE(load_screen(save_screen(screen), back));
    const Node& l = back.root.children[0];
    CHECK(l.list == ListSource::Items);
    CHECK(l.list_gap == 6);
    REQUIRE(l.children.size() == 2);
    CHECK(l.children[0].children[1].picture_from == "item.icon");
    CHECK(l.children[0].children[0].text == "{item.name} ×{item.count}");
    // Not a list: nothing written.
    CHECK_FALSE(contains(save_screen(make_screen("Пустой", 100, 100)), "\"list\""));

    const std::string html = screen_html(screen);
    CHECK(contains(html, "forge-list=\"items\" forge-list-gap=\"6\" forge-cell=\"10 12 120 48\""));
    CHECK(contains(html, "forge-picture=\"item.icon\""));
    CHECK(contains(html, "forge-text=\"{item.name} ×{item.count}\""));
    // The list scrolls, under the mouse's wheel too.
    CHECK(contains(node_css(l, &back.root, 1280, 720), "overflow-y: auto;"));
    CHECK(contains(html, "#n" + std::to_string(list.id) + ", "));
    // The journal.
    list.list = ListSource::Quests;
    screen.root.children = {list};
    CHECK(contains(screen_html(screen), "forge-list=\"quests\""));
    CHECK(list_fields(ListSource::Quests).front().first == "item.title");
    CHECK(list_fields(ListSource::None).empty());

    // On a component's copy, the list and the picture are its own.
    CHECK(override_of("list") == "game");
    CHECK(override_of("list_gap") == "game");
    CHECK(override_of("picture_from") == "game");
}

TEST_CASE("ui design: movement survives saving and goes onto the page") {
    Screen s = make_screen("Магазин", 1280, 720);
    s.appear = Appear::Rise;
    s.appear_time = 0.4f;
    Node coin = rect_node(s.next_id++, 100, 100, 64, 64);
    coin.fills.push_back(Paint{});
    coin.motion.kind = MotionKind::Custom;
    coin.motion.duration = 2;
    coin.motion.back = true;
    MotionKey k;
    k.at = 0.5f;
    k.y = -10;
    k.tint = true;
    k.color = {255, 0, 0, 255};
    k.blur = 2;
    coin.motion.keys.push_back(k);
    Node button = rect_node(s.next_id++, 200, 100, 120, 40);
    button.smooth = 0.2f;
    button.smooth_easing = Easing::EaseOut;
    button.children.push_back(rect_node(s.next_id++, 0, 0, 10, 10));
    s.root.children.push_back(coin);
    s.root.children.push_back(button);

    Screen back;
    REQUIRE(load_screen(save_screen(s), back));
    CHECK(back.appear == Appear::Rise);
    CHECK(back.appear_time == doctest::Approx(0.4f));
    CHECK(back.root.children[0].motion == s.root.children[0].motion);
    CHECK(back.root.children[1].smooth == doctest::Approx(0.2f));
    CHECK(back.root.children[1].smooth_easing == Easing::EaseOut);

    // A custom movement starts and ends where the layer stands.
    const std::vector<MotionKey> keys = motion_keys(s.root.children[0].motion);
    REQUIRE(keys.size() == 3);
    CHECK(keys.front().at == 0);
    CHECK(keys.back().at == 1);

    const std::string page = screen_html(s, {});
    const std::string coin_id = std::to_string(s.root.children[0].id);
    CHECK(contains(page, "@keyframes m" + coin_id));
    CHECK(contains(page, "animation: 2s ease-in-out infinite alternate m" + coin_id));
    CHECK(contains(page, "translate(0px, -10px)"));
    CHECK(contains(page, "background-color: #ff0000"));
    CHECK(contains(page, "blur(2px) brightness(1)"));
    CHECK(contains(page, "transition: all 0.2s ease-out"));
    CHECK(contains(page, "forge-appear=\"rise\" forge-appear-time=\"0.4\""));
    // The child changes smoothly with its button.
    CHECK(contains(node_css(button.children[0], &button, 120, 40, nullptr, 0.2f), "transition: all 0.2s"));

    // On the canvas nothing moves.
    HtmlOptions still;
    still.motion = false;
    CHECK_FALSE(contains(screen_html(s, still), "@keyframes"));

    // Presets scale with their strength.
    Motion pulse;
    pulse.kind = MotionKind::Pulse;
    pulse.strength = 2;
    CHECK(motion_keys(pulse)[1].scale == doctest::Approx(1.16f));
}

// The screens for checking the player's screen sizes by hand (13.2):
// games/examples/screen-sizes/ (copied into a game's ui/ folder). They are
// built here, so they always load and their pages match what the editor
// writes; FORGE_WRITE_EXAMPLES=1 writes them again.
namespace {

Node text_node(u32 id, const char* name, f32 x, f32 y, f32 w, f32 h, const char* text, f32 size, TextAlign align) {
    Node n;
    n.id = id;
    n.name = name;
    n.type = NodeType::Text;
    n.x = x;
    n.y = y;
    n.w = w;
    n.h = h;
    n.text = text;
    n.text_style.size = size;
    n.text_style.align = align;
    return n;
}
Paint solid(Color c) {
    Paint p;
    p.color = c;
    return p;
}

std::pair<Screen, Screen> size_check_screens() {
    // A HUD over the world: coins in the top right corner, hearts at the
    // bottom left, the bag's button at the bottom right, a mark at the top middle.
    Screen hud = make_screen("Проверка: HUD", 1920, 1080);
    hud.show = ScreenShow::Playing;
    hud.fit = ScreenFit::Expand;
    hud.safe = 40;
    Node coins = text_node(hud.next_id++, "Монеты", 1380, 40, 500, 60, "Монеты: {inv.coins}", 40, TextAlign::Right);
    coins.horizontal = Constraint::End;
    Node hearts = rect_node(hud.next_id++, 40, 900, 400, 40); // over the game's own bottom bar
    hearts.name = "Сердца";
    hearts.vertical = Constraint::End;
    hearts.fills = {solid({200, 50, 60, 255})};
    hearts.bar.value = "hero.hearts";
    hearts.bar.max = "hero.hearts_max";
    Node bag = rect_node(hud.next_id++, 1680, 860, 200, 80);
    bag.name = "Кнопка «Сумка»";
    bag.type = NodeType::Frame;
    bag.horizontal = Constraint::End;
    bag.vertical = Constraint::End;
    bag.fills = {solid({42, 32, 24, 240})};
    bag.radius = {12, 12, 12, 12};
    bag.on_click = {{ActionKind::Show, "проверка_сумка"}};
    bag.children = {text_node(hud.next_id++, "Сумка", 0, 18, 200, 44, "Сумка", 34, TextAlign::Center)};
    Node mark = text_node(hud.next_id++, "Середина", 760, 40, 400, 50, "середина экрана", 30, TextAlign::Center);
    mark.horizontal = Constraint::Center;
    hud.root.children = {coins, hearts, bag, mark};

    // The bag: a window in the middle, whole on any screen (bars around),
    // a grid of the hero's things with pictures, a close button.
    Screen bag_screen = make_screen("Проверка: сумка", 1920, 1080);
    bag_screen.show = ScreenShow::Command;
    bag_screen.fit = ScreenFit::Fit;
    bag_screen.bars = {10, 14, 20, 255};
    bag_screen.root.fills = {solid({0, 0, 0, 140})};
    Node window = rect_node(bag_screen.next_id++, 460, 140, 1000, 800);
    window.name = "Окно";
    window.type = NodeType::Frame;
    window.horizontal = Constraint::Center;
    window.vertical = Constraint::Center;
    window.fills = {solid({30, 26, 22, 250})};
    window.radius = {16, 16, 16, 16};
    window.clip = true;
    Node title = text_node(bag_screen.next_id++, "Заголовок", 40, 30, 600, 60, "Сумка", 48, TextAlign::Left);
    Node close = rect_node(bag_screen.next_id++, 900, 30, 60, 60);
    close.name = "Закрыть";
    close.type = NodeType::Frame;
    close.fills = {solid({90, 40, 40, 255})};
    close.radius = {30, 30, 30, 30};
    close.on_click = {{ActionKind::Close, ""}};
    close.children = {text_node(bag_screen.next_id++, "×", 0, 6, 60, 48, "×", 40, TextAlign::Center)};
    Node list;
    list.id = bag_screen.next_id++;
    list.name = "Вещи";
    list.type = NodeType::Frame;
    list.x = 40;
    list.y = 120;
    list.w = 920;
    list.h = 270; // three rows: the game's eleven things scroll
    list.list = ListSource::Items;
    list.list_gap = 10;
    Node cell = rect_node(bag_screen.next_id++, 0, 0, 296, 80);
    cell.name = "Ячейка";
    cell.type = NodeType::Frame;
    cell.fills = {solid({52, 44, 36, 255})};
    cell.radius = {10, 10, 10, 10};
    cell.on_click = {{ActionKind::Message, "взял {item.id}"}};
    Node icon = rect_node(bag_screen.next_id++, 10, 10, 60, 60);
    icon.name = "Картинка предмета";
    icon.picture_from = "item.icon";
    cell.children = {icon, text_node(bag_screen.next_id++, "Название", 84, 22, 200, 36, "{item.name} {item.count}", 26, TextAlign::Left)};
    list.children = {cell, text_node(bag_screen.next_id++, "Пусто", 0, 0, 600, 40, "В сумке пусто", 28, TextAlign::Left)};
    window.children = {title, close, list};
    bag_screen.root.children = {window};
    return {hud, bag_screen};
}

std::string read_text(const std::filesystem::path& p) {
    std::vector<u8> bytes;
    if (!read_file(p, bytes)) return {};
    std::string out(bytes.begin(), bytes.end());
    std::erase(out, '\r'); // a Windows checkout may turn line ends into CRLF
    return out;
}

} // namespace

TEST_CASE("ui design: the screens for checking screen sizes are as the editor writes them") {
    const std::filesystem::path dir = std::filesystem::path(FORGE_SOURCE_DIR) / "games" / "examples" / "screen-sizes";
    const auto [hud, bag] = size_check_screens();
    const std::pair<const char*, const Screen*> files[] = {{"проверка_hud", &hud}, {"проверка_сумка", &bag}};
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
    }
    // What the check needs: stuck to corners, the middle, a list with pictures, a close button.
    CHECK(contains(screen_html(hud), "forge-click=\"[[&quot;show&quot;,&quot;проверка_сумка&quot;]]\""));
    CHECK(contains(screen_html(bag), "forge-fit=\"fit\""));
    CHECK(contains(screen_html(bag), "forge-picture=\"item.icon\""));
}

// The example of windows' behaviour (13.11): games/examples/window-behaviour/ui, a main menu, a screen over the game
// and two windows, as the editor writes them. The game's self-test (forge_slice --test --scene windows) reads these
// files from disk, in the build and in the package; FORGE_WRITE_EXAMPLES=1 writes them again.
namespace {

Node example_button(Screen& s, const char* name, f32 x, f32 y, f32 w, std::vector<Action> actions) {
    Node b = rect_node(s.next_id++, x, y, w, 90);
    b.name = name;
    b.type = NodeType::Frame;
    b.fills = {solid({232, 176, 74, 255})};
    b.radius = {14, 14, 14, 14};
    b.on_click = std::move(actions);
    Node label = text_node(s.next_id++, "Надпись", 0, 20, w, 50, name, 36, TextAlign::Center);
    label.text_style.color = {27, 33, 39, 255};
    b.children = {label};
    return b;
}

Node example_panel(Screen& s, const char* name, f32 x, f32 y, f32 w, f32 h) {
    Node p = rect_node(s.next_id++, x, y, w, h);
    p.name = name;
    p.type = NodeType::Frame;
    p.fills = {solid({36, 48, 63, 255})};
    p.radius = {16, 16, 16, 16};
    return p;
}

Node example_text(Screen& s, const char* name, f32 x, f32 y, f32 w, f32 h, const char* words, f32 size) {
    Node t = text_node(s.next_id++, name, x, y, w, h, words, size, TextAlign::Center);
    t.text_style.color = {235, 240, 245, 255};
    return t;
}

std::vector<std::pair<const char*, Screen>> window_example_screens() {
    // The main menu: «Настройки» opens the window over the menu, «Играть» starts the game.
    Screen menu = make_screen("Окна: меню", 1920, 1080);
    menu.show = ScreenShow::Menu;
    menu.root.fills = {solid({24, 30, 38, 255})};
    menu.root.children = {example_text(menu, "Заголовок", 460, 140, 1000, 90, "Поведение окон", 64),
                          example_button(menu, "Настройки", 760, 340, 400, {{ActionKind::Show, "окна_настройки"}}),
                          example_button(menu, "Играть", 760, 460, 400, {{ActionKind::NewGame, ""}}),
                          example_text(menu, "Громкость", 560, 640, 800, 60, "Громкость: {demo.volume}", 32)};
    // Over the game: the same window, and a button of the game's own to see whether clicks reach it.
    Screen hud = make_screen("Окна: игра", 1920, 1080);
    hud.show = ScreenShow::Playing;
    hud.root.children = {example_button(hud, "Настройки", 40, 40, 300, {{ActionKind::Show, "окна_настройки"}}),
                         example_button(hud, "Монета", 40, 160, 300, {{ActionKind::Change, "demo.coins += 1"}}),
                         example_text(hud, "Монеты", 40, 270, 300, 50, "Монеты: {demo.coins}", 32)};
    // A window as a new one is made (anywhere, closed by Esc, darkening what is under it) that stops the game.
    Screen settings = make_screen("Окна: настройки", 1920, 1080);
    settings.show = ScreenShow::Command;
    settings.pauses = true;
    settings.dim = true;
    Node panel = example_panel(settings, "Окно", 560, 240, 800, 600);
    panel.children = {example_text(settings, "Заголовок", 0, 30, 800, 70, "Настройки", 48),
                      example_text(settings, "Громкость", 0, 110, 800, 50, "Громкость: {demo.volume}", 32),
                      example_button(settings, "Громче", 200, 190, 400, {{ActionKind::Change, "demo.volume += 1"}}),
                      example_button(settings, "Справка", 200, 310, 400, {{ActionKind::Show, "окна_справка"}}),
                      example_button(settings, "Закрыть", 200, 430, 400, {{ActionKind::Close, ""}})};
    settings.root.children = {panel};
    // A second window, only over the game, that Esc does not close and that does not darken: beside it the
    // first window's buttons are still pressed.
    Screen help = make_screen("Окна: справка", 1920, 1080);
    help.show = ScreenShow::Command;
    help.esc_closes = false;
    help.over = WindowOver::Game;
    Node note = example_panel(help, "Окно", 1400, 240, 480, 400);
    note.children = {example_text(help, "Текст", 20, 30, 440, 160, "Справка только над игрой. Esc её не закрывает.", 32),
                     example_button(help, "Понятно", 40, 270, 400, {{ActionKind::Close, ""}})};
    help.root.children = {note};
    return {{"окна_меню", menu}, {"окна_игра", hud}, {"окна_настройки", settings}, {"окна_справка", help}};
}

} // namespace

TEST_CASE("ui design: the example of windows' behaviour is as the editor writes it") {
    const std::filesystem::path dir = std::filesystem::path(FORGE_SOURCE_DIR) / "games" / "examples" / "window-behaviour" / "ui";
    const bool write = std::getenv("FORGE_WRITE_EXAMPLES") != nullptr;
    const auto screens = window_example_screens();
    for (const auto& [name, screen] : screens) {
        CAPTURE(name);
        const std::string json = save_screen(screen), html = screen_html(screen);
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
    }
    // What the game's check needs on the pages: the window's pause, Esc, veil and place.
    CHECK(contains(screen_html(screens[2].second), "forge-screen=\"command\""));
    CHECK(contains(screen_html(screens[2].second), "forge-pauses=\"1\" forge-dim=\"1\""));
    CHECK(contains(screen_html(screens[2].second), "<div id=\"forge-dim\"></div>"));
    CHECK(contains(screen_html(screens[3].second), "forge-esc=\"0\" forge-over=\"game\""));
    CHECK_FALSE(contains(screen_html(screens[3].second), "forge-dim"));
}
