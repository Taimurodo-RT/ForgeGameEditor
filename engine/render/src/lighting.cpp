#include "forge/render/lighting.h"

#include "forge/core/jobs.h"
#include "forge/core/log.h"
#include "forge/core/profile.h"
#include "forge/core/time.h"
#include "forge/render/gpu.h"

#include "shaders/light_composite_frag.h"
#include "shaders/light_composite_vert.h"
#include "shaders/light_spread_comp.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

namespace forge::render {

using world::Chunk;
using world::kChunkSize;

namespace {

constexpr u32 kGroup = 16;   // compute group is 16 × 16, as in the shader
constexpr i32 kMargin = 40;  // tiles of world outside the screen that still cast light onto it

struct SpreadParams {
    f32 transmit[4];
    u32 width, height, first, pad;
};

u32 channel(f32 v) { return static_cast<u32>(std::clamp(v, 0.0f, 3.996f) * 256.0f); }

u32 pack_source(f32 r, f32 g, f32 b, u32 kind) { return channel(r) | channel(g) << 10 | channel(b) << 20 | kind << 30; }

} // namespace

LightRenderer::~LightRenderer() { shutdown(); }

bool LightRenderer::init(SDL_GPUDevice* device, SDL_GPUTextureFormat target_format) {
    device_ = device;
    const u32 cells = kMaxCellsW * kMaxCellsH;
    SDL_GPUBufferCreateInfo binfo{};
    binfo.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
    binfo.size = cells * 4;
    cells_ = SDL_CreateGPUBuffer(device_, &binfo);
    binfo.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE |
                  SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    binfo.size = cells * 16;
    light_[0] = SDL_CreateGPUBuffer(device_, &binfo);
    light_[1] = SDL_CreateGPUBuffer(device_, &binfo);
    SDL_GPUTransferBufferCreateInfo tinfo{};
    tinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tinfo.size = cells * 4;
    transfer_ = SDL_CreateGPUTransferBuffer(device_, &tinfo);
    spread_ = create_compute_pipeline(device_, shaders::light_spread_comp);
    if (!cells_ || !light_[0] || !light_[1] || !transfer_ || !spread_) {
        FORGE_ERROR("lighting: setup failed: %s", SDL_GetError());
        return false;
    }

    SDL_GPUShader* vs = create_shader(device_, shaders::light_composite_vert);
    SDL_GPUShader* fs = create_shader(device_, shaders::light_composite_frag);
    if (!vs || !fs) {
        if (vs) SDL_ReleaseGPUShader(device_, vs);
        if (fs) SDL_ReleaseGPUShader(device_, fs);
        return false;
    }
    // picture = picture × light
    SDL_GPUColorTargetDescription color{};
    color.format = target_format;
    color.blend_state.enable_blend = true;
    color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_COLOR;
    color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    SDL_GPUGraphicsPipelineCreateInfo pinfo{};
    pinfo.vertex_shader = vs;
    pinfo.fragment_shader = fs;
    pinfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pinfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pinfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pinfo.target_info.color_target_descriptions = &color;
    pinfo.target_info.num_color_targets = 1;
    composite_ = SDL_CreateGPUGraphicsPipeline(device_, &pinfo);
    SDL_ReleaseGPUShader(device_, vs);
    SDL_ReleaseGPUShader(device_, fs);
    if (!composite_) {
        FORGE_ERROR("lighting: pipeline creation failed: %s", SDL_GetError());
        return false;
    }
    return true;
}

void LightRenderer::shutdown() {
    if (!device_) return;
    if (spread_) SDL_ReleaseGPUComputePipeline(device_, spread_);
    if (composite_) SDL_ReleaseGPUGraphicsPipeline(device_, composite_);
    if (cells_) SDL_ReleaseGPUBuffer(device_, cells_);
    for (SDL_GPUBuffer*& b : light_) {
        if (b) SDL_ReleaseGPUBuffer(device_, b);
        b = nullptr;
    }
    if (transfer_) SDL_ReleaseGPUTransferBuffer(device_, transfer_);
    spread_ = nullptr;
    composite_ = nullptr;
    cells_ = nullptr;
    transfer_ = nullptr;
    device_ = nullptr;
    ready_ = false;
}

void LightRenderer::prepare(SDL_GPUCommandBuffer* cmd, const world::World& world, const Camera2D& camera, u32 width,
                            u32 height) {
    FORGE_ZONE_N("Lighting prepare");
    const u64 t0 = time_now_ns();
    ready_ = false;

    // The grid: visible tiles plus a margin, one cell per `step` tiles, with
    // its corner on a multiple of step so cells do not shift while scrolling.
    world::Rect view = camera.visible_tiles(width, height);
    view.x0 -= kMargin, view.y0 -= kMargin, view.x1 += kMargin, view.y1 += kMargin;
    i32 step = 1;
    while ((view.x1 - view.x0) / step >= static_cast<i32>(kMaxCellsW) - 1 ||
           (view.y1 - view.y0) / step >= static_cast<i32>(kMaxCellsH) - 1)
        step *= 2;
    auto floor_div = [](i32 a, i32 b) { return a >= 0 ? a / b : -((-a + b - 1) / b); };
    const i32 gx0 = floor_div(view.x0, step) * step, gy0 = floor_div(view.y0, step) * step;
    const u32 gw = static_cast<u32>(floor_div(view.x1 - gx0, step) + 1);
    const u32 gh = static_cast<u32>(floor_div(view.y1 - gy0, step) + 1);
    grid_.resize(static_cast<usize>(gw) * gh);

    // Cells from tiles, row by row in parallel. A cell looks at the tile in
    // its middle; unloaded chunks are dark and block light.
    const u32 layers = world.layer_count();
    const u32 blocking = std::min(rules_.blocking_layer, layers - 1);
    const LightRules& rules = rules_;
    const u32 dark = pack_source(0, 0, 0, static_cast<u32>(LightKind::Opaque));
    jobs::parallel_for(gh, 8, [&](u32 b, u32 e) {
        for (u32 row = b; row < e; ++row) {
            const i32 ty = gy0 + static_cast<i32>(row) * step + step / 2;
            u32* out = grid_.data() + static_cast<usize>(row) * gw;
            const Chunk* chunk = nullptr;
            i32 chunk_x = INT32_MIN;
            for (u32 col = 0; col < gw; ++col) {
                const i32 tx = gx0 + static_cast<i32>(col) * step + step / 2;
                const i32 cx = tx >> world::kChunkShift;
                if (cx != chunk_x) {
                    chunk_x = cx;
                    chunk = world.find_chunk({cx, ty >> world::kChunkShift});
                }
                if (!chunk) {
                    out[col] = dark;
                    continue;
                }
                const u32 index = world::local_index(tx, ty);
                const world::TileId front = chunk->layer(blocking)[index];
                u32 kind = 0;
                if (front != 0 && front < rules.kinds.size()) kind = static_cast<u32>(rules.kinds[front]);
                f32 r = 0, g = 0, bl = 0;
                if (front != 0 && front < rules.glow.size()) {
                    const Color& c = rules.glow[front];
                    r = c.r, g = c.g, bl = c.b;
                }
                if (rules.sky) {
                    bool open = true;
                    for (u32 l = 0; l < layers && open; ++l) open = chunk->layer(l)[index] == 0;
                    if (open) {
                        r = std::max(r, rules.sky_color.r);
                        g = std::max(g, rules.sky_color.g);
                        bl = std::max(bl, rules.sky_color.b);
                    }
                }
                out[col] = pack_source(r, g, bl, kind);
            }
        }
    });
    // Lamps: the brightest source wins in each cell.
    for (const PointLight& l : lights_) {
        const i32 cx = (static_cast<i32>(std::floor(l.x)) - gx0) / step;
        const i32 cy = (static_cast<i32>(std::floor(l.y)) - gy0) / step;
        if (l.x < gx0 || l.y < gy0 || cx >= static_cast<i32>(gw) || cy >= static_cast<i32>(gh)) continue;
        u32& cell = grid_[static_cast<usize>(cy) * gw + static_cast<usize>(cx)];
        const u32 r = std::max(cell & 1023u, channel(l.r)), g = std::max((cell >> 10) & 1023u, channel(l.g)),
                  b = std::max((cell >> 20) & 1023u, channel(l.b));
        cell = r | g << 10 | b << 20 | (cell & (3u << 30));
    }
    stats_.lights = static_cast<u32>(lights_.size());
    lights_.clear();
    stats_.cells_w = gw;
    stats_.cells_h = gh;
    stats_.step = static_cast<u32>(step);

    void* mapped = SDL_MapGPUTransferBuffer(device_, transfer_, true);
    if (!mapped) return;
    std::memcpy(mapped, grid_.data(), grid_.size() * sizeof(u32));
    SDL_UnmapGPUTransferBuffer(device_, transfer_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src{transfer_, 0};
    SDL_GPUBufferRegion dst{cells_, 0, static_cast<u32>(grid_.size() * sizeof(u32))};
    SDL_UploadToGPUBuffer(copy, &src, &dst, true);
    SDL_EndGPUCopyPass(copy);
    stats_.cpu_ms = ns_to_ms(time_now_ns() - t0);

    // Spread: ping-pong between the two light buffers.
    SpreadParams p{};
    std::copy(std::begin(rules_.transmit), std::end(rules_.transmit), p.transmit);
    p.width = gw;
    p.height = gh;
    u32 from = 0;
    for (u32 i = 0; i <= kSteps; ++i) {
        const u32 to = from ^ 1u;
        p.first = i == 0 ? 1u : 0u;
        SDL_GPUStorageBufferReadWriteBinding rw{};
        rw.buffer = light_[to];
        rw.cycle = false;
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, nullptr, 0, &rw, 1);
        SDL_BindGPUComputePipeline(pass, spread_);
        SDL_GPUBuffer* ro[2] = {cells_, light_[from]};
        SDL_BindGPUComputeStorageBuffers(pass, 0, ro, 2);
        SDL_PushGPUComputeUniformData(cmd, 0, &p, sizeof(p));
        SDL_DispatchGPUCompute(pass, (gw + kGroup - 1) / kGroup, (gh + kGroup - 1) / kGroup, 1);
        SDL_EndGPUComputePass(pass);
        from = to;
    }
    result_ = from;

    Composite& c = composite_params_;
    c.ambient[0] = rules_.ambient.r;
    c.ambient[1] = rules_.ambient.g;
    c.ambient[2] = rules_.ambient.b;
    c.ambient[3] = 1.0f;
    c.screen[0] = static_cast<f32>(width);
    c.screen[1] = static_cast<f32>(height);
    c.camera[0] = static_cast<f32>(camera.snapped_x() - gx0);
    c.camera[1] = static_cast<f32>(camera.snapped_y() - gy0);
    c.zoom = camera.zoom;
    c.step = static_cast<f32>(step);
    c.width = gw;
    c.height = gh;
    ready_ = true;
}

void LightRenderer::draw(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass) {
    if (!ready_) return;
    FORGE_ZONE_N("Lighting draw");
    SDL_BindGPUGraphicsPipeline(pass, composite_);
    SDL_BindGPUFragmentStorageBuffers(pass, 0, &light_[result_], 1);
    SDL_PushGPUFragmentUniformData(cmd, 0, &composite_params_, sizeof(composite_params_));
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
}

} // namespace forge::render
