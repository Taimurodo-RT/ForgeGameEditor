// Part 6а of docs/models/сцены-модель.md («Сцена и актёры»): a scene's stage puts shapes, pictures of «Ресурсы» and
// characters of «Векторная» on it, each where and as big as it says; its scene(p) is recorded as a clip, and an actor
// plays its character's clips in the scene's time (play waits, loop goes round, stop holds), the scene's own
// transitions of the actor's parts over them by rule 5. The marks of the actors' clips are the scene's at their
// moments, on every round of a loop (one at a clip's end at each round's end, as a game's screen gives it). A frame of the scene is the background, the pictures and the actors drawn alone
// in their poses, laid over each other in the stage's order.
// Part 6б: the camera (its matrix, center_on, along, follow and its end) and the transitions in and out, the player's
// frames of each kind and of one's own function against frames worked out by hand; the errors of both.
// Part 6в: the music's calls and what plays at a moment (fades, the same music, silence), the waits for the player and
// the talks as stops of the scene's own flow, the actors' clips going on while the scene waits; the errors of all three.
// Part 6г2: the scene's folder as the game reads it, its marks as the tab lists them, where the stage's picture in the
// game puts each picture and actor against the tab's rig, a run drawing only its own, an actor's frame beyond its canvas,
// and the scene on a game's screen: its layers, its marks frame by frame, its music, its end, skipped and dropped; a
// picture the camera turns drawn where it stands unturned (as the game draws it).
// Part 6г3: the scene's marks by the time it lived (its waits counted), those made after a wait coming after it, and the
// scene on a game's screen standing at its waits and talks while its actors and its music go on, skipped or dropped in a
// talk closing it.

#include "forge/assets/image.h"
#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/game/scenes.h"
#include "forge/game/screens.h"
#include "forge/pixel/bake.h"
#include "forge/pixel/doc.h"
#include "forge/pixel/draw.h"
#include "forge/pixel/motion.h"
#include "forge/pixel/motion_edit.h"
#include "forge/pixel/rig.h"
#include "forge/pixel/scene.h"
#include "forge/pixel/scene_gift.h"
#include "forge/pixel/screen.h"
#include "forge/pixel/script.h"
#include "forge/pixel/timeline.h"
#include "forge/render/offscreen.h"
#include "forge/ui/live_svg.h"
#include "forge/ui/ui.h"
#include "forge/vector/motion_file.h"
#include "forge/vector/scene_file.h"
#include "forge/vector/svg.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/ElementUtilities.h>
#include <doctest/doctest.h>
#include <thorvg.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <numbers>
#include <set>
#include <span>
#include <ostream> // doctest's INFO prints a string_view through it; MSVC needs the whole class
#include <string>
#include <vector>

using namespace forge;
using namespace forge::pixel;

namespace {

// The hedgehog: a head and a nose in a body, feet below; its file turns the nose round 30, 26. walk bobs the head and
// marks a step halfway, wave turns the nose, stretch is as long as p.len, land marks its end.
const char* kHedgehog = R"(return {
  params = { height = 4, len = 1 },
  clips = {
    walk = function(p)
      part("head").y(-p.height, 0.25)
      emit("шаг")
      part("head").y(0, 0.25)
    end,
    wave = function(p)
      part("nose").rot(30, 0.5)
    end,
    stretch = function(p)
      wait(p.len)
    end,
    land = function(p)
      wait(0.5)
      emit("земля")
    end,
  },
  draw = function(p, rng)
    canvas(64, 64)
    group("body", function()
      circle("head", { x = 32, y = 28, r = 14, fill = "#c09060" })
      rect("nose", { x = 30, y = 26, w = 4, h = 3, fill = "#a06040" })
    end)
    rect("feet", { x = 20, y = 50, w = 24, h = 8, fill = "#604020" })
  end,
}
)";

// The beetle: hop lifts the shell and swaps to the flat drawing on the way down.
const char* kBeetle = R"(return {
  clips = {
    hop = function(p)
      part("shell").y(-6, 0.3)
      swap("flat", 0.2)
      part("shell").y(0, 0.3)
    end,
  },
  draw = function(p, rng)
    canvas(48, 32)
    ellipse("shell", { x = 24, y = 18, rx = 18, ry = 12, fill = "#30a050" })
    circle("eye", { x = 36, y = 14, r = 3, fill = "#ffffff" })
  end,
  drawings = {
    flat = function(p, rng)
      canvas(48, 32)
      ellipse("shell", { x = 24, y = 22, rx = 20, ry = 8, fill = "#208040" })
      circle("eye", { x = 38, y = 20, r = 3, fill = "#ffffff" })
    end,
  },
}
)";

// The flag, drawn in «Ресурсы» (no draw in its code): its picture's top left at -10, -20, its 0, 0 the dot's middle.
// talk opens the mouth.
const char* kFlag = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="40" height="40" viewBox="-10 -20 40 40">
  <rect id="cloth" x="-10" y="-20" width="40" height="40" fill="#ff0000"/>
  <circle id="dot" cx="0" cy="0" r="5" fill="#0000ff"/>
  <path id="mouth" d="M 10 8 L 24 8 L 24 10 L 10 10 Z" fill="#000000"/>
  <path id="open" d="M 10 4 L 24 4 L 24 16 L 10 16 Z" fill="#000000" display="none"/>
</svg>
)svg";

const char* kFlagCode = R"(return {
  clips = {
    talk = function(p)
      part("mouth"):morph("open", 0.5)
    end,
  },
}
)";

// «Ресурсы» in memory: the three characters, a 40 × 30 picture and a few broken files.
struct Files {
    std::map<std::string, std::vector<u8>> bytes;
    std::map<std::string, std::string> svgs; // by Guid

    void put(const std::string& path, std::string_view text) { bytes[path].assign(text.begin(), text.end()); }

    SceneFiles scene() {
        SceneFiles f;
        f.read = [this](const std::string& path, std::vector<u8>& out, std::string& why) {
            const auto it = bytes.find(path);
            if (it == bytes.end()) return why = "нет файла «" + path + "»", false;
            out = it->second;
            return true;
        };
        f.svg = [this](const std::string& guid, std::string& text, std::string& why) {
            const auto it = svgs.find(guid);
            if (it == svgs.end()) return why = "нет SVG " + guid, false;
            text = it->second;
            return true;
        };
        return f;
    }
};

assets::CookedTexture forest() {
    assets::CookedTexture t;
    t.width = 40;
    t.height = 30;
    t.rgba8.resize(40 * 30 * 4);
    for (u32 y = 0; y < 30; ++y)
        for (u32 x = 0; x < 40; ++x) {
            u8* p = &t.rgba8[(y * 40 + x) * 4];
            p[0] = static_cast<u8>(40 + x * 4), p[1] = static_cast<u8>(120 + y * 3), p[2] = 60, p[3] = 255;
        }
    return t;
}

void project(Files& f) {
    PixelDoc h;
    h.script = "Ёжик.luau";
    h.frame_w = h.frame_h = 64;
    h.pivots["nose"] = {30, 26};
    f.put("персонажи/Ёжик.pixel.json", write_pixel_doc(h));
    f.put("персонажи/Ёжик.luau", kHedgehog);
    PixelDoc b;
    b.script = "Жук.luau";
    b.frame_w = 48;
    b.frame_h = 32;
    f.put("персонажи/Жук.pixel.json", write_pixel_doc(b));
    f.put("персонажи/Жук.luau", kBeetle);
    PixelDoc bad = h;
    bad.pivots.clear();
    bad.script = "Сломан.luau";
    f.put("персонажи/Сломан.pixel.json", write_pixel_doc(bad));
    f.put("персонажи/Сломан.luau", "return {\n  clips = { walk = function(p)\n    wait(1)\n  end },\n  draw = function(p, rng)\n    rect(\"a\", { w = 1 })\n  end,\n}\n");
    PixelDoc sc = h;
    sc.pivots.clear();
    sc.script = "Сцена.luau";
    f.put("персонажи/Сцена.pixel.json", write_pixel_doc(sc));
    f.put("персонажи/Сцена.luau", new_scene_code("Сцена"));
    PixelDoc fl;
    fl.svg = "flag-svg";
    fl.script = "Флаг.luau";
    fl.frame_w = fl.frame_h = 40;
    f.put("персонажи/Флаг.pixel.json", write_pixel_doc(fl));
    f.put("персонажи/Флаг.luau", kFlagCode);
    f.svgs["flag-svg"] = kFlag;
    std::vector<u8> png;
    REQUIRE(assets::encode_image(forest(), ".png", png));
    f.bytes["картинки/лес.png"] = png;
    f.put("картинки/лес.jpg", "не картинка");
}

// The scene's code: stage from line 3, scene from the line after the stage's end.
std::string scene_code(std::string_view stage, std::string_view scene) {
    return "return {\n  stage = function(p, rng)\n" + std::string(stage) + "\n  end,\n  scene = function(p)\n" + std::string(scene) + "\n  end,\n}\n";
}

// Lines 3–8; the scene's from 11.
const char* kStage = R"(    canvas(160, 120)
    background("#203040")
    picture("лес", "картинки/лес.png", { x = 8, y = 6 })
    actor("ёжик", "персонажи/Ёжик", { x = 40, y = 100, pivot = { 32, 60 } })
    actor("жук", "персонажи/Жук", { x = 120, y = 90, scale = 0.5, rot = 15, pivot = { 24, 16 } })
    path("дорога", { d = "M 40 100 L 120 100", fill = "none", stroke = "#ffffff", hidden = true }))";

bool has(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

void load(Scene& s, Files& files, std::string_view scene, std::string_view stage = kStage) {
    const bool ok = s.load(scene_code(stage, scene), {}, files.scene());
    INFO(s.error());
    REQUIRE(ok);
}

void record(Scene& s) {
    const bool ok = s.record();
    INFO(s.error());
    REQUIRE(ok);
}

const ActorPlay& play_of(const Recording& rec, std::string_view actor, ActorPlay::Kind kind, usize nth = 0) {
    for (const ActorPlay& p : rec.plays)
        if (p.actor == actor && p.kind == kind && nth-- == 0) return p;
    FAIL("no such play");
    return rec.plays.front();
}

} // namespace

TEST_CASE("Сцена: stage ставит фигуры, картинки и актёров") {
    Files files;
    project(files);
    Scene s;
    load(s, files, "    wait(1)");
    const Stage& st = s.stage();
    CHECK(st.width() == 160);
    CHECK(st.height() == 120);
    CHECK(st.background == "#203040");
    CHECK(st.background_line == 4);
    REQUIRE(st.items.size() == 3);
    const StageItem& pic = st.items[0];
    CHECK(pic.kind == StageItem::Kind::Picture);
    CHECK(pic.id == "лес");
    CHECK(pic.file == "картинки/лес.png");
    CHECK(pic.x == 8);
    CHECK(pic.y == 6);
    CHECK(pic.w == 40);
    CHECK(pic.h == 30);
    CHECK_FALSE(pic.sized);
    CHECK(pic.line == 5);
    const StageItem& hog = st.items[1];
    CHECK(hog.kind == StageItem::Kind::Actor);
    CHECK(hog.file == "персонажи/Ёжик");
    CHECK(hog.w == 64);
    CHECK(hog.h == 64);
    CHECK(hog.origin.x == 0);
    CHECK(hog.origin.y == 0);
    CHECK(hog.pivot.x == 32);
    CHECK(hog.pivot.y == 60);
    CHECK(hog.line == 6);
    const StageItem& bug = st.items[2];
    CHECK(bug.scale == 0.5);
    CHECK(bug.rot == 15);
    CHECK(bug.w == 48);
    CHECK(bug.h == 32);
    // The order of the calls: each item's group where it was put, the path after them.
    std::vector<std::string> ids;
    for (const DrawShape& d : st.drawing.shapes) ids.push_back(d.id);
    CHECK(ids == std::vector<std::string>{"лес", "ёжик", "жук", "дорога"});
    for (const StageItem& it : st.items) CHECK(st.drawing.shapes[static_cast<usize>(it.group)].id == it.id);

    // The stage's rig: the items are groups, the actors' pictures hosted in theirs with their ids under the actor's.
    const Rig& rig = s.rig();
    CHECK(rig.width() == 160);
    for (const char* id : {"лес", "ёжик", "жук", "дорога"}) CHECK(rig.part(id) >= 0);
    REQUIRE(s.actor_rig("ёжик"));
    CHECK(rig.hosted("ёжик") == s.actor_rig("ёжик"));
    CHECK(rig.hosted("лес") == nullptr);
    CHECK(s.actor_rig("лес") == nullptr);
    CHECK(&rig.holder("ёжик/head") == s.actor_rig("ёжик"));
    CHECK(rig.holder("ёжик/head").part("ёжик/head") >= 0);
    CHECK(rig.holder("ёжик/нет").part("ёжик/нет") < 0);
    CHECK(&rig.holder("жук/shell") == s.actor_rig("жук"));
    CHECK(rig.holder("жук/shell").drawings() == std::vector<std::string>{"flat"});
    // A picture turns round its middle, an actor round the point of the scene its pivot stands at.
    const vector::Point pp = rig.pivot(rig.part("лес")), ap = rig.pivot(rig.part("ёжик"));
    CHECK(pp.x == 28);
    CHECK(pp.y == 21);
    CHECK(ap.x == 40);
    CHECK(ap.y == 100);
    // The actor's parts turn round its character's pivots.
    const SceneCharacter* c = s.character("ёжик");
    REQUIRE(c);
    const Rig& ar = *s.actor_rig("ёжик");
    for (const char* id : {"body", "head", "nose", "feet"}) {
        const vector::Point a = ar.pivot(ar.part(std::string("ёжик/") + id)), b = c->rig.pivot(c->rig.part(id));
        CHECK(a.x == b.x);
        CHECK(a.y == b.y);
    }
    // The nose's the one its file gives, not its middle.
    const vector::Point nose = ar.pivot(ar.part("ёжик/nose"));
    CHECK(nose.x == 30);
    CHECK(nose.y == 26);
    CHECK(s.character("лес") == nullptr);
    const std::vector<std::string>& read = s.files();
    for (const char* f : {"картинки/лес.png", "персонажи/Ёжик.pixel.json", "персонажи/Ёжик.luau", "персонажи/Жук.pixel.json", "персонажи/Жук.luau"})
        CHECK(std::find(read.begin(), read.end(), f) != read.end());

    const std::string svg = stage_svg(st);
    CHECK(has(svg, "transform=\"translate(8 6)\""));
    CHECK(has(svg, "transform=\"matrix("));
    CHECK(has(svg, "fill=\"none\""));

    // Without canvas the scene is 1920 × 1080; a picture of the size it is given; a background as #rrggbb.
    Scene big;
    load(big, files, "    wait(1)", "    background(\"#234\")\n    picture(\"лес\", \"картинки/лес.png\", { x = 10, y = 20, w = 80, h = 60 })");
    CHECK(big.stage().width() == kSceneWidth);
    CHECK(big.stage().height() == kSceneHeight);
    CHECK(big.stage().background == "#223344");
    REQUIRE(big.stage().items.size() == 1);
    CHECK(big.stage().items[0].sized);
    CHECK(big.stage().items[0].w == 80);
    CHECK(big.stage().items[0].h == 60);
    Scene bare;
    load(bare, files, "    wait(1)", "    rect(\"a\", { x = 0, y = 0, w = 4, h = 4 })");
    CHECK(bare.stage().background.empty());
}

TEST_CASE("Сцена: актёр из SVG — его рисунок от начала viewBox") {
    Files files;
    project(files);
    Scene s;
    load(s, files, R"(    at(1, part("флаг/mouth"):morph("флаг/open", 0.2))
    actor("флаг"):play("talk")
    wait(1))",
         "    canvas(160, 120)\n    actor(\"флаг\", \"персонажи/Флаг\", { x = 60, y = 50 })");
    REQUIRE(s.stage().items.size() == 1);
    const StageItem& it = s.stage().items[0];
    CHECK(it.origin.x == -10);
    CHECK(it.origin.y == -20);
    CHECK(it.w == 40);
    CHECK(it.h == 40);
    record(s);
    CHECK(s.recording()->length == doctest::Approx(1.5));
    // Its group is its picture's box where the picture is: its 0, 0 at 60, 50 of the scene.
    REQUIRE(s.frame(0));
    Rig& rig = s.rig();
    f64 b[4];
    rig.bounds(rig.part("флаг"), translation(0, 0), b);
    CHECK(b[0] == doctest::Approx(50));
    CHECK(b[1] == doctest::Approx(30));
    CHECK(b[2] == doctest::Approx(90));
    CHECK(b[3] == doctest::Approx(70));
    auto at = [&](f64 t, u32 x, u32 y) {
        REQUIRE(s.frame(t));
        const assets::CookedTexture px = s.draw(160, 120);
        const u8* p = &px.rgba8[(y * 160 + x) * 4];
        return Rgba8{p[0], p[1], p[2], p[3]};
    };
    CHECK(at(0, 60, 50) == Rgba8{0, 0, 255, 255}); // the dot
    CHECK(at(0, 51, 31) == Rgba8{255, 0, 0, 255}); // the cloth's top left
    CHECK(at(0, 48, 50).a == 0);                   // the stage without background
    // The mouth: shut at first, open at the clip's end and after it; the scene's morph at rest before its start leaves
    // the clip's, at its end it is the scene's.
    CHECK(at(0, 77, 63) == Rgba8{255, 0, 0, 255});
    CHECK(at(0.5, 77, 63) == Rgba8{0, 0, 0, 255});
    CHECK(at(0.8, 77, 63) == Rgba8{0, 0, 0, 255});
    CHECK(at(1.4, 77, 63) == Rgba8{0, 0, 0, 255});
}

TEST_CASE("Сцена: матрица актёра") {
    StageItem a;
    a.kind = StageItem::Kind::Actor;
    a.x = 100, a.y = 50, a.pivot = {32, 60}, a.scale = 2, a.sx = 1.5, a.rot = 90;
    const Affine m = actor_matrix(a);
    // The pivot stands at x, y; a step right in the picture is 3 down on the scene (turned clockwise, scaled 2 × 1.5),
    // a step down is 2 left.
    const vector::Point p = m.apply({32, 60}), r = m.apply({33, 60}), d = m.apply({32, 61});
    CHECK(p.x == doctest::Approx(100));
    CHECK(p.y == doctest::Approx(50));
    CHECK(r.x == doctest::Approx(100));
    CHECK(r.y == doctest::Approx(53));
    CHECK(d.x == doctest::Approx(98));
    CHECK(d.y == doctest::Approx(50));
    StageItem plain;
    plain.x = 7, plain.y = -3;
    CHECK(actor_matrix(plain) == translation(7, -3));
}

TEST_CASE("Сцена: ошибки stage — со строкой вызова") {
    Files files;
    project(files);
    struct Case {
        const char* stage;
        const char* says;
    };
    const Case cases[] = {
        {"    actor(\"ёжик\", \"персонажи/Нет\")", "нет файла «персонажи/Нет.pixel.json»"},
        {"    picture(\"лес\", \"картинки/нет.png\")", "нет файла «картинки/нет.png»"},
        {"    picture(\"лес\", \"картинки/лес.jpg\")", "не PNG"},
        {"    actor(\"ёжик\", \"персонажи/Сломан\")", "actor «ёжик»: персонаж «персонажи/Сломан»: код «Сломан.luau»: строка 6"},
        {"    actor(\"ёжик\", \"персонажи/Сцена\")", "сцена, а не персонаж"},
        {"    actor(\"ё/ж\", \"персонажи/Ёжик\")", "буквы, цифры"},
        {"    actor(\"ёжик\", \"персонажи/Ёжик\", { z = 1 })", "поля «z» нет"},
        {"    picture(\"лес\", \"картинки/лес.png\", { w = 10 })", "w и h — вместе"},
        {"    actor(\"ёжик\", \"персонажи/Ёжик\", { params = { len = \"x\" } })", "params — таблица чисел"},
        {"    group(\"g\", function()\n      picture(\"лес\", \"картинки/лес.png\")\n    end)", "не в группу"},
        {"    rect(\"лес\", { x = 0, y = 0, w = 2, h = 2 })\n    picture(\"лес\", \"картинки/лес.png\")", "уже есть на сцене"},
        {"    background(\"#000\")\n    background(\"#111\")", "background — один раз"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.stage);
        Scene s;
        CHECK_FALSE(s.load(scene_code(c.stage, "    wait(1)"), {}, files.scene()));
        CHECK_FALSE(s.loaded());
        INFO(s.error());
        CHECK(has(s.error(), c.says));
        // The call that failed: the last line of the stage.
        const u32 lines = static_cast<u32>(std::count(c.stage, c.stage + std::strlen(c.stage), '\n'));
        CHECK(s.error_line() == 3 + lines - (has(c.stage, "group") ? 1 : 0));
    }
    // A broken character's code says its line.
    Scene s;
    CHECK_FALSE(s.load(scene_code("    actor(\"ёжик\", \"персонажи/Сломан\")", "    wait(1)"), {}, files.scene()));
    CHECK(has(s.error(), "строка 6"));
    // A scene's module has no clips, draw or pose; nor is a character's code a scene.
    CHECK_FALSE(s.load("return {\n  stage = function(p, rng) end,\n  scene = function(p) end,\n  clips = {},\n}\n", {}, files.scene()));
    CHECK(has(s.error(), "у сцены нет clips"));
    CHECK(s.error_line() == 4);
    CHECK_FALSE(s.load(kHedgehog, {}, files.scene()));
    CHECK(has(s.error(), "это не сцена"));
    CHECK_FALSE(s.load("return {\n  stage = function(p, rng) end,\n}\n", {}, files.scene()));
    CHECK(has(s.error(), "scene = function(p)"));
}

TEST_CASE("Сцена: клипы актёров во времени сцены") {
    Files files;
    project(files);
    Scene s;
    load(s, files, R"(    actor("ёжик"):play("walk")
    actor("ёжик"):play("walk", { speed = 2 })
    actor("ёжик"):loop("wave")
    spawn(actor("жук"):play("hop"))
    wait(1)
    actor("ёжик"):stop()
    wait(0.5))");
    CHECK(s.recording() == nullptr);
    record(s);
    const Recording& rec = *s.recording();
    CHECK(rec.length == doctest::Approx(2.25));
    REQUIRE(rec.plays.size() == 5);
    // play waits its clip's length / speed; loop, spawn and stop do not.
    const ActorPlay& walk = play_of(rec, "ёжик", ActorPlay::Kind::Play);
    CHECK(walk.clip == "walk");
    CHECK(walk.start == 0);
    CHECK(walk.length == doctest::Approx(0.5));
    CHECK(walk.clip_length == doctest::Approx(0.5));
    CHECK(walk.site.site >= 0);
    const ActorPlay& fast = play_of(rec, "ёжик", ActorPlay::Kind::Play, 1);
    CHECK(fast.start == doctest::Approx(0.5));
    CHECK(fast.speed == 2);
    CHECK(fast.length == doctest::Approx(0.25));
    const ActorPlay& wave = play_of(rec, "ёжик", ActorPlay::Kind::Loop);
    CHECK(wave.start == doctest::Approx(0.75));
    const ActorPlay& hop = play_of(rec, "жук", ActorPlay::Kind::Play);
    CHECK(hop.start == doctest::Approx(0.75));
    CHECK(hop.length == doctest::Approx(0.8));
    const ActorPlay& stop = play_of(rec, "ёжик", ActorPlay::Kind::Stop);
    CHECK(stop.start == doctest::Approx(1.75));
    CHECK(stop.clip.empty());

    // Which clip at which moment of it.
    struct At {
        const char* actor;
        f64 t;
        const ActorPlay* play;
        f64 time;
    };
    const At at[] = {
        {"ёжик", 0, &walk, 0},       {"ёжик", 0.25, &walk, 0.25}, {"ёжик", 0.6, &fast, 0.2},  {"ёжик", 1.05, &wave, 0.3},
        {"ёжик", 1.6, &wave, 0.35},  {"ёжик", 2.0, &wave, 0},     {"ёжик", 9, &wave, 0},       {"жук", 0.5, nullptr, 0},
        {"жук", 0.95, &hop, 0.2},    {"жук", 2.0, &hop, 0.8},     {"нет", 1, nullptr, 0},
    };
    for (const At& a : at) {
        CAPTURE(a.actor);
        CAPTURE(a.t);
        const ActorMoment m = actor_moment(rec, a.actor, a.t);
        CHECK(m.play == a.play);
        CHECK(m.time == doctest::Approx(a.time));
    }
    // A stop holds the moment the one before it showed at its start, a stop after a stop that one's.
    Recording two = rec;
    ActorPlay again = stop;
    again.start = 2.1;
    again.order += 100;
    two.plays.push_back(again);
    const ActorMoment held = actor_moment(two, "ёжик", 2.2);
    CHECK(held.play == &two.plays[2]);
    CHECK(held.time == doctest::Approx(0));

    // A clip's parameters: the stage's over the character's, a play's over both; the clip recorded with them.
    Scene p;
    load(p, files, R"(    actor("ёжик"):play("stretch", { params = { len = 2 } })
    actor("ёжик"):play("stretch")
    actor("ёж2"):play("stretch"))",
         "    actor(\"ёжик\", \"персонажи/Ёжик\")\n    actor(\"ёж2\", \"персонажи/Ёжик\", { params = { len = 0.5 } })");
    record(p);
    CHECK(p.recording()->length == doctest::Approx(3.5));
    CHECK(play_of(*p.recording(), "ёжик", ActorPlay::Kind::Play).length == doctest::Approx(2));
    CHECK(play_of(*p.recording(), "ёжик", ActorPlay::Kind::Play).params.at("len") == 2);
    CHECK(play_of(*p.recording(), "ёжик", ActorPlay::Kind::Play, 1).length == doctest::Approx(1));
    CHECK(play_of(*p.recording(), "ёж2", ActorPlay::Kind::Play).length == doctest::Approx(0.5));
}

TEST_CASE("Сцена: что сцене нельзя — со строкой") {
    Files files;
    project(files);
    struct Case {
        const char* scene;
        const char* says;
    };
    const Case cases[] = {
        {"    part(\"ёжик/head\"):along(\"дорога\", 1)", "у части актёра «ёжик/head» нет along"},
        {"    part(\"ёжик/head\"):attach(\"лес\")", "у части актёра «ёжик/head» нет attach"},
        {"    part(\"ёжик/head\"):snap_to(\"лес\", 0.2)", "у части актёра «ёжик/head» нет snap_to"},
        {"    copies(\"ёжик\", 2, function(i, c) end)", "копии актёра — не в этой версии"},
        {"    copies(\"ёжик/head\", 2, function(i, c) end)", "у части актёра «ёжик/head» нет copies"},
        {"    part(\"лес\"):along(\"ёжик/head\", 1)", "часть актёра, а здесь нужна часть сцены"},
        {"    part(\"лес\"):attach(\"ёжик/head\")", "часть актёра, а здесь нужна часть сцены"},
        {"    part(\"ёжик/head\"):mask(\"дорога\")", "из другого рисунка"},
        {"    part(\"дорога\"):morph(\"ёжик/head\", 0.2)", "из другого рисунка"},
        {"    part(\"лес\").fill(\"#ff0000\", 0.2)", "у картинки «лес» нет «fill»"},
        {"    part(\"лес\").tint(\"#ff0000\", 0.2)", "у картинки «лес» нет «tint»"},
        {"    swap(\"flat\", 0.2)", "swap — у персонажа"},
        {"    start_from(\"walk\")", "start_from — у клипа персонажа"},
        {"    actor(\"ёжик\"):part(\"нет\")", "у актёра «ёжик» нет части «нет»"},
        {"    actor(\"нет\"):play(\"walk\")", "на сцене нет актёра «нет»"},
        {"    actor(\"лес\"):play(\"walk\")", "на сцене нет актёра «лес»"},
        {"    actor(\"нет\"):to({ x = 1 }, 0)", "на сцене нет актёра «нет»"},
        {"    actor(\"ёжик\"):play(\"walk\", { speed = 0 })", "speed — число больше 0"},
        {"    actor(\"ёжик\"):play(\"walk\", { blend = -1 })", "play: blend — число секунд, 0 или больше"},
        {"    actor(\"ёжик\"):loop(\"walk\", { blend = \"x\" })", "loop: blend — число секунд, 0 или больше"},
        {"    actor(\"ёжик\"):play(\"walk\", { pace = 1 })", "поля «pace» нет"},
        {"    actor(\"ёжик\"):play(\"walk\", { params = { len = \"x\" } })", "params — таблица чисел"},
        {"    actor(\"ёжик\"):play()", "первым — имя клипа"},
        {"    actor(\"ёжик\"):stop(1)", "stop() — без аргументов"},
        {"    actor(\"жук\"):play(\"walk\")", "у персонажа «персонажи/Жук» нет клипа «walk» (есть «hop»)"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.scene);
        Scene s;
        load(s, files, c.scene);
        CHECK_FALSE(s.record());
        CHECK(s.recording() == nullptr);
        INFO(s.error());
        CHECK(has(s.error(), c.says));
        CHECK(s.error_line() == 11);
    }
    // The whole actor is a part of the stage: it goes along the stage's path and is attached to its picture.
    Scene s;
    load(s, files, "    part(\"ёжик\"):along(\"дорога\", 1)\n    part(\"жук\"):attach(\"лес\")\n    part(\"лес\").x(10, 1)");
    record(s);
    CHECK(s.frame(1));
}

TEST_CASE("Сцена: переход сцены поверх позы клипа (правило 5)") {
    PartPose clip;
    clip.x = 1, clip.y = 2, clip.rot = 10, clip.skew_y = 3, clip.scale = 2, clip.sy = 0.5, clip.opacity = 0.5, clip.dash = 1;
    clip.has_fill = true, clip.fill = {10, 20, 30, 255}, clip.fill_k = 1;
    clip.draw_to = 0.5;
    clip.has_tint = true, clip.tint = {1, 2, 3, 255};
    clip.flash = 0.25;
    clip.blur = 2;
    clip.mask = "ёжик/m";
    PartPose scene;
    scene.x = 2, scene.y = -2, scene.rot = 5, scene.skew_x = 4, scene.skew_y = 1, scene.scale = 1.5, scene.sx = 2, scene.sy = 3;
    scene.opacity = 0.5, scene.dash = 2;
    PartPose a = clip;
    over_part(a, scene);
    CHECK(a.x == 3);
    CHECK(a.y == 0);
    CHECK(a.rot == 15);
    CHECK(a.skew_x == 4);
    CHECK(a.skew_y == 4);
    CHECK(a.dash == 3);
    CHECK(a.scale == 3);
    CHECK(a.sx == 2);
    CHECK(a.sy == 1.5);
    CHECK(a.opacity == 0.25);
    CHECK(a.visible);
    // What the scene did not move from rest is the clip's.
    CHECK(a.has_fill);
    CHECK(a.fill == Rgba8{10, 20, 30, 255});
    CHECK(a.draw_from == 0);
    CHECK(a.draw_to == 0.5);
    CHECK(a.tint == Rgba8{1, 2, 3, 255});
    CHECK(a.flash == 0.25);
    CHECK(a.blur == 2);
    CHECK(a.mask == "ёжик/m");
    // What it moved is its own; seen when both say so.
    PartPose looks;
    looks.has_fill = true, looks.fill = {200, 0, 0, 255}, looks.fill_k = 0.5;
    looks.has_stroke = true, looks.stroke = {0, 200, 0, 255}, looks.stroke_k = 0.25;
    looks.has_stroke_width = true, looks.stroke_width = 3, looks.stroke_width_k = 1;
    looks.draw_from = 0.2;
    looks.has_tint = true, looks.tint = {9, 9, 9, 255}, looks.tint_k = 0.75;
    looks.flash = 0.5;
    looks.has_flash_color = true, looks.flash_color = {255, 0, 0, 255};
    looks.blur = 1;
    looks.has_shadow = true, looks.shadow = {0, 0, 0, 128}, looks.shadow_x = 2;
    looks.mask = "ёжик/n", looks.mask_invert = true;
    looks.visible = false;
    PartPose b = clip;
    over_part(b, looks);
    CHECK(b.fill == Rgba8{200, 0, 0, 255});
    CHECK(b.fill_k == 0.5);
    CHECK(b.has_stroke);
    CHECK(b.stroke_k == 0.25);
    CHECK(b.has_stroke_width);
    CHECK(b.stroke_width == 3);
    CHECK(b.draw_from == doctest::Approx(0.2));
    CHECK(b.draw_to == 1);
    CHECK(b.tint == Rgba8{9, 9, 9, 255});
    CHECK(b.tint_k == 0.75);
    CHECK(b.flash == 0.5);
    CHECK(b.has_flash_color);
    CHECK(b.blur == 1);
    CHECK(b.has_shadow);
    CHECK(b.shadow_x == 2);
    CHECK(b.mask == "ёжик/n");
    CHECK(b.mask_invert);
    CHECK_FALSE(b.visible);
    // A colour at k = 0 is the scene at rest: the clip's stays.
    PartPose rest;
    rest.has_fill = true, rest.fill = {1, 1, 1, 255}, rest.fill_k = 0;
    PartPose c = clip;
    over_part(c, rest);
    CHECK(c.fill == Rgba8{10, 20, 30, 255});
    // Hidden by its clip, a part the scene does not hide stays hidden.
    PartPose hidden;
    hidden.visible = false;
    over_part(hidden, PartPose{});
    CHECK_FALSE(hidden.visible);
}

TEST_CASE("Сцена: часть актёра — клип в его миг и переход сцены поверх") {
    Files files;
    project(files);
    Scene s;
    load(s, files, R"(    actor("ёжик"):loop("walk")
    part("ёжик/head").x(5, 0)
    part("ёжик/head"):to({ rot = 20, sx = 2 }, 0.5)
    part("ёжик").x(10, 0)
    part("ёжик").tint("#ff0000", 0)
    actor("ёжик"):part("nose").opacity(0.5, 0)
    wait(1))");
    record(s);
    const f64 t = 0.6;
    REQUIRE(s.frame(t));
    const ActorMoment m = actor_moment(*s.recording(), "ёжик", t);
    REQUIRE(m.play);
    CHECK(m.time == doctest::Approx(0.1));
    // The same character posed by its clip at that moment, the scene's transition over it.
    SceneCharacter c;
    std::string why;
    REQUIRE(load_character(files.scene(), "персонажи/Ёжик", c, why));
    PoseScript code;
    REQUIRE(code.load(c.code));
    Pose pose;
    REQUIRE(code.pose(c.rig, m.time, "walk", c.doc.params, c.doc.seed, pose));
    PartPose head;
    head.x = 5, head.rot = 20, head.sx = 2;
    over_part(pose.parts["head"], head);
    pose.parts["nose"].opacity *= 0.5;
    REQUIRE(c.rig.pose(pose, why));
    const Rig& ar = *s.actor_rig("ёжик");
    for (const char* id : {"head", "nose", "body"}) {
        CAPTURE(id);
        const Affine want = c.rig.matrix(c.rig.part(id)), got = ar.matrix(ar.part(std::string("ёжик/") + id));
        CHECK(got.a == doctest::Approx(want.a));
        CHECK(got.b == doctest::Approx(want.b));
        CHECK(got.c == doctest::Approx(want.c));
        CHECK(got.d == doctest::Approx(want.d));
        CHECK(got.e == doctest::Approx(want.e));
        CHECK(got.f == doctest::Approx(want.f));
    }
    // The whole actor: moved as its group of the stage, round its pivot; tinted as its picture.
    const Rig& rig = s.rig();
    const Affine g = rig.matrix(rig.part("ёжик")), want = translation(10, 0) * actor_matrix(s.stage().items[1]);
    CHECK(g.e == doctest::Approx(want.e));
    CHECK(g.f == doctest::Approx(want.f));
    const assets::CookedTexture px = s.draw(160, 120);
    REQUIRE(px.width == 160);
    // The head above the nose, (32, 20) of the hedgehog's picture: at 40 − 32 + 10 + 32, 100 − 60 + 20 of the scene, red.
    const u8* head_px = &px.rgba8[(60 * 160 + 50) * 4];
    CHECK(head_px[0] == 0xc0);
    CHECK(head_px[1] == 0);
    CHECK(head_px[2] == 0);
    CHECK(head_px[3] == 255);
    // A colour of the whole actor is its picture's: the beetle's shell green.
    Scene green;
    load(green, files, "    part(\"жук\").fill(\"#00ff00\", 0)\n    wait(1)");
    record(green);
    REQUIRE(green.frame(0.5));
    const assets::CookedTexture shot = green.draw(160, 120);
    const vector::Point shell = actor_matrix(green.stage().items[2]).apply({24, 18});
    const u8* shell_px = &shot.rgba8[(static_cast<usize>(shell.y) * 160 + static_cast<usize>(shell.x)) * 4];
    CHECK(shell_px[0] == 0);
    CHECK(shell_px[1] == 255);
    CHECK(shell_px[2] == 0);
    CHECK(shell_px[3] == 255);
}

TEST_CASE("Сцена: метки клипов — метки сцены в их мигах") {
    Files files;
    project(files);
    Scene s;
    load(s, files, R"(    emit("начало")
    actor("ёжик"):play("walk")
    actor("ёжик"):loop("walk", { speed = 2 })
    wait(0.7))");
    record(s);
    std::vector<Mark> marks;
    REQUIRE(s.marks(marks));
    REQUIRE(marks.size() == 5);
    CHECK(marks[0].name == "начало");
    CHECK(marks[0].actor.empty());
    const f64 times[] = {0, 0.25, 0.625, 0.875, 1.125};
    for (usize i = 0; i < 5; ++i) {
        CAPTURE(i);
        CHECK(marks[i].time == doctest::Approx(times[i]));
        if (i) {
            CHECK(marks[i].name == "шаг");
            CHECK(marks[i].actor == "ёжик");
        }
    }
    // A clip cut by the actor's next one gives no marks past it.
    Scene cut;
    load(cut, files, R"(    spawn(actor("ёжик"):play("walk"))
    wait(0.1)
    actor("ёжик"):play("wave"))");
    record(cut);
    REQUIRE(cut.marks(marks));
    CHECK(marks.empty());
    // A loop's mark at its clip's end: at each round's end, the scene's end too, as a game's screen gives a looped
    // clip's marks frame by frame.
    Scene land;
    load(land, files, "    actor(\"ёжик\"):loop(\"land\")\n    wait(1)");
    record(land);
    REQUIRE(land.marks(marks));
    vector::MotionClip game;
    game.length = 0.5;
    game.marks.push_back({false, "земля", 0.5, 0});
    std::vector<f64> passed;
    f64 counted = -1;
    for (int k = 0; k <= 60; ++k) {
        const f64 played = k / 60.0;
        passed.insert(passed.end(), vector::motion_marks_passed(game, counted, played, true).size(), played);
        counted = played;
    }
    REQUIRE(passed.size() == 2);
    REQUIRE(marks.size() == passed.size());
    for (usize i = 0; i < marks.size(); ++i) {
        CAPTURE(i);
        CHECK(marks[i].name == "земля");
        CHECK(marks[i].time == doctest::Approx(passed[i]));
    }
    // Before recording there are none.
    Scene fresh;
    load(fresh, files, "    wait(1)");
    REQUIRE(fresh.marks(marks));
    CHECK(marks.empty());
}

namespace {

// dst over by src, straight RGBA.
void over(assets::CookedTexture& dst, const assets::CookedTexture& src) {
    REQUIRE(dst.rgba8.size() == src.rgba8.size());
    for (usize i = 0; i < dst.rgba8.size(); i += 4) {
        const f64 sa = src.rgba8[i + 3] / 255.0, da = dst.rgba8[i + 3] / 255.0, oa = sa + da * (1 - sa);
        for (int k = 0; k < 3; ++k) {
            const f64 c = oa > 0 ? (src.rgba8[i + k] * sa + dst.rgba8[i + k] * da * (1 - sa)) / oa : 0;
            dst.rgba8[i + k] = static_cast<u8>(std::lround(c));
        }
        dst.rgba8[i + 3] = static_cast<u8>(std::lround(oa * 255));
    }
}

} // namespace

TEST_CASE("Сцена: кадр — фон, картинка и актёры, нарисованные по отдельности") {
    Files files;
    project(files);
    Scene s;
    load(s, files, R"(    actor("ёжик"):loop("walk")
    actor("жук"):play("hop")
    wait(0.4))");
    record(s);
    CHECK(s.recording()->length == doctest::Approx(1.2));
    // The characters drawn alone, as their own tabs draw them, not cut at their canvases (as in a scene: part 6г2).
    struct Alone {
        const char* id;
        const char* file;
        SceneCharacter c;
        PoseScript code;
    };
    Alone alone[2] = {{"ёжик", "персонажи/Ёжик", {}, {}}, {"жук", "персонажи/Жук", {}, {}}};
    for (Alone& a : alone) {
        std::string why;
        REQUIRE(load_character(files.scene(), a.file, a.c, why));
        REQUIRE(a.code.load(a.c.code));
        a.c.rig.uncut();
    }
    for (const f64 t : {0.0, 0.1, 0.35, 0.45, 1.0, 1.2}) {
        CAPTURE(t);
        REQUIRE(s.frame(t));
        const assets::CookedTexture got = s.draw(160, 120);
        assets::CookedTexture want;
        want.width = 160;
        want.height = 120;
        want.rgba8.resize(160 * 120 * 4);
        for (usize i = 0; i < want.rgba8.size(); i += 4) want.rgba8[i] = 0x20, want.rgba8[i + 1] = 0x30, want.rgba8[i + 2] = 0x40, want.rgba8[i + 3] = 255;
        const assets::CookedTexture pic = forest();
        for (u32 y = 0; y < 30; ++y)
            for (u32 x = 0; x < 40; ++x)
                for (int k = 0; k < 4; ++k) want.rgba8[((y + 6) * 160 + x + 8) * 4 + k] = pic.rgba8[(y * 40 + x) * 4 + k];
        const assets::CookedTexture still = want;
        for (usize i = 0; i < 2; ++i) {
            Alone& a = alone[i];
            const ActorMoment m = actor_moment(*s.recording(), a.id, t);
            REQUIRE(m.play);
            Pose pose;
            REQUIRE(a.code.pose(a.c.rig, m.time, m.play->clip, a.c.doc.params, a.c.doc.seed, pose));
            std::string why;
            REQUIRE(a.c.rig.pose(pose, why));
            over(want, a.c.rig.draw(160, 120, actor_matrix(s.stage().items[i + 1])));
        }
        int worst = 0;
        usize moved = 0;
        for (usize i = 0; i < want.rgba8.size(); ++i) {
            worst = std::max(worst, std::abs(int(got.rgba8[i]) - int(want.rgba8[i])));
            moved += got.rgba8[i] != still.rgba8[i];
        }
        // Within 2 of 255 a channel: ThorVG blends each shape over the scene in 8 bits, the test the actor drawn
        // alone over it.
        CHECK(worst <= 2);
        CHECK(moved > 500); // the actors are there
    }
}

TEST_CASE("Сцена: файл сцены и новая сцена") {
    SceneDoc d;
    d.script = "Вступление.luau";
    d.seed = 7;
    d.params = {{"speed", 1.5}};
    d.fps = 30;
    d.ranged = true;
    d.from = 0.5;
    d.to = 4;
    d.lengths.waits["встреча"] = 0.6;
    d.lengths.durations["оборот"] = 0.8;
    const std::string text = write_scene_doc(d);
    SceneDoc back;
    std::string why;
    REQUIRE(read_scene_doc(text, back, why));
    CHECK(back == d);
    CHECK(write_scene_doc(back) == text);
    SceneDoc plain;
    plain.script = "Сцена.luau";
    const std::string bare = write_scene_doc(plain);
    CHECK_FALSE(has(bare, "range"));
    CHECK_FALSE(has(bare, "waits"));
    CHECK_FALSE(has(bare, "durations"));
    REQUIRE(read_scene_doc(bare, back, why));
    CHECK(back == plain);
    const char* bad[][2] = {
        {"[]", "не объект"},
        {"{\"fps\": 0}", "fps"},
        {"{\"fps\": 300}", "fps"},
        {"{\"seed\": -1}", "seed"},
        {"{\"script\": 1}", "script"},
        {"{\"params\": {\"a\": \"b\"}}", "params.a"},
        {"{\"range\": [2, 1]}", "range"},
        {"{\"waits\": {\"a\": -1}}", "waits.a"},
        {"{\"durations\": []}", "durations"},
        {"{", "это не JSON"},
    };
    for (const auto& [json, says] : bad) {
        CAPTURE(json);
        SceneDoc keep = d;
        CHECK_FALSE(read_scene_doc(json, keep, why));
        CHECK(has(why, says));
        CHECK(keep == d);
    }
    // A new scene: 1920 × 1080, its background, a second long.
    Files files;
    Scene s;
    REQUIRE(s.load(new_scene_code("Сцена"), {}, files.scene()));
    CHECK(s.stage().width() == 1920);
    CHECK(s.stage().background == "#203040");
    CHECK(s.stage().items.empty());
    record(s);
    CHECK(s.recording()->length == doctest::Approx(1));
    CHECK(s.frame(0.5));
    const assets::CookedTexture px = s.draw(192, 108);
    REQUIRE(px.width == 192);
    CHECK(px.rgba8[0] == 0x20);
    CHECK(px.rgba8[1] == 0x30);
    CHECK(px.rgba8[2] == 0x40);
    CHECK(has(new_scene_code("Встреча"), "-- «Встреча»"));
}

TEST_CASE("Сцена: ids актёра под его именем") {
    const std::string svg = R"svg(<?xml version="1.0"?>
<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="10" height="10" viewBox="0 0 10 10">
  <defs><linearGradient id="g"><stop offset="0" stop-color="#fff"/></linearGradient></defs>
  <!-- id="c" url(#c) -->
  <rect id="a" width="4" height="4" fill="url(#g)" style="stroke: url('#g')"/>
  <use href="#a" xlink:href="#a" x="5"/>
  <g id='b' data-id="x"><circle cx="5" cy="5" r="1"/></g>
</svg>
)svg";
    const std::string out = prefix_svg_ids(svg, "ёжик");
    for (const char* want : {"id=\"ёжик/g\"", "fill=\"url(#ёжик/g)\"", "stroke: url('#ёжик/g')", "href=\"#ёжик/a\"", "xlink:href=\"#ёжик/a\"",
                             "id='ёжик/b'", "data-id=\"x\"", "<!-- id=\"c\" url(#c) -->", "viewBox=\"0 0 10 10\">\n<g id=\"ёжик\">", "</g>\n</svg>"})
        CHECK_MESSAGE(has(out, want), want);
    Rig rig;
    std::string why;
    REQUIRE_MESSAGE(rig.load(out, why), why);
    CHECK(rig.part("ёжик") >= 0);
    CHECK(rig.part("ёжик/a") >= 0);
    CHECK(rig.part("ёжик/b") >= 0);
    CHECK(rig.part("a") < 0);
    CHECK(rig.parts()[static_cast<usize>(rig.part("ёжик/a"))].parent == rig.part("ёжик"));

    Pose p;
    p.parts["head"].x = 1;
    p.parts["head"].mask = "eyes";
    p.parts["arm"].carry_by = "body";
    p.morphs["mouth"] = {"mouth_o", 0.5};
    p.order.push_back({"nose", OrderStep::Kind::Above, "head"});
    p.copies.push_back({"искра[1]", "искра", 1});
    p.drawing = "flat";
    p.swap = true;
    p.swap_from = "angry";
    p.swap_k = 0.25;
    const Pose q = prefix_pose(p, "ёжик");
    CHECK(q.parts.size() == 2);
    CHECK(q.parts.at("ёжик/head").x == 1);
    CHECK(q.parts.at("ёжик/head").mask == "ёжик/eyes");
    CHECK(q.parts.at("ёжик/head").carry_by.empty());
    CHECK(q.parts.at("ёжик/arm").carry_by == "ёжик/body");
    CHECK(q.morphs.at("ёжик/mouth").target == "ёжик/mouth_o");
    CHECK(q.morphs.at("ёжик/mouth").k == 0.5);
    CHECK(q.order[0] == OrderStep{"ёжик/nose", OrderStep::Kind::Above, "ёжик/head"});
    CHECK(q.copies[0] == PartCopy{"ёжик/искра[1]", "ёжик/искра", 1});
    CHECK(q.drawing == "flat");
    CHECK(q.swap);
    CHECK(q.swap_from == "angry");
    CHECK(q.swap_k == 0.25);
}

TEST_CASE("Сцена: код сцены — клип «scene» вкладки") {
    Files files;
    project(files);
    Scene s;
    load(s, files, R"(    at(0.5, actor("ёжик"):play("walk"))
    actor("жук"):play("hop")
    actor("жук"):loop("hop")
    part("ёжик"):to({ x = 10 }, 0.5)
    wait(0.3))");
    record(s);
    PoseScript& code = s.script();
    // Bound to its stage, the scene is one clip "scene": recorded as Scene::record records it.
    REQUIRE(code.clips() == std::vector<std::string>{"scene"});
    const Recording* rec = code.recording(s.rig(), "scene", {}, 1);
    INFO(code.error());
    REQUIRE(rec);
    CHECK(rec->length == doctest::Approx(s.recording()->length));
    REQUIRE(rec->plays.size() == s.recording()->plays.size());
    for (usize i = 0; i < rec->plays.size(); ++i) {
        CAPTURE(i);
        CHECK(rec->plays[i].actor == s.recording()->plays[i].actor);
        CHECK(rec->plays[i].start == doctest::Approx(s.recording()->plays[i].start));
        CHECK(rec->plays[i].length == doctest::Approx(s.recording()->plays[i].length));
    }
    // Its lengths are the doc's "scene" lengths; another clip is no clip of a scene.
    CHECK_FALSE(code.recording(s.rig(), "walk", {}, 1));
    CHECK(has(code.error(), "клипа «walk»"));
    // The script's pose of it, put on the stage by pose(), draws the frame frame() draws.
    for (const f64 t : {0.0, 0.6, 0.9, 1.4}) {
        CAPTURE(t);
        Pose own;
        REQUIRE(code.pose(s.rig(), t, "scene", {}, 1, own));
        REQUIRE(s.pose(t, rec, own));
        const assets::CookedTexture a = s.draw(160, 120);
        REQUIRE(s.frame(t));
        const assets::CookedTexture b = s.draw(160, 120);
        CHECK(a.rgba8 == b.rgba8);
    }
    // Without a recording the actors are at rest: the stage as it stands.
    REQUIRE(s.pose(0.9, nullptr, {}));
    const assets::CookedTexture rest = s.draw(160, 120);
    Scene still;
    load(still, files, "    wait(1)");
    REQUIRE(still.frame(0.9));
    CHECK(still.draw(160, 120).rgba8 == rest.rgba8);
    // The actors' marks alone, by the plays of the recording given.
    std::vector<Mark> theirs, all;
    REQUIRE(s.actor_marks(*rec, theirs));
    REQUIRE(s.marks(all));
    CHECK(all.size() == theirs.size() + rec->marks.size());
    REQUIRE_FALSE(theirs.empty());
    for (const Mark& m : theirs) CHECK(m.actor == "ёжик");
    CHECK(theirs.front().time == doctest::Approx(0.75));
    // A script not bound to a stage is no clip: a scene's code read on its own.
    PoseScript alone;
    REQUIRE(alone.load(scene_code(kStage, "    wait(1)")));
    CHECK(alone.clips().empty());
    Recording none;
    CHECK_FALSE(alone.record(&s.rig(), "scene", {}, 1, {}, none));
    CHECK(has(alone.error(), "это сцена"));
    // A character's code takes no stage.
    PoseScript hedgehog;
    REQUIRE(hedgehog.load(kHedgehog));
    const std::vector<std::string> clips = hedgehog.clips();
    hedgehog.set_scene(&s.stage(), nullptr);
    CHECK(hedgehog.clips() == clips);
}

TEST_CASE("Сцена: начало клипа актёра тянется, как у блока") {
    Files files;
    project(files);
    Scene s;
    const std::string scene = R"(    at(0.5, actor("ёжик"):play("walk"))
    delay(0.25, actor("жук"):play("hop"))
    actor("жук"):play("hop"))";
    load(s, files, scene);
    record(s);
    const Recording& rec = *s.recording();
    const std::string text = scene_code(kStage, scene);
    const CodeSites& code = s.script().code();
    std::string out, why;
    // at(…): its number is the start.
    const ActorPlay& walk = play_of(rec, "ёжик", ActorPlay::Kind::Play);
    CHECK(walk.start == doctest::Approx(0.5));
    REQUIRE(can_edit_play_start(rec, walk, code, why));
    REQUIRE(edit_play_start(rec, walk, 0.75, code, text, out, why));
    CHECK(has(out, "at(0.75, actor(\"ёжик\"):play(\"walk\"))"));
    // delay(…): how long after, moved by as much.
    const ActorPlay& hop = play_of(rec, "жук", ActorPlay::Kind::Play);
    CHECK(hop.start == doctest::Approx(0.25));
    REQUIRE(edit_play_start(rec, hop, 0.5, code, text, out, why));
    CHECK(has(out, "delay(0.5, actor(\"жук\"):play(\"hop\"))"));
    CHECK_FALSE(edit_play_start(rec, hop, -0.5, code, text, out, why));
    CHECK(has(why, "не меньше 0"));
    // One after another: no start of its own in the code.
    const ActorPlay& next = play_of(rec, "жук", ActorPlay::Kind::Play, 1);
    CHECK_FALSE(can_edit_play_start(rec, next, code, why));
    CHECK(has(why, "своего начала в коде у него нет"));
    CHECK_FALSE(edit_play_start(rec, next, 2, code, text, out, why));
}

TEST_CASE("Сцена: протяжка мышью пишет строку в scene") {
    Files files;
    project(files);
    Scene s;
    load(s, files, R"(    actor("ёжик"):play("walk")
    wait(0.5))");
    record(s);
    const PoseScript& code = s.script();
    // The scene's function is its one clip for the code's edits; a top-level scene without stage is not: no scene, it
    // does not load, and its code has no clip «scene».
    REQUIRE(code.code().clip("scene"));
    PoseScript character;
    CHECK_FALSE(character.load("return {\n  scene = function(p)\n    wait(1)\n  end,\n  clips = {},\n}\n"));
    CHECK(has(character.error(), "у сцены нужен stage"));
    CHECK_FALSE(character.code().clip("scene"));
    // The actor and an actor's part dragged at 0: a first line of scene, as a character's clip has it.
    const std::string text = scene_code(kStage, "    actor(\"ёжик\"):play(\"walk\")\n    wait(0.5)");
    std::string out, why;
    REQUIRE(edit_part_move(*s.recording(), "ёжик", 0, 1.0 / 60, 4, 0, code.code(), text, out, why));
    INFO(why);
    const std::string head = "  scene = function(p)\n";
    CHECK(out == scene_code(kStage, "    at(0, part(\"ёжик\"):to({ x = 4 }, 0))\n    actor(\"ёжик\"):play(\"walk\")\n    wait(0.5)"));
    REQUIRE(edit_part_move(*s.recording(), "ёжик/head", 0, 1.0 / 60, 0, -3, code.code(), text, out, why));
    CHECK(has(out, head + "    at(0, part(\"ёжик/head\"):to({ y = -3 }, 0))\n    actor(\"ёжик\"):play(\"walk\")"));
    // The line it writes is the scene's own: recorded again, the actor stands 4 to the right at 0.
    Scene moved;
    REQUIRE(edit_part_move(*s.recording(), "ёжик", 0, 1.0 / 60, 4, 0, code.code(), text, out, why));
    REQUIRE(moved.load(out, {}, files.scene()));
    REQUIRE(moved.record());
    const Block* x = nullptr;
    for (const Block& b : moved.recording()->blocks)
        if (moved.recording()->parts[b.part] == "ёжик" && b.prop == Prop::X) x = &b;
    REQUIRE(x);
    CHECK(x->start == 0);
    CHECK(x->value.v == doctest::Approx(4));
}

// ---- part 6б: the camera and the transitions ----

namespace {

// The stage of 6а with a hidden circle for a transition's mask: lines 3–9, the scene's from 12.
const char* kStageB = R"(    canvas(160, 120)
    background("#203040")
    picture("лес", "картинки/лес.png", { x = 8, y = 6 })
    actor("ёжик", "персонажи/Ёжик", { x = 40, y = 100, pivot = { 32, 60 } })
    actor("жук", "персонажи/Жук", { x = 120, y = 90, scale = 0.5, rot = 15, pivot = { 24, 16 } })
    path("дорога", { d = "M 40 100 L 120 100", fill = "none", stroke = "#ffffff", hidden = true })
    circle("круг", { x = 80, y = 60, r = 30, fill = "#ffffff", hidden = true }))";

// The camera at t of a recorded scene.
SceneCamera camera_at(Scene& s, f64 t) {
    const bool ok = s.frame(t);
    INFO(s.error());
    REQUIRE(ok);
    return s.camera();
}

Rgba8 pixel_at(const assets::CookedTexture& t, u32 x, u32 y) {
    const u8* p = &t.rgba8[(static_cast<usize>(y) * t.width + x) * 4];
    return {p[0], p[1], p[2], p[3]};
}

// Whether a pixel is within 2 of a colour in each channel.
bool near(Rgba8 got, Rgba8 want) {
    return std::abs(got.r - want.r) <= 2 && std::abs(got.g - want.g) <= 2 && std::abs(got.b - want.b) <= 2 &&
           std::abs(got.a - want.a) <= 2;
}

// a share k of the way from b to a: what a over b at opacity k gives on opaque pixels.
Rgba8 mix(Rgba8 a, Rgba8 b, f64 k) {
    auto c = [&](u8 x, u8 y) { return static_cast<u8>(std::lround(x * k + y * (1 - k))); };
    return {c(a.r, b.r), c(a.g, b.g), c(a.b, b.b), 255};
}

constexpr Rgba8 kBack{0x20, 0x30, 0x40, 255};

} // namespace

TEST_CASE("Сцена: матрица кадра через камеру") {
    // The middle of the canvas moved by x, y stands in the middle of the frame; a step right on the scene is zoom
    // steps in the frame, turned back by rot (the camera turned clockwise, the scene turns the other way).
    const vector::Point c{90, 55};
    struct Case {
        SceneCamera cam;
        vector::Point right, down; // where c + (1, 0) and c + (0, 1) stand in the frame
    };
    const Case cases[] = {
        {{10, -5, 1, 0}, {81, 60}, {80, 61}},
        {{10, -5, 2, 0}, {82, 60}, {80, 62}},
        {{10, -5, 1, 90}, {80, 59}, {81, 60}},
        {{10, -5, 2, 90}, {80, 58}, {82, 60}},
        {{10, -5, 2, -90}, {80, 62}, {78, 60}},
    };
    for (const Case& k : cases) {
        CAPTURE(k.cam.zoom);
        CAPTURE(k.cam.rot);
        const Affine m = camera_matrix(k.cam, 160, 120);
        const vector::Point mid = m.apply(c), r = m.apply({c.x + 1, c.y}), d = m.apply({c.x, c.y + 1});
        CHECK(mid.x == doctest::Approx(80));
        CHECK(mid.y == doctest::Approx(60));
        CHECK(r.x == doctest::Approx(k.right.x));
        CHECK(r.y == doctest::Approx(k.right.y));
        CHECK(d.x == doctest::Approx(k.down.x));
        CHECK(d.y == doctest::Approx(k.down.y));
    }
    // 45°: a step right goes up and right by √½ each.
    const vector::Point q = camera_matrix({0, 0, 1, 45}, 160, 120).apply({81, 60});
    CHECK(q.x == doctest::Approx(80 + std::sqrt(0.5)));
    CHECK(q.y == doctest::Approx(60 - std::sqrt(0.5)));
    CHECK(camera_matrix({}, 160, 120) == Affine{});
    // The player's frame at a view's side: the stage's shape, its longer side the side, wide or tall.
    u32 fw = 0, fh = 0;
    frame_size(160, 90, 400, fw, fh);
    CHECK((fw == 400 && fh == 225));
    frame_size(90, 160, 400, fw, fh);
    CHECK((fw == 225 && fh == 400));
    frame_size(100, 100, 300, fw, fh);
    CHECK((fw == 300 && fh == 300));
    frame_size(1, 2000, 300, fw, fh);
    CHECK((fw == 1 && fh == 300));
    CHECK(camera_part("camera"));
    CHECK(camera_part("camera/in-new"));
    CHECK(camera_part("camera/out-old"));
    CHECK_FALSE(camera_part("cameras"));
    CHECK_FALSE(camera_part("ёжик"));

    // The camera of a recording: its x, y, zoom and rot, the frame the scene through it.
    Files files;
    project(files);
    Scene s;
    load(s, files, R"(    camera():to({ x = 10, y = -5, zoom = 2, rot = 90 }, 1)
    camera().zoom(1, 1))");
    record(s);
    CHECK(s.recording()->length == doctest::Approx(2));
    CHECK(camera_at(s, 0) == SceneCamera{});
    CHECK(camera_at(s, 1) == SceneCamera{10, -5, 2, 90});
    const SceneCamera end = camera_at(s, 2);
    CHECK(end.x == 10);
    CHECK(end.zoom == 1);
    CHECK(s.camera_matrix() == camera_matrix(end, 160, 120));
    CHECK(s.camera().x == 10);
    // The frame through it is the scene drawn by its matrix; the stage's own drawing is the whole scene still.
    REQUIRE(s.frame(1));
    const assets::CookedTexture through = s.draw_frame(160, 120, Affine{});
    const assets::CookedTexture want = s.draw(160, 120, s.camera_matrix());
    CHECK(through.width == 160);
    CHECK(through.rgba8 == want.rgba8);
    CHECK(s.draw(160, 120, Affine{}).rgba8 != want.rgba8);
    // Moved 20 right: the frame's 10, 10 is the scene's 30, 10, the picture's 22, 4.
    load(s, files, "    camera().x(20)");
    record(s);
    REQUIRE(s.frame(0));
    const assets::CookedTexture moved = s.draw_frame(160, 120, Affine{});
    const assets::CookedTexture pic = forest();
    CHECK(near(pixel_at(moved, 10, 10), pixel_at(pic, 22, 4)));
    CHECK(near(pixel_at(moved, 130, 10), kBack)); // the scene's 150, 10
    // Fitted into a drawing twice the size: the same frame, each pixel twice.
    const assets::CookedTexture big = s.draw_frame(320, 240);
    CHECK(near(pixel_at(big, 21, 21), pixel_at(pic, 22, 4)));
}

TEST_CASE("Сцена: картинка, повёрнутая камерой, на своём месте") {
    // ThorVG drew a turned picture half a pixel and half a texel up and left of its place (third_party/thorvg samples it
    // at the pixels' centres): turned by 180°, the frame is the unturned one upside down, pixel for pixel, inside the
    // picture «лес», at its own size and zoomed, the camera on its middle (28, 21).
    Files files;
    project(files);
    for (const char* zoom : {"1", "3"}) {
        CAPTURE(zoom);
        Scene flat, turned;
        const std::string on = std::string("    camera().x(-52, 0)\n    camera().y(-39, 0)\n    camera().zoom(") + zoom + ", 0)\n";
        load(flat, files, on + "    wait(1)");
        load(turned, files, on + "    camera().rot(180, 0)\n    wait(1)");
        record(flat);
        record(turned);
        REQUIRE(flat.frame(0.5));
        REQUIRE(turned.frame(0.5));
        const assets::CookedTexture a = flat.draw_frame(160, 120, Affine{}), b = turned.draw_frame(160, 120, Affine{});
        const f64 z = std::atof(zoom);
        usize inside = 0, off = 0;
        for (u32 y = 0; y < 120; ++y)
            for (u32 x = 0; x < 160; ++x) {
                // The pixel's centre on the stage: inside the picture (40 × 30 at 8, 6), a texel from its edges.
                const f64 sx = (x + 0.5 - 80) / z + 28, sy = (y + 0.5 - 60) / z + 21;
                if (sx < 9 || sx > 47 || sy < 7 || sy > 35) continue;
                ++inside;
                off += !near(pixel_at(b, 159 - x, 119 - y), pixel_at(a, x, y));
            }
        CHECK(inside > 1000);
        CHECK(off == 0);

        // A shape masked by the picture: the tab masks by an item's box, so the masked shape is the picture's box both
        // ways.
        const std::string stage = std::string(kStage) + "\n    rect(\"пятно\", { x = 0, y = 0, w = 160, h = 120, fill = \"#ff00ff\" })";
        Scene flat_mask, turned_mask;
        load(flat_mask, files, on + "    part(\"пятно\"):mask(\"лес\")\n    wait(1)", stage);
        load(turned_mask, files, on + "    camera().rot(180, 0)\n    part(\"пятно\"):mask(\"лес\")\n    wait(1)", stage);
        record(flat_mask);
        record(turned_mask);
        REQUIRE(flat_mask.frame(0.5));
        REQUIRE(turned_mask.frame(0.5));
        const assets::CookedTexture c = flat_mask.draw_frame(160, 120, Affine{}), d = turned_mask.draw_frame(160, 120, Affine{});
        auto spot = [](Rgba8 p) { return p.r == 255 && p.g == 0 && p.b == 255; };
        usize spots = 0, apart = 0;
        for (u32 y = 0; y < 120; ++y)
            for (u32 x = 0; x < 160; ++x) {
                spots += spot(pixel_at(c, x, y));
                apart += spot(pixel_at(d, 159 - x, 119 - y)) != spot(pixel_at(c, x, y));
            }
        CHECK(spots == static_cast<usize>(std::lround(40 * z) * std::lround(30 * z)));
        CHECK(apart == 0);
    }
}

TEST_CASE("ThorVG: маска картинкой, повёрнутой и приближенной, — тексель в центре пикселя") {
    // A mask is drawn on a surface of one channel by ThorVG's own raster without smoothing (third_party/thorvg): a
    // picture in a mask, turned (not by a right angle, which ThorVG draws as only scaled), takes the texel under each
    // pixel's centre. A sieve of 4 × 3 texels, every other one clear, each 10 × 10 pixels at its own size and 30 × 30
    // zoomed: half a texel off would move its cells by 5 and 15 pixels.
    tvg::Initializer::init(0);
    std::vector<u32> sieve(12);
    for (u32 k = 0; k < 12; ++k) sieve[k] = (k % 4 + k / 4) % 2 ? 0u : 0xffffffffu;
    for (const f64 z : {1.0, 3.0}) {
        CAPTURE(z);
        // The texel's place → the frame's: zoomed, turned by 30° round the sieve's middle, that on the frame's.
        const f64 s = 10 * z, c = s * std::cos(std::numbers::pi / 6), n = s * std::sin(std::numbers::pi / 6);
        std::vector<u32> buffer(160 * 120, 0);
        {
            tvg::SwCanvas* canvas = tvg::SwCanvas::gen();
            canvas->target(buffer.data(), 160, 160, 120, tvg::ColorSpace::ABGR8888S);
            tvg::Shape* spot = tvg::Shape::gen();
            spot->appendRect(0, 0, 160, 120);
            spot->fill(255, 0, 255);
            tvg::Picture* pic = tvg::Picture::gen();
            REQUIRE(pic->load(sieve.data(), 4, 3, tvg::ColorSpace::ARGB8888, true) == tvg::Result::Success);
            pic->transform(tvg::Matrix{static_cast<f32>(c), static_cast<f32>(-n), static_cast<f32>(80 - 2 * c + 1.5 * n),
                                       static_cast<f32>(n), static_cast<f32>(c), static_cast<f32>(60 - 2 * n - 1.5 * c), 0, 0, 1});
            spot->mask(pic, tvg::MaskMethod::Alpha);
            canvas->add(spot);
            canvas->update();
            canvas->draw(true);
            canvas->sync();
            delete canvas;
        }
        usize seen = 0, off = 0;
        for (u32 y = 0; y < 120; ++y)
            for (u32 x = 0; x < 160; ++x) {
                // The pixel's centre in the sieve's texels; those a hair from a texel's edge are left out, and those
                // a pixel and a half from the sieve's (where the edge of the turned box falls is the scan's, not the
                // texel's).
                const f64 dx = x + 0.5 - 80, dy = y + 0.5 - 60, edge = 1.5 / s;
                const f64 u = (c * dx + n * dy) / (s * s) + 2, v = (-n * dx + c * dy) / (s * s) + 1.5;
                if (u < edge || v < edge || u > 4 - edge || v > 3 - edge) continue;
                if (std::fabs(u - std::round(u)) < 0.05 || std::fabs(v - std::round(v)) < 0.05) continue;
                const bool hole = (static_cast<int>(u) + static_cast<int>(v)) % 2 == 0;
                ++seen;
                off += hole != ((buffer[y * 160 + x] & 0xffffffu) == 0xff00ffu && buffer[y * 160 + x] >> 24 == 255); // ABGR: r, g, b, a
            }
        CHECK(seen > static_cast<usize>(800 * z * z));
        CHECK(off == 0);
    }
    tvg::Initializer::term();
}

TEST_CASE("Сцена: камера к цели и по пути") {
    Files files;
    project(files);
    Scene s;
    // To an actor where its pivot stands then, to a shape's middle, to a point of the scene.
    load(s, files, R"(    part("ёжик").x(20)
    camera():center_on("ёжик", 1)
    camera():center_on("круг", 1, "linear")
    wait(0.5)
    camera():center_on({ x = 100, y = 30 }, 0))", kStageB);
    record(s);
    CHECK(camera_at(s, 0) == SceneCamera{});
    SceneCamera c = camera_at(s, 1);
    CHECK(c.x == doctest::Approx(-20)); // 60 - 80
    CHECK(c.y == doctest::Approx(40));  // 100 - 60
    c = camera_at(s, 1.5);
    CHECK(c.x == doctest::Approx(-10));
    CHECK(c.y == doctest::Approx(20));
    c = camera_at(s, 2);
    CHECK(c.x == doctest::Approx(0));
    CHECK(c.y == doctest::Approx(0));
    c = camera_at(s, 2.5);
    CHECK(c.x == doctest::Approx(20));
    CHECK(c.y == doctest::Approx(-30));
    CHECK(s.recording()->length == doctest::Approx(2.5));
    // The frame's middle along the path: the path's start, middle and end.
    load(s, files, "    camera():along(\"дорога\", 2, \"linear\")", kStageB);
    record(s);
    c = camera_at(s, 0);
    CHECK(c.x == doctest::Approx(-40));
    CHECK(c.y == doctest::Approx(40));
    c = camera_at(s, 1);
    CHECK(c.x == doctest::Approx(0));
    CHECK(c.y == doctest::Approx(40));
    c = camera_at(s, 2);
    CHECK(c.x == doctest::Approx(40));
    CHECK(c.y == doctest::Approx(40));
    // part("camera") is the same camera; modules and functions move it as a part.
    load(s, files, R"(    local cam = part("camera")
    cam:to({ x = 5 }, 0)
    all(function() camera().y(7, 0) end))", kStageB);
    record(s);
    c = camera_at(s, 0);
    CHECK(c.x == 5);
    CHECK(c.y == 7);
}

TEST_CASE("Сцена: камера следит за актёром") {
    Files files;
    project(files);
    Scene s;
    // Without a length the frame's middle is on the hedgehog's pivot at once: 40 + 50 t - 80, 100 - 60.
    load(s, files, R"(    all(part("ёжик").x(100, 2, "linear"), camera():follow("ёжик"))
    wait(1))");
    record(s);
    CHECK(s.recording()->length == doctest::Approx(3));
    for (const f64 t : {0.0, 0.5, 1.0, 2.0, 3.0}) {
        CAPTURE(t);
        const SceneCamera c = camera_at(s, t);
        CHECK(c.x == doctest::Approx(40 + 50 * std::min(t, 2.0) - 80));
        CHECK(c.y == doctest::Approx(40));
        CHECK(c.zoom == 1);
    }
    // With one: from where it was to on it, k of the way: middle(t) = target(t) - (1 - k) (target(t0) - middle(t0)).
    load(s, files, R"(    camera().x(10)
    all(part("ёжик").x(100, 2, "linear"), camera():follow("ёжик", 1, "linear")))");
    record(s);
    for (const f64 t : {0.0, 0.25, 0.5, 1.0, 1.5, 2.0}) {
        CAPTURE(t);
        const f64 k = std::min(t, 1.0);
        const SceneCamera c = camera_at(s, t);
        CHECK(c.x == doctest::Approx((40 + 50 * t - 80) - (1 - k) * (-40 - 10)));
        CHECK(c.y == doctest::Approx(40 - (1 - k) * 40));
    }
    // follow waits its length.
    CHECK(s.recording()->length == doctest::Approx(2));
    // follow(nil) ends it: the camera stands where it left it, the next move goes from there.
    load(s, files, R"(    spawn(part("ёжик").x(100, 2, "linear"))
    camera():follow("ёжик")
    wait(1)
    camera():follow(nil)
    wait(0.5)
    camera().x(0, 1, "linear"))");
    record(s);
    CHECK(camera_at(s, 0.5).x == doctest::Approx(-15));
    CHECK(camera_at(s, 1).x == doctest::Approx(10));
    CHECK(camera_at(s, 1.25).x == doctest::Approx(10));
    CHECK(camera_at(s, 1.5).x == doctest::Approx(10));
    CHECK(camera_at(s, 2).x == doctest::Approx(5));
    CHECK(camera_at(s, 2).y == doctest::Approx(40));
    CHECK(camera_at(s, 2.5).x == doctest::Approx(0));
    // x alone ends it too, y staying where it was; zoom and rot do not.
    load(s, files, R"(    spawn(all(part("ёжик").x(100, 2, "linear"), part("ёжик").y(-40, 2, "linear")))
    camera():follow("ёжик")
    wait(0.5)
    camera().zoom(2, 0)
    camera().rot(10, 0)
    wait(0.5)
    camera().x(-30, 0))");
    record(s);
    SceneCamera c = camera_at(s, 0.75);
    CHECK(c.x == doctest::Approx(40 + 37.5 - 80));
    CHECK(c.y == doctest::Approx(100 - 15 - 60));
    CHECK(c.zoom == 2);
    CHECK(c.rot == 10);
    c = camera_at(s, 1);
    CHECK(c.x == doctest::Approx(-30));
    CHECK(c.y == doctest::Approx(100 - 20 - 60));
    c = camera_at(s, 2);
    CHECK(c.x == doctest::Approx(-30));
    CHECK(c.y == doctest::Approx(100 - 20 - 60));
    // A new place of the camera ends it: center_on goes from where the follow had it.
    load(s, files, R"(    spawn(part("ёжик").x(100, 2, "linear"))
    camera():follow("ёжик")
    wait(1)
    camera():center_on({ x = 80, y = 60 }, 1, "linear"))");
    record(s);
    CHECK(camera_at(s, 1).x == doctest::Approx(10));
    CHECK(camera_at(s, 1.5).x == doctest::Approx(5));
    CHECK(camera_at(s, 2).x == doctest::Approx(0));
    CHECK(camera_at(s, 2).y == doctest::Approx(0));
}

TEST_CASE("Сцена: камера и переходы — ошибки со строкой") {
    Files files;
    project(files);
    struct Case {
        const char* scene;
        const char* says;
    };
    const Case cases[] = {
        {"    camera().tint(\"#ff0000\", 0.2)", "у камеры нет свойства «tint»"},
        {"    camera():mask(\"дорога\")", "у камеры нет свойства «mask»"},
        {"    camera().zoom(0, 1)", "zoom — число больше 0"},
        {"    camera():to({ zoom = -1 }, 1)", "zoom — число больше 0"},
        {"    camera(1)", "camera() — без аргументов"},
        {"    camera():center_on(\"ёжик/head\", 1)", "часть актёра, а здесь нужна часть сцены"},
        {"    camera():follow(\"ёжик/head\")", "часть актёра, а здесь нужна часть сцены"},
        {"    camera():follow(\"camera\")", "камера не идёт к самой себе"},
        {"    camera():follow(\"нет\")", "нет"},
        {"    camera():follow(nil, 1)", "follow(nil) — без длины"},
        {"    camera():center_on({ x = 1 }, 1)", "у точки и x, и y"},
        {"    part(\"camera/in-new\").x(1)", "нет части"},
        {"    wait(1) transition(\"растворение\", 1)", "transition — в начале scene"},
        {"    transition(\"растворение\", 1) transition(\"сдвиг\", 1)", "один раз"},
        {"    all(function() transition(\"растворение\", 1) end)", "transition — в начале scene"},
        {"    all(function() leave(\"растворение\", 1) end)", "leave — в самой scene"},
        {"    leave(\"растворение\", 1) wait(1)", "после leave сцена кончается"},
        {"    leave(\"растворение\", 1) log(1)", "после leave сцена кончается"},
        {"    leave(\"растворение\", 1) leave(\"растворение\", 1)", "после leave сцена кончается"},
        {"    transition(\"вихрь\", 1)", "нет вида «вихрь»"},
        {"    transition(\"сдвиг\", 1, { from = \"вбок\" })", "from — «право», «лево», «верх» или «низ»"},
        {"    transition(\"растворение\", 1, { from = \"лево\" })", "from — только у «сдвиг»"},
        {"    transition(\"сдвиг\", 1, { edge = \"лево\" })", "поля «edge» нет"},
        {"    transition(function(old, new) new.tint(\"#fff\", 1) end)", "у кадра перехода нет «tint»"},
        {"    transition(function(old, new) end, 1)", "длина перехода — длина записи функции"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.scene);
        Scene s;
        load(s, files, c.scene);
        CHECK_FALSE(s.record());
        INFO(s.error());
        CHECK(has(s.error(), c.says));
        CHECK(s.error_line() == 11);
    }
    // The name camera is the camera's: a shape or an actor with it, by the line of its call.
    for (const char* stage : {"    canvas(160, 120)\n    rect(\"camera\", { w = 4, h = 4 })",
                              "    canvas(160, 120)\n    actor(\"camera\", \"персонажи/Ёжик\", { x = 1, y = 1 })",
                              "    canvas(160, 120)\n    picture(\"camera\", \"картинки/лес.png\")"}) {
        CAPTURE(stage);
        Scene s;
        CHECK_FALSE(s.load(scene_code(stage, "    wait(1)"), {}, files.scene()));
        INFO(s.error());
        CHECK(has(s.error(), "имя camera — у камеры сцены"));
        CHECK(s.error_line() == 4);
    }
    // camera in the stage, and outside a scene: a character's clip.
    {
        Scene s;
        CHECK_FALSE(s.load(scene_code("    canvas(160, 120)\n    camera()", "    wait(1)"), {}, files.scene()));
        CHECK(has(s.error(), "camera — в scene = function(p) … end"));
    }
    for (const char* call : {"camera()", "transition(\"растворение\", 1)", "leave(\"растворение\", 1)"}) {
        CAPTURE(call);
        PoseScript code;
        REQUIRE(code.load(std::string("return {\n  clips = { a = function(p)\n    ") + call + "\n  end },\n}\n"));
        Recording rec;
        CHECK_FALSE(code.record(nullptr, "a", {}, 1, {}, rec));
        INFO(code.error());
        CHECK(has(code.error(), "только в сцене"));
        CHECK(code.error_line() == 3);
    }
}

TEST_CASE("Сцена: переходы входа и ухода — кадры игрока") {
    Files files;
    project(files);
    const Rgba8 grey = kOutsideScene;
    Scene s;
    auto frame = [&](f64 t) {
        const bool ok = s.frame(t);
        INFO(s.error());
        REQUIRE(ok);
        return s.draw_frame(160, 120, Affine{});
    };
    // «растворение»: the scene shows over what was, linear.
    load(s, files, R"(    transition("растворение", 1)
    wait(2))");
    record(s);
    CHECK(s.recording()->length == doctest::Approx(2)); // transition does not wait
    REQUIRE(s.recording()->transitions.size() == 1);
    const SceneTransition& tr = s.recording()->transitions[0];
    CHECK_FALSE(tr.leave);
    CHECK(tr.kind == "растворение");
    CHECK(tr.start == 0);
    CHECK(tr.length == doctest::Approx(1));
    CHECK(near(pixel_at(frame(0), 150, 10), grey));
    CHECK(near(pixel_at(frame(0.25), 150, 10), mix(kBack, grey, 0.25)));
    CHECK(near(pixel_at(frame(0.5), 150, 10), mix(kBack, grey, 0.5)));
    CHECK(near(pixel_at(frame(1), 150, 10), kBack));
    CHECK(frame(1.5).rgba8 == s.draw(160, 120, s.camera_matrix()).rgba8);

    // «сдвиг» from the right: the scene comes in from x = 160, half way at 0.5 (in_out_cubic).
    load(s, files, "    transition(\"сдвиг\", 1)\n    wait(1)");
    record(s);
    CHECK(s.recording()->transitions[0].from == "право");
    assets::CookedTexture f = frame(0.5);
    CHECK(near(pixel_at(f, 10, 2), grey));
    CHECK(near(pixel_at(f, 79, 2), grey));
    CHECK(near(pixel_at(f, 85, 2), kBack)); // the scene's 5, 2
    CHECK(near(pixel_at(f, 82 + 10, 10), pixel_at(forest(), 4, 4))); // the scene's 12, 10
    CHECK(near(pixel_at(frame(0), 150, 10), grey));
    // From above: at 0.5 the scene's bottom half shows in the frame's top half.
    load(s, files, "    transition(\"сдвиг\", 1, { from = \"верх\" })\n    wait(1)");
    record(s);
    f = frame(0.5);
    CHECK(near(pixel_at(f, 150, 2), kBack)); // the scene's 150, 62
    CHECK(near(pixel_at(f, 150, 100), grey));
    // From the left and below, linear: a quarter in at 0.25.
    load(s, files, "    transition(\"сдвиг\", 1, { from = \"лево\", ease = \"linear\" })\n    wait(1)");
    record(s);
    f = frame(0.25);
    CHECK(near(pixel_at(f, 38, 60), kBack));   // the scene's 158, 60
    CHECK(near(pixel_at(f, 42, 60), grey));
    load(s, files, "    transition(\"сдвиг\", 1, { from = \"низ\", ease = \"linear\" })\n    wait(1)");
    record(s);
    f = frame(0.25);
    CHECK(near(pixel_at(f, 150, 88), grey));
    CHECK(near(pixel_at(f, 150, 92), kBack)); // the scene's 150, 2

    // «приближение»: from half its size and transparent, round the frame's middle: at 0.5 three quarters, half seen.
    load(s, files, "    transition(\"приближение\", 1)\n    wait(1)");
    record(s);
    f = frame(0.5);
    CHECK(near(pixel_at(f, 2, 2), grey));         // outside the scene at 0.75
    CHECK(near(pixel_at(f, 133, 23), mix(kBack, grey, 0.5))); // the scene's 151, 11
    CHECK(near(pixel_at(f, 18, 60), grey));
    CHECK(near(pixel_at(f, 22, 60), mix(kBack, grey, 0.5))); // the scene's 3, 60

    // A function of its own: the new frame seen only in the circle as the camera sees it, the camera 20 right.
    load(s, files, R"(    camera().x(20)
    transition(function(old, new)
      new:mask("круг")
      old.opacity(0.5)
      wait(1)
    end)
    wait(1))", kStageB);
    record(s);
    CHECK(s.recording()->transitions[0].kind.empty());
    CHECK(s.recording()->transitions[0].length == doctest::Approx(1));
    CHECK(s.recording()->length == doctest::Approx(1));
    f = frame(0.5);
    const assets::CookedTexture scene = s.draw(160, 120, s.camera_matrix());
    // The circle at the scene's 80, 60 is at 60, 60 of the frame: the scene inside it, outside half the grey.
    CHECK(near(pixel_at(f, 60, 60), pixel_at(scene, 60, 60)));
    CHECK(near(pixel_at(f, 85, 60), pixel_at(scene, 85, 60)));
    const Rgba8 half{128, 128, 128, 128};
    CHECK(near(pixel_at(f, 95, 60), half));
    CHECK(near(pixel_at(f, 25, 60), half));
    CHECK(near(pixel_at(f, 60, 25), half));
    CHECK(frame(1).rgba8 == scene.rgba8);
}

TEST_CASE("Сцена: уход — длина сцены и актёры во время него") {
    Files files;
    project(files);
    Scene s;
    load(s, files, R"(    actor("ёжик"):loop("walk")
    wait(1)
    leave("растворение", 1))");
    record(s);
    const Recording& rec = *s.recording();
    CHECK(rec.length == doctest::Approx(2));
    REQUIRE(rec.transitions.size() == 1);
    CHECK(rec.transitions[0].leave);
    CHECK(rec.transitions[0].start == doctest::Approx(1));
    CHECK(rec.transitions[0].length == doctest::Approx(1));
    // The actor's loop goes on while the scene leaves.
    const ActorMoment m = actor_moment(rec, "ёжик", 1.6);
    REQUIRE(m.play);
    CHECK(m.time == doctest::Approx(0.1));
    auto frame = [&](f64 t) {
        REQUIRE(s.frame(t));
        return s.draw_frame(160, 120, Affine{});
    };
    CHECK(near(pixel_at(frame(0.5), 150, 10), kBack));
    CHECK(near(pixel_at(frame(1), 150, 10), kBack));
    CHECK(near(pixel_at(frame(1.5), 150, 10), mix(kOutsideScene, kBack, 0.5)));
    CHECK(near(pixel_at(frame(1.75), 150, 10), mix(kOutsideScene, kBack, 0.75)));
    CHECK(near(pixel_at(frame(2), 150, 10), kOutsideScene));
    CHECK(near(pixel_at(frame(3), 150, 10), kOutsideScene));
    // The walk moves the head during the leave: two moments of it differ under the same fade.
    REQUIRE(s.frame(1.5));
    const assets::CookedTexture a = s.draw(160, 120, s.camera_matrix());
    REQUIRE(s.frame(1.6));
    CHECK(s.draw(160, 120, s.camera_matrix()).rgba8 != a.rgba8);
    // A leave of its own: the game's frame comes in from below over the scene.
    load(s, files, R"(    leave(function(old, new)
      new.y(120)
      new.y(0, 1, "linear")
    end))");
    record(s);
    CHECK(s.recording()->length == doctest::Approx(1));
    const assets::CookedTexture f = frame(0.25);
    CHECK(near(pixel_at(f, 150, 88), kBack));
    CHECK(near(pixel_at(f, 150, 92), kOutsideScene));
    // Its game's frame half shown at its end: after the leave the game's alone, not the scene under it.
    load(s, files, R"(    leave(function(old, new)
      new.opacity(0)
      new.opacity(0.5, 1, "linear")
    end))");
    record(s);
    CHECK(near(pixel_at(frame(0.5), 150, 10), mix(kOutsideScene, kBack, 0.25)));
    CHECK(near(pixel_at(frame(1), 150, 10), kOutsideScene));
    // Entering and leaving at once: the leave wins.
    load(s, files, R"(    transition("растворение", 2)
    leave("сдвиг", 1, { from = "лево" }))");
    record(s);
    CHECK(s.recording()->length == doctest::Approx(1));
    CHECK(near(pixel_at(frame(0.5), 10, 10), kOutsideScene));
}

TEST_CASE("Сцена: камера мышью и полями пишет camera()") {
    Files files;
    project(files);
    struct Case {
        const char* scene; // the scene's lines (from 11)
        f64 t, dx, dy;
        const char* written; // the lines after, "" for a refusal
        const char* says;    // the refusal's words
        f64 seen = -1;       // where the camera stands moved by dx, dy: the frame's moment, or the end of its transition
    };
    const Case cases[] = {
        // Nothing of the camera's own at 0: a line at(0, camera():to(…)).
        {"    wait(1)", 0, 8, 0, "    at(0, camera():to({ x = 8 }, 0))\n    wait(1)", ""},
        {"    wait(1)", 0.5, 8, -2, "    at(0.5, camera():to({ x = 8, y = -2 }, 0))\n    wait(1)", ""},
        // Its transition going: the end of it.
        {"    camera():to({ x = 10, zoom = 2 }, 1)", 0.5, 8, 0, "    camera():to({ x = 18, zoom = 2 }, 1)", "", 1},
        {"    camera().x(10, 1)", 0.5, -4, 0, "    camera().x(6, 1)", "", 1},
        // Its x going, its y into the same table.
        {"    camera():to({ x = 10 }, 1)", 0.5, 0, 5, "    camera():to({ x = 10, y = 5 }, 1)", "", 1},
        // On its path or following: refused with the reason.
        {"    camera():along(\"дорога\", 1)", 0.5, 8, 0, "", "путь правится в коде"},
        {"    camera():follow(\"ёжик\")\n    wait(1)", 0.5, 8, 0, "", "камера следит за «ёжик»: правится в коде"},
        // The follow ended: where it left the camera, a line moves it.
        {"    camera():follow(\"ёжик\")\n    wait(0.5)\n    camera():follow(nil)\n    wait(0.5)", 0.75, 8, 0,
         "    at(0.75, camera():to({ x = -32 }, 0))\n    camera():follow(\"ёжик\")\n    wait(0.5)\n    camera():follow(nil)\n    wait(0.5)", ""},
    };
    for (const Case& c : cases) {
        CAPTURE(c.scene);
        CAPTURE(c.t);
        Scene s;
        load(s, files, c.scene);
        record(s);
        const std::string text = scene_code(kStage, c.scene);
        std::string out, why;
        const bool ok = edit_part_move(*s.recording(), "camera", c.t, 1.0 / 60, c.dx, c.dy, s.script().code(), text, out, why);
        INFO(why);
        if (*c.written) {
            REQUIRE(ok);
            CHECK(out == scene_code(kStage, c.written));
            // Recorded again, the camera stands moved by that in that frame, or at the end of the transition going.
            const f64 seen = c.seen >= 0 ? c.seen : c.t;
            const SceneCamera was = camera_at(s, seen);
            Scene moved;
            load(moved, files, std::string_view(out).substr(std::string_view(out).find("  scene = function(p)\n") + 22,
                                                            std::string_view(out).rfind("\n  end,") - std::string_view(out).find("  scene = function(p)\n") - 22));
            record(moved);
            const SceneCamera now = camera_at(moved, seen);
            CHECK(now.x == doctest::Approx(was.x + c.dx));
            CHECK(now.y == doctest::Approx(was.y + c.dy));
        } else {
            CHECK_FALSE(ok);
            CHECK(has(why, c.says));
        }
    }
    // The fields: zoom is the camera's scale, written as zoom.
    Scene s;
    load(s, files, "    wait(1)");
    record(s);
    std::string text = scene_code(kStage, "    wait(1)"), out, why;
    PropValue v;
    v.v = 2;
    REQUIRE(edit_part_prop(*s.recording(), "camera", 0, 1.0 / 60, Prop::Scale, v, s.script().code(), text, out, why));
    CHECK(out == scene_code(kStage, "    at(0, camera().zoom(2, 0))\n    wait(1)"));
    v.v = 30;
    REQUIRE(edit_part_prop(*s.recording(), "camera", 0, 1.0 / 60, Prop::Rot, v, s.script().code(), text, out, why));
    CHECK(out == scene_code(kStage, "    at(0, camera().rot(30, 0))\n    wait(1)"));
    // Into the table of its x going, and its own zoom in it.
    load(s, files, "    camera():to({ x = 10 }, 1)");
    record(s);
    text = scene_code(kStage, "    camera():to({ x = 10 }, 1)");
    v.v = 1.5;
    REQUIRE(edit_part_prop(*s.recording(), "camera", 0.5, 1.0 / 60, Prop::Scale, v, s.script().code(), text, out, why));
    CHECK(out == scene_code(kStage, "    camera():to({ x = 10, zoom = 1.5 }, 1)"));
    load(s, files, "    camera():to({ x = 10, zoom = 1.5 }, 1)");
    record(s);
    text = scene_code(kStage, "    camera():to({ x = 10, zoom = 1.5 }, 1)");
    v.v = 3;
    REQUIRE(edit_part_prop(*s.recording(), "camera", 0.5, 1.0 / 60, Prop::Scale, v, s.script().code(), text, out, why));
    CHECK(out == scene_code(kStage, "    camera():to({ x = 10, zoom = 3 }, 1)"));
    // Its blocks' words: zoom, the follow and where it stands.
    load(s, files, "    camera():to({ zoom = 1.5 }, 1)\n    camera():follow(\"ёжик\")\n    camera():follow(nil)");
    record(s);
    std::vector<std::string> words;
    for (const Block& b : s.recording()->blocks) words.push_back(block_words(*s.recording(), b));
    CHECK(std::count(words.begin(), words.end(), "zoom → 1.5") == 1);
    CHECK(std::count(words.begin(), words.end(), "следит за ёжик") == 2);
    CHECK(std::count(words.begin(), words.end(), "стоит") == 2);
    // A follow's field is what the frame shows: its value is where its target stands.
    PropValue fx;
    bool going = false;
    load(s, files, "    camera():follow(\"ёжик\")\n    wait(1)");
    record(s);
    CHECK_FALSE(part_prop_value(*s.recording(), "camera", 0.5, 1.0 / 60, Prop::X, fx, going));
}

TEST_CASE("Сцена: музыка — вызовы и что звучит в миг t") {
    Files files;
    project(files);
    Scene s;
    load(s, files, R"(    music("тема", { fade = 1, volume = 0.8 })
    wait(2)
    music("тема", { volume = 0.4, fade = 0.5 })
    wait(1)
    music("бой", { fade = 2 })
    wait(3)
    music(nil, { fade = 1 })
    wait(1))");
    record(s);
    const Recording& rec = *s.recording();
    CHECK(rec.length == doctest::Approx(7)); // music does not wait
    REQUIRE(rec.music.size() == 4);
    struct Want {
        const char* name;
        f64 start, fade, volume;
    };
    const Want want[] = {{"тема", 0, 1, 0.8}, {"тема", 2, 0.5, 0.4}, {"бой", 3, 2, 1}, {"", 6, 1, 1}};
    for (usize i = 0; i < 4; ++i) {
        CAPTURE(i);
        CHECK(rec.music[i].name == want[i].name);
        CHECK(rec.music[i].start == doctest::Approx(want[i].start));
        CHECK(rec.music[i].fade == doctest::Approx(want[i].fade));
        CHECK(rec.music[i].volume == doctest::Approx(want[i].volume));
        REQUIRE(rec.music[i].site.site >= 0);
        CHECK(s.script().code().sites()[static_cast<usize>(rec.music[i].site.site)].line == 11 + 2 * i);
    }
    // What plays: the file, how long it has played, its volume, the call that began it.
    struct At {
        f64 t;
        std::vector<MusicVoice> voices;
    };
    const At ats[] = {
        {0, {{"тема", 0, 0, 0}}},
        {0.5, {{"тема", 0.5, 0.4, 0}}}, // half way in: 0,8 · 0,5
        {1.5, {{"тема", 1.5, 0.8, 0}}},
        {2.25, {{"тема", 2.25, 0.6, 0}}}, // the same music goes on, quieter over 0,5 s
        {3, {{"тема", 3, 0.4, 0}, {"бой", 0, 0, 2}}},
        {4, {{"тема", 4, 0.2, 0}, {"бой", 1, 0.5, 2}}}, // one into the other over 2 s
        {5, {{"бой", 2, 1, 2}}},
        {6.5, {{"бой", 3.5, 0.5, 2}}}, // music(nil): it goes out over 1 s
        {7, {}},
    };
    for (const At& a : ats) {
        CAPTURE(a.t);
        const std::vector<MusicVoice> got = music_at(rec, a.t);
        REQUIRE(got.size() == a.voices.size());
        for (usize i = 0; i < got.size(); ++i) {
            CHECK(got[i].name == a.voices[i].name);
            CHECK(got[i].since == doctest::Approx(a.voices[i].since));
            CHECK(got[i].volume == doctest::Approx(a.voices[i].volume));
            CHECK(got[i].call == a.voices[i].call);
        }
    }
    // The timeline's blocks: «тема» with both its calls up to «бой», «бой» up to the silence.
    const std::vector<MusicRun> runs = music_runs(rec);
    REQUIRE(runs.size() == 2);
    CHECK(runs[0].name == "тема");
    CHECK(runs[0].start == 0);
    CHECK(runs[0].end == doctest::Approx(3));
    CHECK(runs[0].fade == doctest::Approx(1));
    CHECK(runs[0].calls == std::vector<u32>{0, 1});
    CHECK(runs[1].name == "бой");
    CHECK(runs[1].start == doctest::Approx(3));
    CHECK(runs[1].end == doctest::Approx(6));
    CHECK(runs[1].calls == std::vector<u32>{2});

    // In spawn(…) and at(…), by time, not by the order made; at once without a fade; after silence the same name anew;
    // the last music to the scene's end.
    load(s, files, R"(    spawn(music("шум"))
    at(1.5, music("тема"))
    wait(1)
    music(nil)
    wait(1)
    music("шум", { volume = 0.5 })
    wait(1))");
    record(s);
    const Recording& r2 = *s.recording();
    REQUIRE(r2.music.size() == 4);
    const std::vector<MusicRun> r2runs = music_runs(r2);
    REQUIRE(r2runs.size() == 3);
    CHECK(r2runs[0].name == "шум");
    CHECK(r2runs[0].end == doctest::Approx(1));
    CHECK(r2runs[1].name == "тема");
    CHECK(r2runs[1].start == doctest::Approx(1.5));
    CHECK(r2runs[1].end == doctest::Approx(2));
    CHECK(r2runs[2].name == "шум");
    CHECK(r2runs[2].start == doctest::Approx(2));
    CHECK(r2runs[2].end == doctest::Approx(3));
    const u32 tema = r2runs[1].calls[0], again = r2runs[2].calls[0];
    CHECK(music_at(r2, 0.5) == std::vector<MusicVoice>{{"шум", 0.5, 1, r2runs[0].calls[0]}});
    CHECK(music_at(r2, 1.25).empty());
    CHECK(music_at(r2, 1.75) == std::vector<MusicVoice>{{"тема", 0.25, 1, tema}});
    CHECK(music_at(r2, 2.5) == std::vector<MusicVoice>{{"шум", 0.5, 0.5, again}});
}

TEST_CASE("Сцена: ожидание и разговор — время сцены стоит, клипы актёров идут") {
    Files files;
    project(files);
    Scene s;
    load(s, files, R"(    actor("ёжик"):loop("walk")
    spawn(actor("жук"):play("hop"))
    wait(0.2)
    wait_action("Поговорить")
    part("лес").x(10, 1)
    talk("Встреча")
    actor("жук"):play("hop")
    wait_action())");
    record(s);
    const Recording& rec = *s.recording();
    // Stops have no length: the next line goes from the same moment, the scene as long as without them.
    REQUIRE(rec.stops.size() == 3);
    CHECK(rec.stops[0].kind == SceneStop::Kind::Action);
    CHECK(rec.stops[0].name == "Поговорить");
    CHECK(rec.stops[0].time == doctest::Approx(0.2));
    CHECK(rec.stops[1].kind == SceneStop::Kind::Talk);
    CHECK(rec.stops[1].name == "Встреча");
    CHECK(rec.stops[1].time == doctest::Approx(1.2));
    CHECK(rec.stops[2].kind == SceneStop::Kind::Action);
    CHECK(rec.stops[2].name.empty());
    const ActorPlay& hop2 = play_of(rec, "жук", ActorPlay::Kind::Play, 1);
    CHECK(hop2.start == doctest::Approx(1.2));
    CHECK(rec.stops[2].time == doctest::Approx(1.2 + hop2.length)); // after the beetle's hop
    CHECK(rec.length == doctest::Approx(rec.stops[2].time));
    const u32 lines[] = {14, 16, 18};
    for (usize i = 0; i < 3; ++i) {
        REQUIRE(rec.stops[i].site.site >= 0);
        CHECK(s.script().code().sites()[static_cast<usize>(rec.stops[i].site.site)].line == lines[i]);
    }
    bool moved = false;
    for (const Block& b : rec.blocks)
        if (rec.parts[b.part] == "лес" && b.prop == Prop::X) {
            moved = true;
            CHECK(b.start == doctest::Approx(0.2));
        }
    CHECK(moved);
    CHECK(rec.stops[1].order < hop2.order); // made after the talk

    // The scene waited 0,15 s for the action, 0,3 s for the talk: a clip begun before a stop is ahead by it.
    const std::vector<f64> held = {0.15, 0.3, 0.7};
    CHECK(actor_moment(rec, "ёжик", 0.1, held).time == doctest::Approx(0.1)); // before the first wait
    CHECK(actor_moment(rec, "ёжик", 0.2, held).time == doctest::Approx(0.35));
    CHECK(actor_moment(rec, "ёжик", 0.2).time == doctest::Approx(0.2)); // none given: as recorded
    CHECK(actor_moment(rec, "ёжик", 1.2, held).time == doctest::Approx(std::fmod(1.65, 0.5))); // round and round
    CHECK(actor_moment(rec, "жук", 0.2, held).time == doctest::Approx(0.35));
    REQUIRE(hop2.clip_length < 1.15);
    CHECK(actor_moment(rec, "жук", 1, held).time == doctest::Approx(hop2.clip_length)); // to its end, held there
    // The hop begun at the talk's moment but after it: the talk does not move it.
    CHECK(actor_moment(rec, "жук", 1.2, held).play == &hop2);
    CHECK(actor_moment(rec, "жук", 1.2, held).time == doctest::Approx(0));
    CHECK(actor_moment(rec, "жук", 1.5, held).time == doctest::Approx(0.3));
    CHECK(actor_moment(rec, "ёжик", rec.length, held).time == doctest::Approx(std::fmod(rec.length + 0.15 + 0.3 + 0.7, 0.5)));

    // The frame: the hedgehog where its clip is 0,15 s on, the scene's own parts where they stand at 0,2.
    auto region = [&](f64 t, std::span<const f64> h) {
        const bool ok = s.frame(t, h);
        INFO(s.error());
        REQUIRE(ok);
        const assets::CookedTexture f = s.draw(160, 120, Affine{});
        std::vector<u8> out;
        for (u32 y = 40; y < 104; ++y)
            out.insert(out.end(), f.rgba8.begin() + (y * 160 + 8) * 4, f.rgba8.begin() + (y * 160 + 72) * 4);
        return out;
    };
    const std::vector<u8> waited = region(0.2, held);
    CHECK(waited == region(0.35, {}));
    CHECK(waited != region(0.2, {}));
    // Half way through its move, the picture «лес» is where the scene's time puts it, not 0,15 s on.
    auto forest_at = [&](f64 t, std::span<const f64> h) {
        REQUIRE(s.frame(t, h));
        return pixel_at(s.draw(160, 120, Affine{}), 20, 10);
    };
    CHECK(forest_at(0.7, held) == forest_at(0.7, {}));
    CHECK_FALSE(near(forest_at(0.7, held), forest_at(0.85, {})));

    // Standing at the talk: the hop made after it at its moment has not begun, the first one holds its end; the
    // hedgehog's loop goes on as without the stand.
    const ActorPlay& hop1 = play_of(rec, "жук", ActorPlay::Kind::Play);
    CHECK(actor_moment(rec, "жук", 1.2, held, 1).play == &hop1);
    CHECK(actor_moment(rec, "жук", 1.2, held, 1).time == doctest::Approx(hop1.clip_length));
    CHECK(actor_moment(rec, "ёжик", 1.2, held, 1).time == doctest::Approx(actor_moment(rec, "ёжик", 1.2, held).time));
    CHECK(actor_moment(rec, "жук", 1.2, held, 0).play == &hop2); // another stop: the hop has begun
    CHECK(actor_moment(rec, "жук", 1.2, held, 7).play == &hop2); // none such: as none

    // A loop stopped at a stop's moment, before it: it holds where it was then, ahead by the waits before it, not by
    // the one after.
    load(s, files, R"(    actor("ёжик"):loop("walk")
    wait(0.2)
    wait_action()
    wait(0.2)
    actor("ёжик"):stop()
    talk("Встреча")
    wait(0.5))");
    record(s);
    const Recording& r2 = *s.recording();
    REQUIRE(r2.stops.size() == 2);
    const ActorPlay& stop = play_of(r2, "ёжик", ActorPlay::Kind::Stop);
    CHECK(stop.order < r2.stops[1].order);
    const std::vector<f64> h2 = {0.05, 0.3};
    CHECK(actor_moment(r2, "ёжик", 0.3, h2).time == doctest::Approx(0.35));
    for (const f64 t : {0.4, 0.9}) {
        CAPTURE(t);
        CHECK(actor_moment(r2, "ёжик", t, h2).time == doctest::Approx(0.45));
        CHECK(actor_moment(r2, "ёжик", t, h2, 1).time == doctest::Approx(0.45));
    }
}

TEST_CASE("Сцена: музыка идёт, пока сцена ждёт, и её наплывы тоже") {
    Files files;
    project(files);
    Scene s;
    load(s, files, R"(    music("тема", { fade = 1 })
    wait(0.5)
    music("тема", { volume = 0.4, fade = 1 })
    talk("Встреча")
    music("бой", { fade = 0.5 })
    wait(1))");
    record(s);
    const Recording& rec = *s.recording();
    REQUIRE(rec.music.size() == 3);
    REQUIRE(rec.stops.size() == 1);
    CHECK(rec.music[1].order < rec.stops[0].order);
    CHECK(rec.stops[0].order < rec.music[2].order);
    auto same = [](const std::vector<MusicVoice>& got, const std::vector<MusicVoice>& want) {
        REQUIRE(got.size() == want.size());
        for (usize i = 0; i < got.size(); ++i) {
            CAPTURE(i);
            CHECK(got[i].name == want[i].name);
            CHECK(got[i].since == doctest::Approx(want[i].since));
            CHECK(got[i].volume == doctest::Approx(want[i].volume));
            CHECK(got[i].call == want[i].call);
        }
    };
    // As recorded: «бой» comes in at 0,5 at once after the talk, «тема» half way up goes out.
    same(music_at(rec, 0.5), {{"тема", 0.5, 0.5, 0}, {"бой", 0, 0, 2}});
    // 2 s into the talk: «тема» played on, quieted to 0,4 over its second; «бой» is after the talk.
    const std::vector<f64> two = {2}, quarter = {0.25};
    same(music_at(rec, 0.5, two, 0), {{"тема", 2.5, 0.4, 0}});
    same(music_at(rec, 0.5, quarter, 0), {{"тема", 0.75, 0.475, 0}});
    // The talk over: «бой» from then, «тема» going out from 0,4.
    same(music_at(rec, 0.5, two), {{"тема", 2.5, 0.4, 0}, {"бой", 0, 0, 2}});
    same(music_at(rec, 0.75, two), {{"тема", 2.75, 0.2, 0}, {"бой", 0.25, 0.5, 2}});
    same(music_at(rec, 1, two), {{"бой", 0.5, 1, 2}});
}

TEST_CASE("Сцена: музыка, ожидание и разговор — ошибки со строкой") {
    Files files;
    project(files);
    struct Case {
        const char* scene;
        const char* says;
    };
    const Case cases[] = {
        {"    music(1)", "имя — файл звуков игры, или nil — тишина"},
        {"    music(\"\")", "имя — файл звуков игры"},
        {"    music(\"папка/тема\")", "без папок"},
        {"    music(\"тема\", { fade = -1 })", "fade — число секунд не меньше 0"},
        {"    music(\"тема\", { volume = 2 })", "volume — громкость от 0 до 1"},
        {"    music(\"тема\", { loud = 1 })", "поля «loud» нет"},
        {"    music(\"тема\", 1)", "вторым — таблица"},
        {"    music(\"тема\", {}, 1)", "не больше"},
        {"    music(nil, { volume = 1 })", "у тишины громкости нет"},
        {"    wait_action(1)", "имя действия «Ввода», или ничего"},
        {"    wait_action(\"\")", "имя действия «Ввода»"},
        {"    wait_action(\"а\", \"б\")", "одно имя"},
        {"    talk()", "имя разговора игры"},
        {"    talk(\"а\", \"б\")", "одно имя"},
        {"    spawn(function() wait_action() end)", "wait_action — только в самом scene"},
        {"    all(function() talk(\"Встреча\") end)", "talk — только в самом scene"},
        {"    leave(\"растворение\", 1) wait_action()", "после leave сцена кончается"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.scene);
        Scene s;
        load(s, files, c.scene);
        CHECK_FALSE(s.record());
        INFO(s.error());
        CHECK(has(s.error(), c.says));
        CHECK(s.error_line() == 11);
    }
    for (const char* call : {"music(\"тема\")", "wait_action()", "talk(\"Встреча\")"}) {
        CAPTURE(call);
        PoseScript code;
        REQUIRE(code.load(std::string("return {\n  clips = { a = function(p)\n    ") + call + "\n  end },\n}\n"));
        Recording rec;
        CHECK_FALSE(code.record(nullptr, "a", {}, 1, {}, rec));
        INFO(code.error());
        CHECK(has(code.error(), "только в сцене"));
        CHECK(code.error_line() == 3);
    }
}

// ---- part 6г1: the scene's files for the game --------------------------------------------------------------------

namespace {

std::string replaced(std::string text, std::string_view from, std::string_view to) {
    const usize at = text.find(from);
    REQUIRE(at != std::string::npos);
    return text.replace(at, from.size(), to);
}

const std::string* gift_file(const SceneGift& g, std::string_view path) {
    for (const SceneGift::File& f : g.files)
        if (f.path == path) return &f.bytes;
    return nullptr;
}

// The element a layer of the scene in the game draws with: a run's itself, a picture's or an actor's inside its own
// layer (part 6г4: its mask by a shape of the stage).
Rml::Element* layer_element(Rml::Element* camera, int i) {
    Rml::Element* e = camera ? camera->GetChild(i) : nullptr;
    return e && e->GetTagName() == "div" ? e->GetChild(0) : e;
}

// A part's value as the game poses it, at rest without a track.
vector::MotionValue game_value_of(const std::vector<vector::MotionPartPose>& parts, std::string_view id, vector::MotionProp p) {
    for (const vector::MotionPartPose& pp : parts)
        if (pp.id == id && pp.has[static_cast<usize>(p)]) return pp.values[static_cast<usize>(p)];
    return vector::motion_rest(p);
}

// Pictures, actors of each kind (drawn by code with pivots, with another drawing, from «Ресурсы» with a morph) and
// shapes; lines 3–11, the scene's from 14.
const char* kGiftStage = R"(    canvas(160, 120)
    background("#203040")
    picture("лес", "картинки/лес.png", { x = 8, y = 6 })
    picture("лес_2", "картинки/лес.png", { x = 100, y = 10, w = 20, h = 15 })
    actor("ёжик", "персонажи/Ёжик", { x = 40, y = 100, pivot = { 32, 60 } })
    actor("жук", "персонажи/Жук", { x = 120, y = 90, scale = 0.5, rot = 15, pivot = { 24, 16 } })
    actor("флаг", "персонажи/Флаг", { x = 20, y = 30 })
    path("дорога", { d = "M 40 100 L 120 100", fill = "none", stroke = "#ffffff", hidden = true })
    rect("пол", { x = 0, y = 110, w = 160, h = 10, fill = "#405060" }))";

// Everything 6а–6в make: a transition in, music, clips of each kind at once, the camera following, the scene's moves of
// an actor and its part, a wait, the same clip at other values, a stop, a talk, silence and a leave.
const char* kGiftScene = R"(    transition("растворение", 0.5)
    music("тема", { fade = 1 })
    actor("ёжик"):loop("walk")
    spawn(actor("жук"):play("hop"))
    spawn(actor("флаг"):play("talk"))
    spawn(part("ёжик/nose").rot(15, 0.5))
    spawn(part("ёжик").tint("#ff0000", 0.25))
    camera():follow("ёжик", 0.5)
    part("ёжик"):along("дорога", 2)
    wait_action("Поговорить")
    actor("ёжик"):play("walk", { params = { height = 8 } })
    actor("ёжик"):stop()
    talk("Встреча")
    part("пол").fill("#806040", 0.5)
    music(nil, { fade = 0.5 })
    leave("сдвиг", 0.5, { from = "лево" }))";

} // namespace

TEST_CASE("Сцена в игре: scene.json — каждое поле туда и обратно, ошибки называют поле") {
    using K = vector::ScenePlay::Kind;
    vector::SceneFile f;
    f.name = "Вступление";
    f.from = "сцены/Вступление.luau";
    f.width = 160;
    f.height = 120;
    f.background = "#203040";
    f.length = 12.4;
    f.pictures = {{"лес", "pictures/лес.png", 8, 6, 40, 30}, {"лес 2", "pictures/лес.png", -4.5, 0.1, 80, 60}};
    f.actors = {{"ёжик", "персонажи/Ёжик", "actors/ёжик.svg", "actors/ёжик.motion.json", {0, 0}, 64, 64},
                {"флаг", "персонажи/Флаг", "actors/флаг.svg", "actors/флаг.motion.json", {-10, -20}, 40, 40}};
    f.plays = {{K::Loop, "ёжик", "walk", 0, 1, 3}, {K::Play, "ёжик", "walk 2", 0.1, 1.5, 4}, {K::Stop, "ёжик", "", 2.5, 1, 9}, {K::Play, "флаг", "talk", 3.2, 1, 8}};
    f.music = {{0, 2, "тема леса", 1, 0.8}, {6, 10, "", 1, 1}};
    f.stops = {{3.2, 7, false, "Поговорить"}, {3.2, 8, true, "Встреча"}, {4, 11, false, ""}};
    f.transitions = {{false, "", "", 0, 0.5}, {true, "сдвиг", "лево", 11.9, 0.5}};
    f.not_drawn = {"blur", "mask_by_item"};
    const std::string text = vector::write_scene_file(f);
    INFO(text);
    vector::SceneFile back;
    std::string why;
    REQUIRE(vector::read_scene_file(text, back, why));
    CHECK(back == f);
    CHECK(vector::write_scene_file(back) == text);
    CHECK(has(text, "\"forge\": \"scene\""));
    CHECK(has(text, "\"version\": 1"));
    CHECK(has(text, "\"length\": 12.4,"));
    CHECK(has(text, "{\"actor\": \"ёжик\", \"kind\": \"stop\", \"at\": 2.5, \"order\": 9}")); // no clip, no speed
    CHECK(has(text, "{\"at\": 6, \"order\": 10, \"fade\": 1, \"volume\": 1}"));             // music(nil): no name
    // A transparent scene has no background.
    vector::SceneFile clear = f;
    clear.background.clear();
    const std::string clear_text = vector::write_scene_file(clear);
    CHECK_FALSE(has(clear_text, "background"));
    REQUIRE(vector::read_scene_file(clear_text, back, why));
    CHECK(back == clear);

    struct Bad {
        const char *from, *to, *said;
    };
    const Bad bad[] = {
        {"\"forge\": \"scene\"", "\"forge\": \"screen\"", "forge: "},
        {"\"version\": 1", "\"version\": 3", "version 3: сцену отдал редактор новее этой игры (она читает 2)"},
        {"\"name\": \"Вступление\"", "\"name\": \"\"", "name: "},
        {"\"canvas\": [160, 120]", "\"canvas\": [160, 0]", "canvas: "},
        {"\"background\": \"#203040\"", "\"background\": \"red\"", "background: "},
        {"\"length\": 12.4", "\"length\": -1", "length: "},
        {"\"file\": \"pictures/лес.png\"", "\"file\": \"../лес.png\"", "pictures[0]: file: "},
        {"\"w\": 80", "\"w\": 0", "pictures[1]: w, h: "},
        {"\"motion\": \"actors/ёжик.motion.json\"", "\"motion\": \"/ёжик.motion.json\"", "actors[0]: motion: "},
        {"\"picture\": \"actors/флаг.svg\"", "\"picture\": \"actors\\\\..\\\\флаг.svg\"", "actors[1]: picture: "},
        {"\"id\": \"флаг\"", "\"id\": \"ёжик\"", "actors[1]: имя «ёжик» дважды"},
        {"\"id\": \"флаг\"", "\"id\": \"лес\"", "actors[1]: имя «лес» дважды"},
        {"\"id\": \"флаг\"", "\"id\": \"camera\"", "actors[1]: имя camera"},
        {"\"size\": [64, 64]", "\"size\": [64]", "actors[0]: size: "},
        {"\"kind\": \"loop\"", "\"kind\": \"spin\"", "plays[0]: kind: «spin»"},
        {"\"actor\": \"флаг\"", "\"actor\": \"лиса\"", "plays[3]: actor: "},
        {"\"kind\": \"stop\"", "\"kind\": \"stop\", \"clip\": \"walk\"", "plays[2]: clip: у stop клипа нет"},
        {"\"clip\": \"walk 2\"", "\"clip\": \"\"", "plays[1]: clip: "},
        {"\"speed\": 1.5", "\"speed\": 0", "plays[1]: speed: "},
        {"\"order\": 3}", "\"order\": -3}", "plays[0]: order: "},
        {"\"volume\": 0.8", "\"volume\": 2", "music[0]: volume: "},
        {"\"name\": \"тема леса\"", "\"name\": 5", "music[0]: name: "},
        {"\"talk\": \"Встреча\"", "\"talk\": \"Встреча\", \"action\": \"\"", "stops[1]: нужно одно из"},
        {"\"talk\": \"Встреча\"", "\"talk\": \"\"", "stops[1]: talk: "},
        {"\"at\": 4, \"order\": 11", "\"at\": 1, \"order\": 11", "stops[2]: at: ожидания идут по времени"},
        {"\"leave\": true", "\"leave\": 1", "transitions[1]: leave: "},
        {"\"length\": 0.5}", "\"length\": -0.5}", "transitions[0]: length: "},
        {"\"not_drawn\": [\"blur\"", "\"not_drawn\": [3", "not_drawn[0]: "},
        {"\"plays\": [", "\"plays\": {\"a\": [", "не JSON"},
    };
    for (const Bad& b : bad) {
        CAPTURE(b.to);
        vector::SceneFile out;
        CHECK_FALSE(vector::read_scene_file(replaced(text, b.from, b.to), out, why));
        CHECK(why.rfind(b.said, 0) == 0);
        CHECK(out == vector::SceneFile{});
    }
}

TEST_CASE("Сцена в игре: «Отдать игре» — папка сцены, её файлы и сверка с вкладкой") {
    Files files;
    project(files);
    Scene s;
    load(s, files, kGiftScene, kGiftStage);
    SceneGift gift;
    std::string why;
    const bool ok = build_scene_gift(s, "Вступление", "сцены/Вступление.luau", files.scene(), gift, why);
    INFO(why);
    REQUIRE(ok);
    const Recording& rec = *s.recording();
    std::vector<std::string> paths;
    for (const SceneGift::File& f : gift.files) paths.push_back(f.path);
    CHECK(std::is_sorted(paths.begin(), paths.end()));
    std::sort(paths.begin(), paths.end());
    const std::vector<std::string> want_paths = {"actors/жук.flat.svg", "actors/жук.motion.json", "actors/жук.svg",       "actors/флаг.motion.json",
                                                 "actors/флаг.svg",      "actors/ёжик.motion.json", "actors/ёжик.svg", "pictures/лес.png",
                                                 "scene.json",           "stage.motion.json",      "stage.svg"};
    CHECK(paths == want_paths);
    CHECK(gift.notes.empty()); // nothing the game will not draw

    // scene.json: what the tab recorded.
    const vector::SceneFile& f = gift.motion.file;
    vector::SceneFile read;
    REQUIRE(vector::read_scene_file(*gift_file(gift, "scene.json"), read, why));
    CHECK(read == f);
    CHECK(f.name == "Вступление");
    CHECK(f.from == "сцены/Вступление.luau");
    CHECK(f.width == 160);
    CHECK(f.height == 120);
    CHECK(f.background == "#203040");
    CHECK(f.length == rec.length);
    CHECK(f.length == doctest::Approx(4));
    REQUIRE(f.pictures.size() == 2);
    CHECK(f.pictures[0] == vector::ScenePicture{"лес", "pictures/лес.png", 8, 6, 40, 30});
    CHECK(f.pictures[1] == vector::ScenePicture{"лес_2", "pictures/лес.png", 100, 10, 20, 15}); // one file of one picture
    CHECK(*gift_file(gift, "pictures/лес.png") == std::string(files.bytes["картинки/лес.png"].begin(), files.bytes["картинки/лес.png"].end()));
    REQUIRE(f.actors.size() == 3);
    CHECK(f.actors[0] == vector::SceneActor{"ёжик", "персонажи/Ёжик", "actors/ёжик.svg", "actors/ёжик.motion.json", {0, 0}, 64, 64});
    CHECK(f.actors[2] == vector::SceneActor{"флаг", "персонажи/Флаг", "actors/флаг.svg", "actors/флаг.motion.json", {-10, -20}, 40, 40});
    // The plays as recorded: the same clip at other values is another clip of the file.
    REQUIRE(f.plays.size() == rec.plays.size());
    for (usize i = 0; i < f.plays.size(); ++i) {
        CAPTURE(i);
        CHECK(static_cast<u8>(f.plays[i].kind) == static_cast<u8>(rec.plays[i].kind));
        CHECK(f.plays[i].actor == rec.plays[i].actor);
        CHECK(f.plays[i].at == rec.plays[i].start);
        CHECK(f.plays[i].order == rec.plays[i].order);
    }
    std::vector<std::string> hedgehog;
    for (const vector::ScenePlay& p : f.plays)
        if (p.actor == "ёжик") hedgehog.push_back(p.clip);
    CHECK(hedgehog == std::vector<std::string>{"walk", "walk 2", ""});
    const vector::MotionFile& hf = gift.motion.actors[0];
    REQUIRE(hf.clips.size() == 2);
    CHECK(hf.clips[0].name == "walk");
    CHECK(hf.clips[1].name == "walk 2");
    // walk bobs the head by p.height: 4, then 8.
    auto lowest = [&](const vector::MotionClip& c) {
        f64 y = 0;
        for (f64 t = 0; t <= c.length; t += 1.0 / 120) y = std::min(y, game_value_of(vector::motion_pose(hf, c, t), "ёжик/head", vector::MotionProp::Y).v);
        return y;
    };
    CHECK(lowest(hf.clips[0]) == doctest::Approx(-4));
    CHECK(lowest(hf.clips[1]) == doctest::Approx(-8));
    CHECK(hf.picture == "ёжик.svg");
    CHECK(hf.from == "персонажи/Ёжик.pixel.json");
    REQUIRE(f.music.size() == 2);
    CHECK(f.music[0] == vector::SceneMusicCall{0, rec.music[0].order, "тема", 1, 1});
    CHECK(f.music[1].name.empty());
    CHECK(f.music[1].at == doctest::Approx(3.5));
    REQUIRE(f.stops.size() == 2);
    CHECK(f.stops[0] == vector::SceneStop{rec.stops[0].time, rec.stops[0].order, false, "Поговорить"});
    CHECK(f.stops[1] == vector::SceneStop{rec.stops[1].time, rec.stops[1].order, true, "Встреча"});
    CHECK(f.stops[0].at == doctest::Approx(2.5));
    REQUIRE(f.transitions.size() == 2);
    CHECK(f.transitions[0] == vector::SceneTransition{false, "растворение", "", 0, 0.5});
    CHECK(f.transitions[1].leave);
    CHECK(f.transitions[1].kind == "сдвиг");
    CHECK(f.transitions[1].from == "лево");
    CHECK(f.transitions[1].at == doctest::Approx(3.5));
    CHECK(f.not_drawn.empty());

    // The pictures: the stage's without its background, its items' groups empty; each actor's ids under its name.
    const std::string& stage_svg_text = *gift_file(gift, "stage.svg");
    CHECK_FALSE(has(stage_svg_text, "#203040"));
    CHECK(has(stage_svg_text, "id=\"пол\""));
    CHECK(has(stage_svg_text, "id=\"ёжик\""));
    CHECK_FALSE(has(stage_svg_text, "ёжик/"));
    const std::string& hedgehog_svg = *gift_file(gift, "actors/ёжик.svg");
    CHECK(has(hedgehog_svg, "id=\"ёжик/head\""));
    CHECK(has(hedgehog_svg, "id=\"ёжик\""));
    CHECK(hedgehog_svg == prefix_svg_ids(s.character("ёжик")->svg, "ёжик"));
    CHECK(has(*gift_file(gift, "actors/жук.flat.svg"), "id=\"жук/shell\""));
    CHECK(has(*gift_file(gift, "actors/флаг.svg"), "id=\"флаг/open\""));
    // Every file is the motion it says.
    vector::MotionFile stage_back, beetle_back;
    REQUIRE(vector::read_motion_file(*gift_file(gift, "stage.motion.json"), stage_back, why));
    CHECK(stage_back == gift.motion.stage);
    REQUIRE(vector::read_motion_file(*gift_file(gift, "actors/жук.motion.json"), beetle_back, why));
    CHECK(beetle_back == gift.motion.actors[1]);
    REQUIRE(gift.motion.stage.clips.size() == 1);
    CHECK(gift.motion.stage.clips[0].name == "scene");
    CHECK(gift.motion.stage.part("camera"));
    CHECK(gift.motion.stage.part("camera")->pivot == vector::Point{80, 60});
    CHECK(gift.motion.stage.part("ёжик"));
    // The scene's turn of the hedgehog's nose is the actor's to place: its pivot in the actor's file.
    REQUIRE(gift.motion.actors[0].part("ёжик/nose"));
    CHECK(gift.motion.actors[0].part("ёжик/nose")->pivot == vector::Point{30, 26});

    // The game's pose at a moment: the stage's group of the hedgehog half way along the road, the scene's turn of its
    // nose over its clip, its tint on its picture, the camera on it.
    const vector::ScenePose pose = vector::scene_pose(gift.motion, 1.5);
    CHECK(pose.phase == vector::ScenePhase::Scene);
    CHECK(game_value_of(pose.stage, "ёжик", vector::MotionProp::X).v == doctest::Approx(40));
    CHECK(game_value_of(pose.stage, "ёжик", vector::MotionProp::Y).v == doctest::Approx(0));
    CHECK(game_value_of(pose.stage, "ёжик", vector::MotionProp::Tint).v == 0); // on its picture, not its group
    const vector::SceneActorPose& h = pose.actors[0];
    CHECK(h.moment.play == 0);
    CHECK(h.moment.time == doctest::Approx(0)); // walk's third round begins
    CHECK(vector::scene_pose(gift.motion, 1.6).actors[0].moment.time == doctest::Approx(0.1));
    CHECK(game_value_of(h.parts, "ёжик/nose", vector::MotionProp::Rot).v == doctest::Approx(15));
    CHECK(game_value_of(h.parts, "ёжик", vector::MotionProp::Tint).v == doctest::Approx(1));
    CHECK(game_value_of(h.parts, "ёжик", vector::MotionProp::Tint).color == vector::MotionColor{255, 0, 0, 255});
    CHECK(pose.camera.values[static_cast<usize>(vector::MotionProp::X)].v == doctest::Approx(80 - 80));
    CHECK(vector::scene_pose(gift.motion, 0.25).phase == vector::ScenePhase::Enter);
    CHECK(vector::scene_pose(gift.motion, 3.75).phase == vector::ScenePhase::Leave);
    CHECK(vector::scene_pose(gift.motion, 4).phase == vector::ScenePhase::Gone);
    // The beetle shows its flat drawing on the way down.
    bool flat = false;
    for (f64 t = 0; t < 1; t += 0.05) flat = flat || vector::scene_pose(gift.motion, t).actors[1].shown.drawing == "flat";
    CHECK(flat);

    // The same again: the same files byte for byte.
    SceneGift again;
    REQUIRE(build_scene_gift(s, "Вступление", "сцены/Вступление.luau", files.scene(), again, why));
    REQUIRE(again.files.size() == gift.files.size());
    for (usize i = 0; i < gift.files.size(); ++i) {
        CAPTURE(gift.files[i].path);
        CHECK(again.files[i].path == gift.files[i].path);
        CHECK(again.files[i].bytes == gift.files[i].bytes);
    }
}

TEST_CASE("Сцена в игре: миг клипа и музыка по файлам — как во вкладке, с ожиданиями и без") {
    Files files;
    project(files);
    Scene s;
    // 6в's waits and talks, and its music going on while the scene waits.
    load(s, files, R"(    music("тема", { fade = 1, volume = 0.8 })
    actor("ёжик"):loop("walk")
    spawn(actor("жук"):play("hop", { speed = 2 }))
    wait(0.2)
    wait_action("Поговорить")
    music("бой", { fade = 0.5 })
    part("лес").x(10, 1)
    talk("Встреча")
    actor("жук"):play("hop")
    actor("ёжик"):stop()
    music(nil, { fade = 1 })
    wait_action()
    wait(0.5))");
    SceneGift gift;
    std::string why;
    const bool ok = build_scene_gift(s, "Ожидание", "сцены/Ожидание.luau", files.scene(), gift, why);
    INFO(why);
    REQUIRE(ok);
    const Recording& rec = *s.recording();
    REQUIRE(rec.stops.size() == 3);
    const std::vector<std::vector<f64>> helds = {{}, {0.15, 0.3, 0.7}, {1, 0, 2}};
    for (const std::vector<f64>& held : helds)
        for (f64 t = 0; t <= rec.length + 1e-9; t += 0.05)
            for (i32 waiting = -1; waiting < 3; ++waiting) {
                CAPTURE(t);
                CAPTURE(waiting);
                CAPTURE(held.size());
                for (const char* actor : {"ёжик", "жук"}) {
                    CAPTURE(actor);
                    const ActorMoment tab = actor_moment(rec, actor, t, held, waiting);
                    const vector::SceneActorMoment game = vector::scene_actor_moment(gift.motion, actor, t, held, waiting);
                    CHECK(game.play == (tab.play ? static_cast<i32>(tab.play - rec.plays.data()) : -1));
                    CHECK(game.time == doctest::Approx(tab.time));
                }
                const std::vector<MusicVoice> heard = music_at(rec, t, held, waiting);
                const std::vector<vector::SceneMusicVoice> played = vector::scene_music_at(gift.motion.file, t, held, waiting);
                REQUIRE(played.size() == heard.size());
                for (usize i = 0; i < heard.size(); ++i) {
                    CHECK(played[i].name == heard[i].name);
                    CHECK(played[i].call == heard[i].call);
                    CHECK(played[i].since == doctest::Approx(heard[i].since));
                    CHECK(played[i].volume == doctest::Approx(heard[i].volume));
                }
            }
    // The beetle's hop at speed 2 is the same clip of the file: speed is the play's, not the clip's.
    CHECK(gift.motion.actors[1].clips.size() == 1);
}

TEST_CASE("Сцена в игре: сверка ловит, где файлы разошлись бы со вкладкой") {
    Files files;
    project(files);
    Scene s;
    load(s, files, kGiftScene, kGiftStage);
    SceneGift gift;
    std::string why;
    REQUIRE(build_scene_gift(s, "Вступление", "сцены/Вступление.luau", files.scene(), gift, why));
    REQUIRE(check_scene_gift(s, gift.motion, why));
    auto refused = [&](const vector::SceneMotion& m, std::string_view said) {
        CAPTURE(said);
        std::string w;
        CHECK_FALSE(check_scene_gift(s, m, w));
        INFO(w);
        CHECK(w.rfind("в игре сцена разошлась бы со вкладкой: ", 0) == 0);
        CHECK(has(w, said));
    };
    auto track = [](vector::MotionClip& c, std::string_view part, vector::MotionProp p) -> vector::MotionTrack& {
        for (vector::MotionTrack& t : c.tracks)
            if (t.part == part && t.prop == p) return t;
        FAIL("no such track");
        return c.tracks.front();
    };
    {
        // A piece of the hedgehog's road a little off.
        vector::SceneMotion m = gift.motion;
        vector::MotionTrack& x = track(m.stage.clips[0], "ёжик", vector::MotionProp::X);
        bool moved = false;
        for (vector::MotionPiece& pc : x.pieces)
            for (vector::MotionValue& v : pc.frames) v.v += 0.5, moved = true;
        REQUIRE(moved);
        refused(m, "«ёжик», место по x");
    }
    {
        // The hedgehog's nose turned by its clip as well as by the scene.
        vector::SceneMotion m = gift.motion;
        vector::MotionClip& walk = m.actors[0].clips[0];
        vector::MotionTrack rot;
        rot.part = "ёжик/nose";
        rot.prop = vector::MotionProp::Rot;
        vector::MotionPiece hold;
        hold.value.v = 3;
        rot.pieces.push_back(hold);
        walk.tracks.push_back(rot);
        refused(m, "«ёжик/nose», поворот");
    }
    {
        // The head's bob of «walk 2» at the first values.
        vector::SceneMotion m = gift.motion;
        m.actors[0].clips[1].tracks = m.actors[0].clips[0].tracks;
        refused(m, "«ёжик/head», место по y");
    }
    {
        // The second play a little later.
        vector::SceneMotion m = gift.motion;
        for (vector::ScenePlay& p : m.file.plays)
            if (p.clip == "walk 2") p.at += 0.1;
        refused(m, "«ёжик», клип и его миг");
    }
    {
        vector::SceneMotion m = gift.motion;
        m.file.music[0].volume = 0.5;
        refused(m, "музыка «тема»");
    }
    {
        vector::SceneMotion m = gift.motion;
        m.file.transitions[1].at += 0.1;
        refused(m, "вход, сцена или уход");
    }
    {
        // A part the scene moves without where it stands: the game would not turn it round its pivot.
        vector::SceneMotion m = gift.motion;
        std::erase_if(m.actors[0].parts, [](const vector::MotionPart& p) { return p.id == "ёжик/nose"; });
        refused(m, "«ёжик/nose»: в файле нет того, где часть стоит");
    }
    {
        // A wait the scene stood at a little later: the clips' moments after it differ (with waits only).
        vector::SceneMotion m = gift.motion;
        m.file.stops[0].at += 0.05;
        refused(m, "с ожиданиями по 0,5 с");
    }
}

TEST_CASE("Сцена в игре: имена, которые не годятся для файла или дали бы один файл") {
    Files files;
    project(files);
    files.bytes["картинки/лес2.png"] = files.bytes["картинки/лес.png"];
    // Characters with drawings whose names make no file, or one file of two.
    auto character = [&](const std::string& name, std::string_view drawings) {
        PixelDoc d;
        d.script = name + ".luau";
        d.frame_w = d.frame_h = 16;
        files.put("персонажи/" + name + ".pixel.json", write_pixel_doc(d));
        files.put("персонажи/" + name + ".luau", "return {\n  clips = { c = function(p) wait(0.1) end },\n  draw = function(p, rng)\n    canvas(16, 16)\n"
                                                 "    rect(\"a\", { x = 0, y = 0, w = 16, h = 16, fill = \"#000000\" })\n  end,\n  drawings = {\n" +
                                                     std::string(drawings) + "  },\n}\n");
    };
    auto drawing = [](std::string_view name) {
        return "    [\"" + std::string(name) + "\"] = function(p, rng)\n      canvas(16, 16)\n      rect(\"a\", { x = 0, y = 0, w = 8, h = 16, fill = \"#000000\" })\n    end,\n";
    };
    character("Злой", drawing("злой?"));
    character("Двое", drawing("Flat") + drawing("flat"));
    auto refused = [&](std::string_view stage, std::string_view said, std::string_view name = "Сцена", std::string_view code = "    wait(1)") {
        Scene s;
        load(s, files, code, stage);
        SceneGift gift;
        std::string why;
        CHECK_FALSE(build_scene_gift(s, name, "сцены/Сцена.luau", files.scene(), gift, why));
        CHECK(why == said);
        CHECK(gift.files.empty());
    };
    // The stage's ids are letters, digits, _ and - already: two of them make one file by their capitals.
    refused(R"(    actor("Ёжик", "персонажи/Ёжик")
    actor("ёжик", "персонажи/Ёжик", { x = 20 }))",
            "актёр «Ёжик» и актёр «ёжик» дали бы один файл «actors/ёжик.svg»");
    refused(R"(    picture("Лес", "картинки/лес.png")
    picture("лес", "картинки/лес2.png"))",
            "картинка «Лес» и картинка «лес» дали бы один файл «pictures/лес.png»");
    // (Its drawings are given with its clips: an actor that plays none gives neither.)
    refused(R"(    actor("злюка", "персонажи/Злой"))", "актёр «злюка»: рисунок «злой?» — такое имя не годится для файла", "Сцена",
            R"(    actor("злюка"):play("c"))");
    refused(R"(    actor("двое", "персонажи/Двое"))", "рисунок «Flat» актёра «двое» и рисунок «flat» актёра «двое» дали бы один файл «actors/двое.flat.svg»",
            "Сцена", R"(    actor("двое"):play("c"))");
    refused("    canvas(10, 10)", "имя сцены «а:б» не годится для имени папки", "а:б");
    // Two pictures of one file are one file, by the first.
    Scene s;
    load(s, files, "    wait(1)", R"(    picture("Лес", "картинки/лес.png")
    picture("лес", "картинки/лес.png")
    actor("Ёжик", "персонажи/Ёжик"))");
    SceneGift gift;
    std::string why;
    CHECK(build_scene_gift(s, "Сцена", "сцены/Сцена.luau", files.scene(), gift, why));
    INFO(why);
    CHECK(gift_file(gift, "pictures/Лес.png"));
    CHECK_FALSE(gift_file(gift, "pictures/лес.png"));
    CHECK(gift_file(gift, "actors/Ёжик.svg"));
    // An actor that plays no clip: its drawings of names that make no file are not given.
    Scene z;
    load(z, files, "    wait(1)", R"(    actor("злюка", "персонажи/Злой"))");
    SceneGift zg;
    CHECK(build_scene_gift(z, "Сцена", "сцены/Сцена.luau", files.scene(), zg, why));
    INFO(why);
    std::vector<std::string> paths;
    for (const SceneGift::File& f : zg.files) paths.push_back(f.path);
    CHECK(paths == std::vector<std::string>{"actors/злюка.svg", "scene.json", "stage.motion.json", "stage.svg"});
}

TEST_CASE("Сцена в игре: актёр без клипов — без файла клипов, его части стоят по записям сцены") {
    Files files;
    project(files);
    Scene s;
    load(s, files, R"(    spawn(actor("жук"):play("hop"))
    spawn(part("ёжик/nose"):to({ rot = 15 }, 0.5))
    part("ёжик"):to({ x = 60 }, 0.5))",
         std::string(kGiftStage));
    SceneGift gift;
    std::string why;
    const bool ok = build_scene_gift(s, "Сцена", "сцены/Сцена.luau", files.scene(), gift, why);
    INFO(why);
    REQUIRE(ok);
    const vector::SceneMotion& m = gift.motion;
    REQUIRE(m.file.actors.size() == 3);
    CHECK(m.file.actors[0].id == "ёжик");
    CHECK(m.file.actors[0].motion.empty());
    CHECK(m.file.actors[1].motion == "actors/жук.motion.json");
    CHECK(m.file.actors[2].motion.empty());
    CHECK(gift_file(gift, "actors/ёжик.svg"));
    CHECK_FALSE(gift_file(gift, "actors/ёжик.motion.json"));
    CHECK_FALSE(gift_file(gift, "actors/флаг.motion.json"));
    CHECK(has(*gift_file(gift, "scene.json"), "\"picture\": \"actors/ёжик.svg\""));
    CHECK_FALSE(has(*gift_file(gift, "scene.json"), "\"motion\": \"\""));
    // The nose turns round its own pivot, by the stage's record of it.
    REQUIRE(m.stage.part("ёжик/nose"));
    CHECK(m.stage.part("ёжик/nose")->pivot.x == doctest::Approx(30));
    CHECK(m.stage.part("ёжик/nose")->pivot.y == doctest::Approx(26));
    const vector::ScenePose pose = vector::scene_pose(m, 0.5);
    const vector::MotionPartPose* nose = nullptr;
    for (const vector::MotionPartPose& pp : pose.actors[0].parts)
        if (pp.id == "ёжик/nose") nose = &pp;
    REQUIRE(nose);
    CHECK(nose->moves);
    CHECK(game_value_of(pose.actors[0].parts, "ёжик/nose", vector::MotionProp::Rot).v == doctest::Approx(15));
    // Read back as the game reads it, written again the same.
    vector::SceneFile back;
    REQUIRE(vector::read_scene_file(*gift_file(gift, "scene.json"), back, why));
    CHECK(back == m.file);
    CHECK(vector::write_scene_file(back) == *gift_file(gift, "scene.json"));
    // Without the record the game would not turn it round its pivot; a play of it would find no clip.
    {
        vector::SceneMotion t = m;
        std::erase_if(t.stage.parts, [](const vector::MotionPart& p) { return p.id == "ёжик/nose"; });
        std::string w;
        CHECK_FALSE(check_scene_gift(s, t, w));
        CHECK(has(w, "«ёжик/nose»: в файле нет того, где часть стоит"));
    }
    {
        vector::SceneMotion t = m;
        t.file.plays.push_back({vector::ScenePlay::Kind::Play, "ёжик", "walk", 0.2, 1, 9});
        std::string w;
        CHECK_FALSE(vector::check_scene(t, w));
        CHECK(w == "актёр «ёжик» играет «walk», а файла клипов у него нет");
    }
}

namespace {

// A ball whose clip grows it twice as big, turning round its middle.
const char* kBall = R"(return {
  clips = {
    grow = function(p)
      part("ball").scale(2, 0.5)
    end,
  },
  draw = function(p, rng)
    canvas(32, 32)
    circle("ball", { x = 16, y = 16, r = 8, fill = "#ff8000" })
  end,
}
)";

} // namespace

TEST_CASE("Сцена в игре: копии сцены, кусок по кадрам и масштаб поверх клипа — по файлам как во вкладке") {
    Files files;
    project(files);
    PixelDoc ball;
    ball.script = "Шар.luau";
    ball.frame_w = ball.frame_h = 32;
    ball.pivots["ball"] = {16, 16};
    files.put("персонажи/Шар.pixel.json", write_pixel_doc(ball));
    files.put("персонажи/Шар.luau", kBall);
    // The floor goes along an arc; the beetle's play at 0.31 s falls between two of the arc's frames, the copies are
    // born then.
    Scene s;
    load(s, files, R"(    spawn(actor("шар"):play("grow"))
    spawn(part("шар/ball").scale(1.5, 0.5))
    spawn(part("пол"):along("дуга", 1))
    wait(0.31)
    copies("пол", 2, function(i, c) c.y(i * -10, 0.2) end)
    actor("жук"):play("hop"))",
         R"(    canvas(160, 120)
    actor("шар", "персонажи/Шар", { x = 80, y = 40 })
    path("дуга", { d = "M 0 100 Q 80 0 160 100", fill = "none", stroke = "#ffffff", hidden = true })
    rect("пол", { x = 0, y = 110, w = 20, h = 10, fill = "#405060" })
    actor("жук", "персонажи/Жук", { x = 120, y = 90 }))");
    SceneGift gift;
    std::string why;
    const bool ok = build_scene_gift(s, "Сцена", "сцены/Сцена.luau", files.scene(), gift, why);
    INFO(why);
    REQUIRE(ok);
    const vector::SceneMotion& m = gift.motion;
    // The floor's way is kept frame by frame: between two frames the game mixes them, so the check (at 0.31 s) took its
    // values only at the frames.
    bool frames = false;
    for (const vector::MotionTrack& t : m.stage.clip("scene")->tracks)
        if (t.part == "пол")
            for (const vector::MotionPiece& pc : t.pieces) frames = frames || pc.kind == vector::MotionPiece::Kind::Frames;
    CHECK(frames);
    // The copies are drawn from their birth, not before.
    CHECK(vector::scene_pose(m, 0.3).copies.empty());
    CHECK(vector::scene_pose(m, 0.32).copies.size() == 2);
    // The scene's scale of the ball multiplies its clip's.
    REQUIRE(m.file.actors[0].id == "шар");
    CHECK(game_value_of(vector::scene_pose(m, 0.5).actors[0].parts, "шар/ball", vector::MotionProp::Scale).v == doctest::Approx(3));
}

TEST_CASE("Сцена в игре: сверка ловит миг клипа без дорожек и то, что видно, только пока сцена ждёт") {
    Files files;
    project(files);
    {
        // stretch moves nothing: only its moment tells a game it went twice as fast.
        Scene s;
        load(s, files, R"(    actor("ёжик"):play("stretch"))", std::string(kGiftStage));
        SceneGift gift;
        std::string why;
        REQUIRE(build_scene_gift(s, "Сцена", "сцены/Сцена.luau", files.scene(), gift, why));
        vector::SceneMotion m = gift.motion;
        REQUIRE(m.file.plays.size() == 1);
        m.file.plays[0].speed = 2;
        std::string w;
        CHECK_FALSE(check_scene_gift(s, m, w));
        CHECK(w == "в игре сцена разошлась бы со вкладкой: 0,033 с, «ёжик», клип и его миг");
    }
    {
        // While the scene stands at the wait, wave goes on; walk, called after the wait, begins when it ends. The nose's
        // turn from 0.2 s of wave is seen only while standing there.
        Scene s;
        load(s, files, R"(    spawn(actor("ёжик"):play("wave"))
    wait(0.1)
    wait_action("Поговорить")
    actor("ёжик"):play("walk"))",
             std::string(kGiftStage));
        SceneGift gift;
        std::string why;
        const bool ok = build_scene_gift(s, "Сцена", "сцены/Сцена.luau", files.scene(), gift, why);
        INFO(why);
        REQUIRE(ok);
        vector::SceneMotion m = gift.motion;
        vector::MotionClip* wave = nullptr;
        for (vector::MotionClip& c : m.actors[0].clips)
            if (c.name == "wave") wave = &c;
        REQUIRE(wave);
        vector::MotionTrack* rot = nullptr;
        for (vector::MotionTrack& t : wave->tracks)
            if (t.part == "ёжик/nose" && t.prop == vector::MotionProp::Rot) rot = &t;
        REQUIRE(rot);
        vector::MotionPiece hold;
        hold.kind = vector::MotionPiece::Kind::Hold;
        hold.at = 0.2;
        hold.value.v = 99;
        rot->pieces.push_back(hold);
        std::string w;
        CHECK_FALSE(check_scene_gift(s, m, w));
        CHECK(w == "в игре сцена разошлась бы со вкладкой: 0,1 с у ожидания 1 через 0,25 с, «ёжик/nose», поворот");
    }
}

TEST_CASE("Сцена в игре: что игра не нарисует") {
    Files files;
    project(files);
    Scene s;
    // A blur; a mask by an actor. A mask by a part of the same actor, or by a shape of the stage, the game draws.
    load(s, files, R"(    actor("ёжик"):play("walk")
    part("ёжик/head"):to({ blur = 2 }, 0.5)
    part("лес"):mask("ёжик")
    part("ёжик"):mask("пол")
    part("ёжик/nose"):mask("ёжик/head"))",
         std::string(kGiftStage));
    SceneGift gift;
    std::string why;
    const bool ok = build_scene_gift(s, "Сцена", "сцены/Сцена.luau", files.scene(), gift, why);
    INFO(why);
    REQUIRE(ok);
    CHECK(gift.motion.file.not_drawn == std::vector<std::string>{"blur", "mask_by_item"});
    REQUIRE(gift.notes.size() == 1);
    CHECK(gift.notes[0] == "В игре не будет: размытие, маска по актёру или картинке");
}

namespace {

namespace fs = std::filesystem;

// An empty folder among the system's temporary ones.
fs::path empty_dir(const char* name) {
    const fs::path dir = fs::temp_directory_path() / utf8_path(name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

void put_file(const fs::path& path, std::string_view bytes) {
    REQUIRE(write_file_atomic(path, std::span(reinterpret_cast<const u8*>(bytes.data()), bytes.size())));
}

SceneGift gift_of(Scene& s, Files& files, std::string_view name = "Вступление") {
    SceneGift gift;
    std::string why;
    const bool ok = build_scene_gift(s, name, "сцены/" + std::string(name) + ".luau", files.scene(), gift, why);
    INFO(why);
    REQUIRE(ok);
    return gift;
}

// «Отдать игре»: the scene's folder game_dir/scenes/<name>/ with the gift's files.
void give(const SceneGift& gift, const fs::path& game_dir, std::string_view name) {
    const fs::path dir = game_dir / "scenes" / utf8_path(std::string(name));
    for (const SceneGift::File& f : gift.files) put_file(dir / utf8_path(f.path), f.bytes);
}

bool near(f64 a, f64 b, f64 eps) { return std::abs(a - b) <= eps; }

// A sky, a picture on the floor and two copies of it, a hedgehog going along the road with its shadow attached to it, a
// beetle and the floor; lines 3–11, the scene's from 14.
const char* kPlaceStage = R"(    canvas(160, 120)
    background("#203040")
    rect("небо", { x = 0, y = 0, w = 160, h = 40, fill = "#80c0ff" })
    picture("лес", "картинки/лес.png", { x = 8, y = 60 })
    path("дорога", { d = "M 40 100 L 120 100", fill = "none", stroke = "#ffffff", hidden = true })
    actor("ёжик", "персонажи/Ёжик", { x = 40, y = 100, pivot = { 32, 60 } })
    ellipse("тень", { x = 40, y = 104, rx = 12, ry = 3, fill = "#000000" })
    actor("жук", "персонажи/Жук", { x = 120, y = 90, scale = 0.5, rot = 15, pivot = { 24, 16 } })
    rect("пол", { x = 0, y = 110, w = 160, h = 10, fill = "#405060" }))";

// Its marks (a sound and messages of its own, the hedgehog's steps and landings: one at the end of each round), music
// coming in and going out, the hedgehog's walk lifting its head 6 above its canvas, the beetle's hop with its swap twice,
// the copies going out for 0.5 s before the rest, the camera coming closer and turning, the sky going to the top at 1.5 s:
// 3.4 s.
const char* kPlaceScene = R"(    sound("бум")
    emit("начало")
    music("тема", { fade = 1 })
    actor("ёжик"):loop("walk", { params = { height = 20 } })
    spawn(actor("жук"):play("hop"))
    spawn(part("жук/eye").x(3, 1))
    part("тень"):attach("ёжик")
    part("лес"):attach("пол")
    copies("лес", 2, function(i, c) c.x(i * 30, 0.5) end)
    spawn(part("пол").x(10, 1))
    spawn(part("ёжик"):along("дорога", 2))
    camera():to({ x = 10, y = -5, zoom = 1.5, rot = 10 }, 1)
    part("небо"):to_top()
    emit("середина")
    sound("тишина")
    actor("ёжик"):loop("land")
    wait(1)
    music(nil, { fade = 0.5 })
    actor("жук"):play("hop", { speed = 2 })
    wait(0.5))";

const vector::MotionPartPose* pose_of(const std::vector<vector::MotionPartPose>& parts, std::string_view id) {
    for (const vector::MotionPartPose& p : parts)
        if (p.id == id) return &p;
    return nullptr;
}

ui::SvgPose stage_pose(const vector::ScenePose& pose) {
    ui::SvgPose out;
    out.picture = "stage.svg";
    out.parts = pose.stage;
    out.copies = pose.copies;
    out.order = pose.order;
    return out;
}

bool opaque(const std::vector<u8>& rgba, u32 w, u32 x, u32 y) { return rgba[(static_cast<usize>(y) * w + x) * 4 + 3] > 200; }
bool clear(const std::vector<u8>& rgba, u32 w, u32 x, u32 y) { return rgba[(static_cast<usize>(y) * w + x) * 4 + 3] == 0; }

} // namespace

TEST_CASE("Сцена в игре: папка сцены читается целиком, ошибка называет файл; сцены игры; звуки по имени") {
    Files files;
    project(files);
    Scene s;
    load(s, files, kGiftScene, kGiftStage);
    const SceneGift gift = gift_of(s, files);
    const fs::path game_dir = empty_dir("forge_tests_папка_сцены");
    give(gift, game_dir, "Вступление");
    vector::SceneMotion read;
    std::string why;
    const bool ok = game::read_scene_folder(game_dir / "scenes" / utf8_path("Вступление"), read, why);
    INFO(why);
    REQUIRE(ok);
    CHECK(read.file == gift.motion.file);
    CHECK(read.stage == gift.motion.stage);
    CHECK(read.actors == gift.motion.actors);

    // Each file missing or broken: the error names it (and the actor or picture it is of).
    struct Broken {
        const char* file;
        const char* bytes; // null: not there
        const char* said;
    };
    const Broken broken[] = {
        {"scene.json", nullptr, "нет «scene.json»"},
        {"scene.json", "{", "scene.json: "},
        {"stage.motion.json", nullptr, "нет «stage.motion.json»"},
        {"stage.motion.json", "{}", "stage.motion.json: "},
        {"stage.svg", nullptr, "нет «stage.svg»"},
        {"actors/ёжик.svg", nullptr, "актёр «ёжик»: нет «actors/ёжик.svg»"},
        {"actors/жук.motion.json", nullptr, "актёр «жук»: нет «actors/жук.motion.json»"},
        {"actors/жук.motion.json", "{}", "актёр «жук»: «actors/жук.motion.json»: "},
        {"actors/жук.flat.svg", nullptr, "актёр «жук»: нет «actors/жук.flat.svg»"},
        {"pictures/лес.png", nullptr, "картинка «лес»: нет «pictures/лес.png»"},
    };
    for (const Broken& b : broken) {
        CAPTURE(b.file);
        CAPTURE(b.bytes);
        give(gift, game_dir, "Сломанная");
        const fs::path dir = game_dir / "scenes" / utf8_path("Сломанная");
        if (b.bytes) put_file(dir / utf8_path(b.file), b.bytes);
        else REQUIRE(fs::remove(dir / utf8_path(b.file)));
        vector::SceneMotion out;
        CHECK_FALSE(game::read_scene_folder(dir, out, why));
        INFO(why);
        CHECK(why.rfind(b.said, 0) == 0);
    }
    // A clip of an actor's file that the scene plays and the file has not.
    give(gift, game_dir, "Сломанная");
    vector::MotionFile beetle = gift.motion.actors[1];
    REQUIRE(beetle.clips.size() == 1);
    beetle.clips[0].name = "jump";
    put_file(game_dir / "scenes" / utf8_path("Сломанная") / utf8_path("actors/жук.motion.json"), vector::write_motion_file(beetle));
    vector::SceneMotion out;
    CHECK_FALSE(game::read_scene_folder(game_dir / "scenes" / utf8_path("Сломанная"), out, why));
    CHECK(has(why, "hop"));

    // The game's scenes: its folders with a scene.json, by name.
    std::error_code ec;
    fs::create_directories(game_dir / "scenes" / utf8_path("Пустая"), ec);
    put_file(game_dir / "scenes" / utf8_path("заметка.txt"), "не сцена");
    CHECK(game::scene_names(game_dir) == std::vector<std::string>{"Вступление", "Сломанная"});
    CHECK(game::scene_names(game_dir / "нет").empty());

    // A sound by name as «Векторная» finds it: the name, else .wav, else .ogg; nothing out of the folder.
    const fs::path sounds = game_dir / "sounds";
    for (const char* f : {"бум.ogg", "шаг.wav", "шаг.ogg", "всплеск"}) put_file(sounds / utf8_path(f), "звук");
    CHECK(game::scene_sound_file(sounds, "бум") == "бум.ogg");
    CHECK(game::scene_sound_file(sounds, "шаг") == "шаг.wav");
    CHECK(game::scene_sound_file(sounds, "шаг.ogg") == "шаг.ogg");
    CHECK(game::scene_sound_file(sounds, "всплеск") == "всплеск");
    CHECK(game::scene_sound_file(sounds, "тишина").empty());
    CHECK(game::scene_sound_file(sounds, "").empty());
    CHECK(game::scene_sound_file(sounds, "../sounds/бум.ogg").empty());
    CHECK(game::scene_sound_file(sounds, "x\\бум.ogg").empty());
    CHECK(game::scene_sound_file({}, "бум").empty());
    fs::remove_all(game_dir, ec);
}

TEST_CASE("Сцена в игре: метки по файлам — те же, что у вкладки; в смене рисунка — части рисунка, из которого он меняется") {
    Files files;
    project(files);
    Scene s;
    load(s, files, kPlaceScene, kPlaceStage);
    const SceneGift gift = gift_of(s, files);
    const vector::SceneMotion& m = gift.motion;
    CHECK(s.recording()->length == doctest::Approx(3.4));
    std::vector<Mark> tab;
    REQUIRE(s.marks(tab));
    std::vector<vector::SceneMark> game;
    std::string why;
    REQUIRE(vector::scene_marks(m, game, why));
    REQUIRE(game.size() == tab.size());
    for (usize i = 0; i < tab.size(); ++i) {
        CAPTURE(i);
        CHECK(game[i].name == tab[i].name);
        CHECK(game[i].sound == (tab[i].kind == Mark::Kind::Sound));
        CHECK(game[i].actor == tab[i].actor);
        CHECK(game[i].at == doctest::Approx(tab[i].time));
    }
    // By hand: the scene's own before the actors' at one moment, the walk's steps up to the land's loop (not at it), a
    // landing at the end of each round of it, none past the scene's end.
    std::vector<std::string> said;
    for (const vector::SceneMark& mk : game) said.push_back(mk.name + (mk.actor.empty() ? "" : " " + mk.actor));
    CHECK(said == std::vector<std::string>{"бум", "начало", "шаг ёжик", "шаг ёжик", "шаг ёжик", "середина", "тишина", "земля ёжик", "земля ёжик",
                                           "земля ёжик"});
    const f64 at[] = {0, 0, 0.25, 0.75, 1.25, 1.5, 1.5, 2, 2.5, 3};
    for (usize i = 0; i < game.size(); ++i) CHECK(game[i].at == doctest::Approx(at[i]));
    // A loop too short gives too many.
    vector::SceneMotion fast = m;
    for (vector::ScenePlay& p : fast.file.plays)
        if (p.clip == "land") p.speed = 1e5;
    CHECK_FALSE(vector::scene_marks(fast, game, why));
    CHECK(why == "актёр «ёжик»: меток клипов больше 100000 — клип по кругу слишком короткий");

    // The beetle's swap to its flat drawing: the parts as they stand in each drawing, the tab's, the scene's move of its
    // eye over the clip in both.
    const usize beetle = 1;
    REQUIRE(m.file.actors[beetle].id == "жук");
    usize swaps = 0;
    for (const f64 t : {0.35, 0.4, 0.45, 2.7}) {
        CAPTURE(t);
        REQUIRE(s.frame(t));
        const vector::ScenePose pose = vector::scene_pose(m, t);
        const vector::SceneActorPose& ap = pose.actors[beetle];
        REQUIRE(ap.shown.swap);
        CHECK(ap.shown.from == vector::kMotionMainDrawing);
        CHECK(ap.shown.drawing == "flat");
        const Rig* main = s.actor_rig("жук");
        REQUIRE(main);
        const Rig* flat = main->drawing("flat");
        REQUIRE(flat);
        for (const char* id : {"жук/shell", "жук/eye"}) {
            CAPTURE(id);
            const vector::MotionPartPose* from = pose_of(ap.from_parts, id);
            const vector::MotionPartPose* to = pose_of(ap.parts, id);
            REQUIRE(from);
            REQUIRE(to);
            REQUIRE(from->moves);
            REQUIRE(to->moves);
            const Affine tab_from = main->matrix(main->part(id)), tab_to = flat->matrix(flat->part(id));
            for (const auto& [game_m, tab_m] : {std::pair{from->local, tab_from}, std::pair{to->local, tab_to}}) {
                CHECK(near(game_m.a, tab_m.a, 1e-6));
                CHECK(near(game_m.b, tab_m.b, 1e-6));
                CHECK(near(game_m.c, tab_m.c, 1e-6));
                CHECK(near(game_m.d, tab_m.d, 1e-6));
                CHECK(near(game_m.e, tab_m.e, 1e-4));
                CHECK(near(game_m.f, tab_m.f, 1e-4));
            }
        }
        ++swaps;
    }
    CHECK(swaps == 4);
    // Out of a swap there are none.
    CHECK(vector::scene_pose(m, 0.1).actors[beetle].from_parts.empty());

    // A mark at the end of «hop»: its second play, twice as fast, says it at half its moment, here the scene's very end.
    vector::SceneMotion clicks = m;
    std::vector<const vector::ScenePlay*> hops;
    for (const vector::ScenePlay& p : clicks.file.plays)
        if (p.actor == "жук" && p.kind == vector::ScenePlay::Kind::Play) hops.push_back(&p);
    REQUIRE(hops.size() == 2);
    CHECK(hops[1]->speed == 2);
    const auto hop = std::find_if(clicks.actors[beetle].clips.begin(), clicks.actors[beetle].clips.end(),
                                  [](const vector::MotionClip& c) { return c.name == "hop"; });
    REQUIRE(hop != clicks.actors[beetle].clips.end());
    REQUIRE(hops[0]->at + hop->length < hops[1]->at);
    hop->marks.push_back({false, "щелчок", hop->length, 0});
    // On the nanoseconds the moments of a scene's file are counted in.
    auto ns = [](f64 t) { return std::round(t * 1e9) / 1e9; };
    clicks.file.length = ns(ns(hops[1]->at) + hop->length / 2);
    REQUIRE(vector::scene_marks(clicks, game, why));
    std::vector<f64> clicked;
    for (const vector::SceneMark& mk : game)
        if (mk.name == "щелчок") {
            CHECK(mk.actor == "жук");
            clicked.push_back(mk.at);
        }
    REQUIRE(clicked.size() == 2);
    CHECK(clicked[0] == doctest::Approx(hops[0]->at + hop->length));
    CHECK(clicked[1] == clicks.file.length);
}

TEST_CASE("Сцена в игре: места картинок, копий и актёров в рисунке сцены, камера — как во вкладке; пробег рисует только своё") {
    Files files;
    project(files);
    Scene s;
    load(s, files, kPlaceScene, kPlaceStage);
    const SceneGift gift = gift_of(s, files);
    const vector::SceneMotion& m = gift.motion;
    const std::string& stage_svg = *gift_file(gift, "stage.svg");
    ui::SvgPlaces places;
    std::string why;
    REQUIRE(places.load("/сцена/stage.svg", stage_svg, why));
    std::vector<std::string> items;
    for (const vector::ScenePicture& p : m.file.pictures) items.push_back(p.id);
    for (const vector::SceneActor& a : m.file.actors) items.push_back(a.id);
    std::vector<f64> moments;
    for (int k = 0; k <= 68; ++k) moments.push_back(k * 0.05);
    for (int k = 0; k <= 125; ++k) moments.push_back(k / 37.0);
    usize copies = 0;
    for (const f64 t : moments) {
        CAPTURE(t);
        REQUIRE(s.frame(t));
        const vector::ScenePose pose = vector::scene_pose(m, t);
        usize found = 0;
        for (const ui::SvgPlace& p : places.place(stage_pose(pose))) {
            if (p.key.empty() || std::find(items.begin(), items.end(), p.id) == items.end()) continue;
            CAPTURE(p.key);
            const i32 part = p.key[0] == 'c' ? s.rig().copy(p.key.substr(1)) : s.rig().part(p.id);
            REQUIRE(part >= 0);
            const Affine tab = s.rig().global_matrix(part);
            // A path the file keeps frame by frame the game mixes between its frames: within a hundredth of a unit.
            INFO("game ", p.m.a, " ", p.m.b, " ", p.m.c, " ", p.m.d, " ", p.m.e, " ", p.m.f, "; tab ", tab.a, " ", tab.b, " ", tab.c, " ", tab.d, " ", tab.e,
                 " ", tab.f);
            CHECK(near(p.m.a, tab.a, 1e-4));
            CHECK(near(p.m.b, tab.b, 1e-4));
            CHECK(near(p.m.c, tab.c, 1e-4));
            CHECK(near(p.m.d, tab.d, 1e-4));
            CHECK(near(p.m.e, tab.e, 1e-2));
            CHECK(near(p.m.f, tab.f, 1e-2));
            CHECK(p.shown);
            CHECK(p.opacity == 1);
            copies += p.key[0] == 'c';
            ++found;
        }
        CHECK(found == items.size() + 2); // and the two copies of the picture
        const vector::MotionMatrix cam = vector::scene_camera_matrix(pose.camera, m.file.width, m.file.height);
        const Affine tab_cam = s.camera_matrix();
        CHECK(near(cam.a, tab_cam.a, 1e-6));
        CHECK(near(cam.b, tab_cam.b, 1e-6));
        CHECK(near(cam.c, tab_cam.c, 1e-6));
        CHECK(near(cam.d, tab_cam.d, 1e-6));
        CHECK(near(cam.e, tab_cam.e, 1e-4));
        CHECK(near(cam.f, tab_cam.f, 1e-4));
    }
    CHECK(copies == 2 * moments.size());
    // The camera came closer and turned.
    REQUIRE(s.frame(1.5));
    CHECK(s.camera_matrix().a == doctest::Approx(1.5 * std::cos(10 * 3.14159265358979 / 180)));

    // A run draws only its own elements of the stage's top level: the sky alone, then the shadow alone where the tab has
    // it, gone along with the hedgehog it hangs on (hidden in the run, standing where it is).
    const vector::SvgOutline o = vector::scan_svg(stage_svg);
    auto key = [&](const char* id) {
        const i32 e = o.find(id);
        REQUIRE(e > 0);
        return "e" + std::to_string(e);
    };
    REQUIRE(s.frame(2));
    const ui::SvgPose stage = stage_pose(vector::scene_pose(m, 2));
    places.place(stage);
    const ui::SvgPose sky = places.run_pose(stage, {key("небо")});
    CHECK(sky.only == std::vector<std::string>{key("небо")});
    CHECK_FALSE(pose_of(sky.parts, "ёжик"));
    std::vector<u8> rgba;
    REQUIRE(ui::render_posed_svg(stage_svg, sky, 160, 120, rgba, why));
    CHECK(opaque(rgba, 160, 80, 10));
    CHECK(clear(rgba, 160, 80, 115)); // the floor
    const ui::SvgPose shadow = places.run_pose(stage, {key("тень")});
    CHECK(pose_of(shadow.parts, "ёжик")); // what it hangs on
    REQUIRE(ui::render_posed_svg(stage_svg, shadow, 160, 120, rgba, why));
    f64 b[4];
    s.rig().bounds(s.rig().part("тень"), Affine{}, b);
    CHECK(b[0] > 50); // gone with the hedgehog from 40
    CHECK(opaque(rgba, 160, static_cast<u32>((b[0] + b[2]) / 2), static_cast<u32>((b[1] + b[3]) / 2)));
    CHECK(clear(rgba, 160, 40, 104)); // where it was at rest
    CHECK(clear(rgba, 160, 80, 10));  // the sky
    CHECK(clear(rgba, 160, 80, 115)); // the floor
}

TEST_CASE("Сцена в игре: рамка актёра — куда дотягивается его рисунок в сцене, целыми наружу") {
    Files files;
    project(files);
    Scene s;
    load(s, files, kPlaceScene, kPlaceStage);
    const SceneGift gift = gift_of(s, files);
    const vector::SceneFile& f = gift.motion.file;
    // The walk lifts the hedgehog's head (r 14 round 32, 28) by 20: to -6; the beetle's hop stays inside its canvas.
    CHECK(f.actor("ёжик")->reach == vector::SceneBox{0, -6, 64, 70});
    CHECK(f.actor("жук")->reach == vector::SceneBox{});
    CHECK(has(*gift_file(gift, "scene.json"), "\"size\": [64, 64], \"reach\": [0, -6, 64, 70]}"));
    CHECK_FALSE(has(*gift_file(gift, "scene.json"), "\"size\": [48, 32], \"reach\""));
    REQUIRE(s.frame(0.25));
    f64 r[4];
    REQUIRE(s.actor_reach("ёжик", r));
    CHECK(r[0] == doctest::Approx(18));
    CHECK(r[1] == doctest::Approx(-6));
    CHECK(r[2] == doctest::Approx(46));
    CHECK(r[3] == doctest::Approx(58));
    CHECK_FALSE(s.actor_reach("лиса", r));
    // The tab does not cut it at its canvas either: the head above it is drawn (over the sky).
    {
        const assets::CookedTexture frame = s.draw(160, 120);
        const vector::Point above = (s.rig().to_pixels(160, 120) * s.rig().global_matrix(s.rig().part("ёжик"))).apply({32, -3});
        CAPTURE(above.x);
        CAPTURE(above.y);
        REQUIRE(above.x >= 0);
        REQUIRE(above.y >= 0);
        CHECK(near(pixel_at(frame, static_cast<u32>(above.x), static_cast<u32>(above.y)), Rgba8{0xc0, 0x90, 0x60, 255}));
    }
    // The game draws it in that box: the head above its canvas is there.
    const vector::ScenePose pose = vector::scene_pose(gift.motion, 0.25);
    ui::SvgPose hog;
    hog.picture = "ёжик.svg";
    hog.parts = pose.actors[0].parts;
    std::vector<u8> own, wide;
    std::string why;
    REQUIRE(ui::render_posed_svg(*gift_file(gift, "actors/ёжик.svg"), hog, 64, 64, own, why));
    hog.view_x = 0, hog.view_y = -6, hog.view_w = 64, hog.view_h = 70;
    REQUIRE(ui::render_posed_svg(*gift_file(gift, "actors/ёжик.svg"), hog, 64, 70, wide, why));
    CHECK(opaque(wide, 64, 32, 1)); // y -5 of the picture
    CHECK(clear(wide, 64, 32, 69)); // y 63: below the feet
    for (const u32 y : {0u, 10u, 30u, 52u}) {
        CAPTURE(y);
        int worst = 0;
        for (usize i = 0; i < 64 * 4; ++i)
            worst = std::max(worst, std::abs(static_cast<int>(own[static_cast<usize>(y) * 64 * 4 + i]) - static_cast<int>(wide[static_cast<usize>(y + 6) * 64 * 4 + i])));
        CHECK(worst <= 2);
    }
    // A broken box does not read.
    std::string text = *gift_file(gift, "scene.json");
    text.replace(text.find("[0, -6, 64, 70]"), 15, "[0, -6, 0, 70]");
    vector::SceneFile back;
    CHECK_FALSE(vector::read_scene_file(text, back, why));
    CHECK(why == "actors[0]: reach: нужна рамка рисунка в сцене [x, y, w, h], w и h больше 0");
}

namespace {

// Files from disk by their UTF-8 names, read whole (Cyrillic paths on Windows too).
struct SceneFilesOnDisk final : Rml::FileInterface {
    struct Opened {
        std::vector<u8> bytes;
        usize at = 0;
    };
    Rml::FileHandle Open(const Rml::String& path) override {
        auto* f = new Opened;
        if (!read_file(utf8_path(path), f->bytes)) {
            delete f;
            return 0;
        }
        return reinterpret_cast<Rml::FileHandle>(f);
    }
    void Close(Rml::FileHandle file) override { delete reinterpret_cast<Opened*>(file); }
    size_t Read(void* buffer, size_t size, Rml::FileHandle file) override {
        auto* f = reinterpret_cast<Opened*>(file);
        const usize n = std::min(size, f->bytes.size() - f->at);
        if (n) std::memcpy(buffer, f->bytes.data() + f->at, n);
        f->at += n;
        return n;
    }
    bool Seek(Rml::FileHandle file, long offset, int origin) override {
        auto* f = reinterpret_cast<Opened*>(file);
        const long base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? static_cast<long>(f->at) : static_cast<long>(f->bytes.size());
        if (base + offset < 0 || base + offset > static_cast<long>(f->bytes.size())) return false;
        f->at = static_cast<usize>(base + offset);
        return true;
    }
    size_t Tell(Rml::FileHandle file) override { return reinterpret_cast<Opened*>(file)->at; }
};

// A renderer that draws nothing: the scene's document is only laid out and posed.
struct SceneNoRender final : Rml::RenderInterface {
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override { return 1; }
    void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override {}
    void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
    Rml::TextureHandle LoadTexture(Rml::Vector2i& size, const Rml::String&) override {
        size = {40, 30};
        return 1;
    }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override { return 1; }
    void ReleaseTexture(Rml::TextureHandle) override {}
    void EnableScissorRegion(bool) override {}
    void SetScissorRegion(Rml::Rectanglei) override {}
};

} // namespace

TEST_CASE("Сцена в игре: на экране игры — слои, метки по кадрам, музыка, конец, пропуск и сброс") {
    Files files;
    project(files);
    Scene s;
    load(s, files, kPlaceScene, kPlaceStage);
    const SceneGift gift = gift_of(s, files);
    const vector::SceneMotion& m = gift.motion;
    std::vector<Mark> marks;
    REQUIRE(s.marks(marks));
    const fs::path game_dir = empty_dir("forge_tests_сцена_на_экране");
    give(gift, game_dir, "Вступление");
    put_file(game_dir / "sounds" / utf8_path("бум.ogg"), "звук");
    put_file(game_dir / "sounds" / utf8_path("тема.ogg"), "музыка");
    const vector::SvgOutline o = vector::scan_svg(*gift_file(gift, "stage.svg"));
    auto key = [&](const char* id) {
        const i32 e = o.find(id);
        REQUIRE(e > 0);
        return "e" + std::to_string(e);
    };

    SceneNoRender render;
    SceneFilesOnDisk disk;
    Rml::SetRenderInterface(&render);
    Rml::SetFileInterface(&disk);
    REQUIRE(Rml::Initialise());
    ui::install_live_svg();
    Rml::Context* context = Rml::CreateContext("scene", {400, 240});
    REQUIRE(context);
    {
        game::GameScene scene;
        std::vector<std::string> heard;
        std::vector<f64> heard_at;
        scene.on_message = [&](const std::string& name) {
            heard.push_back("сообщение " + name);
            heard_at.push_back(scene.time());
        };
        scene.on_sound = [&](const std::string& file) {
            heard.push_back("звук " + file);
            heard_at.push_back(scene.time());
        };
        std::vector<game::SceneTune> tunes;
        f64 share = 1;
        scene.on_music = [&](const std::vector<game::SceneTune>& t, f64 g) {
            tunes = t;
            share = g;
        };
        struct End {
            std::string name;
            bool skipped = false;
            u64 serial = 0;
        };
        std::vector<End> ends;
        scene.on_end = [&](const std::string& name, bool skipped, u64 serial) { ends.push_back({name, skipped, serial}); };

        std::string why;
        for (const std::string bad : {"Нет", "", "..", "сцены/Вступление", "C:Вступление"}) {
            CAPTURE(bad);
            CHECK_FALSE(scene.play(context, game_dir, bad, {}, why));
            CHECK(why == "нет сцены «" + bad + "»: отдайте её игре во вкладке «Векторная»");
        }
        give(gift, game_dir, "Сломанная");
        std::error_code ec;
        fs::remove(game_dir / "scenes" / utf8_path("Сломанная") / "stage.svg", ec);
        CHECK_FALSE(scene.play(context, game_dir, "Сломанная", {}, why));
        CHECK(why == "сцена «Сломанная»: нет «stage.svg»");
        CHECK_FALSE(scene.going());
        CHECK(scene.document() == nullptr);

        // The expected heard, in order: the tab's marks, sounds of the game's sounds by name, none missing.
        std::vector<std::string> want;
        std::vector<f64> want_at;
        for (const Mark& mk : marks) {
            if (mk.kind == Mark::Kind::Sound && mk.name != "бум") continue;
            want.push_back(mk.kind == Mark::Kind::Sound ? "звук " + mk.name + ".ogg" : "сообщение " + mk.name);
            want_at.push_back(mk.time);
        }
        REQUIRE(want.size() == 9);

        // To the end at a frame rate: what it says comes in the frame its moment passes.
        auto play_through = [&](f64 dt, u64 serial) {
            CAPTURE(dt);
            heard.clear();
            heard_at.clear();
            REQUIRE(scene.play(context, game_dir, "Вступление", {true, false}, why));
            CHECK(scene.serial() == serial);
            CHECK(scene.going());
            CHECK_FALSE(scene.over(serial));
            CHECK(scene.name() == "Вступление");
            CHECK_FALSE(scene.how().skip);
            CHECK_FALSE(scene.play(context, game_dir, "Вступление", {}, why));
            CHECK(why == "идёт сцена «Вступление»");
            scene.update(0.5, 400, 240); // the first frame shows its moment 0
            CHECK(scene.time() == 0);
            u64 still = 0;
            for (int frame = 1; scene.going() && frame < 1000; ++frame) {
                scene.update(dt, 400, 240);
                const f64 t = scene.time();
                if (!scene.going()) break;
                // Its music now and the game's under it.
                std::vector<game::SceneTune> now;
                for (const vector::SceneMusicVoice& v : vector::scene_music_at(m.file, t)) now.push_back({v.call, v.name + ".ogg", v.since, v.volume});
                CHECK(tunes == now);
                CHECK(share == doctest::Approx(std::clamp(1 - t, 0.0, 1.0)));
                // Past 2.5 s nothing of the stage moves: its runs are not posed again.
                if (t > 2.55 && !still) still = scene.run_poses();
                if (still) CHECK(scene.run_poses() == still);
            }
            CHECK_FALSE(scene.going());
            CHECK(scene.over(serial));
            CHECK_FALSE(scene.skipped(serial));
            // Its last frame stays till the game's logic has run after its end (part 6г4; untold, the next update), then
            // it goes.
            CHECK(scene.document() != nullptr);
            REQUIRE(!ends.empty());
            CHECK(ends.back().name == "Вступление");
            CHECK_FALSE(ends.back().skipped);
            CHECK(ends.back().serial == serial);
            CHECK(heard == want);
            REQUIRE(heard_at.size() == want_at.size());
            for (usize i = 0; i < want_at.size(); ++i) {
                CAPTURE(heard[i]);
                CHECK(heard_at[i] >= want_at[i] - 1e-9);
                CHECK(heard_at[i] < want_at[i] + dt + 1e-9);
            }
            CHECK(still > 0);
            // Its music goes out over a second while the game's comes back.
            CHECK(share == 0);
            for (int k = 1; k <= 60; ++k) {
                scene.update(1.0 / 60, 400, 240);
                CHECK(share == doctest::Approx(k / 60.0));
                if (k == 1) CHECK(scene.document() == nullptr);
            }
            scene.update(1.0 / 60, 400, 240);
            CHECK(tunes.empty());
            CHECK(share == 1);
        };
        play_through(1.0 / 60, 1);
        play_through(1.0 / 37, 2);
        const std::vector<std::string> problems = scene.problems();
        CHECK(std::count(problems.begin(), problems.end(), "сцена «Вступление»: звука «тишина» нет в звуках игры") == 1);

        // Its document: the camera's frame fitted to the screen, the background's colour all round, the layers in its
        // order; an actor in its frame, posed by its clip.
        REQUIRE(scene.play(context, game_dir, "Вступление", {true, true}, why));
        CHECK(scene.serial() == 3);
        scene.update(0, 400, 240);
        Rml::ElementDocument* doc = scene.document();
        REQUIRE(doc);
        context->Update();
        REQUIRE(doc->GetElementById("bg"));
        const Rml::Property* bg = doc->GetElementById("bg")->GetLocalProperty("background-color");
        REQUIRE(bg);
        CHECK(bg->Get<Rml::Colourb>() == Rml::Colourb(32, 48, 64, 255));
        Rml::Element* frame = doc->GetElementById("frame");
        Rml::Element* camera = doc->GetElementById("camera");
        REQUIRE(frame);
        REQUIRE(camera);
        CHECK(frame->GetAbsoluteLeft() == doctest::Approx(40));
        CHECK(frame->GetAbsoluteTop() == doctest::Approx(0));
        CHECK(frame->GetBox().GetSize().x == doctest::Approx(320));
        CHECK(frame->GetBox().GetSize().y == doctest::Approx(240));
        // The bars over it, the background's colour to the frame's edges.
        Rml::Element* bars = doc->GetElementById("bars");
        REQUIRE(bars);
        CHECK(bars->GetProperty<Rml::Colourb>("border-left-color") == Rml::Colourb(32, 48, 64, 255));
        CHECK(bars->GetBox().GetEdge(Rml::BoxArea::Border, Rml::BoxEdge::Left) == doctest::Approx(40));
        CHECK(bars->GetBox().GetEdge(Rml::BoxArea::Border, Rml::BoxEdge::Right) == doctest::Approx(40));
        CHECK(bars->GetBox().GetEdge(Rml::BoxArea::Border, Rml::BoxEdge::Top) == 0);
        CHECK(bars->GetBox().GetEdge(Rml::BoxArea::Border, Rml::BoxEdge::Bottom) == 0);
        CHECK(bars->GetBox().GetSize(Rml::BoxArea::Border).x == doctest::Approx(400));
        Rml::Element* look = doc->GetElementById("look");
        REQUIRE(look);
        CHECK(look->GetChild(look->GetNumChildren() - 1) == bars);
        const std::vector<std::string> first = {"run " + key("небо"),
                                                "picture лес " + key("лес"),
                                                "picture лес cлес[1]",
                                                "picture лес cлес[2]",
                                                "run " + key("дорога"),
                                                "actor ёжик",
                                                "run " + key("тень"),
                                                "actor жук",
                                                "run " + key("пол")};
        CHECK(scene.layers() == first);
        CHECK(camera->GetNumChildren() == static_cast<int>(first.size()));
        for (int k = 0; k < 15; ++k) scene.update(1.0 / 60, 400, 240);
        CHECK(scene.time() == doctest::Approx(0.25));
        Rml::Element* hog = layer_element(camera, 5);
        REQUIRE(hog);
        CHECK(hog->GetAttribute<Rml::String>("src", "") == "actors/ёжик.svg");
        CHECK(hog->GetLocalProperty("width")->Get<float>() == doctest::Approx(64));
        CHECK(hog->GetLocalProperty("height")->Get<float>() == doctest::Approx(70));
        const ui::SvgPose* posed = ui::svg_posed(hog);
        REQUIRE(posed);
        CHECK(posed->picture == "ёжик.svg");
        CHECK(posed->view_y == -6);
        CHECK(posed->view_h == 70);
        CHECK(game_value_of(posed->parts, "ёжик/head", vector::MotionProp::Y).v == doctest::Approx(-20));
        const ui::SvgPose* sky = ui::svg_posed(camera->GetChild(0));
        REQUIRE(sky);
        CHECK(sky->only == std::vector<std::string>{key("небо")});
        context->Update();
        context->Render();
        // Its layers cut at the frame's edge, though a transform moves them (the layout sees no overflow).
        Rml::Rectanglei cut;
        REQUIRE(Rml::ElementUtilities::GetClippingRegion(hog, cut));
        CHECK(cut.Position() == Rml::Vector2i(40, 0));
        CHECK(cut.Size() == Rml::Vector2i(320, 240));
        // At 0.35 s the beetle swaps to its flat drawing: its layer shows the swap, the parts of both drawings.
        for (int k = 0; k < 6; ++k) scene.update(1.0 / 60, 400, 240);
        CHECK(scene.time() == doctest::Approx(0.35));
        const ui::SvgPose* bug = ui::svg_posed(layer_element(camera, 7));
        REQUIRE(bug);
        CHECK(bug->picture == "жук.svg");
        CHECK(bug->swap);
        CHECK(bug->swap_from == vector::kMotionMainDrawing);
        CHECK(bug->drawing == "flat");
        CHECK(bug->swap_k > 0);
        CHECK(bug->swap_k < 1);
        CHECK(pose_of(bug->from_parts, "жук/shell"));
        // Its clock read to the nanosecond: 90 frames of 1/60 are 1.5, the moment a clip may begin (summed, 1.4999999999999996).
        for (int k = 0; k < 69; ++k) scene.update(1.0 / 60, 400, 240);
        CHECK(scene.time() == 1.5);
        // The sky to the top at 1.5 s: the floor's run and the sky's are one.
        for (int k = 0; k < 5; ++k) scene.update(1.0 / 60, 400, 240);
        CHECK(scene.time() > 1.5);
        CHECK(scene.layers() == std::vector<std::string>{"picture лес " + key("лес"), "picture лес cлес[1]", "picture лес cлес[2]",
                                                         "run " + key("дорога"), "actor ёжик", "run " + key("тень"), "actor жук",
                                                         "run " + key("пол") + " " + key("небо")});
        context->Update();
        context->Render();

        // Skipped: what it had left to say, in order, its sounds not; it ends, skipped.
        heard.clear();
        scene.skip();
        CHECK(heard == std::vector<std::string>{"сообщение земля", "сообщение земля", "сообщение земля"});
        CHECK_FALSE(scene.going());
        CHECK(scene.over(3));
        CHECK(scene.skipped(3));
        CHECK(ends.back().serial == 3);
        CHECK(ends.back().skipped);
        // Skipped between frames, its last frame stays till the game's logic has run (part 6г4; untold, an update is a
        // run), then it goes: nothing came in over it.
        CHECK(scene.document() != nullptr);
        scene.update(1.0 / 60, 400, 240);
        CHECK(scene.document() == nullptr);
        CHECK(scene.layers().empty());

        // Dropped: nothing said, no end; its music stops and the game's is back at once.
        REQUIRE(scene.play(context, game_dir, "Вступление", {}, why));
        for (int k = 0; k < 40; ++k) scene.update(1.0 / 60, 400, 240);
        CHECK_FALSE(tunes.empty());
        CHECK(share < 1);
        heard.clear();
        const usize ended = ends.size();
        scene.drop();
        CHECK(heard.empty());
        CHECK(ends.size() == ended);
        CHECK(scene.over(4));
        CHECK_FALSE(scene.skipped(4));
        CHECK(tunes.empty());
        CHECK(share == 1);
        CHECK(scene.document() == nullptr);
        scene.update(1.0 / 60, 400, 240);
        CHECK(tunes.empty());
    }
    Rml::Shutdown();
    std::error_code ec;
    fs::remove_all(game_dir, ec);
}

TEST_CASE("Сцена в игре: прозрачность картинки и актёра — у их слоёв") {
    Files files;
    project(files);
    Scene s;
    load(s, files, R"(    part("лес").opacity(0.25, 0)
    part("ёжик").opacity(0.5, 1, "linear")
    wait(1))",
         kPlaceStage);
    const SceneGift gift = gift_of(s, files, "Туман");
    const fs::path game_dir = empty_dir("forge_tests_сцена_прозрачность");
    give(gift, game_dir, "Туман");

    SceneNoRender render;
    SceneFilesOnDisk disk;
    Rml::SetRenderInterface(&render);
    Rml::SetFileInterface(&disk);
    REQUIRE(Rml::Initialise());
    ui::install_live_svg();
    Rml::Context* context = Rml::CreateContext("scene", {400, 240});
    REQUIRE(context);
    {
        game::GameScene scene;
        std::string why;
        REQUIRE(scene.play(context, game_dir, "Туман", {}, why));
        scene.update(0, 400, 240);
        REQUIRE(scene.document());
        Rml::Element* camera = scene.document()->GetElementById("camera");
        REQUIRE(camera);
        // A layer's opacity: its group's in the stage's picture at that moment, as the tab draws it.
        auto opacity = [&](const std::string& layer) {
            const std::vector<std::string> layers = scene.layers();
            for (usize i = 0; i < layers.size(); ++i)
                if (layers[i].rfind(layer, 0) == 0) {
                    const Rml::Property* p = layer_element(camera, static_cast<int>(i))->GetLocalProperty("opacity");
                    return p ? static_cast<f64>(p->Get<float>()) : -1.0;
                }
            return -2.0;
        };
        CHECK(opacity("picture лес") == doctest::Approx(0.25));
        CHECK(opacity("actor ёжик") == doctest::Approx(1));
        for (int k = 0; k < 30; ++k) scene.update(1.0 / 60, 400, 240);
        CHECK(scene.time() == doctest::Approx(0.5));
        CHECK(opacity("picture лес") == doctest::Approx(0.25));
        CHECK(opacity("actor ёжик") == doctest::Approx(0.75));
        CHECK(opacity("actor жук") == doctest::Approx(1));
    }
    Rml::Shutdown();
    std::error_code ec;
    fs::remove_all(game_dir, ec);
}

TEST_CASE("Сцена в игре: места — в единицах рамки рисунка, не его размера на экране") {
    // A picture drawn twice its box's size, its box moved: a group stands where its own units put it.
    ui::SvgPlaces places;
    std::string why;
    REQUIRE(places.load("/рамка/stage.svg",
                        R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100" viewBox="10 0 100 50">)svg"
                        R"svg(<g id="a" transform="translate(30 20)"><rect width="4" height="4"/></g></svg>)svg",
                        why));
    ui::SvgPose pose;
    pose.picture = "stage.svg";
    const std::vector<ui::SvgPlace>& top = places.place(pose);
    REQUIRE(top.size() == 1);
    CHECK(top[0].id == "a");
    CHECK(top[0].m.a == doctest::Approx(1));
    CHECK(top[0].m.b == doctest::Approx(0));
    CHECK(top[0].m.c == doctest::Approx(0));
    CHECK(top[0].m.d == doctest::Approx(1));
    CHECK(top[0].m.e == doctest::Approx(30));
    CHECK(top[0].m.f == doctest::Approx(20));
}

namespace {

// Part 6г3: a scene waiting for an action, any key and a talk at one moment and any key at its end, the hedgehog walking
// before the waits and landing from the first; the stage kPlaceStage's.
const char* kWaitScene = R"(    sound("бум")
    emit("начало")
    music("тема", { fade = 1 })
    actor("ёжик"):loop("walk")
    wait(1)
    emit("середина")
    wait_action("Поговорить")
    emit("ответ")
    actor("ёжик"):loop("land")
    wait_action()
    talk("Встреча")
    wait(1)
    sound("тишина")
    wait_action())";

// Where a scene is when it has lived `lived` seconds, standing at each stop as long as `wait` says (gone on when it has,
// to a nanosecond).
vector::SceneAt at_lived(const vector::SceneFile& f, std::span<const f64> wait, f64 lived) {
    vector::SceneAt a;
    a.held.assign(f.stops.size(), 0);
    f64 rest = lived;
    for (usize i = 0; i < f.stops.size(); ++i) {
        const f64 gap = f.stops[i].at - a.t;
        if (rest < gap) break;
        a.t = f.stops[i].at;
        rest -= gap;
        if (rest < wait[i] - 1e-9) {
            a.held[i] = rest;
            a.waiting = static_cast<i32>(i);
            return a;
        }
        a.held[i] = wait[i];
        rest = std::max(0.0, rest - wait[i]);
    }
    a.t = std::min(a.t + rest, f.length);
    return a;
}

std::vector<std::string> said(const std::vector<vector::SceneMark>& marks) {
    std::vector<std::string> out;
    for (const vector::SceneMark& m : marks) out.push_back(m.name + (m.actor.empty() ? "" : " " + m.actor));
    return out;
}

} // namespace

TEST_CASE("Сцена в игре: метки по прожитому времени — без ожиданий как у вкладки, с ожиданиями после тех, что сделаны до них") {
    Files files;
    project(files);
    Scene s;
    load(s, files, kWaitScene, kPlaceStage);
    const SceneGift gift = gift_of(s, files);
    const vector::SceneMotion& m = gift.motion;
    const vector::SceneFile& f = m.file;
    CHECK(f.length == doctest::Approx(2));
    REQUIRE(f.stops.size() == 4);
    const char* names[] = {"Поговорить", "", "Встреча", ""};
    for (usize i = 0; i < 4; ++i) {
        CAPTURE(i);
        CHECK(f.stops[i].name == names[i]);
        CHECK(f.stops[i].talk == (i == 2));
        CHECK(f.stops[i].at == doctest::Approx(i == 3 ? 2 : 1));
    }
    // The scene's own marks keep their place among its waits: «середина» before the first, «ответ» between the first two,
    // «тишина» before the last; in its file and back.
    const vector::MotionClip* own = m.stage.clip("scene");
    REQUIRE(own);
    std::map<std::string, u32> order;
    for (const vector::MotionMark& mk : own->marks) order[mk.name] = mk.order;
    REQUIRE(order.size() == 5);
    CHECK(order["середина"] <= f.stops[0].order);
    CHECK(order["ответ"] > f.stops[0].order);
    CHECK(order["ответ"] <= f.stops[1].order);
    CHECK(order["тишина"] > f.stops[2].order);
    CHECK(order["тишина"] <= f.stops[3].order);
    for (const Mark& mk : s.recording()->marks) CHECK(mk.order == order[mk.name]);
    const std::string& text = *gift_file(gift, "stage.motion.json");
    CHECK(has(text, "\"emit\": \"ответ\", \"at\": 1, \"order\": " + std::to_string(order["ответ"]) + "}"));
    vector::MotionFile back;
    std::string why;
    REQUIRE(vector::read_motion_file(text, back, why));
    CHECK(back == m.stage);
    std::string broken = text;
    const std::string tail = "\"order\": " + std::to_string(order["ответ"]) + "}";
    broken.replace(broken.find(tail), tail.size(), "\"order\": -1}");
    CHECK_FALSE(vector::read_motion_file(broken, back, why));
    CHECK(has(why, "clips.scene.marks["));
    CHECK(has(why, "]: order: нужен номер метки среди вызовов сцены, целое от 0"));
    broken = text;
    broken.replace(broken.find(tail), tail.size(), "\"order\": 1.5}");
    CHECK_FALSE(vector::read_motion_file(broken, back, why));
    CHECK(has(why, "order: нужен номер"));
    // A scene with no waits writes none.
    Scene plain;
    load(plain, files, kPlaceScene, kPlaceStage);
    for (const vector::MotionMark& mk : gift_of(plain, files).motion.stage.clip("scene")->marks) CHECK(mk.order == 0);

    // Without waits: the tab's marks at the tab's moments.
    std::vector<vector::SceneMark> tab, got;
    REQUIRE(vector::scene_marks(m, tab, why));
    CHECK(said(tab) == std::vector<std::string>{"бум", "начало", "шаг ёжик", "шаг ёжик", "середина", "ответ", "земля ёжик", "тишина", "земля ёжик"});
    const std::vector<f64> none(4, 0);
    REQUIRE(vector::scene_marks_between(m, nullptr, at_lived(f, none, 2), got, why));
    CHECK(got == tab);
    // With them (0.6 s for the action, 0.3 for the key, 0.4 the talk, 0.5 at the end): by the time it lived. The steps go
    // on while it waits up to the landing; the landing begins when the first wait is done, a landing comes in the talk and
    // one at the end's wait.
    const std::vector<f64> wait = {0.6, 0.3, 0.4, 0.5};
    const vector::SceneAt end = at_lived(f, wait, 3.8);
    CHECK(end.t == doctest::Approx(2));
    CHECK(end.waiting == -1);
    CHECK(vector::scene_lived(end) == doctest::Approx(3.8));
    REQUIRE(vector::scene_marks_between(m, nullptr, end, got, why));
    CHECK(said(got) == std::vector<std::string>{"бум", "начало", "шаг ёжик", "шаг ёжик", "середина", "шаг ёжик", "ответ", "земля ёжик", "земля ёжик",
                                                "земля ёжик", "тишина", "земля ёжик"});
    const f64 lived_at[] = {0, 0, 0.25, 0.75, 1, 1.25, 1.6, 2.1, 2.6, 3.1, 3.3, 3.6};
    REQUIRE(got.size() == std::size(lived_at));
    for (usize i = 0; i < got.size(); ++i) {
        CAPTURE(i);
        CHECK(got[i].at == doctest::Approx(lived_at[i]));
    }
    // A step of the walk at the very moment the landing begins (0.75 s at the first wait): the landing's time, not the walk's.
    const std::vector<f64> exact = {0.75, 0.3, 0.4, 0.5};
    REQUIRE(vector::scene_marks_between(m, nullptr, at_lived(f, exact, 3.95), got, why));
    CHECK(said(got) == std::vector<std::string>{"бум", "начало", "шаг ёжик", "шаг ёжик", "середина", "шаг ёжик", "ответ", "земля ёжик", "земля ёжик",
                                                "земля ёжик", "тишина", "земля ёжик"});
    REQUIRE(got.size() == 12);
    CHECK(got[6].at == doctest::Approx(1.75));
    CHECK(got[7].at == doctest::Approx(2.25));
    // Standing at the first wait 0.3 s: what was made after it has not begun, the steps go on.
    const vector::SceneAt first = at_lived(f, wait, 1.3);
    CHECK(first.waiting == 0);
    CHECK(first.held[0] == doctest::Approx(0.3));
    REQUIRE(vector::scene_marks_between(m, nullptr, first, got, why));
    CHECK(said(got) == std::vector<std::string>{"бум", "начало", "шаг ёжик", "шаг ёжик", "середина", "шаг ёжик"});
    // How long a thing has lived since it began: the music from 0, the landing from when the first wait was done.
    const u32 music = f.music[0].order;
    u32 land = 0;
    for (const vector::ScenePlay& p : f.plays)
        if (p.clip == "land") land = p.order;
    CHECK(vector::scene_since(f, first, 0, music) == doctest::Approx(1.3));
    CHECK(vector::scene_since(f, first, 1, land) == -1);
    const vector::SceneAt talk = at_lived(f, wait, 2.0);
    CHECK(talk.waiting == 2);
    CHECK(vector::scene_since(f, talk, 1, land) == doctest::Approx(0.4));
    CHECK(vector::scene_since(f, talk, 0, music) == doctest::Approx(2));

    // Frame by frame, from nothing at its start: each mark once, in the frame its lived time passes, the same in all.
    for (const std::vector<f64>* w : {&none, &wait})
        for (const f64 dt : {1.0 / 60, 1.0 / 37, 0.1, 0.35}) {
            CAPTURE(dt);
            CAPTURE((*w)[0]);
            const f64 last = 2 + (*w)[0] + (*w)[1] + (*w)[2] + (*w)[3];
            std::vector<vector::SceneMark> all, whole;
            REQUIRE(vector::scene_marks_between(m, nullptr, at_lived(f, *w, last), whole, why));
            vector::SceneAt was = at_lived(f, *w, 0);
            REQUIRE(vector::scene_marks_between(m, nullptr, was, got, why));
            CHECK(said(got) == std::vector<std::string>{"бум", "начало"});
            all = got;
            for (int k = 1;; ++k) {
                const f64 lived = std::min(k * dt, last);
                const vector::SceneAt now = at_lived(f, *w, lived);
                REQUIRE(vector::scene_marks_between(m, &was, now, got, why));
                for (const vector::SceneMark& mk : got) {
                    CAPTURE(mk.name);
                    CHECK(mk.at >= vector::scene_lived(was) - 1e-9);
                    CHECK(mk.at <= vector::scene_lived(now) + 1e-9);
                }
                all.insert(all.end(), got.begin(), got.end());
                was = now;
                if (lived >= last) break;
            }
            CHECK(all == whole);
            // Again from where it is: nothing.
            REQUIRE(vector::scene_marks_between(m, &was, was, got, why));
            CHECK(got.empty());
        }

    // A loop too short at once.
    vector::SceneMotion fast = m;
    for (vector::ScenePlay& p : fast.file.plays)
        if (p.clip == "land") p.speed = 1e5;
    CHECK_FALSE(vector::scene_marks_between(fast, nullptr, end, got, why));
    CHECK(why == "меток клипов за раз больше 100000 — клип по кругу слишком короткий");
}

TEST_CASE("Сцена в игре: ожидание и разговор на экране — время сцены стоит, клипы и музыка идут, пропуск закрывает разговор") {
    Files files;
    project(files);
    Scene s;
    load(s, files, kWaitScene, kPlaceStage);
    const SceneGift gift = gift_of(s, files);
    const vector::SceneMotion& m = gift.motion;
    const fs::path game_dir = empty_dir("forge_tests_сцена_ждёт");
    give(gift, game_dir, "Ожидание");
    Scene first;
    load(first, files, R"(    music("тема", { fade = 1 })
    wait_action()
    emit("сразу")
    talk("Нет")
    emit("потом")
    wait(0.5))",
         kPlaceStage);
    give(gift_of(first, files, "Сразу"), game_dir, "Сразу");
    put_file(game_dir / "sounds" / utf8_path("бум.ogg"), "звук");
    put_file(game_dir / "sounds" / utf8_path("тема.ogg"), "музыка");

    SceneNoRender render;
    SceneFilesOnDisk disk;
    Rml::SetRenderInterface(&render);
    Rml::SetFileInterface(&disk);
    REQUIRE(Rml::Initialise());
    ui::install_live_svg();
    Rml::Context* context = Rml::CreateContext("scene", {400, 240});
    REQUIRE(context);
    {
        game::GameScene scene;
        std::vector<std::string> heard;
        std::vector<f64> heard_at; // the time it lived then
        scene.on_message = [&](const std::string& name) {
            heard.push_back("сообщение " + name);
            heard_at.push_back(scene.lived());
        };
        scene.on_sound = [&](const std::string& file) {
            heard.push_back("звук " + file);
            heard_at.push_back(scene.lived());
        };
        std::vector<game::SceneTune> tunes;
        f64 share = 1;
        scene.on_music = [&](const std::vector<game::SceneTune>& t, f64 g) {
            tunes = t;
            share = g;
        };
        std::vector<std::string> ends;
        scene.on_end = [&](const std::string& name, bool skipped, u64) { ends.push_back(name + (skipped ? " пропущена" : "")); };
        // The game's dialogue: open after on_talk when it fits, closed by the player (open = false) or by end_talk.
        bool open = false, fits = true;
        std::vector<std::string> talks;
        int closed = 0;
        scene.on_talk = [&](const std::string& d) {
            talks.push_back(d);
            if (fits) open = true;
            return fits;
        };
        scene.talking = [&] { return open; };
        scene.end_talk = [&] {
            open = false;
            ++closed;
        };
        const f64 dt = 1.0 / 60;
        auto frames = [&](int n) {
            for (int k = 0; k < n; ++k) scene.update(dt, 400, 240);
        };
        auto hedgehog = [&]() -> const ui::SvgPose* {
            Rml::Element* camera = scene.document() ? scene.document()->GetElementById("camera") : nullptr;
            REQUIRE(camera);
            for (int i = 0; i < camera->GetNumChildren(); ++i)
                if (layer_element(camera, i)->GetAttribute<Rml::String>("src", "") == "actors/ёжик.svg") return ui::svg_posed(layer_element(camera, i));
            return nullptr;
        };
        auto head = [](const std::vector<vector::MotionPartPose>& parts) { return game_value_of(parts, "ёжик/head", vector::MotionProp::Y).v; };
        REQUIRE(m.file.actors[0].id == "ёжик");

        std::string why;
        REQUIRE(scene.play(context, game_dir, "Ожидание", {true, true}, why));
        CHECK(scene.waiting() == nullptr);
        CHECK(scene.waiting_index() == -1);
        scene.update(0.5, 400, 240);
        CHECK(heard == std::vector<std::string>{"звук бум.ogg", "сообщение начало"});
        // Its time to the first wait: it stands at 1 s, having said what was before it, not what was after.
        frames(60);
        REQUIRE(scene.waiting());
        CHECK(scene.waiting_index() == 0);
        CHECK(scene.waiting()->name == "Поговорить");
        CHECK_FALSE(scene.waiting()->talk);
        CHECK(scene.time() == 1);
        CHECK(heard.back() == "сообщение середина");
        // 0.6 s at it: its time stands, the hedgehog walks on (a step at 1.25 s lived), the music plays on.
        frames(36);
        CHECK(scene.time() == 1);
        CHECK(scene.waiting_index() == 0);
        CHECK(scene.held()[0] == doctest::Approx(0.6));
        CHECK(scene.lived() == doctest::Approx(1.6));
        CHECK(heard.size() == 6);
        CHECK(heard.back() == "сообщение шаг");
        const std::vector<f64> held = scene.held();
        const ui::SvgPose* hog = hedgehog();
        REQUIRE(hog);
        CHECK(head(hog->parts) == doctest::Approx(head(vector::scene_pose(m, 1, held, 0).actors[0].parts)));
        CHECK(head(hog->parts) != doctest::Approx(head(vector::scene_pose(m, 1).actors[0].parts)));
        std::vector<game::SceneTune> now;
        for (const vector::SceneMusicVoice& v : vector::scene_music_at(m.file, 1, held, 0)) now.push_back({v.call, v.name + ".ogg", v.since, v.volume});
        CHECK(tunes == now);
        REQUIRE(tunes.size() == 1);
        CHECK(tunes[0].since == doctest::Approx(1.6));
        CHECK(share == 0);
        CHECK(talks.empty());
        // Moved on: what came after the wait comes, and it stands at the next, any key, at the same moment.
        scene.go_on();
        frames(1);
        CHECK(heard.back() == "сообщение ответ");
        CHECK(scene.waiting_index() == 1);
        CHECK(scene.waiting()->name.empty());
        CHECK(scene.time() == 1);
        frames(17);
        scene.go_on();
        // A frame of no time (the pause menu up): it does not come to the talk, which could not open over the menu.
        scene.update(0, 400, 240);
        CHECK(talks.empty());
        CHECK(scene.waiting() == nullptr);
        CHECK(scene.held()[1] == doctest::Approx(0.3));
        // The talk: it opens the game's dialogue and stands while that is open; the landing comes meanwhile.
        frames(1);
        CHECK(scene.waiting_index() == 2);
        CHECK(scene.waiting()->talk);
        CHECK(talks == std::vector<std::string>{"Встреча"});
        CHECK(open);
        frames(23);
        CHECK(talks.size() == 1);
        CHECK(scene.time() == 1);
        CHECK(heard.back() == "сообщение земля");
        open = false; // the player ended it
        frames(1);
        CHECK(scene.waiting() == nullptr);
        CHECK(scene.time() == doctest::Approx(1 + dt));
        CHECK(closed == 0);
        // To the wait at its end: it stands there, its landing goes on; it ends when moved on.
        frames(59);
        CHECK(scene.time() == 2);
        CHECK(scene.waiting_index() == 3);
        frames(30);
        CHECK(scene.going());
        CHECK(ends.empty());
        scene.go_on();
        frames(1);
        CHECK_FALSE(scene.going());
        CHECK(ends == std::vector<std::string>{"Ожидание"});
        CHECK(scene.waiting() == nullptr);
        CHECK(scene.waiting_index() == -1);
        const std::vector<std::string> want = {"звук бум.ogg",     "сообщение начало", "сообщение шаг",    "сообщение шаг",
                                               "сообщение середина", "сообщение шаг",    "сообщение ответ",  "сообщение земля",
                                               "сообщение земля",  "сообщение земля",  "сообщение земля"};
        CHECK(heard == want);
        const f64 want_at[] = {0, 0, 0.25, 0.75, 1, 1.25, 1.6, 2.1, 2.6, 3.1, 3.6};
        REQUIRE(heard_at.size() == std::size(want_at));
        for (usize i = 0; i < heard_at.size(); ++i) {
            CAPTURE(i);
            CHECK(heard_at[i] >= want_at[i] - 1e-9);
            CHECK(heard_at[i] < want_at[i] + dt + 1e-9);
        }
        const std::vector<std::string> problems = scene.problems();
        CHECK(std::count(problems.begin(), problems.end(), "сцена «Ожидание»: звука «тишина» нет в звуках игры") == 1);

        // Skipped at a wait: what it had left to say, those after the wait too, its sounds not.
        REQUIRE(scene.play(context, game_dir, "Ожидание", {true, true}, why));
        scene.update(0, 400, 240);
        frames(60 + 18);
        CHECK(scene.waiting_index() == 0);
        heard.clear();
        scene.skip();
        CHECK(heard == std::vector<std::string>{"сообщение ответ", "сообщение земля", "сообщение земля"});
        CHECK(ends.back() == "Ожидание пропущена");
        CHECK(scene.waiting() == nullptr);
        CHECK(talks.size() == 1);
        // Skipped in its talk: the talk closes; dropped in it: the same, nothing said.
        for (const bool skip : {true, false}) {
            CAPTURE(skip);
            REQUIRE(scene.play(context, game_dir, "Ожидание", {true, true}, why));
            scene.update(0, 400, 240);
            frames(60);
            scene.go_on();
            frames(1);
            scene.go_on();
            frames(6);
            REQUIRE(scene.waiting_index() == 2);
            CHECK(open);
            heard.clear();
            const int was = closed;
            if (skip) scene.skip();
            else scene.drop();
            CHECK(closed == was + 1);
            CHECK_FALSE(open);
            CHECK_FALSE(scene.going());
            CHECK(heard == (skip ? std::vector<std::string>{"сообщение земля", "сообщение земля"} : std::vector<std::string>{}));
        }
        const usize ended = ends.size();
        // Another dialogue open when it comes to its talk: it waits for that to end, then opens its own.
        REQUIRE(scene.play(context, game_dir, "Ожидание", {true, true}, why));
        scene.update(0, 400, 240);
        frames(60);
        scene.go_on();
        frames(1);
        open = true; // the game's own
        talks.clear();
        scene.go_on();
        frames(10);
        CHECK(scene.waiting_index() == 2);
        CHECK(talks.empty());
        open = false;
        frames(1);
        CHECK(talks == std::vector<std::string>{"Встреча"});
        CHECK(open);
        CHECK(scene.waiting_index() == 2);
        open = false;
        frames(1);
        CHECK(scene.waiting() == nullptr);
        scene.drop();
        CHECK(closed == 2); // its own was over: not closed again
        // A talk that does not begin: a line in the log, the scene goes on at once, from the same frame.
        fits = false;
        REQUIRE(scene.play(context, game_dir, "Ожидание", {true, true}, why));
        scene.update(0, 400, 240);
        frames(60);
        scene.go_on();
        frames(1);
        scene.go_on();
        frames(1);
        CHECK(scene.waiting() == nullptr);
        CHECK(scene.time() == doctest::Approx(1 + dt));
        CHECK(scene.held()[2] == 0);
        CHECK_FALSE(open);
        const std::vector<std::string> lines = scene.problems();
        CHECK(std::count(lines.begin(), lines.end(),
                         "сцена «Ожидание»: разговор «Встреча» не начался (его нет в игре или ни одно начало не подошло) — сцена идёт дальше") == 1);
        scene.drop();
        CHECK(ends.size() == ended);

        // A wait at its very start, before what it says first; a talk that does not begin, at the same moment.
        REQUIRE(scene.play(context, game_dir, "Сразу", {true, true}, why));
        heard.clear();
        scene.update(0.5, 400, 240);
        CHECK(scene.waiting_index() == 0);
        CHECK(heard.empty());
        frames(5);
        CHECK(scene.time() == 0);
        CHECK(scene.lived() == doctest::Approx(5 * dt));
        // Its music began before the wait: the game's goes out under it by the time it lived, waiting.
        CHECK(share == doctest::Approx(1 - 5 * dt));
        scene.go_on();
        frames(1);
        CHECK(heard == std::vector<std::string>{"сообщение сразу", "сообщение потом"});
        CHECK(scene.waiting() == nullptr);
        frames(30);
        CHECK_FALSE(scene.going());
        CHECK(ends.back() == "Сразу");
        // Not going: nothing waits, go_on does nothing.
        scene.go_on();
        CHECK(scene.waiting() == nullptr);
    }
    Rml::Shutdown();
    std::error_code ec;
    fs::remove_all(game_dir, ec);
}

TEST_CASE("Сцена в игре: площадь фигуры для маски — как у вкладки (Rig::area), разница только на краях") {
    // Filled, only outlined, hidden (only outlined: all of it, as ThorVG reads a hidden shape without its paints) and
    // faded shapes, one turned by the scene, through a camera that moves, zooms and turns, on a screen twice the canvas.
    Files files;
    project(files);
    Scene s;
    const char* stage = R"(    canvas(160, 120)
    background("#203040")
    rect("плита", { x = 20, y = 20, w = 50, h = 30, fill = "#ff0000" })
    circle("кольцо", { x = 110, y = 40, r = 20, fill = "none", stroke = "#00ff00", stroke_width = 6 })
    rect("тайник", { x = 30, y = 70, w = 30, h = 30, fill = "none", stroke = "#0000ff", stroke_width = 4, hidden = true })
    ellipse("дымка", { x = 110, y = 90, rx = 25, ry = 12, fill = "#ffffff", opacity = 0.3 })
    rect("пол", { x = 0, y = 110, w = 160, h = 10, fill = "#405060" }))";
    load(s, files, R"(    part("плита").rot(30)
    camera().x(10)
    camera().zoom(1.25)
    camera().rot(8)
    wait(1))",
         stage);
    record(s);
    const SceneGift gift = gift_of(s, files);
    const vector::SceneMotion& m = gift.motion;
    ui::SvgPlaces places;
    std::string why;
    REQUIRE(places.load("/сцена/stage.svg", *gift_file(gift, "stage.svg"), why));
    REQUIRE(s.frame(0.5));
    const vector::ScenePose pose = vector::scene_pose(m, 0.5);
    const std::vector<std::string> ids = {"плита", "кольцо", "тайник", "дымка"};
    places.place(stage_pose(pose), ids);
    const vector::MotionMatrix cam = vector::scene_camera_matrix(pose.camera, m.file.width, m.file.height);
    constexpr u32 w = 320, h = 240;
    const vector::MotionMatrix screen = vector::MotionMatrix{2, 0, 0, 2, 0, 0} * cam;
    for (const std::string& id : ids) {
        CAPTURE(id);
        const std::vector<u8> tab = s.rig().area(s.rig().part(id), w, h, Affine{2, 0, 0, 2, 0, 0} * s.camera_matrix());
        std::vector<u8> game;
        REQUIRE(places.area(id, screen, w, h, game));
        REQUIRE(tab.size() == game.size());
        // Where the tab's is solid (a pixel and its eight neighbours all in or all out) the game's is the same; elsewhere
        // (the edges) within a pixel's smoothing.
        int inside = 0, outside = 0, off = 0, edge_off = 0;
        for (u32 y = 1; y + 1 < h; ++y)
            for (u32 x = 1; x + 1 < w; ++x) {
                const int v = tab[y * w + x];
                bool solid = v == 0 || v == 255;
                for (int dy = -1; dy <= 1 && solid; ++dy)
                    for (int dx = -1; dx <= 1 && solid; ++dx) solid = tab[(y + dy) * w + x + dx] == v;
                const int d = std::abs(int(game[y * w + x]) - v);
                if (solid) {
                    (v ? inside : outside) += 1;
                    if (d > 2) ++off;
                } else if (d > 160) ++edge_off;
            }
        CHECK(inside > 150);
        CHECK(outside > 150);
        CHECK(off == 0);
        CHECK(edge_off == 0);
        // Its box holds all of it, in the stage's units.
        f64 bx = 0, by = 0, bw = 0, bh = 0;
        REQUIRE(places.area_box(id, bx, by, bw, bh));
        const vector::MotionMatrix back = vector::inverse(screen);
        for (u32 y = 0; y < h; ++y)
            for (u32 x = 0; x < w; ++x) {
                if (game[y * w + x] < 8) continue;
                const vector::Point q = back.apply({x + 0.5, y + 0.5});
                CHECK((q.x > bx - 1 && q.x < bx + bw + 1 && q.y > by - 1 && q.y < by + bh + 1));
                if (!(q.x > bx - 1 && q.x < bx + bw + 1 && q.y > by - 1 && q.y < by + bh + 1)) {
                    x = w, y = h; // once is enough
                }
            }
    }
    // The outline's area is the ring alone: its middle is empty.
    std::vector<u8> ring;
    REQUIRE(places.area("кольцо", vector::MotionMatrix{}, 160, 120, ring));
    CHECK(ring[40 * 160 + 110] == 0);
    CHECK(ring[40 * 160 + 130] == 255);
    // The hidden outline's is all of it, its middle too.
    std::vector<u8> hidden;
    REQUIRE(places.area("тайник", vector::MotionMatrix{}, 160, 120, hidden));
    CHECK(hidden[85 * 160 + 45] == 255);
    // A part not asked for at place, or none such: no area.
    std::vector<u8> none;
    CHECK_FALSE(places.area("пол", screen, w, h, none));
    CHECK(places.area_shapes("пол").empty());
    CHECK_FALSE(places.area("нет", screen, w, h, none));
    f64 x = 1, y = 1, bw = 1, bh = 1;
    CHECK_FALSE(places.area_box("нет", x, y, bw, bh));
    CHECK(bw == 0);
}

namespace {

// The game's screen for scenes (part 6г4): the game's UI over a world of one grey on the GPU (software Vulkan on CI),
// a frame drawn as the shell draws it (the world in a texture of its own when a scene's transition shows the game's
// frame). The grey is the tab's «то, что было».
struct SceneScreen {
    u32 w = 0, h = 0;
    SDL_GPUDevice* device = nullptr;
    SDL_GPUTexture* world = nullptr;
    SDL_GPUTexture* target = nullptr;
    ui::Ui ui;
    Rml::Context* context = nullptr;

    SceneScreen(u32 width, u32 height) : w(width), h(height) {
        device = render::create_offscreen_device();
        REQUIRE(device);
        world = render::create_render_target(device, w, h);
        target = render::create_render_target(device, w, h);
        REQUIRE(world);
        REQUIRE(target);
        ui::UiConfig config;
        config.root = fs::path(FORGE_SOURCE_DIR) / "ui";
        config.hot_reload = false;
        config.debugger = false;
        config.display_scale = 1;
        REQUIRE(ui.init(device, nullptr, config));
        context = ui.create_context("scene", static_cast<int>(w), static_cast<int>(h));
        REQUIRE(context);
    }
    ~SceneScreen() {
        ui.shutdown();
        SDL_ReleaseGPUTexture(device, world);
        SDL_ReleaseGPUTexture(device, target);
        render::destroy_offscreen_device(device);
    }

    static void fill(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* texture, Rgba8 c) {
        SDL_GPUColorTargetInfo color{};
        color.texture = texture;
        color.load_op = SDL_GPU_LOADOP_CLEAR;
        color.store_op = SDL_GPU_STOREOP_STORE;
        color.clear_color = {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1};
        SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &color, 1, nullptr));
    }

    // The scene's time goes dt, then the frame is drawn: its pixels.
    std::vector<u8> frame(game::GameScene& scene, f64 dt) {
        scene.update(dt, static_cast<int>(w), static_cast<int>(h));
        for (Rml::ElementDocument* d : scene.documents()) d->PullToFront();
        return draw(scene.wants_backdrop());
    }

    // The frame as it stands drawn (the world in its texture when `backdrop`): its pixels.
    std::vector<u8> draw(bool backdrop = false) {
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
        REQUIRE(cmd);
        fill(cmd, target, kOutsideScene);
        if (backdrop) {
            fill(cmd, world, kOutsideScene);
            ui.set_world(world);
        }
        ui.update();
        ui.render(cmd, target, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, w, h);
        ui.set_world(nullptr);
        REQUIRE(SDL_SubmitGPUCommandBuffer(cmd));
        std::vector<u8> rgba;
        REQUIRE(render::read_pixels(device, target, w, h, rgba));
        return rgba;
    }
};

Rgba8 rgba_at(const std::vector<u8>& rgba, u32 w, u32 x, u32 y) {
    const u8* p = &rgba[(static_cast<usize>(y) * w + x) * 4];
    return {p[0], p[1], p[2], p[3]};
}

// The game's frame against the tab's draw_frame on a screen of the canvas's size: where the tab's is solid (a pixel and
// its eight neighbours one colour), the game's within 6 of it over black (under a transition's frames); empty when they
// agree, else where not. `said`: how many were compared.
std::string frame_differs(const std::vector<u8>& game, const assets::CookedTexture& tab, std::string& said) {
    const u32 w = tab.width, h = tab.height;
    if (game.size() != tab.rgba8.size()) return "размеры кадров разные";
    int solid = 0, off = 0, worst = 0;
    std::string why;
    for (u32 y = 1; y + 1 < h; ++y)
        for (u32 x = 1; x + 1 < w; ++x) {
            const Rgba8 t = pixel_at(tab, x, y);
            bool one = true;
            for (int dy = -1; dy <= 1 && one; ++dy)
                for (int dx = -1; dx <= 1 && one; ++dx) {
                    const Rgba8 n = pixel_at(tab, x + dx, y + dy);
                    one = std::abs(n.r - t.r) <= 1 && std::abs(n.g - t.g) <= 1 && std::abs(n.b - t.b) <= 1 && std::abs(n.a - t.a) <= 1;
                }
            if (!one) continue;
            ++solid;
            const Rgba8 g = rgba_at(game, w, x, y);
            auto over_black = [&](u8 c) { return static_cast<int>(std::lround(c * t.a / 255.0)); };
            const int d = std::max({std::abs(g.r - over_black(t.r)), std::abs(g.g - over_black(t.g)), std::abs(g.b - over_black(t.b))});
            worst = std::max(worst, d);
            if (d > 6 && off++ == 0)
                why = "в " + std::to_string(x) + ", " + std::to_string(y) + " игра " + std::to_string(g.r) + " " + std::to_string(g.g) + " " +
                      std::to_string(g.b) + ", вкладка " + std::to_string(t.r) + " " + std::to_string(t.g) + " " + std::to_string(t.b) + " " +
                      std::to_string(t.a);
        }
    said = std::to_string(solid) + " сплошных из " + std::to_string(w * h) + ", расхождение до " + std::to_string(worst);
    if (off == 0 && solid >= static_cast<int>(w * h) / 2) return {};
    return off ? std::to_string(off) + " расходятся (" + why + ")" : "сплошных только " + std::to_string(solid);
}

// What a scene's check also looks at, each moment: the game's frame, the tab's and the screen.
using FrameCheck = std::function<void(f64 t, const std::vector<u8>& game, const assets::CookedTexture& tab, SceneScreen& screen)>;

// A scene of the tab given to a game, played on the screen from its start: the game's frame at each moment against the
// tab's (moments in order, the first 0); the tab's of `tab_code` when given (what the game draws where it will not draw
// what the scene says).
void game_frames_as_tab(Files& files, std::string_view stage, std::string_view code, const std::vector<f64>& moments, const char* name,
                        std::string_view tab_code = {}, const FrameCheck& also = {}) {
    INFO("сцена «", name, "»");
    Scene given, other;
    load(given, files, code, stage);
    record(given);
    Scene& s = tab_code.empty() ? given : other;
    if (!tab_code.empty()) {
        load(other, files, tab_code, stage);
        record(other);
    }
    const SceneGift gift = gift_of(given, files, name);
    const fs::path game_dir = empty_dir("forge_tests_переходы_в_игре");
    give(gift, game_dir, name);
    {
        SceneScreen screen(160, 120);
        game::GameScene scene;
        scene.set_ui(&screen.ui);
        std::string why;
        REQUIRE(scene.play(screen.context, game_dir, name, {}, why));
        f64 at = 0;
        for (usize i = 0; i < moments.size(); ++i) {
            const f64 t = moments[i];
            CAPTURE(t);
            const std::vector<u8> game = screen.frame(scene, i == 0 ? 0 : t - at);
            at = t;
            CHECK(scene.time() == doctest::Approx(t));
            const bool ok = s.frame(t);
            INFO(s.error());
            REQUIRE(ok);
            std::string said;
            const assets::CookedTexture tab = s.draw_frame(160, 120, Affine{});
            const std::string differs = frame_differs(game, tab, said);
            INFO(said);
            CHECK_MESSAGE(differs.empty(), differs);
            if (also) also(t, game, tab, screen);
        }
        scene.drop();
    }
    std::error_code ec;
    fs::remove_all(game_dir, ec);
}

} // namespace

TEST_CASE("Сцена в игре: кадры входа и ухода — как кадр вкладки, у которой «то, что было» — кадр игры") {
    Files files;
    project(files);
    // «растворение» in, then «сдвиг» out to the game from the right.
    game_frames_as_tab(files, kStage, R"(    transition("растворение", 1)
    wait(1)
    leave("сдвиг", 1))",
                       {0, 0.25, 0.5, 1, 1.25, 1.5, 1.75, 2}, "Растворение");
    // «сдвиг» in from above (the game's frame black where it has gone), «приближение» in.
    game_frames_as_tab(files, kStage, "    transition(\"сдвиг\", 1, { from = \"верх\" })\n    wait(1)", {0, 0.3, 0.5, 0.8, 1}, "Сдвиг");
    game_frames_as_tab(files, kStage, "    transition(\"приближение\", 1)\n    wait(1)", {0, 0.25, 0.5, 0.75, 1}, "Приближение");
    // One of its own: the new frame seen only in the circle as the camera sees it, the old one faded, through a camera
    // that moves and zooms; and the new one outside the circle, turned and smaller, over the old going left.
    game_frames_as_tab(files, kStageB, R"(    camera().x(8)
    camera().zoom(1.25)
    transition(function(old, new)
      new:mask("круг")
      old.opacity(0.5)
      wait(1)
    end)
    wait(1))",
                       {0, 0.5, 0.99, 1}, "Своя");
    game_frames_as_tab(files, kStageB, R"(    transition(function(old, new)
      new:mask("круг", { invert = true })
      new.rot(6)
      new.scale(0.9)
      old.opacity(0.75)
      old.x(-40, 1, "linear")
    end)
    wait(1))",
                       {0, 0.5, 0.99, 1}, "Наоборот");
    // A leave of its own: the game's frame from below, masked by the circle.
    game_frames_as_tab(files, kStageB, R"(    wait(0.5)
    leave(function(old, new)
      new:mask("круг")
      new.y(120)
      new.y(0, 1, "linear")
    end))",
                       {0, 0.5, 0.75, 1.25, 1.5}, "Уход");
}

TEST_CASE("Сцена в игре: маска картинки и актёра по фигуре сцены — как во вкладке") {
    Files files;
    project(files);
    const char* stage = R"(    canvas(160, 120)
    background("#203040")
    picture("лес", "картинки/лес.png", { x = 8, y = 6 })
    actor("ёжик", "персонажи/Ёжик", { x = 40, y = 100, pivot = { 32, 60 } })
    actor("жук", "персонажи/Жук", { x = 120, y = 90, scale = 0.5, rot = 15, pivot = { 24, 16 } })
    circle("окно", { x = 30, y = 20, r = 14, fill = "#ffffff", hidden = true })
    rect("щель", { x = 20, y = 60, w = 40, h = 12, fill = "#ffffff", hidden = true })
    circle("дыра", { x = 120, y = 85, r = 6, fill = "#ffffff", hidden = true })
    rect("полоса", { x = -40000, y = 6, w = 40030, h = 20, fill = "#ffffff", hidden = true })
    rect("рамка", { x = 10, y = 10, w = 30.05, h = 20.05, fill = "#ffffff", hidden = true }))";
    // The picture inside a circle, the hedgehog in a slit that moves, the beetle outside a hole; the camera zoomed and
    // turned (its view inside the canvas).
    game_frames_as_tab(files, stage, R"(    part("лес"):mask("окно")
    part("ёжик"):mask("щель")
    part("жук"):mask("дыра", { invert = true })
    camera().x(6)
    camera().zoom(1.2)
    camera().rot(4)
    part("щель").y(20, 1, "linear"))",
                       {0, 0.3, 0.6, 0.9}, "Маски");
    // Unmasked again: whole, as 6г2 draws it; its mask's picture gone from the GPU.
    game_frames_as_tab(files, stage, R"(    part("лес"):mask("окно")
    wait(0.5)
    part("лес"):mask()
    wait(0.5))",
                       {0, 0.25, 0.75}, "Без маски", {}, [](f64 t, const std::vector<u8>&, const assets::CookedTexture&, SceneScreen& screen) {
                           CHECK(screen.ui.gpu_images() == (t < 0.5 ? 1u : 0u));
                       });
    // By an actor or a picture: drawn whole («В игре не будет: маска по актёру или картинке»), as the tab without them.
    game_frames_as_tab(files, stage, R"(    part("лес"):mask("ёжик")
    part("жук"):mask("лес", { invert = true })
    wait(0.5))",
                       {0, 0.25}, "По актёру", "    wait(0.5)",
                       [](f64, const std::vector<u8>& game, const assets::CookedTexture& tab, SceneScreen&) {
                           // The picture's own pixels too (a gradient, none of them solid), all there.
                           int off = 0;
                           for (u32 y = 8; y < 34; ++y)
                               for (u32 x = 10; x < 46; ++x) {
                                   const Rgba8 t = pixel_at(tab, x, y), g = rgba_at(game, 160, x, y);
                                   if (std::abs(g.r - t.r) > 8 || std::abs(g.g - t.g) > 8 || std::abs(g.b - t.b) > 8) ++off;
                               }
                           CHECK(off == 0);
                       });
    // A shape far out of the camera's view (a strip 40 000 long): its raster only of what the camera sees, at the
    // screen's scale, its edge where the tab has it.
    game_frames_as_tab(files, stage, "    part(\"лес\"):mask(\"полоса\")\n    wait(0.5)", {0, 0.25}, "Длинная полоса");
    // A box not of whole pixels (30,05 × 20,05 on a still camera, the raster 31 × 21): the far edges where the tab has
    // them, the raster's last pixel not stretched over the box.
    game_frames_as_tab(files, stage, "    part(\"лес\"):mask(\"рамка\")\n    wait(0.5)", {0, 0.25}, "Край маски", {},
                       [](f64, const std::vector<u8>& game, const assets::CookedTexture& tab, SceneScreen&) {
                           int off = 0;
                           auto near = [&](u32 x, u32 y) {
                               const Rgba8 t = pixel_at(tab, x, y), g = rgba_at(game, 160, x, y);
                               if (std::abs(g.r - t.r) > 48 || std::abs(g.g - t.g) > 48 || std::abs(g.b - t.b) > 48) ++off;
                           };
                           for (u32 y = 12; y < 28; ++y) near(39, y);
                           for (u32 x = 12; x < 38; ++x) near(x, 29);
                           CHECK(off == 0);
                       });
}

TEST_CASE("Сцена в игре: сцена за сценой — прежняя стоит старым кадром до конца входа новой, последний кадр — до хода логики") {
    Files files;
    project(files);
    const char* kOne = "    canvas(160, 120)\n    background(\"#203040\")";
    const char* kTwo = "    canvas(160, 120)\n    background(\"#a04020\")";
    const fs::path game_dir = empty_dir("forge_tests_сцены_подряд");
    // «А» and «В» without a transition, «Б» coming in by «растворение», «Г» going out to the game by «сдвиг».
    Scene a, b, c, d;
    auto give_one = [&](Scene& s, const char* name, const char* stage, const char* code) {
        load(s, files, code, stage);
        record(s);
        give(gift_of(s, files, name), game_dir, name);
    };
    give_one(a, "А", kOne, "    wait(1)");
    give_one(b, "Б", kTwo, "    transition(\"растворение\", 1)\n    wait(1)");
    give_one(c, "В", kOne, "    wait(1)");
    give_one(d, "Г", kOne, "    wait(0.5)\n    leave(\"сдвиг\", 0.5)");
    const Rgba8 one{0x20, 0x30, 0x40, 255}, two{0xa0, 0x40, 0x20, 255};
    // How far «Б» has come in at t, by its tab (whose old frame is the grey).
    auto come = [&](f64 t) {
        const bool ok = b.frame(t);
        INFO(b.error());
        REQUIRE(ok);
        const Rgba8 p = pixel_at(b.draw_frame(160, 120, Affine{}), 80, 60);
        return (p.r - kOutsideScene.r) / static_cast<f64>(two.r - kOutsideScene.r);
    };
    // x and y mixed by k, within 3 of each channel.
    auto mix = [](Rgba8 p, Rgba8 x, Rgba8 y, f64 k) {
        auto near_by = [&](u8 got, u8 from, u8 to) { return std::abs(got - (from + (to - from) * k)) <= 3; };
        return near_by(p.r, x.r, y.r) && near_by(p.g, x.g, y.g) && near_by(p.b, x.b, y.b);
    };

    SceneScreen screen(160, 120);
    game::GameScene scene;
    scene.set_ui(&screen.ui);
    u64 runs = 0;
    scene.logic_ran(runs);
    std::vector<std::string> ends;
    scene.on_end = [&](const std::string& name, bool skipped, u64) { ends.push_back(name + (skipped ? " пропущена" : "")); };
    auto middle = [&](f64 dt) { return rgba_at(screen.frame(scene, dt), 160, 80, 60); };
    std::string why;

    // «А» ends; in the frames of a screen of 144 Hz in which the logic does not run, its last frame stays.
    REQUIRE(scene.play(screen.context, game_dir, "А", {}, why));
    CHECK(mix(middle(0), one, one, 0));
    CHECK(mix(middle(0.5), one, one, 0));
    CHECK(mix(middle(0.5), one, one, 0));
    CHECK_FALSE(scene.going());
    CHECK(ends == std::vector<std::string>{"А"});
    for (int k = 0; k < 2; ++k) {
        CHECK(mix(middle(1.0 / 144), one, one, 0));
        CHECK(scene.documents().size() == 1);
    }
    // The logic runs, hears it and begins «Б»: it comes in over the last frame of «А», no frame of the game between.
    scene.logic_ran(++runs);
    REQUIRE(scene.play(screen.context, game_dir, "Б", {}, why));
    CHECK(scene.documents().size() == 2);
    CHECK_FALSE(scene.wants_backdrop());
    CHECK(mix(middle(0), one, two, come(0)));
    for (const f64 t : {0.25, 0.5, 0.75}) {
        CAPTURE(t);
        scene.logic_ran(++runs);
        CHECK(mix(middle(0.25), one, two, come(t)));
        CHECK(scene.documents().size() == 2);
        CHECK_FALSE(scene.wants_backdrop());
    }
    // Come in, it stands alone: the one before goes.
    CHECK(mix(middle(0.25), two, two, 0));
    CHECK(scene.documents().size() == 1);

    // «Б» ends, «В» begins without a transition: «Б» goes at once.
    CHECK(mix(middle(1), two, two, 0));
    CHECK_FALSE(scene.going());
    REQUIRE(scene.documents().size() == 1);
    scene.logic_ran(++runs);
    REQUIRE(scene.play(screen.context, game_dir, "В", {}, why));
    CHECK(scene.documents().size() == 1);
    CHECK(mix(middle(0), one, one, 0));

    // «В» skipped between frames is the one before too: its last frame, the moment of the skip, under «Б» coming in.
    CHECK(mix(middle(0.5), one, one, 0));
    scene.skip();
    CHECK(ends.back() == "В пропущена");
    CHECK(mix(middle(1.0 / 144), one, one, 0));
    CHECK(scene.documents().size() == 1);
    scene.logic_ran(++runs);
    REQUIRE(scene.play(screen.context, game_dir, "Б", {}, why));
    CHECK(scene.documents().size() == 2);
    middle(0);
    CHECK(mix(middle(0.5), one, two, come(0.5)));

    // «Б» ends and nothing comes after it: once the logic has run, its last frame goes and the game is seen.
    CHECK(mix(middle(0.5), two, two, 0));
    CHECK(mix(middle(1), two, two, 0));
    CHECK_FALSE(scene.going());
    CHECK(mix(middle(1.0 / 144), two, two, 0));
    scene.logic_ran(++runs);
    CHECK(mix(middle(1.0 / 144), kOutsideScene, kOutsideScene, 0));
    CHECK(scene.documents().empty());

    // «Г» goes out to the game: «Б» after it comes in over the game, not over its last frame.
    REQUIRE(scene.play(screen.context, game_dir, "Г", {}, why));
    CHECK(mix(middle(0), one, one, 0));
    middle(0.5);
    middle(0.5);
    CHECK_FALSE(scene.going());
    scene.logic_ran(++runs);
    REQUIRE(scene.play(screen.context, game_dir, "Б", {}, why));
    CHECK(scene.documents().size() == 1);
    middle(0);
    CHECK(scene.wants_backdrop());
    CHECK(mix(middle(0.5), kOutsideScene, two, come(0.5)));

    // Dropped (a load, a new game, the main menu): all of it goes at once.
    scene.drop();
    CHECK(scene.documents().empty());
    CHECK_FALSE(scene.wants_backdrop());
    CHECK(mix(middle(1.0 / 60), kOutsideScene, kOutsideScene, 0));
    std::error_code ec;
    fs::remove_all(game_dir, ec);
}

namespace {

// Part 6г5а: the hedgehog's calls with blends from each kind of what it showed, the beetle from rest; lines from 11.
// A play waits for its clip: spawned, the next line comes at once.
const char* kBlendScene = R"(    spawn(actor("жук"):play("hop", { blend = 0.6 }))
    actor("ёжик"):loop("walk")
    wait(0.6)
    spawn(actor("ёжик"):play("wave", { blend = 0.3 }))
    wait(0.1)
    spawn(actor("ёжик"):play("walk", { blend = 0.4 }))
    wait(1)
    actor("ёжик"):stop()
    wait(0.2)
    actor("ёжик"):loop("walk", { blend = 0.8, speed = 2 })
    wait(0.5)
    actor("ёжик"):play("stretch", { blend = 5, params = { len = 0 } })
    wait(0.3)
    spawn(actor("ёжик"):play("wave", { blend = 2 }))
    wait(0.1)
    wait_action("Поговорить")
    wait(0.9))";

f64 smooth(f64 x) { return vector::timing(vector::Timing::Smooth, x); }

} // namespace

TEST_CASE("Смешивание (6г5а): что актёр показывает в смене — от клипа, который идёт, от снимка смены, от stop, от покоя") {
    Files files;
    project(files);
    Scene s;
    load(s, files, kBlendScene);
    const SceneGift gift = gift_of(s, files); // records the scene
    const Recording& rec = *s.recording();
    const ActorPlay& walk_loop = play_of(rec, "ёжик", ActorPlay::Kind::Loop);
    const ActorPlay& wave = play_of(rec, "ёжик", ActorPlay::Kind::Play, 0);
    const ActorPlay& walk = play_of(rec, "ёжик", ActorPlay::Kind::Play, 1);
    const ActorPlay& fast = play_of(rec, "ёжик", ActorPlay::Kind::Loop, 1);
    const ActorPlay& stretch = play_of(rec, "ёжик", ActorPlay::Kind::Play, 2);
    const ActorPlay& wave_again = play_of(rec, "ёжик", ActorPlay::Kind::Play, 3);
    const ActorPlay& hop = play_of(rec, "жук", ActorPlay::Kind::Play);
    CHECK(wave.blend == 0.3);
    CHECK(walk_loop.blend == 0);
    CHECK(fast.blend == 0.8);
    CHECK(stretch.clip_length == 0);
    struct Step {
        const ActorPlay* play;
        f64 time, k;
    };
    auto steps = [&](const char* actor, f64 t, std::span<const f64> held = {}, i32 waiting = -1) {
        std::vector<Step> out;
        for (const ActorBlendStep& st : actor_moment(rec, actor, t, held, waiting).blend) out.push_back({st.play, st.time, st.k});
        return out;
    };
    auto same = [](const std::vector<Step>& a, const std::vector<Step>& b) {
        bool ok = a.size() == b.size();
        for (usize i = 0; ok && i < a.size(); ++i) ok = a[i].play == b[i].play && std::abs(a[i].time - b[i].time) < 1e-9 && std::abs(a[i].k - b[i].k) < 1e-9;
        return ok;
    };
    // The beetle from rest, over 0,6 s of hop's 0,8.
    CHECK(same(steps("жук", 0.1), {{nullptr, 0, 1}, {&hop, 0.1, smooth(0.1 / 0.6)}}));
    CHECK(steps("жук", 0.6).empty());
    // wave from the walk going on round by its own time.
    CHECK(same(steps("ёжик", 0.65), {{&walk_loop, 0.15, 1}, {&wave, 0.05, smooth(0.05 / 0.3)}}));
    CHECK(same(steps("ёжик", 0.69), {{&walk_loop, 0.19, 1}, {&wave, 0.09, smooth(0.09 / 0.3)}}));
    // walk in the middle of wave's blend: from what was shown then, held, no chain going on.
    const std::vector<Step> shown_then = {{&walk_loop, 0.2, 1}, {&wave, 0.1, smooth(0.1 / 0.3)}};
    for (const f64 t : {0.75, 0.9, 1.05}) {
        CAPTURE(t);
        std::vector<Step> want = shown_then;
        want.push_back({&walk, t - 0.7, smooth((t - 0.7) / 0.4)});
        CHECK(same(steps("ёжик", t), want));
    }
    const ActorMoment over = actor_moment(rec, "ёжик", 1.2);
    CHECK(over.play == &walk);
    CHECK(over.time == doctest::Approx(0.5));
    CHECK(over.blend.empty());
    // After the stop, the loop at speed 2 from the pose it held; a loop's blend longer than its clip is not cut.
    CHECK(same(steps("ёжик", 1.95), {{&walk, 0.5, 1}, {&fast, 0.1, smooth(0.1 / 0.8)}}));
    CHECK(same(steps("ёжик", 2.2), {{&walk, 0.5, 1}, {&fast, 0.1, smooth(0.6 / 0.8)}}));
    CHECK(steps("ёжик", 2.31).empty());
    // A clip of length 0 comes at once; wave's blend of 2 s no longer than wave, from it.
    CHECK(actor_moment(rec, "ёжик", 2.5).play == &stretch);
    CHECK(steps("ёжик", 2.5).empty());
    CHECK(same(steps("ёжик", 2.75), {{&stretch, 0, 1}, {&wave_again, 0.05, smooth(0.05 / 0.5)}}));
    CHECK(steps("ёжик", 3.2).empty());
    // The scene waits at 2,8: the new clip goes on, its blend too.
    REQUIRE(rec.stops.size() == 1);
    const std::vector<f64> held = {0.2};
    CHECK(same(steps("ёжик", 2.8, {}, 0), {{&stretch, 0, 1}, {&wave_again, 0.1, smooth(0.1 / 0.5)}}));
    CHECK(same(steps("ёжик", 2.8, held, 0), {{&stretch, 0, 1}, {&wave_again, 0.3, smooth(0.3 / 0.5)}}));
    CHECK(same(steps("ёжик", 2.9, held), {{&stretch, 0, 1}, {&wave_again, 0.4, smooth(0.4 / 0.5)}}));
    CHECK(steps("ёжик", 3.05, held).empty());

    // The files say the same at every moment, with and without the wait.
    CHECK(gift.motion.file.plays[static_cast<usize>(&wave - rec.plays.data())].blend == 0.3);
    for (const std::vector<f64>& h : {std::vector<f64>{}, held})
        for (f64 t = 0; t <= rec.length + 1e-9; t += 0.01)
            for (i32 waiting = -1; waiting < 1; ++waiting)
                for (const char* actor : {"ёжик", "жук"}) {
                    CAPTURE(t);
                    CAPTURE(actor);
                    CAPTURE(waiting);
                    const ActorMoment tab = actor_moment(rec, actor, t, h, waiting);
                    const vector::SceneActorMoment game = vector::scene_actor_moment(gift.motion, actor, t, h, waiting);
                    CHECK(game.play == (tab.play ? static_cast<i32>(tab.play - rec.plays.data()) : -1));
                    CHECK(game.time == doctest::Approx(tab.time));
                    REQUIRE(game.blend.size() == tab.blend.size());
                    for (usize i = 0; i < tab.blend.size(); ++i) {
                        CHECK(game.blend[i].play == (tab.blend[i].play ? static_cast<i32>(tab.blend[i].play - rec.plays.data()) : -1));
                        CHECK(game.blend[i].time == doctest::Approx(tab.blend[i].time));
                        CHECK(game.blend[i].k == doctest::Approx(tab.blend[i].k));
                    }
                }
}

TEST_CASE("Смешивание (6г5а): тот же клип ещё раз — с начала, от своей прежней позы") {
    Files files;
    project(files);
    Scene s;
    load(s, files, R"(    spawn(actor("ёжик"):play("wave"))
    wait(0.3)
    actor("ёжик"):play("wave", { blend = 0.2 })
    wait(0.1))");
    const SceneGift gift = gift_of(s, files); // records the scene
    const Recording& rec = *s.recording();
    const ActorPlay& first = play_of(rec, "ёжик", ActorPlay::Kind::Play, 0);
    const ActorPlay& again = play_of(rec, "ёжик", ActorPlay::Kind::Play, 1);
    CHECK(again.start == doctest::Approx(0.3));
    // At 0,4 s: the first wave going on (0,4 s of it), the new one from its start (0,1 s) over it by half.
    const ActorMoment m = actor_moment(rec, "ёжик", 0.4);
    CHECK(m.play == &again);
    CHECK(m.time == doctest::Approx(0.1));
    REQUIRE(m.blend.size() == 2);
    CHECK(m.blend[0].play == &first);
    CHECK(m.blend[0].time == doctest::Approx(0.4));
    CHECK(m.blend[1].play == &again);
    CHECK(m.blend[1].time == doctest::Approx(0.1));
    CHECK(m.blend[1].k == doctest::Approx(0.5));
    const vector::SceneActorMoment game = vector::scene_actor_moment(gift.motion, "ёжик", 0.4);
    REQUIRE(game.blend.size() == 2);
    CHECK(game.blend[0].play == static_cast<i32>(&first - rec.plays.data()));
    CHECK(game.blend[0].time == doctest::Approx(0.4));
    CHECK(game.blend[1].play == static_cast<i32>(&again - rec.plays.data()));
    CHECK(game.blend[1].k == doctest::Approx(0.5));
    // The nose comes back from the first's turn towards the start: halfway between them, in the tab and in the game.
    const vector::MotionFile& hf = gift.motion.actors[0];
    using P = vector::MotionProp;
    auto nose_at = [&](f64 t) { return game_value_of(vector::motion_pose(hf, *hf.clip("wave"), t), "ёжик/nose", P::Rot).v; };
    CHECK(nose_at(0.4) > nose_at(0.1) + 5);
    const f64 nose = (nose_at(0.4) + nose_at(0.1)) / 2;
    ScenePoses poses;
    REQUIRE(s.poses(0.4, {}, -1, poses));
    CHECK(poses.actors["ёжик"].parts["ёжик/nose"].rot == doctest::Approx(nose));
    const vector::ScenePose whole = vector::scene_pose(gift.motion, 0.4);
    CHECK(game_value_of(whole.actors[0].parts, "ёжик/nose", P::Rot).v == doctest::Approx(nose));
    // After its blend, the new wave alone.
    const ActorMoment after = actor_moment(rec, "ёжик", 0.55);
    CHECK(after.play == &again);
    CHECK(after.time == doctest::Approx(0.25));
    CHECK(after.blend.empty());
}

TEST_CASE("Смешивание (6г5а): loop клипа длины 0 приходит сразу, как play") {
    Files files;
    project(files);
    Scene s;
    load(s, files, R"(    actor("ёжик"):loop("walk")
    wait(0.3)
    actor("ёжик"):loop("stretch", { blend = 0.5, params = { len = 0 } })
    wait(0.3))");
    const SceneGift gift = gift_of(s, files); // records the scene
    const Recording& rec = *s.recording();
    const ActorPlay& stretch = play_of(rec, "ёжик", ActorPlay::Kind::Loop, 1);
    CHECK(stretch.clip_length == 0);
    CHECK(stretch.blend == 0.5);
    for (const f64 t : {0.3, 0.4, 0.55}) {
        CAPTURE(t);
        const ActorMoment m = actor_moment(rec, "ёжик", t);
        CHECK(m.play == &stretch);
        CHECK(m.blend.empty());
        const vector::SceneActorMoment game = vector::scene_actor_moment(gift.motion, "ёжик", t);
        CHECK(game.play == static_cast<i32>(&stretch - rec.plays.data()));
        CHECK(game.blend.empty());
    }
}

TEST_CASE("Смешивание (6г5а): поза в смене — свойства, рисунки и метки по правилам 5е, во вкладке и в игре") {
    Files files;
    project(files);
    Scene s;
    load(s, files, kBlendScene);
    const SceneGift gift = gift_of(s, files);
    const vector::SceneMotion& m = gift.motion;
    const vector::MotionFile& hf = m.actors[0];
    const vector::MotionFile& bf = m.actors[1];
    using P = vector::MotionProp;
    auto clip_value = [](const vector::MotionFile& f, const char* clip, f64 t, const char* part, P p) {
        return game_value_of(vector::motion_pose(f, *f.clip(clip), t), part, p).v;
    };
    auto tab_part = [&](f64 t, const char* actor, const char* part) {
        ScenePoses poses;
        REQUIRE(s.poses(t, {}, -1, poses));
        return poses.actors[actor].parts[part];
    };
    // wave from walk going on: the head comes from walk's bob to rest, the nose from rest to wave's turn.
    {
        const f64 t = 0.65, k = smooth(0.05 / 0.3);
        const f64 head = (1 - k) * clip_value(hf, "walk", 0.15, "ёжик/head", P::Y), nose = k * clip_value(hf, "wave", 0.05, "ёжик/nose", P::Rot);
        CHECK(head < -1);
        CHECK(nose > 0);
        CHECK(tab_part(t, "ёжик", "ёжик/head").y == doctest::Approx(head));
        CHECK(tab_part(t, "ёжик", "ёжик/nose").rot == doctest::Approx(nose));
        const vector::ScenePose whole = vector::scene_pose(m, t);
        const vector::SceneActorPose& g = whole.actors[0];
        CHECK(game_value_of(g.parts, "ёжик/head", P::Y).v == doctest::Approx(head));
        CHECK(game_value_of(g.parts, "ёжик/nose", P::Rot).v == doctest::Approx(nose));
    }
    // From a blend going on: what it showed then, held, into walk.
    {
        const f64 k0 = smooth(0.1 / 0.3), k = smooth(0.15 / 0.4);
        const f64 head_then = (1 - k0) * clip_value(hf, "walk", 0.2, "ёжик/head", P::Y);
        const f64 head = head_then + (clip_value(hf, "walk", 0.15, "ёжик/head", P::Y) - head_then) * k;
        const f64 nose = (1 - k) * k0 * clip_value(hf, "wave", 0.1, "ёжик/nose", P::Rot);
        CHECK(tab_part(0.85, "ёжик", "ёжик/head").y == doctest::Approx(head));
        CHECK(tab_part(0.85, "ёжик", "ёжик/nose").rot == doctest::Approx(nose));
        const vector::ScenePose whole = vector::scene_pose(m, 0.85);
        const vector::SceneActorPose& g = whole.actors[0];
        CHECK(game_value_of(g.parts, "ёжик/head", P::Y).v == doctest::Approx(head));
        CHECK(game_value_of(g.parts, "ёжик/nose", P::Rot).v == doctest::Approx(nose));
    }
    // The beetle from rest into hop's swap to its flat drawing: the main drawing weighed (1 − k) and hop's share of it,
    // flat on top as a swap by its share k · s.
    {
        const f64 t = 0.4, k = smooth(0.4 / 0.6);
        const vector::MotionShown hop = vector::motion_shown(bf, *bf.clip("hop"), 0.4);
        REQUIRE(hop.swap);
        REQUIRE(hop.drawing == "flat");
        const vector::ScenePose whole = vector::scene_pose(m, t);
        const vector::SceneActorPose& g = whole.actors[1];
        CHECK(g.shown.drawing == "flat");
        CHECK(g.shown.swap);
        CHECK(g.shown.from == vector::kMotionMainDrawing);
        CHECK(g.shown.k == doctest::Approx(k * hop.k));
        ScenePoses poses;
        REQUIRE(s.poses(t, {}, -1, poses));
        const Pose& tab = poses.actors["жук"];
        CHECK(tab.drawing == "flat");
        CHECK(tab.swap);
        CHECK(tab.swap_from.empty());
        CHECK(tab.swap_k == doctest::Approx(k * hop.k));
        CHECK(tab.parts.at("жук/shell").y == doctest::Approx(k * clip_value(bf, "hop", 0.4, "жук/shell", P::Y)));
    }
    // The walk loop's steps stop with wave at 0,6: its round past 0,75 says nothing; walk again says its own.
    std::vector<vector::SceneMark> marks;
    std::string why;
    REQUIRE(vector::scene_marks(m, marks, why));
    std::vector<f64> steps;
    for (const vector::SceneMark& mk : marks)
        if (mk.name == "шаг") steps.push_back(mk.at);
    REQUIRE(steps.size() >= 3);
    CHECK(steps[0] == doctest::Approx(0.25));
    CHECK(steps[1] == doctest::Approx(0.95)); // walk from 0,7, not walk's round at 0,75
    std::vector<Mark> tab_marks;
    REQUIRE(s.marks(tab_marks));
    REQUIRE(tab_marks.size() == marks.size());
    for (usize i = 0; i < marks.size(); ++i) CHECK(tab_marks[i].time == doctest::Approx(marks[i].at));
}

TEST_CASE("Смешивание (6г5а): scene.json со сменой — версия 2 и обратно, без смен — 1; сверка ловит другую смену") {
    using K = vector::ScenePlay::Kind;
    vector::SceneFile f;
    f.name = "Смена";
    f.from = "сцены/Смена.luau";
    f.length = 2;
    f.actors = {{"ёжик", "персонажи/Ёжик", "actors/ёжик.svg", "actors/ёжик.motion.json", {0, 0}, 64, 64}};
    f.plays = {{K::Loop, "ёжик", "walk", 0, 1, 0}, {K::Play, "ёжик", "wave", 0.5, 1.5, 1, 0.3}, {K::Stop, "ёжик", "", 1, 1, 2}};
    const std::string text = vector::write_scene_file(f);
    INFO(text);
    CHECK(has(text, "\"version\": 2"));
    CHECK(has(text, "{\"actor\": \"ёжик\", \"kind\": \"play\", \"clip\": \"wave\", \"at\": 0.5, \"speed\": 1.5, \"order\": 1, \"blend\": 0.3}"));
    CHECK(has(text, "{\"actor\": \"ёжик\", \"kind\": \"loop\", \"clip\": \"walk\", \"at\": 0, \"speed\": 1, \"order\": 0}"));
    vector::SceneFile back;
    std::string why;
    REQUIRE(vector::read_scene_file(text, back, why));
    CHECK(back == f);
    CHECK(vector::write_scene_file(back) == text);
    // Without a blend, version 1, as a game made before reads it.
    vector::SceneFile plain = f;
    plain.plays[1].blend = 0;
    CHECK(has(vector::write_scene_file(plain), "\"version\": 1"));
    CHECK_FALSE(has(vector::write_scene_file(plain), "blend"));
    struct Bad {
        const char *from, *to, *said;
    };
    const Bad bad[] = {
        {"\"version\": 2", "\"version\": 1", "plays[1]: blend: смена клипов — в файле версии 2"},
        {"\"version\": 2", "\"version\": 3", "version 3: сцену отдал редактор новее этой игры (она читает 2)"},
        {"\"blend\": 0.3", "\"blend\": -0.3", "plays[1]: blend: нужны секунды смены от 0"},
        {"\"blend\": 0.3", "\"blend\": \"0.3\"", "plays[1]: blend: нужны секунды смены от 0"},
        {"\"order\": 2}", "\"order\": 2, \"blend\": 1}", "plays[2]: blend: у stop смены нет"},
    };
    for (const Bad& b : bad) {
        CAPTURE(b.to);
        vector::SceneFile out;
        CHECK_FALSE(vector::read_scene_file(replaced(text, b.from, b.to), out, why));
        CHECK(why == b.said);
    }

    // The gift's check: the files with another blend would not show what the tab shows.
    Files files;
    project(files);
    Scene s;
    load(s, files, kBlendScene);
    const SceneGift gift = gift_of(s, files);
    CHECK(has(*gift_file(gift, "scene.json"), "\"version\": 2"));
    vector::SceneMotion other = gift.motion;
    for (vector::ScenePlay& p : other.file.plays)
        if (p.clip == "wave" && p.at > 2) p.blend = 0.25;
    CHECK_FALSE(check_scene_gift(s, other, why));
    CHECK(has(why, "«ёжик», смена клипов"));
    // A scene without blends goes as before: version 1.
    Scene before;
    load(before, files, "    actor(\"ёжик\"):loop(\"walk\")\n    wait(1)");
    CHECK(has(*gift_file(gift_of(before, files), "scene.json"), "\"version\": 1"));
}

TEST_CASE("Смешивание (6г5а): blend_pose вкладки — как motion_blend игры: копии гаснут и проявляются, связь и порядок прежние") {
    Rig rig;
    std::string why;
    REQUIRE(rig.load(R"(<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10"><rect id="a" width="4" height="4"/><rect id="b" width="4" height="4"/></svg>)", why));
    Pose from, to;
    from.parts["a"].x = 4;
    from.parts["a"].visible = false;
    from.parts["a"].carry_by = "b";
    from.parts["a"].fill = {255, 0, 0, 255};
    from.parts["a"].has_fill = true;
    from.parts["искра[1]"].y = 2;
    from.copies = {{"искра[1]", "a", 1}};
    from.order = {{"a", OrderStep::Kind::Top, ""}};
    from.morphs["b"] = {"a", 0.5};
    to.parts["a"].x = 8;
    to.parts["b"].rot = 90;
    to.parts["искра[2]"].opacity = 0.5;
    to.copies = {{"искра[2]", "a", 2}};
    const f64 k = 0.25;
    const Pose out = blend_pose(rig, from, to, k);
    CHECK(out.parts.at("a").x == doctest::Approx(5));
    CHECK_FALSE(out.parts.at("a").visible); // until k is 1
    CHECK(out.parts.at("a").carry_by == "b");
    CHECK(out.parts.at("a").has_fill);
    CHECK(out.parts.at("a").fill == Rgba8{255, 0, 0, 255});
    CHECK(out.parts.at("a").fill_k == doctest::Approx(0.75));
    CHECK(out.parts.at("b").rot == doctest::Approx(22.5));
    CHECK(out.morphs.at("b").target == "a");
    CHECK(out.morphs.at("b").k == doctest::Approx(0.375));
    CHECK(out.parts.at("искра[1]").y == 2);
    CHECK(out.parts.at("искра[1]").opacity == doctest::Approx(0.75));
    CHECK(out.parts.at("искра[2]").opacity == doctest::Approx(0.125));
    REQUIRE(out.copies.size() == 2);
    CHECK(out.copies[0].id == "искра[2]");
    CHECK(out.copies[1].id == "искра[1]");
    CHECK(out.order == from.order);
    CHECK(out.drawing.empty());
    CHECK_FALSE(out.swap);
    CHECK(blend_pose(rig, from, to, 0).parts.at("a").x == 4);
    CHECK(blend_pose(rig, from, to, 1).parts.at("a").x == 8);
    // The same by the game's rule.
    auto side = [](const Pose& p) {
        vector::MotionClipPose c;
        std::set<std::string> ids;
        for (const auto& [id, pp] : p.parts) ids.insert(id);
        for (const auto& [id, mm] : p.morphs) ids.insert(id);
        for (const std::string& id : ids) {
            vector::MotionPartPose pp;
            pp.id = id;
            for (usize i = 0; i < vector::kMotionPropCount; ++i) {
                const auto mp = static_cast<vector::MotionProp>(i);
                pp.values[i] = screen_value(p, id, mp);
                pp.has[i] = mp != vector::MotionProp::Drawing;
            }
            c.parts.push_back(pp);
        }
        for (const PartCopy& k : p.copies) c.copies.push_back({k.id, k.source, k.number, 0});
        return c;
    };
    vector::MotionFile file;
    const vector::MotionClipPose game = vector::motion_blend(file, side(from), side(to), k);
    for (const vector::MotionPartPose& pp : game.parts)
        for (usize i = 0; i < vector::kMotionPropCount; ++i) {
            const auto mp = static_cast<vector::MotionProp>(i);
            if (mp == vector::MotionProp::Drawing || mp == vector::MotionProp::Link) continue;
            CAPTURE(pp.id);
            CAPTURE(i);
            CHECK(vector::motion_same(mp, pp.values[i], screen_value(out, pp.id, mp)));
        }
    REQUIRE(game.copies.size() == 2);
    CHECK(game.copies[0].id == "искра[2]");
    CHECK(game.copies[1].id == "искра[1]");
}

// ---- part 6г5б: a scene behind a screen ----

namespace {

// A scene behind a screen (2 s a round): a transition in and a leave that a screen does not draw, the hedgehog walking
// right, its sound at the start and «тишина» at 1.5 s, messages, a wait for an action and a talk that pass at once,
// music from the start; and what the tab draws of it: the same without its transitions.
const char* kBehindScene = R"(    transition("растворение", 0.5)
    music("тема")
    actor("ёжик"):loop("walk")
    spawn(part("ёжик").x(80, 1.5, "linear"))
    sound("бум")
    emit("начало")
    wait(0.75)
    wait_action("Поговорить")
    talk("Встреча")
    emit("середина")
    wait(0.75)
    sound("тишина")
    leave("сдвиг", 0.5))";
const char* kBehindTab = R"(    music("тема")
    actor("ёжик"):loop("walk")
    spawn(part("ёжик").x(80, 1.5, "linear"))
    sound("бум")
    emit("начало")
    wait(0.75)
    wait_action("Поговорить")
    talk("Встреча")
    emit("середина")
    wait(0.75)
    sound("тишина")
    wait(0.5))";

// The game of the scene «Заставка» (kBehindScene) and «Тихая» (the hedgehog walking for 1 s, no music), the sounds they
// say.
fs::path behind_game(Files& files, const char* folder) {
    Scene s, quiet;
    load(s, files, kBehindScene, kPlaceStage);
    load(quiet, files, "    actor(\"ёжик\"):loop(\"walk\")\n    wait(1)", kPlaceStage);
    const SceneGift gift = gift_of(s, files, "Заставка");
    REQUIRE(gift.motion.file.length == doctest::Approx(2));
    REQUIRE(gift.motion.file.stops.size() == 2);
    const fs::path game_dir = empty_dir(folder);
    give(gift, game_dir, "Заставка");
    give(gift_of(quiet, files, "Тихая"), game_dir, "Тихая");
    for (const char* sound : {"бум.ogg", "тишина.ogg", "тема.ogg"}) put_file(game_dir / "sounds" / utf8_path(sound), "звук");
    return game_dir;
}

// A page as «Интерфейс» writes it, the whole 160 × 120 screen: its role, its scene behind, more of its root's attributes
// and what is in it.
std::string behind_page(std::string_view role, std::string_view scene, std::string_view more = {}, std::string_view inner = {}) {
    std::string html = R"(<!DOCTYPE html><html><head><meta charset="utf-8"></head><body><div forge-screen=")" + std::string(role) +
                       R"(" forge-size="160 120")";
    if (!scene.empty()) html += " forge-scene-behind=\"" + std::string(scene) + "\"";
    html += std::string(more) + R"( style="position: absolute; left: 0; top: 0; width: 160px; height: 120px;">)" + std::string(inner) +
            "</div></body></html>";
    return html;
}

// Whether an element of a scene's document is hidden (display: none).
bool hidden_in(Rml::ElementDocument* doc, const char* id) {
    Rml::Element* e = doc->GetElementById(id);
    REQUIRE(e);
    const Rml::Property* p = e->GetLocalProperty(Rml::PropertyId::Display);
    return p && p->Get<Rml::Style::Display>() == Rml::Style::Display::None;
}

// How much of a scene's frame shows: its look's filter (none: all of it).
f64 look_opacity(const game::GameScene& scene) {
    const std::vector<Rml::ElementDocument*> docs = scene.documents();
    REQUIRE(docs.size() == 1);
    Rml::Element* wrap = docs[0]->GetElementById("look-wrap");
    REQUIRE(wrap);
    const Rml::Property* f = wrap->GetLocalProperty(Rml::PropertyId::Filter);
    if (!f) return 1;
    const std::string text = f->ToString();
    const auto at = text.find("opacity(");
    INFO(text);
    REQUIRE(at != std::string::npos);
    return std::atof(text.c_str() + at + 8);
}

// The visible documents, back to front.
std::vector<Rml::ElementDocument*> shown_documents(Rml::Context* context) {
    std::vector<Rml::ElementDocument*> out;
    for (int i = 0; i < context->GetNumDocuments(); ++i)
        if (context->GetDocument(i)->IsVisible()) out.push_back(context->GetDocument(i));
    return out;
}

// Whether the scene behind a page is shown right under the page: the visible document under it.
bool right_under(Rml::Context* context, const game::GameScreens& screens, std::string_view page) {
    const game::GameScene* scene = screens.scene_behind(page);
    REQUIRE(scene);
    const std::vector<Rml::ElementDocument*> docs = scene->documents();
    REQUIRE(docs.size() == 1);
    const std::vector<Rml::ElementDocument*> shown = shown_documents(context);
    const auto at = std::find(shown.begin(), shown.end(), docs[0]);
    return at != shown.end() && at + 1 != shown.end() && *(at + 1) == screens.document(page);
}

void log_into(LogLevel, const char* message, void* user) { static_cast<std::vector<std::string>*>(user)->push_back(message); }

} // namespace

TEST_CASE("Сцена позади (6г5б): по кругу с начала — переходы не рисуются, ожидания и разговор проходят сразу, сообщений нет, звуки и музыка каждый круг") {
    Files files;
    project(files);
    const fs::path game_dir = behind_game(files, "forge_tests_сцена_позади");
    SceneNoRender render;
    SceneFilesOnDisk disk;
    Rml::SetRenderInterface(&render);
    Rml::SetFileInterface(&disk);
    REQUIRE(Rml::Initialise());
    ui::install_live_svg();
    Rml::Context* context = Rml::CreateContext("behind", {160, 120});
    REQUIRE(context);
    {
        game::GameScene scene;
        std::vector<std::string> heard;
        scene.on_message = [&](const std::string& name) { heard.push_back("сообщение " + name); };
        scene.on_sound = [&](const std::string& file) { heard.push_back("звук " + file); };
        bool talked = false;
        scene.on_talk = [&](const std::string&) { return talked = true; };
        scene.talking = [&] { return talked; };
        std::vector<game::SceneTune> tunes;
        scene.on_music = [&](const std::vector<game::SceneTune>& t, f64) { tunes = t; };
        int ends = 0;
        scene.on_end = [&](const std::string&, bool, u64) { ++ends; };
        game::SceneHow how;
        how.pause = false;
        how.skip = false;
        how.behind = true;
        std::string why;
        REQUIRE(scene.play(context, game_dir, "Заставка", how, why));
        CHECK(scene.how().behind);
        const u64 serial = scene.serial();
        // Each frame: at its time, a frame of the scene (none of its transitions), not standing anywhere, its music from the
        // start of the round.
        auto as_it_should = [&](f64 want) {
            CHECK(scene.going());
            CHECK_FALSE(scene.over(serial));
            CHECK(scene.time() == doctest::Approx(want));
            CHECK(scene.waiting() == nullptr);
            for (const f64 h : scene.held()) CHECK(h == 0);
            CHECK_FALSE(scene.wants_backdrop());
            const std::vector<Rml::ElementDocument*> docs = scene.documents();
            REQUIRE(docs.size() == 1);
            for (const char* id : {"backdrop", "black", "below", "above"}) CHECK(hidden_in(docs[0], id));
            CHECK_FALSE(hidden_in(docs[0], "look-wrap"));
            CHECK(look_opacity(scene) == 1);
            REQUIRE(tunes.size() == 1);
            CHECK(tunes[0].file == "тема.ogg");
            CHECK(tunes[0].since == doctest::Approx(want));
        };
        scene.update(0.5, 160, 120); // the first frame shows its moment 0
        as_it_should(0);
        CHECK(heard == std::vector<std::string>{"звук бум.ogg"});
        // Three rounds at 64 frames a second: the frame of its length is its first.
        std::vector<std::string> want{"звук бум.ogg"};
        for (int frame = 1; frame <= 3 * 128; ++frame) {
            CAPTURE(frame);
            scene.update(1.0 / 64, 160, 120);
            const int in_round = frame % 128;
            as_it_should(in_round / 64.0);
            if (in_round == 96) want.push_back("звук тишина.ogg");
            if (in_round == 0) want.push_back("звук бум.ogg");
            CHECK(heard == want);
        }
        CHECK_FALSE(talked);
        CHECK(ends == 0);
        // At 0.3 s a frame: round on from where it is, each sound in the frame its moment passes.
        heard.clear();
        f64 at = 0;
        for (int frame = 1; frame <= 10; ++frame) {
            CAPTURE(frame);
            const f64 before = at;
            scene.update(0.3, 160, 120);
            at = std::fmod(std::round((at + 0.3) * 1e9) / 1e9, 2.0);
            as_it_should(at);
            std::vector<std::string> now;
            if (before < 1.5 && (at >= 1.5 || at < before)) now.push_back("звук тишина.ogg");
            if (at < before) now.push_back("звук бум.ogg");
            CHECK(heard == now);
            heard.clear();
        }
        // A frame of 5 s (the game stood): the end of one round and the start of the next, not of every round.
        scene.update(5, 160, 120);
        at = std::fmod(at + 5, 2.0);
        as_it_should(at);
        CHECK(heard == std::vector<std::string>{"звук тишина.ogg", "звук бум.ogg"});
        // Dropped: nothing of it; played again, from its start.
        scene.drop();
        CHECK_FALSE(scene.going());
        CHECK(scene.documents().empty());
        CHECK(tunes.empty());
        CHECK(ends == 0);
        heard.clear();
        REQUIRE(scene.play(context, game_dir, "Заставка", how, why));
        scene.update(1.0 / 64, 160, 120);
        as_it_should(0);
        CHECK(heard == std::vector<std::string>{"звук бум.ogg"});
        scene.drop();
    }
    Rml::Shutdown();
    std::error_code ec;
    fs::remove_all(game_dir, ec);
}

TEST_CASE("Сцена позади (6г5б): под экраном, пока он показан — с начала при каждом показе, сразу под ним, с его прозрачностью, его музыкой") {
    Files files;
    project(files);
    const fs::path game_dir = behind_game(files, "forge_tests_сцена_позади_экраны");
    SceneNoRender render;
    SceneFilesOnDisk disk;
    Rml::SetRenderInterface(&render);
    Rml::SetFileInterface(&disk);
    REQUIRE(Rml::Initialise());
    ui::install_live_svg();
    Rml::Context* context = Rml::CreateContext("behind", {160, 120});
    REQUIRE(context);
    std::vector<std::string> log;
    log_set_sink(log_into, &log);
    {
        game::GameScreens screens;
        u32 frames = 0; // the clock: a 64th of a second a frame, to the nanosecond
        screens.set_clock([&] { return 1'000'000'000ULL + static_cast<u64>(frames) * 15'625'000ULL; });
        screens.set_game_dir(game_dir);
        std::vector<std::string> sounds, music;
        std::vector<std::vector<game::SceneTune>> scene_music;
        screens.on_sound = [&](const std::string& file) { sounds.push_back(file); };
        screens.on_music = [&](const std::string& name) { music.push_back(name); };
        screens.on_scene_music = [&](const std::vector<game::SceneTune>& tunes) { scene_music.push_back(tunes); };
        auto page = [&](const char* name, std::string_view role, std::string_view scene, std::string_view more = {}) {
            REQUIRE(screens.load_page(context, name, behind_page(role, scene, more), path_to_utf8(game_dir / "ui" / utf8_path(std::string(name) + ".html"))));
        };
        page("меню", "menu", "Заставка");
        page("окно", "window", "Заставка", R"( forge-appear="fade" forge-appear-time="0.5")");
        page("своя", "window", "Заставка", R"( forge-music="своя.ogg")");
        page("пустое", "window", "Нет такой");
        page("худ", "playing", "Тихая");
        game::Vars vars;
        bool menu = true, playing = false;
        auto to = [&](u32 frame) {
            while (frames < frame) {
                ++frames;
                screens.update(vars, playing, menu, 160, 120);
            }
        };
        auto time_of = [&](const char* name) {
            const game::GameScene* scene = screens.scene_behind(name);
            REQUIRE(scene);
            return scene->time();
        };
        auto tunes_since = [&](f64 since) {
            REQUIRE(screens.scene_music().size() == 1);
            CHECK(screens.scene_music()[0].file == "тема.ogg");
            CHECK(screens.scene_music()[0].since == doctest::Approx(since));
            REQUIRE_FALSE(scene_music.empty());
            CHECK(scene_music.back() == screens.scene_music());
        };

        // The main menu up: its scene from the start, right under it; the menu has no music: the scene's is its.
        screens.update(vars, playing, menu, 160, 120);
        REQUIRE(screens.scene_behind("меню"));
        CHECK(screens.scene_behind_name("меню") == "Заставка");
        CHECK(screens.scene_behind("меню")->how().behind);
        CHECK(time_of("меню") == 0);
        CHECK(right_under(context, screens, "меню"));
        CHECK(sounds == std::vector<std::string>{"бум.ogg"});
        CHECK(screens.music().empty());
        CHECK(music.empty());
        tunes_since(0);
        CHECK(look_opacity(*screens.scene_behind("меню")) == 1);
        CHECK(screens.scene_behind("окно") == nullptr);
        CHECK(screens.scene_behind_name("окно") == "Заставка");
        CHECK(screens.scene_behind_name("своя") == "Заставка");
        CHECK(screens.scene_behind_name("нет такого экрана").empty());
        // Round and round by the screens' clock, its sounds and music with it.
        to(96);
        CHECK(time_of("меню") == doctest::Approx(1.5));
        CHECK(sounds == std::vector<std::string>{"бум.ogg", "тишина.ogg"});
        tunes_since(1.5);
        to(128);
        CHECK(time_of("меню") == doctest::Approx(0));
        CHECK(sounds == std::vector<std::string>{"бум.ogg", "тишина.ogg", "бум.ogg"});
        tunes_since(0);
        to(140);
        // Brought to the front by RmlUi (a click on it): back right under its page.
        screens.scene_behind("меню")->documents()[0]->PullToFront();
        to(141);
        CHECK(right_under(context, screens, "меню"));
        to(142);

        // A window over the menu with the same scene: a player of its own from its start, right under the window, over the
        // menu; as much of it as of the window while it comes in. Without music of its own, its scene's is the music.
        REQUIRE(screens.show("окно", true));
        to(143);
        REQUIRE(screens.scene_behind("окно"));
        CHECK(screens.scene_behind("окно") != screens.scene_behind("меню"));
        CHECK(time_of("окно") == 0);
        CHECK(time_of("меню") == doctest::Approx(15 / 64.0));
        CHECK(right_under(context, screens, "окно"));
        CHECK(right_under(context, screens, "меню"));
        {
            const std::vector<Rml::ElementDocument*> shown = shown_documents(context);
            const auto pos = [&](Rml::ElementDocument* d) { return std::find(shown.begin(), shown.end(), d) - shown.begin(); };
            CHECK(pos(screens.document("меню")) < pos(screens.scene_behind("окно")->documents()[0]));
        }
        CHECK(look_opacity(*screens.scene_behind("окно")) == doctest::Approx(0).epsilon(1e-4));
        tunes_since(0);
        CHECK(sounds.back() == "бум.ogg");
        to(151); // a quarter of the window's coming in
        CHECK(look_opacity(*screens.scene_behind("окно")) == doctest::Approx(1 - 0.75 * 0.75 * 0.75).epsilon(1e-4));
        Rml::Element* window_root = screens.document("окно")->QuerySelector("[forge-screen]");
        REQUIRE(window_root);
        CHECK(window_root->GetLocalProperty(Rml::PropertyId::Opacity)->Get<float>() == doctest::Approx(1 - 0.75 * 0.75 * 0.75).epsilon(1e-4));
        CHECK(look_opacity(*screens.scene_behind("меню")) == 1);
        tunes_since(8 / 64.0);
        to(175); // in
        CHECK(look_opacity(*screens.scene_behind("окно")) == 1);
        CHECK(screens.scene_behind("окно")->documents()[0]->GetElementById("look-wrap")->GetLocalProperty(Rml::PropertyId::Filter) == nullptr);
        // A window with music of its own over it, its scene's music under it: its own music; the scenes' stops. The menu
        // brought to the front meanwhile (RmlUi does it when something on it takes the keyboard): back under the windows,
        // its scene under it.
        screens.document("меню")->PullToFront();
        REQUIRE(screens.show("своя", true));
        to(176);
        CHECK(right_under(context, screens, "меню"));
        CHECK(right_under(context, screens, "окно"));
        CHECK(right_under(context, screens, "своя"));
        {
            const std::vector<Rml::ElementDocument*> shown = shown_documents(context);
            REQUIRE(shown.size() == 6);
            CHECK(shown[1] == screens.document("меню"));
            CHECK(shown[5] == screens.document("своя"));
        }
        CHECK(screens.music() == "своя.ogg");
        CHECK(music.back() == "своя.ogg");
        CHECK(screens.scene_music().empty());
        CHECK(scene_music.back().empty());
        CHECK(time_of("своя") == 0);
        REQUIRE(screens.show("своя", false));
        to(177);
        CHECK(music.back().empty());
        tunes_since(34 / 64.0);
        // The window going: its scene goes with it, fading as it does; gone, it stops, the menu's music again.
        REQUIRE(screens.show("окно", false));
        to(185);
        REQUIRE(screens.scene_behind("окно"));
        CHECK(look_opacity(*screens.scene_behind("окно")) == doctest::Approx(0.75 * 0.75 * 0.75).epsilon(1e-4));
        CHECK(time_of("окно") == doctest::Approx(42 / 64.0));
        to(210);
        CHECK(screens.scene_behind("окно") == nullptr);
        tunes_since(std::fmod(210 / 64.0, 2.0));
        CHECK(right_under(context, screens, "меню"));
        // Shown again: from its start.
        REQUIRE(screens.show("окно", true));
        to(211);
        CHECK(time_of("окно") == 0);
        REQUIRE(screens.show("окно", false));
        to(250);
        CHECK(screens.scene_behind("окно") == nullptr);

        // Its scene not in the game: the window shows without it, said once in the log, however often it is shown.
        const std::string missing = "экран «пустое»: сцена позади не идёт (нет сцены «Нет такой»: отдайте её игре во вкладке «Векторная»): экран показан без неё";
        auto said = [&] { return std::count_if(log.begin(), log.end(), [&](const std::string& l) { return l.find(missing) != std::string::npos; }); };
        CHECK(said() == 0);
        for (int i = 0; i < 2; ++i) {
            REQUIRE(screens.show("пустое", true));
            to(frames + 2);
            CHECK(screens.document("пустое")->IsVisible());
            CHECK(screens.scene_behind("пустое") == nullptr);
            REQUIRE(screens.show("пустое", false));
            to(frames + 1);
        }
        CHECK(said() == 1);

        // A new game: the menu goes and its scene with it; the screen over the world plays its own (no music: none).
        menu = false;
        playing = true;
        const u32 begun = frames + 1;
        to(begun);
        CHECK(screens.scene_behind("меню") == nullptr);
        CHECK(time_of("худ") == 0);
        CHECK(right_under(context, screens, "худ"));
        CHECK(screens.scene_music().empty());
        CHECK(scene_music.back().empty());
        CHECK(screens.music().empty());
        to(begun + 64 + 16); // «Тихая» round and round too: 1 s a round
        CHECK(time_of("худ") == doctest::Approx(0.25));
        // Back to the menu: from its start.
        menu = true;
        playing = false;
        to(frames + 1);
        CHECK(screens.scene_behind("худ") == nullptr);
        CHECK(time_of("меню") == 0);
        tunes_since(0);
        // Leaving (the editor's «Проверить»): no scene music said; the next update says it again.
        screens.stop_music();
        CHECK(scene_music.back().empty());
        CHECK(screens.scene_music().empty());
        to(frames + 1);
        tunes_since(1 / 64.0);
        // The page read again (its file changed): its scene from the start.
        page("меню", "menu", "Заставка");
        to(frames + 1);
        CHECK(time_of("меню") == 0);
        CHECK(right_under(context, screens, "меню"));
    }
    log_set_sink(nullptr, nullptr);
    Rml::Shutdown();
    std::error_code ec;
    fs::remove_all(game_dir, ec);
}

namespace {

// The frame of a scene behind a screen against the tab's: where the tab's is solid and opaque (outside `skip`: x, y,
// w, h), `shows` of it over the grey under it, within 6; empty when they agree, else where not. `said`: how many were
// compared.
std::string behind_differs(const std::vector<u8>& game, const assets::CookedTexture& tab, f64 shows, const std::array<u32, 4>& skip, std::string& said) {
    const u32 w = tab.width, h = tab.height;
    if (game.size() != tab.rgba8.size()) return "размеры кадров разные";
    int solid = 0, off = 0, worst = 0;
    std::string why;
    for (u32 y = 1; y + 1 < h; ++y)
        for (u32 x = 1; x + 1 < w; ++x) {
            if (x + 1 >= skip[0] && x <= skip[0] + skip[2] && y + 1 >= skip[1] && y <= skip[1] + skip[3]) continue;
            const Rgba8 t = pixel_at(tab, x, y);
            if (t.a != 255) continue;
            bool one = true;
            for (int dy = -1; dy <= 1 && one; ++dy)
                for (int dx = -1; dx <= 1 && one; ++dx) {
                    const Rgba8 n = pixel_at(tab, x + dx, y + dy);
                    one = std::abs(n.r - t.r) <= 1 && std::abs(n.g - t.g) <= 1 && std::abs(n.b - t.b) <= 1 && n.a == 255;
                }
            if (!one) continue;
            ++solid;
            const Rgba8 g = rgba_at(game, w, x, y);
            auto over_grey = [&](u8 c, u8 grey) { return static_cast<int>(std::lround(grey * (1 - shows) + c * shows)); };
            const int d = std::max({std::abs(g.r - over_grey(t.r, kOutsideScene.r)), std::abs(g.g - over_grey(t.g, kOutsideScene.g)),
                                    std::abs(g.b - over_grey(t.b, kOutsideScene.b))});
            worst = std::max(worst, d);
            if (d > 6 && off++ == 0)
                why = "в " + std::to_string(x) + ", " + std::to_string(y) + " игра " + std::to_string(g.r) + " " + std::to_string(g.g) + " " +
                      std::to_string(g.b) + ", вкладка " + std::to_string(t.r) + " " + std::to_string(t.g) + " " + std::to_string(t.b);
        }
    said = std::to_string(solid) + " сплошных из " + std::to_string(w * h) + ", расхождение до " + std::to_string(worst);
    if (off == 0 && solid >= static_cast<int>(w * h) / 2) return {};
    return off ? std::to_string(off) + " расходятся (" + why + ")" : "сплошных только " + std::to_string(solid);
}

bool all_grey(const std::vector<u8>& rgba) {
    for (usize i = 0; i + 3 < rgba.size(); i += 4)
        if (rgba[i] != kOutsideScene.r || rgba[i + 1] != kOutsideScene.g || rgba[i + 2] != kOutsideScene.b) return false;
    return true;
}

} // namespace

TEST_CASE("Сцена позади (6г5б): кадры под экраном — кадры вкладки без переходов, через конец круга; экран над ней; гаснет и проявляется с ним") {
    Files files;
    project(files);
    Scene tab;
    load(tab, files, kBehindTab, kPlaceStage);
    record(tab);
    const fs::path game_dir = behind_game(files, "forge_tests_сцена_позади_кадры");
    {
        SceneScreen screen(160, 120);
        game::GameScreens screens;
        u32 frames = 0;
        screens.set_clock([&] { return 1'000'000'000ULL + static_cast<u64>(frames) * 15'625'000ULL; });
        screens.set_game_dir(game_dir);
        screens.set_ui(&screen.ui);
        // The menu fades in and out over 0.5 s; a square of its own at 4, 4.
        const std::string square = R"(<div style="position: absolute; left: 4px; top: 4px; width: 12px; height: 12px; background-color: #ff00ff;"></div>)";
        REQUIRE(screens.load_page(screen.context, "меню", behind_page("menu", "Заставка", R"( forge-appear="fade" forge-appear-time="0.5")", square),
                                  path_to_utf8(game_dir / "ui" / utf8_path("меню.html"))));
        game::Vars vars;
        bool menu = true;
        auto to = [&](u32 frame) {
            while (frames < frame) {
                ++frames;
                screens.update(vars, false, menu, 160, 120);
            }
        };
        // At this frame: the tab's frame at the scene's time, `shows` of it.
        auto as_tab = [&](f64 shows) {
            const game::GameScene* scene = screens.scene_behind("меню");
            REQUIRE(scene);
            const f64 t = scene->time();
            CAPTURE(t);
            CAPTURE(frames);
            const std::vector<u8> game = screen.draw();
            const bool ok = tab.frame(t);
            INFO(tab.error());
            REQUIRE(ok);
            const assets::CookedTexture want = tab.draw_frame(160, 120, Affine{});
            std::string said;
            const std::string differs = behind_differs(game, want, shows, {4, 4, 12, 12}, said);
            INFO(said);
            CHECK_MESSAGE(differs.empty(), differs);
            return game;
        };
        screens.update(vars, false, menu, 160, 120);
        CHECK(all_grey(screen.draw())); // the menu and its scene coming in from nothing
        to(16);                         // half its coming: 0.875 of it
        CHECK(screens.scene_behind("меню")->time() == doctest::Approx(0.25));
        as_tab(0.875);
        // In: the scene as the tab draws it (no transition of its own), the menu over it.
        for (const u32 at : {32u, 64u, 96u, 112u, 127u, 128u, 144u}) {
            to(at);
            CHECK(screens.scene_behind("меню")->time() == doctest::Approx(std::fmod(at / 64.0, 2.0)));
            const std::vector<u8> game = as_tab(1);
            CHECK(rgba_at(game, 160, 8, 8) == Rgba8{255, 0, 255, 255});
        }
        // Going away: it fades with the menu, its time on; gone, nothing of it.
        menu = false;
        to(145 + 16);
        as_tab(0.125);
        to(145 + 40);
        CHECK(screens.scene_behind("меню") == nullptr);
        CHECK(all_grey(screen.draw()));
        // Back: from its start.
        menu = true;
        to(186);
        CHECK(screens.scene_behind("меню")->time() == 0);
        to(186 + 32);
        CHECK(screens.scene_behind("меню")->time() == doctest::Approx(0.5));
        as_tab(1);
    }
    std::error_code ec;
    fs::remove_all(game_dir, ec);
}
