#pragma once

// Draws a SpriteBatch: copied to upload memory and (when sorted) culled and
// sorted on the CPU in parallel, then one upload and one draw call for all of it.

#include "forge/core/types.h"
#include "forge/render/camera.h"
#include "forge/render/sprite_batch.h"

#include <SDL3/SDL_gpu.h>

namespace forge::render {

// A picture inside the sprite sheet, in pixels.
struct SpriteRect {
    u32 x = 0, y = 0, w = 0, h = 0;
};

// One RGBA8 texture holding every frame (straight, not premultiplied, alpha).
struct SpriteSheet {
    const u8* rgba = nullptr;
    u32 width = 0;
    u32 height = 0;
    const SpriteRect* frames = nullptr;
    u32 frame_count = 0;
    bool smooth = false; // linear filtering; off keeps pixel art crisp
};

struct SpriteStats {
    u32 submitted = 0;
    u32 drawn = 0;
    u32 dropped = 0;
    f64 cpu_ms = 0; // copy to upload memory + cull + sort
};

class SpriteRenderer {
public:
    SpriteRenderer() = default;
    ~SpriteRenderer();
    SpriteRenderer(const SpriteRenderer&) = delete;
    SpriteRenderer& operator=(const SpriteRenderer&) = delete;

    // max_sprites bounds how many can be drawn in one frame (36 bytes each,
    // twice: upload memory and GPU memory).
    bool init(SDL_GPUDevice* device, SDL_GPUTextureFormat target_format, const SpriteSheet& sheet,
              u32 max_sprites = 1u << 20);
    void shutdown();
    // Swaps the sheet for another (more frames, other pictures); outside any
    // render pass. Frame numbers already in use keep pointing at their rects.
    bool set_sheet(const SpriteSheet& sheet);

    // Each frame, outside any render pass. sorted = false keeps push order
    // (cheaper; fine for effects where order does not matter).
    void prepare(SDL_GPUCommandBuffer* cmd, SpriteBatch& batch, const Camera2D& camera, u32 width, u32 height,
                 bool sorted = true);
    // Inside a render pass.
    void draw(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass);

    const SpriteStats& stats() const { return stats_; }
    u32 max_sprites() const { return max_sprites_; }

private:
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUGraphicsPipeline* pipeline_ = nullptr;
    SDL_GPUBuffer* sprites_ = nullptr;
    SDL_GPUBuffer* frames_ = nullptr;
    SDL_GPUBuffer* order_ = nullptr;
    SDL_GPUTransferBuffer* transfer_ = nullptr;
    SDL_GPUTexture* sheet_ = nullptr;
    SDL_GPUSampler* sampler_ = nullptr;
    u32 max_sprites_ = 0;
    u32 draw_count_ = 0;
    struct View {
        f32 scale[2];
        f32 offset[2];
        u32 sorted;
        u32 pad[3];
    } view_{};
    SpriteStats stats_;
};

} // namespace forge::render
