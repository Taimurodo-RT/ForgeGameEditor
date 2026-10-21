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
#include <span>
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
    // Part 4в: carried by another part (attach). The part stands as it would and is then moved by C = W · carry in
    // picture units, over its groups, where W is how part `carry_by` stands against its rest as drawn now (after its
    // groups, its own move snapped, its own carry); C = carry when carry_by is empty (a link let go).
    std::string carry_by;
    Affine carry;
};

// What a pose's move, turn, scale and skews do to a part in picture units at rest, round its pivot:
// T(x, y) · T(pivot) · R · S · Kx · Ky · T(−pivot), the skews held within ±kMaxSkew. x and y are rounded to multiples
// of `snap` when it is above 0.
Affine pose_matrix(const PartPose& pp, vector::Point pivot, f64 snap = 0);

// A shape's line in picture units at rest (the path of along, part 4в): a broken line, a closed one with its first
// point again at the end, the length from the start to each point, the angle of each segment in degrees (0 to the
// right, clockwise more), each within 180 of the one before.
struct PathLine {
    std::string id;
    std::vector<vector::Point> points;
    std::vector<f64> lengths;
    std::vector<f64> angles; // by segment: points.size() − 1
    bool closed = false;
    f64 length() const { return lengths.empty() ? 0 : lengths.back(); }
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

// A copy of a part of the picture (copies, part 4г of the motion model): drawn from the part at rest, above it in its
// group, the copies of one part by number. A pose moves it by its id, as a part.
struct PartCopy {
    std::string id;     // "искра[3]"
    std::string source; // "искра"
    u32 number = 0;     // 3: above the part's copies with smaller numbers
    bool operator==(const PartCopy&) const = default;
};

struct Pose {
    std::map<std::string, PartPose> parts; // the picture's parts and the copies of `copies`
    std::map<std::string, ShapeMorph> morphs;
    std::vector<OrderStep> order; // from the SVG's order, one after another
    std::vector<PartCopy> copies; // the copies drawn: one made by an earlier pose and not here is hidden
    // The drawing shown (part 4д): empty for the main one, else a name of the rig's drawings. In a swap, the one it goes
    // from and how far, 0 … 1: the pairs morph by it, the old parts fade by 1 − k, the new ones come in by k.
    std::string drawing;
    bool swap = false;
    std::string swap_from;
    f64 swap_k = 0;
};

// A copy the last pose drew, with its index for bounds(), carried(), matrix() and pivot().
struct DrawnCopy {
    std::string id;
    i32 index = -1;
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
    // The picture's parts. The copies a pose makes are not among them: their indices come after (drawn_copies, copy).
    std::span<const Part> parts() const;
    i32 part(std::string_view id) const; // -1 when no part of the picture has that id
    // The picture's own size in its units (its viewBox, else width and height), and its top left there.
    f64 width() const;
    f64 height() const;
    vector::Point origin() const;

    // A part's pivot (picture units); the centre of its rest bounds until set. Set, it is the part's with this id in
    // every drawing (part 4д).
    void set_pivot(std::string_view id, vector::Point at);
    vector::Point pivot(i32 part) const;

    // The character's other drawings (part 4д of docs/models/движение-модель.md): pictures of their own on the main
    // one's canvas, by name. Loading the main picture drops them. False with why when the SVG does not load or its size
    // is not the main picture's.
    bool add_drawing(const std::string& name, std::string_view svg, std::string& why);
    std::vector<std::string> drawings() const; // their names, sorted
    // A drawing by name, "" and "draw" this picture itself; null for none.
    Rig* drawing(std::string_view name);
    const Rig* drawing(std::string_view name) const;
    // The name of the picture a part (or an element) with this id is in: "" for this one, else the first drawing by
    // name that has it; "" when none has. holder() is that picture, or for an id "ёжик/…" that none of them has the
    // actor hosted in part «ёжик» (host) as its own holder() finds it.
    std::string holder_name(std::string_view id) const;
    const Rig& holder(std::string_view id) const;

    // Part 6а of docs/models/сцены-модель.md: another picture drawn in a group part of this one (a scene's), above its
    // paints. An actor: its main picture and its drawings, in its own units as the group's children, the one its last
    // pose() shows (in a swap the two, the old under the new), uncut (as a game draws it in its reach, part 6г2); from
    // then on it is drawn only through this rig and stays where it is (no move) while this one is alive. Pixels:
    // straight RGBA drawn over 0, 0 … w, h of the group's units.
    // False with why: no such group, a part hosting already, an actor drawn on its own before or hosted elsewhere.
    bool host(std::string_view part, Rig& actor, std::string& why);
    bool host(std::string_view part, const assets::CookedTexture& pixels, f64 w, f64 h, std::string& why);
    // From now on the picture and its drawings are drawn whole, not cut at their canvas as loaded: a scene's actor.
    void uncut();
    // The actor hosted in a part; null for none.
    Rig* hosted(std::string_view part);
    const Rig* hosted(std::string_view part) const;
    // From a part's own coordinates (its children's, a hosted actor's units) to picture units as the last pose() left
    // them; identity for no part.
    Affine global_matrix(i32 part) const;
    // The picture the last pose() showed: this one or a drawing, in a swap the one it goes to; what the tab's tree,
    // bounds and mouse go by.
    Rig& shown();
    const Rig& shown() const;
    // Whether the last pose() was in a swap: the frame is two pictures, the old one under the new.
    bool swapping() const;

    // Puts the picture in the pose, from the rest pose. Moves are rounded to multiples of `snap` picture units when it
    // is above 0 (the baker's whole pixels). A copy is made from its part at rest the first time a pose has it and kept
    // while the picture is loaded. False with why naming the part, the copy or the morph that cannot be done; the rest
    // of the pose is still applied.
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

    // Where a part's shapes cover a w × h drawing by px as the last pose() left them, as the mask of a part takes them
    // (opaque, a hidden shape its area): a byte of cover a pixel, row by row; empty for a part with no shapes. A
    // transition's mask in a scene (part 6б of docs/models/сцены-модель.md).
    std::vector<u8> area(i32 part, u32 w, u32 h, const Affine& px);
    // A part's pose bounds in the pixels of a w × h drawing (after the last pose()): x0, y0, x1, y1.
    void bounds(i32 part, u32 w, u32 h, f64 out[4]);
    // The same in the pixels picture units go to by `px`.
    void bounds(i32 part, const Affine& px, f64 out[4]);
    // Where the last pose() draws (part 6г2 of docs/models/сцены-модель.md, the frame a game draws an actor in), in the
    // pixels picture units go to by `px`: every shape shown with a fill or a stroke, its stroke's width all round, of the
    // pictures the pose shows (in a swap the two). False when it draws nothing.
    bool reach(const Affine& px, f64 out[4]);
    // How the last pose() carries a part's pivot, in picture units (identity at rest): by the parts it is inside and by
    // its own move, not by its own turn and scale, which are round the pivot. The pivot stands at
    // carried(part).apply(pivot(part)); a point p of the pose is the pivot inverse(carried(part)).apply(p) (dragged).
    Affine carried(i32 part) const;
    // A part's own matrix as the last pose() left it (from its parent's coordinates, as SVG's transform).
    Affine matrix(i32 part) const;
    // A part at rest (part 5в of the motion model, what a game's screen moves it by): where its parent stands in
    // picture units, the frame its move, turn and scale are taken in, and its own transform in the file. The pose's
    // matrix() is rest_frame⁻¹ · pose_matrix · rest_frame · rest_local when nothing carries it.
    Affine rest_frame(i32 part) const;
    Affine rest_local(i32 part) const;
    // The copies the last pose() drew (part 4г), by index.
    std::vector<DrawnCopy> drawn_copies() const;
    // A copy's index once a pose made it, drawn or not; -1 for none.
    i32 copy(std::string_view id) const;
    // Whether two parts are neighbours in one group, the ones whose order can change against each other.
    bool siblings(i32 a, i32 b) const;
    // Whether any shape of a part has a dash pattern of its own.
    bool dashed(i32 part) const;
    // Whether a part draws any shapes (a group with display="none" has none in ThorVG).
    bool has_shapes(i32 part) const;
    // A shape part's line at rest in picture units, flattened (part 4в); false with why for a group, more than one
    // contour or a line of length 0.
    bool path_of(i32 part, PathLine& out, std::string& why) const;

private:
    struct Impl;
    struct SwapSide; // which picture of a swap this one is, and the other
    std::unique_ptr<Impl> impl_;

    // pose(), draw() and bake() of one picture: the other drawings out of it.
    bool pose_alone(const Pose& pose, std::string& why, f64 snap, const Rig& family, const SwapSide* side);
    assets::CookedTexture draw_alone(u32 w, u32 h, const Affine& px);
    void bake_alone(u32 w, u32 h, std::vector<u8>& out);
    // The pose as drawing d takes it: its parts, copies, morphs and order; a mask, a link and an order neighbour it
    // has not are off.
    Pose for_drawing(const Pose& pose, const Rig& d) const;
    // Whether part `id` (of the picture, or a copy the pose draws) is a shape that goes into a swap's pairs: not a
    // group, not hidden by display="none".
    bool swap_shape(const Pose& pose, const std::string& id) const;
};

// A rect 0, 0 … rw, rh of one colour in a w × h drawing by px, smooth, straight RGBA (part 6б of the scenes model: what
// was before a scene and the game after it, in the tab).
assets::CookedTexture draw_rect(u32 w, u32 h, f64 rw, f64 rh, Rgba8 color, const Affine& px);

// "#rgb", "#rrggbb", "rgb(r, g, b)" and a few names; false for anything else.
bool parse_color(std::string_view text, Rgba8& out);
// A colour of a part's property (part 4б of the motion model): what parse_color reads, or "#rrggbbaa" with its alpha.
bool parse_part_color(std::string_view text, Rgba8& out);
// "#rrggbb", or "#rrggbbaa" when its alpha is not full.
std::string color_hex(const Rgba8& c);

} // namespace forge::pixel
