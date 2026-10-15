#pragma once

// What ThorVG does not draw in an SVG, and the shapes a pose script morphs into, which are often hidden, so ThorVG
// never builds them («Векторная», step 2 of docs/models/вектор-модель.md). The outline itself is forge::vector's.

#include "forge/core/types.h"
#include "forge/vector/path.h"
#include "forge/vector/svg.h"
#include "forge/vector/svg_shape.h"

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

// The shapes a morph reads (forge::vector's, shared with the interface's live pictures).
using vector::shape_path;
using vector::transform_chain;

} // namespace forge::pixel
