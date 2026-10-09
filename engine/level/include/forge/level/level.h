#pragma once

// A level as the level editor sees it: a streamed tile world and the objects
// placed in it, kept in a folder of region files.
//
// What a level is made of comes from the game, through a LevelModule: the
// generator that makes everything nobody changed, the tiles and objects the
// palette offers, and how the world is drawn. The editor only paints and
// places through it, so one editor serves every game.
//
// Changes are kept as region files in the level folder (tiles of changed
// chunks, objects of visited chunks), physics.json (the world's gravity),
// light.json (the time of day), areas.json (named areas, the spawn point),
// tiles.json with tiles.png (the level's own tiles) and world.json (what is
// around: the game's world or nothing; forge/level/own_tiles.h); a new game
// starts from a copy of that folder, and the generator fills in the rest.

#include "forge/core/math.h"
#include "forge/core/types.h"
#include "forge/level/own_tiles.h"
#include "forge/objects/library.h"
#include "forge/render/camera.h"
#include "forge/render/lighting.h"
#include "forge/scene/scene.h"
#include "forge/sim/cells.h"
#include "forge/sim/tiles.h"
#include "forge/world/world.h"

#include <SDL3/SDL_gpu.h>

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace forge::level {

// A tile the palette offers: a value to paint on one layer.
struct TileDef {
    std::string id;    // "stone": stable, for files and tests
    std::string name;  // "Камень"
    std::string group; // "Земля и камень"
    std::string hint;  // "копается киркой"
    u32 layer = 0;
    world::TileId value = 0;
    std::string key; // shortcut shown on the palette ("1"), may be empty
};

// An object the palette offers: a villager, an item, a crate.
struct ObjectDef {
    std::string id;    // "miner"
    std::string name;  // "Шахтёр Борис"
    std::string group; // "Жители"
    std::string hint;  // "даёт задание про кирку"
    u64 key = 0;       // its template's (objects::Template), 0: none
    // A template that is not (yet) the game's, such as one of the shared
    // library: drawn as the game would draw it. Null: the game's, by key.
    const objects::Template* tmpl = nullptr;
};

// The level's own id of an object, kept in the saves: entities are made
// anew whenever their chunk loads, so editors find objects by this.
struct LevelId {
    u64 id = 0;
};

// The level's physics that is not tiles or objects: the world's pull, in
// tiles / s² (side view: 0, 40). Kept in physics.json in the level folder;
// without it a level has its game's (LevelModule::default_physics). Gravity
// points are objects of the level (sim::GravitySource), water and sand its
// tiles.
struct LevelPhysics {
    f32 gravity_x = 0, gravity_y = 0;
    friend bool operator==(const LevelPhysics&, const LevelPhysics&) = default;
};
// The strongest pull a level may have, each way.
inline constexpr f32 kMaxGravity = 200;
// Finite and within ±kMaxGravity.
bool valid_physics(const LevelPhysics& p);
// Reads folder/physics.json over out (fields it lacks keep out's values).
// No file: true, out unchanged, *found false. A file that cannot be used
// (not JSON, a field of the wrong type, a value out of range): false, out
// unchanged, *error in the author's words.
bool load_physics(const std::filesystem::path& folder, LevelPhysics& out, bool* found = nullptr,
                  std::string* error = nullptr);
bool save_physics(const std::filesystem::path& folder, const LevelPhysics& p, std::string* error = nullptr);

// The level's light that is not its light sources: the time of day, in hours
// (0 ≤ time < 24), which sets the sky's light. Kept in light.json in the level
// folder; without it noon (12:00), the light the game always had. Light
// sources are objects of the level (LightSource).
struct LevelLight {
    f32 time = 12;
    friend bool operator==(const LevelLight&, const LevelLight&) = default;
};
// Finite and within 0 ≤ time < 24.
bool valid_light(const LevelLight& l);
// As load_physics, for folder/light.json.
bool load_light(const std::filesystem::path& folder, LevelLight& out, bool* found = nullptr, std::string* error = nullptr);
bool save_light(const std::filesystem::path& folder, const LevelLight& l, std::string* error = nullptr);

// A light source placed in a level: an object of it, its scene::Position the
// centre. Its colour, how bright it is at the centre and how far its light
// goes are three settings that do not change each other: the light fades
// smoothly to nothing at the radius (render::lamp_light), however bright.
struct LightSource {
    f32 r = 1, g = 0.82f, b = 0.55f; // colour, 0..1 each
    f32 brightness = 1.5f;           // 0..kMaxBrightness; 0 gives no light
    f32 radius = 8;                  // tiles, kMinLightRadius..kMaxLightRadius
};
inline constexpr f32 kMaxBrightness = 4;
inline constexpr f32 kMinLightRadius = 1;
inline constexpr f32 kMaxLightRadius = render::kMaxLampRadius;
// The lamp a source at (x, y) gives: its colour times its brightness, its
// radius. Values a file got wrong are kept within the limits; a number that
// is not finite gives no light.
render::PointLight point_light(f64 x, f64 y, const LightSource& s);

// The «Зоны» mode's data: named rectangles of the level (areas) and where a
// new game puts its hero (the spawn point), kept in areas.json in the level
// folder, not in its chunks (forge/level/areas.h has the rest). In code they
// are areas: sim::Zones are how awake the simulation is.
//
//   {"areas": [{"id": "9f2c41d07a5be318", "name": "Шахта", "x0": 10, "y0": 20, "x1": 40, "y1": 35,
//               "music": "шахта.wav"}],
//    "spawn": {"x": 12.5, "y": 34}}
//
// An area has an id of its own (random, as LevelId): links name it by that
// ("area:" and the id), so renaming it keeps them. Its rectangle is in whole
// tiles, [x0, x1) × [y0, y1): a point on its left or top edge is in it, on
// its right or bottom edge is not, and chunk borders mean nothing to it. Its
// music is a file of the game's sounds. Without areas.json a level has no
// areas and no spawn point: a new game starts where the game always did.
struct Area {
    u64 id = 0;
    std::string name;
    i32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::string music; // a file of the game's sounds folder; "" none
    bool contains(f64 x, f64 y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
    i64 size() const { return static_cast<i64>(x1 - x0) * static_cast<i64>(y1 - y0); }
    friend bool operator==(const Area&, const Area&) = default;
};

struct LevelAreas {
    std::vector<Area> areas; // in the order drawn
    bool spawn = false;      // the level has a spawn point
    f64 spawn_x = 0, spawn_y = 0; // the point under the hero's feet, in tiles
    const Area* find(u64 id) const;
    Area* find(u64 id);
    friend bool operator==(const LevelAreas&, const LevelAreas&) = default;
};

inline constexpr usize kMaxAreas = 1024;
inline constexpr usize kMaxAreaName = 60;  // characters
inline constexpr i32 kMaxAreaSide = 4096;  // tiles
inline constexpr f64 kMaxSpawn = 1e7;      // tiles from the origin, each way

// "9f2c41d07a5be318": 16 hex digits, as areas.json and links keep it.
std::string area_id_text(u64 id);
// 1..16 hex digits, not 0.
bool parse_area_id(std::string_view text, u64& id);
// A name as typed: spaces around it go; "" when nothing is left.
std::string clean_area_name(std::string_view text);
// A plain file name ("шахта.wav"): no folders, not "." or "..".
bool valid_music_name(std::string_view name);
// What is wrong with an area, in the author's words; "" when nothing.
std::string area_problem(const Area& a);

// Reads folder/areas.json. No file: true, out unchanged, *found false. A file
// that cannot be used (not JSON, a field of the wrong kind, an area with no
// id, the same id twice, a rectangle that is empty or too big, a spawn point
// that is not a number): false, out unchanged, *error in the author's words.
bool load_areas(const std::filesystem::path& folder, LevelAreas& out, bool* found = nullptr, std::string* error = nullptr);
// Writes folder/areas.json (refuses areas with problems).
bool save_areas(const std::filesystem::path& folder, const LevelAreas& a, std::string* error = nullptr);
std::string areas_json(const LevelAreas& a);
inline constexpr const char* kAreasFile = "areas.json";
// Where an areas.json that could not be read is kept when the level writes a
// new one over it.
inline constexpr const char* kBrokenAreasFile = "areas.broken.json";

// What the world view shows besides the tiles.
struct ViewOptions {
    bool game_light = false; // the game's own light (dark caves, torches) instead of daylight
    bool grid = false;
    bool chunks = false; // chunk borders
    // An hour to look at the level in instead of its own (LevelLight::time),
    // only in the view: negative: the level's.
    f32 preview_time = -1;
};

class Level;

class LevelModule {
public:
    virtual ~LevelModule() = default;

    virtual std::string title() const = 0; // "Старая шахта"
    virtual world::WorldDesc world_desc() const = 0;
    virtual std::shared_ptr<const world::Generator> generator() const = 0;
    // Registers the saved components and the populator.
    virtual void setup_scene(scene::Scene& scene) = 0;

    virtual const std::vector<std::string>& layer_names() const = 0; // "Стены", "Блоки", "Жидкости"
    // The layer that holds liquids (no tile of the level's own goes there);
    // -1: none.
    virtual i32 liquids_layer() const { return -1; }
    virtual const std::vector<TileDef>& tiles() const = 0;
    // A palette picture of a tile: size × size RGBA pixels.
    virtual void tile_icon(const TileDef& tile, u32 size, std::vector<u8>& rgba) const = 0;
    // Where the editor looks first (the start of a new game).
    virtual void start(f64& x, f64& y) const = 0;
    // A place name for the status line ("Деревня"), may be empty.
    virtual std::string place(f64 x, f64 y) const { (void)x; (void)y; return {}; }
    // The colour of a tile on the minimap, RGBA8 as pack_color (red in the
    // low byte); 0: nothing there, the layer below shows.
    virtual u32 map_color(u32 layer, world::TileId value) const { (void)layer; (void)value; return 0; }
    // What is behind every layer (the sky).
    virtual Color background() const { return {0.47f, 0.68f, 0.90f, 1.0f}; }
    // What the level's hour (LevelLight::time) means in the game, for «Свет»:
    // the game's own words, its night among them.
    virtual std::string hour_words(f64 hour) const { (void)hour; return {}; }
    // Where a game started near (x, y) puts its player (the point under its
    // feet); false when there is no room near. For «Играть отсюда».
    virtual bool play_spot(Level& level, f64 x, f64 y, f64& out_x, f64& out_y) const {
        (void)level;
        out_x = x;
        out_y = y;
        return true;
    }

    // Drawing the level into a part of the target. prepare() runs outside
    // any render pass, draw() inside one whose viewport is that part.
    virtual bool init_view(SDL_GPUDevice* device, SDL_GPUTextureFormat format) = 0;
    virtual void shutdown_view() = 0;
    virtual void prepare_view(SDL_GPUCommandBuffer* cmd, Level& level, const render::Camera2D& camera, u32 width,
                              u32 height, const ViewOptions& options, f64 time) = 0;
    virtual void draw_view(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass) = 0;
    // The level's world and entities are about to go: drop anything bound
    // to them (renderers, queries).
    virtual void level_closing(Level& level) { (void)level; }

    // --- objects ---
    // The game's object library (kinds and templates), if it has one; the
    // palette then offers its templates.
    virtual objects::Library* library() { return nullptr; }
    // Changes whenever objects() may have changed (a template was added,
    // renamed or deleted): palettes rebuild.
    virtual u64 objects_version() const { return 0; }
    virtual const std::vector<ObjectDef>& objects() const {
        static const std::vector<ObjectDef> none;
        return none;
    }
    // A palette picture of an object: size × size RGBA pixels.
    virtual void object_icon(const ObjectDef& def, u32 size, std::vector<u8>& rgba) const {
        (void)def;
        rgba.assign(static_cast<usize>(size) * size * 4, 0);
    }
    // The hero's picture (for the links board): size × size RGBA pixels.
    virtual void hero_icon(u32 size, std::vector<u8>& rgba) const { rgba.assign(static_cast<usize>(size) * size * 4, 0); }
    // Makes objects()[index] standing on the point (x, y) (its feet there);
    // an empty entity when that place is not loaded.
    virtual flecs::entity place_object(Level& level, usize index, f64 x, f64 y) {
        (void)level, (void)index, (void)x, (void)y;
        return {};
    }
    // Which of objects() an entity is; -1: not something the editor shows.
    virtual i32 object_kind(flecs::entity e) const { (void)e; return -1; }
    // The rectangle an object covers, in tiles.
    virtual bool object_box(flecs::entity e, f64& x0, f64& y0, f64& x1, f64& y1) const {
        (void)e, (void)x0, (void)y0, (void)x1, (void)y1;
        return false;
    }
    // After the editor moved an object (a villager's home goes with him).
    virtual void object_moved(flecs::entity e) { (void)e; }
    // Whether the properties panel shows this saved component's fields.
    virtual bool object_component_shown(const reflect::TypeInfo* type) const { (void)type; return true; }

    // --- physics ---
    // The game's own pull: what a level without physics.json has.
    virtual LevelPhysics default_physics() const { return {}; }
    // Palette tiles (TileDef::id) the «Физика» mode pours into free cells:
    // "water", "sand". Empty: the mode has no areas.
    virtual std::vector<std::string> physics_fills() const { return {}; }
    // The game's liquids and falling tiles, and its solid tiles in rules, as
    // its simulation has them: for the flow trial in the editor. Null: none.
    virtual std::unique_ptr<sim::CellSim> make_cells(sim::CollisionRules& rules) const {
        (void)rules;
        return nullptr;
    }
};

class Level {
public:
    explicit Level(LevelModule& module);
    ~Level();
    Level(const Level&) = delete;
    Level& operator=(const Level&) = delete;

    // Opens (or starts) the level kept in folder. An empty folder is a level
    // nobody changed yet: everything comes from the generator. A level with
    // nothing around (world.json) has the empty world instead, and its
    // chunks are not peopled when first visited.
    bool open(const std::filesystem::path& folder, std::string* error = nullptr);

    LevelModule& module() { return module_; }
    world::World& world() { return *world_; }
    scene::Scene& scene() { return *scene_; }
    const std::filesystem::path& folder() const { return folder_; }

    // Once per frame: keeps these areas (in tiles: the view) loaded.
    void update(std::span<const world::Rect> focus);
    // Loads an area now (with the current focus kept), for edits far from
    // the view: undo of a stroke made elsewhere.
    void ensure_loaded(const world::Rect& tiles);

    // Writes changed chunks and objects to the folder, physics.json when the
    // physics changed, light.json when the time of day did, areas.json when
    // the areas did and tiles.json with tiles.png when the own tiles did.
    struct SaveReport {
        bool ok = false;
        u32 tile_chunks = 0;
        u32 object_chunks = 0;
        bool physics = false; // physics.json written
        bool light = false;   // light.json written
        bool areas = false;   // areas.json written
        bool own_tiles = false; // tiles.json and tiles.png written
        f64 ms = 0;
        std::string error; // when not ok, in the author's words
    };
    SaveReport save();

    // The world's pull (physics.json, else the game's).
    const LevelPhysics& physics() const { return physics_; }
    void set_physics(const LevelPhysics& p);
    // Not saved yet.
    bool physics_changed() const { return physics_ != physics_saved_; }
    // Why physics.json could not be read when the level opened (the game's
    // pull is used then); empty when it was fine or not there.
    const std::string& physics_error() const { return physics_error_; }

    // The time of day (light.json, else noon).
    const LevelLight& light() const { return light_; }
    void set_light(const LevelLight& l);
    bool light_changed() const { return light_ != light_saved_; }
    // Why light.json could not be read (noon is used then).
    const std::string& light_error() const { return light_error_; }

    // The areas and the spawn point (areas.json, else none).
    const LevelAreas& areas() const { return areas_; }
    void set_areas(const LevelAreas& a);
    bool areas_changed() const { return areas_ != areas_saved_; }
    // Why areas.json could not be read (no areas then; the file stays as it
    // is, and is kept as areas.broken.json when the areas are saved anew).
    const std::string& areas_error() const { return areas_error_; }
    // Bumped by every change of the areas (for panels that show them).
    u64 areas_version() const { return areas_version_; }

    // The level's own tiles (tiles.json and tiles.png, else none).
    const LevelTiles& own_tiles() const { return own_tiles_; }
    // false (nothing changes) when they have a problem (tiles_problem).
    bool set_own_tiles(const LevelTiles& t);
    bool own_tiles_changed() const { return own_tiles_ != own_tiles_saved_; }
    // Why tiles.json could not be read (no own tiles then: their cells show
    // nothing; the files stay as they are, and go aside as tiles.broken.json
    // and tiles.broken.png when the own tiles are saved anew).
    const std::string& own_tiles_error() const { return own_tiles_error_; }
    // Bumped by every change of the own tiles (the palette, the atlas).
    u64 own_tiles_version() const { return own_tiles_version_; }

    // What is around the level's chunks (world.json, else the game's world).
    // It is what the world was made with, so it changes only by writing
    // world.json and opening the level again.
    const LevelWorld& around() const { return around_; }
    // Why world.json could not be read (the game's world is around then).
    const std::string& around_error() const { return around_error_; }

    // Bumped by every edit made through set_tile (for "unsaved" marks).
    u64 edits() const { return edits_; }
    // Paints one cell; false when its chunk is not loaded.
    bool set_tile(u32 layer, i32 x, i32 y, world::TileId value);
    world::TileId tile(u32 layer, i32 x, i32 y) const { return world_->tile(layer, x, y); }
    bool loaded(i32 x, i32 y) const;

    // --- objects ---
    // The object with this LevelId (empty when its chunk is not loaded).
    flecs::entity find(u64 id);
    // An object's LevelId; assign: give it one when it has none (0 otherwise).
    u64 id_of(flecs::entity e, bool assign);
    static u64 new_id();
    // The editable object under a point (the smallest one there), or empty.
    flecs::entity pick(f64 x, f64 y);
    // Editable objects whose boxes touch the rectangle.
    void objects_in(f64 x0, f64 y0, f64 x1, f64 y1, std::vector<flecs::entity>& out);
    // Bumped by every object edit (place, move, change, delete).
    void touch_objects() { ++object_edits_; ++edits_; }
    u64 object_edits() const { return object_edits_; }

private:
    LevelModule& module_;
    std::filesystem::path folder_;
    std::unique_ptr<world::World> world_;
    std::unique_ptr<scene::Scene> scene_;
    std::vector<world::Rect> focus_;
    flecs::query<LevelId> ids_;
    u64 edits_ = 0;
    u64 object_edits_ = 0;
    LevelPhysics physics_, physics_saved_;
    std::string physics_error_;
    LevelLight light_, light_saved_;
    std::string light_error_;
    LevelAreas areas_, areas_saved_;
    std::string areas_error_;
    u64 areas_version_ = 0;
    LevelTiles own_tiles_, own_tiles_saved_;
    std::string own_tiles_error_;
    u64 own_tiles_version_ = 0;
    LevelWorld around_;
    std::string around_error_;
};

// Copies a level folder into a game's world folder (a new game starts from
// the level as the author left it). Missing source: nothing to copy.
bool copy_level(const std::filesystem::path& level, const std::filesystem::path& to, std::string* error = nullptr);

} // namespace forge::level

FORGE_REFLECT_DECLARE(forge::level::LevelId)
FORGE_REFLECT_DECLARE(forge::level::LevelPhysics)
FORGE_REFLECT_DECLARE(forge::level::LevelLight)
FORGE_REFLECT_DECLARE(forge::level::LightSource)
