#include "forge/platform/app.h"

#include "forge/core/jobs.h"
#include "forge/core/log.h"
#include "forge/core/profile.h"
#include "forge/core/time.h"

#include <SDL3/SDL.h>

#include <cstdio>

namespace forge {

int App::run(const AppConfig& config) {
    FORGE_THREAD_NAME("Main");
    jobs::init();

    if (!config.headless && !create_window_and_gpu(config)) {
        jobs::shutdown();
        return 1;
    }
    if (!on_init()) {
        destroy_window_and_gpu();
        jobs::shutdown();
        return 1;
    }

    u64 previous = time_now_ns();
    while (!quit_) {
        FORGE_ZONE_N("Frame");

        if (!config.headless) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT) quit_ = true;
                track_window(event);
                on_event(event);
            }
        }

        const u64 now = time_now_ns();
        const u64 frame_ns = now - previous;
        previous = now;

        on_frame(static_cast<f64>(frame_ns) / 1e9);
        if (!config.headless && !hidden_) render_frame();

        update_stats(frame_ns);
        FORGE_FRAME_MARK();

        if (!config.headless && config.stats_in_title && stats_.frame % 30 == 0) {
            char title[512];
            std::snprintf(title, sizeof(title), "%s  |  %.1f FPS  avg %.2f ms  worst %.2f ms%s%s", config.title,
                          stats_.avg_ms > 0 ? 1000.0 / stats_.avg_ms : 0.0, stats_.avg_ms, stats_.worst_ms,
                          status_.empty() ? "" : "  |  ", status_.c_str());
            SDL_SetWindowTitle(window_, title);
        }
        if (config.max_frames && stats_.frame >= config.max_frames) quit_ = true;
        if (!config.headless) throttle(config, now);
    }

    on_shutdown();
    destroy_window_and_gpu();
    jobs::shutdown();
    return 0;
}

void App::track_window(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_WINDOW_FOCUS_GAINED: focused_ = true; break;
    case SDL_EVENT_WINDOW_FOCUS_LOST: focused_ = false; break;
    case SDL_EVENT_WINDOW_MINIMIZED:
    case SDL_EVENT_WINDOW_HIDDEN: hidden_ = true; break;
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
    case SDL_EVENT_WINDOW_SHOWN: hidden_ = false; break;
    default: break;
    }
}

// Without this a minimized window spins: acquiring its image fails at once,
// so the loop runs flat out on a core and keeps the GPU driver busy, which
// starves other programs (a game started next to the editor could hang).
void App::throttle(const AppConfig& config, u64 frame_start_ns) {
    u32 fps = 0;
    if (hidden_) fps = 20;
    else if (!focused_ && config.background_fps) fps = config.background_fps;
    if (!fps) return;
    const u64 budget = 1'000'000'000ull / fps;
    const u64 spent = time_now_ns() - frame_start_ns;
    if (spent >= budget) return;
    // Sleep until the frame's time is up or an event arrives (a click on the
    // window wakes it at once).
    const u64 left_ms = (budget - spent) / 1'000'000ull;
    if (left_ms == 0) return;
    SDL_WaitEventTimeout(nullptr, static_cast<Sint32>(left_ms)); // leaves the event queued
}

bool App::create_window_and_gpu(const AppConfig& config) {
    // The click that brings the window to the front also presses the button
    // under the mouse, as in other programs.
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        FORGE_ERROR("SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    sdl_initialized_ = true;
    window_ = SDL_CreateWindow(config.title, config.width, config.height,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                   (config.fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
    if (!window_) {
        FORGE_ERROR("SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    // SPIR-V for Vulkan, DXBC/DXIL for Direct3D 12, MSL for Metal.
    const SDL_GPUShaderFormat formats = config.shader_formats;
#if defined(NDEBUG)
    const bool debug = false;
#else
    const bool debug = true;
#endif
    gpu_ = SDL_CreateGPUDevice(formats, debug, nullptr);
    if (!gpu_) {
        FORGE_ERROR("SDL_CreateGPUDevice failed: %s", SDL_GetError());
        return false;
    }
    if (!SDL_ClaimWindowForGPUDevice(gpu_, window_)) {
        FORGE_ERROR("SDL_ClaimWindowForGPUDevice failed: %s", SDL_GetError());
        return false;
    }
    // Mailbox gives the lowest latency without tearing when vsync is off.
    const SDL_GPUPresentMode mode =
        !config.vsync && SDL_WindowSupportsGPUPresentMode(gpu_, window_, SDL_GPU_PRESENTMODE_MAILBOX)
            ? SDL_GPU_PRESENTMODE_MAILBOX
            : SDL_GPU_PRESENTMODE_VSYNC;
    SDL_SetGPUSwapchainParameters(gpu_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode);

    FORGE_INFO("GPU backend: %s", SDL_GetGPUDeviceDriver(gpu_));
    return true;
}

void App::destroy_window_and_gpu() {
    if (gpu_ && window_) SDL_ReleaseWindowFromGPUDevice(gpu_, window_);
    if (gpu_) SDL_DestroyGPUDevice(gpu_);
    if (window_) SDL_DestroyWindow(window_);
    if (sdl_initialized_) SDL_Quit();
    gpu_ = nullptr;
    window_ = nullptr;
    sdl_initialized_ = false;
}

void App::render_frame() {
    FORGE_ZONE();
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu_);
    if (!cmd) return;

    SDL_GPUTexture* swapchain = nullptr;
    u32 width = 0, height = 0;
    if (SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window_, &swapchain, &width, &height) && swapchain)
        on_render(cmd, swapchain, width, height);
    SDL_SubmitGPUCommandBuffer(cmd);
}

void App::on_render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, u32, u32) {
    SDL_GPUColorTargetInfo info{};
    info.texture = target;
    info.clear_color = SDL_FColor{0.071f, 0.086f, 0.102f, 1.0f}; // Forge UI "surface"
    info.load_op = SDL_GPU_LOADOP_CLEAR;
    info.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &info, 1, nullptr);
    SDL_EndGPURenderPass(pass);
}

SDL_GPUTextureFormat App::swapchain_format() const {
    return gpu_ && window_ ? SDL_GetGPUSwapchainTextureFormat(gpu_, window_) : SDL_GPU_TEXTUREFORMAT_INVALID;
}

void App::update_stats(u64 frame_ns) {
    const f64 ms = ns_to_ms(frame_ns);
    history_[stats_.frame % FrameStats::kWindow] = ms;
    ++stats_.frame;

    const u64 n = stats_.frame < FrameStats::kWindow ? stats_.frame : FrameStats::kWindow;
    f64 sum = 0, worst = 0;
    for (u64 i = 0; i < n; ++i) {
        sum += history_[i];
        if (history_[i] > worst) worst = history_[i];
    }
    stats_.last_ms = ms;
    stats_.avg_ms = sum / static_cast<f64>(n);
    stats_.worst_ms = worst;
    FORGE_PLOT("frame ms", ms);
}

} // namespace forge
