#pragma once

// What the game and the level editor share of an RPG map seen from above (step 14.4a, 14.4-rpg-модель.md, «Уровни»):
// the blocks' components, the map as the author drew it (the layers «Земля» and «Препятствия», the hero, villagers,
// things to pick up, places), how a map is drawn, and the level module the editor paints with.

#include "rpg_rules.h"

#include "forge/data/reflect.h"
#include "forge/level/level.h"
#include "forge/objects/library.h"
#include "forge/render/camera.h"
#include "forge/render/sprite_batch.h"
#include "forge/render/sprite_renderer.h"

#include <SDL3/SDL_gpu.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rpg {

// «Герой»: how fast he walks.
struct Hero {
    f32 speed = 4;
};
// «Житель»: the dialogue he says (an id of «Сюжет»).
struct Villager {
    std::string talk;
};
// «Предмет»: what it gives to the inventory (inv.<what>) and how many.
struct Pickup {
    std::string what = "key";
    u32 count = 1;
};
// «Место»: a cell the hero comes to; drawn in the game when «Видно в игре» is on.
struct Place {
    bool shown = false;
};
void register_components(forge::scene::Scene& scene);

// The blocks' ids (games/modules/rpg/kinds.json). «pickup» and its «what» are the names forge::objects::Library::item_of
// knows.
inline constexpr std::string_view kHeroBlock = "hero";
inline constexpr std::string_view kVillagerBlock = "villager";
inline constexpr std::string_view kPickupBlock = "pickup";
inline constexpr std::string_view kPlaceBlock = "place";

// The layers and their tiles.
inline constexpr forge::u32 kGround = 0, kObstacles = 1;
inline constexpr forge::world::TileId kGrass = 1, kPath = 2, kSand = 3, kWater = 4;
inline constexpr forge::world::TileId kWall = 1, kTree = 2, kFence = 3, kRock = 4;
// A cell one walks on: ground that is not water, nothing on «Препятствия».
inline bool walkable(forge::world::TileId ground, forge::world::TileId obstacle) {
    return ground >= kGrass && ground <= kSand && obstacle == 0;
}

// A thing of a map, as the author put it.
struct Thing {
    enum class Kind : u8 { Hero, Villager, Pickup, Place };
    Kind kind = Kind::Pickup;
    const forge::objects::Template* tmpl = nullptr;
    // Its name in the saves: "<template id>@<cell x>,<cell y>", "#2"… after it for more of one template on one cell.
    std::string id;
    f64 x = 0, y = 0; // feet
    i32 cx = 0, cy = 0; // the cell
    f32 speed = 4;      // a hero's
    std::string talk;   // a villager's
    std::string what;   // a pickup's
    u32 count = 1;
    bool shown = true; // a place's «Видно в игре»
};

// A map as the author made it.
struct MapData {
    Map map;
    std::vector<forge::world::TileId> ground, obstacles; // by cell of map, row by row from its top left
    Thing hero;
    // In order from the top, then from the left, then by template id: the order is the file's, not the scene's.
    std::vector<Thing> villagers, pickups, places;
    forge::world::TileId ground_at(i32 x, i32 y) const;
    forge::world::TileId obstacle_at(i32 x, i32 y) const;
};
// Reads it from an open level (its objects loaded). False, why in the author's words («Карта «Деревня»: …»): no ground,
// no hero or more than one, the hero not on ground one walks on, a thing to pick up that gives nothing.
bool read_map(forge::level::Level& level, const forge::objects::Library& library, const std::string& name, MapData& out,
              std::string& why);

// The box of a thing's that the hero's feet touch to pick it up, and a place's cell.
Box pickup_box(const Thing& t);
Box place_box(const Thing& t);

// Draws maps: the tiles (pictures the code makes: placeholders, not art), the things by their templates' pictures (a
// thing with no picture is a figure of its kind's colour). The game and the editor draw with it alike.
class Painter {
public:
    bool init(SDL_GPUDevice* device, SDL_GPUTextureFormat format);
    void shutdown();
    bool ready() const { return ready_; }
    // The sheet made anew when the templates' pictures changed; outside any render pass.
    void update(const forge::objects::Library& library);

    u32 white() const { return 0; }
    u32 tile(forge::u32 layer, forge::world::TileId value) const;
    // A template's frame (its picture's, else its kind's figure), how many frames it has, its size in tiles (one wide).
    u32 frame(const forge::objects::Template& t, const forge::objects::Library& library, u32 n) const;
    u32 frames(const forge::objects::Template& t) const;
    f32 aspect(const forge::objects::Template& t) const; // height / width of a frame
    // The colours of the things without a picture, and of the tiles' middles (what a test looks for).
    static forge::u32 figure_colour(Thing::Kind kind);
    static forge::u32 tile_colour(forge::u32 layer, forge::world::TileId value);

    forge::render::SpriteBatch& batch() { return batch_; }
    // Each frame: begin(), push sprites, prepare() outside a render pass, draw() inside one.
    void begin(const forge::render::Camera2D& camera);
    void prepare(SDL_GPUCommandBuffer* cmd, const forge::render::Camera2D& camera, u32 width, u32 height);
    void draw(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass);
    // A sprite of a frame with its middle at a point of the map (cells), size in cells (negative w: mirrored).
    void put(u32 frame, f64 x, f64 y, f32 w, f32 h, u32 color = 0xffffffffu, u32 order = 1);
    // A thing standing with its feet at (x, y): one cell wide, as tall as its picture says.
    void put_thing(const forge::objects::Template& t, const forge::objects::Library& library, u32 frame, f64 x, f64 y,
                   bool mirrored, u32 order, u32 color = 0xffffffffu);
    // The tiles of a rectangle of cells, by what a function says is there.
    template <class Fn> void put_tiles(const forge::world::Rect& cells, Fn tile_at) {
        for (i32 y = cells.y0; y < cells.y1; ++y)
            for (i32 x = cells.x0; x < cells.x1; ++x) {
                const forge::world::TileId g = tile_at(kGround, x, y), o = tile_at(kObstacles, x, y);
                if (g) put(tile(kGround, g), x + 0.5, y + 0.5, 1, 1, 0xffffffffu, 0);
                if (o) put(tile(kObstacles, o), x + 0.5, y + 0.5, 1, 1, 0xffffffffu, 1);
            }
    }

private:
    void make_sheet(const forge::objects::Library& library);

    bool ready_ = false;
    forge::render::SpriteRenderer sprites_;
    forge::render::SpriteBatch batch_;
    std::vector<u8> rgba_;
    u32 width_ = 0, height_ = 0;
    std::vector<forge::render::SpriteRect> rects_;
    struct Pic {
        u32 first = 0, count = 1;
        f32 aspect = 1;
    };
    std::unordered_map<u64, Pic> pics_; // by template key
    u32 figures_ = 0;                   // the first of the kinds' figures (one per Thing::Kind)
    std::string signature_ = "-";
    bool sheet_changed_ = false;
};

// The state a thing's picture plays in (the tab «Анимация»): «Стоит», and the hero's «Идёт».
std::vector<forge::level::LevelModule::AnimState> anim_states_of(const forge::objects::Library& library,
                                                                 const forge::objects::Template& t);
// The animation of a state: the template's own when it can play, else the module's rule.
forge::objects::Clip clip_of(const forge::objects::Library& library, const forge::objects::Template& t, std::string_view state);

// The editor's view of an RPG map: the layers «Земля» and «Препятствия», nothing around, the things by their pictures,
// places as squares; objects are the game's templates.
class RpgLevel final : public forge::level::LevelModule {
public:
    RpgLevel();
    ~RpgLevel() override;

    // The game's objects: its kinds.json and objects/.
    bool load_objects(const std::filesystem::path& game_dir, std::string* error = nullptr);
    const forge::objects::Library& library_ref() const { return library_; }

    std::string title() const override { return "RPG сверху"; }
    forge::world::WorldDesc world_desc() const override;
    std::shared_ptr<const forge::world::Generator> generator() const override { return gen_; }
    void setup_scene(forge::scene::Scene& scene) override;
    const std::vector<std::string>& layer_names() const override { return layers_; }
    const std::vector<forge::level::TileDef>& tiles() const override { return tiles_; }
    void tile_icon(const forge::level::TileDef& tile, u32 size, std::vector<u8>& rgba) const override;
    // The hero as last drawn, else the middle of the map, else the place a new map is drawn at.
    void start(f64& x, f64& y) const override;
    u32 map_color(u32 layer, forge::world::TileId value) const override;
    forge::Color background() const override;

    bool init_view(SDL_GPUDevice* device, SDL_GPUTextureFormat format) override;
    void shutdown_view() override;
    void prepare_view(SDL_GPUCommandBuffer* cmd, forge::level::Level& level, const forge::render::Camera2D& camera,
                      u32 width, u32 height, const forge::level::ViewOptions& options, f64 time) override;
    void draw_view(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass) override;

    forge::objects::Library* library() override { return &library_; }
    u64 objects_version() const override { return library_.version(); }
    const std::vector<forge::level::ObjectDef>& objects() const override;
    void object_icon(const forge::level::ObjectDef& def, u32 size, std::vector<u8>& rgba) const override;
    void hero_icon(u32 size, std::vector<u8>& rgba) const override;
    // In the middle of the cell under the point, its feet on the cell's bottom.
    flecs::entity place_object(forge::level::Level& level, usize index, f64 x, f64 y) override;
    i32 object_kind(flecs::entity e) const override;
    bool object_box(flecs::entity e, f64& x0, f64& y0, f64& x1, f64& y1) const override;
    std::vector<AnimState> anim_states(const forge::objects::Library& library, const forge::objects::Template& t) const override;

    // What a map says now (for the tests): "" when it can be played.
    std::string map_problem(forge::level::Level& level, const std::string& name) const;

private:
    forge::objects::Library library_;
    std::shared_ptr<const forge::world::Generator> gen_;
    std::vector<std::string> layers_ = {"Земля", "Препятствия"};
    std::vector<forge::level::TileDef> tiles_;
    mutable std::vector<forge::level::ObjectDef> defs_;
    mutable u64 defs_version_ = ~0ull;
    mutable f64 start_x_ = 4, start_y_ = 4;
    Painter painter_;
};

} // namespace rpg

FORGE_REFLECT_DECLARE(rpg::Hero)
FORGE_REFLECT_DECLARE(rpg::Villager)
FORGE_REFLECT_DECLARE(rpg::Pickup)
FORGE_REFLECT_DECLARE(rpg::Place)
