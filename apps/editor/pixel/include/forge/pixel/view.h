#pragma once

// The vector view of «Векторная» as numbers (part 3в of docs/models/движение-модель.md, «Просмотр»): its zoom in steps
// of ×1.25 from ×1, the picture fitted in, to ×16; the pan that keeps the picture over the view; the grid's step and
// lines; the coordinates under the mouse as the code writes them.
//
//   PictureView v{.width = 288, .height = 288, .box = {0, 72, 288, 216}};  // the picture's box at ×1
//   v.zoom_to(3, {100, 100});    // ×1.95, the point under (100, 100) staying there as far as the edges let it
//   v.affine() * rig.to_pixels(288, 288)  // picture units to the view's points at this zoom

#include "forge/pixel/rig.h"

#include "forge/assets/asset_pipeline.h"

#include <string>

namespace forge::pixel {

// The last step: ×16. Steps 0 … 12 are 1.25 to their power.
inline constexpr i32 kZoomTop = 13;
f64 zoom_of(i32 step);
// The zoom as the line under the views writes it: "×1,56", "×16"; "" at ×1.
std::string zoom_words(i32 step);

struct PictureView {
    f64 width = 0, height = 0;    // the view's points
    f64 box[4] = {0, 0, 0, 0};    // the picture's box at ×1 in them: x0, y0, x1, y1
    i32 step = 0;                 // 0 … kZoomTop
    f64 ox = 0, oy = 0;           // a point p of the view at ×1 stands at zoom · p + (ox, oy)

    f64 zoom() const { return zoom_of(step); }
    // The view at ×1 to the view at this zoom.
    Affine affine() const;
    // Step `to` (kept in 0 … kZoomTop), the view's point `at` staying put; then the edges, which win.
    void zoom_to(i32 to, vector::Point at);
    // Along by dx, dy points; the edges win.
    void pan(f64 dx, f64 dy);
    // ×1, fitted.
    void fit();
    // Along each axis: where the picture is wider than the view its edge does not come in from the view's; where it is
    // not, it stands in the middle. At ×1 that is no pan.
    void clamp();
};

// The grid's step in picture units: 1, 2, 5, 10, 20, 50 … the least whose cell is `cell` points or more.
f64 grid_step(f64 points_per_unit, f64 cell = 8);
// Grid lines over a drawing whose picture units go to its pixels by `px`: a line every `step` units, `line` pixels
// wide, #808080 over what is drawn at 40 %, at 80 % through x = 0 and y = 0. A line at x stands in pixel columns
// floor(px(x)) … + line − 1, and so for y.
void draw_grid(assets::CookedTexture& image, const Affine& px, f64 step, u32 line);

// A point of the picture as the code writes it, to tenths: "12.5, -3" ("-0" as "0").
std::string coords_words(vector::Point p);
// "#rrggbb" of a pixel; "" for one with nothing in it (alpha 0).
std::string color_words(const u8 rgba[4]);

} // namespace forge::pixel
