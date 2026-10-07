#include "forge/editor/ui_design.h"

#include <doctest/doctest.h>

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
