#include "forge/level/areas.h"

#include "forge/core/file.h"
#include "forge/core/path.h"

#include <yyjson.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace forge::level {

namespace fs = std::filesystem;

namespace {

usize characters(std::string_view s) {
    usize n = 0;
    for (const char c : s) n += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    return n;
}

std::string in_quotes(std::string_view s) { return "«" + std::string(s) + "»"; }

} // namespace

const Area* LevelAreas::find(u64 id) const {
    for (const Area& a : areas)
        if (a.id == id) return &a;
    return nullptr;
}

Area* LevelAreas::find(u64 id) {
    for (Area& a : areas)
        if (a.id == id) return &a;
    return nullptr;
}

std::string area_id_text(u64 id) {
    char text[20];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(id));
    return text;
}

bool parse_area_id(std::string_view text, u64& id) {
    if (text.empty() || text.size() > 16) return false;
    u64 v = 0;
    for (const char c : text) {
        u64 d;
        if (c >= '0' && c <= '9') d = static_cast<u64>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<u64>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = static_cast<u64>(c - 'A' + 10);
        else return false;
        v = v << 4 | d;
    }
    if (v == 0) return false;
    id = v;
    return true;
}

std::string clean_area_name(std::string_view text) {
    auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!text.empty() && space(text.front())) text.remove_prefix(1);
    while (!text.empty() && space(text.back())) text.remove_suffix(1);
    return std::string(text);
}

bool valid_music_name(std::string_view name) {
    if (name.empty() || name.size() > 200 || name == "." || name == "..") return false;
    for (const char c : name)
        if (c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 0x20) return false;
    return true;
}

std::string area_problem(const Area& a) {
    if (a.id == 0) return "нет id";
    if (clean_area_name(a.name).empty()) return "нет имени";
    if (clean_area_name(a.name) != a.name) return "пробелы по краям имени";
    if (characters(a.name) > kMaxAreaName) return "имя длиннее " + std::to_string(kMaxAreaName) + " знаков";
    for (const char c : a.name)
        if (static_cast<unsigned char>(c) < 0x20) return "в имени перевод строки или другой служебный знак";
    if (a.x1 <= a.x0 || a.y1 <= a.y0) return "пустой прямоугольник: правый край должен быть правее левого, нижний ниже верхнего";
    if (static_cast<i64>(a.x1) - a.x0 > kMaxAreaSide || static_cast<i64>(a.y1) - a.y0 > kMaxAreaSide)
        return "больше " + std::to_string(kMaxAreaSide) + " клеток в сторону";
    if (std::abs(static_cast<f64>(a.x0)) > kMaxSpawn || std::abs(static_cast<f64>(a.y0)) > kMaxSpawn ||
        std::abs(static_cast<f64>(a.x1)) > kMaxSpawn || std::abs(static_cast<f64>(a.y1)) > kMaxSpawn)
        return "слишком далеко от начала мира";
    if (!a.music.empty() && !valid_music_name(a.music)) return "музыка " + in_quotes(a.music) + " — не имя файла из звуков игры";
    return {};
}

bool load_areas(const fs::path& folder, LevelAreas& out, bool* found, std::string* error) {
    if (found) *found = false;
    const fs::path file = folder / kAreasFile;
    std::error_code ec;
    if (!fs::exists(file, ec)) return true;
    if (found) *found = true;
    auto fail = [&](const std::string& why) {
        if (error) *error = std::string(kAreasFile) + ": " + why;
        return false;
    };
    std::vector<u8> bytes;
    if (!fs::is_regular_file(file, ec) || !read_file(file, bytes)) return fail("файл не читается");
    yyjson_read_err err{};
    yyjson_doc* doc = yyjson_read_opts(reinterpret_cast<char*>(bytes.data()), bytes.size(), 0, nullptr, &err);
    if (!doc) return fail(std::string("файл испорчен: ") + (err.msg ? err.msg : "не JSON") + " (байт " + std::to_string(err.pos) + ")");
    struct Free {
        yyjson_doc* d;
        ~Free() { yyjson_doc_free(d); }
    } free_doc{doc};
    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) return fail("файл испорчен: в нём не объект JSON");
    LevelAreas a;
    yyjson_val* list = yyjson_obj_get(root, "areas");
    if (list && !yyjson_is_arr(list)) return fail("«areas» — не список зон");
    if (yyjson_arr_size(list) > kMaxAreas) return fail("зон больше " + std::to_string(kMaxAreas));
    usize i, n;
    yyjson_val* v;
    yyjson_arr_foreach(list, i, n, v) {
        const std::string at = "зона " + std::to_string(i + 1);
        if (!yyjson_is_obj(v)) return fail(at + ": не объект");
        Area z;
        yyjson_val* id = yyjson_obj_get(v, "id");
        if (!yyjson_is_str(id) || !parse_area_id({yyjson_get_str(id), yyjson_get_len(id)}, z.id))
            return fail(at + ": id — не 16 шестнадцатеричных цифр");
        yyjson_val* name = yyjson_obj_get(v, "name");
        if (!yyjson_is_str(name)) return fail(at + ": имя — не строка");
        z.name.assign(yyjson_get_str(name), yyjson_get_len(name));
        const std::string who = at + " " + in_quotes(z.name);
        i32* coords[] = {&z.x0, &z.y0, &z.x1, &z.y1};
        const char* keys[] = {"x0", "y0", "x1", "y1"};
        for (int k = 0; k < 4; ++k) {
            yyjson_val* c = yyjson_obj_get(v, keys[k]);
            if (!yyjson_is_int(c)) return fail(who + ": " + keys[k] + " — не целое число");
            const i64 value = yyjson_get_sint(c);
            if (yyjson_is_uint(c) && yyjson_get_uint(c) > static_cast<u64>(INT32_MAX)) return fail(who + ": " + keys[k] + " слишком велико");
            if (value < INT32_MIN || value > INT32_MAX) return fail(who + ": " + keys[k] + " слишком велико");
            *coords[k] = static_cast<i32>(value);
        }
        if (yyjson_val* music = yyjson_obj_get(v, "music"); music && !yyjson_is_null(music)) {
            if (!yyjson_is_str(music)) return fail(who + ": музыка — не строка");
            z.music.assign(yyjson_get_str(music), yyjson_get_len(music));
        }
        if (const std::string why = area_problem(z); !why.empty()) return fail(who + ": " + why);
        if (a.find(z.id)) return fail(who + ": id " + area_id_text(z.id) + " уже есть у другой зоны");
        a.areas.push_back(std::move(z));
    }
    if (yyjson_val* spawn = yyjson_obj_get(root, "spawn"); spawn && !yyjson_is_null(spawn)) {
        yyjson_val* x = yyjson_obj_get(spawn, "x");
        yyjson_val* y = yyjson_obj_get(spawn, "y");
        if (!yyjson_is_obj(spawn) || !yyjson_is_num(x) || !yyjson_is_num(y)) return fail("точка появления: x и y — не числа");
        a.spawn = true;
        a.spawn_x = yyjson_get_num(x);
        a.spawn_y = yyjson_get_num(y);
        if (!std::isfinite(a.spawn_x) || !std::isfinite(a.spawn_y) || std::abs(a.spawn_x) > kMaxSpawn || std::abs(a.spawn_y) > kMaxSpawn)
            return fail("точка появления слишком далеко от начала мира");
    }
    out = std::move(a);
    return true;
}

std::string areas_json(const LevelAreas& a) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_val* list = yyjson_mut_arr(doc);
    for (const Area& z : a.areas) {
        yyjson_mut_val* o = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, o, "id", area_id_text(z.id).c_str());
        yyjson_mut_obj_add_strncpy(doc, o, "name", z.name.data(), z.name.size());
        yyjson_mut_obj_add_sint(doc, o, "x0", z.x0);
        yyjson_mut_obj_add_sint(doc, o, "y0", z.y0);
        yyjson_mut_obj_add_sint(doc, o, "x1", z.x1);
        yyjson_mut_obj_add_sint(doc, o, "y1", z.y1);
        if (!z.music.empty()) yyjson_mut_obj_add_strncpy(doc, o, "music", z.music.data(), z.music.size());
        yyjson_mut_arr_append(list, o);
    }
    yyjson_mut_obj_add_val(doc, root, "areas", list);
    if (a.spawn) {
        yyjson_mut_val* s = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_real(doc, s, "x", a.spawn_x);
        yyjson_mut_obj_add_real(doc, s, "y", a.spawn_y);
        yyjson_mut_obj_add_val(doc, root, "spawn", s);
    }
    usize len = 0;
    char* text = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY_TWO_SPACES, &len);
    std::string out = text ? std::string(text, len) + "\n" : std::string();
    std::free(text);
    yyjson_mut_doc_free(doc);
    return out;
}

bool save_areas(const fs::path& folder, const LevelAreas& a, std::string* error) {
    for (const Area& z : a.areas)
        if (const std::string why = area_problem(z); !why.empty()) {
            if (error) *error = std::string(kAreasFile) + " не записан: зона " + in_quotes(z.name) + ": " + why;
            return false;
        }
    if (a.areas.size() > kMaxAreas) {
        if (error) *error = std::string(kAreasFile) + " не записан: зон больше " + std::to_string(kMaxAreas);
        return false;
    }
    const std::string text = areas_json(a);
    std::error_code ec;
    if (!folder.empty()) fs::create_directories(folder, ec);
    if (text.empty() || folder.empty() ||
        !write_file_atomic(folder / kAreasFile, {reinterpret_cast<const u8*>(text.data()), text.size()})) {
        if (error) *error = "не удалось записать " + path_to_utf8(folder / kAreasFile);
        return false;
    }
    return true;
}

const Area* music_area(const LevelAreas& a, f64 x, f64 y) {
    const Area* best = nullptr;
    for (const Area& z : a.areas)
        if (!z.music.empty() && z.contains(x, y) && (!best || z.size() <= best->size())) best = &z;
    return best;
}

void AreaWatch::settle(const LevelAreas& a, f64 x, f64 y) {
    inside_.clear();
    for (const Area& z : a.areas)
        if (z.contains(x, y)) inside_.push_back(z.id);
}

void AreaWatch::step(const LevelAreas& a, f64 x, f64 y, std::vector<AreaEvent>& out) {
    std::vector<u64> now;
    for (const Area& z : a.areas)
        if (z.contains(x, y)) now.push_back(z.id);
    if (now == inside_) return;
    // Left: in the list order of the areas (one that is gone from the list
    // last), then came in.
    for (const Area& z : a.areas)
        if (inside(z.id) && std::find(now.begin(), now.end(), z.id) == now.end()) out.push_back({z.id, false});
    for (const u64 id : inside_)
        if (!a.find(id)) out.push_back({id, false});
    for (const u64 id : now)
        if (!inside(id)) out.push_back({id, true});
    inside_ = std::move(now);
}

bool AreaWatch::inside(u64 id) const { return std::find(inside_.begin(), inside_.end(), id) != inside_.end(); }

SetAreas::SetAreas(Level& level, LevelAreas before, LevelAreas after, std::string label, std::string merge)
    : level_(level), before_(std::move(before)), after_(std::move(after)), label_(std::move(label)), merge_(std::move(merge)) {}

void SetAreas::apply(editor::Document&) { level_.set_areas(after_); }

void SetAreas::revert(editor::Document&) { level_.set_areas(before_); }

bool SetAreas::try_merge(const editor::Command& next) {
    // The history merges only commands of one merge key: SetAreas ones.
    const auto* n = static_cast<const SetAreas*>(&next);
    if (!n || merge_.empty() || n->merge_ != merge_) return false;
    after_ = n->after_;
    label_ = n->label_;
    return true;
}

} // namespace forge::level
