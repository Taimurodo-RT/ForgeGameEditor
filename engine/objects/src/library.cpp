#include "forge/objects/library.h"

#include "forge/core/file.h"
#include "forge/core/hash.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/data/json.h"
#include "forge/scene/scene.h"

#include <yyjson.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <random>

FORGE_REFLECT(forge::objects::ObjectRef, 1) {
    t.field("template", &forge::objects::ObjectRef::key).hidden();
    t.field("rev", &forge::objects::ObjectRef::rev).hidden();
    t.field("overrides", &forge::objects::ObjectRef::overrides).hidden();
}

namespace forge::objects {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kExtension = ".object.json";

std::string text_of(yyjson_val* v) {
    const char* s = yyjson_get_str(v);
    return s ? std::string(s, yyjson_get_len(v)) : std::string();
}

// One JSON value written compactly ("10", "\"coins\"", "{...}").
std::string write_value(yyjson_val* v) {
    if (!v) return {};
    usize len = 0;
    char* out = yyjson_val_write(v, 0, &len);
    if (!out) return {};
    std::string s(out, len);
    std::free(out);
    return s;
}

std::string json_text(std::string_view text) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* v = yyjson_mut_strncpy(doc, text.data(), text.size());
    yyjson_mut_doc_set_root(doc, v);
    usize len = 0;
    char* out = yyjson_mut_write(doc, 0, &len);
    std::string s = out ? std::string(out, len) : std::string();
    std::free(out);
    yyjson_mut_doc_free(doc);
    return s;
}

std::vector<std::pair<std::string, std::string>> read_values(yyjson_val* obj) {
    std::vector<std::pair<std::string, std::string>> values;
    if (!yyjson_is_obj(obj)) return values;
    yyjson_obj_iter it = yyjson_obj_iter_with(obj);
    while (yyjson_val* key = yyjson_obj_iter_next(&it)) values.emplace_back(text_of(key), write_value(yyjson_obj_iter_get_val(key)));
    return values;
}

// A component type by its full ("slice::Item") or short ("Item") name.
const reflect::TypeInfo* find_component(std::string_view name) {
    if (const reflect::TypeInfo* t = reflect::find_type(name)) return t;
    for (const reflect::TypeInfo* t : reflect::all_types()) {
        const std::string_view n = t->name;
        if (n.size() > name.size() + 2 && n.ends_with(name) && n.substr(n.size() - name.size() - 2, 2) == "::")
            return t;
    }
    return nullptr;
}

flecs::entity_t component_id(scene::Scene& scene, const reflect::TypeInfo* type) {
    for (const auto& c : scene.saved_components())
        if (c.type == type) return c.id;
    return 0;
}

// The field of a component's JSON (compact), "" when missing.
std::string field_of(const std::string& component_json, std::string_view field) {
    yyjson_doc* doc = yyjson_read(component_json.data(), component_json.size(), 0);
    if (!doc) return {};
    std::string out = write_value(yyjson_obj_getn(yyjson_doc_get_root(doc), field.data(), field.size()));
    yyjson_doc_free(doc);
    return out;
}

// A choice's id for a stored number ("coins" for 1), as JSON.
std::string choice_json(const PropDef& prop, const std::string& field_json) {
    if (prop.choices.empty()) return field_json;
    f64 v = 0;
    if (prop.info && prop.info->type && prop.info->type->kind == reflect::Kind::Enum) {
        // Enums are written by name: find the number behind it.
        yyjson_doc* doc = yyjson_read(field_json.data(), field_json.size(), 0);
        const std::string name = doc ? text_of(yyjson_doc_get_root(doc)) : std::string();
        if (doc) yyjson_doc_free(doc);
        if (const reflect::EnumValue* e = prop.info->type->find_enum(name)) v = static_cast<f64>(e->value);
    } else {
        v = std::strtod(field_json.c_str(), nullptr);
    }
    for (const Choice& c : prop.choices)
        if (std::abs(c.value - v) < 1e-6) return json_text(c.id);
    return field_json;
}

// What a property value (JSON) becomes in its field's JSON.
std::string field_json(const PropDef& prop, const std::string& value) {
    if (prop.choices.empty()) return value;
    yyjson_doc* doc = yyjson_read(value.data(), value.size(), 0);
    const std::string id = doc ? text_of(yyjson_doc_get_root(doc)) : std::string();
    if (doc) yyjson_doc_free(doc);
    for (const Choice& c : prop.choices) {
        if (c.id != id) continue;
        if (prop.info && prop.info->type && prop.info->type->kind == reflect::Kind::Enum) {
            if (const reflect::EnumValue* e = prop.info->type->find_enum(static_cast<i64>(c.value))) return json_text(e->name);
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.17g", c.value);
        return buf;
    }
    return {};
}

std::string to_json_of(const reflect::TypeInfo* type, const void* data) { return data::to_json(type, data, false); }

// A component with these fields changed (fields: a JSON object).
bool merge_into(const reflect::TypeInfo* type, void* object, const std::string& fields) {
    data::LoadReport report;
    return data::from_json(type, object, fields, report);
}

struct Scratch {
    explicit Scratch(const reflect::TypeInfo* t) : type(t), buf((t->size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t) + 1) {
        type->construct(buf.data());
    }
    ~Scratch() { type->destruct(buf.data()); }
    void* data() { return buf.data(); }
    const reflect::TypeInfo* type;
    std::vector<std::max_align_t> buf;
};

std::vector<std::string> split(std::string_view list) {
    std::vector<std::string> out;
    usize start = 0;
    while (start <= list.size()) {
        const usize end = std::min(list.find(',', start), list.size());
        if (end > start) out.emplace_back(list.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

std::string random_id() {
    static std::mt19937_64 rng{std::random_device{}()};
    char buf[24];
    std::snprintf(buf, sizeof(buf), "o%015llx", static_cast<unsigned long long>(rng() & 0xfffffffffffffffull));
    return buf;
}

// A file name from a template's name: characters Windows refuses become "_".
std::string file_stem(std::string_view name) {
    std::string s;
    for (char c : name) s += std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos || (c >= 0 && c < 32) ? '_' : c;
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    return s.empty() ? std::string("объект") : s;
}

} // namespace

// --- small types ------------------------------------------------------------

const PropDef* KindDef::prop(std::string_view prop_id) const {
    for (const PropDef& p : props)
        if (p.id == prop_id) return &p;
    return nullptr;
}

const std::string* Template::value(std::string_view prop) const {
    for (const auto& [k, v] : values)
        if (k == prop) return &v;
    return nullptr;
}

bool ObjectRef::overrides_prop(std::string_view prop) const {
    for (const std::string& p : split(overrides))
        if (p == prop) return true;
    return false;
}

void ObjectRef::set_override(std::string_view prop, bool on) {
    std::vector<std::string> list = split(overrides);
    const auto it = std::find(list.begin(), list.end(), prop);
    if (on == (it != list.end())) return;
    if (on) list.emplace_back(prop);
    else list.erase(it);
    overrides.clear();
    for (const std::string& p : list) overrides += (overrides.empty() ? "" : ",") + p;
}

u32 values_rev(const std::vector<std::pair<std::string, std::string>>& values) {
    u64 h = kFnvOffset;
    for (const auto& [k, v] : values) {
        h = fnv1a(k, h);
        h = fnv1a("=", h);
        h = fnv1a(v, h);
        h = fnv1a(";", h);
    }
    return static_cast<u32>(h ^ (h >> 32)) | 1u; // never 0: 0 is "never applied"
}

// --- files ------------------------------------------------------------------

std::optional<Template> read_template(const fs::path& file, std::string* error) {
    std::vector<u8> bytes;
    if (!read_file(file, bytes)) {
        if (error) *error = "не читается";
        return std::nullopt;
    }
    yyjson_read_err err{};
    yyjson_doc* doc = yyjson_read_opts(reinterpret_cast<char*>(bytes.data()), bytes.size(), YYJSON_READ_ALLOW_COMMENTS |
                                       YYJSON_READ_ALLOW_TRAILING_COMMAS, nullptr, &err);
    if (!doc) {
        if (error) *error = std::string("ошибка JSON: ") + err.msg;
        return std::nullopt;
    }
    yyjson_val* root = yyjson_doc_get_root(doc);
    Template t;
    t.id = text_of(yyjson_obj_get(root, "id"));
    t.name = text_of(yyjson_obj_get(root, "name"));
    t.kind = text_of(yyjson_obj_get(root, "kind"));
    t.about = text_of(yyjson_obj_get(root, "about"));
    t.genre = text_of(yyjson_obj_get(root, "genre"));
    t.values = read_values(yyjson_obj_get(root, "values"));
    yyjson_doc_free(doc);
    if (t.id.empty() || t.kind.empty()) {
        if (error) *error = "нет id или kind";
        return std::nullopt;
    }
    if (t.name.empty()) t.name = t.id;
    t.key = fnv1a(t.id);
    t.rev = values_rev(t.values);
    t.file = file;
    return t;
}

std::string template_json(const Template& t) {
    std::string s = "{\n";
    s += "  \"id\": " + json_text(t.id) + ",\n";
    s += "  \"name\": " + json_text(t.name) + ",\n";
    s += "  \"kind\": " + json_text(t.kind) + ",\n";
    if (!t.genre.empty()) s += "  \"genre\": " + json_text(t.genre) + ",\n";
    if (!t.about.empty()) s += "  \"about\": " + json_text(t.about) + ",\n";
    s += "  \"values\": {";
    for (usize i = 0; i < t.values.size(); ++i)
        s += (i ? ",\n    " : "\n    ") + json_text(t.values[i].first) + ": " + t.values[i].second;
    s += t.values.empty() ? "}\n" : "\n  }\n";
    s += "}\n";
    return s;
}

// --- the library ------------------------------------------------------------

Library::Library() = default;
Library::~Library() = default;

bool Library::load(const fs::path& kinds_file, const fs::path& folder, std::string* error) {
    kinds_.clear();
    templates_.clear();
    folder_ = folder;
    ++version_;
    std::vector<u8> bytes;
    if (!read_file(kinds_file, bytes)) {
        if (error) *error = "нет файла видов объектов " + path_to_utf8(kinds_file);
        return false;
    }
    yyjson_read_err err{};
    yyjson_doc* doc = yyjson_read_opts(reinterpret_cast<char*>(bytes.data()), bytes.size(),
                                       YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_TRAILING_COMMAS, nullptr, &err);
    if (!doc) {
        if (error) *error = path_to_utf8(kinds_file) + ": ошибка JSON на " + std::to_string(err.pos) + ": " + err.msg;
        return false;
    }
    genres_.clear();
    usize i, n;
    yyjson_val* g;
    yyjson_arr_foreach(yyjson_obj_get(yyjson_doc_get_root(doc), "genres"), i, n, g) genres_.push_back(text_of(g));
    yyjson_val* list = yyjson_obj_get(yyjson_doc_get_root(doc), "kinds");
    yyjson_val* k;
    yyjson_arr_foreach(list, i, n, k) {
        KindDef kind;
        kind.id = text_of(yyjson_obj_get(k, "id"));
        kind.name = text_of(yyjson_obj_get(k, "name"));
        kind.group = text_of(yyjson_obj_get(k, "group"));
        kind.icon = text_of(yyjson_obj_get(k, "icon"));
        kind.about = text_of(yyjson_obj_get(k, "about"));
        if (yyjson_val* f = yyjson_obj_get(k, "foot")) kind.foot = yyjson_get_num(f);
        for (auto& [part_name, json] : read_values(yyjson_obj_get(k, "components"))) {
            KindDef::Part part{part_name, json, find_component(part_name)};
            if (!part.type) FORGE_WARN("Вид «%s»: нет компонента %s", kind.name.c_str(), part_name.c_str());
            kind.components.push_back(std::move(part));
        }
        usize pi, pn;
        yyjson_val* p;
        yyjson_arr_foreach(yyjson_obj_get(k, "props"), pi, pn, p) {
            PropDef prop;
            prop.id = text_of(yyjson_obj_get(p, "id"));
            prop.name = text_of(yyjson_obj_get(p, "name"));
            prop.hint = text_of(yyjson_obj_get(p, "hint"));
            const std::string bind = text_of(yyjson_obj_get(p, "bind")); // "Item.count"
            const usize dot = bind.rfind('.');
            prop.component = bind.substr(0, dot == std::string::npos ? 0 : dot);
            prop.field = dot == std::string::npos ? bind : bind.substr(dot + 1);
            prop.advanced = yyjson_get_bool(yyjson_obj_get(p, "advanced"));
            if (yyjson_val* mn = yyjson_obj_get(p, "min")) prop.min = yyjson_get_num(mn), prop.has_range = true;
            if (yyjson_val* mx = yyjson_obj_get(p, "max")) prop.max = yyjson_get_num(mx), prop.has_range = true;
            usize ci, cn;
            yyjson_val* c;
            yyjson_arr_foreach(yyjson_obj_get(p, "choices"), ci, cn, c) {
                prop.choices.push_back({text_of(yyjson_obj_get(c, "id")), text_of(yyjson_obj_get(c, "name")),
                                        yyjson_get_num(yyjson_obj_get(c, "value"))});
            }
            prop.type = find_component(prop.component);
            prop.info = prop.type ? prop.type->find_field(prop.field) : nullptr;
            if (!prop.info) {
                FORGE_WARN("Вид «%s»: свойство «%s» привязано к %s, а такого поля нет", kind.name.c_str(),
                           prop.name.c_str(), bind.c_str());
                continue;
            }
            if (!prop.has_range && prop.info->has_range) {
                prop.has_range = true;
                prop.min = prop.info->min;
                prop.max = prop.info->max;
            }
            kind.props.push_back(std::move(prop));
        }
        yyjson_val* pr;
        yyjson_arr_foreach(yyjson_obj_get(k, "presets"), pi, pn, pr) {
            kind.presets.push_back({text_of(yyjson_obj_get(pr, "name")), text_of(yyjson_obj_get(pr, "genre")),
                                    text_of(yyjson_obj_get(pr, "about")), read_values(yyjson_obj_get(pr, "values"))});
        }
        if (!kind.id.empty()) kinds_.push_back(std::move(kind));
    }
    yyjson_doc_free(doc);
    reload_templates();
    return true;
}

void Library::reload_templates() {
    templates_.clear();
    ++version_;
    std::error_code ec;
    for (fs::directory_iterator it(folder_, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = path_to_utf8(it->path().filename());
        if (!it->is_regular_file(ec) || !name.ends_with(kExtension)) continue;
        std::string error;
        std::optional<Template> t = read_template(it->path(), &error);
        if (!t) {
            FORGE_WARN("Шаблон %s не прочитан: %s", name.c_str(), error.c_str());
            continue;
        }
        if (find(t->key)) {
            FORGE_WARN("Шаблон %s: id «%s» уже занят, файл пропущен", name.c_str(), t->id.c_str());
            continue;
        }
        if (!kind(t->kind)) FORGE_WARN("Шаблон %s: нет вида «%s»", name.c_str(), t->kind.c_str());
        templates_.push_back(std::move(*t));
    }
    sort_templates();
}

void Library::sort_templates() {
    std::sort(templates_.begin(), templates_.end(), [](const Template& a, const Template& b) {
        return a.kind != b.kind ? a.kind < b.kind : a.name < b.name;
    });
}

std::vector<std::string> Library::genres() const {
    std::vector<std::string> out = genres_;
    auto add = [&](const std::string& g) {
        if (!g.empty() && std::find(out.begin(), out.end(), g) == out.end()) out.push_back(g);
    };
    for (const KindDef& k : kinds_)
        for (const Preset& p : k.presets) add(p.genre);
    for (const Template& t : templates_) add(t.genre);
    return out;
}

const KindDef* Library::kind(std::string_view id) const {
    for (const KindDef& k : kinds_)
        if (k.id == id) return &k;
    return nullptr;
}

const Template* Library::find(u64 key) const {
    for (const Template& t : templates_)
        if (t.key == key) return &t;
    return nullptr;
}

const Template* Library::find(std::string_view id) const { return find(fnv1a(id)); }

std::string Library::free_name(std::string_view wanted) const {
    auto taken = [&](const std::string& name) {
        return std::any_of(templates_.begin(), templates_.end(), [&](const Template& t) { return t.name == name; });
    };
    std::string name(wanted);
    for (int n = 2; taken(name); ++n) name = std::string(wanted) + " " + std::to_string(n);
    return name;
}

std::optional<Template> Library::make(const KindDef& k, const Preset* preset, std::string_view name) const {
    Template t;
    do t.id = random_id();
    while (find(t.id));
    t.key = fnv1a(t.id);
    t.kind = k.id;
    t.name = free_name(name.empty() ? (preset ? preset->name : k.name) : name);
    if (preset) {
        t.about = preset->about;
        t.genre = preset->genre;
        t.values = preset->values;
    }
    t.rev = values_rev(t.values);
    // A file of its own: the name, or the name and a number when taken.
    std::string stem = file_stem(t.name);
    fs::path file = folder_ / utf8_path(stem + std::string(kExtension));
    std::error_code ec;
    for (int n = 2; fs::exists(file, ec); ++n) file = folder_ / utf8_path(stem + " " + std::to_string(n) + std::string(kExtension));
    t.file = file;
    return t;
}

bool Library::write(const Template& t, std::string* error) const {
    const std::string json = template_json(t);
    if (write_file_atomic(t.file, std::span(reinterpret_cast<const u8*>(json.data()), json.size()))) return true;
    if (error) *error = "не записывается " + path_to_utf8(t.file);
    return false;
}

bool Library::put(Template t, std::string* error) {
    t.key = fnv1a(t.id);
    t.rev = values_rev(t.values);
    if (t.file.empty()) t.file = folder_ / utf8_path(file_stem(t.name) + std::string(kExtension));
    if (!write(t, error)) return false;
    auto it = std::find_if(templates_.begin(), templates_.end(), [&](const Template& o) { return o.key == t.key; });
    if (it != templates_.end()) {
        if (it->file != t.file) {
            std::error_code ec;
            fs::remove(it->file, ec);
        }
        *it = std::move(t);
    } else {
        templates_.push_back(std::move(t));
    }
    sort_templates();
    ++version_;
    return true;
}

bool Library::remove(u64 key) {
    auto it = std::find_if(templates_.begin(), templates_.end(), [&](const Template& o) { return o.key == key; });
    if (it == templates_.end()) return false;
    std::error_code ec;
    fs::remove(it->file, ec);
    templates_.erase(it);
    ++version_;
    return true;
}

std::string Library::value(const Template& t, const PropDef& prop) const {
    if (const std::string* v = t.value(prop.id)) return *v;
    // The kind's starting value, else the field's default.
    const KindDef* k = kind_of(t);
    if (!prop.type) return {};
    Scratch s(prop.type);
    if (k)
        for (const KindDef::Part& part : k->components)
            if (part.type == prop.type) merge_into(prop.type, s.data(), part.json);
    return choice_json(prop, field_of(to_json_of(prop.type, s.data()), prop.field));
}

Template Library::with_value(const Template& t, std::string_view prop, std::string json) const {
    Template out = t;
    auto it = std::find_if(out.values.begin(), out.values.end(), [&](const auto& v) { return v.first == prop; });
    if (it != out.values.end()) it->second = std::move(json);
    else out.values.emplace_back(std::string(prop), std::move(json));
    out.rev = values_rev(out.values);
    return out;
}

// --- copies -----------------------------------------------------------------

void Library::attach(scene::Scene& scene) {
    scene.register_component<ObjectRef>();
    scene.set_unpacked([this, &scene](flecs::entity e) { on_unpacked(scene, e); });
}

void Library::on_unpacked(scene::Scene& scene, flecs::entity e) const {
    const ObjectRef* ref = e.try_get<ObjectRef>();
    if (!ref) {
        if (!adopt_) return;
        const Template* t = adopt_(e);
        if (!t) return;
        // Made before templates: what differs from the template is its own.
        ObjectRef r{t->key, t->rev, {}};
        if (const KindDef* k = kind_of(*t))
            for (const PropDef& p : k->props)
                if (value(scene, e, p) != value(*t, p)) r.set_override(p.id, true);
        e.set<ObjectRef>(r);
        return;
    }
    const Template* t = find(ref->key);
    if (t && ref->rev != t->rev) apply(scene, e, *t);
}

flecs::entity Library::spawn(scene::Scene& scene, const Template& t, f64 x, f64 feet_y) const {
    const KindDef* k = kind_of(t);
    if (!k) return {};
    flecs::entity e = scene.spawn(scene::Position::at_tile(x, feet_y - k->foot - 0.02));
    if (!e.is_valid()) return e;
    ecs_world_t* world = scene.ecs().c_ptr();
    for (const KindDef::Part& part : k->components) {
        const flecs::entity_t id = part.type ? component_id(scene, part.type) : 0;
        if (!id) continue;
        Scratch s(part.type);
        merge_into(part.type, s.data(), part.json);
        ecs_set_id(world, e, id, part.type->size, s.data());
    }
    e.set<ObjectRef>({t.key, 0, {}});
    apply(scene, e, t);
    return e;
}

void Library::apply(scene::Scene& scene, flecs::entity e, const Template& t) const {
    const KindDef* k = kind_of(t);
    ObjectRef* ref = e.try_get_mut<ObjectRef>();
    if (!k || !ref) return;
    ecs_world_t* world = scene.ecs().c_ptr();
    for (const KindDef::Part& part : k->components) {
        const flecs::entity_t id = part.type ? component_id(scene, part.type) : 0;
        if (!id) continue;
        std::string fields;
        for (const PropDef& p : k->props) {
            if (p.type != part.type || ref->overrides_prop(p.id)) continue;
            const std::string v = field_json(p, value(t, p));
            if (v.empty()) continue;
            fields += (fields.empty() ? "{" : ",") + json_text(p.field) + ":" + v;
        }
        if (!ecs_has_id(world, e, id)) {
            // The kind gained a part since this copy was made.
            Scratch s(part.type);
            merge_into(part.type, s.data(), part.json);
            ecs_set_id(world, e, id, part.type->size, s.data());
        }
        if (fields.empty()) continue;
        fields += "}";
        void* data = ecs_get_mut_id(world, e, id);
        if (!data) continue;
        merge_into(part.type, data, fields);
        ecs_modified_id(world, e, id);
    }
    ref = e.try_get_mut<ObjectRef>();
    if (ref) ref->rev = t.rev;
}

u32 Library::refresh(scene::Scene& scene) const {
    std::vector<flecs::entity> behind;
    scene.ecs().each([&](flecs::entity e, const ObjectRef& ref) {
        const Template* t = find(ref.key);
        if (t && ref.rev != t->rev) behind.push_back(e);
    });
    for (flecs::entity e : behind) apply(scene, e, *find(e.get<ObjectRef>().key));
    return static_cast<u32>(behind.size());
}

const Template* Library::template_of(flecs::entity e) const {
    const ObjectRef* ref = e.is_alive() ? e.try_get<ObjectRef>() : nullptr;
    return ref ? find(ref->key) : nullptr;
}

std::string Library::value(scene::Scene& scene, flecs::entity e, const PropDef& prop) const {
    const flecs::entity_t id = prop.type ? component_id(scene, prop.type) : 0;
    const void* data = id && e.is_alive() ? ecs_get_id(scene.ecs().c_ptr(), e, id) : nullptr;
    if (!data) return {};
    return choice_json(prop, field_of(to_json_of(prop.type, data), prop.field));
}

bool Library::set_value(scene::Scene& scene, flecs::entity e, const PropDef& prop, const std::string& json) const {
    const flecs::entity_t id = prop.type ? component_id(scene, prop.type) : 0;
    ecs_world_t* world = scene.ecs().c_ptr();
    void* data = id && e.is_alive() ? ecs_get_mut_id(world, e, id) : nullptr;
    const std::string v = field_json(prop, json);
    if (!data || v.empty()) return false;
    if (!merge_into(prop.type, data, "{" + json_text(prop.field) + ":" + v + "}")) return false;
    ecs_modified_id(world, e, id);
    if (ObjectRef* ref = e.try_get_mut<ObjectRef>()) {
        const Template* t = find(ref->key);
        ref->set_override(prop.id, !t || value(*t, prop) != value(scene, e, prop));
    }
    return true;
}

// --- text -------------------------------------------------------------------

std::string Library::display(const PropDef& prop, const std::string& json) {
    yyjson_doc* doc = yyjson_read(json.data(), json.size(), 0);
    if (!doc) return json;
    yyjson_val* v = yyjson_doc_get_root(doc);
    std::string out;
    if (yyjson_is_str(v)) {
        out = text_of(v);
        for (const Choice& c : prop.choices)
            if (c.id == out) out = c.name;
    } else if (yyjson_is_bool(v)) {
        out = yyjson_get_bool(v) ? "да" : "нет";
    } else if (yyjson_is_int(v)) {
        out = std::to_string(yyjson_get_sint(v));
    } else if (yyjson_is_num(v)) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.3g", yyjson_get_num(v));
        out = buf;
    } else {
        out = json;
    }
    yyjson_doc_free(doc);
    return out;
}

std::optional<std::string> Library::parse(const PropDef& prop, std::string_view text) {
    if (!prop.choices.empty()) {
        for (const Choice& c : prop.choices)
            if (c.id == text || c.name == text) return json_text(c.id);
        return std::nullopt;
    }
    using reflect::Kind;
    const Kind kind = prop.info && prop.info->type ? prop.info->type->kind : Kind::String;
    if (kind == Kind::Bool) {
        if (text == "да" || text == "true" || text == "1") return std::string("true");
        if (text == "нет" || text == "false" || text == "0") return std::string("false");
        return std::nullopt;
    }
    if (kind == Kind::String) return json_text(text);
    std::string s(text);
    for (char& c : s)
        if (c == ',') c = '.';
    char* end = nullptr;
    f64 v = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) return std::nullopt;
    if (prop.has_range) v = std::clamp(v, prop.min, prop.max);
    const bool integer = kind >= Kind::I8 && kind <= Kind::U64;
    char buf[64];
    if (integer) std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(std::llround(v)));
    else std::snprintf(buf, sizeof(buf), "%.6g", v);
    return std::string(buf);
}

} // namespace forge::objects
