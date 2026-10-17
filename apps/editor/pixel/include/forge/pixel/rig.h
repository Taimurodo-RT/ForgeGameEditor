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

// A skew is kept within ±kMaxSkew degrees: past them its tangent runs off to infinity.
inline constexpr f64 kMaxSkew = 89;

// What a pose does to one part, from its rest. Turning, scaling and skewing are around the part's pivot; a part inside
// another moves with it, as in SVG. The looks (fill … visible) are of every shape of the part, a group's shapes too.
struct PartPose {
    f64 x = 0, y = 0;       // picture units
    f64 rot = 0;            // degrees, clockwise on screen (SVG's rotate)
    f64 scale = 1, sx = 1, sy = 1;
    f64 skew_x = 0, skew_y = 0; // degrees, SVG's skewX and skewY after the scale
    f64 opacity = 1;        // times its own
    bool has_fill = false;  // the fill of the part (every shape of a group) becomes `fill`
    Rgba8 fill;
    f64 fill_k = 1;         // how far: 0 its own colour, 1 `fill` (a gradient takes it from 0.5 on)
    bool has_stroke = false; // the stroke colour becomes `stroke`, as the fill; a shape without one gets it
    Rgba8 stroke;
    f64 stroke_k = 1;
    bool has_stroke_width = false; // the stroke width becomes `stroke_width` (picture units, 0 at least) by `stroke_width_k`
    f64 stroke_width = 0;
    f64 stroke_width_k = 1;
    f64 dash = 0;           // added to each shape's own dash offset (stroke-dashoffset)
    f64 draw_from = 0, draw_to = 1; // the share of each shape's path drawn (ThorVG's trimpath); none when from ≥ to
    bool visible = true;
    // Part 4б. The tint multiplies each shape's colours by white + (tint − white) · tint_k; the flash takes their red,
    // green and blue `flash` (0 … 1) of the way to white + (flash_color − white) · flash_color_k. A part's own first,
    // then its groups', after every part's fill and stroke.
    bool has_tint = false;
    Rgba8 tint;
    f64 tint_k = 1;
    f64 flash = 0;
    bool has_flash_color = false;
    Rgba8 flash_color;
    f64 flash_color_k = 1;
    // ThorVG's blur and drop shadow on the part's scene, in picture units however the part and its groups stand: the
    // blur's sigma, the shadow's colour (its alpha times shadow_k), offset and sigma.
    f64 blur = 0;
    bool has_shadow = false;
    Rgba8 shadow;
    f64 shadow_k = 1;
    f64 shadow_x = 0, shadow_y = 0, shadow_blur = 0;
    // Drawn only inside (outside when mask_invert) the shapes of part `mask` as they stand: empty for none.
    std::string mask;
    bool mask_invert = false;
};

// A part put above or below its neighbours in its group (to_top, to_bottom, above, below of the motion), in order.
struct OrderStep {
    enum class Kind : u8 { Top, Bottom, Above, Below };
    std::string part;
    Kind kind = Kind::Top;
    std::string other; // the neighbour of Above and Below
    bool operator==(const OrderStep&) const = default;
};

// A shape morphed into another element of the same SVG (it may be hidden): k = 0 is the shape, 1 the target.
struct ShapeMorph {
    std::string target;
    f64 k = 0;
};

struct Pose {
    std::map<std::string, PartPose> parts;
    std::map<std::string, ShapeMorph> morphs;
    std::vector<OrderStep> order; // from the SVG's order, one after another
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
    // (in the order drawn now) that covers at least half of it. No half-transparent pixels, no colours in between.
    assets::CookedTexture bake(u32 w, u32 h, const BakeOptions& options);

    // A part's pose bounds in the pixels of a w × h drawing (after the last pose()): x0, y0, x1, y1.
    void bounds(i32 part, u32 w, u32 h, f64 out[4]);
    // The same in the pixels picture units go to by `px`.
    void bounds(i32 part, const Affine& px, f64 out[4]);
    // How the last pose() carries a part's pivot, in picture units (identity at rest): by the parts it is inside and by
    // its own move, not by its own turn and scale, which are round the pivot. The pivot stands at
    // carried(part).apply(pivot(part)); a point p of the pose is the pivot inverse(carried(part)).apply(p) (dragged).
    Affine carried(i32 part) const;
    // A part's own matrix as the last pose() left it (from its parent's coordinates, as SVG's transform).
    Affine matrix(i32 part) const;
    // Whether two parts are neighbours in one group, the ones whose order can change against each other.
    bool siblings(i32 a, i32 b) const;
    // Whether any shape of a part has a dash pattern of its own.
    bool dashed(i32 part) const;
    // Whether a part draws any shapes (a group with display="none" has none in ThorVG).
    bool has_shapes(i32 part) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// "#rgb", "#rrggbb", "rgb(r, g, b)" and a few names; false for anything else.
bool parse_color(std::string_view text, Rgba8& out);
// A colour of a part's property (part 4б of the motion model): what parse_color reads, or "#rrggbbaa" with its alpha.
bool parse_part_color(std::string_view text, Rgba8& out);
// "#rrggbb", or "#rrggbbaa" when its alpha is not full.
std::string color_hex(const Rgba8& c);

} // namespace forge::pixel
