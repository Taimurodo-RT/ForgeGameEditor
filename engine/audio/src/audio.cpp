#include "forge/audio/audio.h"

#include "forge/core/file.h"
#include "forge/core/path.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

// stb_vorbis, compiled in vorbis.c
extern "C" int stb_vorbis_decode_memory(const unsigned char* mem, int len, int* channels, int* sample_rate, short** output);

namespace forge::audio {

namespace {

constexpr f32 kPi = 3.14159265358979f;

// Any SDL format, rate and layout -> stereo float at kRate.
ClipPtr convert(const SDL_AudioSpec& from, const u8* data, usize bytes, std::string* error) {
    const SDL_AudioSpec to{SDL_AUDIO_F32, 2, static_cast<int>(kRate)};
    u8* out = nullptr;
    int out_len = 0;
    if (!SDL_ConvertAudioSamples(&from, data, static_cast<int>(bytes), &to, &out, &out_len)) {
        if (error) *error = std::string("звук не переводится: ") + SDL_GetError();
        return nullptr;
    }
    auto clip = std::make_shared<Clip>();
    clip->samples.resize(static_cast<usize>(out_len) / sizeof(f32));
    std::memcpy(clip->samples.data(), out, clip->samples.size() * sizeof(f32));
    SDL_free(out);
    return clip;
}

bool starts_with(std::span<const u8> b, const char* magic, usize at = 0) {
    const usize n = std::strlen(magic);
    return b.size() >= at + n && std::memcmp(b.data() + at, magic, n) == 0;
}

void SDLCALL feed(void* user, SDL_AudioStream* stream, int additional, int) {
    if (additional <= 0) return;
    thread_local std::vector<f32> buffer;
    const u32 frames = static_cast<u32>(additional) / (2 * sizeof(f32));
    buffer.assign(static_cast<usize>(frames) * 2, 0.0f);
    static_cast<Mixer*>(user)->mix(buffer.data(), frames);
    SDL_PutAudioStreamData(stream, buffer.data(), static_cast<int>(buffer.size() * sizeof(f32)));
}

} // namespace

ClipPtr decode(std::span<const u8> bytes, std::string* error) {
    if (starts_with(bytes, "RIFF") && starts_with(bytes, "WAVE", 8)) {
        SDL_IOStream* io = SDL_IOFromConstMem(bytes.data(), bytes.size());
        SDL_AudioSpec spec{};
        u8* data = nullptr;
        u32 len = 0;
        if (!io || !SDL_LoadWAV_IO(io, true, &spec, &data, &len)) {
            if (error) *error = std::string("WAV не читается: ") + SDL_GetError();
            return nullptr;
        }
        ClipPtr clip = convert(spec, data, len, error);
        SDL_free(data);
        return clip;
    }
    if (starts_with(bytes, "OggS")) {
        int channels = 0, rate = 0;
        short* data = nullptr;
        const int frames = stb_vorbis_decode_memory(bytes.data(), static_cast<int>(bytes.size()), &channels, &rate, &data);
        if (frames < 0 || !data || channels <= 0) {
            if (error) *error = "OGG не читается (это не Vorbis или файл испорчен)";
            if (data) std::free(data);
            return nullptr;
        }
        const SDL_AudioSpec spec{SDL_AUDIO_S16, channels, rate};
        ClipPtr clip = convert(spec, reinterpret_cast<const u8*>(data),
                               static_cast<usize>(frames) * static_cast<usize>(channels) * sizeof(short), error);
        std::free(data);
        return clip;
    }
    if (error) *error = "это не WAV и не OGG: переведите его в «Ресурсах» через «Конвертировать…»";
    return nullptr;
}

ClipPtr load(const std::filesystem::path& file, std::string* error) {
    std::vector<u8> bytes;
    if (!read_file(file, bytes)) {
        if (error) *error = "файл не читается: " + path_to_utf8(file);
        return nullptr;
    }
    return decode(bytes, error);
}

bool encode_wav(const Clip& clip, std::vector<u8>& out) {
    out.clear();
    auto put32 = [&](u32 v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
    };
    auto put16 = [&](u16 v) {
        out.push_back(static_cast<u8>(v));
        out.push_back(static_cast<u8>(v >> 8));
    };
    const u32 bytes = static_cast<u32>(clip.samples.size() * 2);
    out.reserve(44 + bytes);
    for (char c : std::string("RIFF")) out.push_back(static_cast<u8>(c));
    put32(36 + bytes);
    for (char c : std::string("WAVEfmt ")) out.push_back(static_cast<u8>(c));
    put32(16);
    put16(1);
    put16(2);
    put32(kRate);
    put32(kRate * 4);
    put16(4);
    put16(16);
    for (char c : std::string("data")) out.push_back(static_cast<u8>(c));
    put32(bytes);
    for (f32 v : clip.samples) put16(static_cast<u16>(static_cast<i16>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f))));
    return true;
}

bool readable(const std::filesystem::path& file) {
    std::string ext = path_to_utf8(file.extension());
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".wav" || ext == ".ogg";
}

void place(f64 dx, f64 dy, f32 range, f32& volume, f32& pan) {
    const f64 d = std::sqrt(dx * dx + dy * dy);
    const f64 near = std::max(1.0, range * 0.15);
    if (d <= near) volume = 1;
    else if (d >= range) volume = 0;
    else {
        const f64 t = (d - near) / (range - near);
        volume = static_cast<f32>((1 - t) * (1 - t)); // falls off softly, then quickly
    }
    pan = static_cast<f32>(std::clamp(dx / std::max<f64>(range * 0.5, 1.0), -1.0, 1.0)) * 0.8f;
}

// --- the mixer ----------------------------------------------------------------

Mixer::~Mixer() { close(); }

bool Mixer::open() {
    if (stream_) return true;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return false;
    const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, static_cast<int>(kRate)};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, this);
    if (!stream_) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    SDL_ResumeAudioStreamDevice(stream_);
    return true;
}

void Mixer::close() {
    if (!stream_) return;
    SDL_DestroyAudioStream(stream_); // stops the callback first
    stream_ = nullptr;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

Mixer::Slot* Mixer::find(Voice v) {
    for (Slot& s : slots_)
        if (s.id == v.id) return &s;
    return nullptr;
}
const Mixer::Slot* Mixer::find(Voice v) const {
    for (const Slot& s : slots_)
        if (s.id == v.id) return &s;
    return nullptr;
}

Voice Mixer::play(const ClipPtr& clip, const Play& p) {
    if (!clip || clip->frames() == 0) return {};
    std::lock_guard lock(mutex_);
    if (slots_.size() >= kMaxVoices) {
        // The oldest sound that is not a loop makes room (a loop is usually
        // something the game keeps on purpose).
        auto oldest = slots_.end();
        for (auto it = slots_.begin(); it != slots_.end(); ++it)
            if (!it->loop && (oldest == slots_.end() || it->id < oldest->id)) oldest = it;
        if (oldest == slots_.end()) oldest = slots_.begin();
        slots_.erase(oldest);
    }
    Slot s;
    s.clip = clip;
    s.id = next_id_++;
    if (next_id_ == 0) next_id_ = 1;
    s.volume = std::max(0.0f, p.volume);
    s.pan = std::clamp(p.pan, -1.0f, 1.0f);
    s.pitch = std::clamp(p.pitch, 0.05f, 8.0f);
    s.loop = p.loop;
    s.bus = p.bus;
    s.at = p.at;
    s.x = p.x;
    s.y = p.y;
    s.range = std::max(1.0f, p.range);
    slots_.push_back(std::move(s));
    ++started_;
    return {slots_.back().id};
}

Voice Mixer::play(const Sound& sound, Play p) {
    if (sound.clips.empty()) return {};
    usize pick = 0;
    f32 r1 = 0, r2 = 0;
    {
        std::lock_guard lock(mutex_);
        auto next = [this] {
            random_ ^= random_ << 13;
            random_ ^= random_ >> 17;
            random_ ^= random_ << 5;
            return static_cast<f32>(random_ & 0xFFFFFF) / static_cast<f32>(0x1000000);
        };
        pick = static_cast<usize>(next() * static_cast<f32>(sound.clips.size())) % sound.clips.size();
        r1 = next();
        r2 = next();
    }
    p.volume *= sound.volume * (1 - sound.volume_spread * r1);
    p.pitch *= 1 + sound.pitch_spread * (r2 * 2 - 1);
    if (p.range == Play{}.range) p.range = sound.range;
    return play(sound.clips[pick], p);
}

SoundPtr sound_of(ClipPtr clip, f32 volume, f32 pitch_spread) {
    if (!clip) return nullptr;
    auto s = std::make_shared<Sound>();
    s->clips.push_back(std::move(clip));
    s->volume = volume;
    s->pitch_spread = pitch_spread;
    return s;
}

bool Mixer::playing(Voice v) const {
    std::lock_guard lock(mutex_);
    const Slot* s = find(v);
    return s && !s->stopping;
}

void Mixer::stop(Voice v) {
    std::lock_guard lock(mutex_);
    if (Slot* s = find(v)) {
        s->stopping = true;
        if (!stream_) slots_.erase(slots_.begin() + (s - slots_.data()));
    }
}

void Mixer::stop_all() {
    std::lock_guard lock(mutex_);
    if (!stream_) slots_.clear();
    for (Slot& s : slots_) s.stopping = true;
}

void Mixer::set(Voice v, f32 volume, f32 pan) {
    std::lock_guard lock(mutex_);
    if (Slot* s = find(v)) {
        s->volume = std::max(0.0f, volume);
        s->pan = std::clamp(pan, -1.0f, 1.0f);
    }
}

void Mixer::set_pitch(Voice v, f32 pitch) {
    std::lock_guard lock(mutex_);
    if (Slot* s = find(v)) s->pitch = std::clamp(pitch, 0.05f, 8.0f);
}

void Mixer::set_position(Voice v, f64 x, f64 y) {
    std::lock_guard lock(mutex_);
    if (Slot* s = find(v)) {
        s->x = x;
        s->y = y;
    }
}

void Mixer::set_listener(f64 x, f64 y) {
    std::lock_guard lock(mutex_);
    listener_x_ = x;
    listener_y_ = y;
}

void Mixer::set_master(f32 volume) {
    std::lock_guard lock(mutex_);
    master_ = std::clamp(volume, 0.0f, 2.0f);
}

void Mixer::set_volume(Bus bus, f32 volume) {
    std::lock_guard lock(mutex_);
    bus_volume_[static_cast<int>(bus)] = std::clamp(volume, 0.0f, 2.0f);
}

void Mixer::pause(Bus bus, bool on) {
    std::lock_guard lock(mutex_);
    bus_paused_[static_cast<int>(bus)] = on;
}

u32 Mixer::voices() const {
    std::lock_guard lock(mutex_);
    u32 n = 0;
    for (const Slot& s : slots_) n += s.stopping ? 0 : 1;
    return n;
}

u32 Mixer::voices(Bus bus) const {
    std::lock_guard lock(mutex_);
    u32 n = 0;
    for (const Slot& s : slots_) n += !s.stopping && s.bus == bus ? 1 : 0;
    return n;
}

void Mixer::mix(f32* out, u32 frames) {
    std::lock_guard lock(mutex_);
    mix_locked(out, frames);
}

void Mixer::advance(f64 seconds) {
    if (stream_ || seconds <= 0) return;
    std::lock_guard lock(mutex_);
    for (Slot& s : slots_)
        if (!bus_paused_[static_cast<int>(s.bus)]) s.pos += seconds * kRate * s.pitch;
    std::erase_if(slots_, [](const Slot& s) { return s.stopping || (!s.loop && s.pos >= s.clip->frames()); });
    for (Slot& s : slots_)
        if (s.loop) s.pos = std::fmod(s.pos, static_cast<f64>(s.clip->frames()));
}

void Mixer::mix_locked(f32* out, u32 frames) {
    std::fill(out, out + static_cast<usize>(frames) * 2, 0.0f);
    if (frames == 0) return;
    for (Slot& s : slots_) {
        const int b = static_cast<int>(s.bus);
        if (bus_paused_[b] && !s.stopping) continue;
        const f32* data = s.clip->samples.data();
        const u32 length = s.clip->frames();
        // Constant power pan, 1 in the middle; gains ramp across the block
        // so changes (and stops) do not click.
        f32 v = s.stopping ? 0.0f : s.volume * bus_volume_[b] * master_;
        f32 pan = s.pan;
        if (s.at) {
            f32 heard = 0, side = 0;
            place(s.x - listener_x_, s.y - listener_y_, s.range, heard, side);
            v *= heard;
            pan = std::clamp(pan + side, -1.0f, 1.0f);
        }
        const f32 angle = (pan + 1) * kPi * 0.25f;
        const f32 to_l = v * std::cos(angle) * 1.41421356f, to_r = v * std::sin(angle) * 1.41421356f;
        if (!s.started) {
            s.gain_l = to_l;
            s.gain_r = to_r;
            s.started = true;
        }
        const f32 step_l = (to_l - s.gain_l) / static_cast<f32>(frames), step_r = (to_r - s.gain_r) / static_cast<f32>(frames);
        f32 gl = s.gain_l, gr = s.gain_r;
        f64 pos = s.pos;
        for (u32 i = 0; i < frames; ++i) {
            if (pos >= length) {
                if (!s.loop) break;
                pos = std::fmod(pos, static_cast<f64>(length));
            }
            const u32 a = static_cast<u32>(pos);
            const u32 next = a + 1 < length ? a + 1 : (s.loop ? 0 : a);
            const f32 t = static_cast<f32>(pos - a);
            const f32 l = data[a * 2] + (data[next * 2] - data[a * 2]) * t;
            const f32 r = data[a * 2 + 1] + (data[next * 2 + 1] - data[a * 2 + 1]) * t;
            gl += step_l;
            gr += step_r;
            out[i * 2] += l * gl;
            out[i * 2 + 1] += r * gr;
            pos += s.pitch;
        }
        s.gain_l = to_l;
        s.gain_r = to_r;
        s.pos = pos;
    }
    std::erase_if(slots_, [](const Slot& s) { return s.stopping || (!s.loop && s.pos >= s.clip->frames()); });
    // Many loud sounds at once: bend the peaks instead of clipping them.
    for (usize i = 0; i < static_cast<usize>(frames) * 2; ++i) {
        const f32 x = out[i];
        out[i] = std::fabs(x) <= 0.8f ? x : std::copysign(0.8f + 0.2f * std::tanh((std::fabs(x) - 0.8f) / 0.2f), x);
    }
}

// --- synthesis -------------------------------------------------------------------

ClipPtr synth(std::span<const Tone> tones) {
    f32 length = 0;
    for (const Tone& t : tones) length = std::max(length, t.start + t.seconds);
    const u32 frames = static_cast<u32>(std::ceil(length * kRate));
    auto clip = std::make_shared<Clip>();
    clip->samples.assign(static_cast<usize>(frames) * 2, 0.0f);
    u32 noise = 0x2545F491u;
    for (const Tone& t : tones) {
        const u32 first = static_cast<u32>(t.start * kRate);
        const u32 count = static_cast<u32>(t.seconds * kRate);
        f64 phase = 0;
        f32 held = 0;
        for (u32 i = 0; i < count && first + i < frames; ++i) {
            const f32 x = static_cast<f32>(i) / static_cast<f32>(count);
            const f32 hz = t.from_hz * std::pow(t.to_hz / t.from_hz, x);
            const f64 before = phase;
            phase += hz / kRate;
            const f32 ph = static_cast<f32>(phase - std::floor(phase));
            f32 wave = 0;
            switch (t.wave) {
            case Wave::Sine: wave = std::sin(ph * 2 * kPi); break;
            case Wave::Square: wave = ph < 0.5f ? 0.6f : -0.6f; break;
            case Wave::Triangle: wave = 4 * std::fabs(ph - 0.5f) - 1; break;
            case Wave::Noise:
                // Noise held for one period of the pitch: lower pitch, rougher sound.
                if (std::floor(phase) != std::floor(before) || i == 0) {
                    noise ^= noise << 13;
                    noise ^= noise >> 17;
                    noise ^= noise << 5;
                    held = static_cast<f32>(noise) / 2147483648.0f - 1.0f;
                }
                wave = held;
                break;
            }
            const f32 secs = static_cast<f32>(i) / kRate;
            f32 env = t.attack > 0 && secs < t.attack ? secs / t.attack : 1.0f;
            if (t.decay > 0) env *= std::exp(-t.decay * std::max(0.0f, secs - t.attack));
            const f32 tail = (t.seconds - secs) / 0.005f; // the last 5 ms fade, no click
            if (tail < 1) env *= std::max(0.0f, tail);
            const f32 v = wave * env * t.volume;
            clip->samples[(first + i) * 2] += v;
            clip->samples[(first + i) * 2 + 1] += v;
        }
    }
    return clip;
}

} // namespace forge::audio
