// engine/vector (step 1 of docs/models/вектор-модель.md): paths read from SVG and written back, SVG's basic shapes,
// the morph of two shapes, easing and the trig behind them. lunasvg, which draws SVG in the interface, is the second
// opinion on what a path means: a path read here and written back must draw as the original.

#include "forge/vector/ease.h"
#include "forge/vector/morph.h"
#include "forge/vector/path.h"
#include "forge/vector/trig.h"

#include <RmlUi/Core/Tween.h>
#include <doctest/doctest.h>
#include <lunasvg.h>

#include <bit>
#include <cmath>
#include <limits>
#include <ostream> // doctest's INFO prints a string_view through it; MSVC needs the whole class
#include <string>
#include <utility>
#include <vector>

using namespace forge;
using namespace forge::vector;

namespace {

constexpr f64 kPi = trig::kPi;

f64 dist(Point a, Point b) { return std::hypot(a.x - b.x, a.y - b.y); }

bool near(Point a, Point b, f64 eps) { return dist(a, b) <= eps; }

Path parsed(std::string_view d) {
    Path p;
    std::string why;
    const bool ok = parse_path(d, p, &why);
    INFO(d, " ", why);
    CHECK(ok);
    return p;
}

// Every point of a contour, 64 steps a curve.
std::vector<Point> samples(const Contour& c) {
    std::vector<Point> out;
    for (usize i = 0; i < c.curves.size(); ++i)
        for (int s = 0; s < 64; ++s) out.push_back(point_at(c.from(i), c.curves[i], s / 64.0));
    return out;
}

// lunasvg's picture of an SVG body on a 64 × 64 canvas, RGBA rows.
std::vector<u8> draw(const std::string& body) {
    const std::string svg = "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='64' viewBox='0 0 64 64'>" +
                            body + "</svg>";
    auto doc = lunasvg::Document::loadFromData(svg);
    REQUIRE(doc);
    lunasvg::Bitmap bitmap = doc->renderToBitmap(64, 64, 0x00000000);
    REQUIRE(!bitmap.isNull());
    std::vector<u8> out;
    for (int y = 0; y < 64; ++y) {
        const u8* row = bitmap.data() + static_cast<usize>(y) * static_cast<usize>(bitmap.stride());
        out.insert(out.end(), row, row + 64 * 4);
    }
    return out;
}

std::string path_element(const std::string& d) { return "<path fill='black' d='" + d + "'/>"; }

// How two pictures differ: the largest difference of a channel, and how many channels differ at all.
struct Difference {
    int largest = 0;
    usize count = 0;
};

Difference compare(const std::vector<u8>& a, const std::vector<u8>& b) {
    Difference d;
    for (usize i = 0; i < a.size(); ++i) {
        const int diff = std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
        d.largest = std::max(d.largest, diff);
        d.count += diff != 0;
    }
    return d;
}

// How much of the canvas the picture covers, in pixels (its alpha added up).
f64 covered(const std::vector<u8>& picture) {
    f64 n = 0;
    for (usize i = 3; i < picture.size(); i += 4) n += picture[i] / 255.0;
    return n;
}

// A star of n points around (cx, cy), counterclockwise on the screen, its first point straight up.
Path star(int n, f64 cx, f64 cy, f64 outer, f64 inner) {
    std::vector<Point> pts;
    for (int i = 0; i < 2 * n; ++i) {
        f64 sa = 0, ca = 0;
        trig::sin_cos(-kPi / 2 - i * kPi / n, sa, ca); // the same bits everywhere, for the last check
        const f64 r = i % 2 == 0 ? outer : inner;
        pts.push_back({cx + r * ca, cy + r * sa});
    }
    return polyline(pts, true);
}

Shape filled(Path p, Paint fill = rgb8(0, 0, 0)) {
    Shape s;
    s.path = std::move(p);
    s.look.fill = fill;
    return s;
}

} // namespace

TEST_CASE("Vector trig: as the C library, to the last bits") {
    for (f64 a = -20; a <= 20; a += 0.0137) {
        CHECK(std::fabs(trig::sin(a) - std::sin(a)) <= 4e-16);
        CHECK(std::fabs(trig::cos(a) - std::cos(a)) <= 4e-16);
    }
    for (f64 y = -3; y <= 3; y += 0.125)
        for (f64 x = -3; x <= 3; x += 0.125) CHECK(std::fabs(trig::atan2(y, x) - std::atan2(y, x)) <= 1e-15);
    for (f64 x = -30; x <= 30; x += 0.0173) CHECK(std::fabs(trig::exp(x) / std::exp(x) - 1) <= 1e-15);
    CHECK(trig::sin(0) == 0);
    CHECK(trig::cos(0) == 1);
    CHECK(trig::atan2(0, 0) == 0);
    CHECK(trig::atan2(0, -1) == kPi);
    CHECK(trig::atan2(-1, 0) == -kPi / 2);
    CHECK(trig::exp(0) == 1);
    CHECK(trig::exp(-800) == 0);
    CHECK(std::isinf(trig::exp(800)));
    CHECK(std::isnan(trig::sin(1e10)));
}

TEST_CASE("Vector ease: exact ends, the interface's curves, its words") {
    for (Ease e : kEases) {
        INFO(ease_word(e));
        CHECK(ease(e, 0) == 0);
        CHECK(ease(e, 1) == 1);
        CHECK(ease(e, -0.5) == 0);
        CHECK(ease(e, 1.5) == 1);
        CHECK(ease(e, std::numeric_limits<f64>::quiet_NaN()) == 0);
    }
    CHECK(ease(Ease::Linear, 0.3) == 0.3);
    CHECK(ease(Ease::In, 0.5) == 0.125);
    CHECK(ease(Ease::Out, 0.5) == 0.875);
    CHECK(ease(Ease::Smooth, 0.5) == 0.5);
    CHECK(ease(Ease::Smooth, 0.25) == 0.0625);

    // The same curves the interface moves layers with: RmlUi's tweens for the words of ui_design.cpp's easing_css.
    const Rml::Tween rml[] = {
        Rml::Tween(Rml::Tween::Cubic, Rml::Tween::InOut),  Rml::Tween(Rml::Tween::Linear, Rml::Tween::InOut),
        Rml::Tween(Rml::Tween::Cubic, Rml::Tween::In),     Rml::Tween(Rml::Tween::Cubic, Rml::Tween::Out),
        Rml::Tween(Rml::Tween::Back, Rml::Tween::Out),     Rml::Tween(Rml::Tween::Bounce, Rml::Tween::Out),
        Rml::Tween(Rml::Tween::Elastic, Rml::Tween::Out)};
    for (usize i = 0; i < std::size(kEases); ++i)
        for (int s = 0; s <= 100; ++s) {
            const f64 t = s / 100.0;
            INFO(ease_word(kEases[i]), " at ", t);
            CHECK(std::fabs(ease(kEases[i], t) - rml[i](static_cast<float>(t))) <= 1e-5);
        }

    // Back and Elastic go past the end and come back.
    f64 back_peak = 0, elastic_peak = 0;
    for (int s = 0; s <= 1000; ++s) {
        back_peak = std::max(back_peak, ease(Ease::Back, s / 1000.0));
        elastic_peak = std::max(elastic_peak, ease(Ease::Elastic, s / 1000.0));
    }
    CHECK(back_peak == doctest::Approx(1.1).epsilon(0.001));
    CHECK(elastic_peak == doctest::Approx(1.26).epsilon(0.01));

    const char* words[] = {"smooth", "linear", "in", "out", "back", "bounce", "elastic"};
    for (usize i = 0; i < std::size(kEases); ++i) {
        CHECK(std::string(ease_word(kEases[i])) == words[i]);
        Ease e = Ease::Linear;
        CHECK(ease_from_word(words[i], e));
        CHECK(e == kEases[i]);
    }
    Ease e = Ease::Back;
    CHECK_FALSE(ease_from_word("ease-in", e));
    CHECK(e == Ease::Back);
}

TEST_CASE("Vector path: lines, H, V, Z and relative commands") {
    Path p = parsed("M10 20 L40 50");
    REQUIRE(p.contours.size() == 1);
    const Contour& c = p.contours[0];
    CHECK(c.start == Point{10, 20});
    REQUIRE(c.curves.size() == 1);
    CHECK(c.curves[0].to == Point{40, 50});
    CHECK(near(c.curves[0].c1, {20, 30}, 1e-12));
    CHECK(near(c.curves[0].c2, {30, 40}, 1e-12));
    CHECK_FALSE(c.closed);

    // m first is absolute; h, v relative; z draws back to the start as a curve of its own.
    p = parsed("m10 20 l5 5 h10 v-5 z");
    REQUIRE(p.contours.size() == 1);
    const Contour& r = p.contours[0];
    CHECK(r.start == Point{10, 20});
    REQUIRE(r.curves.size() == 4);
    CHECK(r.curves[0].to == Point{15, 25});
    CHECK(r.curves[1].to == Point{25, 25});
    CHECK(r.curves[2].to == Point{25, 20});
    CHECK(r.curves[3].to == Point{10, 20});
    CHECK(r.closed);

    // Already back at the start: Z adds nothing.
    CHECK(parsed("M0 0 L10 0 L0 0 Z").contours[0].curves.size() == 2);
    // Pairs after M are lines; after m relative lines.
    p = parsed("M0 0 10 0 10 10");
    REQUIRE(p.contours[0].curves.size() == 2);
    CHECK(p.contours[0].curves[1].to == Point{10, 10});
    p = parsed("m5 5 10 0 0 10");
    CHECK(p.contours[0].curves[1].to == Point{15, 15});
    // A lone M is no contour; a drawing command after Z starts a new contour at the same start.
    p = parsed("M0 0 M5 5 L6 6 L5 7 Z L0 0 Z");
    REQUIRE(p.contours.size() == 2);
    CHECK(p.contours[0].start == Point{5, 5});
    CHECK(p.contours[1].start == Point{5, 5});
    CHECK(p.contours[1].curves[0].to == Point{0, 0});
    // Numbers as SVG writes them.
    p = parsed("M.5-.5L1e1,2E-1+3-4M1.5.5L2 2");
    REQUIRE(p.contours.size() == 2);
    CHECK(p.contours[0].start == Point{0.5, -0.5});
    CHECK(p.contours[0].curves[0].to == Point{10, 0.2});
    CHECK(p.contours[0].curves[1].to == Point{3, -4});
    CHECK(p.contours[1].start == Point{1.5, 0.5});
    // Nothing is no error.
    CHECK(parsed("").contours.empty());
    CHECK(parsed(" \n\t").contours.empty());
}

TEST_CASE("Vector path: curves, mirrored control points and quadratic curves") {
    Path p = parsed("M0 0 C0 10 10 10 10 0 S20 -10 20 0");
    REQUIRE(p.contours[0].curves.size() == 2);
    CHECK(p.contours[0].curves[1].c1 == Point{10, -10}); // (10, 10) mirrored through (10, 0)
    CHECK(p.contours[0].curves[1].c2 == Point{20, -10});
    // S with no curve before: the first control point is the current point.
    p = parsed("M0 0 L5 0 S10 10 15 0");
    CHECK(p.contours[0].curves[1].c1 == Point{5, 0});
    // s and c relative.
    p = parsed("M10 10 c0 10 10 10 10 0 s10 -10 10 0");
    CHECK(p.contours[0].curves[0].c1 == Point{10, 20});
    CHECK(p.contours[0].curves[1].c1 == Point{20, 0});
    CHECK(p.contours[0].curves[1].to == Point{30, 10});

    // Q (5, 10) from (0, 0) to (10, 0): cubic control points two thirds of the way to it; T mirrors it.
    p = parsed("M0 0 Q5 10 10 0 T20 0");
    REQUIRE(p.contours[0].curves.size() == 2);
    const Contour& q = p.contours[0];
    CHECK(near(q.curves[0].c1, {10.0 / 3, 20.0 / 3}, 1e-12));
    CHECK(near(q.curves[0].c2, {20.0 / 3, 20.0 / 3}, 1e-12));
    CHECK(near(q.curves[1].c1, {10 + 10.0 / 3, -20.0 / 3}, 1e-12));
    CHECK(near(q.curves[1].c2, {20 - 10.0 / 3, -20.0 / 3}, 1e-12));
    // The cubic is the same curve as the quadratic: its middle is the quadratic's, (5, 5).
    CHECK(near(point_at(q.start, q.curves[0], 0.5), {5, 5}, 1e-12));
    // T with no Q right before: a straight line.
    p = parsed("M0 0 T10 0");
    CHECK(near(p.contours[0].curves[0].c1, {0, 0}, 1e-12));
    p = parsed("M0 0 Q5 10 10 0 L20 0 T30 0");
    CHECK(near(p.contours[0].curves[2].c1, {20, 0}, 1e-12));
    CHECK(near(p.contours[0].curves[2].c2, {70.0 / 3, 0}, 1e-12));
}

TEST_CASE("Vector path: arcs") {
    // Half a circle around (10, 0) from (0, 0) to (20, 0), the positive way: over the top on the screen.
    const Path half = parsed("M0 0 A10 10 0 0 1 20 0");
    REQUIRE(half.contours[0].curves.size() == 2);
    const Contour& h = half.contours[0];
    CHECK(near(h.curves[0].to, {10, -10}, 1e-12));
    CHECK(h.curves[1].to == Point{20, 0}); // the end is exact
    for (Point s : samples(h)) CHECK(std::fabs(dist(s, {10, 0}) - 10) <= 3e-3);
    // Radii too small to reach grow until they do; flags may run into the numbers.
    for (const char* d : {"M0 0 A1 1 0 0 1 20 0", "M0 0a10 10 0 0120 0", "M0 0 A10,10,0,0,1,20,0"}) {
        INFO(d);
        const Path p = parsed(d);
        REQUIRE(p.contours[0].curves.size() == 2);
        for (usize i = 0; i < 2; ++i) {
            CHECK(near(p.contours[0].curves[i].c1, h.curves[i].c1, 1e-12));
            CHECK(near(p.contours[0].curves[i].to, h.curves[i].to, 1e-12));
        }
    }
    // The other sweep goes under.
    CHECK(near(parsed("M0 0 A10 10 0 0 0 20 0").contours[0].curves[0].to, {10, 10}, 1e-12));
    // The large arc: three quarters of a circle, in three curves.
    // The large arc the negative way from (0, 0) to (10, 10): three quarters around (0, 10), in three curves.
    const Path large = parsed("M0 0 A10 10 0 1 0 10 10");
    CHECK(large.contours[0].curves.size() == 3);
    CHECK(near(large.contours[0].curves[0].to, {-10, 10}, 1e-12));
    for (Point s : samples(large.contours[0])) CHECK(std::fabs(dist(s, {0, 10}) - 10) <= 3e-3);
    // A turned ellipse ends exactly at its end (lunasvg checks its shape below).
    const Path tilted = parsed("M0 0 A20 10 30 0 1 30 10");
    CHECK(tilted.contours[0].curves.back().to == Point{30, 10});
    // No radius: a line. The same ends: nothing.
    CHECK(parsed("M0 0 A0 10 0 0 1 20 0").contours[0].curves[0] == line({0, 0}, {20, 0}));
    CHECK(parsed("M5 5 A10 10 0 0 1 5 5").contours.empty());
}

TEST_CASE("Vector path: errors stop where they are") {
    Path p;
    std::string why;
    CHECK_FALSE(parse_path("L10 10", p, &why));
    CHECK(p.contours.empty());
    CHECK(why.find("M") != std::string::npos);
    CHECK_FALSE(parse_path("M0 0 L10", p, &why));
    CHECK(p.contours.empty());
    CHECK(why.find("L") != std::string::npos);
    CHECK_FALSE(parse_path("M0 0 L10 10 L20", p, &why));
    REQUIRE(p.contours.size() == 1);
    CHECK(p.contours[0].curves.size() == 1);
    CHECK_FALSE(parse_path("M0 0 L10 10 L10 0 Z 5", p, &why));
    REQUIRE(p.contours.size() == 1);
    CHECK(p.contours[0].closed);
    CHECK(why.find("«5»") != std::string::npos);
    CHECK_FALSE(parse_path("M0 0 X 1", p, &why));
    CHECK(why.find("знак 6") != std::string::npos);
    CHECK_FALSE(parse_path("M0 0 L1e999 0", p, &why));
    CHECK_FALSE(parse_path("M0 0 A10 10 0 2 1 5 5", p, &why)); // a flag is 0 or 1
    CHECK_FALSE(parse_path("M0 0 L1e 0", p, &why));
    CHECK_FALSE(parse_path("M0 0 L- 0", p, &why));
}

TEST_CASE("Vector path: written as d and read back the same") {
    CHECK(to_d(parsed("M0 0 C1 2 3 4 5 6 Z")) == "M 0 0 C 1 2 3 4 5 6 C 3.333333333333334 4 1.666666666666667 2 0 0 Z");
    CHECK(to_d(parsed("M-0 0.1 C1e-7 2 3 4 5 6")) == "M 0 0.1 C 1e-07 2 3 4 5 6");
    for (const char* d : {"M0 0 A10 10 0 0 1 20 0", "M3 3 Q5 10 10 0 T20 0 L30 5 Z",
                          "M0 0 A20 10 30 0 1 30 10 V40 H-5 Z", "M10 10 c0 10 10 10 10 0 s10 -10 10 0 z m5 5 l1 2 3 -4"}) {
        INFO(d);
        const Path p = parsed(d);
        CHECK(parsed(to_d(p)) == p);
    }
    CHECK(to_d({}).empty());
}

TEST_CASE("Vector path: basic shapes as SVG 2 lays them out") {
    const Path c = circle(10, 20, 5);
    REQUIRE(c.contours.size() == 1);
    const Contour& cc = c.contours[0];
    CHECK(cc.closed);
    CHECK(cc.start == Point{15, 20});
    REQUIRE(cc.curves.size() == 4);
    CHECK(cc.curves[0].to == Point{10, 25}); // then clockwise on the screen
    CHECK(cc.curves[1].to == Point{5, 20});
    CHECK(cc.curves[2].to == Point{10, 15});
    CHECK(cc.curves[3].to == Point{15, 20});
    CHECK(area(cc) > 0);
    CHECK(area(cc) == doctest::Approx(kPi * 25).epsilon(3e-4));
    for (Point s : samples(cc)) CHECK(std::fabs(dist(s, {10, 20}) - 5) <= 5 * 3e-4);

    // An open contour counts as closed by a straight line back.
    CHECK(area(parsed("M10 0 L20 0 L20 10").contours[0]) == doctest::Approx(50));
    CHECK(area(parsed("M0 0 L10 0 L10 10 Z").contours[0]) == doctest::Approx(50));
    CHECK(area(parsed("M0 0 L0 10 L10 10 Z").contours[0]) == doctest::Approx(-50));

    const Path r = rect(0, 0, 10, 20);
    REQUIRE(r.contours[0].curves.size() == 4);
    CHECK(r.contours[0].start == Point{0, 0});
    CHECK(r.contours[0].curves[0].to == Point{10, 0});
    CHECK(area(r.contours[0]) == 200);
    const Path rr = rect(0, 0, 10, 20, 2, 3);
    REQUIRE(rr.contours[0].curves.size() == 8);
    CHECK(rr.contours[0].start == Point{2, 0});
    CHECK(rr.contours[0].curves[1].to == Point{10, 3});
    // Corners as large as the sides: no sides left, an ellipse.
    const Path round = rect(0, 0, 10, 20, 50, 50);
    CHECK(round.contours[0].curves.size() == 4);
    CHECK(area(round.contours[0]) == doctest::Approx(area(ellipse(5, 10, 5, 10).contours[0])));

    CHECK(rect(0, 0, 0, 5).contours.empty());
    CHECK(circle(0, 0, 0).contours.empty());
    CHECK(ellipse(0, 0, 5, -1).contours.empty());

    const std::vector<Point> pts{{0, 0}, {10, 0}, {10, 10}};
    CHECK(polyline(pts, false).contours[0].curves.size() == 2);
    CHECK(polyline(pts, true).contours[0].curves.size() == 3);
    const std::vector<Point> back{{0, 0}, {10, 0}, {10, 10}, {0, 0}};
    CHECK(polyline(back, true).contours[0].curves.size() == 3);
    CHECK(polyline(std::vector<Point>{{1, 1}}, true).contours.empty());

    std::vector<Point> read;
    std::string why;
    CHECK(parse_points("0,0 10,0 10 10", read, &why));
    CHECK(read == pts);
    CHECK_FALSE(parse_points("0 0 10", read, &why));
    CHECK(read.size() == 1);
    CHECK_FALSE(parse_points("0 0 x", read, &why));
}

TEST_CASE("Vector path: lunasvg draws what was read and written back as the original") {
    for (const char* d : {"M8 8 L56 8 L32 56 Z", "M4 32 A28 28 0 0 1 60 32 Z", "M10 50 Q20 0 32 30 T56 20 L56 56 Z",
                          "M8 8 C8 40 56 40 56 8 S40 56 8 56 Z", "M32 4 a28 14 30 1 0 1 0 Z",
                          "M4 4 h56 v56 h-56 z M20 20 v24 h24 v-24 z",
                          "M8 32 A30 10 -20 0 1 56 32 A30 10 -20 0 1 8 32"}) {
        INFO(d);
        const std::vector<u8> original = draw(path_element(d));
        const std::vector<u8> again = draw(path_element(to_d(parsed(d))));
        CHECK(covered(original) > 200);
        const Difference diff = compare(original, again);
        CHECK(diff.largest <= 2);
    }
    // SVG's own shapes against ours.
    struct Case {
        std::string element;
        Path path;
    };
    const std::vector<Point> tri{{8, 56}, {32, 8}, {56, 56}};
    const Case cases[] = {
        {"<circle cx='32' cy='32' r='24'/>", circle(32, 32, 24)},
        {"<ellipse cx='32' cy='30' rx='26' ry='14'/>", ellipse(32, 30, 26, 14)},
        {"<rect x='6' y='10' width='50' height='40' rx='12' ry='8'/>", rect(6, 10, 50, 40, 12, 8)},
        {"<rect x='6' y='10' width='50' height='40'/>", rect(6, 10, 50, 40)},
        {"<polygon points='8,56 32,8 56,56'/>", polyline(tri, true)},
    };
    for (const Case& c : cases) {
        INFO(c.element);
        const Difference diff = compare(draw(c.element), draw(path_element(to_d(c.path))));
        CHECK(diff.largest <= 2);
    }
}

TEST_CASE("Vector morph: the ends are the two shapes") {
    const Shape round = filled(circle(32, 32, 24), rgb8(200, 40, 40));
    const Shape spiky = filled(star(5, 32, 32, 28, 12), rgb8(40, 40, 200));
    const Morph m = prepare(round, spiky);
    REQUIRE(m.from.path.contours.size() == 1);
    REQUIRE(m.to.path.contours.size() == 1);
    const Contour& a = m.from.path.contours[0];
    const Contour& b = m.to.path.contours[0];
    CHECK(a.curves.size() == 10);
    CHECK(b.curves.size() == 10);

    // The ends: exactly the prepared shapes, which draw as the originals.
    CHECK(mix(m, 0) == m.from);
    CHECK(mix(m, 1) == m.to);
    // lunasvg flattens a curve into lines, and a curve cut in pieces into more of them: edge pixels may differ by a
    // tenth of their colour, and the pieces cover the exact area at least as closely as the whole curve (a circle of
    // radius 24: 1802 pixels against 1799, of 1810).
    for (const auto& [prepared, original] : {std::pair{&m.from, &round}, std::pair{&m.to, &spiky}}) {
        const std::vector<u8> p = draw(path_element(to_d(prepared->path)));
        const std::vector<u8> o = draw(path_element(to_d(original->path)));
        CHECK(compare(p, o).largest <= 26);
        const f64 exact = std::fabs(area(original->path.contours[0]));
        CHECK(std::fabs(covered(p) - exact) <= std::fabs(covered(o) - exact) + 1);
    }
    CHECK(area(a) == doctest::Approx(area(round.path.contours[0])).epsilon(1e-12));
    CHECK(std::fabs(area(b)) == doctest::Approx(std::fabs(area(spiky.path.contours[0]))).epsilon(1e-12));

    // The circle's four quarters are cut by length into 3, 3, 2, 2 pieces at equal steps.
    const Contour& c0 = round.path.contours[0];
    CHECK(near(a.curves[0].to, point_at(c0.start, c0.curves[0], 1.0 / 3), 1e-12));
    CHECK(near(a.curves[1].to, point_at(c0.start, c0.curves[0], 2.0 / 3), 1e-12));
    CHECK(a.curves[2].to == c0.curves[0].to);
    CHECK(near(a.curves[8].to, point_at(c0.from(3), c0.curves[3], 0.5), 1e-12));
    CHECK(a.curves[9].to == c0.curves[3].to);

    // The star went round the other way: it is turned round to go as the circle does.
    CHECK(area(spiky.path.contours[0]) < 0);
    CHECK(area(b) > 0);
    // Its start moved to where the pair is closest: no other start is closer.
    auto cost = [&](usize r) {
        f64 s = 0;
        for (usize i = 0; i < 10; ++i) {
            const Cubic& p = a.curves[i];
            const Cubic& q = b.curves[(i + r) % 10];
            for (auto [u, v] : {std::pair{p.c1, q.c1}, std::pair{p.c2, q.c2}, std::pair{p.to, q.to}})
                s += dist(u, v) * dist(u, v);
        }
        return s;
    };
    for (usize r = 1; r < 10; ++r) CHECK(cost(0) <= cost(r));
    CHECK_FALSE(b.start == spiky.path.contours[0].start);

    // Halfway: every point halfway, the colour halfway.
    const Shape half = mix(m, 0.5);
    for (usize i = 0; i < 10; ++i) {
        const Point p = a.curves[i].to, q = b.curves[i].to;
        CHECK(near(half.path.contours[0].curves[i].to, {(p.x + q.x) / 2, (p.y + q.y) / 2}, 1e-12));
    }
    CHECK(hex(half.look.fill) == "#782878");
    // The picture halfway is between the two: it covers more than the star's inside and less than the circle.
    const f64 at_half = covered(draw(path_element(to_d(half.path))));
    CHECK(at_half < covered(draw(path_element(to_d(round.path)))));
    CHECK(at_half > covered(draw(path_element(to_d(spiky.path)))));
}

TEST_CASE("Vector morph: contours one side lacks shrink into their centre") {
    // A ring (a circle with a hole) into a disc: the hole shrinks to a point at its own centre.
    Path ring = circle(32, 32, 24);
    Path hole = circle(40, 30, 8);
    ring.contours.push_back(hole.contours[0]);
    const Morph m = prepare(filled(ring), filled(circle(32, 32, 20)));
    REQUIRE(m.to.path.contours.size() == 2);
    const Contour& dot = m.to.path.contours[1];
    CHECK(dot.closed);
    CHECK(dot.curves.size() == 4);
    CHECK(dot.start == Point{40, 30});
    for (const Cubic& q : dot.curves) CHECK((q.c1 == Point{40, 30} && q.c2 == Point{40, 30} && q.to == Point{40, 30}));
    const Shape half = mix(m, 0.5);
    CHECK(near(half.path.contours[1].start, {44, 30}, 1e-12)); // halfway between (48, 30) and the centre
    CHECK(mix(m, 1) == m.to);

    // The other way round: it grows out of its centre.
    const Morph grow = prepare(filled(circle(32, 32, 20)), filled(ring));
    REQUIRE(grow.from.path.contours.size() == 2);
    CHECK(grow.from.path.contours[1].start == Point{40, 30});
}

TEST_CASE("Vector morph: open contours, closing lines, flat contours") {
    // Open: the nearer ends pair, so a line does not flip over.
    const Morph lines = prepare(filled(parsed("M0 0 L10 0")), filled(parsed("M10 5 L0 5")));
    CHECK(lines.to.path.contours[0].start == Point{0, 5});
    CHECK(lines.to.path.contours[0].curves.back().to == Point{10, 5});
    // A closed contour that stops short of its start gets the line back.
    Shape open_end;
    Contour c;
    c.start = {0, 0};
    c.curves = {line({0, 0}, {10, 0}), line({10, 0}, {10, 10})};
    c.closed = true;
    open_end.path.contours.push_back(c);
    const Morph closing = prepare(open_end, filled(rect(0, 0, 10, 10)));
    CHECK(closing.from.path.contours[0].curves.size() == 4);
    CHECK(closing.from.path.contours[0].curves.back().to == Point{0, 0});
    // A flat closed contour (no inside: here an L drawn there and back) has no way round of its own: the other
    // contour goes whichever way, from whichever start, brings the pair closest.
    const Morph flat = prepare(filled(parsed("M0 0 L0 10 L20 10 L0 10 Z")), filled(rect(0, 0, 20, 10)));
    const Contour& fa = flat.from.path.contours[0];
    const Contour& fb = flat.to.path.contours[0];
    REQUIRE(fb.curves.size() == 4);
    CHECK(area(fa) == 0);
    auto apart = [&](const Contour& b, usize r) {
        f64 s = 0;
        for (usize i = 0; i < 4; ++i) {
            const Cubic& p = fa.curves[i];
            const Cubic& q = b.curves[(i + r) % 4];
            for (auto [u, v] : {std::pair{p.c1, q.c1}, std::pair{p.c2, q.c2}, std::pair{p.to, q.to}}) s += dist(u, v) * dist(u, v);
        }
        return s;
    };
    Contour turned{fb.start, {}, true};
    for (usize i = 4; i-- > 0;) turned.curves.push_back({fb.curves[i].c2, fb.curves[i].c1, fb.from(i)});
    for (usize r = 0; r < 4; ++r) {
        CHECK(apart(fb, 0) <= apart(fb, r));
        CHECK(apart(fb, 0) <= apart(turned, r));
    }
    CHECK(area(fb) < 0); // the rectangle went round the other way for it
    CHECK(mix(flat, 1) == flat.to);
    // A contour that is a lone point grows out of it.
    Shape dot;
    dot.path.contours.push_back(Contour{{5, 5}, {}, true});
    const Morph from_dot = prepare(dot, filled(rect(0, 0, 10, 10)));
    REQUIRE(from_dot.from.path.contours[0].curves.size() == 4);
    CHECK(from_dot.from.path.contours[0].curves[2].to == Point{5, 5});
    CHECK(mix(from_dot, 0.5).path.contours[0].curves.size() == 4);
    // A pair not made by prepare mixes only what both sides have.
    Morph uneven{filled(rect(0, 0, 10, 10)), filled(parsed("M0 0 L5 5"))};
    uneven.from.path.contours.push_back(uneven.from.path.contours[0]);
    const Shape some = mix(uneven, 0.5);
    REQUIRE(some.path.contours.size() == 1);
    CHECK(some.path.contours[0].curves.size() == 1);
    // Closed below 0.5, as the second from it.
    const Morph to_open = prepare(filled(rect(0, 0, 10, 10)), filled(parsed("M0 0 L10 0 L10 10 L0 10")));
    CHECK(mix(to_open, 0.49).path.contours[0].closed);
    CHECK_FALSE(mix(to_open, 0.5).path.contours[0].closed);
}

TEST_CASE("Vector morph: colours, opacity and stroke") {
    Shape a = filled(rect(0, 0, 10, 10), rgb8(255, 0, 0));
    Shape b = filled(rect(0, 0, 20, 20));
    b.look.fill = {};
    b.look.stroke = rgb8(0, 0, 255, 0.5);
    b.look.stroke_width = 4;
    a.look.stroke_width = 1; // off: its width does not count
    a.look.opacity = 0.2;
    const Morph m = prepare(a, b);
    CHECK(mix(m, 0).look == a.look);
    CHECK(mix(m, 1).look == b.look);
    const Look half = mix(m, 0.5).look;
    // The fill fades out in its own colour; the stroke fades in in its own colour, at its own width all along.
    CHECK(half.fill.on);
    CHECK(hex(half.fill) == "#ff0000");
    CHECK(half.fill.opacity == 0.5);
    CHECK(hex(half.stroke) == "#0000ff");
    CHECK(half.stroke.opacity == 0.25);
    CHECK(half.stroke_width == 4);
    CHECK(half.opacity == doctest::Approx(0.6));
    // Past the ends the points go on, the look stays at the end.
    const Shape past = mix(m, 1.5);
    CHECK(past.look == b.look);
    CHECK(mix(m, -0.5).look == a.look);
    CHECK(near(past.path.contours[0].curves[0].to, {25, 0}, 1e-12)); // (10, 0) → (20, 0), half again
    // Both off: off.
    Shape c = a, d = b;
    c.look.fill = {};
    d.look.fill = {};
    CHECK_FALSE(mix(prepare(c, d), 0.5).look.fill.on);

    CHECK(hex(rgb8(255, 128, 0)) == "#ff8000");
    CHECK(hex(Paint{}) == "none");
    CHECK(hex(Paint{true, 1.2, -1, 0.5, 1}) == "#ff0080");
}

TEST_CASE("Vector morph: frames reuse their arrays") {
    const Morph m = prepare(filled(circle(32, 32, 24)), filled(star(6, 32, 32, 28, 14)));
    Shape frame;
    mix(m, 0.25, frame);
    const Cubic* data = frame.path.contours[0].curves.data();
    for (int i = 0; i < 60; ++i) mix(m, i / 59.0, frame);
    CHECK(frame.path.contours[0].curves.data() == data);
    CHECK(frame == m.to);
}

TEST_CASE("Vector: the same bits on every system") {
    // These numbers come from Linux; Windows must give them to the last bit (CI runs this on both).
    CHECK(std::bit_cast<u64>(trig::sin(1)) == 0x3FEAED548F090CEEull);
    CHECK(std::bit_cast<u64>(trig::cos(2.5)) == 0xBFE9A2F7EF858B7Dull);
    CHECK(std::bit_cast<u64>(trig::atan2(-0.3, -2)) == 0xC007F10E1DC6B048ull);
    CHECK(std::bit_cast<u64>(trig::exp(-3.7)) == 0x3F99511FC6871044ull);
    CHECK(std::bit_cast<u64>(ease(Ease::Elastic, 0.37)) == 0x3FEE0AF2B26DA365ull);
    const Path arc = parsed("M3 7 A30 15 30 1 1 41 13 Q50 30 20 40 Z");
    CHECK(to_d(arc) ==
          "M 3 7"
          " C -9.803650388958767 -2.230894903677415 -15.09456066359772 -13.924701305171292"
          " -9.01626457504401 -19.55797666485327"
          " C -2.9379684864902966 -25.191252024535252 12.363635558554291 -22.775209280580842"
          " 25.73543467586918 -14.07086625681435"
          " C 39.10723379318406 -5.366523233047859 45.82841116132182 6.553114814787936 41 13"
          " C 47 24.333333333333336 40 33.333333333333336 20 40"
          " C 14.333333333333336 29.000000000000004 8.666666666666668 18 3 7 Z");
    const Morph m = prepare(filled(arc), filled(star(5, 30, 20, 25, 10)));
    CHECK(to_d(mix(m, ease(Ease::Back, 0.6)).path) ==
          "M 22.227039570359754 24.688755359146487"
          " C 17.63534376367749 21.18392704510081 12.857047119054954 17.740272329804977"
          " 7.844256336406051 14.25193892129705"
          " C 14.618130924654293 14.593325244699939 21.15751137487617 14.89003287489073"
          " 27.41450438698557 15.03620951990748"
          " C 29.266502271641126 9.119430511733148 30.889412371138008 3.0027231844461433"
          " 32.421744748260814 -3.2920424520401714"
          " C 33.95407712538363 2.8064252987091876 35.39583178013238 8.726834740259235"
          " 36.885518775291686 14.49105588252333"
          " C 42.78020089790484 14.192321371596346 48.84007063147431 13.813725620085131"
          " 55.125968048485745 13.46316514717261"
          " C 49.62520009460005 16.9423244061709 44.35045982415633 20.44951894376789"
          " 39.36258730964025 24.092645279146495"
          " C 40.964236211318116 29.808876185386406 42.88877807299598 35.58306223829299"
          " 45.13621289467385 41.41520343786623"
          " C 40.0990879031159 37.22100821191082 35.38485587155795 33.08476813262208 30.9935168 29.006483199999998"
          " C 25.89015735510871 33.30003010595541 20.786797910217427 37.59357701191082"
          " 15.68343846532614 41.88712391786622"
          " C 17.864638833670675 36.15433439829298 20.04583920201522 30.421544878719736"
          " 22.227039570359754 24.688755359146487 Z");
}
