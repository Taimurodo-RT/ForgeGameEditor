#pragma once

// A vector character posed and drawn by ThorVG («Векторная», step 2 of docs/models/вектор-модель.md). The SVG is
// loaded once; a pose moves its parts (elements with an id) around their pivots and morphs its shapes, always from the
// rest pose, so any frame is drawn at once. Only the editor links this: games get the baked picture.
//
// Coordinates are the SVG's own user units (inside its viewBox), the ones the author draws in. Pivots and the moves of
// a pose are given in them.

#include "forge/assets/asset_pipeline.h"
#include "forge/core/types.h"
#include "forge/pixel/svg.h"
#include "forge/vector/path.h"

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace forge::pixel {

// SVG's matrix(a b c d e f): x' = a x + c y + e, y' = b x + d y + f.
struct Affine {
    f64 a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    vector::Point apply(vector::Point p) const { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
    bool operator==(const Affine&) const = default;
};
Affine operator*(const Affine& m, const Affine& n); // m after n
Affine inverse(const Affine& m);                    // identity for a matrix that has none
Affine translation(f64 x, f64 y);

struct Rgba8 {
    u8 r = 0, g = 0, b = 0, a = 255;
    bool operator==(const Rgba8&) const = default;
};

struct Part {
    std::string id;
    i32 parent = -1;  // the nearest part above it; -1 when only the picture is
    i32 element = -1; // in Rig::outline()
    bool shape = false; // a shape (path, rect, …) rather than a group
    // Its rest bounds in picture units: x0, y0, x1, y1.
    f64 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// What a pose does to one part, from its rest. Turning and scaling are around the part's pivot; a part inside another
// moves with it, as in SVG.
struct PartPose {
    f64 x = 0, y = 0;       // picture units
    f64 rot = 0;            // degrees, clockwise on screen (SVG's rotate)
    f64 scale = 1, sx = 1, sy = 1;
    f64 opacity = 1;        // times its own
    bool has_fill = false;  // the fill of the part (every shape of a group) becomes `fill`
    Rgba8 fill;
    f64 fill_k = 1;         // how far: 0 its own colour, 1 `fill` (a gradient takes it from 0.5 on)
};

// A shape morphed into another element of the same SVG (it may be hidden): k = 0 is the shape, 1 the target.
struct ShapeMorph {
    std::string target;
    f64 k = 0;
};

struct Pose {
    std::map<std::string, PartPose> parts;
    std::map<std::string, ShapeMorph> morphs;
};

struct BakeOptions {
    std::vector<Rgba8> palette; // empty: the SVG's own colours; else every pixel the nearest of these
    bool outline = false;       // a 1-pixel outline of `outline_color` round the figure
    Rgba8 outline_color{0, 0, 0, 255};
};

class Rig {
public:
    Rig();
    ~Rig();
    Rig(Rig&&) noexcept;
    Rig& operator=(Rig&&) noexcept;

    // False with why (Russian) when the text is not SVG or ThorVG does not load it.
    bool load(std::string_view svg, std::string& why);
    bool loaded() const;
    const SvgOutline& outline() const;
    const std::vector<Part>& parts() const;
    i32 part(std::string_view id) const; // -1 when no part has that id
    // The picture's own size in its units (its viewBox, else width and height).
    f64 width() const;
    f64 height() const;

    // A part's pivot (picture units); the centre of its rest bounds until set.
    void set_pivot(std::string_view id, vector::Point at);
    vector::Point pivot(i32 part) const;

    // Puts the picture in the pose, from the rest pose. Moves are rounded to multiples of `snap` picture units when it
    // is above 0 (the baker's whole pixels). False with why naming the part or the morph that cannot be done; the
    // rest of the pose is still applied.
    bool pose(const Pose& pose, std::string& why, f64 snap = 0);

    // Picture units to the pixels of a w × h drawing: the picture fitted in, centred (SVG's "xMidYMid meet").
    Affine to_pixels(u32 w, u32 h) const;
    // Smooth, as the vector looks: straight RGBA.
    assets::CookedTexture draw(u32 w, u32 h);
    // The same with picture units going to the drawing's pixels by `px` (the view zoomed and moved).
    assets::CookedTexture draw(u32 w, u32 h, const Affine& px);
    // Pixel art: each shape drawn alone, its fill and its stroke apart; a pixel takes the exact colour of the last one
    // (in the SVG's order) that covers at least half of it. No half-transparent pixels, no colours in between.
    assets::CookedTexture bake(u32 w, u32 h, const BakeOptions& options);

    // A part's pose bounds in the pixels of a w × h drawing (after the last pose()): x0, y0, x1, y1.
    void bounds(i32 part, u32 w, u32 h, f64 out[4]);
    // The same in the pixels picture units go to by `px`.
    void bounds(i32 part, const Affine& px, f64 out[4]);
    // How the last pose() carries a part's pivot, in picture units (identity at rest): by the parts it is inside and by
    // its own move, not by its own turn and scale, which are round the pivot. The pivot stands at
    // carried(part).apply(pivot(part)); a point p of the pose is the pivot inverse(carried(part)).apply(p) (dragged).
    Affine carried(i32 part) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// "#rgb", "#rrggbb", "rgb(r, g, b)" and a few names; false for anything else.
bool parse_color(std::string_view text, Rgba8& out);

} // namespace forge::pixel
