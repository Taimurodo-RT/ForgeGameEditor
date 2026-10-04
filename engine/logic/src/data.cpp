// verbs.json and logic.json.

#include "forge/logic/logic.h"

#include "forge/core/file.h"

#include <yyjson.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace forge::logic {

namespace {

std::string str(yyjson_val* obj, const char* key, std::string_view fallback = {}) {
    yyjson_val* v = yyjson_obj_get(obj, key);
    const char* s = yyjson_get_str(v);
    return s ? std::string(s, yyjson_get_len(v)) : std::string(fallback);
}

std::vector<std::string> strs(yyjson_val* obj, const char* key) {
    std::vector<std::string> out;
    yyjson_val* list = yyjson_obj_get(obj, key);
    if (const char* s = yyjson_get_str(list)) out.emplace_back(s, yyjson_get_len(list));
    usize i, n;
    yyjson_val* v;
    if (yyjson_is_arr(list))
        yyjson_arr_foreach(list, i, n, v) if (const char* s = yyjson_get_str(v)) out.emplace_back(s, yyjson_get_len(v));
    return out;
}

bool flag(yyjson_val* obj, const char* key) {
    yyjson_val* v = yyjson_obj_get(obj, key);
    return yyjson_is_bool(v) && yyjson_get_bool(v);
}

yyjson_doc* read_doc(std::string_view json, std::string* error) {
    yyjson_read_err err{};
    yyjson_doc* doc = yyjson_read_opts(const_cast<char*>(json.data()), json.size(),
                                       YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_TRAILING_COMMAS, nullptr, &err);
    if (!doc && error) *error = std::string("ошибка в JSON: ") + (err.msg ? err.msg : "?") + " (позиция " + std::to_string(err.pos) + ")";
    return doc;
}

bool read_text(const std::filesystem::path& file, std::string& out) {
    std::vector<u8> bytes;
    if (!read_file(file, bytes)) return false;
    out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

Side side(std::string_view s) { return s == "a" ? Side::A : Side::B; }

} // namespace

// --- verbs ---------------------------------------------------------------

bool Verbs::load(const std::filesystem::path& file, std::string* error) {
    std::string text;
    if (!read_text(file, text)) {
        if (error) *error = "не читается " + file.filename().string();
        return false;
    }
    return parse(text, error);
}

bool Verbs::parse(std::string_view json, std::string* error) {
    yyjson_doc* doc = read_doc(json, error);
    if (!doc) return false;
    yyjson_val* list = yyjson_obj_get(yyjson_doc_get_root(doc), "verbs");
    if (!yyjson_is_arr(list)) {
        yyjson_doc_free(doc);
        if (error) *error = "нет списка \"verbs\"";
        return false;
    }
    std::vector<VerbDef> verbs;
    usize i, n;
    yyjson_val* v;
    yyjson_arr_foreach(list, i, n, v) {
        VerbDef d;
        d.id = str(v, "id");
        d.name = str(v, "name");
        d.plural = str(v, "plural", d.name);
        d.icon = str(v, "icon", "arrow_forward");
        d.object_case = str(v, "case", "acc");
        const std::string when = str(v, "when", "touch");
        d.always = when == "always";
        d.touch = side(str(v, "touch", "b"));
        d.needs = str(v, "needs");
        d.action = str(v, "do");
        d.target = side(str(v, "target", "b"));
        d.sound = str(v, "sound");
        d.fail = str(v, "fail");
        d.about = str(v, "about");
        d.a_is = str(v, "a");
        d.b_is = str(v, "b");
        d.a_has = strs(v, "a_has");
        d.b_has = strs(v, "b_has");
        d.step = str(v, "step");
        if (d.id.empty() || d.name.empty() || d.action.empty()) continue;
        verbs.push_back(std::move(d));
    }
    yyjson_doc_free(doc);
    verbs_ = std::move(verbs);
    return true;
}

const VerbDef* Verbs::find(std::string_view id) const {
    for (const VerbDef& v : verbs_)
        if (v.id == id) return &v;
    return nullptr;
}

// --- ideas ---------------------------------------------------------------

bool Ideas::load(const std::filesystem::path& file, std::string* error) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        ideas_.clear();
        return true;
    }
    std::string text;
    if (!read_text(file, text)) {
        if (error) *error = "не читается " + file.filename().string();
        return false;
    }
    return parse(text, error);
}

bool Ideas::parse(std::string_view json, std::string* error) {
    yyjson_doc* doc = read_doc(json, error);
    if (!doc) return false;
    yyjson_val* list = yyjson_obj_get(yyjson_doc_get_root(doc), "ideas");
    if (!yyjson_is_arr(list)) {
        yyjson_doc_free(doc);
        if (error) *error = "нет списка \"ideas\"";
        return false;
    }
    std::vector<Idea> ideas;
    usize i, n;
    yyjson_val* v;
    yyjson_arr_foreach(list, i, n, v) {
        Idea d;
        d.id = str(v, "id");
        d.name = str(v, "name");
        d.icon = str(v, "icon", "lightbulb");
        d.group = str(v, "group");
        d.about = str(v, "about");
        d.verb = str(v, "verb");
        usize j, m;
        yyjson_val* f;
        yyjson_val* fields = yyjson_obj_get(v, "fields");
        if (yyjson_is_arr(fields))
            yyjson_arr_foreach(fields, j, m, f) d.fields.push_back({side(str(f, "side", "b")), str(f, "label")});
        yyjson_val* r = yyjson_obj_get(v, "refine");
        d.night = flag(r, "night");
        d.once = flag(r, "once");
        d.sound = flag(r, "sound");
        d.hint = flag(r, "hint");
        if (d.id.empty() || d.name.empty() || d.verb.empty()) continue;
        ideas.push_back(std::move(d));
    }
    yyjson_doc_free(doc);
    ideas_ = std::move(ideas);
    return true;
}

const Idea* Ideas::find(std::string_view id) const {
    for (const Idea& d : ideas_)
        if (d.id == id) return &d;
    return nullptr;
}

const Idea* Ideas::of_verb(std::string_view verb) const {
    for (const Idea& d : ideas_)
        if (d.verb == verb) return &d;
    return nullptr;
}

// --- links ---------------------------------------------------------------

bool Logic::load(const std::filesystem::path& file, std::string* error) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        *this = {};
        return true;
    }
    std::string text;
    if (!read_text(file, text)) {
        if (error) *error = "не читается " + file.filename().string();
        return false;
    }
    return parse(text, error);
}

bool Logic::parse(std::string_view json, std::string* error) {
    yyjson_doc* doc = read_doc(json, error);
    if (!doc) return false;
    yyjson_val* root = yyjson_doc_get_root(doc);
    Logic out;
    usize i, n;
    yyjson_val* v;
    yyjson_arr_foreach(yyjson_obj_get(root, "links"), i, n, v) {
        Link l;
        l.id = static_cast<u32>(yyjson_get_uint(yyjson_obj_get(v, "id")));
        l.a = str(v, "a");
        l.verb = str(v, "verb");
        l.b = str(v, "b");
        l.night = flag(v, "night");
        l.once = flag(v, "once");
        l.sound = flag(v, "sound");
        l.hint = flag(v, "hint");
        if (l.a.empty() || l.b.empty() || l.verb.empty()) continue;
        if (l.id == 0 || out.find(l.id)) l.id = 0; // given one below
        out.links.push_back(std::move(l));
    }
    for (const Link& l : out.links) out.next_id = std::max(out.next_id, l.id + 1);
    for (Link& l : out.links)
        if (l.id == 0) l.id = out.next_id++;
    yyjson_val* spots = yyjson_obj_get(root, "board");
    if (yyjson_is_obj(spots)) {
        yyjson_obj_iter it = yyjson_obj_iter_with(spots);
        while (yyjson_val* key = yyjson_obj_iter_next(&it)) {
            yyjson_val* at = yyjson_obj_iter_get_val(key);
            if (!yyjson_is_arr(at) || yyjson_arr_size(at) < 2) continue;
            out.board.push_back({std::string(yyjson_get_str(key), yyjson_get_len(key)),
                                 static_cast<f32>(yyjson_get_num(yyjson_arr_get(at, 0))),
                                 static_cast<f32>(yyjson_get_num(yyjson_arr_get(at, 1)))});
        }
    }
    yyjson_doc_free(doc);
    *this = std::move(out);
    return true;
}

std::string Logic::json() const {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_val* list = yyjson_mut_arr(doc);
    for (const Link& l : links) {
        yyjson_mut_val* o = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_uint(doc, o, "id", l.id);
        yyjson_mut_obj_add_strncpy(doc, o, "a", l.a.data(), l.a.size());
        yyjson_mut_obj_add_strncpy(doc, o, "verb", l.verb.data(), l.verb.size());
        yyjson_mut_obj_add_strncpy(doc, o, "b", l.b.data(), l.b.size());
        if (l.night) yyjson_mut_obj_add_bool(doc, o, "night", true);
        if (l.once) yyjson_mut_obj_add_bool(doc, o, "once", true);
        if (l.sound) yyjson_mut_obj_add_bool(doc, o, "sound", true);
        if (l.hint) yyjson_mut_obj_add_bool(doc, o, "hint", true);
        yyjson_mut_arr_append(list, o);
    }
    yyjson_mut_obj_add_val(doc, root, "links", list);
    yyjson_mut_val* board_obj = yyjson_mut_obj(doc);
    for (const Spot& s : board) {
        yyjson_mut_val* at = yyjson_mut_arr(doc);
        yyjson_mut_arr_add_real(doc, at, static_cast<f64>(std::round(s.x)));
        yyjson_mut_arr_add_real(doc, at, static_cast<f64>(std::round(s.y)));
        yyjson_mut_obj_add(board_obj, yyjson_mut_strncpy(doc, s.thing.data(), s.thing.size()), at);
    }
    yyjson_mut_obj_add_val(doc, root, "board", board_obj);
    usize len = 0;
    char* out = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY_TWO_SPACES, &len);
    std::string s = out ? std::string(out, len) + "\n" : std::string();
    std::free(out);
    yyjson_mut_doc_free(doc);
    return s;
}

bool Logic::save(const std::filesystem::path& file, std::string* error) const {
    const std::string text = json();
    if (!write_file_atomic(file, std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()))) {
        if (error) *error = "не записывается " + file.filename().string();
        return false;
    }
    return true;
}

Link* Logic::find(u32 id) {
    for (Link& l : links)
        if (l.id == id) return &l;
    return nullptr;
}

const Link* Logic::find(u32 id) const { return const_cast<Logic*>(this)->find(id); }

u32 Logic::add(Link link) {
    link.id = next_id++;
    links.push_back(std::move(link));
    return links.back().id;
}

bool Logic::remove(u32 id) {
    const auto it = std::find_if(links.begin(), links.end(), [&](const Link& l) { return l.id == id; });
    if (it == links.end()) return false;
    links.erase(it);
    return true;
}

const Spot* Logic::spot(std::string_view thing) const {
    for (const Spot& s : board)
        if (s.thing == thing) return &s;
    return nullptr;
}

void Logic::set_spot(std::string_view thing, f32 x, f32 y) {
    for (Spot& s : board)
        if (s.thing == thing) {
            s.x = x;
            s.y = y;
            return;
        }
    board.push_back({std::string(thing), x, y});
}

} // namespace forge::logic
