#pragma once

// A game's levels: their ids, names and the start level (game/levels.json).
//
// A level's id never changes: renaming a level changes its name, making it the
// start changes start_level. Its folder comes from the id, so the list cannot
// point outside the game: "level" is game/level (the one level every game had
// before), any other id is game/levels/<id>. A game without levels.json has
// that one level, named «Уровень 1», and nothing is written into it until the
// author changes the list.

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace forge::level {

inline constexpr const char* kLevelsFile = "levels.json";
inline constexpr std::string_view kFirstLevel = "level";
inline constexpr std::string_view kFirstLevelName = "Уровень 1";
inline constexpr unsigned kMaxLevelName = 60; // characters

struct LevelEntry {
    std::string id;
    std::string name;
};

struct LevelList {
    std::vector<LevelEntry> levels; // in the order they were made; never empty when read
    std::string start;              // the id of the start level, one of levels
    bool from_file = false;         // read from levels.json (false: a game without it, its one level)
    bool broken = false;            // levels.json is there and cannot be used: the one level, and nothing is written
    bool locked = false;            // levels.json is read, but not all of it: used as read, and nothing is written
                                    // (writing it anew would lose what was left out)
    std::string problem;            // why it is broken or locked
    std::vector<std::string> notes; // what was left out or put right when it was read, in words

    const LevelEntry* find(std::string_view id) const;
    const LevelEntry& start_level() const;
    // Whether a change of the list may be written (neither broken nor locked), and why not, in words.
    bool writable() const { return !broken && !locked; }
    std::string why_unchanged() const;
};

// "level", or letters a-z, digits, "_" and "-", 1 to 40 of them.
bool valid_level_id(std::string_view id);
// game/level for "level", game/levels/<id> for another.
std::filesystem::path level_folder(const std::filesystem::path& game, std::string_view id);
// The entry whose folder that is (the same folder, however written); null for a folder that is no level of the
// game (a level opened with --level from elsewhere).
const LevelEntry* level_of_folder(const std::filesystem::path& game, const LevelList& list,
                                  const std::filesystem::path& folder);

LevelList read_levels(const std::filesystem::path& game);
// The whole list in one file, written over the old one at once. Refused for a broken or locked list: the author's
// file is not written over.
bool write_levels(const std::filesystem::path& game, const LevelList& list, std::string* error);

// The folder a new game starts in: the start level's; id is its id. A game without levels.json: game/level.
std::filesystem::path start_level_folder(const std::filesystem::path& game, std::string* id = nullptr,
                                         std::vector<std::string>* notes = nullptr);

// What is wrong with the name for the level id (empty for a new one): empty when it fits (not empty, not longer
// than kMaxLevelName, no control characters, no other level of the name where case does not count).
std::string level_name_problem(const LevelList& list, std::string_view name, std::string_view id = {});
// «Уровень 2», «Уровень 3», …: the first no level has.
std::string free_level_name(const LevelList& list);
// "l" and 8 hex digits, no level's id and no folder in the game.
std::string new_level_id(const std::filesystem::path& game, const LevelList& list);

// Changes of the list, each written at once. On success list is the new list; otherwise list and the game are as
// they were and error says why. A new level: its empty folder, then levels.json (the folder goes again if that is
// not written); id is its id.
bool add_level(const std::filesystem::path& game, LevelList& list, std::string_view name, std::string& id,
               std::string* error);
bool rename_level(const std::filesystem::path& game, LevelList& list, std::string_view id, std::string_view name,
                  std::string* error);
bool set_start_level(const std::filesystem::path& game, LevelList& list, std::string_view id, std::string* error);

} // namespace forge::level
