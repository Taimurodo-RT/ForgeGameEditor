#include "forge/level/own_tiles.h"

#include "forge/assets/image.h"
#include "forge/core/file.h"
#include "forge/core/path.h"

#include <yyjson.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace forge::level {

namespace fs = std::filesystem;

namespace {

usize characters(std::string_view s) {
    usize n = 0;
    for (const char c : s) n += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    return n;
}

std::string in_quotes(std::string_view s) { return "«" + std::string(s) + "»"; }

bool has_control(std::string_view s) {
    for (const char c : s)
        if (static_cast<unsigned char>(c) < 0x20) return true;
    return false;
}

struct Doc {
    yyjson_doc* d = nullptr;
    ~Doc() { yyjson_doc_free(d); }
};

// The root object of a JSON file, or why there is none.
yyjson_val* read_json(const fs::path& file, Doc& doc, std::string& why) {
    std::vector<u8> bytes;
    std::error_code ec;
    if (!fs::is_regular_file(file, ec) || !read_file(file, bytes)) {
        why = "файл не читается";
        return nullptr;
    }
    yyjson_read_err err{};
    doc.d = yyjson_read_opts(reinterpret_cast<char*>(bytes.data()), bytes.size(), 0, nullptr, &err);
    if (!doc.d) {
        why = std::string("файл испорчен: ") + (err.msg ? err.msg : "не JSON") + " (байт " + std::to_string(err.pos) + ")";
        return nullptr;
    }
    yyjson_val* root = yyjson_doc_get_root(doc.d);
    if (!yyjson_is_obj(root)) {
        why = "файл испорчен: в нём не объект JSON";
        return nullptr;
    }
    return root;
}

std::string write_json(yyjson_mut_doc* doc) {
    usize len = 0;
    char* text = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY_TWO_SPACES, &len);
    std::string out = text ? std::string(text, len) + "\n" : std::string();
    std::free(text);
    yyjson_mut_doc_free(doc);
    return out;
}

bool write_text(const fs::path& file, const std::string& text) {
    return !text.empty() && write_file_atomic(file, {reinterpret_cast<const u8*>(text.data()), text.size()});
}

} // namespace

u32 max_own_tiles(u32 px) {
    const u32 cell = std::max<u32>(16, px);
    const u64 row = kMaxAtlasPx / cell, cells = row * row;
    return static_cast<u32>(std::min<u64>(kMaxOwnTiles, cells > kFirstOwnTile ? cells - kFirstOwnTile : 0));
}

const OwnTile* LevelTiles::find(world::TileId id) const {
    for (const OwnTile& t : tiles)
        if (t.id == id) return &t;
    return nullptr;
}

std::string tiles_problem(const LevelTiles& t, u32 layer_count, i32 liquids_layer) {
    if (t.tiles.empty()) return t.rgba.empty() ? std::string() : "картинки есть, а тайлов нет";
    if (t.px < kMinOwnTilePx || t.px > kMaxOwnTilePx)
        return "сторона картинки тайла " + std::to_string(t.px) + " px: нужна от " + std::to_string(kMinOwnTilePx) + " до " +
               std::to_string(kMaxOwnTilePx);
    const u32 most = max_own_tiles(t.px);
    if (t.tiles.size() > most) return "тайлов " + std::to_string(t.tiles.size()) + ", больше " + std::to_string(most);
    if (t.rgba.size() != t.tiles.size() * t.px * t.px * 4) return "картинок не столько, сколько тайлов";
    std::vector<bool> taken(most, false);
    for (usize i = 0; i < t.tiles.size(); ++i) {
        const OwnTile& o = t.tiles[i];
        const std::string at = "тайл " + std::to_string(i + 1) + (o.name.empty() ? std::string() : " " + in_quotes(o.name));
        if (o.id < kFirstOwnTile || o.id >= kFirstOwnTile + most)
            return at + ": id " + std::to_string(o.id) + " не от " + std::to_string(kFirstOwnTile) + " до " +
                   std::to_string(kFirstOwnTile + most - 1);
        if (taken[o.id - kFirstOwnTile]) return at + ": id " + std::to_string(o.id) + " уже есть у другого тайла";
        taken[o.id - kFirstOwnTile] = true;
        if (o.name.empty()) return at + ": нет имени";
        if (characters(o.name) > kMaxOwnTileName) return at + ": имя длиннее " + std::to_string(kMaxOwnTileName) + " знаков";
        if (has_control(o.name)) return at + ": в имени перевод строки или другой служебный знак";
        if (o.layer >= layer_count || static_cast<i32>(o.layer) == liquids_layer)
            return at + ": слой " + std::to_string(o.layer) + " — не слой тайлов этой игры";
        if (characters(o.from) > 200 || has_control(o.from)) return at + ": «откуда» длиннее 200 знаков или с служебным знаком";
    }
    return {};
}

std::string tiles_json(const LevelTiles& t) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_uint(doc, root, "px", t.px);
    yyjson_mut_val* list = yyjson_mut_arr(doc);
    for (const OwnTile& o : t.tiles) {
        yyjson_mut_val* v = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_uint(doc, v, "id", o.id);
        yyjson_mut_obj_add_strncpy(doc, v, "name", o.name.data(), o.name.size());
        yyjson_mut_obj_add_uint(doc, v, "layer", o.layer);
        yyjson_mut_obj_add_bool(doc, v, "solid", o.solid);
        if (!o.from.empty()) yyjson_mut_obj_add_strncpy(doc, v, "from", o.from.data(), o.from.size());
        yyjson_mut_arr_append(list, v);
    }
    yyjson_mut_obj_add_val(doc, root, "tiles", list);
    return write_json(doc);
}

bool load_tiles(const fs::path& folder, LevelTiles& out, u32 layer_count, i32 liquids_layer, bool* found, std::string* error) {
    if (found) *found = false;
    const fs::path file = folder / kTilesFile;
    std::error_code ec;
    if (!fs::exists(file, ec)) return true;
    if (found) *found = true;
    auto fail = [&](const std::string& why) {
        if (error) *error = std::string(kTilesFile) + ": " + why;
        return false;
    };
    Doc doc;
    std::string why;
    yyjson_val* root = read_json(file, doc, why);
    if (!root) return fail(why);
    LevelTiles t;
    yyjson_val* px = yyjson_obj_get(root, "px");
    if (!yyjson_is_uint(px) || yyjson_get_uint(px) > kMaxOwnTilePx) return fail("«px» — не сторона картинки в пикселях");
    t.px = static_cast<u32>(yyjson_get_uint(px));
    yyjson_val* list = yyjson_obj_get(root, "tiles");
    if (!yyjson_is_arr(list)) return fail("«tiles» — не список тайлов");
    if (yyjson_arr_size(list) > kMaxOwnTiles) return fail("тайлов больше " + std::to_string(kMaxOwnTiles));
    usize i, n;
    yyjson_val* v;
    yyjson_arr_foreach(list, i, n, v) {
        const std::string at = "тайл " + std::to_string(i + 1);
        if (!yyjson_is_obj(v)) return fail(at + ": не объект");
        OwnTile o;
        yyjson_val* id = yyjson_obj_get(v, "id");
        if (!yyjson_is_uint(id) || yyjson_get_uint(id) > 0xffff) return fail(at + ": id — не целое число от 0 до 65535");
        o.id = static_cast<world::TileId>(yyjson_get_uint(id));
        yyjson_val* name = yyjson_obj_get(v, "name");
        if (!yyjson_is_str(name)) return fail(at + ": имя — не строка");
        o.name.assign(yyjson_get_str(name), yyjson_get_len(name));
        yyjson_val* layer = yyjson_obj_get(v, "layer");
        if (!yyjson_is_uint(layer) || yyjson_get_uint(layer) > 64) return fail(at + ": слой — не номер слоя");
        o.layer = static_cast<u32>(yyjson_get_uint(layer));
        if (yyjson_val* solid = yyjson_obj_get(v, "solid"); solid && !yyjson_is_null(solid)) {
            if (!yyjson_is_bool(solid)) return fail(at + ": «solid» — не true или false");
            o.solid = yyjson_get_bool(solid);
        }
        if (yyjson_val* from = yyjson_obj_get(v, "from"); from && !yyjson_is_null(from)) {
            if (!yyjson_is_str(from)) return fail(at + ": «from» — не строка");
            o.from.assign(yyjson_get_str(from), yyjson_get_len(from));
        }
        t.tiles.push_back(std::move(o));
    }
    if (!t.tiles.empty()) {
        // The pictures: a grid of px × px, kOwnTileColumns in a row.
        std::vector<u8> bytes;
        const fs::path picture = folder / kTilesPicture;
        if (!fs::is_regular_file(picture, ec) || !read_file(picture, bytes))
            return fail(std::string("нет картинки тайлов ") + kTilesPicture);
        assets::CookedTexture img;
        if (!assets::decode_image(bytes, img, &why)) return fail(std::string(kTilesPicture) + " не читается: " + why);
        const usize count = t.tiles.size();
        const u32 columns = static_cast<u32>(std::min<usize>(count, kOwnTileColumns));
        const u32 rows = static_cast<u32>((count + kOwnTileColumns - 1) / kOwnTileColumns);
        if (t.px == 0 || img.width != columns * t.px || img.height != rows * t.px)
            return fail(std::string(kTilesPicture) + " " + std::to_string(img.width) + " × " + std::to_string(img.height) +
                        " px, а нужно " + std::to_string(columns * t.px) + " × " + std::to_string(rows * t.px) +
                        " px (тайлов: " + std::to_string(count) + ", сторона " + std::to_string(t.px) + " px)");
        t.rgba.resize(count * t.px * t.px * 4);
        for (usize k = 0; k < count; ++k) {
            const u32 x0 = static_cast<u32>(k % kOwnTileColumns) * t.px, y0 = static_cast<u32>(k / kOwnTileColumns) * t.px;
            for (u32 y = 0; y < t.px; ++y)
                std::memcpy(&t.rgba[(k * t.px + y) * t.px * 4], &img.rgba8[(static_cast<usize>(y0 + y) * img.width + x0) * 4],
                            static_cast<usize>(t.px) * 4);
        }
    } else {
        t.px = 0;
    }
    if (const std::string problem = tiles_problem(t, layer_count, liquids_layer); !problem.empty()) return fail(problem);
    out = std::move(t);
    return true;
}

bool save_tiles(const fs::path& folder, const LevelTiles& t, std::string* error) {
    std::error_code ec;
    if (t.tiles.empty()) {
        fs::remove(folder / kTilesFile, ec);
        if (ec) {
            if (error) *error = "не удалось убрать " + path_to_utf8(folder / kTilesFile);
            return false;
        }
        fs::remove(folder / kTilesPicture, ec);
        return true;
    }
    if (t.px == 0 || t.rgba.size() != t.tiles.size() * t.px * t.px * 4) {
        if (error) *error = std::string(kTilesFile) + " не записан: картинок не столько, сколько тайлов";
        return false;
    }
    const usize count = t.tiles.size();
    assets::CookedTexture img;
    img.width = static_cast<u32>(std::min<usize>(count, kOwnTileColumns)) * t.px;
    img.height = static_cast<u32>((count + kOwnTileColumns - 1) / kOwnTileColumns) * t.px;
    img.rgba8.assign(static_cast<usize>(img.width) * img.height * 4, 0);
    for (usize k = 0; k < count; ++k) {
        const u32 x0 = static_cast<u32>(k % kOwnTileColumns) * t.px, y0 = static_cast<u32>(k / kOwnTileColumns) * t.px;
        for (u32 y = 0; y < t.px; ++y)
            std::memcpy(&img.rgba8[(static_cast<usize>(y0 + y) * img.width + x0) * 4], t.picture(k) + static_cast<usize>(y) * t.px * 4,
                        static_cast<usize>(t.px) * 4);
    }
    std::vector<u8> png;
    if (!folder.empty()) fs::create_directories(folder, ec);
    if (folder.empty() || !assets::encode_image(img, ".png", png) || !write_file_atomic(folder / kTilesPicture, png)) {
        if (error) *error = "не удалось записать " + path_to_utf8(folder / kTilesPicture);
        return false;
    }
    if (!write_text(folder / kTilesFile, tiles_json(t))) {
        if (error) *error = "не удалось записать " + path_to_utf8(folder / kTilesFile);
        return false;
    }
    return true;
}

bool load_world(const fs::path& folder, LevelWorld& out, bool* found, std::string* error) {
    if (found) *found = false;
    const fs::path file = folder / kWorldFile;
    std::error_code ec;
    if (!fs::exists(file, ec)) return true;
    if (found) *found = true;
    Doc doc;
    std::string why;
    yyjson_val* root = read_json(file, doc, why);
    yyjson_val* around = root ? yyjson_obj_get(root, "around") : nullptr;
    if (root && !(yyjson_is_str(around) && (std::strcmp(yyjson_get_str(around), "game") == 0 ||
                                            std::strcmp(yyjson_get_str(around), "empty") == 0)))
        why = "«around» — не \"game\" и не \"empty\"";
    if (!why.empty()) {
        if (error) *error = std::string(kWorldFile) + ": " + why;
        return false;
    }
    out.empty_around = std::strcmp(yyjson_get_str(around), "empty") == 0;
    return true;
}

bool save_world(const fs::path& folder, const LevelWorld& w, std::string* error) {
    std::error_code ec;
    if (!w.empty_around) {
        fs::remove(folder / kWorldFile, ec);
        if (ec && error) *error = "не удалось убрать " + path_to_utf8(folder / kWorldFile);
        return !ec;
    }
    if (!folder.empty()) fs::create_directories(folder, ec);
    if (folder.empty() || !write_text(folder / kWorldFile, "{\n  \"around\": \"empty\"\n}\n")) {
        if (error) *error = "не удалось записать " + path_to_utf8(folder / kWorldFile);
        return false;
    }
    return true;
}

} // namespace forge::level
