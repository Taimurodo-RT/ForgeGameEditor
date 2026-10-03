#pragma once

// Particles that live entirely on the GPU: spawned, moved and drawn by the
// video card, so a million of them cost the CPU almost nothing. The CPU only
// says where and how many to spawn each frame.
//
// Positions are kept relative to the system's origin, so a system belongs to
// a place in the world (a battle, a weather area around the camera).

#include "forge/core/types.h"
#include "forge/render/camera.h"
#include "forge/render/sprite_renderer.h"

#include <SDL3/SDL_gpu.h>

#include <vector>

namespace forge::render {

// One burst or one frame's worth of a stream. Angles in radians, clockwise
// on screen (0 = right, π/2 = down).
struct ParticleEmit {
    f64 x = 0, y = 0;        // world tiles
    u32 count = 0;
    f32 radius = 0;          // spawn anywhere inside this disc
    f32 speed_min = 0, speed_max = 1; // tiles per second
    f32 angle = 0;           // direction of flight...
    f32 spread = 6.2831853f; // ...and the width of the cone around it (2π: all around)
    f32 life_min = 1, life_max = 2; // seconds
    f32 size_start = 0.25f, size_end = 0.25f; // tiles
    f32 spin = 0;            // up to this many radians per second, either way
    u32 color = 0xffffffffu; // RGBA8; fades out over the second half of life
    u32 frame = 0;           // picture in the sprite sheet
};

struct ParticleLook {
    f32 gravity_x = 0, gravity_y = 0; // tiles per second²
    f32 drag = 0;                     // share of speed lost per second
    bool additive = false;            // glows add up (fire, sparks); off: normal blending (smoke, rain)
};

struct ParticleStats {
    u32 capacity = 0;
    u32 slots_used = 0;      // slots ever filled (dead ones are reused in turn)
    u32 spawned_last = 0;    // last frame
    u64 spawned_total = 0;
};

class ParticleSystem {
public:
    ParticleSystem() = default;
    ~ParticleSystem();
    ParticleSystem(const ParticleSystem&) = delete;
    ParticleSystem& operator=(const ParticleSystem&) = delete;

    // capacity: most particles alive at once (48 bytes each on the GPU). When
    // full, new particles replace the oldest.
    bool init(SDL_GPUDevice* device, SDL_GPUTextureFormat target_format, const SpriteSheet& sheet, u32 capacity,
              f64 origin_x, f64 origin_y, const ParticleLook& look = {});
    void shutdown();

    void set_look(const ParticleLook& look) { look_ = look; }
    void emit(const ParticleEmit& emit);

    // Each frame, outside any render pass: spawns what was emitted and moves
    // everything by dt seconds (both on the GPU).
    void simulate(SDL_GPUCommandBuffer* cmd, f32 dt);
    // Inside a render pass.
    void draw(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass, const Camera2D& camera, u32 width, u32 height);

    const ParticleStats& stats() const { return stats_; }

private:
    struct GpuEmitter {
        f32 x, y;
        f32 radius, speed_min, speed_max, angle, spread, life_min, life_max, size_start, size_end, spin;
        u32 color, frame, first, count;
    };
    static_assert(sizeof(GpuEmitter) == 64);

    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUComputePipeline* emit_pipeline_ = nullptr;
    SDL_GPUComputePipeline* update_pipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* draw_pipeline_ = nullptr;
    SDL_GPUBuffer* particles_ = nullptr;
    SDL_GPUBuffer* emitters_ = nullptr;
    SDL_GPUBuffer* frames_ = nullptr;
    SDL_GPUTransferBuffer* transfer_ = nullptr;
    SDL_GPUTexture* sheet_ = nullptr;
    SDL_GPUSampler* sampler_ = nullptr;
    u32 capacity_ = 0;
    u32 max_emitters_ = 4096;
    u32 cursor_ = 0; // next slot in the ring
    u32 seed_ = 1;
    f64 origin_x_ = 0, origin_y_ = 0;
    ParticleLook look_;
    std::vector<GpuEmitter> pending_;
    u32 pending_count_ = 0;
    ParticleStats stats_;
};

} // namespace forge::render
