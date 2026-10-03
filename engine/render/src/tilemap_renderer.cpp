#include "forge/render/tilemap_renderer.h"

#include "forge/core/log.h"
#include "forge/core/profile.h"
#include "forge/render/gpu.h"

#include "shaders/tilemap_frag.h"
#include "shaders/tilemap_vert.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace forge::render {

using world::Chunk;
using world::ChunkCoord;
using world::kChunkSize;
using world::kChunkTiles;
using world::Rect;
using world::TileId;

namespace {

// Chunks sent to the GPU per frame at most; the rest wait for the next frame.
// 192 chunks × 16 KiB = 3 MiB of copies, well inside a frame on any PCIe bus.
constexpr u32 kUploadBudget = 192;
// Vulkan only guarantees 128 MiB per storage buffer binding.
constexpr usize kMaxTileBufferBytes = 128 * MiB;

struct ViewUniforms {
    f32 scale[2];
    f32 unused[2];
};

struct LayerUniforms {
    f32 tint[4];
    u32 slot_words;
    u32 layer_words;
    u32 atlas_cells;
    f32 atlas_lod;
    u32 liquid;
    u32 full;
    u32 down;
    u32 pad;
    f32 liquid_colors[16][4];
};

} // namespace

TilemapRenderer::~TilemapRenderer() { shutdown(); }

bool TilemapRenderer::init(SDL_GPUDevice* device, SDL_GPUTextureFormat target_format, world::World& world,
                           const TileAtlas& atlas, u32 max_slots) {
    device_ = device;
    world_ = &world;
    layer_count_ = world.layer_count();
    slot_bytes_ = layer_count_ * kChunkTiles * static_cast<u32>(sizeof(TileId));
    max_slots_ = static_cast<u32>(std::min<usize>(max_slots, kMaxTileBufferBytes / slot_bytes_));
    tints_.assign(layer_count_, Color{});
    liquid_layers_.assign(layer_count_, 0);
    // Back layer (walls, ground under objects) a little darker by default.
    if (layer_count_ > 1) tints_[0] = Color{0.55f, 0.55f, 0.6f, 1.0f};

    SDL_GPUBufferCreateInfo tiles_info{};
    tiles_info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    tiles_info.size = max_slots_ * slot_bytes_;
    tiles_ = SDL_CreateGPUBuffer(device_, &tiles_info);

    SDL_GPUBufferCreateInfo inst_info{};
    inst_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    inst_info.size = max_slots_ * static_cast<u32>(sizeof(Instance));
    instances_ = SDL_CreateGPUBuffer(device_, &inst_info);

    transfer_bytes_ = static_cast<usize>(kUploadBudget) * slot_bytes_ + static_cast<usize>(max_slots_) * sizeof(Instance);
    SDL_GPUTransferBufferCreateInfo tb_info{};
    tb_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tb_info.size = static_cast<u32>(transfer_bytes_);
    transfer_ = SDL_CreateGPUTransferBuffer(device_, &tb_info);
    if (!tiles_ || !instances_ || !transfer_) {
        FORGE_ERROR("tilemap: buffer creation failed: %s", SDL_GetError());
        return false;
    }

    SDL_GPUShader* vs = create_shader(device_, shaders::tilemap_vert);
    SDL_GPUShader* fs = create_shader(device_, shaders::tilemap_frag);
    if (!vs || !fs) {
        if (vs) SDL_ReleaseGPUShader(device_, vs);
        if (fs) SDL_ReleaseGPUShader(device_, fs);
        return false;
    }

    SDL_GPUColorTargetDescription color{};
    color.format = target_format;
    color.blend_state.enable_blend = true;
    color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

    SDL_GPUVertexBufferDescription vb{};
    vb.slot = 0;
    vb.pitch = sizeof(Instance);
    vb.input_rate = SDL_GPU_VERTEXINPUTRATE_INSTANCE;
    SDL_GPUVertexAttribute attrs[2]{};
    attrs[0].location = 0;
    attrs[0].buffer_slot = 0;
    attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attrs[0].offset = offsetof(Instance, x);
    attrs[1].location = 1;
    attrs[1].buffer_slot = 0;
    attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_UINT;
    attrs[1].offset = offsetof(Instance, slot);

    SDL_GPUGraphicsPipelineCreateInfo pinfo{};
    pinfo.vertex_shader = vs;
    pinfo.fragment_shader = fs;
    pinfo.vertex_input_state.vertex_buffer_descriptions = &vb;
    pinfo.vertex_input_state.num_vertex_buffers = 1;
    pinfo.vertex_input_state.vertex_attributes = attrs;
    pinfo.vertex_input_state.num_vertex_attributes = 2;
    pinfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pinfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pinfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pinfo.target_info.color_target_descriptions = &color;
    pinfo.target_info.num_color_targets = 1;
    pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &pinfo);
    SDL_ReleaseGPUShader(device_, vs);
    SDL_ReleaseGPUShader(device_, fs);
    if (!pipeline_) {
        FORGE_ERROR("tilemap: pipeline creation failed: %s", SDL_GetError());
        return false;
    }

    SDL_GPUSamplerCreateInfo sinfo{};
    sinfo.min_filter = SDL_GPU_FILTER_NEAREST;
    sinfo.mag_filter = SDL_GPU_FILTER_NEAREST;
    sinfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sinfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sinfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sinfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sinfo.max_lod = 1000.0f;
    sampler_ = SDL_CreateGPUSampler(device_, &sinfo);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!sampler_ || !cmd || !create_atlas(cmd, atlas)) {
        if (cmd) SDL_CancelGPUCommandBuffer(cmd);
        FORGE_ERROR("tilemap: atlas setup failed: %s", SDL_GetError());
        return false;
    }
    SDL_SubmitGPUCommandBuffer(cmd);

    free_slots_.clear();
    for (u32 i = max_slots_; i-- > 0;) free_slots_.push_back(i);
    world_->add_listener(this);
    stats_.slots_total = max_slots_;
    FORGE_INFO("tilemap: %u chunk slots, %.1f MiB of tiles on the GPU", max_slots_,
               static_cast<f64>(max_slots_) * slot_bytes_ / static_cast<f64>(MiB));
    return true;
}

bool TilemapRenderer::create_atlas(SDL_GPUCommandBuffer* cmd, const TileAtlas& atlas) {
    atlas_cells_ = atlas.cells_per_row;
    atlas_cell_px_ = atlas.cell_px;
    const u32 size = atlas.cell_px * atlas.cells_per_row;
    // Mips down to one texel per cell: far away, a tile becomes its average colour.
    atlas_levels_ = 1;
    while ((atlas.cell_px >> (atlas_levels_ - 1)) > 1) ++atlas_levels_;

    SDL_GPUTextureCreateInfo tinfo{};
    tinfo.type = SDL_GPU_TEXTURETYPE_2D;
    tinfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tinfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    tinfo.width = size;
    tinfo.height = size;
    tinfo.layer_count_or_depth = 1;
    tinfo.num_levels = atlas_levels_;
    atlas_ = SDL_CreateGPUTexture(device_, &tinfo);
    if (!atlas_) return false;

    const u32 bytes = size * size * 4;
    SDL_GPUTransferBufferCreateInfo tb_info{};
    tb_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tb_info.size = bytes;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &tb_info);
    if (!tb) return false;
    void* mapped = SDL_MapGPUTransferBuffer(device_, tb, false);
    std::memcpy(mapped, atlas.rgba, bytes);
    SDL_UnmapGPUTransferBuffer(device_, tb);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = tb;
    SDL_GPUTextureRegion dst{};
    dst.texture = atlas_;
    dst.w = size;
    dst.h = size;
    dst.d = 1;
    SDL_UploadToGPUTexture(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    if (atlas_levels_ > 1) SDL_GenerateMipmapsForGPUTexture(cmd, atlas_);
    SDL_ReleaseGPUTransferBuffer(device_, tb); // freed once the GPU is done with it
    return true;
}

void TilemapRenderer::shutdown() {
    if (!device_) return;
    if (world_) world_->remove_listener(this);
    if (pipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
    if (tiles_) SDL_ReleaseGPUBuffer(device_, tiles_);
    if (instances_) SDL_ReleaseGPUBuffer(device_, instances_);
    if (transfer_) SDL_ReleaseGPUTransferBuffer(device_, transfer_);
    if (atlas_) SDL_ReleaseGPUTexture(device_, atlas_);
    if (sampler_) SDL_ReleaseGPUSampler(device_, sampler_);
    pipeline_ = nullptr;
    tiles_ = instances_ = nullptr;
    transfer_ = nullptr;
    atlas_ = nullptr;
    sampler_ = nullptr;
    device_ = nullptr;
    world_ = nullptr;
    slots_.clear();
}

void TilemapRenderer::set_layer_tint(u32 layer, Color tint) {
    if (layer < tints_.size()) tints_[layer] = tint;
}

void TilemapRenderer::set_layer_liquid(u32 layer, const Color* colors, u32 count, u32 full) {
    if (layer >= liquid_layers_.size()) return;
    liquid_layers_[layer] = 1;
    liquid_full_ = full;
    for (u32 i = 0; i < 16; ++i) liquid_colors_[i] = i < count ? colors[i] : Color{};
}

void TilemapRenderer::on_chunk_unloading(Chunk& chunk) {
    auto it = slots_.find(chunk.coord);
    if (it == slots_.end()) return;
    free_slots_.push_back(it->second.index);
    slots_.erase(it);
}

bool TilemapRenderer::acquire_slot(ChunkCoord coord, Slot*& out) {
    auto it = slots_.find(coord);
    if (it != slots_.end()) {
        out = &it->second;
        return true;
    }
    u32 index;
    if (!free_slots_.empty()) {
        index = free_slots_.back();
        free_slots_.pop_back();
    } else {
        // Every slot is taken: reuse the one off screen the longest.
        auto oldest = slots_.end();
        for (auto s = slots_.begin(); s != slots_.end(); ++s)
            if (s->second.last_seen < frame_ && (oldest == slots_.end() || s->second.last_seen < oldest->second.last_seen))
                oldest = s;
        if (oldest == slots_.end()) return false; // all on screen this frame
        index = oldest->second.index;
        slots_.erase(oldest);
    }
    Slot slot;
    slot.index = index;
    out = &slots_.emplace(coord, slot).first->second;
    return true;
}

void TilemapRenderer::prepare(SDL_GPUCommandBuffer* cmd, const Camera2D& camera, u32 width, u32 height) {
    FORGE_ZONE_N("Tilemap prepare");
    ++frame_;
    stats_.visible_chunks = 0;
    stats_.uploads = 0;
    frame_instances_.clear();
    frame_uploads_.clear();

    // Snap the camera to whole pixels so tiles do not shimmer while scrolling.
    const f64 zoom = camera.zoom;
    const f64 cx = camera.snapped_x();
    const f64 cy = camera.snapped_y();

    Rect chunks = world::chunks_of(camera.visible_tiles(width, height));
    if (!world_->bounds().empty()) chunks = chunks.clipped(world_->bounds());
    for (i32 y = chunks.y0; y < chunks.y1; ++y) {
        for (i32 x = chunks.x0; x < chunks.x1; ++x) {
            Chunk* chunk = world_->find_chunk({x, y});
            if (!chunk) continue;
            ++stats_.visible_chunks;
            Slot* slot = nullptr;
            if (!acquire_slot(chunk->coord, slot)) continue;
            slot->last_seen = frame_;
            if ((!slot->uploaded || slot->revision != chunk->revision) && frame_uploads_.size() < kUploadBudget) {
                frame_uploads_.push_back(chunk);
                slot->uploaded = true;
                slot->revision = chunk->revision;
            }
            if (!slot->uploaded) continue;
            frame_instances_.push_back({static_cast<f32>(static_cast<f64>(x) * kChunkSize - cx),
                                        static_cast<f32>(static_cast<f64>(y) * kChunkSize - cy), slot->index});
        }
    }
    draw_count_ = static_cast<u32>(frame_instances_.size());
    stats_.drawn_chunks = draw_count_;
    stats_.uploads = static_cast<u32>(frame_uploads_.size());
    stats_.slots_used = max_slots_ - static_cast<u32>(free_slots_.size());

    scale_x_ = static_cast<f32>(2.0 * zoom / width);
    scale_y_ = static_cast<f32>(-2.0 * zoom / height);
    atlas_lod_ = std::clamp(std::log2(static_cast<f32>(atlas_cell_px_) / camera.zoom), 0.0f,
                            static_cast<f32>(atlas_levels_ - 1));

    if (frame_uploads_.empty() && draw_count_ == 0) return;
    const usize instance_offset = static_cast<usize>(kUploadBudget) * slot_bytes_;
    auto* mapped = static_cast<u8*>(SDL_MapGPUTransferBuffer(device_, transfer_, true));
    if (!mapped) return;
    for (usize i = 0; i < frame_uploads_.size(); ++i)
        std::memcpy(mapped + i * slot_bytes_, frame_uploads_[i]->tiles, slot_bytes_);
    std::memcpy(mapped + instance_offset, frame_instances_.data(), frame_instances_.size() * sizeof(Instance));
    SDL_UnmapGPUTransferBuffer(device_, transfer_);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    for (usize i = 0; i < frame_uploads_.size(); ++i) {
        SDL_GPUTransferBufferLocation src{transfer_, static_cast<u32>(i * slot_bytes_)};
        SDL_GPUBufferRegion dst{tiles_, slots_.at(frame_uploads_[i]->coord).index * slot_bytes_, slot_bytes_};
        // No cycling: the other slots in this buffer must survive.
        SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    }
    if (draw_count_ > 0) {
        SDL_GPUTransferBufferLocation src{transfer_, static_cast<u32>(instance_offset)};
        SDL_GPUBufferRegion dst{instances_, 0, draw_count_ * static_cast<u32>(sizeof(Instance))};
        SDL_UploadToGPUBuffer(copy, &src, &dst, true); // rewritten whole every frame
    }
    SDL_EndGPUCopyPass(copy);
}

void TilemapRenderer::draw_layers(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass, u32 first, u32 count) {
    if (draw_count_ == 0 || first >= layer_count_) return;
    FORGE_ZONE_N("Tilemap draw");
    SDL_BindGPUGraphicsPipeline(pass, pipeline_);
    SDL_GPUBufferBinding vb{instances_, 0};
    SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
    SDL_GPUTextureSamplerBinding tex{atlas_, sampler_};
    SDL_BindGPUFragmentSamplers(pass, 0, &tex, 1);
    SDL_BindGPUFragmentStorageBuffers(pass, 0, &tiles_, 1);

    const ViewUniforms view{{scale_x_, scale_y_}, {0, 0}};
    SDL_PushGPUVertexUniformData(cmd, 0, &view, sizeof(view));
    const u32 end = std::min(layer_count_, first + count);
    for (u32 layer = first; layer < end; ++layer) {
        const Color& t = tints_[layer];
        LayerUniforms u{};
        const f32 tint[4] = {t.r, t.g, t.b, t.a};
        std::memcpy(u.tint, tint, sizeof(tint));
        u.slot_words = slot_bytes_ / 4;
        u.layer_words = layer * (kChunkTiles / 2);
        u.atlas_cells = atlas_cells_;
        u.atlas_lod = atlas_lod_;
        u.liquid = liquid_layers_[layer];
        u.full = liquid_full_;
        u.down = liquid_down_;
        for (u32 i = 0; i < 16; ++i) {
            const Color& c = liquid_colors_[i];
            const f32 rgba[4] = {c.r, c.g, c.b, c.a};
            std::memcpy(u.liquid_colors[i], rgba, sizeof(rgba));
        }
        SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof(u));
        SDL_DrawGPUPrimitives(pass, 6, draw_count_, 0, 0);
    }
}

} // namespace forge::render
