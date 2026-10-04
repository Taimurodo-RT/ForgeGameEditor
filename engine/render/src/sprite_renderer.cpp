#include "forge/render/sprite_renderer.h"

#include "forge/core/log.h"
#include "forge/core/profile.h"
#include "forge/core/time.h"
#include "forge/render/gpu.h"

#include "shaders/sprite_frag.h"
#include "shaders/sprite_vert.h"

#include <algorithm>
#include <vector>

namespace forge::render {

SpriteRenderer::~SpriteRenderer() { shutdown(); }

bool SpriteRenderer::init(SDL_GPUDevice* device, SDL_GPUTextureFormat target_format, const SpriteSheet& sheet,
                          u32 max_sprites) {
    device_ = device;
    max_sprites_ = max_sprites;
    const u32 sprite_bytes = max_sprites_ * static_cast<u32>(sizeof(Sprite));
    const u32 order_bytes = max_sprites_ * static_cast<u32>(sizeof(u32));

    SDL_GPUBufferCreateInfo binfo{};
    binfo.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    binfo.size = sprite_bytes;
    sprites_ = SDL_CreateGPUBuffer(device_, &binfo);
    binfo.size = order_bytes;
    order_ = SDL_CreateGPUBuffer(device_, &binfo);
    SDL_GPUTransferBufferCreateInfo tinfo{};
    tinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tinfo.size = sprite_bytes + order_bytes;
    transfer_ = SDL_CreateGPUTransferBuffer(device_, &tinfo);
    if (!sprites_ || !order_ || !transfer_) {
        FORGE_ERROR("sprites: buffer creation failed: %s", SDL_GetError());
        return false;
    }

    SDL_GPUShader* vs = create_shader(device_, shaders::sprite_vert);
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
    color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
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
    pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &pinfo);
    SDL_ReleaseGPUShader(device_, vs);
    SDL_ReleaseGPUShader(device_, fs);
    if (!pipeline_) {
        FORGE_ERROR("sprites: pipeline creation failed: %s", SDL_GetError());
        return false;
    }

    SDL_GPUSamplerCreateInfo sinfo{};
    const SDL_GPUFilter filter = sheet.smooth ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    sinfo.min_filter = filter;
    sinfo.mag_filter = filter;
    sinfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sinfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sinfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sinfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = SDL_CreateGPUSampler(device_, &sinfo);

    if (!sampler_ || !set_sheet(sheet)) {
        FORGE_ERROR("sprites: sheet setup failed: %s", SDL_GetError());
        return false;
    }
    FORGE_INFO("sprites: up to %u per frame, %u frames in a %ux%u sheet", max_sprites_, sheet.frame_count, sheet.width,
               sheet.height);
    return true;
}

bool SpriteRenderer::set_sheet(const SpriteSheet& sheet) {
    if (!device_ || !sheet.rgba || sheet.width == 0 || sheet.height == 0) return false;
    // Frame rectangles as texture coordinates.
    std::vector<f32> uv(static_cast<usize>(std::max(sheet.frame_count, 1u)) * 4);
    for (u32 i = 0; i < sheet.frame_count; ++i) {
        const SpriteRect& r = sheet.frames[i];
        uv[i * 4 + 0] = static_cast<f32>(r.x) / static_cast<f32>(sheet.width);
        uv[i * 4 + 1] = static_cast<f32>(r.y) / static_cast<f32>(sheet.height);
        uv[i * 4 + 2] = static_cast<f32>(r.x + r.w) / static_cast<f32>(sheet.width);
        uv[i * 4 + 3] = static_cast<f32>(r.y + r.h) / static_cast<f32>(sheet.height);
    }
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) return false;
    SDL_GPUBuffer* frames = create_buffer_with_data(device_, cmd, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, uv.data(),
                                                    static_cast<u32>(uv.size() * sizeof(f32)));
    SDL_GPUTexture* texture = create_texture_rgba8(device_, cmd, sheet.rgba, sheet.width, sheet.height);
    SDL_SubmitGPUCommandBuffer(cmd);
    if (!frames || !texture) {
        if (frames) SDL_ReleaseGPUBuffer(device_, frames);
        if (texture) SDL_ReleaseGPUTexture(device_, texture);
        return false;
    }
    // The GPU lets go of the old ones once the frames using them are done.
    if (frames_) SDL_ReleaseGPUBuffer(device_, frames_);
    if (sheet_) SDL_ReleaseGPUTexture(device_, sheet_);
    frames_ = frames;
    sheet_ = texture;
    return true;
}

void SpriteRenderer::shutdown() {
    if (!device_) return;
    if (pipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
    if (sprites_) SDL_ReleaseGPUBuffer(device_, sprites_);
    if (frames_) SDL_ReleaseGPUBuffer(device_, frames_);
    if (order_) SDL_ReleaseGPUBuffer(device_, order_);
    if (transfer_) SDL_ReleaseGPUTransferBuffer(device_, transfer_);
    if (sheet_) SDL_ReleaseGPUTexture(device_, sheet_);
    if (sampler_) SDL_ReleaseGPUSampler(device_, sampler_);
    pipeline_ = nullptr;
    sprites_ = frames_ = order_ = nullptr;
    transfer_ = nullptr;
    sheet_ = nullptr;
    sampler_ = nullptr;
    device_ = nullptr;
}

void SpriteRenderer::prepare(SDL_GPUCommandBuffer* cmd, SpriteBatch& batch, const Camera2D& camera, u32 width,
                             u32 height, bool sorted) {
    FORGE_ZONE_N("Sprites prepare");
    const u64 t0 = time_now_ns();
    const f64 cx = camera.snapped_x(), cy = camera.snapped_y();
    const f64 half_w = static_cast<f64>(width) / (2.0 * camera.zoom);
    const f64 half_h = static_cast<f64>(height) / (2.0 * camera.zoom);
    const f64 ox = batch.origin_x(), oy = batch.origin_y();
    const ViewBox view{static_cast<f32>(cx - half_w - ox), static_cast<f32>(cy - half_h - oy),
                       static_cast<f32>(cx + half_w - ox), static_cast<f32>(cy + half_h - oy)};
    view_.scale[0] = static_cast<f32>(2.0 * camera.zoom / width);
    view_.scale[1] = static_cast<f32>(-2.0 * camera.zoom / height);
    view_.offset[0] = static_cast<f32>(ox - cx);
    view_.offset[1] = static_cast<f32>(oy - cy);
    view_.sorted = sorted ? 1u : 0u;

    SpriteList list;
    if (batch.size() > 0) {
        auto* mapped = static_cast<u8*>(SDL_MapGPUTransferBuffer(device_, transfer_, true));
        if (mapped) {
            auto* indices = reinterpret_cast<u32*>(mapped + static_cast<usize>(max_sprites_) * sizeof(Sprite));
            list = batch.finish(view, sorted, reinterpret_cast<Sprite*>(mapped), indices, max_sprites_);
            SDL_UnmapGPUTransferBuffer(device_, transfer_);
        }
    }
    draw_count_ = list.draws;
    if (list.sprites > 0) {
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTransferBufferLocation src{transfer_, 0};
        SDL_GPUBufferRegion dst{sprites_, 0, list.sprites * static_cast<u32>(sizeof(Sprite))};
        SDL_UploadToGPUBuffer(copy, &src, &dst, true);
        if (sorted && list.draws > 0) {
            SDL_GPUTransferBufferLocation isrc{transfer_, max_sprites_ * static_cast<u32>(sizeof(Sprite))};
            SDL_GPUBufferRegion idst{order_, 0, list.draws * static_cast<u32>(sizeof(u32))};
            SDL_UploadToGPUBuffer(copy, &isrc, &idst, true);
        }
        SDL_EndGPUCopyPass(copy);
    }
    const SpriteBatchStats& bs = batch.stats();
    stats_.submitted = bs.submitted;
    stats_.drawn = draw_count_;
    stats_.dropped = bs.dropped;
    stats_.cpu_ms = ns_to_ms(time_now_ns() - t0);
}

void SpriteRenderer::draw(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass) {
    if (draw_count_ == 0) return;
    FORGE_ZONE_N("Sprites draw");
    SDL_BindGPUGraphicsPipeline(pass, pipeline_);
    SDL_GPUBuffer* buffers[3] = {sprites_, frames_, order_};
    SDL_BindGPUVertexStorageBuffers(pass, 0, buffers, 3);
    SDL_GPUTextureSamplerBinding tex{sheet_, sampler_};
    SDL_BindGPUFragmentSamplers(pass, 0, &tex, 1);
    SDL_PushGPUVertexUniformData(cmd, 0, &view_, sizeof(view_));
    SDL_DrawGPUPrimitives(pass, 6, draw_count_, 0, 0);
}

} // namespace forge::render
