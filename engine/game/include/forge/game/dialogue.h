#pragma once

// Dialogues as data, played by a runner that needs nothing else: the same
// file runs in the game, in the editor's preview and in a test.
//
//   {
//     "id": "miner",
//     "speakers": {"miner": {"name": "Старый шахтёр", "color": "#e8b04a"}},
//     "start": [{"if": "quest.pickaxe == 2", "goto": "thanks"}, {"goto": "hello"}],
//     "nodes": [
//       {"id": "hello", "speaker": "miner", "text": "Здравствуй, {hero.name}!",
//        "do": "met_miner = 1", "next": "ask"},
//       {"id": "ask", "speaker": "miner", "text": "Поможешь старику?",
//        "choices": [
//          {"text": "Помогу.", "do": "quest.pickaxe = 1", "goto": "thanks_ahead"},
//          {"text": "Где кирка?", "if": "quest.pickaxe >= 1", "goto": "where"},
//          {"text": "Некогда.", "goto": "end"}]},
//       {"id": "talk", "speaker": "miner", "text": "Спрашивай.",
//        "keywords": [{"words": ["кирк*", "инструмент"], "goto": "where"}],
//        "fallback": "dunno",
//        "choices": [{"text": "Пока.", "goto": "end"}]}
//     ]
//   }
//
// A node shows one line. With choices the player picks one; with keywords
// the player may also type a question, matched by words ("кирк*" matches
// any word starting with "кирк"). Without either, "next" follows (or the
// dialogue ends). A node without text is a junction: "branches" (like
// "start") pick where to go. "goto": "end" ends it. "if" hides a choice or skips a
// start entry; "do" runs actions (see vars.h) when the node is shown or the
// choice is taken.

#include "forge/game/vars.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace forge::game {

struct Speaker {
    std::string name;
    std::string color;    // "#rrggbb", empty = the theme's colour
    std::string portrait; // image path, may be empty
};

struct DialogueChoice {
    std::string text;
    Expr condition;
    Expr actions;
    std::string go_to; // node id, or empty / "end" to finish
};

struct DialogueKeyword {
    std::vector<std::string> words; // lower case; "stem*" matches by prefix
    Expr condition;
    Expr actions;
    std::string go_to;
};

struct DialogueEntry {
    Expr condition;
    std::string go_to;
};

struct DialogueNode {
    std::string id;
    std::string speaker; // key in speakers, empty = narrator
    std::string text;
    Expr actions;        // run when the node is shown
    std::vector<DialogueChoice> choices;
    std::vector<DialogueKeyword> keywords;
    std::string fallback; // node shown when a typed question matches nothing
    // A node without text is a junction: after its actions the first branch
    // whose condition holds is taken, else next.
    std::vector<DialogueEntry> branches;
    std::string next;
};

// Problems found when loading: errors stop the dialogue from loading,
// warnings (an unreachable node, an unknown function) do not.
struct DialogueReport {
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    bool ok() const { return errors.empty(); }
};

class Dialogue {
public:
    // known_calls: functions the game answers; others are reported as warnings
    // (empty = do not check).
    bool load(std::string_view json, DialogueReport& report, const std::vector<std::string>& known_calls = {});

    const std::string& id() const { return id_; }
    const DialogueNode* node(std::string_view id) const;
    const Speaker* speaker(std::string_view key) const;
    const std::vector<DialogueNode>& nodes() const { return nodes_; }
    const std::vector<DialogueEntry>& entries() const { return entries_; }

private:
    std::string id_;
    std::unordered_map<std::string, Speaker> speakers_;
    std::vector<DialogueNode> nodes_;
    std::unordered_map<std::string, u32> index_;
    std::vector<DialogueEntry> entries_;
};

// What the player sees right now.
struct DialogueLine {
    std::string speaker; // display name
    std::string color;
    std::string portrait;
    std::string text;    // with variables put in
    std::vector<std::string> choices; // only the ones whose condition holds
    bool asks_keyword = false;         // the player may type a question
};

// Plays one dialogue at a time. Variables live outside (the save file);
// calls go to the game.
class DialogueRunner {
public:
    DialogueRunner(Vars& vars, CallFn call = {}) : vars_(vars), call_(std::move(call)) {}

    // False when no start entry applies (nothing to say right now).
    bool start(const Dialogue& dialogue);
    // Jumps straight to a node (editor preview).
    bool start_at(const Dialogue& dialogue, std::string_view node);
    void stop();

    bool active() const { return dialogue_ != nullptr; }
    const DialogueLine& line() const { return line_; }
    std::string_view node_id() const;

    // A line without choices: show the next one (or end).
    void advance();
    // index into line().choices
    bool choose(u32 index);
    // A typed question: true when a keyword matched; otherwise the fallback
    // node is shown (or nothing happens without one).
    bool ask(std::string_view typed);

    // Lines shown so far in this dialogue, oldest first (for a log view).
    const std::vector<DialogueLine>& history() const { return history_; }

private:
    void show(std::string_view id);
    void build_line();

    Vars& vars_;
    CallFn call_;
    const Dialogue* dialogue_ = nullptr;
    const DialogueNode* node_ = nullptr;
    std::vector<u32> visible_; // line_.choices index -> node choice index
    DialogueLine line_;
    std::vector<DialogueLine> history_;
    u32 steps_ = 0; // guards against a loop of nodes without player input
};

// Lower case for ASCII and Cyrillic (UTF-8).
std::string to_lower_utf8(std::string_view text);
// Splits typed text into lower-case words (letters and digits).
std::vector<std::string> split_words(std::string_view text);
bool keyword_matches(const std::vector<std::string>& words, std::string_view keyword);

} // namespace forge::game
