#pragma once

// A clip recorded from code (step 1 of docs/models/движение-модель.md): the clip function of the character's script
// runs once and leaves blocks on tracks («часть · свойство»), each block made by one call of the code. A frame is then
// read from the tracks at any time t at once: the block of the track that started last at t or before decides, an
// earlier one still running is cut (as a key on a timeline). Bindings (eye.x(function() … end)) are the script's
// functions, asked for their value at t: functions of t and the parts' values, the same whatever was read before, so
// one that would keep something between frames is refused at its line.
//
// Times are seconds from the clip's start, rounded to nanoseconds as they are summed, so 0.1 three times is 0.3.

#include "forge/pixel/rig.h"
#include "forge/vector/timing.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace forge::pixel {

// In the order of a part's tracks on the timeline (part 4а of the model adds skew_x … visible and the order, part 4б
// tint … shadow_blur and the mask, part 4в the link of attach and detach). Drawing is the character's, not a part's
// (swap, part 4д): its track's part is kDrawingPart.
enum class Prop : u8 {
    X, Y, Rot, Scale, Sx, Sy, SkewX, SkewY, Opacity, Fill, Stroke, StrokeWidth, Dash, DrawFrom, DrawTo, Visible,
    Tint, Flash, FlashColor, Blur, Shadow, ShadowX, ShadowY, ShadowBlur, Morph, Mask, Link, Order, Drawing
};
inline constexpr usize kPropCount = 29;
// "x", "y", "rot", "scale", "sx", "sy", "skew_x", "skew_y", "opacity", "fill", "stroke", "stroke_width", "dash",
// "draw_from", "draw_to", "visible", "tint", "flash", "flash_color", "blur", "shadow", "shadow_x", "shadow_y",
// "shadow_blur", "morph", and "маска", "крепление", "порядок" and "рисунок" for the mask, the link, the order and the
// drawing, which are no fields of the code.
const char* prop_word(Prop p);
bool prop_from_word(std::string_view word, Prop& out);
// Plain numbers: all but the colours, stroke_width (nil is its own), visible, morph, the mask and the order.
bool prop_is_number(Prop p);
// Colours with a share over their rest (fill, stroke, tint, flash_color, shadow).
bool prop_is_color(Prop p);
// Calls at a moment rather than properties (the mask, the link and the order): no length, not in save, restore and
// rest.
bool prop_is_call(Prop p);

// A property's value. For a colour, stroke_width and morph `v` is the share k of the colour, the width or the target
// shape over the part's rest (0: the rest, its own colour, width or shape), the colour, the width `n` or the target
// beside it. Visible is v 1 or 0. The mask: v 0 none, 1 inside the part `target`, 2 outside it. The link: v 1 carried
// by the part `target` (attach), 0 let go (detach). The order: v is a kind of OrderStep (rig.h), `target` the neighbour
// of above and below. The drawing: `target` its name, "draw" (or empty, at rest) the main one.
struct PropValue {
    f64 v = 0;
    Rgba8 color;
    std::string target;
    f64 n = 0;
    bool operator==(const PropValue&) const = default;
};
// At rest: 1 for scale, sx, sy, opacity, draw_to and visible, k = 0 (the rest) for the colours, stroke_width and
// morph, 0 for the rest (no mask, no link, the main drawing).
PropValue rest_value(Prop p);
// From a to b by k: exact at k = 0 and 1. A colour from none comes in at its own colour; a morph between two targets
// goes back to the part's own shape and on to the other. Two widths given go as numbers (past them too, for back and
// elastic); from or to its own width k stays in 0 … 1, as a colour's. Visible, the mask, the link, the order and the
// drawing change at k = 1 only (a frame shows a swap's way by Pose::swap_k).
PropValue mix(Prop p, const PropValue& a, const PropValue& b, f64 k);

// A call of the code: file 0 is the character's script, the others the modules it requires (Recording::files); site
// is its place in that file's CodeSites::sites(), -1 for none.
struct SiteRef {
    u32 file = 0;
    i32 site = -1;
    bool operator==(const SiteRef&) const = default;
};

// An easing bezier(x1, y1, x2, y2) of the code (part 4ж): its numbers and the call that wrote them.
struct BezierUse {
    vector::Bezier curve;
    SiteRef site;
};

// The part of the drawing's track (swap, part 4д): no part of a picture has an empty id.
inline constexpr const char* kDrawingPart = "";

struct Block {
    u32 part = 0; // in Recording::parts
    Prop prop = Prop::X;
    f64 start = 0, length = 0; // a binding has no length: it holds until another block of its track starts
    u32 order = 0;             // of two blocks that start together, the later made wins

    enum class To : u8 {
        Value,   // value
        Back,    // where block `ref` started from (t:back)
        Saved,   // what save `ref` kept (part:restore)
        Rest,    // rest_value (part:rest)
        Binding, // the script's function `binding`, at each t
        Along,   // x, y or rot of part:along (part 4в): along Recording::alongs[ref] by the timing
        Snap,    // x or y of part:snap_to: `value`, where the pivot of part Recording::snaps[ref] stood at the call
        // x or y of a scene's camera():follow (part 6б of docs/models/сцены-модель.md): at each t the camera's middle
        // where the pivot of Recording::follows[ref]'s target stands, less the share 1 − k of how far from it the
        // camera was at the start; `value` is the number that put the middle on the target then.
        Follow,
        Hold, // where the track is at its start: x or y stands where a follow left it (the other one moved, follow(nil))
    };
    To to = To::Value;
    PropValue value;
    u32 ref = 0;
    i32 binding = -1;

    // How it goes: a timing word, or an author's easing (Recording::curves), or bezier(…) (Recording::beziers, part
    // 4ж), or a spring, or tween(fn)'s frames.
    vector::Timing timing = vector::Timing::Smooth;
    i32 curve = -1, bezier = -1;
    bool spring = false;
    vector::Spring spring_def;
    f64 tolerance = 0.001;
    std::vector<PropValue> frames; // tween(fn): evenly over the length, the first at start, the last at the end
    i32 arc = -1;                  // x and y going on an arc together: the other one's block
    bool arc_reverse = false;

    // The code: the call that made it, which of its arguments hold the value (a table's field named after the
    // property when value_field), the length and the timing (-1: none); the at(…) or delay(…) whose first argument
    // is its start, when it starts there.
    SiteRef site;
    i8 value_arg = -1, length_arg = -1, timing_arg = -1;
    bool value_field = false;
    SiteRef start_site;

    // resolve(): the value it starts from, the one it goes to, a spring's positions (1/120 s apart, the first after
    // one step). A link's carry: the matrix C of the part (PartPose::carry) is W(target) · carry while it holds,
    // `carry` itself once let go.
    PropValue from, end;
    std::vector<f64> steps;
    Affine carry;
};

// part:save(): the values of all a part's properties at `time`.
struct Saved {
    u32 part = 0;
    f64 time = 0;
    u32 order = 0;
    PropValue values[kPropCount];
};

// wait_until("name", s) and duration("name", s), loop_until("name", …): a time event of the timeline, its length from
// .pixel.json when the file has one.
struct TimeEvent {
    std::string name;
    bool wait = true; // wait_until; false for duration and loop_until
    f64 start = 0, length = 0;
    bool from_file = false;
    SiteRef site;
};

// emit("name"[, s]) and sound("name"): marks for the game.
struct Mark {
    enum class Kind : u8 { Emit, Sound };
    Kind kind = Kind::Emit;
    std::string name;
    f64 time = 0, length = 0;
    SiteRef site;
    std::string actor; // a scene's (part 6а): the actor whose clip made it, «ёжик: шаг»; empty for the scene's own
};

// actor("ёжик"):play("walk"[, { speed, params }]), :loop(…) and :stop() in a scene (part 6а of
// docs/models/сцены-модель.md): from `start` the actor plays its character's clip, once (holding its last pose after)
// or round and round, or stands in the pose it showed then. A play waits `length` = clip_length / speed; a loop and a
// stop do not wait.
struct ActorPlay {
    enum class Kind : u8 { Play, Loop, Stop };
    Kind kind = Kind::Play;
    std::string actor;
    std::string clip;   // empty for a stop
    f64 start = 0;
    f64 length = 0;      // in the scene's seconds
    f64 clip_length = 0; // the clip's own (its recording's)
    f64 speed = 1;
    std::map<std::string, f64> params; // over the actor's
    SiteRef site;       // the play(…) call
    SiteRef start_site; // the at(…) or delay(…) whose first argument is its start, when it starts there
    u32 order = 0;      // of two that start together, the later made wins
};

// wait(s) and t:wait(s): a pause the timeline shows as a block too.
struct Pause {
    f64 start = 0, length = 0;
    SiteRef site;
    i8 length_arg = 0;
};

// log(…): a line of the console, at its time.
struct LogLine {
    f64 time = 0;
    SiteRef site;
    u32 line = 0; // in the site's file
    std::string text;
};

// part:along("route", s, timing, { turn, from, to, relative }) (part 4в): the path, the shares of its length the part
// goes from and to, and the part's pivot at rest, which x and y move onto the path.
struct Along {
    u32 path = 0; // in Recording::paths
    f64 from = 0, to = 1;
    bool turn = false, relative = false;
    vector::Point pivot;
};

// A part that links or places by others (attach, snap_to of part 4в): its pivot at rest and the part it is inside,
// for where it stands in a frame without the picture.
struct Bone {
    std::string id;
    vector::Point pivot; // picture units
    i32 parent = -1;     // in Recording::bones; -1 when only the picture is
};

// What the recording warns of (a skew past its limit, a dash with no pattern to move): a line of the console, once a
// part, at the call of its first block.
struct Note {
    SiteRef site;
    u32 line = 0; // in the site's file
    std::string text;
};

// copies("искра", n, function(i, c) … end) (part 4г): a copy of a part of the picture, drawn from `born` to the clip's
// end; its blocks are a part's, by its id.
struct Copy {
    std::string id;     // "искра[3]"
    std::string source; // "искра"
    u32 number = 0;     // 3: the part's copies in the clip, by their calls
    f64 born = 0;
    SiteRef site; // the copies(…) call
};

inline constexpr u32 kMaxCopiesCall = 64;
inline constexpr u32 kMaxCopies = 256;

// start_from("idle") (part 5б): where clip `clip` left a property of a part at its end. A link's carry beside it
// (Block::carry): W(target) · carry while it holds, carry itself once let go.
struct StartValue {
    std::string part; // its id; kDrawingPart for the drawing
    Prop prop = Prop::X;
    PropValue value;
    Affine carry;
    bool operator==(const StartValue&) const = default;
};

// The pose a clip starts from: the values of its parts before their tracks' first blocks, in place of the rest.
struct StartPose {
    std::string clip;               // empty: none, the clip starts at rest
    SiteRef site;                   // the start_from(…) call
    std::vector<StartValue> values; // those not at rest, by part (as `clip` has them) then property
    std::vector<OrderStep> order;   // the order's calls of `clip` up to its end, before the clip's own
};

// camera():follow("ёжик"[, s, timing]) in a scene (part 6б of docs/models/сцены-модель.md): the part or actor followed
// and the camera's pivot, the middle of the canvas, that its x and y move.
struct Follow {
    std::string target;
    vector::Point pivot;
};

// transition(…) and leave(…) in a scene (part 6б): how the scene comes in over what was before it, from 0, and how it
// goes out to the game at its end. Their frames are parts of the recording (kLayerParts of scene.h).
struct SceneTransition {
    bool leave = false;
    std::string kind; // "растворение", "сдвиг", "приближение"; empty for the author's own function
    std::string from; // «сдвиг»'s edge: "право", "лево", "верх", "низ"
    f64 start = 0, length = 0;
    SiteRef site; // the call
};

// music("имя"[, { fade, volume }]) in a scene (part 6в of docs/models/сцены-модель.md): from `start` the scene's music
// is the game's sound file `name` (empty: music(nil), silence), reached over `fade` seconds; the same name again goes on
// playing, only its volume moves to the new one.
struct SceneMusic {
    std::string name;
    f64 start = 0, fade = 0, volume = 1;
    u32 order = 0; // with SceneStop::order: a call at a stop's time after it begins when the wait ends
    SiteRef site;  // the call
};

// wait_action(["Действие"]) and talk("разговор") in a scene (part 6в): the scene's time stands at `time` until the
// player acts or the talk ends, while its actors' clips go on.
struct SceneStop {
    enum class Kind : u8 { Action, Talk };
    Kind kind = Kind::Action;
    std::string name; // the action's words, or the talk's; empty: wait_action() — any key
    f64 time = 0;
    u32 order = 0; // with ActorPlay::order: a clip started at `time` after the stop is not moved by it
    SiteRef site;  // the call
};

struct Recording {
    std::string clip;
    f64 length = 0;                 // where the clip's own flow ends; what runs on in the background is cut there
    std::vector<std::string> files; // by SiteRef::file: "" for the character's script, then module names
    std::vector<std::string> parts; // by Block::part: the parts' ids
    std::vector<Block> blocks;      // in the order they were made
    std::vector<Saved> saves;
    std::vector<std::vector<f64>> curves; // authors' easings: kCurvePoints values over 0 … 1
    std::vector<BezierUse> beziers;       // bezier(…) easings (part 4ж)
    std::vector<TimeEvent> events;
    std::vector<Mark> marks;
    std::vector<Pause> pauses;
    std::vector<LogLine> log;
    std::vector<Note> notes;
    std::vector<PathLine> paths;   // along's paths, each once
    std::vector<Along> alongs;     // by Block::ref of To::Along
    std::vector<std::string> snaps; // by Block::ref of To::Snap: the part placed by
    std::vector<Bone> bones;       // the parts of links and snaps, with the parts they are inside, a parent before them
    std::vector<Copy> copies;      // in the order of their calls: by number within each part
    StartPose start;               // start_from(…) (part 5б); its parts are among `parts`
    std::vector<ActorPlay> plays;  // a scene's actors' clips (part 6а), in the order they were laid out
    std::vector<Follow> follows;   // by Block::ref of To::Follow (part 6б)
    std::vector<SceneTransition> transitions; // a scene's entrance and leave (part 6б), as laid out
    std::vector<SceneMusic> music;            // a scene's music calls (part 6в), as laid out
    std::vector<SceneStop> stops;             // a scene's waits for the player and talks (part 6в), by time
    u64 generation = 0; // the script's recording it came from: bindings are asked of that one only

    // resolve(): each track's blocks by start and order.
    struct Track {
        u32 part = 0;
        Prop prop = Prop::X;
        std::vector<u32> blocks;
    };
    std::vector<Track> tracks;
    std::vector<i32> track_of; // parts.size() × kPropCount → index in tracks, -1 for none
    std::vector<i32> start_of; // parts.size() × kPropCount → index in start.values, -1 for none

    i32 track(u32 part, Prop p) const;
};

// The lengths of a clip's time events from .pixel.json, by name: wait_until's, and duration's and loop_until's.
struct MotionLengths {
    std::map<std::string, f64> waits, durations;
    bool operator==(const MotionLengths&) const = default;
};

inline constexpr usize kCurvePoints = 257;
inline constexpr usize kMaxBlocks = 10000;

// A time as recordings keep it: to the nanosecond.
f64 snap_time(f64 t);

// What a binding is worth at t; false with why. The script's, for a recording that has bindings.
struct BindingEval {
    virtual ~BindingEval() = default;
    virtual bool eval(const Recording& rec, i32 binding, Prop prop, f64 t, PropValue& out, std::string& why) = 0;
};

// The tracks, then each block's start value and end value in the order of time (start, then order): where the track
// was at its start, where it goes; springs run from there. Again after blocks were added. False with why.
bool resolve(Recording& rec, BindingEval* eval, std::string& why);

// Where start_from left a part's property (after resolve, part 5б); null when it starts at rest.
const StartValue* start_value(const Recording& rec, u32 part, Prop p);
// A part's property before its track's first block: where start_from left it, else its rest.
PropValue first_value(const Recording& rec, u32 part, Prop p);
// The pose a clip that starts from this one starts with (part 5б): every property of every part at the recording's
// length, its own start where no track moved it, a binding's value then; the copies and their calls of the order left
// out (attach, mask and order name parts of the picture, so nothing else hangs on a copy). False with why.
bool end_pose(const Recording& rec, BindingEval* eval, StartPose& out, std::string& why);

// A track's value at t (after resolve): first_value before its first block.
bool sample(const Recording& rec, u32 track, f64 t, BindingEval* eval, PropValue& out, std::string& why);
// A block's value at t, as if it decided.
bool block_value(const Recording& rec, const Block& b, f64 t, BindingEval* eval, PropValue& out, std::string& why);
// How far block b's timing has gone at t: 0 at its start and before, 1 at its end and after (as eased, an easing that
// overshoots past them).
f64 block_share(const Recording& rec, const Block& b, f64 t);
// Block b's easing at s, the share of its time within 0 … 1: its bezier(…) (part 4ж), its author's easing or its word.
f64 block_ease(const Recording& rec, const Block& b, f64 s);
// The block that decides a track at t (after resolve): the last, by start then order, that started at t or before; -1
// before the first one.
i32 deciding_block(const Recording& rec, u32 track, f64 t);
// Whether block b's transition goes in the frame at t, `frame` seconds long: after its start and not after its end; a
// block of length 0 in the frame it starts in; a binding and a camera's follow from its start on (while it decides).
bool block_going(const Block& b, f64 t, f64 frame);
// Of all a part's tracks, the deciding block that started last at t or before (of two that started together, the one
// of the first property in Prop's order); -1 when none has.
i32 latest_block(const Recording& rec, u32 part, f64 t);

// The pose of all parts at t, from the tracks: the copies born at t or before with theirs; the drawing shown and, in a
// swap's block, the one it comes from and its share k within 0 … 1 (part 4д).
bool sample_pose(const Recording& rec, f64 t, BindingEval* eval, Pose& out, std::string& why);

// The point at share u of a path's length and, when angle is given, the direction of its segment there in degrees.
// An open path holds u within 0 … 1; a closed one goes round past them, its angle on by a turn each time round.
vector::Point path_point(const PathLine& path, f64 u, f64* angle = nullptr);
// How bone b stands against its rest at t (after resolve), in picture units: C · W(parent) · M, where M is its own
// move, turn, scale and skews round its pivot and C its link's carry. False with why.
bool bone_world(const Recording& rec, u32 bone, f64 t, BindingEval* eval, Affine& out, std::string& why);
// The same without its own move: C · W(parent).
bool bone_frame(const Recording& rec, u32 bone, f64 t, BindingEval* eval, Affine& out, std::string& why);
i32 bone_of(const Recording& rec, std::string_view id); // -1 for none
i32 copy_of(const Recording& rec, std::string_view id); // its index in Recording::copies; -1 for none

// The old pose(t) on top of a recorded one: moves and turns add, scales and opacity multiply, a fill or a morph the
// script sets replaces the recorded one.
void layer_pose(Pose& base, const Pose& over);

} // namespace forge::pixel
