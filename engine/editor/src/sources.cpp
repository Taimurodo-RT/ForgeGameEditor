#include "forge/editor/sources.h"

#include "forge/assets/content_hash.h"
#include "forge/assets/image.h"
#include "forge/audio/audio.h"
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

// A name as a file system that ignores case sees it (Windows, and macOS as a rule): the letters of the Latin
// (with Latin-1), Greek and Cyrillic alphabets in lower case, everything else as it is. Two names with one key are
// one file there, so a game's folder keeps them apart on every system.
std::string key_of(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (usize i = 0; i < name.size(); ++i) {
        const u8 c = static_cast<u8>(name[i]);
        if (c >= 'A' && c <= 'Z') {
            out += static_cast<char>(c + 32);
            continue;
        }
        // Every letter folded here is two bytes of UTF-8, and stays two.
        if (c >= 0xC0 && c < 0xE0 && i + 1 < name.size() && (static_cast<u8>(name[i + 1]) & 0xC0) == 0x80) {
            u32 cp = ((c & 0x1Fu) << 6) | (static_cast<u8>(name[i + 1]) & 0x3Fu);
            if ((cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) || (cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) || (cp >= 0x410 && cp <= 0x42F))
                cp += 0x20;
            else if (cp >= 0x400 && cp <= 0x40F)
                cp += 0x50;
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
            ++i;
            continue;
        }
        out += static_cast<char>(c);
    }
    return out;
}

// The file of a folder that has this name as the file system may see it (another case of it), if any: its own name.
std::optional<std::string> on_disk(const fs::path& folder, std::string_view name) {
    const std::string key = key_of(name);
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string there = path_to_utf8(it->path().filename());
        if (key_of(there) == key) return there;
    }
    return std::nullopt;
}

// Whether an asset's file, as it is now, may become its copy in that folder.
bool fits(const Asset& a, std::string_view folder, std::span<const u8> bytes, const std::string& hash, std::string* why) {
    if (!a.imported.empty() && hash != a.imported) {
        if (why) *why = "«Ресурсы» не импортировали его в этом виде (файл повреждён или изменён после последнего «Обновить»)";
        return false;
    }
    return usable(folder, bytes, why);
}

// A file one change wrote, with what it had before (none: the change made it).
struct Written {
    fs::path file;
    std::optional<std::vector<u8>> was;
};

// What a change wrote, as it was before it, last first. Empty when all of it is back; else what is not.
std::string put_back(const std::vector<Written>& written) {
    std::string left;
    std::error_code ec;
    for (auto it = written.rbegin(); it != written.rend(); ++it) {
        const bool back = it->was ? write_file_atomic(it->file, *it->was) : fs::remove(it->file, ec);
        if (!back) left += (left.empty() ? "" : ", ") + path_to_utf8(it->file);
    }
    return left;
}

// Bytes into a file of the game, remembered with what it had (to be put back). False when it cannot be read or
// written (error says why).
bool write_copy(const fs::path& file, std::span<const u8> bytes, std::vector<Written>& written, std::string* error) {
    std::error_code ec;
    std::optional<std::vector<u8>> was;
    if (fs::exists(file, ec)) {
        std::vector<u8> old;
        if (!read_file(file, old)) {
            if (error) *error = "файл игры " + path_to_utf8(file) + " не читается";
            return false;
        }
        was = std::move(old);
    }
    if (!write_file_atomic(file, bytes)) {
        if (error) *error = "файл игры " + path_to_utf8(file) + " не записался";
        return false;
    }
    written.push_back({file, std::move(was)});
    return true;
}

} // namespace

std::string hash_of(std::span<const u8> bytes) { return assets::hash_bytes(bytes).to_hex(); }

bool usable(std::string_view folder, std::span<const u8> bytes, std::string* why) {
    std::string error;
    if (folder == "pictures") {
        assets::CookedTexture picture;
        if (assets::decode_image(bytes, picture, &error)) return true;
        if (why) *why = "это не картинка, которую читает игра (" + error + ")";
        return false;
    }
    if (folder == "sounds") {
        if (audio::decode(bytes, &error)) return true;
        if (why) *why = "это не звук, который читает игра (" + error + ")";
        return false;
    }
    return true;
}

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
    std::string refused, twice;
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
        if (of_file(e.file)) { // one file (in any case of its name), one asset: the first one named
            twice += (twice.empty() ? "" : ", ") + e.file;
            continue;
        }
        e.asset = *id;
        e.from = str(val, "from");
        e.hash = str(val, "hash");
        entries_.push_back(std::move(e));
    }
    yyjson_doc_free(doc);
    if (error) {
        if (!refused.empty()) *error = path_to_utf8(file) + ": не файл папки игры, пропущено: " + refused;
        if (!twice.empty()) *error += (error->empty() ? path_to_utf8(file) + ": " : "; ") + "файл назван дважды, взят первый: " + twice;
    }
    return true;
}

const Entry* Sources::of_file(std::string_view file) const {
    const std::string key = key_of(file);
    auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return key_of(e.file) == key; });
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
    if (std::string why; !fits(asset, folder, bytes, hash, &why)) {
        if (error) *error = "ресурс «" + asset.rel + "» не взят: " + why;
        return {};
    }
    std::error_code ec;
    fs::create_directories(game_ / utf8_path(folder), ec);
    const std::vector<Entry> before = entries_;
    std::vector<Written> written;
    // Recorded, or nothing of it stays.
    auto commit = [&](const std::string& file) -> std::string {
        std::string why;
        if (save(&why)) return name_of(file);
        entries_ = before;
        const std::string left = put_back(written);
        if (error) *error = why + (left.empty() ? "; файлы игры оставлены как были" : "; не вернулись как были: " + left);
        return {};
    };
    // The copy this asset already has there.
    for (Entry& e : entries_) {
        if (e.asset != asset.id || key_of(folder_of(e.file)) != key_of(folder)) continue;
        const std::string file = e.file;
        std::vector<u8> there;
        if ((!read_file(game_ / utf8_path(file), there) || there != bytes) && !write_copy(game_ / utf8_path(file), bytes, written, error))
            return {};
        e.from = asset.rel;
        e.hash = hash;
        return commit(file);
    }
    // A new one under its own name. A name another asset's copy has is not free, with its file there or not, nor is
    // a file's of the folder, in any case of the letters (one file where case does not count); a file of that name
    // with the same content and no asset is taken as it is.
    const fs::path name = asset.file.filename();
    const std::string stem = path_to_utf8(name.stem()), ext = path_to_utf8(name.extension());
    const fs::path dir = game_ / utf8_path(folder);
    std::string file = std::string(folder) + "/" + path_to_utf8(name);
    for (int n = 2;; ++n) {
        if (!of_file(file)) {
            const std::optional<std::string> there = on_disk(dir, name_of(file));
            if (!there) {
                if (!write_copy(game_ / utf8_path(file), bytes, written, error)) return {};
                break;
            }
            std::vector<u8> was;
            if (read_file(dir / utf8_path(*there), was) && was == bytes) {
                file = std::string(folder) + "/" + *there;
                break;
            }
        }
        file = std::string(folder) + "/" + stem + " " + std::to_string(n) + ext;
    }
    entries_.push_back({file, asset.id, asset.rel, hash});
    return commit(file);
}

Report Sources::sync(const std::function<std::optional<Asset>(const Guid&)>& find) {
    Report r;
    bool dirty = false;
    std::error_code ec;
    const std::vector<Entry> before = entries_;
    std::vector<Written> written;
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
        if (a->rel != e.from) {
            e.from = a->rel; // renamed or moved in «Ресурсы»: the same asset
            dirty = true;
        }
        const std::string hash = hash_of(bytes);
        if (there && hash == e.hash) continue;
        if (std::string why; !fits(*a, folder_of(e.file), bytes, hash, &why)) {
            r.refused.push_back({e, why, !there});
            continue;
        }
        if (std::string error; !write_copy(copy, bytes, written, &error)) {
            r.errors.push_back(error);
            continue;
        }
        e.hash = hash;
        dirty = true;
        (there ? r.updated : r.restored).push_back(e);
    }
    if (std::string error; dirty && !save(&error)) {
        // Nothing of it done: the copies as they were, the entries as they were, said once more at the next look.
        entries_ = before;
        const std::string left = put_back(written);
        std::string files;
        for (const auto* done : {&r.updated, &r.restored})
            for (const Entry& e : *done) files += (files.empty() ? "" : ", ") + e.file;
        r.errors.push_back(error + ": из «Ресурсов» ничего не перенесено" + (files.empty() ? "" : " (" + files + ")") +
                           (left.empty() ? ", файлы игры как были" : "; не вернулись как были: " + left));
        r.updated.clear();
        r.restored.clear();
    }
    return r;
}

} // namespace forge::editor::sources
