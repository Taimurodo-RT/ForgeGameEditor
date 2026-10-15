// The tab of «Пиксельные» as numbers (step 3 of docs/models/движение-модель.md, part 3а «Код и время»): a clip cut
// into frames by its range, its fps and «Повторять»; the range in .pixel.json; the strip baked by range; a flag of a
// time event dragged with Shift and without; the timeline's scale and ruler; a block's words and a call's lines and
// characters; require of the game's animations/ folder.

#include "forge/core/file.h"
#include "forge/core/path.h"
#include "forge/pixel/bake.h"
#include "forge/pixel/clip_cut.h"
#include "forge/pixel/code_sites.h"
#include "forge/pixel/doc.h"
#include "forge/pixel/rig.h"
#include "forge/pixel/script.h"
#include "forge/pixel/timeline.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
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
    CHECK(range_frames(0, 0.1, 30, true) == 3); // 0.1 · 30 = 3.0000000000000004
    CHECK(range_frames(0, 0.1, 30, false) == 4);
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
    v.zoom(0.001, 600); // all of it at least
    CHECK(v.per_second() == 200);
    CHECK(v.start == 0);
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

    // «Другое зерно»: another, below a million, the same each time.
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
    CHECK(s.recording(rig, "d", {}, 1) == nullptr);
    CHECK(s.error() == "в коде нет клипа «d»: clips = { d = function(p) … end }");
}
