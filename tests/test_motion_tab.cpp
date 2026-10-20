// The tab of «Векторная» as numbers (step 3 of docs/models/движение-модель.md, part 3а «Код и время»): a clip cut
// into frames by its range, its fps and «Повторять»; the range in .pixel.json; the strip baked by range; a flag of a
// time event dragged with Shift and without; the timeline's scale and ruler; a block's words and a call's lines and
// characters; require of the game's animations/ folder; the picture the code draws (its draw section).

#include "forge/core/file.h"
#include "forge/core/path.h"
#include "forge/pixel/bake.h"
#include "forge/pixel/clip_cut.h"
#include "forge/pixel/code_sites.h"
#include "forge/pixel/doc.h"
#include "forge/pixel/draw.h"
#include "forge/pixel/rig.h"
#include "forge/pixel/script.h"
#include "forge/pixel/timeline.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <ostream> // doctest's INFO prints a string_view through it; MSVC needs the whole class
#include <random>
#include <string>
#include <vector>

using namespace forge;
using namespace forge::pixel;
namespace fs = std::filesystem;

namespace {

constexpr u32 kLimit = 64;

// One part, "a", a square in a 16 × 16 picture.
const char* kSquare = R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16" width="16" height="16">
  <rect id="a" x="2" y="6" width="4" height="4" fill="#ff0000"/>
</svg>)svg";

Rig square() {
    Rig rig;
    std::string why;
    const bool ok = rig.load(kSquare, why);
    INFO(why);
    REQUIRE(ok);
    return rig;
}

PoseScript script_of(std::string_view source) {
    PoseScript s;
    const bool ok = s.load(source);
    INFO(s.error());
    REQUIRE(ok);
    return s;
}

ClipSpec spec(std::string state, u32 frames, f32 fps, bool loop) {
    ClipSpec c;
    c.state = std::move(state);
    c.frames = frames;
    c.fps = fps;
    c.loop = loop;
    return c;
}

ClipSpec ranged(ClipSpec c, f64 from, f64 to) {
    c.ranged = true;
    c.from = from;
    c.to = to;
    return c;
}

// A folder of its own under the system's temporary one, gone at the end.
struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / utf8_path("forge_motion_tab_" + std::to_string(rd()) + "_" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_text(const fs::path& file, std::string_view text) {
    fs::create_directories(file.parent_path());
    const std::vector<u8> bytes(text.begin(), text.end());
    REQUIRE(write_file_atomic(file, bytes));
}

} // namespace

TEST_CASE("Motion tab: a clip's frames by its range, its fps and «Повторять»") {
    // ⌈(to − from)·fps⌉ with «Повторять», one more without it.
    CHECK(range_frames(0, 0.75, 12, true) == 9);
    CHECK(range_frames(0, 0.75, 12, false) == 10);
    CHECK(range_frames(0.25, 0.75, 12, true) == 6);
    CHECK(range_frames(0.25, 0.75, 12, false) == 7);
    CHECK(range_frames(0, 0.7, 12, true) == 9); // 8.4 frames: the last one starts before the end
    CHECK(range_frames(0, 0.7, 12, false) == 10);
    CHECK(range_frames(0, 0.1, 30, true) == 3);
    CHECK(range_frames(0, 0.1, 30, false) == 4);
    CHECK(range_frames(0.1, 0.8, 30, true) == 21); // (0.8 − 0.1) · 30 = 21.000000000000004: no 22nd frame for it
    CHECK(range_frames(0.1, 0.8, 30, false) == 22);
    CHECK(range_frames(0, 0, 12, true) == 1);
    CHECK(range_frames(0, 0, 12, false) == 1);
    CHECK(range_frames(0.5, 0.25, 12, false) == 1);

    // A clip of pose(t) alone: its «Кадров», frame k at k / fps, as before; a range in the file does nothing to it.
    const ClipCut stand = cut_clip(ranged(spec("stand", 4, 10, false), 0.1, 0.2), false, 0);
    CHECK_FALSE(stand.code);
    CHECK(stand.frames == 4);
    CHECK(stand.length == 0.4);
    CHECK(stand.from == 0);
    for (u32 k = 0; k < 4; ++k) CHECK(stand.time(k) == k / 10.0);
    CHECK_FALSE(stand.cut);

    // A clip of the code: counted from its length, the last frame without «Повторять» on its end exactly.
    const ClipCut jump = cut_clip(spec("jump", 4, 12, false), true, 0.75);
    CHECK(jump.code);
    CHECK(jump.frames == 10);
    CHECK(jump.from == 0);
    CHECK(jump.to == 0.75);
    CHECK(jump.time(0) == 0);
    CHECK(jump.time(1) == 1 / 12.0);
    CHECK(jump.time(8) == 8 / 12.0);
    CHECK(jump.time(9) == 0.75);
    const ClipCut odd = cut_clip(spec("jump", 4, 12, false), true, 0.7);
    CHECK(odd.frames == 10);
    CHECK(odd.time(8) == 8 / 12.0);
    CHECK(odd.time(9) == 0.7); // not 0.75: the clip stops at its end
    // The last frame on the range's end exactly, though 0.1 + 21 / 30 is 0.7999999999999999.
    const ClipCut hop = cut_clip(ranged(spec("jump", 4, 30, false), 0.1, 0.8), true, 1.1);
    CHECK(hop.frames == 22);
    CHECK(hop.time(20) == 0.1 + 20 / 30.0);
    CHECK(hop.time(21) == 0.8);
    const ClipCut walk = cut_clip(spec("walk", 4, 12, true), true, 0.75);
    CHECK(walk.frames == 9);
    CHECK(walk.time(8) == 8 / 12.0);
    CHECK(whole_clip(spec("walk", 4, 12, true), true, 0.75).frames == 9);

    // Its range.
    const ClipCut part = cut_clip(ranged(spec("jump", 4, 12, false), 0.25, 0.75), true, 0.75);
    CHECK(part.frames == 7);
    CHECK(part.from == 0.25);
    CHECK(part.time(0) == 0.25);
    CHECK(part.time(1) == 0.25 + 1 / 12.0);
    CHECK(part.time(6) == 0.75);
    CHECK_FALSE(part.cut);
    CHECK(whole_clip(ranged(spec("jump", 4, 12, false), 0.25, 0.75), true, 0.75).frames == 10);
    // A range past the clip's end (the code got shorter): cut to it, and said.
    const ClipCut past = cut_clip(ranged(spec("jump", 4, 12, false), 0.5, 2), true, 0.75);
    CHECK(past.cut);
    CHECK(past.from == 0.5);
    CHECK(past.to == 0.75);
    CHECK(past.frames == 4);
    const ClipCut beyond = cut_clip(ranged(spec("jump", 4, 12, true), 1, 2), true, 0.75);
    CHECK(beyond.cut);
    CHECK(beyond.from == 0.75);
    CHECK(beyond.frames == 1);
    // A clip of no length: one frame.
    const ClipCut none = cut_clip(spec("jump", 4, 12, false), true, 0);
    CHECK(none.frames == 1);
    CHECK(none.time(0) == 0);
}

TEST_CASE("Motion tab: the range of a clip in .pixel.json") {
    PixelDoc d;
    d.svg = "00000000-0000-0000-0000-000000000001";
    d.script = "Ёжик.luau";
    d.clips = {spec("walk", 9, 12, true), ranged(spec("jump", 7, 12, false), 0.25, 0.75)};
    const std::string text = write_pixel_doc(d);
    PixelDoc back;
    std::string why;
    REQUIRE(read_pixel_doc(text, back, why));
    CHECK(back == d);
    CHECK(back.clips[1].ranged);
    CHECK(back.clips[1].from == 0.25);
    CHECK(back.clips[1].to == 0.75);
    CHECK_FALSE(back.clips[0].ranged);
    // Only when it is not all of the clip: a file without ranges has no word of them.
    d.clips[1].ranged = false;
    CHECK(write_pixel_doc(d).find("range") == std::string::npos);

    auto refused = [](const std::string& range) {
        PixelDoc out;
        std::string why;
        CHECK_FALSE(read_pixel_doc(R"({"svg": "x", "script": "x.luau", "clips": {"jump": {"frames": 4, "fps": 12, "loop": false, "range": )" +
                                       range + "}}}",
                                   out, why));
        return why;
    };
    const std::string form = "clips.jump.range: нужно [от, до] в секундах, 0 ≤ от < до ≤ 3600";
    CHECK(refused("[0.5, 0.25]") == form);
    CHECK(refused("[0.5, 0.5]") == form);
    CHECK(refused("[-1, 1]") == form);
    CHECK(refused("[0, 4000]") == form);
    CHECK(refused("1") == form);
    CHECK(refused("[0, 1, 2]") == form);
    CHECK(refused("[0, \"1\"]") == form);
}

TEST_CASE("Motion tab: frames counted from the recording, the strip baked by range") {
    Rig rig = square();
    PoseScript s = script_of(R"(return { clips = {
  jump = function(p)
    local a = part("a")
    a.x(8, 0.5, "linear")
    wait_until("стоп", 0.25)
  end,
  bad = function(p) part("a").x("x") end,
} })");
    PixelDoc doc;
    doc.frame_w = doc.frame_h = 16;
    doc.clips = {spec("jump", 1, 12, false)};
    ClipCut cut;
    std::string why;
    REQUIRE(cut_clip(s, rig, doc, doc.clips[0], cut, why));
    CHECK(cut.code);
    CHECK(cut.length == 0.75);
    CHECK(cut.frames == 10);
    // The length of wait_until from the file: the clip is longer, so it has more frames.
    doc.lengths["jump"].waits["стоп"] = 0.5;
    REQUIRE(cut_clip(s, rig, doc, doc.clips[0], cut, why));
    CHECK(cut.length == 1);
    CHECK(cut.frames == 13);
    PixelDoc counted = doc;
    REQUIRE(count_frames(s, rig, counted, why));
    CHECK(counted.clips[0].frames == 13);

    // Baked by its range: four frames from 0.25 to 0.5, each the pose at its moment.
    doc.clips[0] = ranged(doc.clips[0], 0.25, 0.5);
    Strip strip;
    REQUIRE(bake_strip(rig, s, doc, {}, kLimit, strip, why));
    CHECK(strip.frames == 4);
    REQUIRE(strip.clips.size() == 1);
    CHECK(strip.clips[0].frames == 4);
    const f64 moments[4] = {0.25, 0.25 + 1 / 12.0, 0.25 + 2 / 12.0, 0.5};
    for (u32 k = 0; k < 4; ++k) {
        INFO(k);
        Pose framed, at;
        REQUIRE(frame_pose(s, rig, doc, doc.clips[0], k, framed, why));
        REQUIRE(s.pose(rig, moments[k], "jump", {}, 1, at));
        CHECK(framed.parts["a"].x == at.parts["a"].x);
        CHECK(framed.parts["a"].x == doctest::Approx(16 * moments[k]));
        assets::CookedTexture one;
        REQUIRE(bake_frame(rig, s, doc, doc.clips[0], k, {}, one, why));
        for (u32 y = 0; y < 16; ++y)
            CHECK(std::equal(one.rgba8.begin() + y * 16 * 4, one.rgba8.begin() + (y + 1) * 16 * 4,
                             strip.picture.rgba8.begin() + (y * 64 + k * 16) * 4));
    }

    // A clip of the code that fails names itself; nothing is baked.
    doc.clips = {spec("jump", 1, 12, false), spec("bad", 1, 12, false)};
    CHECK_FALSE(bake_strip(rig, s, doc, {}, kLimit, strip, why));
    CHECK(why == "клип «bad»: строка 7: x: нужно число, а не string");
    CHECK(strip.picture.rgba8.empty());
    // Over the game's limit: how many frames too many.
    doc.clips = {spec("stand", 70, 10, true)};
    CHECK_FALSE(bake_strip(rig, s, doc, {}, kLimit, strip, why));
    CHECK(why == "в клипах 70 кадров, а предел игры 64, лишних 6: уберите кадры у клипов, сузьте диапазоны или поднимите «Предел кадров»");
    doc.clips = {ranged(spec("jump", 1, 12, false), 0, 1)};
    CHECK_FALSE(bake_strip(rig, s, doc, {}, 12, strip, why));
    CHECK(why == "в клипах 13 кадров, а предел игры 12, лишний 1: уберите кадры у клипов, сузьте диапазоны или поднимите «Предел кадров»");
    doc.clips[0] = ranged(doc.clips[0], 0, 0.9);
    CHECK(bake_strip(rig, s, doc, {}, 12, strip, why));
    CHECK(strip.frames == 12);
}

TEST_CASE("Motion tab: a flag dragged, with Shift and without") {
    PoseScript s = script_of(R"(return { clips = { c = function(p)
  local a = part("a")
  wait_until("отрыв", 0.2)
  a.x(1, 0.1)
  wait_until("приземление", 0.3)
  local d = duration("оборот", 0.4)
  a.rot(90, d)
end } })");
    Recording rec;
    REQUIRE(s.record(nullptr, "c", {}, 1, {}, rec));
    REQUIRE(rec.events.size() == 3);
    CHECK(rec.events[1].start == 0.3);
    CHECK(rec.events[2].start == 0.6);

    // Without Shift: its length, and what comes after moves on.
    MotionLengths l = drag_flag(rec, {}, 0, 0.35, false);
    CHECK(l.waits == std::map<std::string, f64>{{"отрыв", 0.35}});
    CHECK(l.durations.empty());
    Recording moved;
    REQUIRE(s.record(nullptr, "c", {}, 1, l, moved));
    CHECK(moved.events[1].start == 0.45);
    CHECK(moved.events[2].start == 0.75);
    // With Shift: the next one gives up as much, the flags after it stay.
    l = drag_flag(rec, {}, 0, 0.35, true);
    CHECK(l.waits == std::map<std::string, f64>{{"отрыв", 0.35}, {"приземление", 0.15}});
    Recording kept;
    REQUIRE(s.record(nullptr, "c", {}, 1, l, kept));
    CHECK(kept.events[1].start == 0.45);
    CHECK(kept.events[1].start + kept.events[1].length == 0.6);
    CHECK(kept.events[2].start == 0.6);
    // Not below 0, either of them.
    l = drag_flag(rec, {}, 0, 0.9, true);
    CHECK(l.waits == std::map<std::string, f64>{{"отрыв", 0.9}, {"приземление", 0}});
    CHECK(drag_flag(rec, {}, 1, 0.1, false).waits == std::map<std::string, f64>{{"приземление", 0}});
    // duration's length goes to durations; the last one has nothing after it to give up.
    l = drag_flag(rec, {}, 2, 1.25, true);
    CHECK(l.durations == std::map<std::string, f64>{{"оборот", 0.65}});
    CHECK(l.waits.empty());
    // To thousandths; the lengths the file has already stay.
    MotionLengths had;
    had.durations["оборот"] = 0.5;
    l = drag_flag(rec, had, 0, 1.0 / 3, true);
    CHECK(l.waits.at("отрыв") == 0.333);
    CHECK(l.waits.at("приземление") == doctest::Approx(0.167));
    CHECK(l.durations == had.durations);
    // An event out of the recording changes nothing.
    CHECK(drag_flag(rec, had, 3, 1, false) == had);
}

TEST_CASE("Motion tab: the timeline's scale, its moves and the ruler") {
    TimeView v;
    v.width = 600;
    v.length = 3;
    v.fps = 12;
    CHECK(v.fit() == 200);
    CHECK(v.most() == 768);
    CHECK(v.x(1.5) == 300);
    CHECK(v.t(300) == 1.5);
    // Nearer round the mouse: the moment under it stays.
    v.zoom(2, 300);
    CHECK(v.per_second() == 400);
    CHECK(v.t(300) == 1.5);
    CHECK(v.start == 0.75);
    v.pan(200);
    CHECK(v.start == 1.25);
    v.pan(10'000); // not past the end
    CHECK(v.start == 1.5);
    v.center(0.25);
    CHECK(v.start == 0);
    v.center(2);
    CHECK(v.start == 1.25);
    v.zoom(100, 0); // 64 points a frame at most
    CHECK(v.per_second() == 768);
    CHECK(v.start == 1.25);
    // Past the most round the mouse: the moment under it stays at the scale it stops at.
    v.zoom(0.5, 300);
    const f64 under = v.t(300);
    v.zoom(100, 300);
    CHECK(v.per_second() == 768);
    CHECK(v.t(300) == doctest::Approx(under).epsilon(1e-12));
    v.center(2);
    v.zoom(0.001, 600); // all of it at least
    CHECK(v.per_second() == 200);
    CHECK(v.start == 0);
    // At the UI's scale 2 the points are window pixels: 64 pixels of the UI a frame are 128 of them.
    v.dp = 2;
    v.zoom(100, 0);
    CHECK(v.most() == 1536);
    CHECK(v.per_second() == 1536);
    v.zoom(0.001, 0);
    v.dp = 1;
    // A clip so short it fits nearer than 64 points a frame: it stays fitted.
    TimeView s;
    s.width = 600;
    s.length = 0.75;
    s.fps = 12;
    s.zoom(4, 100);
    CHECK(s.per_second() == 800);
    CHECK(s.start == 0);

    CHECK(ruler_step(64, 40) == 1);
    CHECK(ruler_step(40, 40) == 1);
    CHECK(ruler_step(200 / 12.0, 40) == 5);
    CHECK(ruler_step(15, 40) == 5);
    CHECK(ruler_step(0.5, 40) == 100);
    CHECK(ruler_step(0.03, 40) == 2000);
    CHECK(ruler_step(0, 40) == 1);
}

TEST_CASE("Motion tab: a block's words, a call's lines and characters, the call at the caret") {
    Recording rec;
    Block b;
    b.prop = Prop::Y;
    b.value.v = 2;
    CHECK(block_words(rec, b) == "y → 2");
    b.value.v = -0.125;
    CHECK(block_words(rec, b) == "y → -0.125");
    b.prop = Prop::Fill;
    b.value.color = {255, 0, 0, 255};
    b.value.v = 1;
    CHECK(block_words(rec, b) == "fill → #ff0000");
    b.value.v = 0.5;
    CHECK(block_words(rec, b) == "fill → #ff0000 0.5");
    b.value.v = 0;
    CHECK(block_words(rec, b) == "fill → свой цвет");
    b.prop = Prop::Morph;
    b.value = {};
    b.value.target = "mouth_open";
    b.value.v = 1;
    CHECK(block_words(rec, b) == "morph → mouth_open");
    b.value.v = 0;
    CHECK(block_words(rec, b) == "morph → своя форма");
    b.prop = Prop::Rot;
    b.to = Block::To::Back;
    CHECK(block_words(rec, b) == "rot → назад");
    b.to = Block::To::Rest;
    CHECK(block_words(rec, b) == "rot → покой");
    b.to = Block::To::Saved;
    CHECK(block_words(rec, b) == "rot → restore");
    b.to = Block::To::Binding;
    CHECK(block_words(rec, b) == "связь");
    b.to = Block::To::Value;
    b.value = {};
    b.value.v = 3;
    b.spring = true;
    CHECK(block_words(rec, b) == "пружина → 3");
    b.spring = false;
    b.frames = {PropValue{}, PropValue{}};
    CHECK(block_words(rec, b) == "tween");

    // Lines and characters, as the code panel counts them.
    const std::string code = "local a = part(\"ёж\")\na.x(\n  1, 0.1)\nwait(1)\n";
    CodeSites sites;
    std::string why;
    u32 why_line = 0;
    REQUIRE(sites.parse(code, why, why_line));
    const auto x = std::find_if(sites.sites().begin(), sites.sites().end(), [](const CallSite& c) { return c.name == "x"; });
    REQUIRE(x != sites.sites().end());
    u32 first = 0, last = 0;
    span_lines(code, x->span, first, last);
    CHECK(first == 2);
    CHECK(last == 3);
    span_lines(code, {0, 0}, first, last);
    CHECK(first == 1);
    CHECK(last == 1);
    const u32 at = static_cast<u32>(code.find("a.x"));
    CHECK(at == 23);
    CHECK(char_index(code, at) == 21); // ё and ж are two bytes each
    CHECK(byte_index(code, 21) == at);
    CHECK(byte_index(code, char_index(code, static_cast<u32>(code.size()))) == code.size());
    CHECK(byte_index(code, 100000) == code.size());
    // The innermost call at a byte; none outside any.
    const std::string nested = "a.x(f(1), 0.1)";
    CodeSites n;
    REQUIRE(n.parse(nested, why, why_line));
    const i32 inner = site_at(n, 5);
    REQUIRE(inner >= 0);
    CHECK(n.sites()[static_cast<usize>(inner)].name == "f");
    const i32 outer = site_at(n, 11);
    REQUIRE(outer >= 0);
    CHECK(n.sites()[static_cast<usize>(outer)].name == "x");
    CHECK(site_at(sites, static_cast<u32>(code.find("local"))) == -1);

    // The panel's text of a file with "\r\n" and a tab, back as it was; its characters and the file's bytes.
    const std::string windows = "local a = 1\r\n\ta.x(\"ё\", 2)\r\nwait(1)\r\n";
    bool crlf = false;
    const std::string panel = panel_text(windows, crlf);
    CHECK(crlf);
    CHECK(panel == "local a = 1\n    a.x(\"ё\", 2)\nwait(1)\n");
    const std::string mostly_lf = panel_text("a\nb\r\nc\n", crlf);
    CHECK_FALSE(crlf);
    CHECK(file_text(mostly_lf, crlf) == "a\nb\nc\n"); // mostly "\n": written with "\n"
    CHECK(file_text("local a = 1\n    a.x\n", true) == "local a = 1\r\n    a.x\r\n");
    const u32 ax = static_cast<u32>(windows.find("a.x"));
    CHECK(panel_char(windows, ax) == 16); // 11 + "\n" + 4 spaces of the tab
    CHECK(file_byte(windows, 16) == ax);
    CHECK(file_byte(windows, 13) == windows.find('\t')); // inside the tab's spaces: the tab
    CHECK(panel_char(windows, static_cast<u32>(windows.find("wait"))) == 28); // "ё" one character, two bytes
    CHECK(file_byte(windows, 1000) == windows.size());
    CHECK(line_char(panel, 1) == 0);
    CHECK(line_char(panel, 2) == 12);
    CHECK(line_char(panel, 3) == 28);
    CHECK(line_char(panel, 99) == 36);

    // «Другое зерно»: another, below a million, the same each time; splitmix64 of the seed, as another program of the
    // same rule counts it.
    CHECK(other_seed(0) == 607535);
    CHECK(other_seed(1) == 822465);
    CHECK(other_seed(12345) == 414944);
    CHECK(other_seed(414944) == 663005);
    CHECK(other_seed(1) == other_seed(1));
    CHECK(other_seed(1) != 1);
    CHECK(other_seed(1) < 1'000'000);
    CHECK(other_seed(other_seed(1)) != other_seed(1));
}

TEST_CASE("Motion tab: require reads animations/ of the game, again when told") {
    TempDir game;
    write_text(game.path / "animations" / utf8_path("движения.luau"), "return { up = function(a) a.y(-2, 0.25) end }");
    write_text(game.path / "animations" / "moves.luau", "return { up = function(a) a.y(-3, 0.25) end }");
    Rig rig = square();
    PoseScript s;
    s.set_module_reader(animations_reader(game.path));
    REQUIRE(s.load(R"(local m = require("движения")
return { clips = { c = function(p) m.up(part("a")) end } })"));
    const Recording* rec = s.recording(rig, "c", {}, 1);
    REQUIRE(rec);
    REQUIRE(rec->files.size() == 2);
    CHECK(rec->files[1] == "движения");
    REQUIRE(rec->blocks.size() == 1);
    CHECK(rec->blocks[0].value.v == -2);
    CHECK(rec->blocks[0].site.file == 1);
    // The module changed on disk: the recording kept stays until forget().
    write_text(game.path / "animations" / utf8_path("движения.luau"), "return { up = function(a) a.y(-4, 0.25) end }");
    CHECK(s.recording(rig, "c", {}, 1)->blocks[0].value.v == -2);
    s.forget();
    REQUIRE(s.recording(rig, "c", {}, 1));
    CHECK(s.recording(rig, "c", {}, 1)->blocks[0].value.v == -4);

    // No such module, a name with a folder, no clip.
    PoseScript missing;
    missing.set_module_reader(animations_reader(game.path));
    CHECK_FALSE(missing.load("local m = require(\"нет\")\nreturn { clips = { c = function(p) end } }"));
    CHECK(missing.error() == "строка 1: require(\"нет\"): нет файла animations/нет.luau в папке игры");
    PoseScript folder;
    folder.set_module_reader(animations_reader(game.path));
    CHECK_FALSE(folder.load("local m = require(\"../moves\")\nreturn { clips = { c = function(p) end } }"));
    CHECK(folder.error() == "строка 1: require(\"../moves\"): имя модуля — без папок и точек, как «движения»");
    // The reader refuses a folder by itself, whoever asks it.
    std::string text, why;
    const ModuleReader read = animations_reader(game.path);
    CHECK(read("moves", text, why));
    CHECK(text == "return { up = function(a) a.y(-3, 0.25) end }");
    for (const char* name : {"../moves", "a/moves", "a\\moves", "c:moves", "..", "."}) {
        why.clear();
        CHECK_FALSE(read(name, text, why));
        CHECK(why == "имя модуля без папок: модуль — файл animations/имя.luau в папке игры");
    }
    CHECK(s.recording(rig, "d", {}, 1) == nullptr);
    CHECK(s.error() == "в коде нет клипа «d»: clips = { d = function(p) … end }");
}

namespace {

// A drawing of the code, REQUIREd to work.
Drawing drawing_of(PoseScript& s, const std::map<std::string, f64>& values = {}, u64 seed = 1) {
    Drawing d;
    const bool ok = s.draw(values, seed, d);
    INFO(s.error());
    REQUIRE(ok);
    return d;
}

// Why the draw of this code fails, "" when it draws (or loads not at all: then why it does not load).
std::string draw_error(std::string_view source) {
    PoseScript s;
    if (!s.load(source)) return "не загружен: " + s.error();
    Drawing d;
    return s.draw({}, 1, d) ? std::string() : s.error();
}

std::string with_draw(std::string_view body) {
    return "return {\n  clips = { stand = function(p) wait(0.1) end },\n  draw = function(p, rng)\n" + std::string(body) + "\n  end,\n}\n";
}

} // namespace

TEST_CASE("Motion tab: draw — every shape and group as the text of an SVG, byte for byte") {
    PoseScript s = script_of(R"lua(return {
  clips = { stand = function(p) wait(0.1) end },
  draw = function(p, rng)
    canvas(32, 24)
    group("body", { x = 1, y = 2, rot = 5, scale = 2, fill = "#f00" }, function()
      rect("torso", { x = 10, y = 14, w = 12, h = 11, r = 3, fill = "#4a7bd0", pivot = { 16, 14 } })
      circle("head", { x = 16, y = 9, r = 6, stroke = "navy", stroke_width = 0.5 })
      group("face", function()
        ellipse("eye", { x = 18, y = 8, rx = 1, ry = 1.5, opacity = 0.5 })
      end)
    end)
    line("stick", { x1 = 0, y1 = 0, x2 = 4, y2 = 4 })
    poly({ points = { 0, 0, 8, 0, 4, 6 }, fill = "rgb(0, 128, 0)" })
    path("рот_открыт", { d = "M 15 11 L 19 11 L 17 13 Z", fill = "none", hidden = true })
  end,
})lua");
    CHECK(s.has_draw());
    const Drawing d = drawing_of(s);
    CHECK(d.canvas);
    CHECK(d.width == 32);
    CHECK(d.height == 24);
    REQUIRE(d.shapes.size() == 8);
    CHECK(d.shapes[0].kind == DrawShape::Kind::Group);
    CHECK(d.shapes[0].line == 5);
    CHECK(d.shapes[1].parent == 0);
    CHECK(d.shapes[1].line == 6);
    CHECK(d.shapes[3].parent == 0);
    CHECK(d.shapes[4].parent == 3);
    CHECK(d.shapes[5].parent == -1);
    CHECK(d.shapes[6].id.empty());
    CHECK(d.shapes[7].line == 14);
    const std::string svg = drawing_svg(d, 99, 99);
    CHECK(svg == R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="24" viewBox="0 0 32 24">
  <g id="body" transform="translate(1 2) rotate(5) scale(2)" fill="#ff0000">
    <rect id="torso" x="10" y="14" width="12" height="11" rx="3" fill="#4a7bd0"/>
    <circle id="head" cx="16" cy="9" r="6" stroke="#000080" stroke-width="0.5"/>
    <g id="face">
      <ellipse id="eye" cx="18" cy="8" rx="1" ry="1.5" opacity="0.5"/>
    </g>
  </g>
  <line id="stick" x1="0" y1="0" x2="4" y2="4" stroke="#000000"/>
  <polygon points="0,0 8,0 4,6" fill="#008000"/>
  <path id="рот_открыт" d="M 15 11 L 19 11 L 17 13 Z" fill="none" display="none"/>
</svg>
)svg");
    // The same code, values and seed: the same drawing.
    CHECK(drawing_of(s) == d);

    // The rig of it: the ids are its parts, nested as drawn; the drawing's pivot, under the file's.
    Rig rig;
    std::string why;
    REQUIRE(rig.load(svg, why));
    CHECK(rig.width() == 32);
    CHECK(rig.height() == 24);
    for (const char* id : {"body", "torso", "head", "face", "eye", "stick", "рот_открыт"}) CHECK(rig.part(id) >= 0);
    const i32 eye = rig.part("eye");
    REQUIRE(eye >= 0);
    CHECK(rig.parts()[static_cast<usize>(eye)].parent == rig.part("face"));
    CHECK(rig.parts()[static_cast<usize>(rig.part("face"))].parent == rig.part("body"));
    PixelDoc doc;
    apply_pivots(rig, doc, &d);
    CHECK(rig.pivot(rig.part("torso")).x == 16);
    CHECK(rig.pivot(rig.part("torso")).y == 14);
    const Part& head = rig.parts()[static_cast<usize>(rig.part("head"))];
    CHECK(rig.pivot(rig.part("head")).x == (head.x0 + head.x1) / 2);
    doc.pivots["torso"] = {3, 4};
    apply_pivots(rig, doc, &d);
    CHECK(rig.pivot(rig.part("torso")).x == 3);
    doc.pivots.clear();
    apply_pivots(rig, doc, &d);
    CHECK(rig.pivot(rig.part("torso")).x == 16);
    apply_pivots(rig, doc);
    CHECK(rig.pivot(rig.part("torso")).x != 16);
}

TEST_CASE("Motion tab: draw — the frame's size without canvas, parameters and the seed, baked") {
    // p.x is not declared: nil, so x is missing.
    CHECK(draw_error(with_draw(R"(    rect("a", { x = p.x, y = 0, w = 2, h = 2 }))")) == "строка 4: rect «a»: нет x");
    PoseScript declared = script_of("return {\n  params = { x = 2 },\n  clips = { stand = function(p) wait(0.1) end },\n"
                                    "  draw = function(p, rng)\n    rect(\"a\", { x = p.x, y = 0, w = 2, h = 2, fill = \"#ff0000\" })\n"
                                    "    rect(\"b\", { x = 0, y = 4, w = rng(1, 1000) / 1000, h = 1 })\n  end,\n}\n");
    const Drawing two = drawing_of(declared);
    CHECK_FALSE(two.canvas);
    CHECK(drawing_svg(two, 40, 30).rfind(R"(<svg xmlns="http://www.w3.org/2000/svg" width="40" height="30" viewBox="0 0 40 30">)", 0) == 0);
    CHECK(two.shapes[0].x == 2);
    CHECK(drawing_of(declared, {{"x", 5}}).shapes[0].x == 5);
    CHECK(drawing_of(declared, {}, 7) == drawing_of(declared, {}, 7));
    CHECK(drawing_of(declared, {}, 7).shapes[1].w != drawing_of(declared, {}, 8).shapes[1].w);

    // Baked from the drawing: the frame is the picture's units, so the square is at its pixels.
    Rig rig;
    std::string why;
    REQUIRE(rig.load(drawing_svg(two, 8, 8), why));
    PixelDoc doc;
    doc.frame_w = doc.frame_h = 8;
    doc.clips = {spec("stand", 1, 10, true)};
    apply_pivots(rig, doc, &two);
    assets::CookedTexture frame;
    REQUIRE(bake_frame(rig, declared, doc, doc.clips[0], 0, bake_options(doc, {}), frame, why));
    REQUIRE(frame.width == 8);
    auto px = [&](u32 x, u32 y) { return frame.rgba8[(static_cast<usize>(y) * 8 + x) * 4]; };
    CHECK(px(2, 0) == 255);
    CHECK(px(3, 1) == 255);
    CHECK(frame.rgba8[(0 * 8 + 2) * 4 + 3] == 255);
    CHECK(frame.rgba8[(3 * 8 + 6) * 4 + 3] == 0);
}

TEST_CASE("Motion tab: draw — errors with their line") {
    CHECK(draw_error(with_draw(R"(    rect("a", { x = 1, y = 2, h = 3 }))")) == "строка 4: rect «a»: нет w");
    CHECK(draw_error(with_draw(R"(    rect("a", { x = 1, y = 2, w = -1, h = 3 }))")) == "строка 4: rect «a»: w не меньше 0");
    CHECK(draw_error(with_draw(R"(    rect("a", { x = 1, y = 2, w = "4", h = 3 }))")) == "строка 4: rect «a»: w — число");
    CHECK(draw_error(with_draw(R"(    circle("a", { x = 1, y = 2, r = 1, fill = "blu" }))")) ==
          "строка 4: circle «a»: fill: цвет «blu» не читается (#rgb, #rrggbb, rgb(r, g, b), имя вроде red или \"none\")");
    CHECK(draw_error(with_draw("    rect(\"a\", { x = 1, y = 2, w = 1, h = 1 })\n    circle(\"a\", { x = 1, y = 2, r = 1 })")) ==
          "строка 5: circle «a»: id «a» уже есть в рисунке");
    CHECK(draw_error(with_draw(R"(    rect("a", { x = 1, y = 2, widht = 1, h = 1 }))")) ==
          "строка 4: rect «a»: поля «widht» нет (есть x, y, w, h, r, fill, stroke, stroke_width, dash, cap, join, opacity, hidden, pivot)");
    CHECK(draw_error(with_draw("    rect(\"a\", { x = 1, y = 2, w = 1, h = 1 })\n    canvas(8, 8)")) == "строка 5: canvas — до первой фигуры");
    CHECK(draw_error(with_draw("    canvas(8, 8)\n    canvas(8, 8)")) == "строка 5: canvas — один раз");
    CHECK(draw_error(with_draw("    canvas(0, 8)")) == "строка 4: canvas(w, h): w и h больше 0");
    CHECK(draw_error(with_draw(R"(    rect("1a", { x = 1, y = 2, w = 1, h = 1 }))")) == "строка 4: rect: id «1a» — буквы, цифры, _ и -, не с цифры и не с -");
    CHECK(draw_error(with_draw(R"(    rect("a b", { x = 1, y = 2, w = 1, h = 1 }))")) == "строка 4: rect: id «a b» — буквы, цифры, _ и -, не с цифры и не с -");
    CHECK(draw_error(with_draw(R"(    rect("a"))")) == "строка 4: rect «a»: поля таблицей: rect(\"id\", { x, y, w, h, r })");
    CHECK(draw_error(with_draw(R"(    group("g", { x = 1 }))")) == "строка 4: group «g»: последним идёт функция: group(\"body\", function() … end)");
    CHECK(draw_error(with_draw(R"(    rect({ x = 1, y = 2, w = 1, h = 1, pivot = { 1, 1 } }))")) ==
          "строка 4: rect: pivot у фигуры без id: опорная точка бывает у части");
    CHECK(draw_error(with_draw(R"(    rect("a", { x = 1, y = 2, w = 1, h = 1, pivot = { 1 } }))")) ==
          "строка 4: rect «a»: pivot — два числа: pivot = { 16, 14 }");
    CHECK(draw_error(with_draw(R"(    rect("a", { x = 1, y = 2, w = 1, h = 1, opacity = 2 }))")) == "строка 4: rect «a»: opacity от 0 до 1");
    CHECK(draw_error(with_draw(R"(    rect("a", { x = 1, y = 2, w = 1, h = 1, hidden = 1 }))")) == "строка 4: rect «a»: hidden — true или false");
    CHECK(draw_error(with_draw(R"(    poly("a", { points = { 0, 0, 1, 1 } }))")) == "строка 4: poly «a»: points — пары чисел x, y, не меньше трёх точек");
    CHECK(draw_error(with_draw(R"(    path("a", { d = "M 0 0 Q" }))")).rfind("строка 4: path «a»: d не читается: ", 0) == 0);
    CHECK(draw_error(with_draw(R"(    part("a"))")) == "строка 4: part — в клипах и pose: в draw частей ещё нет, их рисуют rect, circle, group …");
    CHECK(draw_error(with_draw("    local x = 1")) == "draw ничего не нарисовал: rect, circle, ellipse, line, poly, path, group");
    CHECK(draw_error(with_draw("    group(\"g\", function()\n      rect(\"a\", { x = nil + 1 })\n    end)")).rfind("строка 5: ", 0) == 0);
    CHECK(draw_error("return { clips = { stand = function(p) wait(0.1) end }, draw = 3 }") ==
          "не загружен: draw должна быть функцией: draw = function(p, rng) … end");

    // Drawing in a clip, a clip's call in draw.
    PoseScript s = script_of("return {\n  clips = { stand = function(p)\n    rect(\"a\", { x = 0, y = 0, w = 1, h = 1 })\n  end },\n}\n");
    Rig rig = square();
    Recording rec;
    CHECK_FALSE(s.record(&rig, "stand", {}, 1, {}, rec));
    CHECK(s.error() == "строка 3: rect рисует только внутри draw = function(p, rng) … end");
    CHECK(draw_error(with_draw("    wait(1)")) == "строка 4: wait — внутри функции клипа: clips = { walk = function(p) … end }");
    PoseScript none = script_of("return { clips = { stand = function(p) wait(0.1) end } }");
    Drawing d;
    CHECK_FALSE(none.has_draw());
    CHECK_FALSE(none.draw({}, 1, d));
    CHECK(none.error() == "в коде нет draw = function(p, rng) … end");
}

TEST_CASE("Motion tab: a character the code draws has no svg in .pixel.json") {
    PixelDoc doc;
    doc.script = "Персонаж.luau";
    doc.clips = {spec("stand", 4, 10, true)};
    const std::string text = write_pixel_doc(doc);
    CHECK(text.find("\"svg\":") == std::string::npos);
    PixelDoc back;
    std::string why;
    REQUIRE(read_pixel_doc(text, back, why));
    CHECK(back == doc);
    doc.svg = "d2ca8023-0301-43dd-9e28-f01cae144e16";
    CHECK(write_pixel_doc(doc).find("\"svg\": \"d2ca8023-0301-43dd-9e28-f01cae144e16\"") != std::string::npos);
}
