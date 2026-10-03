#pragma once

#include "forge/core/types.h"

#include <string_view>

namespace forge {

// FNV-1a, 64-bit. Small and constexpr, used for names and ids known at
// compile time. Bulk content hashing uses xxHash in the asset pipeline.
inline constexpr u64 kFnvOffset = 0xcbf29ce484222325ull;
inline constexpr u64 kFnvPrime = 0x100000001b3ull;

constexpr u64 fnv1a(std::string_view text, u64 seed = kFnvOffset) {
    u64 h = seed;
    for (char c : text) {
        h ^= static_cast<u8>(c);
        h *= kFnvPrime;
    }
    return h;
}

constexpr u64 fnv1a_u64(u64 value, u64 seed) {
    u64 h = seed;
    for (int i = 0; i < 8; ++i) {
        h ^= (value >> (i * 8)) & 0xff;
        h *= kFnvPrime;
    }
    return h;
}

} // namespace forge
