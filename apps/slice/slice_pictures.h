#pragma once

// Templates' own pictures. Each template with a picture file gets one more
// frame in the game's sprite sheet, packed under the drawn ones; objects
// that are copies of it are drawn with it instead of their usual frame.

#include "demo_art.h"

#include "forge/core/types.h"
#include "forge/objects/library.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace slice {

using forge::f32;
using forge::u32;
using forge::u64;
using forge::u8;
using forge::usize;

class Pictures {
public:
    struct Picture {
        u32 frame = 0;
        f32 aspect = 1; // width / height
    };
    // The largest side a picture keeps in the sheet; bigger ones are scaled down.
    static constexpr u32 kMaxSide = 256;

    // Makes sheet = base + the templates' pictures. False when nothing
    // changed since the last call (same templates, same files).
    bool update(const forge::objects::Library& library, const forge::demo::SheetImage& base,
                forge::demo::SheetImage& sheet);
    const Picture* of(u64 key) const;
    bool empty() const { return by_key_.empty(); }
    usize count() const { return by_key_.size(); }

private:
    struct Decoded {
        std::filesystem::file_time_type mtime{};
        u32 width = 0, height = 0;
        std::vector<u8> rgba; // empty: could not be read
    };
    const Decoded& decode(const std::filesystem::path& file, std::filesystem::file_time_type mtime);

    std::unordered_map<std::string, Decoded> cache_; // by file
    std::unordered_map<u64, Picture> by_key_;
    std::string signature_ = "-"; // what the last sheet was made from
};

} // namespace slice
