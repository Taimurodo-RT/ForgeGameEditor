#pragma once

// The sounds of a character's code in «Векторная» (part 3в of docs/models/движение-модель.md, «Звук»): the file of
// sound("шаг") among the game's sounds, its wave on the timeline, the frame of the preview it sounds in.
//
//   find_sound(game / "sounds", "шаг")   // sounds/шаг, else шаг.wav, else шаг.ogg: the first there is
//   add_wave(*clip, 0.25, 0.75, 0, 1.0 / 900, columns)  // its loudest sample in each 1/900 s from 0,25 s to the end

#include "forge/audio/audio.h"
#include "forge/pixel/clip_cut.h"
#include "forge/pixel/motion.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace forge::pixel {

// The files sound("name") may be, in the order they are looked for: name, name.wav, name.ogg in `folder`. None for a
// name with a / or \ in it: a sound of the game is a file of its folder.
std::vector<std::filesystem::path> sound_files(const std::filesystem::path& folder, std::string_view name);

struct SoundFile {
    std::filesystem::path file; // the one found; empty when there is none
    audio::ClipPtr clip;        // null when there is none or it does not read
    // Why there is no clip, as the console writes it after "строка 7: ": «звук «шаг» не найден: нет sounds/шаг,
    // шаг.wav, шаг.ogg» or «звук «шаг»: sounds/шаг.wav не читается (…)».
    std::string why;
};
SoundFile find_sound(const std::filesystem::path& folder, std::string_view name);

// The wave of a sound from moment `at` of the clip on, cut at `end` (the clip's end), in columns: column c covers the
// moments [t0 + c·dt, t0 + (c + 1)·dt) and takes the loudest sample of the file there (|s| of both channels) when it
// is louder than what the column holds, so that marks of one name over each other keep the louder.
void add_wave(const audio::Clip& clip, f64 at, f64 end, f64 t0, f64 dt, std::vector<f32>& columns);

// The frame of the preview a mark at moment t sounds in: the first the range shows at t or after it, k = ⌈(t − from)·fps⌉;
// −1 before the range or past its last frame.
i64 sound_frame(const ClipCut& cut, f64 t);
// The sound marks of a recording that the preview passes going from frame `shown` (−1: none yet) to frame `now`, in
// the order they sound: those whose frame is after `shown` and not after `now`; gone round the range (`wrapped`), those
// after `shown` to its end and then those from its start to `now`.
std::vector<usize> passed_sounds(const Recording& rec, const ClipCut& cut, i64 shown, i64 now, bool wrapped);

} // namespace forge::pixel
