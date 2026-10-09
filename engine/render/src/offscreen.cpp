#include "forge/render/offscreen.h"

#include "forge/core/log.h"
#include "forge/render/gpu.h"

#include <SDL3/SDL.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace forge::render {

SDL_GPUDevice* create_offscreen_device() {
    // The GPU device needs the video subsystem even without a window.
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            FORGE_ERROR("SDL_Init failed: %s", SDL_GetError());
            return nullptr;
        }
    }
    SDL_GPUDevice* device = SDL_CreateGPUDevice(supported_shader_formats(), true, nullptr);
    if (!device) {
        FORGE_ERROR("no GPU device: %s", SDL_GetError());
        SDL_Quit();
        return nullptr;
    }
    FORGE_INFO("GPU backend: %s", SDL_GetGPUDeviceDriver(device));
    return device;
}

void destroy_offscreen_device(SDL_GPUDevice* device) {
    if (device) SDL_DestroyGPUDevice(device);
    SDL_Quit();
}

SDL_GPUTexture* create_render_target(SDL_GPUDevice* device, u32 width, u32 height) {
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    SDL_GPUTexture* texture = SDL_CreateGPUTexture(device, &info);
    if (!texture) FORGE_ERROR("render target: %s", SDL_GetError());
    return texture;
}

bool read_pixels(SDL_GPUDevice* device, SDL_GPUTexture* texture, u32 width, u32 height, std::vector<u8>& rgba) {
    SDL_GPUTransferBufferCreateInfo tb_info{};
    tb_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tb_info.size = width * height * 4;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device, &tb_info);
    if (!tb) return false;
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src{};
    src.texture = texture;
    src.w = width;
    src.h = height;
    src.d = 1;
    SDL_GPUTextureTransferInfo dst{};
    dst.transfer_buffer = tb;
    SDL_DownloadFromGPUTexture(copy, &src, &dst);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(device, true, &fence, 1);
    SDL_ReleaseGPUFence(device, fence);
    bool ok = false;
    const auto* pixels = static_cast<const u8*>(SDL_MapGPUTransferBuffer(device, tb, false));
    if (pixels) {
        rgba.assign(pixels, pixels + static_cast<usize>(width) * height * 4);
        ok = true;
        SDL_UnmapGPUTransferBuffer(device, tb);
    }
    SDL_ReleaseGPUTransferBuffer(device, tb);
    return ok;
}

bool write_png(const char* path, u32 width, u32 height, const std::vector<u8>& rgba) {
    const bool ok = rgba.size() >= static_cast<usize>(width) * height * 4 &&
                    stbi_write_png(path, static_cast<int>(width), static_cast<int>(height), 4, rgba.data(),
                                   static_cast<int>(width * 4)) != 0;
    if (ok) FORGE_INFO("saved %s", path);
    else FORGE_ERROR("could not save %s", path);
    return ok;
}

bool save_png(SDL_GPUDevice* device, SDL_GPUTexture* texture, u32 width, u32 height, const char* path) {
    std::vector<u8> rgba;
    if (!read_pixels(device, texture, width, height, rgba)) {
        FORGE_ERROR("could not save %s", path);
        return false;
    }
    return write_png(path, width, height, rgba);
}

} // namespace forge::render
