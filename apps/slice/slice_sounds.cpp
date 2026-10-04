#include "slice_sounds.h"

#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/sim/bodies.h"

#include <cmath>

namespace slice {

using namespace forge;
using audio::Tone;
using audio::Wave;
using scene::Position;

namespace {

// The usual sounds, short and soft: they repeat a lot.
audio::SoundPtr make(Cue cue) {
    auto sound = [](std::initializer_list<Tone> tones, f32 pitch_spread = 0.06f, f32 range = 18) {
        auto s = std::make_shared<audio::Sound>();
        s->clips.push_back(audio::synth(std::span<const Tone>(tones.begin(), tones.size())));
        s->pitch_spread = pitch_spread;
        s->volume_spread = 0.15f;
        s->range = range;
        return s;
    };
    switch (cue) {
    case Cue::Step:
        return sound({{Wave::Noise, 1400, 500, 0.07f, 0.002f, 45, 0.16f}, {Wave::Sine, 120, 80, 0.05f, 0.002f, 40, 0.12f}}, 0.12f, 14);
    case Cue::Jump:
        return sound({{Wave::Square, 260, 520, 0.12f, 0.004f, 14, 0.07f}, {Wave::Noise, 2400, 1200, 0.05f, 0.002f, 50, 0.08f}});
    case Cue::Land:
        return sound({{Wave::Noise, 600, 150, 0.12f, 0.002f, 30, 0.3f}, {Wave::Sine, 110, 55, 0.14f, 0.002f, 25, 0.35f}});
    case Cue::Splash:
        return sound({{Wave::Noise, 3200, 700, 0.4f, 0.01f, 7, 0.22f},
                      {Wave::Noise, 900, 300, 0.3f, 0.02f, 9, 0.18f, 0.06f},
                      {Wave::Sine, 500, 900, 0.08f, 0.005f, 30, 0.08f, 0.1f}});
    case Cue::Dig:
        return sound({{Wave::Noise, 2600, 1300, 0.06f, 0.001f, 60, 0.2f}, {Wave::Triangle, 230, 170, 0.06f, 0.001f, 50, 0.2f}}, 0.15f);
    case Cue::Break:
        return sound({{Wave::Noise, 1600, 250, 0.28f, 0.002f, 13, 0.32f}, {Wave::Square, 150, 70, 0.16f, 0.002f, 20, 0.08f}});
    case Cue::Place:
        return sound({{Wave::Triangle, 190, 120, 0.09f, 0.002f, 35, 0.35f}, {Wave::Noise, 900, 600, 0.05f, 0.002f, 60, 0.12f}});
    case Cue::Pickup:
        return sound({{Wave::Sine, 660, 990, 0.08f, 0.003f, 12, 0.22f}, {Wave::Sine, 990, 1320, 0.14f, 0.003f, 14, 0.2f, 0.07f}}, 0.03f);
    case Cue::Coins:
        return sound({{Wave::Square, 988, 988, 0.08f, 0.002f, 18, 0.09f}, {Wave::Square, 1319, 1319, 0.3f, 0.002f, 9, 0.09f, 0.075f}}, 0.04f);
    case Cue::Crate:
        return sound({{Wave::Noise, 900, 200, 0.4f, 0.002f, 9, 0.35f},
                      {Wave::Triangle, 170, 85, 0.22f, 0.002f, 14, 0.3f},
                      {Wave::Noise, 3000, 1000, 0.15f, 0.002f, 25, 0.12f, 0.06f}});
    case Cue::Torch:
        return sound({{Wave::Noise, 2000, 800, 0.12f, 0.003f, 20, 0.15f}, {Wave::Sine, 420, 300, 0.08f, 0.003f, 30, 0.12f}});
    case Cue::Talk:
        return sound({{Wave::Sine, 520, 620, 0.06f, 0.004f, 20, 0.12f}, {Wave::Sine, 620, 560, 0.07f, 0.004f, 20, 0.12f, 0.07f}}, 0.02f);
    case Cue::Count: break;
    }
    return nullptr;
}

} // namespace

void SliceSounds::init(const std::filesystem::path& folder, bool silent) {
    folder_ = folder;
    silent_ = silent;
    for (usize i = 0; i < cues_.size(); ++i) cues_[i] = make(static_cast<Cue>(i));
    if (!silent && !mixer_.open()) FORGE_WARN("Звука нет: не открылось устройство вывода");
}

void SliceSounds::shutdown() {
    stop_objects();
    mixer_.close();
}

void SliceSounds::set_volumes(f32 master, f32 sound, f32 music) {
    mixer_.set_master(master);
    mixer_.set_volume(audio::Bus::Sound, sound);
    mixer_.set_volume(audio::Bus::Music, music);
}

void SliceSounds::pause(bool on) { mixer_.pause(audio::Bus::Sound, on); }

void SliceSounds::tick(f64 dt) {
    if (!mixer_.has_device()) mixer_.advance(dt);
}

void SliceSounds::play(Cue cue, f64 x, f64 y, f32 volume, f32 pitch) {
    const usize i = static_cast<usize>(cue);
    if (i >= cues_.size() || !cues_[i]) return;
    ++played_[i];
    mixer_.play(*cues_[i], {.volume = volume, .pitch = pitch, .at = true, .x = x, .y = y});
}

void SliceSounds::play(const std::string& name, Cue fallback, f64 x, f64 y, f32 volume, f32 range) {
    if (!name.empty()) {
        if (audio::SoundPtr s = named(name)) {
            ++named_played_;
            mixer_.play(*s, {.volume = volume, .at = true, .x = x, .y = y, .range = range});
            return;
        }
    }
    if (fallback != Cue::Count) play(fallback, x, y, volume);
}

audio::SoundPtr SliceSounds::named(const std::string& name) {
    auto it = named_.find(name);
    if (it != named_.end()) return it->second;
    std::string error;
    audio::ClipPtr clip = audio::load(folder_ / utf8_path(name), &error);
    if (!clip) FORGE_WARN("Звук «%s» не играет: %s", name.c_str(), error.c_str());
    // A little spread, so repeats do not sound the same.
    audio::SoundPtr s = audio::sound_of(std::move(clip), 1.0f, 0.04f);
    named_[name] = s;
    return s;
}

void SliceSounds::update_objects(scene::Scene& scene, f64 x, f64 y, f64 dt) {
    for (auto& [id, loop] : loops_) loop.seen = false;
    scene.ecs().each([&](flecs::entity e, const Position& p, const Sounds& s) {
        const f64 dx = p.tile_x() - x, dy = p.tile_y() - y;
        const f64 d2 = dx * dx + dy * dy, range = s.range;
        if (d2 > range * range * 1.2) return;
        // «Рядом»: a loop that follows the object.
        if (!s.near.empty()) {
            Loop& loop = loops_[e.id()];
            loop.seen = true;
            if (loop.name != s.near || !mixer_.playing(loop.voice)) {
                mixer_.stop(loop.voice);
                loop.name = s.near;
                loop.voice = {};
                if (audio::SoundPtr sound = named(s.near); sound && !sound->empty())
                    loop.voice = mixer_.play(sound->clips.front(),
                                             {.volume = s.volume, .loop = true, .at = true, .x = p.tile_x(), .y = p.tile_y(), .range = s.range});
            } else {
                mixer_.set(loop.voice, s.volume, 0);
                mixer_.set_position(loop.voice, p.tile_x(), p.tile_y());
            }
        }
        // «Шаги»: one every tile it walks on the ground.
        if (!s.step.empty()) {
            const sim::Body* b = e.try_get<sim::Body>();
            if (b && (b->contacts & sim::OnGround) && std::fabs(b->vx) > 0.4f) {
                f64& walked = walked_[e.id()];
                walked += std::fabs(b->vx) * dt;
                if (walked >= 1.0) {
                    walked = 0;
                    play(s.step, Cue::Count, p.tile_x(), p.tile_y(), s.volume * 0.7f, s.range);
                }
            }
        }
    });
    for (auto it = loops_.begin(); it != loops_.end();) {
        if (it->second.seen) {
            ++it;
            continue;
        }
        mixer_.stop(it->second.voice);
        walked_.erase(it->first);
        it = loops_.erase(it);
    }
    if (walked_.size() > 4096) walked_.clear();
}

void SliceSounds::stop_objects() {
    for (auto& [id, loop] : loops_) mixer_.stop(loop.voice);
    loops_.clear();
    walked_.clear();
}

} // namespace slice
