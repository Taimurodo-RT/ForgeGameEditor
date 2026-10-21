#pragma once

// A scene of «Векторная» as a game plays it (part 6г1 of docs/models/сцены-модель.md). «Отдать игре» writes the scene's
// folder game/scenes/<name>/: scene.json (this file: what is on the scene, its actors' clips in its time, its music,
// waits, talks and transitions), the stage's picture stage.svg with its tracks stage.motion.json (a clip "scene" of the
// screens' format, forge/vector/motion_file.h), each actor's picture and clips in actors/ and the pictures in pictures/.
// The game runs none of the author's code: the files hold what the tab shows, worked out.
//
// The scene's time stands while it waits for the player or a talk; its actors' clips and its music go on (part 6в).
// `held` is how long it stood at each of SceneFile::stops, `waiting` the one it stands at now (-1: none): what was made
// at that moment after the wait has not begun yet. These are the tab's rules (forge::pixel::actor_moment, music_at),
// which «Отдать игре» checks the game's against.

#include "forge/vector/motion_file.h"

#include <array>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace forge::vector {

// picture(id, file, { x, y, w, h }) of the stage: its PNG in the folder and where it stands, its top left and size.
struct ScenePicture {
    std::string id;
    std::string file; // "pictures/лес.png"
    f64 x = 0, y = 0, w = 0, h = 0;
    bool operator==(const ScenePicture&) const = default;
};

// A box in a picture's units: its top left and size.
struct SceneBox {
    f64 x = 0, y = 0, w = 0, h = 0;
    bool operator==(const SceneBox&) const = default;
};

// actor(id, character, …) of the stage: its picture and clips in the folder; its picture's top left in its own units and
// its size there (the stage's group of it stands so that they fall where the actor is). An actor that plays no clip has
// no file of clips: the parts of it the scene moves stand by the stage's records (stage.motion.json), in its main
// drawing. Part 6г2: where its drawing reaches in the scene beyond that box, when it does (the game draws it in this
// box, not cut at its own); w 0: within its box. Part 6г6: how far each edge of that reach (left, top, right, bottom)
// goes out per unit of a parameter of the scene, away from the file's value either way; 0: not at all.
struct SceneActor {
    std::string id;
    std::string character; // its file in «Ресурсы», for messages: "персонажи/Ёжик"
    std::string picture;   // "actors/ёжик.svg"
    std::string motion;    // "actors/ёжик.motion.json"; empty: none
    Point origin;
    f64 w = 0, h = 0;
    SceneBox reach;
    std::map<std::string, std::array<f64, 4>> reach_by;
    bool operator==(const SceneActor&) const = default;
};

// play, loop and stop of an actor: from `at` of the scene its clip `clip` (one of its motion file's) at `speed`; of two
// at one moment the one with the greater `order` is seen, and a wait at that moment with a smaller one does not move it.
// Part 6г5а: a play or loop with a `blend` comes in over that many seconds of its clip's play from what the actor showed.
struct ScenePlay {
    enum class Kind : u8 { Play, Loop, Stop };
    Kind kind = Kind::Play;
    std::string actor;
    std::string clip; // empty for a stop
    f64 at = 0, speed = 1;
    u32 order = 0;
    f64 blend = 0; // 0: at once
    bool operator==(const ScenePlay&) const = default;
};

// music("name", { fade, volume }) from `at`; no name: music(nil), silence.
struct SceneMusicCall {
    f64 at = 0;
    u32 order = 0;
    std::string name;
    f64 fade = 0, volume = 1;
    bool operator==(const SceneMusicCall&) const = default;
};

// wait_action("Действие") (empty: any key) or talk("разговор") at `at`.
struct SceneStop {
    f64 at = 0;
    u32 order = 0;
    bool talk = false;
    std::string name;
    bool operator==(const SceneStop&) const = default;
};

// transition and leave: the kind ("растворение", "сдвиг", "приближение"; empty for the author's own), «сдвиг»'s edge,
// its start and length. Its two frames are the parts camera/in-old, camera/in-new, camera/out-old and camera/out-new of
// the stage's tracks.
struct SceneTransition {
    bool leave = false;
    std::string kind, from;
    f64 at = 0, length = 0;
    bool operator==(const SceneTransition&) const = default;
};

struct SceneFile {
    std::string name; // the scene's in the game: its folder's
    std::string from; // its code in «Ресурсы»: "сцены/Вступление.luau"
    f64 width = 1920, height = 1080;
    std::string background; // "#rrggbb" under it all; empty: transparent
    f64 length = 0;         // with its leave
    std::vector<ScenePicture> pictures;
    std::vector<SceneActor> actors;
    std::vector<ScenePlay> plays;       // as recorded
    std::vector<SceneMusicCall> music;  // as recorded
    std::vector<SceneStop> stops;       // by time
    std::vector<SceneTransition> transitions;
    std::vector<std::string> not_drawn; // kMotionNotDrawn's words, and "mask_by_item"
    // Part 6г6: the parameters of the scene's code, by name, at the values the tab gave it at (each of its motion files
    // has them too); a parameter a game does not change, with why («длина сцены»).
    std::vector<MotionParam> params;
    std::map<std::string, std::string> fixed;
    bool operator==(const SceneFile&) const = default;

    const SceneActor* actor(std::string_view id) const;
    const MotionParam* param(std::string_view name) const;
};

// The newest version a game reads. A file with parameters (part 6г6) is version 3, one with a blend (part 6г5а) 2, one
// with neither 1, so a game made before reads what it can.
inline constexpr u32 kSceneFileVersion = 3;
// The parts of the stage's tracks that are the camera and the transitions' frames, not shapes of its picture.
inline constexpr const char* kSceneCamera = "camera";
inline constexpr const char* kSceneFrames[4] = {"camera/in-old", "camera/in-new", "camera/out-old", "camera/out-new"};

// False with why (Russian, naming the field: "plays[2].clip: …") when the text is not such a file, or one newer than the
// game reads.
bool read_scene_file(std::string_view json, SceneFile& out, std::string& why);
// Numbers in their shortest form that reads back as the same number; a trailing newline.
std::string write_scene_file(const SceneFile& file);

// The scene with its files read: the stage's tracks and each actor's clips, by SceneFile::actors (an empty file for one
// without).
struct SceneMotion {
    SceneFile file;
    MotionFile stage; // its clip "scene"
    std::vector<MotionFile> actors;
};
// Whether they fit together: the stage's clip "scene", a file for each actor, each play's clip in its actor's, each motion
// file with the scene's parameters (part 6г6). False with why.
bool check_scene(const SceneMotion& scene, std::string& why);

// Part 6г6: the scene at the values given of its parameters, by name: each clip of the stage's and the actors' files with
// its numbers at them (motion_clip_with), each actor's reach wider by its reach_by × |value − the file's|, outward to
// whole units (its box when it had no reach). A name the scene has not, or a value that is no number: false with why («в
// сцене «Вступление» нет параметра «heigth»: есть h, n», «у сцены «Вступление» нет параметров»), the scene as it was. A
// parameter a game does not change (`fixed`) given another value than the file's: a line in `notes` («параметр n из
// игры не меняется: длина сцены») and the file's numbers.
bool scene_with_params(SceneMotion& scene, const std::map<std::string, f64>& values, std::vector<std::string>& notes, std::string& why);

// One clip of what an actor shows in a blend (part 6г5а): play (an index of file.plays; -1: the character at rest) at its
// moment `time`, coming in by k over what the steps before it show.
struct SceneBlendStep {
    i32 play = -1;
    f64 time = 0;
    f64 k = 1;
    bool operator==(const SceneBlendStep&) const = default;
};
// Which play an actor shows at t of the scene (an index of file.plays; -1: at rest) and its moment in that clip: the play
// begun last, a stop holding the moment the one before it showed; a clip that is no loop held at its end, a loop round
// and round; ahead by the scene's waits after its start.
// Part 6г5а: while a play's blend goes (k = smooth of its clip's play over its blend, held to the clip's length when it
// is no loop), `blend` is what the actor shows, the first step at rest or a clip at its moment, each next blended in
// over it, the last this play. The first: the call before it going on by its own time; that call's pose at the play's
// start, held, when it was a stop or itself in a blend then; the character at rest when there was none. Empty when no
// blend goes.
struct SceneActorMoment {
    i32 play = -1;
    f64 time = 0;
    std::vector<SceneBlendStep> blend;
    bool operator==(const SceneActorMoment&) const = default;
};
SceneActorMoment scene_actor_moment(const SceneMotion& scene, std::string_view actor, f64 t, std::span<const f64> held = {}, i32 waiting = -1);

// The music at t: the one coming in and the ones going out over their fades, how long each has played (from the call
// that began it) and its volume. Music goes on while the scene waits, its fades too.
struct SceneMusicVoice {
    std::string name;
    f64 since = 0, volume = 0;
    u32 call = 0; // in file.music: the call that began it
    bool operator==(const SceneMusicVoice&) const = default;
};
std::vector<SceneMusicVoice> scene_music_at(const SceneFile& file, f64 t, std::span<const f64> held = {}, i32 waiting = -1);

// What the player sees at t: the scene coming in over what was, the scene, it going out to the game, then the game.
enum class ScenePhase : u8 { Enter, Scene, Leave, Gone };
ScenePhase scene_phase(const SceneFile& file, f64 t);

// The scene's transition of an actor's part over the pose its clip gives it (rule 5 of the actors' clips, as
// forge::pixel::over_part): moves, turns, skews and the dash add; scales and opacity multiply; it is seen when both say
// so; the colours, the width, the share drawn, the flash and the morph are the scene's when it moved them from rest, else
// the clip's; the scene's mask over the clip's. `clip`'s matrix is not made again (motion_part_pose does).
void scene_over(MotionPartPose& clip, const MotionPartPose& scene);
// Whether property p of a whole actor goes on its picture (its colours, width, dash, share drawn, tint and flash), not
// on its group of the stage.
bool scene_actor_look(MotionProp p);

// An actor at t: its clip's pose at its moment (in a blend, its steps' poses blended one into the next: motion_blend)
// with the scene's over it, each part as it stands in the drawing shown; the scene's looks of the whole actor on its
// picture's group (the part with the actor's id); its copies drawn, its order's steps (the clip's, then the scene's of
// its parts) and the drawing it shows.
struct SceneActorPose {
    std::string id;
    SceneActorMoment moment;
    std::vector<MotionPartPose> parts;
    std::vector<MotionCopy> copies;
    std::vector<MotionOrderStep> order;
    MotionShown shown;
    std::vector<MotionPartPose> from_parts; // in a swap: the parts as they stand in the drawing it comes from (part 6г2)
};
// Everything at t: the stage's parts (shapes, pictures, the actors' groups: where they stand, their opacity, effects,
// mask and link), its copies drawn and its order's steps; the camera's and the frames' values; the phase; each actor.
struct ScenePose {
    std::vector<MotionPartPose> stage;
    std::vector<MotionCopy> copies;
    std::vector<MotionOrderStep> order;
    MotionPartPose camera;
    MotionPartPose frames[4]; // kSceneFrames
    ScenePhase phase = ScenePhase::Scene;
    std::vector<SceneActorPose> actors; // by file.actors
};
ScenePose scene_pose(const SceneMotion& scene, f64 t, std::span<const f64> held = {}, i32 waiting = -1);
// The camera's matrix (part 6г2): the stage's units to the player's frame, a w × h canvas, q = (w/2, h/2) + zoom ·
// R(−rot) · (p − c), c the middle of the canvas moved by its x, y; its zoom is its scale (forge::pixel::camera_matrix).
MotionMatrix scene_camera_matrix(const MotionPartPose& camera, f64 w, f64 h);

// A mark of the scene (part 6г2): its own (sound and emit of the stage's clip "scene") or of an actor's clip (rule 6 of
// the actors' clips), its name, whether it is a sound, the actor whose clip made it ("" for the scene's) and its moment.
struct SceneMark {
    std::string name;
    bool sound = false;
    std::string actor;
    f64 at = 0;
    bool operator==(const SceneMark&) const = default;
};
// Every mark of the scene, as the tab lists them (forge::pixel::Scene::marks): by their moments, the scene's own before
// the actors' at one moment, the actors' by actor, then by their plays. An actor's mark at moment m of its clip comes at
// its play's start + m / speed, a loop's once each round, up to the actor's next play, loop or stop (not at it) or the
// scene's end (at it too). The scene's time without waits (part 6г3 makes it wait). False with why when there are more
// than 100 000 (a loop too short).
bool scene_marks(const SceneMotion& scene, std::vector<SceneMark>& out, std::string& why);

// Where a scene is in its time as a game plays it (part 6г3): its moment t, how long it stood at each of file.stops (a
// missing one: not at all) and the one it stands at now (-1: none).
struct SceneAt {
    f64 t = 0;
    std::vector<f64> held;
    i32 waiting = -1;
};
// The time it has lived then: seconds since it began, its waits counted.
f64 scene_lived(const SceneAt& at);
// How long it has lived since a thing made at `when` (`order`: a play's, music's or stop's own, a mark's the next one's)
// began, -1 when that has not begun.
f64 scene_since(const SceneFile& file, const SceneAt& at, f64 when, u32 order);
// The marks passed from `from` to `to` (later in the same play; null `from`: from before its start, so its marks at 0
// too), each once, by the time the scene lived (SceneMark::at): its own mark at its moment and the waits made before it
// (a stop of its moment made before it when the stop's order is less than the mark's); an actor's mark at the time lived
// when its play began and its moment in the clip after (a clip goes on while the scene waits), a loop's once each round, up
// to the actor's next play, loop or stop (not at it) or the scene's end. A thing made after the stop the scene stands at,
// at its moment, has not begun. In one moment as scene_marks has them; without waits the same marks at the same moments.
// False with why when more than 100 000 pass at once (a loop too short).
bool scene_marks_between(const SceneMotion& scene, const SceneAt* from, const SceneAt& to, std::vector<SceneMark>& out, std::string& why);

} // namespace forge::vector
