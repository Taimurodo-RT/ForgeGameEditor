#pragma once

// Rendering without a window: for screenshots on CI (software Vulkan) and for
// tools that render thumbnails.

#include "forge/core/types.h"

#include <SDL3/SDL_gpu.h>

#include <vector>

namespace forge::render {

// Starts SDL video (falling back to the offscreen driver on machines without
// a display) and creates a GPU device. nullptr on failure, with the reason logged.
SDL_GPUDevice* create_offscreen_device();
void destroy_offscreen_device(SDL_GPUDevice* device);

// An RGBA8 texture to render into.
SDL_GPUTexture* create_render_target(SDL_GPUDevice* device, u32 width, u32 height);

// Waits for the GPU, reads the texture back and writes it as a PNG. path: UTF-8, as write_png.
bool save_png(SDL_GPUDevice* device, SDL_GPUTexture* texture, u32 width, u32 height, const char* path);
// Waits for the GPU and reads an RGBA8 texture back: width × height × 4 bytes.
bool read_pixels(SDL_GPUDevice* device, SDL_GPUTexture* texture, u32 width, u32 height, std::vector<u8>& rgba);
// Writes RGBA8 pixels as a PNG. path: UTF-8 (Cyrillic and spaces too, on Windows as well), its folder made when missing.
bool write_png(const char* path, u32 width, u32 height, const std::vector<u8>& rgba);

} // namespace forge::render
