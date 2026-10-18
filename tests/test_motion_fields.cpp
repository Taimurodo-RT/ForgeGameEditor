// Part 4ж of docs/models/движение-модель.md («Поля и кривая»): a part's field writes the clip's code by the 3б drag's
// rules for one property (the end of its transition going, a field of another one's table going, a line at(…) of its
// own); bezier(x1, y1, x2, y2) is CSS's cubic-bezier as an easing, computed exactly, its numbers rewritten in its call.

#include "forge/pixel/code_sites.h"
#include "forge/pixel/motion.h"
#include "forge/pixel/motion_edit.h"
#include "forge/pixel/script.h"
#include "forge/vector/timing.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <ostream> // doctest's INFO prints a string_view through it; MSVC needs the whole class
#include <string>
#include <vector>

using namespace forge;
using namespace forge::pixel;

namespace {

PoseScript script_of(std::string_view source) {
    PoseScript s;
    const bool ok = s.load(source);
    INFO(s.error());
    REQUIRE(ok);
    return s;
}

// A script of one clip, "c", its body from line 2 on.
std::string clip_of(std::string_view body) { return "return { clips = { c = function(p, rng)\n" + std::string(body) + "\nend } }"; }

Recording recorded(PoseScript& s) {
    Recording rec;
    const bool ok = s.record(nullptr, "c", {}, 1, {}, rec);
    INFO(s.error());
    REQUIRE(ok);
    return rec;
}

std::string record_error(std::string_view source) {
    PoseScript s;
    Recording rec;
    if (s.load(source)) CHECK_FALSE(s.record(nullptr, "c", {}, 1, {}, rec));
    return s.error();
}

// The nth block of a part's property, in the order the code made them.
const Block& block(const Recording& rec, std::string_view part, Prop p, usize nth = 0) {
    for (const Block& b : rec.blocks)
        if (rec.parts[b.part] == part && b.prop == p && nth-- == 0) return b;
    FAIL("no block " << std::string(part) << " " << prop_word(p));
    return rec.blocks.front();
}

PropValue sampled(const Recording& rec, std::string_view part, Prop p, f64 t) {
    const u32 pi = static_cast<u32>(std::find(rec.parts.begin(), rec.parts.end(), part) - rec.parts.begin());
    const i32 tr = pi < rec.parts.size() ? rec.track(pi, p) : -1;
    PropValue v = rest_value(p);
    std::string why;
    if (tr >= 0) REQUIRE(sample(rec, static_cast<u32>(tr), t, nullptr, v, why));
    return v;
}

f64 value_at(const Recording& rec, std::string_view part, Prop p, f64 t) { return sampled(rec, part, p, t).v; }

// The source with `from` (once) made `to`.
std::string with(std::string_view source, std::string_view from, std::string_view to) {
    std::string s(source);
    const usize at = s.find(from);
    REQUIRE(at != std::string::npos);
    REQUIRE(s.find(from, at + 1) == std::string::npos);
    return s.replace(at, from.size(), to);
}

PropValue number(f64 v) {
    PropValue p;
    p.v = v;
    return p;
}

PropValue color(const char* text) {
    PropValue p;
    if (text) {
        REQUIRE(parse_part_color(text, p.color));
        p.v = 1;
    }
    return p;
}

constexpr vector::Bezier kEase{0.25, 0.1, 0.25, 1};

} // namespace

TEST_CASE("Motion fields: bezier is CSS's cubic-bezier") {
    using vector::bezier;
    // The points of CSS's curves, as an exact bisection of the curve gives them.
    CHECK(bezier(kEase, 0.25) == doctest::Approx(0.40851059135539586).epsilon(1e-6));
    CHECK(bezier(kEase, 0.5) == doctest::Approx(0.802403387584857).epsilon(1e-6));
    CHECK(bezier(kEase, 0.75) == doctest::Approx(0.9604589783489741).epsilon(1e-6));
    CHECK(bezier({0.42, 0, 0.58, 1}, 0.3) == doctest::Approx(0.18739590670531256).epsilon(1e-6));
    CHECK(bezier({0.3, 0, 0.2, 1}, 0.5) == doctest::Approx(0.7971529854160904).epsilon(1e-6));
    // x1 = x2 = 0: x = u³; x1 = x2 = 1: x = 1 − (1 − u)³, flat at the end (Newton's steps give way to halving).
    CHECK(bezier({0, 0, 0, 1}, 0.125) == doctest::Approx(0.5).epsilon(1e-6));
    CHECK(bezier({1, 0, 1, 1}, 0.875) == doctest::Approx(0.5).epsilon(1e-6));
    CHECK(bezier({1, 0, 1, 1}, 0.999) == doctest::Approx(0.972).epsilon(1e-5));
    // Past the ends on the way, as back does.
    CHECK(bezier({0.5, -0.5, 0.5, 1.5}, 0.1) == doctest::Approx(-0.07075622807293012).epsilon(1e-6));
    CHECK(bezier({0.5, -0.5, 0.5, 1.5}, 0.9) == doctest::Approx(1.0707562280729301).epsilon(1e-6));
    // linear is x itself; the ends are exact and x is held to them.
    for (f64 x : {0.1, 0.37, 0.5, 0.9}) CHECK(bezier({0, 0, 1, 1}, x) == doctest::Approx(x).epsilon(1e-7));
    for (const vector::Bezier& b : {kEase, vector::Bezier{1, 0, 1, 1}, vector::Bezier{0.5, -0.5, 0.5, 1.5}}) {
        CHECK(bezier(b, 0) == 0);
        CHECK(bezier(b, 1) == 1);
        CHECK(bezier(b, -0.5) == 0);
        CHECK(bezier(b, 1.5) == 1);
    }
    // y within 0 … 1 at the control points: it only grows.
    f64 last = 0;
    bool grows = true;
    for (int i = 0; i <= 1000; ++i) {
        const f64 y = bezier(kEase, i / 1000.0);
        grows = grows && y >= last;
        last = y;
    }
    CHECK(grows);

    CHECK(vector::bezier_valid(kEase));
    CHECK(vector::bezier_valid({0.3, -2, 0.7, 3}));
    CHECK(vector::bezier_valid({0, 0, 1, 1}));
    CHECK_FALSE(vector::bezier_valid({-0.1, 0, 0.5, 1}));
    CHECK_FALSE(vector::bezier_valid({0.5, 0, 1.1, 1}));
    CHECK_FALSE(vector::bezier_valid({0.5, std::nan(""), 0.5, 1}));
    CHECK_FALSE(vector::bezier_valid({0.5, 0, 0.5, INFINITY}));
}

TEST_CASE("Motion fields: bezier(…) of the code is its block's easing") {
    const std::string src = R"(local top = bezier(0.42, 0, 0.58, 1)
)" + clip_of(R"(local a = part("a")
local e = bezier(0.3, 0, 0.2, 1)
a.x(10, 1, bezier(0.25, 0.1, 0.25, 1))
part("b"):to({ y = 4 }, 0.5, e)
part("c").y(2, 0.5, e)
part("d").rot(e(0.5), 0)
part("f").x(8, 0.4, top)
log(tostring(e)))");
    PoseScript s = script_of(src);
    const Recording rec = recorded(s);
    const Block& x = block(rec, "a", Prop::X);
    REQUIRE(x.bezier >= 0);
    REQUIRE(static_cast<usize>(x.bezier) < rec.beziers.size());
    const BezierUse& use = rec.beziers[static_cast<usize>(x.bezier)];
    CHECK(use.curve == kEase);
    REQUIRE(use.site.site >= 0);
    CHECK(use.site.file == 0);
    CHECK(s.code().sites()[static_cast<usize>(use.site.site)].name == "bezier");
    CHECK(s.code().sites()[static_cast<usize>(use.site.site)].line == 5);
    // The frame: the curve itself, not points of it.
    CHECK(value_at(rec, "a", Prop::X, 0.5) == 10 * vector::bezier(kEase, 0.5));
    CHECK(value_at(rec, "a", Prop::X, 0.25) == 10 * vector::bezier(kEase, 0.25));
    CHECK(block_share(rec, x, 0.75) == vector::bezier(kEase, 0.75));

    // One value in a variable for two calls: both blocks of the one call.
    const Block& b = block(rec, "b", Prop::Y);
    const Block& c = block(rec, "c", Prop::Y);
    REQUIRE(b.bezier >= 0);
    REQUIRE(c.bezier >= 0);
    CHECK(rec.beziers[static_cast<usize>(b.bezier)].site == rec.beziers[static_cast<usize>(c.bezier)].site);
    CHECK(rec.beziers[static_cast<usize>(b.bezier)].curve == vector::Bezier{0.3, 0, 0.2, 1});
    // A line waits for the one before: b from 1, d at 2, f from 2.
    CHECK(b.start == 1);
    CHECK(value_at(rec, "b", Prop::Y, 1.25) == doctest::Approx(4 * vector::bezier({0.3, 0, 0.2, 1}, 0.5)).epsilon(1e-12));
    CHECK(bezier_users(rec, b) == 2);
    CHECK(bezier_users(rec, x) == 1);
    CHECK(bezier_users(rec, block(rec, "d", Prop::Rot)) == 0);
    // Called, it is y at x; written, its numbers.
    CHECK(value_at(rec, "d", Prop::Rot, 2) == vector::bezier({0.3, 0, 0.2, 1}, 0.5));
    REQUIRE(rec.log.size() == 1);
    CHECK(rec.log[0].text == "bezier(0.3, 0, 0.2, 1)");
    // Made outside the clips, when the file runs: its call there.
    const Block& f = block(rec, "f", Prop::X);
    REQUIRE(f.bezier >= 0);
    CHECK(s.code().sites()[static_cast<usize>(rec.beziers[static_cast<usize>(f.bezier)].site.site)].line == 1);
    CHECK(f.start == 2);
    CHECK(value_at(rec, "f", Prop::X, 2.12) == doctest::Approx(8 * vector::bezier({0.42, 0, 0.58, 1}, 0.3)).epsilon(1e-12));

    // x1 and x2 within 0 … 1, numbers all: an error with its line.
    CHECK(record_error(clip_of("part(\"a\").x(1, 1, bezier(1.2, 0, 0.2, 1))")) ==
          "строка 2: bezier: x1 и x2 — от 0 до 1, y1 и y2 — числа");
    CHECK(record_error(clip_of("\npart(\"a\").x(1, 1, bezier(0.2, 0, -0.01, 1))")) ==
          "строка 3: bezier: x1 и x2 — от 0 до 1, y1 и y2 — числа");
    CHECK(record_error(clip_of("part(\"a\").x(1, 1, bezier(0.2, \"0\", 0.3, 1))")) ==
          "строка 2: bezier: x1 и x2 — от 0 до 1, y1 и y2 — числа");
    CHECK(record_error(clip_of("part(\"a\").x(1, 1, bezier(0.2, 0, 0.3))")) == "строка 2: bezier: x1 и x2 — от 0 до 1, y1 и y2 — числа");
    CHECK(record_error(clip_of("part(\"a\").x(1, 1, {})")) == "строка 2: сглаживание — слово, bezier(…) или функция(k)");
    // A curve called with no number.
    CHECK(record_error(clip_of("local e = bezier(0.2, 0, 0.3, 1)\npart(\"a\").x(e(\"a\"), 0)")) ==
          "строка 3: bezier: кривая(x) — x число от 0 до 1");
}

TEST_CASE("Motion fields: the timing as bezier(…), and the numbers of its call") {
    const std::string src = clip_of(R"(local a = part("a")
local k = 0.3
local e = bezier(0.3, 0, 0.2, 1)
a.x(10, 1, "out")
a.y(2, 0.5)
a.rot(5)
part("b").x(1, 0.5, bezier(k, 0, 0.2, 1))
part("c").x(1, 0.5, e)
part("d").x(1, 0.5, e))");
    PoseScript s = script_of(src);
    const Recording rec = recorded(s);
    const CodeSites& code = s.code();
    std::string out, why;
    // A word, none, none and no length: bezier(…) in its place, added after the length.
    REQUIRE(edit_timing_bezier(rec, block(rec, "a", Prop::X), kEase, code, src, out, why));
    CHECK(out == with(src, "a.x(10, 1, \"out\")", "a.x(10, 1, bezier(0.25, 0.1, 0.25, 1))"));
    PoseScript as = script_of(out);
    const Recording ar = recorded(as);
    CHECK(value_at(ar, "a", Prop::X, 0.5) == 10 * vector::bezier(kEase, 0.5));
    REQUIRE(edit_timing_bezier(rec, block(rec, "a", Prop::Y), kEase, code, src, out, why));
    CHECK(out == with(src, "a.y(2, 0.5)", "a.y(2, 0.5, bezier(0.25, 0.1, 0.25, 1))"));
    REQUIRE(edit_timing_bezier(rec, block(rec, "a", Prop::Rot), {0, 0, 1, 1}, code, src, out, why));
    CHECK(out == with(src, "a.rot(5)", "a.rot(5, 0, bezier(0, 0, 1, 1))"));
    CHECK_FALSE(edit_timing_bezier(rec, block(rec, "a", Prop::Y), {1.5, 0, 0.2, 1}, code, src, out, why));
    CHECK(why == "bezier: x1 и x2 — от 0 до 1, y1 и y2 — числа");
    CHECK(out.empty());
    // A word where bezier(…) was: in place of the whole call.
    REQUIRE(edit_timing(rec, block(rec, "b", Prop::X), vector::Timing::OutQuad, code, src, out, why));
    CHECK(out == with(src, "bezier(k, 0, 0.2, 1)", "\"out_quad\""));

    // Its numbers: those that changed; an expression keeps itself with a correction.
    const Block& b = block(rec, "b", Prop::X);
    CHECK(can_edit_bezier(rec, b, code, why));
    CHECK(why.empty());
    REQUIRE(edit_bezier(rec, b, {0.4, 0.25, 0.2, 1}, code, src, out, why));
    CHECK(out == with(src, "bezier(k, 0, 0.2, 1)", "bezier(k + 0.1, 0.25, 0.2, 1)"));
    REQUIRE(edit_bezier(rec, b, {0.3, 0, 0.75, 1.2}, code, src, out, why));
    CHECK(out == with(src, "bezier(k, 0, 0.2, 1)", "bezier(k, 0, 0.75, 1.2)"));
    PoseScript bs = script_of(out);
    const Recording br = recorded(bs);
    CHECK(rec.beziers.size() == br.beziers.size());
    CHECK(br.beziers[static_cast<usize>(block(br, "b", Prop::X).bezier)].curve == vector::Bezier{0.3, 0, 0.75, 1.2});
    const f64 b_start = block(br, "b", Prop::X).start; // after a's three lines
    CHECK(b_start == 1.5);
    CHECK(value_at(br, "b", Prop::X, b_start + 0.25) == doctest::Approx(vector::bezier({0.3, 0, 0.75, 1.2}, 0.5)).epsilon(1e-12));
    // The same numbers: the same text.
    REQUIRE(edit_bezier(rec, b, {0.3, 0, 0.2, 1}, code, src, out, why));
    CHECK(out == src);
    CHECK_FALSE(edit_bezier(rec, b, {0.3, 0, -0.2, 1}, code, src, out, why));
    CHECK(why == "bezier: x1 и x2 — от 0 до 1, y1 и y2 — числа");
    // Shared: one call for both blocks.
    const Block& d = block(rec, "d", Prop::X);
    REQUIRE(edit_bezier(rec, d, {0.3, 0, 0.2, 0.9}, code, src, out, why));
    CHECK(out == with(src, "local e = bezier(0.3, 0, 0.2, 1)", "local e = bezier(0.3, 0, 0.2, 0.9)"));
    CHECK(bezier_users(rec, d) == 2);

    // A block without one; a curve not called by its name; a module's.
    CHECK_FALSE(can_edit_bezier(rec, block(rec, "a", Prop::X), code, why));
    CHECK(why == "у блока нет кривой bezier(…)");
    CHECK_FALSE(edit_bezier(rec, block(rec, "a", Prop::X), kEase, code, src, out, why));
    CHECK(out.empty());
    const std::string alias = clip_of("local bz = bezier\npart(\"a\").x(1, 0.5, bz(0.3, 0, 0.2, 1))");
    PoseScript al = script_of(alias);
    const Recording alr = recorded(al);
    CHECK_FALSE(can_edit_bezier(alr, block(alr, "a", Prop::X), al.code(), why));
    CHECK(why == "кривая не вызовом bezier(…) с четырьмя числами: правьте её в коде");
    PoseScript m;
    m.set_module_reader([](std::string_view name, std::string& text, std::string& w) {
        if (name != "moves") return w = "нет", false;
        text = "return { lift = function(h) h:to({ y = -3 }, 0.4, bezier(0.1, 0.2, 0.3, 0.4)) end }";
        return true;
    });
    const std::string ms = "local mv = require(\"moves\")\n" + clip_of("mv.lift(part(\"a\"))");
    REQUIRE(m.load(ms));
    const Recording mr = recorded(m);
    const Block& my = block(mr, "a", Prop::Y);
    REQUIRE(my.bezier >= 0);
    CHECK(mr.beziers[static_cast<usize>(my.bezier)].site.file == 1);
    CHECK_FALSE(can_edit_bezier(mr, my, m.code(), why));
    CHECK(why == "кривая из модуля «moves.luau»: правьте её в коде");
}

TEST_CASE("Motion fields: a part's field, by the drag's rules for one property") {
    // x, fill and y from 0 to 0.4 (at(…) does not wait), opacity at 0.1, rot from 0.6.
    const std::string src = clip_of(R"(local a = part("a")
at(0, a.x(4, 0.4))
at(0, a.fill("#ff0000", 0.4))
part("a"):to({ y = 2 }, 0.4)
at(0.1, a.opacity(0.5, 0))
at(0.6, a.rot(10, 0.2)))");
    PoseScript s = script_of(src);
    const Recording rec = recorded(s);
    const CodeSites& code = s.code();
    std::string out, why;
    bool going = false;

    // Its transition goes: the field is its end, and typing makes the end.
    CHECK(part_prop_value(rec, "a", 0.2, 0.1, Prop::X, going).v == 4);
    CHECK(going);
    REQUIRE(edit_part_prop(rec, "a", 0.2, 0.1, Prop::X, number(7), code, src, out, why));
    CHECK(out == with(src, "a.x(4, 0.4)", "a.x(7, 0.4)"));
    REQUIRE(edit_part_prop(rec, "a", 0.2, 0.1, Prop::Fill, color("#00ff0080"), code, src, out, why));
    CHECK(out == with(src, "a.fill(\"#ff0000\", 0.4)", "a.fill(\"#00ff0080\", 0.4)"));
    REQUIRE(edit_part_prop(rec, "a", 0.2, 0.1, Prop::Fill, color(nullptr), code, src, out, why));
    CHECK(out == with(src, "a.fill(\"#ff0000\", 0.4)", "a.fill(nil, 0.4)"));
    // Done going: the recording's value there.
    CHECK(part_prop_value(rec, "a", 0.5, 0.1, Prop::X, going).v == 4);
    CHECK_FALSE(going);
    CHECK(part_prop_value(rec, "a", 0.5, 0.1, Prop::Rot, going).v == 0);
    CHECK_FALSE(going);
    CHECK(part_prop_value(rec, "a", 0.7, 0.1, Prop::Rot, going).v == 10);
    CHECK(going);

    // Not going, y's table goes without rot, rot's track started nothing since: the field is added there.
    REQUIRE(edit_part_prop(rec, "a", 0.2, 0.1, Prop::Rot, number(5), code, src, out, why));
    CHECK(out == with(src, "{ y = 2 }", "{ y = 2, rot = 5 }"));
    PoseScript rs = script_of(out);
    const Recording rr = recorded(rs);
    CHECK(value_at(rr, "a", Prop::Rot, 0.4) == 5);
    CHECK(part_prop_value(rr, "a", 0.2, 0.1, Prop::Rot, going).v == 5);
    CHECK(going);
    REQUIRE(edit_part_prop(rec, "a", 0.2, 0.1, Prop::Stroke, color("#102030"), code, src, out, why));
    CHECK(out == with(src, "{ y = 2 }", "{ y = 2, stroke = \"#102030\" }"));
    // An own value goes into no table: a line of its own.
    REQUIRE(edit_part_prop(rec, "a", 0.2, 0.1, Prop::StrokeWidth, PropValue{}, code, src, out, why));
    CHECK(out == with(src, "local a = part(\"a\")", "at(0.2, part(\"a\").stroke_width(nil, 0))\nlocal a = part(\"a\")"));
    // opacity started since that call (at 0.1): a line of its own, not a field.
    REQUIRE(edit_part_prop(rec, "a", 0.2, 0.1, Prop::Opacity, number(0.25), code, src, out, why));
    CHECK(out == with(src, "local a = part(\"a\")", "at(0.2, part(\"a\").opacity(0.25, 0))\nlocal a = part(\"a\")"));
    PoseScript os = script_of(out);
    const Recording orr = recorded(os);
    CHECK(value_at(orr, "a", Prop::Opacity, 0.2) == 0.25);
    CHECK(value_at(orr, "a", Prop::Opacity, 0.15) == 0.5);

    // A part without a block: a line at the frame, length 0; a frame off the thousandths starts before it.
    REQUIRE(edit_part_prop(rec, "b", 0.25, 0.1, Prop::SkewX, number(10), code, src, out, why));
    CHECK(out == with(src, "local a = part(\"a\")", "at(0.25, part(\"b\").skew_x(10, 0))\nlocal a = part(\"a\")"));
    PoseScript bs = script_of(out);
    const Recording br = recorded(bs);
    CHECK(value_at(br, "b", Prop::SkewX, 0.25) == 10);
    CHECK(value_at(br, "b", Prop::SkewX, 0.2) == 0);
    CHECK(part_prop_value(rec, "b", 0.25, 0.1, Prop::Sx, going).v == 1);
    CHECK_FALSE(going);
    REQUIRE(edit_part_prop(rec, "b", 5.0 / 12, 1.0 / 12, Prop::Sy, number(2), code, src, out, why));
    CHECK(out == with(src, "local a = part(\"a\")", "at(0.416, part(\"b\").sy(2, 0))\nlocal a = part(\"a\")"));

    // Of two tables going, the one that started last; started together, the first by the list.
    const std::string two = clip_of(R"(part("a"):to({ y = 2 }, 0.5)
at(0.1, part("a"):to({ sx = 2 }, 0.5))
at(0, part("b"):to({ sy = 2 }, 0.5))
at(0, part("b"):to({ x = 2 }, 0.5)))");
    PoseScript ts = script_of(two);
    const Recording tr = recorded(ts);
    REQUIRE(edit_part_prop(tr, "a", 0.2, 0.1, Prop::Scale, number(3), ts.code(), two, out, why));
    CHECK(out == with(two, "{ sx = 2 }", "{ sx = 2, scale = 3 }"));
    REQUIRE(edit_part_prop(tr, "b", 0.2, 0.1, Prop::Rot, number(3), ts.code(), two, out, why));
    CHECK(out == with(two, "{ x = 2 }", "{ x = 2, rot = 3 }"));
}

TEST_CASE("Motion fields: what a part's field refuses, and own values in a table") {
    const std::string src = clip_of(R"(local a = part("a")
a.x(function(t) return t end)
a.y(2, 0.1):back(0.2)
part("a"):to({ fill = "#ff0000", sx = 2 }, 0.4))");
    PoseScript s = script_of(src);
    const Recording rec = recorded(s);
    const CodeSites& code = s.code();
    std::string out, why;
    // A binding and a back decide: their words.
    CHECK_FALSE(edit_part_prop(rec, "a", 0.6, 0.1, Prop::X, number(1), code, src, out, why));
    CHECK(why == "значение блока считает функция связи: правьте её в коде");
    CHECK_FALSE(edit_part_prop(rec, "a", 0.2, 0.1, Prop::Y, number(1), code, src, out, why));
    CHECK(why == "блок возвращает туда, откуда начался переход: его значения в коде нет");
    CHECK(out.empty());
    // Own in a table (it goes from 0.3, after y and its back): refused, the field and the block's value alike.
    const std::string own = "своё в таблице { … } не пишется: nil в таблице — нет поля; своё — отдельным вызовом part(\"a\").fill(nil, …)";
    CHECK_FALSE(edit_part_prop(rec, "a", 0.5, 0.1, Prop::Fill, PropValue{}, code, src, out, why));
    CHECK(why == own);
    CHECK_FALSE(edit_value(rec, block(rec, "a", Prop::Fill), Prop::Fill, PropValue{}, code, src, out, why));
    CHECK(why == own);
    CHECK(out.empty());
    REQUIRE(edit_value(rec, block(rec, "a", Prop::Fill), Prop::Fill, color("#0000ff"), code, src, out, why));
    CHECK(out == with(src, "\"#ff0000\"", "\"#0000ff\""));
    // What a field never is: the order, the mask, the link, the drawing.
    CHECK_FALSE(edit_part_prop(rec, "a", 0.2, 0.1, Prop::Order, number(1), code, src, out, why));
    CHECK(why == "порядок частей правится в коде: to_top, to_bottom, above, below");
    CHECK_FALSE(edit_part_prop(rec, "a", 0.2, 0.1, Prop::Drawing, number(1), code, src, out, why));
    CHECK(why == "рисунок правится в коде: swap(…)");

    // A module's block going: its words; not going, the line is the character's own.
    PoseScript m;
    m.set_module_reader([](std::string_view name, std::string& text, std::string& w) {
        if (name != "moves") return w = "нет", false;
        text = "return { lift = function(h) h:to({ y = -3 }, 0.4) end }";
        return true;
    });
    const std::string ms = "local mv = require(\"moves\")\n" + clip_of("mv.lift(part(\"a\"))");
    REQUIRE(m.load(ms));
    const Recording mr = recorded(m);
    CHECK_FALSE(edit_part_prop(mr, "a", 0.2, 0.1, Prop::Y, number(1), m.code(), ms, out, why));
    CHECK(why == "блок из общего модуля «moves.luau»: правка изменит всех персонажей, правьте его в коде");
    REQUIRE(edit_part_prop(mr, "a", 0.2, 0.1, Prop::X, number(1), m.code(), ms, out, why));
    CHECK(out.find("at(0.2, part(\"a\").x(1, 0))") != std::string::npos);
}
