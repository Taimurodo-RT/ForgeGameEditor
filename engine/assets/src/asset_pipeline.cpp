#include "forge/assets/asset_pipeline.h"

#include "forge/core/jobs.h"
#include "forge/core/log.h"
#include "forge/core/profile.h"
#include "forge/core/time.h"
#include "forge/data/json.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace forge::assets {

namespace {

constexpr const char* kMetaExtension = ".meta";

bool read_file(const fs::path& path, std::vector<u8>& out) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamoff size = in.tellg();
    if (size < 0) return false;
    out.resize(static_cast<usize>(size));
    in.seekg(0);
    return size == 0 || static_cast<bool>(in.read(reinterpret_cast<char*>(out.data()), size));
}

// Write to a temporary file, then rename: a crash or a reader never sees half a file.
bool write_file_atomic(const fs::path& path, std::span<const u8> bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) return false;
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

std::string to_generic_utf8(const fs::path& p) {
    const std::u8string s = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

std::string lower_extension(const fs::path& p) {
    std::string ext = to_generic_utf8(p.extension());
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return ext;
}

std::string join_tags(const std::vector<std::string>& tags) {
    std::string out;
    for (const std::string& t : tags) {
        if (!out.empty()) out += ' ';
        out += t;
    }
    return out;
}

struct Entry {
    fs::path abs;
    std::string rel;
    u64 size = 0;
    i64 mtime = 0;
    i64 meta_mtime = 0; // 0: no .meta yet
    const Importer* importer = nullptr;

    // Filled on worker threads.
    std::optional<AssetMeta> meta;
    const AssetRecord* prev = nullptr;
    bool unchanged = false;
    std::optional<ContentHash> hash;

    // Decided on the main thread.
    Guid id;
    bool write_meta = false;
    bool cook = false;
    ContentHash cook_key;
    CookOutput out;
};

} // namespace

AssetPipeline::AssetPipeline(fs::path assets_dir, fs::path library_dir)
    : assets_dir_(std::move(assets_dir)), library_dir_(std::move(library_dir)) {}

AssetPipeline::~AssetPipeline() = default;

bool AssetPipeline::open(std::string* error) {
    std::error_code ec;
    fs::create_directories(library_dir_ / "cooked", ec);
    if (ec) {
        if (error) *error = "cannot create " + to_generic_utf8(library_dir_) + ": " + ec.message();
        return false;
    }
    return db_.open(library_dir_ / "assets.db", error);
}

void AssetPipeline::add_importer(std::unique_ptr<Importer> importer) { importers_.push_back(std::move(importer)); }

const Importer* AssetPipeline::importer_for(const fs::path& file) const {
    const std::string ext = lower_extension(file);
    for (const auto& imp : importers_) {
        if (imp->handles(ext)) return imp.get();
    }
    return nullptr;
}

fs::path AssetPipeline::cooked_path(const ContentHash& key) const {
    const std::string hex = key.to_hex();
    return library_dir_ / "cooked" / hex.substr(0, 2) / (hex + ".bin");
}

RefreshReport AssetPipeline::refresh() {
    FORGE_ZONE();
    RefreshReport report;
    const u64 t0 = time_now_ns();

    // --- what the index knew last time ---
    std::vector<AssetRecord> records = db_.all();
    std::unordered_map<Guid, const AssetRecord*, GuidHash> by_id;
    std::unordered_map<std::string, const AssetRecord*> by_path;
    by_id.reserve(records.size());
    by_path.reserve(records.size());
    for (const AssetRecord& r : records) {
        by_id.emplace(r.id, &r);
        by_path.emplace(r.path, &r);
    }

    // --- what is on disk now (cheap: names, sizes, times) ---
    std::vector<Entry> entries;
    std::unordered_map<std::string, i64> meta_mtimes; // source path -> its .meta's time
    {
        FORGE_ZONE_N("Scan folder");
        std::error_code ec;
        // Relative paths by cutting the root prefix: fs::relative resolves
        // every path against the disk and costs several system calls each.
        const usize root_len = assets_dir_.native().size() + 1;
        auto relative = [&](const fs::path& p) { return to_generic_utf8(fs::path(p.native().substr(root_len))); };
        fs::recursive_directory_iterator it(assets_dir_, fs::directory_options::skip_permission_denied, ec), end;
        for (; it != end; it.increment(ec)) {
            if (ec) break;
            const fs::path& p = it->path();
            const std::string filename = to_generic_utf8(p.filename());
            if (!filename.empty() && filename[0] == '.') { // hidden files and folders (.git, .DS_Store)
                if (it->is_directory()) it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file()) continue;
            const std::string ext = lower_extension(p);
            if (ext == kMetaExtension) {
                std::string source = relative(p);
                source.resize(source.size() - std::strlen(kMetaExtension));
                meta_mtimes[source] = static_cast<i64>(it->last_write_time().time_since_epoch().count());
                continue;
            }
            if (ext == ".tmp") continue;
            Entry e;
            e.abs = p;
            e.rel = relative(p);
            e.size = it->file_size();
            e.mtime = static_cast<i64>(it->last_write_time().time_since_epoch().count());
            e.importer = importer_for(p);
            if (e.importer) entries.push_back(std::move(e));
        }
    }
    for (Entry& e : entries) {
        auto it = meta_mtimes.find(e.rel);
        if (it != meta_mtimes.end()) e.meta_mtime = it->second;
    }
    report.files = static_cast<u32>(entries.size());

    // --- read metas, skip untouched files, hash the rest: on all cores ---
    jobs::parallel_for(static_cast<u32>(entries.size()), 64, [&](u32 begin, u32 end) {
        FORGE_ZONE_N("Check files");
        for (u32 i = begin; i < end; ++i) {
            Entry& e = entries[i];
            std::error_code ec;
            // The common case: file and .meta untouched since the last scan.
            // Decided from directory listing data alone, nothing is opened.
            if (auto it = by_path.find(e.rel); it != by_path.end() && e.meta_mtime != 0) {
                const AssetRecord* p = it->second;
                if (p->size == e.size && p->mtime == e.mtime && p->meta_mtime == e.meta_mtime &&
                    p->type == e.importer->name() && p->importer_version == e.importer->version() &&
                    fs::exists(cooked_path(p->cook_key), ec)) {
                    e.prev = p;
                    e.meta = AssetMeta{p->id, {}};
                    e.unchanged = true;
                    continue;
                }
            }

            fs::path meta_path = e.abs;
            meta_path += kMetaExtension;
            std::vector<u8> meta_bytes;
            if (read_file(meta_path, meta_bytes)) {
                AssetMeta meta;
                data::LoadReport lr;
                if (data::from_json(meta, {reinterpret_cast<const char*>(meta_bytes.data()), meta_bytes.size()}, lr) &&
                    !meta.id.is_null()) {
                    e.meta = std::move(meta);
                }
            }
            if (e.meta) {
                auto it = by_id.find(e.meta->id);
                if (it != by_id.end()) e.prev = it->second;
            } else {
                auto it = by_path.find(e.rel);
                if (it != by_path.end()) e.prev = it->second;
            }
            e.hash = hash_file(e.abs);
        }
    });

    // --- decide ids and work: one thread, no file access ---
    std::unordered_set<std::string> present;
    present.reserve(entries.size());
    for (const Entry& e : entries) present.insert(e.rel);

    std::unordered_set<Guid, GuidHash> seen;
    seen.reserve(entries.size());
    // Index records that lost their file, by content: a file without a .meta
    // whose content matches one of them was moved or renamed outside the editor.
    std::unordered_map<u64, std::vector<const AssetRecord*>> orphans_by_hash;
    for (const AssetRecord& r : records) {
        if (!present.count(r.path)) orphans_by_hash[r.source_hash.lo].push_back(&r);
    }

    // Untouched files first: a file still at its recorded path has the
    // strongest claim to its id, ahead of any copy that carries the same .meta.
    for (Entry& e : entries) {
        if (e.unchanged && seen.insert(e.meta->id).second) {
            e.id = e.meta->id;
            ++report.unchanged;
        } else if (e.unchanged) {
            e.unchanged = false; // a second file claims this id: look at it properly
            e.hash = hash_file(e.abs);
        }
    }

    for (Entry& e : entries) {
        if (e.unchanged) continue;
        if (!e.hash) {
            report.messages.push_back(e.rel + ": cannot read the file");
            ++report.failed;
            // Keep its record: the file is still there, just unreadable right now.
            if (e.meta) seen.insert(e.meta->id);
            else if (e.prev) seen.insert(e.prev->id);
            continue;
        }

        if (e.meta && !seen.count(e.meta->id)) {
            e.id = e.meta->id;
        } else {
            if (e.meta) {
                // A copied file brought its original's .meta: give the copy its own id.
                report.messages.push_back(e.rel + ": duplicate id (copied .meta?), a new id was assigned");
                e.prev = nullptr;
            } else {
                auto it = orphans_by_hash.find(e.hash->lo);
                if (it != orphans_by_hash.end()) {
                    for (const AssetRecord*& r : it->second) {
                        if (r && r->source_hash == *e.hash && !seen.count(r->id)) {
                            e.prev = r;
                            r = nullptr;
                            break;
                        }
                    }
                }
            }
            AssetMeta meta = e.meta.value_or(AssetMeta{});
            if (e.prev && !e.meta) {
                meta.id = e.prev->id;
                ++report.moved;
            } else {
                meta.id = Guid::generate();
                e.prev = nullptr;
                ++report.added;
            }
            e.meta = std::move(meta);
            e.id = e.meta->id;
            e.write_meta = true;
        }
        seen.insert(e.id);

        const std::string salt = std::string(e.importer->name()) + '#' + std::to_string(e.importer->version());
        e.cook_key = hash_combine(*e.hash, {reinterpret_cast<const u8*>(salt.data()), salt.size()});
        std::error_code ec;
        if (e.prev && e.prev->cook_key == e.cook_key && fs::exists(cooked_path(e.cook_key), ec)) {
            ++report.retouched; // touched, moved or checked out again: same content, nothing to cook
        } else {
            e.cook = true;
        }
    }

    // --- write metas and cook: on all cores ---
    jobs::parallel_for(static_cast<u32>(entries.size()), 8, [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) {
            Entry& e = entries[i];
            if (e.write_meta) {
                const std::string json = data::to_json(*e.meta);
                fs::path meta_path = e.abs;
                meta_path += kMetaExtension;
                if (!write_file_atomic(meta_path, {reinterpret_cast<const u8*>(json.data()), json.size()})) {
                    e.out.error = "cannot write " + to_generic_utf8(meta_path.filename());
                    continue;
                }
                std::error_code ec;
                e.meta_mtime = static_cast<i64>(fs::last_write_time(meta_path, ec).time_since_epoch().count());
            }
            if (!e.cook) continue;
            FORGE_ZONE_N("Cook asset");
            std::vector<u8> source;
            if (!read_file(e.abs, source)) {
                e.out.error = "cannot read the file";
                continue;
            }
            e.importer->cook({source, e.rel, &*e.meta}, e.out);
            if (e.out.error.empty() && !write_file_atomic(cooked_path(e.cook_key), e.out.cooked)) {
                e.out.error = "cannot write the cooked result";
            }
            e.out.cooked.clear();
            e.out.cooked.shrink_to_fit();
        }
    });

    // --- record everything in one transaction ---
    {
        FORGE_ZONE_N("Update index");
        db_.begin();
        for (Entry& e : entries) {
            if (e.unchanged || !e.hash) continue;
            if (!e.out.error.empty()) {
                report.messages.push_back(e.rel + ": " + e.out.error);
                ++report.failed;
                continue; // keep the old record and cooked result, if any
            }
            AssetRecord r;
            r.id = e.id;
            r.path = e.rel;
            r.type = e.importer->name();
            r.name = to_generic_utf8(e.abs.stem());
            r.tags = join_tags(e.meta->tags);
            r.size = e.size;
            r.mtime = e.mtime;
            r.meta_mtime = e.meta_mtime;
            r.source_hash = *e.hash;
            r.cook_key = e.cook_key;
            r.importer_version = e.importer->version();
            db_.upsert(r);
            if (e.cook) {
                db_.set_dependencies(e.id, e.out.dependencies);
                ++report.cooked;
            }
        }
        for (const AssetRecord& r : records) {
            if (!seen.count(r.id)) {
                db_.remove(r.id);
                ++report.removed;
            }
        }
        db_.commit();
    }

    report.ms = ns_to_ms(time_now_ns() - t0);
    return report;
}

} // namespace forge::assets
