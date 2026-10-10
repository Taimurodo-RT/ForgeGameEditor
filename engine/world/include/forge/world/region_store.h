#pragma once

// Changed chunks on disk, grouped into region files of 32 × 32 chunks, the
// way Minecraft and Terraria-likes keep huge worlds: a save touches only the
// regions that changed, and a chunk is read straight from its region when the
// player comes back. Untouched chunks are never written; the generator makes
// them again.
//
// Region file "<prefix>.<x>.<y>.fwr" (prefix "r" for tiles, "e" for
// entities), little endian:
//   header   "FWR1", u32 version, i32 region x, i32 region y, u32 tag, u32 count
//   entries  count × { u32 chunk index in region (y * 32 + x), u32 offset, u32 size }
//   data     encoded chunks (see encode_chunk)

#include "forge/core/types.h"
#include "forge/world/coords.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace forge::world {

inline constexpr i32 kRegionShift = 5;
inline constexpr i32 kRegionSize = 1 << kRegionShift; // 32 chunks = 2048 tiles

inline ChunkCoord region_of(ChunkCoord c) { return {c.x >> kRegionShift, c.y >> kRegionShift}; }

// Where one chunk's bytes are. Reading it needs nothing else, so any thread can.
struct ChunkLocation {
    std::filesystem::path file;
    u32 offset = 0;
    u32 size = 0;
};

bool read_chunk_bytes(const ChunkLocation& location, std::vector<u8>& out);

class RegionStore {
public:
    // Region files are named "<prefix>.<x>.<y>.fwr", so several stores can
    // share one save folder.
    explicit RegionStore(std::string prefix = "r") : prefix_(std::move(prefix)) {}

    // Reads the headers of every region in the folder (creating the folder if
    // needed). tag guards against reading data of another layout (tiles: the
    // layer count). Damaged region files are skipped with a warning.
    bool open(const std::filesystem::path& folder, u32 tag, std::string* error = nullptr);

    bool locate(ChunkCoord chunk, ChunkLocation& out) const;
    usize chunk_count() const;
    // Every chunk saved here.
    std::vector<ChunkCoord> chunks() const;
    usize region_count() const { return regions_.size(); }

    struct Write {
        ChunkCoord chunk;
        const std::vector<u8>* bytes; // null: the chunk is removed
    };
    // Writes these chunks, keeping every other chunk already saved in the
    // same regions. Each region file is replaced atomically; a region left
    // with no chunks is removed. No chunk may be read from this store while
    // it runs.
    bool write(const std::vector<Write>& chunks, u32* regions_written = nullptr, usize* bytes_written = nullptr);
    // The folder was moved (renamed) to another place, its files as they were: they are read and written there
    // from now on.
    void moved(const std::filesystem::path& folder);

private:
    struct Entry {
        u32 offset = 0;
        u32 size = 0;
    };
    struct Region {
        std::filesystem::path file;
        std::unordered_map<u32, Entry> entries; // by chunk index inside the region
    };

    bool load_header(const std::filesystem::path& file);
    std::filesystem::path region_path(ChunkCoord region) const;

    std::string prefix_;
    std::filesystem::path folder_;
    u32 tag_ = 0;
    std::unordered_map<ChunkCoord, Region, ChunkCoordHash> regions_;
};

} // namespace forge::world
