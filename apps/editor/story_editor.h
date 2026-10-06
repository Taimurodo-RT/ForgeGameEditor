#pragma once

// The «Сюжет» tab: the game's conversations, written like a play.
//
// «Сценарий» shows a conversation as a script: who speaks, the line, a stage
// direction in italics above it. Coloured marks say when a line is spoken
// (blue: «если задание «Потерянная кирка»: ищет кирку»), what it changes
// (violet: «задание → выполнено», «герою: Монеты +30») and where the talk
// goes on (grey: «дальше: «Не сходишь?…»», a click jumps there). The hero's
// answers sit under the line; for questions the player types, the topics
// list a few words each (any form of a word matches) with the answer next to
// them. Everything is edited in place: a click on a line, an answer, a
// topic's words or a scene's title opens it for typing (Enter keeps it, Esc
// cancels); a click on a mark offers a list (quest stages, the game's things,
// the other lines) instead of formulas.
//
// The right column plays the conversation without the game: what the game
// knows (quest stages, what the hero carries) is set with buttons, the line
// being shown is lit in the script, and every step is logged.
//
// Conversations are the game's dialogues/*.json (dialogue.h), written at
// once on each change; every change is one step of the tab's history. A
// folder in dialogues/ is one story of several talks (an imported Ren'Py
// game): its talks jump into each other ("ch1:ch1_main"), the test follows
// them. «Импорт из Ren'Py…» brings such a story in (converters/renpy.py).
// A big talk shows a page of lines at a time; its scenes are listed on the
// left. An imported story also has its cast («Персонажи»: names, colours,
// poses, how much each says) and the game's settings («Настройки новеллы»,
// kept for the visual novel module), each a page of its own.

#include "converters.h"
#include "forge/editor/document.h"
#include "forge/editor/undo.h"
#include "forge/game/dialogue.h"
#include "forge/game/dialogue_source.h"
#include "forge/game/quests.h"
#include "forge/objects/library.h"
#include "forge/ui/ui.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <mutex>
#include <string>
#include <vector>

namespace forge::editor_app {

class StoryEditor {
public:
    explicit StoryEditor(objects::Library* library) : library_(library) {}

    // game_dir holds dialogues/ and quests.json.
    bool init(ui::Ui& ui, const std::filesystem::path& game_dir);
    void bind(Rml::DataModelConstructor& model);
    void set_model(Rml::DataModelHandle handle) { model_ = handle; }
    // A template's picture as the UI shows it (the object library's).
    std::function<std::string(const objects::Template&)> template_icon;
    // For the folder dialog of «Импорт из Ren'Py…».
    SDL_Window* window = nullptr;

    void update(Rml::Context* context);
    bool handle_event(const SDL_Event& e);
    bool handle_key(const SDL_KeyboardEvent& k);

    editor::UndoStack& history() { return history_; }
    void undo();
    void redo();
    std::string status() const;

    // --- actions (clicks, keys, the self-test) ---
    // Conversations: their ids (file names without .json; "story/name" for
    // the talks of a story folder), and the open one.
    std::vector<std::string> talks() const;
    bool open(const std::string& talk);
    const std::string& opened() const { return talk_; }
    const game::DialogueSource& source() const { return src_; }
    // A new conversation, opened, its first line open for typing.
    std::string new_talk();

    // Typing: what is "text"/"note"/"scene" (of a node), "choice"/"topic"/
    // "words" (with an index), "fallback" (of a node), "speaker" (a speaker's
    // key), "cond"/"act" (raw source of a mark, see edit_mark).
    bool begin_edit(const std::string& what, const std::string& node, int index = -1);
    bool editing() const { return !edit_what_.empty(); }
    void set_edit_text(std::string text);
    bool commit_edit();
    void cancel_edit();

    // Structure.
    std::string add_line_after(const std::string& node); // the new node's id
    bool add_choice(const std::string& node);
    bool add_topic(const std::string& node);
    std::string add_scene();
    bool remove_line(const std::string& node);
    bool remove_choice(const std::string& node, int index);
    bool remove_topic(const std::string& node, int index);

    // Marks: a click opens a list of what it can be. where: "start" (index =
    // entry), "branch" (node, index), "choice" (node, index), "node" (node),
    // "topic" (node, index), "fallback" (node); part: "if", "do" (mark index
    // in mark), "goto", "speaker". mark -1 on "do" adds one.
    bool open_menu(const std::string& where, const std::string& node, int index, const std::string& part, int mark = -1);
    bool menu_open() const { return m_menu_; }
    usize menu_items() const { return menu_.size(); }
    const std::string& menu_text(usize i) const { return m_menu_items_[i].text; }
    // The item whose text is text (for tests); -1 none.
    int menu_find(const std::string& text) const;
    bool menu_pick(usize i);
    void close_menu();
    // Scrolls the script to a line and flashes it ("talk:node" opens that
    // talk of the story first).
    void jump(const std::string& node);
    // A big talk: the lines shown (a page), moved by the page buttons.
    usize lines_shown() const { return m_lines_.size(); }
    bool line_shown(const std::string& node) const;
    void page(int dir);

    // A Ren'Py game (its folder or one of its .rpy files) becomes a story
    // folder in dialogues/; done in the background, then its first talk opens.
    bool import_renpy(const std::filesystem::path& source);
    bool importing() const { return import_job_ != 0; }
    // A story's cast and settings pages (false when it has none); open()
    // goes back to the script.
    bool show_cast(const std::string& story);
    bool show_novel(const std::string& story);
    const std::string& view() const { return view_; } // "" the script, "cast", "novel"
    usize cast_size() const { return m_cast_.size(); }
    std::string cast_name(usize i) const { return m_cast_[i].name; }
    usize setting_groups() const { return m_novel_groups_.size(); }

    // The test on the right.
    void play();
    bool playing() const { return runner_.active(); }
    std::string play_node() const { return std::string(runner_.node_id()); }
    const game::DialogueLine& play_line() const { return runner_.line(); }
    bool play_choose(usize visible);
    bool play_next();
    bool play_ask(const std::string& typed);
    // What the game knows: a quest's stage (cycled), an item's count, a
    // variable the talk's conditions read.
    void set_quest(const std::string& quest, f64 value);
    void step_var(const std::string& var, int dir);
    void set_var(const std::string& var, f64 value) { vars_.set(var, value); dirty_ = true; }
    void step_item(const std::string& item, int dir);
    void step_quest(const std::string& quest, int dir);
    f64 var(const std::string& name) const { return vars_.get(name).number(); }
    // Plain words of a condition or an action, as the marks show them.
    std::vector<std::string> phrases(const std::string& source, bool action) const;

private:
    struct Chip {
        Rml::String kind, icon, text, part; // kind: cond, eff, go, else, stage, code; part: if, do, goto, next, call, stage
        int mark = -1;
        bool seq = false; // "next" to the line just below
        Rml::String look;  // its classes ("go seq")
    };
    struct RuleView {
        int index = 0;
        bool lit = false;
        std::vector<Chip> chips;
    };
    struct ChoiceView {
        int index = 0;
        Rml::String text;
        bool lit = false, editing = false;
        std::vector<Chip> chips;
    };
    struct TopicView {
        int index = 0;
        Rml::String words, text, answer; // answer: the node shown inline ("" = a jump chip)
        bool lit = false, editing_words = false, editing_text = false;
        std::vector<Chip> chips;
    };
    struct LineView {
        Rml::String id, kind, scene, note, who, color, text, fallback, pose;
        bool lit = false, flash = false, editing = false, editing_note = false, editing_scene = false, editing_fallback = false;
        bool topics_on = false, choices_on = false, can_add = false, has_fallback = false;
        bool seq = false; // goes on to the line just below
        int editing_stage = -1; // the staging step being typed
        std::vector<Chip> chips; // when / changes / where next
        std::vector<Chip> stage; // what happens on screen first
        std::vector<ChoiceView> choices;
        std::vector<TopicView> topics;
    };
    struct TalkRow {
        Rml::String kind, id, name, letter, color; // kind: talk, story (a folder), scene (of the open talk)
        int lines = 0;
        bool selected = false, open = false;
    };
    struct TalkInfo { // a talk file's summary, kept while the file does not change
        std::filesystem::file_time_type time;
        std::string name, color;
        int lines = 0;
    };
    struct CastView { // a character of a story's cast
        Rml::String key, name, letter, color, sub, poses, talks, extra;
        int lines = 0, pose_count = 0;
    };
    struct SettingRow {
        Rml::String name, value;
    };
    struct SettingGroup {
        Rml::String name;
        std::vector<SettingRow> items;
    };
    struct SpeakerRow {
        Rml::String key, name, letter, color;
        bool editing = false;
    };
    struct MenuItem {
        Rml::String text, icon, group;
        bool current = false;
    };
    struct StateRow {
        Rml::String id, kind, name, value, icon; // kind: quest, item
        int count = 0;
    };
    struct LogRow {
        Rml::String icon, text, kind;
    };
    // What a menu item does.
    struct MenuAction {
        std::string value; // the new source / node id; "\x01new" a new line, "\x01raw" typing
    };

    void load_quests();
    void place_menu(Rml::Event& ev);
    void load_talk();
    game::Dialogue& open_dialogue(); // the open talk, as the test plays it
    const game::Dialogue* resolve(std::string_view talk);
    // The test went into another talk of the story: show that one.
    void follow();
    std::string story_of(const std::string& talk) const; // "ddlc" of "ddlc/script-ch1", "" for a lone talk
    std::string full_id(const std::string& ref) const;    // "script-ch1" in the open story → "ddlc/script-ch1"
    std::vector<std::string> vars_used() const;
    void poll_import();
    std::filesystem::path story_file(const std::string& story, const char* name) const;
    void rebuild();
    void rebuild_play();
    void rebuild_state();
    void rebuild_talks();
    // Records a change of the open conversation.
    void change(const game::DialogueSource& after, std::string label);
    void apply(const std::string& talk, const std::string& json);
    bool write(const std::string& talk, const std::string& json) const;
    std::filesystem::path talk_path(const std::string& talk) const;
    void reload_play(bool keep_place);
    void log(const char* icon, std::string text, const char* kind = "");
    void log_line(); // the runner's current line, narrator lines without "speaker:"
    game::Value call(std::string_view name, const std::vector<game::Value>& args);
    // Phrases.
    std::string item_name(const std::string& id) const;
    std::string stage_name(const game::Quest& q, f64 at) const;
    const game::Quest* quest_of_var(std::string_view var) const;
    std::string anchor(const std::string& node) const;
    std::vector<Chip> cond_chips(const std::string& cond, bool otherwise) const;
    std::vector<Chip> act_chips(const std::string& act) const;
    std::vector<Chip> stage_chips(const std::vector<std::string>& stage) const;
    std::string stage_phrase(const std::string& stage) const;
    Chip go_chip(const std::string& go, bool next) const;
    // A node answering a topic, shown inline (only reached from there).
    bool inline_answer(const std::string& node, const std::string& asker) const;
    std::vector<std::string> items_used() const;
    std::string* edit_target(game::DialogueSource& s) const;
    template <typename T>
    void set(T& member, const T& value, const char* name) {
        if (member == value) return;
        member = value;
        if (model_) model_.DirtyVariable(name);
    }

    friend class StoryCommand;

    objects::Library* library_;
    ui::Ui* ui_ = nullptr;
    Rml::Context* context_ = nullptr;
    Rml::DataModelHandle model_;
    editor::Document doc_; // the history needs one; the talks live in their files
    editor::UndoStack history_{doc_};
    std::filesystem::path game_dir_;
    std::string talk_;
    game::DialogueSource src_;
    game::QuestBook quests_;
    bool dirty_ = true;

    // Typing.
    std::string edit_what_, edit_node_;
    int edit_index_ = -1;
    bool focus_edit_ = false;
    std::string flash_;
    u64 flash_until_ = 0;
    std::string scroll_to_;

    // The open menu.
    std::string menu_where_, menu_node_, menu_part_;
    int menu_index_ = -1, menu_mark_ = -1;
    std::vector<MenuAction> menu_;

    // The test. Every talk played is kept here (the open one and those the
    // story went into), at stable addresses for the runner.
    game::Vars vars_;
    std::map<std::string, std::unique_ptr<game::Dialogue>> loaded_;
    bool runner_started_ = false, replay_ = false; // the test began; begin it again
    game::DialogueRunner runner_{vars_, [this](std::string_view n, const std::vector<game::Value>& a) { return call(n, a); }};
    int lit_rule_ = -1, lit_choice_ = -1, lit_topic_ = -1;
    std::string lit_choice_node_, lit_topic_node_;

    // Model mirrors.
    std::vector<TalkRow> m_talks_;
    mutable std::map<std::string, TalkInfo> talk_info_;
    std::set<std::string> stories_open_; // story folders unfolded in the list
    // A big talk's page: from this line (index among the lines shown).
    usize page_from_ = 0;
    std::string page_to_; // a line to bring onto the page at the next rebuild
    Rml::String m_page_text_;
    bool m_page_on_ = false, m_page_prev_ = false, m_page_next_ = false;
    // Import.
    Converters import_converters_;
    bool import_loaded_ = false;
    u64 import_job_ = 0;
    std::filesystem::path import_source_, import_staging_;
    std::mutex import_mutex_;
    std::vector<std::filesystem::path> import_picked_; // from the folder dialog (another thread)
    Rml::String m_import_text_;
    // The cast and settings pages.
    std::string view_;
    Rml::String m_view_story_, m_novel_title_, m_novel_info_;
    std::vector<CastView> m_cast_;
    std::vector<SettingGroup> m_novel_groups_;
    std::vector<SpeakerRow> m_speakers_;
    std::vector<RuleView> m_rules_;
    std::vector<LineView> m_lines_;
    Rml::String m_title_, m_count_;
    Rml::String m_edit_text_;
    int m_edit_rows_ = 2;
    bool m_menu_ = false;
    Rml::String m_menu_title_;
    std::vector<MenuItem> m_menu_items_;
    float m_menu_x_ = 0, m_menu_y_ = 0;
    // The test.
    Rml::String m_play_who_, m_play_color_, m_play_text_, m_ask_text_;
    std::vector<Rml::String> m_play_choices_;
    bool m_play_on_ = false, m_play_asks_ = false, m_play_next_ = false;
    std::vector<StateRow> m_state_;
    std::vector<LogRow> m_log_;
};

} // namespace forge::editor_app
