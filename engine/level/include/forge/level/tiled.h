#pragma once

// Maps of Tiled (https://www.mapeditor.org), read as Tiled writes them: a .tmx
// with its tilesets (in the map or in .tsx files next to it) and object
// templates (.tx). Reading changes nothing; what becomes of the map in a
// level is tiled_import.h's business.
//
// Read: every orientation and attribute the TMX format has (so the import can
// say what it does not carry over), tile layer data in CSV, base64 (plain,
// zlib, gzip) and the old <tile> elements, the chunks of infinite maps (also
// at negative coordinates), group layers (their visibility, opacity and
// offsets passed down to the layers in them), objects of every shape and
// template, properties. Not read: zstd data and the JSON formats of maps and
// tilesets (.tmj, .tsj) — such a map is refused, with the reason; a template
// in JSON (.tj) is told like a missing one.
//
// The rules are Tiled's own (https://doc.mapeditor.org/en/stable/reference/
// tmx-map-format/ and .../global-tile-ids/): a gid of 0 is an empty cell; the
// four high bits are flags (horizontal, vertical, diagonal flip, 120° turn on
// hexagonal maps) and come off before the tileset is looked up; the tileset is
// the one with the largest firstgid not above the gid, and the tile's id in it
// is gid - firstgid. A missing .tsx or .tx does not stop the map being read:
// the tileset says it is missing, and so does the import.

#include "forge/core/types.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace forge::level::tiled {

inline constexpr u32 kFlipH = 0x80000000u;    // mirrored left to right
inline constexpr u32 kFlipV = 0x40000000u;    // mirrored top to bottom
inline constexpr u32 kFlipD = 0x20000000u;    // mirrored across the diagonal from the top left (done first)
inline constexpr u32 kRotate120 = 0x10000000u; // hexagonal maps only
inline constexpr u32 kGidFlags = 0xF0000000u;

struct Property {
    std::string name;
    std::string type = "string"; // string, int, float, bool, color, file, object, class
    std::string value;           // as written ("true"/"false" for bool; a class's members are not kept)
    friend bool operator==(const Property&, const Property&) = default;
};
const Property* find_property(const std::vector<Property>& props, std::string_view name);
// A bool property set to true.
bool is_true(const std::vector<Property>& props, std::string_view name);

// A tile of a tileset that has something of its own.
struct Tile {
    u32 id = 0;
    std::string type;            // its class
    std::filesystem::path image; // in a collection of images: its picture
    std::string image_source;    // as written
    u32 image_w = 0, image_h = 0;
    i32 x = 0, y = 0;            // the part of its picture it shows (Tiled 1.9); w = 0: all of it
    u32 w = 0, h = 0;
    bool collision = false;      // has collision shapes
    bool animated = false;
    u32 first_frame = 0;         // when animated: the tile its first frame shows
    std::vector<Property> props;
};

struct Tileset {
    u32 firstgid = 0;
    std::string source;         // the .tsx as written in the map ("" when the tileset is in the map)
    std::filesystem::path file; // that .tsx
    std::string missing;        // why the .tsx could not be read ("" when it was, or there is none)
    std::string name;
    u32 tile_w = 0, tile_h = 0, spacing = 0, margin = 0, count = 0, columns = 0;
    std::filesystem::path image; // the picture of all its tiles; empty for a collection of images
    std::string image_source;
    u32 image_w = 0, image_h = 0;
    bool trans = false;          // this colour is see-through in the picture
    u8 trans_rgb[3] = {};
    i32 offset_x = 0, offset_y = 0;                 // <tileoffset>
    std::string render_size = "tile", fill_mode = "stretch", alignment = "unspecified";
    std::vector<Tile> tiles;                        // by id
    std::vector<Property> props;
    bool collection() const { return image_source.empty(); }
    const Tile* tile(u32 id) const;
    // Whether the tileset has a tile with this id.
    bool has(u32 id) const;
};

// A rectangle of cells: gids with their flags, row by row.
struct Chunk {
    i32 x = 0, y = 0; // in cells
    u32 w = 0, h = 0;
    std::vector<u32> gids;
};

struct Object {
    enum class Shape : u8 { Rect, Point, Ellipse, Polygon, Polyline, Text, Capsule };
    u32 id = 0;
    std::string name, type;
    Shape shape = Shape::Rect;
    f64 x = 0, y = 0, w = 0, h = 0, rotation = 0; // pixels; degrees clockwise
    u32 gid = 0;                                  // a tile object, with flags
    bool visible = true;
    std::string template_source;                  // .tx as written
    std::string template_missing;                 // why it could not be read
    std::vector<Property> props;
};

struct Layer {
    enum class Kind : u8 { Tiles, Objects, Image };
    Kind kind = Kind::Tiles;
    u32 id = 0;
    std::string name;     // with the groups it is in: "Группа/Слой"
    std::string type;     // its class
    bool visible = true;  // and its groups
    f64 opacity = 1;      // times its groups'
    f64 offset_x = 0, offset_y = 0; // pixels, plus its groups'
    f64 parallax_x = 1, parallax_y = 1;
    std::string tint;     // "#aarrggbb" or ""; its own or a group's
    std::string mode;     // blend mode; "" normal
    std::vector<Chunk> chunks;   // a tile layer's cells (one chunk on a finite map)
    std::vector<Object> objects; // an object layer's
    std::string image_source;    // an image layer's
    std::vector<Property> props;
};

struct Map {
    std::filesystem::path file;
    std::string version, tiled_version;
    std::string orientation = "orthogonal", render_order = "right-down";
    u32 w = 0, h = 0, tile_w = 0, tile_h = 0;
    bool infinite = false;
    std::string background; // "#rrggbb" or ""
    std::vector<Tileset> tilesets; // by firstgid
    std::vector<Layer> layers;     // in drawing order (bottom first), group layers flattened
    std::vector<Property> props;
};

// Reads a map and what it uses; false and why (in words) when the map itself
// cannot be read.
bool read_map(const std::filesystem::path& tmx, Map& out, std::string* error = nullptr);

// The tileset of a gid and the tile's id in it; nullptr for an empty cell or a
// gid below every firstgid.
const Tileset* tileset_of(const Map& map, u32 gid, u32& local);

} // namespace forge::level::tiled
