// The view and the sounds of «Векторная» as numbers (part 3в of docs/models/движение-модель.md, «Просмотр» and
// «Звук»): the zoom's steps, the pan the edges allow, the grid's step and lines, the coordinates and colours as the
// line under the views writes them; the file of sound("шаг") by its three names, the wave's columns, the frames the
// preview sounds a mark in.

#include "forge/audio/audio.h"
#include "forge/core/file.h"
#include "forge/core/path.h"
#include "forge/pixel/clip_cut.h"
#include "forge/pixel/motion.h"
#include "forge/pixel/sound.h"
#include "forge/pixel/view.h"

#include <doctest/doctest.h>

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

bool near(f64 a, f64 b) { return std::abs(a - b) < 1e-9; }

// A view of 288 points with a picture twice as wide as high fitted in: its box at ×1.
PictureView wide() {
    PictureView v;
    v.width = v.height = 288;
    v.box[0] = 0, v.box[1] = 72, v.box[2] = 288, v.box[3] = 216;
    v.fit();
    return v;
}

// A folder of its own under the system's temporary one, gone at the end.
struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / utf8_path("forge_motion_view_" + std::to_string(rd()) + "_" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_bytes(const fs::path& file, const std::vector<u8>& bytes) {
    fs::create_directories(file.parent_path());
    REQUIRE(write_file_atomic(file, bytes));
}

// A WAV file: 16-bit stereo at 48 kHz, so that it reads as its own samples / 32768 with nothing converted.
std::vector<u8> wav(const std::vector<i16>& left, const std::vector<i16>& right) {
    std::vector<u8> out;
    auto u32le = [&](u32 v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
    };
    auto u16le = [&](u16 v) {
        out.push_back(static_cast<u8>(v));
        out.push_back(static_cast<u8>(v >> 8));
    };
    const u32 frames = static_cast<u32>(left.size());
    out.insert(out.end(), {'R', 'I', 'F', 'F'});
    u32le(36 + frames * 4);
    out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    u32le(16);
    u16le(1);
    u16le(2);
    u32le(48000);
    u32le(48000 * 4);
    u16le(4);
    u16le(16);
    out.insert(out.end(), {'d', 'a', 't', 'a'});
    u32le(frames * 4);
    for (u32 i = 0; i < frames; ++i) {
        u16le(static_cast<u16>(left[i]));
        u16le(static_cast<u16>(right[i]));
    }
    return out;
}

// A cut of the code's clip: [from, to] at fps, with «Повторять» or without.
ClipCut cut(f64 from, f64 to, f32 fps, bool loop) {
    ClipCut c;
    c.code = true;
    c.length = to;
    c.from = from;
    c.to = to;
    c.fps = fps;
    c.loop = loop;
    c.frames = range_frames(from, to, fps, loop);
    return c;
}

} // namespace

TEST_CASE("Motion view: the zoom's steps and how they are written") {
    CHECK(zoom_of(0) == 1);
    CHECK(zoom_of(1) == 1.25);
    CHECK(near(zoom_of(12), std::pow(1.25, 12)));
    CHECK(zoom_of(kZoomTop) == 16);
    CHECK(zoom_of(kZoomTop + 1) == 16);
    CHECK(zoom_of(-1) == 1);
    CHECK(zoom_words(0) == "");
    CHECK(zoom_words(1) == "×1,25");
    CHECK(zoom_words(2) == "×1,56");
    CHECK(zoom_words(3) == "×1,95");
    CHECK(zoom_words(4) == "×2,44");
    CHECK(zoom_words(12) == "×14,55");
    CHECK(zoom_words(kZoomTop) == "×16");
}

TEST_CASE("Motion view: zoom round a point, the pan the edges allow") {
    PictureView v = wide();
    CHECK(v.step == 0);
    CHECK(v.affine() == Affine{});

    SUBCASE("at ×1 there is no pan") {
        v.pan(10, -10);
        CHECK(v.affine() == Affine{});
    }
    SUBCASE("the middle stays where it is") {
        v.zoom_to(4, {144, 144});
        const vector::Point m = v.affine().apply({144, 144});
        CHECK(near(m.x, 144));
        CHECK(near(m.y, 144));
        CHECK(near(v.zoom(), std::pow(1.25, 4)));
    }
    SUBCASE("a point by the mouse stays under it while the edges allow") {
        v.zoom_to(6, {200, 150});
        const vector::Point a = v.affine().apply({200, 150});
        CHECK(near(a.x, 200));
        CHECK(near(a.y, 150));
        // The ×1 point under (200, 150) is still under it a step further.
        const vector::Point under = inverse(v.affine()).apply({200, 150});
        v.zoom_to(7, {200, 150});
        const vector::Point b = v.affine().apply(under);
        CHECK(near(b.x, 200));
        CHECK(near(b.y, 150));
    }
    SUBCASE("at the edge the edge wins") {
        // ×1.25: 360 points wide, 180 high in a view of 288: the left edge may not come in, the height stands in the
        // middle, so the point at the corner moves.
        v.zoom_to(1, {0, 0});
        CHECK(near(v.ox, 0));
        CHECK(near(v.oy, (288 - 1.25 * 288) / 2));
        const vector::Point c = v.affine().apply({0, 72});
        CHECK(near(c.x, 0));
        CHECK(near(c.y, 54));
    }
    SUBCASE("the pan stops at the picture's edges") {
        v.zoom_to(4, {144, 144});
        const f64 z = v.zoom();
        v.pan(1e6, 1e6);
        CHECK(near(v.ox, 0));
        CHECK(near(v.oy, -z * 72));
        v.pan(-1e7, -1e7);
        CHECK(near(v.ox, 288 - z * 288));
        CHECK(near(v.oy, 288 - z * 216));
        v.pan(5, 7);
        CHECK(near(v.ox, 288 - z * 288 + 5));
        CHECK(near(v.oy, 288 - z * 216 + 7));
    }
    SUBCASE("steps past the ends stay at them; 0 fits again") {
        v.zoom_to(40, {10, 10});
        CHECK(v.step == kZoomTop);
        CHECK(v.zoom() == 16);
        v.zoom_to(-5, {10, 10});
        CHECK(v.step == 0);
        CHECK(v.affine() == Affine{});
        v.zoom_to(9, {30, 250});
        v.fit();
        CHECK(v.step == 0);
        CHECK(v.affine() == Affine{});
    }
    SUBCASE("the screen to the picture and back") {
        v.zoom_to(5, {100, 200});
        v.pan(-13, 4);
        const Affine to = v.affine() * Affine{2, 0, 0, 2, 0, 72}; // a 144 × 72 picture fitted in
        const vector::Point p{37.5, 12.25};
        const vector::Point back = inverse(to).apply(to.apply(p));
        CHECK(std::abs(back.x - p.x) < 1e-9);
        CHECK(std::abs(back.y - p.y) < 1e-9);
    }
}

TEST_CASE("Motion view: the grid's step and lines") {
    CHECK(grid_step(9) == 1);
    CHECK(grid_step(8) == 1);
    CHECK(grid_step(7.9) == 2);
    CHECK(grid_step(3) == 5);
    CHECK(grid_step(1) == 10);
    CHECK(grid_step(0.5) == 20);
    CHECK(grid_step(0.1) == 100);
    CHECK(grid_step(1e-4) == 1e5);

    auto at = [](const assets::CookedTexture& t, u32 x, u32 y) {
        const u8* p = &t.rgba8[(static_cast<usize>(y) * t.width + x) * 4];
        return std::vector<u8>{p[0], p[1], p[2], p[3]};
    };
    assets::CookedTexture t;
    t.width = 20, t.height = 10;
    t.rgba8.assign(20 * 10 * 4, 0);
    // A unit two pixels wide, x = 0 in column 3 and y = 0 in row 1: lines every 2 units.
    const Affine px{2, 0, 0, 2, 3, 1};
    draw_grid(t, px, 2, 1);
    const std::vector<u8> none{0, 0, 0, 0}, line{128, 128, 128, 102}, axis{128, 128, 128, 204};
    CHECK(at(t, 3, 0) == axis);
    CHECK(at(t, 7, 0) == line);
    CHECK(at(t, 19, 0) == line);
    CHECK(at(t, 7, 1) == axis);
    CHECK(at(t, 7, 5) == line);
    CHECK(at(t, 7, 9) == line);
    CHECK(at(t, 0, 0) == none);
    CHECK(at(t, 4, 0) == none);
    CHECK(at(t, 6, 2) == none);
    u32 lit = 0;
    for (usize i = 3; i < t.rgba8.size(); i += 4) lit += t.rgba8[i] != 0;
    CHECK(lit == 5 * 10 + 3 * 20 - 5 * 3); // columns 3, 7, 11, 15, 19 and rows 1, 5, 9

    // Over white, the grey at 40 %; two pixels wide, the line's next column too.
    assets::CookedTexture w;
    w.width = 8, w.height = 2;
    w.rgba8.assign(8 * 2 * 4, 255);
    draw_grid(w, Affine{4, 0, 0, 4, 2, 2.5}, 1, 2); // no y line within its two rows
    CHECK(at(w, 2, 0) == std::vector<u8>{153, 153, 153, 255}); // the axis: 128·0.8 + 255·0.2
    CHECK(at(w, 3, 0) == std::vector<u8>{153, 153, 153, 255});
    CHECK(at(w, 6, 1) == std::vector<u8>{204, 204, 204, 255}); // x = 1: 128·0.4 + 255·0.6
    CHECK(at(w, 7, 1) == std::vector<u8>{204, 204, 204, 255});
    CHECK(at(w, 4, 0) == std::vector<u8>{255, 255, 255, 255});
}

TEST_CASE("Motion view: coordinates and colours as the line writes them") {
    CHECK(coords_words({12.5, -3}) == "12.5, -3");
    CHECK(coords_words({-0.04, 7.25}) == "0, 7.3");
    CHECK(coords_words({1234.56, -0.96}) == "1234.6, -1");
    CHECK(coords_words({0, 0}) == "0, 0");
    const u8 orange[4] = {255, 136, 0, 255}, faint[4] = {255, 136, 0, 10}, empty[4] = {255, 136, 0, 0};
    CHECK(color_words(orange) == "#ff8800");
    CHECK(color_words(faint) == "#ff8800");
    CHECK(color_words(empty) == "");
}

TEST_CASE("Motion sound: the file by its three names") {
    TempDir dir;
    const fs::path sounds = dir.path / "sounds";
    const std::vector<u8> tone = wav(std::vector<i16>(480, 1000), std::vector<i16>(480, -1000));
    write_bytes(sounds / utf8_path("шаг.wav"), tone);
    SoundFile s = find_sound(sounds, "шаг");
    CHECK(s.file == sounds / utf8_path("шаг.wav"));
    REQUIRE(s.clip);
    CHECK(s.clip->frames() == 480);
    CHECK(s.why.empty());

    // Its own name first.
    write_bytes(sounds / utf8_path("шаг"), wav(std::vector<i16>(96, 5), std::vector<i16>(96, 5)));
    s = find_sound(sounds, "шаг");
    CHECK(s.file == sounds / utf8_path("шаг"));
    REQUIRE(s.clip);
    CHECK(s.clip->frames() == 96);

    std::vector<u8> ogg;
    REQUIRE(read_file(fs::path(FORGE_SOURCE_DIR) / "tests/data/tone.ogg", ogg));
    write_bytes(sounds / utf8_path("звон.ogg"), ogg);
    s = find_sound(sounds, "звон");
    CHECK(s.file == sounds / utf8_path("звон.ogg"));
    CHECK(s.clip);

    s = find_sound(sounds, "нет");
    CHECK(s.file.empty());
    CHECK(!s.clip);
    CHECK(s.why == "звук «нет» не найден: нет sounds/нет, нет.wav, нет.ogg");

    write_bytes(sounds / utf8_path("шум.wav"), {'n', 'o', 't', ' ', 'a', ' ', 's', 'o', 'u', 'n', 'd'});
    s = find_sound(sounds, "шум");
    CHECK(s.file == sounds / utf8_path("шум.wav"));
    CHECK(!s.clip);
    CHECK(s.why.rfind("звук «шум»: sounds/шум.wav не читается (", 0) == 0);

    // A sound of the game is a file of its folder.
    write_bytes(dir.path / utf8_path("выше.wav"), tone);
    CHECK(sound_files(sounds, "../выше").empty());
    CHECK(sound_files(sounds, "a\\b").empty());
    s = find_sound(sounds, "../выше");
    CHECK(!s.clip);
    CHECK(s.file.empty());
    CHECK(sound_files(sounds, "шаг") ==
          std::vector<fs::path>{sounds / utf8_path("шаг"), sounds / utf8_path("шаг.wav"), sounds / utf8_path("шаг.ogg")});
}

TEST_CASE("Motion sound: the wave's columns") {
    // 0,1 s at 48 kHz: the left channel 100, 200 … 1000 for 10 ms each, the right one silent but for one sample.
    std::vector<i16> left(4800), right(4800, 0);
    for (usize i = 0; i < left.size(); ++i) left[i] = static_cast<i16>(100 * (i / 480 + 1));
    right[1000] = -20000;
    std::string error;
    const std::vector<u8> bytes = wav(left, right);
    const audio::ClipPtr clip = audio::decode(bytes, &error);
    REQUIRE_MESSAGE(clip, error);
    REQUIRE(clip->frames() == 4800);
    auto loud = [](f64 v) { return static_cast<f32>(v / 32768.0); };

    // From 0,25 s, cut at 0,33 s; a column every 10 ms from 0,23 s.
    std::vector<f32> cols(12, 0);
    add_wave(*clip, 0.25, 0.33, 0.23, 0.01, cols);
    CHECK(cols[0] == 0);
    CHECK(cols[1] == 0);
    CHECK(cols[2] == loud(100));
    CHECK(cols[3] == loud(200));
    CHECK(cols[4] == loud(20000)); // sample 1000 is in the third 10 ms
    CHECK(cols[5] == loud(400));
    CHECK(cols[9] == loud(800));
    CHECK(cols[10] == 0); // past the clip's end
    CHECK(cols[11] == 0);

    // A second mark of the same name 20 ms later: the louder of the two in each column.
    add_wave(*clip, 0.27, 0.33, 0.23, 0.01, cols);
    CHECK(cols[4] == loud(20000));
    CHECK(cols[5] == loud(400));
    CHECK(cols[6] == loud(20000)); // the second one's loud sample
    CHECK(cols[7] == loud(600));
    CHECK(cols[9] == loud(800));
    CHECK(cols[10] == 0);

    // Its end: the file is 0,1 s long, so nothing after 0,35 s even when the clip goes on.
    std::vector<f32> tail(4, 0);
    add_wave(*clip, 0.25, 9, 0.33, 0.01, tail);
    CHECK(tail[0] == loud(900));
    CHECK(tail[1] == loud(1000));
    CHECK(tail[2] == 0);
    CHECK(tail[3] == 0);
}

TEST_CASE("Motion sound: the frame a mark sounds in, and the marks a preview passes") {
    const ClipCut all = cut(0, 1, 10, true); // frames 0 … 0,9
    CHECK(all.frames == 10);
    CHECK(sound_frame(all, 0) == 0);
    CHECK(sound_frame(all, 0.25) == 3);
    CHECK(sound_frame(all, 0.3) == 3);
    CHECK(sound_frame(all, 0.9) == 9);
    CHECK(sound_frame(all, 0.95) == -1); // after the last frame, before the range comes round
    CHECK(sound_frame(all, -0.1) == -1);

    const ClipCut range = cut(0.25, 1, 10, true); // 0,25 + k / 10, 8 frames
    CHECK(range.frames == 8);
    CHECK(sound_frame(range, 0.25) == 0);
    CHECK(sound_frame(range, 0.3) == 1);
    CHECK(sound_frame(range, 0.2) == -1);

    const ClipCut once = cut(0, 0.75, 12, false); // the last frame at 0,75 exactly
    CHECK(once.frames == 10);
    CHECK(sound_frame(once, 0.75) == 9);
    CHECK(sound_frame(once, 0.74) == 9);
    CHECK(sound_frame(once, 0.76) == -1);

    Recording rec;
    auto mark = [&](Mark::Kind kind, const char* name, f64 t) { rec.marks.push_back({kind, name, t, 0, {}}); };
    mark(Mark::Kind::Sound, "шаг", 0.3);  // frame 3
    mark(Mark::Kind::Emit, "удар", 0.3);  // not a sound
    mark(Mark::Kind::Sound, "звон", 0);   // frame 0
    mark(Mark::Kind::Sound, "хлоп", 0.7); // frame 7
    mark(Mark::Kind::Sound, "шаг", 0.95); // after the last frame: never
    auto names = [&](std::vector<usize> marks) {
        std::string s;
        for (usize i : marks) s += (s.empty() ? "" : " ") + rec.marks[i].name;
        return s;
    };
    // From the start: its own frame sounds too.
    CHECK(names(passed_sounds(rec, all, -1, 0, false)) == "звон");
    CHECK(names(passed_sounds(rec, all, 0, 1, false)) == "");
    // Frames skipped: theirs sound in the frame shown.
    CHECK(names(passed_sounds(rec, all, 1, 8, false)) == "шаг хлоп");
    // Started in the middle, at frame 3.
    CHECK(names(passed_sounds(rec, all, 2, 3, false)) == "шаг");
    // Round the range: the end of one pass, then the start of the next.
    CHECK(names(passed_sounds(rec, all, 5, 0, true)) == "хлоп звон");
    CHECK(names(passed_sounds(rec, all, 8, 4, true)) == "звон шаг");
    // The range from 0,25: the mark at 0 is outside it, the one at 0,3 is its frame 1, the one at 0,95 its last.
    CHECK(names(passed_sounds(rec, range, -1, 7, false)) == "шаг хлоп шаг");
}
