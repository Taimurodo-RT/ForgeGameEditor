#include "forge/game/game_module.h"

#include "forge/core/file.h"
#include "forge/core/path.h"

#include <yyjson.h>

#include <system_error>
#include <vector>

namespace forge::game {

namespace fs = std::filesystem;

GameModule game_module_of(std::string_view json, std::string& id, std::string* error) {
    id.clear();
    auto broken = [&](std::string why) {
        if (error) *error = std::move(why);
        return GameModule::Broken;
    };
    yyjson_doc* doc = yyjson_read(json.data(), json.size(), 0);
    yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return broken("game.json не читается");
    }
    yyjson_val* v = yyjson_obj_get(root, "module");
    if (!v) {
        yyjson_doc_free(doc);
        id = kOldGameModule;
        return GameModule::Old;
    }
    const bool text = yyjson_is_str(v);
    std::string named = text ? std::string(yyjson_get_str(v), yyjson_get_len(v)) : std::string();
    yyjson_doc_free(doc);
    if (!text) return broken("в game.json «module» — не строка");
    if (named.empty()) return broken("в game.json пустой «module»");
    id = std::move(named);
    return GameModule::Named;
}

GameModule game_module(const fs::path& game_dir, std::string& id, std::string* error) {
    id.clear();
    std::error_code ec;
    const fs::path file = game_dir / "game.json";
    std::vector<u8> bytes;
    if (!fs::is_regular_file(file, ec)) {
        if (error) *error = "в папке игры " + path_to_utf8(game_dir) + " нет game.json";
        return GameModule::Broken;
    }
    if (!read_file(file, bytes)) {
        if (error) *error = path_to_utf8(file) + " не читается";
        return GameModule::Broken;
    }
    return game_module_of(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), id, error);
}

} // namespace forge::game
