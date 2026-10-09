#pragma once

// The «Уровень» tab: paints a game's streamed world with tiles. Panels
// (palette, minimap, properties, history, log) dock around the world view
// and can be dragged by their headers; the layout is remembered.
//
// Everything game-specific comes from a forge::level::LevelModule, so the
// same tab serves any game; this app opens «Старая шахта».

#include "forge/editor/document.h"
#include "forge/editor/undo.h"
#include "forge/level/level.h"
#include "forge/level/light.h"
#include "forge/level/object_edit.h"
#include "forge/level/physics.h"
#include "forge/level/tile_edit.h"
#include "forge/render/camera.h"
#include "forge/render/sprite_batch.h"
#include "forge/render/sprite_renderer.h"
#include "forge/ui/ui.h"

#include "demo_art.h"
#include "dock_view.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace forge::editor_app {

enum class Tool : u8 { Brush, Line, Rect, Fill, Eraser, Picker };
// What the tab edits: the icons over the level. Zones say when they come.
enum class Mode : u8 { Select, Tiles, Objects, Physics, Light, Zones };
// The «Физика» mode's tools: picking and dragging gravity points, placing
// one (press at the centre, drag out the radius), pouring water or sand
// into the free cells of a rectangle.
enum class PhysTool : u8 { Select, Point, Water, Sand };
// The «Свет» mode's tools: picking and dragging light sources, placing one
// (press at the centre, drag out the radius, as a gravity point).
enum class LightTool : u8 { Select, Source };
// The world's pull as the panel shows it: which way (as sim::cells_down,
// None: no pull) and how strong.
enum class PullDir : u8 { Down, Left, Up, Right, None };

struct LevelConfig {
    std::filesystem::path folder;   // the level's files
    std::filesystem::path settings; // where the panel layout is kept
    std::filesystem::path game_exe; // the game, for «Играть отсюда»; empty: no launching
    std::filesystem::path game_data; // the game's data for it (the project's game/); empty: the game's own
    bool offscreen = false;
};

class LevelEditor {
public:
    explicit LevelEditor(level::LevelModule& module);
    ~LevelEditor();

    // Before the document loads: opens the level, makes palette icons.
    bool init(ui::Ui& ui, SDL_GPUDevice* device, SDL_GPUTextureFormat format, const LevelConfig& config);
    void bind(Rml::DataModelConstructor& model);
    void set_model(Rml::DataModelHandle handle) {
        model_ = handle;
        dock_.set_model(handle);
    }
    // True while the UI updates: input events then come from bindings, not the user.
    void set_ui_updating(bool on) { ui_updating_ = on; }
    void shutdown();

    // Each frame while the tab is open; context: to find the elements.
    void update(f64 dt, Rml::Context* context);
    // Outside a render pass, then inside the one that draws the window.
    void prepare(SDL_GPUCommandBuffer* cmd);
    void draw(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass);

    // Mouse in the world view and on the panel headers; x, y in pixels.
    // ui_used: the UI took the event. True when this tab used it.
    bool handle_event(const SDL_Event& e, f32 density, bool ui_used, Rml::Context* context);
    bool handle_key(const SDL_KeyboardEvent& k);

    editor::UndoStack& history() { return history_; }
    void undo();
    void redo();
    // False when something was not written (the author is told what).
    bool save();
    // Saves and starts the game with the hero at the view's centre.
    bool play_here();
    // The command line play_here() runs (for the self-test).
    std::vector<std::string> play_command(f64 x, f64 y) const;
    // Where the game started from here writes the links that happen.
    static std::filesystem::path fired_file();

    std::string title() const { return module_.title(); }
    std::string status() const;
    bool dirty() const { return history_.dirty(); }
    // Opens another level folder in place of this one (its history goes;
    // unsaved changes are lost, so save first).
    bool open_folder(const std::filesystem::path& folder);

    // --- for the self-test and benchmarks ---
    level::Level& level() { return *level_; }
    render::Camera2D& camera() { return camera_; }
    editor::DockLayout& dock() { return dock_.layout(); }
    Tool tool() const { return tool_; }
    Mode mode() const { return mode_; }
    void set_mode(Mode m);
    void set_tool(Tool t);
    void select_tile(usize index);
    usize tile_index() const { return tile_; }
    u32 layer() const { return layer_; }
    i32 brush_radius() const { return radius_; }
    void set_brush_radius(i32 r);
    // Screen pixel of a tile point (in the view), false when outside it.
    bool screen_of(f64 tx, f64 ty, f32& x, f32& y) const;
    f32 view_x() const { return vx_; }
    f32 view_y() const { return vy_; }
    f32 view_w() const { return vw_; }
    f32 view_h() const { return vh_; }
    // Where the dock area is on screen (panel rectangles are relative to it).
    f32 dock_x() const { return dock_.x(); }
    f32 dock_y() const { return dock_.y(); }
    void reset_layout();
    // Objects
    void arm_object(i32 index); // -1: none (clicks select)
    void arm_template(u64 key); // the palette object of this template
    i32 armed_object() const { return object_; }
    const std::vector<u64>& selection() const { return selection_; }
    void select_objects(std::vector<u64> ids);
    void delete_selection();
    // A field of the properties panel (row i) set from text, as the user types it.
    void set_field(int i, const std::string& text, bool dragging);
    u64 minimap_updates() const { return minimap_updates_; }
    // Physics
    void set_phys_tool(PhysTool t);
    PhysTool phys_tool() const { return ph_tool_; }
    // The world's pull from the panel: a direction, a strength as typed (0..200).
    void set_pull_dir(PullDir d);
    PullDir pull_dir() const;
    void set_pull_strength(const std::string& text, bool dragging);
    f32 pull_strength() const;
    // The flow trial: water and sand run here and now, then go back.
    bool start_trial();
    void reset_trial();
    bool trial_running() const { return trial_.running(); }
    const level::FlowTrial& trial() const { return trial_; }
    // Ends a drag in the view as if it never began (Esc, the right button).
    bool cancel_gesture();
    bool gesture() const { return ph_drag_ != PhysDrag::None || stroke_ || moving_; }
    // The last thing a physics tool said (poured, skipped, refused).
    const std::string& phys_note() const { return ph_note_; }
    // Light
    void set_light_tool(LightTool t);
    LightTool light_tool() const { return lt_tool_; }
    // The level's time of day from the panel: "18:30" or hours ("18.5");
    // a slider's drag is one history entry.
    void set_level_time(const std::string& text, bool dragging);
    // Looking at the level at another hour: only the view (no history, no
    // files, the game does not see it); empty text or leaving «Свет» ends it.
    void set_preview_time(const std::string& text);
    f32 preview_time() const { return view_.preview_time; }
    const level::ViewOptions& view() const { return view_; }
    // The last thing the «Свет» mode said (placed, refused).
    const std::string& light_note() const { return lt_note_; }

    const std::vector<std::string>& panel_ids() const { return dock_.panel_ids(); }

private:
    struct PaletteTile {
        int index = 0;
        Rml::String name, icon, key;
        int layer = 0;
    };
    struct PaletteGroup {
        Rml::String name;
        std::vector<PaletteTile> tiles;
    };
    struct FieldView {
        Rml::String kind; // text, slider, bool, enum, readonly
        Rml::String label, value, hint;
        float min = 0, max = 0, step = 0;
        bool own = false;      // a property this copy sets its own way (not the template's)
        bool advanced = false; // under «Подробно»
    };
    struct FieldRef {
        const reflect::TypeInfo* type = nullptr; // nullptr: the position ("x" or "y") or a property
        std::string path;
        std::vector<std::string> options;
        const objects::PropDef* prop = nullptr; // a property of the template's copy
        std::string template_value;             // the template's value of it, as shown
        bool limited = false;                   // a number kept within min..max
        f64 min = 0, max = 0;
        bool refuse = false;                    // past min..max: refused, not moved to the edge (a place)
    };
    enum class PhysDrag : u8 { None, Place, Move, Radius, Area };
    void build_objects();

    template <typename T>
    void set(T& member, const T& value, const char* name) {
        if (member == value) return;
        member = value;
        model_.DirtyVariable(name);
    }

    void sync_model();
    void update_minimap();
    bool over_view(f32 x, f32 y, Rml::Context* context) const;
    void to_tile(f32 x, f32 y, f64& tx, f64& ty) const;
    void cell_at(f32 x, f32 y, i32& cx, i32& cy) const;

    void press(f32 x, f32 y);
    void drag(f32 x, f32 y);
    void release();
    void pick(i32 x, i32 y);
    world::TileId paint_value() const;
    std::string stroke_label() const;
    void shape_cells(std::vector<level::Cell>& out) const;
    void push_overlay(f64 ox, f64 oy);
    bool objects_mode() const { return mode_ == Mode::Select || mode_ == Mode::Objects; }
    void press_objects(f32 x, f32 y, bool add);
    void drag_objects(f32 x, f32 y);
    void rebuild_fields(Rml::Context* context);
    void physics_fields(flecs::entity e);
    void light_fields(flecs::entity e);
    // The things with a centre and a radius the mode edits: gravity points
    // in «Физика», light sources in «Свет». The same gestures make, move and
    // size both (PhysDrag).
    bool ring_of(flecs::entity e) const;
    f32 ring_radius(flecs::entity e) const;
    void set_ring_radius(flecs::entity e, f32 r);
    const reflect::TypeInfo* ring_type() const;
    const char* ring_name() const;
    f64 ring_max() const;
    // Each of the mode's things loaded now: (entity, x, y, radius).
    template <typename F>
    void each_ring(F&& f);
    // The one whose centre (or, ring: the selected one's circle) is under a
    // tile point; empty when none.
    flecs::entity point_at(f64 tx, f64 ty, bool ring);
    bool press_ring(f64 tx, f64 ty, bool place);
    bool press_physics(f32 x, f32 y);
    bool press_light(f32 x, f32 y);
    void push_light(f64 ox, f64 oy, f64 px);
    void drag_physics(f32 x, f32 y);
    void release_physics();
    void pour(i32 x0, i32 y0, i32 x1, i32 y1);
    void push_physics(f64 ox, f64 oy, f64 px);
    // An edit is about to happen: a running trial goes back first.
    void edit_begins();
    // Sets a physics field from text; false when the text is not a valid value.
    bool parse_number(const FieldRef& ref, const std::string& label, const std::string& text, f64& out) const;
    // Where a centre may be put (X and Y in the panel): inside the world; a
    // number past it is refused (a typo must not send the object to the edge).
    void place_limits(f64& x0, f64& y0, f64& x1, f64& y1) const;

    level::LevelModule& module_;
    std::unique_ptr<level::Level> level_;
    editor::Document doc_; // the undo stack wants one; tiles live in the level
    editor::UndoStack history_{doc_};
    LevelConfig config_;
    ui::Ui* ui_ = nullptr;
    Rml::DataModelHandle model_;
    DockView dock_;

    demo::SheetImage art_;
    render::SpriteRenderer back_, front_;
    render::SpriteBatch back_batch_, front_batch_;
    render::Camera2D camera_;
    bool view_ready_ = false;

    // The view on screen, in pixels.
    f32 vx_ = 0, vy_ = 0, vw_ = 0, vh_ = 0;
    bool view_shown_ = false;

    Mode mode_ = Mode::Tiles;
    Tool tool_ = Tool::Brush;
    usize tile_ = 0;
    u32 layer_ = 0;
    i32 radius_ = 1;
    bool rect_outline_ = false;
    level::ViewOptions view_;
    f64 time_ = 0;

    // Mouse
    f32 mouse_x_ = 0, mouse_y_ = 0;
    bool hover_ = false;
    i32 hover_x_ = 0, hover_y_ = 0;
    bool panning_ = false;
    u8 pan_button_ = 0;
    std::unique_ptr<level::TileStroke> stroke_;
    bool shaping_ = false; // a line or rectangle being dragged out
    i32 start_x_ = 0, start_y_ = 0, last_x_ = 0, last_y_ = 0;

    // Minimap
    static constexpr u32 kMapPx = 192;
    static constexpr i32 kMapTilesPerPx = 2;
    std::vector<u8> map_rgba_;
    std::string map_name_;
    u64 map_serial_ = 0, minimap_updates_ = 0;
    f64 map_time_ = -1;
    u64 map_edits_ = ~0ull;
    u32 map_resident_ = 0;
    f64 map_cx_ = 0, map_cy_ = 0; // the map's centre, in tiles

    SDL_Process* game_ = nullptr;

    // Objects
    i32 object_ = -1;              // armed palette object
    u64 armed_key_ = 0;            // its template's key
    u64 objects_version_ = 0;      // of the module's objects() the palette shows
    std::vector<u64> selection_;   // LevelIds
    u64 selection_version_ = 1;
    bool moving_ = false;
    f64 move_x_ = 0, move_y_ = 0;  // where the drag started, in tiles
    std::vector<level::MoveObjects::Move> move_start_;
    u64 fields_built_ = 0;
    bool ui_updating_ = false;
    std::vector<FieldRef> field_refs_;

    // Physics
    PhysTool ph_tool_ = PhysTool::Select;
    PhysDrag ph_drag_ = PhysDrag::None;
    f64 ph_cx_ = 0, ph_cy_ = 0, ph_r_ = 0;           // Place: the centre and radius so far
    f64 ph_grab_x_ = 0, ph_grab_y_ = 0;              // Move: where the drag started
    f64 ph_from_x_ = 0, ph_from_y_ = 0;              // Move: the point's centre before
    std::string ph_before_;                          // Radius: the source before (JSON)
    f32 ph_r0_ = 0;
    i32 ph_ax_ = 0, ph_ay_ = 0, ph_bx_ = 0, ph_by_ = 0; // Area: the corner cells
    u64 ph_preview_ = 0;                             // bumped while a drag changes a point
    f32 ph_strength_ = 40;                           // the pull's strength when it comes back from «нет»
    std::string ph_note_;
    level::FlowTrial trial_;

    // Light
    LightTool lt_tool_ = LightTool::Select;
    std::string lt_note_;

    // Model mirrors
    std::vector<PaletteGroup> m_palette_;
    std::vector<Rml::String> m_layers_;
    Rml::String m_tool_ = "brush", m_tool_name_, m_tool_help_;
    Rml::String m_mode_ = "tiles", m_mode_name_, m_mode_help_, m_mode_when_;
    int m_tile_ = 0, m_layer_ = 0, m_radius_ = 1;
    Rml::String m_tile_name_, m_tile_hint_, m_tile_icon_, m_tile_layer_;
    bool m_light_ = false, m_grid_ = false, m_chunks_ = false, m_outline_ = false;
    bool m_dirty_ = false;
    Rml::String m_minimap_, m_place_, m_info_;
    std::vector<Rml::String> m_history_;
    std::vector<PaletteGroup> m_objects_;
    int m_object_ = -1, m_sel_count_ = 0;
    Rml::String m_sel_name_, m_sel_hint_, m_sel_icon_, m_sel_kind_;
    bool m_details_ = false; // «Подробно» open
    std::vector<FieldView> m_fields_;
    Rml::String m_ph_tool_ = "select", m_ph_tool_name_, m_ph_tool_help_, m_ph_dir_, m_ph_world_, m_ph_error_, m_ph_trial_text_;
    Rml::String m_ph_strength_ = "40", m_ph_note_;
    bool m_ph_trial_ = false, m_ph_can_trial_ = false;
    Rml::String m_lt_tool_ = "select", m_lt_tool_name_, m_lt_tool_help_, m_lt_time_, m_lt_hours_, m_lt_sky_, m_lt_error_;
    Rml::String m_lt_preview_, m_lt_preview_hours_, m_lt_note_;
    bool m_lt_previewing_ = false;
    int m_history_cursor_ = 0;
    u64 history_version_ = 0;
    f64 info_time_ = -1;
};

const char* tool_id(Tool t);
const char* mode_id(Mode m);
const char* phys_tool_id(PhysTool t);
const char* light_tool_id(LightTool t);

} // namespace forge::editor_app
