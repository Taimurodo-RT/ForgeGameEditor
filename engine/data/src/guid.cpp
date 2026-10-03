#include "forge/data/guid.h"

#include <random>

namespace forge {

Guid Guid::generate() {
    // One generator per thread, seeded from the OS: no locking, no repeats
    // across threads.
    thread_local std::mt19937_64 rng{[] {
        std::random_device rd;
        return (static_cast<u64>(rd()) << 32) ^ rd();
    }()};
    Guid g{rng(), rng()};
    g.hi = (g.hi & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull; // version 4
    g.lo = (g.lo & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull; // RFC 4122 variant
    return g;
}

std::string Guid::to_string() const {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    auto put = [&](u64 v, int nibbles) {
        for (int i = nibbles - 1; i >= 0; --i) out.push_back(kHex[(v >> (i * 4)) & 0xF]);
    };
    put(hi >> 32, 8);
    out.push_back('-');
    put(hi >> 16, 4);
    out.push_back('-');
    put(hi, 4);
    out.push_back('-');
    put(lo >> 48, 4);
    out.push_back('-');
    put(lo, 12);
    return out;
}

std::optional<Guid> Guid::parse(std::string_view text) {
    if (text.size() != 36 || text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-') {
        return std::nullopt;
    }
    Guid g;
    int nibble = 0;
    for (char c : text) {
        if (c == '-') continue;
        u64 v;
        if (c >= '0' && c <= '9') v = static_cast<u64>(c - '0');
        else if (c >= 'a' && c <= 'f') v = static_cast<u64>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = static_cast<u64>(c - 'A' + 10);
        else return std::nullopt;
        u64& half = nibble < 16 ? g.hi : g.lo;
        half = (half << 4) | v;
        ++nibble;
    }
    return g;
}

} // namespace forge
