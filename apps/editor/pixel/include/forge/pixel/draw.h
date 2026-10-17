#pragma once

// A character's picture drawn by its code («Рисунок из кода (часть 3а)» of docs/models/движение-модель.md): the `draw`
// section of «Имя.luau» calls rect, circle, ellipse, line, poly, path and group, in the order they are painted, as
// Motion Canvas writes the nodes of a scene in code. The drawing becomes the text of an SVG, which the rig loads as it
// loads a file of «Ресурсы»: parts, pivots, morphs and baking are the same. No file is written; the code is the source.
//
//   draw = function(p, rng)
//     canvas(32, 32)                                      -- the picture's units; the frame's size without it
//     group("body", function()
//       rect("torso", { x = 10, y = 14, w = 12, h = 11, r = 3, fill = "#4a7bd0" })
//       circle("head", { x = 16, y = 9, r = 6, fill = "#f0c090", pivot = { 16, 14 } })
//     end)
//     path("mouth_open", { d = "M 15 11 L 19 11 L 17 13 Z", fill = "#802020", hidden = true })
//   end,

#include "forge/core/types.h"
#include "forge/vector/path.h"

#include <string>
#include <vector>

namespace forge::pixel {

class Rig;
struct PixelDoc;

inline constexpr u32 kMaxDrawShapes = 10000;
inline constexpr u32 kMaxDrawDepth = 32; // groups inside groups

struct DrawShape {
    enum class Kind : u8 { Rect, Circle, Ellipse, Line, Poly, Path, Group };
    Kind kind = Kind::Rect;
    std::string id;  // empty: drawn, but not a part
    i32 parent = -1; // the group it is drawn in, in Drawing::shapes
    // Rect: x, y (its top left), w, h, r. Circle: x, y (its centre), r. Ellipse: x, y, rx, ry. Line: x1, y1, x2, y2.
    // Group: x, y, rot, scale, where it stands at rest.
    f64 x = 0, y = 0, w = 0, h = 0, r = 0, rx = 0, ry = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0, rot = 0, scale = 1;
    std::vector<f64> points; // Poly: x1, y1, x2, y2, …
    std::string d;           // Path, as written
    std::string fill, stroke; // "#rrggbb" or "none"; empty: SVG's own (a black fill, a stroke on a line only)
    f64 stroke_width = -1;    // below 0: SVG's own (1)
    std::vector<f64> dash;    // stroke-dasharray; empty: none
    std::string cap, join;    // stroke-linecap ("butt", "round", "square"), stroke-linejoin ("miter", "round", "bevel"); empty: SVG's own
    f64 opacity = 1;
    bool hidden = false;      // display: none — a shape to morph into
    bool has_pivot = false;
    vector::Point pivot;      // in the picture's units
    u32 line = 0;             // the call's line in the script
    bool operator==(const DrawShape&) const = default;
};

struct Drawing {
    bool canvas = false; // canvas(w, h) was called
    f64 width = 0, height = 0;
    u32 canvas_line = 0; // its line in the script
    std::vector<DrawShape> shapes; // in the order drawn: a group before what is drawn in it
    bool operator==(const Drawing&) const = default;
};

// The SVG of a drawing: canvas(w, h) units, or w × h (the frame's) when the code set none. The same drawing, the same
// text, byte for byte.
std::string drawing_svg(const Drawing& d, u32 w, u32 h);

// The pivot the drawing gives a part, by its id; false when it gives none.
bool drawn_pivot(const Drawing& d, const std::string& id, vector::Point& out);

} // namespace forge::pixel
