#pragma once

// The «Интерфейс» tab: the game's screens (HUD, menus, windows), drawn the way
// Figma draws frames.
//
// Left: the game's screens and the layers of the open one (a tree; the eye
// hides a layer, the lock keeps it from being picked on the canvas; a double
// click renames). Middle: the canvas. The screen is drawn by the UI engine
// exactly as the game will draw it (the page design::screen_html makes), into
// a picture of its own that the canvas shows scaled. Around it: rulers (drag
// out of a ruler for a guide line, drag a guide back onto it to remove it),
// the pasteboard, the selection with its eight handles and size, and while
// moving or resizing the snapping lines: edges and middles of the screen,
// of the other layers and of guides, equal gaps between neighbours (pink
// lines and numbers, as in Figma). Right: the selected layer's design:
// place and size, constraints, auto layout, fills, stroke, corners, effects,
// opacity and blend, text.
//
// Tools: V select, F frame, R rectangle, O ellipse, T text (drag to draw,
// or click for a default size). Wheel: scroll; Ctrl+wheel: zoom at the
// mouse; middle button or Space+drag: pan; Shift+0 / Shift+1: 100% / fit.
// Keys on the selection: arrows nudge (Shift: 10 px), Delete removes,
// Ctrl+D duplicates, Ctrl+C/V copy and paste, Ctrl+G puts it into a frame,
// Shift+A gives it auto layout, Ctrl+] / Ctrl+[ bring forward / send back,
// Esc selects the parent, Enter the first child.
//
// Screens are the game's ui/<name>.json; every change writes it and the page
// ui/<name>.html at once (one step of the tab's history each; a drag is one
// step).

#include "forge/editor/document.h"
#include "forge/editor/ui_design.h"
#include "forge/editor/undo.h"
#include "forge/ui/drawing.h"
#include "forge/ui/ui.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace forge::editor_app {

class UiEditor {
public:
    // game_dir holds ui/ (made with a first screen when missing).
    bool init(ui::Ui& ui, const std::filesystem::path& game_dir);
    void shutdown();
    void bind(Rml::DataModelConstructor& model);
    void set_model(Rml::DataModelHandle handle) { model_ = handle; }

    // Shown or hidden with the tab: the screen is drawn only while shown.
    void set_shown(bool shown);
    void update(Rml::Context* context);
    // Mouse on the canvas and rulers. density: window pixels per event unit.
    bool handle_event(const SDL_Event& e, f32 density, Rml::Context* context);
    bool handle_key(const SDL_KeyboardEvent& k);

    editor::UndoStack& history() { return history_; }
    void undo();
    void redo();
    std::string status() const;

    // --- actions (clicks, keys, the self-test) ---
    std::vector<std::string> screens() const; // file names without .json
    bool open(const std::string& screen);
    const std::string& opened() const { return name_; }
    std::string new_screen();
    const editor::design::Screen& screen() const { return screen_; }

    enum class Tool : u8 { Select, Frame, Rectangle, Ellipse, Text };
    void set_tool(Tool tool);
    Tool tool() const { return tool_; }

    void select(const std::vector<u32>& ids);
    const std::vector<u32>& selection() const { return selection_; }
    // Adds a layer as the tool would on a click at (x, y) of the screen; its id.
    u32 add_layer(editor::design::NodeType type, f32 x, f32 y, f32 w, f32 h, u32 parent = 0);
    bool remove_selection();
    bool duplicate_selection();
    bool wrap_selection_in_frame();
    bool add_auto_layout();
    bool move_selection(f32 dx, f32 dy);
    // Changes a field of the selected layers by its name in the design panel
    // ("x", "w", "fill.0.color", "text", "layout.gap"...); false when the
    // value does not fit.
    bool set_property(const std::string& field, const std::string& value);
    // The screen's box of a layer (the screen's pixels), after layout.
    std::optional<editor::design::Rect> layer_box(u32 id) const;

    // The canvas view: zoom (1 = 100%) and where the screen's top left is
    // on the canvas (pixels of the canvas).
    f32 zoom() const { return zoom_; }
    void set_zoom(f32 zoom, f32 at_x, f32 at_y);
    void zoom_to_fit();
    // Screen pixels to canvas pixels and back.
    f32 to_canvas_x(f32 x) const { return pan_x_ + x * zoom_; }
    f32 to_canvas_y(f32 y) const { return pan_y_ + y * zoom_; }
    f32 to_screen_x(f32 x) const { return (x - pan_x_) / zoom_; }
    f32 to_screen_y(f32 y) const { return (y - pan_y_) / zoom_; }
    // The canvas element's box in the window (for tests and input).
    f32 canvas_left() const { return canvas_x_; }
    f32 canvas_top() const { return canvas_y_; }

private:
    friend class UiCommand;

    // --- files ---
    std::filesystem::path ui_dir() const { return game_dir_ / "ui"; }
    std::filesystem::path json_path(const std::string& name) const { return ui_dir() / (name + ".json"); }
    std::filesystem::path html_path(const std::string& name) const { return ui_dir() / (name + ".html"); }
    bool write(const std::string& name, const editor::design::Screen& screen) const;
    void apply(const std::string& name, const std::string& json, const std::vector<u32>& selection);
    // Records a change: the screen as it is now against before.
    void commit(const std::string& before, std::string label, std::string merge = {});

    // --- the page on the canvas ---
    void rebuild_page();
    void read_boxes();
    // The layer under a screen point: the deepest visible, unlocked one;
    // then the one to pick at the current selection's level (deep: the deepest).
    u32 hit(f32 x, f32 y, bool deep) const;
    u32 container_at(f32 x, f32 y) const;
    editor::design::Rect selection_box() const;
    editor::design::SnapTargets snap_targets() const;

    // --- view ---
    void read_canvas(Rml::Context* context);
    void refresh_view();
    void refresh_rulers();
    void refresh_overlay();
    void refresh_layers();
    void refresh_screens();
    void refresh_props();
    void dirty(const char* name);
    void dirty_all();

    // --- mouse ---
    enum class Drag : u8 { None, Pan, Move, Resize, Draw, Marquee, Guide };
    bool press(f32 mx, f32 my, u8 button, u8 clicks, Rml::Context* context);
    void drag_to(f32 mx, f32 my);
    void release();

    // Field edits.
    bool set_on(editor::design::Node& n, const std::string& field, const std::string& value);

    ui::Ui* ui_ = nullptr;
    std::filesystem::path game_dir_;
    Rml::DataModelHandle model_;
    Rml::Context* page_context_ = nullptr;
    Rml::ElementDocument* page_ = nullptr;
    bool shown_ = false;
    bool page_dirty_ = true;

    editor::Document doc_; // the history needs one; the screens live in their files
    editor::UndoStack history_{doc_};

    std::string name_;
    editor::design::Screen screen_;
    std::vector<u32> selection_;
    std::unordered_map<u32, editor::design::Rect> boxes_; // after the page's layout
    Tool tool_ = Tool::Select;
    std::vector<editor::design::Node> clipboard_;

    // View.
    f32 zoom_ = 0.5f, pan_x_ = 40, pan_y_ = 40;
    f32 canvas_x_ = 0, canvas_y_ = 0, canvas_w_ = 0, canvas_h_ = 0;
    bool fit_pending_ = true;
    f32 mouse_x_ = 0, mouse_y_ = 0; // window pixels
    bool space_down_ = false;

    // Drag.
    Drag drag_ = Drag::None;
    bool dragged_ = false;
    f32 grab_mx_ = 0, grab_my_ = 0;       // window pixels
    f32 grab_pan_x_ = 0, grab_pan_y_ = 0; // view at the grab
    std::string grab_json_;               // the screen at the grab (undo)
    struct Grabbed {
        u32 id;
        f32 x, y, w, h;
    };
    std::vector<Grabbed> grabbed_;
    editor::design::Rect grab_box_{};     // the selection's box at the grab (screen pixels)
    int handle_ = -1;                     // 0..7: tl, t, tr, r, br, b, bl, l
    u32 drawn_ = 0;                       // the layer being drawn
    int guide_ = -1;                      // the guide being dragged
    bool guide_vertical_ = true;
    std::vector<editor::design::SnapLine> snap_lines_;
    std::vector<editor::design::SnapGap> snap_gaps_;
    editor::design::Rect marquee_{};
    u32 hover_ = 0;
    u64 last_click_ns_ = 0;
    f32 last_click_x_ = 0, last_click_y_ = 0;

    // Lines on the canvas (rulers, snapping).
    struct Lines final : ui::LineSource {
        std::vector<ui::Line> list;
        u64 v = 0;
        const std::vector<ui::Line>& lines() const override { return list; }
        u64 version() const override { return v; }
    };
    Lines ruler_x_, ruler_y_, overlay_lines_;

    // --- bound to the document ---
    struct ScreenRow {
        Rml::String name, title, size;
        bool selected = false;
    };
    struct LayerRow {
        int id = 0;
        Rml::String name, icon;
        int depth = 0;
        bool selected = false, visible = true, locked = false, container = false, open = true, hidden_by_parent = false;
    };
    struct Tick {
        float at = 0;
        Rml::String label;
    };
    struct Box {
        float x = 0, y = 0, w = 0, h = 0;
    };
    struct Label {
        float x = 0, y = 0;
        Rml::String text;
    };
    struct FillRow {
        int index = 0;
        Rml::String kind, kind_name, hex, hex2, opacity, swatch, angle, image, fit;
        bool visible = true;
    };
    struct EffectRow {
        int index = 0;
        Rml::String kind, kind_name, hex, x, y, blur, spread, swatch;
        bool visible = true, shadow = true;
    };
    struct Props {
        bool any = false, many = false, root = false, text = false, container = false, in_layout = false,
             has_layout = false, has_stroke = false;
        Rml::String name, type_name, icon;
        Rml::String x, y, w, h, rotation;
        Rml::String horizontal, vertical, width_sizing, height_sizing;
        Rml::String layout_mode, gap, pad_t, pad_r, pad_b, pad_l;
        int align = 0;
        bool space_between = false, clip = false, absolute = false;
        Rml::String radius, radius_tl, radius_tr, radius_br, radius_bl;
        bool radius_mixed = false;
        Rml::String opacity, blend;
        Rml::String stroke_hex, stroke_swatch = "transparent", stroke_width, stroke_align, stroke_style;
        Rml::String content, family, size, weight, line_height, letter_spacing, text_align, text_case, decoration,
            text_hex, text_swatch = "transparent";
        bool italic = false;
    };

    std::vector<ScreenRow> m_screens_;
    std::vector<LayerRow> m_layers_;
    std::vector<Tick> m_ticks_x_, m_ticks_y_;
    std::vector<Box> m_selected_;
    Box m_sel_box_;
    bool m_handles_ = false;
    Box m_hover_;
    bool m_hovering_ = false;
    Box m_marquee_;
    bool m_marqueeing_ = false;
    std::vector<Label> m_measures_;
    Rml::String m_size_label_;
    Label m_frame_label_;
    Box m_frame_;
    std::vector<Box> m_guides_x_, m_guides_y_;
    Rml::String m_tool_ = "select";
    Rml::String m_zoom_text_;
    Rml::String m_title_;
    Rml::String m_renaming_; // a layer id or "screen:<name>" being renamed
    Rml::String m_rename_text_;
    Props m_p_;
    std::vector<FillRow> m_fills_;
    std::vector<EffectRow> m_effects_;
    std::vector<Rml::String> m_families_;
    std::vector<int> m_nine_{0, 1, 2, 3, 4, 5, 6, 7, 8}; // the 3x3 align grid
    std::vector<u32> closed_; // frames folded in the layer list
};

} // namespace forge::editor_app
