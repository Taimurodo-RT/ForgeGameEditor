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
            }
        }

        const u64 now = time_now_ns();
        const u64 frame_ns = now - previous;
        previous = now;

        on_frame(static_cast<f64>(frame_ns) / 1e9);
        if (!config.headless) present_clear();

        update_stats(frame_ns);
        FORGE_FRAME_MARK();

        if (!config.headless && stats_.frame % 30 == 0) {
            char title[160];
            std::snprintf(title, sizeof(title), "%s  |  %.1f FPS  avg %.2f ms  worst %.2f ms", config.title,
                          stats_.avg_ms > 0 ? 1000.0 / stats_.avg_ms : 0.0, stats_.avg_ms, stats_.worst_ms);
            SDL_SetWindowTitle(window_, title);
        }
        if (config.max_frames && stats_.frame >= config.max_frames) quit_ = true;
    }

    on_shutdown();
    destroy_window_and_gpu();
    jobs::shutdown();
    return 0;
}

bool App::create_window_and_gpu(const AppConfig& config) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        FORGE_ERROR("SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    sdl_initialized_ = true;
    window_ = SDL_CreateWindow(config.title, config.width, config.height,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window_) {
        FORGE_ERROR("SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    // SPIR-V for Vulkan, DXIL for Direct3D 12, MSL for Metal.
    const SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL;
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

void App::present_clear() {
    FORGE_ZONE();
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu_);
    if (!cmd) return;

    SDL_GPUTexture* swapchain = nullptr;
    if (SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window_, &swapchain, nullptr, nullptr) && swapchain) {
        SDL_GPUColorTargetInfo target{};
        target.texture = swapchain;
        target.clear_color = SDL_FColor{0.071f, 0.086f, 0.102f, 1.0f}; // Forge UI "surface"
        target.load_op = SDL_GPU_LOADOP_CLEAR;
        target.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
        SDL_EndGPURenderPass(pass);
    }
    SDL_SubmitGPUCommandBuffer(cmd);
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
