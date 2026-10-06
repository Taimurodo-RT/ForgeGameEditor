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

// --- reading data written by an older layout --------------------------------
//
// Saved worlds and levels must outlive changes to the types: a field added,
// removed, renamed (former_name), moved or widened. Next to such data goes a
// description of the layout it was written with (append_schema); a reader
// whose type has changed since reads the payload through it, field by field
// by name. Fields the type no longer has are skipped, new fields keep their
// defaults, numbers convert between kinds (i32 → f32, f32 → f64…), enum
// values are matched by name.

// Describes the layout append_binary writes for this type.
void append_schema(const reflect::TypeInfo* type, std::vector<u8>& out);

class SavedSchema {
public:
    // Parses a description written by append_schema; advances `bytes` past it.
    bool parse(std::span<const u8>& bytes);
    // Reads one payload written with this layout into `type`'s object.
    BinaryError read(const reflect::TypeInfo* type, void* object, std::span<const u8>& bytes) const;

    struct Node {
        reflect::Kind kind = reflect::Kind::Struct;
        u32 size = 0;
        bool raw = false;        // written as its memory bytes
        bool enum_signed = false;
        struct Field {
            std::string name;
            u32 offset = 0;
            u32 node = 0;
        };
        std::vector<Field> fields;              // Struct
        u32 element = 0;                        // Array
        std::vector<reflect::EnumValue> values; // Enum
    };

private:
    bool parse_node(std::span<const u8>& bytes, u32 depth, u32& index);
    std::vector<Node> nodes_; // nodes_[0]: the type itself
};

template <typename T>
std::vector<u8> to_binary(const T& object) {
    return to_binary(reflect::type_of<T>(), &object);
}

template <typename T>
BinaryError from_binary(T& object, std::span<const u8> bytes) {
    return from_binary(reflect::type_of<T>(), &object, bytes);
}

} // namespace forge::data
