// Names in Russian: the forms of a thing's name and the phrases of links.

#include "forge/logic/logic.h"

#include "forge/objects/library.h"

#include <algorithm>
#include <array>

namespace forge::logic {

namespace {

bool ends(std::string_view s, std::string_view tail) { return s.size() >= tail.size() && s.substr(s.size() - tail.size()) == tail; }

// The last letter (UTF-8) and the word without it.
std::string_view last(std::string_view w) {
    if (w.empty()) return {};
    usize i = w.size() - 1;
    while (i > 0 && (static_cast<u8>(w[i]) & 0xC0) == 0x80) --i;
    return w.substr(i);
}
std::string_view chop(std::string_view w, usize letters = 1) {
    for (usize k = 0; k < letters && !w.empty(); ++k) w.remove_suffix(last(w).size());
    return w;
}
bool one_of(std::string_view letter, std::initializer_list<std::string_view> set) {
    for (std::string_view s : set)
        if (letter == s) return true;
    return false;
}
// г к х ж ш ч щ: after these, ы becomes и.
bool velar_or_hush(std::string_view letter) { return one_of(letter, {"г", "к", "х", "ж", "ш", "ч", "щ", "Г", "К", "Х", "Ж", "Ш", "Ч", "Щ"}); }
bool hush(std::string_view letter) { return one_of(letter, {"ж", "ш", "ч", "щ", "ц"}); }
bool vowel(std::string_view letter) {
    return one_of(letter, {"а", "е", "ё", "и", "о", "у", "ы", "э", "ю", "я", "А", "Е", "Ё", "И", "О", "У", "Ы", "Э", "Ю", "Я"});
}

struct Five {
    std::string acc, dat, ins, gen;
};

Five noun_forms(std::string_view w, bool animate, bool plural) {
    const std::string s(w);
    auto with = [&](std::string_view base, std::string_view a, std::string_view d, std::string_view i, std::string_view g) {
        Five f{std::string(base) + std::string(a), std::string(base) + std::string(d), std::string(base) + std::string(i),
               std::string(base) + std::string(g)};
        return f;
    };
    if (plural) {
        if (ends(w, "ы") || ends(w, "и")) {
            const std::string_view base = chop(w);
            std::string gen;
            // A few common words lose their ending («монет»); the rest take «-ов».
            static constexpr std::array<std::string_view, 6> bare = {"онеты", "ягоды", "звёзды", "деньги", "руды", "двери"};
            bool is_bare = false;
            for (std::string_view b : bare)
                if (ends(w, b)) is_bare = true;
            if (ends(w, "деньги")) gen = std::string(chop(w, 3)) + "ег";
            else if (ends(w, "двери")) gen = std::string(base) + "ей";
            else if (is_bare) gen = std::string(base);
            else if (hush(last(base))) gen = std::string(base) + "ей";
            else gen = std::string(base) + "ов";
            Five f{animate ? gen : s, std::string(base) + "ам", std::string(base) + "ами", gen};
            if (ends(w, "двери")) f.ins = std::string(base) + "ями", f.dat = std::string(base) + "ям";
            return f;
        }
        return {s, s, s, s};
    }
    const std::string_view l = last(w);
    if (l == "а") {
        const std::string_view base = chop(w);
        return with(base, "у", "е", hush(last(base)) ? "ей" : "ой", velar_or_hush(last(base)) ? "и" : "ы");
    }
    if (l == "я") {
        const std::string_view base = chop(w);
        if (ends(w, "ия")) return with(base, "ю", "и", "ей", "и");
        return with(base, "ю", "е", "ей", "и");
    }
    if (l == "о") {
        const std::string_view base = chop(w);
        return {s, std::string(base) + "у", std::string(base) + "ом", std::string(base) + "а"};
    }
    if (l == "е" || l == "ё") {
        const std::string_view base = chop(w);
        return {s, std::string(base) + "ю", std::string(base) + "ем", std::string(base) + "я"};
    }
    if (l == "ь") {
        const std::string_view base = chop(w);
        static constexpr std::array<std::string_view, 10> masculine = {"тель", "арь", "ярь", "ень", "онь", "гвоздь", "дождь", "руль", "уголь", "камень"};
        bool masc = false;
        for (std::string_view m : masculine)
            if (ends(w, m)) masc = true;
        if (ends(w, "камень")) return with(chop(w, 3), animate ? "ня" : "ень", "ню", "нем", "ня");
        if (masc) {
            const std::string gen = std::string(base) + "я";
            return {animate ? gen : s, std::string(base) + "ю", std::string(base) + "ем", gen};
        }
        return {s, std::string(base) + "и", std::string(base) + "ью", std::string(base) + "и"};
    }
    if (l == "й") {
        const std::string_view base = chop(w);
        const std::string gen = std::string(base) + "я";
        return {animate ? gen : s, std::string(base) + "ю", std::string(base) + "ем", gen};
    }
    if (vowel(l)) return {s, s, s, s}; // «Кофе», «Пианино», «Рагу»: do not change
    // A consonant: masculine. «Зверёк» → «Зверька», «Замок» → «Замка».
    std::string base = s;
    if (w.size() > 8 && (ends(w, "ёк") || ends(w, "ек"))) base = std::string(chop(w, 2)) + "ьк";
    else if (w.size() > 8 && ends(w, "ок")) base = std::string(chop(w, 2)) + "к";
    const std::string gen = base + "а";
    return {animate ? gen : s, base + "у", base + "ом", gen};
}

// A leading adjective ("Старый", "Железная"): its forms, or false.
bool adjective_forms(std::string_view w, bool animate, Five& f) {
    const std::string_view base = chop(w, 2);
    const std::string b(base);
    const bool soft_base = !velar_or_hush(last(base));
    if (ends(w, "ый") || ends(w, "ой") || ends(w, "ий")) {
        const bool soft = ends(w, "ий") && soft_base;
        const std::string gen = b + (soft ? "его" : "ого");
        f = {animate ? gen : std::string(w), b + (soft ? "ему" : "ому"), b + (velar_or_hush(last(base)) || ends(w, "ий") ? "им" : "ым"), gen};
        return true;
    }
    if (ends(w, "ая")) {
        f = {b + "ую", b + "ой", b + "ой", b + "ой"};
        return true;
    }
    if (ends(w, "яя")) {
        f = {b + "юю", b + "ей", b + "ей", b + "ей"};
        return true;
    }
    if (ends(w, "ое") || ends(w, "ее")) {
        const bool soft = ends(w, "ее");
        f = {std::string(w), b + (soft ? "ему" : "ому"), b + (soft ? "им" : "ым"), b + (soft ? "его" : "ого")};
        return true;
    }
    if (ends(w, "ые") || ends(w, "ие")) {
        const bool i = ends(w, "ие");
        const std::string gen = b + (i ? "их" : "ых");
        f = {animate ? gen : std::string(w), b + (i ? "им" : "ым"), b + (i ? "ими" : "ыми"), gen};
        return true;
    }
    return false;
}

std::vector<std::string_view> words(std::string_view s) {
    std::vector<std::string_view> out;
    usize i = 0;
    while (i < s.size()) {
        const usize j = s.find(' ', i);
        out.push_back(s.substr(i, j == std::string_view::npos ? std::string_view::npos : j - i));
        if (j == std::string_view::npos) break;
        i = j + 1;
    }
    return out;
}

std::string join(const std::vector<std::string>& parts) {
    std::string out;
    for (const std::string& p : parts) {
        if (!out.empty()) out += ' ';
        out += p;
    }
    return out;
}

} // namespace

const std::string& Forms::get(std::string_view form) const {
    if (form == "acc") return acc;
    if (form == "dat") return dat;
    if (form == "ins") return ins;
    if (form == "gen") return gen;
    return nom;
}

bool looks_plural(std::string_view name) {
    const std::vector<std::string_view> ws = words(name);
    if (ws.empty()) return false;
    std::string_view w = ws[0];
    // A leading adjective tells: «Старые доски».
    if (ws.size() > 1 && (ends(w, "ые") || ends(w, "ие"))) return true;
    if (ws.size() > 1 && (ends(w, "ый") || ends(w, "ий") || ends(w, "ой") || ends(w, "ая") || ends(w, "яя") || ends(w, "ое")))
        return false;
    if (ends(w, "ий") || ends(w, "ый")) return false;
    return ends(w, "ы") || (ends(w, "и") && w.size() > 4);
}

Forms decline(std::string_view name, bool animate) {
    Forms f;
    f.nom = std::string(name);
    std::vector<std::string_view> ws = words(name);
    if (ws.empty()) return f;
    const bool plural = looks_plural(name);
    std::vector<std::string> acc, dat, ins, gen;
    usize changed = 0;
    Five adj;
    if (ws.size() > 1 && adjective_forms(ws[0], animate, adj)) {
        const Five n = noun_forms(ws[1], animate, plural);
        acc = {adj.acc, n.acc};
        dat = {adj.dat, n.dat};
        ins = {adj.ins, n.ins};
        gen = {adj.gen, n.gen};
        changed = 2;
    } else {
        const Five n = noun_forms(ws[0], animate, plural);
        acc = {n.acc};
        dat = {n.dat};
        ins = {n.ins};
        gen = {n.gen};
        changed = 1;
    }
    for (usize i = changed; i < ws.size(); ++i) {
        const std::string rest(ws[i]);
        acc.push_back(rest), dat.push_back(rest), ins.push_back(rest), gen.push_back(rest);
    }
    f.acc = join(acc);
    f.dat = join(dat);
    f.ins = join(ins);
    f.gen = join(gen);
    return f;
}

Thing hero_thing() {
    Thing t;
    t.id = std::string(kHero);
    t.name = "Герой";
    t.animate = true;
    t.forms = {"герой", "героя", "герою", "героем", "героя"};
    return t;
}

bool is_area(std::string_view thing) { return thing.starts_with(kAreaPrefix); }

Thing area_thing(std::string_view id, std::string_view name) {
    Thing t;
    t.id = std::string(id);
    t.name = std::string(name);
    t.area = true;
    t.plural = looks_plural(name);
    t.forms = decline(name, false);
    return t;
}

Thing thing_of(const objects::Library& library, const objects::Template& t) {
    Thing th;
    th.id = t.id;
    th.name = t.name;
    th.animate = library.has_block(t, "villager") || library.has_block(t, "control");
    th.plural = looks_plural(t.name);
    th.forms = decline(t.name, th.animate);
    for (const objects::BlockDef* b : library.blocks_of(t)) th.blocks.push_back(b->id);
    return th;
}

std::string fill(std::string_view text, const Thing& a, const Thing& b) {
    std::string out;
    usize i = 0;
    while (i < text.size()) {
        if (text[i] != '{') {
            out += text[i++];
            continue;
        }
        const usize j = text.find('}', i);
        if (j == std::string_view::npos) {
            out += text.substr(i);
            break;
        }
        const std::string_view key = text.substr(i + 1, j - i - 1);
        const Thing* t = key.starts_with("a") ? &a : key.starts_with("b") ? &b : nullptr;
        if (!t) {
            out += text.substr(i, j - i + 1);
        } else {
            const usize colon = key.find(':');
            out += colon == std::string_view::npos ? t->forms.nom : t->forms.get(key.substr(colon + 1));
        }
        i = j + 1;
    }
    return out;
}

std::string phrase(const Link& link, const VerbDef& verb, const Thing& a, const Thing& b) {
    (void)link;
    return a.name + " " + (a.plural ? verb.plural : verb.name) + " " + b.forms.get(verb.object_case);
}

std::string meaning(const Link& link, const VerbDef& verb, const Thing& a, const Thing& b) {
    if (!link.code.empty()) return "Вместо обычного действия работает свой код (режим «Код»).";
    if (own_scheme(link, verb, a, b)) return "Связь уточнена в режиме «Схема»: что происходит, решают её ноды.";
    std::string s = fill(verb.about, a, b);
    std::vector<std::string> extra;
    if (link.night) extra.push_back("только ночью");
    if (link.once) extra.push_back("только один раз");
    if (link.sound && !verb.sound.empty()) extra.push_back("со звуком");
    if (link.hint && !verb.fail.empty()) extra.push_back("иначе подсказка «" + fill(verb.fail, a, b) + "»");
    if (!extra.empty()) {
        s += " Уточнено: ";
        for (usize i = 0; i < extra.size(); ++i) s += (i ? ", " : "") + extra[i];
        s += ".";
    }
    return s;
}

namespace {

bool side_fits(std::string_view rule, const std::vector<std::string>& has, const Thing& t) {
    if (rule == "hero" && t.id != kHero) return false;
    if (rule == "thing" && (t.id == kHero || t.area)) return false;
    if (rule == "area" && !t.area) return false;
    if (has.empty()) return true;
    for (const std::string& b : has)
        if (std::find(t.blocks.begin(), t.blocks.end(), b) != t.blocks.end()) return true;
    return false;
}

std::string quoted(const Thing& t) { return "«" + t.name + "»"; }

} // namespace

bool suits(const VerbDef& verb, const Thing& a, const Thing& b) {
    if (a.id == b.id) return false;
    if (verb.always && a.id == kHero) return false;
    return side_fits(verb.a_is, verb.a_has, a) && side_fits(verb.b_is, verb.b_has, b);
}

std::vector<Refine> refinements(const Link& link, const VerbDef& verb, const Thing& a, const Thing& b) {
    std::vector<Refine> out;
    if (!link.code.empty() || own_scheme(link, verb, a, b)) return out; // its own code or scheme decides everything
    out.push_back({"night", "Только ночью", "связь срабатывает, только когда в игре ночь", link.night});
    out.push_back({"once", "Только один раз", a.area || b.area ? "один раз за игру" : "у каждой копии вещи — один раз за игру", link.once});
    if (!verb.sound.empty()) out.push_back({"sound", "Со звуком", "обычный звук игры для этого действия", link.sound});
    if (!verb.fail.empty() && (!verb.needs.empty() || link.night))
        out.push_back({"hint", "Подсказка, если не вышло", "«" + fill(verb.fail, a, b) + "»", link.hint});
    return out;
}

std::vector<Step> steps(const Link& link, const VerbDef& verb, const Thing& a, const Thing& b) {
    std::vector<Step> out;
    if (verb.always) {
        out.push_back({"when", "all_inclusive", "Всё время, пока " + quoted(a) + " есть на уровне", ""});
    } else {
        const Thing& touched = verb.touch == Side::A ? (a.id == kHero ? b : a) : (b.id == kHero ? a : b);
        if (touched.area) out.push_back({"when", "login", "Когда герой входит в " + touched.forms.get("acc"), ""});
        else out.push_back({"when", "bolt", "Когда герой касается " + touched.forms.get("gen"), ""});
    }
    if (!link.code.empty()) {
        const usize n = code_lines(link.code);
        out.push_back({"then", "code", "Свой код: " + std::to_string(n) + " " + (n % 10 == 1 && n % 100 != 11 ? "строка" : n % 10 >= 2 && n % 10 <= 4 && (n % 100 < 12 || n % 100 > 14) ? "строки" : "строк"), "code"});
        return out;
    }
    if (own_scheme(link, verb, a, b)) {
        const usize n = scheme_nodes(link);
        out.push_back({"then", "account_tree", "Уточнено в Схеме: " + std::to_string(n) + " " + (n % 10 == 1 && n % 100 != 11 ? "нода" : n % 10 >= 2 && n % 10 <= 4 && (n % 100 < 12 || n % 100 > 14) ? "ноды" : "нод"), "graph"});
        return out;
    }
    bool checks = false;
    if (link.night) {
        out.push_back({"if", "dark_mode", "Если сейчас ночь", "night"});
        checks = true;
    }
    if (!verb.needs.empty()) {
        const Thing& need = verb.needs == "a" ? a : b;
        out.push_back({"if", "inventory_2", "Если у героя есть " + quoted(need), ""});
        checks = true;
    }
    if (link.once) out.push_back({"if", "looks_one", "Если здесь это ещё не случалось", "once"});
    const std::string action = verb.step.empty() ? fill(verb.about, a, b) : fill(verb.step, a, b);
    out.push_back({"then", verb.icon.empty() ? "arrow_forward" : verb.icon, action, ""});
    if (link.sound && !verb.sound.empty()) out.push_back({"then", "volume_up", "Обычный звук действия", "sound"});
    if (checks && link.hint && !verb.fail.empty())
        out.push_back({"else", "info", "Подсказка над героем: «" + fill(verb.fail, a, b) + "»", "hint"});
    return out;
}

} // namespace forge::logic
