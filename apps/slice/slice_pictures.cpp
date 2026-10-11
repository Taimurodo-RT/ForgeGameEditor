#include "slice_pictures.h"

#include "forge/assets/image.h"
#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"

#include <algorithm>

namespace slice {

using namespace forge;
namespace fs = std::filesystem;

bool hero_frames_ok(u32 frames, u32 width, std::string* why) {
    if (frames != 1 && frames != 4) {
        if (why) *why = "кадров в картинке " + std::to_string(frames) + ", а у героя их 4 (стоит, шаг, шаг, в воздухе) или 1";
        return false;
    }
    if (width % frames != 0) {
        if (why) *why = "ширина картинки " + std::to_string(width) + " не делится на 4 кадра";
        return false;
    }
    return true;
}

bool hero_picture_ok(const objects::Library& library, const objects::Template& t, std::string* why) {
    if (!library.has_block(t, "hero")) {
        if (why) *why = "не вид «Герой»";
        return false;
    }
    if (t.picture.empty()) {
        if (why) *why = "без картинки";
        return false;
    }
    std::vector<u8> bytes;
    assets::CookedTexture image;
    if (!read_file(library.picture_file(t), bytes) || !assets::decode_image(bytes, image) || !image.width || !image.height) {
        if (why) *why = "картинка " + t.picture + " не читается";
        return false;
    }
    return hero_frames_ok(t.frames, image.width, why);
}

const char* pose_id(Pose pose) {
    switch (pose) {
    case Pose::Stand: return "stand";
    case Pose::Walk: return "walk";
    case Pose::Air: return "air";
    case Pose::Idle: return "idle";
    }
    return "";
}

const char* pose_name(Pose pose) {
    switch (pose) {
    case Pose::Stand: return "Стоит";
    case Pose::Walk: return "Идёт";
    case Pose::Air: return "В воздухе";
    case Pose::Idle: return "Покой";
    }
    return "";
}

Mover mover_of(const objects::Library& library, const objects::Template& t) {
    if (library.has_block(t, "hero")) return Mover::Hero;
    if (library.has_block(t, "control")) return Mover::Walker;
    return Mover::Still;
}

std::vector<Pose> poses_of(Mover mover) {
    switch (mover) {
    case Mover::Hero: return {Pose::Stand, Pose::Walk, Pose::Air};
    case Mover::Walker: return {Pose::Stand, Pose::Walk};
    case Mover::Still: return {Pose::Idle};
    }
    return {};
}

objects::Clip pose_rule(Mover mover, Pose pose, u32 frames) {
    if (mover == Mover::Hero && frames == 4) {
        if (pose == Pose::Walk) return {{1, 2}, 10, true};
        if (pose == Pose::Air) return {{3}, 10, true};
    }
    if (mover == Mover::Walker && pose == Pose::Walk) {
        objects::Clip all{{}, 7.5f, true};
        for (u32 f = 0; f < std::max(1u, frames); ++f) all.frames.push_back(f);
        return all;
    }
    return {{0}, 10, true};
}

const Pictures::Decoded& Pictures::decode(const fs::path& file, fs::file_time_type mtime, u32 frames) {
    Decoded& d = cache_[path_to_utf8(file) + "/" + std::to_string(frames)];
    if (d.mtime == mtime && (d.width || !d.rgba.empty())) return d;
    d = {};
    d.mtime = mtime;
    std::vector<u8> bytes;
    assets::CookedTexture image;
    std::string error;
    if (!read_file(file, bytes) || !assets::decode_image(bytes, image, &error) || !image.width || !image.height) {
        FORGE_WARN("картинка объекта %s не читается %s", path_to_utf8(file).c_str(), error.c_str());
        return d;
    }
    d.file_width = image.width;
    if (frames > 1 && image.width % frames != 0) {
        FORGE_WARN("картинка %s: ширина %u не делится на %u кадров, она рисуется целиком", path_to_utf8(file).c_str(), image.width,
                   frames);
        frames = 1;
    }
    d.frames = frames;
    // Each frame scaled down when bigger (nearest: pixel art stays crisp), all by one scale.
    const u32 sw = image.width / frames;
    const u32 side = std::max(sw, image.height);
    const f32 scale = side > kMaxSide ? static_cast<f32>(kMaxSide) / static_cast<f32>(side) : 1.0f;
    const u32 fw = std::max(1u, static_cast<u32>(static_cast<f32>(sw) * scale));
    d.width = fw * frames;
    d.height = std::max(1u, static_cast<u32>(static_cast<f32>(image.height) * scale));
    d.rgba.resize(static_cast<usize>(d.width) * d.height * 4);
    for (u32 y = 0; y < d.height; ++y)
        for (u32 x = 0; x < d.width; ++x) {
            const u32 f = x / fw;
            const u32 sx = f * sw + std::min(sw - 1, static_cast<u32>(static_cast<f32>(x - f * fw) / scale));
            const u32 sy = std::min(image.height - 1, static_cast<u32>(static_cast<f32>(y) / scale));
            const u8* src = &image.rgba8[(static_cast<usize>(sy) * image.width + sx) * 4];
            std::copy(src, src + 4, &d.rgba[(static_cast<usize>(y) * d.width + x) * 4]);
        }
    return d;
}

bool Pictures::update(const objects::Library& library, const demo::SheetImage& base, demo::SheetImage& sheet) {
    struct Wanted {
        u64 key;
        fs::path file;
        fs::file_time_type mtime;
        u32 frames;
        bool hero;
    };
    std::vector<Wanted> wanted;
    std::string signature;
    for (const objects::Template& t : library.templates()) {
        if (t.picture.empty()) continue;
        const fs::path file = library.picture_file(t);
        std::error_code ec;
        const fs::file_time_type mtime = fs::last_write_time(file, ec);
        wanted.push_back({t.key, file, ec ? fs::file_time_type{} : mtime, t.frames, library.has_block(t, "hero")});
        signature += std::to_string(t.key) + "=" + path_to_utf8(file) + "@" +
                     std::to_string(wanted.back().mtime.time_since_epoch().count()) + "/" + std::to_string(t.frames) +
                     (wanted.back().hero ? "h" : "") + ";";
    }
    if (signature == signature_ && !sheet.rgba.empty()) {
        set_clips(library);
        return false;
    }
    signature_ = signature;
    by_key_.clear();
    hero_key_ = 0;

    // Shelves under the drawn frames, one pixel apart; each frame of a strip a place of its own, on the next shelf
    // when it does not fit on this one.
    struct Place {
        const Decoded* image;
        u32 sx, w; // its columns in the picture
        u32 x, y;  // where in the sheet
    };
    std::vector<Place> places;
    std::vector<std::pair<u64, Picture>> made;
    const u32 width = std::max(base.width, kMaxSide + 2);
    u32 x = 1, y = base.height + 1, shelf = 0;
    std::string hero_id;
    u32 heroes = 0;
    for (const Wanted& w : wanted) {
        const Decoded& d = decode(w.file, w.mtime, w.frames);
        if (d.rgba.empty()) continue;
        const u32 fw = d.width / d.frames;
        std::vector<Place> mine;
        u32 at_x = x, at_y = y, at_shelf = shelf;
        for (u32 f = 0; f < d.frames; ++f) {
            if (at_x + fw + 1 > width) {
                at_x = 1;
                at_y += at_shelf + 1;
                at_shelf = 0;
            }
            mine.push_back({&d, f * fw, fw, at_x, at_y});
            at_x += fw + 1;
            at_shelf = std::max(at_shelf, d.height);
        }
        if (at_y + d.height + 1 > kMaxHeight) {
            FORGE_WARN("картинки объектов не помещаются в лист, %s пропущена", path_to_utf8(w.file).c_str());
            continue;
        }
        x = at_x;
        y = at_y;
        shelf = at_shelf;
        made.push_back({w.key, {static_cast<u32>(base.frames.size() + places.size()), d.frames,
                                static_cast<f32>(fw) / static_cast<f32>(d.height)}});
        places.insert(places.end(), mine.begin(), mine.end());
        if (w.hero) {
            const objects::Template* t = library.find(w.key);
            std::string why;
            if (!hero_frames_ok(w.frames, d.file_width, &why))
                FORGE_WARN("картинка героя %s: %s, герой рисуется как прежде", path_to_utf8(w.file).c_str(), why.c_str());
            else if (t) {
                ++heroes;
                if (hero_id.empty() || t->id < hero_id) {
                    hero_id = t->id;
                    hero_key_ = w.key;
                }
            }
        }
    }
    if (heroes > 1)
        FORGE_WARN("объектов вида «Герой» с картинкой %u: герой рисуется картинкой «%s», первого по id", heroes, hero_id.c_str());
    const u32 height = places.empty() ? base.height : y + shelf + 1;

    sheet.width = width;
    sheet.height = height;
    sheet.rgba.assign(static_cast<usize>(width) * height * 4, 0);
    for (u32 row = 0; row < base.height; ++row)
        std::copy_n(&base.rgba[static_cast<usize>(row) * base.width * 4], static_cast<usize>(base.width) * 4,
                    &sheet.rgba[static_cast<usize>(row) * width * 4]);
    sheet.frames = base.frames;
    for (const Place& p : places) {
        const Decoded& d = *p.image;
        for (u32 row = 0; row < d.height; ++row)
            std::copy_n(&d.rgba[(static_cast<usize>(row) * d.width + p.sx) * 4], static_cast<usize>(p.w) * 4,
                        &sheet.rgba[(static_cast<usize>(p.y + row) * width + p.x) * 4]);
        sheet.frames.push_back({p.x, p.y, p.w, d.height});
    }
    for (auto& [key, picture] : made) by_key_[key] = picture;
    set_clips(library);
    return true;
}

void Pictures::set_clips(const objects::Library& library) {
    for (auto& [key, picture] : by_key_) {
        const objects::Template* t = library.find(key);
        const Mover mover = t ? mover_of(library, *t) : Mover::Still;
        for (usize i = 0; i < kPoses; ++i) {
            const Pose pose = static_cast<Pose>(i);
            picture.clips[i] = pose_rule(mover, pose, picture.frames);
            if (!t) continue;
            const auto own = t->animations.find(pose_id(pose));
            if (own != t->animations.end() && objects::clip_problem(own->second, picture.frames).empty()) picture.clips[i] = own->second;
        }
    }
}

const Pictures::Picture* Pictures::of(u64 key) const {
    const auto it = by_key_.find(key);
    return it == by_key_.end() ? nullptr : &it->second;
}

} // namespace slice
