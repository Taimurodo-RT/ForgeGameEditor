#pragma once

// What an import of a Tiled map made of it, kept in the level folder
// (tiled.json), so a new import of the same map updates what it made before:
// the same own tiles for the same tiles, the same objects and zones for the
// same objects of the map (tiled_import.h).

#include "forge/core/types.h"
#include "forge/world/coords.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace forge::level::tiled {

inline constexpr const char* kRecordFile = "tiled.json";
inline constexpr const char* kBrokenRecordFile = "tiled.broken.json";

struct Record {
    std::string map;                                             // the map's file name
    i32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;                          // the cells it wrote
    std::vector<std::pair<std::string, world::TileId>> tiles;    // a stack of tiles → own tile id
    std::vector<std::pair<u32, u64>> objects;                    // Tiled object id → LevelId
    std::vector<std::pair<u32, u64>> zones;                      // Tiled object id → area id
    std::vector<std::pair<std::string, std::string>> templates;  // a tile's picture → template id
    u32 spawn = 0;                                               // the Tiled id of the point that set the spawn point
    bool empty() const { return map.empty(); }
    friend bool operator==(const Record&, const Record&) = default;
};
// No file: true, out empty, *found false. A file that cannot be used: false
// (out unchanged), *error in the author's words.
bool load_record(const std::filesystem::path& folder, Record& out, bool* found = nullptr, std::string* error = nullptr);
// An empty record removes the file.
bool save_record(const std::filesystem::path& folder, const Record& r, std::string* error = nullptr);
std::string record_json(const Record& r);

} // namespace forge::level::tiled
