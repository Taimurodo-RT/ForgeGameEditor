// The rules of walking in an RPG seen from above (step 14.4a, 14.4-rpg-модель.md, «Как проверяется 14.4a»): eight
// directions as fast on a diagonal as straight, walls that stop an axis, sliding along a wall, corners, the edge of
// the map and water, long steps that do not pass a thin wall, and the villager the hero talks to, with no window.

#include "rpg_rules.h"

#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

using namespace rpg;

namespace {

// Rows of a map from the top, the first cell at (0, 0): '.' free, '#' a wall or water, ' ' no ground.
Map map_of(const std::vector<std::string>& rows) {
    Map m(0, 0, static_cast<i32>(rows.front().size()), static_cast<i32>(rows.size()));
    for (i32 y = 0; y < m.h(); ++y)
        for (i32 x = 0; x < m.w(); ++x) m.set(x, y, rows[static_cast<usize>(y)][static_cast<usize>(x)] == '.');
    return m;
}

// A room of 9 × 9 free cells with walls around: the middle is (4.5, 5.0).
Map room() {
    return map_of({"###########", "#.........#", "#.........#", "#.........#", "#.........#", "#.........#",
                   "#.........#", "#.........#", "#.........#", "#.........#", "###########"});
}

} // namespace

TEST_CASE("rpg: four actions make eight directions, as fast on a diagonal as straight") {
    struct Case {
        bool up, down, left, right;
        Dir dir;
    };
    const Case cases[] = {
        {false, true, false, false, Dir::S}, {false, true, true, false, Dir::SW}, {false, false, true, false, Dir::W},
        {true, false, true, false, Dir::NW}, {true, false, false, false, Dir::N}, {true, false, false, true, Dir::NE},
        {false, false, false, true, Dir::E}, {false, true, false, true, Dir::SE},
    };
    for (const Case& c : cases) {
        const Heading h = heading(c.up, c.down, c.left, c.right);
        CHECK(std::hypot(h.x, h.y) == doctest::Approx(1.0));
        CHECK(facing(h, Dir::S) == c.dir);
        CHECK((h.x > 0) == c.right);
        CHECK((h.x < 0) == c.left);
        CHECK((h.y > 0) == c.down);
        CHECK((h.y < 0) == c.up);
    }
    // Nothing held, or opposite ones: nowhere, and the hero looks where he looked.
    CHECK(heading(false, false, false, false).none());
    CHECK(heading(true, true, false, false).none());
    CHECK(heading(false, false, true, true).none());
    CHECK(facing(heading(true, true, true, true), Dir::NE) == Dir::NE);
    // Up and down cancel, left stays.
    CHECK(facing(heading(true, true, true, false), Dir::S) == Dir::W);
    CHECK(heading(true, true, true, false).x == -1.0);

    // The same time walks the same way straight and on a diagonal.
    const Map m = room();
    const Heading d = heading(false, true, false, true);
    const Moved diag = move(m, {}, 3.0, 4.0, d.x * 2, d.y * 2, kHeroFeet);
    const Moved straight = move(m, {}, 3.0, 4.0, 2, 0, kHeroFeet);
    CHECK(std::hypot(diag.x - 3.0, diag.y - 4.0) == doctest::Approx(2.0));
    CHECK(std::hypot(straight.x - 3.0, straight.y - 4.0) == doctest::Approx(2.0));
    CHECK_FALSE(diag.hit_x);
    CHECK_FALSE(diag.hit_y);
}

TEST_CASE("rpg: the way to a point is by its angle, 45 degrees each") {
    CHECK(toward(0.05, -0.65, Dir::S) == Dir::N); // a villager a hair to the side of straight up
    CHECK(toward(-0.05, -0.65, Dir::S) == Dir::N);
    CHECK(toward(1, 1, Dir::S) == Dir::SE);
    CHECK(toward(-1, 0.1, Dir::S) == Dir::W);
    CHECK(toward(-1, -1, Dir::S) == Dir::NW);
    CHECK(toward(1, -0.2, Dir::S) == Dir::E);
    CHECK(toward(0, 2, Dir::N) == Dir::S);
    CHECK(toward(-0.5, 1, Dir::N) == Dir::SW); // 116.6°: past 112.5°
    CHECK(toward(-0.3, 1, Dir::N) == Dir::S);  // 106.7°
    CHECK(toward(0, 0, Dir::NE) == Dir::NE);
    CHECK(toward(NAN, 1, Dir::W) == Dir::W);
}

TEST_CASE("rpg: the ways have ids the saves keep") {
    for (u8 i = 0; i < 8; ++i) {
        Dir back = Dir::S;
        REQUIRE(dir_of(dir_id(static_cast<Dir>(i)), back));
        CHECK(back == static_cast<Dir>(i));
    }
    Dir d = Dir::E;
    CHECK_FALSE(dir_of("up", d));
    CHECK_FALSE(dir_of("", d));
    CHECK(d == Dir::E);
}

TEST_CASE("rpg: a wall stops the axis that meets it at its edge") {
    const Map m = room();
    // Right to the wall at x = 10: the feet's right edge stops on it.
    const Moved r = move(m, {}, 5.0, 5.0, 10, 0, kHeroFeet);
    CHECK(r.hit_x);
    CHECK_FALSE(r.hit_y);
    CHECK(r.x == doctest::Approx(10.0 - kHeroFeet.half_w).epsilon(1e-6));
    CHECK(r.y == 5.0);
    // Up to the wall at y = 1: the feet's top stops on it.
    const Moved u = move(m, {}, 5.0, 5.0, 0, -10, kHeroFeet);
    CHECK(u.hit_y);
    CHECK(u.y == doctest::Approx(1.0 + kHeroFeet.depth).epsilon(1e-6));
    // Down to the wall at y = 10: the feet's bottom is the place.
    const Moved d = move(m, {}, 5.0, 5.0, 0, 10, kHeroFeet);
    CHECK(d.y == doctest::Approx(10.0).epsilon(1e-6));
    // Where it stopped, it fits; a hair further does not.
    CHECK(fits(m, {}, r.x, r.y, kHeroFeet));
    CHECK_FALSE(fits(m, {}, r.x + 1e-6, r.y, kHeroFeet));
    CHECK(fits(m, {}, u.x, u.y, kHeroFeet));
    CHECK_FALSE(fits(m, {}, u.x, u.y - 1e-6, kHeroFeet));
    CHECK(fits(m, {}, d.x, d.y, kHeroFeet));
    CHECK_FALSE(fits(m, {}, d.x, d.y + 1e-6, kHeroFeet));
}

TEST_CASE("rpg: on a diagonal into a wall the hero slides along it") {
    const Map m = room();
    // Up and right from near the top wall: y stops, x goes on the whole way.
    const Heading h = heading(true, false, false, true);
    const Moved s = move(m, {}, 3.0, 1.5, h.x * 3, h.y * 3, kHeroFeet);
    CHECK(s.hit_y);
    CHECK_FALSE(s.hit_x);
    CHECK(s.x == doctest::Approx(3.0 + h.x * 3));
    CHECK(s.y == doctest::Approx(1.0 + kHeroFeet.depth).epsilon(1e-6));
    // Into the corner: both stop.
    const Moved c = move(m, {}, 9.0, 1.5, h.x * 3, h.y * 3, kHeroFeet);
    CHECK(c.hit_x);
    CHECK(c.hit_y);
    CHECK(c.x == doctest::Approx(10.0 - kHeroFeet.half_w).epsilon(1e-6));
}

TEST_CASE("rpg: two walls meeting at a corner do not let the hero through between them") {
    // Free cells touch only at a corner: (1,1) and (2,2).
    const Map m = map_of({"####", "#.##", "##.#", "####"});
    const Heading h = heading(false, true, false, true);
    const Moved g = move(m, {}, 1.5, 1.9, h.x * 2, h.y * 2, kHeroFeet);
    CHECK(fits(m, {}, g.x, g.y, kHeroFeet));
    CHECK(cell_x(g.x) == 1);
    CHECK(g.x <= 2.0 - kHeroFeet.half_w);
    CHECK(g.y <= 2.0);
}

TEST_CASE("rpg: the edge of the map and water are walls") {
    // No ground beyond the map; '#' here is water as well as a wall: the map knows only free or not.
    const Map m = map_of({"...", ".#.", "..."});
    CHECK_FALSE(m.free(-1, 0));
    CHECK_FALSE(m.free(3, 1));
    CHECK_FALSE(m.free(1, 1));
    CHECK(m.free(0, 0));
    const Moved left = move(m, {}, 1.0, 1.0, -5, 0, kHeroFeet);
    CHECK(left.hit_x);
    CHECK(left.x == doctest::Approx(kHeroFeet.half_w).epsilon(1e-6));
    // Down from the top row into the water cell's column: stops above it.
    const Moved down = move(m, {}, 1.5, 0.9, 0, 2, kHeroFeet);
    CHECK(down.hit_y);
    CHECK(down.y == doctest::Approx(1.0).epsilon(1e-6));
    // A map made not from 0: its cells are where it says.
    Map far(-64, -64, 128, 128);
    far.set(-64, -64, true);
    far.set(63, 63, true);
    CHECK(far.free(-64, -64));
    CHECK(far.free(63, 63));
    CHECK_FALSE(far.free(64, 63));
    CHECK_FALSE(far.free(-65, -64));
}

TEST_CASE("rpg: a long step does not pass a thin wall") {
    const Map m = map_of({"...#...", "...#...", "...#..."});
    // 50 cells at once (a stall of the game): stops at the wall.
    const Moved r = move(m, {}, 1.0, 2.0, 50, 0, kHeroFeet);
    CHECK(r.hit_x);
    CHECK(r.x == doctest::Approx(3.0 - kHeroFeet.half_w).epsilon(1e-6));
    // Not a number goes nowhere.
    const Moved n = move(m, {}, 1.0, 2.0, NAN, 0, kHeroFeet);
    CHECK(n.x == 1.0);
    CHECK(n.y == 2.0);
}

TEST_CASE("rpg: villagers' feet are walls too") {
    const Map m = room();
    const std::vector<Box> bodies = {feet_box(6.0, 5.0, kHeroFeet)};
    const Moved r = move(m, bodies, 3.0, 5.0, 5, 0, kHeroFeet);
    CHECK(r.hit_x);
    CHECK(r.x == doctest::Approx(6.0 - 2 * kHeroFeet.half_w).epsilon(1e-6));
    // Past it, a row lower: free.
    const Moved below = move(m, bodies, 3.0, 5.5, 5, 0, kHeroFeet);
    CHECK_FALSE(below.hit_x);
    // Put inside a wall or a body: it walks out as told, never stuck.
    const Moved out = move(m, bodies, 6.0, 5.0, 0, 1, kHeroFeet);
    CHECK(out.y == doctest::Approx(6.0));
    CHECK(fits(m, bodies, out.x, out.y, kHeroFeet));
}

TEST_CASE("rpg: the hero talks to the nearest villager in reach, the first of equal ones") {
    const std::vector<Spot> spots = {{5.0, 5.0}, {3.0, 5.0}, {7.0, 5.0}, {4.0, 7.0}};
    CHECK(nearest(spots, 3.2, 5.0, kTalkReach) == 1);
    // Equally far from (4,5) and (6,5)... (5,5) is nearest; between (3,5) and (5,5) at x=4 the first wins.
    CHECK(nearest(spots, 4.0, 5.0, kTalkReach) == 0);
    CHECK(nearest(spots, 6.0, 5.0, kTalkReach) == 0);
    // Exactly at the reach: in.
    CHECK(nearest(spots, 4.0, 5.0 + 2.0, 0.0) == 3);
    CHECK(nearest({{0, 0}}, 1.5, 0, kTalkReach) == 0);
    // Further: none.
    CHECK(nearest({{0, 0}}, 1.51, 0, kTalkReach) == -1);
    CHECK(nearest({}, 0, 0, kTalkReach) == -1);
}

TEST_CASE("rpg: a thing stands in the cell above its feet") {
    // A copy put on the cell (3, 4) stands at (3.5, 5.0).
    CHECK(cell_x(3.5) == 3);
    CHECK(cell_y(5.0) == 4);
    CHECK(cell_x(-0.5) == -1);
    CHECK(cell_y(0.0) == -1);
}
