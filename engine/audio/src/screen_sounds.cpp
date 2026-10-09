#include "forge/audio/screen_sounds.h"

#include "forge/core/log.h"
#include "forge/core/path.h"

namespace forge::audio {

void ScreenSounds::attach(Mixer* mixer, std::filesystem::path folder) {
    if (mixer_ != mixer) detach();
    mixer_ = mixer;
    if (folder != folder_) clips_ = put_;
    folder_ = std::move(folder);
}

void ScreenSounds::detach() {
    stop_music();
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
        voice_ = mixer_->play(c, {.loop = true, .bus = Bus::Music});
        ++music_starts_;
    }
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
