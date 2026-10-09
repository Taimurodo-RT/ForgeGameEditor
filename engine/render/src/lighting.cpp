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
#include <limits>
#include <queue>

namespace forge::render {

using world::Chunk;
using world::kChunkSize;

namespace {

constexpr u32 kGroup = 16;   // compute group is 16 × 16, as in the shader
constexpr i32 kMargin = kLightMargin; // tiles of world outside the screen that still cast light onto it

struct SpreadParams {
    f32 transmit[4];
    u32 width, height, first, pad;
};

u32 channel(f32 v) { return static_cast<u32>(std::clamp(v, 0.0f, 3.996f) * 256.0f); }

u32 pack_source(f32 r, f32 g, f32 b, u32 kind) { return channel(r) | channel(g) << 10 | channel(b) << 20 | kind << 30; }

constexpr f64 kSqrt2 = 1.41421356237309515;
constexpr f64 kFar = std::numeric_limits<f64>::infinity();

// The grid a lamp's light is worked out over.
struct LampGrid {
    const u32* cells;
    u32 w, h;
    i32 x0, y0;
    u32 step;
    f64 cost[4]; // tiles of air one tile of each kind counts for; kFar: none passes
};

LampGrid lamp_grid(const u32* cells, u32 w, u32 h, i32 x0, i32 y0, u32 step, const f32 transmit[4]) {
    LampGrid g{cells, w, h, x0, y0, std::max(step, 1u), {1, 1, 1, 1}};
    const f64 open = transmit[0];
    for (u32 k = 0; k < 4; ++k) {
        const f64 t = transmit[k];
        if (!(t > 0)) g.cost[k] = kFar;
        else if (open > 0 && open < 1 && t < open) g.cost[k] = std::log(t) / std::log(open);
        else g.cost[k] = 1;
    }
    return g;
}

using detail::LampBox;

void one_lamp(const LampGrid& g, const PointLight& l, LampBox& out) {
    out.bw = out.bh = 0;
    const f64 radius = std::min<f64>(l.radius, kMaxLampRadius);
    if (!(radius > 0) || !std::isfinite(l.x) || !std::isfinite(l.y)) return;
    const f64 step = g.step;
    const f64 fx = (l.x - g.x0) / step, fy = (l.y - g.y0) / step;
    if (fx < 0 || fy < 0 || fx >= g.w || fy >= g.h) return; // off the grid: farther than the margin
    const i32 lx = static_cast<i32>(fx), ly = static_cast<i32>(fy);
    const i32 reach = static_cast<i32>(std::ceil(radius / step)) + 1;
    out.cx0 = std::max(lx - reach, 0);
    out.cy0 = std::max(ly - reach, 0);
    const i32 cx1 = std::min(lx + reach, static_cast<i32>(g.w) - 1), cy1 = std::min(ly + reach, static_cast<i32>(g.h) - 1);
    out.bw = static_cast<u32>(cx1 - out.cx0 + 1);
    out.bh = static_cast<u32>(cy1 - out.cy0 + 1);
    const usize n = static_cast<usize>(out.bw) * out.bh;
    out.way.assign(n, kFar);
    out.rgb.assign(n * 3, 0.0f);

    // The shortest way from the lamp's cell to every cell of the box, through
    // its 8 neighbours; a cell costs by its kind, a diagonal step √2.
    using Item = std::pair<f64, u32>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    const u32 start = static_cast<u32>(ly - out.cy0) * out.bw + static_cast<u32>(lx - out.cx0);
    out.way[start] = 0;
    open.push({0.0, start});
    // A way longer than this cannot end inside the radius (see below).
    const f64 longest = radius * 2 + step * 2;
    while (!open.empty()) {
        const auto [d, i] = open.top();
        open.pop();
        if (d > out.way[i]) continue;
        const i32 bx = static_cast<i32>(i % out.bw), by = static_cast<i32>(i / out.bw);
        for (i32 dy = -1; dy <= 1; ++dy)
            for (i32 dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                const i32 nx = bx + dx, ny = by + dy;
                if (nx < 0 || ny < 0 || nx >= static_cast<i32>(out.bw) || ny >= static_cast<i32>(out.bh)) continue;
                const usize gi = static_cast<usize>(out.cy0 + ny) * g.w + static_cast<usize>(out.cx0 + nx);
                const f64 cost = g.cost[g.cells[gi] >> 30];
                if (cost == kFar) continue;
                const f64 nd = d + cost * step * (dx != 0 && dy != 0 ? kSqrt2 : 1.0);
                const u32 ni = static_cast<u32>(ny) * out.bw + static_cast<u32>(nx);
                if (nd >= out.way[ni] || nd > longest) continue;
                out.way[ni] = nd;
                open.push({nd, ni});
            }
    }

    // The way through free air would be the same steps (max + (√2 − 1) min
    // cells); in the open, light goes straight, so a cell is as far as its
    // straight distance, made longer by as much as its way is longer than the
    // free one. Near the lamp the ratio of the two is at least ½, so a way
    // past `longest` is past the radius.
    const f32 r = l.r, gg = l.g, b = l.b;
    for (u32 y = 0; y < out.bh; ++y)
        for (u32 x = 0; x < out.bw; ++x) {
            const usize i = static_cast<usize>(y) * out.bw + x;
            const f64 way = out.way[i];
            if (way == kFar) continue;
            const i32 cx = out.cx0 + static_cast<i32>(x), cy = out.cy0 + static_cast<i32>(y);
            const f64 tx = g.x0 + (cx + 0.5) * step, ty = g.y0 + (cy + 0.5) * step;
            const f64 straight = std::hypot(tx - l.x, ty - l.y);
            const i32 ax = std::abs(cx - lx), ay = std::abs(cy - ly);
            const f64 free = (std::max(ax, ay) + (kSqrt2 - 1) * std::min(ax, ay)) * step;
            const f64 d = free > 0 ? straight * (way / free) : straight;
            const f32 k = lamp_falloff(d, radius);
            if (k <= 0) continue;
            out.rgb[i * 3 + 0] = std::clamp(r * k, 0.0f, 4.0f);
            out.rgb[i * 3 + 1] = std::clamp(gg * k, 0.0f, 4.0f);
            out.rgb[i * 3 + 2] = std::clamp(b * k, 0.0f, 4.0f);
        }
}

void merge_lamp(const LampBox& box, u32 w, std::vector<f32>& rgb) {
    for (u32 y = 0; y < box.bh; ++y)
        for (u32 x = 0; x < box.bw; ++x) {
            const f32* from = &box.rgb[(static_cast<usize>(y) * box.bw + x) * 3];
            f32* to = &rgb[((static_cast<usize>(box.cy0) + y) * w + static_cast<usize>(box.cx0) + x) * 3];
            for (u32 c = 0; c < 3; ++c) to[c] = std::max(to[c], from[c]);
        }
}

} // namespace

f32 lamp_falloff(f64 d, f64 radius) {
    if (!(radius > 0) || !(d < radius)) return 0;
    const f64 q = d > 0 ? d / radius : 0;
    const f64 k = 1 - q * q;
    return static_cast<f32>(k * k);
}

void lamp_light(const u32* cells, u32 w, u32 h, i32 x0, i32 y0, u32 step, const f32 transmit[4],
                std::span<const PointLight> lamps, std::vector<f32>& rgb) {
    rgb.resize(static_cast<usize>(w) * h * 3, 0.0f);
    const LampGrid g = lamp_grid(cells, w, h, x0, y0, step, transmit);
    LampBox box;
    for (const PointLight& l : lamps) {
        one_lamp(g, l, box);
        merge_lamp(box, w, rgb);
    }
}

Color sky_at(std::span<const SkyKey> keys, f64 hour, const Color& fallback) {
    if (keys.empty() || !std::isfinite(hour)) return fallback;
    hour = std::fmod(hour, 24.0);
    if (hour < 0) hour += 24;
    // The key at or before this hour (the last of yesterday before the first).
    usize at = keys.size() - 1;
    for (usize i = 0; i < keys.size(); ++i)
        if (keys[i].hour <= hour) at = i;
    const SkyKey& a = keys[at];
    const SkyKey& b = keys[(at + 1) % keys.size()];
    f64 from = a.hour, to = b.hour, now = hour;
    if (to <= from) to += 24; // across midnight
    if (now < from) now += 24;
    const f64 span = to - from;
    const f32 t = span > 0 ? static_cast<f32>(std::clamp((now - from) / span, 0.0, 1.0)) : 0.0f;
    auto mix = [t](f32 x, f32 y) { return x == y ? x : x + (y - x) * t; };
    return {mix(a.color.r, b.color.r), mix(a.color.g, b.color.g), mix(a.color.b, b.color.b), 1.0f};
}

LightRenderer::~LightRenderer() { shutdown(); }

bool LightRenderer::init(SDL_GPUDevice* device, SDL_GPUTextureFormat target_format) {
    device_ = device;
    const u32 cells = kMaxCellsW * kMaxCellsH;
    SDL_GPUBufferCreateInfo binfo{};
    binfo.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
    binfo.size = cells * 4;
    cells_ = SDL_CreateGPUBuffer(device_, &binfo);
    binfo.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    lamps_ = SDL_CreateGPUBuffer(device_, &binfo);
    binfo.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE |
                  SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    binfo.size = cells * 16;
    light_[0] = SDL_CreateGPUBuffer(device_, &binfo);
    light_[1] = SDL_CreateGPUBuffer(device_, &binfo);
    SDL_GPUTransferBufferCreateInfo tinfo{};
    tinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tinfo.size = cells * 8; // the cells, then the lamps with a radius
    transfer_ = SDL_CreateGPUTransferBuffer(device_, &tinfo);
    spread_ = create_compute_pipeline(device_, shaders::light_spread_comp);
    if (!cells_ || !lamps_ || !light_[0] || !light_[1] || !transfer_ || !spread_) {
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
    if (lamps_) SDL_ReleaseGPUBuffer(device_, lamps_);
    for (SDL_GPUBuffer*& b : light_) {
        if (b) SDL_ReleaseGPUBuffer(device_, b);
        b = nullptr;
    }
    if (transfer_) SDL_ReleaseGPUTransferBuffer(device_, transfer_);
    spread_ = nullptr;
    composite_ = nullptr;
    cells_ = nullptr;
    lamps_ = nullptr;
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
    // Lamps: the brightest source wins in each cell; those with a radius go
    // their own way below.
    std::vector<PointLight>& lamps = lamps_now_;
    lamps.clear();
    for (const PointLight& l : lights_) {
        if (l.radius > 0) {
            lamps.push_back(l);
            continue;
        }
        const i32 cx = (static_cast<i32>(std::floor(l.x)) - gx0) / step;
        const i32 cy = (static_cast<i32>(std::floor(l.y)) - gy0) / step;
        if (l.x < gx0 || l.y < gy0 || cx >= static_cast<i32>(gw) || cy >= static_cast<i32>(gh)) continue;
        u32& cell = grid_[static_cast<usize>(cy) * gw + static_cast<usize>(cx)];
        const u32 r = std::max(cell & 1023u, channel(l.r)), g = std::max((cell >> 10) & 1023u, channel(l.g)),
                  b = std::max((cell >> 20) & 1023u, channel(l.b));
        cell = r | g << 10 | b << 20 | (cell & (3u << 30));
    }
    stats_.lights = static_cast<u32>(lights_.size());
    stats_.lamps = static_cast<u32>(lamps.size());
    lights_.clear();
    stats_.cells_w = gw;
    stats_.cells_h = gh;
    stats_.x0 = gx0;
    stats_.y0 = gy0;
    stats_.step = static_cast<u32>(step);

    // Lamps with a radius: each on a job thread, then the brightest per cell.
    lamp_rgb_.clear();
    if (!lamps.empty()) {
        const LampGrid lg = lamp_grid(grid_.data(), gw, gh, gx0, gy0, static_cast<u32>(step), rules_.transmit);
        if (lamp_boxes_.size() < lamps.size()) lamp_boxes_.resize(lamps.size());
        std::vector<LampBox>& boxes = lamp_boxes_;
        jobs::parallel_for(static_cast<u32>(lamps.size()), 1, [&](u32 b, u32 e) {
            for (u32 i = b; i < e; ++i) one_lamp(lg, lamps[i], boxes[i]);
        });
        lamp_rgb_.assign(grid_.size() * 3, 0.0f);
        for (usize i = 0; i < lamps.size(); ++i) merge_lamp(boxes[i], gw, lamp_rgb_);
        lamp_packed_.resize(grid_.size());
        for (usize i = 0; i < grid_.size(); ++i)
            lamp_packed_[i] = channel(lamp_rgb_[i * 3]) | channel(lamp_rgb_[i * 3 + 1]) << 10 | channel(lamp_rgb_[i * 3 + 2]) << 20;
    }

    void* mapped = SDL_MapGPUTransferBuffer(device_, transfer_, true);
    if (!mapped) return;
    const u32 bytes = static_cast<u32>(grid_.size() * sizeof(u32));
    std::memcpy(mapped, grid_.data(), bytes);
    if (!lamps.empty()) std::memcpy(static_cast<u8*>(mapped) + bytes, lamp_packed_.data(), bytes);
    SDL_UnmapGPUTransferBuffer(device_, transfer_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src{transfer_, 0};
    SDL_GPUBufferRegion dst{cells_, 0, bytes};
    SDL_UploadToGPUBuffer(copy, &src, &dst, true);
    if (!lamps.empty()) {
        SDL_GPUTransferBufferLocation lsrc{transfer_, bytes};
        SDL_GPUBufferRegion ldst{lamps_, 0, bytes};
        SDL_UploadToGPUBuffer(copy, &lsrc, &ldst, true);
    }
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
    c.lamps = lamps.empty() ? 0u : 1u;
    ready_ = true;
}

Color LightRenderer::lamp_at(f64 x, f64 y) const {
    if (lamp_rgb_.empty() || stats_.step == 0) return {0, 0, 0, 1};
    const f64 fx = (x - stats_.x0) / stats_.step, fy = (y - stats_.y0) / stats_.step;
    if (!(fx >= 0 && fy >= 0 && fx < stats_.cells_w && fy < stats_.cells_h)) return {0, 0, 0, 1};
    const usize i = (static_cast<usize>(fy) * stats_.cells_w + static_cast<usize>(fx)) * 3;
    return {lamp_rgb_[i], lamp_rgb_[i + 1], lamp_rgb_[i + 2], 1};
}

void LightRenderer::draw(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass) {
    if (!ready_) return;
    FORGE_ZONE_N("Lighting draw");
    SDL_BindGPUGraphicsPipeline(pass, composite_);
    SDL_GPUBuffer* read[2] = {light_[result_], lamps_};
    SDL_BindGPUFragmentStorageBuffers(pass, 0, read, 2);
    SDL_PushGPUFragmentUniformData(cmd, 0, &composite_params_, sizeof(composite_params_));
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
}

} // namespace forge::render
