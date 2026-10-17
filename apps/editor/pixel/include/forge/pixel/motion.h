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

enum class Prop : u8 { X, Y, Rot, Scale, Sx, Sy, Opacity, Fill, Morph };
inline constexpr usize kPropCount = 9;
// "x", "y", "rot", "scale", "sx", "sy", "opacity", "fill", "morph".
const char* prop_word(Prop p);
bool prop_from_word(std::string_view word, Prop& out);
bool prop_is_number(Prop p); // all but fill and morph

// A property's value. For fill and morph `v` is the share k of the colour or of the target shape over the part's own
// (0: its own), the colour or the target beside it.
struct PropValue {
    f64 v = 0;
    Rgba8 color;
    std::string target;
    bool operator==(const PropValue&) const = default;
};
// At rest: 0 for x, y and rot, 1 for scale, sx, sy and opacity, k = 0 for fill and morph.
PropValue rest_value(Prop p);
// From a to b by k: exact at k = 0 and 1. A colour from none comes in at its own colour; a morph between two targets
// goes back to the part's own shape and on to the other.
PropValue mix(Prop p, const PropValue& a, const PropValue& b, f64 k);

// A call of the code: file 0 is the character's script, the others the modules it requires (Recording::files); site
// is its place in that file's CodeSites::sites(), -1 for none.
struct SiteRef {
    u32 file = 0;
    i32 site = -1;
    bool operator==(const SiteRef&) const = default;
};

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
    };
    To to = To::Value;
    PropValue value;
    u32 ref = 0;
    i32 binding = -1;

    // How it goes: a timing word, or an author's easing (Recording::curves), or a spring, or tween(fn)'s frames.
    vector::Timing timing = vector::Timing::Smooth;
    i32 curve = -1;
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
    // one step).
    PropValue from, end;
    std::vector<f64> steps;
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

struct Recording {
    std::string clip;
    f64 length = 0;                 // where the clip's own flow ends; what runs on in the background is cut there
    std::vector<std::string> files; // by SiteRef::file: "" for the character's script, then module names
    std::vector<std::string> parts; // by Block::part: the parts' ids
    std::vector<Block> blocks;      // in the order they were made
    std::vector<Saved> saves;
    std::vector<std::vector<f64>> curves; // authors' easings: kCurvePoints values over 0 … 1
    std::vector<TimeEvent> events;
    std::vector<Mark> marks;
    std::vector<Pause> pauses;
    std::vector<LogLine> log;
    u64 generation = 0; // the script's recording it came from: bindings are asked of that one only

    // resolve(): each track's blocks by start and order.
    struct Track {
        u32 part = 0;
        Prop prop = Prop::X;
        std::vector<u32> blocks;
    };
    std::vector<Track> tracks;
    std::vector<i32> track_of; // parts.size() × kPropCount → index in tracks, -1 for none

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

// A track's value at t (after resolve): rest before its first block.
bool sample(const Recording& rec, u32 track, f64 t, BindingEval* eval, PropValue& out, std::string& why);
// A block's value at t, as if it decided.
bool block_value(const Recording& rec, const Block& b, f64 t, BindingEval* eval, PropValue& out, std::string& why);
// The block that decides a track at t (after resolve): the last, by start then order, that started at t or before; -1
// before the first one.
i32 deciding_block(const Recording& rec, u32 track, f64 t);
// Whether block b's transition goes in the frame at t, `frame` seconds long: after its start and not after its end; a
// block of length 0 in the frame it starts in; a binding from its start on (while it decides).
bool block_going(const Block& b, f64 t, f64 frame);
// Of all a part's tracks, the deciding block that started last at t or before (of two that started together, the one
// of the first property in Prop's order); -1 when none has.
i32 latest_block(const Recording& rec, u32 part, f64 t);

// The pose of all parts at t, from the tracks.
bool sample_pose(const Recording& rec, f64 t, BindingEval* eval, Pose& out, std::string& why);

// The old pose(t) on top of a recorded one: moves and turns add, scales and opacity multiply, a fill or a morph the
// script sets replaces the recorded one.
void layer_pose(Pose& base, const Pose& over);

} // namespace forge::pixel
