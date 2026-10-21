#pragma once

// A scene of «Векторная» on the game's screen (part 6г2 of docs/models/сцены-модель.md). «Отдать игре» writes the
// scene's folder game/scenes/<name>/ (forge/vector/scene_file.h); the game's logic shows it (forge.scene.play, the
// node «Показать сцену») and the shell (shell.h) holds it while it plays.
//
// The scene is a document of the game's interface of its own, over the game's screens and HUD and under the shell's
// (dialogue, pause menu, messages): a frame of the camera's view fitted to the screen and cut at its edge, the
// background's colour all round when the scene has one; in it a layer with the camera's matrix, and in that the
// scene's layers in its order: runs of its stage's shapes (a live SVG of stage.svg drawing only those, forge/ui/
// live_svg.h), its pictures (their PNG) and its actors (a live SVG of each one's picture posed as its clip has it, in
// the box its drawing reaches). Where each picture and actor stands is where the stage's picture in the pose of that
// moment puts its group (ui::SvgPlaces): the same code as the runs, so what hangs on an actor does not part from it.
//
// Its time goes by what the shell gives it: only while the game is on the screen (a menu up stands it). It stands at its
// waits (part 6г3): at a wait_action until the shell, hearing the player, says go_on; at a talk while the dialogue it
// opens (on_talk) is open, after one open already ends; its actors' clips and its music go on meanwhile. Its marks go as
// the time it lived (its waits counted) passes them: an emit to on_message, a sound (name, name.wav or name.ogg of the
// game's sounds) to on_sound. Its music: the tunes it plays now and how loud the game's own is under them, to on_music.
// At its end, or skipped, on_end.
//
// Part 6г4: it comes in and goes out by its transitions, each frame of one the whole screen in its track's pose, its
// mask a shape of the stage as the camera sees it; the game's frame is a snapshot of what is drawn under the scene (the
// shell draws the world into a texture for it: wants_backdrop). A scene played by the logic that heard the end of the one
// before comes in over that one's last frame, which is held till the game's logic has run once after the end
// (logic_ran). A picture's or an actor's mask by a shape of the stage is drawn.
//
// Part 6г5б: a scene behind a screen of the game (forge-scene-behind, screens.h) is a player of its own with
// SceneHow::behind: round and round from its start, its frame all its time (no transitions drawn), its waits and talks
// passing at once, its messages not going to the logic (its sounds sound), its music beginning again with each round, as
// much of it showing as its screen does (set_alpha).

#include "forge/core/types.h"
#include "forge/vector/scene_file.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Rml {
class Context;
class ElementDocument;
} // namespace Rml

namespace forge::ui {
class Ui;
}

namespace forge::game {

// How a scene plays (forge.scene.play's options, the node's fields).
struct SceneHow {
    bool pause = true; // the world stands and does not hear the player while it plays
    bool skip = true;  // the pause menu has «Пропустить сцену»
    // Part 6г5б: a scene behind a screen, not a cut-scene: round and round, its frame all its time, waits and talks
    // passing at once, no messages to the logic, never ending (drop stops it).
    bool behind = false;
};

// A tune of the scene's music now (vector::scene_music_at): the file of the game's sounds it plays, by the call
// that began it.
struct SceneTune {
    u32 call = 0;
    std::string file; // "лес.ogg"
    f64 since = 0, volume = 0;
    bool operator==(const SceneTune&) const = default;
};

// A scene's folder as the game reads it: scene.json, stage.motion.json and its actors' clips, checked together
// (vector::check_scene), and the pictures they draw there. False with why, naming the file.
bool read_scene_folder(const std::filesystem::path& dir, vector::SceneMotion& out, std::string& why);
// The scenes a game has: the folders of game_dir/scenes with a scene.json, by name.
std::vector<std::string> scene_names(const std::filesystem::path& game_dir);
// A sound's file in the game's sounds folder as «Векторная» finds it: name, else name.wav, else name.ogg; empty when
// none is there or the name is no file of the folder.
std::string scene_sound_file(const std::filesystem::path& sounds, std::string_view name);

class GameScene {
public:
    GameScene();
    ~GameScene();
    GameScene(const GameScene&) = delete;
    GameScene& operator=(const GameScene&) = delete;

    // Starts scene `name` of game_dir/scenes in the context; false with why («нет сцены «X»: …», the folder's error).
    // One that goes already is refused (its name in why); one whose music is going out after its end lets it go.
    bool play(Rml::Context* context, const std::filesystem::path& game_dir, std::string_view name, const SceneHow& how, std::string& why);
    // Every frame: dt seconds of the scene pass (0 while it stands: then it does not go on from a stop it was let go
    // from either), its frame fitted to the view of w × h pixels. The first frame after play shows its moment 0.
    void update(f64 dt, int width, int height);
    // The player skips it (or the logic stops it): the messages it has left go to on_message in order, its sounds do
    // not; it ends, skipped.
    void skip();
    // It ends at once, without its messages and without on_end (load, a new game, the main menu); its music stops and
    // the game's comes back.
    void drop();

    bool going() const;
    const std::string& name() const;
    const SceneHow& how() const;
    f64 time() const;
    // Part 6г3: the wait or talk it stands at now (null while its time goes), its place in the scene's stops (-1).
    const vector::SceneStop* waiting() const;
    i32 waiting_index() const;
    // The player did what it waits for: its time goes on from the same moment in the next frame.
    void go_on();
    // For checks: how long it stood at each of its stops, and the time it lived (its waits counted).
    std::vector<f64> held() const;
    f64 lived() const;
    // A line about it in the log and in problems(), once.
    void note(const std::string& line);
    // Each play's own number (from 1): on_end's, and over() for the logic waiting on it.
    u64 serial() const;
    // The play with this number has ended (or never began): so too while its last frame is held.
    bool over(u64 serial) const;
    // Whether the play with this number was skipped (when over).
    bool skipped(u64 serial) const;
    // The one on the screen at the top: the one playing, else the last frame held of the one over (part 6г4).
    Rml::ElementDocument* document() const;
    // All of them, bottom to top (the one before standing under the one coming in, part 6г4): the shell pulls them over
    // the game's screens in this order.
    std::vector<Rml::ElementDocument*> documents() const;
    // Part 6г4: whether a transition shows the game's frame now, and the shell is to draw the world into a texture for it.
    bool wants_backdrop() const;
    // The UI whose renderer takes the masks it makes (none: they are not made).
    void set_ui(ui::Ui* ui);
    // Part 6г5б: how much of it shows, 0..1, faded as one picture (a scene behind a screen comes and goes with it).
    void set_alpha(f64 alpha);
    // Whether its scene has music (a call of `music`): a screen without music of its own plays it.
    bool has_music() const;
    // Part 6г4: how many times the game's logic has run so far (its ticks), each frame before update: the last frame of
    // the one over stays till this changes after its end, so the scene the logic begins on hearing it comes in over it
    // (on a screen of 144 Hz the logic's next tick may be two frames on). Not told, each update is taken for a run.
    void logic_ran(u64 runs);

    std::function<void(const std::string& message)> on_message;
    std::function<void(const std::string& file)> on_sound;
    std::function<void(const std::vector<SceneTune>& tunes, f64 game_share)> on_music;
    std::function<void(const std::string& name, bool skipped, u64 serial)> on_end;
    // Its talk("…"): opens the game's dialogue, false when it did not begin (none such, none of its starts fits); whether
    // a dialogue is open; closes the one open (its own, when it is skipped or dropped).
    std::function<bool(const std::string& dialogue)> on_talk;
    std::function<bool()> talking;
    std::function<void()> end_talk;

    // What the scene does not draw or play, each line once (also in the log).
    const std::vector<std::string>& problems() const;
    // For checks: the layers in their order, "run e3 e4", "picture лес", "actor ёжик".
    std::vector<std::string> layers() const;
    // For checks: how many times the runs' pictures were posed since play.
    u64 run_poses() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace forge::game
