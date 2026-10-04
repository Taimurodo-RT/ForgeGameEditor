#include "forge/assets/asset_pipeline.h"
#include "forge/assets/image.h"

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
// Static: the renderer has its own copy for screenshots.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include <stb_image_write.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
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
        CookedTexture tex;
        if (!decode_image(in.source, tex, &out.error)) return;
        out.cooked = data::to_binary(tex);
    }
};

// Sounds are shipped as they are; the type lets the library filter and play them.
class AudioImporter final : public Importer {
public:
    const char* name() const override { return "audio"; }
    u32 version() const override { return 1; }
    bool handles(std::string_view ext) const override {
        return ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac";
    }
    void cook(const CookInput& in, CookOutput& out) const override { out.cooked.assign(in.source.begin(), in.source.end()); }
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
    add_importer(std::make_unique<AudioImporter>());
    add_importer(std::make_unique<CopyImporter>()); // last: catches everything
}

// --- pictures --------------------------------------------------------------------

bool decode_image(std::span<const u8> bytes, CookedTexture& out, std::string* error) {
    if (bytes.size() > static_cast<usize>(INT32_MAX)) {
        if (error) *error = "image file is too large";
        return false;
    }
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, 4);
    if (!pixels) {
        if (error) *error = std::string("cannot decode image: ") + stbi_failure_reason();
        return false;
    }
    out.width = static_cast<u32>(w);
    out.height = static_cast<u32>(h);
    out.rgba8.resize(static_cast<usize>(w) * static_cast<usize>(h) * 4);
    std::memcpy(out.rgba8.data(), pixels, out.rgba8.size());
    stbi_image_free(pixels);
    return true;
}

bool can_encode_image(std::string_view ext) {
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga";
}

bool encode_image(const CookedTexture& image, std::string_view ext, std::vector<u8>& out) {
    out.clear();
    if (image.width == 0 || image.height == 0 || image.rgba8.size() < static_cast<usize>(image.width) * image.height * 4)
        return false;
    auto sink = [](void* context, void* data, int size) {
        auto* bytes = static_cast<std::vector<u8>*>(context);
        const u8* p = static_cast<const u8*>(data);
        bytes->insert(bytes->end(), p, p + size);
    };
    const int w = static_cast<int>(image.width), h = static_cast<int>(image.height);
    const void* px = image.rgba8.data();
    int ok = 0;
    if (ext == ".png") ok = stbi_write_png_to_func(sink, &out, w, h, 4, px, w * 4);
    else if (ext == ".jpg" || ext == ".jpeg") ok = stbi_write_jpg_to_func(sink, &out, w, h, 4, px, 92);
    else if (ext == ".bmp") ok = stbi_write_bmp_to_func(sink, &out, w, h, 4, px);
    else if (ext == ".tga") ok = stbi_write_tga_to_func(sink, &out, w, h, 4, px);
    return ok != 0 && !out.empty();
}

CookedTexture transform_image(const CookedTexture& in, ImageOp op) {
    const u32 w = in.width, h = in.height;
    CookedTexture out;
    auto pixel = [&](u32 x, u32 y) { return in.rgba8.data() + (static_cast<usize>(y) * w + x) * 4; };
    auto put = [&](u32 x, u32 y, const u8* p) { std::memcpy(out.rgba8.data() + (static_cast<usize>(y) * out.width + x) * 4, p, 4); };
    switch (op) {
    case ImageOp::RotateCw:
    case ImageOp::RotateCcw:
        out.width = h;
        out.height = w;
        out.rgba8.resize(in.rgba8.size());
        for (u32 y = 0; y < h; ++y)
            for (u32 x = 0; x < w; ++x) {
                if (op == ImageOp::RotateCw) put(h - 1 - y, x, pixel(x, y));
                else put(y, w - 1 - x, pixel(x, y));
            }
        break;
    case ImageOp::FlipH:
    case ImageOp::FlipV:
        out.width = w;
        out.height = h;
        out.rgba8.resize(in.rgba8.size());
        for (u32 y = 0; y < h; ++y)
            for (u32 x = 0; x < w; ++x) {
                if (op == ImageOp::FlipH) put(w - 1 - x, y, pixel(x, y));
                else put(x, h - 1 - y, pixel(x, y));
            }
        break;
    case ImageOp::Half: {
        out.width = std::max(1u, w / 2);
        out.height = std::max(1u, h / 2);
        out.rgba8.resize(static_cast<usize>(out.width) * out.height * 4);
        for (u32 y = 0; y < out.height; ++y)
            for (u32 x = 0; x < out.width; ++x) {
                // Average a 2 × 2 block, colours weighted by how opaque they are.
                u32 sum[4] = {0, 0, 0, 0};
                for (u32 dy = 0; dy < 2; ++dy)
                    for (u32 dx = 0; dx < 2; ++dx) {
                        const u8* p = pixel(std::min(w - 1, x * 2 + dx), std::min(h - 1, y * 2 + dy));
                        for (int c = 0; c < 3; ++c) sum[c] += static_cast<u32>(p[c]) * p[3];
                        sum[3] += p[3];
                    }
                u8 q[4];
                for (int c = 0; c < 3; ++c) q[c] = static_cast<u8>(sum[3] ? sum[c] / sum[3] : 0);
                q[3] = static_cast<u8>(sum[3] / 4);
                put(x, y, q);
            }
        break;
    }
    case ImageOp::Double:
        out.width = w * 2;
        out.height = h * 2;
        out.rgba8.resize(static_cast<usize>(out.width) * out.height * 4);
        for (u32 y = 0; y < out.height; ++y)
            for (u32 x = 0; x < out.width; ++x) put(x, y, pixel(x / 2, y / 2));
        break;
    }
    return out;
}

CookedTexture fit_image(const CookedTexture& in, u32 box) {
    CookedTexture out;
    out.width = out.height = box;
    out.rgba8.assign(static_cast<usize>(box) * box * 4, 0);
    if (in.width == 0 || in.height == 0 || box == 0) return out;
    const u32 big = std::max(in.width, in.height);
    if (big <= box) {
        // Whole-factor growth, nearest neighbour.
        const u32 k = box / big;
        const u32 w = in.width * k, h = in.height * k, ox = (box - w) / 2, oy = (box - h) / 2;
        for (u32 y = 0; y < h; ++y)
            for (u32 x = 0; x < w; ++x)
                std::memcpy(out.rgba8.data() + (static_cast<usize>(oy + y) * box + ox + x) * 4,
                            in.rgba8.data() + (static_cast<usize>(y / k) * in.width + x / k) * 4, 4);
        return out;
    }
    // Shrink: each target pixel averages up to 4 × 4 samples of its area.
    const f64 scale = static_cast<f64>(big) / box;
    const u32 w = std::max(1u, static_cast<u32>(in.width / scale)), h = std::max(1u, static_cast<u32>(in.height / scale));
    const u32 ox = (box - std::min(box, w)) / 2, oy = (box - std::min(box, h)) / 2;
    const u32 taps = std::min<u32>(4, static_cast<u32>(scale) + 1);
    for (u32 y = 0; y < h && y < box; ++y)
        for (u32 x = 0; x < w && x < box; ++x) {
            u32 sum[4] = {0, 0, 0, 0}, n = 0;
            for (u32 ty = 0; ty < taps; ++ty)
                for (u32 tx = 0; tx < taps; ++tx) {
                    const u32 sx = std::min(in.width - 1, static_cast<u32>((x + (tx + 0.5) / taps) * scale));
                    const u32 sy = std::min(in.height - 1, static_cast<u32>((y + (ty + 0.5) / taps) * scale));
                    const u8* p = in.rgba8.data() + (static_cast<usize>(sy) * in.width + sx) * 4;
                    for (int c = 0; c < 3; ++c) sum[c] += static_cast<u32>(p[c]) * p[3];
                    sum[3] += p[3];
                    ++n;
                }
            u8* q = out.rgba8.data() + (static_cast<usize>(oy + y) * box + ox + x) * 4;
            for (int c = 0; c < 3; ++c) q[c] = static_cast<u8>(sum[3] ? sum[c] / sum[3] : 0);
            q[3] = static_cast<u8>(sum[3] / n);
        }
    return out;
}

} // namespace forge::assets
