#pragma once

// Turns source files in the project's asset folder into cooked data the game
// loads directly. Runs in the background in the editor; only what changed is
// redone:
//   - unchanged size and time: nothing is read at all,
//   - changed time, same content (a git checkout): the index is updated,
//   - changed content or importer: the file is cooked again, on all cores.
//
// Every source file gets a small "<file>.meta" next to it holding its Guid,
// so references survive renames and moves, and the Guid lives in git.

#include "forge/assets/asset_database.h"
#include "forge/data/reflect.h"

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace forge::assets {

// Saved next to each source file as JSON.
struct AssetMeta {
    Guid id;
    std::vector<std::string> tags;
};

struct CookInput {
    std::span<const u8> source;
    std::string_view path; // relative path, for messages
    const AssetMeta* meta = nullptr;
};

struct CookOutput {
    std::vector<u8> cooked;
    std::vector<Guid> dependencies;
    std::string error; // set on failure; the previous cooked result stays in use
};

class Importer {
public:
    virtual ~Importer() = default;
    virtual const char* name() const = 0;
    // Bump when the cooked format or the cooking changes: every asset of
    // this importer is cooked again.
    virtual u32 version() const = 0;
    virtual bool handles(std::string_view extension) const = 0; // lowercase, with dot
    // Called on worker threads; must not touch shared state.
    virtual void cook(const CookInput& in, CookOutput& out) const = 0;
};

struct RefreshReport {
    u32 files = 0;     // source files found
    u32 unchanged = 0; // skipped without reading
    u32 retouched = 0; // time changed, content the same
    u32 cooked = 0;
    u32 failed = 0;
    u32 added = 0;     // new files (a .meta was created)
    u32 moved = 0;     // found under a new path, Guid kept
    u32 removed = 0;
    f64 ms = 0;
    std::vector<std::string> messages;
};

class AssetPipeline {
public:
    // assets_dir: the project's source assets. library_dir: cooked output and
    // the index database (not in git; can always be rebuilt).
    AssetPipeline(std::filesystem::path assets_dir, std::filesystem::path library_dir);
    ~AssetPipeline();

    bool open(std::string* error = nullptr);
    void add_importer(std::unique_ptr<Importer> importer);
    void add_default_importers(); // images, plain copy fallback

    RefreshReport refresh();

    AssetDatabase& database() { return db_; }
    std::filesystem::path cooked_path(const ContentHash& cook_key) const { return cooked_file(library_dir_, cook_key); }
    // Where the cooked result of cook_key lies in a library folder.
    static std::filesystem::path cooked_file(const std::filesystem::path& library_dir, const ContentHash& cook_key);
    static std::filesystem::path database_file(const std::filesystem::path& library_dir) { return library_dir / "assets.db"; }

private:
    const Importer* importer_for(const std::filesystem::path& file) const;

    std::filesystem::path assets_dir_;
    std::filesystem::path library_dir_;
    AssetDatabase db_;
    std::vector<std::unique_ptr<Importer>> importers_;
};

// Cooked form produced by the image importer.
struct CookedTexture {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> rgba8; // width * height * 4, top row first
};

} // namespace forge::assets

FORGE_REFLECT_DECLARE(forge::assets::AssetMeta)
FORGE_REFLECT_DECLARE(forge::assets::CookedTexture)
