#include "forge/audio/audio.h"
#include "forge/core/file.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace forge;

namespace {

// A WAV file in memory: mono 16-bit at 22 050 Hz, a sine.
std::vector<u8> make_wav(u32 frames, u32 rate = 22050) {
    std::vector<u8> out;
    auto u32le = [&](u32 v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
    };
    auto u16le = [&](u16 v) {
        out.push_back(static_cast<u8>(v));
        out.push_back(static_cast<u8>(v >> 8));
    };
    out.insert(out.end(), {'R', 'I', 'F', 'F'});
    u32le(36 + frames * 2);
    out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    u32le(16);
    u16le(1);
    u16le(1);
    u32le(rate);
    u32le(rate * 2);
    u16le(2);
    u16le(16);
    out.insert(out.end(), {'d', 'a', 't', 'a'});
    u32le(frames * 2);
    for (u32 i = 0; i < frames; ++i)
        u16le(static_cast<u16>(static_cast<i16>(std::sin(i * 0.1) * 16000)));
    return out;
}

f32 peak(const std::vector<f32>& buf, int channel) {
    f32 p = 0;
    for (usize i = static_cast<usize>(channel); i < buf.size(); i += 2) p = std::max(p, std::fabs(buf[i]));
    return p;
}

std::vector<f32> mixed(audio::Mixer& m, u32 frames) {
    std::vector<f32> buf(static_cast<usize>(frames) * 2);
    m.mix(buf.data(), frames);
    return buf;
}

} // namespace

TEST_CASE("audio: WAV and OGG decode to stereo at 48 kHz") {
    std::string error;
    const std::vector<u8> wav = make_wav(22050);
    audio::ClipPtr clip = audio::decode(wav, &error);
    REQUIRE_MESSAGE(clip, error);
    CHECK(clip->samples.size() % 2 == 0);
    CHECK(std::abs(clip->seconds() - 1.0) < 0.01);
    CHECK(peak(clip->samples, 0) > 0.4f);
    CHECK(peak(clip->samples, 1) > 0.4f); // mono goes to both sides

    audio::ClipPtr ogg = audio::load(std::filesystem::path(FORGE_SOURCE_DIR) / "tests/data/tone.ogg", &error);
    REQUIRE_MESSAGE(ogg, error);
    CHECK(std::abs(ogg->seconds() - 0.25) < 0.01);
    CHECK(peak(ogg->samples, 0) > 0.3f);

    const std::vector<u8> junk = {'I', 'D', '3', 4, 0, 0, 1, 2, 3};
    CHECK_FALSE(audio::decode(junk, &error));
    CHECK(error.find("Конвертировать") != std::string::npos);
    CHECK(audio::readable("a/шаг.OGG"));
    CHECK_FALSE(audio::readable("a/шаг.mp3"));
}

TEST_CASE("audio: synthesized sounds") {
    const audio::Tone tones[] = {{audio::Wave::Sine, 880, 1760, 0.1f}, {audio::Wave::Noise, 2000, 500, 0.2f, 0.001f, 10, 0.3f, 0.05f}};
    audio::ClipPtr clip = audio::synth(tones);
    REQUIRE(clip);
    CHECK(std::abs(clip->seconds() - 0.25) < 0.001);
    CHECK(peak(clip->samples, 0) > 0.2f);
    CHECK(std::fabs(clip->samples.back()) < 1e-3f); // ends quietly, no click
    std::vector<u8> wav;
    REQUIRE(audio::encode_wav(*clip, wav));
    audio::ClipPtr back = audio::decode(wav);
    REQUIRE(back);
    CHECK(back->frames() == clip->frames());
    CHECK(std::fabs(back->samples[1000] - clip->samples[1000]) < 1e-3f);
}

TEST_CASE("audio: the mixer plays, pans, loops and stops") {
    audio::Mixer m; // no device: mix() is what would be heard
    const audio::Tone tone[] = {{audio::Wave::Square, 440, 440, 0.5f, 0.001f, 0, 0.5f}};
    audio::ClipPtr clip = audio::synth(tone);

    audio::Voice v = m.play(clip, {.volume = 1, .pan = -1});
    CHECK(m.playing(v));
    std::vector<f32> buf = mixed(m, 4800);
    CHECK(peak(buf, 0) > 0.2f);
    CHECK(peak(buf, 1) < 1e-3f); // all the way left
    m.stop(v);
    CHECK_FALSE(m.playing(v));
    CHECK(m.voices() == 0);

    // Ends on its own; a loop does not.
    audio::Voice once = m.play(clip);
    audio::Voice loop = m.play(clip, {.loop = true});
    m.advance(1.0);
    CHECK_FALSE(m.playing(once));
    CHECK(m.playing(loop));
    // A paused bus keeps its sounds and is silent.
    m.pause(audio::Bus::Sound, true);
    CHECK(peak(mixed(m, 2400), 0) == 0.0f);
    m.advance(5.0);
    CHECK(m.playing(loop));
    m.pause(audio::Bus::Sound, false);
    CHECK(peak(mixed(m, 2400), 0) > 0.2f);
    // The settings' volumes.
    m.set_volume(audio::Bus::Sound, 0);
    mixed(m, 2400);
    CHECK(peak(mixed(m, 2400), 0) < 1e-3f);
    m.set_volume(audio::Bus::Sound, 1);
    m.stop_all();
    CHECK(m.voices() == 0);

    // More sounds than the mixer holds: the oldest makes room.
    for (u32 i = 0; i < audio::Mixer::kMaxVoices + 10; ++i) m.play(clip);
    CHECK(m.voices() == audio::Mixer::kMaxVoices);
    m.stop_all();
}

TEST_CASE("audio: sounds in the world are heard from the listener") {
    audio::Mixer m;
    const audio::Tone tone[] = {{audio::Wave::Square, 440, 440, 2.0f, 0.001f, 0, 0.5f}};
    audio::ClipPtr clip = audio::synth(tone);
    m.set_listener(100, 50);
    audio::Voice v = m.play(clip, {.at = true, .x = 101, .y = 50, .range = 16});
    mixed(m, 960); // the gains settle
    const std::vector<f32> close = mixed(m, 4800);
    CHECK(peak(close, 0) > 0.2f);
    // To the right, half way out: quieter, and more on the right.
    m.set_position(v, 108, 50);
    mixed(m, 960);
    const std::vector<f32> right = mixed(m, 4800);
    CHECK(peak(right, 1) < peak(close, 1));
    CHECK(peak(right, 1) > peak(right, 0) * 1.5f);
    // Out of range: silent, though still playing.
    m.set_listener(0, 0);
    mixed(m, 960);
    CHECK(peak(mixed(m, 4800), 0) < 1e-3f);
    CHECK(m.playing(v));

    float volume = 0, pan = 0;
    audio::place(-3, 0, 20, volume, pan);
    CHECK(volume == 1.0f);
    CHECK(pan < 0);
    audio::place(0, 30, 20, volume, pan);
    CHECK(volume == 0.0f);
}

TEST_CASE("audio: a sound picks among its clips and varies a little") {
    audio::Mixer m;
    const audio::Tone a[] = {{audio::Wave::Sine, 300, 300, 0.1f}};
    const audio::Tone b[] = {{audio::Wave::Sine, 600, 600, 0.3f}};
    audio::Sound s;
    s.clips = {audio::synth(a), audio::synth(b)};
    s.pitch_spread = 0.1f;
    for (int i = 0; i < 40; ++i) CHECK(m.play(s));
    CHECK(m.started() == 40);
    m.advance(0.12); // the short clips end, the long ones go on
    const u32 left = m.voices();
    CHECK(left > 5);
    CHECK(left < 35);
    CHECK_FALSE(m.play(audio::Sound{}));
    CHECK(audio::sound_of(nullptr) == nullptr);
}
