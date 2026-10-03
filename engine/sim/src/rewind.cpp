#include "forge/sim/rewind.h"

#include "forge/core/profile.h"
#include "forge/sim/bodies.h"
#include "forge/sim/rigid.h"
#include "forge/sim/time.h"
#include "forge/sim/zones.h"

#include <algorithm>
#include <cstring>
#include <unordered_set>

namespace forge::sim {

// An entity record: u64 id, u32 mask of tracked components present, then
// their bytes in tracking order (Position is always the first).

Rewind::Rewind(world::World& world, scene::Scene& scene, const RewindDesc& desc, f32 tick_seconds)
    : world_(world), scene_(scene), desc_(desc), tick_seconds_(tick_seconds), recording_(desc.record) {
    flecs::world& ecs = scene_.ecs();
    outside_id_ = ecs.component<OutsideTime>().id();
    positions_ = ecs.query<scene::Position>();
    track_id(ecs.component<scene::Position>().id(), sizeof(scene::Position));
    track_id(ecs.component<Body>().id(), sizeof(Body));
    track_id(ecs.component<RigidBody>().id(), sizeof(RigidBody));
}

Rewind::~Rewind() { positions_ = {}; }

void Rewind::track_id(flecs::entity_t id, u32 size) {
    for (const Tracked& t : tracked_)
        if (t.id == id) return;
    if (tracked_.size() < 32) tracked_.push_back({id, size});
}

bool Rewind::start(f32 speed) {
    if (frames_.empty()) return false;
    speed_ = speed;
    playing_ = true;
    applied_ = nullptr;
    return true;
}

void Rewind::stop() {
    if (!playing_) return;
    playing_ = false;
    if (applied_) time_ = applied_->time;
    // What lay ahead is forgotten.
    while (!frames_.empty() && frames_.back().time > time_) {
        bytes_ -= frames_.back().entities.size();
        frames_.pop_back();
    }
    for (auto& [c, h] : chunks_) {
        while (h.copies.size() > 1 && h.copies.back().time > time_) {
            bytes_ -= h.copies.back().tiles.size() * sizeof(world::TileId);
            h.copies.pop_back();
        }
        h.written = nullptr;
    }
    applied_ = nullptr;
    last_record_ = time_;
}

void Rewind::set_recording(bool on) {
    recording_ = on;
    if (!on) clear();
}

void Rewind::clear() {
    frames_.clear();
    chunks_.clear();
    bytes_ = 0;
    applied_ = nullptr;
    playing_ = false;
    last_record_ = -1e30;
}

RewindStats Rewind::stats() const {
    RewindStats s;
    s.frames = static_cast<u32>(frames_.size());
    s.entities = frames_.empty() ? 0 : frames_.back().count;
    for (const auto& [c, h] : chunks_) s.chunk_copies += static_cast<u32>(h.copies.size());
    s.bytes = bytes_;
    s.seconds = frames_.empty() ? 0.0f : static_cast<f32>((time_ - frames_.front().time) * tick_seconds_);
    return s;
}

void Rewind::on_chunk_loaded(world::ChunkCoord c) { loaded_at_[c] = time_; }

void Rewind::on_chunk_unloading(world::ChunkCoord c) {
    loaded_at_.erase(c);
    auto it = chunks_.find(c);
    if (it == chunks_.end()) return;
    for (const ChunkCopy& copy : it->second.copies) bytes_ -= copy.tiles.size() * sizeof(world::TileId);
    chunks_.erase(it);
}

void Rewind::after_tick(f32 world_scale, const Zones& zones) {
    if (playing_ || world_scale <= 0) return;
    time_ += world_scale;
    if (!recording_ || time_ - last_record_ < static_cast<f64>(desc_.every)) return;
    record(zones);
    last_record_ = time_;
    trim();
}

void Rewind::record(const Zones& zones) {
    FORGE_ZONE_N("Rewind record");
    Frame& f = frames_.emplace_back();
    f.time = time_;
    const usize layer_tiles = static_cast<usize>(world_.layer_count()) * world::kChunkTiles;
    world_.for_each_ready([&](world::Chunk& chunk) {
        if (zones.zone_of(chunk.coord) == Zone::Asleep) return;
        f.chunks.push_back(chunk.coord);
        ChunkHistory& h = chunks_[chunk.coord];
        if (!h.copies.empty() && h.copies.back().revision == chunk.revision) return;
        ChunkCopy& copy = h.copies.emplace_back();
        copy.time = time_;
        copy.revision = chunk.revision;
        copy.tiles.assign(chunk.tiles, chunk.tiles + layer_tiles);
        bytes_ += layer_tiles * sizeof(world::TileId);
    });
    std::sort(f.chunks.begin(), f.chunks.end(),
              [](world::ChunkCoord a, world::ChunkCoord b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });

    ecs_world_t* w = scene_.ecs().c_ptr();
    const u8* cols[32];
    positions_.run([&](flecs::iter& it) {
        while (it.next()) {
            ecs_table_t* table = it.c_ptr()->table;
            if (ecs_table_has_id(w, table, outside_id_)) continue;
            const i32 offset = it.c_ptr()->offset;
            u32 mask = 0;
            usize bytes = 12;
            for (usize k = 0; k < tracked_.size(); ++k) {
                cols[k] = static_cast<const u8*>(ecs_table_get_id(w, table, tracked_[k].id, offset));
                if (cols[k]) {
                    mask |= 1u << k;
                    bytes += tracked_[k].size;
                }
            }
            const scene::Position* pos = &it.field<scene::Position>(0)[0];
            const flecs::entity_t* ents = it.c_ptr()->entities;
            const u32 n = static_cast<u32>(it.count());
            for (u32 i = 0; i < n; ++i) {
                if (zones.zone_of(pos[i].chunk()) == Zone::Asleep) continue;
                const usize at = f.entities.size();
                f.entities.resize(at + bytes);
                u8* out = f.entities.data() + at;
                const u64 id = ents[i];
                std::memcpy(out, &id, 8);
                std::memcpy(out + 8, &mask, 4);
                out += 12;
                for (usize k = 0; k < tracked_.size(); ++k) {
                    if (!cols[k]) continue;
                    std::memcpy(out, cols[k] + static_cast<usize>(i) * tracked_[k].size, tracked_[k].size);
                    out += tracked_[k].size;
                }
                ++f.count;
            }
        }
    });
    f.entities.shrink_to_fit();
    bytes_ += f.entities.size();
}

void Rewind::trim() {
    const f64 oldest = time_ - static_cast<f64>(desc_.seconds / tick_seconds_);
    while (frames_.size() > 1 && (frames_.front().time < oldest || bytes_ > desc_.max_bytes)) {
        bytes_ -= frames_.front().entities.size();
        frames_.pop_front();
    }
    if (frames_.empty()) return;
    // Chunk copies older than the oldest frame: keep the last of them, the
    // state at that frame.
    const f64 first = frames_.front().time;
    for (auto& [c, h] : chunks_)
        while (h.copies.size() > 1 && h.copies[1].time <= first) {
            bytes_ -= h.copies.front().tiles.size() * sizeof(world::TileId);
            h.copies.pop_front();
        }
}

bool Rewind::play_tick() {
    if (!playing_ || frames_.empty()) return false;
    time_ = std::max(time_ - static_cast<f64>(speed_), frames_.front().time);
    // The latest recording at or before the time we went back to.
    const Frame* f = &frames_.front();
    for (auto it = frames_.rbegin(); it != frames_.rend(); ++it)
        if (it->time <= time_) {
            f = &*it;
            break;
        }
    if (f == applied_) return false;
    apply(*f);
    applied_ = f;
    return true;
}

void Rewind::apply(const Frame& frame) {
    FORGE_ZONE_N("Rewind restore");
    const f64 t = frame.time;
    auto loaded_by = [&](world::ChunkCoord c) {
        auto it = loaded_at_.find(c);
        return it == loaded_at_.end() || it->second <= t;
    };

    // Tiles: each chunk as it was at that time.
    restored_.clear();
    const usize layer_tiles = static_cast<usize>(world_.layer_count()) * world::kChunkTiles;
    for (auto& [c, h] : chunks_) {
        world::Chunk* chunk = world_.find_chunk(c);
        if (!chunk || !chunk->tiles || h.copies.empty()) continue;
        const ChunkCopy* best = &h.copies.front();
        for (const ChunkCopy& copy : h.copies) {
            if (copy.time > t) break;
            best = &copy;
        }
        if (h.written ? (h.written == best && chunk->revision == h.written_revision) : best->revision == chunk->revision)
            continue;
        std::memcpy(chunk->tiles, best->tiles.data(), layer_tiles * sizeof(world::TileId));
        ++chunk->revision;
        h.written = best;
        h.written_revision = chunk->revision;
        restored_.push_back(c);
    }

    // Entities: those that came since go, the recorded ones are put back.
    ecs_world_t* w = scene_.ecs().c_ptr();
    std::unordered_set<flecs::entity_t> recorded;
    recorded.reserve(frame.count);
    for (usize at = 0; at < frame.entities.size();) {
        u64 id;
        u32 mask;
        std::memcpy(&id, frame.entities.data() + at, 8);
        std::memcpy(&mask, frame.entities.data() + at + 8, 4);
        recorded.insert(id);
        at += 12;
        for (usize k = 0; k < tracked_.size(); ++k)
            if (mask & (1u << k)) at += tracked_[k].size;
    }
    std::vector<flecs::entity_t> gone;
    positions_.run([&](flecs::iter& it) {
        while (it.next()) {
            if (ecs_table_has_id(w, it.c_ptr()->table, outside_id_)) continue;
            const scene::Position* pos = &it.field<scene::Position>(0)[0];
            const flecs::entity_t* ents = it.c_ptr()->entities;
            for (usize i = 0; i < it.count(); ++i) {
                if (recorded.count(ents[i])) continue;
                const world::ChunkCoord c = pos[i].chunk();
                const bool watched = std::binary_search(frame.chunks.begin(), frame.chunks.end(), c,
                                                        [](world::ChunkCoord a, world::ChunkCoord b) {
                                                            return a.y != b.y ? a.y < b.y : a.x < b.x;
                                                        });
                if (watched && loaded_by(c)) gone.push_back(ents[i]);
            }
        }
    });
    for (flecs::entity_t e : gone) ecs_delete(w, e);

    for (usize at = 0; at < frame.entities.size();) {
        u64 id;
        u32 mask;
        std::memcpy(&id, frame.entities.data() + at, 8);
        std::memcpy(&mask, frame.entities.data() + at + 8, 4);
        const u8* data = frame.entities.data() + at + 12;
        at += 12;
        for (usize k = 0; k < tracked_.size(); ++k)
            if (mask & (1u << k)) at += tracked_[k].size;
        scene::Position p;
        std::memcpy(&p, data, sizeof(p));
        if (!ecs_is_alive(w, id)) {
            // Destroyed since: back, if its chunk has been here all along
            // (one that left and came back has it already, under a new id)
            // and nothing else took its slot.
            world::Chunk* chunk = world_.find_chunk(p.chunk());
            if (!chunk || !chunk->tiles || !loaded_by(p.chunk())) continue;
            const flecs::entity_t holder = ecs_get_alive(w, static_cast<u32>(id));
            if (holder != 0 && holder != id) continue;
            ecs_make_alive(w, id);
        }
        for (usize k = 0; k < tracked_.size(); ++k) {
            if (mask & (1u << k)) {
                ecs_set_id(w, id, tracked_[k].id, tracked_[k].size, data);
                data += tracked_[k].size;
            } else if (ecs_has_id(w, id, tracked_[k].id)) {
                ecs_remove_id(w, id, tracked_[k].id);
            }
        }
    }
}

} // namespace forge::sim
