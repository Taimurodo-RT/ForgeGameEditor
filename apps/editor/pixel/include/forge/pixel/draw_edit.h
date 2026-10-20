#pragma once

// Shapes drawn with the mouse and a shape's own fields (part 4е of docs/models/движение-модель.md, «Фигуры мышью»):
// the calls the tools of «Векторная» write into the code of a drawing, and what the «Фигура» section of a part rewrites
// in the call that drew it. Where a new call goes is code_sites.h (insert_drawing_line); every rewrite leaves the rest
// of the file byte for byte.
//
//   NewShape s{NewShape::Kind::Rect, {{10, 6}, {22, 14}}, "#5968e8"};
//   shape_call(s, "rect_1", text, why);   // rect("rect_1", { x = 10, y = 6, w = 12, h = 8, fill = "#5968e8" })
//   edit_shape_field(code, source, shape, "w", "44", out, why);   // { …, w = 12, … } → { …, w = 44, … }

#include "forge/pixel/code_sites.h"
#include "forge/pixel/draw.h"

#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace forge::pixel {

// A point of a tool, in picture units: to tenths, or on the nearest line of a grid `step` apart when step is above 0.
f64 tool_round(f64 v, f64 step);
vector::Point tool_point(vector::Point p, f64 step);
// A part's move dragged with «Привязка»: whole steps of the grid (the move as it is when step is 0).
f64 snap_steps(f64 v, f64 step);

inline constexpr const char* kShapeColor = "#5968e8"; // the colour of new shapes until another is chosen

struct NewShape {
    enum class Kind : u8 { Rect, Ellipse, Line, Path, Poly };
    Kind kind = Kind::Rect;
    std::vector<vector::Point> points; // rect and ellipse: two corners; line: its ends; path and poly: the pen's points
    std::string color = kShapeColor;   // the fill of a rect, an ellipse and a poly, the stroke of a line and a path
};
// "rect", "ellipse", "line", "path", "poly".
const char* new_shape_word(NewShape::Kind k);
// The id of a new shape: its word and the least number from 1 that no part has (`taken`, the ids of all drawings).
std::string new_shape_id(NewShape::Kind k, const std::set<std::string>& taken);
// Those ids: the parts of the main picture (of an SVG too) and every id the code's drawings give, the main one's and
// the others'. A shape in a group hidden or of opacity 0 is no part (ThorVG draws nothing in such a group), but its id
// is in its drawing all the same.
std::set<std::string> taken_shape_ids(const Rig& main, const std::vector<const Drawing*>& drawings);
// The pen's points: a click where the last point is adds none.
void add_pen_point(std::vector<vector::Point>& points, vector::Point p);
// The call that draws it, as the code writes it (numbers by format_number): rect and ellipse from two corners in any
// direction, path open with fill "none", poly closed. False with why when it has no size: a rect or an ellipse without
// width or height, a line of no length, a path of fewer than two points, a poly of fewer than three.
bool shape_call(const NewShape& s, const std::string& id, std::string& out, std::string& why);

// The «Фигура» section: the fields of a shape's call in the order shown; poly and path have no fields of place, a line
// no fill.
std::vector<std::string> shape_fields(DrawShape::Kind k);
bool shape_field_is_color(std::string_view field);
// The value it was drawn with: a number field's number (false for a colour, a field the shape has not, a stroke_width
// not written); as text, a number by format_number, a colour "#rrggbb" or "none", "" for SVG's own (no fill or stroke
// written, no stroke_width).
bool shape_field_number(const DrawShape& s, std::string_view field, f64& out);
std::string shape_field_text(const DrawShape& s, std::string_view field);
// Whether the field can be written into the call that drew the shape: false with why (drawn by a module, no call, the
// fields not a table in the call, a colour written as an expression, not a field of the shape).
bool can_edit_shape(const CodeSites& code, const DrawShape& s, std::string_view field, std::string& why);
// The field written from the text typed: a number checked as draw checks it (finite; w, h, r, rx, ry and stroke_width
// not below 0; opacity 0 … 1) and rewritten (a literal anew, an expression with a correction, a missing field added, a
// group without a table given one); a colour checked as draw reads it and written as typed. The same value: out is the
// source. False with why.
bool edit_shape_field(const CodeSites& code, std::string_view source, const DrawShape& s, std::string_view field,
                      std::string_view text, std::string& out, std::string& why);

} // namespace forge::pixel
