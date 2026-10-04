#pragma once

// The «Уровень» tab: paints a game's streamed world with tiles. Panels
// (palette, minimap, properties, history, log) dock around the world view
// and can be dragged by their headers; the layout is remembered.
//
// Everything game-specific comes from a forge::level::LevelModule, so the
// same tab serves any game; this app opens «Старая шахта».

#include "forge/editor/document.h"
#include "forge/editor/dock.h"
#include "forge/editor/undo.h"
#include "forge/level/level.h"
#include "forge/level/object_edit.h"
#include "forge/level/tile_edit.h"
#include "forge/render/camera.h"
#include "forge/render/sprite_batch.h"
#include "forge/render/sprite_renderer.h"
#include "forge/ui/ui.h"

#include "demo_art.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace forge::editor_app {

enum class Tool : u8 { Brush, Line, Rect, Fill, Eraser, Picker };
// What the tab edits: the icons over the level. Only Select and Tiles work
// yet; the others say when they come.
enum class Mode : u8 { Select, Tiles, Objects, Physics, Light, Zones };

struct LevelConfig {
    std::filesystem::path folder;   // the level's files
    std::filesystem::path settings; // where the panel layout is kept
    std::filesystem::path game_exe; // the game, for «Играть отсюда»; empty: no launching
    bool offscreen = false;
};

class LevelEditor {
public:
    explicit LevelEditor(level::LevelModule& module);
    ~LevelEditor();

    // Before the document loads: opens the level, makes palette icons.
    bool init(ui::Ui& ui, SDL_GPUDevice* device, SDL_GPUTextureFormat format, const LevelConfig& config);
    void bind(Rml::DataModelConstructor& model);
    void set_model(Rml::DataModelHandle handle) { model_ = handle; }
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
    void save();
    // Saves and starts the game with the hero at the view's centre.
    bool play_here();
    // The command line play_here() runs (for the self-test).
    std::vector<std::string> play_command(f64 x, f64 y) const;

    std::string title() const { return module_.title(); }
    std::string status() const;
    bool dirty() const { return history_.dirty(); }

    // --- for the self-test and benchmarks ---
    level::Level& level() { return *level_; }
    render::Camera2D& camera() { return camera_; }
    editor::DockLayout& dock() { return dock_; }
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
    f32 dock_x() const { return dock_x_; }
    f32 dock_y() const { return dock_y_; }
    void reset_layout();
    // Objects
    void arm_object(i32 index); // -1: none (clicks select)
    i32 armed_object() const { return object_; }
    const std::vector<u64>& selection() const { return selection_; }
    void select_objects(std::vector<u64> ids);
    void delete_selection();
    // A field of the properties panel (row i) set from text, as the user types it.
    void set_field(int i, const std::string& text, bool dragging);
    u64 minimap_updates() const { return minimap_updates_; }

    static const std::vector<std::string>& panel_ids();

private:
    struct DockTab {
        Rml::String id, title, icon;
        bool active = false;
    };
    struct DockFrame {
        float x = 0, y = 0, w = 0, h = 0;
        bool view = false;
        bool used = false;
        std::vector<DockTab> tabs;
    };
    struct DockGap {
        float x = 0, y = 0, w = 0, h = 0;
        bool column = false;
        bool used = false;
    };
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
        Rml::String label, value;
        float min = 0, max = 0, step = 0;
    };
    struct FieldRef {
        const reflect::TypeInfo* type = nullptr; // nullptr: the position ("x" or "y")
        std::string path;
        std::vector<std::string> options;
    };

    template <typename T>
    void set(T& member, const T& value, const char* name) {
        if (member == value) return;
        member = value;
        model_.DirtyVariable(name);
    }

    void apply_layout(Rml::Context* context);
    void save_layout();
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

    level::LevelModule& module_;
    std::unique_ptr<level::Level> level_;
    editor::Document doc_; // the undo stack wants one; tiles live in the level
    editor::UndoStack history_{doc_};
    LevelConfig config_;
    ui::Ui* ui_ = nullptr;
    Rml::DataModelHandle model_;
    editor::DockLayout dock_;
    std::string default_layout_;

    demo::SheetImage art_;
    render::SpriteRenderer back_, front_;
    render::SpriteBatch back_batch_, front_batch_;
    render::Camera2D camera_;
    bool view_ready_ = false;

    // The view and the dock area on screen, in pixels.
    f32 vx_ = 0, vy_ = 0, vw_ = 0, vh_ = 0;
    f32 dock_x_ = 0, dock_y_ = 0, dock_w_ = 0, dock_h_ = 0;
    u64 applied_dock_version_ = 0;
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
    // Panel drags
    std::string grab_panel_;
    bool dragging_panel_ = false;
    f32 grab_x_ = 0, grab_y_ = 0;
    editor::DockLayout::Drop drop_;
    i32 grab_splitter_ = -1;

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
    std::vector<u64> selection_;   // LevelIds
    u64 selection_version_ = 1;
    bool moving_ = false;
    f64 move_x_ = 0, move_y_ = 0;  // where the drag started, in tiles
    std::vector<level::MoveObjects::Move> move_start_;
    u64 fields_built_ = 0;
    bool ui_updating_ = false;
    std::vector<FieldRef> field_refs_;

    // Model mirrors
    std::vector<DockFrame> m_frames_;
    std::vector<DockGap> m_gaps_;
    bool m_drop_ = false;
    float m_drop_x_ = 0, m_drop_y_ = 0, m_drop_w_ = 0, m_drop_h_ = 0;
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
    Rml::String m_sel_name_, m_sel_hint_, m_sel_icon_;
    std::vector<FieldView> m_fields_;
    int m_history_cursor_ = 0;
    u64 history_version_ = 0;
    f64 info_time_ = -1;
};

const char* tool_id(Tool t);
const char* mode_id(Mode m);

} // namespace forge::editor_app
