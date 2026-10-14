#pragma once

// What ThorVG does not draw in an SVG, and the shapes a pose script morphs into, which are often hidden, so ThorVG
// never builds them («Пиксельные», step 2 of docs/models/вектор-модель.md). The outline itself is forge::vector's.

#include "forge/core/types.h"
#include "forge/vector/path.h"
#include "forge/vector/svg.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace forge::pixel {

using vector::scan_svg;
using vector::SvgElement;
using vector::SvgOutline;

// What ThorVG draws otherwise or not at all, one Russian line per element, in document order.
std::vector<std::string> unsupported(const SvgOutline& svg);

// The element's outline in its own coordinates (path, rect, circle, ellipse, line, polyline, polygon); false with why
// for anything else.
bool shape_path(const SvgOutline& svg, i32 index, vector::Path& out, std::string& why);

// The `transform` attributes from the root down to the element: two elements with equal chains share coordinates.
std::string transform_chain(const SvgOutline& svg, i32 index);

} // namespace forge::pixel
