// Factory demo: 10 000 production blocks (like Factorio) spread over a
// top-down world. Every one of them keeps working whether it is on screen
// or not: drills mine ore and coal, belts carry it, pumps and boilers make
// steam, engines power the smelters and assemblers through their poles,
// and gears pile up in the chests at the end.
//
//   WASD / arrows   move the camera (Shift: faster)    mouse wheel   zoom
//   1 2 3           left mouse builds a belt / pipe / pole
//   R               turn the belt to build              right mouse   remove
//   F               fly to a random block far away      Space         pause
//
//   forge_factory_demo [--blocks N] [--no-vsync]
//   forge_factory_demo --screenshot out.png [--blocks N]   offscreen, after 40 s of play

#include "demo_art.h"
#include "forge/core/jobs.h"
#include "forge/core/log.h"
#include "forge/core/profile.h"
#include "forge/core/time.h"
#include "forge/platform/app.h"
#include "forge/render/gpu.h"
#include "forge/render/offscreen.h"
#include "forge/render/sprite_renderer.h"
#include "forge/render/tilemap_renderer.h"
#include "forge/sim/clock.h"
#include "forge/sim/factory.h"
#include "forge/sim/factory_sample.h"
#include "forge/world/generators.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

using namespace forge;
using namespace forge::world;
using namespace forge::sim;
using render::Camera2D;
using render::Sprite;

namespace {

constexpr i32 kGapX = 4, kGapY = 3; // tiles between blocks
constexpr u32 kMaxSprites = 1u << 18;

u32 frame_of_item(ItemId item) {
    switch (item) {
        case kItemOre: return demo::kFrameOre;
        case kItemPlate: return demo::kFramePlate;
        case kItemGear: return demo::kFrameGear;
        default: return demo::kFrameCoal;
    }
}

class Demo {
public:
    bool init(SDL_GPUDevice* device, SDL_GPUTextureFormat format, u32 blocks) {
        WorldDesc wd;
        wd.layer_count = 1; // ground only: the factory draws itself
        world_ = std::make_unique<World>(wd, std::make_shared<TopDownGenerator>(2026));
        per_row_ = std::max(1u, static_cast<u32>(std::sqrt(static_cast<f64>(blocks))));
        blocks_.reserve(blocks);
        for (u32 i = 0; i < blocks; ++i)
            blocks_.push_back(build_sample_block(factory_, static_cast<i32>(i % per_row_) * (kSampleBlockW + kGapX),
                                                 static_cast<i32>(i / per_row_) * (kSampleBlockH + kGapY)));
        factory_.tick(0);

        atlas_ = demo::make_tile_atlas();
        sheet_ = demo::make_sprite_sheet();
        if (!tiles_.init(device, format, *world_, {atlas_.data(), demo::kTileCellPx, demo::kTileCells})) return false;
        if (!sprites_.init(device, format, sheet_.sheet(), kMaxSprites)) return false;
        // Start over the middle of the factory.
        camera.x = per_row_ / 2 * (kSampleBlockW + kGapX) + kSampleBlockW * 0.5;
        camera.y = per_row_ / 2 * (kSampleBlockH + kGapY) + kSampleBlockH * 0.5;
        camera.zoom = 24;
        return true;
    }

    void build(u32 tool, f64 x, f64 y) {
        const i32 tx = static_cast<i32>(std::floor(x)), ty = static_cast<i32>(std::floor(y));
        if (tool == 0) factory_.place_belt(tx, ty, belt_dir);
        else if (tool == 1) factory_.place_pipe(tx, ty);
        else factory_.place_pole(tx, ty);
    }

    void remove(f64 x, f64 y) {
        const i32 tx = static_cast<i32>(std::floor(x)), ty = static_cast<i32>(std::floor(y));
        if (factory_.remove_belt(tx, ty) || factory_.remove_pipe(tx, ty) || factory_.remove_pole(tx, ty)) return;
        const MachineId m = factory_.machine_at(tx, ty);
        if (m != kNoMachine) factory_.remove_machine(m);
    }

    void fly_somewhere() {
        const u32 i = static_cast<u32>(time_now_ns() / 1000) % static_cast<u32>(blocks_.size());
        camera.x = (i % per_row_) * (kSampleBlockW + kGapX) + kSampleBlockW * 0.5;
        camera.y = (i / per_row_) * (kSampleBlockH + kGapY) + kSampleBlockH * 0.5;
    }

    void update(f64 dt, u32 width, u32 height, bool paused) {
        FORGE_ZONE_N("Demo update");
        const Rect view = camera.visible_tiles(width, height);
        world_->update(view);
        const u32 ticks = clock_.advance(paused ? 0.0 : dt);
        sim_ms = 0;
        for (u32 i = 0; i < ticks; ++i) {
            factory_.tick(clock_.step());
            sim_ms += factory_.stats().total_ms;
        }

        batch_.begin(camera.x, camera.y, kMaxSprites);
        auto push = [&](f32 x, f32 y, f32 w, f32 h, u32 frame, u32 order, f32 angle = 0, u32 color = 0xffffffffu) {
            Sprite* s = batch_.push(1);
            if (!s) return;
            s->x = static_cast<f32>(x - camera.x);
            s->y = static_cast<f32>(y - camera.y);
            s->w = w;
            s->h = h;
            s->angle = angle;
            s->frame = frame;
            s->color = color;
            s->order = order;
        };
        // Belts, pipes and poles in view, tile by tile.
        for (i32 y = view.y0; y < view.y1; ++y)
            for (i32 x = view.x0; x < view.x1; ++x) {
                const f32 cx = static_cast<f32>(x) + 0.5f, cy = static_cast<f32>(y) + 0.5f;
                Dir dir;
                FluidId fluid;
                if (factory_.belt_at(x, y, &dir)) {
                    push(cx, cy, 1, 1, demo::kFrameBelt, 0, static_cast<f32>(static_cast<u8>(dir)) * 1.5707963f);
                } else if (factory_.pipe_at(x, y, &fluid)) {
                    const u32 tint = fluid == kFluidWater   ? render::pack_color(150, 200, 255)
                                     : fluid == kFluidSteam ? render::pack_color(235, 235, 235)
                                                            : 0xffffffffu;
                    push(cx, cy, 1, 1, demo::kFramePipe, 0, 0, tint);
                } else if (factory_.pole_at(x, y)) {
                    push(cx, cy - 0.3f, 1, 1.6f, demo::kFramePole, 3);
                }
            }
        // Items on belts.
        factory_.for_each_item(view, [&](f32 x, f32 y, ItemId item) { push(x, y, 0.45f, 0.45f, frame_of_item(item), 1); });
        // Machines, dimmed while they wait for power.
        factory_.for_each_machine([&](MachineId id, const MachineDesc& d) {
            if (d.x + d.w <= view.x0 || d.x >= view.x1 || d.y + d.h <= view.y0 || d.y >= view.y1) return;
            u32 frame = demo::kFrameChest;
            if (d.kind == MachineKind::Generator) frame = demo::kFrameEngine;
            else if (d.kind == MachineKind::Crafter) {
                if (d.recipe.fluid_out == kFluidWater) frame = demo::kFramePump;
                else if (d.recipe.fluid_out == kFluidSteam) frame = demo::kFrameBoiler;
                else if (d.recipe.in[0].count == 0) frame = demo::kFrameDrill;
                else if (d.recipe.out.item == kItemPlate) frame = demo::kFrameFurnace;
                else frame = demo::kFrameAssembler;
            }
            const MachineState s = factory_.machine(id);
            const bool powered = d.power <= 0 || d.kind != MachineKind::Crafter || s.satisfaction > 0.5f;
            const u32 color = powered ? 0xffffffffu : render::pack_color(150, 150, 170);
            push(static_cast<f32>(d.x) + d.w * 0.5f, static_cast<f32>(d.y) + d.h * 0.5f, d.w, d.h, frame, 2, 0, color);
        });
    }

    void render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, u32 width, u32 height) {
        tiles_.prepare(cmd, camera, width, height);
        sprites_.prepare(cmd, batch_, camera, width, height);
        SDL_GPUColorTargetInfo info{};
        info.texture = target;
        info.clear_color = SDL_FColor{0.2f, 0.3f, 0.2f, 1.0f};
        info.load_op = SDL_GPU_LOADOP_CLEAR;
        info.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &info, 1, nullptr);
        tiles_.draw(cmd, pass);
        sprites_.draw(cmd, pass);
        SDL_EndGPURenderPass(pass);
    }

    void shutdown() {
        sprites_.shutdown();
        tiles_.shutdown();
        world_.reset();
    }

    const FactoryStats& stats() const { return factory_.stats(); }
    u64 gears() const {
        u64 n = 0;
        for (const SampleBlock& b : blocks_) n += factory_.machine(b.sink).made;
        return n;
    }

    Camera2D camera;
    Dir belt_dir = Dir::East;
    f64 sim_ms = 0;

private:
    std::unique_ptr<World> world_;
    Factory factory_;
    SimClock clock_;
    std::vector<SampleBlock> blocks_;
    u32 per_row_ = 1;
    std::vector<u8> atlas_;
    demo::SheetImage sheet_;
    render::TilemapRenderer tiles_;
    render::SpriteRenderer sprites_;
    render::SpriteBatch batch_;
};

class FactoryDemo final : public App {
public:
    u32 blocks = 10'000;

    void on_render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, u32 width, u32 height) override {
        width_ = width;
        height_ = height;
        if (!ready_ && !failed_) {
            ready_ = demo_.init(gpu(), swapchain_format(), blocks);
            failed_ = !ready_;
            if (failed_) request_quit();
        }
        if (!ready_) return;
        demo_.update(dt_, width, height, paused_);
        demo_.render(cmd, target, width, height);
        if (frame_stats().frame % 30 == 0) {
            const FactoryStats& s = demo_.stats();
            const char* tools[3] = {"belt", "pipe", "pole"};
            const char* dirs[4] = {"east", "south", "west", "north"};
            char text[256];
            std::snprintf(text, sizeof(text),
                          "tool: %s (%s)  machines %u  belts %u  items %u  power nets %u  gears %llu  factory %.2f ms%s",
                          tools[tool_], dirs[static_cast<u8>(demo_.belt_dir)], s.machines, s.belt_tiles, s.items,
                          s.power_nets, static_cast<unsigned long long>(demo_.gears()), demo_.sim_ms,
                          paused_ ? "  [paused]" : "");
            set_status(text);
        }
    }

    void on_frame(f64 dt) override {
        dt_ = std::min(dt, 0.1);
        const bool* keys = SDL_GetKeyboardState(nullptr);
        f64 dx = 0, dy = 0;
        if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT]) dx -= 1;
        if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) dx += 1;
        if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]) dy -= 1;
        if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN]) dy += 1;
        const f64 speed = (keys[SDL_SCANCODE_LSHIFT] ? 4000.0 : 800.0) / demo_.camera.zoom;
        demo_.camera.x += dx * speed * dt_;
        demo_.camera.y += dy * speed * dt_;
        f32 mx = 0, my = 0;
        const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&mx, &my);
        if (!ready_) return;
        f64 tx, ty;
        mouse_tile(mx, my, tx, ty);
        if (buttons & SDL_BUTTON_LMASK) demo_.build(tool_, tx, ty);
        if (buttons & SDL_BUTTON_RMASK) demo_.remove(tx, ty);
    }

    void on_event(const SDL_Event& e) override {
        if (e.type == SDL_EVENT_MOUSE_WHEEL)
            demo_.camera.zoom = std::clamp(demo_.camera.zoom * (e.wheel.y > 0 ? 1.25f : 0.8f), 6.0f, 64.0f);
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
            if (e.key.key == SDLK_SPACE) paused_ = !paused_;
            if (e.key.key == SDLK_R) demo_.belt_dir = static_cast<Dir>((static_cast<u8>(demo_.belt_dir) + 1) & 3);
            if (e.key.key == SDLK_F && ready_) demo_.fly_somewhere();
            if (e.key.key >= SDLK_1 && e.key.key <= SDLK_3) tool_ = static_cast<u32>(e.key.key - SDLK_1);
        }
    }

    void on_shutdown() override {
        const FrameStats& s = frame_stats();
        FORGE_INFO("frame avg %.3f ms, worst %.3f ms (last %u frames)", s.avg_ms, s.worst_ms, FrameStats::kWindow);
        demo_.shutdown();
    }

private:
    void mouse_tile(f32 mx, f32 my, f64& tx, f64& ty) {
        const f32 density = SDL_GetWindowPixelDensity(window());
        demo_.camera.screen_to_tile(mx * density, my * density, width_, height_, tx, ty);
    }

    Demo demo_;
    bool ready_ = false, failed_ = false, paused_ = false;
    u32 tool_ = 0;
    f64 dt_ = 0;
    u32 width_ = 0, height_ = 0;
};

// Offscreen: 40 seconds of play (belts fill, boilers start, gears come out),
// then a picture.
int run_screenshot(u32 blocks, const char* path) {
    jobs::init();
    SDL_GPUDevice* device = render::create_offscreen_device();
    if (!device) {
        jobs::shutdown();
        return 1;
    }
    const u32 w = 1280, h = 720;
    SDL_GPUTexture* target = render::create_render_target(device, w, h);
    int result = 1;
    {
        Demo demo;
        if (target && demo.init(device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, blocks)) {
            for (u32 f = 0; f < 60 * 40; ++f) demo.update(1.0 / 60.0, w, h, false);
            SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
            demo.render(cmd, target, w, h);
            SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
            SDL_WaitForGPUFences(device, true, &fence, 1);
            SDL_ReleaseGPUFence(device, fence);
            const FactoryStats& s = demo.stats();
            FORGE_INFO("%u machines, %u items on belts, %llu gears made; last factory tick %.3f ms", s.machines, s.items,
                       static_cast<unsigned long long>(demo.gears()), s.total_ms);
            result = render::save_png(device, target, w, h, path) ? 0 : 1;
        }
        demo.shutdown();
    }
    if (target) SDL_ReleaseGPUTexture(device, target);
    render::destroy_offscreen_device(device);
    jobs::shutdown();
    return result;
}

} // namespace

int main(int argc, char** argv) {
    AppConfig config;
    config.title = "Forge Factory";
    config.shader_formats = render::supported_shader_formats();
    FactoryDemo app;
    const char* screenshot = nullptr;
    for (int i = 1; i < argc; ++i) {
        const bool has_value = i + 1 < argc;
        if (std::strcmp(argv[i], "--no-vsync") == 0) config.vsync = false;
        else if (std::strcmp(argv[i], "--blocks") == 0 && has_value) app.blocks = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--screenshot") == 0 && has_value) screenshot = argv[++i];
    }
    if (screenshot) return run_screenshot(app.blocks, screenshot);
    return app.run(config);
}
