#pragma once

// Drawing in documents what boxes cannot: lines and small shapes.
//
//   <lines source="scheme-wires"/>   lines given by C++ (the wires of a node
//                                    graph, a grid), with soft edges
//   <shape kind="exec" fill="true"/> a flow pin of a node: an arrow head,
//                                    outlined, or filled with fill="true";
//                                    its colour is the element's `color`
//
// Lines are in the element's pixels from its top left corner. A source says
// when its lines changed (version); the element builds its mesh again only
// then.

#include "forge/core/types.h"

#include <string>
#include <vector>

namespace forge::ui {

struct Line {
    std::vector<f32> points; // x0, y0, x1, y1…
    u32 color = 0xffffffffu; // RGBA, red in the high byte
    f32 width = 2;
};

class LineSource {
public:
    virtual ~LineSource() = default;
    virtual const std::vector<Line>& lines() const = 0;
    virtual u64 version() const = 0;
};

// nullptr removes the source. The element does not own it.
void register_line_source(const std::string& name, LineSource* source);

// Points of a node graph's wire from (x0, y0) to (x1, y1): it leaves to the
// right and comes in from the left, curving between (a cubic Bézier, as in
// Unreal's Blueprints). Appended to out.
void wire_curve(f32 x0, f32 y0, f32 x1, f32 y1, std::vector<f32>& out);

// The distance from (x, y) to a line's points (for clicks on wires).
f32 distance_to(const std::vector<f32>& points, f32 x, f32 y);

} // namespace forge::ui
