#pragma once

// RmlUi's render interface on SDL_GPU, with everything RmlUi can draw: layers,
// clip masks (rounded overflow), filters (blur, drop-shadow, opacity, colour
// matrices), mask images and gradients. Edges are smoothed with MSAA.
//
// RmlUi calls this interface while it walks the documents, but SDL_GPU cannot
// upload data in the middle of a render pass. So calls are recorded, and
// render() first uploads everything new in one copy pass and then replays the
// recording into render passes.

#include "forge/core/types.h"

#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/Types.h>
#include <SDL3/SDL_gpu.h>

#include <unordered_map>
#include <vector>

namespace forge::ui {

class GpuRenderer final : public Rml::RenderInterface {
public:
    // msaa: requested sample count; falls back to what the GPU supports.
    bool init(SDL_GPUDevice* device, u32 msaa);
    void shutdown();

    // Recording window around Rml::Context::Render().
    void begin_frame(u32 width, u32 height);
    // Uploads, replays the recording into cmd and blends the result over
    // target (which already holds the world or a cleared background).
    void end_frame(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, SDL_GPUTextureFormat target_format);

    u32 msaa_samples() const { return samples_; }

    // Frame statistics for the profiler and tests.
    struct Stats {
        u32 draws = 0;
        u32 passes = 0;
        u32 uploads = 0; // geometries and textures uploaded this frame
        u32 layers = 0;  // layer textures allocated
    };
    const Stats& stats() const { return stats_; }

    // Rml::RenderInterface
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;

    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;

    void EnableClipMask(bool enable) override;
    void RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle geometry,
                          Rml::Vector2f translation) override;

    void SetTransform(const Rml::Matrix4f* transform) override;

    Rml::LayerHandle PushLayer() override;
    void CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination, Rml::BlendMode blend_mode,
                         Rml::Span<const Rml::CompiledFilterHandle> filters) override;
    void PopLayer() override;

    Rml::TextureHandle SaveLayerAsTexture() override;
    Rml::CompiledFilterHandle SaveLayerAsMaskImage() override;

    Rml::CompiledFilterHandle CompileFilter(const Rml::String& name, const Rml::Dictionary& parameters) override;
    void ReleaseFilter(Rml::CompiledFilterHandle filter) override;

    Rml::CompiledShaderHandle CompileShader(const Rml::String& name, const Rml::Dictionary& parameters) override;
    void RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry,
                      Rml::Vector2f translation, Rml::TextureHandle texture) override;
    void ReleaseShader(Rml::CompiledShaderHandle shader) override;

    struct Geometry;
    struct Texture;
    struct Filter;
    struct Shader;

private:
    enum class Stencil : u8 { None, Test, WriteReplace, WriteIncr, Count };
    enum class Blend : u8 { None, Premultiplied, Count };

    enum class OpType : u8 {
        Draw,
        DrawGradient,
        Scissor,
        Transform,
        ClipEnable,
        ClipMask,
        PushLayer,
        PopLayer,
        Composite,
        SaveTexture,
        SaveMask
    };
    struct Op {
        OpType type;
        u8 mode = 0; // clip operation, blend mode, enable flag
        u32 index = 0; // transform index, layer handles, filter range start
        u32 count = 0; // filter range size, destination layer
        Geometry* geometry = nullptr;
        Texture* texture = nullptr;
        Shader* shader = nullptr;
        Rml::Vector2f translation{};
        Rml::Rectanglei rect = Rml::Rectanglei::MakeInvalid();
    };

    struct Upload {
        SDL_GPUBuffer* buffer = nullptr;
        Texture* texture = nullptr;
        u32 offset = 0; // in the staging bytes
        u32 size = 0;
    };

    bool create_pipelines();
    SDL_GPUGraphicsPipeline* make_pipeline(SDL_GPUShader* vs, SDL_GPUShader* fs, SDL_GPUTextureFormat format,
                                           SDL_GPUSampleCount samples, bool depth_stencil, Blend blend,
                                           Stencil stencil, bool vertex_input);
    SDL_GPUGraphicsPipeline* output_pipeline(SDL_GPUTextureFormat format);
    bool ensure_targets(u32 width, u32 height);
    void destroy_targets();
    SDL_GPUTexture* layer_texture(u32 index);
    Texture* make_texture(u32 width, u32 height, bool sampler_only);

    void upload_pending(SDL_GPUCommandBuffer* cmd);
    void flush_releases();

    // Replay.
    void replay(SDL_GPUCommandBuffer* cmd);
    void begin_layer_pass(u32 layer);
    void end_pass();
    void apply_scissor();
    void bind_pipeline(SDL_GPUGraphicsPipeline* pipeline);
    void draw_geometry(const Geometry& geometry, Rml::Vector2f translation, SDL_GPUTexture* texture,
                       SDL_GPUGraphicsPipeline* pipeline);
    void fill_stencil(u8 value);
    void resolve_layer(u32 layer, SDL_GPUTexture* destination);
    void composite(const Op& op);
    void render_filters(u32 first, u32 count);
    void render_blur(float sigma, u32 source_destination, u32 temp, Rml::Rectanglei window);
    struct FullscreenDraw {
        SDL_GPUTexture* target;
        SDL_GPUGraphicsPipeline* pipeline;
        SDL_GPUTexture* texture;
        SDL_GPUTexture* texture2 = nullptr;
        Rml::Rectanglei viewport = Rml::Rectanglei::MakeInvalid();
        Rml::Rectanglei scissor = Rml::Rectanglei::MakeInvalid();
        const void* vertex_uniforms = nullptr;
        u32 vertex_uniforms_size = 0;
        const void* fragment_uniforms = nullptr;
        u32 fragment_uniforms_size = 0;
    };
    void fullscreen(const FullscreenDraw& draw);
    void blit(SDL_GPUTexture* source, Rml::Rectanglei from, SDL_GPUTexture* destination, Rml::Rectanglei to);
    void copy_texture(SDL_GPUTexture* source, Rml::Rectanglei from, SDL_GPUTexture* destination);
    Rml::Rectanglei window_rect() const; // the scissor region, or the whole viewport

    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUSampleCount sample_count_ = SDL_GPU_SAMPLECOUNT_1;
    u32 samples_ = 1;
    SDL_GPUTextureFormat ds_format_ = SDL_GPU_TEXTUREFORMAT_INVALID;
    static constexpr SDL_GPUTextureFormat kColorFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

    SDL_GPUShader* vs_main_ = nullptr;
    SDL_GPUShader* vs_fullscreen_ = nullptr;
    SDL_GPUShader* vs_blur_ = nullptr;
    SDL_GPUShader* fs_texture_ = nullptr;
    SDL_GPUShader* fs_gradient_ = nullptr;
    SDL_GPUShader* fs_passthrough_ = nullptr;
    SDL_GPUShader* fs_color_matrix_ = nullptr;
    SDL_GPUShader* fs_blend_mask_ = nullptr;
    SDL_GPUShader* fs_drop_shadow_ = nullptr;
    SDL_GPUShader* fs_blur_ = nullptr;

    // Into layers (multisampled, with the shared stencil buffer).
    SDL_GPUGraphicsPipeline* geometry_[static_cast<int>(Stencil::Count)] = {};
    SDL_GPUGraphicsPipeline* gradient_[2] = {};        // Stencil None / Test
    SDL_GPUGraphicsPipeline* composite_[2][2] = {};    // [blend][stencil test]
    SDL_GPUGraphicsPipeline* stencil_fill_ = nullptr;
    // Into single-sampled postprocess textures.
    SDL_GPUGraphicsPipeline* copy_ = nullptr;
    SDL_GPUGraphicsPipeline* copy_blend_ = nullptr;
    SDL_GPUGraphicsPipeline* color_matrix_ = nullptr;
    SDL_GPUGraphicsPipeline* blend_mask_ = nullptr;
    SDL_GPUGraphicsPipeline* blur_ = nullptr;
    SDL_GPUGraphicsPipeline* drop_shadow_ = nullptr;
    std::unordered_map<u32, SDL_GPUGraphicsPipeline*> output_; // by target format

    SDL_GPUSampler* linear_clamp_ = nullptr;
    SDL_GPUSampler* linear_repeat_ = nullptr;
    Texture* white_ = nullptr;

    // Render targets, sized to the viewport.
    u32 target_width_ = 0, target_height_ = 0;
    std::vector<SDL_GPUTexture*> layers_;
    SDL_GPUTexture* depth_stencil_ = nullptr;
    SDL_GPUTexture* postprocess_[3] = {};
    SDL_GPUTexture* mask_ = nullptr;

    // Recording.
    u32 width_ = 0, height_ = 0;
    std::vector<Op> ops_;
    std::vector<Rml::Matrix4f> transforms_; // [0] is the plain projection
    std::vector<Rml::CompiledFilterHandle> filter_lists_;
    u32 layer_depth_ = 0; // while recording, to hand out layer handles
    bool recording_ = false;

    // Uploads and releases waiting for the next end_frame().
    std::vector<u8> staging_;
    std::vector<Upload> uploads_;
    SDL_GPUTransferBuffer* transfer_ = nullptr;
    u32 transfer_size_ = 0;
    std::vector<Geometry*> release_geometry_;
    std::vector<Texture*> release_textures_;
    std::vector<Filter*> release_filters_;
    std::vector<Shader*> release_shaders_;

    // Replay state.
    SDL_GPUCommandBuffer* cmd_ = nullptr;
    SDL_GPURenderPass* pass_ = nullptr;
    i32 pass_layer_ = -1;
    SDL_GPUGraphicsPipeline* bound_pipeline_ = nullptr;
    std::vector<bool> layer_needs_clear_;
    std::vector<u32> layer_stack_;
    bool stencil_ready_ = false;
    Rml::Rectanglei scissor_ = Rml::Rectanglei::MakeInvalid();
    u32 transform_ = 0;
    bool clip_enabled_ = false;
    u8 stencil_ref_ = 0;

    Stats stats_{};
};

} // namespace forge::ui
