#pragma once

// Time: how fast the world runs, places where it runs differently, and
// things that live on their own clock.
//
// The simulation always ticks at its fixed rate in real time; what changes
// is how much world time each tick carries. Slow motion is short ticks (not
// fewer of them), so movement stays smooth; at 0 the world stands still.
//
//   Simulation::set_time_scale(s)   the whole world: 0 stops it, 0.25 slow, 2 fast
//   TimeBubble                      an area where time runs at its own speed
//   OutsideTime                     an entity on its own clock: world speed,
//                                   bubbles and rewinding leave it alone
//                                   (the player moving normally in a slowed world)
//
// Game systems get the right dt for each entity from each_due(); entities
// whose time stands still are skipped.

#include "forge/core/types.h"
#include "forge/data/reflect.h"
#include "forge/world/coords.h"

#include <span>
#include <vector>

namespace forge::sim {

// Saved with its entity; the entity's Position is the centre.
struct TimeBubble {
    f32 radius = 6;    // tiles
    f32 scale = 0.25f; // time speed inside: 0 stopped, < 1 slower, > 1 faster
    bool fade = false; // full effect at the centre, none at the edge
};

// Saved with its entity.
struct OutsideTime {
    f32 scale = 1; // its own time speed
};

class TimeField {
public:
    struct Placed {
        f64 x, y;
        TimeBubble bubble;
    };
    // Once per tick, before anything moves.
    void build(std::span<const Placed> bubbles);
    bool empty() const { return bubbles_.empty(); }
    // Bubbles' combined speed at a point (1 outside them all; overlapping
    // bubbles multiply).
    f32 at(f64 x, f64 y) const;

private:
    std::vector<Placed> bubbles_;
    world::Rect box_; // tiles every bubble is inside of
};

} // namespace forge::sim

FORGE_REFLECT_DECLARE(forge::sim::TimeBubble)
FORGE_REFLECT_DECLARE(forge::sim::OutsideTime)
