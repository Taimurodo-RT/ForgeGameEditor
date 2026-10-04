#include "gpu_renderer.h"

#include "forge/core/log.h"
#include "forge/core/profile.h"
#include "forge/render/gpu.h"

#include "shaders/ui_blend_mask_frag.h"
#include "shaders/ui_blur_frag.h"
#include "shaders/ui_blur_vert.h"
#include "shaders/ui_color_matrix_frag.h"
#include "shaders/ui_drop_shadow_frag.h"
#include "shaders/ui_fullscreen_vert.h"
#include "shaders/ui_gradient_frag.h"
#include "shaders/ui_passthrough_frag.h"
#include "shaders/ui_texture_frag.h"
#include "shaders/ui_vert.h"

#include <RmlUi/Core/DecorationTypes.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Math.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wshadow"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STBI_NO_STDIO
#include <stb_image.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <cmath>
#include <cstring>

namespace forge::ui {

struct GpuRenderer::Geometry {
    SDL_GPUBuffer* buffer = nullptr; // vertices, then 32-bit indices
    u32 index_offset = 0;
    u32 index_count = 0;
};

struct GpuRenderer::Texture {
    SDL_GPUTexture* texture = nullptr;
    u32 width = 0, height = 0;
};

namespace {

enum class FilterType : u8 { Opacity, Blur, DropShadow, ColorMatrix, MaskImage };
enum class GradientFunction : i32 { Linear, Radial, Conic, RepeatingLinear, RepeatingRadial, RepeatingConic };

constexpr int kMaxStops = 16;
constexpr int kBlurWeights = 4;

// Uniform blocks, laid out as std140.
struct MainUniforms {
    float transform[16];
    float translate[2];
    float pad[2];
};
struct QuadUniforms {
    float uv_offset[2];
    float uv_scale[2];
};
struct TintUniforms {
    float tint[4];
};
struct BlurVertexUniforms {
    float texel_offset[2];
    float pad[2];
};
struct BlurFragmentUniforms {
    float weights[4];
    float uv_min[2];
    float uv_max[2];
};
struct ShadowUniforms {
    float color[4];
    float uv_min[2];
    float uv_max[2];
};
struct GradientUniforms {
    i32 func;
    i32 num_stops;
    float p[2];
    float v[2];
    float pad[2];
    float colors[kMaxStops][4];
    float positions[kMaxStops];
};
static_assert(sizeof(GradientUniforms) == 352);

Rml::Colourf to_colorf(Rml::ColourbPremultiplied c) {
    Rml::Colourf out;
    for (int i = 0; i < 4; ++i) out[i] = static_cast<float>(c[i]) / 255.0f;
    return out;
}

// Large blurs run at reduced resolution: halve the image pass_level times,
// then blur with a sigma of at most 3 texels.
void sigma_to_parameters(float desired_sigma, int& pass_level, float& sigma) {
    constexpr int kMaxPasses = 10;
    constexpr float kMaxSinglePassSigma = 3.0f;
    pass_level = std::clamp(Rml::Math::Log2(static_cast<int>(desired_sigma * (2.0f / kMaxSinglePassSigma))), 0,
                            kMaxPasses);
    sigma = std::clamp(desired_sigma / static_cast<float>(1 << pass_level), 0.0f, kMaxSinglePassSigma);
}

void blur_weights(float sigma, float out[kBlurWeights]) {
    float normalization = 0.0f;
    for (int i = 0; i < kBlurWeights; ++i) {
        if (std::fabs(sigma) < 0.1f) out[i] = i == 0 ? 1.0f : 0.0f;
        else
            out[i] = std::exp(-static_cast<float>(i * i) / (2.0f * sigma * sigma)) /
                     (std::sqrt(2.0f * Rml::Math::RMLUI_PI) * sigma);
        normalization += (i == 0 ? 1.0f : 2.0f) * out[i];
    }
    for (int i = 0; i < kBlurWeights; ++i) out[i] /= normalization;
}

// Texture lookups clamped to texel centres inside rect, so bilinear filtering
// does not bleed in neighbouring content.
void uv_limits(Rml::Rectanglei rect, u32 width, u32 height, float min[2], float max[2]) {
    min[0] = (static_cast<float>(rect.p0.x) + 0.5f) / static_cast<float>(width);
    min[1] = (static_cast<float>(rect.p0.y) + 0.5f) / static_cast<float>(height);
    max[0] = (static_cast<float>(rect.p1.x) - 0.5f) / static_cast<float>(width);
    max[1] = (static_cast<float>(rect.p1.y) - 0.5f) / static_cast<float>(height);
}

const QuadUniforms kQuadIdentity = {{0, 0}, {1, 1}};
const TintUniforms kTintOne = {{1, 1, 1, 1}};
const TintUniforms kTintZero = {{0, 0, 0, 0}};

} // namespace

struct GpuRenderer::Filter {
    FilterType type;
    float value = 1.0f;  // opacity
    float sigma = 0.0f;  // blur, drop shadow
    Rml::Vector2f offset{};
    Rml::ColourbPremultiplied color{};
    Rml::Matrix4f color_matrix = Rml::Matrix4f::Identity();
};

struct GpuRenderer::Shader {
    GradientUniforms gradient{};
};

// ---------------------------------------------------------------------------
// Setup

bool GpuRenderer::init(SDL_GPUDevice* device, u32 msaa) {
    device_ = device;

    // D24S8 where available (Nvidia, AMD on Windows), D32S8 elsewhere.
    ds_format_ = SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT, SDL_GPU_TEXTURETYPE_2D,
                                              SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
                     ? SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT
                     : SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;

    // Vulkan only guarantees 1 and 4 samples, so 2 may fall back to 4.
    const u32 candidates[] = {msaa, 4, 2, 1};
    for (u32 s : candidates) {
        SDL_GPUSampleCount count = s >= 8 ? SDL_GPU_SAMPLECOUNT_8
                                   : s >= 4 ? SDL_GPU_SAMPLECOUNT_4
                                   : s >= 2 ? SDL_GPU_SAMPLECOUNT_2
                                            : SDL_GPU_SAMPLECOUNT_1;
        if (count == SDL_GPU_SAMPLECOUNT_1 ||
            (SDL_GPUTextureSupportsSampleCount(device, kColorFormat, count) &&
             SDL_GPUTextureSupportsSampleCount(device, ds_format_, count))) {
            sample_count_ = count;
            samples_ = count == SDL_GPU_SAMPLECOUNT_8 ? 8 : count == SDL_GPU_SAMPLECOUNT_4 ? 4
                                                        : count == SDL_GPU_SAMPLECOUNT_2   ? 2
                                                                                           : 1;
            break;
        }
    }

    vs_main_ = render::create_shader(device, shaders::ui_vert);
    vs_fullscreen_ = render::create_shader(device, shaders::ui_fullscreen_vert);
    vs_blur_ = render::create_shader(device, shaders::ui_blur_vert);
    fs_texture_ = render::create_shader(device, shaders::ui_texture_frag);
    fs_gradient_ = render::create_shader(device, shaders::ui_gradient_frag);
    fs_passthrough_ = render::create_shader(device, shaders::ui_passthrough_frag);
    fs_color_matrix_ = render::create_shader(device, shaders::ui_color_matrix_frag);
    fs_blend_mask_ = render::create_shader(device, shaders::ui_blend_mask_frag);
    fs_drop_shadow_ = render::create_shader(device, shaders::ui_drop_shadow_frag);
    fs_blur_ = render::create_shader(device, shaders::ui_blur_frag);
    if (!vs_main_ || !vs_fullscreen_ || !vs_blur_ || !fs_texture_ || !fs_gradient_ || !fs_passthrough_ ||
        !fs_color_matrix_ || !fs_blend_mask_ || !fs_drop_shadow_ || !fs_blur_)
        return false;
    if (!create_pipelines()) return false;

    SDL_GPUSamplerCreateInfo sampler{};
    sampler.min_filter = SDL_GPU_FILTER_LINEAR;
    sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    linear_clamp_ = SDL_CreateGPUSampler(device, &sampler);
    sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    linear_repeat_ = SDL_CreateGPUSampler(device, &sampler);
    if (!linear_clamp_ || !linear_repeat_) {
        FORGE_ERROR("ui: sampler creation failed: %s", SDL_GetError());
        return false;
    }

    const u8 white[4] = {255, 255, 255, 255};
    white_ = reinterpret_cast<Texture*>(GenerateTexture({white, 4}, {1, 1}));
    if (!white_) return false;

    transforms_.reserve(64);
    ops_.reserve(4096);
    FORGE_INFO("ui: renderer ready, MSAA x%u", samples_);
    return true;
}

void GpuRenderer::shutdown() {
    if (!device_) return;
    flush_releases();
    if (white_) {
        SDL_ReleaseGPUTexture(device_, white_->texture);
        delete white_;
        white_ = nullptr;
    }
    for (Upload& u : uploads_) (void)u; // buffers were created by geometries that RmlUi releases itself
    uploads_.clear();
    destroy_targets();
    SDL_GPUGraphicsPipeline* pipelines[] = {
        geometry_[0], geometry_[1], geometry_[2], geometry_[3], gradient_[0], gradient_[1], composite_[0][0],
        composite_[0][1], composite_[1][0], composite_[1][1], stencil_fill_, copy_, copy_blend_, color_matrix_,
        blend_mask_, blur_, drop_shadow_};
    for (SDL_GPUGraphicsPipeline* p : pipelines)
        if (p) SDL_ReleaseGPUGraphicsPipeline(device_, p);
    for (auto& [format, p] : output_) SDL_ReleaseGPUGraphicsPipeline(device_, p);
    output_.clear();
    SDL_GPUShader* shaders[] = {vs_main_,        vs_fullscreen_,  vs_blur_,       fs_texture_, fs_gradient_,
                                fs_passthrough_, fs_color_matrix_, fs_blend_mask_, fs_drop_shadow_, fs_blur_};
    for (SDL_GPUShader* s : shaders)
        if (s) SDL_ReleaseGPUShader(device_, s);
    if (linear_clamp_) SDL_ReleaseGPUSampler(device_, linear_clamp_);
    if (linear_repeat_) SDL_ReleaseGPUSampler(device_, linear_repeat_);
    if (transfer_) SDL_ReleaseGPUTransferBuffer(device_, transfer_);
    device_ = nullptr;
}

SDL_GPUGraphicsPipeline* GpuRenderer::make_pipeline(SDL_GPUShader* vs, SDL_GPUShader* fs, SDL_GPUTextureFormat format,
                                                    SDL_GPUSampleCount samples, bool depth_stencil, Blend blend,
                                                    Stencil stencil, bool vertex_input) {
    SDL_GPUColorTargetDescription target{};
    target.format = format;
    if (blend == Blend::Premultiplied) {
        target.blend_state.enable_blend = true;
        target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    }
    const bool writes_stencil = stencil == Stencil::WriteReplace || stencil == Stencil::WriteIncr;
    if (writes_stencil) {
        target.blend_state.enable_color_write_mask = true;
        target.blend_state.color_write_mask = 0;
    }

    SDL_GPUVertexBufferDescription buffer{};
    buffer.slot = 0;
    buffer.pitch = sizeof(Rml::Vertex);
    buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute attributes[3]{};
    attributes[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, static_cast<Uint32>(offsetof(Rml::Vertex, position))};
    attributes[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, static_cast<Uint32>(offsetof(Rml::Vertex, colour))};
    attributes[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, static_cast<Uint32>(offsetof(Rml::Vertex, tex_coord))};

    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = vs;
    info.fragment_shader = fs;
    if (vertex_input) {
        info.vertex_input_state.vertex_buffer_descriptions = &buffer;
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_attributes = attributes;
        info.vertex_input_state.num_vertex_attributes = 3;
    }
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.multisample_state.sample_count = samples;
    if (depth_stencil && stencil != Stencil::None) {
        SDL_GPUStencilOpState op{};
        op.fail_op = SDL_GPU_STENCILOP_KEEP;
        op.depth_fail_op = SDL_GPU_STENCILOP_KEEP;
        op.pass_op = stencil == Stencil::WriteReplace ? SDL_GPU_STENCILOP_REPLACE
                     : stencil == Stencil::WriteIncr  ? SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP
                                                      : SDL_GPU_STENCILOP_KEEP;
        op.compare_op = stencil == Stencil::Test ? SDL_GPU_COMPAREOP_EQUAL : SDL_GPU_COMPAREOP_ALWAYS;
        info.depth_stencil_state.front_stencil_state = op;
        info.depth_stencil_state.back_stencil_state = op;
        info.depth_stencil_state.compare_mask = 0xFF;
        info.depth_stencil_state.write_mask = writes_stencil ? 0xFF : 0;
        info.depth_stencil_state.enable_stencil_test = true;
    }
    info.target_info.color_target_descriptions = &target;
    info.target_info.num_color_targets = 1;
    info.target_info.has_depth_stencil_target = depth_stencil;
    info.target_info.depth_stencil_format = depth_stencil ? ds_format_ : SDL_GPU_TEXTUREFORMAT_INVALID;

    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device_, &info);
    if (!pipeline) FORGE_ERROR("ui: pipeline creation failed: %s", SDL_GetError());
    return pipeline;
}

bool GpuRenderer::create_pipelines() {
    const SDL_GPUSampleCount ms = sample_count_;
    for (int s = 0; s < static_cast<int>(Stencil::Count); ++s)
        geometry_[s] = make_pipeline(vs_main_, fs_texture_, kColorFormat, ms, true, Blend::Premultiplied,
                                     static_cast<Stencil>(s), true);
    gradient_[0] = make_pipeline(vs_main_, fs_gradient_, kColorFormat, ms, true, Blend::Premultiplied, Stencil::None, true);
    gradient_[1] = make_pipeline(vs_main_, fs_gradient_, kColorFormat, ms, true, Blend::Premultiplied, Stencil::Test, true);
    for (int b = 0; b < 2; ++b)
        for (int t = 0; t < 2; ++t)
            composite_[b][t] = make_pipeline(vs_fullscreen_, fs_passthrough_, kColorFormat, ms, true,
                                             b ? Blend::Premultiplied : Blend::None,
                                             t ? Stencil::Test : Stencil::None, false);
    stencil_fill_ = make_pipeline(vs_fullscreen_, fs_passthrough_, kColorFormat, ms, true, Blend::None,
                                  Stencil::WriteReplace, false);

    const SDL_GPUSampleCount one = SDL_GPU_SAMPLECOUNT_1;
    copy_ = make_pipeline(vs_fullscreen_, fs_passthrough_, kColorFormat, one, false, Blend::None, Stencil::None, false);
    copy_blend_ = make_pipeline(vs_fullscreen_, fs_passthrough_, kColorFormat, one, false, Blend::Premultiplied,
                                Stencil::None, false);
    color_matrix_ = make_pipeline(vs_fullscreen_, fs_color_matrix_, kColorFormat, one, false, Blend::None,
                                  Stencil::None, false);
    blend_mask_ = make_pipeline(vs_fullscreen_, fs_blend_mask_, kColorFormat, one, false, Blend::None, Stencil::None,
                                false);
    blur_ = make_pipeline(vs_blur_, fs_blur_, kColorFormat, one, false, Blend::None, Stencil::None, false);
    drop_shadow_ = make_pipeline(vs_fullscreen_, fs_drop_shadow_, kColorFormat, one, false, Blend::None,
                                 Stencil::None, false);

    for (SDL_GPUGraphicsPipeline* p : geometry_)
        if (!p) return false;
    return gradient_[0] && gradient_[1] && composite_[0][0] && composite_[0][1] && composite_[1][0] &&
           composite_[1][1] && stencil_fill_ && copy_ && copy_blend_ && color_matrix_ && blend_mask_ && blur_ &&
           drop_shadow_;
}

SDL_GPUGraphicsPipeline* GpuRenderer::output_pipeline(SDL_GPUTextureFormat format) {
    auto it = output_.find(static_cast<u32>(format));
    if (it != output_.end()) return it->second;
    SDL_GPUGraphicsPipeline* p = make_pipeline(vs_fullscreen_, fs_passthrough_, format, SDL_GPU_SAMPLECOUNT_1, false,
                                               Blend::Premultiplied, Stencil::None, false);
    output_[static_cast<u32>(format)] = p;
    return p;
}

bool GpuRenderer::ensure_targets(u32 width, u32 height) {
    if (width == target_width_ && height == target_height_ && depth_stencil_) return true;
    destroy_targets();
    target_width_ = width;
    target_height_ = height;

    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.format = ds_format_;
    info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    info.sample_count = sample_count_;
    depth_stencil_ = SDL_CreateGPUTexture(device_, &info);

    info.format = kColorFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    for (SDL_GPUTexture*& t : postprocess_) t = SDL_CreateGPUTexture(device_, &info);
    mask_ = SDL_CreateGPUTexture(device_, &info);

    if (!depth_stencil_ || !postprocess_[0] || !postprocess_[1] || !postprocess_[2] || !mask_) {
        FORGE_ERROR("ui: render target creation failed: %s", SDL_GetError());
        destroy_targets();
        return false;
    }
    return true;
}

void GpuRenderer::destroy_targets() {
    for (SDL_GPUTexture* t : layers_)
        if (t) SDL_ReleaseGPUTexture(device_, t);
    layers_.clear();
    layer_needs_clear_.clear();
    if (depth_stencil_) SDL_ReleaseGPUTexture(device_, depth_stencil_);
    depth_stencil_ = nullptr;
    for (SDL_GPUTexture*& t : postprocess_) {
        if (t) SDL_ReleaseGPUTexture(device_, t);
        t = nullptr;
    }
    if (mask_) SDL_ReleaseGPUTexture(device_, mask_);
    mask_ = nullptr;
    target_width_ = target_height_ = 0;
    stats_.layers = 0;
}

SDL_GPUTexture* GpuRenderer::layer_texture(u32 index) {
    while (layers_.size() <= index) {
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = kColorFormat;
        // Multisampled textures can only be rendered to and resolved; with
        // MSAA off the layer is read directly.
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | (samples_ == 1 ? SDL_GPU_TEXTUREUSAGE_SAMPLER : 0);
        info.width = target_width_;
        info.height = target_height_;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        info.sample_count = sample_count_;
        SDL_GPUTexture* t = SDL_CreateGPUTexture(device_, &info);
        if (!t) FORGE_ERROR("ui: layer creation failed: %s", SDL_GetError());
        layers_.push_back(t);
        layer_needs_clear_.push_back(true);
        stats_.layers = static_cast<u32>(layers_.size());
    }
    return layers_[index];
}

GpuRenderer::Texture* GpuRenderer::make_texture(u32 width, u32 height, bool sampler_only) {
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = kColorFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | (sampler_only ? 0 : SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
    info.width = std::max(width, 1u);
    info.height = std::max(height, 1u);
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    SDL_GPUTexture* t = SDL_CreateGPUTexture(device_, &info);
    if (!t) {
        FORGE_ERROR("ui: texture creation failed: %s", SDL_GetError());
        return nullptr;
    }
    auto* texture = new Texture;
    texture->texture = t;
    texture->width = info.width;
    texture->height = info.height;
    return texture;
}

// ---------------------------------------------------------------------------
// Resources

Rml::CompiledGeometryHandle GpuRenderer::CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                         Rml::Span<const int> indices) {
    const u32 vertex_bytes = static_cast<u32>(vertices.size() * sizeof(Rml::Vertex));
    const u32 index_bytes = static_cast<u32>(indices.size() * sizeof(int));
    if (vertex_bytes == 0 || index_bytes == 0) return {};

    SDL_GPUBufferCreateInfo info{};
    info.usage = SDL_GPU_BUFFERUSAGE_VERTEX | SDL_GPU_BUFFERUSAGE_INDEX;
    info.size = vertex_bytes + index_bytes;
    SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(device_, &info);
    if (!buffer) {
        FORGE_ERROR("ui: geometry buffer failed: %s", SDL_GetError());
        return {};
    }

    // RmlUi may release the geometry before the frame is drawn, so its data
    // is copied out now and uploaded with everything else in end_frame().
    const u32 offset = static_cast<u32>(staging_.size());
    staging_.resize(offset + info.size);
    std::memcpy(staging_.data() + offset, vertices.data(), vertex_bytes);
    std::memcpy(staging_.data() + offset + vertex_bytes, indices.data(), index_bytes);
    uploads_.push_back({buffer, nullptr, offset, info.size});

    auto* geometry = new Geometry;
    geometry->buffer = buffer;
    geometry->index_offset = vertex_bytes;
    geometry->index_count = static_cast<u32>(indices.size());
    return reinterpret_cast<Rml::CompiledGeometryHandle>(geometry);
}

void GpuRenderer::RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                                 Rml::TextureHandle texture) {
    if (!geometry || !recording_) return;
    Op op{OpType::Draw};
    op.geometry = reinterpret_cast<Geometry*>(geometry);
    op.texture = reinterpret_cast<Texture*>(texture);
    op.translation = translation;
    ops_.push_back(op);
}

void GpuRenderer::ReleaseGeometry(Rml::CompiledGeometryHandle geometry) {
    if (geometry) release_geometry_.push_back(reinterpret_cast<Geometry*>(geometry));
}

Rml::TextureHandle GpuRenderer::LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) {
    Rml::FileInterface* files = Rml::GetFileInterface();
    Rml::FileHandle file = files->Open(source);
    if (!file) {
        FORGE_WARN("ui: image not found: %s", source.c_str());
        return {};
    }
    const size_t size = files->Length(file);
    std::vector<u8> bytes(size);
    files->Read(bytes.data(), size, file);
    files->Close(file);

    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, 4);
    if (!pixels) {
        FORGE_WARN("ui: cannot decode image %s: %s", source.c_str(), stbi_failure_reason());
        return {};
    }
    // RmlUi blends in premultiplied alpha.
    const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
    for (size_t i = 0; i < count; ++i) {
        u8* p = pixels + i * 4;
        const u32 a = p[3];
        p[0] = static_cast<u8>((p[0] * a + 127) / 255);
        p[1] = static_cast<u8>((p[1] * a + 127) / 255);
        p[2] = static_cast<u8>((p[2] * a + 127) / 255);
    }
    dimensions = {w, h};
    Rml::TextureHandle handle = GenerateTexture({pixels, count * 4}, dimensions);
    stbi_image_free(pixels);
    return handle;
}

Rml::TextureHandle GpuRenderer::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) {
    if (dimensions.x <= 0 || dimensions.y <= 0) return {};
    Texture* texture = make_texture(static_cast<u32>(dimensions.x), static_cast<u32>(dimensions.y), true);
    if (!texture) return {};
    // Texture copies start at 512-byte offsets (Direct3D 12 placement rule).
    const u32 offset = static_cast<u32>((staging_.size() + 511) & ~size_t(511));
    staging_.resize(offset + source.size());
    std::memcpy(staging_.data() + offset, source.data(), source.size());
    uploads_.push_back({nullptr, texture, offset, static_cast<u32>(source.size())});
    return reinterpret_cast<Rml::TextureHandle>(texture);
}

void GpuRenderer::ReleaseTexture(Rml::TextureHandle texture) {
    if (texture) release_textures_.push_back(reinterpret_cast<Texture*>(texture));
}

// ---------------------------------------------------------------------------
// Recording

void GpuRenderer::begin_frame(u32 width, u32 height) {
    width_ = std::max(width, 1u);
    height_ = std::max(height, 1u);
    ops_.clear();
    transforms_.clear();
    filter_lists_.clear();
    // Pixels to clip space (y down); z from OpenGL's [-1, 1] to [0, 1].
    Rml::Matrix4f projection = Rml::Matrix4f::ProjectOrtho(0, static_cast<float>(width_), static_cast<float>(height_),
                                                           0, -10000, 10000);
    Rml::Matrix4f depth = Rml::Matrix4f::Identity();
    depth.SetColumn(2, Rml::Vector4f(0, 0, 0.5f, 0));
    depth.SetColumn(3, Rml::Vector4f(0, 0, 0.5f, 1));
    transforms_.push_back(depth * projection);
    layer_depth_ = 0;
    recording_ = true;
}

void GpuRenderer::EnableScissorRegion(bool enable) {
    if (!enable && recording_) {
        Op op{OpType::Scissor};
        ops_.push_back(op);
    }
}

void GpuRenderer::SetScissorRegion(Rml::Rectanglei region) {
    if (!recording_) return;
    Op op{OpType::Scissor};
    op.rect = region;
    ops_.push_back(op);
}

void GpuRenderer::EnableClipMask(bool enable) {
    if (!recording_) return;
    Op op{OpType::ClipEnable};
    op.mode = enable ? 1 : 0;
    ops_.push_back(op);
}

void GpuRenderer::RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle geometry,
                                   Rml::Vector2f translation) {
    if (!geometry || !recording_) return;
    Op op{OpType::ClipMask};
    op.mode = static_cast<u8>(operation);
    op.geometry = reinterpret_cast<Geometry*>(geometry);
    op.translation = translation;
    ops_.push_back(op);
}

void GpuRenderer::SetTransform(const Rml::Matrix4f* transform) {
    if (!recording_) return;
    Op op{OpType::Transform};
    if (transform) {
        op.index = static_cast<u32>(transforms_.size());
        transforms_.push_back(transforms_[0] * *transform);
    }
    ops_.push_back(op);
}

Rml::LayerHandle GpuRenderer::PushLayer() {
    ++layer_depth_;
    Op op{OpType::PushLayer};
    op.index = layer_depth_;
    ops_.push_back(op);
    return static_cast<Rml::LayerHandle>(layer_depth_);
}

void GpuRenderer::CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination, Rml::BlendMode blend_mode,
                                  Rml::Span<const Rml::CompiledFilterHandle> filters) {
    Op op{OpType::Composite};
    op.mode = static_cast<u8>(blend_mode);
    op.index = static_cast<u32>(filter_lists_.size());
    op.count = static_cast<u32>(filters.size());
    filter_lists_.insert(filter_lists_.end(), filters.begin(), filters.end());
    op.rect = Rml::Rectanglei::FromPositionSize({static_cast<int>(source), static_cast<int>(destination)}, {0, 0});
    ops_.push_back(op);
}

void GpuRenderer::PopLayer() {
    if (layer_depth_ > 0) --layer_depth_;
    ops_.push_back(Op{OpType::PopLayer});
}

Rml::TextureHandle GpuRenderer::SaveLayerAsTexture() {
    // The texture has the size of the scissor region, which is only known
    // while replaying, so find the last scissor recorded.
    Rml::Rectanglei bounds = Rml::Rectanglei::MakeInvalid();
    for (auto it = ops_.rbegin(); it != ops_.rend(); ++it) {
        if (it->type == OpType::Scissor) {
            bounds = it->rect;
            break;
        }
    }
    if (!bounds.Valid() || bounds.Width() <= 0 || bounds.Height() <= 0) return {};
    Texture* texture = make_texture(static_cast<u32>(bounds.Width()), static_cast<u32>(bounds.Height()), false);
    if (!texture) return {};
    Op op{OpType::SaveTexture};
    op.texture = texture;
    op.rect = bounds;
    ops_.push_back(op);
    return reinterpret_cast<Rml::TextureHandle>(texture);
}

Rml::CompiledFilterHandle GpuRenderer::SaveLayerAsMaskImage() {
    ops_.push_back(Op{OpType::SaveMask});
    auto* filter = new Filter{FilterType::MaskImage};
    return reinterpret_cast<Rml::CompiledFilterHandle>(filter);
}

Rml::CompiledFilterHandle GpuRenderer::CompileFilter(const Rml::String& name, const Rml::Dictionary& parameters) {
    auto* f = new Filter{FilterType::ColorMatrix};
    if (name == "opacity") {
        f->type = FilterType::Opacity;
        f->value = Rml::Get(parameters, "value", 1.0f);
    } else if (name == "blur") {
        f->type = FilterType::Blur;
        f->sigma = Rml::Get(parameters, "sigma", 1.0f);
    } else if (name == "drop-shadow") {
        f->type = FilterType::DropShadow;
        f->sigma = Rml::Get(parameters, "sigma", 0.0f);
        f->color = Rml::Get(parameters, "color", Rml::Colourb()).ToPremultiplied();
        f->offset = Rml::Get(parameters, "offset", Rml::Vector2f(0.0f));
    } else if (name == "brightness") {
        const float v = Rml::Get(parameters, "value", 1.0f);
        f->color_matrix = Rml::Matrix4f::Diag(v, v, v, 1.0f);
    } else if (name == "contrast") {
        const float v = Rml::Get(parameters, "value", 1.0f);
        const float gray = 0.5f - 0.5f * v;
        f->color_matrix = Rml::Matrix4f::Diag(v, v, v, 1.0f);
        f->color_matrix.SetColumn(3, Rml::Vector4f(gray, gray, gray, 1.0f));
    } else if (name == "invert") {
        const float v = std::clamp(Rml::Get(parameters, "value", 1.0f), 0.0f, 1.0f);
        const float inverted = 1.0f - 2.0f * v;
        f->color_matrix = Rml::Matrix4f::Diag(inverted, inverted, inverted, 1.0f);
        f->color_matrix.SetColumn(3, Rml::Vector4f(v, v, v, 1.0f));
    } else if (name == "grayscale") {
        const float v = Rml::Get(parameters, "value", 1.0f);
        const float r = 1.0f - v;
        const Rml::Vector3f g = v * Rml::Vector3f(0.2126f, 0.7152f, 0.0722f);
        f->color_matrix = Rml::Matrix4f::FromRows({g.x + r, g.y, g.z, 0}, {g.x, g.y + r, g.z, 0},
                                                  {g.x, g.y, g.z + r, 0}, {0, 0, 0, 1});
    } else if (name == "sepia") {
        const float v = Rml::Get(parameters, "value", 1.0f);
        const float r = 1.0f - v;
        const Rml::Vector3f rm = v * Rml::Vector3f(0.393f, 0.769f, 0.189f);
        const Rml::Vector3f gm = v * Rml::Vector3f(0.349f, 0.686f, 0.168f);
        const Rml::Vector3f bm = v * Rml::Vector3f(0.272f, 0.534f, 0.131f);
        f->color_matrix = Rml::Matrix4f::FromRows({rm.x + r, rm.y, rm.z, 0}, {gm.x, gm.y + r, gm.z, 0},
                                                  {bm.x, bm.y, bm.z + r, 0}, {0, 0, 0, 1});
    } else if (name == "hue-rotate") {
        // https://www.w3.org/TR/filter-effects-1/#attr-valuedef-type-huerotate
        const float v = Rml::Get(parameters, "value", 1.0f);
        const float s = std::sin(v), c = std::cos(v);
        f->color_matrix = Rml::Matrix4f::FromRows(
            {0.213f + 0.787f * c - 0.213f * s, 0.715f - 0.715f * c - 0.715f * s, 0.072f - 0.072f * c + 0.928f * s, 0},
            {0.213f - 0.213f * c + 0.143f * s, 0.715f + 0.285f * c + 0.140f * s, 0.072f - 0.072f * c - 0.283f * s, 0},
            {0.213f - 0.213f * c - 0.787f * s, 0.715f - 0.715f * c + 0.715f * s, 0.072f + 0.928f * c + 0.072f * s, 0},
            {0, 0, 0, 1});
    } else if (name == "saturate") {
        const float v = Rml::Get(parameters, "value", 1.0f);
        f->color_matrix = Rml::Matrix4f::FromRows(
            {0.213f + 0.787f * v, 0.715f - 0.715f * v, 0.072f - 0.072f * v, 0},
            {0.213f - 0.213f * v, 0.715f + 0.285f * v, 0.072f - 0.072f * v, 0},
            {0.213f - 0.213f * v, 0.715f - 0.715f * v, 0.072f + 0.928f * v, 0}, {0, 0, 0, 1});
    } else {
        FORGE_WARN("ui: unsupported filter '%s'", name.c_str());
        delete f;
        return {};
    }
    return reinterpret_cast<Rml::CompiledFilterHandle>(f);
}

void GpuRenderer::ReleaseFilter(Rml::CompiledFilterHandle filter) {
    if (filter) release_filters_.push_back(reinterpret_cast<Filter*>(filter));
}

Rml::CompiledShaderHandle GpuRenderer::CompileShader(const Rml::String& name, const Rml::Dictionary& parameters) {
    auto* shader = new Shader;
    GradientUniforms& g = shader->gradient;
    const bool repeating = Rml::Get(parameters, "repeating", false);
    if (name == "linear-gradient") {
        g.func = static_cast<i32>(repeating ? GradientFunction::RepeatingLinear : GradientFunction::Linear);
        const Rml::Vector2f p0 = Rml::Get(parameters, "p0", Rml::Vector2f(0.0f));
        const Rml::Vector2f p1 = Rml::Get(parameters, "p1", Rml::Vector2f(0.0f));
        g.p[0] = p0.x, g.p[1] = p0.y;
        g.v[0] = p1.x - p0.x, g.v[1] = p1.y - p0.y;
    } else if (name == "radial-gradient") {
        g.func = static_cast<i32>(repeating ? GradientFunction::RepeatingRadial : GradientFunction::Radial);
        const Rml::Vector2f c = Rml::Get(parameters, "center", Rml::Vector2f(0.0f));
        const Rml::Vector2f r = Rml::Get(parameters, "radius", Rml::Vector2f(1.0f));
        g.p[0] = c.x, g.p[1] = c.y;
        g.v[0] = 1.0f / r.x, g.v[1] = 1.0f / r.y;
    } else if (name == "conic-gradient") {
        g.func = static_cast<i32>(repeating ? GradientFunction::RepeatingConic : GradientFunction::Conic);
        const Rml::Vector2f c = Rml::Get(parameters, "center", Rml::Vector2f(0.0f));
        const float angle = Rml::Get(parameters, "angle", 0.0f);
        g.p[0] = c.x, g.p[1] = c.y;
        g.v[0] = std::cos(angle), g.v[1] = std::sin(angle);
    } else {
        FORGE_WARN("ui: unsupported shader '%s'", name.c_str());
        delete shader;
        return {};
    }
    auto it = parameters.find("color_stop_list");
    if (it == parameters.end() || it->second.GetType() != Rml::Variant::COLORSTOPLIST) {
        delete shader;
        return {};
    }
    const Rml::ColorStopList& stops = it->second.GetReference<Rml::ColorStopList>();
    g.num_stops = std::min(static_cast<i32>(stops.size()), kMaxStops);
    for (i32 i = 0; i < g.num_stops; ++i) {
        const Rml::Colourf c = to_colorf(stops[static_cast<size_t>(i)].color);
        for (int k = 0; k < 4; ++k) g.colors[i][k] = c[k];
        g.positions[i] = stops[static_cast<size_t>(i)].position.number;
    }
    return reinterpret_cast<Rml::CompiledShaderHandle>(shader);
}

void GpuRenderer::RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry,
                               Rml::Vector2f translation, Rml::TextureHandle) {
    if (!shader || !geometry || !recording_) return;
    Op op{OpType::DrawGradient};
    op.shader = reinterpret_cast<Shader*>(shader);
    op.geometry = reinterpret_cast<Geometry*>(geometry);
    op.translation = translation;
    ops_.push_back(op);
}

void GpuRenderer::ReleaseShader(Rml::CompiledShaderHandle shader) {
    if (shader) release_shaders_.push_back(reinterpret_cast<Shader*>(shader));
}

// ---------------------------------------------------------------------------
// Upload and replay

void GpuRenderer::upload_pending(SDL_GPUCommandBuffer* cmd) {
    if (uploads_.empty()) return;
    FORGE_ZONE();
    const u32 size = static_cast<u32>(staging_.size());
    if (size > transfer_size_) {
        if (transfer_) SDL_ReleaseGPUTransferBuffer(device_, transfer_);
        transfer_size_ = std::max(size, transfer_size_ * 2);
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = transfer_size_;
        transfer_ = SDL_CreateGPUTransferBuffer(device_, &info);
        if (!transfer_) {
            FORGE_ERROR("ui: transfer buffer failed: %s", SDL_GetError());
            transfer_size_ = 0;
            return;
        }
    }
    void* mapped = SDL_MapGPUTransferBuffer(device_, transfer_, true);
    if (!mapped) return;
    std::memcpy(mapped, staging_.data(), size);
    SDL_UnmapGPUTransferBuffer(device_, transfer_);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    for (const Upload& u : uploads_) {
        if (u.buffer) {
            SDL_GPUTransferBufferLocation src{transfer_, u.offset};
            SDL_GPUBufferRegion dst{u.buffer, 0, u.size};
            SDL_UploadToGPUBuffer(copy, &src, &dst, false);
        } else if (u.texture) {
            SDL_GPUTextureTransferInfo src{};
            src.transfer_buffer = transfer_;
            src.offset = u.offset;
            SDL_GPUTextureRegion dst{};
            dst.texture = u.texture->texture;
            dst.w = u.texture->width;
            dst.h = u.texture->height;
            dst.d = 1;
            SDL_UploadToGPUTexture(copy, &src, &dst, false);
        }
    }
    SDL_EndGPUCopyPass(copy);
    stats_.uploads = static_cast<u32>(uploads_.size());
    uploads_.clear();
    staging_.clear();
}

void GpuRenderer::flush_releases() {
    // SDL keeps resources alive until the GPU is done with them.
    for (Geometry* g : release_geometry_) {
        // A geometry compiled and released before any upload never reaches the GPU.
        for (Upload& u : uploads_)
            if (u.buffer == g->buffer) u.buffer = nullptr;
        SDL_ReleaseGPUBuffer(device_, g->buffer);
        delete g;
    }
    release_geometry_.clear();
    for (Texture* t : release_textures_) {
        for (Upload& u : uploads_)
            if (u.texture == t) u.texture = nullptr;
        SDL_ReleaseGPUTexture(device_, t->texture);
        delete t;
    }
    release_textures_.clear();
    for (Filter* f : release_filters_) delete f;
    release_filters_.clear();
    for (Shader* s : release_shaders_) delete s;
    release_shaders_.clear();
}

void GpuRenderer::end_frame(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, SDL_GPUTextureFormat target_format) {
    FORGE_ZONE();
    recording_ = false;
    stats_.draws = stats_.passes = stats_.uploads = 0;
    if (cmd && target && ensure_targets(width_, height_)) {
        upload_pending(cmd);
        replay(cmd);

        // Blend the finished UI over the target.
        resolve_layer(0, postprocess_[0]);
        SDL_GPUGraphicsPipeline* out = output_pipeline(target_format);
        if (out) {
            const Rml::Rectanglei full = Rml::Rectanglei::FromSize({static_cast<int>(width_), static_cast<int>(height_)});
            FullscreenDraw d{target, out, postprocess_[0]};
            d.viewport = full;
            d.scissor = full;
            d.vertex_uniforms = &kQuadIdentity;
            d.vertex_uniforms_size = sizeof(kQuadIdentity);
            d.fragment_uniforms = &kTintOne;
            d.fragment_uniforms_size = sizeof(kTintOne);
            fullscreen(d);
        }
    }
    cmd_ = nullptr;
    flush_releases();
}

void GpuRenderer::end_pass() {
    if (pass_) {
        SDL_EndGPURenderPass(pass_);
        pass_ = nullptr;
    }
    pass_layer_ = -1;
    bound_pipeline_ = nullptr;
}

Rml::Rectanglei GpuRenderer::window_rect() const {
    const Rml::Rectanglei full = Rml::Rectanglei::FromSize({static_cast<int>(width_), static_cast<int>(height_)});
    if (!scissor_.Valid()) return full;
    Rml::Rectanglei r = scissor_;
    r.p0 = Rml::Math::Clamp(r.p0, full.p0, full.p1);
    r.p1 = Rml::Math::Clamp(r.p1, r.p0, full.p1);
    return r;
}

void GpuRenderer::apply_scissor() {
    const Rml::Rectanglei r = window_rect();
    const SDL_Rect rect{r.Left(), r.Top(), r.Width(), r.Height()};
    SDL_SetGPUScissor(pass_, &rect);
}

void GpuRenderer::begin_layer_pass(u32 layer) {
    if (pass_ && pass_layer_ == static_cast<i32>(layer)) return;
    end_pass();
    SDL_GPUTexture* texture = layer_texture(layer);
    SDL_GPUColorTargetInfo color{};
    color.texture = texture;
    color.load_op = layer_needs_clear_[layer] ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    color.store_op = SDL_GPU_STOREOP_STORE;
    layer_needs_clear_[layer] = false;
    SDL_GPUDepthStencilTargetInfo ds{};
    ds.texture = depth_stencil_;
    ds.load_op = SDL_GPU_LOADOP_DONT_CARE;
    ds.store_op = SDL_GPU_STOREOP_DONT_CARE;
    ds.stencil_load_op = stencil_ready_ ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
    ds.stencil_store_op = SDL_GPU_STOREOP_STORE;
    ds.clear_stencil = 0;
    stencil_ready_ = true;
    pass_ = SDL_BeginGPURenderPass(cmd_, &color, 1, &ds);
    pass_layer_ = static_cast<i32>(layer);
    ++stats_.passes;
    SDL_GPUViewport viewport{0, 0, static_cast<float>(width_), static_cast<float>(height_), 0, 1};
    SDL_SetGPUViewport(pass_, &viewport);
    apply_scissor();
    SDL_SetGPUStencilReference(pass_, stencil_ref_);
}

void GpuRenderer::bind_pipeline(SDL_GPUGraphicsPipeline* pipeline) {
    if (pipeline != bound_pipeline_) {
        SDL_BindGPUGraphicsPipeline(pass_, pipeline);
        bound_pipeline_ = pipeline;
    }
}

void GpuRenderer::draw_geometry(const Geometry& geometry, Rml::Vector2f translation, SDL_GPUTexture* texture,
                                SDL_GPUGraphicsPipeline* pipeline) {
    bind_pipeline(pipeline);
    SDL_GPUBufferBinding vb{geometry.buffer, 0};
    SDL_BindGPUVertexBuffers(pass_, 0, &vb, 1);
    SDL_GPUBufferBinding ib{geometry.buffer, geometry.index_offset};
    SDL_BindGPUIndexBuffer(pass_, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    if (texture) {
        SDL_GPUTextureSamplerBinding binding{texture, linear_repeat_};
        SDL_BindGPUFragmentSamplers(pass_, 0, &binding, 1);
    }
    MainUniforms u{};
    std::memcpy(u.transform, transforms_[transform_].data(), sizeof(u.transform));
    u.translate[0] = translation.x;
    u.translate[1] = translation.y;
    SDL_PushGPUVertexUniformData(cmd_, 0, &u, sizeof(u));
    SDL_DrawGPUIndexedPrimitives(pass_, geometry.index_count, 1, 0, 0, 0);
    ++stats_.draws;
}

void GpuRenderer::fill_stencil(u8 value) {
    // Covers the whole layer whatever the scissor, like a stencil clear.
    const SDL_Rect full{0, 0, static_cast<int>(width_), static_cast<int>(height_)};
    SDL_SetGPUScissor(pass_, &full);
    bind_pipeline(stencil_fill_);
    SDL_SetGPUStencilReference(pass_, value);
    SDL_GPUTextureSamplerBinding binding{white_->texture, linear_clamp_};
    SDL_BindGPUFragmentSamplers(pass_, 0, &binding, 1);
    SDL_PushGPUVertexUniformData(cmd_, 0, &kQuadIdentity, sizeof(kQuadIdentity));
    SDL_PushGPUFragmentUniformData(cmd_, 0, &kTintOne, sizeof(kTintOne));
    SDL_DrawGPUPrimitives(pass_, 3, 1, 0, 0);
    apply_scissor();
}

void GpuRenderer::resolve_layer(u32 layer, SDL_GPUTexture* destination) {
    end_pass();
    SDL_GPUTexture* texture = layer_texture(layer);
    if (samples_ > 1 || layer_needs_clear_[layer]) {
        SDL_GPUColorTargetInfo color{};
        color.texture = texture;
        color.load_op = layer_needs_clear_[layer] ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        color.store_op = samples_ > 1 ? SDL_GPU_STOREOP_RESOLVE_AND_STORE : SDL_GPU_STOREOP_STORE;
        color.resolve_texture = destination;
        layer_needs_clear_[layer] = false;
        SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd_, &color, 1, nullptr));
        ++stats_.passes;
    }
    if (samples_ == 1) {
        const Rml::Rectanglei full = Rml::Rectanglei::FromSize({static_cast<int>(width_), static_cast<int>(height_)});
        copy_texture(texture, full, destination);
    }
}

void GpuRenderer::fullscreen(const FullscreenDraw& d) {
    end_pass();
    SDL_GPUColorTargetInfo color{};
    color.texture = d.target;
    color.load_op = SDL_GPU_LOADOP_LOAD;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd_, &color, 1, nullptr);
    ++stats_.passes;
    SDL_GPUViewport viewport{static_cast<float>(d.viewport.Left()), static_cast<float>(d.viewport.Top()),
                             static_cast<float>(d.viewport.Width()), static_cast<float>(d.viewport.Height()), 0, 1};
    SDL_SetGPUViewport(pass, &viewport);
    const SDL_Rect scissor{d.scissor.Left(), d.scissor.Top(), std::max(d.scissor.Width(), 0),
                           std::max(d.scissor.Height(), 0)};
    SDL_SetGPUScissor(pass, &scissor);
    SDL_BindGPUGraphicsPipeline(pass, d.pipeline);
    SDL_GPUTextureSamplerBinding bindings[2] = {{d.texture, linear_clamp_}, {d.texture2, linear_clamp_}};
    SDL_BindGPUFragmentSamplers(pass, 0, bindings, d.texture2 ? 2 : 1);
    if (d.vertex_uniforms) SDL_PushGPUVertexUniformData(cmd_, 0, d.vertex_uniforms, d.vertex_uniforms_size);
    if (d.fragment_uniforms) SDL_PushGPUFragmentUniformData(cmd_, 0, d.fragment_uniforms, d.fragment_uniforms_size);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

void GpuRenderer::blit(SDL_GPUTexture* source, Rml::Rectanglei from, SDL_GPUTexture* destination,
                       Rml::Rectanglei to) {
    if (from.Width() <= 0 || from.Height() <= 0 || to.Width() <= 0 || to.Height() <= 0) return;
    end_pass();
    SDL_GPUBlitInfo info{};
    info.source = {source, 0, 0, static_cast<Uint32>(from.Left()), static_cast<Uint32>(from.Top()),
                   static_cast<Uint32>(from.Width()), static_cast<Uint32>(from.Height())};
    info.destination = {destination, 0, 0, static_cast<Uint32>(to.Left()), static_cast<Uint32>(to.Top()),
                        static_cast<Uint32>(to.Width()), static_cast<Uint32>(to.Height())};
    info.load_op = SDL_GPU_LOADOP_LOAD;
    info.filter = SDL_GPU_FILTER_LINEAR;
    SDL_BlitGPUTexture(cmd_, &info);
    ++stats_.passes;
}

void GpuRenderer::copy_texture(SDL_GPUTexture* source, Rml::Rectanglei from, SDL_GPUTexture* destination) {
    if (from.Width() <= 0 || from.Height() <= 0) return;
    end_pass();
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd_);
    SDL_GPUTextureLocation src{};
    src.texture = source;
    src.x = static_cast<Uint32>(from.Left());
    src.y = static_cast<Uint32>(from.Top());
    SDL_GPUTextureLocation dst{};
    dst.texture = destination;
    SDL_CopyGPUTextureToTexture(copy, &src, &dst, static_cast<Uint32>(from.Width()),
                                static_cast<Uint32>(from.Height()), 1, false);
    SDL_EndGPUCopyPass(copy);
}

void GpuRenderer::render_blur(float sigma, u32 source_destination, u32 temp, Rml::Rectanglei window) {
    SDL_GPUTexture* sd = postprocess_[source_destination];
    SDL_GPUTexture* tmp = postprocess_[temp];
    const int w = static_cast<int>(width_), h = static_cast<int>(height_);
    const Rml::Rectanglei full = Rml::Rectanglei::FromSize({w, h});

    int pass_level = 0;
    sigma_to_parameters(sigma, pass_level, sigma);

    // Downscale by repeated halving with bilinear filtering; the image shrinks
    // towards the top-left corner and the scissor follows it.
    Rml::Rectanglei scissor = window;
    const Rml::Rectanglei half = Rml::Rectanglei::FromSize({std::max(w / 2, 1), std::max(h / 2, 1)});
    // With odd sizes, scale the UVs so fetches land exactly between texels.
    const QuadUniforms down = {{0, 0},
                               {w % 2 == 1 ? 1.0f - 1.0f / static_cast<float>(w) : 1.0f,
                                h % 2 == 1 ? 1.0f - 1.0f / static_cast<float>(h) : 1.0f}};
    for (int i = 0; i < pass_level; ++i) {
        scissor.p0 = (scissor.p0 + Rml::Vector2i(1)) / 2;
        scissor.p1 = Rml::Math::Max(scissor.p1 / 2, scissor.p0);
        const bool from_source = i % 2 == 0;
        FullscreenDraw d{from_source ? tmp : sd, copy_, from_source ? sd : tmp};
        d.viewport = half;
        d.scissor = scissor;
        d.vertex_uniforms = &down;
        d.vertex_uniforms_size = sizeof(down);
        d.fragment_uniforms = &kTintOne;
        d.fragment_uniforms_size = sizeof(kTintOne);
        fullscreen(d);
    }
    // The downscaled image must end up in temp.
    if (pass_level % 2 == 0) {
        FullscreenDraw d{tmp, copy_, sd};
        d.viewport = full;
        d.scissor = scissor;
        d.vertex_uniforms = &kQuadIdentity;
        d.vertex_uniforms_size = sizeof(kQuadIdentity);
        d.fragment_uniforms = &kTintOne;
        d.fragment_uniforms_size = sizeof(kTintOne);
        fullscreen(d);
    }

    BlurFragmentUniforms fu{};
    blur_weights(sigma, fu.weights);
    uv_limits(scissor, width_, height_, fu.uv_min, fu.uv_max);

    // Vertical pass: temp -> source_destination.
    BlurVertexUniforms vu{{0.0f, 1.0f / static_cast<float>(h)}, {0, 0}};
    FullscreenDraw v{sd, blur_, tmp};
    v.viewport = full;
    v.scissor = scissor;
    v.vertex_uniforms = &vu;
    v.vertex_uniforms_size = sizeof(vu);
    v.fragment_uniforms = &fu;
    v.fragment_uniforms_size = sizeof(fu);
    fullscreen(v);

    // A transparent 1px border around the region keeps the upscale below
    // from pulling in stale pixels.
    FullscreenDraw clear{tmp, copy_, sd};
    clear.viewport = full;
    clear.scissor = scissor.Extend(1).Intersect(full);
    clear.vertex_uniforms = &kQuadIdentity;
    clear.vertex_uniforms_size = sizeof(kQuadIdentity);
    clear.fragment_uniforms = &kTintZero;
    clear.fragment_uniforms_size = sizeof(kTintZero);
    fullscreen(clear);

    // Horizontal pass: source_destination -> temp.
    BlurVertexUniforms hu{{1.0f / static_cast<float>(w), 0.0f}, {0, 0}};
    FullscreenDraw hd{tmp, blur_, sd};
    hd.viewport = full;
    hd.scissor = scissor;
    hd.vertex_uniforms = &hu;
    hd.vertex_uniforms_size = sizeof(hu);
    hd.fragment_uniforms = &fu;
    hd.fragment_uniforms_size = sizeof(fu);
    fullscreen(hd);

    // Upscale back into the window. A second, exact power-of-two upscale
    // keeps the result steady when the region moves by a pixel.
    blit(tmp, scissor, sd, window);
    const Rml::Rectanglei exact =
        Rml::Rectanglei::FromCorners(scissor.p0 * (1 << pass_level), scissor.p1 * (1 << pass_level));
    if (exact.p0 != window.p0 || exact.p1 != window.p1) blit(tmp, scissor, sd, exact);
}

void GpuRenderer::render_filters(u32 first, u32 count) {
    const Rml::Rectanglei window = window_rect();
    const Rml::Rectanglei full = Rml::Rectanglei::FromSize({static_cast<int>(width_), static_cast<int>(height_)});
    for (u32 i = first; i < first + count; ++i) {
        const Filter* f = reinterpret_cast<const Filter*>(filter_lists_[i]);
        if (!f) continue;
        FullscreenDraw d{postprocess_[1], copy_, postprocess_[0]};
        d.viewport = full;
        d.scissor = window;
        d.vertex_uniforms = &kQuadIdentity;
        d.vertex_uniforms_size = sizeof(kQuadIdentity);
        switch (f->type) {
        case FilterType::Opacity: {
            const TintUniforms tint = {{f->value, f->value, f->value, f->value}};
            d.fragment_uniforms = &tint;
            d.fragment_uniforms_size = sizeof(tint);
            fullscreen(d);
            std::swap(postprocess_[0], postprocess_[1]);
            break;
        }
        case FilterType::Blur:
            render_blur(f->sigma, 0, 1, window);
            break;
        case FilterType::DropShadow: {
            ShadowUniforms su{};
            const Rml::Colourf c = to_colorf(f->color);
            for (int k = 0; k < 4; ++k) su.color[k] = c[k];
            uv_limits(window, width_, height_, su.uv_min, su.uv_max);
            const QuadUniforms shift = {{-f->offset.x / static_cast<float>(width_),
                                         -f->offset.y / static_cast<float>(height_)},
                                        {1, 1}};
            d.pipeline = drop_shadow_;
            d.vertex_uniforms = &shift;
            d.fragment_uniforms = &su;
            d.fragment_uniforms_size = sizeof(su);
            fullscreen(d);
            if (f->sigma >= 0.5f) render_blur(f->sigma, 1, 2, window);
            // The element itself on top of its shadow.
            FullscreenDraw over{postprocess_[1], copy_blend_, postprocess_[0]};
            over.viewport = full;
            over.scissor = window;
            over.vertex_uniforms = &kQuadIdentity;
            over.vertex_uniforms_size = sizeof(kQuadIdentity);
            over.fragment_uniforms = &kTintOne;
            over.fragment_uniforms_size = sizeof(kTintOne);
            fullscreen(over);
            std::swap(postprocess_[0], postprocess_[1]);
            break;
        }
        case FilterType::ColorMatrix: {
            d.pipeline = color_matrix_;
            d.fragment_uniforms = f->color_matrix.data();
            d.fragment_uniforms_size = sizeof(float) * 16;
            fullscreen(d);
            std::swap(postprocess_[0], postprocess_[1]);
            break;
        }
        case FilterType::MaskImage: {
            d.pipeline = blend_mask_;
            d.texture2 = mask_;
            fullscreen(d);
            std::swap(postprocess_[0], postprocess_[1]);
            break;
        }
        }
    }
}

void GpuRenderer::composite(const Op& op) {
    const u32 source = static_cast<u32>(op.rect.p0.x);
    const u32 destination = static_cast<u32>(op.rect.p0.y);
    resolve_layer(source, postprocess_[0]);
    render_filters(op.index, op.count);

    begin_layer_pass(destination);
    const bool blend = static_cast<Rml::BlendMode>(op.mode) == Rml::BlendMode::Blend;
    bind_pipeline(composite_[blend ? 1 : 0][clip_enabled_ ? 1 : 0]);
    SDL_GPUTextureSamplerBinding binding{postprocess_[0], linear_clamp_};
    SDL_BindGPUFragmentSamplers(pass_, 0, &binding, 1);
    SDL_PushGPUVertexUniformData(cmd_, 0, &kQuadIdentity, sizeof(kQuadIdentity));
    SDL_PushGPUFragmentUniformData(cmd_, 0, &kTintOne, sizeof(kTintOne));
    SDL_DrawGPUPrimitives(pass_, 3, 1, 0, 0);
    ++stats_.draws;
}

void GpuRenderer::replay(SDL_GPUCommandBuffer* cmd) {
    FORGE_ZONE();
    cmd_ = cmd;
    pass_ = nullptr;
    pass_layer_ = -1;
    bound_pipeline_ = nullptr;
    stencil_ready_ = false;
    scissor_ = Rml::Rectanglei::MakeInvalid();
    transform_ = 0;
    clip_enabled_ = false;
    stencil_ref_ = 0;
    u32 depth = 0;
    layer_texture(0);
    layer_needs_clear_[0] = true;

    for (const Op& op : ops_) {
        switch (op.type) {
        case OpType::Draw: {
            begin_layer_pass(depth);
            SDL_GPUTexture* texture = op.texture ? op.texture->texture : white_->texture;
            draw_geometry(*op.geometry, op.translation, texture,
                          geometry_[static_cast<int>(clip_enabled_ ? Stencil::Test : Stencil::None)]);
            break;
        }
        case OpType::DrawGradient: {
            begin_layer_pass(depth);
            bind_pipeline(gradient_[clip_enabled_ ? 1 : 0]);
            SDL_PushGPUFragmentUniformData(cmd_, 0, &op.shader->gradient, sizeof(GradientUniforms));
            draw_geometry(*op.geometry, op.translation, nullptr, gradient_[clip_enabled_ ? 1 : 0]);
            break;
        }
        case OpType::Scissor:
            scissor_ = op.rect;
            if (pass_) apply_scissor();
            break;
        case OpType::Transform:
            transform_ = op.index;
            break;
        case OpType::ClipEnable:
            clip_enabled_ = op.mode != 0;
            break;
        case OpType::ClipMask: {
            begin_layer_pass(depth);
            SDL_GPUTexture* white = white_->texture;
            switch (static_cast<Rml::ClipMaskOperation>(op.mode)) {
            case Rml::ClipMaskOperation::Set:
                fill_stencil(0);
                SDL_SetGPUStencilReference(pass_, 1);
                draw_geometry(*op.geometry, op.translation, white, geometry_[static_cast<int>(Stencil::WriteReplace)]);
                stencil_ref_ = 1;
                break;
            case Rml::ClipMaskOperation::SetInverse:
                fill_stencil(1);
                SDL_SetGPUStencilReference(pass_, 0);
                draw_geometry(*op.geometry, op.translation, white, geometry_[static_cast<int>(Stencil::WriteReplace)]);
                stencil_ref_ = 1;
                break;
            case Rml::ClipMaskOperation::Intersect:
                draw_geometry(*op.geometry, op.translation, white, geometry_[static_cast<int>(Stencil::WriteIncr)]);
                ++stencil_ref_;
                break;
            }
            SDL_SetGPUStencilReference(pass_, stencil_ref_);
            break;
        }
        case OpType::PushLayer:
            depth = op.index;
            layer_texture(depth);
            layer_needs_clear_[depth] = true;
            break;
        case OpType::PopLayer:
            if (depth > 0) --depth;
            break;
        case OpType::Composite:
            composite(op);
            break;
        case OpType::SaveTexture: {
            resolve_layer(depth, postprocess_[0]);
            const Rml::Rectanglei full =
                Rml::Rectanglei::FromSize({static_cast<int>(width_), static_cast<int>(height_)});
            Rml::Rectanglei from = op.rect.Intersect(full);
            from.p1 = Rml::Math::Max(from.p1, from.p0);
            copy_texture(postprocess_[0], from, op.texture->texture);
            break;
        }
        case OpType::SaveMask: {
            resolve_layer(depth, postprocess_[0]);
            copy_texture(postprocess_[0],
                         Rml::Rectanglei::FromSize({static_cast<int>(width_), static_cast<int>(height_)}), mask_);
            break;
        }
        }
    }
    end_pass();
}

} // namespace forge::ui
