#pragma once

// What «Старая шахта» sounds like. The usual sounds (steps, a jump, digging,
// a coin) are made from numbers, so the game has sound without any files;
// an object's «Звук» block names its own ones from the game's sounds folder.
// Everything in the world is heard from the hero: louder near, on the left
// or the right.

#include "slice_level.h"

#include "forge/audio/audio.h"
#include "forge/scene/scene.h"

#include <array>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace slice {

// The game's usual sounds.
enum class Cue : u8 { Step, Jump, Land, Splash, Dig, Break, Place, Pickup, Coins, Crate, Torch, Talk, Count };

class SliceSounds {
public:
    // Makes the usual sounds; named ones are read from folder when first
    // played. silent: no device (tests), time still moves.
    void init(const std::filesystem::path& folder, bool silent);
    void shutdown();
    forge::audio::Mixer& mixer() { return mixer_; }

    // The game's settings (0..1) and its pause.
    void set_volumes(f32 master, f32 sound, f32 music);
    void pause(bool on);
    void set_listener(f64 x, f64 y) { mixer_.set_listener(x, y); }

    // A usual sound at a place in the world.
    void play(Cue cue, f64 x, f64 y, f32 volume = 1, f32 pitch = 1);
    // An object's own sound (by name), else the usual one (Cue::Count: none).
    void play(const std::string& name, Cue fallback, f64 x, f64 y, f32 volume = 1, f32 range = 16);
    // The loops of objects with a «Рядом» sound around (x, y): they start,
    // follow their objects and stop when out of hearing; steps of objects
    // that walk. Each frame while the game runs.
    void update_objects(forge::scene::Scene& scene, f64 x, f64 y, f64 dt);
    // Stops what update_objects started (a level goes away).
    void stop_objects();
    // Each frame: without a device, sounds end in time anyway.
    void tick(f64 dt);

    // A sound of the game's sounds folder by name (cached); null when there
    // is no such file or it cannot be read (said once).
    forge::audio::SoundPtr named(const std::string& name);

    // --- for the self-test ---
    u32 played(Cue cue) const { return played_[static_cast<usize>(cue)]; }
    u32 played_named() const { return named_played_; }
    u32 loops() const { return static_cast<u32>(loops_.size()); }

private:
    struct Loop {
        forge::audio::Voice voice;
        std::string name;
        bool seen = false;
    };
    forge::audio::Mixer mixer_;
    bool silent_ = false;
    std::filesystem::path folder_;
    std::array<forge::audio::SoundPtr, static_cast<usize>(Cue::Count)> cues_{};
    std::array<u32, static_cast<usize>(Cue::Count)> played_{};
    u32 named_played_ = 0;
    std::unordered_map<std::string, forge::audio::SoundPtr> named_;
    std::unordered_map<flecs::entity_t, Loop> loops_;
    std::unordered_map<flecs::entity_t, f64> walked_; // tiles since the last step
};

} // namespace slice
