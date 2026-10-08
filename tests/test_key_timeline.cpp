#include "forge/core/file.h"
#include "forge/core/path.h"
#include "forge/editor/ui_design.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace d = forge::editor::design;

namespace {

d::Motion four_keys() {
    d::Motion m;
    m.kind = d::MotionKind::Custom;
    const forge::f32 ats[] = {0, 0.25f, 0.5f, 1};
    for (int i = 0; i < 4; ++i) {
        d::MotionKey k;
        k.at = ats[i];
        k.x = static_cast<forge::f32>(i * 100); // tells the keys apart
        m.keys.push_back(k);
    }
    m.keys[2].tint = true;
    m.keys[2].color = {255, 0, 0, 255};
    return m;
}

std::vector<forge::f32> xs(const d::Motion& m) {
    std::vector<forge::f32> out;
    for (const d::MotionKey& k : m.keys) out.push_back(k.x);
    return out;
}

std::vector<forge::f32> ats(const d::Motion& m) {
    std::vector<forge::f32> out;
    for (const d::MotionKey& k : m.keys) out.push_back(k.at);
    return out;
}

std::string read_text(const std::filesystem::path& p) {
    std::vector<forge::u8> bytes;
    if (!forge::read_file(p, bytes)) return {};
    std::string out(bytes.begin(), bytes.end());
    std::erase(out, '\r'); // a Windows checkout may turn line ends into CRLF
    return out;
}

} // namespace

TEST_CASE("key timeline: a key carried past its neighbour stays itself") {
    d::Motion m = four_keys();
    CHECK(d::move_motion_key(m, 1, 0.75f) == 2);
    CHECK(ats(m) == std::vector<forge::f32>{0, 0.5f, 0.75f, 1});
    CHECK(xs(m) == std::vector<forge::f32>{0, 200, 100, 300}); // the moved key (x 100) is third now
    CHECK(m.keys[1].tint);                                      // the neighbour kept its colour
    CHECK(m.keys[1].color == d::Color{255, 0, 0, 255});
    // Back again: the same four keys as before.
    CHECK(d::move_motion_key(m, 2, 0.25f) == 1);
    CHECK(m == four_keys());
}

TEST_CASE("key timeline: on a neighbour's very time the moved key stays on its own side") {
    d::Motion m = four_keys();
    // From the left onto 50 %: before the key already there.
    CHECK(d::move_motion_key(m, 1, 0.5f) == 1);
    CHECK(ats(m) == std::vector<forge::f32>{0, 0.5f, 0.5f, 1});
    CHECK(xs(m) == std::vector<forge::f32>{0, 100, 200, 300});
    // From the right onto 50 %: after the keys there.
    m = four_keys();
    CHECK(d::move_motion_key(m, 3, 0.5f) == 3);
    CHECK(xs(m) == std::vector<forge::f32>{0, 100, 200, 300});
    CHECK(ats(m) == std::vector<forge::f32>{0, 0.25f, 0.5f, 0.5f});
    // Further than the time it shares: passes it.
    CHECK(d::move_motion_key(m, 3, 0.4f) == 2);
    CHECK(xs(m) == std::vector<forge::f32>{0, 100, 300, 200});
    // The played keys keep that order (motion_keys sorts stably).
    const std::vector<d::MotionKey> played = d::motion_keys(m);
    REQUIRE(played.size() == 5); // and the layer's own place at the end, where no key is
    CHECK(played[2].x == 300);
}

TEST_CASE("key timeline: the ends of the timeline and keys out of reach") {
    d::Motion m = four_keys();
    CHECK(d::move_motion_key(m, 0, -0.5f) == 0); // already at 0: nothing changes
    CHECK(m == four_keys());
    CHECK(d::move_motion_key(m, 1, 1.5f) == 2); // past the end: at the end, before the key there
    CHECK(ats(m) == std::vector<forge::f32>{0, 0.5f, 1, 1});
    CHECK(xs(m) == std::vector<forge::f32>{0, 200, 100, 300});
    CHECK(d::move_motion_key(m, 3, -2) == 1); // past the start: at 0, after the key there
    CHECK(xs(m) == std::vector<forge::f32>{0, 300, 200, 100});
    const d::Motion before = m;
    CHECK(d::move_motion_key(m, 9, 0.5f) == 9); // no such key
    CHECK(m == before);
}

TEST_CASE("key timeline: keys written out of order are put in order, the moved one followed") {
    d::Motion m = four_keys();
    std::swap(m.keys[0], m.keys[3]); // 1, .25, .5, 0
    // The key at 0.25 (x 100) is index 1 here and in time order too.
    CHECK(d::move_motion_key(m, 1, 0.75f) == 2);
    CHECK(ats(m) == std::vector<forge::f32>{0, 0.5f, 0.75f, 1});
    CHECK(xs(m) == std::vector<forge::f32>{0, 200, 100, 300});
    // An unsorted key found where it lands after sorting: index 0 is the key at 1 (x 300).
    m = four_keys();
    std::swap(m.keys[0], m.keys[3]);
    CHECK(d::move_motion_key(m, 0, 0.1f) == 1);
    CHECK(xs(m) == std::vector<forge::f32>{0, 300, 100, 200});
}

TEST_CASE("key timeline: a moved key reaches the page as one @keyframes, its other properties kept") {
    d::Screen s = d::make_screen("Шкала", 400, 300);
    d::Node n;
    n.id = s.next_id++;
    n.type = d::NodeType::Rectangle;
    n.w = n.h = 50;
    n.motion = four_keys();
    n.motion.duration = 2;
    n.motion.delay = 1;
    n.motion.easing = d::Easing::Linear;
    d::move_motion_key(n.motion, 1, 0.75f);
    s.root.children.push_back(n);
    const std::string html = d::screen_html(s);
    CHECK(html.find("@keyframes m" + std::to_string(n.id)) != std::string::npos);
    CHECK(html.find("75% {") != std::string::npos);
    CHECK(html.find("25% {") == std::string::npos);
    CHECK(html.find("animation: 2s linear 1s infinite m" + std::to_string(n.id)) != std::string::npos);
    d::Screen back;
    REQUIRE(d::load_screen(d::save_screen(s), back));
    CHECK(back.root.children[0].motion == n.motion);
}

namespace {

// games/examples/key-timeline: a coin crossing the screen by four keys, with a delay, back and forth.
d::Screen timeline_screen() {
    d::Screen s = d::make_screen("Шкала: пример", 1920, 1080);
    s.show = d::ScreenShow::Command;
    d::Node title;
    title.id = s.next_id++;
    title.type = d::NodeType::Text;
    title.name = "Заголовок";
    title.x = 660;
    title.y = 80;
    title.w = 600;
    title.h = 60;
    title.text = "Шкала времени";
    title.text_style.size = 48;
    title.text_style.color = {255, 255, 255, 255};
    d::Node coin;
    coin.id = s.next_id++;
    coin.type = d::NodeType::Ellipse;
    coin.name = "Монета";
    coin.x = 200;
    coin.y = 480;
    coin.w = coin.h = 120;
    d::Paint gold;
    gold.color = {0xe8, 0xb0, 0x4a, 0xff};
    coin.fills.push_back(gold);
    d::Motion& m = coin.motion;
    m.kind = d::MotionKind::Custom;
    m.duration = 3;
    m.delay = 0.5f;
    m.easing = d::Easing::Linear;
    m.loop = true;
    m.back = true;
    d::MotionKey k0, k1, k2, k3;
    k1.at = 0.3f;
    k1.x = 400;
    k1.y = -120;
    k2.at = 0.6f;
    k2.x = 800;
    k2.scale = 1.3f;
    k2.tint = true;
    k2.color = {0xff, 0x40, 0x40, 0xff};
    k3.at = 1;
    k3.x = 1200;
    m.keys = {k0, k1, k2, k3};
    s.root.children = {title, coin};
    return s;
}

} // namespace

TEST_CASE("key timeline: the example screen is as the editor writes it") {
    const std::filesystem::path dir = std::filesystem::path(FORGE_SOURCE_DIR) / "games" / "examples" / "key-timeline";
    const d::Screen s = timeline_screen();
    const std::string json = d::save_screen(s), html = d::screen_html(s);
    const std::filesystem::path json_file = dir / forge::utf8_path("шкала_пример.json"), html_file = dir / forge::utf8_path("шкала_пример.html");
    if (std::getenv("FORGE_WRITE_EXAMPLES")) {
        std::filesystem::create_directories(dir);
        REQUIRE(forge::write_file_atomic(json_file, std::span(reinterpret_cast<const forge::u8*>(json.data()), json.size())));
        REQUIRE(forge::write_file_atomic(html_file, std::span(reinterpret_cast<const forge::u8*>(html.data()), html.size())));
    }
    d::Screen back;
    REQUIRE(d::load_screen(read_text(json_file), back));
    CHECK(d::save_screen(back) == json);
    CHECK(read_text(html_file) == html);
    const d::Motion& m = back.root.children[1].motion;
    CHECK(m.kind == d::MotionKind::Custom);
    CHECK(m.keys.size() == 4);
    CHECK(m.keys[2].tint);
    CHECK(html.find("animation: 3s linear 0.5s infinite alternate m3") != std::string::npos);
}
