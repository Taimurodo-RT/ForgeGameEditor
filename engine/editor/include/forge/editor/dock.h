#pragma once

// Docking layout of an editor screen: panels that can be dragged by their
// header to another place, put together as tabs, and resized by the gaps
// between them, around one fixed view (the level's world).
//
// The layout is a tree. A split divides its rectangle between two children,
// side by side or one above the other; a stack is a leaf showing one of its
// panels (more than one: tabs in its header). The view is a leaf of its own
// that is never moved or closed.
//
// The layout only computes rectangles; the UI places the panels' elements
// there. It is saved as a small JSON text with the user's settings.

#include "forge/core/types.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace forge::editor {

struct DockRect {
    f32 x = 0, y = 0, w = 0, h = 0;
    bool contains(f32 px, f32 py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    bool operator==(const DockRect&) const = default;
};

class DockLayout {
public:
    // Where a dragged panel goes, relative to a stack: into it as a tab
    // (Center), or beside it in a new stack.
    enum class Zone : u8 { None, Center, Left, Right, Top, Bottom };

    struct Drop {
        Zone zone = Zone::None;
        i32 stack = -1;   // index into stacks()
        DockRect preview; // the place the panel would take
    };

    // A stack after layout(): its frame, the header strip and the body.
    struct Stack {
        DockRect frame, header, body;
        bool view = false;
        std::vector<std::string> panels;
        usize active = 0;
    };
    // The gap between the two children of a split, to drag.
    struct Splitter {
        DockRect rect;
        bool column = false; // children above each other: the gap is horizontal
    };

    DockLayout();
    ~DockLayout();
    DockLayout(const DockLayout&) = delete;
    DockLayout& operator=(const DockLayout&) = delete;

    // The layout written as text, e.g.
    //   {"row":0.2,"a":{"panels":["palette"]},"b":{"column":0.7,"a":{"view":true},"b":{"panels":["log","history"]}}}
    // "row": children side by side, "column": above each other; the number is
    // the first child's share. False when the text is not a layout of exactly
    // these panels (then nothing changes).
    bool load(std::string_view json, const std::vector<std::string>& panels);
    std::string save() const;

    // Places everything into area; gap: between stacks; header: height of a
    // stack's header strip.
    void layout(const DockRect& area, f32 gap, f32 header);
    const std::vector<Stack>& stacks() const { return stacks_; }
    const std::vector<Splitter>& splitters() const { return splitters_; }
    // Increases whenever the tree changes (moves, tabs, resizes, loads).
    u64 version() const { return version_; }

    // The rectangle of a panel's body; false when the panel is a hidden tab
    // (or unknown).
    bool panel_rect(std::string_view panel, DockRect& out) const;
    DockRect view_rect() const;
    bool has_panel(std::string_view panel) const;

    // Shows a panel (makes it the open tab of its stack).
    void activate(std::string_view panel);
    // Where a panel dropped at (x, y) would go.
    Drop drop_at(f32 x, f32 y, std::string_view panel) const;
    // Moves a panel; false when the drop changes nothing.
    bool move(std::string_view panel, const Drop& drop);
    // Drags splitter i (from splitters()) to the point (x, y).
    void drag_splitter(usize index, f32 x, f32 y);

    // Smallest size a stack keeps when a splitter is dragged.
    static constexpr f32 kMinSize = 96;

    struct Node; // the tree, inside dock.cpp

private:
    void place(Node& node, const DockRect& rect);
    Node* find_stack(std::string_view panel) const;
    std::unique_ptr<Node>& slot_of(Node* node);
    void remove_panel(std::string_view panel);
    std::unique_ptr<Node> root_;
    std::vector<Stack> stacks_;
    std::vector<Node*> stack_nodes_;
    std::vector<Splitter> splitters_;
    std::vector<Node*> splitter_nodes_;
    f32 gap_ = 0, header_ = 0;
    DockRect area_;
    u64 version_ = 1;
};

} // namespace forge::editor
