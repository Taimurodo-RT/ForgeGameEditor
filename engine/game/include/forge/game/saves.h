#pragma once

// Save slots, settings and where a game keeps its files.
//
// A running game works in a session folder: the world's region files, the
// entities and the game's own state are written there by World::save(),
// Scene::save() and the game. Saving into a slot copies the session folder
// over the slot (through a temporary folder and a rename, so a crash never
// leaves half a slot). Loading copies a slot into a fresh session folder.
// So any slot can be saved over at any time, and progress that was not saved
// never touches the slots.
//
//   <user dir>/saves/<slot id>/slot.json    title, place, play time, date, the game's module
//   <user dir>/saves/<slot id>/...          what the session folder held
//   <user dir>/session/                     the game being played
//   <user dir>/settings.json

#include "forge/core/types.h"
#include "forge/data/reflect.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace forge::game {

// --- folders -----------------------------------------------------------------

// The folder holding the running executable.
std::filesystem::path exe_dir();
// The game's data: "data" next to the executable when it is there (a
// packaged game), else dev_dir (running from the build folder).
std::filesystem::path find_data_dir(const std::filesystem::path& dev_dir);
// Where saves and settings go: %APPDATA%/<org>/<game> on Windows,
// ~/.local/share/<org>/<game> on Linux. Created if missing.
std::filesystem::path user_dir(std::string_view org, std::string_view game);

// --- slots -------------------------------------------------------------------

struct SlotInfo {
    std::string id;
    std::string title;    // "Сохранение 3" or what the player typed
    std::string location; // where the hero was ("Старая шахта")
    f64 playtime_s = 0;
    i64 saved_at = 0;     // seconds since 1970 (UTC)
    bool autosave = false;
    u32 version = 1;      // of the game's save format
    // The module of the game that saved it (step 14.3a), slot.json "module" read by the rule of game.json
    // (game_module.h): none in the file (a slot from before) is "slice"; an empty one or one that is no string leaves
    // module empty and module_error saying why: such a slot is no slot of any module, "slice" included.
    std::string module;
    std::string module_error;
};

// Whether a game of module may load slot: the slot's module is its own. False: why, in words for the player.
bool slot_fits(const SlotInfo& slot, std::string_view module, std::string* why = nullptr);

class SaveSlots {
public:
    explicit SaveSlots(std::filesystem::path user_dir);

    // Newest first. Folders without a readable slot.json are left out.
    std::vector<SlotInfo> list() const;
    // One slot's slot.json; none when there is no readable one.
    std::optional<SlotInfo> info(std::string_view id) const;
    std::optional<SlotInfo> latest() const;
    bool exists(std::string_view id) const;
    std::filesystem::path folder(std::string_view id) const;

    // A fresh id for a new slot ("slot-3", or "autosave").
    std::string new_id() const;

    // --- the session ---
    std::filesystem::path session() const { return root_ / "session"; }
    // Empties the session folder (a new game) or fills it from a slot.
    bool begin_session(std::string_view from_slot = {}, std::string* error = nullptr);
    // Copies the session into the slot (replacing it) with info as slot.json.
    // info.id names the slot; saved_at is filled in; info.module, when set, goes in as "module".
    bool commit(SlotInfo info, std::string* error = nullptr);

    bool remove(std::string_view id);

private:
    std::filesystem::path root_;
};

// --- settings ----------------------------------------------------------------

struct Settings {
    bool fullscreen = false;
    bool vsync = true;
    f32 ui_scale = 1.0f;
    f32 master_volume = 1.0f;
    f32 music_volume = 0.7f;
    f32 sound_volume = 1.0f;
    std::string theme; // empty = the game's own
    bool show_fps = false;
};

Settings load_settings(const std::filesystem::path& user_dir);
bool save_settings(const std::filesystem::path& user_dir, const Settings& settings);

std::string format_playtime(f64 seconds); // "1 ч 05 мин", "12 мин"
std::string format_date(i64 unix_seconds); // local time, "04.10.2026 14:05"

} // namespace forge::game

FORGE_REFLECT_DECLARE(forge::game::Settings)
FORGE_REFLECT_DECLARE(forge::game::SlotInfo)
