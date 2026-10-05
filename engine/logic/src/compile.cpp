// Links to Luau: one module per template that listens.

#include "forge/logic/logic.h"

#include <algorithm>
#include <map>

namespace forge::logic {

namespace {

std::string quote(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        out += c;
    }
    return out + "\"";
}

// One line of comment text (no line breaks).
std::string comment(std::string_view s) {
    std::string out(s);
    std::replace(out.begin(), out.end(), '\n', ' ');
    return out;
}

const std::string& thing_of(const Link& l, Side s) { return s == Side::A ? l.a : l.b; }
Side other(Side s) { return s == Side::A ? Side::B : Side::A; }

// Lines with the link (index) each one comes from.
struct Writer {
    std::string text;
    std::vector<u32> lines;
    void line(std::string_view s, u32 link = script::SourceMap::kNoNode) {
        text += s;
        text += '\n';
        lines.push_back(link);
    }
};

struct Plan {
    usize index = 0; // in Logic::links
    const Link* link = nullptr;
    const VerbDef* verb = nullptr;
    const Thing* a = nullptr;
    const Thing* b = nullptr;
    Side listener = Side::B;
};

bool fits(std::string_view rule, const Thing& t) {
    if (rule == "hero") return t.id == kHero;
    if (rule == "thing") return t.id != kHero;
    return true;
}

// Checks a link and decides whose copies listen for it.
bool plan(const Link& l, usize index, const Verbs& verbs, const FindThing& things, Plan& p, std::string& problem) {
    p.index = index;
    p.link = &l;
    p.verb = verbs.find(l.verb);
    if (!p.verb) {
        problem = "неизвестное действие «" + l.verb + "»";
        return false;
    }
    p.a = things(l.a);
    p.b = things(l.b);
    if (!p.a || !p.b) {
        problem = "нет такой вещи: «" + (p.a ? l.b : l.a) + "»";
        return false;
    }
    if (!fits(p.verb->a_is, *p.a) || !fits(p.verb->b_is, *p.b)) {
        const bool a_wrong = !fits(p.verb->a_is, *p.a);
        const std::string& rule = a_wrong ? p.verb->a_is : p.verb->b_is;
        problem = "«" + p.verb->name + "»: " + (a_wrong ? "первой" : "второй") + " вещью может быть только " +
                  (rule == "hero" ? "герой" : "вещь, а не герой");
        return false;
    }
    if (l.a == kHero && l.b == kHero) {
        problem = "герой связан сам с собой";
        return false;
    }
    if (p.verb->always) {
        if (l.a == kHero) {
            problem = "«" + p.verb->name + "» у героя не бывает: его ведёт игрок";
            return false;
        }
        p.listener = Side::A;
        return true;
    }
    // The hero's side cannot listen: then the other thing does.
    p.listener = thing_of(l, p.verb->touch) == kHero ? other(p.verb->touch) : p.verb->touch;
    return true;
}

// The entity of a side, seen from the listening copy.
std::string entity(const Plan& p, Side s) {
    if (s == p.listener) return "self";
    const std::string& t = thing_of(*p.link, s);
    if (t == kHero) return "hero";
    return "logic.nearest(" + quote(t) + ", self, 32)";
}

// What the link does, from its check to its end (in: the indent).
void emit_body(Writer& w, const Plan& p, const std::string& in, bool fired) {
    const Link& l = *p.link;
    const VerbDef& v = *p.verb;
    const u32 at = static_cast<u32>(p.index);
    std::string cond = "target";
    bool can_fail = false;
    if (!v.needs.empty()) {
        const std::string& needed = thing_of(l, v.needs == "a" ? Side::A : Side::B);
        if (needed != kHero) {
            cond += " and logic.has(hero, " + quote(needed) + ")";
            can_fail = true;
        }
    }
    if (l.night) {
        cond += " and logic.night()";
        can_fail = true;
    }
    w.line(in + "if " + cond + " then", at);
    std::string body = in + "  ";
    if (l.once) {
        w.line(body + "if logic.first(self, " + std::to_string(l.id) + ") then", at);
        body += "  ";
    }
    if (fired) w.line(body + "logic.fired(" + std::to_string(l.id) + ")", at);
    w.line(body + "logic.act(" + quote(v.action) + ", target, " + quote(thing_of(l, v.target)) + ", " +
               entity(p, other(v.target)) + ", hero)",
           at);
    if (l.sound && !v.sound.empty()) w.line(body + "logic.sound(self, " + quote(v.sound) + ")", at);
    if (l.once) w.line(in + "  end", at);
    if (l.hint && can_fail && !v.fail.empty()) {
        w.line(in + "else", at);
        w.line(in + "  logic.hint(hero, " + quote(fill(v.fail, *p.a, *p.b)) + ")", at);
    }
    w.line(in + "end", at);
}

void emit(Writer& w, const Plan& p, const char* indent) {
    const Link& l = *p.link;
    const u32 at = static_cast<u32>(p.index);
    const std::string in(indent);
    w.line(in + "-- " + comment(phrase(l, *p.verb, *p.a, *p.b)) + (l.code.empty() ? "" : " (свой код)"), at);
    w.line(in + "do", at);
    w.line(in + "  local target = " + entity(p, p.verb->target), at);
    if (l.code.empty()) {
        emit_body(w, p, in + "  ", true);
    } else {
        // Its own code: it happened, then the author's lines.
        w.line(in + "  logic.fired(" + std::to_string(l.id) + ")", at);
        usize start = 0;
        while (start <= l.code.size()) {
            usize end = l.code.find('\n', start);
            if (end == std::string::npos) end = l.code.size();
            std::string_view line(l.code.data() + start, end - start);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (end == l.code.size() && line.empty()) break;
            w.line(in + "  " + std::string(line), at);
            start = end + 1;
        }
    }
    w.line(in + "end", at);
}

struct Built {
    std::map<std::string, std::vector<Plan>> touch, always; // by listening template
    std::vector<Problem> problems;
};

Built build(const Logic& logic, const Verbs& verbs, const FindThing& things) {
    Built b;
    for (usize i = 0; i < logic.links.size(); ++i) {
        const Link& l = logic.links[i];
        Plan p;
        std::string problem;
        if (!plan(l, i, verbs, things, p, problem)) {
            b.problems.push_back({l.id, problem});
            continue;
        }
        (p.verb->always ? b.always : b.touch)[thing_of(l, p.listener)].push_back(p);
    }
    return b;
}

void write_module(Writer& w, const std::string& thing, const Built& b, const FindThing& things) {
    const Thing* t = things(thing);
    w.line("-- Связи «" + comment(t ? t->name : thing) + "». Сделано из режима «Связи»: правки здесь перезапишутся.");
    w.line("local logic = forge.logic");
    w.line("local S = {}");
    if (const auto it = b.always.find(thing); it != b.always.end()) {
        w.line("function S.on_start(self)");
        w.line("  local hero = logic.hero()");
        for (const Plan& p : it->second) emit(w, p, "  ");
        w.line("end");
    }
    if (const auto it = b.touch.find(thing); it != b.touch.end()) {
        w.line("function S.on_enter(self, other)");
        w.line("  if not logic.is_hero(other) then return end");
        w.line("  local hero = other");
        for (const Plan& p : it->second) emit(w, p, "  ");
        w.line("end");
    }
    w.line("return S");
}

std::vector<std::string> listeners(const Built& b) {
    std::vector<std::string> out;
    for (const auto& [t, _] : b.touch) out.push_back(t);
    for (const auto& [t, _] : b.always)
        if (!b.touch.contains(t)) out.push_back(t);
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

std::string module_name(std::string_view thing) { return "logic:" + std::string(thing); }

const Module* Compiled::find(std::string_view thing) const {
    for (const Module& m : modules)
        if (m.thing == thing) return &m;
    return nullptr;
}

Compiled compile(const Logic& logic, const Verbs& verbs, const FindThing& things) {
    const Built b = build(logic, verbs, things);
    Compiled out;
    out.problems = b.problems;
    for (const std::string& thing : listeners(b)) {
        Writer w;
        write_module(w, thing, b, things);
        Module m;
        m.thing = thing;
        m.name = module_name(thing);
        m.source = std::move(w.text);
        m.map.line_node = std::move(w.lines);
        m.touch = b.touch.contains(thing);
        out.modules.push_back(std::move(m));
    }
    return out;
}

std::string default_code(const Link& link, const Verbs& verbs, const FindThing& things) {
    Link plain = link;
    plain.code.clear();
    Plan p;
    std::string problem;
    if (!plan(plain, 0, verbs, things, p, problem)) return "-- " + comment(problem) + "\n";
    Writer w;
    emit_body(w, p, "", false);
    return w.text;
}

usize code_lines(std::string_view code) {
    usize n = 0;
    for (char c : code) n += c == '\n';
    return n + (!code.empty() && code.back() != '\n');
}

std::string listing(const Logic& logic, const Verbs& verbs, const FindThing& things, script::SourceMap* map) {
    const Built b = build(logic, verbs, things);
    Writer w;
    bool first = true;
    for (const std::string& thing : listeners(b)) {
        if (!first) w.line("");
        first = false;
        write_module(w, thing, b, things);
    }
    for (const Problem& p : b.problems) w.line("-- не работает: " + comment(p.text));
    if (map) map->line_node = w.lines;
    return w.text;
}

} // namespace forge::logic
