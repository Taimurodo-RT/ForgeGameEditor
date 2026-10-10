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

// Whether a picture of `frames` frames (Template::frames) `width` pixels wide can be the hero's: four frames its width
// divides (stands, step, step, in the air), or one. why: what is wrong otherwise, for the log and the editor.
bool hero_frames_ok(u32 frames, u32 width, std::string* why = nullptr);
// The same of a template, its picture read: a template of kind «Герой» (block hero) with a picture that reads and
// hero_frames_ok. The game draws the hero by the first such template by id; the editor tells which one it is.
bool hero_picture_ok(const forge::objects::Library& library, const forge::objects::Template& t, std::string* why = nullptr);

class Pictures {
public:
    // A picture of frames (Template::frames: a strip of equal frames side by side) is that many frames of the sheet,
    // one after another; a strip whose width the frames do not divide is one frame (and a warning).
    struct Picture {
        u32 frame = 0;  // the first
        u32 frames = 1; // how many
        f32 aspect = 1; // width / height of one
    };
    // The largest side a frame keeps in the sheet; bigger ones are scaled down. The sheet is at least kMaxSide + 2
    // wide, so any frame fits on a shelf of its own with a pixel either side.
    static constexpr u32 kMaxSide = 256;
    // The highest the sheet grows; pictures past it are left out (and a warning).
    static constexpr u32 kMaxHeight = 8192;

    // Makes sheet = base + the templates' pictures. False when nothing
    // changed since the last call (same templates, same files).
    bool update(const forge::objects::Library& library, const forge::demo::SheetImage& base,
                forge::demo::SheetImage& sheet);
    const Picture* of(u64 key) const;
    // The hero's picture: of the templates hero_picture_ok, the first by id; null without one (the hero is drawn as
    // before then).
    const Picture* hero() const { return hero_key_ ? of(hero_key_) : nullptr; }
    bool empty() const { return by_key_.empty(); }
    usize count() const { return by_key_.size(); }

private:
    // A picture cut into its frames, each scaled down to kMaxSide if bigger: the frames side by side, as many as it
    // has (1 when the width of the file does not divide into the frames asked for).
    struct Decoded {
        std::filesystem::file_time_type mtime{};
        u32 file_width = 0; // of the file, before it was cut and scaled
        u32 frames = 1;
        u32 width = 0, height = 0; // of the frames side by side
        std::vector<u8> rgba;      // empty: could not be read
    };
    const Decoded& decode(const std::filesystem::path& file, std::filesystem::file_time_type mtime, u32 frames);

    std::unordered_map<std::string, Decoded> cache_; // by file and frames
    std::unordered_map<u64, Picture> by_key_;
    u64 hero_key_ = 0;
    std::string signature_ = "-"; // what the last sheet was made from
};

} // namespace slice
