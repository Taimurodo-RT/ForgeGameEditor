// Render demo: a top-down world crowded with critters and falling leaves, all
// drawn as sprites (one draw call for all of them), with fireflies as GPU
// particles drifting over them.
//
//   WASD / arrows   move (Shift: faster)      mouse wheel   zoom
//   Space           pause the critters        left mouse    sparks
//   N               night: some critters carry lamps
//
//   forge_render_demo [--sprites N] [--particles N] [--day] [--no-vsync]
//   forge_render_demo --bench FRAMES [--sprites N] [--particles N] [--day]   offscreen 1920×1080, timings
//   forge_render_demo --screenshot out.png [--sprites N] [--zoom pixels_per_tile]

#include "demo_art.h"
#include "forge/core/jobs.h"
#include "forge/core/log.h"
#include "forge/core/profile.h"
#include "forge/core/time.h"
#include "forge/platform/app.h"
#include "forge/render/gpu.h"
#include "forge/render/lighting.h"
#include "forge/render/offscreen.h"
#include "forge/render/particles.h"
#include "forge/render/sprite_renderer.h"
#include "forge/render/tilemap_renderer.h"
#include "forge/world/generators.h"
#include "forge/world/world.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace forge;
using namespace forge::world;
using render::Camera2D;
using render::Sprite;

namespace {

constexpr i32 kWorldChunks = 1024;
constexpr f64 kAreaX = 32768, kAreaY = 32768; // corner of the crowded area, tiles

u32 hash32(u32 a, u32 b) {
    u32 h = a * 374761393u + b * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

f32 unit(u32 h) { return static_cast<f32>(h & 0xffffff) / 16777216.0f; }

// Many small things moving on their own: critters walk and bounce off the
// area's edges, every 16th one is a leaf that drifts and spins above them.
class Crowd {
public:
    void init(u32 count, f32 width, f32 height) {
        count_ = count;
        w_ = width;
        h_ = height;
        x_.resize(count);
        y_.resize(count);
        vx_.resize(count);
        vy_.resize(count);
        phase_.resize(count);
        for (u32 i = 0; i < count; ++i) {
            x_[i] = unit(hash32(i, 1)) * width;
            y_[i] = unit(hash32(i, 2)) * height;
            const f32 a = unit(hash32(i, 3)) * 6.2831853f, speed = 0.5f + unit(hash32(i, 4)) * 2.5f;
            vx_[i] = std::cos(a) * speed;
            vy_[i] = std::sin(a) * speed;
            phase_[i] = unit(hash32(i, 5)) * 100.0f;
        }
    }

    // Moves everything and writes the sprites, in parallel blocks. view_top and
    // view_height (tiles, relative to the area corner) give the draw order.
    void update(f32 dt, render::SpriteBatch& batch, f32 view_top, f32 view_height) {
        FORGE_ZONE_N("Crowd update");
        constexpr u32 kBlock = 8192;
        const u32 blocks = (count_ + kBlock - 1) / kBlock;
        jobs::parallel_for(blocks, 1, [&](u32 b, u32 e) {
            for (u32 k = b; k < e; ++k) {
                const u32 begin = k * kBlock, end = std::min(count_, begin + kBlock);
                Sprite* out = batch.push(end - begin);
                for (u32 i = begin; i < end; ++i) {
                    f32 x = x_[i] + vx_[i] * dt, y = y_[i] + vy_[i] * dt;
                    if (x < 0 || x > w_) vx_[i] = -vx_[i], x = std::clamp(x, 0.0f, w_);
                    if (y < 0 || y > h_) vy_[i] = -vy_[i], y = std::clamp(y, 0.0f, h_);
                    x_[i] = x, y_[i] = y;
                    const f32 phase = phase_[i] += dt;
                    if (!out) continue;
                    Sprite& s = out[i - begin];
                    s.x = x;
                    s.y = y;
                    if ((i & 15) == 0) {
                        s.w = s.h = 0.9f;
                        s.angle = phase * 2.0f;
                        s.frame = demo::kFrameLeaf;
                        s.color = 0xffffffffu;
                        s.order = render::draw_order(2, y - view_top, view_height);
                    } else {
                        const u32 kind = i % demo::kCritterKinds;
                        s.w = vx_[i] < 0 ? -1.0f : 1.0f; // face where it walks
                        s.h = 1.0f;
                        s.angle = 0;
                        s.frame = kind * 2 + (static_cast<u32>(phase * 6.0f) & 1u);
                        s.color = 0xffffffffu;
                        // Feet at the bottom edge decide who stands in front.
                        s.order = render::draw_order(1, y + 0.5f - view_top, view_height);
                    }
                }
            }
        });
    }

    u32 count() const { return count_; }
    f32 x(u32 i) const { return x_[i]; }
    f32 y(u32 i) const { return y_[i]; }

private:
    u32 count_ = 0;
    f32 w_ = 0, h_ = 0;
    std::vector<f32> x_, y_, vx_, vy_, phase_;
};

struct FrameTimes {
    f64 crowd_ms = 0, sprites_ms = 0, tiles_ms = 0, particles_ms = 0, light_ms = 0;
};

constexpr f32 kTilesPerLamp = 150; // at night, about one lamp per this many tiles of the crowd's area

// Fireflies: a steady stream that keeps about `alive` particles in the air.
constexpr f32 kFireflyLifeMin = 2.0f, kFireflyLifeMax = 4.0f;

// Everything the demo draws; used by the window, the benchmark and the screenshot.
class Scene {
public:
    bool init(SDL_GPUDevice* device, SDL_GPUTextureFormat format, u32 sprites, u32 particles) {
        WorldDesc desc;
        desc.bounds = {0, 0, kWorldChunks, kWorldChunks};
        world_ = std::make_unique<World>(desc, std::make_shared<TopDownGenerator>(2026));
        atlas_ = demo::make_tile_atlas();
        sheet_ = demo::make_sprite_sheet();
        if (!tiles_.init(device, format, *world_, {atlas_.data(), demo::kTileCellPx, demo::kTileCells})) return false;
        tiles_.set_layer_tint(0, Color{});
        if (!sprites_.init(device, format, sheet_.sheet(), std::max(sprites, 1024u))) return false;
        render::ParticleLook fireflies;
        fireflies.gravity_y = -0.3f; // drift up the screen
        fireflies.drag = 0.6f;
        fireflies.additive = true;
        particle_target_ = particles;
        if (particles > 0 && !fireflies_.init(device, format, sheet_.sheet(), particles, kAreaX, kAreaY, fireflies))
            return false;
        render::ParticleLook sparks;
        sparks.gravity_y = 6.0f;
        sparks.drag = 1.5f;
        sparks.additive = true;
        if (!sparks_.init(device, format, sheet_.sheet(), 65536, kAreaX, kAreaY, sparks)) return false;
        if (!lights_.init(device, format)) return false;
        set_night(night_);
        // Area for the crowd: about 7 critters per tile on a 1080p screen at zoom 4.
        const f32 side = std::sqrt(static_cast<f32>(sprites) / 7.0f);
        area_w_ = std::max(64.0f, side * 16.0f / 9.0f);
        area_h_ = std::max(36.0f, side * 9.0f / 16.0f);
        crowd_.init(sprites, area_w_, area_h_);
        lamp_every_ = std::max(1u, static_cast<u32>(static_cast<f32>(sprites) / (area_w_ * area_h_ / kTilesPerLamp)));
        capacity_ = sprites;
        camera.x = kAreaX + area_w_ * 0.5;
        camera.y = kAreaY + area_h_ * 0.5;
        camera.zoom = 4.0f;
        return true;
    }

    void update(f32 dt, u32 width, u32 height, bool paused) {
        u64 t0 = time_now_ns();
        world_->update(camera.visible_tiles(width, height));
        batch_.begin(kAreaX, kAreaY, capacity_);
        const f32 view_h = static_cast<f32>(height) / camera.zoom;
        const f32 view_top = static_cast<f32>(camera.y - kAreaY) - view_h * 0.5f;
        crowd_.update(paused ? 0.0f : dt, batch_, view_top, view_h);
        times.crowd_ms = ns_to_ms(time_now_ns() - t0);
        if (particle_target_ > 0 && !paused) emit_fireflies(dt);
        dt_ = paused ? 0.0f : dt;
    }

    void set_night(bool night) {
        night_ = night;
        lights_.set_rules(demo::top_down_light_rules(night));
    }
    bool night() const { return night_; }

    // A burst of sparks at a world position.
    void burst(f64 x, f64 y) {
        render::ParticleEmit e;
        e.x = x;
        e.y = y;
        e.count = 3000;
        e.speed_min = 2;
        e.speed_max = 14;
        e.life_min = 0.4f;
        e.life_max = 1.4f;
        e.size_start = 0.5f;
        e.size_end = 0.1f;
        e.color = render::pack_color(255, 180, 80);
        e.frame = demo::kFrameGlow;
        sparks_.emit(e);
    }

    void render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, u32 width, u32 height) {
        u64 t0 = time_now_ns();
        tiles_.prepare(cmd, camera, width, height);
        const u64 t1 = time_now_ns();
        sprites_.prepare(cmd, batch_, camera, width, height);
        const u64 t2 = time_now_ns();
        if (particle_target_ > 0) fireflies_.simulate(cmd, dt_);
        sparks_.simulate(cmd, dt_);
        const u64 t3 = time_now_ns();
        if (night_) {
            const u32 lamp_colors[3][3] = {{220, 170, 100}, {120, 200, 255}, {255, 120, 200}};
            for (u32 i = 0; i < crowd_.count(); i += lamp_every_) {
                const u32* c = lamp_colors[(i / lamp_every_) % 3];
                lights_.add({kAreaX + crowd_.x(i), kAreaY + crowd_.y(i), static_cast<f32>(c[0]) / 240.0f,
                             static_cast<f32>(c[1]) / 240.0f, static_cast<f32>(c[2]) / 240.0f});
            }
            lights_.prepare(cmd, *world_, camera, width, height);
        }
        const u64 t4 = time_now_ns();
        times.tiles_ms = ns_to_ms(t1 - t0);
        times.sprites_ms = ns_to_ms(t2 - t1);
        times.particles_ms = ns_to_ms(t3 - t2);
        times.light_ms = ns_to_ms(t4 - t3);

        SDL_GPUColorTargetInfo info{};
        info.texture = target;
        info.clear_color = SDL_FColor{0.07f, 0.09f, 0.10f, 1.0f};
        info.load_op = SDL_GPU_LOADOP_CLEAR;
        info.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &info, 1, nullptr);
        tiles_.draw(cmd, pass);
        sprites_.draw(cmd, pass);
        if (night_) lights_.draw(cmd, pass); // glowing particles stay bright: drawn after
        if (particle_target_ > 0) fireflies_.draw(cmd, pass, camera, width, height);
        sparks_.draw(cmd, pass, camera, width, height);
        SDL_EndGPURenderPass(pass);
    }

    void finish_loading() { world_->finish_loading(); }
    void shutdown() {
        lights_.shutdown();
        sparks_.shutdown();
        fireflies_.shutdown();
        sprites_.shutdown();
        tiles_.shutdown();
        world_.reset();
    }
    const render::SpriteStats& sprite_stats() const { return sprites_.stats(); }
    const render::ParticleStats& particle_stats() const { return fireflies_.stats(); }
    const render::LightStats& light_stats() const { return lights_.stats(); }

    Camera2D camera;
    FrameTimes times;

private:
    // Spawns this frame's share so that about particle_target_ are alive.
    void emit_fireflies(f32 dt) {
        constexpr u32 kSpots = 64;
        const f32 per_second = static_cast<f32>(particle_target_) / ((kFireflyLifeMin + kFireflyLifeMax) * 0.5f);
        spawn_debt_ += per_second * dt;
        const u32 total = static_cast<u32>(spawn_debt_);
        spawn_debt_ -= static_cast<f32>(total);
        const u32 colors[4] = {render::pack_color(255, 230, 120, 150), render::pack_color(180, 255, 140, 150),
                               render::pack_color(255, 190, 90, 150), render::pack_color(150, 220, 255, 150)};
        for (u32 k = 0; k < kSpots; ++k) {
            const u32 h = hash32(++emit_seq_, 77);
            render::ParticleEmit e;
            e.x = kAreaX + unit(h) * area_w_;
            e.y = kAreaY + unit(hash32(h, 1)) * area_h_;
            e.count = total / kSpots + (k < total % kSpots ? 1 : 0);
            e.radius = std::max(area_w_, area_h_) / 8.0f;
            e.speed_min = 0.2f;
            e.speed_max = 1.5f;
            e.life_min = kFireflyLifeMin;
            e.life_max = kFireflyLifeMax;
            e.size_start = 0.6f;
            e.size_end = 0.15f;
            e.color = colors[h & 3];
            e.frame = demo::kFrameGlow;
            fireflies_.emit(e);
        }
    }

    std::unique_ptr<World> world_;
    std::vector<u8> atlas_;
    demo::SheetImage sheet_;
    render::TilemapRenderer tiles_;
    render::SpriteRenderer sprites_;
    render::SpriteBatch batch_;
    render::ParticleSystem fireflies_, sparks_;
    render::LightRenderer lights_;
    bool night_ = true;
    u32 lamp_every_ = 1;
    u32 particle_target_ = 0;
    f32 spawn_debt_ = 0, dt_ = 0;
    u32 emit_seq_ = 0;
    Crowd crowd_;
    u32 capacity_ = 0;
    f32 area_w_ = 0, area_h_ = 0;
};

class RenderDemo final : public App {
public:
    u32 sprite_count = 1'000'000;
    u32 particle_count = 1'000'000;
    bool night = true;

    void on_render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, u32 width, u32 height) override {
        width_ = width;
        height_ = height;
        if (!ready_ && !failed_) {
            ready_ = scene_.init(gpu(), swapchain_format(), sprite_count, particle_count);
            if (ready_) scene_.set_night(night);
            failed_ = !ready_;
            if (failed_) request_quit();
        }
        if (!ready_) return;
        scene_.update(dt_, width, height, paused_);
        scene_.render(cmd, target, width, height);
        if (frame_stats().frame % 30 == 0) {
            const render::SpriteStats& s = scene_.sprite_stats();
            char text[256];
            std::snprintf(text, sizeof(text), "sprites %u of %u on screen  particles %u  crowd %.2f ms  sort+upload %.2f ms  zoom %.2g%s",
                          s.drawn, s.submitted, scene_.particle_stats().slots_used, scene_.times.crowd_ms,
                          scene_.times.sprites_ms, static_cast<f64>(scene_.camera.zoom), paused_ ? "  [paused]" : "");
            set_status(text);
        }
    }

    void on_frame(f64 dt) override {
        dt_ = static_cast<f32>(std::min(dt, 0.1));
        const bool* keys = SDL_GetKeyboardState(nullptr);
        f64 dx = 0, dy = 0;
        if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT]) dx -= 1;
        if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) dx += 1;
        if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]) dy -= 1;
        if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN]) dy += 1;
        const f64 speed = (keys[SDL_SCANCODE_LSHIFT] ? 4000.0 : 1000.0) / scene_.camera.zoom;
        scene_.camera.x += dx * speed * dt_;
        scene_.camera.y += dy * speed * dt_;

        f32 mx = 0, my = 0;
        if (ready_ && (SDL_GetMouseState(&mx, &my) & SDL_BUTTON_LMASK)) {
            const f32 density = SDL_GetWindowPixelDensity(window());
            f64 tx, ty;
            scene_.camera.screen_to_tile(mx * density, my * density, width_, height_, tx, ty);
            scene_.burst(tx, ty);
        }
    }

    void on_event(const SDL_Event& e) override {
        if (e.type == SDL_EVENT_MOUSE_WHEEL)
            scene_.camera.zoom = std::clamp(scene_.camera.zoom * (e.wheel.y > 0 ? 1.25f : 0.8f), 0.25f, 64.0f);
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_SPACE) paused_ = !paused_;
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_N && ready_) scene_.set_night(!scene_.night());
    }

    void on_shutdown() override {
        const FrameStats& s = frame_stats();
        FORGE_INFO("frame avg %.3f ms, worst %.3f ms (last %u frames)", s.avg_ms, s.worst_ms, FrameStats::kWindow);
        scene_.shutdown();
    }

private:
    Scene scene_;
    bool ready_ = false, failed_ = false, paused_ = false;
    f32 dt_ = 0;
    u32 width_ = 0, height_ = 0;
};

struct Series {
    std::vector<f64> v;
    void print(const char* name) {
        if (v.empty()) return;
        std::sort(v.begin(), v.end());
        f64 sum = 0;
        for (f64 x : v) sum += x;
        FORGE_INFO("%-22s avg %7.3f ms   p99 %7.3f ms   worst %7.3f ms", name, sum / static_cast<f64>(v.size()),
                   v[v.size() * 99 / 100], v.back());
    }
};

// Offscreen: renders frames at 1920×1080 and waits for the GPU after each,
// so "frame" is the full CPU + GPU time without a monitor's limit.
int run_offscreen(u32 sprites, u32 particles, bool night, u32 frames, const char* screenshot, f32 zoom) {
    jobs::init();
    SDL_GPUDevice* device = render::create_offscreen_device();
    if (!device) {
        jobs::shutdown();
        return 1;
    }
    const u32 w = screenshot ? 1280 : 1920, h = screenshot ? 720 : 1080;
    SDL_GPUTexture* target = render::create_render_target(device, w, h);
    int result = 1;
    {
        Scene scene;
        if (target && scene.init(device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, sprites, particles)) {
            scene.set_night(night);
            if (zoom > 0) scene.camera.zoom = zoom;
            // Long enough for the particle stream to fill up (life is 2-4 s).
            const u32 warmup = particles > 0 ? 250 : 20;
            Series crowd, sort, tiles, parts, light, frame;
            for (u32 f = 0; f < warmup + frames; ++f) {
                const u64 t0 = time_now_ns();
                scene.update(1.0f / 60.0f, w, h, false);
                if (f < 20) scene.finish_loading();
                if (f == warmup - 30) scene.burst(scene.camera.x, scene.camera.y);
                SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
                scene.render(cmd, target, w, h);
                SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
                SDL_WaitForGPUFences(device, true, &fence, 1);
                SDL_ReleaseGPUFence(device, fence);
                if (f < warmup) continue;
                crowd.v.push_back(scene.times.crowd_ms);
                sort.v.push_back(scene.times.sprites_ms);
                tiles.v.push_back(scene.times.tiles_ms);
                parts.v.push_back(scene.times.particles_ms);
                light.v.push_back(scene.times.light_ms);
                frame.v.push_back(ns_to_ms(time_now_ns() - t0));
            }
            const render::SpriteStats& s = scene.sprite_stats();
            FORGE_INFO("%u sprites submitted, %u on screen, %u dropped; %u particle slots in use, %u spawned per frame; "
                       "%u threads, %ux%u",
                       s.submitted, s.drawn, s.dropped, scene.particle_stats().slots_used,
                       scene.particle_stats().spawned_last, jobs::thread_count(), w, h);
            crowd.print("move + write sprites");
            sort.print("cull + sort + upload");
            tiles.print("tiles");
            parts.print("particles (CPU part)");
            if (night) {
                const render::LightStats& ls = scene.light_stats();
                FORGE_INFO("night: %u lamps, light grid %ux%u cells of %u tile(s)", ls.lights, ls.cells_w, ls.cells_h, ls.step);
                light.print("light grid (CPU part)");
            }
            frame.print("frame (CPU + GPU)");
            result = s.drawn > 0 && s.dropped == 0 ? 0 : 1;
            if (screenshot && !render::save_png(device, target, w, h, screenshot)) result = 1;
        }
        scene.shutdown();
    }
    if (target) SDL_ReleaseGPUTexture(device, target);
    render::destroy_offscreen_device(device);
    jobs::shutdown();
    return result;
}

} // namespace

int main(int argc, char** argv) {
    AppConfig config;
    config.title = "Forge Render";
    config.shader_formats = render::supported_shader_formats();
    RenderDemo app;
    u32 bench_frames = 0;
    const char* screenshot = nullptr;
    f32 zoom = 0;
    for (int i = 1; i < argc; ++i) {
        const bool has_value = i + 1 < argc;
        if (std::strcmp(argv[i], "--no-vsync") == 0) config.vsync = false;
        else if (std::strcmp(argv[i], "--sprites") == 0 && has_value) app.sprite_count = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--day") == 0) app.night = false;
        else if (std::strcmp(argv[i], "--particles") == 0 && has_value) app.particle_count = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--bench") == 0 && has_value) bench_frames = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--screenshot") == 0 && has_value) screenshot = argv[++i];
        else if (std::strcmp(argv[i], "--zoom") == 0 && has_value) zoom = std::strtof(argv[++i], nullptr);
    }
    if (screenshot) return run_offscreen(app.sprite_count, app.particle_count, app.night, 3, screenshot, zoom);
    if (bench_frames > 0) return run_offscreen(app.sprite_count, app.particle_count, app.night, bench_frames, nullptr, zoom);
    return app.run(config);
}
