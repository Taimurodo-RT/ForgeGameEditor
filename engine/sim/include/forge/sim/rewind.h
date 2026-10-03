#pragma once

// Rewinding time: the simulation keeps recording the last seconds of the
// world around the camera and can play them back, like Braid or Prince of
// Persia.
//
// What is recorded, every few ticks of world time:
//   - entities in Active and Near chunks: their Position, Body and
//     RigidBody, plus any components the game adds with track<T>();
//   - the tiles of those chunks (blocks, liquids, sand), copied only when
//     they changed since the last copy.
// Going back restores all of it: things that were spawned since are gone,
// things destroyed since come back (with the tracked components), dug
// tunnels close, water flows back up.
//
// Left alone: entities with OutsideTime (they keep moving during the
// rewind and stay where they are), the world far away (it waits while time
// runs back), and always-on factories (they wait too).

#include "forge/core/types.h"
#include "forge/scene/scene.h"
#include "forge/world/world.h"

#include <deque>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace forge::sim {

class Zones;

struct RewindDesc {
    bool record = false;         // off by default: only games that rewind pay for it
    f32 seconds = 10;            // how far back it can go, in world time
    u32 every = 2;               // ticks of world time between recordings
    usize max_bytes = 512u << 20; // older recordings are dropped above this
};

struct RewindStats {
    u32 frames = 0;   // recordings kept
    u32 entities = 0; // in the last one
    u32 chunk_copies = 0;
    usize bytes = 0;
    f32 seconds = 0; // how far back it can go now
};

class Rewind {
public:
    Rewind(world::World& world, scene::Scene& scene, const RewindDesc& desc, f32 tick_seconds);
    ~Rewind();
    Rewind(const Rewind&) = delete;
    Rewind& operator=(const Rewind&) = delete;

    // Components recorded and restored with entities (besides Position,
    // Body and RigidBody). Plain data only.
    template <typename T>
    void track() {
        static_assert(std::is_trivially_copyable_v<T>, "rewind copies components as bytes");
        track_id(scene_.ecs().component<T>().id(), sizeof(T));
    }

    // Starts going back, speed seconds of world time per second; false if
    // nothing is recorded. The world stands still meanwhile, except what is
    // OutsideTime.
    bool start(f32 speed = 1);
    void set_speed(f32 speed) { speed_ = speed; }
    // Time runs forward again from where the rewind got to; what was ahead
    // of it is forgotten.
    void stop();
    bool playing() const { return playing_; }
    // Switching it off forgets what was recorded.
    void set_recording(bool on);
    bool recording() const { return recording_; }
    void clear();
    RewindStats stats() const;

    // --- driven by Simulation ---------------------------------------------
    // After a tick of world time scale (0..): records when one is due.
    void after_tick(f32 world_scale, const Zones& zones);
    // A tick of playback; true when something was restored (bodies in
    // physics must be rebuilt).
    bool play_tick();
    // Chunks whose tiles the last restore rewrote.
    const std::vector<world::ChunkCoord>& restored_chunks() const { return restored_; }
    void on_chunk_loaded(world::ChunkCoord c);
    void on_chunk_unloading(world::ChunkCoord c);

private:
    struct Tracked {
        flecs::entity_t id;
        u32 size;
    };
    struct Frame {
        f64 time;                             // world time, in ticks
        std::vector<u8> entities;             // records, see rewind.cpp
        std::vector<world::ChunkCoord> chunks; // chunks whose entities were recorded (sorted)
        u32 count = 0;
    };
    struct ChunkCopy {
        f64 time;
        u32 revision;
        std::vector<world::TileId> tiles;
    };
    struct ChunkHistory {
        std::deque<ChunkCopy> copies;
        f64 loaded_at = 0; // world time it (last) came into memory
        u32 written_revision = ~0u; // revision after the last restore
        const ChunkCopy* written = nullptr;
    };

    void track_id(flecs::entity_t id, u32 size);
    void record(const Zones& zones);
    void apply(const Frame& frame);
    void trim();

    world::World& world_;
    scene::Scene& scene_;
    RewindDesc desc_;
    f32 tick_seconds_;
    std::vector<Tracked> tracked_;
    flecs::entity_t outside_id_ = 0;
    flecs::query<scene::Position> positions_;
    std::deque<Frame> frames_;
    std::unordered_map<world::ChunkCoord, ChunkHistory, world::ChunkCoordHash> chunks_;
    std::unordered_map<world::ChunkCoord, f64, world::ChunkCoordHash> loaded_at_;
    f64 time_ = 0;        // world time now, in ticks
    f64 last_record_ = -1e30;
    bool recording_ = false;
    bool playing_ = false;
    f32 speed_ = 1;
    const Frame* applied_ = nullptr;
    usize bytes_ = 0;
    std::vector<world::ChunkCoord> restored_;
};

} // namespace forge::sim
