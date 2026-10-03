#include "forge/assets/content_hash.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <cstdio>
#include <memory>

namespace forge::assets {

namespace {
ContentHash from_xxh(XXH128_hash_t h) { return {h.low64, h.high64}; }
} // namespace

std::string ContentHash::to_hex() const {
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx", static_cast<unsigned long long>(hi),
                  static_cast<unsigned long long>(lo));
    return buf;
}

ContentHash hash_bytes(std::span<const u8> bytes) { return from_xxh(XXH3_128bits(bytes.data(), bytes.size())); }

ContentHash hash_combine(ContentHash a, std::span<const u8> more) {
    XXH128_hash_t seed{a.lo, a.hi};
    XXH3_state_t* state = XXH3_createState();
    XXH3_128bits_reset(state);
    XXH3_128bits_update(state, &seed, sizeof(seed));
    XXH3_128bits_update(state, more.data(), more.size());
    const ContentHash out = from_xxh(XXH3_128bits_digest(state));
    XXH3_freeState(state);
    return out;
}

std::optional<ContentHash> hash_file(const std::filesystem::path& path) {
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> file(
#if defined(_WIN32)
        _wfopen(path.c_str(), L"rb"),
#else
        std::fopen(path.c_str(), "rb"),
#endif
        &std::fclose);
    if (!file) return std::nullopt;

    XXH3_state_t* state = XXH3_createState();
    XXH3_128bits_reset(state);
    // Streamed in blocks, so a 2 GB video costs 256 KB of memory to hash.
    static constexpr usize kBlock = 256 * 1024;
    std::unique_ptr<u8[]> buffer(new u8[kBlock]);
    usize n;
    while ((n = std::fread(buffer.get(), 1, kBlock, file.get())) > 0) XXH3_128bits_update(state, buffer.get(), n);
    const bool failed = std::ferror(file.get()) != 0;
    const ContentHash out = from_xxh(XXH3_128bits_digest(state));
    XXH3_freeState(state);
    if (failed) return std::nullopt;
    return out;
}

} // namespace forge::assets
