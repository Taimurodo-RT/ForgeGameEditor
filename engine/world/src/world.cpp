#include "forge/world/world.h"

#include "forge/core/assert.h"
#include "forge/core/jobs.h"
#include "forge/core/profile.h"
#include "forge/core/time.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace forge::world {

struct World::LoadJob {
    Chunk* chunk = nullptr;
    const Generator* generator = nullptr;
    u32 layer_count = 0;
    std::vector<u8> stored; // compressed edits to restore instead of generating
    ChunkLocation disk;     // or: where the saved chunk is on disk
    bool from_disk = false;
    bool restored = false;

    static void run(void* data, u32) {
        FORGE_ZONE_N("Load chunk");
        auto* job = static_cast<LoadJob*>(data);
        Chunk& chunk = *job->chunk;
        const ChunkTiles out{chunk.tiles, job->layer_count};
        if (job->from_disk && !read_chunk_bytes(job->disk, job->stored)) job->stored.clear();
        job->restored = !job->stored.empty() &&
                        decode_chunk(job->stored.data(), job->stored.size(), chunk.tiles, job->layer_count);
        if (!job->restored) job->generator->generate(chunk.coord, out);
        chunk.edited = job->restored;
        // Straight from the save: unchanged until edited again.
        if (job->restored && job->from_disk) chunk.saved_revision = chunk.revision;
        chunk.state.store(ChunkState::Ready, std::memory_order_release);
    }
};

namespace {

bool inside_any(const std::vector<Rect>& rects, ChunkCoord c) {
    for (const Rect& r : rects)
        if (r.contains(c.x, c.y)) return true;
    return false;
}

} // namespace

World::World(const WorldDesc& desc, std::shared_ptr<const Generator> generator)
    : desc_(desc),
      generator_(std::move(generator)),
      chunk_pool_(align_up(sizeof(Chunk), kCacheLine) + static_cast<usize>(desc.layer_count) * kChunkTiles * sizeof(TileId),
                  kCacheLine, 64, MemoryCategory::World),
      tiles_offset_(align_up(sizeof(Chunk), kCacheLine)) {
    FORGE_VERIFY(desc_.layer_count >= 1 && desc_.layer_count <= 16);
    FORGE_VERIFY(generator_ != nullptr);
    // Enough to keep every thread busy across a frame: a chunk takes tens of
    // microseconds, and loads are only started once per update().
    if (desc_.max_loads_in_flight == 0) desc_.max_loads_in_flight = std::max(64u, jobs::thread_count() * 32);
}

World::~World() {
    finish_loading();
    for (Chunk* c : resident_list_) free_chunk(c);
}

bool World::in_bounds(ChunkCoord c) const { return desc_.bounds.empty() || desc_.bounds.contains(c.x, c.y); }

Chunk* World::allocate_chunk(ChunkCoord coord) {
    void* block = chunk_pool_.alloc();
    auto* chunk = new (block) Chunk();
    chunk->coord = coord;
    chunk->tiles = reinterpret_cast<TileId*>(static_cast<u8*>(block) + tiles_offset_);
    return chunk;
}

void World::free_chunk(Chunk* chunk) {
    chunk->~Chunk();
    chunk_pool_.free(chunk);
}

void World::start_load(ChunkCoord coord) {
    Chunk* chunk = allocate_chunk(coord);
    resident_.emplace(coord, chunk);
    resident_list_.push_back(chunk);

    auto* job = new LoadJob();
    job->chunk = chunk;
    job->generator = generator_.get();
    job->layer_count = desc_.layer_count;
    if (auto it = stored_edits_.find(coord); it != stored_edits_.end()) {
        stored_bytes_ -= it->second.size();
        job->stored = std::move(it->second);
        stored_edits_.erase(it);
    } else if (store_ && store_->locate(coord, job->disk)) {
        job->from_disk = true;
    }
    loading_.push_back(job);
    jobs::submit_background({&LoadJob::run, job, 0}, &loads_);
}

void World::integrate_finished() {
    FORGE_ZONE();
    usize kept = 0;
    std::vector<Chunk*> arrived;
    for (LoadJob* job : loading_) {
        if (job->chunk->state.load(std::memory_order_acquire) != ChunkState::Ready) {
            loading_[kept++] = job;
            continue;
        }
        if (job->restored) ++restored_;
        else ++generated_;
        arrived.push_back(job->chunk);
        delete job;
    }
    loading_.resize(kept);

    for (Chunk* chunk : arrived) {
        // The focus moved away while it was loading: drop it right away.
        if (!last_keep_.empty() && !inside_any(last_keep_, chunk->coord)) {
            unload(chunk);
            continue;
        }
        for (WorldListener* l : listeners_) l->on_chunk_loaded(*chunk);
    }
}

void World::unload(Chunk* chunk) {
    for (WorldListener* l : listeners_) l->on_chunk_unloading(*chunk);
    // Changed and not yet on disk: keep it until the next save.
    if (chunk->edited && chunk->revision != chunk->saved_revision) {
        std::vector<u8> bytes = encode_chunk(chunk->tiles, desc_.layer_count);
        stored_bytes_ += bytes.size();
        stored_edits_[chunk->coord] = std::move(bytes);
    }
    resident_.erase(chunk->coord);
    chunk->tiles = nullptr; // marks it for release by update(), which compacts resident_list_ in bulk
    ++unloaded_;
}

void World::update(std::span<const Rect> focus_tiles) {
    FORGE_ZONE_N("World update");
    integrate_finished();

    std::vector<Rect> want, keep;
    for (const Rect& f : focus_tiles) {
        if (f.empty()) continue;
        const Rect chunks = chunks_of(f);
        want.push_back(chunks.expanded(desc_.load_margin));
        keep.push_back(chunks.expanded(desc_.load_margin + desc_.keep_extra));
    }

    // Drop chunks far from every focus.
    if (keep != last_keep_) {
        FORGE_ZONE_N("Unload far chunks");
        last_keep_ = keep;
        for (Chunk* c : resident_list_) {
            if (c->tiles == nullptr) continue;
            if (c->state.load(std::memory_order_acquire) != ChunkState::Ready) continue; // still loading
            if (!inside_any(keep, c->coord)) unload(c);
        }
    }
    release_unloaded();

    // Start loads for missing chunks, nearest to a focus centre first.
    if (loading_.size() >= desc_.max_loads_in_flight) return;
    struct Candidate {
        i64 distance;
        ChunkCoord coord;
    };
    std::vector<Candidate> candidates;
    for (const Rect& w : want) {
        const Rect r = desc_.bounds.empty() ? w : w.clipped(desc_.bounds);
        const i64 cx2 = static_cast<i64>(r.x0) + r.x1; // twice the centre, to stay in integers
        const i64 cy2 = static_cast<i64>(r.y0) + r.y1;
        for (i32 y = r.y0; y < r.y1; ++y) {
            for (i32 x = r.x0; x < r.x1; ++x) {
                if (resident_.count({x, y})) continue;
                const i64 dx = 2 * static_cast<i64>(x) + 1 - cx2;
                const i64 dy = 2 * static_cast<i64>(y) + 1 - cy2;
                candidates.push_back({dx * dx + dy * dy, {x, y}});
            }
        }
    }
    const usize budget = desc_.max_loads_in_flight - loading_.size();
    if (candidates.size() > budget) {
        std::partial_sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(budget), candidates.end(),
                          [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });
        candidates.resize(budget);
    }
    for (const Candidate& c : candidates) {
        if (loading_.size() >= desc_.max_loads_in_flight) break;
        if (!in_bounds(c.coord) || resident_.count(c.coord)) continue; // overlapping foci
        start_load(c.coord);
    }
}

void World::finish_loading() {
    FORGE_ZONE();
    jobs::wait(loads_);
    integrate_finished();
    release_unloaded();
}

bool World::open_save(const std::filesystem::path& folder, std::string* error) {
    FORGE_VERIFY(resident_.empty()); // before the first update()
    auto store = std::make_unique<RegionStore>();
    if (!store->open(folder, desc_.layer_count, error)) return false;
    store_ = std::move(store);
    return true;
}

SaveReport World::save() {
    FORGE_ZONE_N("World save");
    SaveReport report;
    if (!store_) return report;
    const u64 start = time_now_ns();
    // No chunk may be read from the region files while they are rewritten.
    finish_loading();

    std::vector<Chunk*> changed;
    for (Chunk* c : resident_list_)
        if (c->tiles && c->edited && c->revision != c->saved_revision) changed.push_back(c);
    std::vector<std::vector<u8>> encoded(changed.size());
    jobs::parallel_for(static_cast<u32>(changed.size()), 16, [&](u32 b, u32 e) {
        for (u32 i = b; i < e; ++i) encoded[i] = encode_chunk(changed[i]->tiles, desc_.layer_count);
    });

    std::vector<RegionStore::Write> writes;
    writes.reserve(changed.size() + stored_edits_.size());
    for (usize i = 0; i < changed.size(); ++i) writes.push_back({changed[i]->coord, &encoded[i]});
    for (const auto& [coord, bytes] : stored_edits_) writes.push_back({coord, &bytes});

    report.ok = store_->write(writes, &report.regions, &report.bytes);
    report.chunks = static_cast<u32>(writes.size());
    if (report.ok) {
        for (Chunk* c : changed) c->saved_revision = c->revision;
        stored_edits_.clear();
        stored_bytes_ = 0;
    }
    report.ms = ns_to_ms(time_now_ns() - start);
    return report;
}

void World::release_unloaded() {
    if (resident_list_.size() == resident_.size()) return; // nothing was unloaded
    usize n = 0;
    for (Chunk* c : resident_list_) {
        if (c->tiles == nullptr) free_chunk(c);
        else resident_list_[n++] = c;
    }
    resident_list_.resize(n);
}

Chunk* World::find_chunk(ChunkCoord coord) {
    auto it = resident_.find(coord);
    if (it == resident_.end()) return nullptr;
    return it->second->state.load(std::memory_order_acquire) == ChunkState::Ready ? it->second : nullptr;
}

const Chunk* World::find_chunk(ChunkCoord coord) const { return const_cast<World*>(this)->find_chunk(coord); }

TileId World::tile(u32 layer, i32 x, i32 y) const {
    FORGE_ASSERT(layer < desc_.layer_count);
    const Chunk* c = find_chunk(chunk_of(x, y));
    return c ? c->layer(layer)[local_index(x, y)] : kEmptyTile;
}

bool World::set_tile(u32 layer, i32 x, i32 y, TileId id) {
    FORGE_ASSERT(layer < desc_.layer_count);
    Chunk* c = find_chunk(chunk_of(x, y));
    if (!c) return false;
    TileId& t = c->layer(layer)[local_index(x, y)];
    if (t != id) {
        t = id;
        ++c->revision;
        c->edited = true;
    }
    return true;
}

void World::add_listener(WorldListener* listener) { listeners_.push_back(listener); }

void World::remove_listener(WorldListener* listener) {
    listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), listener), listeners_.end());
}

WorldStats World::stats() const {
    WorldStats s;
    s.resident = static_cast<u32>(resident_.size());
    s.loading = static_cast<u32>(loading_.size());
    s.stored_edits = static_cast<u32>(stored_edits_.size());
    s.stored_bytes = stored_bytes_;
    s.generated = generated_;
    s.restored = restored_;
    s.unloaded = unloaded_;
    return s;
}

// --- chunk encoding --------------------------------------------------------
//
// [u8 version][u8 layer_count] then runs over all layers in order:
// [varint run length][u16 tile, little endian]. Natural terrain is mostly long
// runs of air, dirt and stone, so a chunk shrinks from 16 KiB to a few hundred
// bytes.

namespace {
constexpr u8 kCodecVersion = 1;

void put_varint(std::vector<u8>& out, u32 v) {
    while (v >= 0x80) {
        out.push_back(static_cast<u8>(v | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<u8>(v));
}

bool get_varint(const u8*& p, const u8* end, u32& v) {
    v = 0;
    for (int shift = 0; shift < 35; shift += 7) {
        if (p == end) return false;
        const u8 b = *p++;
        v |= static_cast<u32>(b & 0x7f) << shift;
        if (!(b & 0x80)) return true;
    }
    return false;
}
} // namespace

std::vector<u8> encode_chunk(const TileId* tiles, u32 layer_count) {
    std::vector<u8> out;
    out.reserve(256);
    out.push_back(kCodecVersion);
    out.push_back(static_cast<u8>(layer_count));
    const usize total = static_cast<usize>(layer_count) * kChunkTiles;
    usize i = 0;
    while (i < total) {
        const TileId value = tiles[i];
        usize j = i + 1;
        while (j < total && tiles[j] == value) ++j;
        put_varint(out, static_cast<u32>(j - i));
        out.push_back(static_cast<u8>(value & 0xff));
        out.push_back(static_cast<u8>(value >> 8));
        i = j;
    }
    return out;
}

bool decode_chunk(const u8* data, usize size, TileId* tiles, u32 layer_count) {
    if (size < 2 || data[0] != kCodecVersion || data[1] != layer_count) return false;
    const u8* p = data + 2;
    const u8* end = data + size;
    const usize total = static_cast<usize>(layer_count) * kChunkTiles;
    usize i = 0;
    while (i < total) {
        u32 run = 0;
        if (!get_varint(p, end, run) || run == 0 || run > total - i || end - p < 2) return false;
        const TileId value = static_cast<TileId>(p[0] | (p[1] << 8));
        p += 2;
        std::fill(tiles + i, tiles + i + run, value);
        i += run;
    }
    return p == end;
}

} // namespace forge::world
