#include "forge/level/tiled_import.h"

#include "forge/assets/image.h"
#include "forge/core/file.h"
#include "forge/core/hash.h"
#include "forge/core/path.h"

#include <yyjson.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>

namespace forge::level::tiled {

namespace fs = std::filesystem;

namespace {

usize characters(std::string_view s) {
    usize n = 0;
    for (const char c : s) n += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    return n;
}

// At most n characters (an ellipsis in place of the rest).
std::string cut(std::string s, usize n) {
    if (characters(s) <= n) return s;
    usize at = 0, seen = 0;
    while (at < s.size() && seen < n - 1) {
        ++at;
        while (at < s.size() && (static_cast<unsigned char>(s[at]) & 0xC0) == 0x80) ++at;
        ++seen;
    }
    s.resize(at);
    return s + "…";
}

std::string without_control(std::string s) {
    for (char& c : s)
        if (static_cast<unsigned char>(c) < 0x20) c = ' ';
    return s;
}

std::string q(std::string_view s) { return "«" + std::string(s) + "»"; }

void note(std::vector<Note>& notes, const std::string& what, u64 n = 1) {
    if (n == 0) return;
    for (Note& x : notes)
        if (x.what == what) {
            x.count += n;
            return;
        }
    notes.push_back({what, n});
}

bool lower_ascii_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

// --- pictures ---

struct Image {
    bool ok = false;
    std::string why;
    u32 w = 0, h = 0;
    std::vector<u8> rgba;
};

class Pictures {
public:
    const Image& get(const fs::path& file, bool trans, const u8* rgb) {
        for (auto& [f, t, img] : cache_)
            if (f == file && t == (trans ? (rgb[0] << 16 | rgb[1] << 8 | rgb[2]) : -1)) return *img;
        auto img = std::make_unique<Image>();
        std::vector<u8> bytes;
        std::error_code ec;
        assets::CookedTexture tex;
        if (!fs::is_regular_file(file, ec) || !read_file(file, bytes)) img->why = "нет файла";
        else if (std::string why; !assets::decode_image(bytes, tex, &why)) img->why = "не читается: " + why;
        else {
            img->ok = true;
            img->w = tex.width;
            img->h = tex.height;
            img->rgba = std::move(tex.rgba8);
            // The see-through colour (Tiled's "trans").
            if (trans)
                for (usize p = 0; p + 3 < img->rgba.size(); p += 4)
                    if (img->rgba[p] == rgb[0] && img->rgba[p + 1] == rgb[1] && img->rgba[p + 2] == rgb[2]) img->rgba[p + 3] = 0;
        }
        cache_.emplace_back(file, trans ? (rgb[0] << 16 | rgb[1] << 8 | rgb[2]) : -1, std::move(img));
        return *std::get<2>(cache_.back());
    }

private:
    std::vector<std::tuple<fs::path, i32, std::unique_ptr<Image>>> cache_;
};

// A tile's picture as the gid's flags turn it: the diagonal flip first (x and
// y swap), then the horizontal and the vertical, as Tiled does.
void turned(const u8* src, u32 w, u32 h, usize stride, u32 flags, std::vector<u8>& out, u32& ow, u32& oh) {
    const bool d = flags & kFlipD, fh = flags & kFlipH, fv = flags & kFlipV;
    ow = d ? h : w;
    oh = d ? w : h;
    out.resize(static_cast<usize>(ow) * oh * 4);
    for (u32 y = 0; y < oh; ++y)
        for (u32 x = 0; x < ow; ++x) {
            const u32 a = fh ? ow - 1 - x : x, b = fv ? oh - 1 - y : y;
            const u32 sx = d ? b : a, sy = d ? a : b;
            std::memcpy(&out[(static_cast<usize>(y) * ow + x) * 4], src + static_cast<usize>(sy) * stride + static_cast<usize>(sx) * 4, 4);
        }
}

// One picture over another (both straight alpha, the same size).
void over(std::vector<u8>& dst, const std::vector<u8>& top) {
    for (usize p = 0; p + 3 < dst.size(); p += 4) {
        const u32 ta = top[p + 3], da = dst[p + 3];
        if (ta == 255 || da == 0) {
            std::memcpy(&dst[p], &top[p], 4);
            continue;
        }
        if (ta == 0) continue;
        const u32 oa = ta * 255 + da * (255 - ta); // ×255
        for (int c = 0; c < 3; ++c)
            dst[p + c] = static_cast<u8>((top[p + c] * ta * 255 + dst[p + c] * da * (255 - ta) + oa / 2) / oa);
        dst[p + 3] = static_cast<u8>((oa + 127) / 255);
    }
}

std::string flags_text(u32 flags) {
    std::string s;
    if (flags & kFlipD) s += "d";
    if (flags & kFlipH) s += "h";
    if (flags & kFlipV) s += "v";
    return s;
}

std::string flags_words(u32 flags) {
    std::string s;
    if (flags & kFlipD) s += "⤡";
    if (flags & kFlipH) s += "↔";
    if (flags & kFlipV) s += "↕";
    return s.empty() ? s : " " + s;
}

// What tiled.json calls a tileset: its file as the map writes it, or its name when it is in the map.
std::string tileset_key(const Tileset& ts) { return ts.source.empty() ? "@" + ts.name : ts.source; }

struct TileRef {
    const Tileset* ts = nullptr;
    u32 local = 0, flags = 0;
    const Tile* own = nullptr; // the tile the cell names (not its first frame)
};

// Finds a tile's picture; the reason when there is none (which list it goes in: missing, or skipped).
class Tiles {
public:
    Tiles(const Map& map, Pictures& pictures) : map_(map), pictures_(pictures) {}

    // The tile a cell shows: its first frame when animated.
    bool resolve(u32 gid, TileRef& out) const {
        out.flags = gid & (kFlipH | kFlipV | kFlipD);
        out.ts = tileset_of(map_, gid, out.local);
        if (!out.ts) return false;
        out.own = out.ts->tile(out.local);
        if (out.own && out.own->animated) out.local = out.own->first_frame;
        return true;
    }
    std::string key(const TileRef& r) const { return tileset_key(*r.ts) + "#" + std::to_string(r.local) + flags_text(r.flags); }
    std::string name(const TileRef& r) const {
        const Tile* t = r.ts->tile(r.local);
        const std::string base = t && !t->type.empty() ? t->type : r.ts->name + " " + std::to_string(r.local);
        return base + flags_words(r.flags);
    }
    // The picture, turned; false with *missing (a file not there) or *skipped (not something Forge has).
    bool picture(const TileRef& r, std::vector<u8>& out, u32& w, u32& h, std::string& missing, std::string& skipped) {
        const Tileset& ts = *r.ts;
        if (!ts.missing.empty()) {
            missing = "набор тайлов " + q(ts.source) + ": " + ts.missing;
            return false;
        }
        const Tile* t = ts.tile(r.local);
        if (ts.collection()) {
            if (!ts.has(r.local)) {
                skipped = "тайлы, которых нет в своём наборе (" + ts.name + ")";
                return false;
            }
            const Image& img = pictures_.get(t->image, false, nullptr);
            if (!img.ok) {
                missing = "картинка " + q(t->image_source) + " (" + ts.name + "): " + img.why;
                return false;
            }
            i32 x = t->x, y = t->y;
            u32 sw = t->w ? t->w : img.w, sh = t->h ? t->h : img.h;
            if (x < 0 || y < 0 || static_cast<u64>(x) + sw > img.w || static_cast<u64>(y) + sh > img.h) {
                skipped = "тайлы с частью картинки за её краем (" + ts.name + ")";
                return false;
            }
            turned(&img.rgba[(static_cast<usize>(y) * img.w + static_cast<usize>(x)) * 4], sw, sh, static_cast<usize>(img.w) * 4,
                   r.flags, out, w, h);
            return true;
        }
        const Image& img = pictures_.get(ts.image, ts.trans, ts.trans_rgb);
        if (!img.ok) {
            missing = "картинка " + q(ts.image_source) + " (" + ts.name + "): " + img.why;
            return false;
        }
        // Its tiles are cut from the picture as it is (Tiled counts them so, whatever tilecount says).
        const u32 step_w = ts.tile_w + ts.spacing, step_h = ts.tile_h + ts.spacing;
        const u32 columns = ts.tile_w && img.w + ts.spacing >= 2 * ts.margin + step_w ? (img.w - 2 * ts.margin + ts.spacing) / step_w : 0;
        const u32 rows = ts.tile_h && img.h + ts.spacing >= 2 * ts.margin + step_h ? (img.h - 2 * ts.margin + ts.spacing) / step_h : 0;
        if (static_cast<u64>(r.local) >= static_cast<u64>(columns) * rows) {
            skipped = "тайлы, которых нет в картинке своего набора (" + ts.name + ")";
            return false;
        }
        const u32 x = ts.margin + (r.local % columns) * step_w, y = ts.margin + (r.local / columns) * step_h;
        turned(&img.rgba[(static_cast<usize>(y) * img.w + x) * 4], ts.tile_w, ts.tile_h, static_cast<usize>(img.w) * 4, r.flags, out,
               w, h);
        return true;
    }

private:
    const Map& map_;
    Pictures& pictures_;
};

std::string template_id_for(const TileRef& r, const std::string& map_stem) {
    std::string base = "tiled_" + map_stem + "_" + (r.ts->source.empty() ? r.ts->name : path_to_utf8(utf8_path(r.ts->source).stem())) +
                       "_" + std::to_string(r.local) + flags_text(r.flags);
    // A file name on every system: letters and digits, the rest as "_".
    std::string out;
    for (const char c : base) {
        const unsigned char u = static_cast<unsigned char>(c);
        out += u >= 0x80 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ? c : '_';
    }
    return out;
}

// The id of a zone or an object the map makes: the same for the same map
// and Tiled object every time. An import taken back with Ctrl+Z and made
// again, or made again after its tiled.json was lost, gives its zones and
// objects their old ids: «Логика»'s links to a zone keep working, and the
// objects already in the level are found instead of doubled.
u64 stable_id(const std::string& map_name, std::string_view what, u32 tiled_id) {
    const u64 id = fnv1a("tiled|" + map_name + "|" + std::string(what) + "|" + std::to_string(tiled_id));
    return id ? id : 1;
}

template <typename K, typename V>
const V* lookup(const std::vector<std::pair<K, V>>& list, const K& k) {
    for (const auto& [a, b] : list)
        if (a == k) return &b;
    return nullptr;
}

} // namespace

world::TileId Plan::at(u32 layer_index, i32 x, i32 y) const {
    if (x < x0 || x >= x1 || y < y0 || y >= y1) return 0;
    const usize i = static_cast<usize>(y - y0) * static_cast<usize>(x1 - x0) + static_cast<usize>(x - x0);
    return layer_index == 0 ? walls[i] : blocks[i];
}

Target default_target(const Map& map, const Layer& layer) {
    if (layer.kind != Layer::Kind::Tiles || !layer.visible) return Target::Skip;
    if (is_true(layer.props, "solid") || is_true(layer.props, "collides") || is_true(layer.props, "collision")) return Target::Blocks;
    // All its tiles have collision shapes.
    bool any = false;
    for (const Chunk& c : layer.chunks)
        for (const u32 gid : c.gids) {
            if ((gid & ~kGidFlags) == 0) continue;
            u32 local = 0;
            const Tileset* ts = tileset_of(map, gid, local);
            const Tile* t = ts ? ts->tile(local) : nullptr;
            if (!t || !t->collision) return Target::Walls;
            any = true;
        }
    return any ? Target::Blocks : Target::Walls;
}

bool plan(const Map& map, const Options& o, const LevelTiles& own, const LevelAreas& areas, const Record& before, Plan& out,
          std::string* refusal) {
    out = {};
    auto refuse = [&](const std::string& why) {
        if (refusal) *refusal = why;
        return false;
    };
    out.map_name = path_to_utf8(map.file.filename());
    if (map.orientation != "orthogonal")
        return refuse("карта " + q(map.orientation) + ": Forge переносит только ортогональные карты (квадратная сетка)");
    if (map.tile_w != map.tile_h)
        return refuse("клетки карты " + std::to_string(map.tile_w) + " × " + std::to_string(map.tile_h) + " px: нужны квадратные");
    if (map.tile_w < kMinOwnTilePx || map.tile_w > kMaxOwnTilePx)
        return refuse("клетки карты " + std::to_string(map.tile_w) + " px: Forge берёт от " + std::to_string(kMinOwnTilePx) + " до " +
                      std::to_string(kMaxOwnTilePx) + " px");
    const u32 px = map.tile_w;
    out.px = px;
    if (!own.empty() && own.px != px)
        return refuse("у уровня уже есть свои тайлы стороной " + std::to_string(own.px) + " px, а клетки карты — " + std::to_string(px) +
                      " px");
    const std::string map_stem = path_to_utf8(map.file.stem());
    // An earlier import of another map: its record is replaced, what it made stays.
    const Record prev = before.map == out.map_name ? before : Record{};
    if (!before.empty() && before.map != out.map_name)
        note(out.skipped, "запись прежнего импорта карты " + q(before.map) + " заменяется (сделанное им остаётся как есть)");

    if (!map.background.empty()) note(out.skipped, "цвет фона карты " + map.background + " (за уровнем Forge — небо игры)");
    Pictures pictures;
    Tiles tiles(map, pictures);
    auto target_of = [&](usize li) { return li < o.targets.size() ? o.targets[li] : default_target(map, map.layers[li]); };

    // --- the cells: which layers, where, how far they move ---
    struct Source {
        usize layer;
        Target target;
        i32 dx = 0, dy = 0;
    };
    std::vector<Source> sources;
    bool rect = false;
    i64 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    auto grow = [&](i64 a0, i64 b0, i64 a1, i64 b1) {
        if (a1 <= a0 || b1 <= b0) return;
        if (!rect) x0 = a0, y0 = b0, x1 = a1, y1 = b1, rect = true;
        else x0 = std::min(x0, a0), y0 = std::min(y0, b0), x1 = std::max(x1, a1), y1 = std::max(y1, b1);
    };
    if (!map.infinite) grow(0, 0, map.w, map.h);
    for (usize li = 0; li < map.layers.size(); ++li) {
        const Layer& l = map.layers[li];
        if (l.kind == Layer::Kind::Image) {
            note(out.skipped, "слои-картинки (" + l.name + ")");
            continue;
        }
        if (l.kind != Layer::Kind::Tiles) continue;
        const Target t = target_of(li);
        if (t == Target::Skip) {
            if (!l.visible) note(out.skipped, "скрытые слои тайлов не переносятся: " + q(l.name));
            else note(out.skipped, "слои тайлов, которые не переносятся: " + q(l.name));
            continue;
        }
        Source s{li, t};
        const f64 fx = l.offset_x / px, fy = l.offset_y / px;
        s.dx = static_cast<i32>(std::lround(fx));
        s.dy = static_cast<i32>(std::lround(fy));
        if (fx != s.dx || fy != s.dy) note(out.skipped, "сдвиг слоя не на целую клетку, округлён: " + q(l.name));
        if (l.opacity < 1) note(out.skipped, "прозрачность слоя (в Forge слой непрозрачен): " + q(l.name));
        if (!l.tint.empty()) note(out.skipped, "оттенок слоя: " + q(l.name));
        if (!l.mode.empty()) note(out.skipped, "режим смешивания слоя: " + q(l.name));
        if (l.parallax_x != 1 || l.parallax_y != 1) note(out.skipped, "параллакс слоя: " + q(l.name));
        for (const Property& p : l.props)
            if (p.name != "solid" && p.name != "collides" && p.name != "collision")
                note(out.skipped, "свойства слоёв, кроме solid, collides и collision: " + q(l.name + ": " + p.name));
        for (const Chunk& c : l.chunks) grow(c.x + s.dx, c.y + s.dy, i64(c.x) + c.w + s.dx, i64(c.y) + c.h + s.dy);
        sources.push_back(s);
    }
    if (rect && static_cast<u64>(x1 - x0) * static_cast<u64>(y1 - y0) > kMaxImportCells)
        return refuse("карта покрывает " + std::to_string(x1 - x0) + " × " + std::to_string(y1 - y0) + " клеток: Forge переносит до " +
                      std::to_string(kMaxImportCells) + " клеток за раз");
    constexpr i64 kFar = static_cast<i64>(kMaxSpawn);
    if (rect && (x0 < -kFar || y0 < -kFar || x1 > kFar || y1 > kFar))
        return refuse("клетки карты слишком далеко от начала координат");
    out.x0 = static_cast<i32>(x0);
    out.y0 = static_cast<i32>(y0);
    out.x1 = static_cast<i32>(x1);
    out.y1 = static_cast<i32>(y1);
    const usize W = static_cast<usize>(out.x1 - out.x0), H = static_cast<usize>(out.y1 - out.y0);
    out.walls.assign(W * H, 0);
    out.blocks.assign(W * H, 0);

    // The stacks of tiles at each cell of each level layer, in Tiled's order: keys first, pictures after.
    std::vector<std::vector<u32>> stacks[2];
    stacks[0].assign(W * H, {});
    stacks[1].assign(W * H, {});
    std::vector<u8> blocks_drawn(W * H, 0); // a blocks layer has drawn here (to find background drawn over it)
    u64 above = 0;
    u64 unknown = 0;
    for (const Source& s : sources) {
        const Layer& l = map.layers[s.layer];
        const int to = s.target == Target::Walls ? 0 : 1;
        for (const Chunk& c : l.chunks)
            for (u32 cy = 0; cy < c.h; ++cy)
                for (u32 cx = 0; cx < c.w; ++cx) {
                    const u32 gid = c.gids[static_cast<usize>(cy) * c.w + cx];
                    if ((gid & ~kGidFlags) == 0) continue;
                    TileRef r;
                    if (!tiles.resolve(gid, r)) {
                        ++unknown;
                        continue;
                    }
                    const usize i = static_cast<usize>(c.y + static_cast<i32>(cy) + s.dy - out.y0) * W +
                                    static_cast<usize>(c.x + static_cast<i32>(cx) + s.dx - out.x0);
                    stacks[to][i].push_back(gid);
                    if (to == 1) blocks_drawn[i] = 1;
                    else above += blocks_drawn[i];
                }
    }
    if (unknown)
        return refuse(std::to_string(unknown) + " клеток с номером тайла меньше первого firstgid: такого тайла нет ни в одном наборе, карта испорчена");
    note(out.skipped, "клетки, где в Tiled фон над блоками (в Forge фон всегда под блоками)", above);

    // --- the level's own tiles: the stacks, by their keys ---
    out.tiles = own;
    std::vector<bool> used(max_own_tiles(px), false);
    for (const OwnTile& t : own.tiles)
        if (t.id >= kFirstOwnTile && static_cast<usize>(t.id - kFirstOwnTile) < used.size()) used[t.id - kFirstOwnTile] = true;
    usize next_free = 0;
    auto free_id = [&]() -> world::TileId {
        while (next_free < used.size() && used[next_free]) ++next_free;
        if (next_free >= used.size()) return 0;
        used[next_free] = true;
        return static_cast<world::TileId>(kFirstOwnTile + next_free);
    };
    // Earlier keys stay in the record while their tiles do: the same ids for the same tiles next time too.
    for (const auto& [key, id] : prev.tiles)
        if (own.find(id)) out.record.tiles.emplace_back(key, id);
    std::unordered_map<std::string, world::TileId> by_key;
    std::unordered_map<std::string, bool> refreshed;
    u64 no_room = 0;
    for (int to = 0; to < 2; ++to) {
        const u32 layer = to == 0 ? o.walls : o.blocks;
        std::vector<world::TileId>& cells = to == 0 ? out.walls : out.blocks;
        for (usize i = 0; i < W * H; ++i) {
            const std::vector<u32>& stack = stacks[to][i];
            if (stack.empty()) continue;
            std::string key = to == 0 ? "Фон|" : "Блоки|";
            std::vector<TileRef> refs;
            for (usize k = 0; k < stack.size(); ++k) {
                TileRef r;
                tiles.resolve(stack[k], r);
                refs.push_back(r);
                key += (k ? " + " : "") + tiles.key(r);
            }
            if (auto it = by_key.find(key); it != by_key.end()) {
                cells[i] = it->second;
                continue;
            }
            // The picture: the stack's tiles over each other; those without a picture are told and left out.
            std::vector<u8> pic, part;
            std::string name, from;
            usize drawn = 0;
            for (const TileRef& r : refs) {
                u32 w = 0, h = 0;
                std::string missing, skipped;
                if (!tiles.picture(r, part, w, h, missing, skipped)) {
                    if (!missing.empty()) note(out.missing, missing);
                    else note(out.skipped, skipped);
                    continue;
                }
                if (w != px || h != px) {
                    note(out.skipped, "тайлы не по размеру клетки карты (" + r.ts->name + ", " + std::to_string(w) + " × " + std::to_string(h) +
                                          " px)");
                    continue;
                }
                if (r.ts->offset_x || r.ts->offset_y) note(out.skipped, "сдвиг тайлов набора (tileoffset) не переносится: " + q(r.ts->name));
                if (r.own && r.own->animated) note(out.skipped, "анимации тайлов: стоит первый кадр — " + q(r.ts->name));
                if (r.own && r.own->collision && to == 0)
                    note(out.skipped, "тайлы с формой столкновений на «Фоне»: там они не твёрдые — " + q(r.ts->name));
                else if (r.own && r.own->collision && !r.own->collision_whole)
                    note(out.skipped, "формы столкновений не на всю клетку: в «Блоках» клетка твёрдая целиком — " + q(r.ts->name));
                if (drawn++ == 0) pic = part;
                else over(pic, part);
                name += (name.empty() ? "" : " + ") + tiles.name(r);
                from += (from.empty() ? "" : " + ") + tiles.key(r);
            }
            if (drawn == 0) {
                by_key.emplace(key, 0);
                continue;
            }
            world::TileId id = 0;
            if (const world::TileId* known = lookup(out.record.tiles, key)) id = *known;
            if (id == 0) {
                id = free_id();
                if (id == 0) {
                    ++no_room;
                    by_key.emplace(key, 0);
                    continue;
                }
                out.record.tiles.emplace_back(key, id);
            }
            by_key.emplace(key, id);
            cells[i] = id;
            OwnTile t;
            t.id = id;
            t.name = cut(without_control(name), kMaxOwnTileName);
            t.layer = layer;
            t.solid = to == 1;
            t.from = cut(without_control("Tiled: " + out.map_name + ", " + from), 200);
            auto at = std::find_if(out.tiles.tiles.begin(), out.tiles.tiles.end(), [&](const OwnTile& x) { return x.id == id; });
            if (at == out.tiles.tiles.end()) {
                out.tiles.px = px;
                out.tiles.tiles.push_back(t);
                out.tiles.rgba.insert(out.tiles.rgba.end(), pic.begin(), pic.end());
                ++out.tiles_new;
            } else {
                const usize k = static_cast<usize>(at - out.tiles.tiles.begin());
                const bool same = *at == t && std::equal(pic.begin(), pic.end(), out.tiles.rgba.begin() + static_cast<std::ptrdiff_t>(k * px * px * 4));
                *at = t;
                std::copy(pic.begin(), pic.end(), out.tiles.rgba.begin() + static_cast<std::ptrdiff_t>(k * px * px * 4));
                if (!same && !refreshed[key]) ++out.tiles_updated;
                refreshed[key] = true;
            }
        }
    }
    // Cells refused for want of room, counted per cell.
    if (no_room)
        note(out.skipped, "разных тайлов и их стопок больше, чем помещается в уровень (" + std::to_string(max_own_tiles(px)) +
                              "): лишние клетки пусты");
    // Cells whose stacks have no picture at all are empty: count them where they are told.
    {
        u64 empty_cells = 0;
        for (int to = 0; to < 2; ++to)
            for (usize i = 0; i < W * H; ++i)
                if (!stacks[to][i].empty() && (to == 0 ? out.walls[i] : out.blocks[i]) == 0) ++empty_cells;
        note(out.skipped, "клетки, оставшиеся пустыми (их тайлы выше в этом списке)", empty_cells);
    }

    // --- objects ---
    out.areas = areas;
    std::vector<u64> keep_zones;
    bool spawn_set = false;
    std::unordered_map<std::string, usize> picture_index;
    for (usize li = 0; li < map.layers.size(); ++li) {
        const Layer& l = map.layers[li];
        if (l.kind != Layer::Kind::Objects) continue;
        if (!l.visible) {
            note(out.skipped, "объекты скрытых слоёв (" + l.name + ")", l.objects.size());
            continue;
        }
        if (l.offset_x || l.offset_y) note(out.skipped, "сдвиг слоя объектов не переносится: " + q(l.name));
        // A tile layer drawn over these objects.
        for (usize lj = li + 1; lj < map.layers.size(); ++lj)
            if (map.layers[lj].kind == Layer::Kind::Tiles && target_of(lj) != Target::Skip &&
                std::any_of(l.objects.begin(), l.objects.end(), [](const Object& x) { return x.gid != 0; })) {
                note(out.skipped, "слои тайлов над объектами (в Forge объекты всегда над тайлами): " + q(map.layers[lj].name));
                break;
            }
        for (const Object& obj : l.objects) {
            const std::string who = "объект " + std::to_string(obj.id) + (obj.name.empty() ? "" : " " + q(obj.name));
            if (!obj.template_missing.empty()) {
                note(out.missing, "шаблон " + q(obj.template_source) + " (" + who + "): " + obj.template_missing);
                continue;
            }
            if (!obj.visible) {
                note(out.skipped, "скрытые объекты");
                continue;
            }
            if (obj.rotation != 0) {
                note(out.skipped, "повёрнутые объекты");
                continue;
            }
            for (const Property& p : obj.props)
                if (p.name != "music") note(out.skipped, "свойства объектов, кроме music: " + q(p.name));
            if (obj.gid != 0) {
                // A picture.
                TileRef r;
                if (!tiles.resolve(obj.gid, r))
                    return refuse(who + ": номер тайла меньше первого firstgid, такого тайла нет ни в одном наборе — карта испорчена");
                const std::string key = tiles.key(r);
                usize pi;
                if (auto it = picture_index.find(key); it != picture_index.end()) pi = it->second;
                else {
                    Picture p;
                    std::string missing, skipped;
                    if (!tiles.picture(r, p.rgba, p.w, p.h, missing, skipped)) {
                        if (!missing.empty()) note(out.missing, missing);
                        else note(out.skipped, skipped);
                        continue;
                    }
                    p.key = key;
                    p.name = cut(without_control(tiles.name(r)), 60);
                    if (const std::string* id = lookup(prev.templates, key)) {
                        p.template_id = *id;
                        p.known = true;
                    } else {
                        std::string made = template_id_for(r, map_stem);
                        const std::string base = made;
                        for (int n = 2; o.template_taken && o.template_taken(made); ++n) made = base + "_" + std::to_string(n);
                        p.template_id = made;
                    }
                    pi = out.pictures.size();
                    picture_index.emplace(key, pi);
                    out.pictures.push_back(std::move(p));
                }
                const Picture& p = out.pictures[pi];
                f64 w = obj.w, h = obj.h;
                if (w <= 0 || h <= 0) w = p.w, h = p.h;
                if (std::fabs(w / h - static_cast<f64>(p.w) / p.h) > 0.01 * (static_cast<f64>(p.w) / p.h))
                    note(out.skipped, "объекты-тайлы, растянутые не в пропорции картинки: ширина — по картинке");
                // Where on the object its x, y is: the tileset's alignment; bottom left on orthogonal maps by default.
                f64 ax = 0, ay = 1;
                const std::string& al = r.ts->alignment;
                if (al == "topleft") ax = 0, ay = 0;
                else if (al == "top") ax = 0.5, ay = 0;
                else if (al == "topright") ax = 1, ay = 0;
                else if (al == "left") ax = 0, ay = 0.5;
                else if (al == "center") ax = 0.5, ay = 0.5;
                else if (al == "right") ax = 1, ay = 0.5;
                else if (al == "bottom") ax = 0.5, ay = 1;
                else if (al == "bottomright") ax = 1, ay = 1;
                PlannedObject po;
                po.tiled_id = obj.id;
                po.name = obj.name;
                po.picture = pi;
                po.x = (obj.x - ax * w + w * 0.5) / px;
                po.y = (obj.y - ay * h + h * 0.5) / px;
                po.half_height = h / px * 0.5;
                if (const u64* id = lookup(prev.objects, obj.id)) {
                    po.level_id = *id;
                    po.known = true;
                } else {
                    po.level_id = stable_id(out.map_name, "object", obj.id);
                }
                out.record.objects.emplace_back(obj.id, po.level_id);
                out.objects.push_back(std::move(po));
                continue;
            }
            switch (obj.shape) {
            case Object::Shape::Point:
                if (lower_ascii_equal(obj.name, "spawn") || lower_ascii_equal(obj.type, "spawn")) {
                    if (spawn_set) {
                        note(out.skipped, "вторая и следующие точки появления");
                        break;
                    }
                    spawn_set = true;
                    out.areas.spawn = true;
                    out.areas.spawn_x = obj.x / px;
                    out.areas.spawn_y = obj.y / px;
                    out.record.spawn = obj.id;
                } else {
                    note(out.skipped, "точки, кроме точки появления (spawn)");
                }
                break;
            case Object::Shape::Rect: {
                if (obj.w <= 0 || obj.h <= 0) {
                    note(out.skipped, "прямоугольники нулевого размера");
                    break;
                }
                Area a;
                a.x0 = static_cast<i32>(std::lround(obj.x / px));
                a.y0 = static_cast<i32>(std::lround(obj.y / px));
                a.x1 = std::max(a.x0 + 1, static_cast<i32>(std::lround((obj.x + obj.w) / px)));
                a.y1 = std::max(a.y0 + 1, static_cast<i32>(std::lround((obj.y + obj.h) / px)));
                if (std::fmod(obj.x, px) != 0 || std::fmod(obj.y, px) != 0 || std::fmod(obj.w, px) != 0 || std::fmod(obj.h, px) != 0)
                    note(out.skipped, "зоны не по границам клеток: края сдвинуты к ближайшим");
                if (a.x1 - a.x0 > kMaxAreaSide || a.y1 - a.y0 > kMaxAreaSide) {
                    note(out.skipped, "зоны больше " + std::to_string(kMaxAreaSide) + " клеток в сторону");
                    break;
                }
                std::string name = obj.name.empty() ? obj.type : obj.name;
                name = cut(without_control(name), kMaxAreaName);
                if (const Property* m = find_property(obj.props, "music"); m && !m->value.empty()) {
                    fs::path file;
                    const fs::path p = utf8_path(m->value);
                    file = (p.is_absolute() ? p : map.file.parent_path() / p).lexically_normal();
                    std::error_code ec;
                    if (!fs::is_regular_file(file, ec)) note(out.missing, "музыка " + q(m->value) + " (" + who + "): нет файла");
                    else {
                        a.music = path_to_utf8(file.filename());
                        if (std::none_of(out.music.begin(), out.music.end(), [&](const auto& x) { return x.first == file; }))
                            out.music.emplace_back(file, a.music);
                    }
                }
                const u64* known = lookup(prev.zones, obj.id);
                Area* old = known ? out.areas.find(*known) : nullptr;
                // Not in the record (taken back, the record lost): the zone an earlier import made has its id.
                if (!old) old = out.areas.find(stable_id(out.map_name, "zone", obj.id));
                a.id = old ? old->id : stable_id(out.map_name, "zone", obj.id);
                // Names are told apart: the first free of "Имя", "Имя (2)", …
                if (name.empty()) name = "Зона " + std::to_string(obj.id);
                const std::string base = name;
                for (int n = 2; std::any_of(out.areas.areas.begin(), out.areas.areas.end(),
                                            [&](const Area& x) { return x.id != a.id && x.name == name; });
                     ++n)
                    name = cut(base, kMaxAreaName - 5) + " (" + std::to_string(n) + ")";
                if (name != base) note(out.skipped, "зоны с именем другой зоны: переименованы");
                a.name = name;
                if (old) {
                    if (*old != a) ++out.zones_updated;
                    *old = a;
                } else {
                    if (out.areas.areas.size() >= kMaxAreas) {
                        note(out.skipped, "зоны сверх " + std::to_string(kMaxAreas));
                        break;
                    }
                    out.areas.areas.push_back(a);
                    ++out.zones_new;
                }
                keep_zones.push_back(a.id);
                out.record.zones.emplace_back(obj.id, a.id);
                break;
            }
            case Object::Shape::Ellipse: note(out.skipped, "объекты-эллипсы"); break;
            case Object::Shape::Polygon: note(out.skipped, "объекты-многоугольники"); break;
            case Object::Shape::Polyline: note(out.skipped, "объекты-ломаные"); break;
            case Object::Shape::Text: note(out.skipped, "текстовые объекты"); break;
            case Object::Shape::Capsule: note(out.skipped, "объекты-капсулы"); break;
            }
        }
    }
    // An earlier import's zones and objects the map no longer has go.
    for (const auto& [tid, zid] : prev.zones)
        if (std::find(keep_zones.begin(), keep_zones.end(), zid) == keep_zones.end()) {
            const auto n = out.areas.areas.size();
            std::erase_if(out.areas.areas, [&](const Area& a) { return a.id == zid; });
            out.zones_removed += static_cast<u32>(n - out.areas.areas.size());
        }
    for (const auto& [tid, lid] : prev.objects)
        if (!lookup(out.record.objects, tid)) out.remove_objects.push_back(lid);
    // The spawn point an earlier import set, when the map has none now, goes too.
    if (!spawn_set && prev.spawn != 0) out.areas.spawn = false;

    for (const Picture& p : out.pictures) out.record.templates.emplace_back(p.key, p.template_id);
    // Earlier templates stay remembered (their objects may come back).
    for (const auto& [key, id] : prev.templates)
        if (!lookup(out.record.templates, key)) out.record.templates.emplace_back(key, id);
    out.record.map = out.map_name;
    out.record.x0 = out.x0;
    out.record.y0 = out.y0;
    out.record.x1 = out.x1;
    out.record.y1 = out.y1;
    if (const std::string problem = tiles_problem(out.tiles, o.layer_count, o.liquids); !problem.empty())
        return refuse("тайлы уровня после импорта: " + problem);
    return true;
}

// --- tiled.json ---

namespace {

struct Doc {
    yyjson_doc* d = nullptr;
    ~Doc() { yyjson_doc_free(d); }
};

std::string hex(u64 v) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

bool from_hex(std::string_view s, u64& out) {
    if (s.size() != 16) return false;
    out = 0;
    for (const char c : s) {
        u64 d;
        if (c >= '0' && c <= '9') d = static_cast<u64>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<u64>(c - 'a' + 10);
        else return false;
        out = out << 4 | d;
    }
    return out != 0;
}

} // namespace

std::string record_json(const Record& r) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strncpy(doc, root, "map", r.map.data(), r.map.size());
    yyjson_mut_val* cells = yyjson_mut_arr(doc);
    for (const i32 v : {r.x0, r.y0, r.x1, r.y1}) yyjson_mut_arr_add_sint(doc, cells, v);
    yyjson_mut_obj_add_val(doc, root, "cells", cells);
    if (r.spawn) yyjson_mut_obj_add_uint(doc, root, "spawn", r.spawn);
    yyjson_mut_val* tiles = yyjson_mut_obj(doc);
    for (const auto& [k, id] : r.tiles) yyjson_mut_obj_add(tiles, yyjson_mut_strncpy(doc, k.data(), k.size()), yyjson_mut_uint(doc, id));
    yyjson_mut_obj_add_val(doc, root, "tiles", tiles);
    auto ids = [&](const char* name, const std::vector<std::pair<u32, u64>>& list) {
        yyjson_mut_val* v = yyjson_mut_obj(doc);
        for (const auto& [tid, id] : list) {
            const std::string k = std::to_string(tid), h = hex(id);
            yyjson_mut_obj_add(v, yyjson_mut_strncpy(doc, k.data(), k.size()), yyjson_mut_strncpy(doc, h.data(), h.size()));
        }
        yyjson_mut_obj_add_val(doc, root, name, v);
    };
    ids("objects", r.objects);
    ids("zones", r.zones);
    yyjson_mut_val* templates = yyjson_mut_obj(doc);
    for (const auto& [k, id] : r.templates)
        yyjson_mut_obj_add(templates, yyjson_mut_strncpy(doc, k.data(), k.size()), yyjson_mut_strncpy(doc, id.data(), id.size()));
    yyjson_mut_obj_add_val(doc, root, "templates", templates);
    usize len = 0;
    char* text = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY_TWO_SPACES, &len);
    std::string out = text ? std::string(text, len) + "\n" : std::string();
    std::free(text);
    yyjson_mut_doc_free(doc);
    return out;
}

bool load_record(const fs::path& folder, Record& out, bool* found, std::string* error) {
    if (found) *found = false;
    const fs::path file = folder / kRecordFile;
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        out = {};
        return true;
    }
    if (found) *found = true;
    auto fail = [&](const std::string& why) {
        if (error) *error = std::string(kRecordFile) + ": " + why;
        return false;
    };
    std::vector<u8> bytes;
    if (!read_file(file, bytes)) return fail("файл не читается");
    Doc doc;
    yyjson_read_err err{};
    doc.d = yyjson_read_opts(reinterpret_cast<char*>(bytes.data()), bytes.size(), 0, nullptr, &err);
    yyjson_val* root = doc.d ? yyjson_doc_get_root(doc.d) : nullptr;
    if (!yyjson_is_obj(root)) return fail("файл испорчен: не JSON-объект");
    Record r;
    yyjson_val* map = yyjson_obj_get(root, "map");
    if (!yyjson_is_str(map) || yyjson_get_len(map) == 0) return fail("«map» — не имя карты");
    r.map.assign(yyjson_get_str(map), yyjson_get_len(map));
    yyjson_val* cells = yyjson_obj_get(root, "cells");
    if (!yyjson_is_arr(cells) || yyjson_arr_size(cells) != 4) return fail("«cells» — не четыре числа");
    i32* edge[4] = {&r.x0, &r.y0, &r.x1, &r.y1};
    for (usize i = 0; i < 4; ++i) {
        yyjson_val* v = yyjson_arr_get(cells, i);
        if (!yyjson_is_int(v)) return fail("«cells» — не четыре числа");
        *edge[i] = static_cast<i32>(yyjson_get_sint(v));
    }
    if (yyjson_val* s = yyjson_obj_get(root, "spawn"); s) {
        if (!yyjson_is_uint(s) || yyjson_get_uint(s) > 0xFFFFFFFFull) return fail("«spawn» — не id объекта");
        r.spawn = static_cast<u32>(yyjson_get_uint(s));
    }
    usize i, n;
    yyjson_val *k, *v;
    yyjson_val* tiles = yyjson_obj_get(root, "tiles");
    if (!yyjson_is_obj(tiles)) return fail("«tiles» — не объект");
    yyjson_obj_foreach(tiles, i, n, k, v) {
        if (!yyjson_is_uint(v) || yyjson_get_uint(v) < kFirstOwnTile || yyjson_get_uint(v) > 0xFFFF) return fail("«tiles»: не id тайла");
        r.tiles.emplace_back(std::string(yyjson_get_str(k), yyjson_get_len(k)), static_cast<world::TileId>(yyjson_get_uint(v)));
    }
    auto ids = [&](const char* name, std::vector<std::pair<u32, u64>>& list) {
        yyjson_val* obj = yyjson_obj_get(root, name);
        if (!yyjson_is_obj(obj)) return false;
        usize a, b;
        yyjson_val *key, *val;
        yyjson_obj_foreach(obj, a, b, key, val) {
            i64 tid = 0;
            u64 id = 0;
            const std::string ks(yyjson_get_str(key), yyjson_get_len(key));
            char* end = nullptr;
            tid = std::strtoll(ks.c_str(), &end, 10);
            if (ks.empty() || *end || tid <= 0 || tid > 0xFFFFFFFFll || !yyjson_is_str(val) ||
                !from_hex({yyjson_get_str(val), yyjson_get_len(val)}, id))
                return false;
            list.emplace_back(static_cast<u32>(tid), id);
        }
        return true;
    };
    if (!ids("objects", r.objects)) return fail("«objects» — не список id");
    if (!ids("zones", r.zones)) return fail("«zones» — не список id");
    yyjson_val* templates = yyjson_obj_get(root, "templates");
    if (!yyjson_is_obj(templates)) return fail("«templates» — не объект");
    yyjson_obj_foreach(templates, i, n, k, v) {
        if (!yyjson_is_str(v)) return fail("«templates»: не id шаблона");
        r.templates.emplace_back(std::string(yyjson_get_str(k), yyjson_get_len(k)), std::string(yyjson_get_str(v), yyjson_get_len(v)));
    }
    out = std::move(r);
    return true;
}

bool save_record(const fs::path& folder, const Record& r, std::string* error) {
    std::error_code ec;
    if (r.empty()) {
        fs::remove(folder / kRecordFile, ec);
        if (ec) {
            if (error) *error = "не удалось убрать " + path_to_utf8(folder / kRecordFile);
            return false;
        }
        return true;
    }
    const std::string text = record_json(r);
    if (!folder.empty()) fs::create_directories(folder, ec);
    if (folder.empty() || text.empty() || !write_file_atomic(folder / kRecordFile, {reinterpret_cast<const u8*>(text.data()), text.size()})) {
        if (error) *error = "не удалось записать " + path_to_utf8(folder / kRecordFile);
        return false;
    }
    return true;
}

} // namespace forge::level::tiled
