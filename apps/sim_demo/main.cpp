// Simulation demo: creatures living in a side-view world at a steady 60
// ticks per second. They walk, jump over steps and fall; gravity wells pull
// them in and make them glow while they are inside (trigger events).
//
//   WASD / arrows   move the camera (Shift: faster)    mouse wheel   zoom
//   left mouse      place a gravity well (click one again to remove it)
//   right mouse     dig
//   G               turn the world's gravity by 90°    Space   pause
//
// Creatures far from the camera are simulated less often, then not at all,
// then packed away with their chunk; they are back when the camera returns.
//
//   forge_sim_demo [--creatures N] [--no-vsync]
//   forge_sim_demo --screenshot out.png [--creatures N]   offscreen, after 3 s of play

#include "demo_art.h"
#include "forge/core/jobs.h"
#include "forge/core/log.h"
#include "forge/core/profile.h"
#include "forge/core/time.h"
#include "forge/platform/app.h"
#include "forge/render/gpu.h"
#include "forge/render/lighting.h"
#include "forge/render/offscreen.h"
#include "forge/render/sprite_renderer.h"
#include "forge/render/tilemap_renderer.h"
#include "forge/sim/simulation.h"
#include "forge/world/generators.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

struct DemoWalker {
    forge::f32 speed = 3;
    forge::f32 dir = 1;
    forge::u32 seed = 0;
    forge::u8 wells = 0; // gravity wells it is inside of
};
FORGE_REFLECT_DECLARE(DemoWalker)
FORGE_REFLECT(DemoWalker, 1) {
    t.field("speed", &DemoWalker::speed);
    t.field("dir", &DemoWalker::dir);
    t.field("seed", &DemoWalker::seed);
    t.field("wells", &DemoWalker::wells);
}

using namespace forge;
using namespace forge::world;
using namespace forge::sim;
using render::Camera2D;
using render::Sprite;
using scene::Position;

namespace {

constexpr i32 kHomeWidth = 2048; // tiles along which the creatures start
constexpr f32 kGravity = 40;
constexpr f32 kWellRadius = 9;

u32 hash32(u32 a, u32 b) {
    u32 h = a * 374761393u + b * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

class Demo {
public:
    bool init(SDL_GPUDevice* device, SDL_GPUTextureFormat format, u32 creatures) {
        WorldDesc wd;
        world_ = std::make_unique<World>(wd, std::make_shared<SideViewGenerator>(7));
        scene_ = std::make_unique<scene::Scene>(*world_);
        scene_->register_component<DemoWalker>();
        SimDesc sd;
        sd.gravity_y = kGravity;
        sim_ = std::make_unique<Simulation>(*world_, *scene_, sd);
        for (TileId t : {TileGrass, TileDirt, TileStone, TileSand, TileCopper, TileIron, TileGold})
            sim_->collision().set(t, TileShape::Solid);

        atlas_ = demo::make_tile_atlas();
        sheet_ = demo::make_sprite_sheet();
        if (!tiles_.init(device, format, *world_, {atlas_.data(), demo::kTileCellPx, demo::kTileCells})) return false;
        if (!sprites_.init(device, format, sheet_.sheet(), std::max(creatures + 1024, 4096u))) return false;
        if (!lights_.init(device, format)) return false;
        lights_.set_rules(demo::side_view_light_rules());
        capacity_ = creatures + 1024;

        // Load the home stretch, put the creatures on its surface, then let the
        // camera decide what stays loaded.
        const Rect home{0, -400, kHomeWidth, 400};
        for (u32 last = ~0u; world_->stats().resident != last;) {
            last = world_->stats().resident;
            sim_->update(0, home);
            world_->finish_loading();
        }
        std::vector<i32> surface(kHomeWidth);
        for (i32 x = 0; x < kHomeWidth; ++x) {
            i32 y = home.y0;
            while (y < home.y1 && world_->tile(1, x, y) == TileAir) ++y;
            surface[static_cast<usize>(x)] = y;
        }
        std::vector<i32> xs(creatures);
        for (u32 i = 0; i < creatures; ++i) xs[i] = static_cast<i32>(hash32(i, 1) % kHomeWidth);
        std::sort(xs.begin(), xs.end());
        for (u32 i = 0; i < creatures; ++i) {
            const i32 x = xs[i];
            flecs::entity e = scene_->spawn(Position::at_tile(x + 0.5, surface[static_cast<usize>(x)] - 1.0 - (hash32(i, 2) % 6)));
            if (!e.is_valid()) continue;
            Body b;
            b.half_w = 0.35f;
            b.half_h = 0.45f;
            e.set<Body>(b);
            e.set<DemoWalker>({1.5f + static_cast<f32>(hash32(i, 3) % 300) / 100.0f, (i & 1) ? 1.0f : -1.0f, i, 0});
        }

        walkers_ = scene_->ecs().query<Position, Body, DemoWalker>();
        sim_->add_system([this](const TickContext& ctx) {
            each_due(ctx, walkers_, [&](flecs::entity_t, f32, Position&, Body& b, DemoWalker& w) {
                w.seed = hash32(w.seed, static_cast<u32>(ctx.tick));
                if ((w.seed & 511) == 0) w.dir = -w.dir;
                if (!(b.contacts & OnGround)) return;
                // "Forward" is across the pull: right of it when gravity is down.
                const f32 g = std::sqrt(b.gx * b.gx + b.gy * b.gy);
                if (g <= 0) return;
                const f32 nx = b.gx / g, ny = b.gy / g;
                const f32 fx = -ny * w.dir, fy = nx * w.dir;
                const bool blocked = (fx > 0.5f && (b.contacts & HitRight)) || (fx < -0.5f && (b.contacts & HitLeft)) ||
                                     (fy > 0.5f && (b.contacts & HitBottom)) || (fy < -0.5f && (b.contacts & HitTop));
                if (blocked) {
                    if ((w.seed & 3) != 0) {
                        b.vx = -nx * 13.0f + fx * w.speed;
                        b.vy = -ny * 13.0f + fy * w.speed;
                        return;
                    }
                    w.dir = -w.dir;
                }
                // Walk: keep the speed along the pull, set it across.
                const f32 along = b.vx * nx + b.vy * ny;
                b.vx = nx * along + fx * w.speed;
                b.vy = ny * along + fy * w.speed;
            });
        });

        camera.x = kHomeWidth * 0.5;
        camera.y = surface[kHomeWidth / 2] - 12.0;
        camera.zoom = 16;
        return true;
    }

    void toggle_well(f64 x, f64 y) {
        scene_->ecs().each([&](flecs::entity e, const Position& p, const GravitySource&) {
            if (std::hypot(p.tile_x() - x, p.tile_y() - y) < 2.0) remove_ = e;
        });
        if (remove_.is_valid()) {
            remove_.destruct();
            remove_ = flecs::entity();
            return;
        }
        flecs::entity w = scene_->spawn(Position::at_tile(x, y));
        if (!w.is_valid()) return;
        GravitySource g;
        g.radius = kWellRadius;
        g.strength = 70;
        g.replace = true;
        w.set<GravitySource>(g);
        w.set<Trigger>({kWellRadius});
        w.set<KeepAwake>({kWellRadius});
    }

    void dig(f64 x, f64 y) {
        const i32 cx = static_cast<i32>(std::floor(x)), cy = static_cast<i32>(std::floor(y));
        for (i32 dy = -1; dy <= 1; ++dy)
            for (i32 dx = -1; dx <= 1; ++dx) world_->set_tile(1, cx + dx, cy + dy, TileAir);
    }

    // Turns the world's gravity a quarter turn clockwise: down, left, up, right.
    void turn_gravity() {
        gravity_turn_ = (gravity_turn_ + 1) % 4;
        const f32 dirs[4][2] = {{0, 1}, {-1, 0}, {0, -1}, {1, 0}};
        sim_->set_gravity(dirs[gravity_turn_][0] * kGravity, dirs[gravity_turn_][1] * kGravity);
    }
    const char* gravity_name() const {
        const char* names[4] = {"down", "left", "up", "right"};
        return names[gravity_turn_];
    }

    void update(f64 dt, u32 width, u32 height, bool paused) {
        FORGE_ZONE_N("Demo update");
        const u64 t0 = time_now_ns();
        sim_->update(paused ? 0.0 : dt, camera.visible_tiles(width, height));
        sim_ms = ns_to_ms(time_now_ns() - t0);

        // Trigger events: creatures glow while inside a well.
        flecs::world& ecs = scene_->ecs();
        for (const TriggerEvent& t : sim_->frame_events().triggers) {
            flecs::entity other = ecs.entity(t.other);
            if (!other.is_alive()) continue;
            if (DemoWalker* w = other.try_get_mut<DemoWalker>()) {
                if (t.entered) ++w->wells;
                else if (w->wells > 0) --w->wells;
            }
        }

        // Sprites, between the last tick and the next for smooth motion.
        batch_.begin(camera.x, camera.y, capacity_);
        const f32 alpha = sim_->clock().alpha();
        const u32 phase = static_cast<u32>(sim_->clock().tick() / 8);
        walkers_.each([&](const Position& p, const Body& b, const DemoWalker& w) {
            Sprite* s = batch_.push(1);
            if (!s) return;
            f64 x, y;
            draw_position(p, b, paused ? 1.0f : alpha, x, y);
            s->x = static_cast<f32>(x - camera.x);
            s->y = static_cast<f32>(y - camera.y);
            s->w = w.dir < 0 ? -1.0f : 1.0f;
            s->h = 1.0f;
            // Feet toward the pull it feels.
            s->angle = (b.gx != 0 || b.gy != 0) ? std::atan2(-b.gx, b.gy) : 0.0f;
            const u32 kind = w.seed % demo::kCritterKinds;
            s->frame = kind * 2 + (((b.contacts & OnGround) ? phase + w.seed : 0) & 1u);
            s->color = w.wells > 0 ? render::pack_color(255, 170, 255) : 0xffffffffu;
            s->order = 1;
        });
        wells_.clear();
        ecs.each([&](const Position& p, const GravitySource& g) {
            wells_.push_back({p.tile_x(), p.tile_y()});
            Sprite s;
            s.x = static_cast<f32>(p.tile_x() - camera.x);
            s.y = static_cast<f32>(p.tile_y() - camera.y);
            s.w = s.h = g.radius * 2.0f;
            s.frame = demo::kFrameGlow;
            s.color = render::pack_color(150, 90, 255, 90);
            s.order = 0;
            batch_.push(s);
        });
    }

    void render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, u32 width, u32 height) {
        tiles_.prepare(cmd, camera, width, height);
        sprites_.prepare(cmd, batch_, camera, width, height);
        for (const auto& [x, y] : wells_) lights_.add({x, y, 1.2f, 0.6f, 2.0f});
        lights_.prepare(cmd, *world_, camera, width, height);

        SDL_GPUColorTargetInfo info{};
        info.texture = target;
        info.clear_color = SDL_FColor{0.45f, 0.65f, 0.85f, 1.0f};
        info.load_op = SDL_GPU_LOADOP_CLEAR;
        info.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &info, 1, nullptr);
        tiles_.draw(cmd, pass);
        sprites_.draw(cmd, pass);
        lights_.draw(cmd, pass);
        SDL_EndGPURenderPass(pass);
    }

    void shutdown() {
        lights_.shutdown();
        sprites_.shutdown();
        tiles_.shutdown();
        walkers_ = {}; // queries belong to the ECS world: released before it
        sim_.reset();
        scene_.reset();
        world_.reset();
    }

    const SimStats& stats() const { return sim_->stats(); }
    u32 alive() const { return scene_->stats().entities; }

    Camera2D camera;
    f64 sim_ms = 0;

private:
    std::unique_ptr<World> world_;
    std::unique_ptr<scene::Scene> scene_;
    std::unique_ptr<Simulation> sim_;
    flecs::query<Position, Body, DemoWalker> walkers_;
    flecs::entity remove_;
    std::vector<u8> atlas_;
    demo::SheetImage sheet_;
    render::TilemapRenderer tiles_;
    render::SpriteRenderer sprites_;
    render::SpriteBatch batch_;
    render::LightRenderer lights_;
    std::vector<std::pair<f64, f64>> wells_;
    u32 capacity_ = 0;
    u32 gravity_turn_ = 0;
};

class SimDemo final : public App {
public:
    u32 creatures = 30'000;

    void on_render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, u32 width, u32 height) override {
        width_ = width;
        height_ = height;
        if (!ready_ && !failed_) {
            ready_ = demo_.init(gpu(), swapchain_format(), creatures);
            failed_ = !ready_;
            if (failed_) request_quit();
        }
        if (!ready_) return;
        demo_.update(dt_, width, height, paused_);
        demo_.render(cmd, target, width, height);
        if (frame_stats().frame % 30 == 0) {
            const SimStats& s = demo_.stats();
            char text[256];
            std::snprintf(text, sizeof(text),
                          "creatures in memory %u  chunks active %u near %u asleep %u  sim %.2f ms  gravity %s%s",
                          demo_.alive(), s.zones.active, s.zones.near, s.zones.asleep, demo_.sim_ms, demo_.gravity_name(),
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
        if (ready_ && (SDL_GetMouseState(&mx, &my) & SDL_BUTTON_RMASK)) {
            f64 tx, ty;
            mouse_tile(mx, my, tx, ty);
            demo_.dig(tx, ty);
        }
    }

    void on_event(const SDL_Event& e) override {
        if (e.type == SDL_EVENT_MOUSE_WHEEL)
            demo_.camera.zoom = std::clamp(demo_.camera.zoom * (e.wheel.y > 0 ? 1.25f : 0.8f), 2.0f, 64.0f);
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT && ready_) {
            f64 tx, ty;
            mouse_tile(e.button.x, e.button.y, tx, ty);
            demo_.toggle_well(tx, ty);
        }
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
            if (e.key.key == SDLK_SPACE) paused_ = !paused_;
            if (e.key.key == SDLK_G && ready_) demo_.turn_gravity();
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
    f64 dt_ = 0;
    u32 width_ = 0, height_ = 0;
};

// Offscreen: plays 3 seconds with two gravity wells near the camera, then
// saves a picture.
int run_screenshot(u32 creatures, const char* path) {
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
        if (target && demo.init(device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, creatures)) {
            demo.toggle_well(demo.camera.x - 14, demo.camera.y - 2);
            demo.toggle_well(demo.camera.x + 16, demo.camera.y + 2);
            for (u32 f = 0; f < 180; ++f) {
                demo.update(1.0 / 60.0, w, h, false);
                SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
                demo.render(cmd, target, w, h);
                SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
                SDL_WaitForGPUFences(device, true, &fence, 1);
                SDL_ReleaseGPUFence(device, fence);
            }
            const SimStats& s = demo.stats();
            FORGE_INFO("%u creatures in memory; chunks active %u, near %u, asleep %u; last frame sim %.3f ms",
                       demo.alive(), s.zones.active, s.zones.near, s.zones.asleep, demo.sim_ms);
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
    config.title = "Forge Simulation";
    config.shader_formats = render::supported_shader_formats();
    SimDemo app;
    const char* screenshot = nullptr;
    for (int i = 1; i < argc; ++i) {
        const bool has_value = i + 1 < argc;
        if (std::strcmp(argv[i], "--no-vsync") == 0) config.vsync = false;
        else if (std::strcmp(argv[i], "--creatures") == 0 && has_value) app.creatures = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--screenshot") == 0 && has_value) screenshot = argv[++i];
    }
    if (screenshot) return run_screenshot(app.creatures, screenshot);
    return app.run(config);
}
