#pragma once

// A tile world of any size, kept in memory only around the places that
// matter. Chunks near the camera (and any other focus) are generated or
// restored on background threads; chunks left behind are dropped, and if the
// player changed them, their tiles are kept compressed so nothing is lost.
//
// Tiles are plain arrays per chunk and layer, never ECS entities: a 64×64
// chunk with two layers is 16 KiB, so a screen full of the world is a few
// megabytes and a whole 64k × 64k world would only ever be touched piece by
// piece.
//
// Threading: every World method is called from one thread (the main thread).
// Generators run on job threads and must be thread-safe.

#include "forge/core/jobs.h"
#include "forge/core/memory.h"
#include "forge/core/types.h"
#include "forge/world/coords.h"
#include "forge/world/region_store.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <span>
#include <unordered_map>
#include <vector>

namespace forge::world {

// Writable tiles of one chunk: layer_count arrays of kChunkTiles, row-major.
struct ChunkTiles {
    TileId* data = nullptr;
    u32 layer_count = 0;
    TileId* layer(u32 index) const { return data + static_cast<usize>(index) * kChunkTiles; }
};

// Fills a chunk from scratch. Must be deterministic (the same chunk always
// comes out the same, so unchanged chunks never need saving) and safe to call
// from many threads at once.
class Generator {
public:
    virtual ~Generator() = default;
    virtual void generate(ChunkCoord coord, const ChunkTiles& out) const = 0;
};

enum class ChunkState : u8 {
    Loading, // a job is filling the tiles; do not touch them
    Ready,
};

struct Chunk {
    ChunkCoord coord;
    std::atomic<ChunkState> state{ChunkState::Loading};
    // Bumped on every change; systems that mirror tiles (GPU, physics) compare
    // it with the revision they last copied.
    u32 revision = 0;
    // Differs from what the generator makes, so it must be kept on unload.
    bool edited = false;
    // revision when the chunk last matched the save on disk (~0u: never saved).
    u32 saved_revision = ~0u;
    TileId* tiles = nullptr;

    TileId* layer(u32 index) const { return tiles + static_cast<usize>(index) * kChunkTiles; }
};

// Told about chunks arriving and leaving, on the main thread, during update().
class WorldListener {
public:
    virtual ~WorldListener() = default;
    virtual void on_chunk_loaded(Chunk& chunk) { (void)chunk; }
    // The chunk's memory is released right after this call.
    virtual void on_chunk_unloading(Chunk& chunk) { (void)chunk; }
};

struct WorldDesc {
    u32 layer_count = 2;
    // Limits of the world in chunks; empty = no edges (generated forever).
    Rect bounds{};
    // Chunks kept ready beyond the edge of each focus, so moving does not
    // reveal holes. Chunks this far plus keep_extra are dropped.
    i32 load_margin = 2;
    i32 keep_extra = 2;
    // Background jobs in flight at once; 0 = 32 per thread (at least 64).
    u32 max_loads_in_flight = 0;
};

struct WorldStats {
    u32 resident = 0;      // chunks in memory (ready or loading)
    u32 loading = 0;       // being generated or restored right now
    u32 stored_edits = 0;  // unloaded changed chunks not saved yet, kept compressed in memory
    usize stored_bytes = 0;
    u64 generated = 0;     // totals since creation
    u64 restored = 0;
    u64 unloaded = 0;
};

struct SaveReport {
    bool ok = false;
    u32 chunks = 0;  // chunks written
    u32 regions = 0; // region files written
    usize bytes = 0;
    f64 ms = 0;
};

class World {
public:
    World(const WorldDesc& desc, std::shared_ptr<const Generator> generator);
    ~World(); // waits for loads in flight

    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // Once per frame: integrates finished loads, starts new ones for every
    // focus (rectangles in tiles: the camera view, important objects), and
    // drops chunks far from all of them. Nearest chunks load first.
    void update(std::span<const Rect> focus_tiles);
    void update(const Rect& focus_tiles) { update(std::span<const Rect>(&focus_tiles, 1)); }

    // Blocks until every chunk wanted by the last update() is ready (loading
    // screens, tests, teleports).
    void finish_loading();

    // Keeps this world's changes in a folder of region files: chunks saved
    // there come back from disk instead of the generator. Call before the
    // first update().
    bool open_save(const std::filesystem::path& folder, std::string* error = nullptr);
    // Writes every chunk changed since the last save (in memory or unloaded)
    // to the save folder. Only the regions holding them are rewritten.
    SaveReport save();
    bool has_save() const { return store_ != nullptr; }

    // nullptr when the chunk is not in memory or not ready yet.
    Chunk* find_chunk(ChunkCoord coord);
    const Chunk* find_chunk(ChunkCoord coord) const;

    // kEmptyTile when the chunk is not ready.
    TileId tile(u32 layer, i32 x, i32 y) const;
    // False when the chunk is not ready (nothing is changed).
    bool set_tile(u32 layer, i32 x, i32 y, TileId id);

    // Ready chunks, in no particular order (not the ones being unloaded).
    template <typename Fn>
    void for_each_ready(Fn&& fn) {
        for (Chunk* c : resident_list_)
            if (c->tiles && c->state.load(std::memory_order_acquire) == ChunkState::Ready) fn(*c);
    }

    void add_listener(WorldListener* listener);
    void remove_listener(WorldListener* listener);

    u32 layer_count() const { return desc_.layer_count; }
    const Rect& bounds() const { return desc_.bounds; }
    WorldStats stats() const;

private:
    struct LoadJob;

    Chunk* allocate_chunk(ChunkCoord coord);
    void free_chunk(Chunk* chunk);
    void start_load(ChunkCoord coord);
    void integrate_finished();
    void unload(Chunk* chunk);
    void release_unloaded();
    bool in_bounds(ChunkCoord coord) const;

    WorldDesc desc_;
    std::shared_ptr<const Generator> generator_;
    BlockPool chunk_pool_;
    usize tiles_offset_ = 0;

    std::unordered_map<ChunkCoord, Chunk*, ChunkCoordHash> resident_;
    std::vector<Chunk*> resident_list_;
    std::vector<LoadJob*> loading_;
    JobCounter loads_;
    std::vector<WorldListener*> listeners_;

    std::unique_ptr<RegionStore> store_;

    // Changed chunks that were unloaded before being saved: compressed tiles.
    std::unordered_map<ChunkCoord, std::vector<u8>, ChunkCoordHash> stored_edits_;
    usize stored_bytes_ = 0;

    std::vector<Rect> last_keep_;
    u64 generated_ = 0;
    u64 restored_ = 0;
    u64 unloaded_ = 0;
};

// Compact encoding of a chunk's tiles (runs of equal tiles), used for
// unloaded changes now and for save files later.
std::vector<u8> encode_chunk(const TileId* tiles, u32 layer_count);
bool decode_chunk(const u8* data, usize size, TileId* tiles, u32 layer_count);

} // namespace forge::world
