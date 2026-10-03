#pragma once

#include "forge/core/types.h"

#include <optional>
#include <string>
#include <string_view>

namespace forge {

// 128-bit globally unique id. Assets, scene entities and anything referenced
// from saves or other files are addressed by Guid, never by runtime ids.
struct Guid {
    u64 hi = 0;
    u64 lo = 0;

    static Guid generate(); // random (version 4 layout)
    static std::optional<Guid> parse(std::string_view text);

    bool is_null() const { return hi == 0 && lo == 0; }
    std::string to_string() const; // xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx

    friend bool operator==(const Guid&, const Guid&) = default;
    friend auto operator<=>(const Guid&, const Guid&) = default;
};

struct GuidHash {
    usize operator()(const Guid& g) const { return static_cast<usize>(g.hi ^ (g.lo * 0x9E3779B97F4A7C15ull)); }
};

} // namespace forge
