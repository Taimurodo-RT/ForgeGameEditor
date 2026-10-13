#pragma once

// The outline of an SVG file as written: its elements, their ids and attributes, and how they nest («Пиксельные»,
// step 2 of docs/models/вектор-модель.md). ThorVG draws the picture; this is read for what ThorVG does not draw (named
// to the author instead of drawn otherwise) and for the shapes a pose script morphs into, which are often hidden, so
// ThorVG never builds them.

#include "forge/core/types.h"
#include "forge/vector/path.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace forge::pixel {

struct SvgElement {
    std::string tag; // without a namespace prefix: "svg:g" is "g"
    std::string id;
    i32 parent = -1; // index in SvgOutline::elements; -1 for the root
    u32 line = 1;
    std::vector<std::pair<std::string, std::string>> attrs; // as written, entities decoded

    const std::string* attr(std::string_view name) const;
    // A presentation property: the attribute, or the same name in `style` (which wins, as in CSS).
    std::string property(std::string_view name) const;
};

struct SvgOutline {
    std::vector<SvgElement> elements; // in document order, the root first
    std::string error;                // why the file does not read (Russian, with the line); empty when it does

    i32 find(std::string_view id) const; // -1 when no element has that id
    // How the element is named to the author: «id» or the tag and its line.
    std::string name_of(i32 index) const;
};

SvgOutline scan_svg(std::string_view text);

// What ThorVG draws otherwise or not at all, one Russian line per element, in document order.
std::vector<std::string> unsupported(const SvgOutline& svg);

// The element's outline in its own coordinates (path, rect, circle, ellipse, line, polyline, polygon); false with why
// for anything else.
bool shape_path(const SvgOutline& svg, i32 index, vector::Path& out, std::string& why);

// The `transform` attributes from the root down to the element: two elements with equal chains share coordinates.
std::string transform_chain(const SvgOutline& svg, i32 index);

} // namespace forge::pixel
