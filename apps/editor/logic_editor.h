#pragma once

// The «Логика» tab: what happens in the game, as links between things.
//
// The board shows the game's things (the hero and object templates, each
// with its own picture from the object library). An arrow from one thing to
// another with a verb on it is a link: «Ключ открывает Дверь». Click a
// thing, then another one, and pick what the first does with the second
// (the list offers the other way round too). Click a link to refine it:
// only at night, only once, with a sound, a hint when it cannot happen.
// Things are dragged around the board; the empty board drags as a whole.
//
// The right column says everything in plain words. Links that cannot work
// (a verb the game lacks, a thing deleted from the library) are marked and
// say why.
//
// Links are the game's logic.json (verbs: verbs.json), written at once on
// each change; every change is one step of the tab's history (Ctrl+Z). The
// game reads them when it starts.
//
// The same logic has other views, switched on the mode strip above the
// board; switching changes nothing in the logic, and the editor remembers the
// author's mode:
//   «Идеи» — each link as a ready idea («Дверь с ключом») with a few
//   fields: which things it is about (a click picks another fitting thing)
//   and its refinements. «Новая идея» adds one from the game's ideas.json.
//   «Шаги» — each link as steps: when it happens, the checks, what is done,
//   what happens otherwise. Steps made by refinements are taken away with ×
//   and added with «Добавить шаг».
//   «Схема» — each link as a node graph to edit freely (scheme_view.h).
//   «Код» — the Luau the links become, line by line; a line belongs to its
//   link (a click selects it); «Править» gives a link its own code.
//
// While the game runs from the editor, links that happen light up in every
// view: the game writes their ids to a file the tab watches.

#include "forge/editor/document.h"
#include "forge/editor/undo.h"
#include "forge/level/level.h"
#include "forge/logic/logic.h"
#include "forge/objects/library.h"
#include "forge/ui/ui.h"
#include "scheme_view.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace forge::editor_app {

class LogicEditor {
public:
    explicit LogicEditor(level::LevelModule& module);

    // game_dir holds verbs.json; file is the links (logic.json).
    bool init(ui::Ui& ui, const std::filesystem::path& game_dir, const std::filesystem::path& file);
    // The events the game's module has (forge::modules::ModuleDef::events, set before init): verbs.json is read with
    // them, as the game reads it, and a verb of another event makes it fail. Empty: any.
    std::vector<std::string> events;
    // The file a game started from the editor writes the links that happen
    // to (one id a line); the tab lights them up.
    void set_fired_file(std::filesystem::path file) { fired_file_ = std::move(file); }
    // Where the author's mode is remembered (set before init; remember false:
    // neither read nor written, as in offscreen runs).
    void set_settings(std::filesystem::path folder, bool remember) {
        settings_ = std::move(folder);
        remember_ = remember;
    }
    void bind(Rml::DataModelConstructor& model);
    void set_model(Rml::DataModelHandle handle) {
        model_ = handle;
        scheme_.set_model(handle);
    }
    // A template's picture as the UI shows it (the object library's).
    std::function<std::string(const objects::Template&)> template_icon;
    // The open level's areas as things of the board («Зоны»), and a number
    // that changes whenever they do (the board is rebuilt then).
    std::function<std::vector<logic::Thing>()> level_areas;
    std::function<u64()> areas_version;
    // The game's levels as read from levels.json (logic::level_thing): where a link «уходит на уровень» may send
    // the hero. An entry not read is no level. areas_version changes with them too.
    std::function<std::vector<logic::Thing>()> levels;

    // Each frame while the tab is open.
    void update(Rml::Context* context);
    bool handle_event(const SDL_Event& e, f32 density, bool ui_used);
    bool handle_key(const SDL_KeyboardEvent& k);

    editor::UndoStack& history() { return history_; }
    void undo();
    void redo();
    std::string status() const;

    // --- actions (the board, buttons, keys, the self-test) ---
    const logic::Logic& links() const { return logic_; }
    const logic::Verbs& verbs() const { return verbs_; }
    // A thing onto the board (a template id or "hero"), at a free place.
    bool add_thing(const std::string& id);
    // Off the board, with its links.
    bool remove_thing(const std::string& id);
    bool move_thing(const std::string& id, f32 x, f32 y);
    // A click on a thing: selects it, or, with another thing selected, asks
    // what the selected one does with this one.
    void click_thing(const std::string& id);
    void click_board();
    bool picking() const { return !pick_a_.empty(); }
    usize pick_options() const { return m_verbs_.size(); }
    const std::string& pick_phrase(usize i) const { return m_verbs_[i].phrase; }
    // Option i of the picker: the link is made and selected.
    bool pick(usize i);
    void select_link(u32 id);
    void select_thing(const std::string& id);
    u32 selected_link() const { return sel_link_; }
    const std::string& selected_thing() const { return sel_thing_; }
    // Refinements of the selected link (or of link): "night", "once",
    // "sound", "hint".
    bool refine(const std::string& what, bool on);
    bool refine(u32 link, const std::string& what, bool on);
    // The view: "links" (the board), "ideas", "steps", "code".
    bool set_mode(const std::string& mode);
    const std::string& mode() const { return mode_; }
    // «Шаги»: the steps of a link as shown, and «Добавить шаг» on its card.
    std::vector<logic::Step> steps_of(u32 link) const;
    void open_adds(u32 link);
    // «Идеи»: the gallery of ideas, a new link from one (with the first
    // fitting things), and another thing for a field of a link.
    void open_gallery(bool open);
    bool gallery_open() const { return m_gallery_; }
    bool add_idea(const std::string& idea);
    // The chooser of a field: side "a" or "b" of link.
    void open_choices(u32 link, const std::string& side);
    usize choices() const { return m_choices_.size(); }
    const std::string& choice(usize i) const { return m_choices_[i].id; }
    bool choose(const std::string& thing);
    const logic::Idea* idea_of(u32 link) const;
    // «Куда» of a link that sends the hero to a level: the level (its id in the list) and where it comes out
    // there ("" its spawn point, or one of its areas), as one step of the history. Another level keeps the
    // area only when it is that level's.
    bool set_destination(u32 link, const std::string& level, const std::string& arrive);
    bool choose_level(const std::string& level);   // of the selected link
    bool choose_arrival(const std::string& arrive); // of the selected link
    // What «Куда» offers for the selected link (ids: level ids; "" and area ids), and which is chosen.
    std::vector<std::string> level_choices() const;
    std::vector<std::string> arrival_choices() const;
    // A link happened in the running game: lit for a moment (the game's
    // file does this; tests call it).
    void light(u32 link);
    bool lit(u32 link) const { return lit_.contains(link); }
    // «Код»: the lines shown and the link of each (0 none).
    usize code_lines() const { return m_code_.size(); }
    u32 code_link(usize line) const { return static_cast<u32>(m_code_[line].link); }
    bool remove_link();
    // «Код» block: a link's own code instead of its verb's action. edit_code
    // opens it (the verb's action as code when it has none yet) in «Код»;
    // save_code checks and keeps it (false and a message when it does not
    // compile); reset_code gives the link its verb back.
    bool edit_code(u32 link);
    bool editing_code() const { return editing_ != 0; }
    void set_code_text(std::string text);
    bool save_code(std::string* error = nullptr);
    void cancel_code();
    bool reset_code(u32 link);
    // Plain words: the phrase and the meaning of a link ("" when unknown).
    std::string phrase_of(u32 link) const;
    std::string meaning_of(u32 link) const;
    // Why a link cannot work ("" when it can).
    std::string problem_of(u32 link) const;
    usize things_on_board() const { return m_things_.size(); }
    // Where thing i is drawn (board pixels, after panning).
    bool thing_at(const std::string& id, f32& x, f32& y) const;

    // A thing's own scheme (no link): made when it has none, then shown in
    // «Схема». Returns its id (0: not a thing that can have one).
    u32 thing_scheme(const std::string& thing);
    bool remove_thing_scheme(u32 id);

private:
    struct ThingView {
        Rml::String id, name, icon;
        float x = 0, y = 0;
        int links = 0;
        bool selected = false, broken = false;
    };
    struct LinkView {
        int id = 0;
        Rml::String verb, phrase, icon;
        float lx = 0, ly = 0, len = 0, angle = 0; // the line: start, length, degrees
        float hx = 0, hy = 0;                     // the arrowhead
        float cx = 0, cy = 0;                     // the verb chip
        int refined = 0;
        bool selected = false, broken = false, lit = false;
    };
    struct NavRow {
        Rml::String id, name, icon, about;
        int links = 0;
        bool selected = false;
    };
    struct VerbOption {
        Rml::String verb, phrase, about, icon;
        bool reversed = false;
    };
    struct Refinement {
        Rml::String id, label, hint;
        bool on = false;
    };
    struct WordRow {
        int id = 0;
        Rml::String phrase, meaning, problem;
        bool selected = false, lit = false;
    };
    struct StepView {
        Rml::String part, label, icon, text, refine;
    };
    struct AddView {
        Rml::String id, label, about;
    };
    struct CardView {
        int id = 0;
        Rml::String phrase, meaning, problem;
        bool selected = false, adding = false, lit = false;
        std::vector<StepView> steps;
        std::vector<AddView> adds;
    };
public:
    // «Схема»: the links as node graphs.
    SchemeView& scheme() { return scheme_; }

private:
    struct CodeLine {
        int n = 0, link = 0;
        Rml::String text;
        bool selected = false, comment = false, lit = false;
    };
    struct FieldView {
        Rml::String side, label, thing, name, icon;
    };
    struct IdeaCard {
        int id = 0;
        Rml::String name, icon, phrase, problem;
        bool selected = false, lit = false;
        std::vector<FieldView> fields;
        std::vector<Refinement> refine;
    };
    struct IdeaView {
        Rml::String id, name, icon, about, group;
        bool can = true; // the game has things for it
    };
    struct ChoiceView {
        Rml::String id, name, icon;
        bool current = false;
    };
    struct GoOption {
        Rml::String id, key, name, about; // key: for the element's id («spawn», an area's hex)
        bool current = false;
        bool operator==(const GoOption&) const = default;
    };

    void load();
    void rebuild();
    void rebuild_side();
    void rebuild_steps();
    void rebuild_code();
    void rebuild_ideas();
    void watch_fired();
    void watch_file();
    // Two things the verb suits, the board's first and not linked so yet.
    bool pair_for(const logic::VerbDef& verb, std::string& a, std::string& b) const;
    void remember_mode() const;
    // Records a change: after is the whole new logic.
    void change(const logic::Logic& after, std::string label, std::string merge = {});
    // A link gets this scheme (logic::set_scheme), as one step of the history.
    void change_scheme(u32 link, const script::Graph& graph, std::string label, std::string merge = {});
    void change_thing_scheme(u32 id, const script::Graph& graph, std::string label, std::string merge = {});
    void apply_json(const std::string& json);
    void save();
    const logic::Thing* thing(std::string_view id) const;
    std::string icon_of(const std::string& id);
    void place_free(logic::Logic& l, const std::string& id) const;
    void open_picker(const std::string& a, const std::string& b);
    void place_picker();
    void close_picker();
    template <typename T>
    void set(T& member, const T& value, const char* name) {
        if (member == value) return;
        member = value;
        if (model_) model_.DirtyVariable(name);
    }

    friend class LogicCommand;
    friend class SchemeView;

    level::LevelModule& module_;
    ui::Ui* ui_ = nullptr;
    Rml::DataModelHandle model_;
    editor::Document doc_; // the history needs one; the links live in their file
    editor::UndoStack history_{doc_};
    std::filesystem::path game_dir_, file_, settings_;
    bool remember_ = false;
    std::string mode_ = "links";
    u32 adding_ = 0; // the card whose «Добавить шаг» is open
    u32 editing_ = 0; // the link whose code is open in «Код»
    u32 choose_link_ = 0;
    std::string choose_side_;
    std::filesystem::path fired_file_;
    u64 fired_at_ = 0, fired_checked_ = 0, fired_size_ = 0; // bytes read; when last looked (ms); size seen
    std::filesystem::file_time_type fired_time_{};
    std::string fired_run_;
    std::map<u32, u64> lit_;               // link -> until (ms)
    // The links' file as last read or written, and when last looked (ms): the
    // running game may change it.
    std::filesystem::file_time_type file_time_{};
    u64 file_checked_ = 0;

    SchemeView scheme_{*this};
    logic::Verbs verbs_;
    logic::Ideas ideas_;
    logic::Logic logic_;
    std::vector<logic::Thing> things_; // the hero and the templates
    std::vector<logic::Thing> level_things_; // the game's levels: where links lead, never on the board
    std::map<u32, std::string> problems_;
    std::vector<logic::Problem> compiled_; // every problem the logic has (the scheme marks nodes with them)
    bool keep_compiled_ = false;           // the next rebuild only moved nodes: the code is the same
    u64 built_lib_ = ~0ull, built_areas_ = ~0ull;
    bool dirty_ = true;
    std::string hero_icon_, area_icon_;

    std::string sel_thing_;
    u32 sel_link_ = 0;
    std::string pick_a_, pick_b_;
    f32 pan_x_ = 0, pan_y_ = 0;
    f32 board_w_ = 0, board_h_ = 0; // as last laid out
    // A drag: of a thing (id) or of the board (empty id).
    bool grabbing_ = false, dragged_ = false;
    std::string grab_id_;
    f32 grab_mx_ = 0, grab_my_ = 0, grab_x_ = 0, grab_y_ = 0;
    bool hint_closed_ = false;

    // Model mirrors
    std::vector<ThingView> m_things_;
    std::vector<LinkView> m_links_;
    std::vector<NavRow> m_board_rows_, m_add_rows_;
    std::vector<VerbOption> m_verbs_;
    std::vector<Refinement> m_refine_;
    std::vector<WordRow> m_words_, m_thing_links_;
    std::vector<CardView> m_cards_;
    std::vector<CodeLine> m_code_;
    std::vector<IdeaCard> m_idea_cards_;
    std::vector<IdeaView> m_ideas_;
    std::vector<ChoiceView> m_choices_;
    std::vector<GoOption> m_go_levels_, m_go_arrivals_;
    bool m_sel_go_ = false;
    Rml::String m_choose_title_;
    bool m_gallery_ = false, m_choosing_ = false;
    Rml::String m_mode_ = "links";
    Rml::String m_pick_title_, m_sel_phrase_, m_sel_meaning_, m_sel_problem_, m_sel_name_, m_sel_icon_, m_count_;
    float m_pick_x_ = 0, m_pick_y_ = 0;
    bool m_editing_ = false, m_sel_code_ = false, m_sel_scheme_ = false, m_sel_own_ = false, m_sel_area_ = false;
    Rml::String m_edit_text_, m_edit_title_, m_edit_error_;
    bool m_picking_ = false, m_has_link_ = false, m_has_thing_ = false, m_hint_ = false;
    int m_sel_links_ = 0;
};

} // namespace forge::editor_app
