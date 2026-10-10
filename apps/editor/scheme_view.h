#pragma once

// «Схема» of the «Логика» tab: each link as a node graph the author edits,
// the way Unreal's Blueprints are edited.
//
// Every link is a frame. Its scheme starts at «Когда» (the link happens) and
// goes on through white flow wires: «Если», «Только в первый раз»,
// «Сделать», «Звук»… Coloured wires carry values (a number, a thing, yes or
// no) from the right side of one node to the left side of another. A link
// that has no scheme of its own shows the one its verb and refinements make;
// the first change gives it its own (logic::set_scheme keeps it plain when
// the change is just a refinement).
//
//   drag a node             moves it
//   drag from a pin         a wire; let go on another pin to join them, on
//                           the empty frame to pick a node that is joined at
//                           once (a click on a pin, then on another, works too)
//   Alt+click on a pin      takes its wires away
//   click a wire            selects it (Delete takes it away)
//   right click, «+ Нода»   the list of nodes, with a search
//   Delete                  the selected node or wire («Когда» stays)
//   «Упорядочить»           lays the frame's nodes out in order
//
// It looks like a Blueprint: a grid under it, wires as curves (drawn by
// <lines>), white flow wires between arrow pins, coloured value wires
// between round ones, nodes as wide as their rows. What happens reads left
// to right; the values a step needs lie under it, to its left. A link whose
// nodes were never moved is shown laid out so; nodes snap to the grid.
//
// A thing may have a scheme of its own, with no link: what each copy does by
// itself («При старте», «Каждый шаг», «При ударе»…). It is a frame too, under
// the links' ones; «+ Своя схема вещи» starts one. Frames are named by ids:
// a link's or a thing scheme's (they share them).
//
// Values that are not wired are typed into the node: numbers, text, yes/no,
// a thing or an action from a list. A node the scheme cannot be built with
// is marked and says why; the rest of the game still runs.

#include "forge/logic/logic.h"
#include "forge/script/nodes.h"
#include "forge/ui/drawing.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace forge::editor_app {

class LogicEditor;

class SchemeView {
public:
    explicit SchemeView(LogicEditor& editor);
    ~SchemeView();
    SchemeView(const SchemeView&) = delete;
    SchemeView& operator=(const SchemeView&) = delete;

    void bind(Rml::DataModelConstructor& model);
    void set_model(Rml::DataModelHandle handle) { model_ = handle; }
    const script::NodeLibrary& nodes() const { return nodes_; }

    // The tab rebuilds (the logic changed, the mode is «Схема»).
    void rebuild();
    // Where the scheme pane is on screen (for wires following the mouse).
    void update(Rml::Context* context);
    bool handle_event(const SDL_Event& e, f32 density);
    bool handle_key(const SDL_KeyboardEvent& k);
    bool dragging() const { return drag_ != Drag::None; }
    // How far the view is panned (the world is drawn moved by it).
    f32 pan_x() const { return pan_x_; }
    f32 pan_y() const { return pan_y_; }

    // --- actions (the mouse, keys and the self-test) ---
    // The scheme of a link as shown (its own, or the one it would have).
    const script::Graph* graph(u32 link) const;
    // A node from the library at (x, y) of the link's scheme; 0 when it
    // cannot be added.
    u32 add_node(u32 link, const std::string& def, f32 x, f32 y);
    bool remove_node(u32 link, u32 node);
    bool move_node(u32 link, u32 node, f32 x, f32 y);
    // The nodes laid out in order (left to right, values under their step).
    bool arrange(u32 link);
    // A whole scheme put in at once (one step of the history), laid out in
    // order when asked.
    bool set_graph(u32 link, const script::Graph& g, const std::string& label, bool arrange = false);
    // Joins an output (a flow exit or a value) to an input; false when they
    // do not fit. A flow exit or a value input keeps one wire: the new one.
    bool connect(u32 link, u32 from, const std::string& from_pin, u32 to, const std::string& to_pin);
    bool disconnect(u32 link, usize wire);
    // The wires of a pin taken away (Alt+click).
    bool unplug(u32 link, u32 node, bool out, const std::string& pin);
    bool set_value(u32 link, u32 node, const std::string& pin, const std::string& value);
    // A click on a pin: the first is remembered, the second joins them.
    void click_pin(u32 link, u32 node, bool out, const std::string& pin);
    bool pin_pending() const { return pend_link_ != 0; }
    void select_node(u32 link, u32 node);
    void select_wire(u32 link, usize wire);
    u32 selected_node() const { return sel_node_; }
    // The list of nodes: opened at (x, y) of the link's scheme; picking one
    // adds it there (and joins a pending pin to it).
    void open_palette(u32 link, f32 x, f32 y);
    void close_palette();
    bool palette_open() const { return m_palette_; }
    void set_search(const std::string& text);
    usize palette_items() const;
    bool palette_pick(const std::string& def);
    // A pick field: its list opened (things, actions, sounds), then a choice.
    void open_pick(u32 link, u32 node, const std::string& pin);
    bool picking() const { return m_choosing_; }
    bool pick(const std::string& value);
    // Why a node cannot work ("" when it can).
    std::string node_problem(u32 link, u32 node) const;
    // Back to what the verb and refinements make; a thing's scheme is taken
    // away.
    bool reset(u32 link);
    // The things that may get a scheme of their own listed («+ Своя схема
    // вещи»); a pick starts one.
    void open_things();
    // Scrolls so the frame is at the top.
    void focus(u32 frame);
    // Pans so the node is in view, when it is not.
    void reveal(u32 link, u32 node);
    // A thing scheme's frame (not a link's).
    bool own(u32 frame) const;
    // Selects a frame: a link's selects the link.
    void select_frame(u32 frame);

    struct PinView {
        Rml::String id, title, kind; // kind: "flow" or the value type's name
        Rml::String field;           // "" none, "text", "number", "check", "pick"
        Rml::String value;
        Rml::String list;            // pick: "thing", "action", "cue"
        bool on = false;             // a pin there (rows may have one side only)
        bool wired = false, pending = false, check = false;
        bool operator==(const PinView&) const = default;
    };
    struct RowView {
        PinView in, out;
        bool operator==(const RowView&) const = default;
    };
    struct NodeView {
        Rml::String id, title, icon, tone, problem, help;
        int link = 0, uid = 0;
        float x = 0, y = 0, w = 0, h = 0;
        bool selected = false, lit = false, fixed = false, warning = false;
        bool hidden = false; // a free place in the list (out of view)
        std::vector<RowView> rows;
        bool operator==(const NodeView&) const = default;
    };
    // A wire: its curve's points in the pane (drawn by <lines source="sc-wires">).
    struct WireView {
        Rml::String id, kind;
        int link = 0, index = 0;
        std::vector<f32> points;
        bool selected = false, lit = false, flow = false, pending = false;
    };
    struct FrameView {
        int id = 0;
        Rml::String phrase, problem, note;
        float x = 0, y = 0, w = 0, h = 0;
        bool selected = false, lit = false, custom = false;
        bool own = false; // a thing's scheme
        bool operator==(const FrameView&) const = default;
    };
    struct OptionView {
        Rml::String id, name;
        bool operator==(const OptionView&) const = default;
    };
    struct PaletteItem {
        Rml::String id, title, icon, help, group, tone;
        bool first = false; // the first of its group: the group's name above it
        bool operator==(const PaletteItem&) const = default;
    };

    const std::vector<NodeView>& node_views() const { return m_nodes_; }
    const std::vector<WireView>& wire_views() const { return m_wires_; }
    const std::vector<FrameView>& frame_views() const { return m_frames_; }
    // The wire under a point of the pane (-1: none) and its frame.
    int wire_at(f32 x, f32 y, u32& link) const;

private:
    enum class Drag { None, Pan, Node, Wire };
    // A node's look: its title, pins in rows and size.
    struct NodeBox {
        Rml::String title;
        std::vector<PinView> left, right;
        f32 w = 0, h = 0;
    };
    // A node's look kept between redraws, with the row of each pin.
    struct Look {
        NodeBox box;
        std::map<std::string, int> in_row, out_row;
    };
    struct Lines : ui::LineSource {
        std::vector<ui::Line> list;
        u64 counter = 0;
        const std::vector<ui::Line>& lines() const override { return list; }
        u64 version() const override { return counter; }
    };
    struct Place {
        f32 x = 0, y = 0;    // the frame's corner on screen
        f32 min_x = 0, min_y = 0; // the scheme's corner
    };

    // The graph to change: the shown one, copied.
    script::Graph editable(u32 link) const;
    // moved_only: only places changed, so the logic needs no compiling again.
    void commit(u32 link, const script::Graph& g, const std::string& label, const std::string& merge = {}, bool moved_only = false);
    const script::PinDef* in_pin(const script::Graph& g, u32 node, const std::string& pin) const;
    const script::PinDef* out_pin(const script::Graph& g, u32 node, const std::string& pin) const;
    bool known(u32 frame) const;
    bool is_exit(const script::Graph& g, u32 node, const std::string& pin) const;
    bool fits(const script::Graph& g, u32 from, const std::string& from_pin, u32 to, const std::string& to_pin) const;
    // Screen point to scheme point of a link.
    bool to_scheme(u32 link, f32 sx, f32 sy, f32& x, f32& y) const;
    u32 frame_at(f32 sx, f32 sy) const;
    void rebuild_palette();
    void prefill(u32 link, script::GraphNode& n) const;
    std::string node_title(u32 link, const script::GraphNode& n, const script::NodeDef* d) const;
    std::string out_title(u32 link, const script::GraphNode& n, const script::PinDef& p) const;
    std::string option_name(const std::string& list, const std::string& id) const;
    // «Где появиться» of «Перейти на уровень»: the level's spawn point and its areas.
    std::vector<OptionView> arrivals(const std::string& level) const;
    NodeBox box_of(u32 link, const script::Graph& g, const script::GraphNode& n,
                   const std::set<std::pair<u32, std::string>>& wired_in) const;
    static std::set<std::pair<u32, std::string>> wired_inputs(const script::Graph& g);
    void lay_out(u32 link, script::Graph& g) const;
    void rebuild_grid();
    // The view again (panned, a node dragged); the schemes as they are.
    void redraw();
    bool place_nodes(std::vector<NodeView> fresh); // true: the list changed
    void shift_node(const NodeView& v);
    void unshift_node();
    void set_pan(f32 x, f32 y);
    static u32 wire_color(const std::string& kind);
    void dirty(const char* name) {
        if (model_) model_.DirtyVariable(name);
    }

    LogicEditor& ed_;
    Rml::DataModelHandle model_;
    script::NodeLibrary nodes_;
    std::map<u32, script::Graph> shown_; // per link, while the view is up
    std::map<u32, Place> places_;
    std::map<u32, std::map<u32, Look>> looks_; // per frame, per node
    std::map<std::pair<u32, u32>, std::pair<std::string, bool>> node_problems_; // (link, node) -> text, warning

    u32 sel_link_node_ = 0, sel_node_ = 0;
    u32 sel_wire_link_ = 0;
    usize sel_wire_ = 0;
    bool wire_selected_ = false;
    // A pin waiting for its other end.
    u32 pend_link_ = 0, pend_node_ = 0;
    bool pend_out_ = false;
    std::string pend_pin_;
    // The list of nodes.
    u32 pal_link_ = 0;
    f32 pal_x_ = 0, pal_y_ = 0;
    std::string search_;

    Drag drag_ = Drag::None;
    bool dragged_ = false;
    u32 drag_link_ = 0, drag_node_ = 0;
    f32 grab_mx_ = 0, grab_my_ = 0, grab_x_ = 0, grab_y_ = 0;
    f32 mouse_x_ = 0, mouse_y_ = 0; // over the pane
    f32 screen_x_ = 0, screen_y_ = 0; // the mouse on screen, last seen
    f32 pan_x_ = 0, pan_y_ = 0;
    f32 cull_x_ = 0, cull_y_ = 0; // the pan when the view was last drawn
    f32 pane_x_ = 0, pane_y_ = 0;   // the pane on screen
    f32 pane_w_ = 0, pane_h_ = 0;
    u32 pick_link_ = 0, pick_node_ = 0;
    std::string pick_pin_;
    bool picking_thing_ = false; // the list is of things for a scheme
    u32 sel_own_ = 0;            // a thing scheme's frame selected
    Rml::Context* context_ = nullptr;
    Lines wires_, grid_;

    // Model mirrors
    std::vector<NodeView> m_nodes_;
    std::vector<WireView> m_wires_;
    std::vector<FrameView> m_frames_;
    std::vector<OptionView> m_things_, m_actions_, m_cues_, m_levels_;
    std::vector<PaletteItem> m_palette_items_;
    bool m_palette_ = false, m_choosing_ = false;
    std::vector<OptionView> m_choices_;
    float m_choose_x_ = 0, m_choose_y_ = 0;
    float m_pal_x_ = 0, m_pal_y_ = 0;
    float m_pan_x_ = 0, m_pan_y_ = 0;     // where the world is placed
    float m_slide_x_ = 0, m_slide_y_ = 0; // and slid from there while the view is dragged
    Rml::String m_search_, m_tip_;
};

} // namespace forge::editor_app
