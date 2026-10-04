#pragma once

// <virtual-list>: a scrolling list that creates elements only for the rows
// on screen, so 50 000 rows cost the same as 50.
//
//   <virtual-list source="hierarchy" row-height="28">
//       <div class="row" vl-class-selected="selected" vl-style-padding-left="indent">
//           <icon vl-attr-name="icon"/>
//           <span vl-text="name"/>
//       </div>
//   </virtual-list>
//
// With cell-width="120" the list is a grid instead: as many cells of at
// least that width as fit side by side, each row-height tall (the icons of
// a file explorer). The children are then the cell template.
//
// The children are the row template. Attributes starting with vl- bind a
// part of the row to a field of the row's data:
//   vl-text="field"            the element's text
//   vl-attr-<name>="field"     an attribute (vl-attr-name on <icon> picks the icon)
//   vl-class-<class>="field"   the class is set when the field is "1" or "true"
//   vl-style-<property>="field" a style property ("24px")
// The data comes from a ListSource registered under the source name; the
// document decides how a row looks, C++ only provides values.

#include "forge/core/types.h"

#include <string>
#include <string_view>

namespace forge::ui {

class ListSource {
public:
    virtual ~ListSource() = default;
    virtual u32 count() const = 0;
    // Text of one field of one row; empty when unknown.
    virtual std::string field(u32 row, std::string_view name) const = 0;
    // Changes whenever rows change; visible rows are re-read when it does.
    virtual u64 version() const = 0;
    // "click" and "dblclick" on a row. modifiers: Rml::Input::KeyModifier bits.
    virtual void on_row_event(u32 row, std::string_view event, int modifiers) {
        (void)row, (void)event, (void)modifiers;
    }
    // A left click on the list outside any row.
    virtual void on_empty_click(int modifiers) { (void)modifiers; }
    // A right click: on a row, or outside any row (row = kNoRow). x, y: the
    // mouse in the context's pixels.
    static constexpr u32 kNoRow = ~0u;
    virtual void on_context(u32 row, float x, float y, int modifiers) { (void)row, (void)x, (void)y, (void)modifiers; }
};

// nullptr removes the source. The list does not own it.
void register_list_source(const std::string& name, ListSource* source);
ListSource* find_list_source(const std::string& name);

// Scrolls every list showing source so that row is visible.
void scroll_list_to(const std::string& source, u32 row);

} // namespace forge::ui
