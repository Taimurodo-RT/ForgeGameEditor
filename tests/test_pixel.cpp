// «Пиксельные» (step 2 of docs/models/вектор-модель.md): the SVG read as written, a character posed and drawn by
// ThorVG, and its frames baked into pixel art. lunasvg, which draws SVG in the interface, is the second opinion: a pose
// must draw as the same SVG with the pose written as SVG transforms.

#include "forge/pixel/bake.h"
#include "forge/pixel/doc.h"
#include "forge/pixel/rig.h"
#include "forge/pixel/script.h"
#include "forge/pixel/svg.h"
#include "forge/vector/ease.h"
#include "forge/vector/trig.h"

#include <doctest/doctest.h>
#include <lunasvg.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <ostream> // doctest's INFO prints a string_view through it; MSVC needs the whole class
#include <set>
#include <string>
#include <vector>

using namespace forge;
using namespace forge::pixel;

namespace {

// A game without a frame limit of its own (objects::kDefaultFrameLimit).
constexpr u32 kLimit = 64;

// A character: a body with a torso, an arm inside a scaled group and a head with a stroke; a mouth beside it and the
// open mouth hidden.
const char* kHero = R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" width="64" height="64">
  <g id="body" transform="translate(4 2)">
    <rect id="torso" x="20" y="20" width="16" height="20" fill="#3060c0"/>
    <g id="arm" transform="scale(1.5)">
      <rect id="hand" x="16" y="14" width="4" height="10" fill="#c03030"/>
    </g>
    <circle id="head" cx="28" cy="14" r="6" fill="#e0b080" stroke="#402000" stroke-width="1.5"/>
  </g>
  <path id="mouth" d="M24 50 L32 50 L32 52 L24 52 Z" fill="#000"/>
  <path id="mouth_open" d="M24 48 L32 48 L32 56 L24 56 Z" fill="#a00000" style="display:none"/>
</svg>)svg";

// kHero with the parts' transforms and the mouth replaced (the pose written as SVG).
std::string hero(const std::string& body, const std::string& arm, const std::string& head, const std::string& mouth) {
    return R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" width="64" height="64">
  <g transform=")svg" + body + R"svg(">
    <rect x="20" y="20" width="16" height="20" fill="#3060c0"/>
    <g transform=")svg" + arm + R"svg(">
      <rect x="16" y="14" width="4" height="10" fill="#c03030"/>
    </g>
    <circle cx="28" cy="14" r="6" fill="#e0b080" stroke="#402000" stroke-width="1.5" transform=")svg" + head + R"svg("/>
  </g>
  )svg" + mouth + "\n</svg>";
}
const char* kMouth = R"svg(<path d="M24 50 L32 50 L32 52 L24 52 Z" fill="#000"/>)svg";

Rig loaded(std::string_view svg) {
    Rig rig;
    std::string why;
    const bool ok = rig.load(svg, why);
    INFO(why);
    REQUIRE(ok);
    return rig;
}

// lunasvg's picture of an SVG at w × h, straight RGBA rows.
assets::CookedTexture luna(const std::string& svg, u32 w, u32 h) {
    auto doc = lunasvg::Document::loadFromData(svg);
    REQUIRE(doc);
    lunasvg::Bitmap bitmap = doc->renderToBitmap(static_cast<int>(w), static_cast<int>(h), 0x00000000);
    REQUIRE(!bitmap.isNull());
    bitmap.convertToRGBA();
    assets::CookedTexture out{w, h, {}};
    for (u32 y = 0; y < h; ++y) {
        const u8* row = bitmap.data() + static_cast<usize>(y) * static_cast<usize>(bitmap.stride());
        out.rgba8.insert(out.rgba8.end(), row, row + w * 4);
    }
    return out;
}

// Pixels the two pictures disagree on: one covers the pixel by much more than the other, or both cover it in clearly
// different colours. Edges drawn by two rasterizers differ a little; a part out of place differs by many pixels.
usize disagree(const assets::CookedTexture& a, const assets::CookedTexture& b) {
    REQUIRE(a.rgba8.size() == b.rgba8.size());
    usize n = 0;
    for (usize i = 0; i < a.rgba8.size(); i += 4) {
        const int da = std::abs(a.rgba8[i + 3] - b.rgba8[i + 3]);
        int dc = 0;
        if (a.rgba8[i + 3] >= 128 && b.rgba8[i + 3] >= 128)
            for (int c = 0; c < 3; ++c) dc = std::max(dc, std::abs(a.rgba8[i + c] - b.rgba8[i + c]));
        n += da > 96 || dc > 48;
    }
    return n;
}

f64 coverage(const assets::CookedTexture& t) {
    f64 n = 0;
    for (usize i = 3; i < t.rgba8.size(); i += 4) n += t.rgba8[i] / 255.0;
    return n;
}

std::set<u32> colours(const assets::CookedTexture& t) {
    std::set<u32> out;
    for (usize i = 0; i < t.rgba8.size(); i += 4)
        if (t.rgba8[i + 3]) out.insert(u32(t.rgba8[i]) << 16 | u32(t.rgba8[i + 1]) << 8 | t.rgba8[i + 2]);
    return out;
}

bool binary_alpha(const assets::CookedTexture& t) {
    for (usize i = 3; i < t.rgba8.size(); i += 4)
        if (t.rgba8[i] != 0 && t.rgba8[i] != 255) return false;
    return true;
}

PartPose moved(f64 x, f64 y) {
    PartPose p;
    p.x = x, p.y = y;
    return p;
}

} // namespace

TEST_CASE("Pixel svg: the outline as written") {
    const SvgOutline svg = scan_svg("\xEF\xBB\xBF<?xml version=\"1.0\"?>\n<!DOCTYPE svg [ <!ENTITY e \"x\"> ]>\n"
                                    "<svg:svg xmlns:svg='http://www.w3.org/2000/svg'>\n<!-- a <g> in a comment -->\n"
                                    "<svg:g id=\"a&amp;b\" style=\"fill: red; stroke:blue\" fill=\"green\">\n"
                                    "  <path id='p' d=\"M0 0 L1 1\"/><![CDATA[ <rect/> ]]>\n</svg:g>\n<rect id=\"&#1074;\"/>\n</svg:svg>");
    INFO(svg.error);
    REQUIRE(svg.error.empty());
    REQUIRE(svg.elements.size() == 4);
    CHECK(svg.elements[0].tag == "svg");
    CHECK(svg.elements[0].prefix == "svg");
    CHECK(svg.elements[1].tag == "g");
    CHECK(svg.elements[2].prefix.empty());
    CHECK(svg.elements[1].id == "a&b");
    CHECK(svg.elements[1].line == 5);
    CHECK(svg.elements[1].property("fill") == "red"); // style wins over the attribute
    CHECK(svg.elements[1].property("stroke") == "blue");
    CHECK(svg.elements[2].parent == 1);
    CHECK(svg.elements[2].line == 6);
    CHECK(svg.elements[3].parent == 0);
    CHECK(svg.elements[3].id == "\xD0\xB2"); // в
    CHECK(svg.find("p") == 2);
    CHECK(svg.find("nope") == -1);
    CHECK(svg.name_of(2) == "«p»");

    // Errors name their line.
    const std::pair<const char*, const char*> bad[] = {
        {"<svg>\n<g>\n</svg>", "строка 3: </svg> закрывает не тот элемент (открыт <g>)"},
        {"<svg>\n<path d=\"M0 0/>\n", "строка 2: значение «d» не закрыто"},
        {"<svg>\n<path d=M0/>", "строка 2: значение «d» не в кавычках"},
        {"<svg><g>", "строка 1: элемент <g> не закрыт"},
        {"<html/>", "строка 1: это не SVG: корень — <html>"},
        {"просто текст", "строка 1: в файле нет ни одного элемента"},
        {"<svg/><svg/>", "строка 1: второй корневой элемент <svg>"},
        {"<svg>\n<!-- comment", "строка 2: комментарий не закрыт"},
    };
    for (const auto& [text, why] : bad) {
        const SvgOutline s = scan_svg(text);
        INFO(text);
        CHECK(s.error == why);
        CHECK(s.elements.empty());
    }
}

TEST_CASE("Pixel svg: what ThorVG draws otherwise is named") {
    const SvgOutline svg = scan_svg(R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <marker id="m"/><path id="line" d="M0 0 L9 9" marker-end="url(#m)"/>
  <svg id="inner"/><text>hi</text><image href="a.png"/>
  <clipPath><path clip-rule="evenodd" d="M0 0 L1 1"/></clipPath>
  <filter><feGaussianBlur/><feOffset/></filter>
  <rect id="a" fill="#f008"/><rect style="stroke: #ff000080"/><rect fill="hsla(0,50%,50%,1)"/>
  <animate/><rect id="fine" fill="#ff0000"/>
</svg>)svg");
    REQUIRE(svg.error.empty());
    const std::vector<std::string> u = unsupported(svg);
    const std::vector<std::string> expected = {
        "«m»: маркеры <marker> ThorVG не рисует",
        "«line»: marker-end — маркеры ThorVG не рисует",
        "«inner»: вложенный <svg> ThorVG рисует не так, как браузер",
        "<text> в строке 3: текст <text> в этой версии не рисуется — переведите его в кривые",
        "<image> в строке 3: вставленные картинки <image> в этой версии не рисуются",
        "<path> в строке 4: clip-rule=\"evenodd\" в <clipPath> — ThorVG обрежет по правилу nonzero",
        "<feOffset> в строке 5: из фильтров ThorVG рисует только размытие, <feOffset> не нарисуется",
        "«a»: цвет fill=\"#f008\" — такую запись ThorVG не читает",
        "<rect> в строке 6: цвет stroke=\"#ff000080\" — такую запись ThorVG не читает",
        "<rect> в строке 6: цвет fill=\"hsla(0,50%,50%,1)\" — такую запись ThorVG не читает",
        "<animate> в строке 7: анимация <animate> не играет — движение задаёт скрипт позы",
    };
    CHECK(u == expected);
}

TEST_CASE("Pixel svg: shapes as paths, transforms as written") {
    const SvgOutline svg = scan_svg(R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <g transform="translate(1 2)"><rect id="r" x="1" y="2px" width="10" height="6" rx="2"/>
  <circle id="c" cx="5" cy="5" r="3"/></g><ellipse id="e" cx="5" cy="5" rx="4" ry="2" transform="scale(2)"/>
  <line id="l" x1="0" y1="0" x2="4" y2="3"/><polygon id="pg" points="0,0 4,0 4,4"/><polyline id="pl" points="0 0 4 0 4 4"/>
  <path id="p" d="M0 0 H4 V4 Z"/><rect id="em" width="1em" height="2"/><path id="bad" d="M0 0 L"/><g id="g"/>
</svg>)svg");
    REQUIRE(svg.error.empty());
    vector::Path p;
    std::string why;
    REQUIRE(shape_path(svg, svg.find("r"), p, why));
    CHECK(p == vector::rect(1, 2, 10, 6, 2, 2)); // ry takes rx
    REQUIRE(shape_path(svg, svg.find("c"), p, why));
    CHECK(p == vector::circle(5, 5, 3));
    REQUIRE(shape_path(svg, svg.find("e"), p, why));
    CHECK(p == vector::ellipse(5, 5, 4, 2));
    REQUIRE(shape_path(svg, svg.find("l"), p, why));
    const vector::Point line[] = {{0, 0}, {4, 3}};
    CHECK(p == vector::polyline(line, false));
    REQUIRE(shape_path(svg, svg.find("pg"), p, why));
    const vector::Point tri[] = {{0, 0}, {4, 0}, {4, 4}};
    CHECK(p == vector::polyline(tri, true));
    REQUIRE(shape_path(svg, svg.find("pl"), p, why));
    CHECK(p == vector::polyline(tri, false));
    REQUIRE(shape_path(svg, svg.find("p"), p, why));
    CHECK(p.contours.size() == 1);
    CHECK_FALSE(shape_path(svg, svg.find("em"), p, why));
    CHECK(why == "width=\"1em\": нужно число (единицы, кроме px, не поддерживаются)");
    CHECK_FALSE(shape_path(svg, svg.find("bad"), p, why));
    CHECK(why.rfind("d: ", 0) == 0);
    CHECK_FALSE(shape_path(svg, svg.find("g"), p, why));
    CHECK(why == "<g> — не фигура (нужны path, rect, circle, ellipse, line, polyline или polygon)");

    CHECK(transform_chain(svg, svg.find("r")) == "translate(1 2)");
    CHECK(transform_chain(svg, svg.find("c")) == "translate(1 2)");
    CHECK(transform_chain(svg, svg.find("e")) == "scale(2)");
    CHECK(transform_chain(svg, svg.find("l")) == "");
}

TEST_CASE("Pixel rig: parts, their nesting, rest bounds and pivots") {
    Rig rig = loaded(kHero);
    CHECK(rig.width() == 64);
    CHECK(rig.height() == 64);
    std::vector<std::string> ids;
    for (const Part& p : rig.parts()) ids.push_back(p.id);
    CHECK(ids == std::vector<std::string>{"body", "torso", "arm", "hand", "head", "mouth", "mouth_open"});
    auto part = [&](const char* id) -> const Part& { return rig.parts()[static_cast<usize>(rig.part(id))]; };
    CHECK(rig.part("nope") == -1);
    CHECK(part("body").parent == -1);
    CHECK_FALSE(part("body").shape);
    CHECK(part("torso").parent == rig.part("body"));
    CHECK(part("torso").shape);
    CHECK(part("arm").parent == rig.part("body"));
    CHECK(part("hand").parent == rig.part("arm"));
    CHECK(part("head").parent == rig.part("body"));
    CHECK(part("mouth").parent == -1);
    CHECK(rig.outline().elements[static_cast<usize>(part("hand").element)].id == "hand");

    auto box = [&](const char* id, f64 x0, f64 y0, f64 x1, f64 y1) {
        const Part& p = part(id);
        INFO(id, ": ", p.x0, " ", p.y0, " ", p.x1, " ", p.y1);
        CHECK(std::abs(p.x0 - x0) < 1e-4);
        CHECK(std::abs(p.y0 - y0) < 1e-4);
        CHECK(std::abs(p.x1 - x1) < 1e-4);
        CHECK(std::abs(p.y1 - y1) < 1e-4);
    };
    box("torso", 24, 22, 40, 42);
    box("hand", 28, 23, 34, 38); // translate(4 2) scale(1.5)
    box("head", 26, 10, 38, 22);
    box("body", 24, 10, 40, 42);
    box("mouth_open", 24, 48, 32, 56); // hidden: its outline all the same
    CHECK(rig.pivot(rig.part("head")) == vector::Point{32, 16});
    CHECK(rig.pivot(rig.part("arm")) == vector::Point{31, 30.5});
    rig.set_pivot("arm", {31, 24});
    CHECK(rig.pivot(rig.part("arm")) == vector::Point{31, 24});
    CHECK(rig.pivot(-1) == vector::Point{});

    // Where a pivot is carried in a pose: by its own move, not by its turn (it turns round it); by the parent's move
    // and turn.
    auto near = [](vector::Point a, vector::Point b) { return std::abs(a.x - b.x) < 1e-4 && std::abs(a.y - b.y) < 1e-4; }; // ThorVG keeps floats
    auto turn = [](f64 deg, vector::Point c) {
        const f64 a = deg * vector::trig::kPi / 180;
        return translation(c.x, c.y) * Affine{std::cos(a), std::sin(a), -std::sin(a), std::cos(a), 0, 0} * translation(-c.x, -c.y);
    };
    const i32 arm = rig.part("arm"), hand = rig.part("hand");
    CHECK(rig.carried(arm) == Affine{});
    std::string why;
    Pose pose;
    pose.parts["arm"].x = 3;
    pose.parts["arm"].rot = 40;
    REQUIRE(rig.pose(pose, why));
    CHECK(near(rig.carried(arm).apply(rig.pivot(arm)), {34, 24}));
    CHECK(near(rig.carried(arm).apply({0, 0}), {3, 0})); // a move only: the turn is round the pivot
    pose.parts["body"].y = -2;
    pose.parts["body"].rot = 90; // round the centre of its bounds, (32, 26): (34, 24) to (34, 28), then up 2
    REQUIRE(rig.pose(pose, why));
    CHECK(near(rig.carried(arm).apply(rig.pivot(arm)), {34, 26}));
    const vector::Point h = rig.pivot(hand); // the centre of its rest bounds, (31, 30.5)
    const Affine body_m = translation(0, -2) * turn(90, {32, 26}), arm_m = translation(3, 0) * turn(40, {31, 24});
    CHECK(near(rig.carried(hand).apply(h), (body_m * arm_m).apply(h)));
    // Dragged back: the point of the pose is the pivot it came from.
    CHECK(near(inverse(rig.carried(hand)).apply(rig.carried(hand).apply({20, 5})), {20, 5}));
    REQUIRE(rig.pose({}, why));
    CHECK(near(rig.carried(hand).apply(h), h));
    CHECK(rig.carried(-1) == Affine{});
}

TEST_CASE("Pixel rig: drawn as lunasvg draws the same SVG") {
    Rig rig = loaded(kHero);
    const assets::CookedTexture rest = rig.draw(128, 128);
    REQUIRE(rest.width == 128);
    const usize base = disagree(rest, luna(hero("translate(4 2)", "scale(1.5)", "", kMouth), 128, 128));
    INFO(base);
    CHECK(base <= 3);

    // A picture whose viewBox starts elsewhere and is wider than tall: fitted in, centred.
    const std::string wide = R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="10 20 64 32" width="128" height="64">
  <rect id="a" x="12" y="22" width="20" height="10" fill="#208020"/><circle id="b" cx="60" cy="40" r="8" fill="#802080"/></svg>)svg";
    Rig w = loaded(wide);
    CHECK(w.width() == 64);
    CHECK(w.height() == 32);
    CHECK(w.to_pixels(64, 64) == Affine{1, 0, 0, 1, -10, -4});
    const assets::CookedTexture flat = w.draw(64, 32);
    CHECK(disagree(flat, luna(wide, 64, 32)) <= 3);
    const assets::CookedTexture square = w.draw(64, 64);
    // The same picture 16 rows lower, nothing above or below it.
    for (u32 y = 0; y < 64; ++y)
        for (u32 x = 0; x < 64; ++x)
            for (u32 c = 0; c < 4; ++c) {
                const u8 got = square.rgba8[(y * 64 + x) * 4 + c];
                const u8 want = y >= 16 && y < 48 ? flat.rgba8[((y - 16) * 64 + x) * 4 + c] : 0;
                if (got != want) {
                    INFO(x, " ", y, " ", c);
                    REQUIRE(got == want);
                }
            }

    // No viewBox: width and height are the picture's units.
    Rig plain = loaded(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="40" height="30"><rect id="r" width="5" height="5"/></svg>)svg");
    CHECK(plain.width() == 40);
    CHECK(plain.height() == 30);
}

TEST_CASE("Pixel rig: a pose draws as the SVG with the pose written as transforms") {
    Rig rig = loaded(kHero);
    rig.set_pivot("arm", {31, 24});
    const assets::CookedTexture rest = rig.draw(128, 128);

    Pose pose;
    pose.parts["body"] = moved(3, -2);
    PartPose arm;
    arm.rot = 30;
    pose.parts["arm"] = arm;
    PartPose head;
    head.scale = 1.2;
    head.sx = 1.25;
    pose.parts["head"] = head;
    std::string why;
    REQUIRE(rig.pose(pose, why));
    const assets::CookedTexture posed = rig.draw(128, 128);
    // The moves are in rest picture units; a part's own matrix is the parent's rest matrix undone, the move, the parent's
    // rest matrix again and the part's own: body translate(4 2), so the arm and the head get translate(-4 -2) … translate(4 2).
    const std::string svg = hero("translate(3 -2) translate(4 2)", "translate(-4 -2) rotate(30 31 24) translate(4 2) scale(1.5)",
                                 "translate(-4 -2) translate(32 16) scale(1.5 1.2) translate(-32 -16) translate(4 2)", kMouth);
    const usize off = disagree(posed, luna(svg, 128, 128));
    INFO(off);
    CHECK(off <= 3);
    // The other way round is far off: the check sees a turn in the wrong direction.
    const std::string wrong = hero("translate(3 -2) translate(4 2)", "translate(-4 -2) rotate(-30 31 24) translate(4 2) scale(1.5)",
                                   "translate(-4 -2) translate(32 16) scale(1.5 1.2) translate(-32 -16) translate(4 2)", kMouth);
    CHECK(disagree(posed, luna(wrong, 128, 128)) > 100);

    // Bounds in pixels follow the pose: the body 3 units right and 2 up, at 2 pixels a unit.
    f64 b[4];
    rig.bounds(rig.part("torso"), 128, 128, b);
    CHECK(std::abs(b[0] - 54) < 1e-3);
    CHECK(std::abs(b[1] - 40) < 1e-3);
    CHECK(std::abs(b[2] - 86) < 1e-3);
    CHECK(std::abs(b[3] - 80) < 1e-3);

    // Back to rest: exactly the first picture.
    REQUIRE(rig.pose({}, why));
    CHECK(rig.draw(128, 128).rgba8 == rest.rgba8);
}

TEST_CASE("Pixel rig: a shape morphs into a hidden one") {
    Rig rig = loaded(kHero);
    Pose pose;
    pose.morphs["mouth"] = {"mouth_open", 1};
    std::string why;
    REQUIRE(rig.pose(pose, why));
    const std::string open = hero("translate(4 2)", "scale(1.5)", "", R"svg(<path d="M24 48 L32 48 L32 56 L24 56 Z" fill="#a00000"/>)svg");
    CHECK(disagree(rig.draw(128, 128), luna(open, 128, 128)) <= 3);

    pose.morphs["mouth"].k = 0.5; // half way: the shape between, the colour between (0 and 160 → 80)
    REQUIRE(rig.pose(pose, why));
    const std::string half = hero("translate(4 2)", "scale(1.5)", "", R"svg(<path d="M24 49 L32 49 L32 54 L24 54 Z" fill="rgb(80,0,0)"/>)svg");
    CHECK(disagree(rig.draw(128, 128), luna(half, 128, 128)) <= 3);

    // A part moved with its morph: both at once.
    pose.parts["mouth"] = moved(2, 0);
    REQUIRE(rig.pose(pose, why));
    const std::string both = hero("translate(4 2)", "scale(1.5)", "", R"svg(<path d="M26 49 L34 49 L34 54 L26 54 Z" fill="rgb(80,0,0)"/>)svg");
    CHECK(disagree(rig.draw(128, 128), luna(both, 128, 128)) <= 3);
}

TEST_CASE("Pixel rig: what a pose cannot do is named, the rest of it is done") {
    Rig rig = loaded(kHero);
    std::string why;
    Pose pose;
    pose.parts["nope"] = moved(5, 0);
    pose.parts["body"] = moved(3, 0);
    CHECK_FALSE(rig.pose(pose, why));
    CHECK(why == "в SVG нет части «nope»");
    f64 b[4];
    rig.bounds(rig.part("torso"), 64, 64, b);
    CHECK(std::abs(b[0] - 27) < 1e-3); // the body moved all the same

    auto morph_error = [&](const char* id, const char* target, const char* expected) {
        Pose p;
        p.morphs[id] = {target, 1};
        std::string w;
        CHECK_FALSE(rig.pose(p, w));
        CHECK(w == expected);
    };
    morph_error("body", "head", "морфинг: «body» — группа, морфинг бывает только у фигуры");
    morph_error("nope", "head", "морфинг: в SVG нет фигуры «nope»");
    morph_error("mouth", "nope", "морфинг «mouth»: в SVG нет фигуры «nope»");
    morph_error("mouth", "head", "морфинг «mouth» в «head»: у фигур разные transform, их точки не в одних координатах");

    Pose nan;
    nan.parts["head"].rot = std::nan("");
    CHECK_FALSE(rig.pose(nan, why));
    CHECK(why == "часть «head»: не число в позе");

    Rig empty;
    CHECK_FALSE(empty.loaded());
    CHECK_FALSE(empty.pose({}, why));
    CHECK(empty.draw(8, 8).rgba8.empty());

    // Files that do not load say why.
    CHECK_FALSE(empty.load("<svg>\n<g>\n</svg>", why));
    CHECK(why == "строка 3: </svg> закрывает не тот элемент (открыт <g>)");
    CHECK_FALSE(empty.load("<html/>", why));
    CHECK(why == "строка 1: это не SVG: корень — <html>");
    CHECK_FALSE(empty.loaded());
}

TEST_CASE("Pixel rig: baked frames have the SVG's colours only, whole pixels, the figure's area") {
    Rig rig = loaded(kHero);
    const assets::CookedTexture smooth = rig.draw(64, 64);
    const assets::CookedTexture baked = rig.bake(64, 64, {});
    REQUIRE(baked.width == 64);
    CHECK(binary_alpha(baked));
    const std::set<u32> svg_colours = {0x3060c0, 0xc03030, 0xe0b080, 0x402000, 0x000000};
    const std::set<u32> got = colours(baked);
    for (u32 c : got) {
        INFO(std::hex, c);
        CHECK(svg_colours.count(c) == 1);
    }
    CHECK(got == svg_colours);
    // The area: each pixel of an edge is in or out by its cover, so the baked area is the smooth one give or take a
    // part of the edge (a pixel filled wherever an edge touches it, as ThorVG's own aliased drawing does, would be
    // half the edge more). At 64 pixels the hero's area is within 1.3% of the smooth one; at 16, where its parts are a
    // few pixels, 17% less.
    const f64 area = coverage(baked), exact = coverage(smooth);
    usize edge = 0;
    for (usize i = 3; i < smooth.rgba8.size(); i += 4) edge += smooth.rgba8[i] > 0 && smooth.rgba8[i] < 255;
    INFO(area, " ", exact, " ", edge);
    CHECK(std::abs(area - exact) <= 0.25 * static_cast<f64>(edge));
    CHECK(std::abs(area - exact) / exact < 0.015);

    // Small frames too: whole pixels and the SVG's colours.
    const assets::CookedTexture small = rig.bake(32, 32, {});
    CHECK(binary_alpha(small));
    for (u32 c : colours(small)) CHECK(svg_colours.count(c) == 1);

    // A palette: every pixel the nearest of its colours.
    BakeOptions dark;
    dark.palette = {{0, 0, 0, 255}, {255, 255, 255, 255}, {200, 40, 40, 255}};
    const assets::CookedTexture snapped = rig.bake(64, 64, dark);
    CHECK(binary_alpha(snapped));
    CHECK(colours(snapped) == std::set<u32>{0x000000, 0xffffff, 0xc82828});

    // The outline: the transparent pixels beside the figure, nothing else changed.
    BakeOptions outlined;
    outlined.outline = true;
    outlined.outline_color = {10, 20, 30, 255};
    const assets::CookedTexture ring = rig.bake(64, 64, outlined);
    for (u32 y = 0; y < 64; ++y)
        for (u32 x = 0; x < 64; ++x) {
            const usize i = (y * 64 + x) * 4;
            auto opaque = [&](i32 px, i32 py) {
                return px >= 0 && py >= 0 && px < 64 && py < 64 && baked.rgba8[(static_cast<usize>(py) * 64 + static_cast<usize>(px)) * 4 + 3] != 0;
            };
            const bool beside = baked.rgba8[i + 3] == 0 && (opaque(i32(x) - 1, i32(y)) || opaque(i32(x) + 1, i32(y)) ||
                                                             opaque(i32(x), i32(y) - 1) || opaque(i32(x), i32(y) + 1));
            if (beside) {
                CHECK(ring.rgba8[i] == 10);
                CHECK(ring.rgba8[i + 3] == 255);
            } else {
                CHECK(std::equal(ring.rgba8.begin() + i, ring.rgba8.begin() + i + 4, baked.rgba8.begin() + i));
            }
        }
}

TEST_CASE("Pixel rig: baking moves whole pixels, recolours and hides parts") {
    Rig rig = loaded(kHero);
    std::string why;
    const assets::CookedTexture rest = rig.bake(32, 32, {}); // two picture units a pixel

    // Moves snapped to whole pixels: 0.6 units is no move, 1.4 is one pixel.
    Pose pose;
    pose.parts["body"] = moved(0.6, 0);
    pose.parts["mouth"] = moved(0.6, 0);
    REQUIRE(rig.pose(pose, why, 2));
    CHECK(rig.bake(32, 32, {}).rgba8 == rest.rgba8);
    pose.parts["body"] = moved(1.4, 0);
    pose.parts["mouth"] = moved(1.4, 0);
    REQUIRE(rig.pose(pose, why, 2));
    const assets::CookedTexture shifted = rig.bake(32, 32, {});
    for (u32 y = 0; y < 32; ++y)
        for (u32 x = 0; x < 32; ++x)
            for (u32 c = 0; c < 4; ++c) {
                const u8 want = x == 0 ? 0 : rest.rgba8[(y * 32 + x - 1) * 4 + c];
                if (shifted.rgba8[(y * 32 + x) * 4 + c] != want) {
                    INFO(x, " ", y, " ", c);
                    REQUIRE(shifted.rgba8[(y * 32 + x) * 4 + c] == want);
                }
            }

    // A new fill for a group: every filled shape in it, strokes as they were.
    Pose green;
    green.parts["body"].has_fill = true;
    green.parts["body"].fill = {0, 255, 0, 255};
    REQUIRE(rig.pose(green, why));
    CHECK(colours(rig.bake(64, 64, {})) == std::set<u32>{0x00ff00, 0x402000, 0x000000});

    // A part fainter than half is not baked: no half-transparent pixels.
    Pose faint;
    faint.parts["body"].opacity = 0.4;
    REQUIRE(rig.pose(faint, why));
    CHECK(colours(rig.bake(64, 64, {})) == std::set<u32>{0x000000});
    faint.parts["body"].opacity = 0.6;
    REQUIRE(rig.pose(faint, why));
    CHECK(colours(rig.bake(64, 64, {})).size() == 5);

    // Rest again after all that: the same frame.
    REQUIRE(rig.pose({}, why));
    CHECK(rig.bake(32, 32, {}).rgba8 == rest.rgba8);
}

TEST_CASE("Pixel rig: a gradient bakes as the colours it draws, whole pixels") {
    Rig rig = loaded(R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32">
  <defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#ff0000"/><stop offset="1" stop-color="#0000ff"/></linearGradient></defs>
  <rect id="r" x="4" y="4" width="24" height="24" fill="url(#g)"/></svg>)svg");
    const assets::CookedTexture baked = rig.bake(32, 32, {});
    CHECK(binary_alpha(baked));
    CHECK(colours(baked).size() > 8);
    CHECK(std::abs(coverage(baked) - 24 * 24) < 1);
    std::string why;
    Pose pose;
    pose.parts["r"].has_fill = true;
    pose.parts["r"].fill = {0, 255, 0, 255};
    REQUIRE(rig.pose(pose, why));
    CHECK(colours(rig.bake(32, 32, {})) == std::set<u32>{0x00ff00});
    REQUIRE(rig.pose({}, why)); // the gradient back
    CHECK(rig.bake(32, 32, {}).rgba8 == baked.rgba8);
}

TEST_CASE("Pixel: colours as SVG writes them") {
    Rgba8 c;
    CHECK(parse_color("#f80", c));
    CHECK(c == Rgba8{255, 136, 0, 255});
    CHECK(parse_color(" #3060C0 ", c));
    CHECK(c == Rgba8{0x30, 0x60, 0xc0, 255});
    CHECK(parse_color("rgb(10, 20,30)", c));
    CHECK(c == Rgba8{10, 20, 30, 255});
    CHECK(parse_color("RGB(100%,0%,50%)", c));
    CHECK(c == Rgba8{255, 0, 128, 255});
    CHECK(parse_color("Green", c));
    CHECK(c == Rgba8{0, 128, 0, 255});
    for (const char* bad : {"", "#ff", "#ff00zz", "rgb(1,2)", "rgb(1,2,3,4)", "url(#g)", "currentColor", "none", "#ff000080"}) {
        INFO(bad);
        CHECK_FALSE(parse_color(bad, c));
    }
}

namespace {

PoseScript script_of(std::string_view source) {
    PoseScript s;
    const bool ok = s.load(source);
    INFO(s.error());
    REQUIRE(ok);
    return s;
}

const char* kWalk = R"lua(
return {
  params = { speed = 1.0, step = 6 },
  pose = function(t, p, rng, clip)
    local s = math.sin(t * p.speed * 2 * math.pi)
    part("arm"):set{ rot = 30 * s }
    part("body"):set{ y = -p.step / 6 * math.abs(s) }
    if clip == "walk" then part("head"):set{ x = rng(1, 3), opacity = 0.5 + rng() / 4, fill = "#102030" } end
    shape("mouth"):morph("mouth_open", ease("smooth", 0.5 + 0.5 * s))
  end,
}
)lua";

} // namespace

TEST_CASE("Pixel script: a pose from the script, its parameters, ease and rng") {
    Rig rig = loaded(kHero);
    PoseScript s = script_of(kWalk);
    REQUIRE(s.params().size() == 2);
    CHECK(s.params()[0].name == "speed"); // by name
    CHECK(s.params()[0].value == 1.0);
    CHECK(s.params()[1].name == "step");
    CHECK(s.params()[1].value == 6.0);

    Pose p;
    REQUIRE(s.pose(rig, 0.25, "stand", {}, 7, p));
    CHECK(p.parts.size() == 2);
    CHECK(std::abs(p.parts["arm"].rot - 30) < 1e-12);
    CHECK(std::abs(p.parts["body"].y + 1) < 1e-12);
    CHECK(p.morphs["mouth"].target == "mouth_open");
    CHECK(p.morphs["mouth"].k == 1.0); // smooth(1)

    // Parameters from the file over the script's; names it does not declare are left out.
    REQUIRE(s.pose(rig, 0.25, "stand", {{"step", 12}, {"nope", 5}}, 7, p));
    CHECK(std::abs(p.parts["body"].y + 2) < 1e-12);

    // rng: the same frame gives the same numbers, another frame or seed others; integers within the bounds.
    Pose a, b, c, d;
    REQUIRE(s.pose(rig, 0.5, "walk", {}, 7, a));
    REQUIRE(s.pose(rig, 0.5, "walk", {}, 7, b));
    CHECK(a.parts["head"].x == b.parts["head"].x);
    CHECK(a.parts["head"].opacity == b.parts["head"].opacity);
    CHECK(a.parts["head"].has_fill);
    CHECK(a.parts["head"].fill == Rgba8{0x10, 0x20, 0x30, 255});
    std::set<f64> xs, ops;
    for (int k = 0; k < 40; ++k) {
        REQUIRE(s.pose(rig, k / 12.0, "walk", {}, 7, c));
        xs.insert(c.parts["head"].x);
        ops.insert(c.parts["head"].opacity);
        REQUIRE(s.pose(rig, k / 12.0, "walk", {}, 8, d));
        ops.insert(d.parts["head"].opacity);
    }
    CHECK(xs == std::set<f64>{1, 2, 3});
    CHECK(ops.size() == 80);
    for (f64 o : ops) CHECK((o >= 0.5 && o < 0.75));

    // Ease: the words of the «Интерфейс» tab, "in" too.
    PoseScript e = script_of(R"lua(return { pose = function(t)
      part("head"):set{ x = ease["in"](0.5), y = ease.out(0.5), rot = ease("back", 0.5), scale = ease.linear(0.25) } end })lua");
    REQUIRE(e.pose(rig, 0, "", {}, 1, p));
    CHECK(p.parts["head"].x == vector::ease(vector::Ease::In, 0.5));
    CHECK(p.parts["head"].y == vector::ease(vector::Ease::Out, 0.5));
    CHECK(p.parts["head"].rot == vector::ease(vector::Ease::Back, 0.5));
    CHECK(p.parts["head"].scale == 0.25);
}

TEST_CASE("Pixel script: the pose is a function of time, nothing carries over") {
    Rig rig = loaded(kHero);
    PoseScript s = script_of(R"lua(
local frames = 0
return { pose = function(t)
  frames = frames + 1
  if seen then part("body"):set{ y = 9 } end
  seen = true
  part("head"):set{ x = frames }
end })lua");
    Pose p;
    for (int i = 0; i < 3; ++i) {
        REQUIRE(s.pose(rig, i, "", {}, 1, p));
        CHECK(p.parts["head"].x == 1);
        CHECK(p.parts.count("body") == 0);
    }
    // No clock and no chance but rng.
    PoseScript clock = script_of("return { pose = function(t) part('head'):set{ x = os.clock() } end }");
    CHECK_FALSE(clock.pose(rig, 0, "", {}, 1, p));
    CHECK(clock.error_line() == 1);
    PoseScript chance = script_of("return { pose = function(t)\n part('head'):set{ x = math.random() }\nend }");
    CHECK_FALSE(chance.pose(rig, 0, "", {}, 1, p));
    CHECK(chance.error_line() == 2);
}

TEST_CASE("Pixel script: errors name their line") {
    Rig rig = loaded(kHero);
    auto load_error = [](std::string_view src, const char* expected) {
        PoseScript s;
        CHECK_FALSE(s.load(src));
        CHECK(s.error() == expected);
        CHECK_FALSE(s.loaded());
    };
    PoseScript syntax;
    CHECK_FALSE(syntax.load("return {\n pose = function(t)\n  part('head'):set{ x = }\n end }"));
    CHECK(syntax.error_line() == 3);
    CHECK(syntax.error().rfind("строка 3: ", 0) == 0);
    load_error("return 5", "скрипт должен вернуть таблицу: return { params = {…}, clips = { walk = function(p) … end } }");
    load_error("return { params = {} }",
               "в таблице скрипта нет ни clips = { walk = function(p) … end }, ни pose = function(t, p, rng, clip) … end");
    load_error("return { params = { speed = 'fast' }, pose = function() end }", "params.speed: параметр — число (скорость, длина шага…)");
    load_error("return { params = 5, pose = function() end }", "params должен быть таблицей: params = { speed = 1 }");
    load_error("\n\nerror('сломано')\nreturn {}", "строка 3: сломано");

    auto pose_error = [&](std::string_view body, const char* expected) {
        PoseScript s = script_of(std::string("return { pose = function(t)\n") + std::string(body) + "\nend }");
        Pose p;
        CHECK_FALSE(s.pose(rig, 0, "walk", {}, 1, p));
        CHECK(s.error() == expected);
        CHECK(s.error_line() == 2);
        CHECK(p.parts.empty());
    };
    pose_error("part('nope'):set{ x = 1 }", "строка 2: в SVG нет части «nope»");
    pose_error("part('head'):set{ spin = 1 }", "строка 2: part(\"head\"):set — поля «spin» нет (есть x, y, rot, scale, sx, sy, opacity, fill)");
    pose_error("part('head'):set{ x = 'left' }", "строка 2: part(\"head\"):set — x: нужно число");
    pose_error("part('head'):set{ fill = 'teal-ish' }", "строка 2: part(\"head\"):set — fill: нужен цвет \"#rrggbb\"");
    pose_error("shape('body'):morph('head', 1)", "строка 2: «body» — группа, морфинг бывает только у фигуры");
    pose_error("shape('mouth'):morph('nope', 1)", "строка 2: в SVG нет фигуры «nope»");
    pose_error("local x = ease('fast', 1)", "строка 2: ease: нет слова «fast» (smooth, linear, in, out, back, bounce, elastic, in_sine … in_out_bounce)");
    pose_error("local x = nil + 1", "строка 2: attempt to perform arithmetic (add) on nil and number");

    // An endless loop stops at the budget, at its line.
    PoseScript endless = script_of("return { pose = function(t)\n while true do end\nend }");
    endless.set_budget_ms(20);
    Pose p;
    CHECK_FALSE(endless.pose(rig, 0, "", {}, 1, p));
    CHECK(endless.error() == "строка 2: скрипт позы считал дольше 20 мс (бесконечный цикл?)");
    // And it works again after.
    PoseScript fine = script_of(kWalk);
    CHECK(fine.pose(rig, 0, "", {}, 1, p));
}

TEST_CASE("Pixel doc: .pixel.json read, written and read back") {
    const char* model = R"json({
  "svg": "0b7f6c1e-1a2b-4c3d-8e9f-001122334455",
  "script": "Герой.luau",
  "frame": [32, 32],
  "seed": 1234,
  "params": {"speed": 1.0, "step": 6},
  "pivots": {"leg_l": [29.5, 41], "leg_r": [34.5, 41], "head": [32, 26]},
  "clips": {
    "walk": {"frames": 8, "fps": 12, "loop": true},
    "stand": {"frames": 4, "fps": 6, "loop": false}
  },
  "waits": {"jump": {"отрыв": 0.18, "приземление": 0}},
  "durations": {"spin": {"оборот": 0.6}},
  "bake": {"palette": "svg", "outline": null, "picture": "aa"}
})json";
    PixelDoc d;
    std::string why;
    REQUIRE(read_pixel_doc(model, d, why));
    CHECK(d.svg == "0b7f6c1e-1a2b-4c3d-8e9f-001122334455");
    CHECK(d.script == "Герой.luau");
    CHECK(d.frame_w == 32);
    CHECK(d.seed == 1234);
    CHECK(d.params.at("step") == 6);
    CHECK(d.pivots.at("leg_l") == vector::Point{29.5, 41});
    REQUIRE(d.clips.size() == 2);
    CHECK(d.clips[0] == ClipSpec{"walk", 8, 12, true}); // in the file's order
    CHECK(d.clips[1] == ClipSpec{"stand", 4, 6, false});
    CHECK_FALSE(d.game_palette);
    CHECK_FALSE(d.outline);
    CHECK(d.picture == "aa");
    REQUIRE(d.lengths.size() == 2);
    CHECK(d.lengths.at("jump").waits == std::map<std::string, f64>{{"отрыв", 0.18}, {"приземление", 0}});
    CHECK(d.lengths.at("jump").durations.empty());
    CHECK(d.lengths.at("spin").durations.at("оборот") == 0.6);

    d.game_palette = true;
    d.outline = true;
    d.outline_color = {16, 32, 48, 255};
    d.hash = "ff";
    d.clips[1].fps = 7.5f;
    d.strip = {{"walk", 0, 8, 12, true}, {"stand", 8, 4, 7.5f, false}};
    const std::string text = write_pixel_doc(d);
    PixelDoc back;
    REQUIRE(read_pixel_doc(text, back, why));
    CHECK(back == d);
    CHECK(text.find("\"outline\": \"#102030\"") != std::string::npos);
    // Without time events the file has neither field.
    d.lengths.clear();
    const std::string plain = write_pixel_doc(d);
    CHECK(plain.find("waits") == std::string::npos);
    CHECK(plain.find("durations") == std::string::npos);

    const std::pair<const char*, const char*> bad[] = {
        {"[1]", "в файле не объект JSON"},
        {"{\"frame\": [0, 32]}", "frame: нужны ширина и высота кадра, целые от 1 до 256"},
        {"{\"frame\": [32]}", "frame: нужны ширина и высота кадра, целые от 1 до 256"},
        {"{\"seed\": -1}", "seed: нужно целое число от 0"},
        {"{\"params\": {\"a\": \"b\"}}", "params.a: нужно число"},
        {"{\"pivots\": {\"leg\": [1]}}", "pivots.leg: нужна точка [x, y]"},
        {"{\"clips\": {\"walk\": {\"frames\": 0}}}", "clips.walk.frames: нужно целое от 1 до 16384"},
        {"{\"clips\": {\"walk\": {\"frames\": 16385}}}", "clips.walk.frames: нужно целое от 1 до 16384"},
        {"{\"clips\": {\"walk\": {\"fps\": 0}}}", "clips.walk.fps: нужно число больше 0 и не больше 60"},
        {"{\"clips\": {\"walk\": {\"loop\": 1}}}", "clips.walk.loop: нужно true или false"},
        {"{\"waits\": {\"jump\": {\"отрыв\": -1}}}", "waits.jump.отрыв: нужно число секунд не меньше 0"},
        {"{\"durations\": {\"spin\": 0.6}}", "durations: нужен объект {\"клип\": {\"имя\": секунды}}"},
        {"{\"bake\": {\"palette\": \"gold\"}}", "bake.palette: \"svg\" (цвета рисунка) или \"game\" (цвета игры)"},
        {"{\"bake\": {\"outline\": \"thick\"}}", "bake.outline: null или цвет \"#rrggbb\""},
        {"{\"bake\": {\"strip\": {\"walk\": {\"first\": 16382, \"frames\": 4, \"fps\": 10, \"loop\": true}}}}",
         "bake.strip.walk: нужно {\"first\": 0, \"frames\": 4, \"fps\": 10, \"loop\": true} в пределах 16384 кадров"},
        {"{\"bake\": {\"strip\": {\"walk\": {\"first\": 0, \"frames\": 4, \"fps\": 10}}}}",
         "bake.strip.walk: нужно {\"first\": 0, \"frames\": 4, \"fps\": 10, \"loop\": true} в пределах 16384 кадров"},
    };
    for (const auto& [text_bad, expected] : bad) {
        PixelDoc x;
        x.seed = 99;
        INFO(text_bad);
        CHECK_FALSE(read_pixel_doc(text_bad, x, why));
        CHECK(why == expected);
        CHECK(x.seed == 99); // left as it was
    }
    CHECK_FALSE(read_pixel_doc("{\"svg\": ", d, why));
    CHECK(why.rfind("это не JSON: ", 0) == 0);
}

TEST_CASE("Pixel bake: the clips' frames side by side, frame k at t = k / fps") {
    Rig rig = loaded(kHero);
    PoseScript s = script_of(kWalk);
    PixelDoc doc;
    doc.frame_w = 24, doc.frame_h = 20;
    doc.seed = 5;
    doc.pivots["arm"] = {31, 24};
    doc.pivots["nope"] = {0, 0};
    doc.clips = {{"stand", 2, 6, true}, {"walk", 3, 12, false}};
    rig.set_pivot("head", {1, 2}); // set before (a pivot dragged, then undone): the file has none for it
    CHECK(apply_pivots(rig, doc) == std::vector<std::string>{"nope"});
    CHECK(rig.pivot(rig.part("arm")) == vector::Point{31, 24});
    CHECK(rig.pivot(rig.part("head")) == vector::Point{32, 16});
    const assets::CookedTexture rest = rig.draw(24, 20);

    Strip strip;
    std::string why;
    REQUIRE(bake_strip(rig, s, doc, {}, kLimit, strip, why));
    CHECK(strip.frames == 5);
    CHECK(strip.picture.width == 24 * 5);
    CHECK(strip.picture.height == 20);
    REQUIRE(strip.clips.size() == 2);
    CHECK(strip.clips[0].state == "stand");
    CHECK(strip.clips[0].first == 0);
    CHECK(strip.clips[0].frames == 2);
    CHECK(strip.clips[1].first == 2);
    CHECK(strip.clips[1].fps == 12);
    CHECK_FALSE(strip.clips[1].loop);
    CHECK(binary_alpha(strip.picture));
    CHECK(rig.draw(24, 20).rgba8 == rest.rgba8); // left at rest

    // Each frame is the one baked alone.
    for (u32 f = 0; f < 5; ++f) {
        const ClipSpec& c = f < 2 ? doc.clips[0] : doc.clips[1];
        assets::CookedTexture one;
        REQUIRE(bake_frame(rig, s, doc, c, f < 2 ? f : f - 2, {}, one, why));
        for (u32 y = 0; y < 20; ++y) {
            INFO(f, " ", y);
            CHECK(std::equal(one.rgba8.begin() + y * 24 * 4, one.rgba8.begin() + (y + 1) * 24 * 4,
                             strip.picture.rgba8.begin() + (y * 120 + f * 24) * 4));
        }
    }
    // Frames differ: the arm swings.
    auto frame_of = [&](u32 f) {
        std::vector<u8> out;
        for (u32 y = 0; y < 20; ++y)
            out.insert(out.end(), strip.picture.rgba8.begin() + (y * 120 + f * 24) * 4, strip.picture.rgba8.begin() + (y * 120 + f * 24 + 24) * 4);
        return out;
    };
    CHECK(frame_of(0) != frame_of(1));
    CHECK(frame_of(2) != frame_of(3));

    // The game's colours when the file says so.
    doc.game_palette = true;
    const BakeOptions game = bake_options(doc, {{255, 255, 255, 255}, {0, 0, 0, 255}});
    REQUIRE(bake_strip(rig, s, doc, game, kLimit, strip, why));
    for (u32 c : colours(strip.picture)) CHECK((c == 0xffffff || c == 0));

    // A frame that fails names its clip and frame; nothing is baked.
    PoseScript broken = script_of("return { pose = function(t, p, rng, clip)\n if t > 0.1 then part('nope'):set{} end\nend }");
    CHECK_FALSE(bake_strip(rig, broken, doc, {}, kLimit, strip, why));
    CHECK(why == "клип «stand», кадр 2: строка 2: в SVG нет части «nope»");
    CHECK(strip.picture.rgba8.empty());
    // As many frames as the game's limit, and not one more: 64 without its own, or the game's.
    doc.clips = {{"stand", 40, 10, true}, {"walk", 24, 10, true}};
    CHECK(bake_strip(rig, s, doc, {}, kLimit, strip, why));
    CHECK(strip.frames == 64);
    doc.clips[1].frames = 25;
    CHECK_FALSE(bake_strip(rig, s, doc, {}, kLimit, strip, why));
    CHECK(why == "в клипах 65 кадров, а предел игры 64, лишний 1: уберите кадры у клипов, сузьте диапазоны или поднимите «Предел кадров»");
    CHECK(strip.picture.rgba8.empty());
    CHECK(bake_strip(rig, s, doc, {}, 65, strip, why));
    CHECK(strip.frames == 65);
    doc.clips = {{"stand", 2, 10, true}, {"walk", 1, 10, true}};
    CHECK_FALSE(bake_strip(rig, s, doc, {}, 2, strip, why));
    CHECK(why == "в клипах 3 кадра, а предел игры 2, лишний 1: уберите кадры у клипов, сузьте диапазоны или поднимите «Предел кадров»");
    // As wide as a texture may be on any video card, and not a pixel wider: 64 frames of 256 fill it exactly.
    const u32 fw = doc.frame_w, fh = doc.frame_h;
    doc.frame_w = kMaxFrameSide;
    doc.frame_h = 4;
    doc.clips = {{"stand", 64, 10, true}};
    REQUIRE(bake_strip(rig, s, doc, {}, kLimit, strip, why));
    CHECK(strip.picture.width == kMaxStripWidth);
    CHECK(strip.picture.width == 16384);
    doc.clips[0].frames = 65;
    CHECK_FALSE(bake_strip(rig, s, doc, {}, 65, strip, why));
    CHECK(why == "полоса 16640 точек шире 16384: столько видеокарта не возьмёт, уменьшите кадр или число кадров");
    CHECK(strip.picture.rgba8.empty());
    doc.frame_w = fw;
    doc.frame_h = fh;
    doc.clips.clear();
    CHECK_FALSE(bake_strip(rig, s, doc, {}, kLimit, strip, why));
    CHECK(why == "нет ни одного клипа с кадрами");
}
