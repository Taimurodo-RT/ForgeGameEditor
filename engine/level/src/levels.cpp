#include "forge/level/levels.h"

#include "forge/core/file.h"
#include "forge/core/path.h"
#include "forge/editor/sources.h"

#include <yyjson.h>

#include <cstdio>
#include <cstdlib>
#include <random>

namespace forge::level {

namespace fs = std::filesystem;

namespace {

std::string in_quotes(std::string_view s) { return "«" + std::string(s) + "»"; }

unsigned characters(std::string_view s) {
    unsigned n = 0;
    for (const char c : s) n += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    return n;
}

LevelList one_level() {
    LevelList list;
    list.levels.push_back({std::string(kFirstLevel), std::string(kFirstLevelName)});
    list.start = std::string(kFirstLevel);
    return list;
}

std::string levels_json(const LevelList& list) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strn(doc, root, "start_level", list.start.data(), list.start.size());
    yyjson_mut_val* levels = yyjson_mut_arr(doc);
    for (const LevelEntry& l : list.levels) {
        yyjson_mut_val* e = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strn(doc, e, "id", l.id.data(), l.id.size());
        yyjson_mut_obj_add_strn(doc, e, "name", l.name.data(), l.name.size());
        yyjson_mut_arr_append(levels, e);
    }
    yyjson_mut_obj_add_val(doc, root, "levels", levels);
    usize len = 0;
    char* text = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY_TWO_SPACES, &len);
    std::string out = text ? std::string(text, len) + "\n" : std::string();
    std::free(text);
    yyjson_mut_doc_free(doc);
    return out;
}

std::string trimmed(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return std::string(s);
}

} // namespace

const LevelEntry* LevelList::find(std::string_view id) const {
    for (const LevelEntry& l : levels)
        if (l.id == id) return &l;
    return nullptr;
}

const LevelEntry& LevelList::start_level() const {
    const LevelEntry* s = find(start);
    return s ? *s : levels.front();
}

bool valid_level_id(std::string_view id) {
    if (id.empty() || id.size() > 40) return false;
    for (const char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}

fs::path level_folder(const fs::path& game, std::string_view id) {
    if (id == kFirstLevel) return game / "level";
    return game / "levels" / utf8_path(id);
}

const LevelEntry* level_of_folder(const fs::path& game, const LevelList& list, const fs::path& folder) {
    const fs::path want = folder.lexically_normal();
    for (const LevelEntry& l : list.levels) {
        const fs::path f = level_folder(game, l.id);
        std::error_code ec;
        if (f.lexically_normal() == want || fs::equivalent(f, folder, ec)) return &l;
    }
    return nullptr;
}

LevelList read_levels(const fs::path& game) {
    const fs::path file = game / kLevelsFile;
    std::error_code ec;
    if (!fs::exists(file, ec) && !ec) return one_level();
    LevelList list = one_level();
    auto broken = [&](const std::string& why) {
        list.broken = true;
        list.problem = std::string(kLevelsFile) + ": " + why;
        return list;
    };
    std::vector<u8> bytes;
    if (!fs::is_regular_file(file, ec) || !read_file(file, bytes)) return broken("файл не читается");
    yyjson_read_err err{};
    yyjson_doc* doc = yyjson_read_opts(reinterpret_cast<char*>(bytes.data()), bytes.size(), 0, nullptr, &err);
    if (!doc) return broken(std::string("файл испорчен: ") + (err.msg ? err.msg : "не JSON") + " (байт " + std::to_string(err.pos) + ")");
    struct Free {
        yyjson_doc* d;
        ~Free() { yyjson_doc_free(d); }
    } free_doc{doc};
    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) return broken("файл испорчен: в нём не объект JSON");
    yyjson_val* levels = yyjson_obj_get(root, "levels");
    if (!yyjson_is_arr(levels)) return broken("«levels» — не список уровней");
    LevelList out;
    out.from_file = true;
    usize i, n;
    yyjson_val* v;
    yyjson_arr_foreach(levels, i, n, v) {
        const std::string at = "уровень " + std::to_string(i + 1);
        yyjson_val* id = yyjson_obj_get(v, "id");
        if (!yyjson_is_obj(v) || !yyjson_is_str(id)) {
            out.notes.push_back(std::string(kLevelsFile) + ": " + at + " пропущен: нет id");
            continue;
        }
        LevelEntry e;
        e.id.assign(yyjson_get_str(id), yyjson_get_len(id));
        if (!valid_level_id(e.id)) {
            out.notes.push_back(std::string(kLevelsFile) + ": " + at + " пропущен: id " + in_quotes(e.id) +
                                " — не буквы a–z, цифры, «_» и «-»");
            continue;
        }
        if (out.find(e.id)) {
            out.notes.push_back(std::string(kLevelsFile) + ": " + at + " пропущен: id " + in_quotes(e.id) + " уже есть");
            continue;
        }
        yyjson_val* name = yyjson_obj_get(v, "name");
        if (yyjson_is_str(name)) e.name = trimmed({yyjson_get_str(name), yyjson_get_len(name)});
        if (e.name.empty()) {
            e.name = e.id;
            out.notes.push_back(std::string(kLevelsFile) + ": у уровня " + in_quotes(e.id) + " нет имени, он назван по id");
        }
        out.levels.push_back(std::move(e));
    }
    if (out.levels.empty()) return broken("в списке нет ни одного уровня");
    yyjson_val* start = yyjson_obj_get(root, "start_level");
    if (yyjson_is_str(start)) out.start.assign(yyjson_get_str(start), yyjson_get_len(start));
    if (!out.find(out.start)) {
        out.notes.push_back(std::string(kLevelsFile) + ": стартового уровня " + in_quotes(out.start) +
                            " нет в списке, стартовый — " + in_quotes(out.levels.front().name));
        out.start = out.levels.front().id;
    }
    return out;
}

bool write_levels(const fs::path& game, const LevelList& list, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (list.broken) return fail(list.problem + "; исправьте или уберите его, иначе список уровней не меняется");
    if (list.levels.empty() || !list.find(list.start)) return fail("в списке уровней нет стартового");
    for (const LevelEntry& l : list.levels)
        if (!valid_level_id(l.id)) return fail("у уровня " + in_quotes(l.name) + " id " + in_quotes(l.id) + " не годится");
    const std::string text = levels_json(list);
    if (text.empty() || !write_file_atomic(game / kLevelsFile, {reinterpret_cast<const u8*>(text.data()), text.size()}))
        return fail("не записался " + path_to_utf8(game / kLevelsFile));
    return true;
}

fs::path start_level_folder(const fs::path& game, std::string* id, std::vector<std::string>* notes) {
    const LevelList list = read_levels(game);
    if (notes) {
        if (list.broken) notes->push_back(list.problem + "; игра начинается с уровня «level»");
        notes->insert(notes->end(), list.notes.begin(), list.notes.end());
    }
    const LevelEntry& s = list.start_level();
    if (id) *id = s.id;
    return level_folder(game, s.id);
}

std::string level_name_problem(const LevelList& list, std::string_view name, std::string_view id) {
    const std::string n = trimmed(name);
    if (n.empty()) return "имя пустое";
    if (characters(n) > kMaxLevelName) return "имя длиннее " + std::to_string(kMaxLevelName) + " знаков";
    for (const char c : n)
        if (static_cast<unsigned char>(c) < 0x20) return "в имени управляющий знак";
    for (const LevelEntry& l : list.levels)
        if (l.id != id && editor::sources::same_name(l.name, n)) return "уровень " + in_quotes(l.name) + " уже есть";
    return {};
}

std::string free_level_name(const LevelList& list) {
    for (unsigned k = 2;; ++k) {
        const std::string name = "Уровень " + std::to_string(k);
        if (level_name_problem(list, name).empty()) return name;
    }
}

std::string new_level_id(const fs::path& game, const LevelList& list) {
    static std::mt19937_64 random{std::random_device{}()};
    for (;;) {
        char b[16];
        std::snprintf(b, sizeof b, "l%08x", static_cast<unsigned>(random() & 0xFFFFFFFFu));
        std::error_code ec;
        if (!list.find(b) && !fs::exists(level_folder(game, b), ec) && !ec) return b;
    }
}

bool add_level(const fs::path& game, LevelList& list, std::string_view name, std::string& id, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (list.broken) return fail(list.problem + "; исправьте или уберите его, иначе список уровней не меняется");
    if (const std::string why = level_name_problem(list, name); !why.empty()) return fail(why);
    const std::string made = new_level_id(game, list);
    const fs::path folder = level_folder(game, made);
    std::error_code ec;
    const bool had_levels = fs::is_directory(folder.parent_path(), ec);
    if (!fs::create_directories(folder, ec) || ec) return fail("не создалась папка " + path_to_utf8(folder));
    LevelList next = list;
    next.from_file = true;
    next.notes.clear();
    next.levels.push_back({made, trimmed(name)});
    std::string why;
    if (!write_levels(game, next, &why)) {
        fs::remove(folder, ec); // empty: made just now
        if (!had_levels) fs::remove(folder.parent_path(), ec);
        return fail(why);
    }
    list = std::move(next);
    id = made;
    return true;
}

bool rename_level(const fs::path& game, LevelList& list, std::string_view id, std::string_view name, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (!list.find(id)) return fail("уровня " + in_quotes(id) + " нет в списке");
    if (const std::string why = level_name_problem(list, name, id); !why.empty()) return fail(why);
    LevelList next = list;
    next.from_file = true;
    next.notes.clear();
    for (LevelEntry& l : next.levels)
        if (l.id == id) l.name = trimmed(name);
    if (!write_levels(game, next, error)) return false;
    list = std::move(next);
    return true;
}

bool set_start_level(const fs::path& game, LevelList& list, std::string_view id, std::string* error) {
    if (!list.find(id)) {
        if (error) *error = "уровня " + in_quotes(id) + " нет в списке";
        return false;
    }
    LevelList next = list;
    next.from_file = true;
    next.notes.clear();
    next.start = std::string(id);
    if (!write_levels(game, next, error)) return false;
    list = std::move(next);
    return true;
}

} // namespace forge::level
