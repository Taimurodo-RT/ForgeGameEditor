#include "forge/level/level.h"

#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/core/time.h"

namespace forge::level {

namespace fs = std::filesystem;

Level::Level(LevelModule& module) : module_(module) {}

Level::~Level() {
    if (world_ && scene_) module_.level_closing(*this);
    // Entities go before the world they listen to.
    scene_.reset();
    world_.reset();
}

bool Level::open(const fs::path& folder, std::string* error) {
    if (world_ && scene_) module_.level_closing(*this);
    scene_.reset();
    world_.reset();
    folder_ = folder;
    world_ = std::make_unique<world::World>(module_.world_desc(), module_.generator());
    if (!folder.empty() && !world_->open_save(folder, error)) return false;
    scene_ = std::make_unique<scene::Scene>(*world_);
    module_.setup_scene(*scene_);
    if (!folder.empty() && !scene_->open_save(folder, error)) return false;
    edits_ = 0;
    return true;
}

void Level::update(std::span<const world::Rect> focus) {
    focus_.assign(focus.begin(), focus.end());
    scene_->update();
    world_->update(focus_);
}

void Level::ensure_loaded(const world::Rect& tiles) {
    std::vector<world::Rect> rects = focus_;
    rects.push_back(tiles);
    // Loads finish on job threads; the next update() hands them to the
    // scene and the renderers, so the area is whole after the second one.
    for (int round = 0; round < 2; ++round) {
        world_->update(rects);
        world_->finish_loading();
    }
    world_->update(rects);
    scene_->update();
}

Level::SaveReport Level::save() {
    SaveReport r;
    const u64 start = time_now_ns();
    const world::SaveReport w = world_->save();
    const scene::SceneSaveReport s = scene_->save();
    r.ok = w.ok && s.ok;
    r.tile_chunks = w.chunks;
    r.object_chunks = s.chunks;
    r.ms = ns_to_ms(time_now_ns() - start);
    return r;
}

bool Level::set_tile(u32 layer, i32 x, i32 y, world::TileId value) {
    if (world_->tile(layer, x, y) == value && loaded(x, y)) return true;
    if (!world_->set_tile(layer, x, y, value)) return false;
    ++edits_;
    return true;
}

bool Level::loaded(i32 x, i32 y) const { return world_->find_chunk(world::chunk_of(x, y)) != nullptr; }

bool copy_level(const fs::path& level, const fs::path& to, std::string* error) {
    std::error_code ec;
    if (level.empty() || !fs::is_directory(level, ec)) return true;
    fs::create_directories(to, ec);
    if (ec) {
        if (error) *error = "не удалось создать папку " + path_to_utf8(to);
        return false;
    }
    for (const fs::directory_entry& e : fs::directory_iterator(level, ec)) {
        if (!e.is_regular_file()) continue;
        fs::copy_file(e.path(), to / e.path().filename(), fs::copy_options::overwrite_existing, ec);
        if (ec) {
            if (error) *error = "не удалось скопировать " + path_to_utf8(e.path().filename());
            return false;
        }
    }
    return true;
}

} // namespace forge::level
