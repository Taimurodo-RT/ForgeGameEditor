#include "forge/render/gpu.h"

#include "forge/core/log.h"

#include <cstring>

namespace forge::render {

SDL_GPUShaderFormat supported_shader_formats() {
#if defined(_WIN32)
    return SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXBC;
#else
    return SDL_GPU_SHADERFORMAT_SPIRV;
#endif
}

SDL_GPUShader* create_shader(SDL_GPUDevice* device, const ShaderBlob& blob) {
    SDL_GPUShaderCreateInfo info{};
    const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);
    if ((formats & SDL_GPU_SHADERFORMAT_DXBC) && blob.dxbc) {
        info.format = SDL_GPU_SHADERFORMAT_DXBC;
        info.code = blob.dxbc;
        info.code_size = blob.dxbc_size;
    } else if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
        info.format = SDL_GPU_SHADERFORMAT_SPIRV;
        info.code = blob.spirv;
        info.code_size = blob.spirv_size;
    } else {
        FORGE_ERROR("shader %s: no bytecode for this GPU backend", blob.name);
        return nullptr;
    }
    info.entrypoint = "main";
    info.stage = blob.stage == ShaderStage::Vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
    info.num_samplers = blob.samplers;
    info.num_storage_textures = blob.storage_textures;
    info.num_storage_buffers = blob.storage_buffers;
    info.num_uniform_buffers = blob.uniform_buffers;
    SDL_GPUShader* shader = SDL_CreateGPUShader(device, &info);
    if (!shader) FORGE_ERROR("shader %s: %s", blob.name, SDL_GetError());
    return shader;
}

namespace {

bool stage(SDL_GPUDevice* device, const void* data, u32 size, SDL_GPUTransferBuffer*& out) {
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    info.size = size;
    out = SDL_CreateGPUTransferBuffer(device, &info);
    if (!out) return false;
    void* mapped = SDL_MapGPUTransferBuffer(device, out, false);
    if (!mapped) return false;
    std::memcpy(mapped, data, size);
    SDL_UnmapGPUTransferBuffer(device, out);
    return true;
}

} // namespace

SDL_GPUBuffer* create_buffer_with_data(SDL_GPUDevice* device, SDL_GPUCommandBuffer* cmd, SDL_GPUBufferUsageFlags usage,
                                       const void* data, u32 size) {
    SDL_GPUBufferCreateInfo info{};
    info.usage = usage;
    info.size = size;
    SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(device, &info);
    SDL_GPUTransferBuffer* tb = nullptr;
    if (!buffer || !stage(device, data, size, tb)) {
        FORGE_ERROR("buffer upload failed: %s", SDL_GetError());
        if (tb) SDL_ReleaseGPUTransferBuffer(device, tb);
        if (buffer) SDL_ReleaseGPUBuffer(device, buffer);
        return nullptr;
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src{tb, 0};
    SDL_GPUBufferRegion dst{buffer, 0, size};
    SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    SDL_ReleaseGPUTransferBuffer(device, tb);
    return buffer;
}

SDL_GPUTexture* create_texture_rgba8(SDL_GPUDevice* device, SDL_GPUCommandBuffer* cmd, const u8* rgba, u32 width,
                                     u32 height) {
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    SDL_GPUTexture* texture = SDL_CreateGPUTexture(device, &info);
    SDL_GPUTransferBuffer* tb = nullptr;
    if (!texture || !stage(device, rgba, width * height * 4, tb)) {
        FORGE_ERROR("texture upload failed: %s", SDL_GetError());
        if (tb) SDL_ReleaseGPUTransferBuffer(device, tb);
        if (texture) SDL_ReleaseGPUTexture(device, texture);
        return nullptr;
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = tb;
    SDL_GPUTextureRegion dst{};
    dst.texture = texture;
    dst.w = width;
    dst.h = height;
    dst.d = 1;
    SDL_UploadToGPUTexture(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    SDL_ReleaseGPUTransferBuffer(device, tb);
    return texture;
}

} // namespace forge::render
