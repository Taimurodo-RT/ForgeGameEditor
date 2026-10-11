#pragma once

// What the editor's self-test looks at inside «Старая шахта» (step 14.3a): its villagers, things and critters, the
// places of its world, the light of its view. The one file of the editor that sees the module's own types is
// slice_checks.cpp (a library of its own, apps/editor/CMakeLists.txt): the editor's code, its self-test included,
// names none of them. Each takes the editor's level module, which must be slice's (is_slice): the self-test's parts
// that look here open «Старая шахта».

#include "forge/core/math.h"
#include "forge/core/types.h"

#include <flecs.h>

#include <string>
#include <vector>

namespace forge::level {
class LevelModule;
}

namespace forge::editor_app::slice_checks {

// Whether module is «Старая шахта»'s (its title); the rest is only for it.
bool is_slice(const level::LevelModule& module);

// Places of its world, in tiles (slice::SliceGenerator): the village's ground, the mine's mouth, its gallery.
struct Places {
    i32 village_y = 0;
    i32 mine_x = 0, mine_y = 0;
    i32 gallery_y = 0, gallery_x1 = 0;
};
Places places(const level::LevelModule& module);

// The light its view was last prepared with («как в игре»), at a point.
Color lamp_at(level::LevelModule& module, f64 x, f64 y);

// The villagers of a scene, and how many of them have bodies (the game moves only those).
void villagers(flecs::world& ecs, int& people, int& with_bodies);

// An entity as a thing of the game (slice::Item): its kind ("coins", "copper", …; "" when it is no thing) and count.
struct Thing {
    std::string kind;
    u32 count = 0;
};
Thing thing(flecs::entity e);
// Every thing of a scene, as a line "item <kind's number> x y" (where it stands, in tiles).
void things(flecs::world& ecs, std::vector<std::string>& out);
// Whether it is a critter (slice::Critter), and one that wanders.
bool critter(flecs::entity e);
bool wanders(flecs::entity e);

} // namespace forge::editor_app::slice_checks
