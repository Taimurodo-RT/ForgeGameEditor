// Part 5б of docs/models/движение-модель.md («Начало с позы»): start_from("idle") as the first line of a clip starts it
// where clip idle stands at its end. Frame 0 of such a clip is idle's last moment drawn, byte for byte; its transitions
// go from there, back returns there, rest to the picture's rest. The drawing, the mask, the link and the order carry
// over, the copies do not. Errors name their line; a round names the lines of both clips.

#include "forge/pixel/code_sites.h"
#include "forge/pixel/motion.h"
#include "forge/pixel/motion_edit.h"
#include "forge/pixel/rig.h"
#include "forge/pixel/script.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <ostream> // doctest's INFO prints a string_view through it; MSVC needs the whole class
#include <string>
#include <vector>

using namespace forge;
using namespace forge::pixel;

namespace {

// A body with its torso, an arm with a hand, a ball, a mouth with a hidden open one to morph into and a cover to mask
// by.
const char* kPicture = R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" width="64" height="64">
  <g id="body"><rect id="torso" x="24" y="24" width="12" height="16" fill="#3060c0"/></g>
  <g id="arm" transform="translate(2 1)"><rect id="hand" x="40" y="30" width="6" height="6" fill="#c03030"/></g>
  <circle id="ball" cx="8" cy="50" r="3" fill="#30a060"/>
  <path id="mouth" d="M 26 34 Q 32 38 38 34" fill="none" stroke="#402010" stroke-width="1.5"/>
  <path id="mouth_o" d="M 27 33 Q 32 40 37 33 Z" fill="#802020" display="none"/>
  <rect id="cover" x="20" y="20" width="10" height="10" fill="#e0e000"/>
</svg>)svg";

Rig loaded() {
    Rig rig;
    std::string why;
    const bool ok = rig.load(kPicture, why);
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

Recording recorded(PoseScript& s, const Rig* rig, std::string_view clip) {
    Recording rec;
    const bool ok = s.record(rig, clip, {}, 1, {}, rec);
    INFO(s.error());
    REQUIRE(ok);
    return rec;
}

std::string record_error(PoseScript& s, const Rig* rig, std::string_view clip) {
    Recording rec;
    CHECK_FALSE(s.record(rig, clip, {}, 1, {}, rec));
    return s.error();
}

Pose frame(PoseScript& s, const Recording& rec, const Rig* rig, f64 t) {
    Pose pose;
    const bool ok = s.frame(rec, rig, t, {}, 1, pose);
    INFO(s.error());
    REQUIRE(ok);
    return pose;
}

assets::CookedTexture shot(Rig& rig, const Pose& pose) {
    std::string why;
    const bool ok = rig.pose(pose, why);
    INFO(why);
    REQUIRE(ok);
    return rig.draw(64, 64);
}

vector::Point pivot_now(const Rig& rig, const char* id) {
    const i32 p = rig.part(id);
    return rig.carried(p).apply(rig.pivot(p));
}

bool near(vector::Point a, vector::Point b, f64 eps = 1e-9) { return std::abs(a.x - b.x) <= eps && std::abs(a.y - b.y) <= eps; }

// Clips by name, each a function of its own lines: "return { clips = {" is line 1, each clip's header a line, its
// body lines after it, "end," a line.
std::string clips_of(const std::vector<std::pair<std::string, std::string>>& clips, std::string_view more = "") {
    std::string s = "return { clips = {\n";
    for (const auto& [name, body] : clips) s += name + " = function(p, rng)\n" + body + "\nend,\n";
    return s + "}" + std::string(more) + " }";
}

const char* kIdle = R"(  local body = part("body")
  all(body.x(5, 0.3), body.rot(20, 0.3), part("arm"):to({ opacity = 0.5, fill = "#ff0000" }, 0.2))
  part("mouth"):morph("mouth_o", 0.2)
  part("ball").visible(false)
  part("ball"):to_top()
  part("torso"):mask("cover")
  part("mouth").stroke_width(3, 0.1))";

} // namespace

TEST_CASE("5б: frame 0 of a clip that starts from idle is idle's end, byte for byte; it goes on from there") {
    Rig rig = loaded();
    PoseScript s = script_of(clips_of({{"idle", kIdle},
                                       {"go", "  start_from(\"idle\")\n  part(\"body\").x(10, 0.2):back(0.2)\n  part(\"body\"):rest(0.1)"}}));
    const Recording idle = recorded(s, &rig, "idle");
    const Recording go = recorded(s, &rig, "go");
    CHECK(idle.start.clip.empty());
    CHECK(go.start.clip == "idle");
    REQUIRE(go.start.site.site >= 0);
    CHECK(s.code().sites()[static_cast<usize>(go.start.site.site)].line == 12);
    CHECK(go.length == doctest::Approx(0.5));

    // Frame 0 is idle at its end: the same picture, the same numbers.
    const Pose end = frame(s, idle, &rig, idle.length), first = frame(s, go, &rig, 0);
    const assets::CookedTexture a = shot(rig, end), b = shot(rig, first);
    CHECK(a.rgba8 != loaded().draw(64, 64).rgba8); // idle's end is not the picture at rest
    CHECK(a.rgba8 == b.rgba8);
    CHECK(first.parts.at("body").x == 5);
    CHECK(first.parts.at("body").rot == 20);
    CHECK(first.parts.at("arm").opacity == 0.5);
    CHECK((first.parts.at("arm").has_fill && first.parts.at("arm").fill == Rgba8{255, 0, 0, 255} && first.parts.at("arm").fill_k == 1));
    CHECK((first.morphs.at("mouth").target == "mouth_o" && first.morphs.at("mouth").k == 1));
    CHECK_FALSE(first.parts.at("ball").visible);
    CHECK((first.parts.at("mouth").has_stroke_width && first.parts.at("mouth").stroke_width == 3));
    CHECK(first.parts.at("torso").mask == "cover");
    CHECK(first.order == std::vector<OrderStep>{{"ball", OrderStep::Kind::Top, ""}});
    CHECK(first.order == end.order);
    // The pose is what differs from the rest, nothing more.
    std::vector<std::pair<std::string, Prop>> kept;
    for (const StartValue& v : go.start.values) kept.emplace_back(v.part, v.prop);
    std::sort(kept.begin(), kept.end());
    CHECK(kept == std::vector<std::pair<std::string, Prop>>{{"arm", Prop::Opacity}, {"arm", Prop::Fill}, {"ball", Prop::Visible}, {"body", Prop::X},
                                                           {"body", Prop::Rot}, {"mouth", Prop::StrokeWidth}, {"mouth", Prop::Morph}, {"torso", Prop::Mask}});

    // No blocks of the pose: the tracks are what the clip moves itself.
    for (const Recording::Track& t : go.tracks) CHECK(go.parts[t.part] == "body");
    const auto arm = static_cast<u32>(std::find(go.parts.begin(), go.parts.end(), "arm") - go.parts.begin());
    REQUIRE(arm < go.parts.size());
    CHECK(go.track(arm, Prop::Opacity) < 0);
    CHECK(first_value(go, arm, Prop::Opacity).v == 0.5);
    CHECK(first_value(go, arm, Prop::X) == rest_value(Prop::X));

    // From the pose: x goes 5 → 10 and back to 5; rest goes to the picture's rest, not to the pose.
    CHECK(frame(s, go, &rig, 0.1).parts.at("body").x == doctest::Approx(7.5));
    CHECK(frame(s, go, &rig, 0.2).parts.at("body").x == 10);
    CHECK(frame(s, go, &rig, 0.4).parts.at("body").x == 5);
    const Pose last = frame(s, go, &rig, 0.5);
    CHECK(last.parts.at("body").x == 0);
    CHECK(last.parts.at("body").rot == 0);
    CHECK(last.parts.at("arm").opacity == 0.5); // nothing of the clip moves the arm
    CHECK(frame(s, go, &rig, 0.45).parts.at("body").rot == doctest::Approx(10));
}

TEST_CASE("5б: save before a block keeps the pose, a value read reads it, a binding at idle's end is its number") {
    Rig rig = loaded();
    PoseScript s = script_of(clips_of({{"idle", "  part(\"body\").x(function() return 7 + now() end)\n  wait(0.5)"},
                                       {"go", "  start_from(\"idle\")\n  log(part(\"body\").x())\n  part(\"body\"):save()\n"
                                              "  part(\"body\").x(0, 0.2)\n  part(\"body\"):restore(0.2)"}}));
    const Recording go = recorded(s, &rig, "go");
    REQUIRE(go.log.size() == 1);
    CHECK(go.log[0].text == "7.5");
    CHECK(frame(s, go, &rig, 0).parts.at("body").x == 7.5);
    CHECK(frame(s, go, &rig, 0.2).parts.at("body").x == 0);
    CHECK(frame(s, go, &rig, 0.4).parts.at("body").x == 7.5);
    // The binding is idle's: go has none of its own, its value is the number.
    for (const Block& b : go.blocks) CHECK(b.to != Block::To::Binding);
    const auto body = static_cast<u32>(std::find(go.parts.begin(), go.parts.end(), "body") - go.parts.begin());
    REQUIRE(start_value(go, body, Prop::X));
    CHECK(start_value(go, body, Prop::X)->value.v == 7.5);
    // A read in a frame, of a part the clip does not move: from the pose too.
    PoseScript f = script_of(clips_of({{"idle", "  part(\"body\").x(5, 0.2)"}, {"go", "  start_from(\"idle\")\n  wait(0.2)"}},
                                      ",\n  pose = function(t, p, rng, clip) if clip == \"go\" then part(\"arm\"):set{ y = part(\"body\").x() } end end"));
    const Recording fg = recorded(f, &rig, "go");
    CHECK(frame(f, fg, &rig, 0.1).parts.at("arm").y == 5);
}

TEST_CASE("5б: a chain of clips, each from the one before, from the first on") {
    Rig rig = loaded();
    PoseScript s = script_of(clips_of({{"a", "  part(\"body\").x(3, 0.1)\n  part(\"ball\"):to_top()"},
                                       {"b", "  start_from(\"a\")\n  part(\"body\").y(4, 0.1)"},
                                       {"c", "  start_from(\"b\")\n  part(\"body\").rot(9, 0.1)"}}));
    const Recording c = recorded(s, &rig, "c");
    const Pose first = frame(s, c, &rig, 0), last = frame(s, c, &rig, 0.1);
    CHECK((first.parts.at("body").x == 3 && first.parts.at("body").y == 4 && first.parts.at("body").rot == 0));
    CHECK((last.parts.at("body").x == 3 && last.parts.at("body").y == 4 && last.parts.at("body").rot == 9));
    CHECK(c.start.clip == "b");
    // The order of a goes on through b, which does not touch it.
    CHECK(first.order == std::vector<OrderStep>{{"ball", OrderStep::Kind::Top, ""}});
}

TEST_CASE("5б: a link holds on, one let go stays where it was; the copies do not carry over") {
    Rig rig = loaded();
    const vector::Point ball0 = pivot_now(rig, "ball");
    PoseScript s = script_of(clips_of({{"idle", "  part(\"ball\"):attach(\"hand\")\n  part(\"arm\").x(6, 0.2)"},
                                       {"back", "  start_from(\"idle\")\n  part(\"arm\").x(0, 0.2)"},
                                       {"drop", "  start_from(\"idle\")\n  part(\"ball\"):detach()\n  part(\"arm\").x(0, 0.2)"}}));
    const Recording idle = recorded(s, &rig, "idle");
    shot(rig, frame(s, idle, &rig, idle.length));
    CHECK(near(pivot_now(rig, "ball"), {ball0.x + 6, ball0.y}));

    // Held by the hand still: the arm goes back, the ball with it.
    const Recording back = recorded(s, &rig, "back");
    const Pose b0 = frame(s, back, &rig, 0);
    CHECK(b0.parts.at("ball").carry_by == "hand");
    shot(rig, b0);
    CHECK(near(pivot_now(rig, "ball"), {ball0.x + 6, ball0.y}));
    shot(rig, frame(s, back, &rig, 0.2));
    CHECK(near(pivot_now(rig, "ball"), ball0));

    // Let go at once: it stays where the hand had it.
    const Recording drop = recorded(s, &rig, "drop");
    const Pose d1 = frame(s, drop, &rig, 0.2);
    CHECK(d1.parts.at("ball").carry_by.empty());
    shot(rig, d1);
    CHECK(near(pivot_now(rig, "ball"), {ball0.x + 6, ball0.y}));

    // Copies: none in the new clip, nor their links, masks and order (attach, mask and order name parts of the picture
    // only, so nothing of the picture hangs on a copy).
    Rig rig2 = loaded();
    PoseScript c = script_of(clips_of({{"idle", "  copies(\"ball\", 2, function(i, c) all(c.x(i * 5, 0.1), c:attach(\"hand\"), c:mask(\"cover\"), c:to_top()) end)\n"
                                                "  part(\"arm\").x(3, 0.2)"},
                                       {"go", "  start_from(\"idle\")\n  wait(0.1)"}}));
    const Recording ci = recorded(c, &rig2, "idle");
    const Pose ce = frame(c, ci, &rig2, ci.length);
    CHECK(ce.copies.size() == 2);
    CHECK(ce.order.size() == 2);
    const Recording cg = recorded(c, &rig2, "go");
    CHECK(cg.copies.empty());
    for (const StartValue& v : cg.start.values) CHECK(v.part.find('[') == std::string::npos);
    CHECK(cg.start.order.empty());
    const Pose g0 = frame(c, cg, &rig2, 0);
    CHECK(g0.copies.empty());
    CHECK(g0.order.empty());
    CHECK(g0.parts.at("arm").x == 3);
    for (const auto& [id, pp] : g0.parts) CHECK(id.find('[') == std::string::npos);
}

TEST_CASE("5б: where parts stand by idle's link: a snap to the held ball, a detach later, an attach, the carry at idle's end") {
    Rig rig = loaded();
    const vector::Point ball0 = pivot_now(rig, "ball");
    PoseScript s = script_of(clips_of({{"idle", "  part(\"ball\"):attach(\"hand\")\n  part(\"arm\").x(6, 0.2)"},
                                       {"reach", "  start_from(\"idle\")\n  part(\"mouth\"):snap_to(\"ball\", 0.1)"},
                                       {"late", "  start_from(\"idle\")\n  wait(0.2)\n  part(\"ball\"):detach()"},
                                       {"round", "  start_from(\"idle\")\n  part(\"arm\"):attach(\"ball\")"}}));
    // The snap reads where the ball stands now: held by the hand of the arm 6 to the right, as idle left them.
    const Recording reach = recorded(s, &rig, "reach");
    shot(rig, frame(s, reach, &rig, 0.1));
    CHECK(near(pivot_now(rig, "mouth"), {ball0.x + 6, ball0.y}, 1e-6));
    // A detach further on: till then the hand holds it.
    const Recording late = recorded(s, &rig, "late");
    const Pose l1 = frame(s, late, &rig, 0.1);
    CHECK(l1.parts.at("ball").carry_by == "hand");
    shot(rig, l1);
    CHECK(near(pivot_now(rig, "ball"), {ball0.x + 6, ball0.y}));
    CHECK(frame(s, late, &rig, 0.2).parts.at("ball").carry_by.empty());
    // The hand holds the ball: the arm on the ball is a round.
    CHECK(record_error(s, &rig, "round") == "строка 17: крепление по кругу: arm → ball → hand → arm");

    // An attach in the clip: the ball stays where it is, the hand where idle left it; it goes with the arm from there.
    PoseScript g = script_of(clips_of({{"idle", "  part(\"arm\").x(6, 0.2)"},
                                       {"grab", "  start_from(\"idle\")\n  part(\"ball\"):attach(\"hand\")\n  part(\"arm\").x(0, 0.2)"}}));
    const Recording grab = recorded(g, &rig, "grab");
    shot(rig, frame(g, grab, &rig, 0));
    CHECK(near(pivot_now(rig, "ball"), ball0));
    shot(rig, frame(g, grab, &rig, 0.2));
    CHECK(near(pivot_now(rig, "ball"), {ball0.x - 6, ball0.y}));

    // The carry idle ends with: attached with the arm out, the arm back, the ball 6 to the left; the clip starts so.
    PoseScript e = script_of(clips_of({{"idle", "  part(\"arm\").x(6, 0.2)\n  part(\"ball\"):attach(\"hand\")\n  part(\"arm\").x(0, 0.2)"},
                                       {"go", "  start_from(\"idle\")\n  wait(0.1)"}}));
    const Recording ei = recorded(e, &rig, "idle"), eg = recorded(e, &rig, "go");
    const assets::CookedTexture end = shot(rig, frame(e, ei, &rig, ei.length));
    CHECK(near(pivot_now(rig, "ball"), {ball0.x - 6, ball0.y}));
    const assets::CookedTexture first = shot(rig, frame(e, eg, &rig, 0));
    CHECK(near(pivot_now(rig, "ball"), {ball0.x - 6, ball0.y}));
    CHECK(end.rgba8 == first.rgba8);

    // A detach in the background after the end of b: at b's end the hand holds the ball still, and c starts so.
    PoseScript c = script_of(clips_of({{"a", "  part(\"ball\"):attach(\"hand\")\n  part(\"arm\").x(6, 0.2)"},
                                       {"b", "  start_from(\"a\")\n  spawn(function() wait(1) part(\"ball\"):detach() end)\n  wait(0.1)"},
                                       {"c", "  start_from(\"b\")\n  wait(0.1)"}}));
    const Recording cb = recorded(c, &rig, "b"), cc = recorded(c, &rig, "c");
    CHECK(cb.length == doctest::Approx(0.1));
    const Pose c0 = frame(c, cc, &rig, 0);
    CHECK(c0.parts.at("ball").carry_by == "hand");
    shot(rig, c0);
    CHECK(near(pivot_now(rig, "ball"), {ball0.x + 6, ball0.y}));
}

TEST_CASE("5б: the drawing shown at idle's end, and a swap from it") {
    const std::string draw = "  draw = function(p, rng)\n    canvas(64, 64)\n    rect(\"face\", { x = 10, y = 10, w = 20, h = 20, fill = \"#c09060\" })\n  end,\n"
                             "  drawings = {\n    angry = function(p, rng)\n      canvas(64, 64)\n"
                             "      rect(\"face\", { x = 12, y = 12, w = 24, h = 24, fill = \"#e06040\" })\n    end,\n  },\n";
    PoseScript s = script_of(clips_of({{"idle", "  swap(\"angry\", 0.2)"}, {"calm", "  start_from(\"idle\")\n  swap(\"draw\", 0.2)"},
                                       {"still", "  start_from(\"idle\")\n  wait(0.2)"}, {"late", "  start_from(\"idle\")\n  wait(0.2)\n  swap(\"draw\", 0.2)"}},
                                      ",\n" + draw));
    const Recording calm = recorded(s, nullptr, "calm"), still = recorded(s, nullptr, "still"), late = recorded(s, nullptr, "late");
    CHECK(frame(s, still, nullptr, 0).drawing == "angry");
    CHECK(frame(s, still, nullptr, 0.2).drawing == "angry");
    CHECK(frame(s, calm, nullptr, 0).drawing == "angry");
    const Pose mid = frame(s, calm, nullptr, 0.1);
    CHECK((mid.swap && mid.swap_from == "angry" && mid.drawing.empty()));
    CHECK(frame(s, calm, nullptr, 0.2).drawing.empty());
    // Before a swap further on: the drawing idle left, not the main one.
    CHECK(frame(s, late, nullptr, 0.1).drawing == "angry");
    CHECK(frame(s, late, nullptr, 0.4).drawing.empty());
}

TEST_CASE("5б: start_from errors name their line; a round names the lines of both clips") {
    Rig rig = loaded();
    auto error = [&](const std::vector<std::pair<std::string, std::string>>& clips, std::string_view clip) {
        PoseScript s = script_of(clips_of(clips));
        return record_error(s, &rig, clip);
    };
    const std::pair<std::string, std::string> idle{"idle", "  part(\"body\").x(1, 0.1)"};
    const std::string first = "start_from(\"idle\") — первая строка клипа: до неё уже переход, ожидание, метка или чтение значения";
    // Lines: idle is 2–4, go's header 5, its body from 6 on; a clip of one line of body takes three.
    CHECK(error({idle, {"go", "  part(\"body\").y(1, 0.1)\n  start_from(\"idle\")"}}, "go") == "строка 7: " + first);
    CHECK(error({idle, {"go", "  wait(0.1)\n  start_from(\"idle\")"}}, "go") == "строка 7: " + first);
    CHECK(error({idle, {"go", "  emit(\"шаг\")\n  start_from(\"idle\")"}}, "go") == "строка 7: " + first);
    CHECK(error({idle, {"go", "  local v = part(\"body\").x()\n  start_from(\"idle\")"}}, "go") == "строка 7: " + first);
    CHECK(error({idle, {"go", "  local t = now()\n  start_from(\"idle\")"}}, "go") == "строка 7: " + first);
    CHECK(error({idle, {"go", "  duration(\"шаг\", 0.1)\n  start_from(\"idle\")"}}, "go") == "строка 7: " + first);
    CHECK(error({idle, {"go", "  part(\"body\"):save()\n  start_from(\"idle\")"}}, "go") == "строка 7: " + first);
    CHECK(error({idle, {"go", "  start_from(\"idle\")\n  start_from(\"idle\")"}}, "go") == "строка 7: " + first);
    CHECK(error({idle, {"go", "  run(function() start_from(\"idle\") end)"}}, "go") ==
          "строка 6: start_from(\"idle\") — в самой функции клипа, не внутри другой функции");
    CHECK(error({idle, {"go", "  start_from(\"nope\")"}}, "go") == "строка 6: в коде нет клипа «nope»: start_from начинает с позы конца другого клипа");
    CHECK(error({idle, {"go", "  start_from(5)"}}, "go") == "строка 6: start_from(\"idle\"): одно имя клипа, с позы конца которого начать");
    CHECK(error({idle, {"go", "  start_from(\"idle\", \"go\")"}}, "go") == "строка 6: start_from(\"idle\"): одно имя клипа, с позы конца которого начать");
    // A local and a log before it are no step of the clip.
    PoseScript ok = script_of(clips_of({idle, {"go", "  local body = part(\"body\")\n  log(\"из покоя\")\n  start_from(\"idle\")\n  body.y(2, 0.1)"}}));
    CHECK(recorded(ok, &rig, "go").start.clip == "idle");

    // Rounds: a from b from a; a from itself. The line is the start_from that closes the round.
    {
        PoseScript s = script_of(clips_of({{"a", "  start_from(\"b\")"}, {"b", "  start_from(\"a\")"}}));
        CHECK(record_error(s, &rig, "a") == "строка 6: клипы начинаются друг с друга по кругу: a, строка 3 → b, строка 6 → a");
        CHECK(s.error_line() == 6);
        CHECK(record_error(s, &rig, "b") == "строка 3: клипы начинаются друг с друга по кругу: b, строка 6 → a, строка 3 → b");
        CHECK(s.error_line() == 3);
    }
    {
        PoseScript s = script_of(clips_of({{"a", "  start_from(\"a\")"}}));
        CHECK(record_error(s, &rig, "a") == "строка 3: клипы начинаются друг с друга по кругу: a, строка 3 → a");
    }
    {
        // A round further on: c from a, a from b, b from a.
        PoseScript s = script_of(clips_of({{"a", "  start_from(\"b\")"}, {"b", "  start_from(\"a\")"}, {"c", "  start_from(\"a\")"}}));
        CHECK(record_error(s, &rig, "c") == "строка 6: клипы начинаются друг с друга по кругу: a, строка 3 → b, строка 6 → a");
    }
    // An error of idle itself: its line and its words, as recording idle gives them.
    {
        PoseScript s = script_of(clips_of({{"idle", "  wait(0.1)\n  part(\"tail\").x(1, 0.1)"}, {"go", "  start_from(\"idle\")"}}));
        const std::string own = record_error(s, &rig, "idle");
        CHECK(own == "строка 4: в рисунке нет части «tail»");
        CHECK(record_error(s, &rig, "go") == own);
        CHECK(s.error_line() == 4);
    }
    // Outside a clip's function.
    {
        PoseScript s = script_of(clips_of({idle}, ", pose = function(t) start_from(\"idle\") end"));
        Pose pose;
        CHECK_FALSE(s.pose(rig, 0, "walk", {}, 1, pose));
        CHECK(s.error() == "строка 5: start_from — внутри функции клипа: clips = { walk = function(p) … end }");
    }
    // Under pcall it still starts from idle: the clip is recorded again with the pose.
    {
        PoseScript s = script_of(clips_of({{"idle", kIdle}, {"go", "  pcall(start_from, \"idle\")\n  part(\"body\").y(3, 0.1)"}}));
        const Recording go = recorded(s, &rig, "go");
        CHECK(go.start.clip == "idle");
        CHECK(frame(s, go, &rig, 0).parts.at("body").x == 5);
    }
    // Two of them under pcall in one try: the first is the one, the second fails as a line after it.
    {
        PoseScript s = script_of(clips_of({idle, {"other", "  part(\"body\").y(2, 0.1)"},
                                           {"go", "  pcall(start_from, \"idle\")\n  pcall(start_from, \"other\")\n  part(\"body\").rot(3, 0.1)"}}));
        const Recording go = recorded(s, &rig, "go");
        CHECK(go.start.clip == "idle");
        const Pose first = frame(s, go, &rig, 0);
        CHECK((first.parts.at("body").x == 1 && first.parts.at("body").y == 0));
    }
}

TEST_CASE("5б: idle's event lengths make its end, and a new length of them records the clip again") {
    Rig rig = loaded();
    PoseScript s = script_of(clips_of({{"idle", "  spawn(part(\"body\").x(10, 1, \"linear\"))\n  wait_until(\"stop\", 0.5)"},
                                       {"go", "  start_from(\"idle\")\n  wait(0.1)"}}));
    const Recording* go = s.recording(rig, "go", {}, 1);
    REQUIRE(go);
    CHECK(frame(s, *go, &rig, 0).parts.at("body").x == doctest::Approx(5));
    // The file's length of idle's event: only every clip's lengths given reach idle.
    MotionLengths l;
    l.waits["stop"] = 0.25;
    s.set_lengths("go", l);
    go = s.recording(rig, "go", {}, 1);
    REQUIRE(go);
    CHECK(frame(s, *go, &rig, 0).parts.at("body").x == doctest::Approx(5));
    s.set_lengths({{"idle", l}});
    go = s.recording(rig, "go", {}, 1);
    REQUIRE(go);
    CHECK(frame(s, *go, &rig, 0).parts.at("body").x == doctest::Approx(2.5));
    // And back.
    s.set_lengths(std::map<std::string, MotionLengths>{});
    go = s.recording(rig, "go", {}, 1);
    REQUIRE(go);
    CHECK(frame(s, *go, &rig, 0).parts.at("body").x == doctest::Approx(5));
    // The recording of idle the tab keeps still reads its bindings after another clip recorded it.
    PoseScript b = script_of(clips_of({{"idle", "  part(\"body\").x(function() return 2 * now() end)\n  wait(0.5)"},
                                       {"go", "  start_from(\"idle\")\n  wait(0.1)"}}));
    const Recording* idle = b.recording(rig, "idle", {}, 1);
    REQUIRE(idle);
    const Recording idle_kept = *idle;
    REQUIRE(b.recording(rig, "go", {}, 1));
    CHECK(frame(b, idle_kept, &rig, 0.25).parts.at("body").x == 0.5);
}

TEST_CASE("5б: a new at line goes right after start_from, which stays the first line") {
    auto inserted = [](std::string_view source) {
        CodeSites code;
        std::string why, out;
        u32 line = 0;
        REQUIRE(code.parse(source, why, line));
        CHECK(insert_clip_line(code, source, "go", "at(0, part(\"body\").x(2, 0))", out, why));
        INFO(why);
        return out;
    };
    const std::string head = "return { clips = {\nidle = function(p) wait(0.1) end,\n";
    CHECK(inserted(head + "go = function(p)\n  start_from(\"idle\")\n  part(\"body\").y(1, 0.1)\nend } }") ==
          head + "go = function(p)\n  start_from(\"idle\")\n  at(0, part(\"body\").x(2, 0))\n  part(\"body\").y(1, 0.1)\nend } }");
    CHECK(inserted(head + "go = function(p)\r\n  start_from(\"idle\")\r\n  part(\"body\").y(1, 0.1)\r\nend } }") ==
          head + "go = function(p)\r\n  start_from(\"idle\")\r\n  at(0, part(\"body\").x(2, 0))\r\n  part(\"body\").y(1, 0.1)\r\nend } }");
    CHECK(inserted(head + "go = function(p)\n  start_from(\"idle\"); -- из покоя\n  wait(0.1)\nend } }") ==
          head + "go = function(p)\n  start_from(\"idle\"); -- из покоя\n  at(0, part(\"body\").x(2, 0))\n  wait(0.1)\nend } }");
    CHECK(inserted(head + "go = function(p)\n  start_from(\"idle\") wait(0.1)\nend } }") ==
          head + "go = function(p)\n  start_from(\"idle\")\n  at(0, part(\"body\").x(2, 0)) wait(0.1)\nend } }");
    CHECK(inserted(head + "go = function(p) start_from(\"idle\") end } }") ==
          head + "go = function(p) start_from(\"idle\")\n  at(0, part(\"body\").x(2, 0)) end } }");
    // Without start_from first, the first line as before; a start_from further down is no first line.
    CHECK(inserted(head + "go = function(p)\n  wait(0.1)\n  start_from(\"idle\")\nend } }") ==
          head + "go = function(p)\n  at(0, part(\"body\").x(2, 0))\n  wait(0.1)\n  start_from(\"idle\")\nend } }");
}

TEST_CASE("5б: a part's field and a drag at frame 0 start from the pose, the at line after start_from") {
    Rig rig = loaded();
    const std::string source = clips_of({{"idle", kIdle}, {"go", "  start_from(\"idle\")\n  part(\"body\").y(4, 0.2)"}});
    PoseScript s = script_of(source);
    const Recording go = recorded(s, &rig, "go");
    PropValue v;
    bool going = true;
    CHECK(part_prop_value(go, "body", 0, 0.1, Prop::X, v, going));
    CHECK((v.v == 5 && !going));
    CHECK(part_prop_value(go, "arm", 0, 0.1, Prop::Opacity, v, going));
    CHECK(v.v == 0.5);
    std::string out, why;
    // A drag of the arm (no track of its own): from where the pose has it.
    CHECK(edit_part_move(go, "arm", 0, 0.1, 3, 0, s.code(), source, out, why));
    INFO(why);
    const usize at = source.find("  start_from(\"idle\")\n") + std::string("  start_from(\"idle\")\n").size();
    CHECK(out == source.substr(0, at) + "  at(0, part(\"arm\"):to({ x = 3 }, 0))\n" + source.substr(at));
    // The body's x has no track in go either: 5 from the pose, 7 after the move.
    CHECK(edit_part_move(go, "body", 0, 0.1, 2, 0, s.code(), source, out, why));
    CHECK(out == source.substr(0, at) + "  at(0, part(\"body\"):to({ x = 7 }, 0))\n" + source.substr(at));
    CHECK(edit_part_prop(go, "arm", 0, 0.1, Prop::Opacity, PropValue{0.25}, s.code(), source, out, why));
    CHECK(out == source.substr(0, at) + "  at(0, part(\"arm\").opacity(0.25, 0))\n" + source.substr(at));
}
