#pragma once

// A character's clips as a game's screen plays them (parts 5в and 5г of docs/models/движение-модель.md): «Отдать экрану»
// in «Векторная» writes Ёжик.motion.json beside Ёжик.svg in the game's pictures, and the game's live SVG shows a clip of
// it on a layer. The game runs none of the author's code: the file holds what the tab's preview shows, worked out.
//
// A clip is tracks of a part's property, each a run of pieces in time, every piece until the next one starts (the
// last until the clip's end):
//   hold    the value stands
//   ease    from `from` to `to` in `length` by an easing (a timing word, a bezier, an author's curve), then `to`
//   frames  values 120 times a second from `at` (to the nanosecond), the last at the very moment before `end`; mixed
//           between them
// Values are as the tab's recording keeps them (forge::pixel::PropValue): a colour, a stroke width and a morph are a
// share over the part's own with the colour, the width or the target beside it. Since version 2 (part 5г) a clip also
// has the copies of parts it makes and the steps of the parts' order.

#include "forge/vector/path.h"
#include "forge/vector/svg.h"
#include "forge/vector/timing.h"

#include <string>
#include <string_view>
#include <vector>

namespace forge::vector {

// What a screen plays of a part: its move, turn, scale and skews (its matrix), its opacity and its looks; since version
// 2 its dash, the share of its outline drawn, its tint and flash, its mask and the part that carries it.
enum class MotionProp : u8 {
    X, Y, Rot, Scale, Sx, Sy, SkewX, SkewY, Opacity, Fill, Stroke, StrokeWidth, Visible, Morph,
    Dash, DrawFrom, DrawTo, Tint, Flash, FlashColor, Mask, Link
};
inline constexpr usize kMotionPropCount = 22;
// "x", "y", "rot", "scale", "sx", "sy", "skew_x", "skew_y", "opacity", "fill", "stroke", "stroke_width", "visible",
// "morph", "dash", "draw_from", "draw_to", "tint", "flash", "flash_color", "mask", "link": the words of the code and
// of the file.
const char* motion_prop_word(MotionProp p);
bool motion_prop_from_word(std::string_view word, MotionProp& out);
// The ones the part's matrix is made of (x … skew_y).
bool motion_prop_moves(MotionProp p);
// The ones a call changes at a moment, held until the next (mask, link): a track of them is holds only.
bool motion_prop_held(MotionProp p);
// The ones of version 2: a file without them is written as version 1.
bool motion_prop_new(MotionProp p);

struct MotionColor {
    u8 r = 0, g = 0, b = 0, a = 255;
    bool operator==(const MotionColor&) const = default;
};

// a, b, c, d, e, f as SVG's matrix(…): x' = a·x + c·y + e, y' = b·x + d·y + f.
struct MotionMatrix {
    f64 a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    Point apply(Point p) const { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
    bool operator==(const MotionMatrix&) const = default;
};
MotionMatrix operator*(const MotionMatrix& m, const MotionMatrix& n); // m after n
MotionMatrix inverse(const MotionMatrix& m);                          // identity for one that has none

// v: a number; for fill, stroke, tint and flash_color the share k of `color` over the part's own colour (over white
// for tint and flash_color), for stroke_width the share of width `n` over its own, for morph the share of the way to
// shape `target`; visible 1 or 0. The mask: v 0 none, 1 inside part `target`, 2 outside it. The link: carried by part
// `target` (empty: let go) with the matrix `m` (forge::pixel::Block::carry).
struct MotionValue {
    f64 v = 0;
    MotionColor color;
    std::string target;
    f64 n = 0;
    MotionMatrix m;
    bool operator==(const MotionValue&) const = default;
};
// At rest: 1 for scale, sx, sy, opacity, draw_to and visible, 0 (the part's own) for the rest: no mask, no link.
MotionValue motion_rest(MotionProp p);
// From a to b by k, as the recording mixes (forge::pixel::mix): numbers past 0 … 1 too; a colour from none comes in at
// its own colour; a morph between two targets goes back to the part's own shape and on to the other; visible, the mask
// and the link change at k = 1.
MotionValue motion_mix(MotionProp p, const MotionValue& a, const MotionValue& b, f64 k);
// The same value however written: a colour, a width or a morph at a share of 0 or less is the part's own (its colour,
// width or target say nothing then).
MotionValue motion_plain(MotionProp p, const MotionValue& v);
// Whether two values show the same: numbers (and a link's matrix) within 1e-9 of each other (relative above 1).
bool motion_same(MotionProp p, const MotionValue& a, const MotionValue& b);

inline constexpr f64 kMotionRate = 120; // frames pieces' values a second
inline constexpr usize kMotionCurvePoints = 257;

struct MotionPiece {
    enum class Kind : u8 { Hold, Ease, Frames };
    Kind kind = Kind::Hold;
    f64 at = 0;
    MotionValue value; // hold
    // ease: the easing is the bezier when has_bezier, else curve `curve` of the file when 0 or more, else `timing`
    f64 length = 0;
    MotionValue from, to;
    Timing timing = Timing::Smooth;
    bool has_bezier = false;
    Bezier bezier;
    i32 curve = -1;
    // frames: at + i / kMotionRate, the last at `end` (just before it)
    f64 end = 0;
    std::vector<MotionValue> frames;
    bool operator==(const MotionPiece&) const = default;
};
// How many values a frames piece from `at` to `end` has: the moments at + i / kMotionRate before `end`, then `end`.
usize motion_frame_count(f64 at, f64 end);
// The moment of value i of a frames piece from `at` (i before the last): at + i / kMotionRate, to the nanosecond as
// the recording keeps its times. Between two moments the piece mixes by the share of the time between them.
f64 motion_frame_moment(f64 at, usize i);

struct MotionTrack {
    std::string part;
    MotionProp prop = MotionProp::X;
    std::vector<MotionPiece> pieces; // by time, the first at 0
    bool operator==(const MotionTrack&) const = default;
};

// What a clip does that a screen does not draw (MotionClip::not_drawn), in this order: "dash", "draw" (a path drawn
// along its length), "tint", "flash", "blur", "shadow", "mask", "link", "copies", "order", "drawing" (a swap of
// drawings), "morph_drawing" (a morph into a shape of another drawing). Since part 5г a screen draws all but "blur",
// "shadow", "drawing" and "morph_drawing"; a file of 5в names the others too.
inline constexpr const char* kMotionNotDrawn[] = {"dash", "draw", "tint", "flash", "blur", "shadow", "mask", "link", "copies", "order", "drawing",
                                                  "morph_drawing"};
// Its words for the author: "пунктир", "прорисовка", …; null for a word that is none of them.
const char* motion_not_drawn_words(std::string_view word);

// sound("шаг") and emit("удар", 0.1) of the code.
struct MotionMark {
    bool sound = false;
    std::string name;
    f64 at = 0, length = 0;
    bool operator==(const MotionMark&) const = default;
};

// copies("искра", n, …) of the code (version 2): copy `id` of part `source`, drawn from `born` on, above the part and
// its copies with smaller numbers; its tracks are a part's, by its id.
struct MotionCopy {
    std::string id;     // "искра[3]"
    std::string source; // "искра"
    u32 number = 0;     // 1, 2, … among the source's copies
    f64 born = 0;
    bool operator==(const MotionCopy&) const = default;
};

// to_top, to_bottom, above and below of the code (version 2): at `at` the part goes above or below its neighbours in
// its group, or right above or below neighbour `other`.
enum class MotionOrder : u8 { Top, Bottom, Above, Below };
const char* motion_order_word(MotionOrder k); // "top", "bottom", "above", "below"
struct MotionOrderStep {
    f64 at = 0;
    std::string part;
    MotionOrder kind = MotionOrder::Top;
    std::string other;
    bool operator==(const MotionOrderStep&) const = default;
};

struct MotionClip {
    std::string name;
    f64 length = 0;
    bool loop = false;
    std::vector<MotionTrack> tracks;
    std::vector<MotionMark> marks;          // by time
    std::vector<std::string> not_drawn;     // what the clip does that a screen does not draw: "blur", "shadow", …
    std::vector<MotionCopy> copies;         // by `born`
    std::vector<MotionOrderStep> order;     // by `at`, steps of one moment as made
    bool operator==(const MotionClip&) const = default;
};

// A part a clip moves or links (and every copy): its pivot, where its parent stands at rest (picture units) and its own
// transform in the file; a copy's are its source's.
struct MotionPart {
    std::string id;
    Point pivot;
    MotionMatrix frame, own;
    bool operator==(const MotionPart&) const = default;
};

struct MotionFile {
    std::string character; // its name, for messages: "Ёжик"
    std::string from;      // its file in «Ресурсы»: what «Отдать экрану» knows its own pair by
    std::string picture;   // "Ёжик.svg"
    std::vector<MotionPart> parts;
    std::vector<std::vector<f64>> curves; // authors' easings, kMotionCurvePoints values over 0 … 1
    std::vector<MotionClip> clips;
    bool operator==(const MotionFile&) const = default;

    const MotionClip* clip(std::string_view name) const;
    const MotionPart* part(std::string_view id) const;
};

// The version a game reads up to; write_motion_file writes 1 for a file with nothing of version 2 (an older game reads
// it).
inline constexpr u32 kMotionFileVersion = 2;
u32 motion_file_version(const MotionFile& file);

// False with why (Russian, naming the field: "clips.idle.tracks[2].pieces[0]: …") when the text is not such a file.
bool read_motion_file(std::string_view json, MotionFile& out, std::string& why);
// Numbers in their shortest form that reads back as the same number; a trailing newline; version
// motion_file_version.
std::string write_motion_file(const MotionFile& file);

// A piece's easing at s, the share of its length within 0 … 1.
f64 motion_ease(const MotionFile& file, const MotionPiece& piece, f64 s);
// A piece of a track of property p: its value at t (seconds of the clip, at its start or after).
MotionValue motion_piece_value(const MotionFile& file, MotionProp p, const MotionPiece& piece, f64 t);
// A track's value at t: its piece then, t held within the clip's 0 … length.
MotionValue motion_track_value(const MotionFile& file, const MotionTrack& track, f64 t, f64 length);

// The part's move, turn, scale and skews round its pivot (forge::pixel::pose_matrix): T(x, y) · T(pivot) · R · S ·
// Kx · Ky · T(−pivot), the skews held within ±89 degrees.
MotionMatrix motion_matrix(f64 x, f64 y, f64 rot, f64 scale, f64 sx, f64 sy, f64 skew_x, f64 skew_y, Point pivot);

// What a clip shows of a part at a moment: the values of its tracks; `has` says which the clip has.
struct MotionPartPose {
    std::string id;
    bool has[kMotionPropCount] = {};
    MotionValue values[kMotionPropCount];
    bool moves = false; // one of x … skew_y: its transform is the clip's
    MotionMatrix local; // then: frame⁻¹ · A · frame · own, the part's transform in the file's terms
};
// Every part of the clip's tracks at t, in the order of their first tracks. A part that moves without a part in the
// file's `parts` stays where it is (read_motion_file refuses such a file).
std::vector<MotionPartPose> motion_pose(const MotionFile& file, const MotionClip& clip, f64 t);
// The first part of the picture a clip's calls name that the picture has not (part 5г): a mask part, a carrier, a part
// of the order and its neighbour, a copy's source; what names it beside ("маска «eye»", "крепление «eye»", "порядок
// «eye»", "копия «искра[1]»"). A part is the first element with the id that draws (g, use or a shape). False when the
// picture has them all.
bool motion_clip_missing(const MotionClip& clip, const SvgOutline& svg, std::string& part, std::string& role);
// Whether a copy is drawn at t: born then or before.
bool motion_copy_drawn(const MotionCopy& copy, f64 t);
// The clip's order steps at t or before, in order, but those of a copy not born yet (as the tab leaves them out).
std::vector<MotionOrderStep> motion_order_at(const MotionClip& clip, f64 t);

// Where a clip is after `played` seconds of play: round and round when it loops, else held at its end. `loop` is how
// the game plays it, which need not be the clip's own (svg_play's `loop`).
f64 motion_clip_time(const MotionClip& clip, f64 played, bool loop);
// The marks a clip passes on its way from `played_before` (not counted) to `played` (counted) seconds of play, round
// and round when it loops (as motion_clip_time); each mark once however many times round. Below 0 for
// `played_before`: from the start, the marks at 0 counted. Indices into clip.marks, in the order passed.
std::vector<usize> motion_marks_passed(const MotionClip& clip, f64 played_before, f64 played, bool loop);

} // namespace forge::vector
