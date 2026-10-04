#pragma once

// The «Объекты» tab: the game's object templates, by kind.
//
// Every card is one template ("Монеты", "Шахтёр Борис"); the left column
// lists the kinds ("Подбираемое", "Житель") and filters by them. The right
// column shows the selected template in plain words: what kind it is, its
// properties ("Что это", "Сколько"), and «Поставить на уровень». «Создать»
// makes a new template from a kind's preset ("Монетка" for a platformer).
//
// Each change writes the template's file at once and is one step of the
// tab's history (Ctrl+Z). Copies on the level follow the template the next
// time the level tab shows (or their chunk loads), and in the game.

#include "forge/editor/document.h"
#include "forge/editor/undo.h"
#include "forge/level/level.h"
#include "forge/objects/library.h"
#include "forge/ui/ui.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace forge::editor_app {

class ObjectLibrary {
public:
    explicit ObjectLibrary(level::LevelModule& module);

    bool init(ui::Ui& ui);
    void bind(Rml::DataModelConstructor& model);
    void set_model(Rml::DataModelHandle handle) { model_ = handle; }
    void set_ui_updating(bool on) { ui_updating_ = on; }
    // Each frame while the tab is open.
    void update(Rml::Context* context);
    bool handle_key(const SDL_KeyboardEvent& k);

    editor::UndoStack& history() { return history_; }
    void undo();
    void redo();
    std::string status() const;

    // «Поставить на уровень»: the editor opens the level with this template armed.
    std::function<void(u64 key)> on_place;

    // --- actions (buttons, keys, the self-test) ---
    objects::Library& library() { return *module_.library(); }
    const objects::Template* selected() const;
    void select(u64 key);
    void show_kind(const std::string& kind); // "": every kind
    void set_search(const std::string& text);
    // A new template of a kind: from its preset (index), or empty (-1).
    bool create(const std::string& kind, int preset);
    bool duplicate();
    bool remove_selected();
    bool rename(const std::string& name);
    bool set_about(const std::string& about);
    // Row i of the properties, set from text as the user types it.
    void set_prop(int i, const std::string& text, bool dragging);
    usize cards() const { return m_cards_.size(); }
    const std::string& card_name(usize i) const { return m_cards_[i].name; }
    bool menu_open() const { return m_menu_; }

private:
    struct KindRow {
        Rml::String id, name, icon, about;
        int count = 0;
        bool selected = false;
    };
    struct Card {
        Rml::String name, kind, icon, about;
        bool selected = false;
    };
    struct PresetRow {
        Rml::String kind, name, genre, about;
        int index = -1;
    };
    struct CreateGroup {
        Rml::String kind, name, icon, about;
        std::vector<PresetRow> presets;
    };
    struct PropView {
        Rml::String kind; // text, slider, bool, enum
        Rml::String label, value, hint;
        float min = 0, max = 0, step = 0;
        bool advanced = false;
    };

    void rebuild();
    void rebuild_props();
    void icon_of(const objects::Template& t);
    void change(objects::Template after, std::string label, std::string merge = {});
    template <typename T>
    void set(T& member, const T& value, const char* name) {
        if (member == value) return;
        member = value;
        if (model_) model_.DirtyVariable(name);
    }

    level::LevelModule& module_;
    ui::Ui* ui_ = nullptr;
    Rml::DataModelHandle model_;
    editor::Document doc_; // the history needs one; templates live in their files
    editor::UndoStack history_{doc_};
    bool ui_updating_ = false;

    u64 built_ = 0;    // the library version the lists show
    u64 selected_ = 0; // template key
    std::string kind_, search_;
    std::vector<u64> card_keys_;
    std::vector<const objects::PropDef*> prop_refs_;
    std::unordered_map<u64, u32> icon_revs_; // key -> rev the icon was drawn for
    u64 props_built_ = 0;

    // Model mirrors
    std::vector<KindRow> m_kinds_;
    std::vector<Card> m_cards_;
    std::vector<CreateGroup> m_create_;
    std::vector<PropView> m_props_;
    Rml::String m_kind_, m_search_, m_count_;
    bool m_menu_ = false, m_details_ = false, m_has_sel_ = false;
    Rml::String m_sel_name_, m_sel_kind_, m_sel_kind_about_, m_sel_about_, m_sel_icon_, m_sel_file_, m_sel_kind_icon_;
    int m_total_ = 0;
};

} // namespace forge::editor_app
