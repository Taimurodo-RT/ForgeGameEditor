#pragma once

// The editor's index of every asset in the project: where it is, what it is,
// what it depends on, and whether its cooked form is up to date. Stored in
// SQLite next to the project, so a project with 50 000 assets opens and
// searches instantly instead of rescanning the disk.
//
// One AssetDatabase per thread (SQLite connections are not shared).

#include "forge/assets/content_hash.h"
#include "forge/data/guid.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace forge::assets {

struct AssetRecord {
    Guid id;
    std::string path;      // relative to the project's asset folder, '/' separators
    std::string type;      // importer name: "image", "audio", "file", ...
    std::string name;      // file name without extension
    std::string tags;      // space-separated, searchable
    u64 size = 0;
    i64 mtime = 0;         // last write time, for the quick "unchanged?" check
    i64 meta_mtime = 0;    // last write time of the .meta next to it
    ContentHash source_hash;
    ContentHash cook_key;  // names the cooked result in the library
    u32 importer_version = 0;
};

struct SearchHit {
    Guid id;
    std::string path;
    std::string type;
    std::string name;
};

class AssetDatabase {
public:
    AssetDatabase() = default;
    ~AssetDatabase();
    AssetDatabase(const AssetDatabase&) = delete;
    AssetDatabase& operator=(const AssetDatabase&) = delete;

    // Opens or creates the database file (":memory:" for tests).
    bool open(const std::filesystem::path& file, std::string* error = nullptr);
    void close();
    bool is_open() const { return db_ != nullptr; }

    // Group many writes into one transaction: one disk sync instead of thousands.
    void begin();
    void commit();

    bool upsert(const AssetRecord& record);
    bool remove(const Guid& id);

    std::optional<AssetRecord> find(const Guid& id);
    std::optional<AssetRecord> find_by_path(std::string_view path);
    std::vector<AssetRecord> all();
    u64 count();

    // Full-text search over names, paths and tags. Every word must match the
    // start of a word in the asset ("кузн меч" finds "Кузнец/меч_стальной").
    // Case-insensitive, Cyrillic included. Optional filter by asset type.
    std::vector<SearchHit> search(std::string_view text, u32 limit = 100, std::string_view type = {});

    void set_dependencies(const Guid& asset, const std::vector<Guid>& depends_on);
    std::vector<Guid> dependents_of(const Guid& asset); // who must be recooked when `asset` changes

    const std::string& last_error() const { return error_; }

private:
    sqlite3_stmt* prepare(const char* sql);
    bool exec(const char* sql);
    bool fail(const char* what);

    sqlite3* db_ = nullptr;
    std::string error_;
    sqlite3_stmt* upsert_ = nullptr;
    sqlite3_stmt* remove_ = nullptr;
    sqlite3_stmt* evict_path_ = nullptr;
    sqlite3_stmt* find_ = nullptr;
    sqlite3_stmt* find_path_ = nullptr;
};

} // namespace forge::assets
