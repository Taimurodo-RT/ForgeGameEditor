#pragma once

// The morph: one shape turning into another (step 1 of the vector model, docs/models/вектор-модель.md). A pair is
// prepared once, then mixed at any k for each frame: k = 0 is the first shape, k = 1 the second, between them a shape
// on the way. The interface, «Векторная» and a game's own scenes use the same pair; whoever draws takes the points
// (ThorVG) or `d` (lunasvg).
//
//   const vector::Morph pair = vector::prepare(closed_mouth, open_mouth);
//   vector::mix(pair, vector::ease(vector::Ease::Smooth, t), mouth); // each frame; no allocation once mouth has its size

#include "forge/vector/path.h"

#include <string>

namespace forge::vector {

// A fill or a stroke: the colour as the SVG writes it (sRGB, each channel 0 … 1) and its own opacity (fill-opacity,
// stroke-opacity). Off is SVG's none.
struct Paint {
    bool on = false;
    f64 r = 0, g = 0, b = 0;
    f64 opacity = 1;
    bool operator==(const Paint&) const = default;
};
// An on paint from 0 … 255 channels.
Paint rgb8(u8 r, u8 g, u8 b, f64 opacity = 1);
// "#rrggbb", or "none" when off, as an SVG attribute takes it.
std::string hex(const Paint& p);

struct Look {
    Paint fill{true, 0, 0, 0, 1}; // SVG's default: filled black, no stroke
    Paint stroke;
    f64 stroke_width = 1;
    f64 opacity = 1; // of the whole shape
    bool operator==(const Look&) const = default;
};

struct Shape {
    Path path;
    Look look;
    bool operator==(const Shape&) const = default;
};

// A pair made ready: both paths have the same contours with as many curves each, so a mix is point by point.
struct Morph {
    Shape from, to;
};

// Makes a pair:
// - a closed contour that does not end at its start gets the straight line back as a curve;
// - contours pair by their order; a contour one side lacks is a point at the centre of the other side's contour there:
//   that contour shrinks into its own centre (or grows out of it the other way);
// - closed contours go round the same way: the second is turned round if it goes the other way; open ones pair their
//   nearer ends;
// - in each pair the side with fewer curves cuts its curves into pieces, the longer ones into more, until both have as
//   many;
// - the start of a closed contour of the second shape moves to where the points of the pair are closest.
// Each shape draws exactly as before: the same curves, cut into pieces and started elsewhere.
Morph prepare(const Shape& from, const Shape& to);

// The pair at k into out (its arrays are reused frame to frame).
// - Points: from at 0, to at 1, on a straight line between; k past the ends (an easing that overshoots) goes on past.
// - Colours, opacities and the stroke width: mixed at k held to 0 … 1. A paint off on one side is the other side's
//   colour at opacity 0 there, so it fades in or out; at k = 0 and 1 the paint is exactly that side's.
// - A contour is closed as `from` below k = 0.5 and as `to` from it.
// A pair not made by prepare() mixes only the contours and curves both sides have.
void mix(const Morph& m, f64 k, Shape& out);
Shape mix(const Morph& m, f64 k);

} // namespace forge::vector
