#pragma once

// A scene's tunes (forge/game/scenes.h: a cut-scene's, or a scene's behind a screen) as the screens' sounds play them
// (forge/audio/screen_sounds.h). Header only: what includes it links forge::game and forge::audio.

#include "forge/audio/screen_sounds.h"
#include "forge/game/scenes.h"

#include <vector>

namespace forge {

inline std::vector<audio::ScreenSounds::Tune> screen_tunes_of(const std::vector<game::SceneTune>& tunes) {
    std::vector<audio::ScreenSounds::Tune> out;
    out.reserve(tunes.size());
    for (const game::SceneTune& t : tunes) out.push_back({t.call, t.file, t.since, static_cast<f32>(t.volume)});
    return out;
}

} // namespace forge
