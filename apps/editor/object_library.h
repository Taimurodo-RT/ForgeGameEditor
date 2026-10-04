#pragma once

// The «Объекты» tab: the game's object templates.
//
// The library is for finding and using objects: the left column sorts them
// by genre ("Платформер", "RPG") and by kind ("Подбираемое", "Житель"), the
// middle shows them as cards, the right column tells what the selected one
// is. Here an object is only placed on the level, copied, renamed, deleted,
// given a genre, or opened in its editor (buttons, keys, the right-click
// menu). Its properties are set in the object's editor, which takes the
// middle and right of the tab until «К библиотеке».
//
// «Общие» shows the shared library: objects kept outside any game (a folder
// of this computer) for every game to take. Objects go between it and the
// game as copies: «Сделать общим» puts the game's object there (or brings
// the shared one up to date), «Взять в игру» copies a shared one into the
// game (or brings the game's one up to date); the game always has its own
// copy, so it runs without the shared library.
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

#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace forge::editor_app {

class ObjectLibrary {
public:
    explicit ObjectLibrary(level::LevelModule& module);
    // Where the shared library is; set before init (empty: none).
    void set_shared_folder(std::filesystem::path folder) { shared_folder_ = std::move(folder); }

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

    // --- actions (buttons, keys, the menu, the self-test) ---
    objects::Library& library() { return *module_.library(); }
    objects::Library& shared() { return shared_; }
    // «Общие» is shown: the cards are the shared library's.
    bool showing_shared() const { return place_ == "s:"; }
    const objects::Template* selected() const;
    void select(u64 key);
    // Which templates the cards show: "" all, "g:Платформер" a genre ("g:"
    // for any game), "k:pickup" a kind.
    void show(const std::string& place);
    const std::string& place() const { return place_; }
    void set_search(const std::string& text);
    // A new template of a kind: from its preset (index), or empty (-1).
    bool create(const std::string& kind, int preset);
    bool duplicate();
    bool remove_selected();
    bool rename(const std::string& name);
    void start_rename();
    bool renaming() const { return renaming_; }
    bool set_genre(const std::string& genre);
    bool place_selected();
    // The selected game object into the shared library (replacing an older
    // copy of it there).
    bool share_selected();
    // The selected shared object into the game (replacing an older copy).
    bool take_selected();
    // The object's editor: its properties in plain words.
    void open_editor();
    void close_editor();
    bool editing() const { return editing_ != 0; }
    bool set_about(const std::string& about);
    // The object's blocks («Тело», «Подбирается»): added with the ones it
    // needs, taken away with the ones that need it.
    bool add_block(const std::string& block);
    bool remove_block(const std::string& block);
    usize block_count() const { return m_blocks_.size(); }
    const std::string& block_id(usize i) const { return m_blocks_[i].id; }
    // Row i of the editor's properties, set from text as the user types it.
    void set_prop(int i, const std::string& text, bool dragging);
    // The object's own picture (in its editor), chosen from the project's
    // images: the file is copied into the library's pictures folder, so it
    // goes with the game.
    std::function<std::vector<std::filesystem::path>()> list_images; // absolute paths
    bool set_picture(const std::filesystem::path& source);
    bool clear_picture();
    void open_pictures();
    void close_pictures();
    bool pictures_open() const { return m_pics_open_; }
    void set_picture_search(const std::string& text);
    usize picture_choices() const { return pic_files_.size(); }
    const std::string& picture_choice(usize i) const { return m_pics_[i].name; }
    bool choose_picture(usize i);
    // The right-click menu: over card i, or over empty space (-1); x, y in
    // window pixels.
    void context_menu(int card, f32 x, f32 y);
    bool menu_open() const { return m_menu_ != ""; }
    usize cards() const { return m_cards_.size(); }
    const std::string& card_name(usize i) const { return m_cards_[i].name; }

private:
    struct NavRow {
        Rml::String place, name, icon, about;
        int count = 0;
        bool selected = false;
    };
    struct Card {
        Rml::String name, kind, genre, icon, about;
        Rml::String badge; // "общий", "в игре", "отличается"
        bool selected = false, renaming = false;
    };
    struct PresetRow {
        Rml::String kind, name, genre, about;
        int index = -1;
    };
    struct CreateGroup {
        Rml::String kind, name, icon, about;
        std::vector<PresetRow> presets;
    };
    struct GenreItem {
        Rml::String name, label;
        bool checked = false;
    };
    struct PropView {
        Rml::String kind; // text, slider, bool, enum
        Rml::String block; // the block it belongs to
        Rml::String label, value, hint;
        float min = 0, max = 0, step = 0;
        bool advanced = false;
        int index = 0; // in m_props_: what the events name
    };
    struct BlockView {
        Rml::String id, name, icon, about, note;
        bool removable = true;
        std::vector<PropView> props;
    };
    struct PicView {
        Rml::String name, folder, icon;
    };

    void rebuild();
    void rebuild_side();
    void change(objects::Template after, std::string label, std::string merge = {});
    void change(objects::Library& lib, objects::Template after, std::string label, std::string merge = {});
    objects::Library& shown() { return showing_shared() ? shared_ : library(); }
    const objects::Library& shown() const { return showing_shared() ? shared_ : *module_.library(); }
    // How the template stands with the other library: "" not there,
    // "same", "differs".
    std::string twin(const objects::Template& t) const;
    std::string icon_path(const objects::Template& t, bool from_shared);
    void set_menu(const std::string& menu);
    void rebuild_pictures();
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

    objects::Library shared_;
    std::filesystem::path shared_folder_;
    bool shared_ready_ = false;
    u64 built_ = 0;    // the library version the lists show
    u64 shared_built_ = 0;
    u64 selected_ = 0; // template key
    u64 editing_ = 0;  // the template open in the editor
    bool renaming_ = false, rename_focus_ = false;
    std::string place_, search_;
    std::vector<u64> card_keys_;
    std::vector<const objects::PropDef*> prop_refs_;
    std::unordered_map<u64, u32> icon_revs_; // key -> look the icon was drawn for
    std::unordered_map<u64, u32> shared_icon_revs_;
    std::vector<std::filesystem::path> pic_files_; // what the chooser shows
    std::unordered_map<std::string, std::string> pic_icons_; // file -> ui image name
    std::string pic_search_;

    // Model mirrors
    std::vector<NavRow> m_genres_, m_kinds_;
    NavRow m_all_, m_shared_;
    std::vector<Card> m_cards_;
    std::vector<CreateGroup> m_create_;
    std::vector<GenreItem> m_genre_items_;
    std::vector<PropView> m_props_;
    Rml::String m_search_, m_count_, m_menu_; // menu: "", "new", "card", "empty", "blocks"
    float m_menu_x_ = 0, m_menu_y_ = 0;
    std::vector<BlockView> m_blocks_, m_add_blocks_;
    std::vector<PicView> m_pics_;
    Rml::String m_pics_search_, m_pics_note_, m_sel_picture_;
    bool m_pics_open_ = false;
    bool m_details_ = false, m_has_sel_ = false, m_editing_ = false, m_in_shared_ = false;
    Rml::String m_sel_twin_, m_shared_note_;
    Rml::String m_sel_name_, m_sel_kind_, m_sel_kind_about_, m_sel_about_, m_sel_icon_, m_sel_file_, m_sel_kind_icon_,
        m_sel_genre_;
    int m_total_ = 0;
};

} // namespace forge::editor_app
