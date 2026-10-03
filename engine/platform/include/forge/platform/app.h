#pragma once

#include "forge/core/types.h"

struct SDL_Window;
struct SDL_GPUDevice;

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
    virtual void on_shutdown() {}

    int run(const AppConfig& config);
    void request_quit() { quit_ = true; }

    const FrameStats& frame_stats() const { return stats_; }
    SDL_Window* window() const { return window_; }
    SDL_GPUDevice* gpu() const { return gpu_; }

private:
    bool create_window_and_gpu(const AppConfig& config);
    void destroy_window_and_gpu();
    void present_clear();
    void update_stats(u64 frame_ns);

    SDL_Window* window_ = nullptr;
    SDL_GPUDevice* gpu_ = nullptr;
    FrameStats stats_{};
    f64 history_[FrameStats::kWindow] = {};
    bool quit_ = false;
    bool sdl_initialized_ = false;
};

} // namespace forge
