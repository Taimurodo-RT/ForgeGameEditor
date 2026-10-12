// Edits of a picture for the «Изображения» tab: the brush, line, fill and the picture's colours; the
// transformations, the frame grid; the effects. Each touches only its area: a frame's edit leaves the other frames
// byte for byte as they were.

#include "forge/assets/image_edit.h"

#include <doctest/doctest.h>

#include <set>
#include <string>
#include <vector>

using namespace forge;
using namespace forge::assets;

namespace {

CookedTexture filled(u32 w, u32 h, Rgba c) {
    CookedTexture t;
    t.width = w;
    t.height = h;
    t.rgba8.resize(static_cast<usize>(w) * h * 4);
    for (usize i = 0; i < t.rgba8.size(); i += 4) {
        t.rgba8[i] = c.r;
        t.rgba8[i + 1] = c.g;
        t.rgba8[i + 2] = c.b;
        t.rgba8[i + 3] = c.a;
    }
    return t;
}

// A strip of n frames w × h, frame i of colour colors[i].
CookedTexture strip(u32 n, u32 w, u32 h, const std::vector<Rgba>& colors) {
    CookedTexture t = filled(n * w, h, {});
    for (u32 i = 0; i < n; ++i)
        for (u32 y = 0; y < h; ++y)
            for (u32 x = 0; x < w; ++x) set_pixel(t, i * w + x, y, colors[i]);
    return t;
}

constexpr Rgba kRed{255, 0, 0, 255}, kGreen{0, 255, 0, 255}, kBlue{0, 0, 255, 255}, kWhite{255, 255, 255, 255},
    kClear{0, 0, 0, 0}, kYellow{255, 255, 0, 255};

usize count(const CookedTexture& t, Rgba c) {
    usize n = 0;
    for (u32 y = 0; y < t.height; ++y)
        for (u32 x = 0; x < t.width; ++x) n += pixel_at(t, x, y) == c;
    return n;
}

} // namespace

TEST_CASE("Image edit: frames of a strip as the game cuts them") {
    const CookedTexture s = strip(4, 8, 6, {kRed, kGreen, kBlue, kWhite});
    CHECK(frame_rect(s, 4, 0) == PixelRect{0, 0, 8, 6});
    CHECK(frame_rect(s, 4, 3) == PixelRect{24, 0, 8, 6});
    CHECK(frame_rect(s, 4, 4).empty());
    CHECK(frame_rect(s, 3, 0).empty()); // 32 does not divide by 3
    CHECK(frame_rect(s, 1, 0) == whole_rect(s));
    CHECK(united({1, 1, 2, 2}, {5, 0, 1, 1}) == PixelRect{1, 0, 5, 3});
    CHECK(united({}, {5, 0, 1, 1}) == PixelRect{5, 0, 1, 1});
}

TEST_CASE("Image edit: brush, line, eraser keep to their area") {
    CookedTexture s = strip(2, 8, 8, {kWhite, kWhite});
    const std::vector<u8> second = copy_rect(s, frame_rect(s, 2, 1));
    // A brush of 3 at (1, 1): pixels 0..2 × 0..2.
    CHECK(paint(s, 1, 1, 3, kRed, frame_rect(s, 2, 0)) == PixelRect{0, 0, 3, 3});
    CHECK(count(s, kRed) == 9);
    // A brush of 4 at the frame's right edge: what would fall into frame 1 does not.
    CHECK(paint(s, 7, 4, 4, kBlue, frame_rect(s, 2, 0)) == PixelRect{6, 3, 2, 4});
    CHECK(copy_rect(s, frame_rect(s, 2, 1)) == second);
    // The same colour again changes nothing.
    CHECK(paint(s, 1, 1, 1, kRed, whole_rect(s)).empty());
    // A line: both ends, every step one pixel.
    CookedTexture l = filled(10, 10, kClear);
    CHECK(paint_line(l, 0, 0, 9, 3, 1, kGreen, whole_rect(l)) == PixelRect{0, 0, 10, 4});
    CHECK(count(l, kGreen) == 10);
    CHECK(pixel_at(l, 0, 0) == kGreen);
    CHECK(pixel_at(l, 9, 3) == kGreen);
    // The eraser: alpha 0.
    CHECK(paint(l, 0, 0, 1, kClear, whole_rect(l)) == PixelRect{0, 0, 1, 1});
    CHECK(pixel_at(l, 0, 0).a == 0);
}

TEST_CASE("Image edit: fill takes exactly one colour joined by sides, inside its area") {
    // A white frame with a red wall down column 3; a diagonal gap does not let the fill through.
    CookedTexture s = strip(2, 8, 8, {kWhite, kWhite});
    for (u32 y = 0; y < 8; ++y) set_pixel(s, 3, y, kRed);
    set_pixel(s, 5, 5, Rgba{255, 255, 254, 255}); // almost white is another colour
    const std::vector<u8> second = copy_rect(s, frame_rect(s, 2, 1));
    CHECK(fill(s, 0, 0, kBlue, frame_rect(s, 2, 0)) == PixelRect{0, 0, 3, 8});
    CHECK(count(s, kBlue) == 24);
    CHECK(pixel_at(s, 4, 0) == kWhite);
    // The right side of the wall: up to the frame's edge, not into frame 1, the almost white pixel kept.
    CHECK(fill(s, 4, 0, kGreen, frame_rect(s, 2, 0)) == PixelRect{4, 0, 4, 8});
    CHECK(pixel_at(s, 5, 5) == Rgba{255, 255, 254, 255});
    CHECK(copy_rect(s, frame_rect(s, 2, 1)) == second);
    CHECK(fill(s, 0, 0, kBlue, whole_rect(s)).empty()); // already that colour
    CHECK(fill(s, 20, 0, kBlue, frame_rect(s, 2, 0)).empty()); // the point is outside the area
}

TEST_CASE("Image edit: the picture's colours, the most first") {
    CookedTexture s = filled(4, 4, kWhite);
    for (u32 x = 0; x < 4; ++x) set_pixel(s, x, 0, kRed);
    set_pixel(s, 0, 1, kBlue);
    set_pixel(s, 1, 1, kClear);
    set_pixel(s, 2, 1, Rgba{1, 2, 3, 0}); // transparent is no colour
    const std::vector<Rgba> c = picture_colors(s);
    REQUIRE(c.size() == 3);
    CHECK(c[0] == kWhite);
    CHECK(c[1] == kRed);
    CHECK(c[2] == kBlue);
    CHECK(picture_colors(s, 2).size() == 2);
}

TEST_CASE("Image edit: turns and mirrors of an area and of the whole picture") {
    // Frame 0 of two, 2 × 2: r g / b w.
    CookedTexture s = strip(2, 2, 2, {kWhite, kYellow});
    set_pixel(s, 0, 0, kRed);
    set_pixel(s, 1, 0, kGreen);
    set_pixel(s, 0, 1, kBlue);
    const std::vector<u8> second = copy_rect(s, frame_rect(s, 2, 1));
    REQUIRE(turn_area(s, frame_rect(s, 2, 0), Turn::Cw));
    // Clockwise: b r / w g.
    CHECK(pixel_at(s, 0, 0) == kBlue);
    CHECK(pixel_at(s, 1, 0) == kRed);
    CHECK(pixel_at(s, 0, 1) == kWhite);
    CHECK(pixel_at(s, 1, 1) == kGreen);
    REQUIRE(turn_area(s, frame_rect(s, 2, 0), Turn::Ccw));
    CHECK(pixel_at(s, 0, 0) == kRed);
    CHECK(pixel_at(s, 1, 0) == kGreen);
    REQUIRE(turn_area(s, frame_rect(s, 2, 0), Turn::FlipH));
    CHECK(pixel_at(s, 0, 0) == kGreen);
    REQUIRE(turn_area(s, frame_rect(s, 2, 0), Turn::FlipV));
    CHECK(pixel_at(s, 0, 1) == kGreen);
    REQUIRE(turn_area(s, frame_rect(s, 2, 0), Turn::Half));
    CHECK(pixel_at(s, 0, 0) == kRed);
    CHECK(copy_rect(s, frame_rect(s, 2, 1)) == second);
    // A quarter turn of a frame that is not square: refused, said why, nothing changed.
    CookedTexture wide = strip(2, 3, 2, {kRed, kGreen});
    const std::vector<u8> was = wide.rgba8;
    std::string why;
    CHECK_FALSE(turn_area(wide, frame_rect(wide, 2, 0), Turn::Cw, &why));
    CHECK(why.find("90°") != std::string::npos);
    CHECK(wide.rgba8 == was);
    // The whole picture: a quarter turn swaps its sides.
    CookedTexture t = filled(3, 2, kWhite);
    set_pixel(t, 0, 0, kRed);
    const CookedTexture cw = turned(t, Turn::Cw);
    CHECK(cw.width == 2);
    CHECK(cw.height == 3);
    CHECK(pixel_at(cw, 1, 0) == kRed);
    CHECK(pixel_at(turned(t, Turn::Ccw), 0, 2) == kRed);
    CHECK(pixel_at(turned(t, Turn::Half), 2, 1) == kRed);
}

TEST_CASE("Image edit: size, canvas, trim") {
    CookedTexture t = filled(2, 2, kWhite);
    set_pixel(t, 0, 0, kRed);
    // The nearest pixel: no new colours.
    const CookedTexture big = resized(t, 4, 4, false);
    CHECK(count(big, kRed) == 4);
    CHECK(count(big, kWhite) == 12);
    CHECK(pixel_at(big, 1, 1) == kRed);
    CHECK(pixel_at(big, 2, 1) == kWhite);
    const CookedTexture back = resized(big, 2, 2, false);
    CHECK(back.rgba8 == t.rgba8);
    // Smooth: one colour stays that colour; red and white halved give their middle.
    CHECK(resized(filled(3, 3, kBlue), 7, 5, true).rgba8 == filled(7, 5, kBlue).rgba8);
    CookedTexture two = filled(2, 1, kWhite);
    set_pixel(two, 0, 0, kRed);
    CHECK(pixel_at(resized(two, 1, 1, true), 0, 0) == Rgba{255, 128, 128, 255});
    // Transparent neighbours do not darken: the colour weighted by alpha.
    CookedTexture half = filled(2, 1, kClear);
    set_pixel(half, 0, 0, kRed);
    CHECK(pixel_at(resized(half, 1, 1, true), 0, 0) == Rgba{255, 0, 0, 128});
    // Canvas: the picture where the anchor says.
    const CookedTexture c = with_canvas(t, 4, 4, Anchor::BottomRight);
    CHECK(pixel_at(c, 2, 2) == kRed);
    CHECK(pixel_at(c, 0, 0) == kClear);
    CHECK(pixel_at(with_canvas(t, 4, 4, Anchor::Center), 1, 1) == kRed);
    CHECK(pixel_at(with_canvas(t, 1, 1, Anchor::TopLeft), 0, 0) == kRed);
    CHECK(pixel_at(with_canvas(t, 1, 1, Anchor::BottomRight), 0, 0) == kWhite);
    // Trim: what is not fully transparent.
    CookedTexture s = filled(6, 5, kClear);
    set_pixel(s, 2, 1, kRed);
    set_pixel(s, 4, 3, Rgba{0, 0, 0, 1});
    CHECK(opaque_bounds(s, whole_rect(s)) == PixelRect{2, 1, 3, 3});
    CHECK(opaque_bounds(filled(3, 3, kClear), PixelRect{0, 0, 3, 3}).empty());
    const CookedTexture cut = cropped(s, PixelRect{2, 1, 3, 3});
    CHECK(cut.width == 3);
    CHECK(pixel_at(cut, 0, 0) == kRed);
}

TEST_CASE("Image edit: each frame by itself keeps the frames' order") {
    CookedTexture s = strip(3, 2, 1, {kRed, kGreen, kBlue});
    set_pixel(s, 1, 0, kWhite); // frame 0: red white
    CookedTexture out;
    REQUIRE(each_frame(s, 3, [](const CookedTexture& f) { return turned(f, Turn::FlipH); }, out));
    CHECK(pixel_at(out, 0, 0) == kWhite); // frame 0 mirrored, still first
    CHECK(pixel_at(out, 1, 0) == kRed);
    CHECK(pixel_at(out, 2, 0) == kGreen);
    CHECK(pixel_at(out, 4, 0) == kBlue);
    // Every frame twice as big.
    REQUIRE(each_frame(s, 3, [](const CookedTexture& f) { return resized(f, 4, 2, false); }, out));
    CHECK(out.width == 12);
    CHECK(frame_rect(out, 3, 2) == PixelRect{8, 0, 4, 2});
    CHECK(pixel_at(out, 8, 1) == kBlue);
    std::string why;
    CHECK_FALSE(each_frame(s, 4, [](const CookedTexture& f) { return f; }, out, &why));
    CHECK_FALSE(why.empty());
}

TEST_CASE("Image edit: a sheet of 2 × 2 frames becomes a strip in reading order") {
    // Frames 4 × 3: top left red, top right green, bottom left blue, bottom right white.
    CookedTexture sheet = filled(8, 6, kClear);
    const Rgba colors[4] = {kRed, kGreen, kBlue, kWhite};
    for (u32 i = 0; i < 4; ++i)
        for (u32 y = 0; y < 3; ++y)
            for (u32 x = 0; x < 4; ++x) set_pixel(sheet, (i % 2) * 4 + x, (i / 2) * 3 + y, colors[i]);
    set_pixel(sheet, 4, 0, kYellow); // the green frame's first pixel
    CookedTexture out;
    u32 frames = 0;
    REQUIRE(sheet_to_strip(sheet, 4, 3, 2, out, &frames));
    CHECK(frames == 4);
    CHECK(out.width == 16);
    CHECK(out.height == 3);
    for (u32 i = 0; i < 4; ++i) CHECK(pixel_at(out, i * 4 + 3, 2) == colors[i]);
    CHECK(pixel_at(out, 4, 0) == kYellow);
    // One column of two: the left column only.
    REQUIRE(sheet_to_strip(sheet, 4, 3, 1, out, &frames));
    CHECK(frames == 2);
    CHECK(pixel_at(out, 4, 2) == kBlue);
    std::string why;
    CHECK_FALSE(sheet_to_strip(sheet, 5, 3, 1, out, nullptr, &why));
    CHECK(why.find("не делится") != std::string::npos);
    CHECK_FALSE(sheet_to_strip(sheet, 4, 3, 3, out, nullptr, &why));
}

TEST_CASE("Image edit: effects, each in its area") {
    SUBCASE("hue +120 turns red into green; frame 1 byte for byte as it was") {
        CookedTexture s = strip(2, 2, 2, {kRed, kRed});
        const std::vector<u8> second = copy_rect(s, frame_rect(s, 2, 1));
        CHECK(apply_effect(s, frame_rect(s, 2, 0), {EffectKind::HueSaturation, 120, 0, 0, {}, {}}) == PixelRect{0, 0, 2, 2});
        CHECK(pixel_at(s, 0, 0) == kGreen);
        CHECK(copy_rect(s, frame_rect(s, 2, 1)) == second);
        CookedTexture b = filled(1, 1, kRed);
        apply_effect(b, whole_rect(b), {EffectKind::HueSaturation, -120, 0, 0, {}, {}});
        CHECK(pixel_at(b, 0, 0) == kBlue);
        apply_effect(b, whole_rect(b), {EffectKind::HueSaturation, 0, -100, 0, {}, {}});
        CHECK(pixel_at(b, 0, 0).r == pixel_at(b, 0, 0).g);
    }
    SUBCASE("invert, grey, brightness keep alpha and leave transparent pixels") {
        CookedTexture s = filled(2, 1, Rgba{10, 20, 30, 200});
        set_pixel(s, 1, 0, Rgba{7, 8, 9, 0});
        apply_effect(s, whole_rect(s), {EffectKind::Invert, 0, 0, 0, {}, {}});
        CHECK(pixel_at(s, 0, 0) == Rgba{245, 235, 225, 200});
        CHECK(pixel_at(s, 1, 0) == Rgba{7, 8, 9, 0});
        apply_effect(s, whole_rect(s), {EffectKind::Grayscale, 0, 0, 0, {}, {}});
        CHECK(pixel_at(s, 0, 0) == Rgba{237, 237, 237, 200});
        CookedTexture g = filled(1, 1, Rgba{100, 100, 100, 255});
        apply_effect(g, whole_rect(g), {EffectKind::BrightnessContrast, 20, 0, 0, {}, {}});
        CHECK(pixel_at(g, 0, 0) == Rgba{151, 151, 151, 255});
        CHECK(apply_effect(g, whole_rect(g), {EffectKind::BrightnessContrast, 0, 0, 0, {}, {}}).empty());
    }
    SUBCASE("outline: transparent pixels beside the figure, by sides, in the area") {
        CookedTexture s = strip(2, 5, 5, {kClear, kClear});
        set_pixel(s, 2, 2, kRed);
        set_pixel(s, 5, 2, kRed); // frame 1's left edge: frame 0 gets no outline from it
        CHECK(apply_effect(s, frame_rect(s, 2, 0), {EffectKind::Outline, 0, 0, 0, kBlue, {}}) == PixelRect{1, 1, 3, 3});
        CHECK(count(s, kBlue) == 4);
        CHECK(pixel_at(s, 1, 1) == kClear); // corners are not beside by a side
        CHECK(pixel_at(s, 4, 2) == kClear);
    }
    SUBCASE("alpha threshold leaves only 0 and 255") {
        CookedTexture s = filled(4, 1, kRed);
        const u8 alphas[4] = {0, 127, 128, 254};
        for (u32 x = 0; x < 4; ++x) set_pixel(s, x, 0, Rgba{255, 0, 0, alphas[x]});
        apply_effect(s, whole_rect(s), {EffectKind::AlphaThreshold, 128, 0, 0, {}, {}});
        for (u32 x = 0; x < 4; ++x) CHECK((pixel_at(s, x, 0).a == 0 || pixel_at(s, x, 0).a == 255));
        CHECK(pixel_at(s, 1, 0).a == 0);
        CHECK(pixel_at(s, 2, 0).a == 255);
    }
    SUBCASE("fewer colours: no more than asked, without dithering") {
        CookedTexture s = filled(4, 4, kClear);
        for (u32 y = 0; y < 4; ++y)
            for (u32 x = 0; x < 4; ++x) set_pixel(s, x, y, Rgba{static_cast<u8>(x * 60), static_cast<u8>(y * 60), 0, 255});
        CHECK(picture_colors(s).size() == 16);
        apply_effect(s, whole_rect(s), {EffectKind::ReduceColors, 4, 0, 0, {}, {}});
        CHECK(picture_colors(s).size() <= 4);
        CHECK(picture_colors(s).size() >= 2);
        // Already few: nothing changes.
        const std::vector<u8> was = s.rgba8;
        CHECK(apply_effect(s, whole_rect(s), {EffectKind::ReduceColors, 8, 0, 0, {}, {}}).empty());
        CHECK(s.rgba8 == was);
    }
    SUBCASE("replace a colour: exactly, or with a tolerance") {
        CookedTexture s = filled(3, 1, kRed);
        set_pixel(s, 1, 0, Rgba{250, 3, 0, 255});
        set_pixel(s, 2, 0, kBlue);
        apply_effect(s, whole_rect(s), {EffectKind::ReplaceColor, 0, 0, 0, kRed, kGreen});
        CHECK(pixel_at(s, 0, 0) == kGreen);
        CHECK(pixel_at(s, 1, 0) == Rgba{250, 3, 0, 255});
        apply_effect(s, whole_rect(s), {EffectKind::ReplaceColor, 5, 0, 0, kRed, kGreen});
        CHECK(pixel_at(s, 1, 0) == kGreen);
        CHECK(pixel_at(s, 2, 0) == kBlue);
    }
}
