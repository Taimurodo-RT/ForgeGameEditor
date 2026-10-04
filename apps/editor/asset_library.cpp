#include "asset_library.h"

#include "forge/assets/image.h"
#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/core/time.h"
#include "forge/data/binary.h"
#include "forge/data/json.h"

#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Input.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace forge::editor_app {

namespace fs = std::filesystem;

namespace {

constexpr u32 kThumbPx = 40;
constexpr u32 kPreviewPx = 256;
constexpr usize kThumbsKept = 2000;
constexpr usize kThumbQueue = 256;
constexpr u32 kSearchLimit = 5000;

const DockConfig::Panel kPanels[] = {
    {"folders", "Папки", "folder"},
    {"preview", "Просмотр", "image"},
    {"history", "История", "history"},
    {"log", "Журнал", "terminal"},
};
const char* kDefaultLayout =
    R"({"row":0.18,"a":{"panels":["folders"]},"b":{"row":0.7,"a":{"column":0.78,"a":{"view":true},"b":{"panels":["log","history"]}},"b":{"panels":["preview"]}}})";

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// '/' separators, as in the index.
std::string slashes(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

std::string parent_of(const std::string& rel) {
    const usize slash = rel.rfind('/');
    return slash == std::string::npos ? std::string() : rel.substr(0, slash);
}

std::string name_of(const std::string& rel) {
    const usize slash = rel.rfind('/');
    return slash == std::string::npos ? rel : rel.substr(slash + 1);
}

std::string extension_of(const std::string& rel) {
    const std::string name = name_of(rel);
    const usize dot = name.rfind('.');
    return dot == std::string::npos || dot == 0 ? std::string() : lower(name.substr(dot));
}

// Folders before their contents, "a/b" right after "a" (the separator sorts first).
bool path_less(const std::string& a, const std::string& b) {
    const usize n = std::min(a.size(), b.size());
    for (usize i = 0; i < n; ++i) {
        if (a[i] == b[i]) continue;
        if (a[i] == '/') return true;
        if (b[i] == '/') return false;
        const char x = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
        const char y = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
        if (x != y) return static_cast<unsigned char>(x) < static_cast<unsigned char>(y);
        return static_cast<unsigned char>(a[i]) < static_cast<unsigned char>(b[i]);
    }
    return a.size() < b.size();
}

const char* kind_name(const std::string& type, const std::string& ext) {
    if (type == "image") return "Картинка";
    if (type == "audio") return "Звук";
    if (ext == ".json") return "Данные";
    if (ext == ".txt" || ext == ".md") return "Текст";
    if (ext == ".ttf" || ext == ".otf") return "Шрифт";
    if (ext == ".rml" || ext == ".rcss") return "Интерфейс";
    if (ext == ".lua" || ext == ".luau") return "Скрипт";
    return "Файл";
}

const char* kind_icon(const std::string& type, const std::string& ext) {
    if (type == "image") return "image";
    if (type == "audio") return "audio_file";
    if (ext == ".json") return "data_object";
    if (ext == ".txt" || ext == ".md") return "description";
    if (ext == ".ttf" || ext == ".otf") return "text_fields";
    return "draft";
}

std::string size_text(u64 bytes) {
    char text[32];
    if (bytes < 1024) std::snprintf(text, sizeof(text), "%llu Б", static_cast<unsigned long long>(bytes));
    else if (bytes < 1024 * 1024) std::snprintf(text, sizeof(text), "%.0f КБ", static_cast<f64>(bytes) / 1024.0);
    else std::snprintf(text, sizeof(text), "%.1f МБ", static_cast<f64>(bytes) / (1024.0 * 1024.0));
    std::string s = text;
    std::replace(s.begin(), s.end(), '.', ',');
    return s;
}

std::string files_text(int n) {
    const int mod10 = n % 10, mod100 = n % 100;
    const char* word = (mod10 == 1 && mod100 != 11)                                 ? "файл"
                       : (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14)) ? "файла"
                                                                                     : "файлов";
    return std::to_string(n) + " " + word;
}

// Moves a file or a folder, its .meta along; across disks by copying.
bool move_path(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    const bool is_file = fs::is_regular_file(from, ec);
    fs::rename(from, to, ec);
    if (ec) {
        ec.clear();
        fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
        if (ec) {
            FORGE_ERROR("Не удалось переместить %s: %s", path_to_utf8(from).c_str(), ec.message().c_str());
            return false;
        }
        fs::remove_all(from, ec);
    }
    if (is_file) {
        fs::path meta_from = from, meta_to = to;
        meta_from += ".meta";
        meta_to += ".meta";
        if (fs::exists(meta_from, ec)) fs::rename(meta_from, meta_to, ec);
    }
    return true;
}

bool valid_name(const std::string& name) {
    if (name.empty() || name == "." || name == ".." || name[0] == '.') return false;
    for (char c : name)
        if (std::strchr("/\\:*?\"<>|", c) || static_cast<unsigned char>(c) < 32) return false;
    return name.back() != ' ' && name.back() != '.';
}

// Duration of a WAV file from its header; 0 when unknown.
f64 wav_seconds(const fs::path& file) {
    std::vector<u8> bytes;
    if (!read_file(file, bytes) || bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0) return 0;
    auto u32_at = [&](usize i) { return static_cast<u32>(bytes[i] | bytes[i + 1] << 8 | bytes[i + 2] << 16 | bytes[i + 3] << 24); };
    u32 byte_rate = 0;
    for (usize at = 12; at + 8 <= bytes.size();) {
        const u32 size = u32_at(at + 4);
        if (std::memcmp(&bytes[at], "fmt ", 4) == 0 && at + 20 <= bytes.size()) byte_rate = u32_at(at + 16);
        if (std::memcmp(&bytes[at], "data", 4) == 0) return byte_rate ? static_cast<f64>(size) / byte_rate : 0;
        at += 8 + size + (size & 1);
    }
    return 0;
}

} // namespace

// --- commands ----------------------------------------------------------------

FileMoves::FileMoves(AssetLibrary& library, std::vector<Move> moves, std::string label)
    : library_(library), moves_(std::move(moves)), label_(std::move(label)) {}

void FileMoves::apply(editor::Document&) {
    for (const Move& m : moves_) move_path(m.from, m.to);
    library_.changed();
}

void FileMoves::revert(editor::Document&) {
    for (auto it = moves_.rbegin(); it != moves_.rend(); ++it) move_path(it->to, it->from);
    library_.changed();
}

FileContents::FileContents(AssetLibrary& library, fs::path file, std::vector<u8> before, std::vector<u8> after,
                           std::string label)
    : library_(library), file_(std::move(file)), before_(std::move(before)), after_(std::move(after)),
      label_(std::move(label)) {}

void FileContents::apply(editor::Document&) {
    if (!write_file_atomic(file_, after_)) FORGE_ERROR("Не удалось записать %s", path_to_utf8(file_).c_str());
    library_.changed();
}

void FileContents::revert(editor::Document&) {
    if (!write_file_atomic(file_, before_)) FORGE_ERROR("Не удалось записать %s", path_to_utf8(file_).c_str());
    library_.changed();
}

// --- setup -------------------------------------------------------------------

AssetLibrary::AssetLibrary() = default;

AssetLibrary::~AssetLibrary() { shutdown(); }

bool AssetLibrary::init(ui::Ui& ui, const AssetsConfig& config) {
    ui_ = &ui;
    config_ = config;
    std::error_code ec;
    fs::create_directories(config_.folder, ec);
    fs::create_directories(config_.library, ec);
    // Deleted files wait in the trash until the next start.
    trash_ = config_.library / "trash";
    staging_ = config_.library / "staging";
    fs::remove_all(trash_, ec);
    fs::remove_all(staging_, ec);

    DockConfig dc;
    dc.prefix = "as_";
    dc.area = "as-dock";
    dc.view = "as-view";
    dc.pane_prefix = "as-pane-";
    for (const auto& p : kPanels) dc.panels.push_back(p);
    dc.default_layout = kDefaultLayout;
    dc.file = "assets_layout.json";
    dock_.init(std::move(dc), config.settings, !config.offscreen);

    ui::register_list_source("assets", this);
    stop_ = false;
    refresh_wanted_ = true; // the first look
    refresher_ = std::thread([this] { refresher_main(); });
    thumbnailer_ = std::thread([this] { thumbnailer_main(); });
    FORGE_INFO("Ресурсы проекта: %s", path_to_utf8(config_.folder).c_str());
    return true;
}

void AssetLibrary::shutdown() {
    {
        std::lock_guard lock(mutex_);
        if (!refresher_.joinable() && !thumbnailer_.joinable() && !ui_) return;
        stop_ = true;
    }
    refresh_cv_.notify_all();
    thumb_cv_.notify_all();
    if (refresher_.joinable()) refresher_.join();
    if (thumbnailer_.joinable()) thumbnailer_.join();
    if (sound_) {
        SDL_DestroyAudioStream(sound_);
        sound_ = nullptr;
    }
    if (ui_) ui::register_list_source("assets", nullptr);
    search_db_.close();
    ui_ = nullptr;
}

void AssetLibrary::bind(Rml::DataModelConstructor& model) {
    dock_.bind(model);
    if (auto s = model.RegisterStruct<TreeRow>()) {
        s.RegisterMember("path", &TreeRow::path);
        s.RegisterMember("name", &TreeRow::name);
        s.RegisterMember("depth", &TreeRow::depth);
        s.RegisterMember("count", &TreeRow::count);
        s.RegisterMember("open", &TreeRow::open);
        s.RegisterMember("kids", &TreeRow::kids);
        s.RegisterMember("selected", &TreeRow::selected);
    }
    model.RegisterArray<std::vector<TreeRow>>();
    if (auto s = model.RegisterStruct<Crumb>()) {
        s.RegisterMember("name", &Crumb::name);
        s.RegisterMember("path", &Crumb::path);
    }
    model.RegisterArray<std::vector<Crumb>>();
    model.Bind("as_tree", &m_tree_);
    model.Bind("as_crumbs", &m_crumbs_);
    model.Bind("as_filter", &m_filter_);
    model.Bind("as_search", &m_search_);
    model.Bind("as_status", &m_status_);
    model.Bind("as_where", &m_where_);
    model.Bind("as_busy", &m_busy_);
    model.Bind("as_searching", &m_searching_);
    model.Bind("as_has_cut", &m_has_cut_);
    model.Bind("as_empty", &m_empty_);
    model.Bind("as_sel_count", &m_sel_count_);
    model.Bind("as_sel_name", &m_sel_name_);
    model.Bind("as_sel_ext", &m_sel_ext_);
    model.Bind("as_sel_kind", &m_sel_kind_);
    model.Bind("as_sel_size", &m_sel_size_);
    model.Bind("as_sel_dims", &m_sel_dims_);
    model.Bind("as_sel_path", &m_sel_path_);
    model.Bind("as_sel_tags", &m_sel_tags_);
    model.Bind("as_sel_preview", &m_sel_preview_);
    model.Bind("as_sel_icon", &m_sel_icon_);
    model.Bind("as_sel_extra", &m_sel_extra_);
    model.Bind("as_sel_image", &m_sel_image_);
    model.Bind("as_sel_audio", &m_sel_audio_);
    model.Bind("as_sel_wav", &m_sel_wav_);
    model.Bind("as_sel_dir", &m_sel_dir_);
    model.Bind("as_sel_editable", &m_sel_editable_);
    model.Bind("as_history", &m_history_);
    model.Bind("as_history_cursor", &m_history_cursor_);

    auto on = [&](const char* name, auto fn) {
        model.BindEventCallback(name, [fn](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& args) { fn(ev, args); });
    };
    auto arg_str = [](const Rml::VariantList& a, usize i) { return i < a.size() ? a[i].Get<Rml::String>() : Rml::String(); };
    auto arg_bool = [](const Rml::VariantList& a, usize i) { return i < a.size() && a[i].Get<bool>(); };
    auto input_value = [](Rml::Event& ev) {
        Rml::Element* e = ev.GetTargetElement();
        return e && e->GetTagName() == "input" ? static_cast<Rml::ElementFormControl*>(e)->GetValue() : Rml::String();
    };

    on("as_go", [this, arg_str](Rml::Event&, const Rml::VariantList& a) { open_folder(arg_str(a, 0)); });
    on("as_tree_toggle", [this, arg_str](Rml::Event& ev, const Rml::VariantList& a) {
        const std::string path = arg_str(a, 0);
        if (!open_dirs_.erase(path)) open_dirs_.insert(path);
        rebuild_tree();
        ev.StopPropagation(); // not also a click on the row
    });
    on("as_filter", [this, arg_str](Rml::Event&, const Rml::VariantList& a) { set_filter(arg_str(a, 0)); });
    on("as_search", [this, arg_str](Rml::Event&, const Rml::VariantList& a) {
        // Typing: the search runs when the keys pause (a search over 50 000
        // files takes tens of milliseconds).
        const std::string text = arg_str(a, 0);
        if (text == search_) {
            search_pending_ = false;
            return;
        }
        search_text_ = text;
        search_at_ = time_ + 0.15;
        search_pending_ = true;
    });
    on("as_search_clear", [this](Rml::Event&, const Rml::VariantList&) { set_search(""); });
    on("as_new_folder", [this](Rml::Event&, const Rml::VariantList&) { new_folder(); });
    on("as_import", [this, arg_str](Rml::Event&, const Rml::VariantList& a) {
        if (config_.offscreen) return;
        auto done = [](void* self, const char* const* list, int) {
            auto* lib = static_cast<AssetLibrary*>(self);
            if (!list) return;
            std::lock_guard lock(lib->mutex_);
            for (; *list; ++list) lib->dialog_files_.push_back(utf8_path(*list));
        };
        const std::string where = path_to_utf8(abs(folder_));
        if (arg_str(a, 0) == "folder") SDL_ShowOpenFolderDialog(done, this, config_.window, where.c_str(), true);
        else SDL_ShowOpenFileDialog(done, this, config_.window, nullptr, 0, where.c_str(), true);
    });
    on("as_cut", [this](Rml::Event&, const Rml::VariantList&) { cut(); });
    on("as_paste", [this](Rml::Event&, const Rml::VariantList&) { paste(); });
    on("as_delete", [this](Rml::Event&, const Rml::VariantList&) { delete_selection(); });
    on("as_refresh", [this](Rml::Event&, const Rml::VariantList&) { refresh(); });
    on("as_rename", [this, arg_str, arg_bool](Rml::Event&, const Rml::VariantList& a) {
        if (!ui_updating_ && arg_bool(a, 1)) rename_selected(arg_str(a, 0));
    });
    on("as_rename_commit", [this, input_value](Rml::Event& ev, const Rml::VariantList&) {
        const std::string value = input_value(ev);
        if (!value.empty() && value != m_sel_name_) rename_selected(value);
    });
    on("as_tags", [this, arg_str, arg_bool](Rml::Event&, const Rml::VariantList& a) {
        if (!ui_updating_ && arg_bool(a, 1)) set_tags(arg_str(a, 0));
    });
    on("as_tags_commit", [this, input_value](Rml::Event& ev, const Rml::VariantList&) {
        const std::string value = input_value(ev);
        if (value != m_sel_tags_) set_tags(value);
    });
    on("as_image", [this, arg_str](Rml::Event&, const Rml::VariantList& a) { edit_image(arg_str(a, 0)); });
    on("as_convert", [this, arg_str](Rml::Event&, const Rml::VariantList& a) { convert_image(arg_str(a, 0)); });
    on("as_play", [this](Rml::Event&, const Rml::VariantList&) { play_sound(); });
    on("as_show", [this](Rml::Event&, const Rml::VariantList&) {
        if (config_.offscreen) return;
        const std::string rel = selection_.size() == 1 ? parent_of(selection_[0]) : folder_;
        std::string url = "file:///" + slashes(path_to_utf8(fs::absolute(abs(rel))));
        SDL_OpenURL(url.c_str());
    });
    on("as_history_jump", [this](Rml::Event&, const Rml::VariantList& a) {
        const int target = a.empty() ? -1 : a[0].Get<int>() + 1;
        if (target < 0) return;
        while (static_cast<int>(history_.cursor()) > target && history_.undo()) {}
        while (static_cast<int>(history_.cursor()) < target && history_.redo()) {}
    });
}

void AssetLibrary::set_model(Rml::DataModelHandle handle) {
    model_ = handle;
    dock_.set_model(handle);
}

// --- background --------------------------------------------------------------

void AssetLibrary::refresher_main() {
    assets::AssetPipeline pipeline(config_.folder, config_.library);
    std::string error;
    const bool ok = pipeline.open(&error);
    if (!ok) {
        FORGE_ERROR("Индекс ресурсов не открылся: %s", error.c_str());
        pipeline_failed_ = true;
    }
    pipeline.add_default_importers();
    std::unique_lock lock(mutex_);
    while (!stop_) {
        refresh_cv_.wait(lock, [&] { return stop_ || refresh_wanted_ || !imports_.empty(); });
        if (stop_) break;
        if (!imports_.empty()) {
            ImportJob job = std::move(imports_.front());
            imports_.pop_front();
            lock.unlock();
            Staged staged;
            staged.folder = job.folder;
            std::error_code ec;
            fs::create_directories(job.staging, ec);
            for (const fs::path& src : job.sources) {
                const fs::path to = job.staging / src.filename();
                ec.clear();
                if (fs::is_directory(src, ec)) fs::copy(src, to, fs::copy_options::recursive, ec);
                else fs::copy_file(src, to, fs::copy_options::overwrite_existing, ec);
                if (ec) staged.errors.push_back(path_to_utf8(src.filename()) + ": " + ec.message());
                else staged.items.push_back(to);
            }
            lock.lock();
            staged_.push_back(std::move(staged));
            continue;
        }
        refresh_wanted_ = false;
        refreshing_ = true;
        lock.unlock();
        Snapshot snap;
        if (ok) {
            snap.report = pipeline.refresh();
            snap.records = pipeline.database().all();
        }
        // Folders, the empty ones too.
        std::error_code ec;
        const usize root_len = config_.folder.native().size() + 1;
        fs::recursive_directory_iterator it(config_.folder, fs::directory_options::skip_permission_denied, ec), end;
        for (; it != end; it.increment(ec)) {
            if (ec) break;
            const std::string name = path_to_utf8(it->path().filename());
            if (!it->is_directory(ec)) continue;
            if (!name.empty() && name[0] == '.') {
                it.disable_recursion_pending();
                continue;
            }
            const auto& native = it->path().native();
            if (native.size() > root_len) snap.dirs.push_back(slashes(path_to_utf8(fs::path(native.substr(root_len)))));
        }
        lock.lock();
        snapshots_.push_back(std::move(snap));
        refreshing_ = false;
    }
}

void AssetLibrary::thumbnailer_main() {
    std::unique_lock lock(mutex_);
    while (!stop_) {
        thumb_cv_.wait(lock, [&] { return stop_ || !thumb_jobs_.empty(); });
        if (stop_) break;
        // The newest first: what is on screen now.
        ThumbJob job = std::move(thumb_jobs_.back());
        thumb_jobs_.pop_back();
        lock.unlock();
        ThumbDone done;
        done.name = job.name;
        done.box = job.box;
        std::vector<u8> bytes;
        assets::CookedTexture tex;
        if (read_file(assets::AssetPipeline::cooked_file(config_.library, job.key), bytes) &&
            data::from_binary(tex, bytes) == data::BinaryError::None && tex.width > 0) {
            done.src_w = tex.width;
            done.src_h = tex.height;
            done.rgba = assets::fit_image(tex, job.box).rgba8;
            done.ok = true;
        }
        lock.lock();
        thumbs_done_.push_back(std::move(done));
    }
}

bool AssetLibrary::busy() const {
    std::lock_guard lock(mutex_);
    return refresh_wanted_ || refreshing_ || !imports_.empty() || !snapshots_.empty() || !staged_.empty();
}

void AssetLibrary::want_thumb(const std::string& name, const assets::ContentHash& key, u32 box) const {
    if (asked_.count(name)) return;
    asked_.insert(name);
    {
        std::lock_guard lock(mutex_);
        thumb_jobs_.push_back({name, key, box});
        if (thumb_jobs_.size() > kThumbQueue) {
            asked_.erase(thumb_jobs_.front().name);
            thumb_jobs_.pop_front();
        }
    }
    thumb_cv_.notify_one();
}

void AssetLibrary::refresh() {
    {
        std::lock_guard lock(mutex_);
        refresh_wanted_ = true;
    }
    refresh_cv_.notify_one();
}

void AssetLibrary::changed() { refresh(); }

void AssetLibrary::opened() {
    open_ = true;
    refresh();
}

fs::path AssetLibrary::staging_dir() { return staging_ / std::to_string(++serial_); }
fs::path AssetLibrary::trash_dir() { return trash_ / std::to_string(++serial_); }

fs::path AssetLibrary::abs(const std::string& rel) const {
    return rel.empty() ? config_.folder : config_.folder / utf8_path(rel);
}

// --- results -----------------------------------------------------------------

void AssetLibrary::take_results() {
    std::vector<Snapshot> snaps;
    std::vector<Staged> staged;
    std::vector<ThumbDone> thumbs;
    std::vector<fs::path> files;
    {
        std::lock_guard lock(mutex_);
        snaps.swap(snapshots_);
        staged.swap(staged_);
        thumbs.swap(thumbs_done_);
        files.swap(dialog_files_);
    }
    if (!snaps.empty()) {
        Snapshot& s = snaps.back();
        records_ = std::move(s.records);
        std::sort(records_.begin(), records_.end(),
                  [](const assets::AssetRecord& a, const assets::AssetRecord& b) { return path_less(a.path, b.path); });
        by_path_.clear();
        by_path_.reserve(records_.size());
        folder_counts_.clear();
        for (usize i = 0; i < records_.size(); ++i) {
            by_path_.emplace(records_[i].path, i);
            ++folder_counts_[parent_of(records_[i].path)];
        }
        dirs_ = std::move(s.dirs);
        std::sort(dirs_.begin(), dirs_.end(), path_less);
        last_report_ = std::move(s.report);
        for (const std::string& m : last_report_.messages) FORGE_WARN("%s", m.c_str());
        if (last_report_.added + last_report_.cooked + last_report_.removed + last_report_.moved > 0)
            FORGE_INFO("Ресурсы обновлены: новых %u, пересобрано %u, перемещено %u, удалено %u (%.0f мс)", last_report_.added,
                       last_report_.cooked, last_report_.moved, last_report_.removed, last_report_.ms);
        if (!have_index_ || !search_db_.is_open()) {
            std::string error;
            if (!pipeline_failed_ && !search_db_.open(assets::AssetPipeline::database_file(config_.library), &error))
                FORGE_WARN("Поиск по ресурсам недоступен: %s", error.c_str());
        }
        have_index_ = true;
        // The open folder may be gone (deleted, renamed outside).
        while (!folder_.empty() && !std::binary_search(dirs_.begin(), dirs_.end(), folder_, path_less)) folder_ = parent_of(folder_);
        std::vector<std::string> kept;
        for (const std::string& rel : selection_)
            if (by_path_.count(rel) || std::binary_search(dirs_.begin(), dirs_.end(), rel, path_less)) kept.push_back(rel);
        if (kept != selection_) {
            selection_ = std::move(kept);
            ++selection_version_;
        }
        rebuild_rows();
        rebuild_tree();
        ++selection_version_; // details (size, tags) may have changed
    }
    for (Staged& s : staged) {
        for (const std::string& e : s.errors) FORGE_WARN("Не импортирован %s", e.c_str());
        if (s.items.empty()) continue;
        const fs::path dest = abs(s.folder);
        std::vector<FileMoves::Move> moves;
        std::vector<std::string> rels;
        for (const fs::path& item : s.items) {
            const std::string name = unique_name(dest, path_to_utf8(item.filename()));
            moves.push_back({item, dest / utf8_path(name)});
            rels.push_back(s.folder.empty() ? name : s.folder + "/" + name);
        }
        const std::string label =
            moves.size() == 1 ? "Импорт: " + name_of(rels[0]) : "Импорт: " + files_text(static_cast<int>(moves.size()));
        history_.execute(std::make_unique<FileMoves>(*this, std::move(moves), label));
        history_.seal();
        FORGE_INFO("%s", label.c_str());
        select(rels);
    }
    bool list_changed = false;
    for (ThumbDone& t : thumbs) {
        asked_.erase(t.name);
        if (!t.ok) continue;
        ui_->set_image(t.name, t.rgba.data(), t.box, t.box);
        thumbs_[t.name] = {t.name, frame_};
        const usize under = t.name.find('_');
        if (under != std::string::npos) dims_[t.name.substr(under + 1)] = {t.src_w, t.src_h};
        ++thumbs_made_;
        if (t.name.rfind("pv_", 0) == 0) {
            if (t.name == preview_wanted_) {
                if (!preview_name_.empty() && preview_name_ != t.name) {
                    ui_->drop_image(preview_name_);
                    thumbs_.erase(preview_name_);
                }
                preview_name_ = t.name;
                preview_w_ = t.src_w;
                preview_h_ = t.src_h;
                ++preview_version_;
            } else {
                ui_->drop_image(t.name); // came too late
                thumbs_.erase(t.name);
            }
        } else {
            list_changed = true;
        }
    }
    if (list_changed) ++list_version_;
    if (thumbs_.size() > kThumbsKept) {
        std::vector<const Thumb*> all;
        for (const auto& [name, t] : thumbs_)
            if (name != preview_name_) all.push_back(&t);
        std::sort(all.begin(), all.end(), [](const Thumb* a, const Thumb* b) { return a->used < b->used; });
        std::vector<std::string> drop;
        for (usize i = 0; i + kThumbsKept / 2 < all.size(); ++i) drop.push_back(all[i]->name);
        for (const std::string& name : drop) {
            ui_->drop_image(name);
            thumbs_.erase(name);
        }
        ++list_version_;
    }
    if (!files.empty()) import(files);
}

// --- what is shown -----------------------------------------------------------

const assets::AssetRecord* AssetLibrary::record(const std::string& rel) const {
    auto it = by_path_.find(rel);
    return it == by_path_.end() ? nullptr : &records_[it->second];
}

void AssetLibrary::rebuild_rows() {
    rows_.clear();
    if (!search_.empty()) {
        if (search_db_.is_open())
            for (const assets::SearchHit& hit : search_db_.search(search_, kSearchLimit, filter_)) {
                auto it = by_path_.find(hit.path);
                if (it != by_path_.end()) rows_.push_back({false, hit.path, name_of(hit.path), static_cast<i32>(it->second)});
            }
    } else {
        for (const std::string& d : dirs_)
            if (parent_of(d) == folder_) rows_.push_back({true, d, name_of(d), -1});
        for (usize i = 0; i < records_.size(); ++i) {
            const assets::AssetRecord& r = records_[i];
            if (!filter_.empty() && r.type != filter_) continue;
            if (parent_of(r.path) == folder_) rows_.push_back({false, r.path, name_of(r.path), static_cast<i32>(i)});
        }
    }
    ++list_version_;
    ++rows_version_;
}

void AssetLibrary::rebuild_tree() {
    m_tree_.clear();
    m_tree_.push_back({"", "Ресурсы", 0, folder_counts_.count("") ? folder_counts_[""] : 0, true, !dirs_.empty(), folder_.empty()});
    for (const std::string& d : dirs_) {
        // Shown when every folder above it is open.
        bool shown = true;
        for (std::string p = parent_of(d); !p.empty(); p = parent_of(p))
            if (!open_dirs_.count(p)) {
                shown = false;
                break;
            }
        if (!shown) continue;
        const int depth = static_cast<int>(std::count(d.begin(), d.end(), '/')) + 1;
        auto next = std::upper_bound(dirs_.begin(), dirs_.end(), d, path_less);
        const bool kids = next != dirs_.end() && next->size() > d.size() && next->compare(0, d.size(), d) == 0 && (*next)[d.size()] == '/';
        auto count = folder_counts_.find(d);
        m_tree_.push_back({d, name_of(d), depth, count == folder_counts_.end() ? 0 : count->second, open_dirs_.count(d) > 0,
                           kids, d == folder_});
    }
    model_.DirtyVariable("as_tree");
}

std::string AssetLibrary::field(u32 row, std::string_view name) const {
    if (row >= rows_.size()) return {};
    const Row& r = rows_[row];
    const assets::AssetRecord* rec = r.record >= 0 ? &records_[static_cast<usize>(r.record)] : nullptr;
    auto thumb = [&]() -> std::string {
        if (!rec || rec->type != "image") return {};
        const std::string key = "th_" + rec->cook_key.to_hex();
        auto it = thumbs_.find(key);
        if (it != thumbs_.end()) {
            it->second.used = frame_;
            return "/memory/" + key;
        }
        want_thumb(key, rec->cook_key, kThumbPx);
        return {};
    };
    if (name == "name") return r.name;
    if (name == "kind") return r.dir ? "Папка" : rec ? kind_name(rec->type, extension_of(r.rel)) : "";
    if (name == "size") {
        if (r.dir) {
            auto it = folder_counts_.find(r.rel);
            return files_text(it == folder_counts_.end() ? 0 : it->second);
        }
        return rec ? size_text(rec->size) : "";
    }
    if (name == "where") return search_.empty() ? std::string() : (parent_of(r.rel).empty() ? "Ресурсы" : parent_of(r.rel));
    if (name == "icon") return r.dir ? "folder" : rec ? kind_icon(rec->type, extension_of(r.rel)) : "draft";
    if (name == "thumb") return thumb();
    if (name == "img") return thumb().empty() ? "0" : "1";
    if (name == "noimg") return thumb().empty() ? "1" : "0";
    if (name == "selected") return std::find(selection_.begin(), selection_.end(), r.rel) != selection_.end() ? "1" : "0";
    if (name == "cut") return std::find(cut_.begin(), cut_.end(), r.rel) != cut_.end() ? "1" : "0";
    return {};
}

bool AssetLibrary::row_has_thumb(usize i) const { return i < rows_.size() && field(static_cast<u32>(i), "img") == "1"; }

void AssetLibrary::on_row_event(u32 row, std::string_view event, int modifiers) {
    if (row >= rows_.size()) return;
    const std::string rel = rows_[row].rel;
    if (event == "dblclick") {
        if (rows_[row].dir) open_folder(rel);
        else if (const auto* rec = record(rel); rec && rec->type == "audio") play_sound();
        return;
    }
    if (event != "click") return;
    if (modifiers & Rml::Input::KM_SHIFT) {
        auto find_row = [&](const std::string& r) {
            for (usize i = 0; i < rows_.size(); ++i)
                if (rows_[i].rel == r) return static_cast<i64>(i);
            return i64(-1);
        };
        const i64 a = find_row(anchor_);
        if (a >= 0) {
            std::vector<std::string> range;
            for (i64 i = std::min<i64>(a, row); i <= std::max<i64>(a, row); ++i) range.push_back(rows_[static_cast<usize>(i)].rel);
            const std::string keep = anchor_;
            select(range);
            anchor_ = keep;
            return;
        }
    }
    if (modifiers & Rml::Input::KM_CTRL) {
        std::vector<std::string> s = selection_;
        auto it = std::find(s.begin(), s.end(), rel);
        if (it != s.end()) s.erase(it);
        else s.push_back(rel);
        select(s);
        anchor_ = rel;
        return;
    }
    select({rel});
}

// --- actions -----------------------------------------------------------------

void AssetLibrary::open_folder(const std::string& rel) {
    std::string target = rel;
    if (!target.empty() && !std::binary_search(dirs_.begin(), dirs_.end(), target, path_less)) return;
    folder_ = target;
    for (std::string p = parent_of(target); !p.empty(); p = parent_of(p)) open_dirs_.insert(p);
    if (!search_.empty()) {
        search_.clear();
        set(m_search_, Rml::String(), "as_search");
    }
    select({});
    rebuild_rows();
    rebuild_tree();
}

void AssetLibrary::set_search(const std::string& text) {
    search_pending_ = false;
    if (text == search_) return;
    search_ = text;
    set(m_search_, Rml::String(text), "as_search");
    rebuild_rows();
}

void AssetLibrary::set_filter(const std::string& type) {
    filter_ = type == filter_ ? std::string() : type; // a second click shows everything
    rebuild_rows();
}

void AssetLibrary::select(std::vector<std::string> rels) {
    if (rels == selection_) return;
    selection_ = std::move(rels);
    anchor_ = selection_.empty() ? std::string() : selection_.back();
    ++selection_version_;
    ++list_version_;
}

std::string AssetLibrary::unique_name(const fs::path& dir, const std::string& name) const {
    std::error_code ec;
    if (!fs::exists(dir / utf8_path(name), ec)) return name;
    const usize dot = name.rfind('.');
    const std::string stem = dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
    const std::string ext = dot == std::string::npos || dot == 0 ? std::string() : name.substr(dot);
    for (int i = 2;; ++i) {
        const std::string candidate = stem + " (" + std::to_string(i) + ")" + ext;
        if (!fs::exists(dir / utf8_path(candidate), ec)) return candidate;
    }
}

void AssetLibrary::import(const std::vector<fs::path>& sources) {
    if (sources.empty()) return;
    {
        std::lock_guard lock(mutex_);
        imports_.push_back({sources, staging_dir(), folder_});
    }
    refresh_cv_.notify_one();
    FORGE_INFO("Копирую в «%s»: %s", folder_.empty() ? "Ресурсы" : folder_.c_str(), files_text(static_cast<int>(sources.size())).c_str());
}

void AssetLibrary::new_folder() {
    const fs::path staged = staging_dir() / "Новая папка";
    std::error_code ec;
    fs::create_directories(staged, ec);
    const std::string name = unique_name(abs(folder_), "Новая папка");
    const std::string rel = folder_.empty() ? name : folder_ + "/" + name;
    history_.execute(std::make_unique<FileMoves>(*this, std::vector<FileMoves::Move>{{staged, abs(rel)}}, "Новая папка: " + name));
    history_.seal();
    // Shown at once; the index catches up.
    dirs_.insert(std::upper_bound(dirs_.begin(), dirs_.end(), rel, path_less), rel);
    if (!folder_.empty()) open_dirs_.insert(folder_);
    rebuild_rows();
    rebuild_tree();
    select({rel});
}

bool AssetLibrary::rename_selected(const std::string& wanted) {
    if (selection_.size() != 1) return false;
    const std::string rel = selection_[0];
    const bool dir = std::binary_search(dirs_.begin(), dirs_.end(), rel, path_less);
    std::string name = wanted;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    if (!valid_name(name)) {
        FORGE_WARN("Имя «%s» не подходит: нельзя пустое, с точкой в начале и со знаками / \\ : * ? \" < > |", wanted.c_str());
        ++selection_version_; // show the old name again
        return false;
    }
    const std::string ext = dir ? std::string() : extension_of(rel);
    if (!ext.empty() && extension_of(name) != ext) name += name_of(rel).substr(name_of(rel).size() - ext.size());
    if (name == name_of(rel)) return false;
    const std::string parent = parent_of(rel);
    const std::string to_rel = parent.empty() ? name : parent + "/" + name;
    std::error_code ec;
    const bool only_case = lower(name) == lower(name_of(rel));
    if (!only_case && fs::exists(abs(to_rel), ec)) {
        FORGE_WARN("«%s» уже есть в этой папке", name.c_str());
        ++selection_version_;
        return false;
    }
    std::vector<FileMoves::Move> moves;
    if (only_case) {
        // Case-only renames go through a temporary name (Windows ignores them otherwise).
        const fs::path tmp = staging_dir() / "rename";
        moves = {{abs(rel), tmp}, {tmp, abs(to_rel)}};
    } else {
        moves = {{abs(rel), abs(to_rel)}};
    }
    history_.execute(std::make_unique<FileMoves>(*this, std::move(moves), "Переименовать: " + name_of(rel) + " → " + name));
    history_.seal();
    if (dir) {
        std::replace(dirs_.begin(), dirs_.end(), rel, to_rel);
        std::sort(dirs_.begin(), dirs_.end(), path_less);
        if (folder_ == rel) folder_ = to_rel;
    }
    rebuild_rows();
    rebuild_tree();
    selection_ = {to_rel};
    anchor_ = to_rel;
    ++selection_version_;
    return true;
}

void AssetLibrary::delete_selection() {
    if (selection_.empty()) return;
    const fs::path bin = trash_dir();
    std::vector<FileMoves::Move> moves;
    for (usize i = 0; i < selection_.size(); ++i)
        moves.push_back({abs(selection_[i]), bin / std::to_string(i) / utf8_path(name_of(selection_[i]))});
    const std::string label =
        selection_.size() == 1 ? "Удалить: " + name_of(selection_[0]) : "Удалить: " + std::to_string(selection_.size()) + " шт.";
    history_.execute(std::make_unique<FileMoves>(*this, std::move(moves), label));
    history_.seal();
    FORGE_INFO("%s (Ctrl+Z вернёт)", label.c_str());
    select({});
}

void AssetLibrary::cut() {
    cut_ = selection_;
    ++list_version_;
    if (!cut_.empty()) FORGE_INFO("Вырезано: %zu. Откройте папку и нажмите «Вставить» (Ctrl+V)", cut_.size());
}

void AssetLibrary::paste() {
    if (cut_.empty()) return;
    const fs::path dest = abs(folder_);
    std::vector<FileMoves::Move> moves;
    std::vector<std::string> rels;
    for (const std::string& rel : cut_) {
        if (parent_of(rel) == folder_) continue; // already here
        if (folder_ == rel || folder_.rfind(rel + "/", 0) == 0) {
            FORGE_WARN("Папку «%s» нельзя переместить в саму себя", name_of(rel).c_str());
            continue;
        }
        const std::string name = unique_name(dest, name_of(rel));
        moves.push_back({abs(rel), dest / utf8_path(name)});
        rels.push_back(folder_.empty() ? name : folder_ + "/" + name);
    }
    cut_.clear();
    ++list_version_;
    if (moves.empty()) return;
    const std::string label = moves.size() == 1 ? "Переместить: " + name_of(rels[0])
                                                : "Переместить: " + std::to_string(moves.size()) + " шт.";
    history_.execute(std::make_unique<FileMoves>(*this, std::move(moves), label));
    history_.seal();
    select(rels);
}

bool AssetLibrary::edit_image(const std::string& op) {
    if (selection_.size() != 1) return false;
    const assets::AssetRecord* rec = record(selection_[0]);
    const std::string ext = extension_of(selection_[0]);
    if (!rec || rec->type != "image" || !assets::can_encode_image(ext)) return false;
    struct OpInfo {
        const char* id;
        assets::ImageOp op;
        const char* label;
    };
    static const OpInfo ops[] = {{"cw", assets::ImageOp::RotateCw, "Повернуть вправо"},
                                 {"ccw", assets::ImageOp::RotateCcw, "Повернуть влево"},
                                 {"fliph", assets::ImageOp::FlipH, "Отразить по горизонтали"},
                                 {"flipv", assets::ImageOp::FlipV, "Отразить по вертикали"},
                                 {"half", assets::ImageOp::Half, "Уменьшить вдвое"},
                                 {"double", assets::ImageOp::Double, "Увеличить вдвое"}};
    const OpInfo* info = nullptr;
    for (const OpInfo& o : ops)
        if (op == o.id) info = &o;
    if (!info) return false;
    const fs::path file = abs(selection_[0]);
    std::vector<u8> before, after;
    assets::CookedTexture img;
    std::string error;
    if (!read_file(file, before) || !assets::decode_image(before, img, &error)) {
        FORGE_WARN("Картинка не читается: %s", error.c_str());
        return false;
    }
    if (info->op == assets::ImageOp::Double && static_cast<u64>(img.width) * img.height > 4096ull * 4096) {
        FORGE_WARN("Картинка и так большая: %u × %u", img.width, img.height);
        return false;
    }
    if (!assets::encode_image(assets::transform_image(img, info->op), ext, after)) return false;
    history_.execute(std::make_unique<FileContents>(*this, file, std::move(before), std::move(after),
                                                    std::string(info->label) + ": " + name_of(selection_[0])));
    history_.seal();
    return true;
}

bool AssetLibrary::convert_image(const std::string& extension) {
    if (selection_.size() != 1 || !assets::can_encode_image(extension)) return false;
    const std::string rel = selection_[0];
    const assets::AssetRecord* rec = record(rel);
    if (!rec || rec->type != "image" || extension_of(rel) == extension) return false;
    std::vector<u8> bytes, out;
    assets::CookedTexture img;
    if (!read_file(abs(rel), bytes) || !assets::decode_image(bytes, img) || !assets::encode_image(img, extension, out))
        return false;
    const std::string stem = name_of(rel).substr(0, name_of(rel).size() - extension_of(rel).size());
    const std::string name = unique_name(abs(parent_of(rel)), stem + extension);
    const fs::path staged = staging_dir() / utf8_path(name);
    std::error_code ec;
    fs::create_directories(staged.parent_path(), ec);
    if (!write_file_atomic(staged, out)) return false;
    const std::string to_rel = parent_of(rel).empty() ? name : parent_of(rel) + "/" + name;
    std::string upper = extension.substr(1);
    for (char& c : upper) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    history_.execute(std::make_unique<FileMoves>(*this, std::vector<FileMoves::Move>{{staged, abs(to_rel)}},
                                                 "В " + upper + ": " + name_of(rel)));
    history_.seal();
    select({to_rel});
    return true;
}

bool AssetLibrary::set_tags(const std::string& text) {
    if (selection_.size() != 1 || !record(selection_[0])) return false;
    fs::path meta_file = abs(selection_[0]);
    meta_file += ".meta";
    std::vector<u8> before;
    assets::AssetMeta meta;
    data::LoadReport report;
    if (!read_file(meta_file, before) ||
        !data::from_json(meta, std::string_view(reinterpret_cast<const char*>(before.data()), before.size()), report)) {
        FORGE_WARN("У файла ещё нет .meta: подождите, пока ресурсы обновятся");
        return false;
    }
    std::vector<std::string> tags;
    std::string word;
    for (char c : text + " ") {
        if (c == ' ' || c == ',' || c == ';') {
            if (!word.empty() && std::find(tags.begin(), tags.end(), word) == tags.end()) tags.push_back(word);
            word.clear();
        } else {
            word += c;
        }
    }
    if (tags == meta.tags) return false;
    meta.tags = std::move(tags);
    const std::string json = data::to_json(meta);
    std::vector<u8> after(json.begin(), json.end());
    history_.execute(std::make_unique<FileContents>(*this, meta_file, std::move(before), std::move(after),
                                                    "Метки: " + name_of(selection_[0])));
    history_.seal();
    return true;
}

void AssetLibrary::play_sound() {
    if (selection_.size() != 1 || extension_of(selection_[0]) != ".wav" || config_.offscreen) return;
    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        FORGE_WARN("Звук недоступен: %s", SDL_GetError());
        return;
    }
    SDL_AudioSpec spec{};
    Uint8* buffer = nullptr;
    Uint32 length = 0;
    if (!SDL_LoadWAV(path_to_utf8(abs(selection_[0])).c_str(), &spec, &buffer, &length)) {
        FORGE_WARN("Звук не читается: %s", SDL_GetError());
        return;
    }
    if (sound_) SDL_DestroyAudioStream(sound_);
    sound_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (sound_) {
        SDL_PutAudioStreamData(sound_, buffer, static_cast<int>(length));
        SDL_FlushAudioStream(sound_);
        SDL_ResumeAudioStreamDevice(sound_);
    }
    SDL_free(buffer);
}

void AssetLibrary::undo() {
    if (history_.undo()) FORGE_INFO("Отменено: %s", history_.redo_label().c_str());
}

void AssetLibrary::redo() {
    if (history_.redo()) FORGE_INFO("Повторено: %s", history_.undo_label().c_str());
}

std::string AssetLibrary::status() const {
    std::string s = folder_.empty() ? "Ресурсы" : "Ресурсы/" + folder_;
    s += " · всего " + files_text(static_cast<int>(records_.size()));
    if (!selection_.empty()) s += " · выбрано " + std::to_string(selection_.size());
    if (!cut_.empty()) s += " · вырезано " + std::to_string(cut_.size());
    return s;
}

// --- frame -------------------------------------------------------------------

void AssetLibrary::update(f64 dt, Rml::Context* context) {
    time_ += dt;
    ++frame_;
    take_results();
    if (search_pending_ && time_ >= search_at_) {
        search_pending_ = false;
        set_search(search_text_);
    }
    if (!context) {
        open_ = false;
        return;
    }
    dock_.update(context);
    sync_model();
}

void AssetLibrary::sync_model() {
    set(m_filter_, Rml::String(filter_), "as_filter");
    set(m_searching_, !search_.empty(), "as_searching");
    set(m_has_cut_, !cut_.empty(), "as_has_cut");
    set(m_busy_, busy(), "as_busy");
    set(m_empty_, have_index_ && rows_.empty(), "as_empty");
    Rml::String where = search_.empty() ? (folder_.empty() ? "Ресурсы" : name_of(folder_)) : "Поиск «" + search_ + "»";
    set(m_where_, where, "as_where");
    char status[160];
    std::snprintf(status, sizeof(status), "%s · %s", files_text(static_cast<int>(rows_.size())).c_str(),
                  have_index_ ? ("в проекте " + files_text(static_cast<int>(records_.size()))).c_str() : "смотрю папку…");
    set(m_status_, Rml::String(status), "as_status");

    // Breadcrumbs
    std::vector<Crumb> crumbs{{"Ресурсы", ""}};
    if (!folder_.empty()) {
        usize at = 0;
        while (at != std::string::npos) {
            const usize slash = folder_.find('/', at);
            const std::string path = folder_.substr(0, slash);
            crumbs.push_back({name_of(path), path});
            at = slash == std::string::npos ? slash : slash + 1;
        }
    }
    if (crumbs.size() != m_crumbs_.size() || crumbs.back().path != m_crumbs_.back().path) {
        m_crumbs_ = std::move(crumbs);
        model_.DirtyVariable("as_crumbs");
    }

    if (selection_version_ != synced_selection_ || rows_version_ != synced_rows_ || preview_version_) sync_preview();

    if (history_.version() != history_version_) {
        history_version_ = history_.version();
        m_history_.clear();
        for (usize i = 0; i < history_.size(); ++i) m_history_.push_back(history_.label_at(i));
        m_history_cursor_ = static_cast<int>(history_.cursor());
        model_.DirtyVariable("as_history");
        model_.DirtyVariable("as_history_cursor");
    }
}

void AssetLibrary::sync_preview() {
    const bool selection_changed = selection_version_ != synced_selection_ || rows_version_ != synced_rows_;
    synced_selection_ = selection_version_;
    synced_rows_ = rows_version_;
    preview_version_ = 0;
    set(m_sel_count_, static_cast<int>(selection_.size()), "as_sel_count");
    Rml::String name, ext, kind, size, dims, path, tags, preview, icon, extra;
    bool image = false, audio = false, dir = false, editable = false, wav = false;
    if (selection_.size() == 1) {
        const std::string& rel = selection_[0];
        path = rel;
        if (std::binary_search(dirs_.begin(), dirs_.end(), rel, path_less)) {
            dir = true;
            name = name_of(rel);
            kind = "Папка";
            icon = "folder";
            auto it = folder_counts_.find(rel);
            size = files_text(it == folder_counts_.end() ? 0 : it->second);
        } else if (const assets::AssetRecord* rec = record(rel)) {
            ext = extension_of(rel);
            name = name_of(rel).substr(0, name_of(rel).size() - ext.size());
            kind = kind_name(rec->type, ext);
            icon = kind_icon(rec->type, ext);
            size = size_text(rec->size);
            tags = rec->tags;
            if (rec->type == "image") {
                image = true;
                editable = assets::can_encode_image(ext);
                const std::string hex = rec->cook_key.to_hex();
                const std::string want = "pv_" + hex;
                if (preview_wanted_ != want) {
                    preview_wanted_ = want;
                    if (!thumbs_.count(want)) want_thumb(want, rec->cook_key, kPreviewPx);
                }
                if (preview_name_ == want) preview = "/memory/" + want;
                auto d = dims_.find(hex);
                if (d != dims_.end()) {
                    char text[48];
                    std::snprintf(text, sizeof(text), "%u × %u пикс.", d->second.first, d->second.second);
                    dims = text;
                }
            } else if (rec->type == "audio") {
                audio = true;
                wav = ext == ".wav";
                if (wav) {
                    const f64 seconds = wav_seconds(abs(rel));
                    char text[48];
                    std::snprintf(text, sizeof(text), "%.1f с", seconds);
                    if (seconds > 0) extra = text;
                } else {
                    extra = "Слушать можно WAV; OGG и MP3 позже";
                }
            }
        }
    } else if (selection_.size() > 1) {
        u64 total = 0;
        for (const std::string& rel : selection_)
            if (const auto* rec = record(rel)) total += rec->size;
        name = "Выбрано: " + std::to_string(selection_.size());
        size = size_text(total);
        icon = "select_all";
    }
    if (!image && !preview_name_.empty()) {
        ui_->drop_image(preview_name_);
        thumbs_.erase(preview_name_);
        preview_name_.clear();
        preview_wanted_.clear();
    }
    // Do not rewrite a field the user is typing in.
    (void)selection_changed;
    set(m_sel_name_, name, "as_sel_name");
    set(m_sel_ext_, ext, "as_sel_ext");
    set(m_sel_kind_, kind, "as_sel_kind");
    set(m_sel_size_, size, "as_sel_size");
    set(m_sel_dims_, dims, "as_sel_dims");
    set(m_sel_path_, path, "as_sel_path");
    set(m_sel_tags_, tags, "as_sel_tags");
    set(m_sel_preview_, preview, "as_sel_preview");
    set(m_sel_icon_, icon, "as_sel_icon");
    set(m_sel_extra_, extra, "as_sel_extra");
    set(m_sel_image_, image, "as_sel_image");
    set(m_sel_audio_, audio, "as_sel_audio");
    set(m_sel_wav_, wav, "as_sel_wav");
    set(m_sel_dir_, dir, "as_sel_dir");
    set(m_sel_editable_, editable, "as_sel_editable");
}

// --- input -------------------------------------------------------------------

bool AssetLibrary::handle_event(const SDL_Event& e, f32 density, bool ui_used, Rml::Context*) {
    switch (e.type) {
    case SDL_EVENT_DROP_BEGIN: dropped_.clear(); return true;
    case SDL_EVENT_DROP_FILE:
        if (e.drop.data) dropped_.push_back(utf8_path(e.drop.data));
        return true;
    case SDL_EVENT_DROP_COMPLETE:
        import(dropped_);
        dropped_.clear();
        return true;
    case SDL_EVENT_MOUSE_MOTION: return dock_.mouse_move(e.motion.x * density, e.motion.y * density);
    case SDL_EVENT_MOUSE_BUTTON_DOWN: return dock_.busy();
    case SDL_EVENT_MOUSE_BUTTON_UP: return dock_.mouse_up();
    default: break;
    }
    (void)ui_used;
    return false;
}

bool AssetLibrary::handle_key(const SDL_KeyboardEvent& k) {
    const bool ctrl = (k.mod & SDL_KMOD_CTRL) != 0;
    if (ctrl && k.key == SDLK_X) { cut(); return true; }
    if (ctrl && k.key == SDLK_V) { paste(); return true; }
    if (ctrl && k.key == SDLK_A) {
        std::vector<std::string> all;
        for (const Row& r : rows_) all.push_back(r.rel);
        select(all);
        return true;
    }
    if (ctrl) return false;
    if (k.key == SDLK_DELETE) { delete_selection(); return true; }
    if (k.key == SDLK_BACKSPACE) {
        if (!folder_.empty()) open_folder(parent_of(folder_));
        return true;
    }
    if (k.key == SDLK_RETURN && selection_.size() == 1 &&
        std::binary_search(dirs_.begin(), dirs_.end(), selection_[0], path_less)) {
        open_folder(selection_[0]);
        return true;
    }
    if (k.key == SDLK_F5) { refresh(); return true; }
    if (k.key == SDLK_ESCAPE) {
        if (!cut_.empty()) {
            cut_.clear();
            ++list_version_;
        } else {
            select({});
        }
        return true;
    }
    return false;
}

} // namespace forge::editor_app
