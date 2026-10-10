#include "forge/scene/scene.h"

#include "forge/core/assert.h"
#include "forge/core/jobs.h"
#include "forge/core/log.h"
#include "forge/core/profile.h"
#include "forge/core/time.h"
#include "forge/data/binary.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

FORGE_REFLECT(forge::scene::Position, 1) {
    t.field("cx", &forge::scene::Position::cx);
    t.field("cy", &forge::scene::Position::cy);
    t.field("x", &forge::scene::Position::x);
    t.field("y", &forge::scene::Position::y);
}

namespace forge::scene {

using world::ChunkCoord;
using world::kChunkSize;

namespace {

// Packed chunk: [u32 magic][u8 visited][u32 entity count][u32 layouts size]
// [layouts] then per entity [u16 component count] and per component
// [u64 type id][u64 schema hash][u32 size][payload]. The entity count stays at
// byte 5: entities that walk out are appended to a stored chunk.
// Layouts: [u16 n] n × ([u64 type id][u64 schema hash][u32 size]
// [data::append_schema]) for the components in the chunk, so a newer game
// reads the chunk even after those types changed. "FSE1" chunks have none.
constexpr u32 kMagic = 0x32455346;   // "FSE2"
constexpr u32 kMagicV1 = 0x31455346; // "FSE1"
constexpr u32 kSaveTag = 1;
constexpr i32 kCellShift = 3; // 8-tile cells, 8 × 8 per chunk
constexpr i32 kCells = kChunkSize >> kCellShift;
// Above this many chunks in the loaded area's bounding box, chunk lookups use
// a hash map instead of a dense grid.
constexpr i64 kMaxGridCells = 1 << 20;

template <typename T>
void put(std::vector<u8>& out, T v) {
    const usize at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &v, sizeof(T));
}

template <typename T>
bool take(const u8*& p, const u8* end, T& v) {
    if (static_cast<usize>(end - p) < sizeof(T)) return false;
    std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    return true;
}

// Brings x, y back into [0, 64) by moving to the neighbouring chunk.
void normalize(Position& p) {
    if (p.x < 0 || p.x >= kChunkSize) {
        const f32 k = std::floor(p.x / kChunkSize);
        p.cx += static_cast<i32>(k);
        p.x -= k * kChunkSize;
        if (p.x >= kChunkSize) p.x = std::nextafter(static_cast<f32>(kChunkSize), 0.0f);
        if (p.x < 0) p.x = 0;
    }
    if (p.y < 0 || p.y >= kChunkSize) {
        const f32 k = std::floor(p.y / kChunkSize);
        p.cy += static_cast<i32>(k);
        p.y -= k * kChunkSize;
        if (p.y >= kChunkSize) p.y = std::nextafter(static_cast<f32>(kChunkSize), 0.0f);
        if (p.y < 0) p.y = 0;
    }
}

u32 cell_of(f32 x, f32 y) {
    const i32 cx = std::clamp(static_cast<i32>(x) >> kCellShift, 0, kCells - 1);
    const i32 cy = std::clamp(static_cast<i32>(y) >> kCellShift, 0, kCells - 1);
    return static_cast<u32>(cy * kCells + cx);
}

} // namespace

Position Position::at_tile(f64 tile_x, f64 tile_y) {
    Position p;
    p.cx = static_cast<i32>(std::floor(tile_x / kChunkSize));
    p.cy = static_cast<i32>(std::floor(tile_y / kChunkSize));
    p.x = static_cast<f32>(tile_x - static_cast<f64>(p.cx) * kChunkSize);
    p.y = static_cast<f32>(tile_y - static_cast<f64>(p.cy) * kChunkSize);
    normalize(p);
    return p;
}

Scene::Scene(world::World& world) : world_(world) {
    register_component<Position>();
    positions_ = ecs_.query<Position>();
    world_.add_listener(this);
}

Scene::~Scene() { world_.remove_listener(this); }

void Scene::add_saved_component(const reflect::TypeInfo* type, flecs::entity_t id) {
    for (const SavedComponent& c : saved_)
        if (c.id == id) return;
    saved_.push_back({type, id});
}

flecs::entity Scene::spawn(const Position& at) {
    Position p = at;
    normalize(p);
    if (!world_.find_chunk(p.chunk())) return flecs::entity();
    index_dirty_ = true;
    return ecs_.entity().set<Position>(p);
}

i32 Scene::dense_index(ChunkCoord c) const {
    if (!grid_.empty()) {
        if (!grid_rect_.contains(c.x, c.y)) return -1;
        const i64 w = grid_rect_.x1 - grid_rect_.x0;
        return grid_[static_cast<usize>((c.y - grid_rect_.y0) * w + (c.x - grid_rect_.x0))];
    }
    auto it = chunk_lookup_.find(c);
    return it == chunk_lookup_.end() ? -1 : static_cast<i32>(it->second);
}

std::vector<u8>& Scene::stored_for(ChunkCoord coord) {
    std::vector<u8>& bytes = stored_[coord];
    if (bytes.empty()) {
        // Start from what the save already holds for this chunk, if anything.
        if (world::ChunkLocation loc; store_ && store_->locate(coord, loc) && world::read_chunk_bytes(loc, bytes) &&
                                      bytes.size() >= 9) {
            stored_bytes_ += bytes.size();
            return bytes;
        }
        bytes.clear();
        // A chunk nobody has loaded yet: its populator must still run.
        put<u32>(bytes, kMagic);
        put<u8>(bytes, 0);
        put<u32>(bytes, 0);
        put<u32>(bytes, 0);
        stored_bytes_ += bytes.size();
    }
    return bytes;
}

void Scene::pack_entity(flecs::entity_t e, std::vector<u8>& out, std::vector<u8>* used) {
    const usize count_at = out.size();
    put<u16>(out, 0);
    u16 count = 0;
    for (usize k = 0; k < saved_.size(); ++k) {
        const SavedComponent& c = saved_[k];
        const void* data = ecs_get_id(ecs_.c_ptr(), e, c.id);
        if (!data) continue;
        if (used) (*used)[k] = 1;
        put<u64>(out, c.type->id);
        put<u64>(out, c.type->schema_hash());
        const usize size_at = out.size();
        put<u32>(out, 0);
        const usize start = out.size();
        data::append_binary(c.type, data, out);
        const u32 size = static_cast<u32>(out.size() - start);
        std::memcpy(out.data() + size_at, &size, sizeof(size));
        ++count;
    }
    std::memcpy(out.data() + count_at, &count, sizeof(count));
}

void Scene::pack_chunk(const ChunkIndex& chunk, std::vector<u8>& out, bool visited) {
    put<u32>(out, kMagic);
    put<u8>(out, visited ? 1 : 0);
    u32 count = chunk.end - chunk.begin;
    for (u32 i = chunk.begin; i < chunk.end; ++i) count -= items_[i].entity == pack_leave_out_; // no entity is 0
    put<u32>(out, count);
    pack_scratch_.clear();
    pack_used_.assign(saved_.size(), 0);
    for (u32 i = chunk.begin; i < chunk.end; ++i)
        if (items_[i].entity != pack_leave_out_) pack_entity(items_[i].entity, pack_scratch_, &pack_used_);
    const usize size_at = out.size();
    put<u32>(out, 0);
    const usize start = out.size();
    u16 n = 0;
    for (u8 u : pack_used_) n = static_cast<u16>(n + u);
    if (n > 0) {
        put<u16>(out, n);
        layouts_.resize(saved_.size());
        for (usize k = 0; k < saved_.size(); ++k) {
            if (!pack_used_[k]) continue;
            if (layouts_[k].empty()) data::append_schema(saved_[k].type, layouts_[k]);
            put<u64>(out, saved_[k].type->id);
            put<u64>(out, saved_[k].type->schema_hash());
            put<u32>(out, static_cast<u32>(layouts_[k].size()));
            out.insert(out.end(), layouts_[k].begin(), layouts_[k].end());
        }
    }
    const u32 size = static_cast<u32>(out.size() - start);
    std::memcpy(out.data() + size_at, &size, sizeof(size));
    out.insert(out.end(), pack_scratch_.begin(), pack_scratch_.end());
}

// The layouts of one chunk, parsed when a component needs one.
struct Scene::Layouts {
    struct Entry {
        u64 type_id = 0, schema = 0;
        std::span<const u8> bytes;
        bool parsed = false, ok = false;
        data::SavedSchema schema_of;
    };
    std::vector<Entry> entries;

    const data::SavedSchema* find(u64 type_id, u64 schema) {
        for (Entry& e : entries) {
            if (e.type_id != type_id || e.schema != schema) continue;
            if (!e.parsed) {
                e.parsed = true;
                std::span<const u8> b = e.bytes;
                e.ok = e.schema_of.parse(b);
            }
            return e.ok ? &e.schema_of : nullptr;
        }
        return nullptr;
    }
};

flecs::entity_t Scene::unpack_one(const u8*& p, const u8* end, u32& skipped, Layouts* layouts, u32* upgraded) {
    u16 components = 0;
    if (!take(p, end, components)) {
        p = end;
        return 0;
    }
    std::vector<std::max_align_t>& scratch = unpack_scratch_;
    flecs::entity_t e = ecs_new(ecs_.c_ptr());
    bool has_position = false;
    for (u16 c = 0; c < components; ++c) {
        u64 type_id = 0, schema = 0;
        u32 size = 0;
        if (!take(p, end, type_id) || !take(p, end, schema) || !take(p, end, size) ||
            static_cast<usize>(end - p) < size) {
            p = end;
            break;
        }
        std::span<const u8> payload(p, size);
        p += size;
        const SavedComponent* saved = nullptr;
        for (const SavedComponent& s : saved_)
            if (s.type->id == type_id) saved = &s;
        // A type the game no longer has: dropped. A changed one: read through
        // the layout the chunk was saved with, when it has one.
        const data::SavedSchema* old = nullptr;
        if (saved && saved->type->schema_hash() != schema && layouts) old = layouts->find(type_id, schema);
        if (!saved || (saved->type->schema_hash() != schema && !old)) {
            ++skipped;
            if (skipped_note_.size() < 200) {
                const reflect::TypeInfo* known = saved ? saved->type : reflect::find_type_by_id(type_id);
                char note[96];
                if (known) std::snprintf(note, sizeof(note), "%s%s (%s)", skipped_note_.empty() ? "" : ", ",
                                         known->name.c_str(), saved ? "changed" : "not used here");
                else std::snprintf(note, sizeof(note), "%sunknown type %016llx", skipped_note_.empty() ? "" : ", ",
                                   static_cast<unsigned long long>(type_id));
                skipped_note_ += note;
            }
            continue;
        }
        const reflect::TypeInfo* t = saved->type;
        scratch.resize((t->size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t) + 1);
        void* object = scratch.data();
        t->construct(object);
        const data::BinaryError read =
            old ? old->read(t, object, payload) : data::read_binary_payload(t, object, payload);
        if (read == data::BinaryError::None && old && upgraded) ++*upgraded;
        if (read == data::BinaryError::None) {
            ecs_set_id(ecs_.c_ptr(), e, saved->id, t->size, object);
            has_position |= t == reflect::type_of<Position>();
        } else {
            ++skipped;
        }
        t->destruct(object);
    }
    if (!has_position) {
        ecs_delete(ecs_.c_ptr(), e);
        return 0;
    }
    return e;
}

std::vector<u8> Scene::pack(flecs::entity_t e) {
    std::vector<u8> out;
    if (ecs_is_alive(ecs_.c_ptr(), e)) pack_entity(e, out);
    return out;
}

flecs::entity Scene::unpack(std::span<const u8> bytes) {
    const u8* p = bytes.data();
    u32 skipped = 0;
    const flecs::entity_t e = unpack_one(p, p + bytes.size(), skipped);
    skipped_note_.clear();
    if (!e) return flecs::entity();
    flecs::entity entity(ecs_, e);
    Position pos = entity.get<Position>();
    normalize(pos);
    if (!world_.find_chunk(pos.chunk())) {
        ecs_delete(ecs_.c_ptr(), e);
        return flecs::entity();
    }
    entity.set<Position>(pos);
    index_dirty_ = true;
    if (unpacked_fn_) unpacked_fn_(entity);
    return entity;
}

void Scene::unpack_chunk(ChunkCoord coord, const std::vector<u8>& bytes, bool& visited) {
    FORGE_ZONE();
    const u8* p = bytes.data();
    const u8* end = p + bytes.size();
    u32 magic = 0, count = 0;
    u8 visited_flag = 0;
    if (!take(p, end, magic) || (magic != kMagic && magic != kMagicV1) || !take(p, end, visited_flag) ||
        !take(p, end, count)) {
        FORGE_WARN("scene: damaged entity data for chunk %d,%d", coord.x, coord.y);
        return;
    }
    visited = visited_flag != 0;
    Layouts layouts;
    if (magic == kMagic) {
        u32 size = 0;
        if (!take(p, end, size) || static_cast<usize>(end - p) < size) {
            FORGE_WARN("scene: damaged entity data for chunk %d,%d", coord.x, coord.y);
            return;
        }
        const u8* q = p;
        const u8* table_end = p + size;
        p = table_end;
        u16 n = 0;
        if (size > 0 && take(q, table_end, n)) {
            for (u16 k = 0; k < n; ++k) {
                Layouts::Entry e;
                u32 len = 0;
                if (!take(q, table_end, e.type_id) || !take(q, table_end, e.schema) || !take(q, table_end, len) ||
                    static_cast<usize>(table_end - q) < len)
                    break;
                e.bytes = std::span<const u8>(q, len);
                q += len;
                layouts.entries.push_back(std::move(e));
            }
        }
    }
    u32 skipped = 0, upgraded = 0;
    for (u32 n = 0; n < count && p < end; ++n)
        if (const flecs::entity_t e = unpack_one(p, end, skipped, &layouts, &upgraded)) {
            ++unpacked_;
            if (unpacked_fn_) unpacked_fn_(flecs::entity(ecs_, e));
        }
    if (skipped)
        FORGE_WARN("scene: chunk %d,%d: %u components could not be read: %s", coord.x, coord.y, skipped,
                   skipped_note_.c_str());
    skipped_note_.clear();
    upgraded_ += upgraded;
    dropped_ += skipped;
}

void Scene::on_chunk_loaded(world::Chunk& chunk) {
    FORGE_ZONE_N("Scene chunk loaded");
    bool visited = false;
    if (auto it = stored_.find(chunk.coord); it != stored_.end()) {
        stored_bytes_ -= it->second.size();
        unpack_chunk(chunk.coord, it->second, visited);
        stored_.erase(it);
    } else if (world::ChunkLocation loc; store_ && store_->locate(chunk.coord, loc)) {
        std::vector<u8> bytes;
        if (world::read_chunk_bytes(loc, bytes)) unpack_chunk(chunk.coord, bytes, visited);
    }
    index_dirty_ = true;
    chunks_dirty_ = true;
    if (!visited) {
        // The populator may leave it unvisited again.
        unvisited_.erase(chunk.coord);
        if (populate_) populate_(chunk.coord, *this);
    }
}

void Scene::on_chunk_unloading(world::Chunk& chunk) {
    FORGE_ZONE_N("Scene chunk unloading");
    if (index_dirty_) rebuild_index();
    auto it = chunk_lookup_.find(chunk.coord);
    if (it == chunk_lookup_.end()) return;
    ChunkIndex& ci = chunks_[it->second];
    std::vector<u8> bytes;
    pack_chunk(ci, bytes, !unvisited_.count(chunk.coord));
    for (u32 i = ci.begin; i < ci.end; ++i) ecs_delete(ecs_.c_ptr(), items_[i].entity);
    packed_ += ci.end - ci.begin;
    stored_bytes_ += bytes.size();
    if (auto old = stored_.find(chunk.coord); old != stored_.end()) stored_bytes_ -= old->second.size();
    stored_[chunk.coord] = std::move(bytes);
    // Gone from the index until the next rebuild.
    ci.end = ci.begin;
    std::fill(std::begin(ci.cell_start), std::end(ci.cell_start), ci.begin);
    if (!grid_.empty() && grid_rect_.contains(chunk.coord.x, chunk.coord.y)) {
        const i64 w = grid_rect_.x1 - grid_rect_.x0;
        grid_[static_cast<usize>((chunk.coord.y - grid_rect_.y0) * w + (chunk.coord.x - grid_rect_.x0))] = -1;
    }
    chunk_lookup_.erase(it);
    chunks_dirty_ = true;
}

void Scene::update() {
    FORGE_ZONE_N("Scene update");
    rebuild_index();
}

void Scene::rebuild_chunk_list() {
    FORGE_ZONE();
    chunks_dirty_ = false;
    chunks_.clear();
    chunk_lookup_.clear();
    world::Rect box{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    world_.for_each_ready([&](world::Chunk& c) {
        chunk_lookup_.emplace(c.coord, static_cast<u32>(chunks_.size()));
        ChunkIndex ci;
        ci.coord = c.coord;
        chunks_.push_back(ci);
        box.x0 = std::min(box.x0, c.coord.x);
        box.y0 = std::min(box.y0, c.coord.y);
        box.x1 = std::max(box.x1, c.coord.x + 1);
        box.y1 = std::max(box.y1, c.coord.y + 1);
    });
    grid_.clear();
    if (!chunks_.empty() && static_cast<i64>(box.x1 - box.x0) * (box.y1 - box.y0) <= kMaxGridCells) {
        grid_rect_ = box;
        grid_.assign(static_cast<usize>(box.x1 - box.x0) * static_cast<usize>(box.y1 - box.y0), -1);
        const i64 w = box.x1 - box.x0;
        for (u32 i = 0; i < chunks_.size(); ++i) {
            const ChunkCoord c = chunks_[i].coord;
            grid_[static_cast<usize>((c.y - box.y0) * w + (c.x - box.x0))] = static_cast<i32>(i);
        }
    }
}

void Scene::rebuild_index() {
    FORGE_ZONE_N("Rebuild scene index");
    index_dirty_ = false;
    moved_out_ = 0;
    // Loaded chunks get dense numbers; only redone when chunks came or went.
    if (chunks_dirty_) rebuild_chunk_list();
    const u32 chunk_count = static_cast<u32>(chunks_.size());

    // Every entity with a Position, table by table, in blocks of up to 4096.
    struct Block {
        const flecs::entity_t* entities;
        Position* positions;
        u32 count;
        u32 offset;
    };
    std::vector<Block> blocks;
    u32 total = 0;
    positions_.run([&](flecs::iter& it) {
        while (it.next()) {
            const u32 n = static_cast<u32>(it.count());
            const flecs::entity_t* ents = it.c_ptr()->entities;
            Position* pos = &it.field<Position>(0)[0];
            for (u32 b = 0; b < n; b += 4096) {
                const u32 m = std::min<u32>(4096, n - b);
                blocks.push_back({ents + b, pos + b, m, total});
                total += m;
            }
        }
    });
    const u32 block_count = static_cast<u32>(blocks.size());

    // Pass 1, parallel per block: normalize positions, find each entity's
    // chunk, count entities per chunk in this block.
    if (scratch_capacity_ < total) {
        // Reused between frames: a fresh 5 MB buffer every frame costs more
        // in page faults than the whole pass.
        scratch_capacity_ = total + total / 4;
        scratch_.reset(new Item[scratch_capacity_]);
    }
    Item* unsorted = scratch_.get();
    block_counts_.assign(static_cast<usize>(block_count) * chunk_count, 0);
    jobs::parallel_for(block_count, 1, [&](u32 b, u32 e) {
        for (u32 k = b; k < e; ++k) {
            const Block& block = blocks[k];
            u32* counts = block_counts_.data() + static_cast<usize>(k) * chunk_count;
            for (u32 i = 0; i < block.count; ++i) {
                Position& p = block.positions[i];
                normalize(p);
                const i32 chunk = dense_index(p.chunk());
                unsorted[block.offset + i] = {block.entities[i], p.x, p.y, chunk};
                if (chunk >= 0) ++counts[chunk];
            }
        }
    });

    // Where each block's entities of each chunk go (blocks in order, so the
    // result does not depend on thread timing).
    u32 running = 0;
    for (u32 c = 0; c < chunk_count; ++c) {
        chunks_[c].begin = running;
        for (u32 k = 0; k < block_count; ++k) {
            u32& slot = block_counts_[static_cast<usize>(k) * chunk_count + c];
            const u32 n = slot;
            slot = running; // now: the next write position
            running += n;
        }
        chunks_[c].end = running;
    }

    // Pass 2, parallel per block: scatter into chunk order.
    items_.resize(running);
    jobs::parallel_for(block_count, 1, [&](u32 b, u32 e) {
        for (u32 k = b; k < e; ++k) {
            const Block& block = blocks[k];
            u32* next = block_counts_.data() + static_cast<usize>(k) * chunk_count;
            for (u32 i = 0; i < block.count; ++i) {
                const Item& item = unsorted[block.offset + i];
                if (item.chunk >= 0) items_[next[item.chunk]++] = item;
            }
        }
    });

    // Pass 3, parallel per chunk: order by 8×8 cell.
    jobs::parallel_for(chunk_count, 16, [&](u32 b, u32 e) {
        std::vector<Item> tmp;
        for (u32 c = b; c < e; ++c) {
            ChunkIndex& ci = chunks_[c];
            u32 cell_counts[kCells * kCells] = {};
            for (u32 i = ci.begin; i < ci.end; ++i) ++cell_counts[cell_of(items_[i].x, items_[i].y)];
            u32 at = ci.begin;
            for (u32 k = 0; k < kCells * kCells; ++k) {
                ci.cell_start[k] = at;
                at += cell_counts[k];
            }
            ci.cell_start[kCells * kCells] = at;
            tmp.assign(items_.begin() + ci.begin, items_.begin() + ci.end);
            u32 fill[kCells * kCells];
            std::copy(ci.cell_start, ci.cell_start + kCells * kCells, fill);
            for (const Item& item : tmp) items_[fill[cell_of(item.x, item.y)]++] = item;
        }
    });

    // Entities that walked out of the loaded area: packed into their chunk.
    // Rare (only at the edges), so done last and one by one.
    if (running != total) {
        for (u32 k = 0; k < total; ++k) {
            if (unsorted[k].chunk >= 0) continue;
            const flecs::entity_t entity = unsorted[k].entity;
            const Position* p = ecs_.entity(entity).try_get<Position>();
            std::vector<u8>& bytes = stored_for(p->chunk());
            const usize before = bytes.size();
            u32 n = 0;
            std::memcpy(&n, bytes.data() + 5, sizeof(n));
            ++n;
            std::memcpy(bytes.data() + 5, &n, sizeof(n));
            pack_entity(entity, bytes);
            stored_bytes_ += bytes.size() - before;
            ecs_delete(ecs_.c_ptr(), entity);
            ++moved_out_;
            ++packed_;
        }
    }
}

u32 Scene::count_in_chunk(ChunkCoord chunk) const {
    auto it = chunk_lookup_.find(chunk);
    return it == chunk_lookup_.end() ? 0 : chunks_[it->second].end - chunks_[it->second].begin;
}

void Scene::query_radius(f64 tile_x, f64 tile_y, f32 radius, std::vector<flecs::entity_t>& out) const {
    const f64 r = radius;
    const world::Rect tiles{static_cast<i32>(std::floor(tile_x - r)), static_cast<i32>(std::floor(tile_y - r)),
                            static_cast<i32>(std::floor(tile_x + r)) + 1, static_cast<i32>(std::floor(tile_y + r)) + 1};
    const world::Rect cr = world::chunks_of(tiles);
    const f32 r2 = radius * radius;
    for (i32 cy = cr.y0; cy < cr.y1; ++cy) {
        for (i32 cx = cr.x0; cx < cr.x1; ++cx) {
            const i32 d = dense_index({cx, cy});
            if (d < 0) continue;
            const ChunkIndex& ci = chunks_[static_cast<usize>(d)];
            if (ci.begin == ci.end) continue;
            // The query centre in this chunk's local tiles.
            const f32 lx = static_cast<f32>(tile_x - static_cast<f64>(cx) * kChunkSize);
            const f32 ly = static_cast<f32>(tile_y - static_cast<f64>(cy) * kChunkSize);
            const i32 c0x = std::clamp(static_cast<i32>(std::floor((lx - radius) / (1 << kCellShift))), 0, kCells - 1);
            const i32 c1x = std::clamp(static_cast<i32>(std::floor((lx + radius) / (1 << kCellShift))), 0, kCells - 1);
            const i32 c0y = std::clamp(static_cast<i32>(std::floor((ly - radius) / (1 << kCellShift))), 0, kCells - 1);
            const i32 c1y = std::clamp(static_cast<i32>(std::floor((ly + radius) / (1 << kCellShift))), 0, kCells - 1);
            for (i32 row = c0y; row <= c1y; ++row) {
                // Cells of one row are next to each other in memory.
                const u32 from = ci.cell_start[row * kCells + c0x];
                const u32 to = ci.cell_start[row * kCells + c1x + 1];
                for (u32 i = from; i < to; ++i) {
                    const f32 dx = items_[i].x - lx, dy = items_[i].y - ly;
                    if (dx * dx + dy * dy <= r2) out.push_back(items_[i].entity);
                }
            }
        }
    }
}

bool Scene::open_save(const std::filesystem::path& folder, std::string* error) {
    auto store = std::make_unique<world::RegionStore>("e");
    if (!store->open(folder, kSaveTag, error)) return false;
    store_ = std::move(store);
    return true;
}

SceneSaveReport Scene::save() {
    FORGE_ZONE_N("Scene save");
    if (!store_) return {};
    return write_chunks(*store_, true);
}

SceneSaveReport Scene::save_copy(const std::filesystem::path& folder, flecs::entity_t leave_out) {
    FORGE_ZONE_N("Scene save copy");
    if (!store_) return {};
    world::RegionStore copy("e");
    std::string error;
    if (!copy.open(folder, kSaveTag, &error)) {
        FORGE_ERROR("scene save: %s", error.c_str());
        return {};
    }
    pack_leave_out_ = leave_out;
    const SceneSaveReport report = write_chunks(copy, false);
    pack_leave_out_ = 0;
    return report;
}

void Scene::moved_save(const std::filesystem::path& folder) {
    if (store_) store_->moved(folder);
}

SceneSaveReport Scene::write_chunks(world::RegionStore& into, bool saved) {
    SceneSaveReport report;
    const u64 start = time_now_ns();
    if (index_dirty_) rebuild_index();

    // Loaded chunks are written even when empty: the mark that their
    // populator already ran.
    std::vector<std::vector<u8>> packed(chunks_.size());
    for (usize c = 0; c < chunks_.size(); ++c) pack_chunk(chunks_[c], packed[c], !unvisited_.count(chunks_[c].coord));
    std::vector<world::RegionStore::Write> writes;
    for (usize c = 0; c < chunks_.size(); ++c) writes.push_back({chunks_[c].coord, &packed[c]});
    for (const auto& [coord, bytes] : stored_) writes.push_back({coord, &bytes});

    report.ok = into.write(writes, &report.regions, &report.bytes);
    report.chunks = static_cast<u32>(writes.size());
    if (report.ok && saved) {
        stored_.clear();
        stored_bytes_ = 0;
    }
    report.ms = ns_to_ms(time_now_ns() - start);
    return report;
}

std::vector<ChunkCoord> Scene::loaded_unvisited() const {
    std::vector<ChunkCoord> out;
    for (const ChunkCoord c : unvisited_)
        if (world_.find_chunk(c)) out.push_back(c);
    return out;
}

std::vector<ChunkCoord> Scene::unloaded_with_entities() const {
    // [u32 magic][u8 visited][u32 entity count]
    auto has_entities = [](const std::vector<u8>& bytes) {
        u32 count = 0;
        if (bytes.size() < 9) return false;
        std::memcpy(&count, bytes.data() + 5, sizeof(count));
        return count > 0;
    };
    std::vector<ChunkCoord> out;
    for (const auto& [coord, bytes] : stored_)
        if (has_entities(bytes)) out.push_back(coord);
    if (store_)
        for (const ChunkCoord c : store_->chunks()) {
            if (stored_.count(c) || world_.find_chunk(c)) continue;
            world::ChunkLocation loc;
            std::vector<u8> bytes;
            if (store_->locate(c, loc) && world::read_chunk_bytes(loc, bytes) && has_entities(bytes)) out.push_back(c);
        }
    return out;
}

SceneStats Scene::stats() const {
    SceneStats s;
    for (const ChunkIndex& c : chunks_) s.entities += c.end - c.begin;
    s.chunks_indexed = static_cast<u32>(chunks_.size());
    s.stored_chunks = static_cast<u32>(stored_.size());
    s.stored_bytes = stored_bytes_;
    s.moved_out = moved_out_;
    s.packed = packed_;
    s.unpacked = unpacked_;
    s.upgraded = upgraded_;
    s.dropped = dropped_;
    return s;
}

} // namespace forge::scene
