#pragma once

// Sound for games and the editor: clips decoded once into memory, and a
// mixer that plays many of them at once on the default output device.
//
//   audio::Mixer mixer;
//   mixer.open();                                   // no device: plays silently
//   audio::ClipPtr ding = audio::load("coin.ogg");
//   mixer.play(ding, {.volume = 0.8f, .pan = -0.3f});
//   audio::Voice fire = mixer.play(crackle, {.loop = true});
//   mixer.set(fire, 0.4f, 0.2f);                    // quieter, a bit to the right
//
// Sounds in the world have a place: the mixer hears them from its listener
// (the hero or the camera), louder near, to the left or right, and keeps
// that up to date as either moves.
//
//   mixer.set_listener(hero_x, hero_y);
//   mixer.play(steps, {.at = true, .x = 10, .y = 4, .range = 16});
//
// A Sound is what games ask for by name: one or more clips (a random one
// each time) with a little spread of pitch and volume, so repeats do not
// sound the same. Today a sound is one file; the sound editor will make
// richer ones without anything else changing.
//
// Everything is stereo float at kRate; files of another rate or layout are
// converted when they load. WAV and OGG are read; other formats go through
// the editor's converters first.

#include "forge/core/types.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

struct SDL_AudioStream;

namespace forge::audio {

inline constexpr u32 kRate = 48000;

// A decoded sound: interleaved stereo samples at kRate.
struct Clip {
    std::vector<f32> samples;
    u32 frames() const { return static_cast<u32>(samples.size() / 2); }
    f64 seconds() const { return static_cast<f64>(frames()) / kRate; }
};
using ClipPtr = std::shared_ptr<const Clip>;

// WAV or OGG from memory or a file; null (with the reason) when it is not one.
ClipPtr decode(std::span<const u8> bytes, std::string* error = nullptr);
ClipPtr load(const std::filesystem::path& file, std::string* error = nullptr);
// A clip as a WAV file (16-bit stereo at kRate).
bool encode_wav(const Clip& clip, std::vector<u8>& out);
// True for the extensions decode() reads (".wav", ".ogg").
bool readable(const std::filesystem::path& file);

// Sounds go through one of the buses; each has its own volume (the game's
// settings) and the sound bus pauses with the game.
enum class Bus : u8 { Sound, Music };

struct Play {
    f32 volume = 1;
    f32 pan = 0;   // -1 left .. 1 right
    f32 pitch = 1; // 2: an octave up and twice as fast
    bool loop = false;
    Bus bus = Bus::Sound;
    // In the world: heard from the listener, up to range tiles away.
    bool at = false;
    f64 x = 0, y = 0;
    f32 range = 16;
};

struct Sound {
    std::vector<ClipPtr> clips; // one is picked at random each time
    f32 volume = 1;
    f32 pitch_spread = 0;  // 0.1: from 10% lower to 10% higher
    f32 volume_spread = 0; // 0.2: up to 20% quieter
    f32 range = 16;        // tiles at which it fades out (in the world)
    bool empty() const { return clips.empty(); }
};
using SoundPtr = std::shared_ptr<const Sound>;
// A sound of one clip.
SoundPtr sound_of(ClipPtr clip, f32 volume = 1, f32 pitch_spread = 0);

// A sound that is playing; it stays valid after the sound ends (calls on
// it then do nothing).
struct Voice {
    u32 id = 0;
    explicit operator bool() const { return id != 0; }
    bool operator==(const Voice&) const = default;
};

// Volume and pan of a sound at (dx, dy) tiles from the listener, heard up
// to range tiles away: full volume close by, fading to nothing at range.
void place(f64 dx, f64 dy, f32 range, f32& volume, f32& pan);

class Mixer {
public:
    Mixer() = default;
    ~Mixer();
    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;

    // Plays on the default output device. False when there is none (a
    // server, a test machine): sounds still "play" and end on time, and
    // mix() gives what would be heard.
    bool open();
    void close();
    bool has_device() const { return stream_ != nullptr; }

    Voice play(const ClipPtr& clip, const Play& p = {});
    // One of the sound's clips, with its spread; p's volume and pitch on top
    // (and its range, when p has none of its own).
    Voice play(const Sound& sound, Play p = {});
    bool playing(Voice v) const;
    // Fades out quickly (no click).
    void stop(Voice v);
    void stop_all();
    void set(Voice v, f32 volume, f32 pan);
    void set_pitch(Voice v, f32 pitch);
    // Where a sound in the world is now (it follows its object).
    void set_position(Voice v, f64 x, f64 y);
    // Where sounds in the world are heard from.
    void set_listener(f64 x, f64 y);

    void set_master(f32 volume);
    void set_volume(Bus bus, f32 volume);
    // A paused bus keeps its sounds where they are (the game's pause menu).
    void pause(Bus bus, bool on);

    // Fills frames of interleaved stereo; the device calls it from its own
    // thread, tests call it directly (without a device).
    void mix(f32* out, u32 frames);
    // Without a device, time still moves: sounds end as if heard.
    void advance(f64 seconds);
    // Sounds playing now.
    u32 voices() const;
    // How many sounds were ever started (for tests).
    u64 started() const { return started_; }

    static constexpr u32 kMaxVoices = 64;

private:
    struct Slot {
        ClipPtr clip;
        u32 id = 0;
        f64 pos = 0; // in frames of the clip
        f32 volume = 1, pan = 0, pitch = 1;
        f32 gain_l = 0, gain_r = 0; // what was used last, ramped towards the new
        bool loop = false, stopping = false, started = false;
        Bus bus = Bus::Sound;
        bool at = false;
        f64 x = 0, y = 0;
        f32 range = 16;
    };
    void mix_locked(f32* out, u32 frames);
    Slot* find(Voice v);
    const Slot* find(Voice v) const;

    mutable std::mutex mutex_;
    std::vector<Slot> slots_;
    u32 next_id_ = 1;
    u64 started_ = 0;
    f32 master_ = 1;
    f64 listener_x_ = 0, listener_y_ = 0;
    u32 random_ = 0x9E3779B9u;
    f32 bus_volume_[2] = {1, 1};
    bool bus_paused_[2] = {false, false};
    SDL_AudioStream* stream_ = nullptr;
    std::vector<f32> scratch_;
};

// --- simple sounds made from numbers, for games without their own files ---

enum class Wave : u8 { Sine, Square, Triangle, Noise };

struct Tone {
    Wave wave = Wave::Sine;
    f32 from_hz = 440, to_hz = 440; // a slide from one pitch to another
    f32 seconds = 0.2f;
    f32 attack = 0.005f; // seconds to full volume
    f32 decay = 4;       // how fast it dies away (0: stays until the end)
    f32 volume = 0.5f;
    f32 start = 0;       // seconds after the clip's beginning
};
// The tones added together into one clip.
ClipPtr synth(std::span<const Tone> tones);

} // namespace forge::audio
