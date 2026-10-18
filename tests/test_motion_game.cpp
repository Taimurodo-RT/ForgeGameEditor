// Part 5а of docs/models/движение-модель.md («Метки в игре»): the marks of a clip of the code (emit and sound) fall on
// the frames of the strip that first show their moments, a window on the frames it holds; the strip of .pixel.json and
// the template's animations keep them; the game learns which marks a world step passed and whether a window goes.

#include "forge/core/file.h"
#include "forge/core/jobs.h"
#include "forge/core/path.h"
#include "forge/logic/logic.h"
#include "forge/objects/library.h"
#include "forge/pixel/bake.h"
#include "forge/pixel/clip_cut.h"
#include "forge/pixel/doc.h"
#include "forge/pixel/rig.h"
#include "forge/pixel/script.h"
#include "forge/scene/scene.h"
#include "forge/script/compiler.h"
#include "forge/script/host.h"
#include "forge/sim/simulation.h"
#include "forge/world/generators.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <ostream> // doctest's INFO prints a string_view through it; MSVC needs the whole class
#include <span>
#include <string>
#include <vector>

using namespace forge;
namespace fs = std::filesystem;

namespace {

pixel::PoseScript script_of(std::string_view source) {
    pixel::PoseScript s;
    const bool ok = s.load(source);
    INFO(s.error());
    REQUIRE(ok);
    return s;
}

// Marks at 0, 0.1 (a window to 0.35), 0.25 and 0.5, the clip's end.
const char* kMarks = R"(return { clips = { c = function(p)
  sound("шаг")
  wait(0.1)
  emit("удар", 0.25)
  wait(0.15)
  emit("искра")
  wait(0.25)
  sound("взмах")
end } })";

pixel::Recording recorded(pixel::PoseScript& s) {
    pixel::Recording rec;
    const bool ok = s.record(nullptr, "c", {}, 1, {}, rec);
    INFO(s.error());
    REQUIRE(ok);
    return rec;
}

pixel::ClipSpec spec(f32 fps, bool loop) {
    pixel::ClipSpec c;
    c.state = "c";
    c.fps = fps;
    c.loop = loop;
    return c;
}

pixel::ClipSpec ranged(pixel::ClipSpec c, f64 from, f64 to) {
    c.ranged = true;
    c.from = from;
    c.to = to;
    return c;
}

// The marks as "name@frame+frames", for one CHECK each.
std::string marks(const std::vector<pixel::StripEvent>& events) {
    std::string out;
    for (const pixel::StripEvent& e : events)
        out += (out.empty() ? "" : " ") + std::string(e.sound ? "♪" : "") + e.name + "@" + std::to_string(e.frame) +
               (e.frames != 1 ? "+" + std::to_string(e.frames) : "");
    return out;
}

std::string names(const objects::Clip& clip, const std::vector<u32>& events) {
    std::string out;
    for (u32 i : events) out += (out.empty() ? "" : " ") + clip.events[i].name + "@" + std::to_string(clip.events[i].frame);
    return out;
}

fs::path temp_folder(const char* name) {
    const fs::path p = fs::temp_directory_path() / name;
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void write_text(const fs::path& p, const std::string& text) {
    REQUIRE(write_file_atomic(p, std::span(reinterpret_cast<const u8*>(text.data()), text.size())));
}

std::string text_of(const fs::path& p) {
    std::vector<u8> bytes;
    REQUIRE(read_file(p, bytes));
    return std::string(bytes.begin(), bytes.end());
}

} // namespace

TEST_CASE("5а: a mark falls on the frame that first shows its moment, at any frames a second") {
    pixel::PoseScript s = script_of(kMarks);
    const pixel::Recording rec = recorded(s);
    REQUIRE(rec.length == doctest::Approx(0.5));
    REQUIRE(rec.marks.size() == 4);
    auto at = [&](const pixel::ClipSpec& c, std::vector<usize>* left = nullptr) {
        return marks(pixel::strip_events(rec, pixel::cut_clip(c, true, rec.length), left));
    };
    // 12 a second, no round: frames at 0, 1/12 … 0.5. The window from 0.1 holds the frames at 1/6, 1/4 and 1/3; the
    // mark at 0.25 is on its frame exactly; the last at the clip's end on its last frame.
    CHECK(at(spec(12, false)) == "♪шаг@0 удар@2+3 искра@3 ♪взмах@6");
    CHECK(at(spec(6, false)) == "♪шаг@0 удар@1+2 искра@2 ♪взмах@3");
    CHECK(at(spec(30, false)) == "♪шаг@0 удар@3+8 искра@8 ♪взмах@15");
    // With «Повторять» the clip's end is frame 0 of the next round: the mark there is past the last frame.
    std::vector<usize> left;
    CHECK(at(spec(12, true), &left) == "♪шаг@0 удар@2+3 искра@3");
    CHECK(left == std::vector<usize>{3});
    // A range not from 0: the frames from 0.1 by tenths; the mark at 0 is before it.
    left.clear();
    CHECK(at(ranged(spec(10, false), 0.1, 0.5), &left) == "удар@0+3 искра@2 ♪взмах@4");
    CHECK(left == std::vector<usize>{0});
    // A range that ends before the last two.
    left.clear();
    CHECK(at(ranged(spec(10, false), 0, 0.2), &left) == "♪шаг@0 удар@1+2");
    CHECK(left == std::vector<usize>{2, 3});

    // A window shorter than a frame: one frame; marks of one frame in the order the code made them.
    pixel::PoseScript s2 = script_of(R"(return { clips = { c = function(p)
  emit("б")
  emit("а", 0.01)
  wait(0.04)
  sound("в")
  wait(0.5)
end } })");
    const pixel::Recording rec2 = recorded(s2);
    CHECK(marks(pixel::strip_events(rec2, pixel::cut_clip(spec(12, false), true, rec2.length))) == "б@0 а@0 ♪в@1");

    // A flow of spawn(…) is written down before what comes after it: the strip has its marks by their frames.
    pixel::PoseScript s3 = script_of(R"(return { clips = { c = function(p)
  spawn(function()
    wait(0.3)
    emit("поздно")
  end)
  emit("рано")
  wait(0.5)
end } })");
    const pixel::Recording rec3 = recorded(s3);
    REQUIRE(rec3.marks.size() == 2);
    CHECK(rec3.marks[0].name == "поздно");
    CHECK(marks(pixel::strip_events(rec3, pixel::cut_clip(spec(10, false), true, rec3.length))) == "рано@0 поздно@3");
}

TEST_CASE("5а: a baked strip keeps the marks of each clip of the code, and .pixel.json keeps them") {
    pixel::Rig rig;
    std::string why;
    REQUIRE(rig.load(R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16" width="16" height="16">
  <rect id="a" x="2" y="6" width="4" height="4" fill="#ff0000"/>
</svg>)svg", why));
    pixel::PoseScript s = script_of(R"(return { clips = {
  c = function(p)
    sound("шаг")
    part("a").x(8, 0.5, "linear")
    emit("удар", 0.2)
    wait(0.2)
  end,
  d = function(p)
    wait(0.25)
    emit("рано")
    wait(0.25)
  end,
} })");
    pixel::PixelDoc doc;
    doc.frame_w = doc.frame_h = 16;
    doc.clips = {spec(12, false), ranged(spec(12, false), 0, 0.2)};
    doc.clips[1].state = "d";
    pixel::Strip strip;
    REQUIRE(pixel::bake_strip(rig, s, doc, {}, 64, strip, why));
    REQUIRE(strip.clips.size() == 2);
    // c: 0.7 s long, 9 frames; the window from 0.5 to 0.7 on its frames at 6/12 … 8/12 and the end.
    CHECK(strip.clips[0].frames == 10);
    CHECK(marks(strip.clips[0].events) == "♪шаг@0 удар@6+4");
    // d: its range ends before its mark.
    CHECK(strip.clips[1].events.empty());
    REQUIRE(strip.left.size() == 1);
    CHECK(strip.left[0].clip == "d");
    CHECK(strip.left[0].name == "рано");
    CHECK(strip.left[0].time == doctest::Approx(0.25));

    // Written into "strip" and read back; a clip without marks has none in the file.
    doc.strip = strip.clips;
    const std::string text = pixel::write_pixel_doc(doc);
    CHECK(text.find("\"events\"") != std::string::npos);
    CHECK(text.find("\"sound\": \"шаг\"") != std::string::npos);
    pixel::PixelDoc read;
    REQUIRE(pixel::read_pixel_doc(text, read, why));
    CHECK(read == doc);
    doc.strip[0].events.clear();
    CHECK(pixel::write_pixel_doc(doc).find("\"events\"") == std::string::npos);

    // A mark the strip cannot hold: the file does not read, naming the field.
    const std::string good = R"({"script": "c.luau", "bake": {"palette": "svg", "outline": null, "strip": {"c": {"first": 0, "frames": 4,
      "fps": 12, "loop": false, "events": [)";
    for (const char* bad : {R"({"frame": 4, "emit": "удар"})", R"({"frame": 0, "emit": "удар", "sound": "шаг"})",
                            R"({"frame": 0, "sound": "шаг", "frames": 2})", R"({"frame": 0, "emit": ""})",
                            R"({"frame": 2, "emit": "удар", "frames": 3})", R"({"frame": 0, "emit": "удар", "frames": 0})", R"({"frame": 0})"}) {
        INFO(bad);
        CHECK_FALSE(pixel::read_pixel_doc(good + bad + "]}}}}", read, why));
        CHECK(why.find("bake.strip.c.events") == 0);
    }
    REQUIRE(pixel::read_pixel_doc(good + R"({"frame": 2, "emit": "удар", "frames": 2}, {"frame": 3, "sound": "шаг"}]}}}})", read, why));
    REQUIRE(read.strip.size() == 1);
    CHECK(marks(read.strip[0].events) == "удар@2+2 ♪шаг@3");
}

TEST_CASE("5а: a template's animation keeps its marks in its file; marks that cannot be are left out") {
    const fs::path dir = temp_folder("forge_motion_game_test");
    write_text(dir / "kinds.json", R"({"kinds": [{"id": "critter", "name": "Житель", "components": {}}]})");
    write_text(dir / "objects" / "ezhik.object.json", R"({"id": "ezhik", "name": "Ёжик", "kind": "critter", "values": {}})");
    objects::Library lib;
    REQUIRE(lib.load(dir / "kinds.json", dir / "objects"));
    objects::Template t = *lib.find("ezhik");
    t.picture = "ёжик.png";
    t.frames = 8;
    REQUIRE(lib.put(t));
    const std::string before = text_of(t.file);
    objects::Clip attack{{4, 5, 6, 7}, 12, false, {{"взмах", true, 0, 1}, {"удар", false, 1, 2}}};
    t.animations["attack"] = attack;
    t.animations["walk"] = {{0, 1, 2, 3}, 12, true, {}};
    REQUIRE(lib.put(t));
    const std::string text = text_of(t.file);
    CHECK(text.find("    \"attack\": {\"frames\": [4, 5, 6, 7], \"fps\": 12, \"loop\": false, \"events\": [{\"frame\": 0, \"sound\": "
                    "\"взмах\"}, {\"frame\": 1, \"emit\": \"удар\", \"frames\": 2}]},\n"
                    "    \"walk\": {\"frames\": [0, 1, 2, 3], \"fps\": 12}\n") != std::string::npos);
    std::optional<objects::Template> read = objects::read_template(t.file);
    REQUIRE(read);
    CHECK(read->animations == t.animations);
    CHECK(objects::template_json(*read) == text);
    // Without marks again: the file as it was before them.
    t.animations.clear();
    REQUIRE(lib.put(t));
    CHECK(text_of(t.file) == before);

    // A file written by hand: each mark that cannot be is left out, the animation and the other marks read.
    write_text(t.file, R"({"id": "ezhik", "name": "Ёжик", "kind": "critter", "picture": "ёжик.png", "frames": 8,
  "animations": {"attack": {"frames": [4, 5, 6, 7], "fps": 12, "loop": false, "events": [
    {"frame": 4, "emit": "за концом"}, {"frame": 0, "emit": "оба", "sound": "оба"}, {"frame": 0, "sound": "звук с окном", "frames": 2},
    {"frame": 0, "emit": ""}, {"frame": 2, "emit": "окно за концом", "frames": 3}, {"frame": 1, "emit": "пустое окно", "frames": 0},
    {"frame": -1, "emit": "минус"}, "не объект", {"frame": 1, "emit": "удар", "frames": 3}, {"frame": 3, "sound": "шаг"}]},
                 "walk": {"frames": [0, 1], "fps": 12, "events": {"frame": 0, "sound": "шаг"}}},
  "values": {}})");
    read = objects::read_template(t.file);
    REQUIRE(read);
    REQUIRE(read->animations.count("attack"));
    const std::vector<objects::ClipEvent> kept{{"удар", false, 1, 3}, {"шаг", true, 3, 1}};
    CHECK(read->animations["attack"].events == kept);
    CHECK(read->animations["attack"].frames == std::vector<u32>{4, 5, 6, 7});
    REQUIRE(read->animations.count("walk")); // "events" not a list: no marks, the animation reads
    CHECK(read->animations["walk"].events.empty());
    fs::remove_all(dir);
}

TEST_CASE("5а: which marks a world step passes, and whether a window goes") {
    // 12 a second: a frame is 5 ticks. A sound on frame 0, a window on frames 1 and 2, a mark on the last.
    const objects::Clip attack{{4, 5, 6, 7}, 12, false, {{"взмах", true, 0, 1}, {"удар", false, 1, 2}, {"конец", false, 3, 1}}};
    CHECK(names(attack, objects::clip_events(attack, -1, 0)) == "взмах@0"); // begun: frame 0 now
    CHECK(objects::clip_events(attack, 0, 4).empty());
    CHECK(names(attack, objects::clip_events(attack, 4, 5)) == "удар@1");
    CHECK(objects::clip_events(attack, 5, 10).empty()); // frame 2 began at 10: nothing begins on it
    CHECK(names(attack, objects::clip_events(attack, 14, 15)) == "конец@3");
    CHECK(objects::clip_events(attack, 15, 1000).empty()); // no round: the last frame stays
    CHECK(names(attack, objects::clip_events(attack, -1, 1000)) == "взмах@0 удар@1 конец@3"); // all of it at once, in order
    CHECK(objects::clip_events(attack, 7, 7).empty());
    CHECK(objects::clip_events(attack, 9, 3).empty());
    const objects::ClipEvent& hit = attack.events[1];
    for (u64 t = 0; t < 30; ++t) {
        INFO(t);
        CHECK(objects::clip_event_going(attack, hit, t) == (t >= 5 && t < 15));
    }
    // A mark on the last frame of one without «Повторять» goes while that frame stays.
    CHECK(objects::clip_event_going(attack, attack.events[2], 1000));
    CHECK_FALSE(objects::clip_event_going(attack, hit, 1000));

    // A round: a step on frames 0 and 2, each round again.
    const objects::Clip walk{{0, 1, 2, 3}, 12, true, {{"шаг", true, 0, 1}, {"шаг", true, 2, 1}}};
    CHECK(names(walk, objects::clip_events(walk, -1, 0)) == "шаг@0");
    CHECK(objects::clip_events(walk, 0, 9).empty());
    CHECK(names(walk, objects::clip_events(walk, 9, 10)) == "шаг@2");
    CHECK(names(walk, objects::clip_events(walk, 19, 20)) == "шаг@0"); // frame 4: frame 0 of the next round
    CHECK(names(walk, objects::clip_events(walk, 0, 20)) == "шаг@2 шаг@0"); // several frames in one step, in order
    CHECK(names(walk, objects::clip_events(walk, 0, 100)) == "шаг@2 шаг@0"); // more than a round: its last round once
    CHECK(objects::clip_place(walk, 20) == 0);
    CHECK(objects::clip_place(walk, 39) == 3);
    // Frames by place, as before.
    CHECK(objects::clip_frame(attack, 12) == 6);

    // A frame taken away: marks that began on it go, a window over it ends sooner.
    objects::Clip cut = attack;
    cut.frames.pop_back();
    CHECK(objects::trim_events(cut) == std::vector<std::string>{"конец"});
    CHECK(cut.events == std::vector<objects::ClipEvent>{{"взмах", true, 0, 1}, {"удар", false, 1, 2}});
    cut.frames.pop_back();
    CHECK(objects::trim_events(cut).empty());
    CHECK(cut.events == std::vector<objects::ClipEvent>{{"взмах", true, 0, 1}, {"удар", false, 1, 1}});

    // Why a mark cannot be the animation's.
    CHECK(objects::event_problem(attack, {"удар", false, 1, 3}).empty());
    CHECK(objects::event_problem(attack, {"", false, 0, 1}) == "у метки нет имени");
    CHECK(objects::event_problem(attack, {"удар", false, 4, 1}) == "метка «удар» на кадре 5, а в анимации их 4");
    CHECK(objects::event_problem(attack, {"удар", false, 2, 3}) == "окно метки «удар» на 3 кадров не помещается в анимацию");
    CHECK(objects::event_problem(attack, {"удар", false, 2, 0}) == "окно метки «удар» на 0 кадров не помещается в анимацию");
    CHECK(objects::event_problem(attack, {"шаг", true, 0, 2}) == "у звука «шаг» нет окна");
}

namespace {

struct PoolScope {
    PoolScope() { jobs::init(3); }
    ~PoolScope() { jobs::shutdown(); }
};

// What the game says to scripts here: the texts they set, and which marks go now (for one thing).
struct MarkBridge final : script::GameBridge {
    std::vector<std::string> said;
    flecs::entity_t thing = 0;
    std::vector<std::string> going;
    f64 var(std::string_view, std::string*) override { return 0; }
    void set_var(std::string_view, f64) override {}
    void set_text(std::string_view, std::string_view text) override { said.emplace_back(text); }
    void screen(std::string_view, std::string_view) override {}
    bool anim_going(flecs::entity_t e, std::string_view mark) override {
        return e == thing && std::find(going.begin(), going.end(), mark) != going.end();
    }
};

struct MarkGame final : logic::Game {
    std::vector<std::string> hints;
    bool is_hero(flecs::entity_t) override { return false; }
    flecs::entity_t hero() override { return 0; }
    bool has(flecs::entity_t, std::string_view) override { return false; }
    bool act(std::string_view, flecs::entity_t, std::string_view, flecs::entity_t, flecs::entity_t) override { return true; }
    void hint(flecs::entity_t, std::string_view text) override { hints.emplace_back(text); }
    void sound(flecs::entity_t, std::string_view) override {}
    bool night() override { return false; }
    void fired(u32) override {}
};

} // namespace

TEST_CASE("5а: a mark comes to on_anim at the next tick, in turn with messages; anim.going asks the game") {
    PoolScope pool;
    world::World world(world::WorldDesc{}, std::make_shared<world::EmptyGenerator>());
    scene::Scene scene(world);
    sim::Simulation sim(world, scene);
    script::ScriptHost scripts(sim, scene);
    MarkBridge bridge;
    scripts.set_user(script::kGameBridge, &bridge);
    const world::Rect view{-64, -64, 128, 64};
    auto run = [&](u32 ticks) {
        for (u32 i = 0; i < ticks; ++i) sim.update(1.0 / 60.0, view);
    };
    sim.update(0, view);
    world.finish_loading();
    sim.update(0, view);
    REQUIRE(scripts.load("fox", R"(
        local S = {}
        function S.on_message(self, name) forge.game.set_var("said", "сообщение " .. name) end
        function S.on_anim(self, name)
            forge.game.set_var("said", "метка " .. name .. (forge.anim.going(self, name) and ", идёт" or ""))
        end
        return S
    )"));
    REQUIRE(scripts.load("deaf", "local S = {}\nfunction S.on_message(self, name) forge.game.set_var(\"said\", \"глухой \" .. name) end\nreturn S"));
    flecs::entity fox = scene.spawn(scene::Position::at_tile(4, 5));
    fox.set<script::Script>({"fox"});
    flecs::entity deaf = scene.spawn(scene::Position::at_tile(8, 5));
    deaf.set<script::Script>({"deaf"});
    flecs::entity plain = scene.spawn(scene::Position::at_tile(9, 5));
    bridge.thing = fox.id();
    bridge.going = {"удар"};
    run(2);

    scripts.send(fox.id(), "раз");
    scripts.anim(fox.id(), "удар");
    scripts.send(fox.id(), "два");
    scripts.anim(fox.id(), "искра");
    scripts.anim(deaf.id(), "удар");  // no on_anim: nothing
    scripts.anim(plain.id(), "удар"); // no module: nothing
    scripts.anim(0, "удар");          // nobody
    CHECK(bridge.said.empty());       // not before the next tick
    run(1);
    CHECK(bridge.said == std::vector<std::string>{"сообщение раз", "метка удар, идёт", "сообщение два", "метка искра"});
    CHECK(scripts.errors().empty());
    run(3);
    CHECK(bridge.said.size() == 4); // each once

    // The event's node and the going one: what the schemes are made of.
    script::ScriptApi api;
    script::register_core_api(api);
    const script::NodeLibrary nodes = logic::node_library(api);
    const script::NodeDef* ev = nodes.find("std.event.anim");
    REQUIRE(ev);
    CHECK(ev->kind == script::NodeKind::Event);
    CHECK(ev->event == "on_anim");
    CHECK(ev->title.get() == "При метке анимации");
    CHECK(logic::thing_event("std.event.anim"));
    const script::NodeDef* going = nodes.find("api.anim.going");
    REQUIRE(going);
    CHECK(going->title.get() == "Идёт метка анимации");
    REQUIRE(going->inputs.size() == 2);
    CHECK(going->inputs[0].title.get() == "Вещь");
    CHECK(going->inputs[1].title.get() == "Метка");
}

TEST_CASE("5а: a thing's own scheme hears its animation's marks, «Метка» filters them, «Идёт метка анимации» asks") {
    PoolScope pool;
    const fs::path dir = fs::temp_directory_path() / "forge_motion_marks";
    fs::remove_all(dir);
    write_text(dir / "kinds.json", R"({"kinds": [{"id": "thing", "name": "Вещь", "group": "Разное", "icon": "box", "foot": 0.5}]})");
    write_text(dir / "objects" / "fox.object.json", R"({"id": "fox", "name": "Лиса", "kind": "thing"})");
    objects::Library library;
    REQUIRE(library.load(dir / "kinds.json", dir / "objects"));

    world::World world(world::WorldDesc{}, std::make_shared<world::EmptyGenerator>());
    scene::Scene scene(world);
    sim::Simulation sim(world, scene);
    script::ScriptHost scripts(sim, scene);
    MarkBridge bridge;
    scripts.set_user(script::kGameBridge, &bridge);
    library.attach(scene);
    MarkGame game;
    logic::Runtime runtime(scripts, library, game);
    const world::Rect view{-64, -64, 128, 64};
    auto run = [&](u32 ticks) {
        for (u32 i = 0; i < ticks; ++i) sim.update(1.0 / 60.0, view);
    };
    sim.update(0, view);
    world.finish_loading();
    run(1);

    // При метке «удар» → «Бьёт»; при любой метке → её имя; при метке «искра», если идёт «удар» → «Искра в окне».
    script::Graph g;
    const u32 hit = g.add("std.event.anim", 0, 0).uid;
    g.find(hit)->set_value("filter", "удар");
    const u32 says = g.add("logic.hint", 280, 0).uid;
    g.find(says)->set_value("text", "Бьёт");
    g.link(hit, script::kFlowNext, says);
    const u32 any = g.add("std.event.anim", 0, 200).uid;
    const u32 name = g.add("logic.hint", 280, 200).uid;
    g.link(any, script::kFlowNext, name);
    g.link(any, "name", name, "text");
    const u32 spark = g.add("std.event.anim", 0, 400).uid;
    g.find(spark)->set_value("filter", "искра");
    const u32 self = g.add("std.self", 0, 560).uid;
    const u32 going = g.add("api.anim.going", 280, 560).uid;
    g.find(going)->set_value("mark", "удар");
    g.link(self, "actor", going, "thing");
    const u32 branch = g.add("std.flow.if", 560, 400).uid;
    g.link(spark, script::kFlowNext, branch);
    g.link(going, "on", branch, "cond");
    const u32 in_window = g.add("logic.hint", 840, 400).uid;
    g.find(in_window)->set_value("text", "Искра в окне");
    g.link(branch, "then", in_window);
    logic::ThingScheme own;
    own.thing = "fox";
    own.graph = g.to_json();
    logic::Logic logic;
    logic.add_scheme(own);

    logic::Verbs verbs;
    REQUIRE(verbs.parse(R"({"verbs": []})"));
    std::vector<logic::Problem> problems;
    REQUIRE(runtime.load(logic, verbs, &problems));
    for (const logic::Problem& p : problems) INFO(p.text);
    CHECK(problems.empty());
    std::vector<logic::Thing> things{logic::hero_thing(), logic::thing_of(library, *library.find("fox"))};
    const logic::FindThing find = [&](std::string_view id) -> const logic::Thing* {
        for (const logic::Thing& t : things)
            if (t.id == id) return &t;
        return nullptr;
    };
    script::ScriptApi api;
    script::register_core_api(api);
    const script::NodeLibrary nodes = logic::node_library(api);
    const logic::Compiled c = logic::compile(logic, verbs, find, &nodes);
    const logic::Module* m = c.find("fox");
    REQUIRE(m);
    CHECK(m->source.find("S.on_anim = {}") != std::string::npos);
    CHECK(m->source.find("S.on_anim[#S.on_anim + 1] = function(self, e1)") != std::string::npos);
    CHECK(m->source.find("forge.anim.going(") != std::string::npos);
    std::string error;
    CHECK_MESSAGE(script::check_syntax(m->source, &error), error);
    runtime.attach(scene);
    flecs::entity fox = library.spawn(scene, *library.find("fox"), 10.5, 6);
    REQUIRE(fox.is_valid());
    bridge.thing = fox.id();
    run(2);

    scripts.anim(fox.id(), "удар");
    run(1);
    CHECK(game.hints == std::vector<std::string>{"Бьёт", "удар"});
    scripts.anim(fox.id(), "искра"); // «удар» does not go: no «Искра в окне»
    run(1);
    CHECK(game.hints == std::vector<std::string>{"Бьёт", "удар", "искра"});
    bridge.going = {"удар"};
    scripts.anim(fox.id(), "искра");
    run(1);
    CHECK(game.hints == std::vector<std::string>{"Бьёт", "удар", "искра", "искра", "Искра в окне"});
    CHECK(scripts.errors().empty());
}

TEST_CASE("5а: a mark's sound is the game's sound of that name, as «Векторная» finds it") {
    const fs::path dir = fs::temp_directory_path() / "forge_motion_mark_sounds";
    fs::remove_all(dir);
    write_text(dir / utf8_path("шаг.ogg"), "x");
    write_text(dir / utf8_path("шаг.wav"), "x");
    write_text(dir / utf8_path("звон"), "x");
    write_text(dir / utf8_path("удар.wav"), "x");
    CHECK(objects::mark_sound_file(dir, "шаг") == dir / utf8_path("шаг.wav")); // .wav before .ogg
    CHECK(objects::mark_sound_file(dir, "звон") == dir / utf8_path("звон"));    // the name itself first
    CHECK(objects::mark_sound_file(dir, "удар.wav") == dir / utf8_path("удар.wav"));
    CHECK(objects::mark_sound_file(dir, "нет").empty());
    CHECK(objects::mark_sound_file(dir, "").empty());
    CHECK(objects::mark_sound_file(dir, "../шаг").empty());
    CHECK(objects::mark_sound_file(dir, "a\\шаг").empty());
    fs::create_directories(dir / utf8_path("папка"));
    CHECK(objects::mark_sound_file(dir, "папка").empty()); // a folder is no sound
    // .ogg alone; a name is never a path, even to a sound that is there.
    write_text(dir / utf8_path("гром.ogg"), "x");
    CHECK(objects::mark_sound_file(dir, "гром") == dir / utf8_path("гром.ogg"));
    write_text(dir / utf8_path("папка") / utf8_path("капля.wav"), "x");
    CHECK(objects::mark_sound_file(dir, "папка/капля").empty());
    CHECK(objects::mark_sound_file(dir / utf8_path("папка"), "../звон").empty());
    CHECK(objects::mark_sound_file(dir / utf8_path("папка"), "..").empty());
#ifndef _WIN32
    write_text(dir / utf8_path("a\\шаг.wav"), "x"); // a file of that name, not a folder
    CHECK(objects::mark_sound_file(dir, "a\\шаг").empty());
#endif
    fs::remove_all(dir);
}

TEST_CASE("5а: «Покадровая» shows under a frame the mark that begins there, a flag over a note, and a window's bar") {
    using B = objects::FrameMark::Begins;
    objects::Clip c{{0, 1, 2, 3, 4}, 10, true, {{"шаг", true, 1, 1}, {"удар", false, 1, 1}, {"взмах", false, 2, 2}, {"звон", true, 2, 1},
                                                 {"стук", false, 3, 1}, {"тик", false, 4, 1}, {"эхо", false, 7, 1}}};
    const std::vector<objects::FrameMark> f = objects::frame_marks(c);
    REQUIRE(f.size() == 5);
    std::string seen;
    for (const objects::FrameMark& m : f)
        seen += std::string(m.begins == B::Emit ? "⚑" : m.begins == B::Sound ? "♪" : "·") + (m.window ? "▬" : "") + " ";
    // Places from 0: a sound before an emit on 1, after one on 2: the flag either way; the window of «взмах» on 2–3, a
    // one-frame emit no bar of its own, and on 3, inside that window, the bar stays; «эхо» past the frames nowhere.
    CHECK(seen == "· ⚑ ⚑▬ ⚑▬ ⚑ ");
    c.events = {{"шаг", true, 0, 1}};
    CHECK(objects::frame_marks(c)[0].begins == B::Sound);
}
