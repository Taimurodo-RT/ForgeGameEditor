#pragma once

// A shader compiled ahead of time for every GPU backend. tools/shaderc makes
// these from GLSL while the engine builds: SPIR-V for Vulkan and, on Windows,
// DXBC for Direct3D 12. The resource counts come from the shader itself, so
// nothing is described twice.

#include "forge/core/types.h"

namespace forge::render {

enum class ShaderStage : u8 { Vertex, Fragment, Compute };

struct ShaderBlob {
    const char* name;
    ShaderStage stage;
    const u8* spirv;
    usize spirv_size;
    const u8* dxbc; // nullptr when not built on Windows
    usize dxbc_size;
    u32 samplers;
    u32 storage_textures; // read-only for compute shaders
    u32 storage_buffers;  // read-only for compute shaders
    u32 uniform_buffers;
    // Compute shaders only.
    u32 readwrite_storage_textures;
    u32 readwrite_storage_buffers;
    u32 threads_x, threads_y, threads_z;
};

} // namespace forge::render
