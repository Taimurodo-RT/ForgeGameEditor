// Part 4в of docs/models/движение-модель.md («Пути и крепления»): along takes a part along the line of a shape,
// snap_to puts its pivot where another part's stands, attach and detach carry it with another part. A path's points
// are checked against the formula of its line; a carried part is the same picture, byte for byte, as the SVG with the
// carry written round it as a matrix worked out here. A hidden shape stands where its transform puts it.

#include "forge/pixel/motion.h"
#include "forge/pixel/motion_edit.h"
#include "forge/pixel/rig.h"
#include "forge/pixel/script.h"
#include "forge/pixel/timeline.h"
#include "forge/vector/timing.h"
#include "forge/vector/trig.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <ostream> // doctest's INFO prints a string_view through it; MSVC needs the whole class
#include <string>
#include <vector>

using namespace forge;
using namespace forge::pixel;

namespace {

// A body, an arm with a hand and its tip, a sword with a blade and a grip, a ball, and hidden shapes to go along: a
// line, a broken line, a rectangle, a circle, a line in a moved group, a path of two contours, and a broken line hidden
// with a transform of its own beside the same one shown.
struct Pic {
    std::map<std::string, std::string> transform; // per id: a transform attribute put in place of its own
    std::map<std::string, std::string> wrap;      // per id: a group round it with this transform
    std::string tail;

    std::string tr(const std::string& id, const char* own = nullptr) const {
        const auto it = transform.find(id);
        if (it != transform.end()) return " transform=\"" + it->second + "\"";
        return own ? std::string(" transform=\"") + own + "\"" : std::string();
    }
    std::string w(const std::string& id, const std::string& el) const {
        const auto it = wrap.find(id);
        return it == wrap.end() ? el : "<g transform=\"" + it->second + "\">" + el + "</g>";
    }
    std::string text() const {
        std::string s = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" width="64" height="64">)"
                        "\n";
        s += "  <g id=\"body\"" + tr("body") + ">" +
             w("torso", "<rect id=\"torso\" x=\"24\" y=\"24\" width=\"12\" height=\"16\" fill=\"#3060c0\"" + tr("torso") + "/>") +
             "</g>\n";
        s += "  <g id=\"arm\"" + tr("arm", "translate(2 1)") + ">" +
             w("hand", "<rect id=\"hand\" x=\"40\" y=\"30\" width=\"6\" height=\"6\" fill=\"#c03030\"" + tr("hand") + "/>") +
             "<circle id=\"tip\" cx=\"43\" cy=\"28\" r=\"1.5\" fill=\"#202020\"" + tr("tip") + "/></g>\n";
        s += "  " + w("sword", "<g id=\"sword\"" + tr("sword") +
                                   "><rect id=\"blade\" x=\"11\" y=\"4\" width=\"3\" height=\"10\" fill=\"#d0d0e0\"/>"
                                   "<rect id=\"grip\" x=\"10\" y=\"14\" width=\"5\" height=\"4\" fill=\"#806040\"/></g>") +
             "\n";
        s += "  <circle id=\"ball\" cx=\"8\" cy=\"50\" r=\"3\" fill=\"#30a060\"" + tr("ball") + "/>\n";
        s += R"svg(  <path id="route" d="M 8 50 L 56 50" display="none"/>
  <polyline id="zig" points="4,4 14,4 14,14" fill="none" display="none"/>
  <rect id="box" x="10" y="40" width="20" height="10" display="none"/>
  <circle id="ring" cx="32" cy="32" r="10" display="none"/>
  <g id="lane" transform="translate(5 3)"><path id="road" d="M 0 0 L 10 0" display="none"/></g>
  <path id="two" d="M 0 0 L 5 0 M 10 0 L 15 0" display="none"/>
  <path id="bent" d="M 0 0 L 10 0 L 10 5" transform="translate(30 5) rotate(30)" display="none"/>
  <path id="bent_shown" d="M 0 0 L 10 0 L 10 5" transform="translate(30 5) rotate(30)" fill="none" stroke="#000000" stroke-width="0.5"/>
)svg";
        return s + tail + "</svg>";
    }
};

std::string picture() { return Pic{}.text(); }

Rig loaded(std::string_view svg) {
    Rig rig;
    std::string why;
    const bool ok = rig.load(svg, why);
    INFO(why);
    REQUIRE(ok);
    return rig;
}

PoseScript script_of(std::string_view source) {
    PoseScript s;
    const bool ok = s.load(source);
    INFO(s.error());
    REQUIRE(ok);
    return s;
}

// A script of one clip, "c", its body from line 2 on.
std::string clip_of(std::string_view body) { return "return { clips = { c = function(p, rng)\n" + std::string(body) + "\nend } }"; }

Recording recorded(PoseScript& s, const Rig* rig) {
    Recording rec;
    const bool ok = s.record(rig, "c", {}, 1, {}, rec);
    INFO(s.error());
    REQUIRE(ok);
    return rec;
}

std::string record_error(PoseScript& s, const Rig* rig) {
    Recording rec;
    CHECK_FALSE(s.record(rig, "c", {}, 1, {}, rec));
    return s.error();
}

Pose frame(PoseScript& s, const Recording& rec, const Rig* rig, f64 t) {
    Pose pose;
    const bool ok = s.frame(rec, rig, t, {}, 1, pose);
    INFO(s.error());
    REQUIRE(ok);
    return pose;
}

const Block& block(const Recording& rec, std::string_view part, Prop p, usize nth = 0) {
    for (const Block& b : rec.blocks)
        if (rec.parts[b.part] == part && b.prop == p && nth-- == 0) return b;
    FAIL("no block ", part, ".", prop_word(p));
    return rec.blocks.front();
}

PathLine path(const Rig& rig, const char* id) {
    PathLine line;
    std::string why;
    const bool ok = rig.path_of(rig.part(id), line, why);
    INFO(why);
    REQUIRE(ok);
    return line;
}

std::string path_error(const Rig& rig, const char* id) {
    PathLine line;
    std::string why;
    CHECK_FALSE(rig.path_of(rig.part(id), line, why));
    return why;
}

// %.9g of a float: it reads back as the same float.
std::string f32_text(f64 v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.9g", static_cast<f64>(static_cast<f32>(v)));
    return buf;
}

std::string matrix(const Affine& m) {
    return "matrix(" + f32_text(m.a) + " " + f32_text(m.b) + " " + f32_text(m.c) + " " + f32_text(m.d) + " " + f32_text(m.e) +
           " " + f32_text(m.f) + ")";
}

usize bytes_off(const assets::CookedTexture& a, const assets::CookedTexture& b) {
    usize off = 0;
    for (usize i = 0; i < a.rgba8.size() && i < b.rgba8.size(); ++i) off += a.rgba8[i] != b.rgba8[i];
    return off;
}

// The posed rig and the reference drawn alike: smooth at 128 × 128, byte for byte.
void same_picture(Rig& posed, const std::string& reference) {
    Rig ref = loaded(reference);
    const assets::CookedTexture a = posed.draw(128, 128), b = ref.draw(128, 128);
    CHECK(b.rgba8 != loaded(picture()).draw(128, 128).rgba8); // the reference shows it: not the picture at rest
    INFO("bytes off: ", bytes_off(a, b));
    CHECK(a.rgba8 == b.rgba8);
}

bool near(vector::Point a, vector::Point b, f64 eps = 1e-9) { return std::abs(a.x - b.x) <= eps && std::abs(a.y - b.y) <= eps; }

// Where a part's pivot stands after the last pose, in picture units.
vector::Point pivot_now(const Rig& rig, const char* id) {
    const i32 p = rig.part(id);
    return rig.carried(p).apply(rig.pivot(p));
}

PartPose moved(f64 x, f64 y, f64 rot = 0, f64 scale = 1) {
    PartPose pp;
    pp.x = x, pp.y = y, pp.rot = rot, pp.scale = scale;
    return pp;
}

} // namespace

TEST_CASE("Motion paths: a shape's line by its length, against the formula of each") {
    const Rig rig = loaded(picture());

    // A line: 48 long, straight to the right.
    const PathLine route = path(rig, "route");
    CHECK_FALSE(route.closed);
    CHECK(route.length() == doctest::Approx(48));
    f64 angle = -1;
    CHECK(near(path_point(route, 0.25, &angle), {20, 50}));
    CHECK(angle == 0);
    CHECK(near(path_point(route, 0), {8, 50}));
    CHECK(near(path_point(route, 1), {56, 50}));
    // An open line holds a share past its ends there.
    CHECK(near(path_point(route, -0.5), {8, 50}));
    CHECK(near(path_point(route, 1.5), {56, 50}));

    // A broken line: equal shares of its length, the corner where the second segment starts, turned down there.
    const PathLine zig = path(rig, "zig");
    CHECK(zig.length() == doctest::Approx(20));
    CHECK(near(path_point(zig, 0.25, &angle), {9, 4}));
    CHECK(angle == 0);
    CHECK(near(path_point(zig, 0.5, &angle), {14, 4}));
    CHECK(angle == doctest::Approx(90));
    CHECK(near(path_point(zig, 0.75, &angle), {14, 9}));
    CHECK(angle == doctest::Approx(90));

    // A rectangle: closed, 60 round; each share 60 · u along its sides from where it starts, the way it goes.
    const PathLine box = path(rig, "box");
    CHECK(box.closed);
    CHECK(box.length() == doctest::Approx(60));
    const vector::Point corners[] = {{10, 40}, {30, 40}, {30, 50}, {10, 50}};
    const vector::Point first = path_point(box, 0);
    usize start = 4;
    for (usize i = 0; i < 4; ++i)
        if (near(first, corners[i], 1e-6)) start = i;
    REQUIRE(start < 4); // it starts at a corner
    const vector::Point next = path_point(box, 0.01);
    // It goes the way of the side from the start to the next corner when the point a little on lies on that side.
    const vector::Point c1 = corners[(start + 1) % 4];
    const f64 cross = (c1.x - first.x) * (next.y - first.y) - (c1.y - first.y) * (next.x - first.x);
    const f64 dot = (c1.x - first.x) * (next.x - first.x) + (c1.y - first.y) * (next.y - first.y);
    const bool forward = std::abs(cross) < 1e-9 && dot > 0;
    auto walk = [&](f64 d) { // along the sides from the start corner
        usize c = start;
        for (;;) {
            const usize n = forward ? (c + 1) % 4 : (c + 3) % 4;
            const f64 side = std::hypot(corners[n].x - corners[c].x, corners[n].y - corners[c].y);
            if (d <= side) return vector::Point{corners[c].x + (corners[n].x - corners[c].x) * d / side, corners[c].y + (corners[n].y - corners[c].y) * d / side};
            d -= side;
            c = n;
        }
    };
    for (f64 u : {0.1, 0.25, 0.4, 0.5, 0.7, 0.9}) {
        INFO("u ", u);
        CHECK(near(path_point(box, u), walk(60 * u), 1e-6));
    }
    // A closed line goes round past 1: its direction on by one turn, the same point.
    f64 a0 = 0, a1 = 0;
    CHECK(near(path_point(box, 0.1, &a0), path_point(box, 1.1, &a1), 1e-9));
    CHECK(std::abs(a1 - a0) == doctest::Approx(360));
    CHECK(near(path_point(box, -0.1), path_point(box, 0.9), 1e-9));

    // A circle (four cubics in ThorVG): on its radius, a quarter of the length a quarter turn round, its direction at
    // right angles to the radius, a whole turn by the end with no jump on the way.
    const PathLine ring = path(rig, "ring");
    CHECK(ring.closed);
    CHECK(ring.length() == doctest::Approx(2 * vector::trig::kPi * 10).epsilon(1e-3));
    f64 before = 0;
    path_point(ring, 0, &before);
    const f64 start_angle = before;
    // Its angles grow going clockwise on screen: the direction a quarter turn on from the radius; back the other way.
    f64 end_angle = 0;
    path_point(ring, 0.999, &end_angle);
    const f64 quarter = end_angle > start_angle ? 90 : -90;
    for (int i = 1; i <= 40; ++i) {
        const f64 u = i / 40.0;
        f64 a = 0;
        const vector::Point p = path_point(ring, u, &a);
        INFO("u ", u);
        CHECK(std::hypot(p.x - 32, p.y - 32) == doctest::Approx(10).epsilon(1e-3));
        CHECK(std::abs(a - before) < 20); // no jump past 180
        // The direction is the radius's turned a quarter (the way the circle goes).
        const f64 radius = std::atan2(p.y - 32, p.x - 32) * 180 / vector::trig::kPi;
        CHECK(std::abs(std::remainder(a - radius - quarter, 360.0)) < 1.5);
        before = a;
    }
    CHECK(std::abs(before - start_angle) == doctest::Approx(360).epsilon(1e-6));
    const vector::Point q0 = path_point(ring, 0.1), q1 = path_point(ring, 0.35);
    const f64 turn = std::remainder((std::atan2(q1.y - 32, q1.x - 32) - std::atan2(q0.y - 32, q0.x - 32)) * 180 / vector::trig::kPi, 360.0);
    CHECK(std::abs(turn) == doctest::Approx(90).epsilon(2e-3));

    // A line in a moved group is where the group puts it.
    const PathLine road = path(rig, "road");
    CHECK(near(path_point(road, 0), {5, 3}));
    CHECK(near(path_point(road, 0.5), {10, 3}));

    // A hidden shape with a transform of its own: the line of the same shape shown, point for point.
    const PathLine bent = path(rig, "bent"), shown = path(rig, "bent_shown");
    REQUIRE(bent.points.size() == shown.points.size());
    for (usize i = 0; i < bent.points.size(); ++i) CHECK(near(bent.points[i], shown.points[i], 0));
    CHECK(near(path_point(bent, 0), {30, 5}, 1e-5));
    CHECK(near(path_point(bent, 2.0 / 3), {30 + 10 * std::cos(vector::trig::kPi / 6), 5 + 10 * std::sin(vector::trig::kPi / 6)}, 1e-5));

    // What is no path.
    CHECK(path_error(rig, "lane") == "путь «lane» — группа, а нужна фигура");
    CHECK(path_error(rig, "two") == "у пути «two» больше одного контура: нужна одна линия");
    Pic with_dot;
    with_dot.tail = R"svg(  <path id="dot" d="M 5 5 L 5 5" display="none"/>
)svg";
    CHECK(path_error(loaded(with_dot.text()), "dot") == "у пути «dot» длина 0");
}

TEST_CASE("Motion paths: along moves a part's pivot along the line by the timing") {
    const Rig rig = loaded(picture());
    const vector::Point ball = rig.pivot(rig.part("ball"));
    REQUIRE(near(ball, {8, 50}, 1e-9));

    // Equal shares of the length for equal shares of the timing; x, y and rot as one call, then the chain goes on.
    PoseScript s = script_of(clip_of(R"(part("ball"):along("route", 1, "linear")
part("ball").x(0, 1, "linear"))"));
    Recording rec = recorded(s, &rig);
    CHECK(rec.length == doctest::Approx(2));
    REQUIRE(rec.paths.size() == 1);
    CHECK(rec.paths[0].id == "route");
    CHECK(block(rec, "ball", Prop::X).to == Block::To::Along);
    CHECK(block(rec, "ball", Prop::Y).to == Block::To::Along);
    CHECK(rec.track(0, Prop::Rot) < 0); // no turn, no rot
    for (f64 t : {0.0, 0.25, 0.5, 1.0}) {
        INFO("t ", t);
        const Pose p = frame(s, rec, &rig, t);
        CHECK(p.parts.at("ball").x == doctest::Approx(48 * t));
        CHECK(p.parts.at("ball").y == doctest::Approx(0).epsilon(1e-12));
    }
    // The next transition goes from where the path ended.
    CHECK(frame(s, rec, &rig, 1.5).parts.at("ball").x == doctest::Approx(24));
    CHECK(block_words(rec, block(rec, "ball", Prop::X)) == "по route");
    CHECK(block_words(rec, block(rec, "ball", Prop::Y)) == "по route");
    // Its end (the block's «куда» in its title): the path's end.
    CHECK(block(rec, "ball", Prop::X).end.v == doctest::Approx(48));

    // A timing word: the share of the length is the timing's.
    s = script_of(clip_of(R"(part("ball"):along("route", 1, "smooth"))"));
    rec = recorded(s, &rig);
    CHECK(frame(s, rec, &rig, 0.25).parts.at("ball").x == doctest::Approx(48 * vector::timing(vector::Timing::Smooth, 0.25)));
    // bezier(…) too (part 4ж).
    s = script_of(clip_of(R"(part("ball"):along("route", 1, bezier(0.3, 0, 0.2, 1)))"));
    rec = recorded(s, &rig);
    CHECK(frame(s, rec, &rig, 0.25).parts.at("ball").x == doctest::Approx(48 * vector::bezier({0.3, 0, 0.2, 1}, 0.25)));
    // back overshoots: an open path holds the part at its ends.
    s = script_of(clip_of(R"(part("ball"):along("route", 1, "back"))"));
    rec = recorded(s, &rig);
    bool held = false;
    for (int i = 0; i <= 100; ++i) {
        const f64 x = frame(s, rec, &rig, i / 100.0).parts.at("ball").x;
        CHECK(x >= -1e-9);
        CHECK(x <= 48 + 1e-9);
        held = held || x == 48;
    }
    CHECK(held);

    // from, to and back the other way; relative from where the part is; with turn the direction as rot.
    s = script_of(clip_of(R"(part("ball"):along("route", 1, "linear", { from = 0.75, to = 0.25 }))"));
    rec = recorded(s, &rig);
    CHECK(frame(s, rec, &rig, 0).parts.at("ball").x == doctest::Approx(36)); // at once at from, not where it was
    CHECK(frame(s, rec, &rig, 0.5).parts.at("ball").x == doctest::Approx(24));
    CHECK(frame(s, rec, &rig, 1).parts.at("ball").x == doctest::Approx(12));
    // Asked before its start (as block_value is), the block is where the part was, as every other block is.
    s = script_of(clip_of(R"(part("ball").x(5, 1, "linear")
part("ball"):along("route", 1, "linear", { from = 0.75 }))"));
    rec = recorded(s, &rig);
    {
        const Block& on = block(rec, "ball", Prop::X, 1);
        PropValue before;
        std::string why;
        REQUIRE(block_value(rec, on, on.start - 0.25, nullptr, before, why));
        CHECK(before.v == doctest::Approx(5));
        CHECK(frame(s, rec, &rig, on.start).parts.at("ball").x == doctest::Approx(36));
    }

    s = script_of(clip_of(R"(part("ball"):to({ x = 3, y = 2 }, 0)
part("ball"):along("route", 1, "linear", { relative = true, from = 0.5 }))"));
    rec = recorded(s, &rig);
    CHECK(frame(s, rec, &rig, 0).parts.at("ball").x == doctest::Approx(3));
    CHECK(frame(s, rec, &rig, 0.5).parts.at("ball").x == doctest::Approx(3 + 12));
    CHECK(frame(s, rec, &rig, 0.5).parts.at("ball").y == doctest::Approx(2));

    // turn: the direction of the line, the way the part goes (back the other way: turned round), within −180 … 180 at
    // its start; round a closed line a whole turn with no jump.
    s = script_of(clip_of(R"(part("ball"):along("zig", 1, "linear", { turn = true })
part("ball"):along("route", 1, "linear", { turn = true, from = 1, to = 0 }))"));
    rec = recorded(s, &rig);
    CHECK(block_words(rec, block(rec, "ball", Prop::Rot)) == "по zig, поворот");
    CHECK(frame(s, rec, &rig, 0.25).parts.at("ball").rot == doctest::Approx(0));
    CHECK(frame(s, rec, &rig, 0.75).parts.at("ball").rot == doctest::Approx(90));
    CHECK(frame(s, rec, &rig, 1.5).parts.at("ball").rot == doctest::Approx(180));
    CHECK(frame(s, rec, &rig, 1.5).parts.at("ball").x == doctest::Approx(24 - 0)); // halfway back along route: 32 − 8

    s = script_of(clip_of(R"(part("ball"):along("ring", 1, "linear", { turn = true }))"));
    rec = recorded(s, &rig);
    f64 last = frame(s, rec, &rig, 0).parts.at("ball").rot;
    CHECK(last > -180 - 1e-9);
    CHECK(last <= 180 + 1e-9);
    const f64 first = last;
    for (int i = 1; i <= 50; ++i) {
        const f64 r = frame(s, rec, &rig, i / 50.0).parts.at("ball").rot;
        CHECK(std::abs(r - last) < 20);
        last = r;
    }
    CHECK(std::abs(last - first) == doctest::Approx(360).epsilon(1e-6));
    // On the circle: the pivot on the radius at each time.
    const Pose half = frame(s, rec, &rig, 0.3);
    CHECK(std::hypot(8 + half.parts.at("ball").x - 32, 50 + half.parts.at("ball").y - 32) == doctest::Approx(10).epsilon(1e-3));

    // Back up the broken line's second segment: turned round to −90, not 270; a closed line from three quarters of its
    // way round starts within −180 … 180 all the same.
    s = script_of(clip_of(R"(part("ball"):along("zig", 1, "linear", { turn = true, from = 1, to = 0.5 }))"));
    rec = recorded(s, &rig);
    CHECK(frame(s, rec, &rig, 0).parts.at("ball").rot == doctest::Approx(-90));
    s = script_of(clip_of(R"(part("ball"):along("ring", 1, "linear", { turn = true, from = 0.75 }))"));
    rec = recorded(s, &rig);
    const f64 late = frame(s, rec, &rig, 0).parts.at("ball").rot;
    CHECK(late > -180 - 1e-9);
    CHECK(late <= 180 + 1e-9);

    // A path in a moved group: where the group puts it.
    s = script_of(clip_of(R"(part("ball"):along("road", 1, "linear"))"));
    rec = recorded(s, &rig);
    CHECK(frame(s, rec, &rig, 0).parts.at("ball").x == doctest::Approx(5 - 8));
    CHECK(frame(s, rec, &rig, 0).parts.at("ball").y == doctest::Approx(3 - 50));
}

TEST_CASE("Motion paths: a part in a moving group goes along the path with its group") {
    Rig rig = loaded(picture());
    // The tip goes along the route while its arm moves and turns: the path is inside the arm's own coordinates.
    PoseScript s = script_of(clip_of(R"(all(part("tip"):along("route", 1, "linear"), part("arm"):to({ x = 5, rot = 20 }, 1, "linear")))"));
    const Recording rec = recorded(s, &rig);
    const vector::Point tip = rig.pivot(rig.part("tip")), arm = rig.pivot(rig.part("arm"));
    for (f64 t : {0.0, 0.4, 1.0}) {
        INFO("t ", t);
        const Pose p = frame(s, rec, &rig, t);
        std::string why;
        REQUIRE(rig.pose(p, why));
        const Affine w = pose_matrix(moved(5 * t, 0, 20 * t), arm);
        CHECK(near(pivot_now(rig, "tip"), w.apply(path_point(path(rig, "route"), t)), 1e-4));
        CHECK(p.parts.at("tip").x == doctest::Approx(path_point(path(rig, "route"), t).x - tip.x));
    }
}

TEST_CASE("Motion places: snap_to puts the part's pivot where the other's stands, however both stand") {
    Rig rig = loaded(picture());
    // The tip in a moved, turned and scaled arm; the sword in a turned group of its own.
    Pic sheathed;
    sheathed.wrap["sword"] = "rotate(20 12 16)";
    Rig in_group = loaded(sheathed.text());
    for (Rig* r : {&rig, &in_group}) {
        PoseScript s = script_of(clip_of(R"(part("arm"):to({ x = 2, y = -3, rot = 30, scale = 1.5 }, 0)
part("sword"):to({ rot = 15, sx = 0.8 }, 0)
part("sword"):snap_to("tip", 0.2, "out_quad")
wait(0.1))"));
        const Recording rec = recorded(s, r);
        const Block& x = block(rec, "sword", Prop::X);
        CHECK(x.to == Block::To::Snap);
        CHECK(x.length == doctest::Approx(0.2));
        CHECK(x.timing == vector::Timing::OutQuad);
        CHECK(block_words(rec, x) == "к tip");
        CHECK(block_words(rec, block(rec, "sword", Prop::Y)) == "к tip");
        std::string why;
        REQUIRE(r->pose(frame(s, rec, r, 0.2), why));
        // The rig stands in ThorVG's float matrices: the same point to a float's last digits.
        CHECK(near(pivot_now(*r, "sword"), pivot_now(*r, "tip"), 1e-4));
        // It does not follow afterwards: the numbers are where the tip stood at the call.
        CHECK(block(rec, "sword", Prop::X).value.v == doctest::Approx(x.value.v));
    }

    // From inside a moving group and from a link: the numbers are in the frame the part stands in at the call.
    struct From {
        const char* code;
        const char* part;
    };
    for (const From& f : {From{R"(part("arm"):to({ x = 2, y = -3, rot = 30, scale = 1.5 }, 0)
part("tip"):snap_to("ball", 0.2))", "tip"},
                          From{R"(part("sword"):attach("hand")
part("hand"):to({ x = 4, rot = -25, scale = 1.2 }, 0)
part("sword"):snap_to("ball", 0.2))", "sword"}}) {
        INFO(f.code);
        PoseScript s = script_of(clip_of(f.code));
        const Recording rec = recorded(s, &rig);
        std::string why;
        REQUIRE(rig.pose(frame(s, rec, &rig, 0.2), why));
        CHECK(near(pivot_now(rig, f.part), pivot_now(rig, "ball"), 1e-4));
    }

    // To a part carried by a link: where the link has carried it.
    PoseScript s = script_of(clip_of(R"(part("hand"):attach("torso")
part("body"):to({ x = -6, y = 4, rot = 10 }, 0.5, "linear")
part("sword"):snap_to("hand"))"));
    const Recording rec = recorded(s, &rig);
    std::string why;
    REQUIRE(rig.pose(frame(s, rec, &rig, 0.5), why));
    CHECK(near(pivot_now(rig, "sword"), pivot_now(rig, "hand"), 1e-4));
    CHECK_FALSE(near(pivot_now(rig, "hand"), rig.pivot(rig.part("hand")), 0.5)); // the hand was carried

    auto error = [&](const char* body) {
        PoseScript e = script_of(clip_of(body));
        return record_error(e, &rig);
    };
    CHECK(error(R"(part("sword"):snap_to("nothing"))") == "строка 2: в рисунке нет части «nothing»");
    CHECK(error(R"(part("sword"):snap_to("sword"))") == "строка 2: часть «sword» не встанет к самой себе");
    CHECK(error(R"(part("sword"):snap_to("blade"))") == "строка 2: часть «sword» не встанет к своей же «blade»");
    CHECK(error(R"(part("sword"):snap_to(3))") == "строка 2: snap_to(\"hand_tip\", с, сглаживание): первым — имя части");
}

TEST_CASE("Motion links: a carried part is the SVG with the carry written round it") {
    Rig rig = loaded(picture());
    const vector::Point hand = rig.pivot(rig.part("hand")), arm = rig.pivot(rig.part("arm")), body = rig.pivot(rig.part("body"));

    // The hand moved, turned and scaled, its arm moving too: the sword goes with the hand from the call on.
    PoseScript s = script_of(clip_of(R"(part("sword"):attach("hand")
all(part("hand"):to({ x = 4, y = -2, rot = 30, scale = 1.5 }, 1, "linear"), part("arm"):to({ x = -3, rot = -10 }, 1, "linear")))"));
    Recording rec = recorded(s, &rig);
    CHECK(block_words(rec, block(rec, "sword", Prop::Link)) == "с hand");
    CHECK(block(rec, "sword", Prop::Link).length == 0);
    Pose p = frame(s, rec, &rig, 1);
    CHECK(p.parts.at("sword").carry_by == "hand");
    std::string why;
    REQUIRE(rig.pose(p, why));
    const Affine c = pose_matrix(moved(-3, 0, -10), arm) * pose_matrix(moved(4, -2, 30, 1.5), hand);
    Pic ref;
    ref.transform["arm"] = matrix(rig.matrix(rig.part("arm")));
    ref.transform["hand"] = matrix(rig.matrix(rig.part("hand")));
    ref.wrap["sword"] = matrix(c);
    same_picture(rig, ref.text());
    // The part's own values stay its own.
    CHECK(p.parts.at("sword").x == 0);

    // Its pivot goes with the carry; the next pose without it puts the pivot back at rest.
    CHECK(near(pivot_now(rig, "sword"), c.apply(rig.pivot(rig.part("sword"))), 1e-4));
    REQUIRE(rig.pose({}, why));
    CHECK(near(pivot_now(rig, "sword"), rig.pivot(rig.part("sword"))));

    // Its own turn goes on under the carry: the carry round the part turned about its own pivot.
    const vector::Point sword = rig.pivot(rig.part("sword"));
    s = script_of(clip_of(R"(part("sword"):attach("hand")
all(part("hand"):to({ x = 4, y = -2, rot = 30 }, 1, "linear"), part("sword"):to({ rot = 45, sx = 1.4 }, 1, "linear")))"));
    rec = recorded(s, &rig);
    p = frame(s, rec, &rig, 1);
    REQUIRE(rig.pose(p, why));
    PartPose own = moved(0, 0, 45);
    own.sx = 1.4;
    Pic turned;
    turned.transform["hand"] = matrix(rig.matrix(rig.part("hand")));
    turned.transform["sword"] = matrix(pose_matrix(own, sword));
    turned.wrap["sword"] = matrix(pose_matrix(moved(4, -2, 30), hand));
    same_picture(rig, turned.text());

    // Let go: it stays where it was carried to, while the hand goes back.
    s = script_of(clip_of(R"(part("sword"):attach("hand")
part("hand"):to({ x = 4, rot = 30 }, 1, "linear")
part("sword"):detach()
part("hand"):to({ x = 0, rot = 0 }, 1, "linear"))"));
    rec = recorded(s, &rig);
    CHECK(block_words(rec, block(rec, "sword", Prop::Link, 1)) == "отпущена");
    p = frame(s, rec, &rig, 2);
    CHECK(p.parts.at("sword").carry_by.empty());
    REQUIRE(rig.pose(p, why));
    Pic let_go;
    let_go.wrap["sword"] = matrix(pose_matrix(moved(4, 0, 30), hand));
    same_picture(rig, let_go.text());

    // Attached where the hand already stood: the sword stays put at the call and goes with the hand from there on.
    s = script_of(clip_of(R"(part("hand"):to({ x = 4, rot = 30 }, 1, "linear")
part("sword"):attach("hand")
part("hand"):to({ x = 0, rot = 0 }, 1, "linear"))"));
    rec = recorded(s, &rig);
    REQUIRE(rig.pose(frame(s, rec, &rig, 1), why));
    CHECK(near(pivot_now(rig, "sword"), rig.pivot(rig.part("sword")), 1e-4));
    REQUIRE(rig.pose(frame(s, rec, &rig, 2), why));
    Pic since;
    since.wrap["sword"] = matrix(inverse(pose_matrix(moved(4, 0, 30), hand)));
    same_picture(rig, since.text());

    // A new link over the one let go: multiplied on the left from its own call.
    s = script_of(clip_of(R"(part("sword"):attach("hand")
part("hand").x(4, 1, "linear")
part("sword"):detach()
part("sword"):attach("torso")
all(part("body"):to({ y = 5, rot = 15 }, 1, "linear"), part("hand").x(0, 1, "linear")))"));
    rec = recorded(s, &rig);
    p = frame(s, rec, &rig, 2);
    CHECK(p.parts.at("sword").carry_by == "torso");
    REQUIRE(rig.pose(p, why));
    Pic over;
    over.transform["body"] = matrix(rig.matrix(rig.part("body")));
    over.wrap["sword"] = matrix(pose_matrix(moved(0, 5, 15), body) * pose_matrix(moved(4, 0), hand));
    same_picture(rig, over.text());

    // The target carried itself: the hand goes with the torso, the sword with the hand.
    s = script_of(clip_of(R"(all(part("hand"):attach("torso"), part("sword"):attach("hand"))
part("body"):to({ x = -5, rot = 20, sy = 1.2 }, 1, "linear"))"));
    rec = recorded(s, &rig);
    p = frame(s, rec, &rig, 1);
    REQUIRE(rig.pose(p, why));
    PartPose b = moved(-5, 0, 20);
    b.sy = 1.2;
    const Affine wb = pose_matrix(b, body);
    Pic chain;
    chain.transform["body"] = matrix(rig.matrix(rig.part("body")));
    chain.wrap["hand"] = matrix(inverse(Affine{1, 0, 0, 1, 2, 1}) * wb * Affine{1, 0, 0, 1, 2, 1}); // in the arm's coordinates
    chain.wrap["sword"] = matrix(wb);
    same_picture(rig, chain.text());
    // The frame's place of a bone agrees with the picture's.
    Affine w;
    REQUIRE(bone_world(rec, static_cast<u32>(bone_of(rec, "hand")), 1, nullptr, w, why));
    CHECK(near(w.apply(hand), pivot_now(rig, "hand"), 1e-4));

    // A target drawn after the part (the ball after the sword) is placed first all the same: the sword goes with where the
    // ball stands in this pose, not where it stood in the last.
    {
        Rig fresh = loaded(picture());
        const vector::Point ball = fresh.pivot(fresh.part("ball"));
        s = script_of(clip_of(R"(part("sword"):attach("ball")
part("ball"):to({ x = 10, y = -6, rot = 20 }, 1, "linear"))"));
        rec = recorded(s, &fresh);
        REQUIRE(fresh.pose(frame(s, rec, &fresh, 1), why));
        Pic later;
        later.transform["ball"] = matrix(fresh.matrix(fresh.part("ball")));
        later.wrap["sword"] = matrix(pose_matrix(moved(10, -6, 20), ball));
        same_picture(fresh, later.text());
    }

    // save, restore and rest do not touch the link.
    s = script_of(clip_of(R"(part("sword"):attach("hand")
part("sword"):save()
part("sword"):rest(0)
part("sword"):restore(0)
part("hand").x(4, 1, "linear"))"));
    rec = recorded(s, &rig);
    CHECK(frame(s, rec, &rig, 1).parts.at("sword").carry_by == "hand");
    for (const Block& bl : rec.blocks) CHECK_FALSE((bl.prop == Prop::Link && bl.to != Block::To::Value));
}

TEST_CASE("Motion links: errors, edits and the words of the timeline") {
    const Rig rig = loaded(picture());
    auto error = [&](const char* body) {
        PoseScript e = script_of(clip_of(body));
        return record_error(e, &rig);
    };
    CHECK(error(R"(part("sword"):attach("nothing"))") == "строка 2: в рисунке нет части «nothing»");
    CHECK(error(R"(part("sword"):attach("sword"))") == "строка 2: часть «sword» не крепится к самой себе");
    CHECK(error(R"(part("sword"):attach("blade"))") == "строка 2: часть «sword» не пойдёт за «blade»: «blade» внутри неё");
    CHECK(error(R"(part("blade"):attach("sword"))") == "строка 2: часть «blade» и так ходит с «sword»: она внутри неё");
    CHECK(error(R"(part("sword"):attach("hand")
part("hand"):attach("sword"))") == "строка 3: крепление по кругу: hand → sword → hand");
    // Round through groups too, whenever in the clip.
    CHECK(error(R"(part("arm"):attach("torso")
wait(1)
part("body"):attach("tip"))") == "строка 4: крепление по кругу: body → tip → arm → torso → body");
    CHECK(error(R"(part("sword"):detach("hand"))") == "строка 2: detach() — без аргументов: часть отпускает то, за что держится");
    CHECK(error(R"(part("sword"):attach("hand"):to({ x = 1 }, 1))") ==
          "строка 2: to — не после attach и detach: у крепления нет длины, следующий переход — отдельной строкой");
    CHECK(error(R"(part("ball"):along("nothing", 1))") == "строка 2: в рисунке нет части-пути «nothing»");
    CHECK(error(R"(part("ball"):along("lane", 1))") == "строка 2: путь «lane» — группа, а нужна фигура");
    CHECK(error(R"(part("ball"):along("two", 1))") == "строка 2: у пути «two» больше одного контура: нужна одна линия");
    CHECK(error(R"(part("ball"):along("ball", 1))") == "строка 2: часть «ball» не идёт по себе");
    CHECK(error(R"(part("ball"):along("route", 1, "linear", { spin = true }))") ==
          "строка 2: along: поля «spin» нет (есть turn, from, to, relative)");
    CHECK(error(R"(part("ball"):along("route", 1, "linear", { turn = 1 }))") == "строка 2: along: turn — true или false");
    CHECK(error(R"(part("ball"):along("route", 1, "linear", { from = 1.5 }))") ==
          "строка 2: along: from и to — доли длины пути от 0 до 1");
    CHECK(error(R"(part("ball"):along("route", 1, "linear", 5))") ==
          "строка 2: along: последним — таблица { turn = true, from = 0, to = 1, relative = true }");
    CHECK(error(R"(part("ball"):along(5))") == "строка 2: along(\"route\", с, сглаживание, { turn = true }): первым — имя фигуры-пути");
    // Without a picture: no lines, pivots or groups to read.
    PoseScript bare = script_of(clip_of(R"(part("ball"):along("route", 1))"));
    CHECK(record_error(bare, nullptr) == "строка 2: along — только с рисунком: он читает линии, опорные точки и группы частей");
    bare = script_of(clip_of(R"(part("sword"):attach("hand"))"));
    CHECK(record_error(bare, nullptr) == "строка 2: attach — только с рисунком: он читает линии, опорные точки и группы частей");

    // Edits: a path's and a place's value are the code's, their length and timing are fields; a link has none.
    const std::string code = clip_of(R"(part("ball"):along("route", 1, "linear", { turn = true })
part("sword"):snap_to("tip", 0.5)
part("sword"):attach("hand")
part("sword"):detach())");
    PoseScript s = script_of(code);
    const Recording rec = recorded(s, &rig);
    std::string why, out;
    CHECK_FALSE(can_edit(rec, block(rec, "ball", Prop::X), BlockEdit::Value, s.code(), why));
    CHECK(why == "путь правится в коде: along(\"route\", …)");
    CHECK_FALSE(can_edit(rec, block(rec, "ball", Prop::Rot), BlockEdit::Value, s.code(), why));
    CHECK(why == "путь правится в коде: along(\"route\", …)");
    CHECK_FALSE(can_edit(rec, block(rec, "sword", Prop::Y), BlockEdit::Value, s.code(), why));
    CHECK(why == "место считается от «tip»: правится в коде");
    CHECK(can_edit(rec, block(rec, "ball", Prop::X), BlockEdit::Length, s.code(), why));
    CHECK(can_edit(rec, block(rec, "ball", Prop::X), BlockEdit::Timing, s.code(), why));
    REQUIRE(edit_length(rec, block(rec, "ball", Prop::Rot), 0.4, s.code(), code, out, why));
    CHECK(out.find(R"(part("ball"):along("route", 0.4, "linear", { turn = true }))") != std::string::npos);
    REQUIRE(edit_timing(rec, block(rec, "sword", Prop::X), vector::Timing::OutQuad, s.code(), code, out, why));
    CHECK(out.find(R"(part("sword"):snap_to("tip", 0.5, "out_quad"))") != std::string::npos);
    for (BlockEdit e : {BlockEdit::Value, BlockEdit::Length, BlockEdit::Timing, BlockEdit::Start}) {
        CHECK_FALSE(can_edit(rec, block(rec, "sword", Prop::Link), e, s.code(), why));
        CHECK(why == "крепление правится в коде: attach(\"hand\"), detach()");
    }
    // A drag of the part while its path or its place goes: refused for the same reason.
    CHECK_FALSE(edit_part_move(rec, "ball", 0.5, 0.1, 2, 0, s.code(), code, out, why));
    CHECK(why == "путь правится в коде: along(\"route\", …)");
    CHECK_FALSE(edit_part_move(rec, "sword", 1.25, 0.1, 0, 2, s.code(), code, out, why));
    CHECK(why == "место считается от «tip»: правится в коде");
    // The words of the values: the link's track.
    PropValue v;
    v.v = 1, v.target = "hand";
    CHECK(value_words(Prop::Link, v) == "с hand");
    CHECK(value_words(Prop::Link, rest_value(Prop::Link)) == "отпущена");
    CHECK(std::string(prop_word(Prop::Link)) == "крепление");
    CHECK(prop_is_call(Prop::Link));
    CHECK(static_cast<usize>(Prop::Mask) + 1 == static_cast<usize>(Prop::Link));
    CHECK(static_cast<usize>(Prop::Link) + 1 == static_cast<usize>(Prop::Order));
    CHECK(mix(Prop::Link, rest_value(Prop::Link), v, 0.5) == rest_value(Prop::Link));

    // A pose given to the rig straight checks its links itself: a link round is dropped (that of the first part found in
    // it), one to no part too, and the rest of the pose is drawn.
    Rig straight = loaded(picture());
    auto drawn = [&](const Pose& pose, bool ok, const char* said) {
        std::string told;
        CHECK(straight.pose(pose, told) == ok);
        CHECK(told == said);
        return straight.draw(128, 128);
    };
    Pose round;
    round.parts["sword"] = moved(6, 0, 20);
    round.parts["sword"].carry_by = "ball";
    round.parts["ball"].carry_by = "sword";
    round.parts["hand"].carry_by = "sword";
    Pose dropped = round;
    dropped.parts["sword"].carry_by.clear();
    Pose alone;
    alone.parts["sword"] = moved(6, 0, 20);
    const assets::CookedTexture with_round = drawn(round, false, "крепление по кругу: «sword» и «ball» несут друг друга");
    CHECK(with_round.rgba8 == drawn(dropped, true, "").rgba8);
    CHECK(with_round.rgba8 != drawn(alone, true, "").rgba8); // the hand and the ball go with the sword
    Pose lost = dropped, lost_dropped = dropped;
    lost.parts["hand"].carry_by = "nothing";
    lost_dropped.parts["hand"].carry_by.clear();
    CHECK(drawn(lost, false, "крепление части «hand»: в рисунке нет части «nothing»").rgba8 == drawn(lost_dropped, true, "").rgba8);
}

TEST_CASE("Motion links: hidden shapes stand where their transform puts them") {
    // A mask hidden with a transform of its own, against the same shape shown and hidden by the clip; beside it a second
    // hidden shape with another transform of its own.
    const char* svg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" width="64" height="64">
  <rect id="face" x="10" y="10" width="40" height="40" fill="#e0b080"/>
  <rect id="lid" x="0" y="0" width="20" height="10" transform="translate(20 15) rotate(25)" %s/>
  <rect id="flap" x="0" y="0" width="6" height="4" transform="translate(44 6) scale(1.5)" %s/>
</svg>)svg";
    char hidden[768], shown[768];
    std::snprintf(hidden, sizeof hidden, svg, "display=\"none\"", "display=\"none\"");
    std::snprintf(shown, sizeof shown, svg, "fill=\"#000000\"", "display=\"none\"");
    Rig a = loaded(hidden), b = loaded(shown);
    // The pivot: the centre of the same box, each by its own transform.
    CHECK(near(a.pivot(a.part("lid")), b.pivot(b.part("lid")), 1e-6));
    CHECK_FALSE(near(a.pivot(a.part("lid")), {10, 5}, 1)); // not where it would be without its transform
    CHECK(near(a.pivot(a.part("flap")), {48.5, 9}, 1e-6));
    PoseScript s = script_of(clip_of(R"(part("face"):mask("lid"))"));
    const Recording rec = recorded(s, &a);
    std::string why;
    REQUIRE(a.pose(frame(s, rec, &a, 0), why));
    PoseScript t = script_of(clip_of(R"(part("lid").visible(false)
part("face"):mask("lid"))"));
    const Recording rec2 = recorded(t, &b);
    REQUIRE(b.pose(frame(t, rec2, &b, 0), why));
    const assets::CookedTexture ma = a.draw(128, 128), mb = b.draw(128, 128);
    INFO("bytes off: ", bytes_off(ma, mb));
    CHECK(ma.rgba8 == mb.rgba8);
}

TEST_CASE("Motion links: baked, a carried part goes with its target pixel for pixel") {
    // The hand moves by a share of a pixel: baked, it goes by whole pixels, and the sword with it.
    Rig rig = loaded(picture());
    std::string why;
    REQUIRE(rig.pose({}, why));
    const assets::CookedTexture rest = rig.bake(64, 64, {});
    for (f64 x : {1.3, 2.6, 3.5, 4.4}) {
        INFO("x ", x);
        PoseScript s = script_of(clip_of("part(\"sword\"):attach(\"hand\")\npart(\"hand\").x(" + std::to_string(x) + ", 1)"));
        const Recording rec = recorded(s, &rig);
        REQUIRE(rig.pose(frame(s, rec, &rig, 1), why, 1));
        const assets::CookedTexture baked = rig.bake(64, 64, {});
        const auto dx = static_cast<i32>(std::round(x));
        // The sword's pixels (x 8 … 17, y 2 … 19 at rest) are the rest's moved by the hand's whole pixels.
        usize off = 0, seen = 0;
        for (i32 y = 0; y < 22; ++y)
            for (i32 px = 6; px < 20; ++px) {
                const usize at = (static_cast<usize>(y) * 64 + static_cast<usize>(px)) * 4;
                const usize to = (static_cast<usize>(y) * 64 + static_cast<usize>(px + dx)) * 4;
                for (usize k = 0; k < 4; ++k) off += rest.rgba8[at + k] != baked.rgba8[to + k];
                seen += rest.rgba8[at + 3] != 0;
            }
        CHECK(seen > 30);
        CHECK(off == 0);
    }
}
