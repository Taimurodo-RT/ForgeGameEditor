#pragma once

// Dockable panels of one editor tab, on screen: places the panes the
// document has (RML elements "<pane>-<id>") and the tab's main view
// over a DockLayout, shows panel headers and splitters through the data
// model, and moves panels when their headers are dragged. The layout is
// remembered in a settings file.
//
// Several tabs can each have one: the data model names start with a prefix
// ("" → dock_frames, dock_grab…; "as_" → as_dock_frames, as_dock_grab…).

#include "forge/editor/dock.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace forge::editor_app {

struct DockConfig {
    std::string prefix;      // data model names
    std::string area;        // the element the dock fills
    std::string view;        // the element placed where the view goes
    std::string pane_prefix; // panes are "<pane_prefix><panel id>"
    struct Panel {
        std::string id, title, icon;
    };
    std::vector<Panel> panels;
    std::string default_layout; // DockLayout JSON
    std::string file;           // settings file name ("level_layout.json")
};

class DockView {
public:
    // Once per data model, before any DockView binds.
    static void register_types(Rml::DataModelConstructor& model);

    // settings: the folder of the file; empty or !persist: nothing is saved.
    void init(DockConfig config, const std::filesystem::path& settings, bool persist);
    void bind(Rml::DataModelConstructor& model);
    void set_model(Rml::DataModelHandle handle) { model_ = handle; }

    // Each frame while the tab is open. False when the dock is not on screen.
    bool update(Rml::Context* context);
    // Panel header drags and splitters; x, y in pixels. True when used.
    bool mouse_move(f32 x, f32 y);
    bool mouse_down() const { return busy(); }
    bool mouse_up();
    bool busy() const { return !grab_panel_.empty() || grab_splitter_ >= 0; }

    void reset();
    editor::DockLayout& layout() { return dock_; }
    const std::vector<std::string>& panel_ids() const { return ids_; }
    // The view's rectangle on screen.
    f32 view_x() const { return dock_x_ + view_.x; }
    f32 view_y() const { return dock_y_ + view_.y; }
    f32 view_w() const { return std::max(0.0f, view_.w); }
    f32 view_h() const { return std::max(0.0f, view_.h); }
    f32 x() const { return dock_x_; }
    f32 y() const { return dock_y_; }

private:
    struct Tab {
        Rml::String id, title, icon;
        bool active = false;
        bool used = false;
    };
    struct Frame {
        float x = 0, y = 0, w = 0, h = 0;
        bool view = false;
        bool used = false;
        std::vector<Tab> tabs;
    };
    struct Gap {
        float x = 0, y = 0, w = 0, h = 0;
        bool column = false;
        bool used = false;
    };
    template <typename T>
    void set(T& member, const T& value, const std::string& name) {
        if (member == value) return;
        member = value;
        model_.DirtyVariable(config_.prefix + name);
    }
    void save();
    const DockConfig::Panel* panel(std::string_view id) const;

    DockConfig config_;
    std::vector<std::string> ids_;
    std::filesystem::path settings_;
    bool persist_ = false;
    editor::DockLayout dock_;
    Rml::DataModelHandle model_;

    f32 dock_x_ = 0, dock_y_ = 0, dock_w_ = 0, dock_h_ = 0;
    editor::DockRect view_{};
    u64 applied_version_ = 0;
    f32 mouse_x_ = 0, mouse_y_ = 0;

    std::string grab_panel_;
    bool dragging_ = false;
    f32 grab_x_ = 0, grab_y_ = 0;
    editor::DockLayout::Drop drop_;
    i32 grab_splitter_ = -1;

    std::vector<Frame> m_frames_;
    std::vector<Gap> m_gaps_;
    bool m_drop_ = false;
    float m_drop_x_ = 0, m_drop_y_ = 0, m_drop_w_ = 0, m_drop_h_ = 0;
};

} // namespace forge::editor_app
