#pragma once

// Entities living in a streamed tile world.
//
// Every entity with a Position belongs to the chunk it stands in. When the
// chunk leaves memory its entities are packed into bytes (through the
// reflection of their components) and removed from the ECS; when the chunk
// comes back they are recreated exactly as they were. Entities that walk into
// a chunk that is not loaded are packed into that chunk. So a world can hold
// millions of objects while only the ones near a focus exist in memory.
//
// Each frame, after game systems move entities, update() rebuilds in one
// parallel pass which chunk every entity is in and a spatial index for
// "who is near" queries. Rebuilding from scratch is cheaper than tracking
// every move once there are hundreds of thousands of entities.
//
// Frame order: simulate (systems change Position) -> Scene::update() ->
// World::update() (which may unload chunks and their entities).

#include "forge/core/types.h"
#include "forge/data/reflect.h"
#include "forge/world/region_store.h"
#include "forge/world/world.h"

#include <flecs.h>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace forge::scene {

// Where an entity is: its chunk and the position inside it, in tiles
// (0 <= x, y < 64 after update()). Splitting the coordinate keeps full float
// precision anywhere in a world billions of tiles wide.
struct Position {
    i32 cx = 0;
    i32 cy = 0;
    f32 x = 0;
    f32 y = 0;

    static Position at_tile(f64 tile_x, f64 tile_y);
    f64 tile_x() const { return static_cast<f64>(cx) * world::kChunkSize + x; }
    f64 tile_y() const { return static_cast<f64>(cy) * world::kChunkSize + y; }
    world::ChunkCoord chunk() const { return {cx, cy}; }
};

class Scene;

// Fills a chunk the first time it is ever loaded (trees, ore deposits,
// animals). Never called again for that chunk, even if all its entities die.
using PopulateFn = std::function<void(world::ChunkCoord, Scene&)>;

struct SceneStats {
    u32 entities = 0;          // with a Position, in loaded chunks
    u32 chunks_indexed = 0;    // loaded chunks in the spatial index
    u32 stored_chunks = 0;     // unloaded chunks whose entities wait in memory for a save
    usize stored_bytes = 0;
    u32 moved_out = 0;         // last update: walked into an unloaded chunk and were packed
    u64 packed = 0;            // totals: entities packed / recreated
    u64 unpacked = 0;
};

struct SceneSaveReport {
    bool ok = false;
    u32 chunks = 0;
    u32 regions = 0;
    usize bytes = 0;
    f64 ms = 0;
};

class Scene final : public world::WorldListener {
public:
    explicit Scene(world::World& world);
    ~Scene() override;
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    flecs::world& ecs() { return ecs_; }

    // Components saved with their chunk. T must be described with
    // FORGE_REFLECT. Position is always registered.
    template <typename T>
    void register_component() {
        add_saved_component(reflect::type_of<T>(), ecs_.component<T>().id());
    }

    // A new entity at a position in a loaded chunk; an empty entity if that
    // chunk is not loaded.
    flecs::entity spawn(const Position& at);

    void set_populator(PopulateFn fn) { populate_ = std::move(fn); }

    // After game systems ran: puts every entity in its chunk, packs those that
    // left the loaded area, rebuilds the spatial index.
    void update();

    // Entities within radius tiles of a point (as of the last update()).
    void query_radius(f64 tile_x, f64 tile_y, f32 radius, std::vector<flecs::entity_t>& out) const;
    u32 count_in_chunk(world::ChunkCoord chunk) const;

    // Entities are saved next to the world's tiles: "e.*.fwr" region files.
    bool open_save(const std::filesystem::path& folder, std::string* error = nullptr);
    SceneSaveReport save();

    SceneStats stats() const;

    void on_chunk_loaded(world::Chunk& chunk) override;
    void on_chunk_unloading(world::Chunk& chunk) override;

private:
    struct SavedComponent {
        const reflect::TypeInfo* type;
        flecs::entity_t id;
    };
    struct Item {
        flecs::entity_t entity;
        f32 x, y;
        i32 chunk; // dense index of the chunk, during a rebuild
    };
    struct ChunkIndex {
        world::ChunkCoord coord;
        u32 begin = 0, end = 0; // range in items_
        u32 cell_start[65] = {}; // 8 × 8 cells of 8 tiles, offsets into items_
    };

    void add_saved_component(const reflect::TypeInfo* type, flecs::entity_t id);
    void rebuild_index();
    void rebuild_chunk_list();
    i32 dense_index(world::ChunkCoord c) const;
    void pack_entity(flecs::entity_t e, std::vector<u8>& out);
    void unpack_chunk(world::ChunkCoord coord, const std::vector<u8>& bytes, bool& visited);
    std::vector<u8>& stored_for(world::ChunkCoord coord);
    void pack_chunk(const ChunkIndex& chunk, std::vector<u8>& out, bool visited);

    world::World& world_;
    flecs::world ecs_;
    flecs::query<Position> positions_;
    std::vector<SavedComponent> saved_;
    PopulateFn populate_;

    // Spatial index, rebuilt by update().
    std::vector<Item> items_;
    std::unique_ptr<Item[]> scratch_;
    u32 scratch_capacity_ = 0;
    std::vector<ChunkIndex> chunks_;
    std::unordered_map<world::ChunkCoord, u32, world::ChunkCoordHash> chunk_lookup_;
    // Dense grid over the loaded area for O(1) chunk lookups in the rebuild.
    world::Rect grid_rect_;
    std::vector<i32> grid_;
    std::vector<u32> block_counts_;
    bool index_dirty_ = true;
    bool chunks_dirty_ = true;

    // Unloaded chunks: packed entities not yet saved to disk.
    std::unordered_map<world::ChunkCoord, std::vector<u8>, world::ChunkCoordHash> stored_;
    usize stored_bytes_ = 0;
    std::unique_ptr<world::RegionStore> store_;

    u32 moved_out_ = 0;
    u64 packed_ = 0;
    u64 unpacked_ = 0;
};

} // namespace forge::scene

FORGE_REFLECT_DECLARE(forge::scene::Position)
