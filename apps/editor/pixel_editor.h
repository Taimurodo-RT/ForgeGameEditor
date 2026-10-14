#pragma once

// «Пиксельные» of the tab «Анимация» (step 2 of docs/models/вектор-модель.md): a vector character of «Ресурсы» posed
// by its script and baked into a strip of pixel frames for a template of the game.
//
// The left column lists the project's «*.pixel.json» (name, frame size, clips) and the SVGs a new one can be made of
// (a «.pixel.json» and a «.luau» beside the SVG). The middle shows the frame twice: the vector as ThorVG draws it in
// the pose (the selected part's bounds and pivot over it) and the pixel frame exactly as it bakes, at its size and
// bigger without smoothing; under them the frames of the selected clip (Space, ← and → as in «Покадровая»; «Калька»
// shows the frames either side faintly). The right column has the SVG's parts as they nest (a click on the vector
// selects the part under it, the selected part's pivot is dragged with the mouse), the script's parameters and seed,
// the clips (a state of the game's module, frames, frames a second from «Покадровая»'s list, «Повторять»), the baking
// (frame size, the SVG's colours or the game's, an outline), «Запечь» and «Отдать шаблону…».
//
// Every change is written into the «.pixel.json» at once and is one step of the tab's history (Ctrl+Z), shared with
// «Покадровая». «Запечь» writes the strip as a picture of «Ресурсы» beside the «.pixel.json» (the same asset again on
// the next bake, so the game's copies follow) and records its hash: a picture changed since («Изображения») is not
// baked over without asking. «Отдать шаблону…» gives a template of the game the picture, «Кадров в картинке» and an
// animation for each baked clip of a state it plays. The script and the SVG are read again when their files change on
// disk; a script that fails says its line, and the views stay on the last pose that worked.

#include "forge/editor/document.h"
#include "forge/editor/undo.h"
#include "forge/level/level.h"
#include "forge/objects/library.h"
#include "forge/pixel/bake.h"
#include "forge/ui/ui.h"

#include "object_library.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace forge::editor_app {

class AssetLibrary;

class PixelEditor {
public:
    PixelEditor(AssetLibrary& assets, level::LevelModule& module, editor::UndoStack& history);
    ~PixelEditor();

    bool init(ui::Ui& ui);
    void bind(Rml::DataModelConstructor& model);
    void set_model(Rml::DataModelHandle handle) { model_ = handle; }
    // Each frame while the mode is shown: files looked at, the clock, the views.
    void update(f64 dt, Rml::Context* context);
    bool handle_event(const SDL_Event& e, f32 density, Rml::Context* context);
    bool handle_key(const SDL_KeyboardEvent& k);
    std::string status() const;

    // main wires these.
    CopyIn copy_in;                                                  // a picture of «Ресурсы» into the game
    std::function<std::vector<pixel::Rgba8>()> game_colors;          // «Цвета игры»
    std::function<std::string(const objects::Template&)> template_icon;
    std::function<std::string(const Guid&)> game_copy;               // the game's picture copied from that asset, or ""

    // --- the characters (buttons, keys, the self-test) ---
    // The «.pixel.json» of the project, by path in «Ресурсы».
    const std::vector<std::string>& listed() const { return listed_; }
    bool open(const std::string& rel);
    const std::string& opened() const { return rel_; }
    bool has() const { return has_; }
    // The SVGs a character can be made of, by path; a new one beside it (one step): «Имя.pixel.json», «Имя.luau».
    const std::vector<std::string>& svgs() const { return svgs_; }
    bool create(const std::string& svg_rel);
    // «В «Пиксельные»» of «Ресурсы» on an SVG: the character made of it when there is one, else a new one.
    bool from_svg(const std::string& svg_rel);
    // Reads the files of the character again (after Ctrl+Z, or when they changed on disk).
    void reload();
    // «Ресурсы» looked at the project's files: the lists again, the SVG found where it is now.
    void indexed() { list_dirty_ = true; }
    // Looks at the dates of the SVG, the script and the «.pixel.json» now (each frame does it twice a second).
    void check_files();

    const pixel::PixelDoc& doc() const { return doc_; }
    const pixel::Rig& rig() const { return rig_; }
    // Why the character does not show or the pose fails: empty when all is well.
    const std::string& problem() const { return problem_; }
    u32 error_line() const { return error_line_; }
    // What the last action said (a refusal, what was done).
    const std::string& note() const { return note_; }

    // Parts: the selected one, and its pivot (one step; picture units).
    bool select_part(const std::string& id);
    const std::string& part() const { return part_; }
    bool set_pivot(const std::string& id, vector::Point at);
    // The mouse on the vector view, in the context's pixels: a press near the selected part's pivot drags it (one step
    // on release), elsewhere selects the part under it.
    void press(f32 x, f32 y);
    void drag(f32 x, f32 y);
    void release();
    // Where a point of the picture (in the pose shown) is on the screen, in the context's pixels; false when the view
    // is not laid out.
    bool screen_of(vector::Point p, f32& x, f32& y) const;
    // A part's pivot as it stands in the pose shown (picture units).
    vector::Point posed_pivot(const std::string& id) const;

    // The script's parameters (the declared ones, by name) and the seed.
    bool set_param(const std::string& name, f64 value);
    bool set_seed(u64 seed);

    // Clips: by state. A state without a clip gets one (4 frames, 10 a second, repeated) and is selected.
    bool select_clip(const std::string& state);
    const std::string& clip() const { return clip_; }
    bool add_clip(const std::string& state);
    bool remove_clip();
    bool set_clip_frames(int n);
    bool set_clip_fps(f32 fps);
    bool set_clip_loop(bool on);
    // The states a clip may be for: the module's, over the game's templates.
    const std::vector<level::LevelModule::AnimState>& states() const { return states_; }

    // The clock: playing or not, a frame back or forward (it stops), a frame chosen, the frames either side.
    void play(bool on);
    bool playing() const { return playing_; }
    void step(int frames);
    void set_frame(u32 k);
    u32 frame() const { return frame_; }
    void set_onion(bool on);
    bool onion() const { return onion_; }

    // Baking: frame size, the game's colours or the SVG's, the outline and its colour.
    bool set_frame_size(u32 w, u32 h);
    bool set_game_palette(bool on);
    bool set_outline(bool on);
    bool set_outline_color(const std::string& text);
    // «Запечь» (one step). A picture changed since it was baked is baked over only with over_edits: without, false and
    // edited() is true.
    bool bake(bool over_edits = false);
    bool edited() const { return edited_; }
    // The baked picture in «Ресурсы» (its path), "" before the first bake or when it is gone.
    std::string picture() const;

    // «Отдать шаблону…»: the templates of the game by key, in the list's order; the picture, its frames and the baked
    // clips the template plays as its animations (one step).
    const std::vector<u64>& templates() const { return templates_; }
    void open_give(bool on);
    bool giving() const { return giving_; }
    bool give(u64 key);

    // What the views show now (for the self-test): the vector, the pixel frame at its size, both as the UI names them.
    const assets::CookedTexture& vector_view() const { return vector_; }
    const assets::CookedTexture& pixel_frame() const { return frame_px_; }
    const std::string& vector_image() const { return m_vector_; }
    const std::string& pixel_image() const { return m_pixel_; }

private:
    struct Row {
        Rml::String name, sub;
        bool selected = false;
    };
    struct SvgRow {
        Rml::String name, sub;
    };
    struct PartRow {
        Rml::String id, label;
        int depth = 0;
        bool shape = false, selected = false;
    };
    struct ParamRow {
        Rml::String name, value;
    };
    struct StateChip {
        Rml::String id, name, sub;
        bool on = false, selected = false;
    };
    struct FrameCell {
        int index = 0;
        bool shown = false;
    };
    struct FpsChip {
        Rml::String label;
        bool selected = false;
    };
    struct TemplateRow {
        Rml::String name, icon, note;
        bool draws = false;
    };
    struct Watched {
        std::filesystem::path file;
        i64 stamp = 0; // write time and size; 0: not there
    };

    pixel::ClipSpec* selected_clip();
    const pixel::ClipSpec* selected_clip() const;
    // The file's new text (one step of the history).
    bool change(const pixel::PixelDoc& after, std::string label);
    void load_svg();
    void load_script();
    void rebuild_list();
    void rebuild_states();
    void rebuild();
    void sync_problem();
    void check_edited();
    void redraw();
    void show(const assets::CookedTexture& vec, const assets::CookedTexture& px);
    vector::Point view_to_picture(f32 x, f32 y) const;
    std::filesystem::path script_file() const;
    std::string name() const;
    std::string state_name(const std::string& id) const; // «Идёт» of "walk"; the id when no template plays it
    void dirty(const char* var) {
        if (model_) model_.DirtyVariable(var);
    }

    AssetLibrary& assets_;
    level::LevelModule& module_;
    editor::UndoStack& history_;
    ui::Ui* ui_ = nullptr;
    Rml::DataModelHandle model_;

    std::vector<std::string> listed_, svgs_;
    std::vector<level::LevelModule::AnimState> states_;
    std::vector<u64> templates_;
    bool list_dirty_ = true;
    u64 lib_seen_ = ~0ull; // the library's version the states and templates were made from
    f64 file_clock_ = 0;

    // The character open.
    std::string rel_;
    std::filesystem::path file_;
    bool has_ = false;
    pixel::PixelDoc doc_;
    pixel::Rig rig_;
    pixel::PoseScript script_;
    Watched svg_watch_, script_watch_, doc_watch_;
    std::string svg_text_, script_text_; // what the rig and the script were made of
    std::string svg_problem_, script_problem_, doc_problem_, pose_problem_, problem_, note_;
    u32 error_line_ = 0;
    std::string part_, clip_;
    bool edited_ = false, giving_ = false;

    // The clock and the views.
    bool playing_ = true, onion_ = false;
    f64 time_ = 0;
    u32 frame_ = 0;
    bool view_dirty_ = true;
    assets::CookedTexture vector_, frame_px_;
    std::string images_[2][3]; // the pictures shown now and before: vector, pixel big, pixel 1:1
    u64 image_serial_ = 0;
    f32 view_x_ = 0, view_y_ = 0, view_w_ = 0, view_h_ = 0; // the vector view on screen (context pixels)
    // The views are drawn at their size in pixels of the window: 288 px of the style, dp_ pixels each.
    f32 dp_ = 1;
    u32 view_px() const;
    bool dragging_ = false, drag_moved_ = false;
    pixel::PixelDoc drag_doc_; // the file's, while a pivot is dragged

    std::vector<Row> m_list_;
    std::vector<SvgRow> m_svgs_;
    std::vector<PartRow> m_parts_;
    std::vector<ParamRow> m_params_;
    std::vector<StateChip> m_states_;
    std::vector<FrameCell> m_frames_;
    std::vector<FpsChip> m_fps_;
    std::vector<TemplateRow> m_templates_;
    Rml::String m_name_, m_vector_, m_pixel_, m_one_, m_problem_, m_note_, m_frame_label_, m_clip_name_, m_seed_, m_fw_, m_fh_,
        m_outline_color_, m_picture_, m_size_, m_total_;
    int m_clip_frames_ = 0;
    bool m_has_ = false, m_has_clip_ = false, m_playing_ = true, m_onion_ = false, m_game_palette_ = false, m_outline_ = false,
         m_clip_loop_ = true, m_edited_ = false, m_giving_ = false, m_can_give_ = false;
};

} // namespace forge::editor_app
