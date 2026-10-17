// Part 4г of docs/models/движение-модель.md («Копии»): copies("искра", n, function(i, c) … end) makes copies of a part
// of the picture, numbered on in the clip, born at the call, each with its own transitions. A copy is checked as its
// part written again in the SVG right above it (and above the copies before it) with an id of its own: posed alike, the
// two pictures are the same byte for byte, smooth and baked.

#include "forge/pixel/motion.h"
#include "forge/pixel/motion_edit.h"
#include "forge/pixel/rig.h"
#include "forge/pixel/script.h"
#include "forge/pixel/timeline.h"

#include <doctest/doctest.h>

#include <map>
#include <ostream> // doctest's INFO prints a string_view through it; MSVC needs the whole class
#include <string>
#include <vector>

using namespace forge;
using namespace forge::pixel;

namespace {

// A body; a moved group with a hidden spark (its dashed stroke and opacity too), a dot and a hidden square for the spark to morph into; a turned leaf of two
// shapes; a hidden group (ThorVG draws nothing of it) and a hidden line to go along. The copies written by hand go right
// after their part, in the order given.
struct Pic {
    std::vector<std::string> spark, dot, leaf; // ids of copies written after each
    std::string dot_id = "dot";

    std::string text() const {
        auto after = [](const std::vector<std::string>& ids, const std::string& element) {
            std::string out;
            for (const std::string& id : ids) out += "<" + element.substr(0, element.find(' ')) + " id=\"" + id + "\"" + element.substr(element.find(' '));
            return out;
        };
        const std::string spark_el =
            R"svg(circle cx="10" cy="10" r="3" fill="#ffa020" stroke="#a04000" stroke-width="1.2" stroke-dasharray="2 1" stroke-linecap="round" opacity="0.8"/>)svg";
        const std::string dot_el = R"svg(circle cx="20" cy="10" r="2" fill="#20a0ff" stroke="#103050" stroke-width="0.5"/>)svg";
        const std::string leaf_el = R"svg(g transform="rotate(10 40 40)"><path d="M 40 40 L 44 36" fill="none" stroke="#206020" stroke-width="1"/><ellipse cx="46" cy="34" rx="4" ry="2" fill="#40a040"/></g>)svg";
        std::string s = R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" width="64" height="64">)svg"
                        "\n";
        s += R"svg(  <g id="body"><rect id="torso" x="24" y="24" width="12" height="16" fill="#3060c0"/></g>)svg"
             "\n";
        s += R"svg(  <g id="fx" transform="translate(4 2)">)svg";
        s += R"svg(<circle id="spark" cx="10" cy="10" r="3" fill="#ffa020" stroke="#a04000" stroke-width="1.2" stroke-dasharray="2 1" stroke-linecap="round" opacity="0.8" display="none"/>)svg" +
             after(spark, spark_el);
        s += "<circle id=\"" + dot_id + R"svg(" cx="20" cy="10" r="2" fill="#20a0ff" stroke="#103050" stroke-width="0.5"/>)svg" + after(dot, dot_el);
        s += R"svg(<rect id="spark_sq" x="7" y="7" width="6" height="6" fill="#802000" display="none"/></g>)svg"
             "\n";
        s += R"svg(  <g id="leaf" transform="rotate(10 40 40)"><path id="stem" d="M 40 40 L 44 36" fill="none" stroke="#206020" stroke-width="1"/>)svg"
             R"svg(<ellipse id="blade" cx="46" cy="34" rx="4" ry="2" fill="#40a040"/></g>)svg" +
             after(leaf, leaf_el) + "\n";
        s += R"svg(  <g id="ghost" display="none"><rect x="0" y="0" width="4" height="4"/></g>)svg"
             "\n";
        s += R"svg(  <path id="route" d="M 10 50 L 50 50" display="none"/>)svg"
             "\n";
        return s + "</svg>";
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

usize bytes_off(const assets::CookedTexture& a, const assets::CookedTexture& b) {
    usize off = 0;
    for (usize i = 0; i < a.rgba8.size() && i < b.rgba8.size(); ++i) off += a.rgba8[i] != b.rgba8[i];
    return off;
}

// Byte for byte, counting the bytes off rather than printing the pictures.
bool same(const assets::CookedTexture& a, const assets::CookedTexture& b) {
    return a.width == b.width && a.height == b.height && a.rgba8.size() == b.rgba8.size() && bytes_off(a, b) == 0;
}

PartPose moved(f64 x, f64 y, f64 rot = 0, f64 scale = 1) {
    PartPose pp;
    pp.x = x, pp.y = y, pp.rot = rot, pp.scale = scale;
    return pp;
}

// The same pose with the copies under the ids they are written by: no copies left to make.
Pose renamed(const Pose& pose, const std::map<std::string, std::string>& ids) {
    auto id = [&](const std::string& s) {
        const auto it = ids.find(s);
        return it == ids.end() ? s : it->second;
    };
    Pose out;
    for (const auto& [k, pp] : pose.parts) out.parts[id(k)] = pp;
    for (const auto& [k, m] : pose.morphs) out.morphs[id(k)] = m;
    for (OrderStep step : pose.order) {
        step.part = id(step.part);
        step.other = id(step.other);
        out.order.push_back(step);
    }
    return out;
}

// The rig in the pose against the copies written by hand, posed alike under their written ids: smooth and baked, byte
// for byte. Shown: the copies change the picture at rest.
void same_as_written(Rig& rig, const Pose& pose, const Pic& written, const std::map<std::string, std::string>& ids, bool shown = true) {
    std::string why;
    REQUIRE(rig.pose(pose, why));
    Rig ref = loaded(written.text());
    REQUIRE(ref.pose(renamed(pose, ids), why));
    const assets::CookedTexture a = rig.draw(128, 128), b = ref.draw(128, 128);
    INFO("bytes off: ", bytes_off(a, b));
    CHECK(same(a, b));
    const assets::CookedTexture ba = rig.bake(64, 64, {}), bb = ref.bake(64, 64, {});
    INFO("baked bytes off: ", bytes_off(ba, bb));
    CHECK(same(ba, bb));
    if (shown) {
        Rig rest = loaded(picture());
        CHECK_FALSE(same(a, rest.draw(128, 128)));
    }
}

// One copy of `source` with `body` (it has i and c) in a clip, against the copy written as `written` and gone by as a
// part in the same body, at t: `before` and `after` are lines of both clips round it.
void same_clip(const char* source, const std::string& body, const Pic& pic, const std::string& written, f64 t,
               const std::string& before = "", const std::string& after = "") {
    Rig rig = loaded(picture());
    PoseScript a = script_of(clip_of(before + "copies(\"" + source + "\", 1, function(i, c)\n" + body + "\nend)\n" + after));
    const Recording ra = recorded(a, &rig);
    std::string why;
    REQUIRE(rig.pose(frame(a, ra, &rig, t), why));
    Rig ref = loaded(pic.text());
    PoseScript b = script_of(clip_of(before + "local i, c = 1, part(\"" + written + "\")\n" + body + "\n" + after));
    const Recording rb = recorded(b, &ref);
    REQUIRE(ref.pose(frame(b, rb, &ref, t), why));
    const assets::CookedTexture da = rig.draw(128, 128), db = ref.draw(128, 128);
    INFO("bytes off: ", bytes_off(da, db));
    CHECK(same(da, db));
    const assets::CookedTexture ba = rig.bake(64, 64, {}), bb = ref.bake(64, 64, {});
    INFO("baked bytes off: ", bytes_off(ba, bb));
    CHECK(same(ba, bb));
}

} // namespace

TEST_CASE("Motion copies: names, numbers and births in the flow") {
    const Rig rig = loaded(picture());
    PoseScript s = script_of(clip_of(R"(copies("spark", 2, function(i, c) c.x(i * 5, 0.1 * i) end)
wait(0.25)
local t = copies("spark", 3, function(i, c) c.y(i, 0.5) end)
at(0.1, copies("dot", 1, function(i, c) c.rot(10 * i, 0.2) end))
spawn(copies("dot", 2, function(i, c) c.x(1, 1) end))
chain(wait(0.1), copies("leaf", 1, function(i, c) c.y(2, 0.1) end))
delay(0.2, copies(part("leaf"), 1, function(i, c) end)))"));
    const Recording rec = recorded(s, &rig);
    struct Want {
        const char* id;
        const char* source;
        u32 number;
        f64 born;
    };
    // Numbered in the order of the calls, each part on from its own; born where the call is laid out.
    const Want want[] = {{"spark[1]", "spark", 1, 0},  {"spark[2]", "spark", 2, 0},  {"spark[3]", "spark", 3, 0.45},
                         {"spark[4]", "spark", 4, 0.45}, {"spark[5]", "spark", 5, 0.45}, {"dot[1]", "dot", 1, 0.1},
                         {"dot[2]", "dot", 2, 0.95},    {"dot[3]", "dot", 3, 0.95},    {"leaf[1]", "leaf", 1, 1.05},
                         {"leaf[2]", "leaf", 2, 1.35}};
    REQUIRE(rec.copies.size() == std::size(want));
    for (usize k = 0; k < rec.copies.size(); ++k) {
        INFO(want[k].id);
        CHECK(rec.copies[k].id == want[k].id);
        CHECK(rec.copies[k].source == want[k].source);
        CHECK(rec.copies[k].number == want[k].number);
        CHECK(rec.copies[k].born == doctest::Approx(want[k].born));
        CHECK(copy_of(rec, want[k].id) == static_cast<i32>(k));
    }
    CHECK(copy_of(rec, "spark") == -1);
    // The functions go together and the line waits for the longest (0.2); spawn's do not hold the line.
    CHECK(rec.length == doctest::Approx(1.35));
    // Each copy's own blocks: i is its number in the call.
    CHECK(block(rec, "spark[1]", Prop::X).value.v == 5);
    CHECK(block(rec, "spark[2]", Prop::X).value.v == 10);
    CHECK(block(rec, "spark[2]", Prop::X).length == doctest::Approx(0.2));
    CHECK(block(rec, "spark[4]", Prop::Y).value.v == 2);
    CHECK(block(rec, "spark[4]", Prop::Y).start == doctest::Approx(0.45));
    CHECK(block(rec, "dot[1]", Prop::Rot).start == doctest::Approx(0.1));
    CHECK(block(rec, "dot[3]", Prop::X).start == doctest::Approx(0.95));
    CHECK(block_words(rec, block(rec, "spark[2]", Prop::X)) == "x → 10");

    // A frame has the copies born by then, and nothing of those not born yet.
    const Pose p = frame(s, rec, &rig, 0.3);
    std::vector<std::string> ids;
    for (const PartCopy& c : p.copies) ids.push_back(c.id);
    CHECK(ids == std::vector<std::string>{"spark[1]", "spark[2]", "dot[1]"});
    CHECK(p.copies[2] == PartCopy{"dot[1]", "dot", 1});
    CHECK(p.parts.at("spark[2]").x == 10);
    CHECK(p.parts.count("spark[3]") == 0);
    CHECK(p.parts.count("leaf[1]") == 0);
    CHECK(frame(s, rec, &rig, 0).copies.size() == 2);
    CHECK(frame(s, rec, &rig, 1.35).copies.size() == 10);

    // Without a picture the copies are recorded all the same: nothing to check them against.
    PoseScript bare = script_of(clip_of(R"(copies("anything", 2, function(i, c) c.x(i, 1) end))"));
    const Recording br = recorded(bare, nullptr);
    REQUIRE(br.copies.size() == 2);
    CHECK(br.copies[1].id == "anything[2]");
    CHECK(br.length == 1);
}

TEST_CASE("Motion copies: errors with their line") {
    const Rig rig = loaded(picture());
    auto error = [&](const char* body) {
        PoseScript e = script_of(clip_of(body));
        return record_error(e, &rig);
    };
    CHECK(error(R"(copies("nope", 1, function() end))") == "строка 2: в рисунке нет части «nope»");
    CHECK(error(R"(copies("ghost", 1, function() end))") ==
          "строка 2: у части «ghost» нет фигур, копиям рисовать нечего: группу с display=\"none\" ThorVG не рисует — спрячьте её "
          "в клипе: ghost.visible(false)");
    for (const char* n : {"0", "65", "1.5", "-1"}) {
        INFO(n);
        CHECK(error((std::string(R"(copies("dot", )") + n + ", function() end)").c_str()) == "строка 2: copies: число копий — целое от 1 до 64");
    }
    CHECK(error(R"(copies("dot", "2", function() end))") == "строка 2: copies: вторым — число копий, целое от 1 до 64");
    CHECK(error(R"(copies("dot", 2, 5))") == "строка 2: copies: третьим — функция(i, c): её зовут для каждой копии");
    CHECK(error(R"(copies("dot", 2, function() end, 1))") == "строка 2: copies: часть, число и функция — не больше");
    CHECK(error(R"(copies(7, 1, function() end))") == "строка 2: copies(\"искра\", n, функция(i, c)): первым — имя части или часть");
    // A copy is not copied: by its handle or by its name.
    CHECK(error(R"(copies("dot", 1, function(i, c)
  copies(c, 1, function() end)
end))") == "строка 3: «dot[1]» — копия, копии делают с части рисунка: copies(\"dot\", …)");
    CHECK(error(R"(copies("dot", 1, function() end)
copies("dot[1]", 1, function() end))") == "строка 3: «dot[1]» — копия, копии делают с части рисунка: copies(\"dot\", …)");
    // 256 in a clip at most.
    CHECK(error(R"(for k = 1, 4 do copies("dot", 64, function() end) end
copies("spark", 1, function() end))") == "строка 3: в клипе больше 256 копий");
    // A copy is gone by only as c.
    CHECK(error(R"(copies("dot", 1, function() end)
part("dot[1]").x(1, 1))") == "строка 3: «dot[1]» — копия: её берут в copies(…, function(i, c) … end)");
    const char* const as_target[] = {
        R"(part("torso"):mask("dot[1]"))", R"(part("torso"):snap_to("dot[1]"))", R"(part("torso"):attach("dot[1]"))",
        R"(part("spark"):above("dot[1]"))"};
    for (const char* call : as_target) {
        INFO(call);
        CHECK(error((std::string("copies(\"dot\", 1, function() end)\n") + call).c_str()) ==
              "строка 3: в рисунке нет части «dot[1]»: копию зовут только через c в copies(…)");
    }
    CHECK(error(R"(copies("dot", 1, function() end)
part("spark"):along("dot[1]", 1))") == "строка 3: в рисунке нет части-пути «dot[1]»: копию зовут только через c в copies(…)");
    // A copy stands beside its part, in the same groups: it goes with them already, and with its part it may.
    CHECK(error(R"(copies("dot", 1, function(i, c) c:attach("fx") end))") == "строка 2: часть «dot[1]» и так ходит с «fx»: она внутри неё");
    CHECK(error(R"(copies("dot", 1, function(i, c) c:above("torso") end))") ==
          "строка 2: dot[1] и torso в разных группах: порядок меняется только среди соседей");
    CHECK(error(R"(copies("leaf", 1, function(i, c) c:morph("spark_sq", 1) end))") ==
          "строка 2: «leaf[1]» — группа, морфинг бывает только у фигуры");
    // A part of the picture with such a name is copied as any other; a copy's name taken by one is refused.
    {
        Pic bracket;
        bracket.dot_id = "dot[1]";
        const Rig other = loaded(bracket.text());
        PoseScript e = script_of(clip_of(R"(copies("dot[1]", 1, function() end))"));
        const Recording rec = recorded(e, &other);
        REQUIRE(rec.copies.size() == 1);
        CHECK(rec.copies[0].id == "dot[1][1]");
    }
    {
        Pic clash;
        clash.dot = {"spark[1]"};
        const Rig other = loaded(clash.text());
        PoseScript e = script_of(clip_of(R"(copies("spark", 1, function() end))"));
        CHECK(record_error(e, &other) == "строка 2: копии нужно имя «spark[1]», а так зовут часть рисунка");
    }
    // Only in a clip's function, and not in tween's.
    CHECK(error(R"(tween(1, function(k) copies("dot", 1, function() end) end))") ==
          "строка 2: copies — внутри tween(с, функция) только мгновенные значения: body.y(v)");
    PoseScript posed = script_of(R"(return { pose = function(t) copies("dot", 1, function() end) end })");
    Pose out;
    CHECK_FALSE(posed.pose(rig, 0, "", {}, 1, out));
    CHECK(posed.error().find("copies — внутри функции клипа") != std::string::npos);
}

TEST_CASE("Motion copies: a copy is its part written again above it") {
    // At rest: two copies of the dot over it, a copy of the hidden spark shown, a copy of the turned leaf.
    {
        Rig rig = loaded(picture());
        PoseScript s = script_of(clip_of(R"(copies("dot", 2, function() end)
copies("spark", 1, function() end)
copies("leaf", 1, function() end))"));
        const Recording rec = recorded(s, &rig);
        Pic w;
        w.dot = {"dot_c1", "dot_c2"};
        w.spark = {"spark_c1"};
        w.leaf = {"leaf_c1"};
        const std::map<std::string, std::string> ids = {{"dot[1]", "dot_c1"}, {"dot[2]", "dot_c2"}, {"spark[1]", "spark_c1"}, {"leaf[1]", "leaf_c1"}};
        same_as_written(rig, frame(s, rec, &rig, 0), w, ids);
        // Posed again, the copies made before go back to their rest: the spark's copy keeps its opacity of 0.8.
        same_as_written(rig, frame(s, rec, &rig, 0), w, ids);
    }
    // Before its birth a copy is not there, its place among its neighbours neither (at() lays its call out at 0,1 s, before
    // the birth); from it on, it is.
    {
        Rig rig = loaded(picture());
        PoseScript s = script_of(clip_of(R"(wait(0.5)
copies("dot", 1, function(i, c)
  c.x(10, 0.5)
  at(0.1, c:to_top())
end))"));
        const Recording rec = recorded(s, &rig);
        CHECK(block(rec, "dot[1]", Prop::Order).start == doctest::Approx(0.1));
        const Pose early = frame(s, rec, &rig, 0.25);
        CHECK(early.copies.empty());
        CHECK(early.order.empty());
        std::string why;
        REQUIRE(rig.pose(early, why));
        CHECK(same(rig.draw(128, 128), loaded(picture()).draw(128, 128)));
        CHECK(rig.drawn_copies().empty());
        Pic w;
        w.dot = {"dot_c1"};
        same_as_written(rig, frame(s, rec, &rig, 0.75), w, {{"dot[1]", "dot_c1"}});
    }
    // The copies over their part by number; to_top puts a copy over its neighbours.
    {
        Rig rig = loaded(picture());
        PoseScript s = script_of(clip_of(R"(copies("dot", 2, function(i, c) c.fill(i == 1 and "#ff0000" or "#00ff00") end))"));
        const Recording rec = recorded(s, &rig);
        const Pose p = frame(s, rec, &rig, 0);
        Pic w;
        w.dot = {"dot_c1", "dot_c2"};
        same_as_written(rig, p, w, {{"dot[1]", "dot_c1"}, {"dot[2]", "dot_c2"}});
        // The other way round is another picture: the order counts.
        Pic swapped;
        swapped.dot = {"dot_c2", "dot_c1"};
        Rig ref = loaded(swapped.text());
        std::string why;
        REQUIRE(ref.pose(renamed(p, {{"dot[1]", "dot_c1"}, {"dot[2]", "dot_c2"}}), why));
        CHECK_FALSE(same(ref.draw(128, 128), rig.draw(128, 128)));

        PoseScript top = script_of(clip_of(R"(copies("dot", 2, function(i, c)
  c.fill(i == 1 and "#ff0000" or "#00ff00")
  if i == 1 then c:to_top() end
end))"));
        const Recording rt = recorded(top, &rig);
        const Pose pt = frame(top, rt, &rig, 0);
        REQUIRE(pt.order.size() == 1);
        CHECK(pt.order[0].part == "dot[1]");
        same_as_written(rig, pt, w, {{"dot[1]", "dot_c1"}, {"dot[2]", "dot_c2"}});
    }
    // Moved, turned, scaled, skewed; its colours, line, tint, flash, blur, shadow; hidden; a copy of the turned leaf.
    {
        Rig rig = loaded(picture());
        PoseScript s = script_of(clip_of(R"(all(copies("dot", 3, function(i, c)
  c:to({ x = 6 * i, y = 3, rot = 30 * i, scale = 1 + i / 4, skew_x = 8 }, 0.5, "linear")
  if i == 1 then c:to({ fill = "#ff8000", stroke = "#000000", stroke_width = 1.5, opacity = 0.6 }, 0) end
  if i == 2 then c:to({ tint = "#80ff80", flash = 0.4, blur = 0.6, draw_to = 0.6 }, 0) end
  if i == 3 then c:to({ shadow = "#00000080", shadow_x = 1, shadow_y = 2, shadow_blur = 0.5 }, 0) end
end), copies("leaf", 2, function(i, c)
  c:to({ x = -10 * i, rot = 20, sy = 1.3, tint = "#ff8080" }, 0.5)
  if i == 2 then c.visible(false) end
end)))"));
        const Recording rec = recorded(s, &rig);
        Pic w;
        w.dot = {"dot_c1", "dot_c2", "dot_c3"};
        w.leaf = {"leaf_c1", "leaf_c2"};
        const std::map<std::string, std::string> ids = {{"dot[1]", "dot_c1"}, {"dot[2]", "dot_c2"}, {"dot[3]", "dot_c3"},
                                                        {"leaf[1]", "leaf_c1"}, {"leaf[2]", "leaf_c2"}};
        same_as_written(rig, frame(s, rec, &rig, 0.25), w, ids);
        same_as_written(rig, frame(s, rec, &rig, 0.5), w, ids);
    }
    // A mask, a morph, a path, a place and links, each against the copy written as a part in the same clip.
    Pic dot1;
    dot1.dot = {"dot_c1"};
    Pic spark1;
    spark1.spark = {"spark_c1"};
    same_clip("dot", R"(c:to({ x = 4, y = 14 }, 0)
c:mask("torso"))", dot1, "dot_c1", 0);
    same_clip("dot", R"(c:mask("torso", { invert = true }))", dot1, "dot_c1", 0);
    same_clip("spark", R"(c:morph("spark_sq", 1, "linear"))", spark1, "spark_c1", 0.5);
    same_clip("dot", R"(c:along("route", 1, "linear", { turn = true }))", dot1, "dot_c1", 0.4);
    same_clip("dot", R"(c:snap_to("torso", 0.5))", dot1, "dot_c1", 1.25, R"(part("body"):to({ x = 6, rot = 20 }, 1, "linear")
)");
    // Its own group moved: the copy stands in it as its part does.
    same_clip("dot", R"(c:snap_to("torso", 0.5))", dot1, "dot_c1", 1.25, R"(part("fx"):to({ x = 6, rot = 20 }, 1, "linear")
)");
    same_clip("dot", R"(c:attach("dot"))", dot1, "dot_c1", 1, "", R"(part("dot"):to({ x = 6, y = 5, rot = 40 }, 1, "linear"))");
    same_clip("dot", R"(c:attach("torso"))", dot1, "dot_c1", 1, "", R"(part("body"):to({ y = 5, rot = 15, sx = 1.2 }, 1, "linear"))");
    same_clip("dot", R"(c:above("spark_sq"))", dot1, "dot_c1", 0);
    same_clip("leaf", R"(c:attach("fx")
c.x(-8, 1))", Pic{{}, {}, {"leaf_c1"}}, "leaf_c1", 1.5, "", R"(part("fx"):to({ x = 5, rot = 10 }, 1))");
}

TEST_CASE("Motion copies: the rig makes a copy once and hides it in a pose without it") {
    Rig rig = loaded(picture());
    const usize n = rig.parts().size();
    Pose p;
    p.copies = {{"dot[1]", "dot", 1}};
    p.parts["dot[1]"] = moved(6, 0);
    std::string why;
    REQUIRE(rig.pose(p, why));
    CHECK(rig.parts().size() == n); // not a part of the picture
    CHECK(rig.part("dot[1]") == -1);
    const i32 index = rig.copy("dot[1]");
    CHECK(index == static_cast<i32>(n));
    REQUIRE(rig.drawn_copies().size() == 1);
    CHECK(rig.drawn_copies()[0].id == "dot[1]");
    CHECK(rig.drawn_copies()[0].index == index);
    CHECK(rig.pivot(index).x == rig.pivot(rig.part("dot")).x);
    CHECK(rig.copy("dot") == -1);
    // As a line it is its part's, in the picture's units through the moved group.
    {
        PathLine a, b;
        REQUIRE(rig.path_of(index, a, why));
        REQUIRE(rig.path_of(rig.part("dot"), b, why));
        CHECK(a.id == "dot[1]");
        REQUIRE(a.points.size() == b.points.size());
        for (usize k = 0; k < a.points.size(); ++k) {
            CHECK(a.points[k].x == b.points[k].x);
            CHECK(a.points[k].y == b.points[k].y);
        }
        CHECK(a.closed == b.closed);
    }

    // A pose without it: hidden, the picture at rest smooth and baked; it is kept.
    REQUIRE(rig.pose({}, why));
    Rig rest = loaded(picture());
    REQUIRE(rest.pose({}, why));
    CHECK(same(rig.draw(128, 128), rest.draw(128, 128)));
    CHECK(same(rig.bake(64, 64, {}), rest.bake(64, 64, {})));
    CHECK(rig.drawn_copies().empty());
    CHECK(rig.copy("dot[1]") == index);
    // Again: the same copy, nothing new made.
    REQUIRE(rig.pose(p, why));
    CHECK(rig.copy("dot[1]") == index);
    Pic w;
    w.dot = {"dot_c1"};
    same_as_written(rig, p, w, {{"dot[1]", "dot_c1"}});

    // Made later with a smaller number, a copy goes under the ones with bigger numbers.
    {
        Rig later = loaded(picture());
        Pose second;
        second.copies = {{"dot[2]", "dot", 2}};
        second.parts["dot[2]"].has_fill = true;
        second.parts["dot[2]"].fill = {0, 255, 0, 255};
        REQUIRE(later.pose(second, why));
        Pose both = second;
        both.copies.insert(both.copies.begin(), PartCopy{"dot[1]", "dot", 1});
        both.parts["dot[1]"].has_fill = true;
        both.parts["dot[1]"].fill = {255, 0, 0, 255};
        Pic two;
        two.dot = {"dot_c1", "dot_c2"};
        same_as_written(later, both, two, {{"dot[1]", "dot_c1"}, {"dot[2]", "dot_c2"}});
    }

    // It turns round its part's pivot, set after it was made too.
    rig.set_pivot("dot", {30, 20});
    Pose turned = p;
    turned.parts["dot[1]"] = moved(0, 0, 90);
    REQUIRE(rig.pose(turned, why));
    CHECK(rig.pivot(index).x == 30);
    CHECK(rig.pivot(index).y == 20);
    {
        Rig ref = loaded(w.text());
        ref.set_pivot("dot", {30, 20});
        ref.set_pivot("dot_c1", {30, 20});
        REQUIRE(ref.pose(renamed(turned, {{"dot[1]", "dot_c1"}}), why));
        CHECK(same(rig.draw(128, 128), ref.draw(128, 128)));
    }

    // What cannot be: named, the rest of the pose drawn.
    auto told = [&](const Pose& bad) {
        std::string said;
        CHECK_FALSE(rig.pose(bad, said));
        return said;
    };
    Pose bad;
    bad.copies = {{"nope[1]", "nope", 1}};
    CHECK(told(bad) == "копия «nope[1]»: в рисунке нет части «nope»");
    bad.copies = {{"body", "dot", 1}};
    CHECK(told(bad) == "копия «body»: так зовут часть рисунка");
    bad.copies = {{"ghost[1]", "ghost", 1}};
    CHECK(told(bad) == "копия «ghost[1]»: у части «ghost» нет фигур");
    bad.copies = {{"dot[1]", "spark", 1}};
    CHECK(told(bad) == "копия «dot[1]» уже есть, но не части «spark»");
    bad.copies.clear();
    bad.parts["dot[1]"] = moved(3, 0);
    CHECK(told(bad) == "в рисунке нет части «dot[1]»"); // made before, not in this pose
    bad.parts.clear();
    bad.morphs["dot[1]"] = {"spark_sq", 1};
    CHECK(told(bad) == "морфинг: в рисунке нет фигуры «dot[1]»");
}

TEST_CASE("Motion copies: a block of a copy is edited as one of a loop") {
    const Rig rig = loaded(picture());
    const std::string code = clip_of(R"(copies("dot", 3, function(i, c) c.x(i * 4, 0.5) end))");
    PoseScript s = script_of(code);
    const Recording rec = recorded(s, &rig);
    std::string out, why;
    // The call is one for all the copies: its length changes for each.
    REQUIRE(edit_length(rec, block(rec, "dot[2]", Prop::X), 0.8, s.code(), code, out, why));
    CHECK(out == clip_of(R"(copies("dot", 3, function(i, c) c.x(i * 4, 0.8) end))"));
    PoseScript again = script_of(out);
    const Recording edited = recorded(again, &rig);
    for (const char* id : {"dot[1]", "dot[2]", "dot[3]"}) CHECK(block(edited, id, Prop::X).length == doctest::Approx(0.8));
    CHECK(edited.copies.size() == 3);
    CHECK(can_edit(rec, block(rec, "dot[3]", Prop::X), BlockEdit::Timing, s.code(), why));
}
