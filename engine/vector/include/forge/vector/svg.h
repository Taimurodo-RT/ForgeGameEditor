#pragma once

// The outline of an SVG file as written: its elements, their ids and attributes, and how they nest. Nothing here
// draws. «Векторная» read it for what ThorVG does not draw and for the shapes a pose script morphs into (step 2 of
// docs/models/вектор-модель.md); «Ресурсы» for what lunasvg does not draw (step 3).

#include "forge/core/types.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace forge::vector {

struct SvgElement {
    std::string tag;    // without a namespace prefix: "svg:g" is "g"
    std::string prefix; // the prefix as written: "inkscape" for <inkscape:grid>, empty for <g>
    std::string id;
    i32 parent = -1; // index in SvgOutline::elements; -1 for the root
    u32 line = 1;
    // Where it is in the text (past a UTF-8 BOM): the '<' of its start tag, past that tag's '>', past its end tag (the
    // start tag's end for <… />).
    usize begin = 0, open_end = 0, end = 0;
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
    // Whether an element above this one has that tag.
    bool inside(i32 index, std::string_view tag) const;
};

SvgOutline scan_svg(std::string_view text);

} // namespace forge::vector
