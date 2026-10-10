#include "forge/editor/sources.h"

#include "forge/assets/content_hash.h"
#include "forge/core/file.h"
#include "forge/core/path.h"

#include <yyjson.h>

#include <algorithm>
#include <cstdlib>

namespace forge::editor::sources {

namespace fs = std::filesystem;

namespace {

std::string str(yyjson_val* o, const char* key) {
    const char* s = yyjson_get_str(yyjson_obj_get(o, key));
    return s ? s : "";
}

// A copy's place in the game's folder: a folder and a file, never out of it (no full path, no "..", nothing a
// Windows path would read as another place).
bool inside(std::string_view file) {
    if (file.empty() || file.find('\\') != std::string_view::npos || file.find(':') != std::string_view::npos) return false;
    const fs::path p = utf8_path(file);
    if (p.has_root_name() || p.has_root_directory()) return false;
    for (const fs::path& part : p)
        if (part == ".." || part == ".") return false;
    return true;
}

std::string folder_of(std::string_view file) {
    const usize slash = file.rfind('/');
    return slash == std::string_view::npos ? std::string() : std::string(file.substr(0, slash));
}

std::string name_of(std::string_view file) {
    const usize slash = file.rfind('/');
    return std::string(slash == std::string_view::npos ? file : file.substr(slash + 1));
}

} // namespace

std::string hash_of(const std::vector<u8>& bytes) { return assets::hash_bytes(bytes).to_hex(); }

bool Sources::load(const fs::path& game_dir, std::string* error) {
    game_ = game_dir;
    entries_.clear();
    std::error_code ec;
    const fs::path file = game_dir / utf8_path(kFile);
    if (!fs::exists(file, ec)) return true;
    std::vector<u8> bytes;
    if (!read_file(file, bytes)) {
        if (error) *error = path_to_utf8(file) + " не читается";
        return false;
    }
    yyjson_doc* doc = yyjson_read(reinterpret_cast<const char*>(bytes.data()), bytes.size(), 0);
    if (!doc) {
        if (error) *error = path_to_utf8(file) + ": это не JSON";
        return false;
    }
    std::string refused;
    yyjson_val* files = yyjson_obj_get(yyjson_doc_get_root(doc), "files");
    usize i, n;
    yyjson_val *key, *val;
    yyjson_obj_foreach(files, i, n, key, val) {
        Entry e;
        e.file = yyjson_get_str(key);
        const std::optional<Guid> id = Guid::parse(str(val, "asset"));
        if (!inside(e.file) || folder_of(e.file).empty() || !id) {
            refused += (refused.empty() ? "" : ", ") + e.file;
            continue;
        }
        e.asset = *id;
        e.from = str(val, "from");
        e.hash = str(val, "hash");
        entries_.push_back(std::move(e));
    }
    yyjson_doc_free(doc);
    if (!refused.empty() && error) *error = path_to_utf8(file) + ": не файл папки игры, пропущено: " + refused;
    return true;
}

const Entry* Sources::of_file(std::string_view file) const {
    auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.file == file; });
    return it == entries_.end() ? nullptr : &*it;
}

bool Sources::save(std::string* error) {
    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) { return a.file < b.file; });
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_val* files = yyjson_mut_obj_add_obj(doc, root, "files");
    for (const Entry& e : entries_) {
        yyjson_mut_val* o = yyjson_mut_obj_add_obj(doc, files, e.file.c_str());
        yyjson_mut_obj_add_strcpy(doc, o, "asset", e.asset.to_string().c_str());
        yyjson_mut_obj_add_strncpy(doc, o, "from", e.from.data(), e.from.size());
        yyjson_mut_obj_add_strncpy(doc, o, "hash", e.hash.data(), e.hash.size());
    }
    usize len = 0;
    char* text = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, &len);
    yyjson_mut_doc_free(doc);
    if (!text) {
        if (error) *error = "sources.json не составился";
        return false;
    }
    std::string out(text, len);
    std::free(text);
    out += "\n";
    const fs::path file = game_ / utf8_path(kFile);
    if (!write_file_atomic(file, std::span(reinterpret_cast<const u8*>(out.data()), out.size()))) {
        if (error) *error = path_to_utf8(file) + " не записался";
        return false;
    }
    return true;
}

std::string Sources::copy_in(const Asset& asset, std::string_view folder, std::string* error) {
    std::vector<u8> bytes;
    if (!read_file(asset.file, bytes)) {
        if (error) *error = "ресурс «" + asset.rel + "» не читается";
        return {};
    }
    const std::string hash = hash_of(bytes);
    std::error_code ec;
    fs::create_directories(game_ / utf8_path(folder), ec);
    auto write = [&](const std::string& file) {
        if (write_file_atomic(game_ / utf8_path(file), bytes)) return true;
        if (error) *error = "файл игры " + file + " не записался";
        return false;
    };
    // The copy this asset already has there.
    for (Entry& e : entries_) {
        if (e.asset != asset.id || folder_of(e.file) != folder) continue;
        std::vector<u8> there;
        if ((!read_file(game_ / utf8_path(e.file), there) || there != bytes) && !write(e.file)) return {};
        e.from = asset.rel;
        e.hash = hash;
        return save(error) ? name_of(e.file) : std::string();
    }
    // A new one under its own name; one of that name with the same content and no other asset is taken as it is.
    const fs::path name = asset.file.filename();
    const std::string stem = path_to_utf8(name.stem()), ext = path_to_utf8(name.extension());
    std::string file = std::string(folder) + "/" + path_to_utf8(name);
    for (int n = 2;; ++n) {
        if (!fs::exists(game_ / utf8_path(file), ec)) {
            if (!write(file)) return {};
            break;
        }
        std::vector<u8> there;
        if (!of_file(file) && read_file(game_ / utf8_path(file), there) && there == bytes) break;
        file = std::string(folder) + "/" + stem + " " + std::to_string(n) + ext;
    }
    entries_.push_back({file, asset.id, asset.rel, hash});
    return save(error) ? name_of(file) : std::string();
}

Report Sources::sync(const std::function<std::optional<Asset>(const Guid&)>& find) {
    Report r;
    bool dirty = false;
    std::error_code ec;
    for (Entry& e : entries_) {
        const fs::path copy = game_ / utf8_path(e.file);
        const bool there = fs::is_regular_file(copy, ec);
        const std::optional<Asset> a = find(e.asset);
        if (!a) {
            (there ? r.gone : r.lost).push_back(e);
            continue;
        }
        std::vector<u8> bytes;
        if (!read_file(a->file, bytes)) {
            r.errors.push_back("ресурс «" + a->rel + "» не читается: " + e.file + " остался как был");
            continue;
        }
        const std::string hash = hash_of(bytes);
        const bool changed = hash != e.hash;
        if (a->rel != e.from) {
            e.from = a->rel; // renamed or moved in «Ресурсы»: the same asset
            dirty = true;
        }
        if (there && !changed) continue;
        if (!write_file_atomic(copy, bytes)) {
            r.errors.push_back("файл игры " + e.file + " не записался");
            continue;
        }
        e.hash = hash;
        dirty = true;
        (there ? r.updated : r.restored).push_back(e);
    }
    if (std::string error; dirty && !save(&error)) r.errors.push_back(error);
    return r;
}

} // namespace forge::editor::sources
