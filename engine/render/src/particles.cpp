#include "forge/render/particles.h"

#include "forge/core/log.h"
#include "forge/core/profile.h"
#include "forge/render/gpu.h"

#include "shaders/particle_vert.h"
#include "shaders/particles_emit_comp.h"
#include "shaders/particles_update_comp.h"
#include "shaders/sprite_frag.h"

#include <algorithm>
#include <cstring>

namespace forge::render {

namespace {

constexpr u32 kGroup = 256;   // threads per compute group, as in the shaders
constexpr u32 kParticleBytes = 48;

struct EmitParams {
    u32 total, emitter_count, first_slot, capacity, seed;
    u32 pad[3];
};

struct UpdateParams {
    f32 gravity[2];
    f32 drag;
    f32 dt;
    u32 count;
    u32 pad[3];
};

struct ViewParams {
    f32 scale[2];
    f32 offset[2];
};

} // namespace

ParticleSystem::~ParticleSystem() { shutdown(); }

bool ParticleSystem::init(SDL_GPUDevice* device, SDL_GPUTextureFormat target_format, const SpriteSheet& sheet,
                          u32 capacity, f64 origin_x, f64 origin_y, const ParticleLook& look) {
    device_ = device;
    capacity_ = std::max(capacity, kGroup);
    origin_x_ = origin_x;
    origin_y_ = origin_y;
    look_ = look;
    stats_ = {};
    stats_.capacity = capacity_;

    SDL_GPUBufferCreateInfo binfo{};
    binfo.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE |
                  SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    binfo.size = capacity_ * kParticleBytes;
    particles_ = SDL_CreateGPUBuffer(device_, &binfo);
    binfo.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
    binfo.size = max_emitters_ * static_cast<u32>(sizeof(GpuEmitter));
    emitters_ = SDL_CreateGPUBuffer(device_, &binfo);
    SDL_GPUTransferBufferCreateInfo tinfo{};
    tinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tinfo.size = binfo.size;
    transfer_ = SDL_CreateGPUTransferBuffer(device_, &tinfo);
    emit_pipeline_ = create_compute_pipeline(device_, shaders::particles_emit_comp);
    update_pipeline_ = create_compute_pipeline(device_, shaders::particles_update_comp);
    if (!particles_ || !emitters_ || !transfer_ || !emit_pipeline_ || !update_pipeline_) {
        FORGE_ERROR("particles: setup failed: %s", SDL_GetError());
        return false;
    }

    SDL_GPUShader* vs = create_shader(device_, shaders::particle_vert);
    SDL_GPUShader* fs = create_shader(device_, shaders::sprite_frag);
    if (!vs || !fs) {
        if (vs) SDL_ReleaseGPUShader(device_, vs);
        if (fs) SDL_ReleaseGPUShader(device_, fs);
        return false;
    }
    SDL_GPUColorTargetDescription color{};
    color.format = target_format;
    color.blend_state.enable_blend = true;
    color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE; // premultiplied alpha
    color.blend_state.dst_color_blendfactor =
        look_.additive ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    SDL_GPUGraphicsPipelineCreateInfo pinfo{};
    pinfo.vertex_shader = vs;
    pinfo.fragment_shader = fs;
    pinfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pinfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pinfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pinfo.target_info.color_target_descriptions = &color;
    pinfo.target_info.num_color_targets = 1;
    draw_pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &pinfo);
    SDL_ReleaseGPUShader(device_, vs);
    SDL_ReleaseGPUShader(device_, fs);

    SDL_GPUSamplerCreateInfo sinfo{};
    sinfo.min_filter = SDL_GPU_FILTER_LINEAR;
    sinfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    sinfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sinfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sinfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sinfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = SDL_CreateGPUSampler(device_, &sinfo);

    // Frame rectangles, half a texel inside so smooth filtering does not
    // pick up the neighbouring frame.
    std::vector<f32> uv(static_cast<usize>(sheet.frame_count) * 4);
    const f32 hx = 0.5f / static_cast<f32>(sheet.width), hy = 0.5f / static_cast<f32>(sheet.height);
    for (u32 i = 0; i < sheet.frame_count; ++i) {
        const SpriteRect& r = sheet.frames[i];
        uv[i * 4 + 0] = static_cast<f32>(r.x) / static_cast<f32>(sheet.width) + hx;
        uv[i * 4 + 1] = static_cast<f32>(r.y) / static_cast<f32>(sheet.height) + hy;
        uv[i * 4 + 2] = static_cast<f32>(r.x + r.w) / static_cast<f32>(sheet.width) - hx;
        uv[i * 4 + 3] = static_cast<f32>(r.y + r.h) / static_cast<f32>(sheet.height) - hy;
    }
    // Every slot starts dead (age >= life).
    std::vector<f32> dead(static_cast<usize>(capacity_) * kParticleBytes / 4, 0.0f);
    for (u32 i = 0; i < capacity_; ++i) dead[static_cast<usize>(i) * 12 + 4] = 1.0f; // age 1, life 0
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (cmd) {
        frames_ = create_buffer_with_data(device_, cmd, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, uv.data(),
                                          static_cast<u32>(uv.size() * sizeof(f32)));
        sheet_ = create_texture_rgba8(device_, cmd, sheet.rgba, sheet.width, sheet.height);
        // Fill the particle buffer through a temporary transfer buffer.
        SDL_GPUTransferBufferCreateInfo dinfo{};
        dinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        dinfo.size = capacity_ * kParticleBytes;
        if (SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &dinfo)) {
            if (void* mapped = SDL_MapGPUTransferBuffer(device_, tb, false)) {
                std::memcpy(mapped, dead.data(), dinfo.size);
                SDL_UnmapGPUTransferBuffer(device_, tb);
                SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
                SDL_GPUTransferBufferLocation src{tb, 0};
                SDL_GPUBufferRegion dst{particles_, 0, dinfo.size};
                SDL_UploadToGPUBuffer(copy, &src, &dst, false);
                SDL_EndGPUCopyPass(copy);
            }
            SDL_ReleaseGPUTransferBuffer(device_, tb);
        }
        SDL_SubmitGPUCommandBuffer(cmd);
    }
    if (!draw_pipeline_ || !sampler_ || !frames_ || !sheet_) {
        FORGE_ERROR("particles: draw setup failed: %s", SDL_GetError());
        return false;
    }
    return true;
}

void ParticleSystem::shutdown() {
    if (!device_) return;
    if (emit_pipeline_) SDL_ReleaseGPUComputePipeline(device_, emit_pipeline_);
    if (update_pipeline_) SDL_ReleaseGPUComputePipeline(device_, update_pipeline_);
    if (draw_pipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, draw_pipeline_);
    if (particles_) SDL_ReleaseGPUBuffer(device_, particles_);
    if (emitters_) SDL_ReleaseGPUBuffer(device_, emitters_);
    if (frames_) SDL_ReleaseGPUBuffer(device_, frames_);
    if (transfer_) SDL_ReleaseGPUTransferBuffer(device_, transfer_);
    if (sheet_) SDL_ReleaseGPUTexture(device_, sheet_);
    if (sampler_) SDL_ReleaseGPUSampler(device_, sampler_);
    emit_pipeline_ = update_pipeline_ = nullptr;
    draw_pipeline_ = nullptr;
    particles_ = emitters_ = frames_ = nullptr;
    transfer_ = nullptr;
    sheet_ = nullptr;
    sampler_ = nullptr;
    device_ = nullptr;
    pending_.clear();
    pending_count_ = 0;
}

void ParticleSystem::emit(const ParticleEmit& e) {
    if (e.count == 0 || pending_.size() >= max_emitters_) return;
    // More than the whole ring in one frame would only overwrite itself.
    const u32 count = std::min(e.count, capacity_ - std::min(pending_count_, capacity_));
    if (count == 0) return;
    GpuEmitter g{};
    g.x = static_cast<f32>(e.x - origin_x_);
    g.y = static_cast<f32>(e.y - origin_y_);
    g.radius = e.radius;
    g.speed_min = e.speed_min;
    g.speed_max = e.speed_max;
    g.angle = e.angle;
    g.spread = e.spread;
    g.life_min = e.life_min;
    g.life_max = e.life_max;
    g.size_start = e.size_start;
    g.size_end = e.size_end;
    g.spin = e.spin;
    g.color = e.color;
    g.frame = e.frame;
    g.first = pending_count_;
    g.count = count;
    pending_.push_back(g);
    pending_count_ += count;
}

void ParticleSystem::simulate(SDL_GPUCommandBuffer* cmd, f32 dt) {
    FORGE_ZONE_N("Particles simulate");
    const u32 spawned = pending_count_;
    if (!pending_.empty()) {
        auto* mapped = static_cast<GpuEmitter*>(SDL_MapGPUTransferBuffer(device_, transfer_, true));
        if (mapped) {
            std::memcpy(mapped, pending_.data(), pending_.size() * sizeof(GpuEmitter));
            SDL_UnmapGPUTransferBuffer(device_, transfer_);
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTransferBufferLocation src{transfer_, 0};
            SDL_GPUBufferRegion dst{emitters_, 0, static_cast<u32>(pending_.size() * sizeof(GpuEmitter))};
            SDL_UploadToGPUBuffer(copy, &src, &dst, true);
            SDL_EndGPUCopyPass(copy);
        }
    }
    stats_.spawned_last = spawned;
    stats_.spawned_total += spawned;
    stats_.slots_used = static_cast<u32>(std::min<u64>(stats_.spawned_total, capacity_));
    const u32 used = stats_.slots_used;

    SDL_GPUStorageBufferReadWriteBinding rw{};
    rw.buffer = particles_;
    rw.cycle = false; // particles live on from frame to frame
    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, nullptr, 0, &rw, 1);
    if (spawned > 0) {
        const EmitParams p{spawned, static_cast<u32>(pending_.size()), cursor_, capacity_, seed_, {}};
        SDL_BindGPUComputePipeline(pass, emit_pipeline_);
        SDL_BindGPUComputeStorageBuffers(pass, 0, &emitters_, 1);
        SDL_PushGPUComputeUniformData(cmd, 0, &p, sizeof(p));
        SDL_DispatchGPUCompute(pass, (spawned + kGroup - 1) / kGroup, 1, 1);
        cursor_ = (cursor_ + spawned) % capacity_;
        seed_ = seed_ * 1664525u + 1013904223u;
    }
    if (used > 0 && dt > 0) {
        const UpdateParams p{{look_.gravity_x, look_.gravity_y}, look_.drag, dt, used, {}};
        SDL_BindGPUComputePipeline(pass, update_pipeline_);
        SDL_PushGPUComputeUniformData(cmd, 0, &p, sizeof(p));
        SDL_DispatchGPUCompute(pass, (used + kGroup - 1) / kGroup, 1, 1);
    }
    SDL_EndGPUComputePass(pass);
    pending_.clear();
    pending_count_ = 0;
}

void ParticleSystem::draw(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass, const Camera2D& camera, u32 width,
                          u32 height) {
    if (stats_.slots_used == 0) return;
    FORGE_ZONE_N("Particles draw");
    const ViewParams view{{static_cast<f32>(2.0 * camera.zoom / width), static_cast<f32>(-2.0 * camera.zoom / height)},
                          {static_cast<f32>(origin_x_ - camera.snapped_x()), static_cast<f32>(origin_y_ - camera.snapped_y())}};
    SDL_BindGPUGraphicsPipeline(pass, draw_pipeline_);
    SDL_GPUBuffer* buffers[2] = {particles_, frames_};
    SDL_BindGPUVertexStorageBuffers(pass, 0, buffers, 2);
    SDL_GPUTextureSamplerBinding tex{sheet_, sampler_};
    SDL_BindGPUFragmentSamplers(pass, 0, &tex, 1);
    SDL_PushGPUVertexUniformData(cmd, 0, &view, sizeof(view));
    SDL_DrawGPUPrimitives(pass, 6, stats_.slots_used, 0, 0);
}

} // namespace forge::render
