#pragma once

// Rendering without a window: for screenshots on CI (software Vulkan) and for
// tools that render thumbnails.

#include "forge/core/types.h"

#include <SDL3/SDL_gpu.h>

namespace forge::render {

// Starts SDL video (falling back to the offscreen driver on machines without
// a display) and creates a GPU device. nullptr on failure, with the reason logged.
SDL_GPUDevice* create_offscreen_device();
void destroy_offscreen_device(SDL_GPUDevice* device);

// An RGBA8 texture to render into.
SDL_GPUTexture* create_render_target(SDL_GPUDevice* device, u32 width, u32 height);

// Waits for the GPU, reads the texture back and writes it as a PNG.
bool save_png(SDL_GPUDevice* device, SDL_GPUTexture* texture, u32 width, u32 height, const char* path);

} // namespace forge::render
