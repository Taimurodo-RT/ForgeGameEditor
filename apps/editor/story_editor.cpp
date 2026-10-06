#include "story_editor.h"

#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <regex>
#include <set>

namespace forge::editor_app {

namespace {

int arg_int(const Rml::VariantList& a, usize i, int fallback) { return i < a.size() ? a[i].Get<int>() : fallback; }
std::string arg_str(const Rml::VariantList& a, usize i) { return i < a.size() ? a[i].Get<Rml::String>() : std::string(); }

constexpr u64 kFlashMs = 1200;
constexpr usize kPage = 150; // lines of a big talk shown at once
constexpr const char* kNarrator = "#8a96a0";
constexpr const char* kNew = "\x01new";
constexpr const char* kRaw = "\x01raw";
constexpr const char* kSpeakerNew = "\x01speaker";

std::string trim(std::string_view s) {
    usize a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) --b;
    return std::string(s.substr(a, b - a));
}

std::vector<std::string> split(std::string_view s, char by) {
    std::vector<std::string> out;
    usize at = 0;
    while (at <= s.size()) {
        const usize next = s.find(by, at);
        const std::string part = trim(s.substr(at, next == std::string_view::npos ? std::string_view::npos : next - at));
        if (!part.empty()) out.push_back(part);
        if (next == std::string_view::npos) break;
        at = next + 1;
    }
    return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view by) {
    std::string out;
    for (usize i = 0; i < parts.size(); ++i) {
        if (i) out += by;
        out += parts[i];
    }
    return out;
}

std::string num(f64 v) {
    char buf[32];
    if (v == std::floor(v)) std::snprintf(buf, sizeof buf, "%.0f", v);
    else std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

// The first letter of a name (UTF-8).
std::string first_letter(std::string_view s) {
    if (s.empty()) return "?";
    const u8 c = static_cast<u8>(s[0]);
    const usize n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
    return std::string(s.substr(0, std::min(n, s.size())));
}

// About as many characters as fit a line of the script.
int rows_for(std::string_view text, int per_row) {
    int chars = 0;
    for (const char c : text)
        if ((static_cast<u8>(c) & 0xC0) != 0x80) ++chars;
    return std::clamp(chars / per_row + 1, 1, 12);
}

// Words as the author typed them: «кирк*» shows as «кирк…».
std::string words_shown(const std::vector<std::string>& words) {
    std::vector<std::string> out;
    for (std::string w : words) {
        if (!w.empty() && w.back() == '*') w = w.substr(0, w.size() - 1) + "…";
        out.push_back(w);
    }
    return join(out, ", ");
}

std::vector<std::string> words_typed(std::string_view text) {
    std::vector<std::string> out;
    for (std::string w : split(text, ',')) {
        if (w.size() >= 3 && w.compare(w.size() - 3, 3, "…") == 0) w = w.substr(0, w.size() - 3) + "*";
        w = game::to_lower_utf8(w);
        if (!w.empty()) out.push_back(w);
    }
    return out;
}

const std::regex& has_re() {
    static const std::regex re(R"re(^\s*(not\s+)?has\(\s*"([^"]+)"\s*(?:,\s*([0-9.]+)\s*)?\)\s*$)re");
    return re;
}
const std::regex& compare_re() {
    static const std::regex re(R"re(^\s*([A-Za-z_][\w.]*)\s*(>=|<=|==|!=|>|<)\s*([0-9.]+)\s*$)re");
    return re;
}
const std::regex& set_re() {
    static const std::regex re(R"re(^\s*([A-Za-z_][\w.]*)\s*(=|\+=|-=)\s*([0-9.]+)\s*$)re");
    return re;
}
const std::regex& give_re() {
    static const std::regex re(R"re(^\s*(give|take)\(\s*"([^"]+)"\s*(?:,\s*([0-9.]+)\s*)?\)\s*$)re");
    return re;
}

// Splits at sep outside quotes and brackets: "a; renpy(\"x; y\")" is two
// actions, "a and (b and c)" two parts.
std::vector<std::string> split_outside(std::string_view s, std::string_view sep) {
    std::vector<std::string> out;
    int depth = 0;
    char quote = 0;
    usize from = 0;
    for (usize i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (quote) {
            if (c == '\\') ++i;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') quote = c;
        else if (c == '(') ++depth;
        else if (c == ')') --depth;
        else if (depth == 0 && s.compare(i, sep.size(), sep) == 0) {
            if (std::string part = trim(s.substr(from, i - from)); !part.empty()) out.push_back(part);
            from = i + sep.size();
            i = from - 1;
        }
    }
    if (std::string part = trim(s.substr(std::min(from, s.size()))); !part.empty()) out.push_back(part);
    return out;
}

// Parts of a condition joined with "and".
std::vector<std::string> cond_parts(const std::string& cond) { return split_outside(cond, " and "); }
// The statements of an action.
std::vector<std::string> actions_of(const std::string& act) { return split_outside(act, ";"); }

// Ren'Py code kept as it was: renpy("..."), the code unescaped.
bool renpy_code(const std::string& source, std::string& code) {
    static const std::regex re(R"re(^\s*renpy\("((?:[^"\\]|\\.)*)"\)\s*$)re");
    std::smatch m;
    if (!std::regex_match(source, m, re)) return false;
    code.clear();
    const std::string body = m[1];
    for (usize i = 0; i < body.size(); ++i) {
        if (body[i] == '\\' && i + 1 < body.size()) ++i;
        code += body[i];
    }
    return true;
}

// The first line of a piece of code, short enough for a mark.
std::string code_line(const std::string& code) {
    std::string line = code.substr(0, code.find('\n'));
    usize chars = 0;
    for (usize i = 0; i < line.size(); ++i)
        if ((static_cast<u8>(line[i]) & 0xC0) != 0x80 && ++chars > 60) return line.substr(0, i) + "…";
    return code.find('\n') != std::string::npos ? line + " …" : line;
}

} // namespace

// One step of the history: a conversation's file before and after.
class StoryCommand final : public editor::Command {
public:
    StoryCommand(StoryEditor& ed, std::string talk, std::string before, std::string after, std::string label)
        : ed_(ed), talk_(std::move(talk)), before_(std::move(before)), after_(std::move(after)), label_(std::move(label)) {}
    void apply(editor::Document&) override { ed_.apply(talk_, after_); }
    void revert(editor::Document&) override { ed_.apply(talk_, before_); }
    std::string label() const override { return label_; }

private:
    StoryEditor& ed_;
    std::string talk_, before_, after_, label_;
};

// --- files ------------------------------------------------------------------

bool StoryEditor::init(ui::Ui& ui, const std::filesystem::path& game_dir) {
    ui_ = &ui;
    game_dir_ = game_dir;
    runner_.set_resolver([this](std::string_view talk) { return resolve(talk); });
    load_quests();
    const std::vector<std::string> all = talks();
    if (!all.empty()) {
        talk_ = all.front();
        load_talk();
        play();
    }
    return true;
}

void StoryEditor::load_quests() {
    std::vector<u8> bytes;
    if (!read_file(game_dir_ / "quests.json", bytes)) return;
    std::vector<std::string> errors;
    if (!quests_.load(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), errors))
        for (const std::string& e : errors) FORGE_ERROR("Задания: %s", e.c_str());
}

std::filesystem::path StoryEditor::talk_path(const std::string& talk) const { return game_dir_ / "dialogues" / utf8_path(talk + ".json"); }

std::vector<std::string> StoryEditor::talks() const {
    // Lone talks first, then each story folder's talks.
    std::vector<std::string> out, stories;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(game_dir_ / "dialogues", ec)) {
        if (e.is_directory(ec)) {
            std::error_code ec2;
            for (const auto& f : std::filesystem::directory_iterator(e.path(), ec2))
                if (f.is_regular_file(ec2) && f.path().extension() == ".json")
                    stories.push_back(path_to_utf8(e.path().filename()) + "/" + path_to_utf8(f.path().stem()));
        } else if (e.is_regular_file(ec) && e.path().extension() == ".json") {
            out.push_back(path_to_utf8(e.path().stem()));
        }
    }
    std::sort(out.begin(), out.end());
    std::sort(stories.begin(), stories.end());
    out.insert(out.end(), stories.begin(), stories.end());
    return out;
}

std::string StoryEditor::story_of(const std::string& talk) const {
    const usize slash = talk.find('/');
    return slash == std::string::npos ? std::string() : talk.substr(0, slash);
}

std::string StoryEditor::full_id(const std::string& ref) const {
    const std::string story = story_of(talk_);
    return story.empty() ? ref : story + "/" + ref;
}

game::Dialogue& StoryEditor::open_dialogue() {
    std::unique_ptr<game::Dialogue>& d = loaded_[talk_];
    if (!d) d = std::make_unique<game::Dialogue>();
    return *d;
}

const game::Dialogue* StoryEditor::resolve(std::string_view talk) {
    const std::string id = full_id(std::string(talk));
    if (auto it = loaded_.find(id); it != loaded_.end() && it->second) return it->second.get();
    std::vector<u8> bytes;
    if (!read_file(talk_path(id), bytes)) return nullptr;
    auto d = std::make_unique<game::Dialogue>();
    game::DialogueReport report;
    if (!d->load(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), report)) {
        for (const std::string& e : report.errors) FORGE_ERROR("Разговор %s: %s", id.c_str(), e.c_str());
        return nullptr;
    }
    return (loaded_[id] = std::move(d)).get();
}

void StoryEditor::follow() {
    const game::Dialogue* d = runner_.dialogue();
    if (!d) return;
    if (d != &open_dialogue())
        for (const auto& [id, p] : loaded_) {
            if (p.get() != d) continue;
            // Shown without starting over: the test goes on in it.
            talk_ = id;
            src_ = {};
            std::vector<u8> bytes;
            std::string error;
            if (read_file(talk_path(talk_), bytes)) src_.parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), &error);
            lit_rule_ = -1;
            page_from_ = 0;
            dirty_ = true;
            break;
        }
    page_to_ = std::string(runner_.node_id());
}

void StoryEditor::load_talk() {
    src_ = {};
    std::vector<u8> bytes;
    if (read_file(talk_path(talk_), bytes)) {
        std::string error;
        if (!src_.parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), &error))
            FORGE_ERROR("Разговор %s: %s", talk_.c_str(), error.c_str());
    }
    runner_.stop();
    reload_play(false);
    dirty_ = true;
}

bool StoryEditor::open(const std::string& talk) {
    const std::vector<std::string> all = talks();
    if (std::find(all.begin(), all.end(), talk) == all.end()) return false;
    commit_edit();
    close_menu();
    if (talk != talk_) {
        talk_ = talk;
        m_log_.clear();
        page_from_ = 0;
        if (const std::string story = story_of(talk); !story.empty()) stories_open_.insert(story);
        load_talk();
    }
    play();
    return true;
}

bool StoryEditor::write(const std::string& talk, const std::string& json) const {
    std::error_code ec;
    std::filesystem::create_directories(talk_path(talk).parent_path(), ec);
    if (write_file_atomic(talk_path(talk), {reinterpret_cast<const u8*>(json.data()), json.size()})) return true;
    FORGE_ERROR("Не удалось сохранить разговор %s", talk.c_str());
    return false;
}

void StoryEditor::apply(const std::string& talk, const std::string& json) {
    write(talk, json);
    if (talk != talk_) {
        talk_ = talk;
        load_talk();
        return;
    }
    std::string error;
    src_.parse(json, &error);
    reload_play(true);
    dirty_ = true;
}

void StoryEditor::change(const game::DialogueSource& after, std::string label) {
    const std::string before = src_.json(), now = after.json();
    if (before == now) return;
    history_.execute(std::make_unique<StoryCommand>(*this, talk_, before, now, std::move(label)));
}

std::string StoryEditor::new_talk() {
    commit_edit();
    std::string id = "talk";
    const std::vector<std::string> all = talks();
    for (u32 n = 2; std::find(all.begin(), all.end(), id) != all.end(); ++n) id = "talk" + std::to_string(n);
    game::DialogueSource s;
    s.id = id;
    s.speakers.push_back({"npc", "Новый житель", "#7ec8a0", ""});
    s.start.push_back({{}, "hello"});
    game::SourceNode hello;
    hello.id = "hello";
    hello.scene = "Первая встреча";
    hello.speaker = "npc";
    s.nodes.push_back(hello);
    history_.execute(std::make_unique<StoryCommand>(*this, id, std::string(), s.json(), "Новый разговор"));
    talk_ = id;
    m_log_.clear();
    load_talk();
    play();
    begin_edit("text", "hello");
    return id;
}

void StoryEditor::undo() {
    cancel_edit();
    close_menu();
    if (history_.undo()) FORGE_INFO("Отменено");
}

void StoryEditor::redo() {
    cancel_edit();
    close_menu();
    if (history_.redo()) FORGE_INFO("Повторено");
}

std::string StoryEditor::status() const {
    usize lines = 0;
    for (const game::SourceNode& n : src_.nodes)
        if (!n.junction()) ++lines;
    const std::string story = story_of(talk_);
    const std::string name = !story.empty() ? talk_.substr(story.size() + 1) : src_.speakers.empty() ? talk_ : src_.speakers.front().name;
    return "Разговор: " + name + ", реплик: " + std::to_string(lines) + " · dialogues/" + talk_ + ".json";
}

// --- phrases ----------------------------------------------------------------

std::string StoryEditor::item_name(const std::string& id) const {
    if (library_)
        if (const objects::Template* t = library_->find(std::string_view(id))) return t->name;
    return id;
}

const game::Quest* StoryEditor::quest_of_var(std::string_view var) const {
    for (const game::Quest& q : quests_.quests())
        if (q.var == var) return &q;
    return nullptr;
}

std::string StoryEditor::stage_name(const game::Quest& q, f64 at) const {
    if (at <= 0) return "не взято";
    if (q.done_at > 0 && at >= q.done_at) return q.done_name.empty() ? "выполнено" : q.done_name;
    for (const game::QuestStage& s : q.stages)
        if (s.at == at) return s.name.empty() ? "этап " + num(at) : s.name;
    return "этап " + num(at);
}

std::vector<std::string> StoryEditor::phrases(const std::string& source, bool action) const {
    std::vector<std::string> out;
    std::smatch m;
    std::string code;
    if (!action) {
        for (std::string part : cond_parts(source)) {
            const bool negated = part.rfind("not ", 0) == 0;
            if (renpy_code(negated ? part.substr(4) : part, code)) {
                out.push_back(std::string(negated ? "не " : "") + "Ren'Py: " + code_line(code));
            } else if (std::regex_match(part, m, has_re())) {
                const std::string name = item_name(m[2]);
                const std::string count = m[3].matched && m[3].str() != "1" ? ": " + m[3].str() : std::string();
                out.push_back(m[1].matched ? "у героя нет: " + name : "у героя есть " + name + count);
            } else if (std::regex_match(part, m, compare_re())) {
                const f64 v = std::atof(m[3].str().c_str());
                const std::string op = m[2];
                if (const game::Quest* q = quest_of_var(m[1].str())) {
                    const std::string t = "задание «" + q->title + "»: ";
                    const std::string s = stage_name(*q, v);
                    out.push_back(op == ">="   ? t + s + (q->done_at > 0 && v >= q->done_at ? std::string() : std::string(" или дальше"))
                                  : op == "==" ? t + s
                                  : op == "!=" ? t + "не " + s
                                  : op == "<"  ? t + "ещё не " + s
                                  : op == ">"  ? t + "дальше, чем " + s
                                               : t + s + " или раньше");
                } else {
                    out.push_back(m[1].str() + " " + (op == ">=" ? "≥" : op == "<=" ? "≤" : op == "!=" ? "≠" : op == "==" ? "=" : op) + " " +
                                  m[3].str());
                }
            } else {
                out.push_back(part);
            }
        }
        return out;
    }
    for (const std::string& st : actions_of(source)) {
        if (renpy_code(st, code)) {
            out.push_back("Ren'Py: " + code_line(code));
        } else if (std::regex_match(st, m, give_re())) {
            const std::string n = m[3].matched ? m[3].str() : "1";
            out.push_back(m[1] == "give" ? "герою: " + item_name(m[2]) + " +" + n : "у героя: " + item_name(m[2]) + " −" + n);
        } else if (std::regex_match(st, m, set_re())) {
            const game::Quest* q = m[2] == "=" ? quest_of_var(m[1].str()) : nullptr;
            if (q) out.push_back("задание «" + q->title + "» → " + stage_name(*q, std::atof(m[3].str().c_str())));
            else out.push_back(m[1].str() + (m[2] == "=" ? " = " : m[2] == "+=" ? " +" : " −") + m[3].str());
        } else {
            out.push_back(st);
        }
    }
    return out;
}

std::string StoryEditor::anchor(const std::string& node) const {
    if (node.empty() || node == "end") return "конец разговора";
    if (node == "return") return "назад, откуда позвали";
    if (const usize colon = node.find(':'); colon != std::string::npos)
        return "«" + node.substr(colon + 1) + "» в разговоре " + node.substr(0, colon);
    const game::SourceNode* n = src_.node(node);
    if (!n) return "«" + node + "» (нет такой реплики)";
    if (n->text.empty()) return !n->scene.empty() ? "«" + n->scene + "»" : n->junction() ? "выбор «" + node + "»" : "«" + node + "»";
    // The line's first phrase.
    std::string t = n->text;
    for (const char* stop : {".", "!", "?", "…"})
        if (const usize at = t.find(stop); at != std::string::npos && at > 0) t = t.substr(0, at);
    usize chars = 0, cut = t.size();
    for (usize i = 0; i < t.size(); ++i)
        if ((static_cast<u8>(t[i]) & 0xC0) != 0x80 && ++chars > 32) {
            cut = i;
            break;
        }
    return "«" + t.substr(0, cut) + "…»";
}

std::vector<StoryEditor::Chip> StoryEditor::cond_chips(const std::string& cond, bool otherwise) const {
    std::vector<Chip> out;
    if (trim(cond).empty()) {
        if (otherwise) out.push_back({"else", "more_horiz", "иначе", "if"});
        return out;
    }
    std::vector<std::string> p = phrases(cond, false);
    const bool code = std::any_of(p.begin(), p.end(), [](const std::string& x) { return x.find("Ren'Py: ") != std::string::npos; });
    out.push_back({code ? "cond code" : "cond", "help", "если " + join(p, " и "), "if"});
    return out;
}

std::vector<StoryEditor::Chip> StoryEditor::act_chips(const std::string& act) const {
    std::vector<Chip> out;
    const std::vector<std::string> p = phrases(act, true);
    for (usize i = 0; i < p.size(); ++i) {
        const bool code = p[i].rfind("Ren'Py: ", 0) == 0;
        out.push_back({code ? "eff code" : "eff", code ? "code" : "bolt", p[i], "do", static_cast<int>(i)});
    }
    return out;
}

StoryEditor::Chip StoryEditor::go_chip(const std::string& go, bool next) const {
    const bool end = go.empty() || go == "end";
    const char* icon = end ? "stop" : go == "return" ? "undo" : next ? "south" : "east";
    return {"go", icon, end ? "конец разговора" : (next && go != "return" ? "дальше: " : "") + anchor(go), next ? "next" : "goto"};
}

// What happens on screen, said in words: «фон: club_day», «на сцене: monika 5».
std::string StoryEditor::stage_phrase(const std::string& stage) const {
    std::vector<std::string> w;
    {
        std::string cur;
        char quote = 0;
        for (const char c : stage) {
            if (quote) {
                if (c == quote) quote = 0;
                else cur += c;
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == ' ') {
                if (!cur.empty()) w.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) w.push_back(cur);
    }
    if (w.empty()) return stage;
    // The words up to a clause (at, with, zorder, as, behind, onlayer, fadein...).
    auto words_until_clause = [&](usize from) {
        static const std::set<std::string> clauses{"at", "with", "zorder", "as", "behind", "onlayer", "fadein", "fadeout", "loop", "noloop", "if_changed", "{"};
        std::vector<std::string> out;
        for (usize i = from; i < w.size() && !clauses.contains(w[i]); ++i) out.push_back(w[i]);
        return join(out, " ");
    };
    auto clause = [&](const char* name) {
        for (usize i = 0; i + 1 < w.size(); ++i)
            if (w[i] == name) return w[i + 1];
        return std::string();
    };
    const std::string& head = w[0];
    std::string out;
    if (head == "scene") {
        std::string bg = words_until_clause(1);
        if (bg.rfind("bg ", 0) == 0) bg = bg.substr(3);
        out = bg.empty() ? "сцена очищена" : "фон: " + bg;
    } else if (head == "show") {
        out = (w.size() > 1 && w[1] == "screen" ? "экран: " : "на сцене: ") + words_until_clause(w.size() > 1 && w[1] == "screen" ? 2 : 1);
        if (const std::string at = clause("at"); !at.empty()) out += " (" + at + ")";
    } else if (head == "hide") {
        out = "уходит: " + words_until_clause(w.size() > 1 && w[1] == "screen" ? 2 : 1);
    } else if (head == "with") {
        return "переход: " + (w.size() > 1 ? w[1] : std::string());
    } else if (head == "play" || head == "queue") {
        const std::string channel = w.size() > 1 ? w[1] : "";
        std::string what = w.size() > 2 ? w[2] : "";
        if (const usize slash = what.find_last_of('/'); slash != std::string::npos) what = what.substr(slash + 1);
        out = (channel == "music" ? "музыка: " : channel == "sound" ? "звук: " : channel + ": ") + what;
    } else if (head == "stop") {
        const std::string channel = w.size() > 1 ? w[1] : "";
        out = channel == "music" ? "музыка стихает" : channel == "sound" ? "звук стихает" : channel + " стихает";
    } else if (head == "voice") {
        out = "голос: " + (w.size() > 1 ? w[1] : std::string());
    } else if (head == "pause") {
        out = w.size() > 1 ? "пауза " + w[1] + " с" : "пауза до щелчка";
    } else if (head == "window") {
        out = w.size() > 1 && w[1] == "hide" ? "окно текста скрыто" : w.size() > 1 && w[1] == "show" ? "окно текста видно" : "окно текста: " + (w.size() > 1 ? w[1] : "");
    } else if (head == "$") {
        return "Ren'Py: " + code_line(trim(stage.substr(1)));
    } else {
        return code_line(stage);
    }
    if (const std::string with = clause("with"); !with.empty() && head != "with") out += ", переход " + with;
    return out;
}

std::vector<StoryEditor::Chip> StoryEditor::stage_chips(const std::vector<std::string>& stage) const {
    std::vector<Chip> out;
    for (usize i = 0; i < stage.size(); ++i) {
        const std::string& head = stage[i].substr(0, stage[i].find(' '));
        const char* icon = head == "scene"                     ? "landscape"
                           : head == "show" || head == "hide"  ? "person"
                           : head == "play" || head == "stop" || head == "queue" ? "music_note"
                           : head == "voice"                   ? "record_voice_over"
                           : head == "pause"                   ? "hourglass_empty"
                           : head == "with"                    ? "blur_on"
                           : head == "$"                       ? "code"
                                                               : "movie";
        out.push_back({head == "$" ? "stage code" : "stage", icon, stage_phrase(stage[i]), "stage", static_cast<int>(i)});
    }
    return out;
}

bool StoryEditor::inline_answer(const std::string& node, const std::string& asker) const {
    const game::SourceNode* n = src_.node(node);
    if (!n || n->text.empty() || !n->choices.empty() || !n->keywords.empty() || !n->branches.empty()) return false;
    if (!n->next.empty() && n->next != asker && n->next != "end") return false;
    // Reached only from the asker's topics.
    for (const game::SourceEntry& e : src_.start)
        if (e.go == node) return false;
    for (const game::SourceNode& o : src_.nodes) {
        if (o.next == node) return false;
        for (const game::SourceEntry& b : o.branches)
            if (b.go == node) return false;
        for (const game::SourceChoice& c : o.choices)
            if (c.go == node) return false;
        if (o.id != asker) {
            if (o.fallback == node) return false;
            for (const game::SourceKeyword& k : o.keywords)
                if (k.go == node) return false;
        }
    }
    return true;
}

std::vector<std::string> StoryEditor::items_used() const {
    std::vector<std::string> out;
    auto scan = [&out](const std::string& s) {
        static const std::regex re(R"re((?:has|give|take|count)\(\s*"([^"]+)")re");
        for (std::sregex_iterator it(s.begin(), s.end(), re), end; it != end; ++it)
            if (std::find(out.begin(), out.end(), (*it)[1].str()) == out.end()) out.push_back((*it)[1].str());
    };
    for (const game::SourceEntry& e : src_.start) scan(e.cond);
    for (const game::SourceNode& n : src_.nodes) {
        scan(n.act);
        for (const game::SourceEntry& b : n.branches) scan(b.cond);
        for (const game::SourceChoice& c : n.choices) {
            scan(c.cond);
            scan(c.act);
        }
        for (const game::SourceKeyword& k : n.keywords) {
            scan(k.cond);
            scan(k.act);
        }
    }
    return out;
}

// --- the script -------------------------------------------------------------

void StoryEditor::rebuild() {
    dirty_ = false;
    rebuild_talks();

    m_speakers_.clear();
    for (const game::SourceSpeaker& s : src_.speakers)
        m_speakers_.push_back({s.key, s.name, first_letter(s.name), s.color.empty() ? kNarrator : s.color,
                               edit_what_ == "speaker" && edit_node_ == s.key});

    const bool on = runner_.active();
    const std::string at(runner_.node_id());
    m_rules_.clear();
    for (usize i = 0; i < src_.start.size(); ++i) {
        const game::SourceEntry& e = src_.start[i];
        RuleView r;
        r.index = static_cast<int>(i);
        r.lit = on && lit_rule_ == static_cast<int>(i);
        r.chips = cond_chips(e.cond, true);
        if (r.chips.empty()) r.chips.push_back({"else", "check", "всегда", "if"});
        r.chips.push_back(go_chip(e.go, false));
        m_rules_.push_back(std::move(r));
    }

    std::set<std::string> hidden;
    for (const game::SourceNode& n : src_.nodes) {
        for (const game::SourceKeyword& k : n.keywords)
            if (inline_answer(k.go, n.id)) hidden.insert(k.go);
        if (!n.fallback.empty() && inline_answer(n.fallback, n.id)) hidden.insert(n.fallback);
    }
    auto editing = [this](const char* what, const std::string& node, int index = -1) {
        return edit_what_ == what && edit_node_ == node && edit_index_ == index;
    };
    std::vector<const game::SourceNode*> shown;
    shown.reserve(src_.nodes.size());
    for (const game::SourceNode& n : src_.nodes)
        if (!hidden.contains(n.id)) shown.push_back(&n);
    // A big talk shows a page of lines; a line asked for comes onto it.
    const bool paged = shown.size() > kPage;
    if (!page_to_.empty()) {
        std::string at = page_to_;
        for (const game::SourceNode& n : src_.nodes)
            for (const game::SourceKeyword& k : n.keywords)
                if (k.go == page_to_ && hidden.contains(page_to_)) at = n.id;
        for (usize i = 0; i < shown.size(); ++i)
            if (shown[i]->id == at && (i < page_from_ || i >= page_from_ + kPage)) page_from_ = i > 10 ? i - 10 : 0;
        page_to_.clear();
    }
    if (!paged || page_from_ >= shown.size()) page_from_ = 0;
    const usize first = page_from_, last = paged ? std::min(shown.size(), first + kPage) : shown.size();
    set(m_page_on_, paged, "st_page_on");
    set(m_page_prev_, paged && first > 0, "st_page_prev");
    set(m_page_next_, paged && last < shown.size(), "st_page_next");
    set(m_page_text_, Rml::String(paged ? "Реплики " + std::to_string(first + 1) + "–" + std::to_string(last) + " из " + std::to_string(shown.size()) : ""),
        "st_page_text");

    m_lines_.clear();
    for (usize k = first; k < last; ++k) {
        const game::SourceNode& n = *shown[k];
        const std::string below = k + 1 < shown.size() ? shown[k + 1]->id : std::string();
        LineView v;
        v.id = n.id;
        const bool silent = n.silent() && n.branches.empty();
        v.kind = n.junction() ? "junction" : silent ? "silent" : "line";
        v.scene = n.scene;
        v.note = n.note;
        v.pose = n.pose;
        const game::SourceSpeaker* sp = src_.speaker(n.speaker);
        v.who = n.junction() ? "Выбор" : silent ? "" : sp ? sp->name : "Рассказчик";
        v.color = sp && !sp->color.empty() ? sp->color : kNarrator;
        v.text = n.text;
        v.stage = stage_chips(n.stage);
        v.lit = on && at == n.id;
        v.flash = flash_ == n.id;
        v.editing = editing("text", n.id);
        v.editing_note = editing("note", n.id);
        v.editing_scene = editing("scene", n.id);
        for (usize i = 0; i < n.stage.size(); ++i)
            if (editing("stage", n.id, static_cast<int>(i))) v.editing_stage = static_cast<int>(i);
        if (n.junction()) {
            for (usize b = 0; b < n.branches.size(); ++b) {
                for (Chip c : cond_chips(n.branches[b].cond, false)) {
                    c.mark = static_cast<int>(b);
                    v.chips.push_back(c);
                }
                Chip g = go_chip(n.branches[b].go, false);
                g.mark = static_cast<int>(b);
                v.chips.push_back(g);
            }
            v.chips.push_back({"else", "more_horiz", "иначе", "else"});
            v.chips.push_back(go_chip(n.next, false));
            v.chips.back().part = "next";
        } else {
            v.chips = act_chips(n.act);
            if (!n.call.empty()) {
                Chip c = go_chip(n.call, false);
                c.part = "call";
                c.icon = "subdirectory_arrow_right";
                c.text = "сначала: " + anchor(n.call);
                v.chips.push_back(c);
            }
            if (n.choices.empty() && n.keywords.empty()) {
                Chip c = go_chip(n.next, true);
                if (!n.call.empty() && n.next != "return" && !n.next.empty()) c.text = "потом: " + anchor(n.next);
                // Going on to the line just below needs no mark: it shows on hover.
                c.seq = !below.empty() && n.next == below;
                v.chips.push_back(c);
            }
        }
        v.choices_on = !n.choices.empty();
        for (usize i = 0; i < n.choices.size(); ++i) {
            const game::SourceChoice& c = n.choices[i];
            ChoiceView cv;
            cv.index = static_cast<int>(i);
            cv.text = c.text;
            cv.lit = lit_choice_node_ == n.id && lit_choice_ == static_cast<int>(i);
            cv.editing = editing("choice", n.id, static_cast<int>(i));
            cv.chips = cond_chips(c.cond, false);
            for (const Chip& e : act_chips(c.act)) cv.chips.push_back(e);
            cv.chips.push_back(go_chip(c.go, false));
            v.choices.push_back(std::move(cv));
        }
        v.topics_on = !n.keywords.empty();
        for (usize i = 0; i < n.keywords.size(); ++i) {
            const game::SourceKeyword& k = n.keywords[i];
            TopicView tv;
            tv.index = static_cast<int>(i);
            tv.words = words_shown(k.words);
            if (hidden.contains(k.go))
                if (const game::SourceNode* a = src_.node(k.go)) {
                    tv.answer = a->id;
                    tv.text = a->text;
                }
            tv.lit = lit_topic_node_ == n.id && lit_topic_ == static_cast<int>(i);
            tv.editing_words = editing("words", n.id, static_cast<int>(i));
            tv.editing_text = editing("topic", n.id, static_cast<int>(i));
            tv.chips = cond_chips(k.cond, false);
            for (const Chip& e : act_chips(k.act)) tv.chips.push_back(e);
            if (tv.answer.empty()) tv.chips.push_back(go_chip(k.go, false));
            v.topics.push_back(std::move(tv));
        }
        v.has_fallback = !n.fallback.empty();
        if (v.has_fallback) {
            if (hidden.contains(n.fallback)) {
                if (const game::SourceNode* f = src_.node(n.fallback)) v.fallback = f->text;
            } else {
                v.fallback = "→ " + anchor(n.fallback);
            }
            v.editing_fallback = editing("fallback", n.id);
        }
        v.can_add = !n.junction() && n.choices.empty() && n.keywords.empty();
        for (Chip& c : v.chips) c.look = c.kind + (c.seq ? " seq" : "");
        m_lines_.push_back(std::move(v));
    }

    const std::string story = story_of(talk_);
    m_title_ = !story.empty() ? story + " · " + talk_.substr(story.size() + 1) : src_.speakers.empty() ? talk_ : "Разговор: " + src_.speakers.front().name;
    m_count_ = status();
    rebuild_state();
    rebuild_play();
    if (model_)
        for (const char* name : {"st_talks", "st_speakers", "st_rules", "st_lines", "st_title", "st_count", "st_editing", "st_edit_rows"})
            model_.DirtyVariable(name);
}

void StoryEditor::rebuild_talks() {
    m_talks_.clear();
    std::string story_shown;
    usize header = 0; // the row of the story being listed
    for (const std::string& t : talks()) {
        const std::string story = story_of(t);
        if (!story.empty() && story != story_shown) {
            story_shown = story;
            TalkRow g;
            g.kind = "story";
            g.id = story;
            g.name = story;
            g.letter = first_letter(story);
            g.color = kNarrator;
            g.open = stories_open_.contains(story);
            header = m_talks_.size();
            m_talks_.push_back(std::move(g));
        }
        if (!story.empty()) {
            ++m_talks_[header].lines;
            if (!m_talks_[header].open) continue;
        }
        // A talk's name and size, read again only when its file changed.
        TalkInfo& info = talk_info_[t];
        std::error_code ec;
        const auto time = std::filesystem::last_write_time(talk_path(t), ec);
        if (info.name.empty() || info.time != time || t == talk_) {
            game::DialogueSource s;
            if (t == talk_) {
                s = src_;
            } else {
                std::vector<u8> bytes;
                std::string error;
                if (read_file(talk_path(t), bytes)) s.parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), &error);
            }
            info.time = time;
            info.name = !story.empty() ? t.substr(story.size() + 1) : s.speakers.empty() ? t : s.speakers.front().name;
            info.color = story.empty() && !s.speakers.empty() && !s.speakers.front().color.empty() ? s.speakers.front().color : kNarrator;
            info.lines = 0;
            for (const game::SourceNode& n : s.nodes)
                if (!n.text.empty()) ++info.lines;
        }
        TalkRow r;
        r.kind = story.empty() ? "talk" : "subtalk";
        r.id = t;
        r.selected = t == talk_;
        r.name = info.name;
        r.letter = first_letter(r.name);
        r.color = info.color;
        r.lines = info.lines;
        m_talks_.push_back(std::move(r));
        // A big open talk lists its scenes to go to.
        if (t == talk_ && info.lines > static_cast<int>(kPage))
            for (const game::SourceNode& n : src_.nodes) {
                if (n.scene.empty()) continue;
                TalkRow sc;
                sc.kind = "scene";
                sc.id = n.id;
                sc.name = n.scene;
                sc.color = kNarrator;
                m_talks_.push_back(std::move(sc));
            }
    }
    if (model_) model_.DirtyVariable("st_talks");
}

// --- typing -----------------------------------------------------------------

std::string* StoryEditor::edit_target(game::DialogueSource& s) const {
    game::SourceNode* n = s.node(edit_node_);
    const usize i = static_cast<usize>(std::max(edit_index_, 0));
    if (edit_what_ == "speaker") {
        for (game::SourceSpeaker& sp : s.speakers)
            if (sp.key == edit_node_) return &sp.name;
        return nullptr;
    }
    if (!n) return nullptr;
    if (edit_what_ == "text") return &n->text;
    if (edit_what_ == "note") return &n->note;
    if (edit_what_ == "scene") return &n->scene;
    if (edit_what_ == "choice") return i < n->choices.size() ? &n->choices[i].text : nullptr;
    if (edit_what_ == "stage") return i < n->stage.size() ? &n->stage[i] : nullptr;
    if (edit_what_ == "topic") {
        if (i >= n->keywords.size()) return nullptr;
        game::SourceNode* a = s.node(n->keywords[i].go);
        return a ? &a->text : nullptr;
    }
    if (edit_what_ == "fallback") {
        game::SourceNode* f = s.node(n->fallback);
        return f ? &f->text : nullptr;
    }
    return nullptr;
}

bool StoryEditor::begin_edit(const std::string& what, const std::string& node, int index) {
    if (editing() && edit_what_ == what && edit_node_ == node && edit_index_ == index) return true;
    commit_edit();
    close_menu();
    std::string text;
    if (what == "words") {
        const game::SourceNode* n = src_.node(node);
        if (!n || index < 0 || static_cast<usize>(index) >= n->keywords.size()) return false;
        text = words_shown(n->keywords[static_cast<usize>(index)].words);
    } else if (what == "cond" || what == "act") {
        text = node; // the raw source, given by the menu
    } else {
        edit_what_ = what;
        edit_node_ = node;
        edit_index_ = index;
        game::DialogueSource copy = src_;
        const std::string* t = edit_target(copy);
        if (!t) {
            edit_what_.clear();
            return false;
        }
        text = *t;
    }
    if (what != "cond" && what != "act") {
        edit_what_ = what;
        edit_node_ = node;
        edit_index_ = index;
    }
    m_edit_text_ = text;
    m_edit_rows_ = rows_for(text, what == "choice" || what == "topic" || what == "fallback" ? 50 : 64);
    focus_edit_ = true;
    if (model_) model_.DirtyVariable("st_edit_text");
    dirty_ = true;
    return true;
}

void StoryEditor::set_edit_text(std::string text) {
    m_edit_text_ = std::move(text);
    if (model_) model_.DirtyVariable("st_edit_text");
}

bool StoryEditor::commit_edit() {
    if (!editing()) return false;
    std::string text = m_edit_text_;
    text.erase(std::remove(text.begin(), text.end(), '\n'), text.end());
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    const std::string what = edit_what_;
    game::DialogueSource after = src_;
    std::string label = "Реплика";
    if (what == "words") {
        game::SourceNode* n = after.node(edit_node_);
        if (n && edit_index_ >= 0 && static_cast<usize>(edit_index_) < n->keywords.size()) {
            const std::vector<std::string> w = words_typed(text);
            if (!w.empty()) n->keywords[static_cast<usize>(edit_index_)].words = w;
        }
        label = "Слова темы";
    } else if (what == "cond" || what == "act") {
        // Typed source of a mark: put where the menu was opened.
        const std::string src = trim(text);
        edit_what_.clear();
        menu_.assign(1, {src});
        m_menu_items_.assign(1, {});
        const bool ok = menu_pick(0);
        dirty_ = true;
        return ok;
    } else if (std::string* t = edit_target(after)) {
        *t = what == "scene" || what == "speaker" || what == "stage" ? trim(text) : text;
        // A staging step typed away is gone.
        if (what == "stage" && t->empty())
            if (game::SourceNode* n = after.node(edit_node_)) n->stage.erase(n->stage.begin() + edit_index_);
        label = what == "note"      ? "Ремарка"
                : what == "scene"   ? "Название сцены"
                : what == "choice"  ? "Ответ героя"
                : what == "topic"   ? "Ответ на вопрос"
                : what == "speaker" ? "Имя говорящего"
                : what == "stage"   ? "Постановка"
                                    : "Реплика";
    }
    edit_what_.clear();
    edit_node_.clear();
    edit_index_ = -1;
    dirty_ = true;
    change(after, label);
    return true;
}

void StoryEditor::cancel_edit() {
    if (!editing()) return;
    edit_what_.clear();
    edit_node_.clear();
    edit_index_ = -1;
    dirty_ = true;
}

// --- structure --------------------------------------------------------------

std::string StoryEditor::add_line_after(const std::string& node) {
    commit_edit();
    game::DialogueSource after = src_;
    game::SourceNode* n = after.node(node);
    if (!n || n->junction() || !n->choices.empty() || !n->keywords.empty()) return {};
    game::SourceNode line;
    line.id = after.fresh_id("line");
    line.speaker = n->speaker;
    line.next = n->next;
    n->next = line.id;
    const i32 at = after.index_of(node);
    after.nodes.insert(after.nodes.begin() + at + 1, line);
    const std::string id = line.id;
    change(after, "Новая реплика");
    begin_edit("text", id);
    return id;
}

bool StoryEditor::add_choice(const std::string& node) {
    commit_edit();
    game::DialogueSource after = src_;
    game::SourceNode* n = after.node(node);
    if (!n || n->junction()) return false;
    // A line that went on by itself now waits for the answer, which goes on.
    game::SourceChoice c;
    c.go = n->choices.empty() ? n->next : "end";
    if (n->choices.empty()) n->next.clear();
    n->choices.push_back(c);
    const int index = static_cast<int>(n->choices.size() - 1);
    change(after, "Новый ответ героя");
    begin_edit("choice", node, index);
    return true;
}

bool StoryEditor::add_topic(const std::string& node) {
    commit_edit();
    game::DialogueSource after = src_;
    game::SourceNode* n = after.node(node);
    if (!n || n->junction()) return false;
    game::SourceNode answer;
    answer.id = after.fresh_id(node + "_answer");
    answer.speaker = n->speaker;
    answer.text = "…";
    answer.next = node;
    game::SourceKeyword k;
    k.words = {"слово"};
    k.go = answer.id;
    n->keywords.push_back(k);
    const int index = static_cast<int>(n->keywords.size() - 1);
    // A question the topics do not know gets an answer too.
    if (n->fallback.empty()) {
        game::SourceNode dunno;
        dunno.id = after.fresh_id(node + "_dunno");
        dunno.speaker = n->speaker;
        dunno.text = "Про это не знаю.";
        dunno.next = node;
        n->fallback = dunno.id;
        after.nodes.push_back(dunno);
    }
    after.nodes.push_back(answer);
    change(after, "Новая тема");
    begin_edit("words", node, index);
    return true;
}

std::string StoryEditor::add_scene() {
    commit_edit();
    game::DialogueSource after = src_;
    game::SourceNode n;
    n.id = after.fresh_id("scene");
    n.scene = "Новая сцена";
    n.speaker = src_.speakers.empty() ? std::string() : src_.speakers.front().key;
    after.nodes.push_back(n);
    const std::string id = n.id;
    change(after, "Новая сцена");
    begin_edit("text", id);
    return id;
}

bool StoryEditor::remove_line(const std::string& node) {
    commit_edit();
    game::DialogueSource after = src_;
    const i32 at = after.index_of(node);
    if (at < 0) return false;
    // What led here goes where this line went.
    const game::SourceNode gone = after.nodes[static_cast<usize>(at)];
    const std::string to = gone.choices.empty() && gone.branches.empty() && gone.keywords.empty() ? gone.next : std::string("end");
    auto fix = [&](std::string& go) {
        if (go == node) go = to.empty() ? "end" : to;
    };
    for (game::SourceEntry& e : after.start) fix(e.go);
    for (game::SourceNode& n : after.nodes) {
        fix(n.next);
        if (n.next == "end") n.next.clear();
        for (game::SourceEntry& b : n.branches) fix(b.go);
        for (game::SourceChoice& c : n.choices) fix(c.go);
        for (game::SourceKeyword& k : n.keywords) fix(k.go);
        if (n.fallback == node) n.fallback.clear();
    }
    // Its inline answers go with it.
    std::vector<std::string> answers;
    for (const game::SourceKeyword& k : gone.keywords)
        if (inline_answer(k.go, node)) answers.push_back(k.go);
    if (!gone.fallback.empty() && inline_answer(gone.fallback, node)) answers.push_back(gone.fallback);
    after.nodes.erase(after.nodes.begin() + at);
    for (const std::string& a : answers)
        if (const i32 i = after.index_of(a); i >= 0) after.nodes.erase(after.nodes.begin() + i);
    change(after, "Реплика убрана");
    return true;
}

bool StoryEditor::remove_choice(const std::string& node, int index) {
    commit_edit();
    game::DialogueSource after = src_;
    game::SourceNode* n = after.node(node);
    if (!n || index < 0 || static_cast<usize>(index) >= n->choices.size()) return false;
    n->choices.erase(n->choices.begin() + index);
    change(after, "Ответ героя убран");
    return true;
}

bool StoryEditor::remove_topic(const std::string& node, int index) {
    commit_edit();
    game::DialogueSource after = src_;
    game::SourceNode* n = after.node(node);
    if (!n || index < 0 || static_cast<usize>(index) >= n->keywords.size()) return false;
    const std::string answer = n->keywords[static_cast<usize>(index)].go;
    const bool own = inline_answer(answer, node);
    n->keywords.erase(n->keywords.begin() + index);
    if (n->keywords.empty() && !n->fallback.empty() && inline_answer(n->fallback, node)) {
        if (const i32 i = after.index_of(n->fallback); i >= 0) after.nodes.erase(after.nodes.begin() + i);
        n->fallback.clear();
    }
    if (own)
        if (const i32 i = after.index_of(answer); i >= 0) after.nodes.erase(after.nodes.begin() + i);
    change(after, "Тема убрана");
    return true;
}

// --- menus ------------------------------------------------------------------

bool StoryEditor::open_menu(const std::string& where, const std::string& node, int index, const std::string& part, int mark) {
    commit_edit();
    const game::SourceNode* n = src_.node(node);
    if (where != "start" && !n) return false;
    if (where == "start" && (index < 0 || static_cast<usize>(index) >= src_.start.size())) return false;
    menu_where_ = where;
    menu_node_ = node;
    menu_index_ = index;
    menu_part_ = part;
    menu_mark_ = mark;
    menu_.clear();
    m_menu_items_.clear();
    const usize i = static_cast<usize>(std::max(index, 0));

    // What it is now.
    std::string now;
    if (part == "if") {
        now = where == "start"    ? src_.start[i].cond
              : where == "branch" ? (i < n->branches.size() ? n->branches[i].cond : "")
              : where == "choice" ? (i < n->choices.size() ? n->choices[i].cond : "")
              : where == "topic"  ? (i < n->keywords.size() ? n->keywords[i].cond : "")
                                  : "";
    } else if (part == "do") {
        const std::string act = where == "choice" ? (i < n->choices.size() ? n->choices[i].act : "")
                                : where == "topic" ? (i < n->keywords.size() ? n->keywords[i].act : "")
                                                   : n->act;
        const std::vector<std::string> st = actions_of(act);
        if (mark >= 0 && static_cast<usize>(mark) < st.size()) now = st[static_cast<usize>(mark)];
    } else if (part == "call") {
        now = n->call;
    } else if (part == "goto" || part == "next") {
        now = where == "start"    ? src_.start[i].go
              : where == "branch" ? (i < n->branches.size() ? n->branches[i].go : "")
              : where == "choice" ? (i < n->choices.size() ? n->choices[i].go : "")
              : where == "topic"  ? (i < n->keywords.size() ? n->keywords[i].go : "")
                                  : n->next;
        if (now.empty()) now = "end";
    } else if (part == "speaker") {
        if (n->junction()) return false;
        now = n->speaker;
    } else {
        return false;
    }
    auto add = [&](std::string text, const char* icon, std::string value, std::string group = {}) {
        m_menu_items_.push_back({text, icon, group, value == now});
        menu_.push_back({std::move(value)});
    };
    std::vector<std::string> items;
    if (library_)
        for (const objects::Template& t : library_->templates())
            if (t.kind == "pickup") items.push_back(t.id);
    for (const std::string& u : items_used())
        if (std::find(items.begin(), items.end(), u) == items.end()) items.push_back(u);

    if (part == "if") {
        m_menu_title_ = "Когда звучит";
        add(where == "start" ? "всегда (иначе)" : "всегда", "check", "");
        for (const game::Quest& q : quests_.quests()) {
            std::vector<f64> stages{0};
            for (const game::QuestStage& s : q.stages) stages.push_back(s.at);
            if (q.done_at > 0) stages.push_back(q.done_at);
            for (const f64 at : stages) {
                add("задание «" + q.title + "»: " + stage_name(q, at), "flag", q.var + " == " + num(at), "Задания");
                if (at > 0 && at != stages.back())
                    add("задание «" + q.title + "»: " + stage_name(q, at) + " или дальше", "flag", q.var + " >= " + num(at), "Задания");
            }
        }
        for (const std::string& it : items) {
            add("у героя есть " + item_name(it), "backpack", "has(\"" + it + "\")", "Что у героя");
            add("у героя нет: " + item_name(it), "backpack", "not has(\"" + it + "\")", "Что у героя");
        }
        if (!now.empty() && std::none_of(m_menu_items_.begin(), m_menu_items_.end(), [](const MenuItem& m) { return m.current; }))
            add("как сейчас: " + join(phrases(now, false), " и "), "help", now);
        add("своё условие…", "edit", kRaw);
    } else if (part == "do") {
        m_menu_title_ = mark < 0 ? "Что меняет" : "Что меняет";
        std::string count = "1";
        std::smatch m;
        if (std::regex_match(now, m, give_re()) && m[3].matched) count = m[3].str();
        for (const game::Quest& q : quests_.quests()) {
            std::vector<f64> stages;
            for (const game::QuestStage& s : q.stages) stages.push_back(s.at);
            if (q.done_at > 0) stages.push_back(q.done_at);
            for (const f64 at : stages) add("задание «" + q.title + "» → " + stage_name(q, at), "flag", q.var + " = " + num(at), "Задания");
        }
        for (const std::string& it : items)
            add("герою: " + item_name(it) + " +" + count, "add_circle", "give(\"" + it + "\", " + count + ")", "Предметы");
        for (const std::string& it : items)
            add("у героя: " + item_name(it) + " −" + count, "remove_circle", "take(\"" + it + "\", " + count + ")", "Предметы");
        if (!now.empty() && std::none_of(m_menu_items_.begin(), m_menu_items_.end(), [](const MenuItem& mi) { return mi.current; }))
            add("как сейчас: " + join(phrases(now, true), ", "), "bolt", now);
        add("своё действие…", "edit", kRaw);
        if (mark >= 0) add("убрать", "delete", "");
    } else if (part == "goto" || part == "next" || part == "call") {
        m_menu_title_ = part == "call" ? "Что сыграть сначала" : "Куда дальше";
        // A big talk offers its scenes and the lines around this one.
        const bool big = src_.nodes.size() > kPage;
        const i32 here = src_.index_of(node);
        for (usize k = 0; k < src_.nodes.size(); ++k) {
            const game::SourceNode& o = src_.nodes[k];
            if (o.id == node && part == "next") continue;
            if (big && o.scene.empty() && std::abs(static_cast<i32>(k) - here) > 12) continue;
            add(anchor(o.id), o.junction() ? "call_split" : "chat", o.id, o.scene);
        }
        if (part != "call") {
            add("конец разговора", "stop", "end");
            add("назад, откуда позвали", "undo", "return");
            add("новая реплика", "add", kNew);
        }
    } else {
        m_menu_title_ = "Кто говорит";
        for (const game::SourceSpeaker& s : src_.speakers) add(s.name, "person", s.key);
        add("рассказчик", "menu_book", "");
        add("новый говорящий…", "person_add", kSpeakerNew);
    }
    m_menu_ = true;
    if (model_)
        for (const char* name : {"st_menu", "st_menu_title", "st_menu_items"}) model_.DirtyVariable(name);
    return true;
}

int StoryEditor::menu_find(const std::string& text) const {
    for (usize i = 0; i < m_menu_items_.size(); ++i)
        if (m_menu_items_[i].text == text) return static_cast<int>(i);
    return -1;
}

void StoryEditor::close_menu() {
    if (!m_menu_) return;
    m_menu_ = false;
    if (model_) model_.DirtyVariable("st_menu");
}

bool StoryEditor::menu_pick(usize pick) {
    if (pick >= menu_.size()) return false;
    const std::string value = menu_[pick].value;
    const std::string where = menu_where_, node = menu_node_, part = menu_part_;
    const int index = menu_index_, mark = menu_mark_;
    close_menu();
    if (value == kRaw) {
        // Typing the mark's source: the menu's place stays for commit_edit.
        std::string now;
        const game::SourceNode* n = src_.node(node);
        const usize i = static_cast<usize>(std::max(index, 0));
        if (part == "if") {
            now = where == "start"    ? src_.start[i].cond
                  : where == "branch" ? n->branches[i].cond
                  : where == "choice" ? n->choices[i].cond
                  : where == "topic"  ? n->keywords[i].cond
                                      : "";
        } else {
            const std::string act = where == "choice" ? n->choices[i].act : where == "topic" ? n->keywords[i].act : n->act;
            const std::vector<std::string> st = actions_of(act);
            if (mark >= 0 && static_cast<usize>(mark) < st.size()) now = st[static_cast<usize>(mark)];
        }
        commit_edit();
        edit_what_ = part == "if" ? "cond" : "act";
        edit_node_ = node;
        edit_index_ = index;
        menu_where_ = where;
        menu_node_ = node;
        menu_index_ = index;
        menu_part_ = part;
        menu_mark_ = mark;
        m_edit_text_ = now;
        m_edit_rows_ = 1;
        focus_edit_ = true;
        if (model_) model_.DirtyVariable("st_edit_text");
        dirty_ = true;
        return true;
    }

    game::DialogueSource after = src_;
    game::SourceNode* n = after.node(node);
    const usize i = static_cast<usize>(std::max(index, 0));
    std::string label;
    if (part == "if") {
        std::string* cond = where == "start"                                    ? &after.start[i].cond
                            : where == "branch" && n && i < n->branches.size() ? &n->branches[i].cond
                            : where == "choice" && n && i < n->choices.size()  ? &n->choices[i].cond
                            : where == "topic" && n && i < n->keywords.size()  ? &n->keywords[i].cond
                                                                                : nullptr;
        if (!cond) return false;
        *cond = value;
        label = "Когда звучит";
    } else if (part == "do") {
        std::string* act = where == "choice" && n && i < n->choices.size()  ? &n->choices[i].act
                           : where == "topic" && n && i < n->keywords.size() ? &n->keywords[i].act
                           : n                                               ? &n->act
                                                                             : nullptr;
        if (!act) return false;
        std::vector<std::string> st = actions_of(*act);
        if (mark >= 0 && static_cast<usize>(mark) < st.size()) {
            if (value.empty()) st.erase(st.begin() + mark);
            else st[static_cast<usize>(mark)] = value;
        } else if (!value.empty()) {
            st.push_back(value);
        }
        *act = join(st, "; ");
        label = "Что меняет";
    } else if (part == "goto" || part == "next" || part == "call") {
        std::string go = value;
        if (value == kNew) {
            game::SourceNode line;
            line.id = after.fresh_id("line");
            line.speaker = n && !n->speaker.empty() ? n->speaker : (after.speakers.empty() ? "" : after.speakers.front().key);
            after.nodes.push_back(line);
            go = line.id;
        }
        std::string* to = where == "start"                                    ? &after.start[i].go
                          : where == "branch" && n && i < n->branches.size() ? &n->branches[i].go
                          : where == "choice" && n && i < n->choices.size()  ? &n->choices[i].go
                          : where == "topic" && n && i < n->keywords.size()  ? &n->keywords[i].go
                          : n && part == "call"                               ? &n->call
                          : n                                                 ? &n->next
                                                                              : nullptr;
        if (!to) return false;
        *to = part == "next" && go == "end" ? std::string() : go;
        label = "Куда дальше";
        change(after, label);
        if (value == kNew) begin_edit("text", go);
        else jump(go);
        return true;
    } else if (part == "speaker") {
        if (!n) return false;
        std::string key = value;
        if (value == kSpeakerNew) {
            key = "speaker";
            for (u32 k = 2; after.speaker(key); ++k) key = "speaker" + std::to_string(k);
            after.speakers.push_back({key, "Новый говорящий", "#7ec8a0", ""});
        }
        n->speaker = key;
        label = "Кто говорит";
        change(after, label);
        if (value == kSpeakerNew) begin_edit("speaker", key);
        return true;
    }
    change(after, label);
    return true;
}

void StoryEditor::jump(const std::string& target) {
    if (target.empty() || target == "end" || target == "return") return;
    std::string node = target;
    if (const usize colon = target.find(':'); colon != std::string::npos) {
        // Another talk of the story: shown, the test goes on where it was.
        const std::string talk = full_id(target.substr(0, colon));
        node = target.substr(colon + 1);
        if (talk != talk_) {
            const std::vector<std::string> all = talks();
            if (std::find(all.begin(), all.end(), talk) == all.end()) return;
            commit_edit();
            close_menu();
            resolve(target.substr(0, colon));
            talk_ = talk;
            src_ = {};
            std::vector<u8> bytes;
            std::string error;
            if (read_file(talk_path(talk_), bytes)) src_.parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), &error);
            lit_rule_ = -1;
            page_from_ = 0;
        }
    }
    // An inline answer is shown in its asker's topics.
    std::string at = node;
    for (const game::SourceNode& n : src_.nodes)
        for (const game::SourceKeyword& k : n.keywords)
            if (k.go == node && inline_answer(node, n.id)) at = n.id;
    flash_ = at;
    flash_until_ = SDL_GetTicks() + kFlashMs;
    scroll_to_ = at;
    page_to_ = at;
    dirty_ = true;
}

bool StoryEditor::line_shown(const std::string& node) const {
    return std::any_of(m_lines_.begin(), m_lines_.end(), [&](const LineView& l) { return l.id == node; });
}

void StoryEditor::page(int dir) {
    if (dir < 0) page_from_ = page_from_ > kPage ? page_from_ - kPage : 0;
    else page_from_ += kPage;
    scroll_to_.clear();
    dirty_ = true;
}

// --- the test ---------------------------------------------------------------

game::Value StoryEditor::call(std::string_view name, const std::vector<game::Value>& a) {
    const std::string item = a.empty() ? std::string() : a[0].text();
    const f64 n = a.size() > 1 ? a[1].number() : 1.0;
    const f64 have = vars_.get("inv." + item).number();
    if (name == "has") return game::Value(have >= n);
    if (name == "count") return game::Value(have);
    if (name == "give") {
        vars_.set("inv." + item, have + n);
        return game::Value(true);
    }
    if (name == "take") {
        if (have < n) return game::Value(false);
        vars_.set("inv." + item, have - n);
        return game::Value(true);
    }
    if (name == "renpy" && !a.empty()) log("code", "Ren'Py не выполняется здесь: " + code_line(a[0].text()));
    return game::Value(0.0);
}

void StoryEditor::log_line() {
    const auto& l = runner_.line();
    log("chat", l.speaker.empty() ? l.text : l.speaker + ": " + l.text);
}

void StoryEditor::log(const char* icon, std::string text, const char* kind) {
    m_log_.push_back({icon, std::move(text), kind});
    if (m_log_.size() > 200) m_log_.erase(m_log_.begin());
    if (model_) model_.DirtyVariable("st_log");
}

void StoryEditor::reload_play(bool keep_place) {
    const std::string node = keep_place && runner_.active() ? std::string(runner_.node_id()) : std::string();
    runner_.stop();
    game::DialogueReport report;
    game::Dialogue& d = open_dialogue();
    d = {};
    if (!d.load(src_.json(), report, {"has", "count", "give", "take", "renpy"}))
        for (const std::string& e : report.errors) FORGE_ERROR("Разговор %s: %s", talk_.c_str(), e.c_str());
    // A talk that had nothing to say (a new one) plays as soon as it has.
    else if (keep_place && !runner_started_) replay_ = true;
    if (!node.empty() && d.node(node)) {
        // Showing the line again does not count: what it changes is kept as it was.
        const std::string saved = vars_.to_json();
        runner_.start_at(d, node);
        vars_.from_json(saved);
    }
    rebuild_play();
}

namespace {
// What changed in the variables, for the log.
std::map<std::string, f64> numbers(const game::Vars& vars) {
    std::map<std::string, f64> out;
    for (const auto& [k, v] : vars.all()) out[k] = v.number();
    return out;
}
} // namespace

void StoryEditor::play() {
    commit_edit();
    runner_.stop();
    lit_rule_ = lit_choice_ = lit_topic_ = -1;
    lit_choice_node_.clear();
    lit_topic_node_.clear();
    const game::CallFn fn = [this](std::string_view n, const std::vector<game::Value>& a) { return call(n, a); };
    for (usize i = 0; i < src_.start.size(); ++i) {
        std::string error;
        if (game::Expr::parse(src_.start[i].cond, &error).test(vars_, fn)) {
            lit_rule_ = static_cast<int>(i);
            break;
        }
    }
    const std::string who = src_.speakers.empty() ? talk_ : src_.speakers.front().name;
    log("play_arrow", "Герой заговорил: " + who);
    const auto before = numbers(vars_);
    runner_started_ = runner_.start(open_dialogue()) && runner_.active();
    if (!runner_started_) log("block", "Сейчас сказать нечего: ни одно начало не подходит");
    // Changes made on the way are logged as the marks say them.
    const auto after = numbers(vars_);
    for (const auto& [k, v] : after) {
        const auto was = before.find(k);
        const f64 old = was == before.end() ? 0 : was->second;
        if (old == v) continue;
        if (const game::Quest* q = quest_of_var(k)) log("bolt", "задание «" + q->title + "» → " + stage_name(*q, v), "eff");
        else if (k.rfind("inv.", 0) == 0) log("bolt", "у героя: " + item_name(k.substr(4)) + " " + num(v), "eff");
    }
    if (runner_.active()) log_line();
    follow();
    dirty_ = true;
}

bool StoryEditor::play_choose(usize visible) {
    if (!runner_.active()) return false;
    const game::SourceNode* n = src_.node(runner_.node_id());
    if (!n) return false;
    const game::CallFn fn = [this](std::string_view nm, const std::vector<game::Value>& a) { return call(nm, a); };
    usize seen = 0;
    for (usize i = 0; i < n->choices.size(); ++i) {
        std::string error;
        if (!game::Expr::parse(n->choices[i].cond, &error).test(vars_, fn)) continue;
        if (seen++ != visible) continue;
        lit_choice_ = static_cast<int>(i);
        lit_choice_node_ = n->id;
        log("reply", "Я: " + n->choices[i].text);
        break;
    }
    lit_topic_ = -1;
    lit_topic_node_.clear();
    const auto before = numbers(vars_);
    if (!runner_.choose(static_cast<u32>(visible))) return false;
    const auto after = numbers(vars_);
    for (const auto& [k, v] : after) {
        const auto was = before.find(k);
        const f64 old = was == before.end() ? 0 : was->second;
        if (old == v) continue;
        if (const game::Quest* q = quest_of_var(k)) log("bolt", "задание «" + q->title + "» → " + stage_name(*q, v), "eff");
        else if (k.rfind("inv.", 0) == 0) log("bolt", item_name(k.substr(4)) + ": " + (v > old ? "+" : "−") + num(std::fabs(v - old)), "eff");
    }
    if (runner_.active()) log_line();
    else log("stop", "Разговор окончен");
    follow();
    dirty_ = true;
    return true;
}

bool StoryEditor::play_next() {
    if (!runner_.active() || !runner_.line().choices.empty()) return false;
    const auto before = numbers(vars_);
    runner_.advance();
    const auto after = numbers(vars_);
    for (const auto& [k, v] : after) {
        const auto was = before.find(k);
        const f64 old = was == before.end() ? 0 : was->second;
        if (old == v) continue;
        if (const game::Quest* q = quest_of_var(k)) log("bolt", "задание «" + q->title + "» → " + stage_name(*q, v), "eff");
        else if (k.rfind("inv.", 0) == 0) log("bolt", item_name(k.substr(4)) + ": " + (v > old ? "+" : "−") + num(std::fabs(v - old)), "eff");
    }
    lit_choice_ = lit_topic_ = -1;
    if (runner_.active()) log_line();
    else log("stop", "Разговор окончен");
    follow();
    dirty_ = true;
    return true;
}

bool StoryEditor::play_ask(const std::string& typed) {
    if (!runner_.active() || !runner_.line().asks_keyword || trim(typed).empty()) return false;
    const game::SourceNode* n = src_.node(runner_.node_id());
    if (!n) return false;
    log("keyboard", "Я: " + typed);
    const std::vector<std::string> words = game::split_words(typed);
    const game::CallFn fn = [this](std::string_view nm, const std::vector<game::Value>& a) { return call(nm, a); };
    lit_topic_ = -1;
    lit_topic_node_ = n->id;
    for (usize i = 0; i < n->keywords.size() && lit_topic_ < 0; ++i) {
        std::string error;
        if (!game::Expr::parse(n->keywords[i].cond, &error).test(vars_, fn)) continue;
        for (const std::string& w : n->keywords[i].words)
            if (game::keyword_matches(words, game::to_lower_utf8(w))) {
                lit_topic_ = static_cast<int>(i);
                break;
            }
    }
    const bool hit = runner_.ask(typed);
    if (runner_.active()) log_line();
    follow();
    m_ask_text_.clear();
    if (model_) model_.DirtyVariable("st_ask_text");
    dirty_ = true;
    return hit;
}

void StoryEditor::set_quest(const std::string& quest, f64 value) {
    const game::Quest* q = quests_.find(quest);
    if (!q) return;
    vars_.set(q->var, value);
    log("tune", "задание «" + q->title + "»: " + stage_name(*q, value));
    dirty_ = true;
}

void StoryEditor::step_quest(const std::string& quest, int dir) {
    const game::Quest* q = quests_.find(quest);
    if (!q) return;
    std::vector<f64> stages{0};
    for (const game::QuestStage& s : q->stages) stages.push_back(s.at);
    if (q->done_at > 0) stages.push_back(q->done_at);
    const f64 now = vars_.get(q->var).number();
    usize at = 0;
    for (usize i = 0; i < stages.size(); ++i)
        if (stages[i] <= now) at = i;
    at = (at + (dir < 0 ? stages.size() - 1 : 1)) % stages.size();
    set_quest(q->id, stages[at]);
}

void StoryEditor::place_menu(Rml::Event& ev) {
    Rml::Element* root = ev.GetCurrentElement()->GetOwnerDocument()->GetElementById("story");
    const Rml::Vector2f at = root ? root->GetAbsoluteOffset(Rml::BoxArea::Border) : Rml::Vector2f(0, 0);
    const Rml::Vector2f size = root ? root->GetBox().GetSize(Rml::BoxArea::Border) : Rml::Vector2f(1200, 800);
    m_menu_x_ = std::clamp(ev.GetParameter<float>("mouse_x", 0) - at.x, 8.0f, std::max(8.0f, size.x - 380));
    m_menu_y_ = std::clamp(ev.GetParameter<float>("mouse_y", 0) - at.y + 12, 8.0f, std::max(8.0f, size.y - 440));
    if (model_) {
        model_.DirtyVariable("st_menu_x");
        model_.DirtyVariable("st_menu_y");
    }
}

void StoryEditor::step_item(const std::string& item, int dir) {
    // As much as the conversation asks for at once.
    f64 step = 1;
    static const std::regex re(R"re(has\(\s*"([^"]+)"\s*,\s*([0-9.]+)\s*\))re");
    auto scan = [&](const std::string& s) {
        for (std::sregex_iterator it(s.begin(), s.end(), re), end; it != end; ++it)
            if ((*it)[1].str() == item) step = std::max(step, std::atof((*it)[2].str().c_str()));
    };
    for (const game::SourceEntry& e : src_.start) scan(e.cond);
    for (const game::SourceNode& n : src_.nodes) {
        for (const game::SourceEntry& b : n.branches) scan(b.cond);
        for (const game::SourceChoice& c : n.choices) scan(c.cond);
    }
    const f64 now = std::max(0.0, vars_.get("inv." + item).number() + dir * step);
    vars_.set("inv." + item, now);
    log("tune", item_name(item) + " у героя: " + num(now));
    dirty_ = true;
}

std::vector<std::string> StoryEditor::vars_used() const {
    // Names in the talk's conditions that are not quests, items or functions.
    std::vector<std::string> out;
    static const std::regex name_re(R"re([A-Za-z_][\w.]*)re");
    static const std::regex text_re(R"re("(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*')re");
    auto scan = [&](const std::string& cond) {
        const std::string plain = std::regex_replace(cond, text_re, "\"\"");
        for (std::sregex_iterator it(plain.begin(), plain.end(), name_re), end; it != end && out.size() < 16; ++it) {
            const std::string name = (*it)[0];
            const usize after = static_cast<usize>(it->position() + it->length());
            if (after < plain.size() && plain[after] == '(') continue;
            if (name == "and" || name == "or" || name == "not" || name == "true" || name == "false" || name.rfind("inv.", 0) == 0 || quest_of_var(name))
                continue;
            if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
        }
    };
    for (const game::SourceEntry& e : src_.start) scan(e.cond);
    for (const game::SourceNode& n : src_.nodes) {
        for (const game::SourceEntry& b : n.branches) scan(b.cond);
        for (const game::SourceChoice& c : n.choices) scan(c.cond);
        for (const game::SourceKeyword& k : n.keywords) scan(k.cond);
    }
    return out;
}

void StoryEditor::step_var(const std::string& var, int dir) {
    const game::Value v = vars_.get(var);
    if (v.is_text()) return;
    vars_.set(var, std::max(0.0, v.number() + dir));
    log("tune", var + " = " + num(vars_.get(var).number()));
    dirty_ = true;
}

void StoryEditor::rebuild_state() {
    m_state_.clear();
    for (const game::Quest& q : quests_.quests()) {
        StateRow r;
        r.id = q.id;
        r.kind = "quest";
        r.name = "Задание «" + q.title + "»";
        r.value = stage_name(q, vars_.get(q.var).number());
        m_state_.push_back(std::move(r));
    }
    for (const std::string& it : items_used()) {
        StateRow r;
        r.id = it;
        r.kind = "item";
        r.name = item_name(it);
        r.count = static_cast<int>(vars_.get("inv." + it).number());
        r.value = num(vars_.get("inv." + it).number());
        if (library_ && template_icon)
            if (const objects::Template* t = library_->find(std::string_view(it))) r.icon = template_icon(*t);
        m_state_.push_back(std::move(r));
    }
    for (const std::string& v : vars_used()) {
        StateRow r;
        r.id = v;
        r.kind = "var";
        r.name = v;
        const game::Value value = vars_.get(v);
        r.value = value.is_text() ? "«" + value.text() + "»" : num(value.number());
        m_state_.push_back(std::move(r));
    }
    if (model_) model_.DirtyVariable("st_state");
}

void StoryEditor::rebuild_play() {
    const bool on = runner_.active();
    const game::DialogueLine& l = runner_.line();
    set(m_play_on_, on, "st_play_on");
    set(m_play_who_, Rml::String(on ? l.speaker : ""), "st_play_who");
    set(m_play_color_, Rml::String(on && !l.color.empty() ? l.color : kNarrator), "st_play_color");
    set(m_play_text_, Rml::String(on ? l.text : ""), "st_play_text");
    set(m_play_asks_, on && l.asks_keyword, "st_play_asks");
    set(m_play_next_, on && l.choices.empty(), "st_play_next");
    std::vector<Rml::String> choices;
    if (on)
        for (const std::string& c : l.choices) choices.push_back(c);
    if (choices != m_play_choices_) {
        m_play_choices_ = std::move(choices);
        if (model_) model_.DirtyVariable("st_play_choices");
    }
}

// --- Ren'Py import -----------------------------------------------------------

bool StoryEditor::import_renpy(const std::filesystem::path& source) {
    if (importing()) return false;
    if (!import_loaded_) {
        import_converters_.load({utf8_path(FORGE_CONVERTERS_DIR), game_dir_ / "converters"});
        import_loaded_ = true;
    }
    const Converter* c = import_converters_.find("renpy");
    if (!c || import_converters_.python().empty()) {
        set(m_import_text_, Rml::String("Для импорта нужен Python: он ставится вместе с редактором"), "st_import_text");
        return false;
    }
    std::error_code ec;
    import_source_ = source;
    import_staging_ = std::filesystem::temp_directory_path(ec) / ("forge_renpy_" + std::to_string(SDL_GetTicksNS()));
    import_job_ = import_converters_.start(*c, source, import_staging_, R"({"whole": true})");
    set(m_import_text_, Rml::String("Переношу игру Ren'Py…"), "st_import_text");
    if (model_) model_.DirtyVariable("st_importing");
    return true;
}

void StoryEditor::poll_import() {
    {
        std::vector<std::filesystem::path> picked;
        {
            std::lock_guard lock(import_mutex_);
            picked.swap(import_picked_);
        }
        if (!picked.empty()) import_renpy(picked.front());
    }
    if (!importing()) return;
    for (Converters::Result& r : import_converters_.take_finished()) {
        if (r.id != import_job_) continue;
        import_job_ = 0;
        if (model_) model_.DirtyVariable("st_importing");
        if (!r.ok) {
            set(m_import_text_, Rml::String("Не получилось: " + r.error), "st_import_text");
            FORGE_ERROR("Импорт Ren'Py: %s", r.error.c_str());
            continue;
        }
        // The story is named after the game's folder (the one holding game/).
        std::error_code ec;
        std::filesystem::path folder = std::filesystem::is_directory(import_source_, ec) ? import_source_ : import_source_.parent_path();
        if (folder.filename() == "game" && folder.has_parent_path()) folder = folder.parent_path();
        std::string name = path_to_utf8(folder.filename());
        for (char& ch : name)
            if (ch == ':' || ch == '/' || ch == '\\' || ch == '.') ch = '_';
        if (name.empty()) name = "renpy";
        std::string story = name;
        for (u32 n = 2; std::filesystem::exists(game_dir_ / "dialogues" / utf8_path(story), ec); ++n) story = name + " " + std::to_string(n);
        const std::filesystem::path dest = game_dir_ / "dialogues" / utf8_path(story);
        std::filesystem::create_directories(dest, ec);
        int talks_in = 0;
        for (const std::filesystem::path& f : r.files) {
            std::filesystem::rename(f, dest / f.filename(), ec);
            if (ec) std::filesystem::copy_file(f, dest / f.filename(), std::filesystem::copy_options::overwrite_existing, ec);
            talks_in += f.extension() == ".json";
        }
        std::filesystem::remove_all(r.out, ec);
        stories_open_.insert(story);
        // The talk the game starts with, else the first.
        std::string first;
        for (const std::string& t : talks())
            if (story_of(t) == story) {
                if (first.empty()) first = t;
                std::vector<u8> bytes;
                if (read_file(talk_path(t), bytes) &&
                    std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()).find("{\"id\": \"start\"") != std::string_view::npos)
                    first = t;
            }
        set(m_import_text_, Rml::String("Перенесено разговоров: " + std::to_string(talks_in) + ". Что не перенеслось, в «Импорт Ren'Py.txt» рядом."),
            "st_import_text");
        FORGE_INFO("Импорт Ren'Py: %d разговоров в dialogues/%s", talks_in, story.c_str());
        if (!first.empty()) open(first);
        dirty_ = true;
    }
}

// --- the UI -----------------------------------------------------------------

void StoryEditor::bind(Rml::DataModelConstructor& model) {
    if (auto s = model.RegisterStruct<Chip>()) {
        s.RegisterMember("kind", &Chip::kind);
        s.RegisterMember("icon", &Chip::icon);
        s.RegisterMember("text", &Chip::text);
        s.RegisterMember("part", &Chip::part);
        s.RegisterMember("mark", &Chip::mark);
        s.RegisterMember("seq", &Chip::seq);
        s.RegisterMember("look", &Chip::look);
    }
    model.RegisterArray<std::vector<Chip>>();
    if (auto s = model.RegisterStruct<RuleView>()) {
        s.RegisterMember("index", &RuleView::index);
        s.RegisterMember("lit", &RuleView::lit);
        s.RegisterMember("chips", &RuleView::chips);
    }
    model.RegisterArray<std::vector<RuleView>>();
    if (auto s = model.RegisterStruct<ChoiceView>()) {
        s.RegisterMember("index", &ChoiceView::index);
        s.RegisterMember("text", &ChoiceView::text);
        s.RegisterMember("lit", &ChoiceView::lit);
        s.RegisterMember("editing", &ChoiceView::editing);
        s.RegisterMember("chips", &ChoiceView::chips);
    }
    model.RegisterArray<std::vector<ChoiceView>>();
    if (auto s = model.RegisterStruct<TopicView>()) {
        s.RegisterMember("index", &TopicView::index);
        s.RegisterMember("words", &TopicView::words);
        s.RegisterMember("text", &TopicView::text);
        s.RegisterMember("answer", &TopicView::answer);
        s.RegisterMember("lit", &TopicView::lit);
        s.RegisterMember("editing_words", &TopicView::editing_words);
        s.RegisterMember("editing_text", &TopicView::editing_text);
        s.RegisterMember("chips", &TopicView::chips);
    }
    model.RegisterArray<std::vector<TopicView>>();
    if (auto s = model.RegisterStruct<LineView>()) {
        s.RegisterMember("id", &LineView::id);
        s.RegisterMember("kind", &LineView::kind);
        s.RegisterMember("scene", &LineView::scene);
        s.RegisterMember("note", &LineView::note);
        s.RegisterMember("who", &LineView::who);
        s.RegisterMember("color", &LineView::color);
        s.RegisterMember("text", &LineView::text);
        s.RegisterMember("fallback", &LineView::fallback);
        s.RegisterMember("pose", &LineView::pose);
        s.RegisterMember("stage", &LineView::stage);
        s.RegisterMember("editing_stage", &LineView::editing_stage);
        s.RegisterMember("lit", &LineView::lit);
        s.RegisterMember("flash", &LineView::flash);
        s.RegisterMember("editing", &LineView::editing);
        s.RegisterMember("editing_note", &LineView::editing_note);
        s.RegisterMember("editing_scene", &LineView::editing_scene);
        s.RegisterMember("editing_fallback", &LineView::editing_fallback);
        s.RegisterMember("topics_on", &LineView::topics_on);
        s.RegisterMember("choices_on", &LineView::choices_on);
        s.RegisterMember("can_add", &LineView::can_add);
        s.RegisterMember("has_fallback", &LineView::has_fallback);
        s.RegisterMember("chips", &LineView::chips);
        s.RegisterMember("choices", &LineView::choices);
        s.RegisterMember("topics", &LineView::topics);
    }
    model.RegisterArray<std::vector<LineView>>();
    if (auto s = model.RegisterStruct<TalkRow>()) {
        s.RegisterMember("kind", &TalkRow::kind);
        s.RegisterMember("open", &TalkRow::open);
        s.RegisterMember("id", &TalkRow::id);
        s.RegisterMember("name", &TalkRow::name);
        s.RegisterMember("letter", &TalkRow::letter);
        s.RegisterMember("color", &TalkRow::color);
        s.RegisterMember("lines", &TalkRow::lines);
        s.RegisterMember("selected", &TalkRow::selected);
    }
    model.RegisterArray<std::vector<TalkRow>>();
    if (auto s = model.RegisterStruct<SpeakerRow>()) {
        s.RegisterMember("key", &SpeakerRow::key);
        s.RegisterMember("name", &SpeakerRow::name);
        s.RegisterMember("letter", &SpeakerRow::letter);
        s.RegisterMember("color", &SpeakerRow::color);
        s.RegisterMember("editing", &SpeakerRow::editing);
    }
    model.RegisterArray<std::vector<SpeakerRow>>();
    if (auto s = model.RegisterStruct<MenuItem>()) {
        s.RegisterMember("text", &MenuItem::text);
        s.RegisterMember("icon", &MenuItem::icon);
        s.RegisterMember("group", &MenuItem::group);
        s.RegisterMember("current", &MenuItem::current);
    }
    model.RegisterArray<std::vector<MenuItem>>();
    if (auto s = model.RegisterStruct<StateRow>()) {
        s.RegisterMember("id", &StateRow::id);
        s.RegisterMember("kind", &StateRow::kind);
        s.RegisterMember("name", &StateRow::name);
        s.RegisterMember("value", &StateRow::value);
        s.RegisterMember("icon", &StateRow::icon);
        s.RegisterMember("count", &StateRow::count);
    }
    model.RegisterArray<std::vector<StateRow>>();
    if (auto s = model.RegisterStruct<LogRow>()) {
        s.RegisterMember("icon", &LogRow::icon);
        s.RegisterMember("text", &LogRow::text);
        s.RegisterMember("kind", &LogRow::kind);
    }
    model.RegisterArray<std::vector<LogRow>>();
    // std::vector<Rml::String> is registered by the editor (main.cpp).

    model.Bind("st_talks", &m_talks_);
    model.Bind("st_speakers", &m_speakers_);
    model.Bind("st_rules", &m_rules_);
    model.Bind("st_lines", &m_lines_);
    model.Bind("st_title", &m_title_);
    model.Bind("st_count", &m_count_);
    model.Bind("st_edit_text", &m_edit_text_);
    model.Bind("st_edit_rows", &m_edit_rows_);
    model.BindFunc("st_editing", [this](Rml::Variant& v) { v = Rml::String(edit_what_ == "cond" || edit_what_ == "act" ? edit_what_ : ""); });
    model.Bind("st_menu", &m_menu_);
    model.Bind("st_menu_title", &m_menu_title_);
    model.Bind("st_menu_items", &m_menu_items_);
    model.Bind("st_menu_x", &m_menu_x_);
    model.Bind("st_menu_y", &m_menu_y_);
    model.Bind("st_play_on", &m_play_on_);
    model.Bind("st_play_who", &m_play_who_);
    model.Bind("st_play_color", &m_play_color_);
    model.Bind("st_play_text", &m_play_text_);
    model.Bind("st_play_choices", &m_play_choices_);
    model.Bind("st_play_asks", &m_play_asks_);
    model.Bind("st_play_next", &m_play_next_);
    model.Bind("st_ask_text", &m_ask_text_);
    model.Bind("st_state", &m_state_);
    model.Bind("st_log", &m_log_);
    model.Bind("st_page_on", &m_page_on_);
    model.Bind("st_page_prev", &m_page_prev_);
    model.Bind("st_page_next", &m_page_next_);
    model.Bind("st_page_text", &m_page_text_);
    model.Bind("st_import_text", &m_import_text_);
    model.BindFunc("st_importing", [this](Rml::Variant& v) { v = importing(); });

    auto on = [&model](const char* name, auto fn) {
        model.BindEventCallback(name, [fn](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& a) { fn(ev, a); });
    };
    on("st_open", [this](Rml::Event&, const Rml::VariantList& a) { open(arg_str(a, 0)); });
    on("st_story", [this](Rml::Event&, const Rml::VariantList& a) {
        const std::string story = arg_str(a, 0);
        if (!stories_open_.erase(story)) stories_open_.insert(story);
        dirty_ = true;
    });
    on("st_scene", [this](Rml::Event&, const Rml::VariantList& a) { jump(arg_str(a, 0)); });
    on("st_page", [this](Rml::Event&, const Rml::VariantList& a) { page(arg_int(a, 0, 1)); });
    on("st_import", [this](Rml::Event&, const Rml::VariantList&) {
        if (!window || importing()) return;
        auto done = [](void* self, const char* const* list, int) {
            auto* ed = static_cast<StoryEditor*>(self);
            if (!list || !*list) return;
            std::lock_guard lock(ed->import_mutex_);
            ed->import_picked_.push_back(utf8_path(*list));
        };
        SDL_ShowOpenFolderDialog(done, this, window, nullptr, false);
    });
    on("st_new_talk", [this](Rml::Event&, const Rml::VariantList&) { new_talk(); });
    on("st_edit", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        begin_edit(arg_str(a, 0), arg_str(a, 1), arg_int(a, 2, -1));
    });
    on("st_stop", [](Rml::Event& ev, const Rml::VariantList&) { ev.StopPropagation(); });
    // A mark: its list; a where-next mark's body jumps there (its arrow opens
    // the list).
    on("st_chip", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        const std::string node = arg_str(a, 0), where = arg_str(a, 1), part = arg_str(a, 3), how = arg_str(a, 5);
        const int index = arg_int(a, 2, -1), mark = arg_int(a, 4, -1);
        if (part == "else") return;
        const game::SourceNode* n = src_.node(node);
        const bool junction = n && n->junction();
        if (part == "stage") {
            begin_edit("stage", node, mark);
            return;
        }
        if (how == "open" && (part == "goto" || part == "next" || part == "call")) {
            std::string go;
            const usize i = static_cast<usize>(std::max(index, 0)), m = static_cast<usize>(std::max(mark, 0));
            if (part == "call" && n) go = n->call;
            else if (where == "start" && i < src_.start.size()) go = src_.start[i].go;
            else if (junction && part == "goto" && m < n->branches.size()) go = n->branches[m].go;
            else if (n && where == "choice" && i < n->choices.size()) go = n->choices[i].go;
            else if (n && where == "topic" && i < n->keywords.size()) go = n->keywords[i].go;
            else if (n) go = n->next;
            jump(go);
            return;
        }
        // A junction's marks carry their branch in mark.
        const bool opened = junction && where == "node" && (part == "if" || part == "goto") ? open_menu("branch", node, mark, part)
                                                                                              : open_menu(where, node, index, part, mark);
        if (opened) place_menu(ev);
    });
    on("st_menu_pick", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        const int i = arg_int(a, 0, -1);
        if (i >= 0) menu_pick(static_cast<usize>(i));
    });
    on("st_menu_close", [this](Rml::Event& ev, const Rml::VariantList&) {
        ev.StopPropagation();
        close_menu();
    });
    on("st_add_line", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        add_line_after(arg_str(a, 0));
    });
    on("st_add_choice", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        add_choice(arg_str(a, 0));
    });
    on("st_add_topic", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        add_topic(arg_str(a, 0));
    });
    on("st_add_scene", [this](Rml::Event& ev, const Rml::VariantList&) {
        ev.StopPropagation();
        add_scene();
    });
    on("st_add_mark", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        // «+ что меняет» / «+ когда» on a line or an answer.
        const std::string node = arg_str(a, 0), where = arg_str(a, 1), part = arg_str(a, 3);
        if (open_menu(where, node, arg_int(a, 2, -1), part, -1)) place_menu(ev);
    });
    on("st_remove_line", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        remove_line(arg_str(a, 0));
    });
    on("st_remove_choice", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        remove_choice(arg_str(a, 0), arg_int(a, 1, -1));
    });
    on("st_remove_topic", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        remove_topic(arg_str(a, 0), arg_int(a, 1, -1));
    });
    on("st_commit", [this](Rml::Event& ev, const Rml::VariantList&) {
        ev.StopPropagation();
        commit_edit();
    });
    on("st_cancel", [this](Rml::Event& ev, const Rml::VariantList&) {
        ev.StopPropagation();
        cancel_edit();
    });
    on("st_play", [this](Rml::Event&, const Rml::VariantList&) { play(); });
    on("st_choose", [this](Rml::Event&, const Rml::VariantList& a) {
        const int i = arg_int(a, 0, -1);
        if (i >= 0) play_choose(static_cast<usize>(i));
    });
    on("st_next", [this](Rml::Event&, const Rml::VariantList&) { play_next(); });
    on("st_ask", [this](Rml::Event& ev, const Rml::VariantList&) {
        // The field's own value: the model may not have it yet.
        std::string typed = m_ask_text_;
        if (Rml::Element* doc = ev.GetCurrentElement()->GetOwnerDocument())
            if (Rml::Element* f = doc->GetElementById("st-ask"))
                if (f->GetTagName() == "input") typed = static_cast<Rml::ElementFormControl*>(f)->GetValue();
        play_ask(typed);
    });
    on("st_state", [this](Rml::Event&, const Rml::VariantList& a) {
        const std::string id = arg_str(a, 0);
        const int dir = arg_int(a, 2, 1);
        if (arg_str(a, 1) == "item") return step_item(id, dir);
        if (arg_str(a, 1) == "var") return step_var(id, dir);
        step_quest(id, dir);
    });
}

void StoryEditor::update(Rml::Context* context) {
    context_ = context;
    if (!flash_.empty() && SDL_GetTicks() > flash_until_) {
        flash_.clear();
        dirty_ = true;
    }
    // Enter in a field keeps what was typed.
    if (editing() && m_edit_text_.find('\n') != Rml::String::npos) commit_edit();
    poll_import();
    if (replay_ && !editing()) {
        replay_ = false;
        play();
    }
    if (dirty_) rebuild();
    if (!context) return;
    Rml::ElementDocument* doc = context->GetDocument(0);
    // Every kind of field is in the page, the one being typed in is shown.
    Rml::Element* field = nullptr;
    if (doc) {
        Rml::ElementList fields;
        doc->QuerySelectorAll(fields, "#st-edit");
        for (Rml::Element* f : fields)
            if (f->IsVisible(true)) field = f;
    }
    if (editing() && focus_edit_ && field) {
        field->Focus();
        if (field->GetTagName() == "textarea" || field->GetTagName() == "input") {
            auto* f = static_cast<Rml::ElementFormControl*>(field);
            const int end = static_cast<int>(f->GetValue().size());
            if (field->GetTagName() == "textarea") static_cast<Rml::ElementFormControlTextArea*>(field)->SetSelectionRange(end, end);
            else static_cast<Rml::ElementFormControlInput*>(field)->SetSelectionRange(end, end);
        }
        focus_edit_ = false;
    } else if (editing() && !focus_edit_ && field && context->GetFocusElement() != field) {
        // Clicked elsewhere: what was typed is kept.
        commit_edit();
    }
    if (!scroll_to_.empty() && doc) {
        if (Rml::Element* line = doc->GetElementById("st-line-" + scroll_to_)) line->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Center, Rml::ScrollAlignment::Nearest, Rml::ScrollBehavior::Smooth));
        scroll_to_.clear();
    }
}

bool StoryEditor::handle_event(const SDL_Event& e) {
    if (e.type != SDL_EVENT_KEY_DOWN) return false;
    if ((e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) && context_)
        if (Rml::Element* f = context_->GetFocusElement(); f && f->GetId() == "st-ask") {
            play_ask(static_cast<Rml::ElementFormControl*>(f)->GetValue());
            static_cast<Rml::ElementFormControl*>(f)->SetValue("");
            return true;
        }
    if (e.key.key == SDLK_ESCAPE) {
        if (editing()) {
            cancel_edit();
            return true;
        }
        if (m_menu_) {
            close_menu();
            return true;
        }
    }
    return false;
}

bool StoryEditor::handle_key(const SDL_KeyboardEvent& k) {
    if (k.key == SDLK_ESCAPE && m_menu_) {
        close_menu();
        return true;
    }
    return false;
}

} // namespace forge::editor_app
