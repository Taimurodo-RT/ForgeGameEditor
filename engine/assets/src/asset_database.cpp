#include "forge/assets/asset_database.h"

#include "forge/core/log.h"
#include "forge/core/profile.h"

#include <sqlite3.h>

#include <cstring>

namespace forge::assets {

namespace {

constexpr int kSchemaVersion = 1;

constexpr const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS assets(
    rowid INTEGER PRIMARY KEY,
    id BLOB NOT NULL UNIQUE,
    path TEXT NOT NULL UNIQUE,
    type TEXT NOT NULL,
    name TEXT NOT NULL,
    tags TEXT NOT NULL DEFAULT '',
    size INTEGER NOT NULL,
    mtime INTEGER NOT NULL,
    meta_mtime INTEGER NOT NULL DEFAULT 0,
    source_hash BLOB,
    cook_key BLOB,
    importer_version INTEGER NOT NULL);

CREATE INDEX IF NOT EXISTS assets_type ON assets(type);

CREATE VIRTUAL TABLE IF NOT EXISTS asset_search USING fts5(
    name, path, tags,
    content='assets', content_rowid='rowid',
    tokenize="unicode61 remove_diacritics 2 tokenchars '_'",
    prefix='1 2 3');

CREATE TRIGGER IF NOT EXISTS assets_ai AFTER INSERT ON assets BEGIN
    INSERT INTO asset_search(rowid, name, path, tags) VALUES (new.rowid, new.name, new.path, new.tags);
END;
CREATE TRIGGER IF NOT EXISTS assets_ad AFTER DELETE ON assets BEGIN
    INSERT INTO asset_search(asset_search, rowid, name, path, tags) VALUES ('delete', old.rowid, old.name, old.path, old.tags);
END;
CREATE TRIGGER IF NOT EXISTS assets_au AFTER UPDATE ON assets BEGIN
    INSERT INTO asset_search(asset_search, rowid, name, path, tags) VALUES ('delete', old.rowid, old.name, old.path, old.tags);
    INSERT INTO asset_search(rowid, name, path, tags) VALUES (new.rowid, new.name, new.path, new.tags);
END;

CREATE TABLE IF NOT EXISTS dependencies(
    asset BLOB NOT NULL,
    depends_on BLOB NOT NULL,
    PRIMARY KEY(asset, depends_on)) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS dependencies_reverse ON dependencies(depends_on);
)sql";

void bind_guid(sqlite3_stmt* s, int i, const Guid& g) {
    u8 bytes[16];
    std::memcpy(bytes, &g.hi, 8);
    std::memcpy(bytes + 8, &g.lo, 8);
    sqlite3_bind_blob(s, i, bytes, 16, SQLITE_TRANSIENT);
}

Guid column_guid(sqlite3_stmt* s, int i) {
    Guid g;
    if (sqlite3_column_bytes(s, i) == 16) {
        const auto* bytes = static_cast<const u8*>(sqlite3_column_blob(s, i));
        std::memcpy(&g.hi, bytes, 8);
        std::memcpy(&g.lo, bytes + 8, 8);
    }
    return g;
}

void bind_hash(sqlite3_stmt* s, int i, const ContentHash& h) { sqlite3_bind_blob(s, i, &h, sizeof(h), SQLITE_TRANSIENT); }

ContentHash column_hash(sqlite3_stmt* s, int i) {
    ContentHash h;
    if (sqlite3_column_bytes(s, i) == sizeof(h)) std::memcpy(&h, sqlite3_column_blob(s, i), sizeof(h));
    return h;
}

void bind_text(sqlite3_stmt* s, int i, std::string_view text) {
    // An empty string_view may have a null data pointer, which SQLite would
    // bind as NULL instead of ''.
    sqlite3_bind_text(s, i, text.data() ? text.data() : "", static_cast<int>(text.size()), SQLITE_TRANSIENT);
}

std::string column_text(sqlite3_stmt* s, int i) {
    const auto* t = reinterpret_cast<const char*>(sqlite3_column_text(s, i));
    return t ? std::string(t, static_cast<usize>(sqlite3_column_bytes(s, i))) : std::string();
}

constexpr const char* kRecordColumns = "id, path, type, name, tags, size, mtime, source_hash, cook_key, importer_version, meta_mtime";

AssetRecord read_record(sqlite3_stmt* s) {
    AssetRecord r;
    r.id = column_guid(s, 0);
    r.path = column_text(s, 1);
    r.type = column_text(s, 2);
    r.name = column_text(s, 3);
    r.tags = column_text(s, 4);
    r.size = static_cast<u64>(sqlite3_column_int64(s, 5));
    r.mtime = sqlite3_column_int64(s, 6);
    r.source_hash = column_hash(s, 7);
    r.cook_key = column_hash(s, 8);
    r.importer_version = static_cast<u32>(sqlite3_column_int(s, 9));
    r.meta_mtime = sqlite3_column_int64(s, 10);
    return r;
}

// Turns what a person typed into an FTS5 query: each word becomes a quoted
// prefix term, so punctuation in the input can never break the query syntax.
std::string to_fts_query(std::string_view text) {
    std::string query;
    usize i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '/' || text[i] == '.' || text[i] == '-')) ++i;
        usize start = i;
        while (i < text.size() && !(text[i] == ' ' || text[i] == '\t' || text[i] == '/' || text[i] == '.' || text[i] == '-')) ++i;
        if (i == start) break;
        std::string word;
        for (usize k = start; k < i; ++k) {
            if (text[k] == '"') word += "\"\"";
            else word += text[k];
        }
        if (!query.empty()) query += ' ';
        query += '"' + word + "\"*";
    }
    return query;
}

} // namespace

AssetDatabase::~AssetDatabase() { close(); }

bool AssetDatabase::fail(const char* what) {
    error_ = std::string(what) + ": " + (db_ ? sqlite3_errmsg(db_) : "database is not open");
    FORGE_WARN("asset database: %s", error_.c_str());
    return false;
}

bool AssetDatabase::exec(const char* sql) {
    char* msg = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
        error_ = msg ? msg : "unknown error";
        sqlite3_free(msg);
        FORGE_WARN("asset database: %s", error_.c_str());
        return false;
    }
    return true;
}

sqlite3_stmt* AssetDatabase::prepare(const char* sql) {
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v3(db_, sql, -1, SQLITE_PREPARE_PERSISTENT, &s, nullptr) != SQLITE_OK) {
        fail("prepare");
        return nullptr;
    }
    return s;
}

bool AssetDatabase::open(const std::filesystem::path& file, std::string* error) {
    close();
    const std::string utf8 = file.u8string().empty() ? std::string() : reinterpret_cast<const char*>(file.u8string().c_str());
    if (sqlite3_open_v2(utf8.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
        fail("open");
        if (error) *error = error_;
        close();
        return false;
    }
    sqlite3_busy_timeout(db_, 2000);

    // WAL: readers never block the writer; NORMAL sync is safe with WAL and
    // much faster. The index can always be rebuilt from the files on disk.
    bool ok = exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA temp_store=MEMORY;");

    int version = 0;
    if (ok) {
        sqlite3_stmt* s = prepare("PRAGMA user_version");
        if (s && sqlite3_step(s) == SQLITE_ROW) version = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }
    if (ok && version != kSchemaVersion) {
        // The index is a cache of the files on disk: on a schema change it is
        // rebuilt rather than migrated.
        ok = exec("DROP TABLE IF EXISTS asset_search; DROP TABLE IF EXISTS assets; DROP TABLE IF EXISTS dependencies;") &&
             exec(kSchema) && exec(("PRAGMA user_version=" + std::to_string(kSchemaVersion)).c_str());
    } else if (ok) {
        ok = exec(kSchema);
    }

    if (ok) {
        upsert_ = prepare((std::string("INSERT INTO assets(") + kRecordColumns +
                           ") VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11) ON CONFLICT(id) DO UPDATE SET "
                           "path=?2, type=?3, name=?4, tags=?5, size=?6, mtime=?7, source_hash=?8, cook_key=?9, "
                           "importer_version=?10, meta_mtime=?11")
                              .c_str());
        remove_ = prepare("DELETE FROM assets WHERE id=?1");
        evict_path_ = prepare("DELETE FROM assets WHERE path=?1 AND id<>?2");
        find_ = prepare((std::string("SELECT ") + kRecordColumns + " FROM assets WHERE id=?1").c_str());
        find_path_ = prepare((std::string("SELECT ") + kRecordColumns + " FROM assets WHERE path=?1").c_str());
        ok = upsert_ && remove_ && evict_path_ && find_ && find_path_;
    }
    if (!ok) {
        if (error) *error = error_;
        close();
    }
    return ok;
}

void AssetDatabase::close() {
    for (sqlite3_stmt** s : {&upsert_, &remove_, &evict_path_, &find_, &find_path_}) {
        sqlite3_finalize(*s);
        *s = nullptr;
    }
    if (db_) sqlite3_close(db_);
    db_ = nullptr;
}

void AssetDatabase::begin() { exec("BEGIN IMMEDIATE"); }
void AssetDatabase::commit() { exec("COMMIT"); }

bool AssetDatabase::upsert(const AssetRecord& r) {
    // A path belongs to one asset. When another asset took this path (files
    // swapped or replaced), its stale row goes; it comes back under its own
    // new path if it still exists.
    bind_text(evict_path_, 1, r.path);
    bind_guid(evict_path_, 2, r.id);
    sqlite3_step(evict_path_);
    sqlite3_reset(evict_path_);

    sqlite3_stmt* s = upsert_;
    bind_guid(s, 1, r.id);
    bind_text(s, 2, r.path);
    bind_text(s, 3, r.type);
    bind_text(s, 4, r.name);
    bind_text(s, 5, r.tags);
    sqlite3_bind_int64(s, 6, static_cast<sqlite3_int64>(r.size));
    sqlite3_bind_int64(s, 7, r.mtime);
    bind_hash(s, 8, r.source_hash);
    bind_hash(s, 9, r.cook_key);
    sqlite3_bind_int(s, 10, static_cast<int>(r.importer_version));
    sqlite3_bind_int64(s, 11, r.meta_mtime);
    const int rc = sqlite3_step(s);
    sqlite3_reset(s);
    return rc == SQLITE_DONE || fail("upsert");
}

bool AssetDatabase::remove(const Guid& id) {
    bind_guid(remove_, 1, id);
    const int rc = sqlite3_step(remove_);
    sqlite3_reset(remove_);
    set_dependencies(id, {});
    return rc == SQLITE_DONE || fail("remove");
}

std::optional<AssetRecord> AssetDatabase::find(const Guid& id) {
    bind_guid(find_, 1, id);
    std::optional<AssetRecord> out;
    if (sqlite3_step(find_) == SQLITE_ROW) out = read_record(find_);
    sqlite3_reset(find_);
    return out;
}

std::optional<AssetRecord> AssetDatabase::find_by_path(std::string_view path) {
    bind_text(find_path_, 1, path);
    std::optional<AssetRecord> out;
    if (sqlite3_step(find_path_) == SQLITE_ROW) out = read_record(find_path_);
    sqlite3_reset(find_path_);
    return out;
}

std::vector<AssetRecord> AssetDatabase::all() {
    FORGE_ZONE();
    std::vector<AssetRecord> out;
    sqlite3_stmt* s = prepare((std::string("SELECT ") + kRecordColumns + " FROM assets").c_str());
    if (!s) return out;
    while (sqlite3_step(s) == SQLITE_ROW) out.push_back(read_record(s));
    sqlite3_finalize(s);
    return out;
}

u64 AssetDatabase::count() {
    sqlite3_stmt* s = prepare("SELECT count(*) FROM assets");
    u64 n = 0;
    if (s && sqlite3_step(s) == SQLITE_ROW) n = static_cast<u64>(sqlite3_column_int64(s, 0));
    sqlite3_finalize(s);
    return n;
}

std::vector<SearchHit> AssetDatabase::search(std::string_view text, u32 limit, std::string_view type) {
    FORGE_ZONE();
    std::vector<SearchHit> out;
    const std::string query = to_fts_query(text);
    sqlite3_stmt* s;
    if (query.empty()) {
        // No words: list (optionally one type), ordered by name.
        s = prepare("SELECT id, path, type, name FROM assets WHERE (?2 = '' OR type = ?2) ORDER BY name LIMIT ?3");
    } else {
        s = prepare("SELECT a.id, a.path, a.type, a.name FROM asset_search JOIN assets a ON a.rowid = asset_search.rowid "
                    "WHERE asset_search MATCH ?1 AND (?2 = '' OR a.type = ?2) ORDER BY bm25(asset_search, 10.0, 2.0, 5.0) LIMIT ?3");
    }
    if (!s) return out;
    bind_text(s, 1, query);
    bind_text(s, 2, type);
    sqlite3_bind_int(s, 3, static_cast<int>(limit));
    while (sqlite3_step(s) == SQLITE_ROW) out.push_back({column_guid(s, 0), column_text(s, 1), column_text(s, 2), column_text(s, 3)});
    sqlite3_finalize(s);
    return out;
}

void AssetDatabase::set_dependencies(const Guid& asset, const std::vector<Guid>& depends_on) {
    sqlite3_stmt* del = prepare("DELETE FROM dependencies WHERE asset=?1");
    if (!del) return;
    bind_guid(del, 1, asset);
    sqlite3_step(del);
    sqlite3_finalize(del);
    if (depends_on.empty()) return;
    sqlite3_stmt* ins = prepare("INSERT OR IGNORE INTO dependencies(asset, depends_on) VALUES(?1, ?2)");
    if (!ins) return;
    for (const Guid& d : depends_on) {
        bind_guid(ins, 1, asset);
        bind_guid(ins, 2, d);
        sqlite3_step(ins);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);
}

std::vector<Guid> AssetDatabase::dependents_of(const Guid& asset) {
    std::vector<Guid> out;
    sqlite3_stmt* s = prepare("SELECT asset FROM dependencies WHERE depends_on=?1");
    if (!s) return out;
    bind_guid(s, 1, asset);
    while (sqlite3_step(s) == SQLITE_ROW) out.push_back(column_guid(s, 0));
    sqlite3_finalize(s);
    return out;
}

} // namespace forge::assets
