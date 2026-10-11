#include "slice_checks.h"

#include "slice_level.h"

#include "forge/scene/scene.h"
#include "forge/sim/bodies.h"

#include <cstdio>

namespace forge::editor_app::slice_checks {

namespace {

// The editor's level module as slice's: the callers checked is_slice (no RTTI here to ask).
const slice::SliceLevel& as_slice(const level::LevelModule& module) { return static_cast<const slice::SliceLevel&>(module); }
slice::SliceLevel& as_slice(level::LevelModule& module) { return static_cast<slice::SliceLevel&>(module); }

} // namespace

bool is_slice(const level::LevelModule& module) { return module.title() == "Старая шахта"; }

Places places(const level::LevelModule& module) {
    const slice::SliceGenerator& g = as_slice(module).slice_generator();
    return {g.village_y(), g.mine_x(), g.mine_y(), g.gallery_y(), g.gallery_x1()};
}

Color lamp_at(level::LevelModule& module, f64 x, f64 y) { return as_slice(module).lights().lamp_at(x, y); }

void villagers(flecs::world& ecs, int& people, int& with_bodies) {
    people = with_bodies = 0;
    ecs.each([&](flecs::entity e, const slice::Npc&) {
        ++people;
        with_bodies += e.has<sim::Body>();
    });
}

Thing thing(flecs::entity e) {
    if (!e.is_valid() || !e.has<slice::Item>()) return {};
    static const char* const kinds[] = {"pickaxe", "coins", "copper", "wood", "torch", "key"};
    const slice::Item& i = e.get<slice::Item>();
    return {i.kind < std::size(kinds) ? kinds[i.kind] : "?", i.count};
}

void things(flecs::world& ecs, std::vector<std::string>& out) {
    ecs.each([&](const scene::Position& p, const slice::Item& i) {
        char t[96];
        std::snprintf(t, sizeof(t), "item %d %.2f %.2f", i.kind, p.tile_x(), p.tile_y());
        out.push_back(t);
    });
}

bool critter(flecs::entity e) { return e.is_valid() && e.has<slice::Critter>(); }

bool wanders(flecs::entity e) {
    return critter(e) && e.get<slice::Critter>().scheme == static_cast<u8>(slice::Scheme::Wander);
}

} // namespace forge::editor_app::slice_checks
