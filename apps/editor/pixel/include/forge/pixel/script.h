#pragma once

// The script of a character («Герой.luau», step 2 of docs/models/вектор-модель.md, clips by step 1 of
// docs/models/движение-модель.md): a Luau module that returns
//
//   local m = require("moves")                   -- animations/moves.luau of the project
//   return {
//     params = { speed = 1.0, height = 6 },      -- numbers, shown as fields in the tab
//     clips = {                                  -- clips written as Motion Canvas writes scenes
//       jump = function(p)
//         local body = part("body")
//         body:to({ y = 2, sy = 0.9 }, 0.1)      -- a line waits for its transition to end
//         all(body.y(-p.height, 0.25, "out_quad"), part("mouth"):morph("mouth_open", 0.1))
//         body.y(0, 0.25, "in_quad"):to(1, 0.05):back(0.1)
//       end,
//     },
//     pose = function(t, p, rng, clip)           -- on top: t is seconds from the clip's start
//       part("head"):set{ rot = 2 * math.sin(t * 8) }
//     end,
//     draw = function(p, rng)                    -- the picture, when the code draws it (draw.h)
//       rect("body", { x = 10, y = 14, w = 12, h = 11, fill = "#4a7bd0" })
//     end,
//   }
//
// A clip is recorded once (motion.h): its function runs with no clock, each transition leaves a block on a track and
// moves the clip's time on. A frame is then read from the tracks at any t, and pose(t, …) adds to it. A script of
// pose alone is a pure function of time as before: each frame runs the module afresh. The only chance is `rng`,
// seeded from the seed (and the frame's time in pose): rng() is 0 … 1, rng(n) 1 … n, rng(a, b) a … b. The game's
// Luau sandbox (no files, no os, a time budget a call), but a state of its own in the editor, not the game's.

#include "forge/pixel/code_sites.h"
#include "forge/pixel/motion.h"
#include "forge/pixel/rig.h"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace forge::pixel {

struct Drawing;
struct Stage;

struct ScriptParam {
    std::string name;
    f64 value = 0; // as the script declares it
};

// require("name"): the text of the project's animations/name.luau; false with why (Russian) when there is none.
using ModuleReader = std::function<bool(std::string_view name, std::string& text, std::string& why)>;
// The reader of a game's code (step 3 of docs/models/движение-модель.md): animations/name.luau in the game's folder
// (beside its game.json). A name with a folder in it is refused.
ModuleReader animations_reader(std::filesystem::path game_dir);

// What a scene's code reaches beyond itself (part 6а of docs/models/сцены-модель.md, scene.h), each false with why.
struct SceneHooks {
    // picture(id, file, …): the size of a picture of «Ресурсы», in pixels.
    std::function<bool(const std::string& file, f64& w, f64& h, std::string& why)> picture;
    // actor(id, character, …): the area of a character's picture in its units: x, y, w, h.
    std::function<bool(const std::string& file, f64 area[4], std::string& why)> character;
    // actor("ёжик"):play("walk", { params }): the length of a clip of the actor's character, recorded with the actor's
    // parameters and `params` over them.
    std::function<bool(const std::string& actor, const std::string& clip, const std::map<std::string, f64>& params, f64& length,
                       std::string& why)>
        clip_length;
};

class PoseScript {
public:
    PoseScript();
    ~PoseScript();
    PoseScript(PoseScript&&) noexcept;
    PoseScript& operator=(PoseScript&&) noexcept;

    // Parses the code (with the place of every call), compiles it and runs the module once. False with error() when it
    // does not parse or compile, fails, or returns no table with clips or a pose function.
    bool load(std::string_view source);
    bool loaded() const;
    // The declared parameters, by name.
    const std::vector<ScriptParam>& params() const;
    // The clips the module's `clips` table has, by name; whether it has pose.
    const std::vector<std::string>& clips() const;
    bool has_pose() const;
    // Whether it has draw = function(p, rng) … end: the character's picture is the code's (draw.h).
    bool has_draw() const;
    // The other drawings of the character (part 4д of docs/models/движение-модель.md): the names of its
    // drawings = { angry = function(p, rng) … end }, sorted; "draw" is never one of them.
    const std::vector<std::string>& drawings() const;
    // A scene (part 6а of docs/models/сцены-модель.md): the module returns stage = function(p, rng) and
    // scene = function(p), and neither clips, pose, draw nor drawings.
    bool is_scene() const;
    // Runs a scene's stage as draw runs: the declared parameters with `values` over them, rng from `seed`; its pictures
    // and characters looked at through `hooks`. False with error(), the line of the call that failed in error_line().
    bool stage(const std::map<std::string, f64>& values, u64 seed, const SceneHooks& hooks, Stage& out);
    // Records a scene's scene(p) as a clip "scene" of the stage's rig (its actors hosted in it, Rig::host): the
    // stage's actors and pictures are its parts, «ёжик/head» an actor's; the actors' clips' lengths come from
    // `hooks`. False with error().
    bool record_scene(const Rig& rig, const Stage& stage, const SceneHooks& hooks, const std::map<std::string, f64>& values,
                      u64 seed, const MotionLengths& lengths, Recording& out);
    // A scene's stage and hooks (Scene::load does it; null for none): from then on the scene is clip "scene" of
    // clips(), and record(), recording() and pose() record and read it as a character's clip, with the rig given
    // (the stage's). They live while this script uses them. Nothing for a script that is not a scene.
    void set_scene(const Stage* stage, const SceneHooks* hooks);
    // Every call of the loaded code and the clip functions: what SiteRef file 0 points into.
    const CodeSites& code() const;
    // The calls of a module a recording required (SiteRef files past 0, by Recording::files); null when not read.
    const CodeSites* module_code(std::string_view name) const;

    // Where require reads modules; without one, require fails.
    void set_module_reader(ModuleReader reader);

    // Records clip `clip`: the declared parameters with `values` over them, rng from `seed`, the time events' lengths
    // from `lengths`. Parts and shapes are checked against the rig when there is one. False with error().
    bool record(const Rig* rig, std::string_view clip, const std::map<std::string, f64>& values, u64 seed,
                const MotionLengths& lengths, Recording& out);
    // The pose at t of a recording of this script: its tracks, its bindings, then pose(t, …) on top. False with error().
    bool frame(const Recording& rec, const Rig* rig, f64 t, const std::map<std::string, f64>& values, u64 seed,
               Pose& out);

    // Runs draw: the declared parameters with `values` over them, rng from `seed` alone. False with error(), the line
    // of the call that failed in error_line().
    bool draw(const std::map<std::string, f64>& values, u64 seed, Drawing& out);
    // Runs drawing `name` of drawings the same way ("draw" is draw itself). False with error(): «рисунок «angry»,
    // строка 12: …», the line in error_line().
    bool draw_named(std::string_view name, const std::map<std::string, f64>& values, u64 seed, Drawing& out);

    // The pose at t for clip `clip`: a clip of `clips` recorded (kept until the code, the values, the seed or its
    // lengths change) and read at t, pose(t, …) on top; or pose alone. Parts and shapes are checked against the rig.
    // False with error().
    bool pose(const Rig& rig, f64 t, std::string_view clip, const std::map<std::string, f64>& values, u64 seed, Pose& out);
    // The lengths pose() records `clip` with.
    void set_lengths(std::string_view clip, const MotionLengths& lengths);
    // The lengths of all clips, by clip, in place of those set before: a clip a start_from names is recorded with its
    // own (part 5б).
    void set_lengths(const std::map<std::string, MotionLengths>& all);
    // The recording of clip `clip` that pose() reads, made when the code, the values, the seed or its lengths changed
    // and kept until they change again (or forget()); null with error() when the code has no such clip or it fails.
    const Recording* recording(const Rig& rig, std::string_view clip, const std::map<std::string, f64>& values, u64 seed);
    // The recordings kept are made again on the next pose() or recording(): a module the code requires changed.
    void forget();

    // Why the last load, record, frame or pose failed: "строка N: …" when it has a line of the script,
    // "движения.luau, строка N: …" for a module.
    const std::string& error() const;
    u32 error_line() const; // 0 when none (or the line is a module's)

    // A call's time budget (100 ms until set); a recording has ten times as much.
    void set_budget_ms(f64 ms);

    struct Impl; // what the Luau functions of the script reach

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace forge::pixel
