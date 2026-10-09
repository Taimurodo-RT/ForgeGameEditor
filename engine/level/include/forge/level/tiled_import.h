#pragma once

// What a map of Tiled becomes in a level, worked out before anything changes
// (the import window shows it, and applies it as one history step):
//
// - one Tiled cell is one cell of the level, (0, 0) at (0, 0), y down in
//   both; the chunks of an infinite map at negative coordinates too;
// - each tile layer goes to the level's background («Фон») or blocks
//   («Блоки») layer, or nowhere; the tiles of the layers that go to one level
//   layer at one cell, in Tiled's order, become one tile of the level's own
//   (a picture of them over each other, flips and turns baked in), the same
//   stack everywhere the same tile;
// - a rectangle object becomes a zone of «Зоны» (its "music" property, a
//   file, the zone's music), a point named or of class "spawn" the hero's
//   spawn point, a tile object a «Картинка» object of a template made of its
//   picture;
// - nothing around the level (world.json): the map is the whole world.
//
// What does not come over (other shapes, rotation, layer opacity, tile
// offsets, …) and what is missing (a .tsx, a picture, a music file) is
// counted and told in words, never dropped silently. A new import of the same
// map (tiled.json remembers what each thing became) updates what it made
// before: the same ids for the same tiles, objects and zones, no doubles;
// what the map no longer has goes; what the author added stays.

#include "forge/level/level.h"
#include "forge/level/tiled.h"
#include "forge/level/tiled_record.h"

#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace forge::level::tiled {

// The largest map: cells it may cover (2048 × 2048).
inline constexpr u64 kMaxImportCells = 2048ull * 2048ull;

// Where a tile layer goes.
enum class Target : u8 { Walls, Blocks, Skip };
// A hidden layer goes nowhere; a layer with the bool property solid,
// collides or collision set, or all of whose tiles have collision shapes, to
// the blocks; any other to the background.
Target default_target(const Map& map, const Layer& layer);

// "Объекты-эллипсы" — 3.
struct Note {
    std::string what;
    u64 count = 0;
};

// A picture a template is made of: a tile, turned as the object shows it.
struct Picture {
    std::string key;         // the tile and its flips, as tiled.json keeps it
    std::string name;        // "Сундук"
    std::string template_id; // the template's id: an earlier import's or a new one
    bool known = false;      // the template was made by an earlier import
    u32 w = 0, h = 0;
    std::vector<u8> rgba;
};

struct PlannedObject {
    u32 tiled_id = 0;
    std::string name;  // Tiled's, for the window
    usize picture = 0; // in Plan::pictures
    f64 x = 0, y = 0;  // its centre, in cells
    f64 half_height = 0.5;
    u64 level_id = 0;  // its LevelId: an earlier import's or a new one
    bool known = false;
};

struct Plan {
    std::string map_name;
    u32 px = 0;                              // the side of a cell, in pixels
    i32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;      // the cells written, right and bottom edges out
    std::vector<world::TileId> walls, blocks; // their new values, row by row (0: empty)
    LevelTiles tiles;                        // the level's own tiles after
    u32 tiles_new = 0, tiles_updated = 0;
    LevelAreas areas;                        // the level's zones and spawn point after
    u32 zones_new = 0, zones_updated = 0, zones_removed = 0;
    std::vector<std::pair<std::filesystem::path, std::string>> music; // a file → its name in the game's sounds
    std::vector<Picture> pictures;
    std::vector<PlannedObject> objects;
    std::vector<u64> remove_objects;         // an earlier import's objects the map no longer has
    std::vector<Note> skipped;               // what does not come over, and why
    std::vector<Note> missing;               // files that are not there
    Record record;                           // tiled.json after
    usize cells() const { return walls.size(); }
    world::TileId at(u32 layer_index, i32 x, i32 y) const; // 0: walls, 1: blocks
};

struct Options {
    std::vector<Target> targets; // by index of map.layers; missing: default_target
    u32 walls = 0, blocks = 1;   // the game's layers
    u32 layer_count = 3;
    i32 liquids = 2;
    // Whether the game already has a template with this id (made by
    // something else): a new one then gets another.
    std::function<bool(const std::string&)> template_taken;
};

// Against the level as it is (its own tiles and zones, an earlier import's
// record). False and the reason, in words, when the map cannot be imported
// at all (not orthogonal, cells not square or of a size the level cannot
// have, too big).
bool plan(const Map& map, const Options& options, const LevelTiles& own, const LevelAreas& areas, const Record& before, Plan& out,
          std::string* refusal = nullptr);

} // namespace forge::level::tiled
