#pragma once

#include "forge/render/shader_blob.h"

#include <SDL3/SDL_gpu.h>

namespace forge::render {

// Shader formats this build can hand to SDL_CreateGPUDevice.
SDL_GPUShaderFormat supported_shader_formats();

// Creates the shader in whichever format the device takes; nullptr (and a
// logged error) when the build has no matching bytecode.
SDL_GPUShader* create_shader(SDL_GPUDevice* device, const ShaderBlob& blob);

// One-off uploads recorded into cmd (the staging memory is freed once the GPU
// has used it). nullptr on failure, with the reason logged.
SDL_GPUBuffer* create_buffer_with_data(SDL_GPUDevice* device, SDL_GPUCommandBuffer* cmd, SDL_GPUBufferUsageFlags usage,
                                       const void* data, u32 size);
SDL_GPUTexture* create_texture_rgba8(SDL_GPUDevice* device, SDL_GPUCommandBuffer* cmd, const u8* rgba, u32 width,
                                     u32 height);

} // namespace forge::render
