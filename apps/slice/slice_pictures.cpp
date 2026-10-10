#include "slice_pictures.h"

#include "forge/assets/image.h"
#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"

#include <algorithm>

namespace slice {

using namespace forge;
namespace fs = std::filesystem;

const Pictures::Decoded& Pictures::decode(const fs::path& file, fs::file_time_type mtime) {
    Decoded& d = cache_[path_to_utf8(file)];
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
    // Big pictures are scaled down (nearest: pixel art stays crisp).
    const u32 side = std::max(image.width, image.height);
    const f32 scale = side > kMaxSide ? static_cast<f32>(kMaxSide) / static_cast<f32>(side) : 1.0f;
    d.width = std::max(1u, static_cast<u32>(static_cast<f32>(image.width) * scale));
    d.height = std::max(1u, static_cast<u32>(static_cast<f32>(image.height) * scale));
    d.rgba.resize(static_cast<usize>(d.width) * d.height * 4);
    for (u32 y = 0; y < d.height; ++y)
        for (u32 x = 0; x < d.width; ++x) {
            const u32 sx = std::min(image.width - 1, static_cast<u32>(static_cast<f32>(x) / scale));
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
    if (signature == signature_ && !sheet.rgba.empty()) return false;
    signature_ = signature;
    by_key_.clear();
    hero_key_ = 0;

    // Shelves under the drawn frames, one pixel apart; the frames of a strip each a place of its own.
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
        const Decoded& d = decode(w.file, w.mtime);
        if (d.rgba.empty()) continue;
        u32 frames = w.frames;
        if (frames > 1 && d.width % frames != 0) {
            FORGE_WARN("картинка %s: ширина %u не делится на %u кадров, она рисуется целиком", path_to_utf8(w.file).c_str(),
                       d.width, frames);
            frames = 1;
        }
        const u32 fw = d.width / frames;
        if (x + d.width + frames > width) {
            x = 1;
            y += shelf + 1;
            shelf = 0;
        }
        if (y + d.height + 1 > 8192) {
            FORGE_WARN("картинки объектов не помещаются в лист, %s пропущена", path_to_utf8(w.file).c_str());
            continue;
        }
        made.push_back({w.key, {static_cast<u32>(base.frames.size() + places.size()), frames,
                                static_cast<f32>(fw) / static_cast<f32>(d.height)}});
        for (u32 f = 0; f < frames; ++f) {
            places.push_back({&d, f * fw, fw, x, y});
            x += fw + 1;
        }
        shelf = std::max(shelf, d.height);
        if (w.hero) {
            const objects::Template* t = library.find(w.key);
            if (frames != 1 && frames != 4)
                FORGE_WARN("у картинки героя %s %u кадров, а нужно 4 или 1: герой рисуется как прежде", path_to_utf8(w.file).c_str(),
                           frames);
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
    return true;
}

const Pictures::Picture* Pictures::of(u64 key) const {
    const auto it = by_key_.find(key);
    return it == by_key_.end() ? nullptr : &it->second;
}

} // namespace slice
