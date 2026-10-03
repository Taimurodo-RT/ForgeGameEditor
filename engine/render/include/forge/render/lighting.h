#pragma once

// 2D light and shadow over a tile world. Every frame the visible tiles (and a
// margin around them) become a grid of cells; tiles decide how much light
// passes through each cell, and light sources (the sky, glowing tiles, lamps)
// are spread over the grid by the GPU. Light flows around corners and fades
// inside rock, so caves are dark and a torch lights its tunnel. The result
// multiplies the picture drawn so far.

#include "forge/core/math.h"
#include "forge/core/types.h"
#include "forge/render/camera.h"
#include "forge/world/world.h"

#include <SDL3/SDL_gpu.h>

#include <vector>

namespace forge::render {

// How tiles take part in lighting.
enum class LightKind : u8 {
    Open = 0,   // air, floors: light passes almost freely
    Dense = 1,  // water, leaves, glass: dims light
    Solid = 2,  // rock, walls: light goes only a few tiles in
    Opaque = 3, // nothing gets through
};

struct LightRules {
    // The layer whose tiles block light (usually the front layer).
    u32 blocking_layer = 1;
    // LightKind of every tile id on that layer; ids past the end are Open,
    // except id 0 which is always Open.
    std::vector<LightKind> kinds;
    // Light kept when passing one tile of each kind.
    f32 transmit[4] = {0.93f, 0.82f, 0.72f, 0.0f};
    // Sky light (side view): cells where every layer is empty glow with this.
    bool sky = false;
    Color sky_color{1.0f, 1.0f, 1.0f, 1.0f};
    // Added everywhere after spreading (top down: daylight; night: low).
    Color ambient{0.0f, 0.0f, 0.0f, 1.0f};
    // Tiles on the blocking layer that glow (lava, crystals): colour per id.
    std::vector<Color> glow;
};

// A lamp. Colour channels above 1 shine further (up to 4).
struct PointLight {
    f64 x = 0, y = 0; // tiles
    f32 r = 1, g = 1, b = 1;
};

struct LightStats {
    u32 cells_w = 0, cells_h = 0;
    u32 step = 1;  // tiles per cell (grows when zoomed far out)
    u32 lights = 0;
    f64 cpu_ms = 0; // building the grid
};

class LightRenderer {
public:
    LightRenderer() = default;
    ~LightRenderer();
    LightRenderer(const LightRenderer&) = delete;
    LightRenderer& operator=(const LightRenderer&) = delete;

    bool init(SDL_GPUDevice* device, SDL_GPUTextureFormat target_format);
    void shutdown();

    void set_rules(const LightRules& rules) { rules_ = rules; }
    LightRules& rules() { return rules_; }

    // Lamps for this frame (cleared by prepare).
    void add(const PointLight& light) { lights_.push_back(light); }

    // Each frame, outside any render pass: builds the grid and spreads light on the GPU.
    void prepare(SDL_GPUCommandBuffer* cmd, const world::World& world, const Camera2D& camera, u32 width, u32 height);
    // Inside a render pass, after everything that light should touch.
    void draw(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass);

    const LightStats& stats() const { return stats_; }

    static constexpr u32 kMaxCellsW = 1024;
    static constexpr u32 kMaxCellsH = 640;
    static constexpr u32 kSteps = 40; // how far light spreads, in cells

private:
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUComputePipeline* spread_ = nullptr;
    SDL_GPUGraphicsPipeline* composite_ = nullptr;
    SDL_GPUBuffer* cells_ = nullptr;
    SDL_GPUBuffer* light_[2] = {nullptr, nullptr};
    SDL_GPUTransferBuffer* transfer_ = nullptr;
    LightRules rules_;
    std::vector<PointLight> lights_;
    std::vector<u32> grid_;
    u32 result_ = 0; // which light buffer holds the answer
    struct Composite {
        f32 ambient[4];
        f32 screen[2];
        f32 camera[2];
        f32 zoom;
        f32 step;
        u32 width;
        u32 height;
    } composite_params_{};
    bool ready_ = false;
    LightStats stats_;
};

} // namespace forge::render
