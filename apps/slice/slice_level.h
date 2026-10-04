#pragma once

// What the game and the level editor share: the saved components, the
// world's shape, the first-visit populator, how objects and torches are
// drawn, and the level module the editor uses to paint «Старая шахта».

#include "demo_art.h"
#include "slice_world.h"

#include "forge/level/level.h"
#include "forge/render/lighting.h"
#include "forge/render/sprite_batch.h"
#include "forge/render/sprite_renderer.h"
#include "forge/render/tilemap_renderer.h"
#include "forge/scene/scene.h"
#include "forge/sim/bodies.h"
#include "forge/sim/rigid.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace slice {

// Saved with their chunk.
struct Hero {
    f32 facing = 1;
};
// who: 0 the miner, 1 the smith.
struct Npc {
    u8 who = 0;
    f32 home_x = 0;
    f32 dir = 0;    // walking: -1, 1; standing: 0
    f32 facing = 1;
    f32 timer = 0;
    u32 seed = 0;
};
enum class ItemKind : u8 { Pickaxe, Coins, Copper, Wood, Torch };
struct Item {
    u8 kind = 0;
    u16 count = 1;
};
struct Critter {
    f32 speed = 2;
    f32 dir = 1;
    u32 seed = 0;
};

// The hero's size, for spawning people.
inline constexpr f32 kHeroHalfW = 0.38f, kHeroHalfH = 0.92f;
// The light of a torch.
inline constexpr forge::Color kTorchLight{1.5f, 1.05f, 0.55f, 1.0f};

forge::world::WorldDesc world_desc();
void register_components(forge::scene::Scene& scene);
// First visit of a chunk: the villagers, the lost pickaxe, crates, critters.
void populate(const SliceGenerator& gen, forge::world::ChunkCoord coord, forge::scene::Scene& scene);
u32 item_frame(ItemKind kind);
u32 hash32(u32 a, u32 b);

// Queries of the drawn objects, made once per ECS world.
struct Objects {
    flecs::query<forge::scene::Position, forge::sim::Body, Npc> npcs;
    flecs::query<forge::scene::Position, forge::sim::Body, Critter> critters;
    flecs::query<forge::scene::Position, forge::sim::Body, Item> items;
    flecs::query<forge::scene::Position, forge::sim::RigidBody> crates;
    void init(flecs::world& ecs);
};

// Sprites of the villagers, critters, items, crates and the anvil, around
// the camera (cam_x, cam_y). alpha: how far between ticks (1 when nothing
// moves); tick: the animation clock.
void push_objects(forge::render::SpriteBatch& batch, Objects& objects, const SliceGenerator& gen, f64 cam_x,
                  f64 cam_y, f32 alpha, u64 tick);
// Flames over the torches in view; their places go to torches (for lights).
void push_torches(forge::render::SpriteBatch& batch, const forge::world::World& world, const forge::world::Rect& view,
                  f64 cam_x, f64 cam_y, u64 tick, std::vector<std::pair<f64, f64>>& torches);
// Binds the tile renderer to a world, with the water and wall looks.
bool init_tiles(forge::render::TilemapRenderer& tiles, SDL_GPUDevice* device, SDL_GPUTextureFormat format,
                forge::world::World& world, const std::vector<u8>& atlas);

// «Старая шахта» for the level editor.
class SliceLevel final : public forge::level::LevelModule {
public:
    SliceLevel();
    ~SliceLevel() override;

    std::string title() const override { return "Старая шахта"; }
    forge::world::WorldDesc world_desc() const override;
    std::shared_ptr<const forge::world::Generator> generator() const override { return gen_; }
    void setup_scene(forge::scene::Scene& scene) override;
    const std::vector<std::string>& layer_names() const override { return layers_; }
    const std::vector<forge::level::TileDef>& tiles() const override { return tiles_; }
    void tile_icon(const forge::level::TileDef& tile, u32 size, std::vector<u8>& rgba) const override;
    void start(f64& x, f64& y) const override;
    std::string place(f64 x, f64 y) const override { return gen_->location(x, y); }
    u32 map_color(u32 layer, TileId value) const override;
    forge::Color background() const override { return {0.47f, 0.68f, 0.90f, 1.0f}; }
    bool play_spot(forge::level::Level& level, f64 x, f64 y, f64& out_x, f64& out_y) const override;

    bool init_view(SDL_GPUDevice* device, SDL_GPUTextureFormat format) override;
    void shutdown_view() override;
    void prepare_view(SDL_GPUCommandBuffer* cmd, forge::level::Level& level, const forge::render::Camera2D& camera,
                      u32 width, u32 height, const forge::level::ViewOptions& options, f64 time) override;
    void draw_view(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass) override;
    void level_closing(forge::level::Level& level) override;

    const SliceGenerator& slice_generator() const { return *gen_; }

private:
    std::shared_ptr<SliceGenerator> gen_;
    std::vector<std::string> layers_;
    std::vector<forge::level::TileDef> tiles_;
    std::vector<u8> atlas_;
    std::vector<u32> map_colors_; // per tile id: the atlas cell's average

    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUTextureFormat format_{};
    forge::demo::SheetImage sheet_;
    forge::render::TilemapRenderer tilemap_;
    const forge::world::World* tilemap_world_ = nullptr;
    forge::render::SpriteRenderer sprites_;
    forge::render::SpriteBatch batch_;
    forge::render::LightRenderer lights_;
    flecs::world* objects_ecs_ = nullptr;
    Objects objects_;
    std::vector<std::pair<f64, f64>> torches_;
    bool game_light_ = false;
    bool view_ready_ = false;
};

} // namespace slice

FORGE_REFLECT_DECLARE(slice::Hero)
FORGE_REFLECT_DECLARE(slice::Npc)
FORGE_REFLECT_DECLARE(slice::Item)
FORGE_REFLECT_DECLARE(slice::Critter)
