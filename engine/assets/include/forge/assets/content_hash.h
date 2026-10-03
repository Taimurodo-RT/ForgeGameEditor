#pragma once

#include "forge/core/types.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace forge::assets {

// 128-bit content hash (XXH3). Decides whether a source file changed and
// names cooked results, so identical inputs are cooked once.
struct ContentHash {
    u64 lo = 0;
    u64 hi = 0;

    bool is_null() const { return lo == 0 && hi == 0; }
    std::string to_hex() const;
    friend bool operator==(const ContentHash&, const ContentHash&) = default;
};

ContentHash hash_bytes(std::span<const u8> bytes);
ContentHash hash_combine(ContentHash a, std::span<const u8> more);
std::optional<ContentHash> hash_file(const std::filesystem::path& path);

} // namespace forge::assets
