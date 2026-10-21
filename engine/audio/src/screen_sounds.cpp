#include "forge/audio/screen_sounds.h"

#include "forge/core/log.h"
#include "forge/core/path.h"

#include <algorithm>

namespace forge::audio {

void ScreenSounds::attach(Mixer* mixer, std::filesystem::path folder) {
    if (mixer_ != mixer) detach();
    mixer_ = mixer;
    if (folder != folder_) clips_ = put_;
    folder_ = std::move(folder);
}

void ScreenSounds::detach() {
    stop_music();
    tunes({});
    screen_tunes({});
    mixer_ = nullptr;
}

void ScreenSounds::put(const std::string& name, ClipPtr clip) {
    clips_[name] = clip;
    put_[name] = std::move(clip);
}

void ScreenSounds::forget() {
    clips_ = put_;
    problems_.clear();
}

ClipPtr ScreenSounds::loaded(const std::string& name) const {
    const auto it = clips_.find(name);
    return it != clips_.end() ? it->second : nullptr;
}

ClipPtr ScreenSounds::clip(const std::string& name) {
    if (name.empty()) return nullptr;
    auto it = clips_.find(name);
    if (it != clips_.end()) return it->second;
    std::string error;
    ClipPtr c;
    // A name in the sounds folder, not a way out of it.
    if (name.find_first_of("/\\:") != std::string::npos || name == "." || name == "..")
        error = "это не имя файла из папки звуков игры";
    else if (!readable(utf8_path(name)))
        error = "игра читает только WAV и OGG";
    else if (folder_.empty())
        error = "у игры нет папки звуков";
    else if (std::error_code ec; !std::filesystem::exists(folder_ / utf8_path(name), ec))
        error = "нет такого файла в папке звуков игры (" + path_to_utf8(folder_) + ")";
    else
        c = load(folder_ / utf8_path(name), &error);
    if (!c) {
        FORGE_WARN("Звук «%s» не играет: %s", name.c_str(), error.c_str()); // a screen's or a place's
        problems_.push_back(name);
    }
    clips_[name] = c; // a missing one too: said once, not read again
    return c;
}

bool ScreenSounds::music_playing() const { return mixer_ && voice_ && mixer_->playing(voice_); }

void ScreenSounds::music(const std::string& name) {
    if (name == music_ && (name.empty() || !voice_ || music_playing())) return;
    if (mixer_ && voice_) mixer_->stop(voice_);
    voice_ = {};
    music_ = name;
    if (name.empty() || !mixer_) return;
    if (ClipPtr c = clip(name)) {
        voice_ = mixer_->play(c, {.volume = share_, .loop = true, .bus = Bus::Music});
        ++music_starts_;
    }
}

void ScreenSounds::play_tunes(Tunes& on, const std::vector<Tune>& want, f32 scale) {
    // A tune's time a little behind where it was is the same play; going back further, its scene began again.
    constexpr f64 kBack = 0.05;
    for (usize i = on.list.size(); i-- > 0;) {
        const Tune& o = on.list[i];
        const f64 latest = on.latest[i];
        if (std::none_of(want.begin(), want.end(), [&](const Tune& t) { return t.call == o.call && t.name == o.name && t.since + kBack >= latest; })) {
            if (mixer_) mixer_->stop(on.voices[i]);
            on.list.erase(on.list.begin() + static_cast<std::ptrdiff_t>(i));
            on.voices.erase(on.voices.begin() + static_cast<std::ptrdiff_t>(i));
            on.latest.erase(on.latest.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    for (const Tune& t : want) {
        const auto o = std::find_if(on.list.begin(), on.list.end(), [&](const Tune& x) { return x.call == t.call; });
        if (o != on.list.end()) {
            f64& latest = on.latest[static_cast<usize>(o - on.list.begin())];
            latest = std::max(latest, t.since);
            if (o->volume != t.volume) {
                o->volume = t.volume;
                if (mixer_) mixer_->set(on.voices[static_cast<usize>(o - on.list.begin())], t.volume * scale, 0);
            }
            continue;
        }
        Voice v;
        if (ClipPtr c = mixer_ ? clip(t.name) : nullptr)
            v = mixer_->play(c, {.volume = t.volume * scale, .loop = true, .bus = Bus::Music, .from = t.since});
        on.list.push_back(t);
        on.voices.push_back(v);
        on.latest.push_back(t.since);
    }
}

void ScreenSounds::tunes(const std::vector<Tune>& want) { play_tunes(tunes_, want, 1); }

void ScreenSounds::screen_tunes(const std::vector<Tune>& want) { play_tunes(screen_tunes_, want, share_); }

Voice ScreenSounds::tune_voice(u32 call) const {
    for (usize i = 0; i < tunes_.list.size(); ++i)
        if (tunes_.list[i].call == call) return tunes_.voices[i];
    return {};
}

Voice ScreenSounds::screen_tune_voice(u32 call) const {
    for (usize i = 0; i < screen_tunes_.list.size(); ++i)
        if (screen_tunes_.list[i].call == call) return screen_tunes_.voices[i];
    return {};
}

void ScreenSounds::set_share(f32 share) {
    if (share == share_) return;
    share_ = share;
    if (mixer_ && voice_) mixer_->set(voice_, share_, 0);
    if (mixer_)
        for (usize i = 0; i < screen_tunes_.list.size(); ++i) mixer_->set(screen_tunes_.voices[i], screen_tunes_.list[i].volume * share_, 0);
}

void ScreenSounds::click(const std::string& name) {
    if (!mixer_) return;
    if (ClipPtr c = clip(name)) {
        mixer_->play(c, {.bus = Bus::Ui});
        ++clicks_;
        last_click_ = name;
    }
}

} // namespace forge::audio
