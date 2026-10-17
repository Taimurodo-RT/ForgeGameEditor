// Part 4а of docs/models/движение-модель.md («Свойства»): skew, stroke, stroke width, dash, the share of a line drawn,
// visibility and the order of parts go as x and fill do (recording, frame, picture, timeline, code edits), and the
// draw fields dash, cap and join. A frame of each is the same picture, byte for byte, as the SVG with the property
// written in it, drawn by the same ThorVG; the share drawn, which SVG has no word for, as ThorVG's own trimpath.

#include "forge/pixel/draw.h"
#include "forge/pixel/motion.h"
#include "forge/pixel/motion_edit.h"
#include "forge/pixel/rig.h"
#include "forge/pixel/script.h"
#include "forge/pixel/timeline.h"
#include "forge/vector/trig.h"

#include <doctest/doctest.h>
#include <lunasvg.h>
#include <thorvg.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <ostream> // doctest's INFO prints a string_view through it; MSVC needs the whole class
#include <string>
#include <vector>

using namespace forge;
using namespace forge::pixel;

namespace {

// A body of three shapes over one another (the head with a stroke), a dashed line and a line of two contours. Each element's
// attributes can be added to, and the order of the body's shapes changed: the same picture with a property written.
struct El {
    const char* id;
    const char* text; // the element without its closing "/>"
};
const El kBody[] = {
    {"torso", R"(<rect id="torso" x="20" y="20" width="16" height="20" fill="#3060c0")"},
    {"arm", R"(<rect id="arm" x="16" y="22" width="8" height="14" fill="#c03030")"},
    {"head", R"(<circle id="head" cx="24" cy="18" r="6" fill="#e0b080" stroke="#402000" stroke-width="1.5")"},
};
const El kRest[] = {
    {"line", R"(<path id="line" d="M8 56 L56 56" fill="none" stroke="#000000" stroke-width="2" stroke-dasharray="4 2")"},
    {"twice", R"(<path id="twice" d="M8 48 L28 48 M36 48 L56 48" fill="none" stroke="#008000" stroke-width="2")"},
};

// The SVG with `extra` attributes per id ("body" for the group's own), the body's shapes in `order` (ids), the
// elements in `drop` left out.
std::string picture(const std::map<std::string, std::string>& extra = {}, std::vector<std::string> order = {},
                    const std::vector<std::string>& drop = {}) {
    if (order.empty()) order = {"torso", "arm", "head"};
    auto with = [&](const El& e) {
        if (std::find(drop.begin(), drop.end(), e.id) != drop.end()) return std::string();
        const auto it = extra.find(e.id);
        return "    " + std::string(e.text) + (it != extra.end() ? " " + it->second : std::string()) + "/>\n";
    };
    const auto body = extra.find("body");
    std::string s = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" width="64" height="64">)"
                    "\n  <g id=\"body\"" +
                    (body != extra.end() ? " " + body->second : std::string()) + ">\n";
    for (const std::string& id : order)
        for (const El& e : kBody)
            if (id == e.id) s += with(e);
    s += "  </g>\n";
    for (const El& e : kRest) s += with(e);
    return s + "</svg>";
}

Rig loaded(std::string_view svg) {
    Rig rig;
    std::string why;
    const bool ok = rig.load(svg, why);
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

// A script of one clip, "c", its body from line 2 on.
std::string clip_of(std::string_view body) { return "return { clips = { c = function(p, rng)\n" + std::string(body) + "\nend } }"; }

Recording recorded(PoseScript& s, const Rig* rig) {
    Recording rec;
    const bool ok = s.record(rig, "c", {}, 1, {}, rec);
    INFO(s.error());
    REQUIRE(ok);
    return rec;
}

std::string record_error(PoseScript& s, const Rig* rig) {
    Recording rec;
    CHECK_FALSE(s.record(rig, "c", {}, 1, {}, rec));
    return s.error();
}

Pose frame(PoseScript& s, const Recording& rec, const Rig* rig, f64 t) {
    Pose pose;
    const bool ok = s.frame(rec, rig, t, {}, 1, pose);
    INFO(s.error());
    REQUIRE(ok);
    return pose;
}

const Block& block(const Recording& rec, std::string_view part, Prop p, usize nth = 0) {
    for (const Block& b : rec.blocks)
        if (rec.parts[b.part] == part && b.prop == p && nth-- == 0) return b;
    FAIL("no block ", part, ".", prop_word(p));
    return rec.blocks.front();
}

// The posed rig and the reference drawn alike: smooth at 128 × 128 and baked at 64 × 64, byte for byte.
void same_picture(Rig& posed, const std::string& reference) {
    Rig ref = loaded(reference);
    const assets::CookedTexture a = posed.draw(128, 128), b = ref.draw(128, 128);
    CHECK(b.rgba8 != loaded(picture()).draw(128, 128).rgba8); // the reference shows it: not the picture at rest
    usize off = 0;
    for (usize i = 0; i < a.rgba8.size() && i < b.rgba8.size(); ++i) off += a.rgba8[i] != b.rgba8[i];
    INFO("bytes off: ", off);
    CHECK(a.rgba8 == b.rgba8);
    CHECK(posed.bake(64, 64, {}).rgba8 == ref.bake(64, 64, {}).rgba8);
}

// lunasvg's picture, straight RGBA rows: the second opinion on a matrix.
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

// %.9g of a float: it reads back as the same float.
std::string f32_text(f64 v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.9g", static_cast<f64>(static_cast<f32>(v)));
    return buf;
}

// The SVG drawn by ThorVG as the rig draws it (the picture fitted into w × h), the shape `id` trimmed to from … to:
// ThorVG's trimpath set directly, for the share of a line drawn.
assets::CookedTexture trimmed(const std::string& svg, u32 w, u32 h, const char* id, f32 from, f32 to) {
    const Rig fit = loaded(svg);
    const Affine px = fit.to_pixels(w, h);
    tvg::Initializer::init(0);
    assets::CookedTexture out{w, h, {}};
    {
        tvg::Picture* pic = tvg::Picture::gen();
        pic->ref();
        REQUIRE(pic->load(svg.data(), static_cast<u32>(svg.size()), "svg", nullptr, true) == tvg::Result::Success);
        std::vector<tvg::Paint*> all;
        tvg::Accessor* acc = tvg::Accessor::gen();
        acc->set(pic, [](const tvg::Paint* p, void* data) {
            static_cast<std::vector<tvg::Paint*>*>(data)->push_back(const_cast<tvg::Paint*>(p));
            return true;
        }, &all);
        delete acc;
        REQUIRE(all.size() > 4);
        const tvg::Matrix d = all[3]->transform();
        const Affine doc{d.e11, d.e21, d.e12, d.e22, d.e13, d.e23};
        const Affine m = px * inverse(doc);
        const tvg::Matrix t{static_cast<f32>(m.a), static_cast<f32>(m.c), static_cast<f32>(m.e),
                            static_cast<f32>(m.b), static_cast<f32>(m.d), static_cast<f32>(m.f), 0, 0, 1};
        bool found = false;
        for (tvg::Paint* p : all)
            if (p->id == tvg::Accessor::id(id) && p->type() == tvg::Type::Shape) {
                static_cast<tvg::Shape*>(p)->trimpath(from, to, false);
                found = true;
            }
        REQUIRE(found);
        std::vector<u32> buffer(static_cast<usize>(w) * h, 0);
        tvg::SwCanvas* canvas = tvg::SwCanvas::gen();
        canvas->target(buffer.data(), w, w, h, tvg::ColorSpace::ABGR8888S);
        canvas->add(pic);
        pic->transform(t);
        canvas->update();
        canvas->draw(true);
        canvas->sync();
        const u8* bytes = reinterpret_cast<const u8*>(buffer.data());
        out.rgba8.assign(bytes, bytes + static_cast<usize>(w) * h * 4);
        canvas->remove(pic);
        delete canvas;
        pic->unref();
    }
    tvg::Initializer::term();
    return out;
}

} // namespace

TEST_CASE("Motion draw: the new properties are recorded and read as x and fill are") {
    const Rig rig = loaded(picture());
    PoseScript s = script_of(clip_of(R"(all(
  part("arm").skew_x(30, 1, "linear"),
  part("arm").skew_y(-20, 1, "linear"),
  part("head"):to({ stroke = "#ff0000", stroke_width = 3 }, 1, "linear"),
  part("line").dash(10, 1, "linear"),
  part("twice").draw_to(0.5, 1, "linear"),
  part("twice").draw_from(0.25, 1, "linear"),
  part("arm").visible(false, 1, "out_back")
)
log(part("head").stroke(), part("head").stroke_width(), part("arm").visible(), part("arm").skew_x())
all(part("head").stroke(nil, 1, "linear"), part("head").stroke_width(nil, 1, "linear")))"));
    const Recording rec = recorded(s, &rig);
    CHECK(rec.length == doctest::Approx(2));
    REQUIRE(rec.log.size() == 1);
    CHECK(rec.log[0].text == "#ff0000\t3\tfalse\t30");

    Pose p = frame(s, rec, &rig, 0.5);
    CHECK(p.parts["arm"].skew_x == doctest::Approx(15));
    CHECK(p.parts["arm"].skew_y == doctest::Approx(-10));
    CHECK(p.parts["head"].has_stroke);
    CHECK(p.parts["head"].stroke == Rgba8{255, 0, 0, 255});
    CHECK(p.parts["head"].stroke_k == doctest::Approx(0.5));
    CHECK(p.parts["head"].has_stroke_width);
    CHECK(p.parts["head"].stroke_width == doctest::Approx(3)); // n is where it goes; k how far from each shape's own
    CHECK(p.parts["head"].stroke_width_k == doctest::Approx(0.5));
    CHECK(p.parts["line"].dash == doctest::Approx(5));
    CHECK(p.parts["twice"].draw_to == doctest::Approx(0.75));
    CHECK(p.parts["twice"].draw_from == doctest::Approx(0.125));
    // Visible changes at the end of its block, whatever its timing does on the way (out_back passes 1 early).
    CHECK(p.parts["arm"].visible);
    CHECK(frame(s, rec, &rig, 0.999).parts["arm"].visible);
    p = frame(s, rec, &rig, 1);
    CHECK_FALSE(p.parts["arm"].visible);
    CHECK(p.parts["arm"].skew_x == 30);
    CHECK(p.parts["head"].stroke_k == 1);
    CHECK(p.parts["head"].stroke_width == 3);
    // Back to its own: nil goes from the colour and the width given to each shape's own.
    p = frame(s, rec, &rig, 1.5);
    CHECK(p.parts["head"].has_stroke);
    CHECK(p.parts["head"].stroke_k == doctest::Approx(0.5));
    CHECK(p.parts["head"].stroke_width_k == doctest::Approx(0.5));
    p = frame(s, rec, &rig, 2);
    CHECK_FALSE(p.parts["head"].has_stroke);
    CHECK_FALSE(p.parts["head"].has_stroke_width);
    // The rest of a property: what the frame has for a part without its track.
    CHECK(p.parts["torso"].visible);
    CHECK(p.parts["torso"].draw_to == 1);
    CHECK(p.order.empty());

    // The block words and the value words of the timeline.
    CHECK(block_words(rec, block(rec, "arm", Prop::Visible)) == "visible → нет");
    CHECK(block_words(rec, block(rec, "head", Prop::Stroke)) == "stroke → #ff0000");
    CHECK(block_words(rec, block(rec, "head", Prop::StrokeWidth)) == "stroke_width → 3");
    CHECK(block_words(rec, block(rec, "head", Prop::Stroke, 1)) == "stroke → свой цвет");
    CHECK(block_words(rec, block(rec, "head", Prop::StrokeWidth, 1)) == "stroke_width → своя толщина");
    CHECK(block_words(rec, block(rec, "arm", Prop::SkewX)) == "skew_x → 30");
    CHECK(block_words(rec, block(rec, "twice", Prop::DrawTo)) == "draw_to → 0.5");
}

TEST_CASE("Motion draw: overshoot, springs, save and restore, rest, bindings, tween") {
    const Rig rig = loaded(picture());
    // Between two widths given, as numbers: back passes them. From its own, it stays between, as a colour.
    PoseScript s = script_of(clip_of(R"(part("head").stroke_width(2)
all(part("head").stroke_width(4, 1, "in_back"), part("torso").stroke_width(4, 1, "in_back")))"));
    const Recording rec = recorded(s, &rig);
    Pose p = frame(s, rec, &rig, 0.3);
    CHECK(p.parts["head"].stroke_width_k == 1);
    CHECK(p.parts["head"].stroke_width < 2); // in_back first goes back
    CHECK(p.parts["torso"].stroke_width_k == 0); // clamped at its own
    CHECK(frame(s, rec, &rig, 0.9).parts["torso"].stroke_width_k > 0);

    // Springs of the new numbers; none of a colour, a width or visible.
    PoseScript spring = script_of(clip_of(R"(part("arm").skew_x:spring(20, "plop"))"));
    const Recording sr = recorded(spring, &rig);
    CHECK(block(sr, "arm", Prop::SkewX).spring);
    PoseScript bad = script_of(clip_of(R"(part("arm").visible:spring(false))"));
    CHECK(record_error(bad, &rig) ==
          "строка 2: пружина — только у чисел: x, y, rot, scale, sx, sy, skew_x, skew_y, opacity, dash, draw_from, draw_to");

    // save and restore, rest: the new properties go back too; the order is no value to go back to.
    PoseScript back = script_of(clip_of(R"(part("arm"):save()
part("arm"):to({ skew_x = 10, stroke = "#00ff00", visible = false, dash = 3 }, 0.5)
part("arm"):to_top()
part("arm"):restore(0.5)
part("head"):to({ draw_from = 0.5, visible = false }, 0.2)
part("head"):rest(0.2))"));
    const Recording br = recorded(back, &rig);
    Pose q = frame(back, br, &rig, 0.5);
    CHECK(q.parts["arm"].skew_x == 10);
    CHECK_FALSE(q.parts["arm"].visible);
    q = frame(back, br, &rig, 1);
    CHECK(q.parts["arm"].skew_x == 0);
    CHECK_FALSE(q.parts["arm"].has_stroke);
    CHECK(q.parts["arm"].visible);
    CHECK(q.parts["arm"].dash == 0);
    REQUIRE(q.order.size() == 1); // restore leaves the order as it is
    CHECK(q.order[0] == OrderStep{"arm", OrderStep::Kind::Top, ""});
    q = frame(back, br, &rig, 1.4);
    CHECK(q.parts["head"].draw_from == 0);
    CHECK(q.parts["head"].visible);
    for (const Block& b : br.blocks) CHECK_FALSE((b.to == Block::To::Saved && b.prop == Prop::Order));
    for (const Block& b : br.blocks) CHECK_FALSE((b.to == Block::To::Rest && b.prop == Prop::Order));

    // Bindings and tween give them as values: a width, a colour, true or false.
    PoseScript bind = script_of(clip_of(R"(part("head").stroke_width(function() return now() * 4 end)
part("head").stroke(function() return now() < 0.5 and "#0000ff" or nil end)
part("arm").visible(function() return now() < 0.5 end)
tween(1, function(k) part("torso").visible(k < 0.5) part("torso").draw_to(1 - k) end))"));
    const Recording bdr = recorded(bind, &rig);
    q = frame(bind, bdr, &rig, 0.25);
    CHECK(q.parts["head"].stroke_width == doctest::Approx(1));
    CHECK(q.parts["head"].stroke == Rgba8{0, 0, 255, 255});
    CHECK(q.parts["arm"].visible);
    CHECK(q.parts["torso"].visible);
    CHECK(q.parts["torso"].draw_to == doctest::Approx(0.75));
    q = frame(bind, bdr, &rig, 0.75);
    CHECK_FALSE(q.parts["head"].has_stroke);
    CHECK_FALSE(q.parts["arm"].visible);
    CHECK_FALSE(q.parts["torso"].visible);
    PoseScript wrong = script_of(clip_of(R"(part("arm").visible(function() return 1 end))"));
    const Recording wr = recorded(wrong, &rig);
    Pose none;
    CHECK_FALSE(wrong.frame(wr, &rig, 0.1, {}, 1, none));
    CHECK(wrong.error().find("связь visible вернула не true и не false") != std::string::npos);

    // What is not a value of a property is named.
    auto error = [&](const char* body) {
        PoseScript e = script_of(clip_of(body));
        return record_error(e, &rig);
    };
    CHECK(error(R"(part("arm").visible(1, 0.1))") == "строка 2: visible: нужно true или false, а не number");
    CHECK(error(R"(part("arm").stroke_width(-1, 0.1))") == "строка 2: stroke_width: нужно число не меньше 0 или nil (своя толщина)");
    CHECK(error(R"(part("arm").stroke("teal-ish", 0.1))") == "строка 2: stroke: нужен цвет \"#rrggbb\"");
    CHECK(error(R"(part("arm"):to({ order = 1 }, 0.1))") ==
          "строка 2: part:to: поля «order» нет (есть x, y, rot, scale, sx, sy, skew_x, skew_y, opacity, fill, stroke, stroke_width, "
          "dash, draw_from, draw_to, visible)");
}

TEST_CASE("Motion draw: widths and colours mixed from each shape's own") {
    // A width given, then its own: for any own width w the mix is the shapes' widths mixed, (1 − k)·2 + k·w.
    PropValue given;
    given.v = 1, given.n = 2;
    const PropValue own = rest_value(Prop::StrokeWidth);
    for (f64 k : {0.25, 0.5, 0.8}) {
        const PropValue m = mix(Prop::StrokeWidth, given, own, k);
        for (f64 w : {0.0, 1.5, 7.0}) CHECK(w * (1 - m.v) + m.n * m.v == doctest::Approx((1 - k) * 2 + k * w));
    }
    // From its own, the share stays in 0 … 1 (a colour's rule); two given go as numbers, past the ends too.
    CHECK(mix(Prop::StrokeWidth, own, given, 1.2).v == 1);
    CHECK(mix(Prop::StrokeWidth, own, given, -0.2).v == 0);
    PropValue four;
    four.v = 1, four.n = 4;
    CHECK(mix(Prop::StrokeWidth, given, four, 1.5).n == doctest::Approx(5));
    CHECK(mix(Prop::StrokeWidth, given, four, -0.5).n == doctest::Approx(1));
    // Stroke as fill; visible and the order without values between.
    PropValue red;
    red.v = 1, red.color = {255, 0, 0, 255};
    CHECK(mix(Prop::Stroke, rest_value(Prop::Stroke), red, 0.5).v == doctest::Approx(0.5));
    CHECK(mix(Prop::Stroke, rest_value(Prop::Stroke), red, 0.5).color == Rgba8{255, 0, 0, 255});
    PropValue off;
    off.v = 0;
    CHECK(mix(Prop::Visible, rest_value(Prop::Visible), off, 0.99).v == 1);
    CHECK(mix(Prop::Visible, rest_value(Prop::Visible), off, 1.2).v == 0);
    CHECK(rest_value(Prop::DrawTo).v == 1);
    CHECK(rest_value(Prop::DrawFrom).v == 0);
    CHECK(rest_value(Prop::Visible).v == 1);
    CHECK(prop_is_number(Prop::SkewX));
    CHECK(prop_is_number(Prop::Dash));
    CHECK_FALSE(prop_is_number(Prop::StrokeWidth));
    CHECK_FALSE(prop_is_number(Prop::Visible));
    CHECK_FALSE(prop_is_number(Prop::Order));
    Prop p;
    CHECK(prop_from_word("draw_from", p));
    CHECK(p == Prop::DrawFrom);
    CHECK(std::string(prop_word(Prop::Order)) == "порядок");
}

TEST_CASE("Motion draw: the order of parts, in the flow, among neighbours") {
    const Rig rig = loaded(picture());
    PoseScript s = script_of(clip_of(R"(part("arm"):to_top()
part("head"):to_top()
wait(0.5)
part("arm"):below("torso")
part("head"):above("torso")
wait(0.5)
part("torso"):to_bottom())"));
    const Recording rec = recorded(s, &rig);
    CHECK(block(rec, "arm", Prop::Order).length == 0);
    CHECK(block_words(rec, block(rec, "arm", Prop::Order)) == "наверх");
    CHECK(block_words(rec, block(rec, "arm", Prop::Order, 1)) == "под torso");
    CHECK(block_words(rec, block(rec, "head", Prop::Order, 1)) == "над torso");
    CHECK(block_words(rec, block(rec, "torso", Prop::Order)) == "вниз");
    using K = OrderStep::Kind;
    // Every call up to t, by time, then as recorded.
    Pose p = frame(s, rec, &rig, 0);
    CHECK(p.order == std::vector<OrderStep>{{"arm", K::Top, ""}, {"head", K::Top, ""}});
    p = frame(s, rec, &rig, 0.7);
    CHECK(p.order == std::vector<OrderStep>{{"arm", K::Top, ""}, {"head", K::Top, ""}, {"arm", K::Below, "torso"}, {"head", K::Above, "torso"}});
    CHECK(frame(s, rec, &rig, 1).order.size() == 5);
    // Recorded one way, timed the other: all() records its first task whole before the second, yet the head's call at
    // 0.25 goes before the arm's at 0.5.
    PoseScript late = script_of(clip_of(R"(all(delay(0.5, part("arm"):to_top()), delay(0.25, part("head"):to_top())))"));
    const Recording later = recorded(late, &rig);
    REQUIRE(later.blocks.size() == 2);
    CHECK(later.parts[later.blocks[0].part] == "arm"); // the premise: the record has them the other way
    CHECK(frame(late, later, &rig, 0.3).order == std::vector<OrderStep>{{"head", K::Top, ""}});
    CHECK(frame(late, later, &rig, 0.7).order == std::vector<OrderStep>{{"head", K::Top, ""}, {"arm", K::Top, ""}});

    // The order is the code's: no field edits it, a chain does not go on from it.
    std::string out, why;
    CHECK_FALSE(can_edit(rec, block(rec, "arm", Prop::Order, 1), BlockEdit::Value, s.code(), why));
    CHECK(why == "порядок частей правится в коде: to_top, to_bottom, above, below");
    auto error = [&](const char* body) {
        PoseScript e = script_of(clip_of(body));
        return record_error(e, &rig);
    };
    CHECK(error(R"(part("arm"):above("line"))") == "строка 2: arm и line в разных группах: порядок меняется только среди соседей");
    CHECK(error(R"(part("arm"):below("tail"))") == "строка 2: в рисунке нет части «tail»");
    CHECK(error(R"(part("arm"):above("arm"))") == "строка 2: above: часть «arm» рядом с самой собой не поставить");
    CHECK(error(R"(part("arm"):to_top(1))") == "строка 2: to_top() — без аргументов");
    CHECK(error(R"(part("arm"):to_top():wait(1))") ==
          "строка 2: wait — не после to_top, to_bottom, above и below: у порядка нет длины, следующий переход — отдельной строкой");
}

TEST_CASE("Motion draw: a frame of each property is the SVG with it written, byte for byte") {
    Rig rig = loaded(picture());
    std::string why;
    const assets::CookedTexture rest = rig.draw(128, 128);
    const assets::CookedTexture rest_baked = rig.bake(64, 64, {});

    SUBCASE("skew: the matrix of SVG's skewX and skewY after the turn and the scale") {
        rig.set_pivot("arm", {17, 29});
        Pose pose;
        PartPose& arm = pose.parts["arm"];
        arm.x = 2, arm.rot = 15, arm.scale = 1.2, arm.skew_x = 20, arm.skew_y = -10;
        REQUIRE(rig.pose(pose, why));
        const Affine m = rig.matrix(rig.part("arm"));
        // translate(x + pivot) rotate scale skewX skewY translate(−pivot), composed here apart from the rig.
        const f64 r = 15 * vector::trig::kPi / 180, tx = std::tan(20 * vector::trig::kPi / 180), ty = std::tan(-10 * vector::trig::kPi / 180);
        const Affine expect = translation(2 + 17, 29) * Affine{std::cos(r), std::sin(r), -std::sin(r), std::cos(r), 0, 0} *
                              Affine{1.2, 0, 0, 1.2, 0, 0} * Affine{1, 0, tx, 1, 0, 0} * Affine{1, ty, 0, 1, 0, 0} * translation(-17, -29);
        for (auto [got, want] : {std::pair{m.a, expect.a}, {m.b, expect.b}, {m.c, expect.c}, {m.d, expect.d}, {m.e, expect.e}, {m.f, expect.f}})
            CHECK(got == doctest::Approx(want).epsilon(1e-6));
        const std::string written = "transform=\"matrix(" + f32_text(m.a) + " " + f32_text(m.b) + " " + f32_text(m.c) + " " +
                                    f32_text(m.d) + " " + f32_text(m.e) + " " + f32_text(m.f) + ")\"";
        same_picture(rig, picture({{"arm", written}}));
        // lunasvg, with the skews written as SVG writes them: the same picture give or take its edges.
        const std::string svg_words = "transform=\"translate(19 29) rotate(15) scale(1.2) skewX(20) skewY(-10) translate(-17 -29)\"";
        CHECK(disagree(rig.draw(128, 128), luna(picture({{"arm", svg_words}}), 128, 128)) <= 3);
        const std::string the_other_way = "transform=\"translate(19 29) rotate(15) scale(1.2) skewY(-10) skewX(20) translate(-17 -29)\"";
        CHECK(disagree(rig.draw(128, 128), luna(picture({{"arm", the_other_way}}), 128, 128)) > 3);
        // Past 89° it stays at 89°.
        Pose far = pose;
        far.parts["arm"].skew_x = 120;
        REQUIRE(rig.pose(far, why));
        const Affine held = rig.matrix(rig.part("arm"));
        far.parts["arm"].skew_x = kMaxSkew;
        REQUIRE(rig.pose(far, why));
        CHECK(rig.matrix(rig.part("arm")) == held);
    }
    SUBCASE("stroke: its colour on each shape, a shape without one getting it at SVG's width") {
        Pose pose;
        pose.parts["head"].has_stroke = true;
        pose.parts["head"].stroke = {255, 0, 0, 255};
        pose.parts["torso"].has_stroke = true;
        pose.parts["torso"].stroke = {255, 0, 0, 255};
        REQUIRE(rig.pose(pose, why));
        same_picture(rig, picture({{"head", "stroke=\"#ff0000\""}, {"torso", "stroke=\"#ff0000\""}}));
        // Half way: the colour between, 0x40 → 0xff is 160.
        pose.parts["head"].stroke_k = 0.5;
        pose.parts.erase("torso");
        REQUIRE(rig.pose(pose, why));
        same_picture(rig, picture({{"head", "stroke=\"rgb(160,16,0)\""}}));
        // A shape without one, half way: the colour itself, coming in from transparent, 255 · ½ is 128.
        pose.parts.erase("head");
        pose.parts["torso"].has_stroke = true;
        pose.parts["torso"].stroke = {255, 0, 0, 255};
        pose.parts["torso"].stroke_k = 0.5;
        REQUIRE(rig.pose(pose, why));
        same_picture(rig, picture({{"torso", "stroke=\"#ff0000\" stroke-opacity=\"0.502\""}}));
    }
    SUBCASE("stroke width: from each shape's own, the group's shapes each") {
        Pose pose;
        pose.parts["head"].has_stroke_width = true;
        pose.parts["head"].stroke_width = 3;
        pose.parts["head"].stroke_width_k = 0.5; // 1.5 → 3: 2.25
        REQUIRE(rig.pose(pose, why));
        same_picture(rig, picture({{"head", "stroke-width=\"2.25\""}}));
        Pose group;
        group.parts["body"].has_stroke_width = true;
        group.parts["body"].stroke_width = 2.5;
        group.parts["body"].has_stroke = true;
        group.parts["body"].stroke = {0, 0, 0, 255};
        REQUIRE(rig.pose(group, why));
        same_picture(rig, picture({{"torso", "stroke=\"#000000\" stroke-width=\"2.5\""},
                                   {"arm", "stroke=\"#000000\" stroke-width=\"2.5\""},
                                   {"head", "stroke=\"#000000\" stroke-width=\"2.5\""}}));
        // The group's width half way, a stroke given to the torso: the torso's own width is SVG's 1, not the 0 it
        // drew, 1 → 3 is 2; the head's 1.5 → 3 is 2.25; the arm, given no stroke, draws none.
        Pose half;
        half.parts["body"].has_stroke_width = true;
        half.parts["body"].stroke_width = 3;
        half.parts["body"].stroke_width_k = 0.5;
        half.parts["torso"].has_stroke = true;
        half.parts["torso"].stroke = {0, 0, 0, 255};
        REQUIRE(rig.pose(half, why));
        same_picture(rig, picture({{"torso", "stroke=\"#000000\" stroke-width=\"2\""}, {"head", "stroke-width=\"2.25\""}}));
        // Below 0, as two widths overshooting with back: none.
        Pose below;
        below.parts["head"].has_stroke_width = true;
        below.parts["head"].stroke_width = -0.5;
        below.parts["head"].stroke_width_k = 1;
        REQUIRE(rig.pose(below, why));
        same_picture(rig, picture({{"head", "stroke-width=\"0\""}}));
    }
    SUBCASE("dash: the pattern moved along the line; a shape without one as it was") {
        Pose pose;
        pose.parts["line"].dash = 3;
        pose.parts["torso"].dash = 3;
        REQUIRE(rig.pose(pose, why));
        same_picture(rig, picture({{"line", "stroke-dashoffset=\"3\""}}));
        CHECK(rig.dashed(rig.part("line")));
        CHECK_FALSE(rig.dashed(rig.part("torso")));
        CHECK_FALSE(rig.dashed(rig.part("body")));
    }
    SUBCASE("the share drawn: ThorVG's trimpath, the contours one line after another") {
        Pose pose;
        pose.parts["twice"].draw_from = 0.25;
        pose.parts["twice"].draw_to = 0.75;
        REQUIRE(rig.pose(pose, why));
        CHECK(rig.draw(128, 128).rgba8 == trimmed(picture(), 128, 128, "twice", 0.25f, 0.75f).rgba8);
        // Half of the length is the first contour whole: the line of two as one.
        pose.parts["twice"].draw_from = 0;
        pose.parts["twice"].draw_to = 0.5;
        REQUIRE(rig.pose(pose, why));
        CHECK(rig.draw(128, 128).rgba8 == trimmed(picture(), 128, 128, "twice", 0, 0.5f).rgba8);
        CHECK(disagree(rig.draw(128, 128), loaded(picture({{"twice", "d=\"M8 48 L28 48\""}})).draw(128, 128)) <= 3);
        // From at or past to: nothing of it, not the rest of it wrapped round.
        pose.parts["twice"].draw_from = 0.6;
        pose.parts["twice"].draw_to = 0.4;
        REQUIRE(rig.pose(pose, why));
        same_picture(rig, picture({}, {}, {"twice"}));
        // A fill is cut too: ThorVG trims the path it fills.
        Pose fill;
        fill.parts["torso"].draw_to = 0.5;
        REQUIRE(rig.pose(fill, why));
        CHECK(rig.draw(128, 128).rgba8 == trimmed(picture(), 128, 128, "torso", 0, 0.5f).rgba8);
        CHECK(rig.draw(128, 128).rgba8 != rest.rgba8);
    }
    SUBCASE("visible: a part hidden with all in it, as display none") {
        Pose pose;
        pose.parts["arm"].visible = false;
        REQUIRE(rig.pose(pose, why));
        same_picture(rig, picture({{"arm", "display=\"none\""}}));
        Pose group;
        group.parts["body"].visible = false;
        REQUIRE(rig.pose(group, why));
        same_picture(rig, picture({{"body", "display=\"none\""}}));
    }
    SUBCASE("the order: as the elements written in that order") {
        Pose pose;
        pose.order = {{"arm", OrderStep::Kind::Top, ""}};
        REQUIRE(rig.pose(pose, why));
        same_picture(rig, picture({}, {"torso", "head", "arm"}));
        pose.order = {{"head", OrderStep::Kind::Bottom, ""}, {"arm", OrderStep::Kind::Below, "torso"}};
        REQUIRE(rig.pose(pose, why));
        same_picture(rig, picture({}, {"head", "arm", "torso"}));
        pose.order = {{"torso", OrderStep::Kind::Above, "head"}, {"torso", OrderStep::Kind::Above, "head"}};
        REQUIRE(rig.pose(pose, why));
        same_picture(rig, picture({}, {"arm", "head", "torso"}));
        // Two calls at once: as recorded.
        PoseScript s = script_of(clip_of(R"(part("torso"):to_top()
part("arm"):to_top())"));
        const Recording rec = recorded(s, &rig);
        REQUIRE(rig.pose(frame(s, rec, &rig, 0), why));
        same_picture(rig, picture({}, {"head", "torso", "arm"}));
        // What cannot be done is named, the rest done.
        Pose bad;
        bad.order = {{"arm", OrderStep::Kind::Above, "line"}, {"tail", OrderStep::Kind::Top, ""}, {"head", OrderStep::Kind::Bottom, ""}};
        CHECK_FALSE(rig.pose(bad, why));
        CHECK(why == "arm и line в разных группах: порядок меняется только среди соседей");
        same_picture(rig, picture({}, {"head", "torso", "arm"}));
    }
    // And back to rest: exactly the first picture, smooth and baked.
    REQUIRE(rig.pose({}, why));
    CHECK(rig.draw(128, 128).rgba8 == rest.rgba8);
    CHECK(rig.bake(64, 64, {}).rgba8 == rest_baked.rgba8);
}

TEST_CASE("Motion draw: a stroke given to a shape whose own is clear is taken back") {
    // stroke-opacity 0: the shape has a width and a clear colour. A stroke given shows at that width; the rest after it
    // is the picture at rest again, the clear colour put back.
    Rig rig = loaded(R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32" width="32" height="32">
  <rect id="box" x="8" y="8" width="16" height="16" fill="#3060c0" stroke="#ff0000" stroke-opacity="0" stroke-width="4"/>
</svg>)");
    std::string why;
    const assets::CookedTexture rest = rig.draw(64, 64);
    Pose given;
    given.parts["box"].has_stroke = true;
    given.parts["box"].stroke = {0, 0, 0, 255};
    REQUIRE(rig.pose(given, why));
    CHECK(rig.draw(64, 64).rgba8 != rest.rgba8);
    REQUIRE(rig.pose({}, why));
    CHECK(rig.draw(64, 64).rgba8 == rest.rgba8);
}

TEST_CASE("Motion draw: a pose of the new numbers that are no numbers is named") {
    Rig rig = loaded(picture());
    std::string why;
    for (int i = 0; i < 6; ++i) {
        Pose pose;
        PartPose& pp = pose.parts["head"];
        f64* field[] = {&pp.skew_x, &pp.skew_y, &pp.stroke_width, &pp.dash, &pp.draw_from, &pp.draw_to};
        *field[i] = std::nan("");
        CHECK_FALSE(rig.pose(pose, why));
        CHECK(why == "часть «head»: не число в позе");
    }
}

TEST_CASE("Motion draw: what the recording warns of, once a part") {
    const Rig rig = loaded(picture());
    PoseScript s = script_of(clip_of(R"(part("arm").skew_x(120, 0.5)
part("arm").skew_y(-150, 0.5)
part("head").skew_x(85, 1, "out_back")
part("torso").skew_x(80, 1)
part("torso").dash(5, 1)
part("line").dash(5, 1))"));
    const Recording rec = recorded(s, &rig);
    REQUIRE(rec.notes.size() == 3);
    CHECK(rec.notes[0].text == "часть «arm»: skew_x доходит до 120°, а наклон держится в −89…89°: дальше кадр не наклоняет");
    CHECK(rec.notes[0].line == 2);
    CHECK(rec.notes[1].text.rfind("часть «head»: skew_x доходит до 9", 0) == 0); // out_back passes 85 on the way
    CHECK(rec.notes[1].line == 4);
    CHECK(rec.notes[2].text == "часть «torso»: dash, а пунктира у её фигур нет");
    CHECK(rec.notes[2].line == 6);
    // Without the rig, no word of the dash: nothing to look at.
    PoseScript d = script_of(clip_of(R"(part("torso").dash(5, 1))"));
    CHECK(recorded(d, nullptr).notes.empty());
}

TEST_CASE("Motion draw: dash, cap and join drawn by the code") {
    PoseScript s = script_of(R"(return {
  clips = { stand = function(p) wait(0.1) end },
  draw = function(p, rng)
    canvas(32, 32)
    path("line", { d = "M 2 16 L 30 16", stroke = "#000000", stroke_width = 2, dash = { 4, 2 }, cap = "round", join = "bevel" })
    group("g", { dash = { 1 } }, function() rect("r", { x = 1, y = 1, w = 4, h = 4 }) end)
  end,
})");
    Drawing d;
    const bool ok = s.draw({}, 1, d);
    INFO(s.error());
    REQUIRE(ok);
    const std::string svg = drawing_svg(d, 32, 32);
    CHECK(svg.find(R"(<path id="line" d="M 2 16 L 30 16" stroke="#000000" stroke-width="2" stroke-dasharray="4 2" stroke-linecap="round" stroke-linejoin="bevel"/>)") !=
          std::string::npos);
    CHECK(svg.find(R"(<g id="g" stroke-dasharray="1">)") != std::string::npos);
    // The rig reads them: the line has a dash to move.
    const Rig rig = loaded(svg);
    CHECK(rig.dashed(rig.part("line")));
    CHECK(rig.dashed(rig.part("g"))); // the group's dash is its shapes'

    auto error = [](const std::string& field) {
        PoseScript e = script_of("return {\n  clips = { stand = function(p) wait(0.1) end },\n  draw = function(p, rng)\n    rect(\"a\", { x = 1, y = 1, w = 2, h = 2, " + field + " })\n  end,\n}");
        Drawing out;
        CHECK_FALSE(e.draw({}, 1, out));
        return e.error();
    };
    CHECK(error("dash = {}") == "строка 4: rect «a»: dash — длины штрихов и промежутков, числа не меньше 0 и не все 0: dash = { 4, 2 }");
    CHECK(error("dash = { 0, 0 }") == "строка 4: rect «a»: dash — длины штрихов и промежутков, числа не меньше 0 и не все 0: dash = { 4, 2 }");
    CHECK(error("dash = { 4, -1 }") == "строка 4: rect «a»: dash — длины штрихов и промежутков, числа не меньше 0 и не все 0: dash = { 4, 2 }");
    CHECK(error("cap = \"flat\"") == "строка 4: rect «a»: cap — \"butt\", \"round\", \"square\", а не «flat»");
    CHECK(error("join = 3") == "строка 4: rect «a»: join — \"miter\", \"round\", \"bevel\"");
}

TEST_CASE("Motion draw: a new property's value written into its call") {
    const Rig rig = loaded(picture());
    const std::string code = clip_of(R"(part("head").stroke("#ff0000", 0.5)
part("head"):to({ stroke_width = 3, skew_x = 10 }, 0.5)
part("arm").visible(false, 0.2)
part("head").stroke_width(nil, 0.3))");
    PoseScript s = script_of(code);
    const Recording rec = recorded(s, &rig);
    std::string out, why;
    PropValue green;
    green.v = 1, green.color = {0, 255, 0, 255};
    REQUIRE(edit_value(rec, block(rec, "head", Prop::Stroke), Prop::Stroke, green, s.code(), code, out, why));
    CHECK(out.find(R"(part("head").stroke("#00ff00", 0.5))") != std::string::npos);
    PropValue four;
    four.v = 1, four.n = 4;
    REQUIRE(edit_value(rec, block(rec, "head", Prop::StrokeWidth), Prop::StrokeWidth, four, s.code(), code, out, why));
    CHECK(out.find("{ stroke_width = 4, skew_x = 10 }") != std::string::npos);
    PropValue on;
    on.v = 1;
    REQUIRE(edit_value(rec, block(rec, "arm", Prop::Visible), Prop::Visible, on, s.code(), code, out, why));
    CHECK(out.find(R"(part("arm").visible(true, 0.2))") != std::string::npos);
    PropValue two;
    two.v = 1, two.n = 2;
    REQUIRE(edit_value(rec, block(rec, "head", Prop::StrokeWidth, 1), Prop::StrokeWidth, two, s.code(), code, out, why));
    CHECK(out.find(R"(part("head").stroke_width(2, 0.3))") != std::string::npos);
    // Its own again: nil written.
    REQUIRE(edit_value(rec, block(rec, "head", Prop::Stroke), Prop::Stroke, rest_value(Prop::Stroke), s.code(), code, out, why));
    CHECK(out.find(R"(part("head").stroke(nil, 0.5))") != std::string::npos);
    // The edited code records the value written.
    REQUIRE(edit_value(rec, block(rec, "head", Prop::StrokeWidth), Prop::StrokeWidth, four, s.code(), code, out, why));
    PoseScript again = script_of(out);
    const Recording rec2 = recorded(again, &rig);
    CHECK(block(rec2, "head", Prop::StrokeWidth).value.n == 4);
    CHECK(block(rec2, "head", Prop::SkewX).value.v == 10);
}
