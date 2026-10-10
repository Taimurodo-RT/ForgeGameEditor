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
// A recorded copy is never taken away and no other file is touched.
//
// Only a file the game can take is copied: one «Ресурсы» imported as it is now
// (Asset::imported) and that reads as a picture or a sound (usable). A broken
// one leaves the copy as it was (Report::refused) until it is put right.
//
// A copy's name is its entry's even while the file is missing: another asset
// of the same file name gets "name 2.ext", so what uses one copy never gets
// the other's content.
//
// Each change is whole: when sources.json cannot be written, the files the
// change wrote get back what they had (a file it made is taken away again)
// and nothing is reported done.
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
#include <span>
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
    // hash_of the content «Ресурсы» last imported without an error; empty when not known. A file with other
    // content (its import failed, or it is not looked at yet) is not copied.
    std::string imported;
};

// An asset that is there, but whose file the game cannot take now.
struct Refusal {
    Entry entry;          // the copy, as it stays
    std::string why;      // what is wrong with the asset's file
    bool missing = false; // the copy is not there either
};

// What sync found, by copy.
struct Report {
    std::vector<Entry> updated;    // the asset changed: the copy has its new content
    std::vector<Entry> restored;   // the copy was not there: made again from its asset
    std::vector<Entry> gone;       // no asset any more: the copy stays as it was
    std::vector<Entry> lost;       // neither the copy nor its asset
    std::vector<Refusal> refused;  // the asset's file is broken: the copy stays as it was (or missing)
    std::vector<std::string> errors;
    bool changed() const { return !updated.empty() || !restored.empty(); }
};

class Sources {
public:
    // The game's sources.json (none yet: no copies known). False when it is
    // there and cannot be read; entries naming a file outside the game's
    // folder, or a file named twice, are left out (error says so).
    bool load(const std::filesystem::path& game_dir, std::string* error = nullptr);
    const std::filesystem::path& game_dir() const { return game_; }
    const std::vector<Entry>& entries() const { return entries_; }
    const Entry* of_file(std::string_view file) const;

    // The copy of an asset in a folder of the game ("pictures", "sounds"):
    // the copy it has there (with the asset's content now), else a new one
    // under the asset's file name, or "name 2.ext" when another file or
    // another asset's copy has that name. Recorded in sources.json. Its name
    // in that folder; empty when the asset's file is not one the game can
    // take, or cannot be read or written (error says why; nothing changed).
    std::string copy_in(const Asset& asset, std::string_view folder, std::string* error = nullptr);

    // Each copy against its asset (find: «Ресурсы» now, by Guid).
    Report sync(const std::function<std::optional<Asset>(const Guid&)>& find);

private:
    bool save(std::string* error);

    std::filesystem::path game_;
    std::vector<Entry> entries_;
};

// The content's hash as sources.json keeps it.
std::string hash_of(std::span<const u8> bytes);

// Whether the game can take these bytes as a file of that folder: a picture of
// "pictures" decodes, a sound of "sounds" decodes (WAV, OGG); a file of any
// other folder is taken as it is. why: what is wrong.
bool usable(std::string_view folder, std::span<const u8> bytes, std::string* why = nullptr);

} // namespace forge::editor::sources
