#include "forge/level/level.h"

#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/core/time.h"
#include "forge/world/generators.h"

#include <algorithm>
#include <cmath>
#include <random>

FORGE_REFLECT(forge::level::LevelId, 1) { t.field("id", &forge::level::LevelId::id); }

namespace forge::level {

namespace fs = std::filesystem;

Level::Level(LevelModule& module) : module_(module) {}

Level::~Level() {
    if (world_ && scene_) module_.level_closing(*this);
    ids_ = {};
    // Entities go before the world they listen to.
    scene_.reset();
    world_.reset();
}

bool Level::open(const fs::path& folder, std::string* error) {
    if (world_ && scene_) module_.level_closing(*this);
    ids_ = {};
    scene_.reset();
    world_.reset();
    folder_ = folder;
    // What is around comes first: the world is made with it. A world.json
    // that cannot be used leaves the game's world around (the file stays).
    around_ = {};
    around_error_.clear();
    if (!folder.empty() && !load_world(folder, around_, nullptr, &around_error_))
        FORGE_WARN("Мир вокруг уровня: %s; вокруг мир игры, файл не тронут", around_error_.c_str());
    std::shared_ptr<const world::Generator> generator = module_.generator();
    if (around_.empty_around) generator = std::make_shared<world::EmptyGenerator>();
    world_ = std::make_unique<world::World>(module_.world_desc(), std::move(generator));
    if (!folder.empty() && !world_->open_save(folder, error)) return false;
    scene_ = std::make_unique<scene::Scene>(*world_);
    module_.setup_scene(*scene_);
    // Nothing around: nobody comes to live there either.
    if (around_.empty_around) scene_->set_populator({});
    scene_->register_component<LevelId>();
    scene_->register_component<LightSource>();
    ids_ = scene_->ecs().query<LevelId>();
    if (!folder.empty() && !scene_->open_save(folder, error)) return false;
    edits_ = 0;
    object_edits_ = 0;
    // A physics.json that cannot be used leaves the game's pull; the author
    // is told, and the file stays as it is until the pull is changed.
    physics_ = module_.default_physics();
    physics_error_.clear();
    if (!folder.empty() && !load_physics(folder, physics_, nullptr, &physics_error_))
        FORGE_WARN("Физика уровня: %s; действует гравитация игры", physics_error_.c_str());
    physics_saved_ = physics_;
    // The same for light.json: noon meanwhile.
    light_ = {};
    light_error_.clear();
    if (!folder.empty() && !load_light(folder, light_, nullptr, &light_error_))
        FORGE_WARN("Свет уровня: %s; действует полдень", light_error_.c_str());
    light_saved_ = light_;
    // And for areas.json: no areas meanwhile, nothing written over it until
    // the areas change.
    areas_ = {};
    areas_error_.clear();
    if (!folder.empty() && !load_areas(folder, areas_, nullptr, &areas_error_))
        FORGE_WARN("Зоны уровня: %s; зон нет, файл не тронут", areas_error_.c_str());
    areas_saved_ = areas_;
    ++areas_version_;
    // And for tiles.json: no own tiles meanwhile (their cells show nothing
    // and keep their values), the files untouched until the tiles change.
    own_tiles_ = {};
    own_tiles_error_.clear();
    const u32 layers = static_cast<u32>(module_.layer_names().size());
    if (!folder.empty() && !load_tiles(folder, own_tiles_, layers, module_.liquids_layer(), nullptr, &own_tiles_error_))
        FORGE_WARN("Тайлы уровня: %s; своих тайлов нет, файл не тронут", own_tiles_error_.c_str());
    own_tiles_saved_ = own_tiles_;
    ++own_tiles_version_;
    return true;
}

bool Level::set_own_tiles(const LevelTiles& t) {
    if (!tiles_problem(t, static_cast<u32>(module_.layer_names().size()), module_.liquids_layer()).empty()) return false;
    if (t == own_tiles_) return true;
    own_tiles_ = t;
    ++own_tiles_version_;
    ++edits_;
    return true;
}

void Level::set_areas(const LevelAreas& a) {
    if (a == areas_) return;
    areas_ = a;
    ++areas_version_;
    ++edits_;
}

void Level::set_light(const LevelLight& l) {
    if (l == light_) return;
    light_ = l;
    ++edits_;
}

void Level::set_physics(const LevelPhysics& p) {
    if (p == physics_) return;
    physics_ = p;
    ++edits_;
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
    if (!r.ok) r.error = "не записались файлы участков в " + path_to_utf8(folder_);
    r.tile_chunks = w.chunks;
    r.object_chunks = s.chunks;
    if (physics_changed()) {
        std::string error;
        if (save_physics(folder_, physics_, &error)) {
            physics_saved_ = physics_;
            physics_error_.clear();
            r.physics = true;
        } else {
            r.ok = false;
            r.error = error;
        }
    }
    if (light_changed()) {
        std::string error;
        if (save_light(folder_, light_, &error)) {
            light_saved_ = light_;
            light_error_.clear();
            r.light = true;
        } else {
            r.ok = false;
            r.error = r.error.empty() ? error : r.error + "; " + error;
        }
    }
    if (areas_changed()) {
        std::string error;
        // A file that could not be read is kept aside, not lost.
        std::error_code ec;
        if (!areas_error_.empty() && fs::exists(folder_ / kAreasFile, ec)) {
            fs::rename(folder_ / kAreasFile, folder_ / kBrokenAreasFile, ec);
            if (ec) error = "не удалось отложить испорченный " + std::string(kAreasFile) + " в " + kBrokenAreasFile;
        }
        if (error.empty() && save_areas(folder_, areas_, &error)) {
            areas_saved_ = areas_;
            areas_error_.clear();
            r.areas = true;
        } else {
            r.ok = false;
            r.error = r.error.empty() ? error : r.error + "; " + error;
        }
    }
    if (own_tiles_changed()) {
        std::string error;
        // Files that could not be read are kept aside, not lost.
        std::error_code ec;
        if (!own_tiles_error_.empty()) {
            if (fs::exists(folder_ / kTilesFile, ec)) fs::rename(folder_ / kTilesFile, folder_ / kBrokenTilesFile, ec);
            if (!ec && fs::exists(folder_ / kTilesPicture, ec)) fs::rename(folder_ / kTilesPicture, folder_ / kBrokenTilesPicture, ec);
            if (ec) error = "не удалось отложить испорченные " + std::string(kTilesFile) + " и " + kTilesPicture;
        }
        if (error.empty() && save_tiles(folder_, own_tiles_, &error)) {
            own_tiles_saved_ = own_tiles_;
            own_tiles_error_.clear();
            r.own_tiles = true;
        } else {
            r.ok = false;
            r.error = r.error.empty() ? error : r.error + "; " + error;
        }
    }
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

flecs::entity Level::find(u64 id) {
    flecs::entity found;
    if (id == 0) return found;
    ids_.each([&](flecs::entity e, const LevelId& l) {
        if (l.id == id) found = e;
    });
    return found;
}

u64 Level::new_id() {
    // Random, so ids from two editing sessions never meet.
    static std::mt19937_64 rng{std::random_device{}() ^ static_cast<u64>(time_now_ns())};
    u64 id = 0;
    while (id == 0) id = rng();
    return id;
}

u64 Level::id_of(flecs::entity e, bool assign) {
    if (!e.is_alive()) return 0;
    if (const LevelId* l = e.try_get<LevelId>()) return l->id;
    if (!assign) return 0;
    const u64 id = new_id();
    e.set<LevelId>({id});
    return id;
}

flecs::entity Level::pick(f64 x, f64 y) {
    std::vector<flecs::entity_t> near;
    scene_->query_radius(x, y, 4.0f, near);
    flecs::entity best;
    f64 best_area = 0;
    for (flecs::entity_t id : near) {
        flecs::entity e(scene_->ecs(), id);
        if (module_.object_kind(e) < 0) continue;
        f64 x0, y0, x1, y1;
        if (!module_.object_box(e, x0, y0, x1, y1) || x < x0 || x >= x1 || y < y0 || y >= y1) continue;
        const f64 area = (x1 - x0) * (y1 - y0);
        if (!best.is_valid() || area < best_area) {
            best = e;
            best_area = area;
        }
    }
    return best;
}

void Level::objects_in(f64 x0, f64 y0, f64 x1, f64 y1, std::vector<flecs::entity>& out) {
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    const f64 cx = (x0 + x1) * 0.5, cy = (y0 + y1) * 0.5;
    const f32 radius = static_cast<f32>(std::hypot(x1 - x0, y1 - y0) * 0.5 + 4.0);
    std::vector<flecs::entity_t> near;
    scene_->query_radius(cx, cy, radius, near);
    for (flecs::entity_t id : near) {
        flecs::entity e(scene_->ecs(), id);
        if (module_.object_kind(e) < 0) continue;
        f64 a0, b0, a1, b1;
        if (module_.object_box(e, a0, b0, a1, b1) && a1 > x0 && a0 < x1 && b1 > y0 && b0 < y1) out.push_back(e);
    }
}

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
