#pragma once

// Cooked form of game data: what a shipped game loads. Compact and fast, with
// whole arrays of plain types copied in one go. It is not meant to survive
// schema changes: the header carries the type's schema hash, and data written
// by a different layout is refused as stale so the asset pipeline rebuilds it
// from the JSON source. Corrupt or truncated input is detected, never trusted.

#include "forge/data/reflect.h"

#include <span>
#include <vector>

namespace forge::data {

struct BinaryHeader {
    static constexpr u32 kMagic = 0x31424746; // "FGB1"
    static constexpr u16 kFormat = 1;

    u32 magic = kMagic;
    u16 format = kFormat;
    u16 flags = 0;
    u64 type_id = 0;
    u64 schema_hash = 0;
    u64 payload_size = 0;
};
static_assert(sizeof(BinaryHeader) == 32);

enum class BinaryError {
    None,
    Truncated,   // input ends early
    BadMagic,    // not Forge binary data
    WrongType,   // holds a different type
    StaleSchema, // written by another layout of this type: rebuild from source
    Corrupt,     // sizes or values that cannot be right
};

const char* binary_error_name(BinaryError error);

std::vector<u8> to_binary(const reflect::TypeInfo* type, const void* object);
// Payload only, no header: for packing many objects into one cooked blob.
void append_binary(const reflect::TypeInfo* type, const void* object, std::vector<u8>& out);

BinaryError from_binary(const reflect::TypeInfo* type, void* object, std::span<const u8> bytes);
// Reads one payload written by append_binary; advances `bytes` past it.
BinaryError read_binary_payload(const reflect::TypeInfo* type, void* object, std::span<const u8>& bytes);

template <typename T>
std::vector<u8> to_binary(const T& object) {
    return to_binary(reflect::type_of<T>(), &object);
}

template <typename T>
BinaryError from_binary(T& object, std::span<const u8> bytes) {
    return from_binary(reflect::type_of<T>(), &object, bytes);
}

} // namespace forge::data
