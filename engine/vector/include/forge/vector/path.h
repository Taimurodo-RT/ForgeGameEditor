#pragma once

// Vector shapes as paths, for the morph and for whoever draws them (step 1 of the vector model,
// docs/models/вектор-модель.md). Nothing is drawn here: ThorVG in «Векторная», lunasvg in the interface, or a GPU
// layer later draws what comes out.
//
// A path is contours of cubic Bézier curves. What SVG writes as lines, quadratic curves and arcs is read as cubics,
// so two shapes can be mixed point by point. A closed contour always ends exactly where it starts: the line `Z` draws
// back to the start is a curve of its own.
//
// The numbers come out the same on every system: only + − × ÷, the square root and trig.h.

#include "forge/core/types.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace forge::vector {

struct Point {
    f64 x = 0, y = 0;
    bool operator==(const Point&) const = default;
};

// One curve from the point before it: two control points and the end.
struct Cubic {
    Point c1, c2, to;
    bool operator==(const Cubic&) const = default;
};

struct Contour {
    Point start;
    std::vector<Cubic> curves;
    bool closed = false;
    bool operator==(const Contour&) const = default;
    // Where curve i begins: the start, or the end of the curve before.
    Point from(usize i) const { return i == 0 ? start : curves[i - 1].to; }
};

struct Path {
    std::vector<Contour> contours;
    bool operator==(const Path&) const = default;
};

// A straight line as a curve: control points at a third and two thirds of the way.
Cubic line(Point from, Point to);
// The point of a curve at t (0 … 1); the ends are exact.
Point point_at(Point from, const Cubic& c, f64 t);
// The curve cut at t into two that draw the same (de Casteljau).
void split(Point from, const Cubic& c, f64 t, Cubic& first, Cubic& second);
// The area a contour encloses, with a sign for the way it goes round: positive clockwise on the screen (SVG's y goes
// down). Exact for cubic curves. An open contour counts as closed by a straight line back to its start.
f64 area(const Contour& c);

// The share of a path from `from` to `to` of its whole length, its contours one line after another, as ThorVG 1.1.2
// draws a shape's trimpath(from, to, false) (RenderTrimPath): every contour comes out open, a closed one's way back to
// its start a line like the others; a curve is cut where ThorVG's estimate of its length reaches the cut (a line's
// length is ThorVG's quick estimate too: the longer side and 3/8 of the shorter). `lines` says whether a straight curve
// (as line() makes it) is a line to ThorVG, as it is in a shape it read from SVG, or a curve, as in a path given to it
// curve by curve (a morph). Worked out in single precision, as ThorVG does, so the points are its own. No path when
// from ≥ to, outside 0 … 1 or the path has less than a curve.
Path trim(const Path& path, f64 from, f64 to, bool lines);

// --- SVG ---

// The path of an SVG `d` attribute: every command (M L H V C S Q T A Z; upper case absolute, lower case relative),
// numbers as SVG writes them (1e3, -.5, 1-2 for two numbers, flags without spaces in arcs).
// An error stops reading where it is, as SVG draws a path up to its error: `out` keeps the path before it, the result
// is false and why says what and where. An empty `d` is no path and no error. Contours without curves (a lone M) are
// left out.
bool parse_path(std::string_view d, Path& out, std::string* why = nullptr);
// The path as `d`: M, C and Z, each number written shortest so that reading it back gives the same number.
std::string to_d(const Path& path);
// The same with a curve that line() makes written as L when `lines`: as ThorVG holds the lines of a share trim() cuts.
std::string to_d(const Path& path, bool lines);

// SVG's basic shapes as paths, laid out as SVG 2 says: they start and go round where an author expects, so a morph
// between them turns the expected way. Sizes of 0 or less give no path.
// rx and ry of a rectangle as SVG resolves them (one missing: the other); here they are held to half the size.
Path rect(f64 x, f64 y, f64 w, f64 h, f64 rx = 0, f64 ry = 0);
Path ellipse(f64 cx, f64 cy, f64 rx, f64 ry);
Path circle(f64 cx, f64 cy, f64 r);
// polyline (open) and polygon (closed). Fewer than two points: no path.
Path polyline(std::span<const Point> points, bool closed);
// The `points` attribute: numbers in pairs. An odd number leaves the last one out, false, as SVG does.
bool parse_points(std::string_view points, std::vector<Point>& out, std::string* why = nullptr);

} // namespace forge::vector
