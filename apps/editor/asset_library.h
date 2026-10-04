#pragma once

// The «Ресурсы» tab: the project's files as a simple explorer. Folders on
// the left, the open folder (or search results) in the middle, a preview
// with the file's details, simple picture edits and links to its editor on
// the right. Files dropped on the window are imported into the open folder.
//
// Every change on disk (import, new folder, rename, cut and paste, delete,
// picture edits, tags) is an undoable command: deleted files go to the
// library's trash, new ones come from its staging folder, so everything is
// a move and Ctrl+Z puts it back.
//
// The index (AssetPipeline) is refreshed on a background thread; previews
// are made on another one, only for the rows on screen.

#include "forge/assets/asset_pipeline.h"
#include "forge/editor/document.h"
#include "forge/editor/undo.h"
#include "forge/ui/ui.h"
#include "forge/ui/virtual_list.h"

#include "dock_view.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace forge::editor_app {

struct AssetsConfig {
    std::filesystem::path folder;   // the project's files
    std::filesystem::path library;  // index, cooked data, trash (not in git)
    std::filesystem::path settings; // the panel layout
    SDL_Window* window = nullptr;   // for the file dialogs
    bool offscreen = false;
};

class AssetLibrary;

// Moves of files and folders (each with its .meta); revert moves them back.
class FileMoves final : public editor::Command {
public:
    struct Move {
        std::filesystem::path from, to;
    };
    FileMoves(AssetLibrary& library, std::vector<Move> moves, std::string label);
    void apply(editor::Document&) override;
    void revert(editor::Document&) override;
    std::string label() const override { return label_; }

private:
    AssetLibrary& library_;
    std::vector<Move> moves_;
    std::string label_;
};

// New contents of one file (a picture edit, a .meta with new tags).
class FileContents final : public editor::Command {
public:
    FileContents(AssetLibrary& library, std::filesystem::path file, std::vector<u8> before, std::vector<u8> after,
                 std::string label);
    void apply(editor::Document&) override;
    void revert(editor::Document&) override;
    std::string label() const override { return label_; }

private:
    AssetLibrary& library_;
    std::filesystem::path file_;
    std::vector<u8> before_, after_;
    std::string label_;
};

class AssetLibrary final : public ui::ListSource {
public:
    AssetLibrary();
    ~AssetLibrary() override;

    bool init(ui::Ui& ui, const AssetsConfig& config);
    void bind(Rml::DataModelConstructor& model);
    void set_model(Rml::DataModelHandle handle);
    void shutdown();

    // Every frame (results from the background threads arrive even while the
    // tab is closed); context: only while the tab is open.
    void update(f64 dt, Rml::Context* context);
    // When the tab opens: look for changes made outside the editor.
    void opened();
    bool handle_event(const SDL_Event& e, f32 density, bool ui_used, Rml::Context* context);
    bool handle_key(const SDL_KeyboardEvent& k);
    void set_ui_updating(bool on) { ui_updating_ = on; }

    editor::UndoStack& history() { return history_; }
    void undo();
    void redo();
    std::string status() const;

    // --- actions (also for the self-test) ---
    void refresh();
    void open_folder(const std::string& rel);
    void set_search(const std::string& text);
    void set_filter(const std::string& type); // "", "image", "audio", "file"
    void select(std::vector<std::string> rels);
    void import(const std::vector<std::filesystem::path>& sources);
    void new_folder();
    bool rename_selected(const std::string& name);
    void delete_selection();
    void cut();
    void paste();
    bool edit_image(const std::string& op); // cw, ccw, fliph, flipv, half, double
    bool convert_image(const std::string& extension);
    bool set_tags(const std::string& tags);
    void play_sound();

    // --- state (for the self-test and benchmarks) ---
    bool busy() const;
    const std::string& folder() const { return folder_; }
    const std::vector<std::string>& selection() const { return selection_; }
    usize record_count() const { return records_.size(); }
    usize row_count() const { return rows_.size(); }
    std::string row_rel(usize i) const { return i < rows_.size() ? rows_[i].rel : std::string(); }
    bool row_has_thumb(usize i) const;
    u32 preview_width() const { return preview_w_; }
    u32 preview_height() const { return preview_h_; }
    std::filesystem::path abs(const std::string& rel) const;
    const std::filesystem::path& root() const { return config_.folder; }
    const std::filesystem::path& trash() const { return trash_; }
    editor::DockLayout& dock() { return dock_.layout(); }
    u64 thumbs_made() const { return thumbs_made_; }

    // ui::ListSource: the middle list.
    u32 count() const override { return static_cast<u32>(rows_.size()); }
    std::string field(u32 row, std::string_view name) const override;
    u64 version() const override { return list_version_; }
    void on_row_event(u32 row, std::string_view event, int modifiers) override;

    // Commands call this after they touched the disk.
    void changed();
    std::filesystem::path staging_dir();
    std::filesystem::path trash_dir();

private:
    struct Row {
        bool dir = false;
        std::string rel, name;
        i32 record = -1;
    };
    struct TreeRow {
        Rml::String path, name;
        int depth = 0, count = 0;
        bool open = false, kids = false, selected = false;
    };
    struct Crumb {
        Rml::String name, path;
    };
    struct ThumbJob {
        std::string name; // memory image name
        assets::ContentHash key;
        u32 box = 0;
    };
    struct ThumbDone {
        std::string name;
        u32 box = 0, src_w = 0, src_h = 0;
        std::vector<u8> rgba;
        bool ok = false;
    };
    struct Snapshot {
        std::vector<assets::AssetRecord> records;
        std::vector<std::string> dirs;
        assets::RefreshReport report;
    };
    struct ImportJob {
        std::vector<std::filesystem::path> sources;
        std::filesystem::path staging;
        std::string folder;
    };
    struct Staged {
        std::vector<std::filesystem::path> items; // in staging
        std::string folder;
        std::vector<std::string> errors;
    };
    struct Thumb {
        std::string name;
        u64 used = 0;
    };

    template <typename T>
    void set(T& member, const T& value, const char* name) {
        if (member == value) return;
        member = value;
        model_.DirtyVariable(name);
    }

    void refresher_main();
    void thumbnailer_main();
    void take_results();
    void rebuild_rows();
    void rebuild_tree();
    void sync_model();
    void sync_preview();
    void want_thumb(const std::string& name, const assets::ContentHash& key, u32 box) const;
    const assets::AssetRecord* record(const std::string& rel) const;
    std::string unique_name(const std::filesystem::path& dir, const std::string& name) const;
    bool selection_text_focus(Rml::Context* context) const;

    AssetsConfig config_;
    ui::Ui* ui_ = nullptr;
    Rml::DataModelHandle model_;
    editor::Document doc_; // the undo stack wants one
    editor::UndoStack history_{doc_};
    DockView dock_;
    std::filesystem::path trash_, staging_;
    u64 serial_ = 0; // trash and staging sub-folders
    bool ui_updating_ = false;
    bool open_ = false;
    f64 time_ = 0;

    // Background threads.
    std::thread refresher_, thumbnailer_;
    mutable std::mutex mutex_;
    mutable std::condition_variable refresh_cv_, thumb_cv_;
    bool stop_ = false;
    bool refresh_wanted_ = false;
    bool refreshing_ = false;
    std::deque<ImportJob> imports_;
    std::vector<Snapshot> snapshots_;
    std::vector<Staged> staged_;
    mutable std::deque<ThumbJob> thumb_jobs_;
    std::vector<ThumbDone> thumbs_done_;
    std::atomic<bool> pipeline_failed_{false};

    // The index as last seen.
    std::vector<assets::AssetRecord> records_;
    std::unordered_map<std::string, usize> by_path_;
    std::vector<std::string> dirs_;
    std::unordered_map<std::string, int> folder_counts_; // files directly inside
    assets::RefreshReport last_report_;
    bool have_index_ = false;
    assets::AssetDatabase search_db_;

    // What is shown.
    std::string folder_, search_, filter_;
    std::string search_text_; // typed, not searched yet
    f64 search_at_ = 0;
    bool search_pending_ = false;
    std::vector<Row> rows_;
    std::set<std::string> open_dirs_;
    std::vector<std::string> selection_;
    std::string anchor_;
    std::vector<std::string> cut_;
    u64 list_version_ = 1, selection_version_ = 1, rows_version_ = 1;

    // Pictures in memory.
    mutable std::unordered_map<std::string, Thumb> thumbs_; // by name, made
    mutable std::set<std::string> asked_;                   // requested, not made yet
    u64 frame_ = 0, thumbs_made_ = 0;
    std::unordered_map<std::string, std::pair<u32, u32>> dims_; // picture sizes by cook key
    std::string preview_name_, preview_wanted_;
    u32 preview_w_ = 0, preview_h_ = 0;
    u64 preview_version_ = 0;

    // Dialogs answer on their own thread.
    std::vector<std::filesystem::path> dialog_files_;
    std::vector<std::filesystem::path> dropped_;

    // Sound
    SDL_AudioStream* sound_ = nullptr;

    // Model mirrors
    std::vector<TreeRow> m_tree_;
    std::vector<Crumb> m_crumbs_;
    Rml::String m_filter_, m_search_, m_status_, m_where_;
    bool m_busy_ = false, m_searching_ = false, m_has_cut_ = false, m_empty_ = false;
    int m_sel_count_ = 0;
    Rml::String m_sel_name_, m_sel_ext_, m_sel_kind_, m_sel_size_, m_sel_dims_, m_sel_path_, m_sel_tags_, m_sel_preview_,
        m_sel_icon_, m_sel_extra_;
    bool m_sel_image_ = false, m_sel_audio_ = false, m_sel_dir_ = false, m_sel_editable_ = false, m_sel_wav_ = false;
    std::vector<Rml::String> m_history_;
    int m_history_cursor_ = 0;
    u64 history_version_ = ~0ull;
    u64 synced_selection_ = 0, synced_rows_ = 0;
};

} // namespace forge::editor_app
