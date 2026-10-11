#pragma once

// The «Ввод» tab: which keys, mouse buttons and gamepad buttons make the game's actions (editors/ввод-модель.md).
//
// Two groups of actions: the game's (its module's, forge::modules::ModuleDef::actions) and the shell's, which every
// game has (pause, journal, the quick saves). An action shows its binds as chips, each with × to take it away;
// «+ Клавиша» waits for a key or a gamepad's button or stick (Esc: no), «Мышь, геймпад» lists the mouse's and the
// gamepad's buttons to pick from without a gamepad at hand; «Как было» gives the action back the module's keys. One
// bind on two actions and an action with no bind at all are said under the list and in the status line.
//
// What the author changed is the game's input.json (only the actions changed; none changed: no file), written at
// once on each change; every change is one step of the tab's history (Ctrl+Z). The game reads it when it starts, and
// the player may change the keys again in its «Настройки».

#include "forge/editor/document.h"
#include "forge/editor/undo.h"
#include "forge/input/input.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace forge::editor_app {

class InputEditor {
public:
    // Before bind: the game's input.json, the module's actions and its name. A file that does not read is left as it
    // is: the tab says why and changes nothing.
    void open(const std::filesystem::path& file, std::vector<input::ActionDef> module_actions, std::string module_name);
    void bind(Rml::DataModelConstructor& model);
    void set_model(Rml::DataModelHandle handle);
    // Every event of the window: a gamepad connected is opened here (its buttons come only then). While an action
    // waits (shown: the tab is open): a key, a gamepad's button or stick is taken for it, Esc takes nothing; true.
    bool handle_event(const SDL_Event& e, bool shown);

    editor::UndoStack& history() { return history_; }
    void undo();
    void redo();
    std::string status() const;

    // --- actions (buttons, keys, the self-test) ---
    // The module's actions, then the shell's.
    const std::vector<input::ActionDef>& defs() const { return defs_; }
    usize module_count() const { return module_count_; }
    // The action's binds as the game plays them (the module's, or the author's), as text: "key:Space".
    std::vector<std::string> binds(std::string_view action) const;
    // Whether the author changed the action's binds (input.json has it).
    bool changed(std::string_view action) const;
    // A bind added at the end of the action's (one it has: false), its index-th taken away, the module's back.
    bool add(std::string_view action, const input::Bind& bind);
    bool remove(std::string_view action, usize index);
    bool reset(std::string_view action);
    // «+ Клавиша»: the action waits for a key (again: it does not); "" none waits.
    void wait_key(std::string_view action);
    const std::string& waiting() const { return waiting_; }
    // «Мышь, геймпад»: the list of the mouse's and the gamepad's buttons under the action (again: closed).
    void show_more(std::string_view action);
    const std::string& more() const { return more_; }
    // One bind on two actions, an action with none, a choice of the file that plays nothing: in words.
    std::vector<std::string> warnings() const;
    // Why input.json does not read ("" it does or there is none).
    const std::string& error() const { return error_; }
    const std::filesystem::path& file() const { return file_; }
    // What the last key taken said (a bind the action has already).
    const std::string& note() const { return note_; }

private:
    struct Chip {
        Rml::String text, words;
        int index = 0;
        bool pad = false;
    };
    struct Row {
        Rml::String id, words;
        std::vector<Chip> binds;
        bool changed = false, waiting = false, more = false, empty = false;
    };
    struct Pick {
        Rml::String text, words;
        bool pad = false, has = false;
    };
    friend class InputCommand;

    // The author's choices as `to` (one step of the history), written to the file.
    bool change(input::Choices to, std::string label);
    // The choices into the file (none: no file) and the views again.
    void put(const input::Choices& choices);
    const input::ActionDef* def(std::string_view action) const;
    void rebuild();
    void dirty(const char* name) {
        if (model_) model_.DirtyVariable(name);
    }

    std::filesystem::path file_;
    std::vector<input::ActionDef> defs_;
    usize module_count_ = 0;
    std::string module_name_;
    input::Choices choices_;
    std::vector<input::Action> resolved_;
    std::vector<std::string> file_warnings_; // reading input.json and resolving it
    std::string error_, waiting_, more_, note_;
    input::Input pads_; // the gamepads connected, open
    Rml::DataModelHandle model_;
    editor::Document doc_; // the history needs one; the keys live in input.json
    editor::UndoStack history_{doc_};

    std::vector<Row> m_game_, m_shell_;
    std::vector<Pick> m_picks_;
    std::vector<Rml::String> m_warnings_;
    Rml::String m_note_, m_error_, m_module_note_;
};

} // namespace forge::editor_app
