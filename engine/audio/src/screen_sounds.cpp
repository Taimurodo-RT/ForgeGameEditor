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

void ScreenSounds::tunes(const std::vector<Tune>& want) {
    for (usize i = tunes_.size(); i-- > 0;)
        if (std::none_of(want.begin(), want.end(), [&](const Tune& t) { return t.call == tunes_[i].call && t.name == tunes_[i].name; })) {
            if (mixer_) mixer_->stop(tune_voices_[i]);
            tunes_.erase(tunes_.begin() + static_cast<std::ptrdiff_t>(i));
            tune_voices_.erase(tune_voices_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    for (const Tune& t : want) {
        const auto on = std::find_if(tunes_.begin(), tunes_.end(), [&](const Tune& o) { return o.call == t.call; });
        if (on != tunes_.end()) {
            if (on->volume != t.volume) {
                on->volume = t.volume;
                if (mixer_) mixer_->set(tune_voices_[static_cast<usize>(on - tunes_.begin())], t.volume, 0);
            }
            continue;
        }
        Voice v;
        if (ClipPtr c = mixer_ ? clip(t.name) : nullptr) v = mixer_->play(c, {.volume = t.volume, .loop = true, .bus = Bus::Music, .from = t.since});
        tunes_.push_back(t);
        tune_voices_.push_back(v);
    }
}

Voice ScreenSounds::tune_voice(u32 call) const {
    for (usize i = 0; i < tunes_.size(); ++i)
        if (tunes_[i].call == call) return tune_voices_[i];
    return {};
}

void ScreenSounds::set_share(f32 share) {
    if (share == share_) return;
    share_ = share;
    if (mixer_ && voice_) mixer_->set(voice_, share_, 0);
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
