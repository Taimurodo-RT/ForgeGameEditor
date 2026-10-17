// Part 4д of docs/models/движение-модель.md («Смена рисунка»): drawings = { angry = function(p, rng) … end } next to
// draw, and swap("angry", 0.4, "smooth") in a clip. Before the swap the frame is the main drawing drawn alone with the
// clip's pose, after it the other one, byte for byte, smooth and baked. In the middle a shape with a pair morphs as
// morph does between the pair written in one SVG, and a part without one is that part faded by 1 − k or k.

#include "forge/pixel/bake.h"
#include "forge/pixel/doc.h"
#include "forge/pixel/draw.h"
#include "forge/pixel/motion.h"
#include "forge/pixel/motion_edit.h"
#include "forge/pixel/rig.h"
#include "forge/pixel/script.h"
#include "forge/pixel/timeline.h"
#include "forge/vector/path.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <ostream> // doctest's INFO prints a string_view through it; MSVC needs the whole class
#include <string>
#include <vector>

using namespace forge;
using namespace forge::pixel;

namespace {

// The hedgehog: a face of a head and a mouth (and a hidden open mouth to morph into), a nose and an unnamed patch of
// grass. «angry»: the face moved and twice as big, a brow and an unnamed square, no nose. «sleepy»: the face lower.
const char* kMain = R"(    canvas(64, 64)
    group("face", { y = 2 }, function()
      circle("head", { x = 32, y = 28, r = 14, fill = "#c09060", stroke = "#402010", stroke_width = 1.5 })
      path("mouth", { d = "M 26 34 Q 32 38 38 34", fill = "none", stroke = "#402010", stroke_width = 1.5 })
      path("mouth_o", { d = "M 27 33 Q 32 40 37 33 Z", fill = "#802020", hidden = true })
    end)
    rect("nose", { x = 30, y = 26, w = 4, h = 3, fill = "#a06040" })
    ellipse({ x = 12, y = 57, rx = 6, ry = 3, fill = "#406020" }))";
const char* kAngry = R"(      canvas(64, 64)
      group("face", { x = 1, scale = 2 }, function()
        path("head", { d = "M 15.5 1 C 23 1 23 1 30 14 C 23 27 8 27 1 14 C 8 1 8 1 15.5 1 Z", fill = "#e06040", stroke = "#401010", stroke_width = 0.75 })
        path("mouth", { d = "M 13 19 Q 15.5 16 18 19", fill = "none", stroke = "#401010", stroke_width = 0.75 })
      end)
      path("brow", { d = "M 22 18 L 30 22 M 42 18 L 34 22", fill = "none", stroke = "#401010", stroke_width = 2, pivot = { 32, 20 } })
      rect({ x = 50, y = 52, w = 8, h = 6, fill = "#2040a0" }))";
const char* kSleepy = R"(      canvas(64, 64)
      group("face", { y = 4 }, function()
        circle("head", { x = 32, y = 28, r = 13, fill = "#a08070" })
        path("mouth", { d = "M 28 36 L 36 36", fill = "none", stroke = "#402010" })
      end)
      rect("nose", { x = 30, y = 26, w = 4, h = 2, fill = "#806050" }))";

// A character's code: the clip "c" from line 3 on, the main drawing, the other drawings as written.
std::string code(std::string_view clip, std::string_view draw = kMain, std::string_view drawings = "") {
    return "return {\n  clips = { c = function(p, rng)\n" + std::string(clip) + "\n  end },\n  draw = function(p, rng)\n" + std::string(draw) +
           "\n  end,\n" + std::string(drawings) + "}\n";
}

std::string drawings_of(std::string_view angry = kAngry, std::string_view sleepy = kSleepy) {
    std::string s = "  drawings = {\n";
    if (!angry.empty()) s += "    angry = function(p, rng)\n" + std::string(angry) + "\n    end,\n";
    if (!sleepy.empty()) s += "    sleepy = function(p, rng)\n" + std::string(sleepy) + "\n    end,\n";
    return s + "  },\n";
}

std::string hedgehog(std::string_view clip) { return code(clip, kMain, drawings_of()); }

PoseScript script_of(std::string_view source) {
    PoseScript s;
    const bool ok = s.load(source);
    INFO(s.error());
    REQUIRE(ok);
    return s;
}

PixelDoc doc64() {
    PixelDoc doc;
    doc.frame_w = doc.frame_h = 64;
    return doc;
}

// The character as the tab makes it: its main drawing loaded, the others put on it, the pivots.
struct Character {
    PoseScript script;
    Drawing main;
    std::vector<NamedDrawing> others;
    Rig rig;

    explicit Character(std::string_view source) {
        bool ok = script.load(source);
        INFO(script.error());
        REQUIRE(ok);
        REQUIRE(script.draw({}, 1, main));
        std::string why;
        u32 line = 0;
        ok = draw_drawings(script, doc64(), others, why, line);
        INFO(why);
        REQUIRE(ok);
        ok = rig.load(drawing_svg(main, 64, 64), why) && add_drawings(rig, others, why, line);
        INFO(why);
        REQUIRE(ok);
        CHECK(apply_pivots(rig, doc64(), &main, &others).empty());
    }

    const Drawing& drawing(std::string_view name) const {
        for (const NamedDrawing& d : others)
            if (d.name == name) return d.drawing;
        return main;
    }

    // Drawing `name` loaded alone, with the pivots the character gives its parts.
    Rig alone(std::string_view name) const {
        Rig r;
        std::string why;
        const bool ok = r.load(drawing_svg(drawing(name), 64, 64), why);
        INFO(why);
        REQUIRE(ok);
        apply_pivots(r, doc64(), &main, &others);
        return r;
    }
};

// A picture drawn by `draw` alone (a reference with more in it).
Rig drawn(std::string_view draw) {
    PoseScript s = script_of(code("wait(0.1)", draw));
    Drawing d;
    REQUIRE(s.draw({}, 1, d));
    Rig r;
    std::string why;
    const bool ok = r.load(drawing_svg(d, 64, 64), why);
    INFO(why);
    REQUIRE(ok);
    apply_pivots(r, doc64(), &d);
    return r;
}

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

usize bytes_off(const assets::CookedTexture& a, const assets::CookedTexture& b) {
    usize off = 0;
    for (usize i = 0; i < a.rgba8.size() && i < b.rgba8.size(); ++i) off += a.rgba8[i] != b.rgba8[i];
    return off;
}

bool same(const assets::CookedTexture& a, const assets::CookedTexture& b) {
    return a.width == b.width && a.height == b.height && a.rgba8.size() == b.rgba8.size() && bytes_off(a, b) == 0;
}

PartPose moved(f64 x, f64 y, f64 rot = 0, f64 scale = 1) {
    PartPose pp;
    pp.x = x, pp.y = y, pp.rot = rot, pp.scale = scale;
    return pp;
}

PartPose faded(f64 opacity) {
    PartPose pp;
    pp.opacity = opacity;
    return pp;
}

Pose swap_pose(const std::string& from, const std::string& to, f64 k) {
    Pose p;
    p.drawing = to;
    p.swap = true;
    p.swap_from = from;
    p.swap_k = k;
    return p;
}

// What of a pose a picture alone has: its parts and the copies of them, no drawing.
Pose only(const Pose& pose, const Rig& r) {
    Pose out;
    for (const PartCopy& c : pose.copies)
        if (r.part(c.source) >= 0) out.copies.push_back(c);
    auto has = [&](const std::string& id) {
        return r.part(id) >= 0 || std::any_of(out.copies.begin(), out.copies.end(), [&](const PartCopy& c) { return c.id == id; });
    };
    for (const auto& [id, pp] : pose.parts)
        if (has(id)) out.parts.emplace(id, pp);
    for (const auto& [id, mo] : pose.morphs)
        if (has(id)) out.morphs.emplace(id, mo);
    for (const OrderStep& s : pose.order)
        if (has(s.part) && (s.other.empty() || has(s.other))) out.order.push_back(s);
    return out;
}

// The rig in the pose against `ref` in its own: smooth and baked, byte for byte.
void same_as(Rig& rig, const Pose& pose, Rig& ref, const Pose& ref_pose) {
    std::string why;
    bool ok = rig.pose(pose, why);
    INFO(why);
    REQUIRE(ok);
    ok = ref.pose(ref_pose, why);
    INFO(why);
    REQUIRE(ok);
    const assets::CookedTexture a = rig.draw(128, 128), b = ref.draw(128, 128);
    INFO("bytes off: ", bytes_off(a, b));
    CHECK(same(a, b));
    const assets::CookedTexture ba = rig.bake(64, 64, {}), bb = ref.bake(64, 64, {});
    INFO("baked bytes off: ", bytes_off(ba, bb));
    CHECK(same(ba, bb));
}

// A path in other units, as the swap puts a shape of another drawing into the units of this one's.
std::string converted(std::string_view d, const Affine& m) {
    vector::Path p;
    REQUIRE(vector::parse_path(d, p));
    for (vector::Contour& c : p.contours) {
        c.start = m.apply(c.start);
        for (vector::Cubic& q : c.curves) q.c1 = m.apply(q.c1), q.c2 = m.apply(q.c2), q.to = m.apply(q.to);
    }
    return vector::to_d(p);
}

// Straight RGBA b over a, worked out in reals: what a swap's frame is, the new picture over the old.
assets::CookedTexture over_of(const assets::CookedTexture& a, const assets::CookedTexture& b) {
    assets::CookedTexture out = a;
    for (usize i = 0; i + 3 < out.rgba8.size(); i += 4) {
        const f64 sa = b.rgba8[i + 3] / 255.0, da = a.rgba8[i + 3] / 255.0, oa = sa + da * (1 - sa);
        if (oa <= 0) continue;
        for (int c = 0; c < 3; ++c)
            out.rgba8[i + c] = static_cast<u8>(std::lround((b.rgba8[i + c] * sa + a.rgba8[i + c] * da * (1 - sa)) / oa));
        out.rgba8[i + 3] = static_cast<u8>(std::lround(oa * 255));
    }
    return out;
}

// Baked pixels of b over a: a pixel is there or not.
assets::CookedTexture baked_over(const assets::CookedTexture& a, const assets::CookedTexture& b) {
    assets::CookedTexture out = a;
    for (usize i = 0; i + 3 < out.rgba8.size(); i += 4)
        if (b.rgba8[i + 3]) std::copy_n(b.rgba8.begin() + static_cast<std::ptrdiff_t>(i), 4, out.rgba8.begin() + static_cast<std::ptrdiff_t>(i));
    return out;
}

const u8* dot(const assets::CookedTexture& t, u32 x, u32 y) { return &t.rgba8[(static_cast<usize>(y) * t.width + x) * 4]; }

} // namespace

TEST_CASE("Motion swap: drawings in the code, their canvas and errors with their line") {
    Character c(hedgehog("wait(0.1)"));
    CHECK(c.script.drawings() == std::vector<std::string>{"angry", "sleepy"});
    REQUIRE(c.others.size() == 2);
    CHECK(c.others[0].name == "angry");
    CHECK(c.others[1].name == "sleepy");
    CHECK(c.rig.drawings() == std::vector<std::string>{"angry", "sleepy"});
    // The same code, the same drawing: «angry» is what draw would draw with its code.
    Drawing angry;
    REQUIRE(c.script.draw_named("angry", {}, 1, angry));
    CHECK(angry == c.drawing("angry"));
    CHECK(c.drawing("angry").shapes.size() == 5);
    Drawing main;
    REQUIRE(c.script.draw_named("draw", {}, 1, main));
    CHECK(main == c.main);
    // Parts are found in whichever drawing has them, the main one first.
    CHECK(c.rig.holder_name("head").empty());
    CHECK(c.rig.holder_name("brow") == "angry");
    CHECK(c.rig.holder_name("nope").empty());
    CHECK(&c.rig.holder("brow") == c.rig.drawing("angry"));
    CHECK(&c.rig.holder("nose") == &c.rig);
    CHECK(c.rig.drawing("nope") == nullptr);
    CHECK(c.rig.drawing("draw") == &c.rig);

    // A character without drawings has none.
    PoseScript plain = script_of(code("wait(0.1)"));
    CHECK(plain.drawings().empty());
    Drawing none;
    CHECK_FALSE(plain.draw_named("angry", {}, 1, none));
    CHECK(plain.error() == "нет рисунка «angry»: есть «draw»");

    auto load_error = [](const std::string& source) {
        PoseScript s;
        CHECK_FALSE(s.load(source));
        return s.error();
    };
    // Lines: the clip is line 3, the main drawing from line 6 on (kMain has 8), drawings at 15.
    CHECK(load_error(code("wait(0.1)", kMain, "  drawings = 5,\n")) ==
          "строка 15: drawings должен быть таблицей: drawings = { angry = function(p, rng) … end }");
    CHECK(load_error(code("wait(0.1)", kMain, "  drawings = {\n    angry = 3,\n  },\n")) ==
          "строка 16: drawings.angry: рисунок — функция, drawings = { angry = function(p, rng) … end }");
    CHECK(load_error(code("wait(0.1)", kMain, "  drawings = {\n    draw = function(p, rng) end,\n  },\n")) ==
          "строка 16: drawings.draw: так зовут основной рисунок, у другого рисунка — своё имя");

    // A drawing that fails says which, at its line; one that draws nothing, at its name.
    PoseScript broken = script_of(code("wait(0.1)", kMain,
                                       "  drawings = {\n    angry = function(p, rng)\n      rect(\"a\", { x = 1, y = 2, h = 3 })\n    end,\n"
                                       "    empty = function(p, rng) end,\n    loud = function(p, rng)\n      error(\"нет\")\n    end,\n  },\n"));
    CHECK(broken.drawings() == std::vector<std::string>{"angry", "empty", "loud"});
    Drawing d;
    CHECK_FALSE(broken.draw_named("angry", {}, 1, d));
    CHECK(broken.error() == "рисунок «angry», строка 17: rect «a»: нет w");
    CHECK(broken.error_line() == 17);
    CHECK_FALSE(broken.draw_named("empty", {}, 1, d));
    CHECK(broken.error() == "строка 19: рисунок «empty» ничего не нарисовал: rect, circle, ellipse, line, poly, path, group");
    CHECK(broken.error_line() == 19);
    CHECK_FALSE(broken.draw_named("loud", {}, 1, d));
    CHECK(broken.error().rfind("рисунок «loud», строка 21: ", 0) == 0);
    CHECK_FALSE(broken.draw_named("happy", {}, 1, d));
    CHECK(broken.error() == "нет рисунка «happy»: есть «draw», «angry», «empty», «loud»");
    // draw_drawings stops at the first that fails, with its line.
    std::vector<NamedDrawing> out;
    std::string why;
    u32 line = 0;
    CHECK_FALSE(draw_drawings(broken, doc64(), out, why, line));
    CHECK(why == "рисунок «angry», строка 17: rect «a»: нет w");
    CHECK(line == 17);
    CHECK(out.empty());

    // Another canvas: an error at the drawing's canvas line, or at its name without one.
    PoseScript wide = script_of(code("wait(0.1)", kMain, "  drawings = {\n    angry = function(p, rng)\n      canvas(64, 48)\n      rect(\"a\", { x = 1, y = 2, w = 3, h = 3 })\n    end,\n  },\n"));
    REQUIRE(draw_drawings(wide, doc64(), out, why, line));
    Rig rig;
    REQUIRE(rig.load(drawing_svg(c.main, 64, 64), why));
    CHECK_FALSE(add_drawings(rig, out, why, line));
    CHECK(why == "строка 17: у рисунка «angry» другой холст: 64 × 48, а у основного 64 × 64");
    CHECK(line == 17);
    PoseScript bare = script_of(code("wait(0.1)", kMain, "  drawings = {\n    angry = function(p, rng)\n      rect(\"a\", { x = 1, y = 2, w = 3, h = 3 })\n    end,\n  },\n"));
    REQUIRE(draw_drawings(bare, doc64(), out, why, line));
    Rig small;
    REQUIRE(small.load(drawing_svg(c.main, 64, 64), why));
    PixelDoc d32 = doc64();
    d32.frame_w = d32.frame_h = 32;
    REQUIRE(draw_drawings(bare, d32, out, why, line)); // no canvas: the frame's size
    CHECK_FALSE(add_drawings(small, out, why, line));
    CHECK(why == "строка 16: у рисунка «angry» другой холст: 32 × 32, а у основного 64 × 64");
    CHECK(line == 16);

    // The main picture an SVG file of the character (no draw): the drawings are code all the same, on its viewBox.
    PoseScript file = script_of("return {\n  clips = { c = function(p, rng) wait(0.1) end },\n" + drawings_of(kAngry, "") + "}\n");
    CHECK_FALSE(file.has_draw());
    REQUIRE(draw_drawings(file, doc64(), out, why, line));
    Rig from_file;
    REQUIRE(from_file.load(R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64"><circle id="head" cx="32" cy="30" r="14"/></svg>)svg", why));
    CHECK(add_drawings(from_file, out, why, line));
    CHECK(from_file.holder_name("brow") == "angry");
    CHECK_FALSE(from_file.add_drawing("draw", drawing_svg(c.main, 64, 64), why));
    CHECK(why == "рисунок «draw»: так зовут основной рисунок");
}

TEST_CASE("Motion swap: blocks of the drawing track, flow and errors with their line") {
    Character c(hedgehog(R"(local head = part("head")
head.y(-2, 0.3)
spawn(part("nose").x(4, 0.6))
swap("angry", 0.2, "smooth")
wait(0.1)
all(swap("sleepy", 0.2, "linear"), part("brow").rot(10, 0.4))
chain(wait(0.05), swap("draw", 0.2))
swap("draw", 0)
swap("angry"))"));
    const Recording rec = recorded(c.script, &c.rig);
    CHECK(rec.parts[block(rec, kDrawingPart, Prop::Drawing).part] == kDrawingPart);
    struct Want {
        const char* to;
        f64 start, length;
        const char* words;
    };
    const Want want[] = {{"angry", 0.3, 0.2, "→ angry"}, {"sleepy", 0.6, 0.2, "→ sleepy"}, {"draw", 1.05, 0.2, "→ draw"},
                         {"draw", 1.25, 0, "→ draw"},    {"angry", 1.25, 0, "→ angry"}};
    for (usize k = 0; k < std::size(want); ++k) {
        INFO(want[k].to, " ", k);
        const Block& b = block(rec, kDrawingPart, Prop::Drawing, k);
        CHECK(b.end.target == want[k].to);
        CHECK(b.start == doctest::Approx(want[k].start));
        CHECK(b.length == doctest::Approx(want[k].length));
        CHECK(block_words(rec, b) == want[k].words);
    }
    CHECK(block(rec, kDrawingPart, Prop::Drawing, 0).timing == vector::Timing::Smooth);
    CHECK(block(rec, kDrawingPart, Prop::Drawing, 1).timing == vector::Timing::Linear);
    CHECK(block(rec, kDrawingPart, Prop::Drawing, 2).timing == vector::Timing::Smooth); // as every transition without one
    // all waits for the brow's 0.4; the brow is a part of «angry» only.
    CHECK(block(rec, "brow", Prop::Rot).start == doctest::Approx(0.6));
    CHECK(rec.length == doctest::Approx(1.25));

    // The frame: the drawing shown, and in a swap the one before it and how far.
    struct Shown {
        f64 t;
        const char* drawing;
        bool swap;
        const char* from;
    };
    const Shown shown[] = {{0.25, "", false, ""},     {0.3, "", false, ""},           {0.4, "angry", true, ""},
                           {0.5, "angry", false, ""}, {0.7, "sleepy", true, "angry"}, {0.9, "sleepy", false, ""},
                           {1.1, "", true, "sleepy"}, {1.25, "angry", false, ""}};
    for (const Shown& s : shown) {
        INFO("t = ", s.t);
        const Pose p = frame(c.script, rec, &c.rig, s.t);
        CHECK(p.drawing == s.drawing);
        CHECK(p.swap == s.swap);
        CHECK(p.swap_from == s.from);
        if (s.swap) CHECK(p.swap_k == doctest::Approx(block_share(rec, block(rec, kDrawingPart, Prop::Drawing, s.t < 0.5 ? 0 : s.t < 1 ? 1 : 2), s.t)));
        CHECK((p.swap_k > 0 && p.swap_k < 1) == s.swap);
    }
    CHECK(frame(c.script, rec, &c.rig, 0.4).swap_k == doctest::Approx(vector::timing(vector::Timing::Smooth, 0.5)));
    CHECK(frame(c.script, rec, &c.rig, 0.7).swap_k == doctest::Approx(0.5));
    // The nose goes on while «angry» has none; the main drawing has it there again.
    CHECK(frame(c.script, rec, &c.rig, 0.6).parts.at("nose").x == doctest::Approx(2));

    // block_share: 0 before the block, 1 after, eased between.
    const Block& first = block(rec, kDrawingPart, Prop::Drawing, 0);
    CHECK(block_share(rec, first, 0.2) == 0);
    CHECK(block_share(rec, first, 0.3) == 0);
    CHECK(block_share(rec, first, 0.5) == 1);
    CHECK(block_share(rec, first, 0.9) == 1);
    CHECK(block_share(rec, block(rec, kDrawingPart, Prop::Drawing, 3), 1.25) == 1);

    // A swap to the drawing shown changes nothing; back to "draw".
    PoseScript again = script_of(hedgehog("swap(\"draw\", 0.2)\nswap(\"angry\", 0.2)\nswap(\"angry\", 0.2)"));
    const Recording ar = recorded(again, &c.rig);
    const Pose p0 = frame(again, ar, &c.rig, 0.1), p1 = frame(again, ar, &c.rig, 0.5);
    CHECK(p0.drawing.empty());
    CHECK_FALSE(p0.swap);
    CHECK(p1.drawing == "angry");
    CHECK_FALSE(p1.swap);

    // The name is the code's, the length and timing are fields.
    std::string why;
    CHECK_FALSE(can_edit(rec, first, BlockEdit::Value, c.script.code(), why));
    CHECK(why == "рисунок правится в коде: swap(…)");
    CHECK(can_edit(rec, first, BlockEdit::Length, c.script.code(), why));
    std::string edited;
    const std::string source = hedgehog("swap(\"angry\", 0.2, \"smooth\")");
    PoseScript one = script_of(source);
    const Recording orec = recorded(one, &c.rig);
    REQUIRE(edit_length(orec, block(orec, kDrawingPart, Prop::Drawing), 0.35, one.code(), source, edited, why));
    CHECK(edited.find("swap(\"angry\", 0.35, \"smooth\")") != std::string::npos);
    PropValue v;
    v.target = "sleepy";
    CHECK_FALSE(edit_value(orec, block(orec, kDrawingPart, Prop::Drawing), Prop::Drawing, v, one.code(), source, edited, why));
    CHECK(why == "рисунок правится в коде: swap(…)");

    auto error = [&](const char* body) {
        PoseScript e = script_of(hedgehog(body));
        return record_error(e, &c.rig);
    };
    CHECK(error("swap(3, 0.2)") == "строка 3: swap: имя рисунка — строка, swap(\"angry\", 0.4)");
    CHECK(error("swap(\"happy\", 0.2)") == "строка 3: нет рисунка «happy»: есть «draw», «angry», «sleepy»");
    CHECK(error("swap(\"angry\", -1)") == "строка 3: swap: длина — число секунд не меньше 0");
    CHECK(error("swap(\"angry\", \"долго\")") == "строка 3: swap: длина — число секунд");
    CHECK(error("swap(\"angry\", 0.2, \"smooth\", 4)") == "строка 3: swap(\"angry\", с, сглаживание): имя, длина и сглаживание — не больше");
    CHECK(error("tween(0.2, function() swap(\"angry\") end)").find("swap — внутри tween(с, функция) только мгновенные значения") != std::string::npos);
    // A part of some drawing is the clip's; one of none is not.
    CHECK(error("part(\"brow\").x(1, 0.1)\npart(\"nope\").x(1, 0.1)") == "строка 4: в рисунке нет части «nope»");
    CHECK(error("part(\"\").x(1, 0.1)") == "строка 3: part(\"\"): у части есть имя, её id в рисунке");
    CHECK(error("part(\"head\"):to({ [\"рисунок\"] = \"angry\" }, 0.2)").rfind("строка 3: part:to: поля «рисунок» нет (есть x, y, rot,", 0) == 0);
    // A morph target of some drawing is the clip's too.
    PoseScript far = script_of(hedgehog("part(\"mouth\"):morph(\"brow\", 0.2)"));
    const Recording fr = recorded(far, &c.rig);
    CHECK(frame(far, fr, &c.rig, 0.3).morphs.at("mouth").target == "brow");
    // parts(…): the main picture's, then those only other drawings have, each once.
    const std::string every_source = hedgehog("for _, q in parts(\"*\") do q.x(1, 0.1) end");
    PoseScript every = script_of(every_source);
    const Recording er = recorded(every, &c.rig);
    const char* ids[] = {"face", "head", "mouth", "mouth_o", "nose", "brow"};
    CHECK(er.blocks.size() == std::size(ids));
    for (usize k = 0; k < std::size(ids); ++k) {
        INFO(ids[k]);
        CHECK(block(er, ids[k], Prop::X).start == doctest::Approx(0.1 * static_cast<f64>(k)));
    }
    // rest, save and restore are a part's: the drawing stays.
    PoseScript kept = script_of(hedgehog(R"(local head = part("head")
head:save()
swap("angry", 0)
head:rest(0.2)
head:restore(0.2))"));
    const Recording kr = recorded(kept, &c.rig);
    usize drawing_blocks = 0;
    for (const Block& b : kr.blocks) drawing_blocks += b.prop == Prop::Drawing;
    CHECK(drawing_blocks == 1);
    CHECK(kr.length == doctest::Approx(0.4));
    CHECK(frame(kept, kr, &c.rig, 0.4).drawing == "angry");
    // No drawing into the call of another block.
    CHECK_FALSE(edit_value(er, block(er, "head", Prop::X), Prop::Drawing, v, every.code(), every_source, edited, why));
    CHECK(why == "рисунок правится в коде: swap(…)");

    // The drawing is not a key of a table, and not a thing pose(t) gives.
    PoseScript old = script_of("return {\n  pose = function(t, state, p)\n    swap(\"angry\")\n    return {}\n  end,\n" + drawings_of() + "}\n");
    Pose op;
    CHECK_FALSE(old.pose(c.rig, 0, "stand", {}, 1, op));
    CHECK(old.error().find("swap — внутри функции клипа") != std::string::npos);
}

TEST_CASE("Motion swap: before and after a swap, each drawing as if drawn alone") {
    Character c(hedgehog(R"(local head = part("head")
head.y(-2, 0.3)
spawn(part("nose").x(4, 0.6))
swap("angry", 0.2, "smooth")
part("brow").rot(15, 0.1)
swap("sleepy", 0.2)
wait(0.1)
swap("draw", 0.2))"));
    const Recording rec = recorded(c.script, &c.rig);
    CHECK(rec.length == doctest::Approx(1.1));
    struct End {
        f64 t;
        const char* drawing;
    };
    // Before the first swap, after each, and with the nose's move done after the way back.
    for (const End& e : {End{0.25, "draw"}, End{0.55, "angry"}, End{0.6, "angry"}, End{0.85, "sleepy"}, End{1.1, "draw"}}) {
        INFO("t = ", e.t);
        const Pose pose = frame(c.script, rec, &c.rig, e.t);
        CHECK_FALSE(pose.swap);
        Rig ref = c.alone(e.drawing);
        same_as(c.rig, pose, ref, only(pose, ref));
        CHECK_FALSE(c.rig.swapping());
        CHECK(&c.rig.shown() == c.rig.drawing(e.drawing));
    }
    const Pose back = frame(c.script, rec, &c.rig, 1.1);
    CHECK(back.parts.at("nose").x == 4);
    CHECK(back.parts.at("brow").rot == 15);
    // The main drawing is not the same as before: the nose moved while it was away.
    Rig rest = c.alone("draw");
    std::string why;
    REQUIRE(rest.pose(only(frame(c.script, rec, &c.rig, 0.25), rest), why));
    REQUIRE(c.rig.pose(back, why));
    CHECK_FALSE(same(c.rig.draw(128, 128), rest.draw(128, 128)));

    // In the middle: two pictures, the shown one the new.
    const Pose mid = frame(c.script, rec, &c.rig, 0.7);
    REQUIRE(mid.swap);
    REQUIRE(c.rig.pose(mid, why));
    CHECK(c.rig.swapping());
    CHECK(&c.rig.shown() == c.rig.drawing("sleepy"));

    // A drawing the rig has not: an error, the main picture as it was.
    Pose happy;
    happy.drawing = "happy";
    CHECK_FALSE(c.rig.pose(happy, why));
    CHECK(why == "нет рисунка «happy»");
    CHECK_FALSE(c.rig.pose(swap_pose("happy", "angry", 0.5), why));
    CHECK(why == "нет рисунка «happy»");
    CHECK(c.rig.pose({}, why));
}

TEST_CASE("Motion swap: a pair morphs as morph does between the two written in one picture") {
    // Every shape has a pair: the old picture is the whole frame. The faces stand in different groups, moved and scaled.
    const char* main = R"(    canvas(64, 64)
    group("face", { y = 2 }, function()
      circle("head", { x = 32, y = 28, r = 14, fill = "#c09060", stroke = "#402010", stroke_width = 1.5 })
      path("mouth", { d = "M 26 34 Q 32 38 38 34", fill = "none", stroke = "#402010", stroke_width = 1.5 }))";
    const std::string head_d = "M 15.5 1 C 23 1 23 1 30 14 C 23 27 8 27 1 14 C 8 1 8 1 15.5 1 Z", mouth_d = "M 13 19 Q 15.5 16 18 19";
    const std::string angry = R"(      canvas(64, 64)
      group("face", { x = 1, scale = 2 }, function()
        path("head", { d = ")" + head_d + R"(", fill = "#e06040", stroke = "#401010", stroke_width = 0.75 })
        path("mouth", { d = ")" + mouth_d + R"(", fill = "none", stroke = "#401010", stroke_width = 0.75 })
      end))";
    Character c(code("wait(0.1)", std::string(main) + "\n    end)", drawings_of(angry, "")));
    // The pair of each written in the main picture, in its group, in its units: the stroke as wide as it looks.
    const Affine to_main = inverse(Affine{1, 0, 0, 1, 0, 2}) * Affine{2, 0, 0, 2, 1, 0};
    Rig ref = drawn(std::string(main) + "\n      path(\"head_t\", { d = \"" + converted(head_d, to_main) +
                    "\", fill = \"#e06040\", stroke = \"#401010\", stroke_width = 1.5 })\n      path(\"mouth_t\", { d = \"" +
                    converted(mouth_d, to_main) + "\", fill = \"none\", stroke = \"#401010\", stroke_width = 1.5 })\n    end)");
    for (f64 k : {0.3, 0.8}) {
        INFO("k = ", k);
        Pose want;
        want.morphs["head"] = {"head_t", k};
        want.morphs["mouth"] = {"mouth_t", k};
        want.parts["head_t"] = moved(1000, 0); // drawn off the canvas: the targets are seen
        want.parts["mouth_t"] = moved(1000, 0);
        same_as(c.rig, swap_pose("", "angry", k), ref, want);
    }
    // The other way: the angry face morphs to the main one's in its own units.
    const Affine to_angry = inverse(Affine{2, 0, 0, 2, 1, 0}) * Affine{1, 0, 0, 1, 0, 2};
    std::string circle_d;
    {
        vector::Path p = vector::circle(32, 28, 14);
        circle_d = vector::to_d(p);
    }
    Rig back = drawn(R"(      canvas(64, 64)
      group("face", { x = 1, scale = 2 }, function()
        path("head", { d = ")" + head_d + R"(", fill = "#e06040", stroke = "#401010", stroke_width = 0.75 })
        path("mouth", { d = ")" + mouth_d + R"(", fill = "none", stroke = "#401010", stroke_width = 0.75 })
        path("head_t", { d = ")" + converted(circle_d, to_angry) + R"(", fill = "#c09060", stroke = "#402010", stroke_width = 0.75 })
        path("mouth_t", { d = ")" + converted("M 26 34 Q 32 38 38 34", to_angry) + R"(", fill = "none", stroke = "#402010", stroke_width = 0.75 })
      end))");
    Pose want;
    want.morphs["head"] = {"head_t", 0.4};
    want.morphs["mouth"] = {"mouth_t", 0.4};
    want.parts["head_t"] = moved(1000, 0);
    want.parts["mouth_t"] = moved(1000, 0);
    same_as(c.rig, swap_pose("angry", "", 0.4), back, want);
}

TEST_CASE("Motion swap: a part without a pair fades, the new over the old") {
    // No id in both: each picture's shapes fade, the new ones over the old.
    const char* main = R"(    canvas(64, 64)
    rect("body", { x = 10, y = 20, w = 30, h = 24, fill = "#3060c0" })
    circle("eye", { x = 24, y = 28, r = 5, fill = "#ffffff", stroke = "#000000" }))";
    const char* angry = R"(      canvas(64, 64)
      rect("tail", { x = 30, y = 30, w = 24, h = 20, fill = "#c03030" })
      path("brow", { d = "M 18 20 L 30 24", fill = "none", stroke = "#401010", stroke_width = 3 }))";
    Character c(code("wait(0.1)", main, drawings_of(angry, "")));
    Rig a = c.alone("draw"), b = c.alone("angry");
    std::string why;
    for (f64 k : {0.25, 0.5}) {
        INFO("k = ", k);
        REQUIRE(c.rig.pose(swap_pose("", "angry", k), why));
        Pose pa, pb;
        pa.parts["body"] = faded(1 - k);
        pa.parts["eye"] = faded(1 - k);
        pb.parts["tail"] = faded(k);
        pb.parts["brow"] = faded(k);
        REQUIRE(a.pose(pa, why));
        REQUIRE(b.pose(pb, why));
        const assets::CookedTexture got = c.rig.draw(128, 128), want = over_of(a.draw(128, 128), b.draw(128, 128));
        INFO("bytes off: ", bytes_off(got, want));
        CHECK(same(got, want));
        // Baked: the old picture's pixels, the new one's over them.
        const assets::CookedTexture baked = c.rig.bake(64, 64, {}), want_baked = baked_over(a.bake(64, 64, {}), b.bake(64, 64, {}));
        INFO("baked bytes off: ", bytes_off(baked, want_baked));
        CHECK(same(baked, want_baked));
    }
    // At a half both are baked; the new tail over the old body where they meet.
    const assets::CookedTexture half = c.rig.bake(64, 64, {});
    CHECK(dot(half, 35, 35)[0] == 0xc0);
    CHECK(dot(half, 15, 40)[2] == 0xc0);
    CHECK(dot(half, 50, 45)[0] == 0xc0);

    // A shape without an id fades as well: the grass at 1 − k, the square at k.
    Character h(hedgehog("wait(0.1)"));
    REQUIRE(h.rig.pose(swap_pose("", "angry", 0.25), why));
    const assets::CookedTexture q = h.rig.draw(64, 64);
    CHECK(std::abs(dot(q, 12, 57)[3] - 191) <= 1);
    CHECK(std::abs(dot(q, 54, 55)[3] - 64) <= 1);
}

TEST_CASE("Motion swap: hidden shapes stay out, a group is no pair, copies swap as parts") {
    // A lid hidden in the main picture and seen in «open»: not a pair, it comes in as a new part. A group and a shape of
    // one id: no pair either.
    const char* main = R"(    canvas(64, 64)
    rect("box", { x = 12, y = 30, w = 40, h = 24, fill = "#806040" })
    rect("lid", { x = 10, y = 22, w = 44, h = 8, fill = "#a08060", hidden = true })
    group("lock", function()
      rect({ x = 56, y = 34, w = 6, h = 6, fill = "#e0c040" })
    end))";
    const char* open = R"(      canvas(64, 64)
      rect("box", { x = 12, y = 30, w = 40, h = 24, fill = "#806040" })
      rect("lid", { x = 10, y = 10, w = 44, h = 8, fill = "#a08060" })
      rect("lock", { x = 56, y = 34, w = 6, h = 6, fill = "#e0c040" }))";
    const std::string src = "return {\n  clips = { c = function(p, rng)\nwait(0.1)\n  end },\n  draw = function(p, rng)\n" + std::string(main) +
                            "\n  end,\n  drawings = {\n    open = function(p, rng)\n" + open + "\n    end,\n  },\n}\n";
    Character c(src);
    std::string why;
    REQUIRE(c.rig.pose(swap_pose("", "open", 0.5), why));
    const assets::CookedTexture got = c.rig.draw(128, 128);
    // The lid is where «open» has it, half there; nothing of it where the main picture hides it.
    CHECK(dot(got, 64, 28)[3] == 128);
    CHECK(dot(got, 64, 50)[3] == 0);
    // The box, a pair of the same shape: whole.
    CHECK(dot(got, 30, 90)[3] == 255);
    CHECK(dot(got, 30, 90)[0] == 0x80);
    // The lock: the old group's unnamed square at a half, the new shape at a half over it (128 over 128).
    CHECK(dot(got, 118, 74)[3] == 192);

    // Copies: one whose part is in both morphs; ends exact.
    Character h(hedgehog(R"(copies("head", 1, function(i, c) all(c.scale(0.3, 0.1), c.x(-22, 0.1), c.y(-20, 0.1)) end)
swap("angry", 0.2)
wait(0.1))"));
    const Recording rec = recorded(h.script, &h.rig);
    const Pose mid = frame(h.script, rec, &h.rig, 0.2);
    REQUIRE(mid.swap);
    REQUIRE(mid.copies.size() == 1);
    REQUIRE(h.rig.pose(mid, why));
    const assets::CookedTexture m = h.rig.draw(64, 64);
    // Its middle is whole, of a colour between the two heads.
    const u8* px = dot(m, 10, 10);
    CHECK(px[3] == 255);
    CHECK(px[0] > 0xc0);
    CHECK(px[0] < 0xe0);
    for (f64 t : {0.1, 0.3}) {
        INFO("t = ", t);
        const Pose pose = frame(h.script, rec, &h.rig, t);
        Rig ref = h.alone(t < 0.2 ? "draw" : "angry");
        same_as(h.rig, pose, ref, only(pose, ref));
    }
}

TEST_CASE("Motion swap: what the shown drawing lacks — morph targets, masks, links and order") {
    Character c(hedgehog("wait(0.1)"));
    // The open mouth is the main picture's only: «angry» morphs to it in its own units.
    Pose pose;
    pose.drawing = "angry";
    pose.morphs["mouth"] = {"mouth_o", 0.5};
    const Affine to_angry = inverse(Affine{2, 0, 0, 2, 1, 0}) * Affine{1, 0, 0, 1, 0, 2};
    std::string angry = kAngry;
    const std::string mouth_line = R"(        path("mouth", { d = "M 13 19 Q 15.5 16 18 19", fill = "none", stroke = "#401010", stroke_width = 0.75 }))";
    REQUIRE(angry.find(mouth_line) != std::string::npos);
    angry.insert(angry.find(mouth_line) + mouth_line.size(), "\n        path(\"mouth_o\", { d = \"" + converted("M 27 33 Q 32 40 37 33 Z", to_angry) +
                                                                 "\", fill = \"#802020\", stroke_width = 0.5, hidden = true })");
    Rig ref = drawn(angry);
    Pose ref_pose;
    ref_pose.morphs = pose.morphs;
    same_as(c.rig, pose, ref, ref_pose);
    // And in the main picture, its own.
    Pose own;
    own.morphs["mouth"] = {"mouth_o", 0.5};
    Rig main = c.alone("draw");
    same_as(c.rig, own, main, own);

    // A mask the shown drawing lacks does not act, a link to a part it lacks carries as if at rest, order beside such a
    // part does not change.
    Pose lacks;
    lacks.drawing = "angry";
    lacks.parts["brow"].mask = "nose";
    lacks.parts["head"].carry_by = "nose";
    lacks.parts["head"].carry = Affine{1, 0, 0, 1, 3, 1};
    lacks.parts["nose"] = moved(10, 0);
    lacks.order.push_back({"brow", OrderStep::Kind::Below, "nose"});
    lacks.order.push_back({"brow", OrderStep::Kind::Bottom, ""});
    Rig b = c.alone("angry");
    Pose want;
    want.parts["brow"] = PartPose{};
    want.parts["head"].carry = Affine{1, 0, 0, 1, 3, 1};
    want.order.push_back({"brow", OrderStep::Kind::Bottom, ""});
    same_as(c.rig, lacks, b, want);
    // In the main picture the link carries by the moved nose.
    Pose in_main = lacks;
    in_main.drawing.clear();
    std::string why;
    REQUIRE(c.rig.pose(in_main, why));
    const assets::CookedTexture carried = c.rig.draw(128, 128);
    in_main.parts["nose"] = PartPose{};
    REQUIRE(c.rig.pose(in_main, why));
    CHECK_FALSE(same(carried, c.rig.draw(128, 128)));
}

TEST_CASE("Motion swap: pivots by id, one for every drawing") {
    Character c(hedgehog("wait(0.1)"));
    // The brow's pivot is «angry»'s; the head has none drawn: the middle of its bounds in each drawing.
    const Rig& angry = *c.rig.drawing("angry");
    CHECK(angry.pivot(angry.part("brow")).x == 32);
    CHECK(angry.pivot(angry.part("brow")).y == 20);
    const Part& ah = angry.parts()[static_cast<usize>(angry.part("head"))];
    CHECK(angry.pivot(angry.part("head")).x == (ah.x0 + ah.x1) / 2);
    const Part& mh = c.rig.parts()[static_cast<usize>(c.rig.part("head"))];
    CHECK(c.rig.pivot(c.rig.part("head")).y == (mh.y0 + mh.y1) / 2);
    // The file's pivot goes to every drawing; one for a part of no drawing is missing.
    PixelDoc doc = doc64();
    doc.pivots["head"] = {30, 31};
    doc.pivots["brow"] = {33, 21};
    doc.pivots["ghost"] = {1, 1};
    CHECK(apply_pivots(c.rig, doc, &c.main, &c.others) == std::vector<std::string>{"ghost"});
    for (const char* name : {"draw", "angry", "sleepy"}) {
        INFO(name);
        const Rig& r = *c.rig.drawing(name);
        CHECK(r.pivot(r.part("head")).x == 30);
        CHECK(r.pivot(r.part("head")).y == 31);
    }
    CHECK(angry.pivot(angry.part("brow")).x == 33);
    // The main drawing's pivot comes first, then the others' by name.
    const std::string drawn_pivots = code("wait(0.1)",
                                          "    canvas(64, 64)\n    circle(\"head\", { x = 32, y = 28, r = 14, pivot = { 31, 40 } })",
                                          drawings_of("      canvas(64, 64)\n      circle(\"head\", { x = 30, y = 28, r = 10, pivot = { 5, 5 } })\n"
                                                      "      rect(\"tail\", { x = 1, y = 1, w = 4, h = 4, pivot = { 2, 3 } })",
                                                      "      canvas(64, 64)\n      rect(\"tail\", { x = 1, y = 1, w = 4, h = 4, pivot = { 4, 4 } })"));
    Character p(drawn_pivots);
    for (const char* name : {"draw", "angry"}) {
        const Rig& r = *p.rig.drawing(name);
        CHECK(r.pivot(r.part("head")).x == 31);
        CHECK(r.pivot(r.part("head")).y == 40);
    }
    for (const char* name : {"angry", "sleepy"}) {
        const Rig& r = *p.rig.drawing(name);
        CHECK(r.pivot(r.part("tail")).x == 2);
        CHECK(r.pivot(r.part("tail")).y == 3);
    }
}

TEST_CASE("Motion swap: a morphed pair, a drawing put again, a pivot set on the character") {
    // The clip morphs the head into a square far off (hidden in the main picture, angry morphs to it in its own units):
    // in a swap the head goes from that square in the old drawing to the same square in the new one, not from its shape.
    const char* main = R"(    canvas(64, 64)
    circle("head", { x = 32, y = 30, r = 10, fill = "#c09060" })
    rect("far", { x = 2, y = 2, w = 8, h = 8, fill = "#c09060", hidden = true }))";
    const char* angry = R"(      canvas(64, 64)
      group("face", { x = 1, scale = 2 }, function()
        circle("head", { x = 15, y = 15, r = 6, fill = "#e06040" })
      end))";
    Character c(code("wait(0.1)", main, drawings_of(angry, "")));
    auto head_box = [&](const Pose& pose) {
        std::string why;
        const bool ok = c.rig.pose(pose, why);
        INFO(why);
        REQUIRE(ok);
        std::array<f64, 4> b{};
        c.rig.bounds(c.rig.part("head"), Affine{}, b.data());
        return b;
    };
    const Pose mid = swap_pose("", "angry", 0.5);
    Pose morphed = mid;
    morphed.morphs["head"] = {"far", 1};
    // At rest the pair is halfway between the two circles: (32; 30) r 10 and (31; 30) r 12.
    const std::array<f64, 4> rest = head_box(mid);
    CHECK(rest[0] == doctest::Approx(20.5));
    CHECK(rest[2] == doctest::Approx(42.5));
    const std::array<f64, 4> square = head_box(morphed);
    for (usize i = 0; i < 4; ++i) {
        INFO("edge ", i);
        CHECK(square[i] == doctest::Approx(i < 2 ? 2 : 10).epsilon(0.001));
    }
    CHECK(head_box(mid) == rest); // the pair at rest again as before the morph

    // A drawing put again: the swap goes to the new one, nothing kept from the one before.
    const char* moved = R"(      canvas(64, 64)
      group("face", { x = 1, scale = 2 }, function()
        circle("head", { x = 10, y = 12, r = 5, fill = "#e06040" })
      end))";
    Character fresh(code("wait(0.1)", main, drawings_of(moved, "")));
    std::string why;
    u32 line = 0;
    REQUIRE(add_drawings(c.rig, fresh.others, why, line));
    CHECK(apply_pivots(c.rig, doc64(), &c.main, &fresh.others).empty());
    same_as(c.rig, mid, fresh.rig, mid);
    same_as(c.rig, morphed, fresh.rig, morphed);

    // A pivot set on the character is every drawing's: the editor sets it so while it is dragged.
    c.rig.set_pivot("head", {30, 31});
    const Rig& a = *c.rig.drawing("angry");
    CHECK(a.pivot(a.part("head")).x == 30);
    CHECK(a.pivot(a.part("head")).y == 31);
}
