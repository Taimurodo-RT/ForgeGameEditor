#pragma once

// The pose script of a character («Герой.luau», step 2 of docs/models/вектор-модель.md): a Luau module that returns
//
//   return {
//     params = { speed = 1.0, step = 6 },        -- numbers, shown as fields in the tab
//     pose = function(t, p, rng, clip)           -- t: seconds from the clip's start; clip: its state ("walk")
//       local s = math.sin(t * p.speed * 2 * math.pi)
//       part("leg_l"):set{ rot = 28 * s }        -- x, y, rot, scale, sx, sy, opacity, fill
//       shape("mouth"):morph("mouth_open", ease("smooth", 0.5 + 0.5 * s))
//     end,
//   }
//
// The pose is a pure function of time: each frame runs the module afresh, so nothing carries over from one frame to
// the next, and the script sees no clock. The only chance is `rng`, seeded from the seed and the frame's time: rng()
// is 0 … 1, rng(n) 1 … n, rng(a, b) a … b. The game's Luau sandbox (no files, no os, a time budget a call), but a state
// of its own in the editor, not the game's.

#include "forge/pixel/rig.h"

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace forge::pixel {

struct ScriptParam {
    std::string name;
    f64 value = 0; // as the script declares it
};

class PoseScript {
public:
    PoseScript();
    ~PoseScript();
    PoseScript(PoseScript&&) noexcept;
    PoseScript& operator=(PoseScript&&) noexcept;

    // Compiles the module and runs it once. False with error() when it does not compile, fails, or returns no table
    // with a pose function.
    bool load(std::string_view source);
    bool loaded() const;
    // The declared parameters, by name.
    const std::vector<ScriptParam>& params() const;

    // The pose at t for clip `clip`: the declared parameters with `values` over them (names the script does not declare
    // are left out), rng from `seed`. Parts and shapes are checked against the rig. False with error().
    bool pose(const Rig& rig, f64 t, std::string_view clip, const std::map<std::string, f64>& values, u64 seed, Pose& out);

    // Why the last load or pose failed: "строка N: …" when it has a line.
    const std::string& error() const;
    u32 error_line() const; // 0 when none

    // A call's time budget (100 ms until set).
    void set_budget_ms(f64 ms);

    struct Impl; // what the Luau functions of the script reach

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace forge::pixel
