#include "forge/pixel/sound.h"

#include "forge/core/path.h"

#include <algorithm>
#include <cmath>
#include <system_error>
#include <utility>

namespace forge::pixel {

namespace fs = std::filesystem;

std::vector<fs::path> sound_files(const fs::path& folder, std::string_view name) {
    if (name.empty() || name.find_first_of("/\\") != std::string_view::npos || name == "." || name == "..") return {};
    const std::string n(name);
    return {folder / utf8_path(n), folder / utf8_path(n + ".wav"), folder / utf8_path(n + ".ogg")};
}

SoundFile find_sound(const fs::path& folder, std::string_view name) {
    SoundFile out;
    const std::string n(name), dir = path_to_utf8(folder.filename());
    for (const fs::path& f : sound_files(folder, name)) {
        std::error_code ec;
        if (!fs::is_regular_file(f, ec)) continue;
        out.file = f;
        std::string error;
        out.clip = audio::load(f, &error);
        if (!out.clip)
            out.why = "звук «" + n + "»: " + dir + "/" + path_to_utf8(f.filename()) + " не читается (" +
                      (error.empty() ? std::string("не WAV и не OGG") : error) + ")";
        return out;
    }
    out.why = "звук «" + n + "» не найден: нет " + dir + "/" + n + ", " + n + ".wav, " + n + ".ogg";
    return out;
}

void add_wave(const audio::Clip& clip, f64 at, f64 end, f64 t0, f64 dt, std::vector<f32>& columns) {
    if (!(dt > 0)) return;
    const f64 rate = audio::kRate;
    // Sample i sounds at at + i / rate: those before `end` only.
    // A few millionths of a sample off are rounding, not another sample.
    const f64 last = std::min<f64>(clip.frames(), std::ceil((end - at) * rate - 1e-6));
    if (!(last > 0)) return;
    for (usize c = 0; c < columns.size(); ++c) {
        const f64 k = static_cast<f64>(c);
        const f64 lo = std::max(0.0, std::ceil((t0 + k * dt - at) * rate - 1e-6));
        const f64 hi = std::min(last, std::ceil((t0 + (k + 1) * dt - at) * rate - 1e-6));
        if (!(lo < hi)) continue;
        f32 most = columns[c];
        for (usize i = static_cast<usize>(lo) * 2, n = static_cast<usize>(hi) * 2; i < n; ++i)
            most = std::max(most, std::abs(clip.samples[i]));
        columns[c] = most;
    }
}

i64 sound_frame(const ClipCut& cut, f64 t) {
    if (t < cut.from - 1e-9 || cut.frames == 0) return -1;
    const f64 k = std::max(0.0, std::ceil((t - cut.from) * cut.fps - 1e-6));
    if (k >= cut.frames) return -1;
    // The first frame at t or after it; the last frame of a clip without «Повторять» stands at `to`, which may come
    // before k / fps does.
    i64 i = static_cast<i64>(k);
    while (i < cut.frames && cut.time(static_cast<u32>(i)) < t - 1e-9) ++i;
    while (i > 0 && cut.time(static_cast<u32>(i - 1)) >= t - 1e-9) --i;
    return i < cut.frames ? i : -1;
}

std::vector<usize> passed_sounds(const Recording& rec, const ClipCut& cut, i64 shown, i64 now, bool wrapped) {
    std::vector<std::pair<i64, usize>> on;
    for (usize i = 0; i < rec.marks.size(); ++i) {
        const Mark& m = rec.marks[i];
        if (m.kind != Mark::Kind::Sound) continue;
        const i64 k = sound_frame(cut, m.time);
        if (k < 0) continue;
        // After the frame shown last and not after this one; gone round, the end of one pass and the start of the next.
        const bool passed = wrapped ? k > shown || k <= now : k > shown && k <= now;
        if (passed) on.push_back({wrapped && k > shown ? k - static_cast<i64>(cut.frames) : k, i});
    }
    std::stable_sort(on.begin(), on.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<usize> out;
    for (const auto& [k, i] : on) out.push_back(i);
    return out;
}

} // namespace forge::pixel
