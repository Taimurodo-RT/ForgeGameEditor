#pragma once

#include <cstddef>
#include <cstdint>

namespace forge {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;
using usize = std::size_t;

inline constexpr usize KiB = 1024;
inline constexpr usize MiB = 1024 * KiB;
inline constexpr usize GiB = 1024 * MiB;

// Size of a cache line; used to keep data written by different threads apart.
inline constexpr usize kCacheLine = 64;

} // namespace forge
