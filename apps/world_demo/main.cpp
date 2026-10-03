// World demo: a 64k × 64k tile world you can fly through, zoom and dig.
//
//   WASD / arrows   move (Shift: faster)
//   mouse wheel     zoom, toward the cursor
//   left mouse      dig        right mouse   build
//   F               automatic flight across the world
//   1 / 2           side view (like Terraria) / top down (like Factorio)
//
//   forge_world_demo [--topdown] [--fly] [--no-vsync]
//   forge_world_demo --headless 600     no window: streaming only, prints a summary
//   forge_world_demo --screenshot a.png render offscreen (no window) and save an image

#include "forge/core/jobs.h"
#include "forge/core/log.h"
#include "forge/core/profile.h"
#include "forge/core/time.h"
#include "forge/platform/app.h"
#include "forge/render/gpu.h"
#include "forge/render/tilemap_renderer.h"
#include "forge/world/generators.h"
#include "forge/world/world.h"

#include <SDL3/SDL.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

using namespace forge;
using namespace forge::world;
using render::Camera2D;

namespace {

constexpr i32 kWorldChunks = 1024; // 64k × 64k tiles
constexpr i32 kWorldTiles = kWorldChunks * kChunkSize;
constexpr i32 kSurfaceY = 2000;    // side view: ground level, tiles from the top
constexpr u32 kCellPx = 16;
constexpr u32 kCells = 16;

u32 pixel_hash(u32 a, u32 b, u32 c) {
    u32 h = a * 374761393u + b * 668265263u + c * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

struct Rgb {
    u8 r, g, b;
};

// Placeholder art until real tile sets arrive with the asset library: each
// sample tile gets a colour, a little texture and an edge.
std::vector<u8> make_atlas() {
    const u32 size = kCellPx * kCells;
    std::vector<u8> px(static_cast<usize>(size) * size * 4, 0);
    auto put = [&](u32 id, u32 x, u32 y, Rgb c, u8 a = 255) {
        const u32 X = (id % kCells) * kCellPx + x, Y = (id / kCells) * kCellPx + y;
        u8* p = &px[(static_cast<usize>(Y) * size + X) * 4];
        p[0] = c.r, p[1] = c.g, p[2] = c.b, p[3] = a;
    };
    auto shade = [](Rgb c, i32 d) {
        auto f = [d](u8 v) { return static_cast<u8>(std::clamp(static_cast<i32>(v) + d, 0, 255)); };
        return Rgb{f(c.r), f(c.g), f(c.b)};
    };
    struct Solid {
        TileId id;
        Rgb color;
        i32 grain;
        bool edge;
    };
    const Solid solids[] = {
        {TileGrass, {86, 160, 64}, 14, true},       {TileDirt, {120, 84, 52}, 12, true},
        {TileStone, {118, 118, 126}, 14, true},     {TileSand, {214, 194, 128}, 10, true},
        {TileDirtWall, {92, 64, 42}, 6, false},     {TileStoneWall, {84, 84, 92}, 6, false},
        {TileWater, {52, 120, 196}, 6, false},      {TileDeepWater, {34, 82, 156}, 5, false},
        {TileMeadow, {104, 168, 72}, 10, false},    {TileForest, {58, 118, 52}, 10, false},
        {TileSnow, {232, 238, 244}, 6, false},
    };
    for (const Solid& s : solids) {
        for (u32 y = 0; y < kCellPx; ++y) {
            for (u32 x = 0; x < kCellPx; ++x) {
                const i32 n = static_cast<i32>(pixel_hash(s.id, x / 2, y / 2) % (2 * s.grain + 1)) - s.grain;
                Rgb c = shade(s.color, n);
                if (s.edge && (x == kCellPx - 1 || y == kCellPx - 1)) c = shade(c, -28);
                if (s.edge && (x == 0 || y == 0)) c = shade(c, 16);
                put(s.id, x, y, c);
            }
        }
    }
    // Grass: dirt with a green top.
    for (u32 y = 0; y < kCellPx; ++y)
        for (u32 x = 0; x < kCellPx; ++x)
            if (y > 4 + pixel_hash(x, 1, 2) % 3) put(TileGrass, x, y, shade(Rgb{120, 84, 52}, static_cast<i32>(pixel_hash(x, y, 9) % 20) - 10));
    // Ores: stone with coloured flecks.
    const std::pair<TileId, Rgb> ores[] = {{TileCopper, {214, 120, 60}}, {TileIron, {196, 170, 150}}, {TileGold, {246, 206, 60}}};
    for (const auto& [id, fleck] : ores)
        for (u32 y = 0; y < kCellPx; ++y)
            for (u32 x = 0; x < kCellPx; ++x) {
                Rgb c = shade(Rgb{118, 118, 126}, static_cast<i32>(pixel_hash(id, x / 2, y / 2) % 29) - 14);
                if (pixel_hash(id, x / 3, y / 3) % 5 == 0) c = fleck;
                if (x == kCellPx - 1 || y == kCellPx - 1) c = shade(c, -28);
                put(id, x, y, c);
            }
    // Objects on a transparent background: tree crowns and rocks.
    for (u32 y = 0; y < kCellPx; ++y)
        for (u32 x = 0; x < kCellPx; ++x) {
            const f32 dx = static_cast<f32>(x) - 7.5f, dy = static_cast<f32>(y) - 7.0f;
            const f32 d = std::sqrt(dx * dx + dy * dy);
            if (d < 6.5f) put(TileTree, x, y, shade(Rgb{40, 96, 40}, static_cast<i32>(pixel_hash(x, y, 3) % 30) - 15 - static_cast<i32>(d * 3)));
            const f32 ry = static_cast<f32>(y) - 9.0f;
            if (std::sqrt(dx * dx + ry * ry * 2.0f) < 6.0f) put(TileRock, x, y, shade(Rgb{128, 124, 120}, static_cast<i32>(pixel_hash(x, y, 4) % 24) - 12 - static_cast<i32>(ry * 3)));
        }
    return px;
}

std::shared_ptr<const Generator> make_generator(bool top_down) {
    if (top_down) return std::make_shared<TopDownGenerator>(2026);
    return std::make_shared<SideViewGenerator>(2026, kSurfaceY);
}

class WorldDemo final : public App {
public:
    bool top_down = false;
    bool auto_fly = false;
    bool headless = false;

    bool on_init() override {
        atlas_ = make_atlas();
        create_world();
        FORGE_INFO("world demo: %s, %d x %d tiles", top_down ? "top down" : "side view", kWorldTiles, kWorldTiles);
        return true;
    }

    void on_event(const SDL_Event& e) override {
        if (e.type == SDL_EVENT_MOUSE_WHEEL) {
            const f32 factor = e.wheel.y > 0 ? 1.25f : 0.8f;
            zoom_toward(camera_.zoom * factor, mouse_x_, mouse_y_);
        } else if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
            if (e.key.key == SDLK_F) auto_fly = !auto_fly;
            if (e.key.key == SDLK_1 && top_down) switch_genre(false);
            if (e.key.key == SDLK_2 && !top_down) switch_genre(true);
        }
    }

    void on_frame(f64 dt) override {
        FORGE_ZONE_N("Demo frame");
        if (headless) {
            // Hold 60 FPS without a window, so streaming gets real-time conditions.
            dt = 1.0 / 60.0;
            const u64 now = time_now_ns();
            if (next_tick_ns_ > now) std::this_thread::sleep_for(std::chrono::nanoseconds(next_tick_ns_ - now));
            next_tick_ns_ = std::max(next_tick_ns_, now) + 16'666'667ull;
        }
        dt = std::min(dt, 0.1);

        if (!headless) handle_input(dt);
        if (auto_fly || headless) {
            // Diagonal at 3000 tiles per second, bouncing off the world's edges.
            camera_.x += fly_dx_ * 3000.0 * dt;
            camera_.y += fly_dy_ * 3000.0 * dt;
            if (camera_.x < 1000 || camera_.x > kWorldTiles - 1000) fly_dx_ = -fly_dx_;
            if (camera_.y < 1000 || camera_.y > kWorldTiles - 1000) fly_dy_ = -fly_dy_;
        }

        const u64 t0 = time_now_ns();
        world_->update(camera_.visible_tiles(width_, height_));
        const f64 ms = ns_to_ms(time_now_ns() - t0);
        update_ms_sum_ += ms;
        update_ms_worst_ = std::max(update_ms_worst_, ms);
        ++frames_;

        if (!headless && frame_stats().frame % 30 == 0) {
            const WorldStats ws = world_->stats();
            const render::TilemapStats& rs = renderer_.stats();
            char text[256];
            std::snprintf(text, sizeof(text), "%s  zoom %.2g  chunks %u/%u on screen, %u in memory  %s",
                          top_down ? "top down" : "side view", static_cast<f64>(camera_.zoom), rs.drawn_chunks,
                          expected_visible(), ws.resident, auto_fly ? "[auto flight]" : "");
            set_status(text);
        }
    }

    void on_render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, u32 width, u32 height) override {
        width_ = width;
        height_ = height;
        if (!renderer_ready_) {
            render::TileAtlas atlas{atlas_.data(), kCellPx, kCells};
            renderer_ready_ = renderer_.init(gpu(), swapchain_format(), *world_, atlas);
            if (top_down) renderer_.set_layer_tint(0, Color{}); // ground is not a back wall
            if (!renderer_ready_) {
                FORGE_ERROR("tile renderer failed to start");
                request_quit();
            }
        }
        if (renderer_ready_) renderer_.prepare(cmd, camera_, width, height);

        SDL_GPUColorTargetInfo info{};
        info.texture = target;
        info.clear_color = top_down ? SDL_FColor{0.07f, 0.09f, 0.10f, 1.0f} : SDL_FColor{0.53f, 0.74f, 0.92f, 1.0f};
        info.load_op = SDL_GPU_LOADOP_CLEAR;
        info.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &info, 1, nullptr);
        if (renderer_ready_) renderer_.draw(cmd, pass);
        SDL_EndGPURenderPass(pass);
    }

    void on_shutdown() override {
        const WorldStats ws = world_->stats();
        FORGE_INFO("frames %llu: World::update avg %.3f ms, worst %.3f ms", static_cast<unsigned long long>(frames_),
                   frames_ ? update_ms_sum_ / static_cast<f64>(frames_) : 0.0, update_ms_worst_);
        FORGE_INFO("chunks: %llu generated, %u in memory, %u changed and stored", static_cast<unsigned long long>(ws.generated),
                   ws.resident, ws.stored_edits);
        const FrameStats& s = frame_stats();
        FORGE_INFO("frame avg %.3f ms, worst %.3f ms (last %u frames)", s.avg_ms, s.worst_ms, FrameStats::kWindow);
        renderer_.shutdown();
        world_.reset();
    }

private:
    void create_world() {
        WorldDesc desc;
        desc.bounds = {0, 0, kWorldChunks, kWorldChunks};
        world_ = std::make_unique<World>(desc, make_generator(top_down));
        camera_.x = kWorldTiles / 2;
        camera_.y = top_down ? kWorldTiles / 2 : kSurfaceY;
        camera_.zoom = 8.0f;
    }

    void switch_genre(bool to_top_down) {
        renderer_.shutdown();
        renderer_ready_ = false;
        world_.reset();
        top_down = to_top_down;
        create_world();
    }

    u32 expected_visible() const {
        const Rect c = chunks_of(camera_.visible_tiles(width_, height_)).clipped(world_->bounds());
        return c.empty() ? 0 : static_cast<u32>((c.x1 - c.x0) * (c.y1 - c.y0));
    }

    void zoom_toward(f32 zoom, f32 sx, f32 sy) {
        zoom = std::clamp(zoom, 0.5f, 64.0f);
        f64 before_x, before_y, after_x, after_y;
        camera_.screen_to_tile(sx, sy, width_, height_, before_x, before_y);
        camera_.zoom = zoom;
        camera_.screen_to_tile(sx, sy, width_, height_, after_x, after_y);
        camera_.x += before_x - after_x;
        camera_.y += before_y - after_y;
    }

    void handle_input(f64 dt) {
        const bool* keys = SDL_GetKeyboardState(nullptr);
        f64 dx = 0, dy = 0;
        if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT]) dx -= 1;
        if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) dx += 1;
        if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]) dy -= 1;
        if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN]) dy += 1;
        // Screen-space speed: the same feel at every zoom.
        const f64 speed = (keys[SDL_SCANCODE_LSHIFT] ? 4000.0 : 1000.0) / camera_.zoom;
        camera_.x = std::clamp(camera_.x + dx * speed * dt, 0.0, static_cast<f64>(kWorldTiles));
        camera_.y = std::clamp(camera_.y + dy * speed * dt, 0.0, static_cast<f64>(kWorldTiles));

        f32 mx = 0, my = 0;
        const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&mx, &my);
        const f32 density = SDL_GetWindowPixelDensity(window());
        mouse_x_ = mx * density;
        mouse_y_ = my * density;
        const bool dig = buttons & SDL_BUTTON_LMASK;
        const bool build = buttons & SDL_BUTTON_RMASK;
        if (!dig && !build) return;
        f64 tx, ty;
        camera_.screen_to_tile(mouse_x_, mouse_y_, width_, height_, tx, ty);
        const TileId put = dig ? TileAir : (top_down ? TileRock : TileStone);
        const i32 cx = static_cast<i32>(std::floor(tx)), cy = static_cast<i32>(std::floor(ty));
        constexpr i32 kBrush = 2;
        for (i32 y = -kBrush; y <= kBrush; ++y)
            for (i32 x = -kBrush; x <= kBrush; ++x)
                if (x * x + y * y <= kBrush * kBrush + 1) world_->set_tile(1, cx + x, cy + y, put);
    }

    std::unique_ptr<World> world_;
    render::TilemapRenderer renderer_;
    bool renderer_ready_ = false;
    std::vector<u8> atlas_;
    Camera2D camera_;
    u32 width_ = 1600, height_ = 900;
    f32 mouse_x_ = 0, mouse_y_ = 0;
    f64 fly_dx_ = 0.8, fly_dy_ = 0.6;
    u64 frames_ = 0;
    u64 next_tick_ns_ = 0;
    f64 update_ms_sum_ = 0, update_ms_worst_ = 0;
};

// Renders a few frames into an offscreen image with no window and saves it:
// checks the whole path (streaming, uploads, shaders, edits) on any machine
// with a GPU driver, including software Vulkan on CI.
int screenshot(const char* path, bool top_down) {
    jobs::init();
    int result = 1;
    // The GPU device needs the video subsystem even without a window; the
    // offscreen driver works on machines with no display at all.
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            FORGE_ERROR("SDL_Init failed: %s", SDL_GetError());
            jobs::shutdown();
            return 1;
        }
    }
    SDL_GPUDevice* device = SDL_CreateGPUDevice(render::supported_shader_formats(), true, nullptr);
    if (!device) {
        FORGE_ERROR("no GPU device: %s", SDL_GetError());
        SDL_Quit();
        jobs::shutdown();
        return 1;
    }
    FORGE_INFO("GPU backend: %s", SDL_GetGPUDeviceDriver(device));
    constexpr u32 kW = 1280, kH = 720;
    SDL_GPUTextureCreateInfo tinfo{};
    tinfo.type = SDL_GPU_TEXTURETYPE_2D;
    tinfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tinfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tinfo.width = kW;
    tinfo.height = kH;
    tinfo.layer_count_or_depth = 1;
    tinfo.num_levels = 1;
    SDL_GPUTexture* target = SDL_CreateGPUTexture(device, &tinfo);

    WorldDesc desc;
    desc.bounds = {0, 0, kWorldChunks, kWorldChunks};
    {
        World world(desc, make_generator(top_down));
        render::TilemapRenderer renderer;
        const std::vector<u8> atlas = make_atlas();
        Camera2D camera;
        camera.x = kWorldTiles / 2 + 0.5;
        camera.y = top_down ? kWorldTiles / 2 : kSurfaceY + 10;
        camera.zoom = top_down ? 8.0f : 4.0f;
        if (target && renderer.init(device, tinfo.format, world, {atlas.data(), kCellPx, kCells})) {
            if (top_down) renderer.set_layer_tint(0, Color{});
            for (int frame = 0; frame < 8; ++frame) {
                world.update(camera.visible_tiles(kW, kH));
                world.finish_loading();
                if (frame == 4) {
                    // Dig a tunnel through the middle: edits must reach the GPU.
                    const i32 y0 = static_cast<i32>(camera.y);
                    for (i32 x = -150; x < 150; ++x)
                        for (i32 y = -4; y <= 4; ++y) world.set_tile(1, static_cast<i32>(camera.x) + x, y0 + y, TileAir);
                }
                SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
                renderer.prepare(cmd, camera, kW, kH);
                SDL_GPUColorTargetInfo info{};
                info.texture = target;
                info.clear_color = top_down ? SDL_FColor{0.07f, 0.09f, 0.10f, 1.0f} : SDL_FColor{0.53f, 0.74f, 0.92f, 1.0f};
                info.load_op = SDL_GPU_LOADOP_CLEAR;
                info.store_op = SDL_GPU_STOREOP_STORE;
                SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &info, 1, nullptr);
                renderer.draw(cmd, pass);
                SDL_EndGPURenderPass(pass);
                SDL_SubmitGPUCommandBuffer(cmd);
            }
            const render::TilemapStats& rs = renderer.stats();
            FORGE_INFO("drawn %u of %u visible chunks, %u slots used", rs.drawn_chunks, rs.visible_chunks, rs.slots_used);

            SDL_GPUTransferBufferCreateInfo tb_info{};
            tb_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
            tb_info.size = kW * kH * 4;
            SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device, &tb_info);
            SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTextureRegion src{};
            src.texture = target;
            src.w = kW;
            src.h = kH;
            src.d = 1;
            SDL_GPUTextureTransferInfo dst{};
            dst.transfer_buffer = tb;
            SDL_DownloadFromGPUTexture(copy, &src, &dst);
            SDL_EndGPUCopyPass(copy);
            SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
            SDL_WaitForGPUFences(device, true, &fence, 1);
            SDL_ReleaseGPUFence(device, fence);
            const auto* pixels = static_cast<const u8*>(SDL_MapGPUTransferBuffer(device, tb, false));
            if (pixels && stbi_write_png(path, kW, kH, 4, pixels, kW * 4)) {
                FORGE_INFO("saved %s", path);
                result = rs.drawn_chunks == rs.visible_chunks && rs.drawn_chunks > 0 ? 0 : 1;
            }
            SDL_UnmapGPUTransferBuffer(device, tb);
            SDL_ReleaseGPUTransferBuffer(device, tb);
        }
        renderer.shutdown();
    }
    if (target) SDL_ReleaseGPUTexture(device, target);
    SDL_DestroyGPUDevice(device);
    SDL_Quit();
    jobs::shutdown();
    return result;
}

} // namespace

int main(int argc, char** argv) {
    AppConfig config;
    config.title = "Forge World";
    config.shader_formats = render::supported_shader_formats();
    WorldDemo app;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--headless") == 0) {
            config.headless = true;
            app.headless = true;
            if (i + 1 < argc) config.max_frames = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--no-vsync") == 0) {
            config.vsync = false;
        } else if (std::strcmp(argv[i], "--topdown") == 0) {
            app.top_down = true;
        } else if (std::strcmp(argv[i], "--fly") == 0) {
            app.auto_fly = true;
        }
    }
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--screenshot") == 0) return screenshot(argv[i + 1], app.top_down);
    return app.run(config);
}
