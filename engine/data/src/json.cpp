#include "forge/data/json.h"

#include "forge/core/profile.h"

#include <yyjson.h>

#include <cmath>
#include <cstring>
#include <limits>

namespace forge::data {

using reflect::FieldInfo;
using reflect::Kind;
using reflect::TypeInfo;

namespace {

constexpr const char* kTypeKey = "$type";
constexpr const char* kVersionKey = "$v";

yyjson_mut_doc* as_doc(void* p) { return static_cast<yyjson_mut_doc*>(p); }
yyjson_mut_val* as_val(void* p) { return static_cast<yyjson_mut_val*>(p); }

yyjson_mut_val* get_key(yyjson_mut_val* obj, std::string_view key) {
    return obj ? yyjson_mut_obj_getn(obj, key.data(), key.size()) : nullptr;
}

// Keys must outlive the document; copy them in.
void put_key(yyjson_mut_doc* doc, yyjson_mut_val* obj, std::string_view key, yyjson_mut_val* value) {
    yyjson_mut_obj_remove_keyn(obj, key.data(), key.size());
    yyjson_mut_obj_add(obj, yyjson_mut_strncpy(doc, key.data(), key.size()), value);
}

// --- writing ---------------------------------------------------------------

i64 read_enum_raw(const TypeInfo* t, const void* p) {
    switch (t->size) {
    case 1: return t->enum_signed ? *static_cast<const i8*>(p) : static_cast<i64>(*static_cast<const u8*>(p));
    case 2: return t->enum_signed ? *static_cast<const i16*>(p) : static_cast<i64>(*static_cast<const u16*>(p));
    case 4: return t->enum_signed ? *static_cast<const i32*>(p) : static_cast<i64>(*static_cast<const u32*>(p));
    default: return *static_cast<const i64*>(p);
    }
}

void write_enum_raw(const TypeInfo* t, void* p, i64 v) {
    switch (t->size) {
    case 1: *static_cast<u8*>(p) = static_cast<u8>(v); break;
    case 2: *static_cast<u16*>(p) = static_cast<u16>(v); break;
    case 4: *static_cast<u32*>(p) = static_cast<u32>(v); break;
    default: *static_cast<i64*>(p) = v; break;
    }
}

yyjson_mut_val* write_value(yyjson_mut_doc* doc, const TypeInfo* t, const void* p);

yyjson_mut_val* write_struct_fields(yyjson_mut_doc* doc, const TypeInfo* t, const void* p, yyjson_mut_val* obj) {
    if (t->version != 1) yyjson_mut_obj_add_uint(doc, obj, kVersionKey, t->version);
    const auto* base = static_cast<const u8*>(p);
    for (const FieldInfo& f : t->fields) {
        if (f.flags & reflect::FieldTransient) continue;
        // Field names live in static TypeInfo, so they can be referenced, not copied.
        yyjson_mut_obj_add(obj, yyjson_mut_strn(doc, f.name.data(), f.name.size()),
                           write_value(doc, f.type, base + f.offset));
    }
    return obj;
}

yyjson_mut_val* write_value(yyjson_mut_doc* doc, const TypeInfo* t, const void* p) {
    switch (t->kind) {
    case Kind::Bool: return yyjson_mut_bool(doc, *static_cast<const bool*>(p));
    case Kind::I8: return yyjson_mut_sint(doc, *static_cast<const i8*>(p));
    case Kind::U8: return yyjson_mut_uint(doc, *static_cast<const u8*>(p));
    case Kind::I16: return yyjson_mut_sint(doc, *static_cast<const i16*>(p));
    case Kind::U16: return yyjson_mut_uint(doc, *static_cast<const u16*>(p));
    case Kind::I32: return yyjson_mut_sint(doc, *static_cast<const i32*>(p));
    case Kind::U32: return yyjson_mut_uint(doc, *static_cast<const u32*>(p));
    case Kind::I64: return yyjson_mut_sint(doc, *static_cast<const i64*>(p));
    case Kind::U64: return yyjson_mut_uint(doc, *static_cast<const u64*>(p));
    case Kind::F32: return yyjson_mut_float(doc, *static_cast<const f32*>(p));
    case Kind::F64: return yyjson_mut_real(doc, *static_cast<const f64*>(p));
    case Kind::String: {
        const auto& s = *static_cast<const std::string*>(p);
        return yyjson_mut_strncpy(doc, s.data(), s.size());
    }
    case Kind::Guid: {
        const std::string s = static_cast<const Guid*>(p)->to_string();
        return yyjson_mut_strncpy(doc, s.data(), s.size());
    }
    case Kind::Vec2: {
        const auto& v = *static_cast<const Vec2*>(p);
        yyjson_mut_val* arr = yyjson_mut_arr(doc);
        yyjson_mut_arr_add_float(doc, arr, v.x);
        yyjson_mut_arr_add_float(doc, arr, v.y);
        return arr;
    }
    case Kind::Color: {
        const auto& c = *static_cast<const Color*>(p);
        yyjson_mut_val* arr = yyjson_mut_arr(doc);
        for (f32 x : {c.r, c.g, c.b, c.a}) yyjson_mut_arr_add_float(doc, arr, x);
        return arr;
    }
    case Kind::Enum: {
        const i64 raw = read_enum_raw(t, p);
        if (const auto* v = t->find_enum(raw)) return yyjson_mut_strn(doc, v->name.data(), v->name.size());
        return yyjson_mut_sint(doc, raw); // a value with no name still round-trips
    }
    case Kind::Struct: return write_struct_fields(doc, t, p, yyjson_mut_obj(doc));
    case Kind::Array: {
        yyjson_mut_val* arr = yyjson_mut_arr(doc);
        const usize n = t->array.size(p);
        const auto* data = static_cast<const u8*>(t->array.data_const(p));
        for (usize i = 0; i < n; ++i) yyjson_mut_arr_append(arr, write_value(doc, t->element, data + i * t->element->size));
        return arr;
    }
    }
    return yyjson_mut_null(doc);
}

// --- reading ---------------------------------------------------------------

struct Reader {
    yyjson_mut_doc* doc;
    LoadReport& report;
    std::string path;

    void warn(const char* what) { report.warnings.push_back((path.empty() ? std::string("(root)") : path) + ": " + what); }

    bool read_number(yyjson_mut_val* v, f64& out) {
        if (!yyjson_mut_is_num(v)) return false;
        out = yyjson_mut_get_num(v);
        return true;
    }

    template <typename I>
    void read_int(yyjson_mut_val* v, void* p) {
        if (yyjson_mut_is_int(v)) {
            // yyjson stores either a signed or an unsigned 64-bit integer.
            bool fits;
            I result{};
            if (yyjson_mut_is_sint(v)) {
                const i64 x = yyjson_mut_get_sint(v);
                fits = std::is_signed_v<I> ? (x >= static_cast<i64>(std::numeric_limits<I>::min()) &&
                                              x <= static_cast<i64>(std::numeric_limits<I>::max()))
                                           : (x >= 0 && static_cast<u64>(x) <= static_cast<u64>(std::numeric_limits<I>::max()));
                result = static_cast<I>(x);
            } else {
                const u64 x = yyjson_mut_get_uint(v);
                fits = x <= static_cast<u64>(std::numeric_limits<I>::max());
                result = static_cast<I>(x);
            }
            if (fits) *static_cast<I*>(p) = result;
            else warn("number out of range, default kept");
            return;
        }
        f64 d;
        if (read_number(v, d) && std::trunc(d) == d && d >= static_cast<f64>(std::numeric_limits<I>::min()) &&
            d <= static_cast<f64>(std::numeric_limits<I>::max())) {
            *static_cast<I*>(p) = static_cast<I>(d);
            return;
        }
        warn("expected an integer, default kept");
    }

    bool read_floats(yyjson_mut_val* v, f32* out, usize count) {
        if (!yyjson_mut_is_arr(v) || yyjson_mut_arr_size(v) != count) return false;
        f32 tmp[4];
        for (usize i = 0; i < count; ++i) {
            f64 d;
            if (!read_number(yyjson_mut_arr_get(v, i), d)) return false;
            tmp[i] = static_cast<f32>(d);
        }
        std::memcpy(out, tmp, count * sizeof(f32));
        return true;
    }

    void read_value(const TypeInfo* t, yyjson_mut_val* v, void* p) {
        switch (t->kind) {
        case Kind::Bool:
            if (yyjson_mut_is_bool(v)) *static_cast<bool*>(p) = yyjson_mut_get_bool(v);
            else warn("expected true/false, default kept");
            return;
        case Kind::I8: read_int<i8>(v, p); return;
        case Kind::U8: read_int<u8>(v, p); return;
        case Kind::I16: read_int<i16>(v, p); return;
        case Kind::U16: read_int<u16>(v, p); return;
        case Kind::I32: read_int<i32>(v, p); return;
        case Kind::U32: read_int<u32>(v, p); return;
        case Kind::I64: read_int<i64>(v, p); return;
        case Kind::U64: read_int<u64>(v, p); return;
        case Kind::F32:
        case Kind::F64: {
            f64 d;
            if (!read_number(v, d)) { warn("expected a number, default kept"); return; }
            if (t->kind == Kind::F32) *static_cast<f32*>(p) = static_cast<f32>(d);
            else *static_cast<f64*>(p) = d;
            return;
        }
        case Kind::String:
            if (yyjson_mut_is_str(v)) static_cast<std::string*>(p)->assign(yyjson_mut_get_str(v), yyjson_mut_get_len(v));
            else warn("expected text, default kept");
            return;
        case Kind::Guid: {
            std::optional<Guid> g;
            if (yyjson_mut_is_str(v)) g = Guid::parse({yyjson_mut_get_str(v), yyjson_mut_get_len(v)});
            if (g) *static_cast<Guid*>(p) = *g;
            else warn("expected a guid, default kept");
            return;
        }
        case Kind::Vec2:
            if (!read_floats(v, &static_cast<Vec2*>(p)->x, 2)) warn("expected [x, y], default kept");
            return;
        case Kind::Color:
            if (!read_floats(v, &static_cast<Color*>(p)->r, 4)) warn("expected [r, g, b, a], default kept");
            return;
        case Kind::Enum: {
            if (yyjson_mut_is_str(v)) {
                if (const auto* e = t->find_enum({yyjson_mut_get_str(v), yyjson_mut_get_len(v)})) {
                    write_enum_raw(t, p, e->value);
                    return;
                }
                warn("unknown enum value, default kept");
                return;
            }
            if (yyjson_mut_is_int(v)) { write_enum_raw(t, p, yyjson_mut_get_sint(v)); return; }
            warn("expected an enum name, default kept");
            return;
        }
        case Kind::Struct: read_struct(t, v, p); return;
        case Kind::Array: {
            if (!yyjson_mut_is_arr(v)) { warn("expected a list, default kept"); return; }
            const usize n = yyjson_mut_arr_size(v);
            t->array.resize(p, n);
            auto* data = static_cast<u8*>(t->array.data(p));
            const usize base_len = path.size();
            usize i = 0;
            yyjson_mut_val* item;
            yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(v);
            while ((item = yyjson_mut_arr_iter_next(&it))) {
                path += '[';
                path += std::to_string(i);
                path += ']';
                read_value(t->element, item, data + i * t->element->size);
                path.resize(base_len);
                ++i;
            }
            return;
        }
        }
    }

    void read_struct(const TypeInfo* t, yyjson_mut_val* obj, void* p) {
        if (!yyjson_mut_is_obj(obj)) { warn("expected an object, defaults kept"); return; }

        u32 saved_version = 1;
        if (yyjson_mut_val* vv = get_key(obj, kVersionKey); vv && yyjson_mut_is_uint(vv)) {
            saved_version = static_cast<u32>(yyjson_mut_get_uint(vv));
        }
        if (saved_version > t->version) {
            warn("saved by a newer version of this type; unknown fields are skipped");
        } else if (saved_version < t->version && t->migrate) {
            JsonObjectEdit edit(doc, obj);
            t->migrate(saved_version, edit);
        }

        auto* base = static_cast<u8*>(p);
        const usize base_len = path.size();
        for (const FieldInfo& f : t->fields) {
            if (f.flags & reflect::FieldTransient) continue;
            yyjson_mut_val* v = get_key(obj, f.name);
            if (!v && !f.former_name.empty()) v = get_key(obj, f.former_name);
            if (!v) continue; // missing: keep the default
            if (!path.empty()) path += '.';
            path += f.name;
            read_value(f.type, v, base + f.offset);
            path.resize(base_len);
        }

        // Report keys nobody claimed, so typos and removed fields are visible.
        yyjson_mut_obj_iter it = yyjson_mut_obj_iter_with(obj);
        yyjson_mut_val* key;
        while ((key = yyjson_mut_obj_iter_next(&it))) {
            const std::string_view name(yyjson_mut_get_str(key), yyjson_mut_get_len(key));
            if (!name.empty() && name[0] == '$') continue;
            bool known = false;
            for (const FieldInfo& f : t->fields) {
                if (f.name == name || f.former_name == name) { known = true; break; }
            }
            if (!known) warn(("unknown field '" + std::string(name) + "' ignored").c_str());
        }
    }
};

} // namespace

// --- JsonObjectEdit --------------------------------------------------------

bool JsonObjectEdit::has(std::string_view key) const { return get_key(as_val(obj_), key) != nullptr; }

bool JsonObjectEdit::rename(std::string_view from, std::string_view to) {
    yyjson_mut_val* v = get_key(as_val(obj_), from);
    if (!v) return false;
    yyjson_mut_obj_remove_keyn(as_val(obj_), from.data(), from.size());
    put_key(as_doc(doc_), as_val(obj_), to, v);
    return true;
}

bool JsonObjectEdit::remove(std::string_view key) {
    return obj_ && yyjson_mut_obj_remove_keyn(as_val(obj_), key.data(), key.size()) != nullptr;
}

std::optional<f64> JsonObjectEdit::get_number(std::string_view key) const {
    yyjson_mut_val* v = get_key(as_val(obj_), key);
    if (!v || !yyjson_mut_is_num(v)) return std::nullopt;
    return yyjson_mut_get_num(v);
}

std::optional<std::string> JsonObjectEdit::get_string(std::string_view key) const {
    yyjson_mut_val* v = get_key(as_val(obj_), key);
    if (!v || !yyjson_mut_is_str(v)) return std::nullopt;
    return std::string(yyjson_mut_get_str(v), yyjson_mut_get_len(v));
}

std::optional<bool> JsonObjectEdit::get_bool(std::string_view key) const {
    yyjson_mut_val* v = get_key(as_val(obj_), key);
    if (!v || !yyjson_mut_is_bool(v)) return std::nullopt;
    return yyjson_mut_get_bool(v);
}

void JsonObjectEdit::set_number(std::string_view key, f64 value) {
    if (obj_) put_key(as_doc(doc_), as_val(obj_), key, yyjson_mut_real(as_doc(doc_), value));
}
void JsonObjectEdit::set_int(std::string_view key, i64 value) {
    if (obj_) put_key(as_doc(doc_), as_val(obj_), key, yyjson_mut_sint(as_doc(doc_), value));
}
void JsonObjectEdit::set_string(std::string_view key, std::string_view value) {
    if (obj_) put_key(as_doc(doc_), as_val(obj_), key, yyjson_mut_strncpy(as_doc(doc_), value.data(), value.size()));
}
void JsonObjectEdit::set_bool(std::string_view key, bool value) {
    if (obj_) put_key(as_doc(doc_), as_val(obj_), key, yyjson_mut_bool(as_doc(doc_), value));
}

JsonObjectEdit JsonObjectEdit::child(std::string_view key) const {
    yyjson_mut_val* v = get_key(as_val(obj_), key);
    return JsonObjectEdit(doc_, v && yyjson_mut_is_obj(v) ? v : nullptr);
}

// --- public API ------------------------------------------------------------

std::string to_json(const TypeInfo* type, const void* object, bool pretty) {
    FORGE_ZONE();
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root;
    if (type->kind == Kind::Struct) {
        root = yyjson_mut_obj(doc);
        yyjson_mut_obj_add(root, yyjson_mut_str(doc, kTypeKey), yyjson_mut_strn(doc, type->name.data(), type->name.size()));
        write_struct_fields(doc, type, object, root);
    } else {
        root = write_value(doc, type, object);
    }
    yyjson_mut_doc_set_root(doc, root);

    yyjson_write_flag flags = YYJSON_WRITE_ALLOW_INVALID_UNICODE;
    if (pretty) flags |= YYJSON_WRITE_PRETTY_TWO_SPACES | YYJSON_WRITE_NEWLINE_AT_END;
    usize len = 0;
    char* text = yyjson_mut_write(doc, flags, &len);
    std::string out = text ? std::string(text, len) : std::string();
    std::free(text);
    yyjson_mut_doc_free(doc);
    return out;
}

bool from_json(const TypeInfo* type, void* object, std::string_view json, LoadReport& report) {
    FORGE_ZONE();
    yyjson_read_err err{};
    yyjson_doc* idoc = yyjson_read_opts(const_cast<char*>(json.data()), json.size(),
                                        YYJSON_READ_ALLOW_TRAILING_COMMAS | YYJSON_READ_ALLOW_COMMENTS, nullptr, &err);
    if (!idoc) {
        report.error = "JSON syntax error at byte " + std::to_string(err.pos) + ": " + (err.msg ? err.msg : "?");
        return false;
    }
    // Migrations edit the document, so it is read in its mutable form.
    yyjson_mut_doc* doc = yyjson_doc_mut_copy(idoc, nullptr);
    yyjson_doc_free(idoc);
    yyjson_mut_val* root = yyjson_mut_doc_get_root(doc);

    bool ok = true;
    if (type->kind == Kind::Struct) {
        yyjson_mut_val* tv = get_key(root, kTypeKey);
        if (!yyjson_mut_is_obj(root)) {
            report.error = "expected an object of type " + type->name;
            ok = false;
        } else if (tv && (!yyjson_mut_is_str(tv) || type->name != std::string_view(yyjson_mut_get_str(tv), yyjson_mut_get_len(tv)))) {
            report.error = "file holds a different type than " + type->name;
            ok = false;
        }
    }
    if (ok) {
        Reader reader{doc, report, {}};
        reader.read_value(type, root, object);
    }
    yyjson_mut_doc_free(doc);
    return ok;
}

} // namespace forge::data
