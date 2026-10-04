#pragma once

// Quests as data. A quest's progress is one variable (a number), so
// dialogues and scripts move it with the same actions ("quest.pickaxe = 2")
// and the save file needs nothing extra. The journal shows the text of the
// stage the variable has reached.
//
//   [{"id": "pickaxe", "title": "Потерянная кирка", "var": "quest.pickaxe",
//     "stages": [{"at": 1, "text": "Найти кирку шахтёра в старой шахте."},
//                {"at": 2, "text": "Вернуть кирку шахтёру."}],
//     "done_at": 3, "done_text": "Шахтёр получил свою кирку."}]

#include "forge/game/vars.h"

#include <string>
#include <string_view>
#include <vector>

namespace forge::game {

struct QuestStage {
    f64 at = 0;
    std::string text;
};

struct Quest {
    std::string id;
    std::string title;
    std::string var;
    std::vector<QuestStage> stages; // sorted by at
    f64 done_at = 0;                // 0 = the quest never closes
    std::string done_text;
};

enum class QuestState : u8 { NotStarted, Active, Done };

struct JournalEntry {
    const Quest* quest = nullptr;
    QuestState state = QuestState::NotStarted;
    std::string text; // the current stage, with variables put in
};

class QuestBook {
public:
    bool load(std::string_view json, std::vector<std::string>& errors);
    const std::vector<Quest>& quests() const { return quests_; }
    const Quest* find(std::string_view id) const;

    QuestState state(const Quest& quest, const Vars& vars) const;
    // Started quests: active first (in file order), then done ones.
    std::vector<JournalEntry> journal(const Vars& vars) const;

private:
    std::vector<Quest> quests_;
};

} // namespace forge::game
