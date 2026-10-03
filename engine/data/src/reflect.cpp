#include "forge/data/reflect.h"

#include "forge/core/assert.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace forge::reflect {

namespace {

struct Registry {
    std::unordered_map<std::string, const TypeInfo*> by_name;
    std::unordered_map<u64, const TypeInfo*> by_id;
};

// Function-local so it exists before any static registrar in another
// translation unit touches it.
Registry& registry() {
    static Registry r;
    return r;
}

template <typename T>
const TypeInfo* builtin(const char* name, Kind kind) {
    static TypeInfo info;
    static bool started = false;
    if (!started) {
        started = true;
        detail::init_common<T>(info, name, kind);
    }
    return &info;
}

u64 hash_type(const TypeInfo* t, std::vector<const TypeInfo*>& visiting) {
    u64 h = fnv1a(t->name);
    h = fnv1a_u64(static_cast<u64>(t->kind), h);
    h = fnv1a_u64(t->size, h);
    h = fnv1a_u64(t->version, h);
    // A type inside itself (through an array) contributes only its name.
    if (std::find(visiting.begin(), visiting.end(), t) != visiting.end()) return h;
    visiting.push_back(t);
    for (const FieldInfo& f : t->fields) {
        if (f.flags & FieldTransient) continue;
        h = fnv1a(f.name, h);
        h = fnv1a_u64(f.offset, h);
        h = fnv1a_u64(hash_type(f.type, visiting), h);
    }
    for (const EnumValue& v : t->enum_values) {
        h = fnv1a(v.name, h);
        h = fnv1a_u64(static_cast<u64>(v.value), h);
    }
    if (t->element) h = fnv1a_u64(hash_type(t->element, visiting), h);
    visiting.pop_back();
    return h;
}

} // namespace

namespace detail {

void register_type(const TypeInfo* info) {
    Registry& r = registry();
    const auto [it, inserted] = r.by_id.emplace(info->id, info);
    FORGE_VERIFY(inserted || it->second == info); // two types with one name, or an id collision
    r.by_name.emplace(info->name, info);
}

} // namespace detail

const TypeInfo* find_type(std::string_view name) {
    const Registry& r = registry();
    auto it = r.by_name.find(std::string(name));
    return it == r.by_name.end() ? nullptr : it->second;
}

const TypeInfo* find_type_by_id(u64 id) {
    const Registry& r = registry();
    auto it = r.by_id.find(id);
    return it == r.by_id.end() ? nullptr : it->second;
}

std::vector<const TypeInfo*> all_types() {
    std::vector<const TypeInfo*> out;
    for (const auto& [id, info] : registry().by_id) out.push_back(info);
    std::sort(out.begin(), out.end(), [](const TypeInfo* a, const TypeInfo* b) { return a->name < b->name; });
    return out;
}

const FieldInfo* TypeInfo::find_field(std::string_view field_name) const {
    for (const FieldInfo& f : fields) {
        if (f.name == field_name) return &f;
    }
    return nullptr;
}

const EnumValue* TypeInfo::find_enum(std::string_view value_name) const {
    for (const EnumValue& v : enum_values) {
        if (v.name == value_name) return &v;
    }
    return nullptr;
}

const EnumValue* TypeInfo::find_enum(i64 value) const {
    for (const EnumValue& v : enum_values) {
        if (v.value == value) return &v;
    }
    return nullptr;
}

u64 TypeInfo::schema_hash() const {
    // Types are immutable after static initialization, so caching is safe;
    // concurrent first calls compute the same value.
    u64 cached = schema_hash_cache.load(std::memory_order_relaxed);
    if (cached == 0) {
        std::vector<const TypeInfo*> visiting;
        const u64 h = hash_type(this, visiting);
        cached = h == 0 ? 1 : h;
        schema_hash_cache.store(cached, std::memory_order_relaxed);
    }
    return cached;
}

const TypeInfo* TypeOf<bool>::get() { return builtin<bool>("bool", Kind::Bool); }
const TypeInfo* TypeOf<i8>::get() { return builtin<i8>("i8", Kind::I8); }
const TypeInfo* TypeOf<u8>::get() { return builtin<u8>("u8", Kind::U8); }
const TypeInfo* TypeOf<i16>::get() { return builtin<i16>("i16", Kind::I16); }
const TypeInfo* TypeOf<u16>::get() { return builtin<u16>("u16", Kind::U16); }
const TypeInfo* TypeOf<i32>::get() { return builtin<i32>("i32", Kind::I32); }
const TypeInfo* TypeOf<u32>::get() { return builtin<u32>("u32", Kind::U32); }
const TypeInfo* TypeOf<i64>::get() { return builtin<i64>("i64", Kind::I64); }
const TypeInfo* TypeOf<u64>::get() { return builtin<u64>("u64", Kind::U64); }
const TypeInfo* TypeOf<f32>::get() { return builtin<f32>("f32", Kind::F32); }
const TypeInfo* TypeOf<f64>::get() { return builtin<f64>("f64", Kind::F64); }
const TypeInfo* TypeOf<std::string>::get() { return builtin<std::string>("string", Kind::String); }
const TypeInfo* TypeOf<Guid>::get() { return builtin<Guid>("guid", Kind::Guid); }
const TypeInfo* TypeOf<Vec2>::get() { return builtin<Vec2>("vec2", Kind::Vec2); }
const TypeInfo* TypeOf<Color>::get() { return builtin<Color>("color", Kind::Color); }

} // namespace forge::reflect
