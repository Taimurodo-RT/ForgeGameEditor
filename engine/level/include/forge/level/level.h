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
// chunks, objects of visited chunks); a new game starts from a copy of that
// folder, and the generator fills in the rest.

#include "forge/core/math.h"
#include "forge/core/types.h"
#include "forge/render/camera.h"
#include "forge/scene/scene.h"
#include "forge/world/world.h"

#include <SDL3/SDL_gpu.h>

#include <filesystem>
#include <memory>
#include <span>
#include <string>
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

// What the world view shows besides the tiles.
struct ViewOptions {
    bool game_light = false; // the game's own light (dark caves, torches) instead of daylight
    bool grid = false;
    bool chunks = false; // chunk borders
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
};

class Level {
public:
    explicit Level(LevelModule& module);
    ~Level();
    Level(const Level&) = delete;
    Level& operator=(const Level&) = delete;

    // Opens (or starts) the level kept in folder. An empty folder is a level
    // nobody changed yet: everything comes from the generator.
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

    // Writes changed chunks and objects to the folder.
    struct SaveReport {
        bool ok = false;
        u32 tile_chunks = 0;
        u32 object_chunks = 0;
        f64 ms = 0;
    };
    SaveReport save();

    // Bumped by every edit made through set_tile (for "unsaved" marks).
    u64 edits() const { return edits_; }
    // Paints one cell; false when its chunk is not loaded.
    bool set_tile(u32 layer, i32 x, i32 y, world::TileId value);
    world::TileId tile(u32 layer, i32 x, i32 y) const { return world_->tile(layer, x, y); }
    bool loaded(i32 x, i32 y) const;

private:
    LevelModule& module_;
    std::filesystem::path folder_;
    std::unique_ptr<world::World> world_;
    std::unique_ptr<scene::Scene> scene_;
    std::vector<world::Rect> focus_;
    u64 edits_ = 0;
};

// Copies a level folder into a game's world folder (a new game starts from
// the level as the author left it). Missing source: nothing to copy.
bool copy_level(const std::filesystem::path& level, const std::filesystem::path& to, std::string* error = nullptr);

} // namespace forge::level
