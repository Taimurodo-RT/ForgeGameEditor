#pragma once

// The living world: runs game rules in fixed ticks over the zones that are
// awake, moves bodies along the tiles and reports what happened as events.
//
// A frame:
//   sim.update(frame_seconds, camera_rect)
//     World::update      chunks around every focus load, far ones leave
//     zones              which loaded chunks are Active, Near or asleep
//     ticks (0..4)       game systems -> bodies move
//     Scene::update      entities re-filed into their chunks
//     triggers           who entered and left each trigger
//   draw, using clock().alpha() for smooth motion
//
// Systems run on the main thread and usually fan out with each_due(), which
// visits only entities whose chunk is due this tick, in parallel.

#include "forge/core/jobs.h"
#include "forge/core/types.h"
#include "forge/data/reflect.h"
#include "forge/scene/scene.h"
#include "forge/sim/bodies.h"
#include "forge/sim/cells.h"
#include "forge/sim/clock.h"
#include "forge/sim/gravity.h"
#include "forge/sim/tiles.h"
#include "forge/sim/zones.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace forge::sim {

// Keeps the area around its entity loaded and Active, wherever the camera
// is: machines, a base, an escort mission's caravan. Saved with the entity.
struct KeepAwake {
    f32 radius = 8; // tiles
};

// Reports entities entering and leaving a circle around its entity: doors,
// traps, pickups, "the player reached the cave". Checked once per frame,
// after everything moved. Saved with the entity.
struct Trigger {
    f32 radius = 2; // tiles
};

struct ContactEvent {
    flecs::entity_t entity = 0;
    u8 began = 0; // Contact bits that were not there the tick before (landed, hit a wall)
    u8 ended = 0; // bits that went away (left the ground)
};

struct TriggerEvent {
    flecs::entity_t trigger = 0;
    flecs::entity_t other = 0;
    bool entered = false; // false: left
};

struct Events {
    std::vector<ContactEvent> contacts;
    std::vector<TriggerEvent> triggers;
    void clear() {
        contacts.clear();
        triggers.clear();
    }
};

struct SimDesc {
    u32 ticks_per_second = 60;
    u32 max_ticks_per_frame = 4;
    ZoneDesc zones;
    f32 gravity_x = 0, gravity_y = 0; // the world's pull, tiles / s² (side view: 0, 40; top-down: 0, 0)
    f32 max_fall = 50;                // tiles / s along the pull
    u32 collision_layer = 1;
    // A tile layer for liquids (the world needs that many layers); ~0u: none.
    u32 liquid_layer = ~0u;
};

struct TickContext {
    u64 tick = 0;
    f32 dt = 0; // seconds per tick (each_due passes a longer one to Near chunks)
    const Zones& zones;
    const TileView& tiles;
    const GravityField& gravity;
    const Events& events; // what happened in the previous tick (triggers: in the previous frame)
    scene::Scene& scene;
};

using SystemFn = std::function<void(const TickContext&)>;

struct SimStats {
    u32 ticks = 0;          // run in the last frame
    f64 systems_ms = 0;     // last frame, all ticks together
    f64 bodies_ms = 0;
    f64 triggers_ms = 0;
    f64 cells_ms = 0;       // liquids and falling tiles
    f64 world_ms = 0;       // World::update + zones + tile view
    f64 scene_ms = 0;       // Scene::update
    u32 bodies_moved = 0;   // in the last tick
    u32 triggers = 0;
    ZoneStats zones;
    CellStats cells;
};

// Runs fn(entity, dt, Position&, C&...) for every entity matching the query
// whose chunk is due on this tick, in parallel blocks. dt is longer for
// entities in Near chunks, which are visited less often.
template <typename... C, typename Fn>
void each_due(const TickContext& ctx, flecs::query<scene::Position, C...>& query, Fn&& fn);

class Simulation final : public world::WorldListener {
public:
    Simulation(world::World& world, scene::Scene& scene, const SimDesc& desc = {});
    ~Simulation() override;
    Simulation(const Simulation&) = delete;
    Simulation& operator=(const Simulation&) = delete;

    // Which tiles block bodies; set up before the first update.
    CollisionRules& collision() { return collision_; }

    // The world's pull; can change during play (turning the world over).
    void set_gravity(f32 x, f32 y) { gravity_.set_world(x, y); }
    const GravityField& gravity() const { return gravity_; }

    // Liquids and falling tiles; nullptr without a liquid layer. Set up the
    // kinds before the first update. They fall the way the world's gravity
    // points most (local sources move bodies only); with no world gravity
    // (top-down games) they stay where they are.
    CellSim* cells() { return cells_.get(); }

    // Game rules, run every tick in the order added, before bodies move.
    void add_system(SystemFn fn) { systems_.push_back(std::move(fn)); }

    // One frame. focus: rectangles in tiles that must be alive (the camera
    // view); entities with KeepAwake add their own. Returns ticks run.
    u32 update(f64 frame_seconds, std::span<const world::Rect> focus);
    u32 update(f64 frame_seconds, const world::Rect& focus) {
        return update(frame_seconds, std::span<const world::Rect>(&focus, 1));
    }

    const SimClock& clock() const { return clock_; }
    const Zones& zones() const { return zones_; }
    const TileView& tiles() const { return tiles_; }
    // Everything that happened during the last frame's ticks.
    const Events& frame_events() const { return frame_events_; }
    const SimStats& stats() const { return stats_; }

    void on_chunk_loaded(world::Chunk& chunk) override;
    void on_chunk_unloading(world::Chunk& chunk) override;

private:
    void tick();
    void step_bodies(const TickContext& ctx);
    void gather_gravity();
    void step_triggers();

    world::World& world_;
    scene::Scene& scene_;
    SimDesc desc_;
    SimClock clock_;
    Zones zones_;
    CollisionRules collision_;
    TileView tiles_;
    bool tiles_dirty_ = true;
    std::vector<SystemFn> systems_;
    flecs::query<scene::Position, Body> bodies_;
    flecs::query<scene::Position, Trigger> triggers_;
    flecs::query<scene::Position, KeepAwake> keep_awake_;
    flecs::query<scene::Position, GravitySource> gravity_sources_;
    GravityField gravity_;
    std::unique_ptr<CellSim> cells_;
    std::vector<GravityField::Placed> placed_;
    std::vector<world::Rect> focus_;
    Events tick_events_, prev_events_, frame_events_;
    // Who is inside each trigger: sorted (trigger, other) pairs.
    std::vector<std::pair<flecs::entity_t, flecs::entity_t>> inside_, inside_next_;
    SimStats stats_;
    u64 current_tick_ = 0; // number of the next tick to run
};

// --- implementation details ------------------------------------------------

namespace detail {

template <typename... C>
struct DueSpan {
    const flecs::entity_t* entities;
    scene::Position* positions;
    std::tuple<C*...> components;
    u32 count;
};

template <typename... C, usize... I>
std::tuple<C*...> fields(flecs::iter& it, std::index_sequence<I...>) {
    return std::tuple<C*...>(&it.field<C>(static_cast<i8>(I + 1))[0]...);
}

} // namespace detail

template <typename... C, typename Fn>
void each_due(const TickContext& ctx, flecs::query<scene::Position, C...>& query, Fn&& fn) {
    using Span = detail::DueSpan<C...>;
    constexpr u32 kBlock = 4096;
    std::vector<Span> spans;
    query.run([&](flecs::iter& it) {
        while (it.next()) {
            const u32 n = static_cast<u32>(it.count());
            const flecs::entity_t* ents = it.c_ptr()->entities;
            scene::Position* pos = &it.template field<scene::Position>(0)[0];
            std::tuple<C*...> comps = detail::fields<C...>(it, std::index_sequence_for<C...>{});
            for (u32 b = 0; b < n; b += kBlock) {
                std::tuple<C*...> shifted = std::apply([b](C*... p) { return std::tuple<C*...>(p + b...); }, comps);
                spans.push_back({ents + b, pos + b, shifted, std::min(kBlock, n - b)});
            }
        }
    });
    jobs::parallel_for(static_cast<u32>(spans.size()), 1, [&](u32 begin, u32 end) {
        for (u32 s = begin; s < end; ++s) {
            const Span& span = spans[s];
            for (u32 i = 0; i < span.count; ++i) {
                scene::Position& p = span.positions[i];
                const u32 due = ctx.zones.ticks_due(p.chunk(), ctx.tick);
                if (due == 0) continue;
                std::apply([&](C*... c) { fn(span.entities[i], ctx.dt * static_cast<f32>(due), p, c[i]...); },
                           span.components);
            }
        }
    });
}

} // namespace forge::sim

FORGE_REFLECT_DECLARE(forge::sim::KeepAwake)
FORGE_REFLECT_DECLARE(forge::sim::Trigger)
