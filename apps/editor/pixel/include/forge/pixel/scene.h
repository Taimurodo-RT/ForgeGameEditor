#pragma once

// A scene of «Векторная» (step 6 of docs/models/движение-модель.md, parts 6а and 6б of docs/models/сцены-модель.md): a
// cut-scene of the project's characters, pictures and shapes, written as a character's clip is, seen by the player
// through its camera.
//
//   return {
//     params = { speed = 1 },
//     stage = function(p, rng)                    -- what is on the scene, as draw: shapes, pictures, actors
//       canvas(1920, 1080)
//       background("#203040")
//       picture("лес", "картинки/лес.png", { x = 0, y = 0, w = 1920, h = 1080 })
//       actor("ёжик", "персонажи/Ёжик", { x = 300, y = 900, scale = 6, pivot = { 16, 32 } })
//       path("дорога", { d = "M 300 900 Q 900 860 1300 900", hidden = true })
//     end,
//     scene = function(p)                         -- what happens, as a clip
//       transition("растворение", 0.5)            -- how it comes in over what was (6б)
//       camera():follow("ёжик", 0.5)              -- the player's frame (6б)
//       actor("ёжик"):loop("walk")
//       part("ёжик"):along("дорога", 3 / p.speed)
//       actor("ёжик"):play("idle")
//       part("ёжик/head").rot(10, 0.3)
//       leave("сдвиг", 0.5, { from = "лево" })    -- how it goes back to the game (6б)
//     end,
//   }
//
// The stage is one SVG (stage_svg): its shapes, and a group for each picture and actor that the rig draws the
// picture's pixels or the actor's own rig in (Rig::host). The scene's code is recorded as a clip of that rig; an actor
// plays the clips of its character's code, recorded there and put in the scene's time by the scene's plays
// (Recording::plays). A frame is one ThorVG drawing of it all; the player's frame (draw_frame) is that drawing
// through the camera, in a transition its two frames laid over each other.

#include "forge/pixel/bake.h"
#include "forge/pixel/doc.h"
#include "forge/pixel/draw.h"
#include "forge/pixel/motion.h"
#include "forge/pixel/rig.h"
#include "forge/pixel/script.h"
#include "forge/vector/scene_file.h"

#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace forge::pixel {

inline constexpr f64 kSceneWidth = 1920, kSceneHeight = 1080;
inline constexpr u32 kMaxSceneItems = 256; // pictures and actors on one scene

// A picture or an actor that stage(p, rng) put on the scene: a group of the stage's drawing (its `group` in
// Stage::drawing.shapes), placed, with a rect of its size in it that it is drawn over.
struct StageItem {
    enum class Kind : u8 { Picture, Actor };
    Kind kind = Kind::Picture;
    std::string id;
    std::string file;   // a picture's path in «Ресурсы»; an actor's character, the path of its .pixel.json without it
    i32 group = -1;     // in Stage::drawing.shapes
    f64 x = 0, y = 0;   // a picture's top left; the point of the scene an actor's pivot stands at
    f64 w = 0, h = 0;   // a picture's size on the scene (its own unless given); an actor's picture's (its units)
    bool sized = false; // a picture's w and h were given
    f64 scale = 1, sx = 1, sy = 1, rot = 0; // an actor's, round its pivot
    vector::Point pivot;  // an actor's: the point of its picture (its units) that stands at x, y
    vector::Point origin; // an actor's picture's top left in its units (its viewBox)
    std::map<std::string, f64> params; // an actor's, over its character's
    u32 line = 0; // the call's line in the script
    i32 site = -1; // the call in the script's CodeSites::sites()
    bool operator==(const StageItem&) const = default;
};

struct Stage {
    Drawing drawing;        // canvas(w, h) of the scene, kSceneWidth × kSceneHeight without it; the items' groups in it
    std::string background; // "#rrggbb" under all of it; empty: none, the scene is transparent
    u32 background_line = 0;
    std::vector<StageItem> items; // in the order of their calls
    f64 width() const { return drawing.canvas ? drawing.width : kSceneWidth; }
    f64 height() const { return drawing.canvas ? drawing.height : kSceneHeight; }
    const StageItem* item(std::string_view id) const;
    bool operator==(const Stage&) const = default;
};

// Part 6б: the scene's camera is a part of the recording only, no shape of the stage: its x and y move the middle of its
// frame from the middle of the canvas, its scale is camera's zoom, its rot the frame's turn. The frames of a
// transition are parts of the recording too: the old and the new one of the entrance, then of the leave.
inline constexpr const char* kCameraPart = "camera";
inline constexpr const char* kLayerParts[4] = {"camera/in-old", "camera/in-new", "camera/out-old", "camera/out-new"};
// Whether an id is the camera's or a transition frame's.
bool camera_part(std::string_view id);
// What was before a scene and the game after it, in the tab: grey of the canvas's size.
inline constexpr Rgba8 kOutsideScene{128, 128, 128, 255};

// The camera as a frame of the recording puts it.
struct SceneCamera {
    f64 x = 0, y = 0, zoom = 1, rot = 0;
    bool operator==(const SceneCamera&) const = default;
};
// Which of a scene's times a moment is: coming in, the scene, going out, after it (the game's, as a game plays it).
using ScenePhase = vector::ScenePhase;
// The stage's units to the player's frame, a w × h canvas: q = (w/2, h/2) + zoom · R(−rot) · (p − c), c = the
// middle of the canvas moved by x, y.
Affine camera_matrix(const SceneCamera& cam, f64 w, f64 h);
// The player's frame at a size of a view: the stage's shape, its longer side `side` points, each side at least 1.
void frame_size(f64 stage_w, f64 stage_h, f64 side, u32& w, u32& h);

// An actor's group's transform: its picture so that `pivot` stands at x, y, turned by rot and scaled round it.
Affine actor_matrix(const StageItem& actor);
// The SVG of the stage: its drawing, the background under it all, the same stage the same text.
std::string stage_svg(const Stage& stage);

// «Вступление.scene.json» beside «Вступление.luau»: what is dragged and set in the tab, not code.
//   {"script": "Вступление.luau", "seed": 1, "params": {"speed": 1}, "fps": 60, "range": [0, 4.5],
//    "waits": {"встреча": 0.6}, "durations": {"оборот": 0.8}}
struct SceneDoc {
    std::string script;
    u64 seed = 1;
    std::map<std::string, f64> params;
    f64 fps = 60; // the player's step and the timeline's frames
    bool ranged = false;
    f64 from = 0, to = 0;
    MotionLengths lengths;
    bool operator==(const SceneDoc&) const = default;
};
bool read_scene_doc(std::string_view json, SceneDoc& out, std::string& why);
std::string write_scene_doc(const SceneDoc& doc);
// The code of a new scene «name».
std::string new_scene_code(std::string_view name);

// An SVG's ids put under a name: id="x" → id="ёжик/x", url(#x) → url(#ёжик/x), href="#x" → href="#ёжик/x", and all
// of it in a group with that id inside the root, which the scene's looks of the whole actor go on.
std::string prefix_svg_ids(std::string_view svg, std::string_view name);
// A pose's ids put under the name the same way: its parts, morphs and their targets, masks, links, order and copies.
Pose prefix_pose(const Pose& pose, std::string_view name);
// The scene's transition of an actor's part over the pose its clip gives it (rule 5 of the actors' clips): moves,
// turns, skews and the dash add; scales and opacity multiply; it is seen when both say so; the colours, the width, the
// share drawn, the flash, the blur and the shadow are the scene's when it moved them from rest, else the clip's; the
// scene's mask over the clip's.
void over_part(PartPose& clip, const PartPose& scene);

// What a scene reads, by paths in «Ресурсы» (forward slashes): a file's bytes (a picture, a character's .pixel.json
// and code), the SVG a character's file names by its Guid, the modules of the game's animations folder.
struct SceneFiles {
    std::function<bool(const std::string& path, std::vector<u8>& bytes, std::string& why)> read;
    std::function<bool(const std::string& guid, std::string& text, std::string& why)> svg;
    ModuleReader modules;
};

// A character as a scene reads it: its file, code and pictures, drawn as its own tab draws them.
struct SceneCharacter {
    std::string file; // its path without .pixel.json
    PixelDoc doc;
    std::string code;
    std::string svg; // its main picture
    std::vector<NamedDrawing> drawings;
    Drawing drawn;     // the main picture as its code draws it
    bool by_code = false;
    Rig rig;           // its clips are recorded against it, its ids as they are
};
// False with why: no file, a file or code that does not read, a picture that does not draw.
bool load_character(const SceneFiles& files, const std::string& file, SceneCharacter& out, std::string& why);

// Which clip an actor shows at t of a scene and its moment in that clip (rules 1–4 of the actors' clips): the play
// started last at t or before, a stop holding the moment of the one before it; null play at rest. `held` (part 6в):
// the seconds the scene waited at each of rec.stops, or none; a clip goes on while the scene waits, so its moment is
// ahead by the waits after its start up to t (a stop at its start counts when made before it). `waiting`: the stop of
// rec.stops the scene stands at now (-1: none), what was made after it at its moment not begun yet.
struct ActorMoment {
    const ActorPlay* play = nullptr;
    f64 time = 0;
};
ActorMoment actor_moment(const Recording& rec, std::string_view actor, f64 t, std::span<const f64> held = {}, i32 waiting = -1);

// Part 6в: the scene's music at t, each sound file playing then — the one coming in and the ones going out over their
// fades — with how long it has played (from the call that began it) and its volume then. Music goes on while the scene
// waits, its fades too: with `held` and `waiting` as actor_moment's, a call's moment and the moment now are their
// scene times and the waits before them.
struct MusicVoice {
    std::string name;
    f64 since = 0, volume = 0;
    u32 call = 0; // in rec.music: the call that began it
    bool operator==(const MusicVoice&) const = default;
};
std::vector<MusicVoice> music_at(const Recording& rec, f64 t, std::span<const f64> held = {}, i32 waiting = -1);
// The timeline's blocks of the scene's music: one file from the call that began it to the next call of another or
// music(nil), else the scene's end; `calls` the calls of it in that time, the first the one that began it.
struct MusicRun {
    std::string name;
    f64 start = 0, end = 0, fade = 0;
    std::vector<u32> calls;
};
std::vector<MusicRun> music_runs(const Recording& rec);
// The marks of the actors' clips in the scene's time (rule 6): each play's up to the next play of its actor or the
// scene's end, a loop's on every round, named by its actor; by time.
struct SceneMarkSource {
    virtual ~SceneMarkSource() = default;
    // The recording of a play's clip, null with why.
    virtual const Recording* clip_of(const ActorPlay& play, std::string& why) = 0;
};
bool actor_marks(const Recording& rec, SceneMarkSource& clips, std::vector<Mark>& out, std::string& why);

// What a frame of the scene gives its pictures (part 6г1): the scene's own pose as its recording gives it, the camera,
// the transitions' frames and the phase it makes; the stage's pose (its own parts, its actors' groups) and each actor's
// (its clip's under its name with the scene's over it), as Scene::pose puts them on the rigs.
struct ScenePoses {
    Pose scene;
    SceneCamera camera;
    PartPose layers[4]; // kLayerParts
    ScenePhase phase = ScenePhase::Scene;
    Pose stage;
    std::map<std::string, Pose> actors;
};

class Scene : SceneMarkSource {
public:
    Scene();
    ~Scene() override;
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    // Reads the code, runs its stage, reads the characters and pictures the stage names and builds the stage's rig with
    // them. False with error() and error_line() when any of it fails: nothing to show.
    bool load(std::string_view code, const SceneDoc& doc, const SceneFiles& files);
    bool loaded() const;
    // Records scene(p) with the doc's parameters, seed and lengths: false with error(); the stage stays, at rest.
    bool record();
    const Recording* recording() const; // null until recorded
    // Poses everything at t of the scene: its own transitions, its actors' clips under theirs (at rest without a
    // recording). False with error().
    bool frame(f64 t, std::span<const f64> held = {}, i32 waiting = -1);
    // The same with the scene's own pose given (its parts, its actors' and their parts', as the script's frame() of a
    // recording gives it) and the actors' clips by the plays of `rec` (null: at rest): the tab poses the scene by the
    // recording on its timeline, a part dragged moved in it; `held` the seconds it waited at rec's stops and `waiting`
    // the one it stands at (part 6в, as actor_moment's), its actors' clips ahead by them. False with error().
    bool pose(f64 t, const Recording* rec, const Pose& scene, std::span<const f64> held = {}, i32 waiting = -1);
    // Part 6г1: what frame() would give the pictures at t, by the recording, without posing them (the game's files are
    // checked against it). False with error().
    bool poses(f64 t, std::span<const f64> held, i32 waiting, ScenePoses& out);
    // The frame as posed, smooth: the scene's units to the drawing's pixels by px, or fitted in.
    assets::CookedTexture draw(u32 w, u32 h, const Affine& px);
    assets::CookedTexture draw(u32 w, u32 h);
    // Part 6б: the camera as the last pose() put it, and the stage's units to the player's frame by it.
    const SceneCamera& camera() const;
    Affine camera_matrix() const;
    // The player's frame as the last pose() put it: the scene through the camera; in a transition its two frames,
    // the old under the new, each in its frame's pose (moved, turned and scaled round the frame's middle, its opacity,
    // its mask a shape of the stage as the camera sees it); before the scene and after its leave kOutsideScene. In a w
    // × h drawing by px (the frame's units, the canvas's, to its pixels), or fitted in.
    assets::CookedTexture draw_frame(u32 w, u32 h, const Affine& px);
    assets::CookedTexture draw_frame(u32 w, u32 h);

    const Stage& stage() const;
    const SceneDoc& doc() const;
    PoseScript& script();
    Rig& rig();
    const Rig& rig() const;
    // An actor's rig as the scene draws it, its ids «ёжик/…»; null for none.
    const Rig* actor_rig(std::string_view id) const;
    // An actor's character as read; null for none.
    const SceneCharacter* character(std::string_view actor) const;
    // Part 6г1: the code of an actor's character that records the clip of a play, and the values it records it with
    // (the character's parameters, the actor's over them, the play's over those). Null with why.
    PoseScript* actor_script(const ActorPlay& play, std::map<std::string, f64>& values, std::string& why);
    // The scene's marks and its actors' clips' at their moments of the scene, by time: false with error().
    bool marks(std::vector<Mark>& out);
    // Only the actors' clips' marks, by the plays of `rec` (a recording of this scene's code), by time.
    bool actor_marks(const Recording& rec, std::vector<Mark>& out);
    // The files of «Ресурсы» it read: when one changes, it is loaded again.
    const std::vector<std::string>& files() const;

    const std::string& error() const;
    u32 error_line() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    const Recording* clip_of(const ActorPlay& play, std::string& why) override;
};

} // namespace forge::pixel
