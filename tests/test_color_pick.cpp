#include "forge/core/file.h"
#include "forge/core/path.h"
#include "forge/editor/color_pick.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <span>

namespace d = forge::editor::design;

TEST_CASE("color pick: every colour survives hue, saturation, brightness and back") {
    // All 16.7 million: the picker opened on a colour and closed shows and keeps exactly it.
    forge::usize wrong = 0;
    for (int r = 0; r < 256; ++r)
        for (int g = 0; g < 256; ++g)
            for (int b = 0; b < 256; ++b) {
                const d::Color c{static_cast<forge::u8>(r), static_cast<forge::u8>(g), static_cast<forge::u8>(b), 255};
                if (d::hsva_to_color(d::color_to_hsva(c)) != c) ++wrong;
            }
    CHECK(wrong == 0);
    for (int a = 0; a < 256; ++a) {
        const d::Color c{0x33, 0x66, 0xcc, static_cast<forge::u8>(a)};
        CHECK(d::hsva_to_color(d::color_to_hsva(c)) == c);
    }
}

TEST_CASE("color pick: known colours as hue, saturation, brightness") {
    const d::Hsva red = d::color_to_hsva({255, 0, 0, 255});
    CHECK(red.h == doctest::Approx(0));
    CHECK(red.s == doctest::Approx(1));
    CHECK(red.v == doctest::Approx(1));
    CHECK(d::color_to_hsva({0, 255, 0, 255}).h == doctest::Approx(120));
    CHECK(d::color_to_hsva({0, 0, 255, 255}).h == doctest::Approx(240));
    CHECK(d::color_to_hsva({255, 0, 255, 255}).h == doctest::Approx(300));
    CHECK(d::hsva_to_color({30, 1, 1, 1}) == d::Color{255, 128, 0, 255});
    CHECK(d::hsva_to_color({360, 1, 1, 1}) == d::Color{255, 0, 0, 255});
    CHECK(d::hsva_to_color({200, 0.5f, 0.8f, 0.5f}).a == 128);
}

TEST_CASE("color pick: grey and black keep the hue and saturation the picker had") {
    const d::Hsva keep{210, 0.7f, 0.9f, 1};
    // Down to black in the square, back up: the hue and saturation are still there.
    const d::Hsva black = d::color_to_hsva({0, 0, 0, 255}, keep);
    CHECK(black.h == doctest::Approx(210));
    CHECK(black.s == doctest::Approx(0.7f));
    CHECK(black.v == doctest::Approx(0));
    const d::Hsva grey = d::color_to_hsva({128, 128, 128, 255}, keep);
    CHECK(grey.h == doctest::Approx(210));
    CHECK(grey.s == doctest::Approx(0));
    // Without anything to keep: hue 0.
    CHECK(d::color_to_hsva({128, 128, 128, 255}).h == doctest::Approx(0));
}

TEST_CASE("color pick: what an author types as HEX") {
    CHECK(d::parse_hex_input("#3366cc") == d::Color{0x33, 0x66, 0xcc, 255});
    CHECK(d::parse_hex_input("3366CC") == d::Color{0x33, 0x66, 0xcc, 255});
    CHECK(d::parse_hex_input("  #36c ") == d::Color{0x33, 0x66, 0xcc, 255});
    CHECK(d::parse_hex_input("36c8") == d::Color{0x33, 0x66, 0xcc, 0x88});
    CHECK(d::parse_hex_input("#3366cc80") == d::Color{0x33, 0x66, 0xcc, 0x80});
    // A part of a colour, a typo, nothing: no colour, and a reason in words.
    for (const char* bad : {"", "#", "3366c", "#3366cc8", "33 66 cc", "zz66cc", "#3366ccff00", "red"}) {
        CAPTURE(bad);
        CHECK(!d::parse_hex_input(bad).has_value());
        CHECK(!d::hex_problem(bad).empty());
    }
    CHECK(d::hex_problem("3366c").find("3, 4, 6 или 8") != std::string::npos);
    CHECK(d::hex_problem("zz66cc").find("0–9") != std::string::npos);
    CHECK(d::hex_problem("#3366cc").empty());
}

TEST_CASE("color pick: alpha typed as a percentage") {
    CHECK(d::parse_alpha_input("100") == forge::u8{255});
    CHECK(d::parse_alpha_input("50%") == forge::u8{128});
    CHECK(d::parse_alpha_input(" 0 % ") == forge::u8{0});
    CHECK(!d::parse_alpha_input("").has_value());
    CHECK(!d::parse_alpha_input("101").has_value());
    CHECK(!d::parse_alpha_input("-1").has_value());
    CHECK(!d::parse_alpha_input("пол").has_value());
}

TEST_CASE("color pick: a drawn pixel back to its colour") {
    // The UI draws with alpha premultiplied: half-transparent #3366cc is 26 33 66 at 128.
    CHECK(d::unpremultiply(0x33, 0x66, 0xcc, 255) == d::Color{0x33, 0x66, 0xcc, 255});
    const d::Color half = d::unpremultiply(26, 51, 102, 128);
    CHECK(std::abs(half.r - 0x33) <= 1);
    CHECK(std::abs(half.g - 0x66) <= 1);
    CHECK(std::abs(half.b - 0xcc) <= 1);
    CHECK(half.a == 128);
    CHECK(d::unpremultiply(0, 0, 0, 0) == d::Color{0, 0, 0, 0});
}

namespace {

std::string read_text(const std::filesystem::path& p) {
    std::vector<forge::u8> bytes;
    if (!forge::read_file(p, bytes)) return {};
    return std::string(bytes.begin(), bytes.end());
}

// games/examples/color-picker: colours as the picker leaves them, alpha everywhere it can be.
d::Screen palette_screen() {
    d::Screen s = d::make_screen("Палитра: пример", 1920, 1080);
    s.show = d::ScreenShow::Command;
    auto layer = [&](d::NodeType type, const char* name, forge::f32 x, forge::f32 y, forge::f32 w, forge::f32 h) {
        d::Node n;
        n.id = s.next_id++;
        n.type = type;
        n.name = name;
        n.x = x;
        n.y = y;
        n.w = w;
        n.h = h;
        return n;
    };
    // A see-through panel (#1B2127 at 60 %) with a shadow at 50 %.
    d::Node panel = layer(d::NodeType::Rectangle, "Панель", 560, 240, 800, 600);
    d::Paint glass;
    glass.color = {0x1b, 0x21, 0x27, 0x99};
    panel.fills.push_back(glass);
    panel.radius = {16, 16, 16, 16};
    d::Effect shadow;
    shadow.color = {0, 0, 0, 0x80};
    shadow.y = 8;
    shadow.blur = 24;
    panel.effects.push_back(shadow);
    // A title at 80 %.
    d::Node title = layer(d::NodeType::Text, "Заголовок", 660, 300, 600, 60);
    title.text = "Палитра цвета";
    title.text_style.size = 48;
    title.text_style.color = {0xff, 0xff, 0xff, 0xcc};
    // A gold bar fading out: a gradient whose last stop is see-through.
    d::Node bar = layer(d::NodeType::Rectangle, "Полоса", 660, 420, 600, 40);
    d::Paint fade;
    fade.kind = d::PaintKind::Linear;
    fade.angle = 90;
    fade.stops = {{{0xe8, 0xb0, 0x4a, 0xff}, 0}, {{0xe8, 0xb0, 0x4a, 0x00}, 1}};
    bar.fills.push_back(fade);
    // An opaque button with a stroke at 50 %.
    d::Node button = layer(d::NodeType::Rectangle, "Кнопка", 760, 700, 400, 96);
    d::Paint gold;
    gold.color = {0xe8, 0xb0, 0x4a, 0xff};
    button.fills.push_back(gold);
    d::Stroke edge;
    edge.color = {0xff, 0xff, 0xff, 0x80};
    edge.width = 2;
    button.strokes.push_back(edge);
    for (d::Node* n : {&panel, &title, &bar, &button}) s.root.children.push_back(std::move(*n));
    return s;
}

} // namespace

TEST_CASE("color pick: the example screen keeps every colour and alpha in its file and page") {
    const std::filesystem::path dir = std::filesystem::path(FORGE_SOURCE_DIR) / "games" / "examples" / "color-picker";
    const d::Screen s = palette_screen();
    const std::string json = d::save_screen(s), html = d::screen_html(s);
    const std::filesystem::path json_file = dir / forge::utf8_path("палитра_пример.json"),
                                html_file = dir / forge::utf8_path("палитра_пример.html");
    if (std::getenv("FORGE_WRITE_EXAMPLES")) {
        std::filesystem::create_directories(dir);
        REQUIRE(forge::write_file_atomic(json_file, std::span(reinterpret_cast<const forge::u8*>(json.data()), json.size())));
        REQUIRE(forge::write_file_atomic(html_file, std::span(reinterpret_cast<const forge::u8*>(html.data()), html.size())));
    }
    d::Screen back;
    REQUIRE(d::load_screen(read_text(json_file), back));
    CHECK(d::save_screen(back) == json);
    CHECK(read_text(html_file) == html);
    // Saved and read again: the same colours, alpha included.
    CHECK(back.root.children[0].fills[0].color == d::Color{0x1b, 0x21, 0x27, 0x99});
    CHECK(back.root.children[0].effects[0].color == d::Color{0, 0, 0, 0x80});
    CHECK(back.root.children[1].text_style.color == d::Color{0xff, 0xff, 0xff, 0xcc});
    CHECK(back.root.children[2].fills[0].stops.back().color == d::Color{0xe8, 0xb0, 0x4a, 0});
    CHECK(back.root.children[3].strokes[0].color == d::Color{0xff, 0xff, 0xff, 0x80});
    // The page the game shows: ordinary layers with these colours.
    CHECK(html.find("rgba(27, 33, 39, 0.6)") != std::string::npos);
    CHECK(html.find("rgba(255, 255, 255, 0.8)") != std::string::npos);
    CHECK(html.find("linear-gradient(90deg") != std::string::npos);
}
