#pragma once

// Runtime type information: the single description of every piece of game
// data. A type described once here gets, with no extra code, its inspector in
// the editor, text and binary serialization, access from scripts and nodes,
// and a schema hash that tells when cooked data must be rebuilt.
//
// Describing a type (at global namespace scope, in one .cpp file):
//
//   // header
//   struct Health { f32 current = 100; f32 max = 100; };
//   FORGE_REFLECT_DECLARE(Health)
//
//   // source
//   FORGE_REFLECT(Health, 1) {
//       t.field("current", &Health::current).range(0, 10000).label("Текущее");
//       t.field("max", &Health::max).range(1, 10000);
//   }
//
// Enums use the same macros, with t.value("Idle", State::Idle).
// Types register themselves during static initialization; after main()
// starts, the registry is read-only and safe to use from any thread.

#include "forge/core/hash.h"
#include "forge/core/math.h"
#include "forge/core/types.h"
#include "forge/data/guid.h"

#include <atomic>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace forge::data {
class JsonObjectEdit; // see json.h
}

namespace forge::reflect {

enum class Kind : u8 {
    Bool,
    I8,
    U8,
    I16,
    U16,
    I32,
    U32,
    I64,
    U64,
    F32,
    F64,
    String, // std::string
    Guid,
    Vec2,
    Color,
    Enum,
    Struct,
    Array, // std::vector<T>
};

enum FieldFlags : u32 {
    FieldNone = 0,
    FieldHidden = 1 << 0,    // not shown in the inspector
    FieldReadOnly = 1 << 1,  // shown but not editable
    FieldTransient = 1 << 2, // runtime-only: never saved or cooked
};

struct TypeInfo;

struct FieldInfo {
    std::string name;
    const TypeInfo* type = nullptr;
    u32 offset = 0;
    u32 flags = FieldNone;
    bool has_range = false;
    f64 min = 0;
    f64 max = 0;
    std::string label;
    std::string tooltip;
    // Key the field was saved under before a rename; read when the current
    // key is missing. Covers the most common migration without code.
    std::string former_name;
};

struct EnumValue {
    std::string name;
    i64 value = 0;
};

struct ArrayOps {
    usize (*size)(const void* array) = nullptr;
    void (*resize)(void* array, usize count) = nullptr;
    void* (*data)(void* array) = nullptr;
    const void* (*data_const)(const void* array) = nullptr;
};

// Called when a saved object is older than the type. Edits the saved JSON
// object in place so it matches the current version, before it is read.
using MigrateFn = void (*)(u32 from_version, data::JsonObjectEdit& object);

struct TypeInfo {
    std::string name;
    u64 id = 0; // fnv1a(name)
    Kind kind = Kind::Struct;
    u32 size = 0;
    u32 align = 0;
    u32 version = 1;
    // Safe to copy with memcpy: binary serialization moves whole arrays of
    // such types in one copy.
    bool trivially_copyable = false;

    std::vector<FieldInfo> fields;     // Struct
    std::vector<EnumValue> enum_values; // Enum
    const TypeInfo* element = nullptr; // Array
    ArrayOps array;                    // Array
    MigrateFn migrate = nullptr;       // Struct

    void (*construct)(void* at) = nullptr;
    void (*destruct)(void* at) = nullptr;

    const FieldInfo* find_field(std::string_view field_name) const;
    const EnumValue* find_enum(std::string_view value_name) const;
    const EnumValue* find_enum(i64 value) const;

    // Hash of the binary layout (names, kinds, sizes, offsets, versions,
    // recursively). Cooked data whose hash differs is stale and is rebuilt.
    u64 schema_hash() const;

    mutable std::atomic<u64> schema_hash_cache{0};
    bool enum_signed = false; // Enum: underlying type is signed
};

// Every registered struct and enum, by name or id.
const TypeInfo* find_type(std::string_view name);
const TypeInfo* find_type_by_id(u64 id);
std::vector<const TypeInfo*> all_types();

// --- type_of<T>() ----------------------------------------------------------

template <typename T>
struct TypeOf; // specialized for every supported type

template <typename T>
struct IsVector : std::false_type {};
template <typename E>
struct IsVector<std::vector<E>> : std::true_type {};

template <typename E>
const TypeInfo* array_type_of();

template <typename T>
const TypeInfo* type_of() {
    using U = std::remove_cv_t<T>;
    if constexpr (IsVector<U>::value) return array_type_of<typename U::value_type>();
    else return TypeOf<U>::get();
}

#define FORGE_REFLECT_BUILTIN(T)                                                                                  \
    template <>                                                                                                    \
    struct TypeOf<T> {                                                                                             \
        static const TypeInfo* get();                                                                              \
    };
FORGE_REFLECT_BUILTIN(bool)
FORGE_REFLECT_BUILTIN(i8)
FORGE_REFLECT_BUILTIN(u8)
FORGE_REFLECT_BUILTIN(i16)
FORGE_REFLECT_BUILTIN(u16)
FORGE_REFLECT_BUILTIN(i32)
FORGE_REFLECT_BUILTIN(u32)
FORGE_REFLECT_BUILTIN(i64)
FORGE_REFLECT_BUILTIN(u64)
FORGE_REFLECT_BUILTIN(f32)
FORGE_REFLECT_BUILTIN(f64)
FORGE_REFLECT_BUILTIN(std::string)
FORGE_REFLECT_BUILTIN(Guid)
FORGE_REFLECT_BUILTIN(Vec2)
FORGE_REFLECT_BUILTIN(Color)
#undef FORGE_REFLECT_BUILTIN

// --- builders --------------------------------------------------------------

class FieldBuilder {
public:
    FieldBuilder(std::vector<FieldInfo>& fields, usize index) : fields_(fields), index_(index) {}

    FieldBuilder& range(f64 min, f64 max) {
        f().has_range = true;
        f().min = min;
        f().max = max;
        return *this;
    }
    FieldBuilder& label(const char* text) { f().label = text; return *this; }
    FieldBuilder& tooltip(const char* text) { f().tooltip = text; return *this; }
    FieldBuilder& hidden() { f().flags |= FieldHidden; return *this; }
    FieldBuilder& read_only() { f().flags |= FieldReadOnly; return *this; }
    FieldBuilder& transient() { f().flags |= FieldTransient; return *this; }
    FieldBuilder& renamed_from(const char* old_name) { f().former_name = old_name; return *this; }

private:
    FieldInfo& f() { return fields_[index_]; }
    std::vector<FieldInfo>& fields_;
    usize index_;
};

template <typename T>
class StructBuilder {
public:
    explicit StructBuilder(TypeInfo& info) : info_(info) {}

    template <typename M>
    FieldBuilder field(const char* name, M T::*member) {
        FieldInfo f;
        f.name = name;
        f.type = type_of<M>();
        f.offset = offset_of(member);
        info_.fields.push_back(std::move(f));
        return FieldBuilder(info_.fields, info_.fields.size() - 1);
    }

    void migrate(MigrateFn fn) { info_.migrate = fn; }

private:
    template <typename M>
    static u32 offset_of(M T::*member) {
        // Address arithmetic on uninitialized storage: no T is constructed.
        alignas(T) static unsigned char storage[sizeof(T)];
        const T* object = reinterpret_cast<const T*>(storage);
        return static_cast<u32>(reinterpret_cast<const unsigned char*>(&(object->*member)) - storage);
    }

    TypeInfo& info_;
};

template <typename T>
class EnumBuilder {
public:
    explicit EnumBuilder(TypeInfo& info) : info_(info) {}
    void value(const char* name, T v) { info_.enum_values.push_back({name, static_cast<i64>(v)}); }

private:
    TypeInfo& info_;
};

template <typename T>
using Builder = std::conditional_t<std::is_enum_v<T>, EnumBuilder<T>, StructBuilder<T>>;

namespace detail {

void register_type(const TypeInfo* info);

template <typename T>
void init_common(TypeInfo& info, const char* name, Kind kind) {
    info.name = name;
    info.id = fnv1a(name);
    info.kind = kind;
    info.size = sizeof(T);
    info.align = alignof(T);
    info.trivially_copyable = std::is_trivially_copyable_v<T>;
    info.construct = [](void* at) { new (at) T(); };
    info.destruct = [](void* at) { static_cast<T*>(at)->~T(); };
}

// Builds a user type exactly once. The TypeInfo is published before its
// fields are described, so a type can contain std::vector of itself.
template <typename T>
const TypeInfo* build(const char* name, u32 version, void (*describe)(Builder<T>&)) {
    static TypeInfo info;
    static bool started = false;
    if (!started) {
        started = true;
        init_common<T>(info, name, std::is_enum_v<T> ? Kind::Enum : Kind::Struct);
        info.version = version;
        if constexpr (std::is_enum_v<T>) info.enum_signed = std::is_signed_v<std::underlying_type_t<T>>;
        Builder<T> builder(info);
        describe(builder);
        register_type(&info);
    }
    return &info;
}

} // namespace detail

template <typename E>
const TypeInfo* array_type_of() {
    static TypeInfo info;
    static bool started = false;
    if (!started) {
        started = true;
        using V = std::vector<E>;
        const TypeInfo* element = type_of<E>();
        detail::init_common<V>(info, "", Kind::Array);
        info.name = "Array<" + element->name + ">";
        info.id = fnv1a(info.name);
        info.element = element;
        info.array.size = [](const void* a) { return static_cast<const V*>(a)->size(); };
        info.array.resize = [](void* a, usize n) { static_cast<V*>(a)->resize(n); };
        info.array.data = [](void* a) -> void* { return static_cast<V*>(a)->data(); };
        info.array.data_const = [](const void* a) -> const void* { return static_cast<const V*>(a)->data(); };
    }
    return &info;
}

} // namespace forge::reflect

#define FORGE_REFLECT_CONCAT_(a, b) a##b
#define FORGE_REFLECT_CONCAT(a, b) FORGE_REFLECT_CONCAT_(a, b)

// In a header, at global namespace scope.
#define FORGE_REFLECT_DECLARE(T)                                                                                   \
    template <>                                                                                                    \
    struct forge::reflect::TypeOf<T> {                                                                             \
        static const ::forge::reflect::TypeInfo* get();                                                            \
    };

// In one source file, at global namespace scope, followed by the body that
// describes the type through `t`.
#define FORGE_REFLECT(T, VERSION)                                                                                  \
    static void FORGE_REFLECT_CONCAT(forge_describe_, __LINE__)(::forge::reflect::Builder<T> & t);                \
    const ::forge::reflect::TypeInfo* forge::reflect::TypeOf<T>::get() {                                           \
        return ::forge::reflect::detail::build<T>(#T, VERSION, &FORGE_REFLECT_CONCAT(forge_describe_, __LINE__)); \
    }                                                                                                              \
    [[maybe_unused]] static const ::forge::reflect::TypeInfo* FORGE_REFLECT_CONCAT(forge_registered_, __LINE__) =  \
        ::forge::reflect::TypeOf<T>::get();                                                                        \
    static void FORGE_REFLECT_CONCAT(forge_describe_, __LINE__)([[maybe_unused]] ::forge::reflect::Builder<T> & t)
