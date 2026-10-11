#pragma once

// The «Анимация» tab: how the game plays a template's picture.
//
// The left column lists the game's templates that have a picture of their own. The middle shows the selected one
// playing (the editor's clock; Space stops and starts it, ← and → go a frame back and forward), and under it the
// frames of its picture, a strip of «Кадров в картинке» equal frames. The right column has the states the game's
// module plays the picture in (forge::level::LevelModule::anim_states; slice: the hero stands, walks, is in the air; a
// walker stands and walks; anything else is idle). A state shows the game's own rule until the author makes it an
// animation of their own: a click on a frame of the strip adds it to the selected state's frames, Backspace takes
// the last away; its frames a second come from a list (each divides the game's 60 ticks a second), «Повторять» or
// it stays on its last frame; «Как в игре» gives the state back to the game's rule. An animation that cannot play
// (a frame past the strip, after «Кадров в картинке» was made fewer) says why, and the game plays the rule then.
//
// The animations are the template file's (objects::Template::animations), written at once on each change; every
// change is one step of the tab's history (Ctrl+Z). The game reads them when it starts, the level view at once.

#include "forge/editor/document.h"
#include "forge/editor/undo.h"
#include "forge/level/level.h"
#include "forge/objects/library.h"
#include "forge/ui/ui.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace forge::editor_app {

class AnimationEditor {
public:
    explicit AnimationEditor(level::LevelModule& module);

    bool init(ui::Ui& ui);
    void bind(Rml::DataModelConstructor& model);
    void set_model(Rml::DataModelHandle handle) { model_ = handle; }
    // Each frame while the tab is open: the preview's clock goes on (while it plays).
    void update(f64 dt);
    bool handle_key(const SDL_KeyboardEvent& k);

    editor::UndoStack& history() { return history_; }
    void undo();
    void redo();
    std::string status() const;

    // A template's picture as the UI shows it (the object library's icon).
    std::function<std::string(const objects::Template&)> template_icon;

    // --- actions (buttons, keys, the self-test) ---
    objects::Library& library() { return *module_.library(); }
    // The templates the list shows, by key, in its order.
    const std::vector<u64>& listed() const { return listed_; }
    const objects::Template* selected() const;
    void select(u64 key);
    // «Сделать анимацию» of «Ресурсы»: the template drawn with the game's picture `picture` (a file of its pictures
    // folder) selected; none: false, and the list says that no template draws «asset_name» yet.
    bool select_picture(const std::string& picture, const std::string& asset_name);
    const std::string& note() const { return m_note_; }
    // The states of the selected template (the module's, with their rules) and the one selected.
    const std::vector<level::LevelModule::AnimState>& states() const { return states_; }
    const std::string& state() const { return state_; }
    bool select_state(const std::string& id);
    // What the selected state plays: the template's own animation when it can play, else the game's rule.
    objects::Clip clip() const;
    // Whether the selected state has an animation of the template's own, and why it cannot play (empty: it can).
    bool own() const;
    std::string problem() const;
    // The selected state's own animation: a frame of the strip added at its end (a state without one of its own gets
    // one of that frame, with the rule's frames a second), its last frame taken away (the last of all gives the state
    // back to the rule), its frames a second, starting over or not, given back to the rule.
    bool add_frame(u32 frame);
    bool remove_last();
    bool set_fps(f32 fps);
    bool set_loop(bool on);
    bool reset_state();
    // «Кадров в картинке» of the selected template (1..kMaxFrames).
    bool set_frames(int n);
    // The preview: playing or not, a frame back or forward (it stops), the ticks of its clock (60 a second) and the
    // frame of the strip it shows now.
    void play(bool on);
    bool playing() const { return playing_; }
    void step(int frames);
    void set_clock(u64 ticks);
    u64 clock() const { return ticks_; }
    u32 shown_frame() const;
    // What the preview shows now, as the UI names its picture (for the self-test: the frame's own image).
    const std::string& preview_image() const { return m_preview_; }
    // The strip's frames as the UI names their pictures.
    std::string strip_image(u32 frame) const;

private:
    struct Row {
        Rml::String name, icon, frames, note;
        bool selected = false;
    };
    struct StateView {
        Rml::String id, name, frames, fps, problem;
        bool own = false, loop = true, selected = false;
    };
    struct Thumb {
        Rml::String icon;
        int index = 0;
        bool used = false, shown = false;
    };
    struct FpsChip {
        Rml::String label;
        bool selected = false;
    };

    // The selected state's own animation as `clip`, one step of the history (nullopt: given back to the rule).
    bool change_state(std::optional<objects::Clip> clip, std::string label);
    void rebuild();
    // The pictures of the selected template's frames, made again when its picture or frames changed.
    void cut_frames();
    void show_frame();
    void dirty(const char* name) {
        if (model_) model_.DirtyVariable(name);
    }

    level::LevelModule& module_;
    ui::Ui* ui_ = nullptr;
    Rml::DataModelHandle model_;
    editor::Document doc_; // the history needs one; the animations live in the template files
    editor::UndoStack history_{doc_};
    u64 built_ = 0;        // the library's version the views were made from
    u64 selected_ = 0;
    std::string state_;
    std::vector<u64> listed_;
    std::vector<level::LevelModule::AnimState> states_;
    // The selected template's frames as pictures: what they were cut from (key, look, frames).
    std::string cut_from_;
    std::string from_note_; // «Сделать анимацию» found no template of the picture
    std::vector<std::string> thumbs_, bigs_;
    u32 cut_frames_ = 0; // how many frames the picture was cut into (1 when its width does not divide)
    bool playing_ = true;
    f64 time_ = 0; // seconds of the preview's clock
    u64 ticks_ = 0;
    u32 shown_ = ~0u;

    std::vector<Row> m_list_;
    std::vector<StateView> m_states_;
    std::vector<Thumb> m_strip_, m_seq_;
    std::vector<FpsChip> m_fps_;
    Rml::String m_note_, m_sel_name_, m_sel_note_, m_preview_, m_problem_, m_state_name_;
    int m_frames_ = 1;
    bool m_has_sel_ = false, m_own_ = false, m_loop_ = true, m_playing_ = true;
};

} // namespace forge::editor_app
