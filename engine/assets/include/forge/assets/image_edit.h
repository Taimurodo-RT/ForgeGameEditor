#pragma once

// Edits of a picture in memory for the «Изображения» tab: the pixel editor (brush, line, fill, eyedropper, the
// picture's colours), the transformations (turn, mirror, size, canvas, trim, the frame grid) and the effects.
//
// A picture is a CookedTexture: RGBA 8 bits, rows top to bottom, nothing lost between edits. An area is the part an
// edit may touch: the whole picture, or one frame of its strip (frame_rect). Nothing outside the area changes, so a
// frame's edit leaves the other frames byte for byte as they were. A strip's frames are equal ones side by side,
// left to right, as the game cuts them («Кадров в картинке»).
//
// Edits in place return the rectangle they changed (empty: nothing), for the tab's history.

#include "forge/assets/asset_pipeline.h"

#include <functional>
#include <span>
#include <string>
#include <vector>

namespace forge::assets {

struct Rgba {
    u8 r = 0, g = 0, b = 0, a = 0;
    bool operator==(const Rgba&) const = default;
};

struct PixelRect {
    u32 x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w == 0 || h == 0; }
    bool contains(i64 px, i64 py) const { return px >= x && py >= y && px < static_cast<i64>(x) + w && py < static_cast<i64>(y) + h; }
    bool operator==(const PixelRect&) const = default;
};

// The whole picture as an area.
PixelRect whole_rect(const CookedTexture& image);
// The smallest rectangle holding both (an empty one adds nothing).
PixelRect united(const PixelRect& a, const PixelRect& b);
// Frame `frame` of a strip of `frames` equal frames side by side, as the game cuts it; empty when the width does not
// divide by `frames` or there is no such frame.
PixelRect frame_rect(const CookedTexture& image, u32 frames, u32 frame);

Rgba pixel_at(const CookedTexture& image, u32 x, u32 y);
void set_pixel(CookedTexture& image, u32 x, u32 y, Rgba color);

// The pixels of a rectangle (rows top to bottom), and put back.
std::vector<u8> copy_rect(const CookedTexture& image, const PixelRect& rect);
void paste_rect(CookedTexture& image, const PixelRect& rect, std::span<const u8> pixels);

// --- the pixel editor ---

// A square brush of `size` pixels (1..4) at (x, y): its top-left pixel is (x - (size - 1) / 2, y - (size - 1) / 2).
// Pixels outside `area` stay. The eraser is the brush with alpha 0 ({0, 0, 0, 0}).
PixelRect paint(CookedTexture& image, i32 x, i32 y, u32 size, Rgba color, const PixelRect& area);
// The brush along a line from one point to the other, both ends painted (Bresenham: every step one pixel).
PixelRect paint_line(CookedTexture& image, i32 x0, i32 y0, i32 x1, i32 y1, u32 size, Rgba color, const PixelRect& area);
// Every pixel of exactly the colour at (x, y) (all four channels) joined to it by sides inside `area` takes `color`.
PixelRect fill(CookedTexture& image, u32 x, u32 y, Rgba color, const PixelRect& area);
// The picture's colours by how many pixels have them, the most first (ties: the lower value first), up to `max`.
// Fully transparent pixels are no colour.
std::vector<Rgba> picture_colors(const CookedTexture& image, usize max = 64);

// --- transformations ---

enum class Turn : u8 { Cw, Ccw, Half, FlipH, FlipV };
// The area turned or mirrored in place. A quarter turn keeps the area only when it is square: otherwise false, and
// why says so (a frame of a strip cannot grow out of its place).
bool turn_area(CookedTexture& image, const PixelRect& area, Turn turn, std::string* why = nullptr);
// The whole picture turned or mirrored: a quarter turn swaps its width and height.
CookedTexture turned(const CookedTexture& image, Turn turn);
// Another size: the nearest pixel (pixel art: no new colours), or smooth (the area's average when it shrinks, between
// neighbours when it grows; colours weighted by how opaque they are).
CookedTexture resized(const CookedTexture& image, u32 width, u32 height, bool smooth);
// Where the old picture stays on a new canvas (Krita's and Aseprite's nine points).
enum class Anchor : u8 { TopLeft, Top, TopRight, Left, Center, Right, BottomLeft, Bottom, BottomRight };
// A canvas of another size: the picture where the anchor says, new pixels transparent, what is outside cut off.
CookedTexture with_canvas(const CookedTexture& image, u32 width, u32 height, Anchor anchor);
// Where the area's pixels that are not fully transparent are (empty: none).
PixelRect opaque_bounds(const CookedTexture& image, const PixelRect& area);
CookedTexture cropped(const CookedTexture& image, const PixelRect& rect);
// A strip of `frames` frames, each frame through `op` and the results side by side again in their order. The results
// must all be one size (else false, why says so); the strip's width must divide by `frames`.
bool each_frame(const CookedTexture& strip, u32 frames, const std::function<CookedTexture(const CookedTexture&)>& op,
                CookedTexture& out, std::string* why = nullptr);
// «Сетка кадров»: a sheet of frames frame_w × frame_h, `columns` of them in a row, rows down to its height → a strip
// the game reads, frames left to right. Order: the sheet's first row left to right, then the next row. Columns past
// `columns` are left out. The sheet's sides must divide by the frame's (else false, why says so). frames: how many.
bool sheet_to_strip(const CookedTexture& sheet, u32 frame_w, u32 frame_h, u32 columns, CookedTexture& out, u32* frames = nullptr,
                    std::string* why = nullptr);

// --- effects ---

enum class EffectKind : u8 {
    BrightnessContrast, // a: brightness −100..100, b: contrast −100..100
    HueSaturation,      // a: hue −180..180 (degrees), b: saturation −100..100, c: lightness −100..100
    Invert,             // the colour, not the alpha
    Grayscale,          // the luma (0.299, 0.587, 0.114)
    Outline,            // a fully transparent pixel beside (by a side) one that is not takes `color`
    AlphaThreshold,     // a: 1..255; alpha a or more → 255, less → 0
    ReduceColors,       // a: 2..64 colours (median cut of the area's colours, each pixel the nearest; no dithering)
    ReplaceColor,       // `color` → `to`; a: tolerance 0..255 (each channel, alpha too; 0: exactly)
};
struct Effect {
    EffectKind kind = EffectKind::Invert;
    i32 a = 0, b = 0, c = 0;
    Rgba color, to;
};
// The effect on the area's pixels (colour effects leave fully transparent pixels as they are).
PixelRect apply_effect(CookedTexture& image, const PixelRect& area, const Effect& effect);

} // namespace forge::assets
