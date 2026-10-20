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

#include "forge/assets/image.h"
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
#include "forge/vector/motion_file.h"
#include "forge/vector/scene_file.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
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
        {"    actor(\"ёжик\"):play(\"walk\", { blend = 0.2 })", "blend — в части 6г"},
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
    // The characters drawn alone, as their own tabs draw them.
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
        {"\"version\": 1", "\"version\": 2", "version 2: сцену отдал редактор новее этой игры"},
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
