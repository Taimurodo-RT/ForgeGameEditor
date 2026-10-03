#include "forge/render/gpu.h"

#include "forge/core/log.h"

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

} // namespace forge::render
