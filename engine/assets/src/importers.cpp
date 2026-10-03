#include "forge/assets/asset_pipeline.h"

#include "forge/data/binary.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO // decode from memory only
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wpedantic"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <stb_image.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <cstring>

FORGE_REFLECT(forge::assets::AssetMeta, 1) {
    t.field("id", &forge::assets::AssetMeta::id).read_only();
    t.field("tags", &forge::assets::AssetMeta::tags);
}

FORGE_REFLECT(forge::assets::CookedTexture, 1) {
    t.field("width", &forge::assets::CookedTexture::width);
    t.field("height", &forge::assets::CookedTexture::height);
    t.field("rgba8", &forge::assets::CookedTexture::rgba8);
}

namespace forge::assets {

namespace {

// PNG, JPEG, BMP, TGA, GIF (first frame), PSD (merged) → RGBA8.
// Block compression (BC7) for the GPU comes with the renderer in step 4.
class ImageImporter final : public Importer {
public:
    const char* name() const override { return "image"; }
    u32 version() const override { return 1; }
    bool handles(std::string_view ext) const override {
        return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga" || ext == ".gif" ||
               ext == ".psd";
    }
    void cook(const CookInput& in, CookOutput& out) const override {
        if (in.source.size() > static_cast<usize>(INT32_MAX)) {
            out.error = "image file is too large";
            return;
        }
        int w = 0, h = 0, channels = 0;
        stbi_uc* pixels = stbi_load_from_memory(in.source.data(), static_cast<int>(in.source.size()), &w, &h, &channels, 4);
        if (!pixels) {
            out.error = std::string("cannot decode image: ") + stbi_failure_reason();
            return;
        }
        CookedTexture tex;
        tex.width = static_cast<u32>(w);
        tex.height = static_cast<u32>(h);
        tex.rgba8.resize(static_cast<usize>(w) * static_cast<usize>(h) * 4);
        std::memcpy(tex.rgba8.data(), pixels, tex.rgba8.size());
        stbi_image_free(pixels);
        out.cooked = data::to_binary(tex);
    }
};

// Anything without a dedicated importer is stored as is, so it is still
// indexed, searchable and shipped with the game.
class CopyImporter final : public Importer {
public:
    const char* name() const override { return "file"; }
    u32 version() const override { return 1; }
    bool handles(std::string_view) const override { return true; }
    void cook(const CookInput& in, CookOutput& out) const override { out.cooked.assign(in.source.begin(), in.source.end()); }
};

} // namespace

void AssetPipeline::add_default_importers() {
    add_importer(std::make_unique<ImageImporter>());
    add_importer(std::make_unique<CopyImporter>()); // last: catches everything
}

} // namespace forge::assets
