// Script demo: everything that moves here is driven by node graphs, loaded
// from apps/script_demo/graphs/*.graph.json and compiled to Luau.
//
//   critter   walks, jumps when blocked, sometimes turns round
//   door      a stone column that opens while someone is near
//             (uses "door_column", a graph made into a node)
//   chest     the first visitor opens it once: coins fly out
//   coin      flies, falls and disappears after a few seconds
//
// Edit a graph file while the demo runs: it is reloaded within half a second
// and the game keeps going (variables stay). Mistakes are logged with the
// node they are in, and the old version keeps running.
//
//   WASD / arrows   move the camera (Shift: faster)    mouse wheel   zoom
//   P               show the slowest nodes in the log   Space   pause
//
//   forge_script_demo [--critters N] [--graphs DIR] [--no-profile] [--no-vsync]
//   forge_script_demo --screenshot out.png [--critters N] [--frames N]   offscreen, after N ticks (360: 6 s)

#include "demo_art.h"
#include "forge/core/file.h"
#include "forge/core/jobs.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/core/time.h"
#include "forge/platform/app.h"
#include "forge/render/gpu.h"
#include "forge/render/offscreen.h"
#include "forge/render/sprite_renderer.h"
#include "forge/render/tilemap_renderer.h"
#include "forge/script/compiler.h"
#include "forge/script/host.h"
#include "forge/script/nodes.h"
#include "forge/sim/simulation.h"
#include "forge/world/generators.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <system_error>
#include <vector>

#ifndef FORGE_DEMO_GRAPHS
#define FORGE_DEMO_GRAPHS "graphs"
#endif

// How a scripted thing looks; graphs change it through its fields.
struct DemoLook {
    forge::u32 frame = 0;
    forge::f32 r = 1, g = 1, b = 1;
};
FORGE_REFLECT_DECLARE(DemoLook)
FORGE_REFLECT(DemoLook, 1) {
    t.field("frame", &DemoLook::frame);
    t.field("r", &DemoLook::r);
    t.field("g", &DemoLook::g);
    t.field("b", &DemoLook::b);
}

using namespace forge;
using namespace forge::world;
using namespace forge::sim;
using namespace forge::script;
using render::Camera2D;
using render::Sprite;
using scene::Position;

namespace {

constexpr i32 kHomeWidth = 512;
constexpr i32 kDoorX = 236;
constexpr i32 kChestX = 276;

// Graphs time every node (as in the editor's play mode); --no-profile compiles
// them as a released game would.
bool g_profile = true;

u32 hash32(u32 a, u32 b) {
    u32 h = a * 374761393u + b * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

// Every *.graph.json in a folder: graphs marked as nodes join the library,
// the rest are compiled and loaded as scripts. Watches the files and does it
// all again when one changes.
class GraphFolder {
public:
    GraphFolder(std::filesystem::path dir, ScriptHost& host) : dir_(std::move(dir)), host_(host) {}

    // Returns true when something was (re)loaded.
    bool refresh() {
        std::map<std::filesystem::path, std::filesystem::file_time_type> now;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
            const std::string name = path_to_utf8(entry.path().filename());
            if (name.size() > 11 && name.ends_with(".graph.json")) now[entry.path()] = entry.last_write_time(ec);
        }
        if (now == seen_) return false;
        seen_ = std::move(now);
        load_all();
        return true;
    }

    const NodeLibrary& library() const { return lib_; }
    u32 loaded() const { return loaded_; }
    // Graphs that did not compile or load last time (their old version keeps running).
    const std::string& broken() const { return broken_; }

private:
    void load_all() {
        broken_.clear();
        auto mark_broken = [&](const std::string& name) { broken_ += (broken_.empty() ? "" : ", ") + name; };
        lib_ = NodeLibrary();
        std::vector<std::string> errors;
        lib_.add_standard(&errors);
        lib_.add_api(host_.api());
        lib_.add_components(host_.exposed_types());
        std::vector<Graph> scripts;
        for (const auto& [path, time] : seen_) {
            std::vector<u8> bytes;
            Graph g;
            std::string err;
            if (!read_file(path, bytes) ||
                !g.from_json(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), &err)) {
                FORGE_WARN("%s: %s", path_to_utf8(path).c_str(), err.empty() ? "cannot read" : err.c_str());
                mark_broken(path_to_utf8(path.filename()));
                continue;
            }
            if (g.as_node.enabled) {
                if (!lib_.add_graph_node(g, &errors)) mark_broken(g.name);
            }
            else scripts.push_back(std::move(g));
        }
        for (const std::string& e : errors) FORGE_WARN("nodes: %s", e.c_str());

        loaded_ = 0;
        CompileOptions opt;
        opt.profile = g_profile;
        for (Graph& g : scripts) {
            std::vector<std::string> notes;
            migrate(g, lib_, &notes);
            for (const std::string& n : notes) FORGE_INFO("%s: updated %s", g.name.c_str(), n.c_str());
            const CompileResult r = compile(g, lib_, opt);
            for (const Diagnostic& d : r.diagnostics) {
                const GraphNode* n = g.find(d.node);
                FORGE_WARN("%s %s, node %u (%s): %s", g.name.c_str(), d.error ? "error" : "warning", d.node,
                           n ? n->def.c_str() : "-", d.message.c_str());
            }
            if (!r.ok) { // the old version keeps running
                mark_broken(g.name);
                continue;
            }
            std::vector<ScriptError> errs;
            if (host_.load(g.name, r.source, &r.map, &errs)) ++loaded_;
            else mark_broken(g.name);
            for (const ScriptError& e : errs) FORGE_WARN("%s:%d: %s", g.name.c_str(), e.line, e.message.c_str());
        }
        FORGE_INFO("graphs: %u scripts loaded from %s", loaded_, path_to_utf8(dir_).c_str());
    }

    std::filesystem::path dir_;
    ScriptHost& host_;
    NodeLibrary lib_;
    std::map<std::filesystem::path, std::filesystem::file_time_type> seen_;
    u32 loaded_ = 0;
    std::string broken_;
};

class Demo {
public:
    bool init(SDL_GPUDevice* device, SDL_GPUTextureFormat format, u32 critters, const std::filesystem::path& graphs) {
        world_ = std::make_unique<World>(WorldDesc{}, std::make_shared<SideViewGenerator>(11));
        scene_ = std::make_unique<scene::Scene>(*world_);
        scene_->register_component<DemoLook>();
        SimDesc sd;
        sd.gravity_y = 40;
        sim_ = std::make_unique<Simulation>(*world_, *scene_, sd);
        for (TileId t : {TileGrass, TileDirt, TileStone, TileSand, TileCopper, TileIron, TileGold})
            sim_->collision().set(t, TileShape::Solid);
        scripts_ = std::make_unique<ScriptHost>(*sim_, *scene_);
        scripts_->expose<DemoLook>();
        graphs_ = std::make_unique<GraphFolder>(graphs, *scripts_);
        graphs_->refresh();

        atlas_ = demo::make_tile_atlas();
        sheet_ = demo::make_sprite_sheet();
        if (!tiles_.init(device, format, *world_, {atlas_.data(), demo::kTileCellPx, demo::kTileCells})) return false;
        if (!sprites_.init(device, format, sheet_.sheet(), critters + 4096)) return false;
        capacity_ = critters + 4096;

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
        for (u32 i = 0; i < critters; ++i) {
            i32 x = static_cast<i32>(hash32(i, 1) % kHomeWidth);
            if (std::abs(x - kDoorX) < 8) x += 16; // the door starts closed
            flecs::entity e = scene_->spawn(Position::at_tile(x + 0.5, surface[static_cast<usize>(x)] - 1.0 - (hash32(i, 2) % 6)));
            if (!e.is_valid()) continue;
            Body b;
            b.half_w = 0.35f;
            b.half_h = 0.45f;
            e.set<Body>(b);
            e.set<DemoLook>({(hash32(i, 3) % demo::kCritterKinds) * 2, 1, 1, 1});
            e.set<Script>({"critter"});
        }
        // The door stands on the ground: its column fills the three tiles above it.
        flecs::entity door = scene_->spawn(Position::at_tile(kDoorX + 0.5, surface[kDoorX] - 1.5));
        door.set<Trigger>({2.5f});
        door.set<Script>({"door"});
        flecs::entity chest = scene_->spawn(Position::at_tile(kChestX + 0.5, surface[kChestX] - 0.5));
        chest.set<Trigger>({1.5f});
        chest.set<DemoLook>({demo::kFrameChest, 1, 1, 1});
        chest.set<Script>({"chest"});

        camera.x = (kDoorX + kChestX) * 0.5;
        camera.y = surface[kDoorX] - 4.0;
        camera.zoom = 20;
        return true;
    }

    void update(f64 dt, u32 width, u32 height, bool paused) {
        reload_timer_ += dt;
        if (reload_timer_ > 0.5) {
            reload_timer_ = 0;
            if (graphs_->refresh()) reloaded_at_ = time_now_ns();
        }
        const u64 t0 = time_now_ns();
        sim_->update(paused ? 0.0 : dt, camera.visible_tiles(width, height));
        sim_ms = ns_to_ms(time_now_ns() - t0);

        batch_.begin(camera.x, camera.y, capacity_);
        const f32 alpha = paused ? 1.0f : sim_->clock().alpha();
        const u32 phase = static_cast<u32>(sim_->clock().tick() / 8);
        scene_->ecs().each([&](flecs::entity e, const Position& p, const DemoLook& look) {
            Sprite* s = batch_.push(1);
            if (!s) return;
            f64 x = p.tile_x(), y = p.tile_y();
            const Body* b = e.try_get<Body>();
            if (b) draw_position(p, *b, alpha, x, y);
            s->x = static_cast<f32>(x - camera.x);
            s->y = static_cast<f32>(y - camera.y);
            s->w = (b && b->vx < 0) ? -1.0f : 1.0f;
            s->h = 1.0f;
            const bool critter = look.frame < demo::kCritterKinds * 2;
            s->frame = critter ? (look.frame & ~1u) + ((b && (b->contacts & OnGround) ? phase + static_cast<u32>(e.id()) : 0) & 1u)
                               : look.frame;
            if (look.frame == demo::kFrameBall) s->w = s->h = 0.7f;
            auto c = [](f32 v) { return static_cast<u8>(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
            s->color = render::pack_color(c(look.r), c(look.g), c(look.b));
            s->order = 1;
        });
    }

    void render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, u32 width, u32 height) {
        tiles_.prepare(cmd, camera, width, height);
        sprites_.prepare(cmd, batch_, camera, width, height);
        SDL_GPUColorTargetInfo info{};
        info.texture = target;
        info.clear_color = SDL_FColor{0.45f, 0.65f, 0.85f, 1.0f};
        info.load_op = SDL_GPU_LOADOP_CLEAR;
        info.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &info, 1, nullptr);
        tiles_.draw_layers(cmd, pass, 0, 2);
        sprites_.draw(cmd, pass);
        SDL_EndGPURenderPass(pass);
    }

    void shutdown() {
        sprites_.shutdown();
        tiles_.shutdown();
        graphs_.reset();
        scripts_.reset();
        sim_.reset();
        scene_.reset();
        world_.reset();
    }

    // "critter #19 std.logic.and 0.41 ms" for the slowest nodes.
    std::string slowest(usize count) const {
        std::string out;
        const std::vector<NodeTime> prof = scripts_->node_profile();
        for (usize i = 0; i < std::min(count, prof.size()); ++i) {
            const NodeTime& t = prof[i];
            char line[160];
            std::snprintf(line, sizeof(line), "%s%s #%u %.2f ms/%llu", out.empty() ? "" : ", ", t.script.c_str(), t.node, t.ms,
                          static_cast<unsigned long long>(t.calls));
            out += line;
        }
        return out;
    }
    void reset_profile() { scripts_->reset_node_profile(); }

    const ScriptStats& script_stats() const { return scripts_->stats(); }
    u32 alive() const { return scene_->stats().entities; }
    const std::string& broken_graphs() const { return graphs_->broken(); }
    bool just_reloaded() const { return reloaded_at_ && time_now_ns() - reloaded_at_ < 2'000'000'000ull; }

    Camera2D camera;
    f64 sim_ms = 0;

private:
    std::unique_ptr<World> world_;
    std::unique_ptr<scene::Scene> scene_;
    std::unique_ptr<Simulation> sim_;
    std::unique_ptr<ScriptHost> scripts_;
    std::unique_ptr<GraphFolder> graphs_;
    std::vector<u8> atlas_;
    demo::SheetImage sheet_;
    render::TilemapRenderer tiles_;
    render::SpriteRenderer sprites_;
    render::SpriteBatch batch_;
    u32 capacity_ = 0;
    f64 reload_timer_ = 0;
    u64 reloaded_at_ = 0;
};

class ScriptDemo final : public App {
public:
    u32 critters = 2000;
    std::filesystem::path graphs = utf8_path(FORGE_DEMO_GRAPHS);

    void on_render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, u32 width, u32 height) override {
        if (!ready_ && !failed_) {
            ready_ = demo_.init(gpu(), swapchain_format(), critters, graphs);
            failed_ = !ready_;
            if (failed_) request_quit();
        }
        if (!ready_) return;
        demo_.update(dt_, width, height, paused_);
        demo_.render(cmd, target, width, height);
        if (frame_stats().frame % 60 == 0) {
            const ScriptStats& s = demo_.script_stats();
            char text[512];
            std::snprintf(text, sizeof(text),
                          "objects %u  sim %.2f ms  scripts %.2f ms (%u calls, %u waiting, %.1f MB)  run-time errors %u  slowest: %s%s%s",
                          demo_.alive(), demo_.sim_ms, s.ms, s.calls, s.waiting, static_cast<f64>(s.memory) / (1 << 20), s.errors,
                          demo_.slowest(2).c_str(), demo_.just_reloaded() ? "  [graphs reloaded]" : "", paused_ ? "  [paused]" : "");
            // A graph that does not compile goes first: the old version keeps
            // running, so nothing else on screen would show it.
            const std::string& broken = demo_.broken_graphs();
            set_status(broken.empty() ? std::string(text) : "GRAPH ERRORS in " + broken + " (see the log)  " + text);
            demo_.reset_profile(); // the title shows the last second
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
    }

    void on_event(const SDL_Event& e) override {
        if (e.type == SDL_EVENT_MOUSE_WHEEL)
            demo_.camera.zoom = std::clamp(demo_.camera.zoom * (e.wheel.y > 0 ? 1.25f : 0.8f), 2.0f, 64.0f);
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
            if (e.key.key == SDLK_SPACE) paused_ = !paused_;
            if (e.key.key == SDLK_P && ready_) FORGE_INFO("slowest nodes: %s", demo_.slowest(8).c_str());
        }
    }

    void on_shutdown() override {
        const FrameStats& s = frame_stats();
        FORGE_INFO("frame avg %.3f ms, worst %.3f ms (last %u frames)", s.avg_ms, s.worst_ms, FrameStats::kWindow);
        if (ready_) demo_.shutdown();
    }

private:
    Demo demo_;
    bool ready_ = false, failed_ = false, paused_ = false;
    f64 dt_ = 0;
};

int run_screenshot(u32 critters, const std::filesystem::path& graphs, const char* path, u32 frames) {
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
        if (target && demo.init(device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, critters, graphs)) {
            f64 script_ms = 0;
            for (u32 f = 0; f < frames; ++f) {
                demo.update(1.0 / 60.0, w, h, false);
                script_ms += demo.script_stats().ms;
                SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
                demo.render(cmd, target, w, h);
                SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
                SDL_WaitForGPUFences(device, true, &fence, 1);
                SDL_ReleaseGPUFence(device, fence);
            }
            const ScriptStats& s = demo.script_stats();
            FORGE_INFO("%u objects; scripts avg %.3f ms per tick, %u calls in the last, %u waiting, %u errors", demo.alive(),
                       script_ms / std::max(frames, 1u), s.calls, s.waiting, s.errors);
            FORGE_INFO("slowest nodes: %s", demo.slowest(5).c_str());
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
    config.title = "Forge Scripts";
    config.shader_formats = render::supported_shader_formats();
    ScriptDemo app;
    const char* screenshot = nullptr;
    u32 frames = 360;
    for (int i = 1; i < argc; ++i) {
        const bool has_value = i + 1 < argc;
        if (std::strcmp(argv[i], "--no-vsync") == 0) config.vsync = false;
        else if (std::strcmp(argv[i], "--critters") == 0 && has_value) app.critters = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--no-profile") == 0) g_profile = false;
        else if (std::strcmp(argv[i], "--graphs") == 0 && has_value) app.graphs = utf8_path(argv[++i]);
        else if (std::strcmp(argv[i], "--screenshot") == 0 && has_value) screenshot = argv[++i];
        else if (std::strcmp(argv[i], "--frames") == 0 && has_value) frames = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
    }
    if (screenshot) return run_screenshot(app.critters, app.graphs, screenshot, frames);
    return app.run(config);
}
