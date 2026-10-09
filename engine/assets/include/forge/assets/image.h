#pragma once

// Pictures in memory for the asset library: read and write the usual files,
// the simple edits (turn, mirror, halve, double) and previews.

#include "forge/assets/asset_pipeline.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace forge::assets {

// PNG, JPEG, BMP, TGA, GIF (first frame), PSD (merged), WebP (first frame) → RGBA8.
bool decode_image(std::span<const u8> bytes, CookedTexture& out, std::string* error = nullptr);
// Whether encode_image can write this extension (lowercase, with dot).
bool can_encode_image(std::string_view extension);
// .png, .jpg/.jpeg (quality 92), .bmp, .tga, .webp (lossless).
bool encode_image(const CookedTexture& image, std::string_view extension, std::vector<u8>& out);

// Deflate data → bytes: with zlib's two-byte header and checksum (zlib_header)
// or without them (as in gzip). False when the data is not valid.
bool inflate(std::span<const u8> bytes, bool zlib_header, std::vector<u8>& out);

enum class ImageOp : u8 { RotateCw, RotateCcw, FlipH, FlipV, Half, Double };
CookedTexture transform_image(const CookedTexture& image, ImageOp op);

// Fits the picture into a box × box square, centred on transparency. Small
// pictures grow by a whole factor (pixel art stays sharp), big ones shrink
// by averaging.
CookedTexture fit_image(const CookedTexture& image, u32 box);

} // namespace forge::assets
