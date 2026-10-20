#pragma once

// What the pose script (script.cpp) and the recording of clips (motion_record.cpp) share: the Luau state, the files
// of the code with their calls, and the recording under way.

#include "forge/pixel/code_sites.h"
#include "forge/pixel/draw.h"
#include "forge/pixel/motion.h"
#include "forge/pixel/scene.h"
#include "forge/pixel/script.h"

#include <lua.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace forge::pixel {

// One file of the code: the character's script (name ""), or a module it requires.
struct ScriptFile {
    std::string name;
    std::string text;
    CodeSites code;
    std::string bytecode; // of the instrumented text
    u32 base = 0;         // the id of its first call
};

// One part of a transition chain: body.y(2, 0.1), :to(…), :back(…), :wait(…), part:rest(…), part:along(…),
// part:snap_to(…) …
struct MoveStep {
    enum class Kind : u8 { Tween, Wait, Back, Rest, Restore, Spring, Bind, Along, Snap };
    Kind kind = Kind::Tween;
    f64 length = 0;
    std::vector<std::pair<Prop, PropValue>> props; // Tween, Spring: where each goes; Rest, Bind: which
    vector::Timing timing = vector::Timing::Smooth;
    i32 curve = -1, bezier = -1;
    bool arc = false, arc_reverse = false;
    vector::Spring spring;
    f64 tolerance = 0.001;
    i32 binding = -1;
    i32 along = -1;     // Along: in Recording::alongs
    std::string target; // Snap: the part whose pivot it goes to
    SiteRef site;
    i8 value_arg = -1, length_arg = -1, timing_arg = -1;
    bool value_field = false;
};

// What a call of the clip's code leaves to be laid out on the timeline: a transition chain, a pause, a flow function
// with its tasks.
struct Task {
    enum class Kind : u8 {
        Move, Wait, All, Any, Chain, Sequence, Delay, At, Spawn, Join, Loop, LoopFor, LoopUntil, Run, WaitUntil, Mark,
        Save, TweenFn, Copies, Play,
    };
    Kind kind = Kind::Move;
    u32 part = 0;        // Move, Save
    bool single = false; // a Move of one property (body.y(…)): its :to takes a value, not a table
    Prop prop = Prop::X;
    std::vector<MoveStep> steps;
    std::vector<u32> children; // the tasks a flow function was given; Join: the ones it waits for
    f64 number = 0;            // seconds, or a loop's count (infinite for loop(f)), or how many copies
    u32 copy = 0;              // Copies: the first in Recording::copies
    i32 fn = -1;               // a function's registry ref
    std::string name;          // wait_until, loop_until, emit, sound
    Mark::Kind mark = Mark::Kind::Emit;
    ActorPlay play;            // Play: an actor's clip in a scene (part 6а), its start and order once laid out
    SiteRef site;
    i8 length_arg = -1;
    bool consumed = false, laid = false;
    f64 start = 0, end = 0;
};

// The code of a function running in the clip: its tasks wait in `pending` and go one after another from `cursor`
// when it ends or reads a value. Blocks laid at start_t (an at(…) or delay(…)) take start_site.
struct MotionContext {
    std::vector<u32> pending;
    f64 cursor = 0;
    SiteRef start_site;
    f64 start_t = -1;
};

struct TweenTrack {
    u32 part = 0;
    Prop prop = Prop::X;
    std::vector<PropValue> frames;
    std::vector<u8> set;
};

struct MotionState {
    Recording* rec = nullptr;
    const MotionLengths* lengths = nullptr;
    std::vector<Task> tasks;
    std::vector<MotionContext> contexts;
    std::map<std::string, u32> parts;            // id → rec->parts
    std::map<u32, std::vector<u32>> save_stacks; // part → rec->saves
    std::vector<std::pair<std::string, std::string>> links; // attach(…) so far, part → target: no link goes round
    u32 order = 0;
    struct Deferred {
        u32 task;
        f64 t;
        SiteRef start_site;
        f64 start_t;
    };
    std::vector<Deferred> deferred; // loop(f) under spawn or at: laid out to the clip's end once it is known
    bool main_done = false;
    // tween(s, f): f's calls of body.y(v) go to these frames.
    bool in_tween = false;
    u32 tween_frame = 0, tween_frames = 0;
    f64 tween_t = 0;
    std::vector<TweenTrack> tween;
    int bindings = LUA_NOREF; // the table of the clip's bindings
    std::vector<int> refs;    // functions to release when done
    std::vector<u32> args;    // a flow function's tasks while it reads them
    std::string error;        // why a step failed, raised where Lua runs
    bool from_lua = false;    // error has its place already (a Luau error, or a task's own line)
    bool read = false;        // a value was read (body.y(), now()): no start_from after it (part 5б)
};

// start_from (part 5б) over the tries of one clip's recording: the end pose it starts from once that is recorded, or
// the clip it asked for when it was not yet.
struct StartRequest {
    const StartPose* pose = nullptr; // what start_from puts in; null on the first try
    std::string wanted;              // start_from("…") met with no pose: that clip is recorded first, then this again
    SiteRef site;                    // its call
};

struct PoseScript::Impl : BindingEval {
    lua_State* L = nullptr;
    std::vector<ScriptFile> files; // [0] the script itself
    u32 next_base = 0;
    std::vector<ScriptParam> params;
    std::vector<std::string> clips;
    bool has_pose = false;
    bool has_draw = false;
    std::vector<std::string> drawings; // the names of drawings = { … } (part 4д), sorted
    std::string error;
    u32 error_line = 0;
    bool ok = false;

    enum class Mode : u8 { None, Load, Pose, Record, Frame, Draw };
    Mode mode = Mode::None;
    i32 site = -1; // the call running now (its id)
    ModuleReader reader;
    int modules = LUA_NOREF; // this run's required modules, by name

    // The call running now.
    const Rig* rig = nullptr;
    Pose* pose = nullptr;
    u64 rng = 1;
    u64 seed = 1; // the character's: a binding's chance is the seed's, its frame's and its own
    u64 budget_ns = 100'000'000;
    u64 start_ns = 0;
    u32 interrupts = 0;
    bool in_call = false, timed_out = false;
    lua_State* active = nullptr; // the thread bindings run on

    // A scene (part 6а of docs/models/сцены-модель.md, scene.h): stage and scene functions; the stage under way
    // (stage()), the stage a recording plays (record_scene), the hooks of either.
    bool scene = false;
    Stage* stage = nullptr;
    const Stage* scene_stage = nullptr;
    const SceneHooks* hooks = nullptr;
    // set_scene: the stage and hooks clip "scene" is recorded with by record(), recording() and pose().
    const Stage* bound_stage = nullptr;
    const SceneHooks* bound_hooks = nullptr;

    // Drawing (draw.cpp): the drawing under way, its ids so far, the groups the call is inside.
    Drawing* drawing = nullptr;
    std::set<std::string> drawn_ids;
    std::vector<i32> groups;
    std::string draw_error; // a drawing call's, until it is raised

    // Recording.
    std::unique_ptr<MotionState> motion;
    StartRequest* start = nullptr; // the recording's start_from (part 5б)
    u64 generation = 0;
    std::map<std::string, std::pair<int, u64>> bindings; // clip → its bindings table and the recording's generation
    // Reading a recording: bindings and getters read it at frame_t.
    const Recording* frame_rec = nullptr;
    f64 frame_t = 0;
    u32 binding_depth = 0;

    // pose() of a clip the code records: the recording kept until its inputs change.
    struct Cached {
        std::string key;
        Recording rec;
        bool ok = false;
        std::string error;
        u32 error_line = 0;
    };
    std::map<std::string, Cached> cache;
    std::map<std::string, MotionLengths> lengths;

    Impl();
    ~Impl() override;

    void fail(const char* raw);
    void begin_call();
    // A scene's hook ran from `since` (time_now_ns): its time is not the call's budget's (part 6а).
    void hook_done(u64 since);
    lua_State* module();
    // The file and the place in it of call `id`.
    SiteRef site_ref(i32 id) const;
    const CallSite* call(SiteRef s) const;
    // "строка 12" for the script, "движения.luau, строка 3" for a module.
    std::string where(SiteRef s) const;
    // Compiles a file of the code; false with error set.
    bool compile(ScriptFile& f);

    bool eval(const Recording& rec, i32 binding, Prop prop, f64 t, PropValue& out, std::string& why) override;
};

PoseScript::Impl& impl_of(lua_State* L);

// The value at idx as tostring gives it, pushed: a table, function, userdata, coroutine or buffer without __tostring by
// its kind alone, as "table". Its address changes from run to run and frame to frame, and a pose is a function of time.
const char* value_text(lua_State* L, int idx, size_t* len);

// draw.cpp: the drawing functions of Luau (canvas, rect, circle, ellipse, line, poly, path, group).
void register_draw_api(lua_State* L);

// motion_record.cpp: the clip functions of Luau (part, all, wait …) and the recording.
void register_motion_api(lua_State* L);
// actor("ёжик") in a scene's scene(p): the actor's handle (draw.cpp's actor calls it outside the stage).
int scene_actor(lua_State* L);
// One try of recording a clip: false with start->wanted set when its start_from needs a pose not given yet. With
// `ends` its end pose is made (end_pose) and its bindings are not kept: it is recorded for a clip that starts from it.
bool record_clip(PoseScript::Impl& im, lua_State* T, const Rig* rig, std::string_view clip,
                 const std::map<std::string, f64>& values, u64 seed, const MotionLengths& lengths, Recording& out,
                 StartRequest* start = nullptr, StartPose* ends = nullptr);
void push_params(PoseScript::Impl& im, lua_State* T, const std::map<std::string, f64>& values);
u64 frame_seed(u64 seed, f64 t, std::string_view clip);

} // namespace forge::pixel
