#include "forge/data/binary.h"

#include "forge/core/profile.h"

#include <bit>
#include <cstring>

namespace forge::data {

using reflect::FieldInfo;
using reflect::Kind;
using reflect::TypeInfo;

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

} // namespace forge::data
