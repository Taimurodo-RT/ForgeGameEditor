#pragma once

// Rigid bodies (Box2D v3): crates that tumble and stack, balls that roll,
// planks, carts. They push each other, rotate, rest on the tiles and float
// in liquids. They run only near the player: in chunks that the zones call
// Active. Elsewhere their state stays in the RigidBody component (saved
// with the entity) and they continue from it when the player comes back.
//
// Solid tiles of the awake area are given to Box2D as boxes merged from
// rows of tiles, rebuilt when the player digs or builds. One-way platforms
// do not hold rigid bodies yet.
//
// Box2D runs on the engine's job system. Its positions are kept relative
// to an origin near the loaded area, so they stay precise in a huge world.

#include "forge/core/types.h"
#include "forge/data/reflect.h"
#include "forge/scene/scene.h"
#include "forge/world/world.h"

#include <unordered_map>
#include <vector>

namespace forge::sim {

class CellSim;
class CollisionRules;
class GravityField;
class TileView;
class Zones;

enum class RigidShape : u8 { Box = 0, Circle = 1 };

// Saved with its entity. The entity's Position is the body's centre.
struct RigidBody {
    u8 shape = 0;                     // RigidShape
    f32 half_w = 0.5f, half_h = 0.5f; // tiles; a circle's radius is half_w
    f32 density = 1;
    f32 friction = 0.6f;
    f32 bounce = 0.1f;
    f32 gravity = 1; // multiplies the gravity it is in
    bool fixed_rotation = false;
    // State, kept here while the body is not simulated.
    f32 angle = 0; // radians, clockwise on screen
    f32 vx = 0, vy = 0, spin = 0;
};

struct RigidContact {
    flecs::entity_t a = 0, b = 0; // b = 0: a hit the tiles
};

struct RigidStats {
    u32 live = 0;        // bodies in Box2D now
    u32 tile_chunks = 0; // chunks whose tiles Box2D knows
    u32 tile_boxes = 0;
    u32 awake = 0;        // bodies Box2D still moves (the rest sleep)
    f64 solver_ms = 0;    // Box2D's own step, the last tick
};

class RigidWorld {
public:
    RigidWorld(scene::Scene& scene, const CollisionRules& rules);
    ~RigidWorld();
    RigidWorld(const RigidWorld&) = delete;
    RigidWorld& operator=(const RigidWorld&) = delete;

    // One tick: bodies wake and sleep with their chunks, the tiles around
    // them are kept up to date, Box2D steps, and the results go back to the
    // entities. Contacts that began are appended to contacts.
    void step(world::World& world, const Zones& zones, const TileView& tiles, const GravityField& gravity,
              const CellSim* cells, u64 tick, f32 dt, std::vector<RigidContact>& contacts);

    // A push, in tile-mass units × tiles/s; false if the body is not
    // simulated right now (then change RigidBody::vx / vy instead).
    bool apply_impulse(flecs::entity_t e, f32 ix, f32 iy);
    // Forgets the bodies in Box2D; they are made again from their components
    // at the next step (after the components were changed from outside, as
    // rewinding does).
    void reset();

    const RigidStats& stats() const { return stats_; }

private:
    struct Impl;
    Impl* impl_;
    RigidStats stats_;
};

} // namespace forge::sim

FORGE_REFLECT_DECLARE(forge::sim::RigidBody)
