#pragma once

#include "forge/render/shader_blob.h"

#include <SDL3/SDL_gpu.h>

namespace forge::render {

// Shader formats this build can hand to SDL_CreateGPUDevice.
SDL_GPUShaderFormat supported_shader_formats();

// Creates the shader in whichever format the device takes; nullptr (and a
// logged error) when the build has no matching bytecode.
SDL_GPUShader* create_shader(SDL_GPUDevice* device, const ShaderBlob& blob);

} // namespace forge::render
