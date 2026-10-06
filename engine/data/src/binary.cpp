#include "forge/data/binary.h"

#include "forge/core/profile.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace forge::data {

using reflect::FieldInfo;
using reflect::Kind;
using reflect::TypeInfo;
using reflect::TypeOf;

// Cooked data is written and read on little-endian machines (x86-64, ARM64).
static_assert(std::endian::native == std::endian::little);

namespace {

void put(std::vector<u8>& out, const void* data, usize size) {
    const auto* p = static_cast<const u8*>(data);
    out.insert(out.end(), p, p + size);
}

template <typename T>
void put(std::vector<u8>& out, T value) {
    put(out, &value, sizeof(T));
}

// Raw values have a fixed size; everything else is length-prefixed.
bool is_raw(const TypeInfo* t) {
    switch (t->kind) {
    case Kind::String:
    case Kind::Array: return false;
    case Kind::Struct: return t->trivially_copyable;
    default: return true;
    }
}

// Smallest number of bytes one value can take; bounds element counts read
// from untrusted input before anything is allocated.
usize min_encoded_size(const TypeInfo* t) {
    if (is_raw(t)) return t->size;
    if (t->kind != Kind::Struct) return 4; // String, Array: a u32 length
    usize total = 0;
    for (const FieldInfo& f : t->fields) {
        if (!(f.flags & reflect::FieldTransient)) total += min_encoded_size(f.type);
    }
    return total;
}

void write_value(const TypeInfo* t, const void* p, std::vector<u8>& out) {
    if (is_raw(t)) {
        put(out, p, t->size);
        return;
    }
    switch (t->kind) {
    case Kind::String: {
        const auto& s = *static_cast<const std::string*>(p);
        put(out, static_cast<u32>(s.size()));
        put(out, s.data(), s.size());
        return;
    }
    case Kind::Array: {
        const usize n = t->array.size(p);
        const auto* data = static_cast<const u8*>(t->array.data_const(p));
        put(out, static_cast<u32>(n));
        if (is_raw(t->element)) {
            put(out, data, n * t->element->size); // the fast path: one copy for the whole array
        } else {
            for (usize i = 0; i < n; ++i) write_value(t->element, data + i * t->element->size, out);
        }
        return;
    }
    case Kind::Struct: {
        const auto* base = static_cast<const u8*>(p);
        for (const FieldInfo& f : t->fields) {
            if (!(f.flags & reflect::FieldTransient)) write_value(f.type, base + f.offset, out);
        }
        return;
    }
    default: return;
    }
}

struct Cursor {
    const u8* pos;
    const u8* end;

    usize left() const { return static_cast<usize>(end - pos); }

    bool take(void* dst, usize size) {
        if (left() < size) return false;
        std::memcpy(dst, pos, size);
        pos += size;
        return true;
    }
};

BinaryError read_value(const TypeInfo* t, void* p, Cursor& in) {
    if (is_raw(t)) {
        if (!in.take(p, t->size)) return BinaryError::Truncated;
        if (t->kind == Kind::Bool && *static_cast<const u8*>(p) > 1) return BinaryError::Corrupt;
        return BinaryError::None;
    }
    switch (t->kind) {
    case Kind::String: {
        u32 len;
        if (!in.take(&len, 4)) return BinaryError::Truncated;
        if (len > in.left()) return BinaryError::Truncated;
        static_cast<std::string*>(p)->assign(reinterpret_cast<const char*>(in.pos), len);
        in.pos += len;
        return BinaryError::None;
    }
    case Kind::Array: {
        u32 n;
        if (!in.take(&n, 4)) return BinaryError::Truncated;
        const usize min = min_encoded_size(t->element);
        if (min > 0 && n > in.left() / min) return BinaryError::Corrupt;
        t->array.resize(p, n);
        auto* data = static_cast<u8*>(t->array.data(p));
        if (is_raw(t->element)) {
            if (!in.take(data, static_cast<usize>(n) * t->element->size)) return BinaryError::Truncated;
            return BinaryError::None;
        }
        for (u32 i = 0; i < n; ++i) {
            const BinaryError e = read_value(t->element, data + static_cast<usize>(i) * t->element->size, in);
            if (e != BinaryError::None) return e;
        }
        return BinaryError::None;
    }
    case Kind::Struct: {
        auto* base = static_cast<u8*>(p);
        for (const FieldInfo& f : t->fields) {
            if (f.flags & reflect::FieldTransient) continue;
            const BinaryError e = read_value(f.type, base + f.offset, in);
            if (e != BinaryError::None) return e;
        }
        return BinaryError::None;
    }
    default: return BinaryError::Corrupt;
    }
}

} // namespace

const char* binary_error_name(BinaryError error) {
    switch (error) {
    case BinaryError::None: return "none";
    case BinaryError::Truncated: return "truncated";
    case BinaryError::BadMagic: return "not Forge binary data";
    case BinaryError::WrongType: return "different type";
    case BinaryError::StaleSchema: return "stale schema, rebuild from source";
    case BinaryError::Corrupt: return "corrupt";
    }
    return "?";
}

void append_binary(const TypeInfo* type, const void* object, std::vector<u8>& out) {
    write_value(type, object, out);
}

std::vector<u8> to_binary(const TypeInfo* type, const void* object) {
    FORGE_ZONE();
    std::vector<u8> out(sizeof(BinaryHeader));
    write_value(type, object, out);
    BinaryHeader h;
    h.type_id = type->id;
    h.schema_hash = type->schema_hash();
    h.payload_size = out.size() - sizeof(BinaryHeader);
    std::memcpy(out.data(), &h, sizeof(h));
    return out;
}

BinaryError read_binary_payload(const TypeInfo* type, void* object, std::span<const u8>& bytes) {
    Cursor in{bytes.data(), bytes.data() + bytes.size()};
    const BinaryError e = read_value(type, object, in);
    bytes = bytes.subspan(static_cast<usize>(in.pos - bytes.data()));
    return e;
}

BinaryError from_binary(const TypeInfo* type, void* object, std::span<const u8> bytes) {
    FORGE_ZONE();
    BinaryHeader h;
    if (bytes.size() < sizeof(h)) return BinaryError::Truncated;
    std::memcpy(&h, bytes.data(), sizeof(h));
    if (h.magic != BinaryHeader::kMagic || h.format != BinaryHeader::kFormat) return BinaryError::BadMagic;
    if (h.type_id != type->id) return BinaryError::WrongType;
    if (h.schema_hash != type->schema_hash()) return BinaryError::StaleSchema;
    if (h.payload_size != bytes.size() - sizeof(h)) return BinaryError::Truncated;

    std::span<const u8> payload = bytes.subspan(sizeof(h));
    const BinaryError e = read_binary_payload(type, object, payload);
    if (e == BinaryError::None && !payload.empty()) return BinaryError::Corrupt; // trailing bytes
    return e;
}

// --- older layouts -------------------------------------------------------------

namespace {

constexpr u32 kMaxSchemaDepth = 32;

void put_name(std::vector<u8>& out, const std::string& s) {
    const u16 n = static_cast<u16>(std::min<usize>(s.size(), 0xFFFF));
    put(out, n);
    put(out, s.data(), n);
}

// [u8 kind][u32 size][u8 flags: 1 raw, 2 enum signed] then
// Struct: [u16 n] n × ([name][u32 offset][node]); Array: [node];
// Enum: [u16 n] n × ([name][i64 value]).
void write_schema(const TypeInfo* t, std::vector<u8>& out, u32 depth) {
    put(out, static_cast<u8>(t->kind));
    put(out, t->size);
    put(out, static_cast<u8>((is_raw(t) ? 1 : 0) | (t->enum_signed ? 2 : 0)));
    if (depth >= kMaxSchemaDepth) {
        if (t->kind == Kind::Struct || t->kind == Kind::Enum) put<u16>(out, 0);
        else if (t->kind == Kind::Array) write_schema(TypeOf<u8>::get(), out, depth); // never deeper
        return;
    }
    switch (t->kind) {
    case Kind::Struct: {
        u16 n = 0;
        for (const FieldInfo& f : t->fields) n = static_cast<u16>(n + ((f.flags & reflect::FieldTransient) ? 0 : 1));
        put(out, n);
        for (const FieldInfo& f : t->fields) {
            if (f.flags & reflect::FieldTransient) continue;
            put_name(out, f.name);
            put(out, f.offset);
            write_schema(f.type, out, depth + 1);
        }
        return;
    }
    case Kind::Array: write_schema(t->element, out, depth + 1); return;
    case Kind::Enum: {
        put(out, static_cast<u16>(t->enum_values.size()));
        for (const reflect::EnumValue& v : t->enum_values) {
            put_name(out, v.name);
            put(out, v.value);
        }
        return;
    }
    default: return;
    }
}

template <typename T>
bool take_span(std::span<const u8>& in, T& v) {
    if (in.size() < sizeof(T)) return false;
    std::memcpy(&v, in.data(), sizeof(T));
    in = in.subspan(sizeof(T));
    return true;
}

bool take_name(std::span<const u8>& in, std::string& s) {
    u16 n = 0;
    if (!take_span(in, n) || in.size() < n) return false;
    s.assign(reinterpret_cast<const char*>(in.data()), n);
    in = in.subspan(n);
    return true;
}

bool is_number(Kind k) { return k <= Kind::F64 || k == Kind::Enum; }

// A number of an old kind, as the widest type of its family.
struct Number {
    bool is_float = false;
    i64 i = 0;
    f64 f = 0;
};

Number load_number(Kind kind, u32 size, bool enum_signed, const u8* p) {
    Number n;
    auto load = [&](auto v) {
        std::memcpy(&v, p, sizeof(v));
        return v;
    };
    switch (kind) {
    case Kind::Bool: n.i = p[0] != 0; break;
    case Kind::I8: n.i = load(i8{}); break;
    case Kind::U8: n.i = load(u8{}); break;
    case Kind::I16: n.i = load(i16{}); break;
    case Kind::U16: n.i = load(u16{}); break;
    case Kind::I32: n.i = load(i32{}); break;
    case Kind::U32: n.i = load(u32{}); break;
    case Kind::I64: n.i = load(i64{}); break;
    case Kind::U64: n.i = static_cast<i64>(load(u64{})); break;
    case Kind::F32: n.is_float = true; n.f = load(f32{}); break;
    case Kind::F64: n.is_float = true; n.f = load(f64{}); break;
    case Kind::Enum:
        switch (size) {
        case 1: n.i = enum_signed ? load(i8{}) : load(u8{}); break;
        case 2: n.i = enum_signed ? load(i16{}) : load(u16{}); break;
        case 4: n.i = enum_signed ? load(i32{}) : load(u32{}); break;
        default: n.i = load(i64{}); break;
        }
        break;
    default: break;
    }
    return n;
}

void store_number(const TypeInfo* t, Number n, u8* p) {
    const i64 i = n.is_float ? static_cast<i64>(std::llround(n.f)) : n.i;
    const f64 f = n.is_float ? n.f : static_cast<f64>(n.i);
    auto store = [&](auto v) { std::memcpy(p, &v, sizeof(v)); };
    switch (t->kind) {
    case Kind::Bool: p[0] = (n.is_float ? n.f != 0 : n.i != 0) ? 1 : 0; break;
    case Kind::I8: store(static_cast<i8>(i)); break;
    case Kind::U8: store(static_cast<u8>(i)); break;
    case Kind::I16: store(static_cast<i16>(i)); break;
    case Kind::U16: store(static_cast<u16>(i)); break;
    case Kind::I32: store(static_cast<i32>(i)); break;
    case Kind::U32: store(static_cast<u32>(i)); break;
    case Kind::I64: store(i); break;
    case Kind::U64: store(static_cast<u64>(i)); break;
    case Kind::F32: store(static_cast<f32>(f)); break;
    case Kind::F64: store(f); break;
    case Kind::Enum:
        switch (t->size) {
        case 1: store(static_cast<u8>(i)); break;
        case 2: store(static_cast<u16>(i)); break;
        case 4: store(static_cast<u32>(i)); break;
        default: store(i); break;
        }
        break;
    default: break;
    }
}

const FieldInfo* field_for(const TypeInfo* t, const std::string& old_name) {
    for (const FieldInfo& f : t->fields)
        if (!(f.flags & reflect::FieldTransient) && f.name == old_name) return &f;
    for (const FieldInfo& f : t->fields)
        if (!(f.flags & reflect::FieldTransient) && !f.former_name.empty() && f.former_name == old_name) return &f;
    return nullptr;
}

} // namespace

void append_schema(const TypeInfo* type, std::vector<u8>& out) { write_schema(type, out, 0); }

bool SavedSchema::parse(std::span<const u8>& bytes) {
    nodes_.clear();
    u32 root = 0;
    return parse_node(bytes, 0, root);
}

bool SavedSchema::parse_node(std::span<const u8>& in, u32 depth, u32& index) {
    if (depth > kMaxSchemaDepth || nodes_.size() > 4096) return false;
    u8 kind = 0, flags = 0;
    u32 size = 0;
    if (!take_span(in, kind) || !take_span(in, size) || !take_span(in, flags)) return false;
    if (kind > static_cast<u8>(Kind::Array)) return false;
    index = static_cast<u32>(nodes_.size());
    nodes_.emplace_back();
    {
        Node& n = nodes_[index];
        n.kind = static_cast<Kind>(kind);
        n.size = size;
        n.raw = (flags & 1) != 0;
        n.enum_signed = (flags & 2) != 0;
        if (n.raw && size > (1u << 20)) return false;
    }
    switch (static_cast<Kind>(kind)) {
    case Kind::Struct: {
        u16 count = 0;
        if (!take_span(in, count)) return false;
        for (u16 k = 0; k < count; ++k) {
            Node::Field f;
            if (!take_name(in, f.name) || !take_span(in, f.offset)) return false;
            if (!parse_node(in, depth + 1, f.node)) return false;
            const Node& child = nodes_[f.node];
            if (nodes_[index].raw && (!child.raw || u64{f.offset} + child.size > size)) return false;
            nodes_[index].fields.push_back(std::move(f));
        }
        return true;
    }
    case Kind::Array: {
        u32 element = 0;
        if (!parse_node(in, depth + 1, element)) return false;
        nodes_[index].element = element;
        return true;
    }
    case Kind::Enum: {
        u16 count = 0;
        if (!take_span(in, count)) return false;
        for (u16 k = 0; k < count; ++k) {
            reflect::EnumValue v;
            if (!take_name(in, v.name) || !take_span(in, v.value)) return false;
            nodes_[index].values.push_back(std::move(v));
        }
        return true;
    }
    default: return true;
    }
}

namespace {

// Old raw bytes of `node` at `src` into the current type at `dst` (nullptr: dropped).
void convert_raw(const std::vector<SavedSchema::Node>& nodes, u32 node, const u8* src, const TypeInfo* t, u8* dst) {
    const SavedSchema::Node& n = nodes[node];
    if (!dst || !t) return;
    if (n.kind == Kind::Struct) {
        if (t->kind != Kind::Struct) return;
        for (const SavedSchema::Node::Field& f : n.fields) {
            const FieldInfo* to = field_for(t, f.name);
            if (!to || !is_raw(to->type)) continue;
            convert_raw(nodes, f.node, src + f.offset, to->type, dst + to->offset);
        }
        return;
    }
    if (is_number(n.kind) && is_number(t->kind)) {
        Number v = load_number(n.kind, n.size, n.enum_signed, src);
        if (n.kind == Kind::Enum && t->kind == Kind::Enum && !n.values.empty()) {
            // By name: values may have been renumbered.
            for (const reflect::EnumValue& ev : n.values) {
                if (ev.value != v.i) continue;
                if (const reflect::EnumValue* now = t->find_enum(ev.name)) v.i = now->value;
                else return; // a value the type no longer has: keep the default
                break;
            }
        }
        store_number(t, v, dst);
        return;
    }
    if (n.kind == t->kind && n.size == t->size) std::memcpy(dst, src, t->size); // Guid, Vec2, Color
}

BinaryError read_old(const std::vector<SavedSchema::Node>& nodes, u32 node, const TypeInfo* t, void* p, Cursor& in) {
    const SavedSchema::Node& n = nodes[node];
    if (n.raw) {
        if (in.left() < n.size) return BinaryError::Truncated;
        const u8* src = in.pos;
        in.pos += n.size;
        if (n.kind == Kind::Struct && t && t->kind == Kind::Struct && !is_raw(t)) {
            // Was a plain struct, now holds a string or an array: field by field.
            for (const SavedSchema::Node::Field& f : n.fields) {
                const FieldInfo* to = field_for(t, f.name);
                if (to && is_raw(to->type)) convert_raw(nodes, f.node, src + f.offset, to->type, static_cast<u8*>(p) + to->offset);
            }
            return BinaryError::None;
        }
        if (t && is_raw(t)) convert_raw(nodes, node, src, t, static_cast<u8*>(p));
        return BinaryError::None;
    }
    switch (n.kind) {
    case Kind::String: {
        u32 len;
        if (!in.take(&len, 4)) return BinaryError::Truncated;
        if (len > in.left()) return BinaryError::Truncated;
        if (t && t->kind == Kind::String) static_cast<std::string*>(p)->assign(reinterpret_cast<const char*>(in.pos), len);
        in.pos += len;
        return BinaryError::None;
    }
    case Kind::Array: {
        u32 count;
        if (!in.take(&count, 4)) return BinaryError::Truncated;
        const SavedSchema::Node& e = nodes[n.element];
        const usize min = e.raw ? e.size : 4;
        if (min > 0 && count > in.left() / min) return BinaryError::Corrupt;
        const bool keep = t && t->kind == Kind::Array;
        u8* data = nullptr;
        if (keep) {
            t->array.resize(p, count);
            data = static_cast<u8*>(t->array.data(p));
        }
        for (u32 i = 0; i < count; ++i) {
            const BinaryError err = read_old(nodes, n.element, keep ? t->element : nullptr,
                                             keep ? data + static_cast<usize>(i) * t->element->size : nullptr, in);
            if (err != BinaryError::None) return err;
        }
        return BinaryError::None;
    }
    case Kind::Struct: {
        const bool keep = t && t->kind == Kind::Struct;
        for (const SavedSchema::Node::Field& f : n.fields) {
            const FieldInfo* to = keep ? field_for(t, f.name) : nullptr;
            const BinaryError err = read_old(nodes, f.node, to ? to->type : nullptr,
                                             to ? static_cast<u8*>(p) + to->offset : nullptr, in);
            if (err != BinaryError::None) return err;
        }
        return BinaryError::None;
    }
    default: return BinaryError::Corrupt;
    }
}

} // namespace

BinaryError SavedSchema::read(const TypeInfo* type, void* object, std::span<const u8>& bytes) const {
    if (nodes_.empty()) return BinaryError::Corrupt;
    Cursor in{bytes.data(), bytes.data() + bytes.size()};
    const BinaryError e = read_old(nodes_, 0, type, object, in);
    bytes = bytes.subspan(static_cast<usize>(in.pos - bytes.data()));
    return e;
}

} // namespace forge::data
