#include "forge/game/dialogue.h"

#include <yyjson.h>

#include <algorithm>
#include <unordered_set>

namespace forge::game {

namespace {

std::string str(yyjson_val* v) { return yyjson_is_str(v) ? std::string(yyjson_get_str(v), yyjson_get_len(v)) : std::string(); }

// "do" may be one string or a list of strings.
std::string actions_source(yyjson_val* v) {
    if (yyjson_is_str(v)) return str(v);
    std::string out;
    if (yyjson_is_arr(v)) {
        yyjson_arr_iter it = yyjson_arr_iter_with(v);
        for (yyjson_val* s; (s = yyjson_arr_iter_next(&it));) {
            out += str(s);
            out += ';';
        }
    }
    return out;
}

bool is_end(std::string_view go_to) { return go_to.empty() || go_to == "end"; }
// "talk:node", a node of another talk.
bool is_other_talk(std::string_view go_to) { return go_to.find(':') != std::string_view::npos; }

class Loader {
public:
    Loader(DialogueReport& report, const std::vector<std::string>& known) : report_(report), known_(known) {}

    Expr condition(yyjson_val* v, const std::string& where) {
        std::string error;
        Expr e = Expr::parse(str(v), &error);
        if (!error.empty()) report_.errors.push_back(where + ": условие «" + str(v) + "»: " + error);
        check_calls(e, where);
        return e;
    }
    Expr actions(yyjson_val* v, const std::string& where) {
        const std::string src = actions_source(v);
        std::string error;
        Expr e = Expr::parse_actions(src, &error);
        if (!error.empty()) report_.errors.push_back(where + ": действие «" + src + "»: " + error);
        check_calls(e, where);
        return e;
    }
    std::vector<DialogueEntry> entries(yyjson_val* v, const std::string& where) {
        std::vector<DialogueEntry> out;
        if (yyjson_is_str(v)) {
            out.push_back({Expr(), str(v)});
            return out;
        }
        yyjson_arr_iter it = yyjson_arr_iter_with(v);
        for (yyjson_val* e; (e = yyjson_arr_iter_next(&it));)
            out.push_back({condition(yyjson_obj_get(e, "if"), where), str(yyjson_obj_get(e, "goto"))});
        return out;
    }

private:
    void check_calls(const Expr& e, const std::string& where) {
        if (known_.empty()) return;
        std::vector<std::string> calls;
        e.collect(&calls, nullptr);
        for (const std::string& c : calls)
            if (std::find(known_.begin(), known_.end(), c) == known_.end())
                report_.warnings.push_back(where + ": неизвестная функция «" + c + "»");
    }

    DialogueReport& report_;
    const std::vector<std::string>& known_;
};

} // namespace

bool Dialogue::load(std::string_view json, DialogueReport& report, const std::vector<std::string>& known_calls) {
    *this = Dialogue();
    yyjson_doc* doc = yyjson_read(json.data(), json.size(), 0);
    yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
    if (!root || !yyjson_is_obj(root)) {
        report.errors.push_back("файл диалога не читается как JSON-объект");
        if (doc) yyjson_doc_free(doc);
        return false;
    }
    Loader load(report, known_calls);
    id_ = str(yyjson_obj_get(root, "id"));

    if (yyjson_val* sp = yyjson_obj_get(root, "speakers"); yyjson_is_obj(sp)) {
        yyjson_obj_iter it = yyjson_obj_iter_with(sp);
        for (yyjson_val* key; (key = yyjson_obj_iter_next(&it));) {
            yyjson_val* v = yyjson_obj_iter_get_val(key);
            Speaker s;
            if (yyjson_is_str(v)) s.name = str(v);
            else {
                s.name = str(yyjson_obj_get(v, "name"));
                s.color = str(yyjson_obj_get(v, "color"));
                s.portrait = str(yyjson_obj_get(v, "portrait"));
            }
            speakers_[str(key)] = std::move(s);
        }
    }

    yyjson_val* nodes = yyjson_obj_get(root, "nodes");
    if (!yyjson_is_arr(nodes)) report.errors.push_back("нет списка nodes");
    yyjson_arr_iter nit = yyjson_arr_iter_with(nodes);
    for (yyjson_val* n; (n = yyjson_arr_iter_next(&nit));) {
        DialogueNode node;
        node.id = str(yyjson_obj_get(n, "id"));
        const std::string where = "узел «" + node.id + "»";
        if (node.id.empty() || node.id == "end") {
            report.errors.push_back("узел без id (или с id «end»)");
            continue;
        }
        if (index_.count(node.id)) {
            report.errors.push_back(where + " встречается дважды");
            continue;
        }
        node.speaker = str(yyjson_obj_get(n, "speaker"));
        node.text = str(yyjson_obj_get(n, "text"));
        node.actions = load.actions(yyjson_obj_get(n, "do"), where);
        node.next = str(yyjson_obj_get(n, "next"));
        node.fallback = str(yyjson_obj_get(n, "fallback"));
        node.call = str(yyjson_obj_get(n, "call"));
        if (yyjson_val* b = yyjson_obj_get(n, "branches")) node.branches = load.entries(b, where);
        yyjson_arr_iter cit = yyjson_arr_iter_with(yyjson_obj_get(n, "choices"));
        for (yyjson_val* c; (c = yyjson_arr_iter_next(&cit));) {
            DialogueChoice choice;
            choice.text = str(yyjson_obj_get(c, "text"));
            choice.condition = load.condition(yyjson_obj_get(c, "if"), where);
            choice.actions = load.actions(yyjson_obj_get(c, "do"), where);
            choice.go_to = str(yyjson_obj_get(c, "goto"));
            node.choices.push_back(std::move(choice));
        }
        yyjson_arr_iter kit = yyjson_arr_iter_with(yyjson_obj_get(n, "keywords"));
        for (yyjson_val* k; (k = yyjson_arr_iter_next(&kit));) {
            DialogueKeyword kw;
            yyjson_arr_iter wit = yyjson_arr_iter_with(yyjson_obj_get(k, "words"));
            for (yyjson_val* w; (w = yyjson_arr_iter_next(&wit));) kw.words.push_back(to_lower_utf8(str(w)));
            kw.condition = load.condition(yyjson_obj_get(k, "if"), where);
            kw.actions = load.actions(yyjson_obj_get(k, "do"), where);
            kw.go_to = str(yyjson_obj_get(k, "goto"));
            if (kw.words.empty()) report.warnings.push_back(where + ": ключевое слово без слов");
            node.keywords.push_back(std::move(kw));
        }
        if (!node.speaker.empty() && !speakers_.count(node.speaker))
            report.warnings.push_back(where + ": неизвестный говорящий «" + node.speaker + "»");
        index_[node.id] = static_cast<u32>(nodes_.size());
        nodes_.push_back(std::move(node));
    }

    if (yyjson_val* start = yyjson_obj_get(root, "start")) entries_ = load.entries(start, "start");
    else if (!nodes_.empty()) entries_.push_back({Expr(), nodes_.front().id});
    yyjson_doc_free(doc);

    // Every jump must land on a node; nodes no jump reaches are reported.
    std::unordered_set<std::string> reached;
    auto target = [&](const std::string& go_to, const std::string& where) {
        if (is_end(go_to) || go_to == "return" || is_other_talk(go_to)) return;
        if (!index_.count(go_to)) report.errors.push_back(where + ": переход на несуществующий узел «" + go_to + "»");
        reached.insert(go_to);
    };
    for (const DialogueEntry& e : entries_) target(e.go_to, "start");
    for (const DialogueNode& n : nodes_) {
        const std::string where = "узел «" + n.id + "»";
        target(n.next, where);
        target(n.fallback, where);
        target(n.call, where);
        for (const DialogueChoice& c : n.choices) target(c.go_to, where);
        for (const DialogueKeyword& k : n.keywords) target(k.go_to, where);
        for (const DialogueEntry& b : n.branches) target(b.go_to, where);
        if (n.text.empty() && !n.choices.empty()) report.warnings.push_back(where + ": выбор без реплики");
    }
    for (const DialogueNode& n : nodes_)
        if (!reached.count(n.id)) report.warnings.push_back("узел «" + n.id + "» недостижим");
    if (entries_.empty()) report.errors.push_back("в диалоге нет ни одного узла");
    return report.ok();
}

const DialogueNode* Dialogue::node(std::string_view id) const {
    auto it = index_.find(std::string(id));
    return it == index_.end() ? nullptr : &nodes_[it->second];
}

const Speaker* Dialogue::speaker(std::string_view key) const {
    auto it = speakers_.find(std::string(key));
    return it == speakers_.end() ? nullptr : &it->second;
}

// --- runner ----------------------------------------------------------------

bool DialogueRunner::start(const Dialogue& dialogue) {
    for (const DialogueEntry& e : dialogue.entries())
        if (e.condition.test(vars_, call_)) return start_at(dialogue, e.go_to);
    stop();
    return false;
}

bool DialogueRunner::start_at(const Dialogue& dialogue, std::string_view node) {
    dialogue_ = &dialogue;
    calls_.clear();
    history_.clear();
    steps_ = 0;
    show(node);
    return active();
}

void DialogueRunner::stop() {
    dialogue_ = nullptr;
    node_ = nullptr;
    calls_.clear();
    line_ = {};
    visible_.clear();
}

std::string_view DialogueRunner::node_id() const { return node_ ? std::string_view(node_->id) : std::string_view(); }

void DialogueRunner::show(std::string_view id) {
    // Junctions follow each other without the player; a loop of them would
    // never return, so it is cut off.
    std::string go_to(id); // id may point into a node that changes below
    for (;;) {
        while (go_to == "return" && !calls_.empty()) {
            dialogue_ = calls_.back().dialogue;
            go_to = std::move(calls_.back().go_to);
            calls_.pop_back();
        }
        if (!dialogue_ || is_end(go_to) || go_to == "return" || ++steps_ > 1000) {
            stop();
            return;
        }
        if (const usize colon = go_to.find(':'); colon != std::string::npos) {
            dialogue_ = resolve_ ? resolve_(std::string_view(go_to).substr(0, colon)) : nullptr;
            go_to.erase(0, colon + 1);
            if (!dialogue_) {
                stop();
                return;
            }
        }
        node_ = dialogue_->node(go_to);
        if (!node_) {
            stop();
            return;
        }
        node_->actions.run(vars_, call_);
        if (!node_->call.empty()) {
            // A talk that goes on in this one comes back here, to next.
            calls_.push_back({dialogue_, node_->next});
            if (calls_.size() > 200) {
                stop();
                return;
            }
            go_to = node_->call;
            continue;
        }
        if (!node_->text.empty() || !node_->choices.empty()) break;
        std::string_view go = node_->next;
        for (const DialogueEntry& b : node_->branches)
            if (b.condition.test(vars_, call_)) {
                go = b.go_to;
                break;
            }
        go_to = std::string(go);
    }
    build_line();
    history_.push_back(line_);
}

void DialogueRunner::build_line() {
    line_ = {};
    visible_.clear();
    if (const Speaker* s = dialogue_->speaker(node_->speaker)) {
        line_.speaker = substitute(s->name, vars_);
        line_.color = s->color;
        line_.portrait = s->portrait;
    }
    line_.text = substitute(node_->text, vars_);
    for (u32 i = 0; i < node_->choices.size(); ++i)
        if (node_->choices[i].condition.test(vars_, call_)) {
            line_.choices.push_back(substitute(node_->choices[i].text, vars_));
            visible_.push_back(i);
        }
    line_.asks_keyword = !node_->keywords.empty();
}

void DialogueRunner::advance() {
    if (!node_ || !line_.choices.empty()) return;
    steps_ = 0;
    show(node_->next);
}

bool DialogueRunner::choose(u32 index) {
    if (!node_ || index >= visible_.size()) return false;
    const DialogueChoice& c = node_->choices[visible_[index]];
    c.actions.run(vars_, call_);
    steps_ = 0;
    const std::string go = c.go_to; // node_ changes in show()
    show(go);
    return true;
}

bool DialogueRunner::ask(std::string_view typed) {
    if (!node_ || node_->keywords.empty()) return false;
    const std::vector<std::string> words = split_words(typed);
    for (const DialogueKeyword& k : node_->keywords) {
        if (!k.condition.test(vars_, call_)) continue;
        const bool hit = std::any_of(k.words.begin(), k.words.end(),
                                     [&](const std::string& kw) { return keyword_matches(words, kw); });
        if (!hit) continue;
        k.actions.run(vars_, call_);
        steps_ = 0;
        const std::string go = k.go_to;
        show(go);
        return true;
    }
    if (!node_->fallback.empty()) {
        steps_ = 0;
        const std::string go = node_->fallback;
        show(go);
    }
    return false;
}

// --- words -----------------------------------------------------------------

namespace {

// Decodes one UTF-8 code point; advances i. Invalid bytes come back as is.
u32 decode(std::string_view s, usize& i) {
    const u8 c = static_cast<u8>(s[i]);
    if (c < 0x80 || i + 1 >= s.size()) {
        ++i;
        return c;
    }
    if ((c & 0xE0) == 0xC0) {
        const u32 cp = ((c & 0x1Fu) << 6) | (static_cast<u8>(s[i + 1]) & 0x3Fu);
        i += 2;
        return cp;
    }
    if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
        const u32 cp = ((c & 0x0Fu) << 12) | ((static_cast<u8>(s[i + 1]) & 0x3Fu) << 6) | (static_cast<u8>(s[i + 2]) & 0x3Fu);
        i += 3;
        return cp;
    }
    ++i;
    return c;
}

void encode(u32 cp, std::string& out) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

u32 lower(u32 cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + 32;
    if (cp >= 0x410 && cp <= 0x42F) return cp + 32;  // А-Я
    if (cp >= 0x400 && cp <= 0x40F) return cp + 80;  // Ѐ-Џ, Ё
    if (cp == 0x451) return 0x435;                    // ё reads as е
    return cp;
}

bool word_char(u32 cp) {
    return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= 0x400 && cp <= 0x4FF);
}

std::u32string code_points(std::string_view s) {
    std::u32string out;
    for (usize i = 0; i < s.size();) out += static_cast<char32_t>(decode(s, i));
    return out;
}

// Letters a Russian (or English) word ending is made of: «шахт|у»,
// «кузнец|ами», «мед|ью».
bool ending_char(char32_t c) {
    static const std::u32string_view endings = U"аеиоуыэюяйьмхвгaeiouy";
    return endings.find(c) != std::u32string_view::npos;
}

// A plain word matches its forms: «шахта» finds «шахту», «шахте», «шахтой»,
// but not «шахтёр».
bool same_word(const std::string& typed, const std::string& keyword) {
    if (typed == keyword) return true;
    const std::u32string w = code_points(typed), k = code_points(keyword);
    if (k.size() < 4) return false;
    usize stem = k.size();
    while (stem > 3 && ending_char(k[stem - 1]) && k[stem - 1] != U'м' && k[stem - 1] != U'х' && k[stem - 1] != U'в' &&
           k[stem - 1] != U'г')
        --stem;
    if (w.size() < stem || w.size() > stem + 3 || w.compare(0, stem, k, 0, stem) != 0) return false;
    for (usize i = stem; i < w.size(); ++i)
        if (!ending_char(w[i])) return false;
    return true;
}

} // namespace

std::string to_lower_utf8(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (usize i = 0; i < text.size();) {
        u32 cp = lower(decode(text, i));
        if (cp == 0x451) cp = 0x435;
        encode(cp, out);
    }
    return out;
}

std::vector<std::string> split_words(std::string_view text) {
    std::vector<std::string> words;
    std::string cur;
    for (usize i = 0; i < text.size();) {
        u32 cp = decode(text, i);
        if (word_char(cp)) {
            cp = lower(cp);
            if (cp == 0x451) cp = 0x435;
            encode(cp, cur);
        } else if (!cur.empty()) {
            words.push_back(std::move(cur));
            cur.clear();
        }
    }
    if (!cur.empty()) words.push_back(std::move(cur));
    return words;
}

bool keyword_matches(const std::vector<std::string>& words, std::string_view keyword) {
    const bool prefix = !keyword.empty() && keyword.back() == '*';
    if (prefix) keyword.remove_suffix(1);
    // A keyword of several words ("старая шахта") must appear in order.
    const std::vector<std::string> parts = split_words(keyword);
    if (parts.empty()) return false;
    for (usize start = 0; start + parts.size() <= words.size(); ++start) {
        bool all = true;
        for (usize j = 0; j < parts.size() && all; ++j) {
            const std::string& w = words[start + j];
            const bool last = j + 1 == parts.size();
            all = (prefix && last) ? w.compare(0, parts[j].size(), parts[j]) == 0 : same_word(w, parts[j]);
        }
        if (all) return true;
    }
    return false;
}

} // namespace forge::game
