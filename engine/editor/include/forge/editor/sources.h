#pragma once

// Where a game's own copies of the author's files came from (step 14.1b).
//
// An object's picture, a screen's picture or a button's sound is a file of
// the game (game/pictures, game/sounds): the game needs nothing outside its
// folder to play. The author picks it in «Ресурсы» (the project's assets/),
// and the editor copies it in. sources.json in the game's folder keeps, for
// each such copy, the asset it was made of: its Guid, which «Ресурсы» keeps
// through renames and moves (the asset's .meta). So
//   - the same asset picked again gives its copy, not a second one
//     ("кристалл 2.png");
//   - a changed asset (edited in another program, then «Обновить» in
//     «Ресурсы») gives its copy the new content under the same name: every
//     object, screen and button that uses the copy shows or plays the new one,
//     and nothing in them changes;
//   - a copy that went missing comes back from its asset; an asset that is
//     gone is said, its copy keeps playing as it was.
// A copy is never taken away and no other file is touched.
//
//   {"files": {"pictures/кристалл.png": {"asset": "8c1e…", "from": "находки/кристалл синий.png", "hash": "…"}}}
//
// "from" is where the asset was when last seen (for messages only); "hash" is
// of the content last copied, so a copy changed in the game folder and not in
// «Ресурсы» is left as it is.

#include "forge/data/guid.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace forge::editor::sources {

inline constexpr std::string_view kFile = "sources.json";

struct Entry {
    std::string file;  // the copy, in the game's folder ("pictures/кристалл.png", with /)
    Guid asset;        // what it was made of
    std::string from;  // the asset's path in «Ресурсы» when last seen (with /)
    std::string hash;  // of the content last copied (hex)
};

// An asset as «Ресурсы» has it now.
struct Asset {
    Guid id;
    std::string rel;            // its path there ("находки/кристалл.png")
    std::filesystem::path file; // on disk
};

// What sync found, by copy.
struct Report {
    std::vector<Entry> updated;  // the asset changed: the copy has its new content
    std::vector<Entry> restored; // the copy was not there: made again from its asset
    std::vector<Entry> gone;     // no asset any more: the copy stays as it was
    std::vector<Entry> lost;     // neither the copy nor its asset
    std::vector<std::string> errors;
    bool changed() const { return !updated.empty() || !restored.empty(); }
};

class Sources {
public:
    // The game's sources.json (none yet: no copies known). False when it is
    // there and cannot be read; entries naming a file outside the game's
    // folder are left out (error says so).
    bool load(const std::filesystem::path& game_dir, std::string* error = nullptr);
    const std::filesystem::path& game_dir() const { return game_; }
    const std::vector<Entry>& entries() const { return entries_; }
    const Entry* of_file(std::string_view file) const;

    // The copy of an asset in a folder of the game ("pictures", "sounds"):
    // the copy it has there (with the asset's content now), else a new one
    // under the asset's file name, or "name 2.ext" when another file has that
    // name. Recorded in sources.json. Its name in that folder; empty when it
    // cannot be read or written (error says why).
    std::string copy_in(const Asset& asset, std::string_view folder, std::string* error = nullptr);

    // Each copy against its asset (find: «Ресурсы» now, by Guid).
    Report sync(const std::function<std::optional<Asset>(const Guid&)>& find);

private:
    bool save(std::string* error);

    std::filesystem::path game_;
    std::vector<Entry> entries_;
};

// The content's hash as sources.json keeps it.
std::string hash_of(const std::vector<u8>& bytes);

} // namespace forge::editor::sources
