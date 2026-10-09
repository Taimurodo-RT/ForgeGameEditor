#include "forge/level/physics.h"

#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/data/json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

FORGE_REFLECT(forge::level::LevelPhysics, 1) {
    t.field("gravity_x", &forge::level::LevelPhysics::gravity_x);
    t.field("gravity_y", &forge::level::LevelPhysics::gravity_y);
}

namespace forge::level {

namespace fs = std::filesystem;

namespace {

constexpr const char* kPhysicsFile = "physics.json";
constexpr f64 kTick = 1.0 / 60.0;

} // namespace

bool valid_physics(const LevelPhysics& p) {
    auto ok = [](f32 v) { return std::isfinite(v) && std::fabs(v) <= kMaxGravity; };
    return ok(p.gravity_x) && ok(p.gravity_y);
}

bool load_physics(const fs::path& folder, LevelPhysics& out, bool* found, std::string* error) {
    if (found) *found = false;
    const fs::path file = folder / kPhysicsFile;
    std::error_code ec;
    if (!fs::exists(file, ec)) return true;
    if (found) *found = true;
    auto fail = [&](const std::string& why) {
        if (error) *error = "physics.json: " + why;
        return false;
    };
    std::vector<u8> bytes;
    if (!fs::is_regular_file(file, ec) || !read_file(file, bytes)) return fail("файл не читается");
    LevelPhysics p = out;
    data::LoadReport report;
    if (!data::from_json(p, {reinterpret_cast<const char*>(bytes.data()), bytes.size()}, report))
        return fail("файл испорчен, в нём нет гравитации (" + report.error + ")");
    if (!report.warnings.empty()) return fail("значение не того вида (" + report.warnings.front() + ")");
    if (!valid_physics(p)) {
        char why[160];
        std::snprintf(why, sizeof(why), "гравитация %g, %g вне пределов: каждая от −%g до %g тайлов/с²",
                      static_cast<f64>(p.gravity_x), static_cast<f64>(p.gravity_y), static_cast<f64>(kMaxGravity),
                      static_cast<f64>(kMaxGravity));
        return fail(why);
    }
    out = p;
    return true;
}

bool save_physics(const fs::path& folder, const LevelPhysics& p, std::string* error) {
    if (!valid_physics(p)) {
        if (error) *error = "physics.json не записан: гравитация вне пределов";
        return false;
    }
    const std::string text = data::to_json(p) + "\n";
    std::error_code ec;
    if (!folder.empty()) fs::create_directories(folder, ec);
    if (folder.empty() || !write_file_atomic(folder / kPhysicsFile, {reinterpret_cast<const u8*>(text.data()), text.size()})) {
        if (error) *error = "не удалось записать " + path_to_utf8(folder / kPhysicsFile);
        return false;
    }
    return true;
}

SetPhysics::SetPhysics(Level& level, LevelPhysics before, LevelPhysics after, std::string label)
    : level_(level), before_(before), after_(after), label_(std::move(label)) {}

bool SetPhysics::try_merge(const editor::Command& next) {
    const auto* n = static_cast<const SetPhysics*>(&next);
    if (!n || n->label_ != label_) return false;
    after_ = n->after_;
    return true;
}

FlowTrial::~FlowTrial() {
    if (running()) reset();
}

bool FlowTrial::start(Level& level, const world::Rect& focus, std::string* error) {
    if (running()) reset();
    rules_ = sim::CollisionRules();
    cells_ = level.module().make_cells(rules_);
    if (!cells_) {
        if (error) *error = "в этой игре нет воды и сыпучих плиток";
        return false;
    }
    world::World& world = level.world();
    const u32 layers = world.layer_count();
    saved_.clear();
    world::Rect chunks{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    world.for_each_ready([&](world::Chunk& c) {
        Saved s;
        s.coord = c.coord;
        s.tiles.assign(c.tiles, c.tiles + static_cast<usize>(layers) * world::kChunkTiles);
        s.revision = c.revision;
        s.edited = c.edited;
        s.clean = !c.edited || c.revision == c.saved_revision;
        s.saved_revision = c.saved_revision;
        saved_.push_back(std::move(s));
        chunks.x0 = std::min(chunks.x0, c.coord.x);
        chunks.y0 = std::min(chunks.y0, c.coord.y);
        chunks.x1 = std::max(chunks.x1, c.coord.x + 1);
        chunks.y1 = std::max(chunks.y1, c.coord.y + 1);
    });
    if (saved_.empty()) {
        cells_.reset();
        if (error) *error = "уровень ещё не загрузился";
        return false;
    }
    const i32 n = world::kChunkSize;
    keep_ = {chunks.x0 * n, chunks.y0 * n, chunks.x1 * n, chunks.y1 * n};
    focus_ = focus;
    const LevelPhysics& p = level.physics();
    down_ = sim::cells_down(p.gravity_x, p.gravity_y);
    // Only the chunks there now: ones that load later are not remembered,
    // so nothing flows into them.
    cells_->rebuild(world, rules_);
    zones_ = sim::Zones();
    tick_ = 0;
    due_ = 0;
    level_ = &level;
    return true;
}

u32 FlowTrial::update(f64 dt) {
    if (!running()) return 0;
    due_ = std::min(due_ + dt, 4 * kTick);
    u32 ran = 0;
    while (due_ >= kTick) {
        due_ -= kTick;
        zones_.update(level_->world(), {&focus_, 1}, tick_);
        if (down_ != ~0u) cells_->step(zones_, tick_, down_);
        ++tick_;
        ++ran;
    }
    return ran;
}

bool FlowTrial::reset() {
    if (!running()) return true;
    world::World& world = level_->world();
    const usize count = static_cast<usize>(world.layer_count()) * world::kChunkTiles;
    bool all = true;
    for (const Saved& s : saved_) {
        world::Chunk* c = world.find_chunk(s.coord);
        if (!c) {
            all = false;
            continue;
        }
        if (c->revision == s.revision) continue; // nothing moved there
        std::memcpy(c->tiles, s.tiles.data(), count * sizeof(world::TileId));
        ++c->revision; // drawn again
        c->edited = s.edited;
        c->saved_revision = s.clean ? c->revision : s.saved_revision;
    }
    if (!all) FORGE_ERROR("Проба: часть участков выгрузилась до сброса, их вода и песок остались как после пробы");
    saved_.clear();
    cells_.reset();
    level_ = nullptr;
    return all;
}

} // namespace forge::level
