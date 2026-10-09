#pragma once

// The level's own tiles: kinds of tile with pictures of their own, besides
// the game's (an import from Tiled makes them), and what lies around the part
// of the world the level holds. Kept in the level folder: tiles.json (the
// kinds), tiles.png (their pictures) and world.json, so the folder is all a
// game needs to show them.
//
// An own tile is a value on one of the game's tile layers like any other;
// the game draws it from its picture, it stops the hero (and light) when it
// is solid, and nothing digs it.

#include "forge/core/types.h"
#include "forge/world/coords.h"

#include <filesystem>
#include <string>
#include <vector>

namespace forge::level {

inline constexpr const char* kTilesFile = "tiles.json";
inline constexpr const char* kTilesPicture = "tiles.png";
inline constexpr const char* kWorldFile = "world.json";
// Where tiles.json and tiles.png that could not be read are kept when the
// level writes new ones over them.
inline constexpr const char* kBrokenTilesFile = "tiles.broken.json";
inline constexpr const char* kBrokenTilesPicture = "tiles.broken.png";

// Own tiles have ids from here on; the game's are below.
inline constexpr world::TileId kFirstOwnTile = 256;
inline constexpr u32 kMaxOwnTiles = 4096;
// The side of their pictures, in pixels (all of a level's the same).
inline constexpr u32 kMinOwnTilePx = 4;
inline constexpr u32 kMaxOwnTilePx = 128;
// A game draws its tiles and the level's from one square picture (an atlas)
// of cells of max(16, px) pixels, cell N for id N; its side is at most this,
// which every GPU can hold.
inline constexpr u32 kMaxAtlasPx = 4096;
// How many own tiles a level with pictures of px may have: as many as fit in
// such an atlas after the game's 256, and kMaxOwnTiles at most.
u32 max_own_tiles(u32 px);
inline constexpr usize kMaxOwnTileName = 60;
// tiles.png has this many pictures in a row (fewer when there are fewer).
inline constexpr u32 kOwnTileColumns = 16;

struct OwnTile {
    world::TileId id = 0;
    std::string name;   // "Трава"
    u32 layer = 0;      // the tile layer it is painted on
    bool solid = false; // stops the hero, and light on the blocking layer
    std::string from;   // where it came from, in words; may be empty
    friend bool operator==(const OwnTile&, const OwnTile&) = default;
};

struct LevelTiles {
    u32 px = 0;                 // the side of every picture; 0 when there are none
    std::vector<OwnTile> tiles; // in the order of their pictures
    std::vector<u8> rgba;       // tiles.size() pictures of px × px RGBA8, one after another
    bool empty() const { return tiles.empty(); }
    const OwnTile* find(world::TileId id) const;
    // The picture of tiles[index].
    const u8* picture(usize index) const { return rgba.data() + index * px * px * 4; }
    friend bool operator==(const LevelTiles&, const LevelTiles&) = default;
};

// What is wrong with them, in the author's words; empty when nothing is.
// Layers are those of a game with layer_count tile layers; liquids (when the
// game has a layer of them) is not one an own tile may be on.
std::string tiles_problem(const LevelTiles& t, u32 layer_count, i32 liquids_layer = -1);
// Reads folder/tiles.json and tiles.png. No tiles.json: true, out empty,
// *found false. Files that cannot be used: false, out unchanged, *error in
// the author's words.
bool load_tiles(const std::filesystem::path& folder, LevelTiles& out, u32 layer_count, i32 liquids_layer = -1,
                bool* found = nullptr, std::string* error = nullptr);
// Writes tiles.png and tiles.json as a pair: both are written aside first
// (*.tmp), the old pair moved aside (*.old), the new one put in its place.
// None: removes both, tiles.json first. Whatever fails, false with the reason
// and the pair that was there stays as it was.
bool save_tiles(const std::filesystem::path& folder, const LevelTiles& t, std::string* error = nullptr);
// tiles.json as written.
std::string tiles_json(const LevelTiles& t);

// What lies around the level's own chunks: the game's world, made by its
// generator and peopled when first visited (as levels always had), or
// nothing: no tiles, no villagers or critters.
struct LevelWorld {
    bool empty_around = false;
    friend bool operator==(const LevelWorld&, const LevelWorld&) = default;
};
// As load_tiles, for folder/world.json ({"around": "game"} or "empty").
bool load_world(const std::filesystem::path& folder, LevelWorld& out, bool* found = nullptr, std::string* error = nullptr);
// The game's world around: removes world.json (a level without it is one).
bool save_world(const std::filesystem::path& folder, const LevelWorld& w, std::string* error = nullptr);

} // namespace forge::level
