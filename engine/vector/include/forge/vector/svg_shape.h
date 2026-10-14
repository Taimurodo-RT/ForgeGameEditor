#pragma once

// The shapes of an SVG file as written, for a morph: an element's outline in its own coordinates, the look its
// attributes give it, and whether two elements share coordinates. «Пиксельные» morph with them in a pose script
// (step 2 of docs/models/вектор-модель.md), a live picture of the interface and a game's script on its screens
// (step 4). Nothing here draws.

#include "forge/vector/morph.h"
#include "forge/vector/svg.h"

#include <string>
#include <string_view>

namespace forge::vector {

// The element's outline in its own coordinates (path, rect, circle, ellipse, line, polyline, polygon); false with why
// for anything else.
bool shape_path(const SvgOutline& svg, i32 index, Path& out, std::string& why);

// The `transform` attributes from the root down to the element: two elements with equal chains share coordinates.
std::string transform_chain(const SvgOutline& svg, i32 index);

// A property as the element shows it: its own or its nearest ancestor's (fill, stroke and their kin inherit).
std::string inherited(const SvgOutline& svg, i32 index, std::string_view name);

// The element or one above it has display: none.
bool display_none(const SvgOutline& svg, i32 index);

// A number as SVG writes one ("0.5", "50%" is 0.5); fallback when it does not read.
f64 svg_number(const std::string& text, f64 fallback);

// "#rgb", "#rrggbb", "rgb(r, g, b)" and a few names into 0 … 255 channels; false for anything else.
bool parse_svg_color(std::string_view text, u8 rgb[3]);

// The look of an element read from its attributes and its ancestors'. What does not read as a colour (a gradient,
// currentColor) stays as `base` has it.
Look look_from_svg(const SvgOutline& svg, i32 index, const Look& base = {});

} // namespace forge::vector
