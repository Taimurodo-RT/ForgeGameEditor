#include "forge/editor/sources.h"

#include "forge/assets/content_hash.h"
#include "forge/assets/image.h"
#include "forge/audio/audio.h"
#include "forge/core/file.h"
#include "forge/core/path.h"

#include <SDL3/SDL_stdinc.h>
#include <yyjson.h>

#include <algorithm>
#include <cstdlib>
#include <map>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

// The file of a folder that is this name where case does not count (same_name), if any: its own name.
std::optional<std::string> on_disk(const fs::path& folder, std::string_view name) {
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string there = path_to_utf8(it->path().filename());
        if (same_name(there, name)) return there;
    }
    return std::nullopt;
}

// An entry whose copy is this very file of the disk: the file system sees one file under both names (where case
// does not count, as it counts there). Never `skip`.
const Entry* sharing(const fs::path& game, const std::vector<Entry>& entries, const fs::path& file, const Entry* skip = nullptr) {
    for (const Entry& o : entries) {
        std::error_code ec;
        if (&o != skip && fs::equivalent(game / utf8_path(o.file), file, ec) && !ec) return &o;
    }
    return nullptr;
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

bool same_name(std::string_view a, std::string_view b) {
    const std::string x(a), y(b);
    // Unicode case folding (CaseFolding.txt, as SDL has it): Łódź and łódź, Ґрунт and ґрунт, Straße and STRASSE.
    if (SDL_strcasecmp(x.c_str(), y.c_str()) == 0) return true;
#ifdef _WIN32
    // And what this Windows itself takes for one name (its own table of capitals).
    const std::wstring wx = utf8_path(x).wstring(), wy = utf8_path(y).wstring();
    if (CompareStringOrdinal(wx.c_str(), static_cast<int>(wx.size()), wy.c_str(), static_cast<int>(wy.size()), TRUE) == CSTR_EQUAL)
        return true;
#endif
    return false;
}

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
    std::map<std::uintmax_t, std::vector<usize>> by_size; // the entries whose copies are there, by their size
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
        // One file, one asset: the first one named. One file is one name where case does not count, or two names
        // the file system here takes for one file.
        bool one = of_file(e.file) != nullptr;
        const fs::path path = game_dir / utf8_path(e.file);
        std::error_code sec;
        const std::uintmax_t size = fs::file_size(path, sec);
        if (!one && !sec) {
            std::vector<usize>& alike = by_size[size];
            for (const usize k : alike) one = one || (fs::equivalent(game_dir / utf8_path(entries_[k].file), path, sec) && !sec);
            if (!one) alike.push_back(entries_.size());
        }
        if (one) {
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
    auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return same_name(e.file, file); });
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
        if (e.asset != asset.id || !same_name(folder_of(e.file), folder)) continue;
        const std::string file = e.file;
        const fs::path path = game_ / utf8_path(file);
        std::vector<u8> there;
        if (!read_file(path, there) || there != bytes) {
            if (const Entry* o = sharing(game_, entries_, path, &e)) {
                if (error) *error = e.file + " и " + o->file + " здесь один файл: ресурс «" + asset.rel + "» не перенесён, чтобы не записать его поверх копии другого";
                return {};
            }
            if (!write_copy(path, bytes, written, error)) return {};
        }
        e.from = asset.rel;
        e.hash = hash;
        return commit(file);
    }
    // A new one under its own name. A name another asset's copy has is not free, with its file there or not, nor is
    // a file's of the folder: one name where case does not count (same_name) is one file. A file of that name with
    // the same content and no asset is taken as it is. A name the file system here has a file for is never written
    // over, even where same_name did not see it (another form of the letters).
    const fs::path name = asset.file.filename();
    const std::string stem = path_to_utf8(name.stem()), ext = path_to_utf8(name.extension());
    const fs::path dir = game_ / utf8_path(folder);
    std::string file = std::string(folder) + "/" + path_to_utf8(name);
    for (int n = 2;; ++n) {
        if (!of_file(file)) {
            const fs::path path = game_ / utf8_path(file);
            if (const std::optional<std::string> there = on_disk(dir, name_of(file))) {
                std::vector<u8> was;
                const fs::path found = dir / utf8_path(*there);
                if (read_file(found, was) && was == bytes && !sharing(game_, entries_, found)) {
                    file = std::string(folder) + "/" + *there;
                    break;
                }
            } else if (const bool taken = fs::exists(path, ec); ec) {
                if (error) *error = "папка игры " + path_to_utf8(dir) + " не читается";
                return {};
            } else if (!taken) {
                if (!write_copy(path, bytes, written, error)) return {};
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
        if (const Entry* o = there ? sharing(game_, entries_, copy, &e) : nullptr) {
            r.errors.push_back(e.file + " и " + o->file + " здесь один файл: ресурс «" + a->rel +
                               "» не перенесён, чтобы не записать его поверх копии другого");
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
