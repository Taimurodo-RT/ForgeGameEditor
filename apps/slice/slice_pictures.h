#pragma once

// Templates' own pictures. Each template with a picture file gets one more
// frame in the game's sprite sheet, packed under the drawn ones; objects
// that are copies of it are drawn with it instead of their usual frame.

#include "demo_art.h"

#include "forge/core/types.h"
#include "forge/objects/library.h"

#include <array>
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

// The states the game plays a template's picture in (the tab «Анимация»), by the ids the template file has them.
enum class Pose : u8 { Stand, Walk, Air, Idle };
inline constexpr usize kPoses = 4;
const char* pose_id(Pose pose);   // "walk"
const char* pose_name(Pose pose); // «Идёт»
// What moves a template's copies, and so the poses it has: the hero (block hero: stands, walks, in the air), a walker
// (block control: stands, walks), anything else (idle).
enum class Mover : u8 { Hero, Walker, Still };
Mover mover_of(const forge::objects::Library& library, const forge::objects::Template& t);
std::vector<Pose> poses_of(Mover mover);
// The game's own rule for a pose of a picture of `frames` frames, what it draws when the template has no animation
// of its own for it: the hero stands in frame 0, walks in 1 and 2 by turns every 6 ticks, is in the air in 3 (of 4;
// of 1, frame 0 in all); a walker walks through all its frames every 8 ticks and stands in 0; anything else is frame 0.
forge::objects::Clip pose_rule(Mover mover, Pose pose, u32 frames);

class Pictures {
public:
    // A picture of frames (Template::frames: a strip of equal frames side by side) is that many frames of the sheet,
    // one after another; a strip whose width the frames do not divide is one frame (and a warning).
    struct Picture {
        u32 frame = 0;  // the first
        u32 frames = 1; // how many
        f32 aspect = 1; // width / height of one
        // What it plays in each pose (by Pose): the template's own animation when it can play on these frames, else
        // the game's rule (pose_rule).
        std::array<forge::objects::Clip, kPoses> clips;
        // Its frame of the sheet in a pose, `ticks` after the pose began.
        u32 at(Pose pose, u64 ticks) const { return frame + forge::objects::clip_frame(clips[static_cast<usize>(pose)], ticks); }
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
    // Each picture's clips from its template's animations as they are now (cheap: on every update).
    void set_clips(const forge::objects::Library& library);

    std::unordered_map<std::string, Decoded> cache_; // by file and frames
    std::unordered_map<u64, Picture> by_key_;
    u64 hero_key_ = 0;
    std::string signature_ = "-"; // what the last sheet was made from
};

} // namespace slice
