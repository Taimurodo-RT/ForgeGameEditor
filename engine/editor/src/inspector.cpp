#include "forge/editor/inspector.h"

#include "forge/core/math.h"
#include "forge/data/guid.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace forge::editor {

using reflect::Kind;
using reflect::TypeInfo;

namespace {

template <typename T>
T& at(void* p) {
    return *static_cast<T*>(p);
}
template <typename T>
const T& at(const void* p) {
    return *static_cast<const T*>(p);
}

std::string number(f64 v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

i64 read_int(const TypeInfo* type, const void* p) {
    switch (type->kind) {
    case Kind::I8: return at<i8>(p);
    case Kind::U8: return at<u8>(p);
    case Kind::I16: return at<i16>(p);
    case Kind::U16: return at<u16>(p);
    case Kind::I32: return at<i32>(p);
    case Kind::U32: return at<u32>(p);
    case Kind::I64: return at<i64>(p);
    case Kind::U64: return static_cast<i64>(at<u64>(p));
    default: return 0;
    }
}

void write_int(const TypeInfo* type, void* p, i64 v) {
    switch (type->kind) {
    case Kind::I8: at<i8>(p) = static_cast<i8>(std::clamp<i64>(v, INT8_MIN, INT8_MAX)); break;
    case Kind::U8: at<u8>(p) = static_cast<u8>(std::clamp<i64>(v, 0, UINT8_MAX)); break;
    case Kind::I16: at<i16>(p) = static_cast<i16>(std::clamp<i64>(v, INT16_MIN, INT16_MAX)); break;
    case Kind::U16: at<u16>(p) = static_cast<u16>(std::clamp<i64>(v, 0, UINT16_MAX)); break;
    case Kind::I32: at<i32>(p) = static_cast<i32>(std::clamp<i64>(v, INT32_MIN, INT32_MAX)); break;
    case Kind::U32: at<u32>(p) = static_cast<u32>(std::clamp<i64>(v, 0, UINT32_MAX)); break;
    case Kind::I64: at<i64>(p) = v; break;
    case Kind::U64: at<u64>(p) = static_cast<u64>(std::max<i64>(v, 0)); break;
    default: break;
    }
}

i64 read_enum(const TypeInfo* type, const void* p) {
    switch (type->size) {
    case 1: return type->enum_signed ? at<i8>(p) : at<u8>(p);
    case 2: return type->enum_signed ? at<i16>(p) : at<u16>(p);
    case 4: return type->enum_signed ? at<i32>(p) : at<u32>(p);
    default: return at<i64>(p);
    }
}

void write_enum(const TypeInfo* type, void* p, i64 v) {
    switch (type->size) {
    case 1: at<u8>(p) = static_cast<u8>(v); break;
    case 2: at<u16>(p) = static_cast<u16>(v); break;
    case 4: at<u32>(p) = static_cast<u32>(v); break;
    default: at<i64>(p) = v; break;
    }
}

bool is_integer(Kind k) { return k >= Kind::I8 && k <= Kind::U64; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

bool parse_f64(std::string_view text, f64& out) {
    const std::string s(trim(text));
    if (s.empty()) return false;
    std::string normalized = s;
    std::replace(normalized.begin(), normalized.end(), ',', '.'); // "0,5" typed in Russian locale
    char* end = nullptr;
    out = std::strtod(normalized.c_str(), &end);
    return end && *end == 0 && std::isfinite(out);
}

void describe(const TypeInfo* type, const void* object, const std::string& prefix, u32 depth,
              std::vector<FieldRow>& out);

void add_row(const reflect::FieldInfo* field, const std::string& name, const TypeInfo* type, const void* value,
             const std::string& path, u32 depth, bool read_only, std::vector<FieldRow>& out) {
    FieldRow row;
    row.path = path;
    row.label = field && !field->label.empty() ? field->label : name;
    row.tooltip = field ? field->tooltip : std::string();
    row.kind = type->kind;
    row.depth = depth;
    row.read_only = read_only || (field && (field->flags & reflect::FieldReadOnly));
    if (field && field->has_range) {
        row.has_range = true;
        row.min = field->min;
        row.max = field->max;
    }
    if (type->kind == Kind::Enum)
        for (const auto& v : type->enum_values) row.options.push_back(v.name);
    if (type->kind == Kind::Array) row.value = std::to_string(type->array.size(value));
    else if (type->kind != Kind::Struct) row.value = value_text(type, value);
    const bool ro = row.read_only;
    out.push_back(std::move(row));

    if (type->kind == Kind::Struct) {
        describe(type, value, path, depth + 1, out);
    } else if (type->kind == Kind::Array) {
        const usize count = type->array.size(value);
        const auto* bytes = static_cast<const u8*>(type->array.data_const(value));
        for (usize i = 0; i < count; ++i)
            add_row(nullptr, "[" + std::to_string(i) + "]", type->element, bytes + i * type->element->size,
                    path + "." + std::to_string(i), depth + 1, ro, out);
    }
}

void describe(const TypeInfo* type, const void* object, const std::string& prefix, u32 depth,
              std::vector<FieldRow>& out) {
    for (const auto& f : type->fields) {
        if (f.flags & reflect::FieldHidden) continue;
        const void* value = static_cast<const u8*>(object) + f.offset;
        add_row(&f, f.name, f.type, value, prefix.empty() ? f.name : prefix + "." + f.name, depth, false, out);
    }
}

// Walks a path to the value it names. field is the last struct field passed
// (for its range), or nullptr inside arrays.
void* resolve(const TypeInfo* type, void* object, std::string_view path, const TypeInfo*& out_type,
              const reflect::FieldInfo*& out_field) {
    out_field = nullptr;
    while (!path.empty()) {
        const usize dot = path.find('.');
        const std::string_view part = path.substr(0, dot);
        path = dot == std::string_view::npos ? std::string_view() : path.substr(dot + 1);
        if (type->kind == Kind::Struct) {
            const reflect::FieldInfo* f = type->find_field(part);
            if (!f) return nullptr;
            object = static_cast<u8*>(object) + f->offset;
            type = f->type;
            out_field = f;
        } else if (type->kind == Kind::Array) {
            char* end = nullptr;
            const std::string s(part);
            const unsigned long long index = std::strtoull(s.c_str(), &end, 10);
            if (s.empty() || *end != 0 || index >= type->array.size(object)) return nullptr;
            object = static_cast<u8*>(type->array.data(object)) + index * type->element->size;
            type = type->element;
            out_field = nullptr;
        } else {
            return nullptr;
        }
    }
    out_type = type;
    return object;
}

bool parse_hex_color(std::string_view text, Color& out) {
    text = trim(text);
    if (!text.empty() && text.front() == '#') text.remove_prefix(1);
    if (text.size() != 6 && text.size() != 8) return false;
    f32 c[4] = {0, 0, 0, 1};
    for (usize i = 0; i < text.size() / 2; ++i) {
        const std::string byte(text.substr(i * 2, 2));
        char* end = nullptr;
        const unsigned long v = std::strtoul(byte.c_str(), &end, 16);
        if (*end != 0) return false;
        c[i] = static_cast<f32>(v) / 255.0f;
    }
    out = {c[0], c[1], c[2], c[3]};
    return true;
}

} // namespace

void describe_fields(const TypeInfo* type, const void* object, std::vector<FieldRow>& out) {
    if (type && type->kind == Kind::Struct) describe(type, object, {}, 0, out);
}

std::string value_text(const TypeInfo* type, const void* p) {
    switch (type->kind) {
    case Kind::Bool: return at<bool>(p) ? "true" : "false";
    case Kind::F32: return number(at<f32>(p));
    case Kind::F64: return number(at<f64>(p));
    case Kind::String: return at<std::string>(p);
    case Kind::Guid: return at<Guid>(p).to_string();
    case Kind::Vec2: return number(at<Vec2>(p).x) + ", " + number(at<Vec2>(p).y);
    case Kind::Color: {
        const Color& c = at<Color>(p);
        auto byte = [](f32 v) { return static_cast<unsigned>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
        char buf[16];
        std::snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", byte(c.r), byte(c.g), byte(c.b), byte(c.a));
        return buf;
    }
    case Kind::Enum: {
        const i64 v = read_enum(type, p);
        const reflect::EnumValue* e = type->find_enum(v);
        return e ? e->name : std::to_string(v);
    }
    case Kind::Struct:
    case Kind::Array: return {};
    default: return std::to_string(read_int(type, p));
    }
}

bool set_field_text(const TypeInfo* type, void* object, std::string_view path, std::string_view text,
                    std::string* error) {
    auto fail = [&](std::string message) {
        if (error) *error = std::move(message);
        return false;
    };
    const TypeInfo* ft = nullptr;
    const reflect::FieldInfo* field = nullptr;
    void* p = resolve(type, object, path, ft, field);
    if (!p) return fail("нет поля " + std::string(path));
    if (field && (field->flags & reflect::FieldReadOnly)) return fail("поле только для чтения");

    auto clamp = [&](f64 v) { return field && field->has_range ? std::clamp(v, field->min, field->max) : v; };
    f64 number_value = 0;
    switch (ft->kind) {
    case Kind::Bool: {
        const std::string_view t = trim(text);
        if (t == "true" || t == "1") at<bool>(p) = true;
        else if (t == "false" || t == "0") at<bool>(p) = false;
        else return fail("ожидается true или false");
        return true;
    }
    case Kind::F32:
        if (!parse_f64(text, number_value)) return fail("ожидается число");
        at<f32>(p) = static_cast<f32>(clamp(number_value));
        return true;
    case Kind::F64:
        if (!parse_f64(text, number_value)) return fail("ожидается число");
        at<f64>(p) = clamp(number_value);
        return true;
    case Kind::String: at<std::string>(p) = std::string(text); return true;
    case Kind::Guid: {
        auto g = Guid::parse(trim(text));
        if (!g) return fail("ожидается GUID");
        at<Guid>(p) = *g;
        return true;
    }
    case Kind::Vec2: {
        // "x, y", "x; y" or "x y" (so a decimal comma cannot be used here).
        std::vector<std::string> parts;
        std::string current;
        for (char ch : text) {
            if (ch == ',' || ch == ';' || ch == ' ' || ch == '\t') {
                if (!current.empty()) parts.push_back(std::move(current));
                current.clear();
            } else {
                current += ch;
            }
        }
        if (!current.empty()) parts.push_back(std::move(current));
        f64 x = 0, y = 0;
        if (parts.size() != 2 || !parse_f64(parts[0], x) || !parse_f64(parts[1], y)) return fail("ожидается «x, y»");
        at<Vec2>(p) = {static_cast<f32>(clamp(x)), static_cast<f32>(clamp(y))};
        return true;
    }
    case Kind::Color: {
        Color c;
        if (!parse_hex_color(text, c)) return fail("ожидается цвет #rrggbb или #rrggbbaa");
        at<Color>(p) = c;
        return true;
    }
    case Kind::Enum: {
        const std::string_view t = trim(text);
        if (const reflect::EnumValue* e = ft->find_enum(t)) {
            write_enum(ft, p, e->value);
            return true;
        }
        return fail("нет варианта " + std::string(t));
    }
    case Kind::Struct:
    case Kind::Array: return fail("составное поле правится по частям");
    default:
        if (is_integer(ft->kind)) {
            if (!parse_f64(text, number_value)) return fail("ожидается целое число");
            write_int(ft, p, static_cast<i64>(std::llround(clamp(number_value))));
            return true;
        }
        return fail("неизвестный тип поля");
    }
}

} // namespace forge::editor
