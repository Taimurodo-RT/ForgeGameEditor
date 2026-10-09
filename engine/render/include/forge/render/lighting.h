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

#include <span>
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
    // Tiles the sky shines through as if the cell were empty (by id,
    // non-zero: yes; ids past the end: no), on the layers of the mask (bit
    // per layer; not one that holds liquids, whose values are not ids): a
    // level's own pictures of a background, which the sky is part of.
    std::vector<u8> sky_through;
    u32 sky_through_layers = 0;
    Color sky_color{1.0f, 1.0f, 1.0f, 1.0f};
    // Added everywhere after spreading (top down: daylight; night: low).
    Color ambient{0.0f, 0.0f, 0.0f, 1.0f};
    // Tiles on the blocking layer that glow (lava, crystals): colour per id.
    std::vector<Color> glow;
};

// A lamp. Without a radius its colour channels above 1 shine further (up to
// 4): the light spreads as the sky's does. With one, the colour is the light
// at the centre and the radius how far it goes (see lamp_light).
struct PointLight {
    f64 x = 0, y = 0; // tiles
    f32 r = 1, g = 1, b = 1;
    f32 radius = 0; // tiles, up to kMaxLampRadius; 0: none
};

// How much world around the screen the light grid has, in tiles: what is
// there still lights the screen (and unloaded chunks there are dark).
inline constexpr i32 kLightMargin = 40;
// The farthest a lamp with a radius reaches, in tiles: the grid has this much
// world around the screen, so a lamp off screen this far still lights it.
inline constexpr f32 kMaxLampRadius = kLightMargin;

// How much of a lamp's light is left d tiles along its way (straight through
// the air, longer around corners and through water or rock): all of it at the
// centre, smoothly less, none at the radius and beyond.
f32 lamp_falloff(f64 d, f64 radius);

// Lamps with a radius over a grid of cells: cells (w × h, row by row, each
// step tiles, the first at tile x0, y0) hold a LightKind in their top 2 bits,
// as the spread has them. Light goes from the lamp's point to a cell's centre
// as far as straight through the air; when it must go around blocks or pass
// through water or rock, the way is longer by as much as it loses there with
// transmit (rock 0.72 against air 0.93: one tile of rock is about 4.5 of air).
// rgb (3 per cell) gets the brightest light per channel, up to 4; cells no
// lamp reaches are left as they were.
void lamp_light(const u32* cells, u32 w, u32 h, i32 x0, i32 y0, u32 step, const f32 transmit[4],
                std::span<const PointLight> lamps, std::vector<f32>& rgb);

// The sky through a day: its colour at hours of the day (0..24, in order);
// between two it changes evenly, from the last on to the first of the next day.
struct SkyKey {
    f32 hour = 0;
    Color color;
};
// The sky's colour at an hour (any, taken within 0..24); no keys: fallback.
Color sky_at(std::span<const SkyKey> keys, f64 hour, const Color& fallback);

struct LightStats {
    u32 cells_w = 0, cells_h = 0;
    i32 x0 = 0, y0 = 0; // the grid's first cell, in tiles
    u32 step = 1;  // tiles per cell (grows when zoomed far out)
    u32 lights = 0;
    u32 lamps = 0;  // of them, with a radius
    f64 cpu_ms = 0; // building the grid (the lamps with a radius too)
};

namespace detail {
// One lamp's light over the cells around it (LightRenderer's scratch).
struct LampBox {
    i32 cx0 = 0, cy0 = 0; // the box's first cell in the grid
    u32 bw = 0, bh = 0;
    std::vector<f32> rgb; // 3 per cell of the box; 0: no light
    std::vector<f64> way; // tiles along the shortest way so far
};
} // namespace detail

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
    // The light of the lamps with a radius at a tile point, as the last
    // prepare() worked it out (0 outside the grid or with none).
    Color lamp_at(f64 x, f64 y) const;

    static constexpr u32 kMaxCellsW = 1024;
    static constexpr u32 kMaxCellsH = 640;
    static constexpr u32 kSteps = 40; // how far light spreads, in cells

private:
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUComputePipeline* spread_ = nullptr;
    SDL_GPUGraphicsPipeline* composite_ = nullptr;
    SDL_GPUBuffer* cells_ = nullptr;
    SDL_GPUBuffer* lamps_ = nullptr; // the lamps with a radius, packed as cells
    SDL_GPUBuffer* light_[2] = {nullptr, nullptr};
    SDL_GPUTransferBuffer* transfer_ = nullptr;
    LightRules rules_;
    std::vector<PointLight> lights_;
    std::vector<u32> grid_;
    std::vector<f32> lamp_rgb_; // 3 per cell; empty without lamps with a radius
    std::vector<u32> lamp_packed_;
    std::vector<PointLight> lamps_now_;
    std::vector<detail::LampBox> lamp_boxes_;
    u32 result_ = 0; // which light buffer holds the answer
    struct Composite {
        f32 ambient[4];
        f32 screen[2];
        f32 camera[2];
        f32 zoom;
        f32 step;
        u32 width;
        u32 height;
        u32 lamps; // 1: the lamps buffer has light
        u32 pad[3];
    } composite_params_{};
    bool ready_ = false;
    LightStats stats_;
};

} // namespace forge::render
