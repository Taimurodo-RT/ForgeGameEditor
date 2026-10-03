#pragma once

#include "forge/core/types.h"

#include <string>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gpu.h>

struct SDL_Window;

namespace forge {

struct AppConfig {
    const char* title = "Forge";
    i32 width = 1600;
    i32 height = 900;
    bool vsync = true;
    // No window or GPU: the frame loop still runs. Used by tests, benchmarks
    // and CI machines without a display.
    bool headless = false;
    // Stop after this many frames (0 = run until the window is closed).
    u64 max_frames = 0;
    // Shader bytecode the app can provide; picks the GPU backends allowed.
    SDL_GPUShaderFormat shader_formats =
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXBC | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL;
};

// Rolling frame-time statistics over the last kWindow frames.
struct FrameStats {
    static constexpr u32 kWindow = 240;
    f64 last_ms = 0;
    f64 avg_ms = 0;
    f64 worst_ms = 0; // worst frame in the window: what players feel as a stutter
    u64 frame = 0;
};

class App {
public:
    virtual ~App() = default;

    // Called once after the window and GPU are ready. Return false to abort.
    virtual bool on_init() { return true; }
    // Called once per frame with the time since the previous frame.
    virtual void on_frame(f64 dt_seconds) { (void)dt_seconds; }
    // Called after on_frame with an open command buffer and the window's
    // image for this frame (not called when headless or minimized). Record
    // copy and render passes into cmd; it is submitted afterwards. The
    // default clears the screen.
    virtual void on_render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, u32 width, u32 height);
    // Every window and input event, before on_frame.
    virtual void on_event(const SDL_Event& event) { (void)event; }
    virtual void on_shutdown() {}

    int run(const AppConfig& config);
    void request_quit() { quit_ = true; }

    const FrameStats& frame_stats() const { return stats_; }
    // Extra text shown in the window title after the frame times.
    void set_status(std::string text) { status_ = std::move(text); }
    SDL_Window* window() const { return window_; }
    SDL_GPUDevice* gpu() const { return gpu_; }
    SDL_GPUTextureFormat swapchain_format() const;

private:
    bool create_window_and_gpu(const AppConfig& config);
    void destroy_window_and_gpu();
    void render_frame();
    void update_stats(u64 frame_ns);

    SDL_Window* window_ = nullptr;
    SDL_GPUDevice* gpu_ = nullptr;
    FrameStats stats_{};
    f64 history_[FrameStats::kWindow] = {};
    std::string status_;
    bool quit_ = false;
    bool sdl_initialized_ = false;
};

} // namespace forge
