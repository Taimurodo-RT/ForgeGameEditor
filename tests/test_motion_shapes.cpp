// Part 4е of docs/models/движение-модель.md («Фигуры мышью»): a shape drawn with the mouse is a new last line of the
// drawing's function, its call written as the code writes numbers; the «Фигура» section rewrites a field of the call
// that drew a shape, the rest of the file byte for byte. A call written so draws what the same call typed by hand does.

#include "forge/pixel/code_sites.h"
#include "forge/pixel/draw.h"
#include "forge/pixel/draw_edit.h"
#include "forge/pixel/script.h"

#include <doctest/doctest.h>

#include <ostream> // doctest's INFO prints a string_view through it; MSVC needs the whole class
#include <set>
#include <string>
#include <vector>

using namespace forge;
using namespace forge::pixel;

namespace {

constexpr const char* kRect = R"(rect("rect_1", { x = 10, y = 6, w = 12, h = 8, fill = "#5968e8" }))";

CodeSites sites_of(std::string_view source) {
    CodeSites c;
    std::string why;
    u32 line = 0;
    const bool ok = c.parse(source, why, line);
    INFO(why);
    REQUIRE(ok);
    return c;
}

// The text with `line` put at the end of drawing `name`; "" with why when refused.
std::string inserted(std::string_view source, std::string_view name, std::string_view line, std::string* why_out = nullptr) {
    const CodeSites c = sites_of(source);
    std::string out, why;
    const bool ok = insert_drawing_line(c, source, name, line, out, why);
    if (why_out) *why_out = why;
    CHECK(ok == why.empty());
    return out;
}

PoseScript script_of(std::string_view source) {
    PoseScript s;
    const bool ok = s.load(source);
    INFO(s.error());
    REQUIRE(ok);
    return s;
}

Drawing drawn(PoseScript& s, std::string_view name = "draw") {
    Drawing d;
    const bool ok = s.draw_named(name, {}, 1, d);
    INFO(s.error());
    REQUIRE(ok);
    return d;
}

const DrawShape& shape(const Drawing& d, std::string_view id) {
    for (const DrawShape& s : d.shapes)
        if (s.id == id) return s;
    FAIL("no shape " << std::string(id));
    return d.shapes.front();
}

std::string call(const NewShape& s, const std::string& id) {
    std::string out, why;
    const bool ok = shape_call(s, id, out, why);
    INFO(why);
    CHECK(ok);
    return out;
}

std::string refused_call(const NewShape& s) {
    std::string out, why;
    CHECK_FALSE(shape_call(s, "x_1", out, why));
    CHECK(out.empty());
    return why;
}

} // namespace

TEST_CASE("Motion shapes: a new last line of a drawing's function") {
    const std::string head = "return {\n  clips = { c = function(p) end },\n";

    // Several lines: before "end", indented as the last statement; a comment above "end" stays above it.
    const std::string many = head + "  draw = function(p, rng)\n    canvas(32, 32)\n    rect(\"a\", { x = 1, y = 1, w = 2, h = 2 })\n    -- the end\n  end,\n}\n";
    CHECK(inserted(many, "draw", kRect) == head + "  draw = function(p, rng)\n    canvas(32, 32)\n    rect(\"a\", { x = 1, y = 1, w = 2, h = 2 })\n    -- the end\n    " +
                                             kRect + "\n  end,\n}\n");
    // The last statement over several lines: its first line's indent.
    const std::string group = head + "  draw = function(p, rng)\n\tgroup(\"g\", function()\n\t\trect(\"a\", { x = 1, y = 1, w = 2, h = 2 })\n\tend)\n  end,\n}\n";
    CHECK(inserted(group, "draw", kRect) == head + "  draw = function(p, rng)\n\tgroup(\"g\", function()\n\t\trect(\"a\", { x = 1, y = 1, w = 2, h = 2 })\n\tend)\n\t" + kRect + "\n  end,\n}\n");
    // Empty: the indent of "end" and two spaces.
    CHECK(inserted(head + "  draw = function(p, rng)\n  end,\n}\n", "draw", kRect) == head + "  draw = function(p, rng)\n    " + kRect + "\n  end,\n}\n");
    // On one line, or an "end" sharing its line: before it.
    CHECK(inserted(head + "  draw = function(p, rng) canvas(8, 8) end,\n}\n", "draw", kRect) == head + "  draw = function(p, rng) canvas(8, 8) " + kRect + " end,\n}\n");
    CHECK(inserted(head + "  draw = function(p, rng) end,\n}\n", "draw", kRect) == head + "  draw = function(p, rng) " + kRect + " end,\n}\n");
    CHECK(inserted(head + "  draw = function(p, rng)\n    canvas(8, 8) end,\n}\n", "draw", kRect) == head + "  draw = function(p, rng)\n    canvas(8, 8) " + kRect + " end,\n}\n");
    // A return that ends it: before the return.
    CHECK(inserted(head + "  draw = function(p, rng)\n    canvas(8, 8)\n    return\n  end,\n}\n", "draw", kRect) ==
          head + "  draw = function(p, rng)\n    canvas(8, 8)\n    " + kRect + "\n    return\n  end,\n}\n");
    CHECK(inserted(head + "  draw = function(p, rng) canvas(8, 8) return end,\n}\n", "draw", kRect) == head + "  draw = function(p, rng) canvas(8, 8) " + kRect + " return end,\n}\n");
    // CRLF lines: the new one too.
    std::string crlf = many;
    for (usize at = 0; (at = crlf.find('\n', at)) != std::string::npos; at += 2) crlf.insert(at, "\r");
    std::string crlf_out = head + "  draw = function(p, rng)\n    canvas(32, 32)\n    rect(\"a\", { x = 1, y = 1, w = 2, h = 2 })\n    -- the end\n    " + kRect + "\n  end,\n}\n";
    for (usize at = 0; (at = crlf_out.find('\n', at)) != std::string::npos; at += 2) crlf_out.insert(at, "\r");
    CHECK(inserted(crlf, "draw", kRect) == crlf_out);

    // A drawing of drawings: its function only.
    const std::string two = head + "  draw = function(p, rng)\n    canvas(32, 32)\n  end,\n  drawings = {\n    open = function(p, rng)\n      canvas(32, 32)\n    end,\n  },\n}\n";
    CHECK(inserted(two, "open", kRect) == head + "  draw = function(p, rng)\n    canvas(32, 32)\n  end,\n  drawings = {\n    open = function(p, rng)\n      canvas(32, 32)\n      " + kRect +
                                             "\n    end,\n  },\n}\n");
    const CodeSites c = sites_of(two);
    REQUIRE(c.drawing_functions().size() == 2);
    CHECK(c.drawing_functions()[0].name == "draw");
    CHECK(c.drawing_functions()[1].name == "open");
    CHECK(c.drawing_function("open")->line == 7);
    CHECK(c.drawing_function("angry") == nullptr);

    // Refused: none, or not a function here.
    std::string why;
    CHECK(inserted(head + "}\n", "draw", kRect, &why).empty());
    CHECK(why == "в коде нет draw = function(p, rng) … end");
    CHECK(inserted(two, "angry", kRect, &why).empty());
    CHECK(why == "в drawings нет рисунка «angry»");
    CHECK(inserted("local shared = require(\"s\")\n" + head + "  draw = shared.draw,\n}\n", "draw", kRect, &why).empty());
    CHECK(why == "draw — не функция в этом файле: фигура не дописывается");
    CHECK(inserted(head + "  draw = function(p, rng) end,\n  drawings = { open = shared.open },\n}\n", "open", kRect, &why).empty());
    CHECK(why == "рисунок «open» — не функция в этом файле: фигура не дописывается");
    // The source the sites were not parsed from: refused, not written somewhere else.
    {
        const CodeSites old = sites_of(many);
        std::string out;
        CHECK_FALSE(insert_drawing_line(old, "return {}", "draw", kRect, out, why));
        CHECK(why == "код изменился: место рисунка устарело");
        CHECK(out.empty());
    }

    // What is written draws: the new shape last, with the call's numbers.
    for (const std::string& text : {inserted(many, "draw", kRect), inserted(crlf, "draw", kRect), inserted(group, "draw", kRect)}) {
        PoseScript s = script_of(text);
        const Drawing d = drawn(s);
        REQUIRE(!d.shapes.empty());
        const DrawShape& last = d.shapes.back();
        CHECK(last.id == "rect_1");
        CHECK(last.kind == DrawShape::Kind::Rect);
        CHECK(last.parent == -1);
        CHECK(last.x == 10);
        CHECK(last.y == 6);
        CHECK(last.w == 12);
        CHECK(last.h == 8);
        CHECK(last.fill == "#5968e8");
    }
    PoseScript s = script_of(inserted(two, "open", kRect));
    CHECK(drawn(s, "open").shapes.back().id == "rect_1");
}

TEST_CASE("Motion shapes: the calls of the tools, their points and ids") {
    using K = NewShape::Kind;
    // Points to tenths, or on the grid's lines; a move of whole steps.
    CHECK(tool_round(3.14, 0) == doctest::Approx(3.1));
    CHECK(tool_round(-0.04, 0) == 0);
    CHECK(format_number(tool_round(-0.04, 0)) == "0");
    CHECK(tool_round(7.4, 5) == 5);
    CHECK(tool_round(7.6, 5) == 10);
    CHECK(tool_round(-3.6, 2) == -4);
    CHECK(tool_round(1.3, 0.5) == 1.5);
    CHECK(tool_point({3.14, 7.6}, 5).x == 5);
    CHECK(tool_point({3.14, 7.6}, 5).y == 10);
    CHECK(snap_steps(4.9, 2) == 4);
    CHECK(snap_steps(-5.1, 2) == -6);
    CHECK(snap_steps(0.9, 2) == 0);
    CHECK(snap_steps(1.23, 0) == 1.23);

    // From two corners in any direction.
    CHECK(call({K::Rect, {{10, 6}, {22, 14}}, "#5968e8"}, "rect_1") == kRect);
    CHECK(call({K::Rect, {{22, 14}, {10, 6}}, "#5968e8"}, "rect_1") == kRect);
    CHECK(call({K::Rect, {{22, 6}, {10, 14}}, "#5968e8"}, "rect_1") == kRect);
    CHECK(call({K::Rect, {{-2.5, 0.1}, {0.2, -3}}, "#102030"}, "rect_2") == R"(rect("rect_2", { x = -2.5, y = -3, w = 2.7, h = 3.1, fill = "#102030" }))");
    CHECK(call({K::Ellipse, {{10, 6}, {22, 14}}, "#5968e8"}, "ellipse_1") == R"(ellipse("ellipse_1", { x = 16, y = 10, rx = 6, ry = 4, fill = "#5968e8" }))");
    CHECK(call({K::Ellipse, {{0.1, 0}, {0.4, 0.3}}, "#5968e8"}, "ellipse_1") == R"(ellipse("ellipse_1", { x = 0.25, y = 0.15, rx = 0.15, ry = 0.15, fill = "#5968e8" }))");
    CHECK(call({K::Line, {{30, 12}, {2, 2}}, "#5968e8"}, "line_1") == R"(line("line_1", { x1 = 30, y1 = 12, x2 = 2, y2 = 2, stroke = "#5968e8" }))");
    CHECK(call({K::Path, {{2, 2}, {10, 4}, {10, 4}, {6, 12}}, "#5968e8"}, "path_1") ==
          R"(path("path_1", { d = "M 2 2 L 10 4 L 6 12", fill = "none", stroke = "#5968e8" }))");
    CHECK(call({K::Poly, {{2, 2}, {10, 4}, {6, 12}}, "#5968e8"}, "poly_1") == R"(poly("poly_1", { points = { 2, 2, 10, 4, 6, 12 }, fill = "#5968e8" }))");
    // Closed on the first point: that one is not twice.
    CHECK(call({K::Poly, {{2, 2}, {10, 4}, {6, 12}, {2, 2}}, "#5968e8"}, "poly_1") == R"(poly("poly_1", { points = { 2, 2, 10, 4, 6, 12 }, fill = "#5968e8" }))");

    // Without a size, nothing.
    const std::string no_size = "фигура без размера не рисуется: протяните мышью";
    CHECK(refused_call({K::Rect, {{10, 6}, {10, 14}}}) == no_size);
    CHECK(refused_call({K::Rect, {{10, 6}, {22, 6.0004}}}) == no_size);
    CHECK(refused_call({K::Ellipse, {{3, 3}, {9, 3}}}) == no_size);
    CHECK(refused_call({K::Rect, {{10, 6}}}) == no_size);
    CHECK(refused_call({K::Line, {{4, 4}, {4, 4}}}) == no_size);
    CHECK(refused_call({K::Path, {{4, 4}, {4, 4}}}) == "перу нужно две разные точки: щёлкните ещё");
    CHECK(refused_call({K::Poly, {{4, 4}, {6, 6}, {4, 4}}}) == "замкнутому перу нужно три разные точки");

    // A click where the last point is adds none.
    std::vector<vector::Point> pen;
    add_pen_point(pen, {1, 1});
    add_pen_point(pen, {1, 1});
    add_pen_point(pen, {2, 1});
    add_pen_point(pen, {1, 1});
    CHECK(pen.size() == 3);

    // The least number no part of any drawing has.
    CHECK(new_shape_id(K::Rect, {}) == "rect_1");
    CHECK(new_shape_id(K::Rect, {"rect_1", "rect_2", "rect_4", "ellipse_3"}) == "rect_3");
    CHECK(new_shape_id(K::Ellipse, {"rect_1", "ellipse_1"}) == "ellipse_2");
    CHECK(new_shape_id(K::Path, {"poly_1"}) == "path_1");
    CHECK(std::string(new_shape_word(K::Poly)) == "poly");
    CHECK(std::string(kShapeColor) == "#5968e8");
}

TEST_CASE("Motion shapes: a call written by a tool draws as the same call typed") {
    const std::string head = "return {\n  clips = { c = function(p) end },\n  draw = function(p, rng)\n    canvas(32, 32)\n    circle(\"head\", { x = 16, y = 12, r = 6, fill = \"#c09060\" })\n";
    const std::string tail = "  end,\n}\n";
    const std::vector<NewShape> shapes = {
        {NewShape::Kind::Rect, {{3, 20}, {11, 28}}, "#5968e8"},
        {NewShape::Kind::Ellipse, {{20, 20}, {29, 25}}, "#a03020"},
        {NewShape::Kind::Line, {{1, 1}, {31, 9.5}}, "#204080"},
        {NewShape::Kind::Path, {{2, 30}, {8, 24}, {14, 30}}, "#5968e8"},
        {NewShape::Kind::Poly, {{18, 2}, {30, 2}, {24, 9}}, "#5968e8"},
    };
    std::string text = head + tail, typed = head;
    std::set<std::string> taken = {"head"};
    for (const NewShape& s : shapes) {
        const std::string id = new_shape_id(s.kind, taken);
        taken.insert(id);
        const std::string line = call(s, id);
        text = inserted(text, "draw", line);
        typed += "    " + line + "\n";
    }
    CHECK(text == typed + tail);
    PoseScript a = script_of(text), b = script_of(typed + tail);
    const Drawing da = drawn(a), db = drawn(b);
    CHECK(drawing_svg(da, 32, 32) == drawing_svg(db, 32, 32));
    REQUIRE(da.shapes.size() == 6);
    CHECK(da.shapes[1].id == "rect_1");
    CHECK(da.shapes[2].id == "ellipse_1");
    CHECK(da.shapes[2].rx == 4.5);
    CHECK(da.shapes[3].kind == DrawShape::Kind::Line);
    CHECK(da.shapes[3].stroke == "#204080");
    CHECK(da.shapes[4].fill == "none");
    CHECK(da.shapes[4].d == "M 2 30 L 8 24 L 14 30");
    CHECK(da.shapes[5].points == std::vector<f64>{18, 2, 30, 2, 24, 9});
}

TEST_CASE("Motion shapes: the «Фигура» fields rewrite the call that drew a shape") {
    const std::string source = R"(local m = require("parts")
return {
  params = { cx = 5 },
  clips = { c = function(p) end },
  draw = function(p, rng)
    canvas(32, 32)
    local red = "#ff0000"
    rect("a", { x = 1, y = 2, w = 10, h = 4, fill = "#ff0000" })
    circle("c", { x = p.cx, y = 8, r = 3 })
    ellipse("e", { x = 16, y = 16, rx = 4, ry = 2, fill = red })
    line("l", { x1 = 0, y1 = 0, x2 = 8, y2 = 8 })
    group("g", function()
      poly("tri", { points = { 0, 0, 4, 0, 2, 3 } })
    end)
    group("h", { x = 2 }, function() path("pa", { d = "M 0 0 L 4 4" }) end)
    for i = 1, 3 do rect("r" .. i, { x = i * 4, y = 20, w = 2, h = 2 }) end
    local o = { x = 0, y = 0, w = 1, h = 1 }
    rect("o", o)
    m.figure()
  end,
}
)";
    PoseScript s;
    s.set_module_reader([](std::string_view name, std::string& text, std::string& why) {
        if (name != "parts") return why = "нет", false;
        text = "return { figure = function() rect(\"mod\", { x = 0, y = 30, w = 2, h = 2 }) end }";
        return true;
    });
    {
        const bool ok = s.load(source);
        INFO(s.error());
        REQUIRE(ok);
    }
    const Drawing d = drawn(s);
    const CodeSites& code = s.code();
    // What a field of a shape writes, or why not.
    auto edit = [&](std::string_view id, std::string_view field, std::string_view text, std::string* why_out = nullptr) {
        std::string out, why;
        const bool ok = edit_shape_field(code, source, shape(d, id), field, text, out, why);
        if (why_out) *why_out = why;
        CHECK(ok == why.empty());
        CHECK(ok != out.empty());
        return out;
    };
    auto with = [&](std::string_view from, std::string_view to) {
        std::string t = source;
        const usize at = t.find(from);
        REQUIRE(at != std::string::npos);
        t.replace(at, from.size(), to);
        return t;
    };
    std::string why;

    // The fields of each kind.
    CHECK(shape_fields(DrawShape::Kind::Rect) == std::vector<std::string>{"x", "y", "w", "h", "r", "fill", "stroke", "stroke_width", "opacity"});
    CHECK(shape_fields(DrawShape::Kind::Circle) == std::vector<std::string>{"x", "y", "r", "fill", "stroke", "stroke_width", "opacity"});
    CHECK(shape_fields(DrawShape::Kind::Ellipse) == std::vector<std::string>{"x", "y", "rx", "ry", "fill", "stroke", "stroke_width", "opacity"});
    CHECK(shape_fields(DrawShape::Kind::Line) == std::vector<std::string>{"x1", "y1", "x2", "y2", "stroke", "stroke_width", "opacity"});
    CHECK(shape_fields(DrawShape::Kind::Group) == std::vector<std::string>{"x", "y", "rot", "scale", "fill", "stroke", "stroke_width", "opacity"});
    CHECK(shape_fields(DrawShape::Kind::Poly) == std::vector<std::string>{"fill", "stroke", "stroke_width", "opacity"});
    CHECK(shape_fields(DrawShape::Kind::Path) == shape_fields(DrawShape::Kind::Poly));
    // As drawn.
    CHECK(shape_field_text(shape(d, "a"), "w") == "10");
    CHECK(shape_field_text(shape(d, "a"), "fill") == "#ff0000");
    CHECK(shape_field_text(shape(d, "a"), "stroke").empty());
    CHECK(shape_field_text(shape(d, "a"), "stroke_width").empty());
    CHECK(shape_field_text(shape(d, "c"), "x") == "5");
    CHECK(shape_field_text(shape(d, "g"), "scale") == "1");
    f64 v = 0;
    CHECK(shape_field_number(shape(d, "a"), "r", v));
    CHECK(v == 0);
    CHECK_FALSE(shape_field_number(shape(d, "a"), "fill", v));
    CHECK_FALSE(shape_field_number(shape(d, "l"), "w", v));

    // A number: the literal anew; a missing field added; the same value nothing.
    CHECK(edit("a", "w", "44") == with("w = 10,", "w = 44,"));
    CHECK(edit("a", "w", " 12,5 ") == with("w = 10,", "w = 12.5,"));
    CHECK(edit("a", "r", "2") == with("fill = \"#ff0000\" })", "fill = \"#ff0000\", r = 2 })"));
    CHECK(edit("a", "w", "10") == source);
    CHECK(edit("a", "w", "10,0") == source);
    CHECK(edit("a", "r", "0") == source);
    CHECK(edit("a", "stroke_width", "1") == source);
    CHECK(edit("a", "stroke_width", "0.5") == with("fill = \"#ff0000\" })", "fill = \"#ff0000\", stroke_width = 0.5 })"));
    CHECK(edit("a", "opacity", "0.5") == with("fill = \"#ff0000\" })", "fill = \"#ff0000\", opacity = 0.5 })"));
    CHECK(edit("l", "x2", "-3") == with("x2 = 8,", "x2 = -3,"));
    // An expression keeps itself: corrected.
    CHECK(edit("c", "x", "7") == with("x = p.cx,", "x = p.cx + 2,"));
    // Checked as draw checks it.
    CHECK(edit("a", "w", "-1", &why).empty());
    CHECK(why == "«w» не меньше 0");
    CHECK(edit("c", "r", "-0.5", &why).empty());
    CHECK(why == "«r» не меньше 0");
    CHECK(edit("a", "x", "abc", &why).empty());
    CHECK(why == "«x» — число");
    CHECK(edit("a", "x", "", &why).empty());
    CHECK(why == "«x» — число");
    CHECK(edit("a", "x", "inf", &why).empty());
    CHECK(why == "«x» — конечное число");
    CHECK(edit("a", "opacity", "1.5", &why).empty());
    CHECK(why == "«opacity» от 0 до 1");

    // A colour: written as typed, a missing one added; the same colour nothing; empty and unreadable refused.
    CHECK(edit("a", "fill", "#00ff00") == with("fill = \"#ff0000\"", "fill = \"#00ff00\""));
    CHECK(edit("a", "fill", "red") == source);
    CHECK(edit("a", "fill", "#F00") == source);
    CHECK(edit("a", "fill", "none") == with("fill = \"#ff0000\"", "fill = \"none\""));
    CHECK(edit("a", "stroke", "rgb(0, 0, 255)") == with("fill = \"#ff0000\" })", "fill = \"#ff0000\", stroke = \"rgb(0, 0, 255)\" })"));
    CHECK(edit("a", "fill", "  ", &why).empty());
    CHECK(why == "пусто не пишется: «none» — без цвета, свой цвет — уберите поле в коде");
    CHECK(edit("a", "fill", "zzz", &why).empty());
    CHECK(why == "цвет «zzz» не читается (#rgb, #rrggbb, rgb(r, g, b), имя вроде red или none)");

    // A group without a table gets one; with one, a field more.
    CHECK(edit("g", "x", "3") == with("group(\"g\", function()", "group(\"g\", { x = 3 }, function()"));
    CHECK(edit("g", "fill", "#123456") == with("group(\"g\", function()", "group(\"g\", { fill = \"#123456\" }, function()"));
    CHECK(edit("g", "scale", "1") == source);
    CHECK(edit("h", "rot", "15") == with("{ x = 2 }", "{ x = 2, rot = 15 }"));
    CHECK(edit("tri", "fill", "#00ff00") == with("points = { 0, 0, 4, 0, 2, 3 } }", "points = { 0, 0, 4, 0, 2, 3 }, fill = \"#00ff00\" }"));

    // One call of a loop drew three: one number for all.
    CHECK(shape(d, "r1").site == shape(d, "r3").site);
    CHECK(shape(d, "r1").site != shape(d, "a").site);
    const std::string loop = edit("r2", "w", "3");
    CHECK(loop == with("y = 20, w = 2,", "y = 20, w = 3,"));
    {
        PoseScript again;
        again.set_module_reader([](std::string_view, std::string& text, std::string&) {
            text = "return { figure = function() end }";
            return true;
        });
        REQUIRE(again.load(loop));
        const Drawing dl = drawn(again);
        for (const char* id : {"r1", "r2", "r3"}) CHECK(shape(dl, id).w == 3);
    }

    // Not written, and why.
    auto cannot = [&](std::string_view id, std::string_view field) {
        std::string w;
        CHECK_FALSE(can_edit_shape(code, shape(d, id), field, w));
        std::string out, w2;
        CHECK_FALSE(edit_shape_field(code, source, shape(d, id), field, "1", out, w2));
        CHECK(w == w2);
        return w;
    };
    CHECK(cannot("e", "fill") == "цвет «fill» задан выражением: правится в коде");
    CHECK(cannot("l", "fill") == "у фигуры нет поля «fill»");
    CHECK(cannot("tri", "x") == "у фигуры нет поля «x»");
    CHECK(cannot("o", "w") == "поля фигуры не таблицей в вызове: правится в коде");
    CHECK(cannot("mod", "w") == "фигуру рисует код модуля: правится в нём");
    CHECK(shape(d, "mod").module);
    CHECK(shape(d, "mod").site == -1);
    std::string w;
    CHECK(can_edit_shape(code, shape(d, "e"), "rx", w));
    CHECK(can_edit_shape(code, shape(d, "c"), "fill", w));
    // A shape's call elsewhere: refused.
    DrawShape lost = shape(d, "a");
    lost.site = -1;
    CHECK_FALSE(can_edit_shape(code, lost, "w", w));
    CHECK(w == "у фигуры нет вызова в коде");

    // Each rewrite draws with its new value.
    {
        PoseScript again;
        again.set_module_reader([](std::string_view, std::string& text, std::string&) {
            text = "return { figure = function() end }";
            return true;
        });
        REQUIRE(again.load(edit("c", "x", "7")));
        CHECK(shape(drawn(again), "c").x == 7);
        REQUIRE(again.load(edit("g", "x", "3")));
        CHECK(shape(drawn(again), "g").x == 3);
        REQUIRE(again.load(edit("a", "stroke", "rgb(0, 0, 255)")));
        CHECK(shape(drawn(again), "a").stroke == "#0000ff");
    }
}
