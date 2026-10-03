#pragma once

// The list of sprites to draw this frame. Game code (any number of threads at
// once) appends sprites; finish() copies them into GPU upload memory and, when
// asked, sorts the ones on screen into draw order. The sort produces a list of
// indices, so the 32-byte sprites themselves are only ever copied in order.
//
// Positions are in tiles relative to an origin chosen at begin() (normally
// the camera), so floats stay precise anywhere in a huge world.

#include "forge/core/math.h"
#include "forge/core/types.h"

#include <atomic>
#include <memory>
#include <vector>

namespace forge::render {

// Exactly what the GPU reads per sprite: 32 bytes.
struct Sprite {
    f32 x = 0, y = 0; // centre, tiles from the batch origin
    f32 w = 1, h = 1; // size in tiles; negative w mirrors the picture
    f32 angle = 0;    // radians, clockwise on screen
    u32 frame = 0;    // picture in the sprite sheet
    u32 color = 0xffffffffu; // tint, RGBA8 (see pack_color); alpha fades the sprite
    u32 order = 0;    // draw order: lower first (see draw_order)
};
static_assert(sizeof(Sprite) == 32);

inline u32 pack_color(u8 r, u8 g, u8 b, u8 a = 255) {
    return static_cast<u32>(r) | static_cast<u32>(g) << 8 | static_cast<u32>(b) << 16 | static_cast<u32>(a) << 24;
}

// Order for top-down games: by layer first, then by the sprite's feet, so
// objects lower on screen are drawn over the ones behind them. y_from_top is
// in tiles from the top of the view (0 .. view height). Only the low 24 bits
// are used, so sorting takes at most three passes (two with one layer).
inline u32 draw_order(u8 layer, f32 y_from_top, f32 view_height) {
    const f32 t = view_height > 0 ? y_from_top / view_height : 0.0f;
    const f32 clamped = t < 0 ? 0 : (t > 1 ? 1 : t);
    return static_cast<u32>(layer) << 16 | static_cast<u32>(clamped * 65535.0f);
}

// A rectangle in tiles relative to the batch origin.
struct ViewBox {
    f32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

struct SpriteBatchStats {
    u32 submitted = 0; // pushed this frame
    u32 drawn = 0;     // instances the GPU will draw
    u32 dropped = 0;   // did not fit into the capacity
};

// What finish() wrote.
struct SpriteList {
    u32 sprites = 0; // copied to out_sprites, in push order
    u32 draws = 0;   // sorted: entries in out_indices; unsorted: same as sprites
};

class SpriteBatch {
public:
    // Starts a frame. capacity is the most sprites that can be pushed.
    void begin(f64 origin_x, f64 origin_y, u32 capacity);
    f64 origin_x() const { return origin_x_; }
    f64 origin_y() const { return origin_y_; }

    // Room for count sprites, safe from any thread; nullptr when the batch is
    // full (those sprites are counted as dropped).
    Sprite* push(u32 count);
    void push(const Sprite& sprite) {
        if (Sprite* s = push(1)) *s = sprite;
    }
    u32 size() const;

    // Copies the sprites to out_sprites (at most capacity). When sorted, also
    // writes to out_indices the sprites that touch the view, in draw order
    // (sprites with the same order keep the order they were pushed in).
    // Unsorted, every sprite is drawn in push order and off-screen ones are
    // left to the GPU to clip.
    SpriteList finish(const ViewBox& view, bool sorted, Sprite* out_sprites, u32* out_indices, u32 capacity);

    const SpriteBatchStats& stats() const { return stats_; }

private:
    std::unique_ptr<Sprite[]> items_;
    u32 capacity_ = 0;
    std::atomic<u32> count_{0};
    f64 origin_x_ = 0, origin_y_ = 0;
    std::vector<u64> keys_, scratch_;
    std::vector<u32> block_counts_;
    SpriteBatchStats stats_;
};

} // namespace forge::render
