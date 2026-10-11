#pragma once

// The rules of walking in an RPG seen from above (step 14.4a, 14.4-rpg-модель.md, «Правила»): no window, no files, no
// clock, so every rule is tested on its own (tests/test_rpg.cpp) and the game only moves and draws what they say.
//
// A map is cells one can walk on or not; a body is its feet, a box at the bottom of where it stands (x right, y down,
// in cells; a thing's place is its feet, as everywhere in Forge). The hero goes by four actions that make eight
// directions, as fast on a diagonal as straight; he moves along x, then along y, in short steps, and an axis that
// meets a wall or another body stops at its edge while the other goes on, so he slides along a wall.
//
//   const Heading h = heading(up, down, left, right);
//   const Moved m = move(map, bodies, x, y, h.x * speed * dt, h.y * speed * dt, kHeroFeet);
//   face = facing(h, face);

#include "forge/core/types.h"

#include <string>
#include <string_view>
#include <vector>

namespace rpg {

using forge::f32;
using forge::f64;
using forge::i32;
using forge::u32;
using forge::u64;
using forge::u8;
using forge::usize;

// The cells a map may have: -kReach … kReach-1 each way, so no more than kMaxSide.
inline constexpr i32 kReach = 64;
inline constexpr i32 kMaxSide = 2 * kReach;
// The longest step a move takes at once: a thin wall is never stepped over.
inline constexpr f64 kStepMax = 0.25;
// How far the hero talks: from his feet to a villager's, in cells.
inline constexpr f64 kTalkReach = 1.5;

// A box in the cells of the map.
struct Box {
    f64 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
// Two boxes share some area (touching edges do not).
bool overlap(const Box& a, const Box& b);

// The feet of a body: half_w each side of its x, depth up from its y.
struct Feet {
    f64 half_w = 0.3;
    f64 depth = 0.3;
};
inline constexpr Feet kHeroFeet{0.3, 0.3};
Box feet_box(f64 x, f64 y, const Feet& f);

// Which cells one can walk on.
class Map {
public:
    Map() = default;
    // w × h cells from (x0, y0), none of them free yet.
    Map(i32 x0, i32 y0, i32 w, i32 h);
    i32 x0() const { return x0_; }
    i32 y0() const { return y0_; }
    i32 w() const { return w_; }
    i32 h() const { return h_; }
    // Outside the map: not free.
    bool free(i32 x, i32 y) const;
    void set(i32 x, i32 y, bool free);

private:
    i32 x0_ = 0, y0_ = 0, w_ = 0, h_ = 0;
    std::vector<u8> free_;
};

// Whether feet at (x, y) stand on free cells only and clear of every body.
bool fits(const Map& map, const std::vector<Box>& bodies, f64 x, f64 y, const Feet& f);

// Where the four actions point: nothing when none is held or two opposite ones cancel; a diagonal is 1/√2 each way,
// so the hero is as fast on it as straight.
struct Heading {
    f64 x = 0, y = 0;
    bool none() const { return x == 0 && y == 0; }
};
Heading heading(bool up, bool down, bool left, bool right);

// The eight ways a body looks, clockwise from down (the way a new hero looks).
enum class Dir : u8 { S, SW, W, NW, N, NE, E, SE };
// The way a heading points; previous when it points nowhere (a hero who stops looks where he went).
Dir facing(Heading h, Dir previous);
// The way from a point to another, of the eight, by the angle (45° each): a villager a hair to the side of straight
// up is still up. previous when the points are one.
Dir toward(f64 dx, f64 dy, Dir previous);
// "s", "sw", "w", "nw", "n", "ne", "e", "se" (the saves keep these).
const char* dir_id(Dir d);
// The way an id names; false for anything else.
bool dir_of(std::string_view id, Dir& out);

struct Moved {
    f64 x = 0, y = 0;
    bool hit_x = false, hit_y = false; // the axis stopped short at a wall or a body
};
// Feet at (x, y) go by (dx, dy): along x, then along y, in steps no longer than kStepMax. An axis that would stand on a
// cell not free or on a body stops at its edge (to within a hair) and the other one goes on. Feet that do not fit
// where they stand (put there so) go where they are told until they fit, so a hero is never stuck.
Moved move(const Map& map, const std::vector<Box>& bodies, f64 x, f64 y, f64 dx, f64 dy, const Feet& f);

// The spot nearest to (x, y) no farther than reach, by the distance between the points; of equal ones the first.
// -1 when none is that near.
struct Spot {
    f64 x = 0, y = 0;
};
i32 nearest(const std::vector<Spot>& spots, f64 x, f64 y, f64 reach);

// The cell a thing stands in: its place is its feet, at the bottom middle of the cell.
i32 cell_x(f64 x);
i32 cell_y(f64 y);

} // namespace rpg
