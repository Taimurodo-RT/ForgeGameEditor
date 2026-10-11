#include "forge/game/saves.h"

#include "forge/game/game_module.h"

#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/data/json.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>
#include <yyjson.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <system_error>

FORGE_REFLECT(forge::game::Settings, 1) {
    t.field("fullscreen", &forge::game::Settings::fullscreen);
    t.field("vsync", &forge::game::Settings::vsync);
    t.field("ui_scale", &forge::game::Settings::ui_scale);
    t.field("master_volume", &forge::game::Settings::master_volume);
    t.field("music_volume", &forge::game::Settings::music_volume);
    t.field("sound_volume", &forge::game::Settings::sound_volume);
    t.field("theme", &forge::game::Settings::theme);
    t.field("show_fps", &forge::game::Settings::show_fps);
}

FORGE_REFLECT(forge::game::SlotInfo, 1) {
    t.field("id", &forge::game::SlotInfo::id);
    t.field("title", &forge::game::SlotInfo::title);
    t.field("location", &forge::game::SlotInfo::location);
    t.field("playtime_s", &forge::game::SlotInfo::playtime_s);
    t.field("saved_at", &forge::game::SlotInfo::saved_at);
    t.field("autosave", &forge::game::SlotInfo::autosave);
    t.field("version", &forge::game::SlotInfo::version);
}

namespace forge::game {

namespace fs = std::filesystem;

// --- folders -----------------------------------------------------------------

fs::path exe_dir() {
    const char* base = SDL_GetBasePath(); // owned by SDL, ends with a separator
    return base ? utf8_path(base) : fs::current_path();
}

fs::path find_data_dir(const fs::path& dev_dir) {
    std::error_code ec;
    const fs::path packaged = exe_dir() / "data";
    if (fs::is_directory(packaged, ec)) return packaged;
    return dev_dir;
}

fs::path user_dir(std::string_view org, std::string_view game) {
    char* pref = SDL_GetPrefPath(std::string(org).c_str(), std::string(game).c_str());
    fs::path p = pref ? utf8_path(pref) : fs::current_path() / "userdata";
    SDL_free(pref);
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

// --- slots -------------------------------------------------------------------

namespace {

bool read_text(const fs::path& path, std::string& out) {
    std::vector<u8> bytes;
    if (!read_file(path, bytes)) return false;
    out.assign(bytes.begin(), bytes.end());
    return true;
}

bool write_text(const fs::path& path, const std::string& text) {
    return write_file_atomic(path, {reinterpret_cast<const u8*>(text.data()), text.size()});
}

bool read_info(const fs::path& folder, SlotInfo& info) {
    std::string text;
    if (!read_text(folder / "slot.json", text)) return false;
    data::LoadReport report;
    if (!data::from_json(info, text, report)) return false;
    // The module, by game.json's rule: none (a slot from before) is "slice", one that is empty or no string is none.
    std::string why;
    if (game_module_of(text, info.module, &why) == GameModule::Broken) {
        info.module.clear();
        // Its words name game.json: here the file is slot.json.
        if (const usize at = why.find("game.json"); at != std::string::npos) why.replace(at, 9, "slot.json");
        info.module_error = why;
    }
    return true;
}

// slot.json: the reflected fields, then "module" when the game named one.
std::string slot_json(const SlotInfo& info) {
    std::string text = data::to_json(info);
    if (info.module.empty()) return text;
    yyjson_doc* doc = yyjson_read(text.data(), text.size(), 0);
    yyjson_mut_doc* mut = doc ? yyjson_doc_mut_copy(doc, nullptr) : nullptr;
    yyjson_doc_free(doc);
    yyjson_mut_val* root = mut ? yyjson_mut_doc_get_root(mut) : nullptr;
    if (!yyjson_mut_is_obj(root)) {
        yyjson_mut_doc_free(mut);
        return text;
    }
    yyjson_mut_obj_add_strncpy(mut, root, "module", info.module.data(), info.module.size());
    usize len = 0;
    char* out = yyjson_mut_write(mut, YYJSON_WRITE_PRETTY_TWO_SPACES, &len);
    yyjson_mut_doc_free(mut);
    if (out) text.assign(out, len);
    std::free(out);
    return text;
}

// Copies a folder's files (recursively) into an empty or missing folder.
bool copy_tree(const fs::path& from, const fs::path& to, std::string* error) {
    std::error_code ec;
    fs::create_directories(to, ec);
    if (!fs::exists(from, ec)) return true;
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        if (error) *error = "не удалось скопировать «" + path_to_utf8(from) + "»: " + ec.message();
        return false;
    }
    return true;
}

} // namespace

SaveSlots::SaveSlots(fs::path user_dir) : root_(std::move(user_dir)) {
    std::error_code ec;
    fs::create_directories(root_ / "saves", ec);
}

fs::path SaveSlots::folder(std::string_view id) const { return root_ / "saves" / utf8_path(id); }

bool SaveSlots::exists(std::string_view id) const {
    SlotInfo info;
    return !id.empty() && read_info(folder(id), info);
}

std::vector<SlotInfo> SaveSlots::list() const {
    std::vector<SlotInfo> out;
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(root_ / "saves", ec)) {
        if (!e.is_directory(ec)) continue;
        const std::string name = path_to_utf8(e.path().filename());
        if (name.empty() || name[0] == '.') continue; // temporary folders of a commit
        SlotInfo info;
        if (!read_info(e.path(), info)) continue;
        info.id = name;
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](const SlotInfo& a, const SlotInfo& b) {
        return a.saved_at != b.saved_at ? a.saved_at > b.saved_at : a.id < b.id;
    });
    return out;
}

std::optional<SlotInfo> SaveSlots::info(std::string_view id) const {
    SlotInfo info;
    if (id.empty() || !read_info(folder(id), info)) return std::nullopt;
    info.id = std::string(id);
    return info;
}

std::optional<SlotInfo> SaveSlots::latest() const {
    std::vector<SlotInfo> all = list();
    if (all.empty()) return std::nullopt;
    return all.front();
}

std::string SaveSlots::new_id() const {
    std::error_code ec;
    for (u32 n = 1;; ++n) {
        std::string id = "slot-" + std::to_string(n);
        if (!fs::exists(folder(id), ec)) return id;
    }
}

bool SaveSlots::begin_session(std::string_view from_slot, std::string* error) {
    std::error_code ec;
    fs::remove_all(session(), ec);
    if (ec) {
        if (error) *error = "не удалось очистить папку игры: " + ec.message();
        return false;
    }
    if (from_slot.empty()) {
        fs::create_directories(session(), ec);
        return true;
    }
    if (!exists(from_slot)) {
        if (error) *error = "нет сохранения «" + std::string(from_slot) + "»";
        return false;
    }
    if (!copy_tree(folder(from_slot), session(), error)) return false;
    fs::remove(session() / "slot.json", ec);
    return true;
}

bool SaveSlots::commit(SlotInfo info, std::string* error) {
    if (info.id.empty()) info.id = new_id();
    info.saved_at = static_cast<i64>(std::time(nullptr));
    const fs::path target = folder(info.id);
    const fs::path temp = root_ / "saves" / utf8_path("." + info.id + ".new");
    const fs::path old = root_ / "saves" / utf8_path("." + info.id + ".old");
    std::error_code ec;
    fs::remove_all(temp, ec);
    fs::remove_all(old, ec);
    if (!copy_tree(session(), temp, error)) return false;
    if (!write_text(temp / "slot.json", slot_json(info))) {
        if (error) *error = "не удалось записать slot.json";
        return false;
    }
    // Swap in: the old slot steps aside, the new one takes its name.
    if (fs::exists(target, ec)) fs::rename(target, old, ec);
    if (ec) {
        if (error) *error = "сохранение занято другой программой: " + ec.message();
        return false;
    }
    fs::rename(temp, target, ec);
    if (ec) {
        fs::rename(old, target, ec); // put the old one back
        if (error) *error = "не удалось записать сохранение";
        return false;
    }
    fs::remove_all(old, ec);
    return true;
}

bool SaveSlots::remove(std::string_view id) {
    if (id.empty()) return false;
    std::error_code ec;
    return fs::remove_all(folder(id), ec) > 0 && !ec;
}

bool slot_fits(const SlotInfo& slot, std::string_view module, std::string* why) {
    const std::string name = slot.title.empty() ? slot.id : slot.title;
    if (!slot.module_error.empty()) {
        if (why) *why = "сохранение «" + name + "» повреждено: " + slot.module_error;
        return false;
    }
    if (slot.module != module) {
        if (why) *why = "сохранение «" + name + "» сделано другой игрой (модуль «" + slot.module + "»), эта игра — модуль «" +
                        std::string(module) + "»";
        return false;
    }
    return true;
}

// --- settings ----------------------------------------------------------------

Settings load_settings(const fs::path& dir) {
    Settings s;
    std::string text;
    if (read_text(dir / "settings.json", text)) {
        data::LoadReport report;
        if (!data::from_json(s, text, report)) FORGE_WARN("settings: %s", report.error.c_str());
    }
    s.ui_scale = std::clamp(s.ui_scale, 0.5f, 3.0f);
    // A volume from 0 (the player's silence, kept as it is) to 1: a file edited by hand to 1.5 or -1 is brought in.
    for (f32* v : {&s.master_volume, &s.music_volume, &s.sound_volume}) *v = std::clamp(*v, 0.0f, 1.0f);
    return s;
}

bool save_settings(const fs::path& dir, const Settings& settings) {
    return write_text(dir / "settings.json", data::to_json(settings));
}

std::string format_playtime(f64 seconds) {
    const i64 minutes = static_cast<i64>(seconds / 60.0);
    char buf[64];
    if (minutes >= 60) std::snprintf(buf, sizeof(buf), "%lld ч %02lld мин", static_cast<long long>(minutes / 60), static_cast<long long>(minutes % 60));
    else std::snprintf(buf, sizeof(buf), "%lld мин", static_cast<long long>(minutes));
    return buf;
}

std::string format_date(i64 unix_seconds) {
    const std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%d.%m.%Y %H:%M", &tm);
    return buf;
}

} // namespace forge::game
