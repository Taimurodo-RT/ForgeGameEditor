#pragma once

// A dialogue file as the author wrote it: the same JSON as dialogue.h, kept
// as text (conditions and actions stay their source strings), so an editor
// can change it and write it back. Dialogue (dialogue.h) is what plays it.
//
// Fields only the editor reads (the game ignores them):
//   "scene": "Первая встреча" — a node that begins a part of the talk;
//   "note":  "Борис сидит на бревне..." — a stage direction above the line;
//   "stage": ["scene bg club_day", "show monika 5 at t11", "play music t2"]
//            — what happens on screen before the line (a visual novel's
//            staging, as Ren'Py writes it; a visual novel module plays it);
//   "pose":  "1u" — the speaker's look while saying the line.

#include "forge/core/types.h"

#include <string>
#include <string_view>
#include <vector>

namespace forge::game {

struct SourceEntry {
    std::string cond; // "if"
    std::string go;   // "goto"
};

struct SourceChoice {
    std::string text;
    std::string cond, act; // "if", "do"
    std::string go;
};

struct SourceKeyword {
    std::vector<std::string> words;
    std::string cond, act;
    std::string go;
};

struct SourceNode {
    std::string id;
    std::string scene, note;
    std::string speaker;
    std::string text;
    std::string act; // "do"
    std::vector<SourceEntry> branches;
    std::vector<SourceKeyword> keywords;
    std::string fallback;
    std::vector<SourceChoice> choices;
    std::string next;
    std::string call; // played first, comes back to next
    std::vector<std::string> stage;
    std::string pose;

    // Without text: a junction that only picks where to go.
    bool junction() const { return text.empty() && !branches.empty(); }
    // Without text and choices: passed through (staging, a call, a jump).
    bool silent() const { return text.empty() && choices.empty(); }
};

struct SourceSpeaker {
    std::string key;
    std::string name, color, portrait;
};

struct DialogueSource {
    std::string id;
    std::vector<SourceSpeaker> speakers;
    std::vector<SourceEntry> start;
    std::vector<SourceNode> nodes; // in the file's order

    bool parse(std::string_view json, std::string* error);
    // Laid out for people: a node a few lines, a choice a line.
    std::string json() const;

    SourceNode* node(std::string_view id);
    const SourceNode* node(std::string_view id) const;
    const SourceSpeaker* speaker(std::string_view key) const;
    i32 index_of(std::string_view id) const; // -1 when missing
    // A new id from a stem: "line", "line2"...
    std::string fresh_id(std::string_view stem) const;
};

} // namespace forge::game
