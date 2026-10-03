#pragma once

// Draws a streamed tile world. Each visible chunk's tiles live in one big GPU
// buffer ("slots"); every frame the renderer uploads only chunks that are new
// or changed and draws all visible chunks of a layer with a single instanced
// draw call. The fragment shader looks each tile up in the atlas, so the cost
// depends on screen pixels, not on how many tiles are visible.

#include "forge/core/math.h"
#include "forge/core/types.h"
#include "forge/world/world.h"

#include <SDL3/SDL_gpu.h>

#include <unordered_map>
#include <vector>

namespace forge::render {

// What the player sees: centre of the screen in tiles and pixels per tile.
struct Camera2D {
    f64 x = 0;
    f64 y = 0;
    f32 zoom = 16.0f;

    // Tiles covering a screen of width × height pixels.
    world::Rect visible_tiles(u32 width, u32 height) const;
    void screen_to_tile(f32 sx, f32 sy, u32 width, u32 height, f64& tx, f64& ty) const;
};

// Square texture of cells_per_row × cells_per_row tiles, each cell_px pixels,
// RGBA8. Tile id N is cell N, row by row.
struct TileAtlas {
    const u8* rgba = nullptr;
    u32 cell_px = 16;
    u32 cells_per_row = 16;
};

struct TilemapStats {
    u32 visible_chunks = 0; // ready chunks on screen
    u32 drawn_chunks = 0;   // of those, already on the GPU
    u32 uploads = 0;        // chunks sent to the GPU this frame
    u32 slots_used = 0;
    u32 slots_total = 0;
};

class TilemapRenderer final : public world::WorldListener {
public:
    TilemapRenderer() = default;
    ~TilemapRenderer() override;
    TilemapRenderer(const TilemapRenderer&) = delete;
    TilemapRenderer& operator=(const TilemapRenderer&) = delete;

    // max_slots bounds how many chunks can be on screen at once (and the GPU
    // memory: max_slots × layers × 8 KiB).
    bool init(SDL_GPUDevice* device, SDL_GPUTextureFormat target_format, world::World& world, const TileAtlas& atlas,
              u32 max_slots = 4096);
    void shutdown();

    // Each frame, outside any render pass: uploads changed chunks and the list
    // of chunks to draw.
    void prepare(SDL_GPUCommandBuffer* cmd, const Camera2D& camera, u32 width, u32 height);
    // Inside a render pass on the target: draws every layer, back to front.
    void draw(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass);

    void set_layer_tint(u32 layer, Color tint);
    const TilemapStats& stats() const { return stats_; }

    void on_chunk_unloading(world::Chunk& chunk) override;

private:
    struct Slot {
        u32 index = 0;
        u32 revision = 0;
        u64 last_seen = 0; // frame it was last on screen
        bool uploaded = false;
    };
    struct Instance {
        f32 x, y;
        u32 slot;
    };

    bool acquire_slot(world::ChunkCoord coord, Slot*& out);
    bool create_atlas(SDL_GPUCommandBuffer* cmd, const TileAtlas& atlas);

    SDL_GPUDevice* device_ = nullptr;
    world::World* world_ = nullptr;
    SDL_GPUGraphicsPipeline* pipeline_ = nullptr;
    SDL_GPUBuffer* tiles_ = nullptr;
    SDL_GPUBuffer* instances_ = nullptr;
    SDL_GPUTransferBuffer* transfer_ = nullptr;
    SDL_GPUTexture* atlas_ = nullptr;
    SDL_GPUSampler* sampler_ = nullptr;

    u32 max_slots_ = 0;
    u32 layer_count_ = 0;
    u32 slot_bytes_ = 0;
    u32 atlas_cells_ = 16;
    u32 atlas_cell_px_ = 16;
    u32 atlas_levels_ = 1;
    usize transfer_bytes_ = 0;

    std::unordered_map<world::ChunkCoord, Slot, world::ChunkCoordHash> slots_;
    std::vector<u32> free_slots_;
    std::vector<Color> tints_;
    std::vector<Instance> frame_instances_;
    std::vector<world::Chunk*> frame_uploads_;
    u32 draw_count_ = 0;
    f32 scale_x_ = 0, scale_y_ = 0;
    f32 atlas_lod_ = 0;
    u64 frame_ = 0;
    TilemapStats stats_;
};

} // namespace forge::render
