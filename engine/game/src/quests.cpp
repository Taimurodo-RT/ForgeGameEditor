#include "forge/game/quests.h"

#include <yyjson.h>

#include <algorithm>

namespace forge::game {

namespace {
std::string str(yyjson_val* v) { return yyjson_is_str(v) ? std::string(yyjson_get_str(v), yyjson_get_len(v)) : std::string(); }
} // namespace

bool QuestBook::load(std::string_view json, std::vector<std::string>& errors) {
    quests_.clear();
    yyjson_doc* doc = yyjson_read(json.data(), json.size(), 0);
    yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
    if (!root || !yyjson_is_arr(root)) {
        errors.push_back("файл заданий должен быть JSON-списком");
        if (doc) yyjson_doc_free(doc);
        return false;
    }
    yyjson_arr_iter it = yyjson_arr_iter_with(root);
    for (yyjson_val* q; (q = yyjson_arr_iter_next(&it));) {
        Quest quest;
        quest.id = str(yyjson_obj_get(q, "id"));
        quest.title = str(yyjson_obj_get(q, "title"));
        quest.var = str(yyjson_obj_get(q, "var"));
        if (quest.var.empty()) quest.var = "quest." + quest.id;
        quest.done_at = yyjson_get_num(yyjson_obj_get(q, "done_at"));
        quest.done_text = str(yyjson_obj_get(q, "done_text"));
        yyjson_arr_iter sit = yyjson_arr_iter_with(yyjson_obj_get(q, "stages"));
        for (yyjson_val* s; (s = yyjson_arr_iter_next(&sit));)
            quest.stages.push_back({yyjson_get_num(yyjson_obj_get(s, "at")), str(yyjson_obj_get(s, "text"))});
        std::sort(quest.stages.begin(), quest.stages.end(), [](const QuestStage& a, const QuestStage& b) { return a.at < b.at; });
        if (quest.id.empty()) errors.push_back("задание без id");
        else if (find(quest.id)) errors.push_back("задание «" + quest.id + "» встречается дважды");
        else if (quest.stages.empty()) errors.push_back("у задания «" + quest.id + "» нет этапов");
        else quests_.push_back(std::move(quest));
    }
    yyjson_doc_free(doc);
    return errors.empty();
}

const Quest* QuestBook::find(std::string_view id) const {
    for (const Quest& q : quests_)
        if (q.id == id) return &q;
    return nullptr;
}

QuestState QuestBook::state(const Quest& quest, const Vars& vars) const {
    const f64 v = vars.get(quest.var).number();
    if (quest.done_at > 0 && v >= quest.done_at) return QuestState::Done;
    return v >= quest.stages.front().at ? QuestState::Active : QuestState::NotStarted;
}

std::vector<JournalEntry> QuestBook::journal(const Vars& vars) const {
    std::vector<JournalEntry> active, done;
    for (const Quest& q : quests_) {
        const QuestState st = state(q, vars);
        if (st == QuestState::NotStarted) continue;
        JournalEntry e{&q, st, {}};
        if (st == QuestState::Done) {
            e.text = substitute(q.done_text, vars);
            done.push_back(std::move(e));
            continue;
        }
        const f64 v = vars.get(q.var).number();
        for (const QuestStage& s : q.stages)
            if (v >= s.at) e.text = substitute(s.text, vars);
        active.push_back(std::move(e));
    }
    for (JournalEntry& e : done) active.push_back(std::move(e));
    return active;
}

} // namespace forge::game
