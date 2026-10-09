#include "forge/level/light.h"

#include "forge/core/file.h"
#include "forge/core/path.h"
#include "forge/data/json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

FORGE_REFLECT(forge::level::LevelLight, 1) { t.field("time", &forge::level::LevelLight::time); }

FORGE_REFLECT(forge::level::LightSource, 1) {
    t.field("r", &forge::level::LightSource::r);
    t.field("g", &forge::level::LightSource::g);
    t.field("b", &forge::level::LightSource::b);
    t.field("brightness", &forge::level::LightSource::brightness);
    t.field("radius", &forge::level::LightSource::radius);
}

namespace forge::level {

namespace fs = std::filesystem;

namespace {

constexpr const char* kLightFile = "light.json";

} // namespace

bool valid_light(const LevelLight& l) { return std::isfinite(l.time) && l.time >= 0 && l.time < 24; }

bool load_light(const fs::path& folder, LevelLight& out, bool* found, std::string* error) {
    if (found) *found = false;
    const fs::path file = folder / kLightFile;
    std::error_code ec;
    if (!fs::exists(file, ec)) return true;
    if (found) *found = true;
    auto fail = [&](const std::string& why) {
        if (error) *error = "light.json: " + why;
        return false;
    };
    std::vector<u8> bytes;
    if (!fs::is_regular_file(file, ec) || !read_file(file, bytes)) return fail("файл не читается");
    LevelLight l = out;
    data::LoadReport report;
    if (!data::from_json(l, {reinterpret_cast<const char*>(bytes.data()), bytes.size()}, report))
        return fail("файл испорчен, в нём нет времени суток (" + report.error + ")");
    if (!report.warnings.empty()) return fail("значение не того вида (" + report.warnings.front() + ")");
    if (!valid_light(l)) {
        char why[120];
        std::snprintf(why, sizeof(why), "время суток %g вне пределов: от 0 до 24 часов (24 не входит)", static_cast<f64>(l.time));
        return fail(why);
    }
    out = l;
    return true;
}

bool save_light(const fs::path& folder, const LevelLight& l, std::string* error) {
    if (!valid_light(l)) {
        if (error) *error = "light.json не записан: время суток вне пределов";
        return false;
    }
    const std::string text = data::to_json(l) + "\n";
    std::error_code ec;
    if (!folder.empty()) fs::create_directories(folder, ec);
    if (folder.empty() || !write_file_atomic(folder / kLightFile, {reinterpret_cast<const u8*>(text.data()), text.size()})) {
        if (error) *error = "не удалось записать " + path_to_utf8(folder / kLightFile);
        return false;
    }
    return true;
}

render::PointLight point_light(f64 x, f64 y, const LightSource& s) {
    render::PointLight l;
    l.x = x;
    l.y = y;
    const bool finite = std::isfinite(s.r) && std::isfinite(s.g) && std::isfinite(s.b) && std::isfinite(s.brightness) &&
                        std::isfinite(s.radius);
    if (!finite) {
        l.r = l.g = l.b = 0;
        l.radius = kMinLightRadius;
        return l;
    }
    const f32 k = std::clamp(s.brightness, 0.0f, kMaxBrightness);
    l.r = std::clamp(s.r, 0.0f, 1.0f) * k;
    l.g = std::clamp(s.g, 0.0f, 1.0f) * k;
    l.b = std::clamp(s.b, 0.0f, 1.0f) * k;
    l.radius = std::clamp(s.radius, kMinLightRadius, kMaxLightRadius);
    return l;
}

void add_light_sources(scene::Scene& scene, render::LightRenderer& lights) {
    scene.ecs().each([&](const scene::Position& p, const LightSource& s) { lights.add(point_light(p.tile_x(), p.tile_y(), s)); });
}

SetLight::SetLight(Level& level, LevelLight before, LevelLight after, std::string label)
    : level_(level), before_(before), after_(after), label_(std::move(label)) {}

bool SetLight::try_merge(const editor::Command& next) {
    const auto* n = static_cast<const SetLight*>(&next);
    if (!n || n->label_ != label_) return false;
    after_ = n->after_;
    return true;
}

std::string clock_text(f64 hours) {
    if (!std::isfinite(hours)) return "?";
    i64 minutes = static_cast<i64>(std::llround(hours * 60));
    minutes = ((minutes % (24 * 60)) + 24 * 60) % (24 * 60);
    char text[16];
    std::snprintf(text, sizeof(text), "%lld:%02lld", static_cast<long long>(minutes / 60), static_cast<long long>(minutes % 60));
    return text;
}

bool parse_clock(const std::string& text, f64& hours) {
    std::string t = text;
    std::replace(t.begin(), t.end(), ',', '.');
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
    if (t.empty()) return false;
    const usize colon = t.find(':');
    if (colon != std::string::npos) {
        // Hours and minutes: whole numbers, minutes 0..59 in two digits.
        const std::string h = t.substr(0, colon), m = t.substr(colon + 1);
        auto digits = [](const std::string& s) { return !s.empty() && s.size() <= 2 && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }); };
        if (!digits(h) || m.size() != 2 || !digits(m)) return false;
        const int mm = std::atoi(m.c_str());
        if (mm > 59) return false;
        hours = std::atoi(h.c_str()) + mm / 60.0;
        return true;
    }
    char* end = nullptr;
    const f64 v = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || !end || *end != 0 || !std::isfinite(v)) return false;
    hours = v;
    return true;
}

} // namespace forge::level
