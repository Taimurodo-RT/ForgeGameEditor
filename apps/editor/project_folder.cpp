#include "project_folder.h"

#include "forge/core/file.h"
#include "forge/core/path.h"

namespace forge::editor_app {

namespace fs = std::filesystem;

namespace {

bool same_bytes(const fs::path& a, const fs::path& b) {
    std::vector<u8> x, y;
    return read_file(a, x) && read_file(b, y) && x == y;
}

} // namespace

bool prepare_project_game(const fs::path& project, const fs::path& template_dir, ProjectGame& out, std::string* error) {
    std::error_code ec;
    out = {};
    out.dir = project / "game";
    if (!fs::exists(out.dir / "game.json", ec)) {
        if (!fs::is_directory(template_dir, ec)) {
            if (error) *error = "нет образца игры: " + path_to_utf8(template_dir);
            return false;
        }
        // Copied into a side folder first, so a stopped copy is never taken for a project.
        const fs::path part = project / "game.copying";
        fs::remove_all(part, ec);
        fs::create_directories(project, ec);
        fs::copy(template_dir, part, fs::copy_options::recursive, ec);
        if (ec) {
            if (error) *error = "не удалось скопировать игру в " + path_to_utf8(out.dir) + ": " + ec.message();
            fs::remove_all(part, ec);
            return false;
        }
        if (fs::exists(out.dir, ec)) fs::remove_all(out.dir, ec); // an empty or broken one
        fs::rename(part, out.dir, ec);
        if (ec) {
            if (error) *error = "не удалось создать " + path_to_utf8(out.dir) + ": " + ec.message();
            return false;
        }
        out.created = true;
        return true;
    }
    for (const fs::directory_entry& e : fs::directory_iterator(template_dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        const fs::path name = e.path().filename();
        const fs::path mine = out.dir / name;
        bool engine = false;
        for (const char* f : kEngineGameFiles) engine = engine || name == f;
        const bool missing = !fs::exists(mine, ec);
        if (!missing && (!engine || same_bytes(e.path(), mine))) continue;
        fs::copy_file(e.path(), mine, fs::copy_options::overwrite_existing, ec);
        if (!ec) out.refreshed.push_back(path_to_utf8(name));
    }
    return true;
}

} // namespace forge::editor_app
