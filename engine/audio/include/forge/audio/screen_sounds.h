#pragma once

// The sound of the game's screens (and of the editor's «Проверить»): the
// music a screen plays while it is up, and a button's sound when it is
// pressed. forge::game::GameScreens says what is wanted, this plays it.
//
//   ScreenSounds s;
//   s.attach(&mixer, game_dir / "sounds");
//   screens.on_music = [&](const std::string& name) { s.music(name); };
//   screens.on_sound = [&](const std::string& name) { s.click(name); };
//
// One music at a time, looped on the music bus: the same name again keeps it
// playing where it is, another stops it and starts its own, "" stops it.
// Button sounds go on the interface bus (the sounds' volume, heard in the
// pause menu too). Sounds are files of the game's sounds folder by name
// (UTF-8, Cyrillic too), read once: a missing or unreadable one is said once
// and stays silent, it is never read again every frame.

#include "forge/audio/audio.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace forge::audio {

class ScreenSounds {
public:
    // mixer: where they play (it outlives this or is detached first); folder:
    // the game's sounds.
    void attach(Mixer* mixer, std::filesystem::path folder);
    // Stops the music and lets the mixer go.
    void detach();
    const std::filesystem::path& folder() const { return folder_; }
    // A sound by name from elsewhere (tests, the self-test), before the folder.
    void put(const std::string& name, ClipPtr clip);
    // Reads the folder's files again when next asked (one was added).
    void forget();

    // The music the screens want now ("": none).
    void music(const std::string& name);
    void stop_music() { music({}); }
    // A scene's music (part 6г2 of docs/models/сцены-модель.md): each tune by the call of the scene that began it, a
    // file of the folder from `since` seconds in, looped on the music bus. What is not wanted any more stops, what
    // goes on takes its volume, what is new begins; one whose `since` went back (its scene began again, part 6г5б)
    // begins again from there.
    struct Tune {
        u32 call = 0;
        std::string name;
        f64 since = 0;
        f32 volume = 0;
        bool operator==(const Tune&) const = default;
    };
    void tunes(const std::vector<Tune>& want);
    const std::vector<Tune>& tunes() const { return tunes_.list; }
    // The voice of the tune a call began (none when its file is missing or it is not playing), for checks.
    Voice tune_voice(u32 call) const;
    // Part 6г5б: the music of a scene behind a screen, as the screens' music (none of music() then): tunes as above,
    // a set of their own beside a cut-scene's, as loud as their volume by the share.
    void screen_tunes(const std::vector<Tune>& want);
    const std::vector<Tune>& screen_tunes() const { return screen_tunes_.list; }
    Voice screen_tune_voice(u32 call) const;
    // How loud the screens' music is under a scene's: 1 as it is, 0 silent.
    void set_share(f32 share);
    f32 share() const { return share_; }
    // A button was pressed: its sound once.
    void click(const std::string& name);

    // The music wanted now and its voice (none when the file is missing).
    const std::string& music_name() const { return music_; }
    Voice music_voice() const { return voice_; }
    bool music_playing() const;
    // Counts, for tests: musics started, button sounds played, sounds that
    // were not found (each name once).
    u32 music_starts() const { return music_starts_; }
    u32 clicks() const { return clicks_; }
    const std::string& last_click() const { return last_click_; }
    const std::vector<std::string>& problems() const { return problems_; }

    // The clip of a name (read once; null when there is none).
    ClipPtr clip(const std::string& name);
    // The clip of a name as read so far (null when it has not been read): what a button played, for checks.
    ClipPtr loaded(const std::string& name) const;

private:
    Mixer* mixer_ = nullptr;
    std::filesystem::path folder_;
    std::unordered_map<std::string, ClipPtr> clips_, put_;
    std::string music_, last_click_;
    Voice voice_;
    // Tunes playing (each as it began: `since` the place it began from) and their voices (by list), at volume × scale;
    // the latest time each was wanted at, to tell a scene begun again.
    struct Tunes {
        std::vector<Tune> list;
        std::vector<Voice> voices;
        std::vector<f64> latest;
    };
    void play_tunes(Tunes& on, const std::vector<Tune>& want, f32 scale);
    Tunes tunes_, screen_tunes_;
    f32 share_ = 1;
    u32 music_starts_ = 0, clicks_ = 0;
    std::vector<std::string> problems_;
};

} // namespace forge::audio
