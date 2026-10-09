#include "forge/level/tiled_apply.h"

#include "forge/level/areas.h"
#include "forge/level/tile_edit.h"

#include "forge/assets/image.h"
#include "forge/core/file.h"
#include "forge/core/hash.h"
#include "forge/core/log.h"
#include "forge/core/path.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <unordered_map>

namespace forge::level::tiled {

namespace fs = std::filesystem;

namespace {

std::string q(std::string_view s) { return "«" + std::string(s) + "»"; }

std::string label_of(const Plan& p) { return "Импорт карты Tiled " + q(p.map_name); }

std::string number(f64 v) {
    char b[40];
    std::snprintf(b, sizeof b, "%.9g", v);
    return b;
}

world::Rect tiles_of(world::ChunkCoord c) {
    return {c.x * world::kChunkSize, c.y * world::kChunkSize, (c.x + 1) * world::kChunkSize, (c.y + 1) * world::kChunkSize};
}

world::Rect around(f64 x, f64 y) {
    const i32 tx = static_cast<i32>(std::floor(x)), ty = static_cast<i32>(std::floor(y));
    return {tx - 2, ty - 2, tx + 3, ty + 3};
}

// Loads the places around these points, all at once, unless they are loaded.
void need(Level& level, const std::vector<world::Rect>& rects) {
    for (const world::Rect& r : rects)
        if (!level.loaded(r.x0, r.y0) || !level.loaded(r.x1 - 1, r.y1 - 1)) {
            level.ensure_loaded(rects);
            return;
        }
}

bool made_by_import(const objects::Template& t) {
    return t.kind == kPictureKind && t.picture.size() > t.id.size() + 1 && t.picture.compare(0, t.id.size() + 1, t.id + "_") == 0;
}

// The half height of a «Картинка», kept within what its property allows.
f64 half_height(const objects::PropDef* prop, f64 v) {
    if (prop && prop->has_range) v = std::clamp(v, prop->min, prop->max);
    return v;
}

const objects::PropDef* half_prop(const objects::Library& library, const objects::Template& t) {
    return library.prop_of(t, "half_height");
}

// The level's cells after the import, chunk by chunk.
struct Cells {
    std::vector<SetChunks::Change> changes;
    u64 cells = 0, author_cells = 0;
};

bool plan_cells(Level& level, const Plan& p, const ApplyOptions& o, bool keep, Cells& out, std::string& refusal) {
    world::World& w = level.world();
    const u32 layers = w.layer_count();
    if (o.walls >= layers || o.blocks >= layers || o.walls == o.blocks) {
        refusal = "слои уровня для тайлов карты выбраны неверно";
        return false;
    }
    const world::Rect rect{p.x0, p.y0, p.x1, p.y1};
    if (rect.empty()) return true;
    const world::Rect cr = world::chunks_of(rect);
    if (const world::Rect& b = w.bounds(); !b.empty() && (cr.x0 < b.x0 || cr.y0 < b.y0 || cr.x1 > b.x1 || cr.y1 > b.y1)) {
        refusal = "карта выходит за край мира игры: клетки x " + std::to_string(b.x0 * world::kChunkSize) + "…" +
                  std::to_string(b.x1 * world::kChunkSize - 1) + ", y " + std::to_string(b.y0 * world::kChunkSize) + "…" +
                  std::to_string(b.y1 * world::kChunkSize - 1);
        return false;
    }
    w.finish_loading();
    std::vector<world::TileId> before, after, made;
    for (i32 cy = cr.y0; cy < cr.y1; ++cy)
        for (i32 cx = cr.x0; cx < cr.x1; ++cx) {
            const world::ChunkCoord c{cx, cy};
            if (!w.peek_chunk(c, before)) {
                refusal = "участок уровня " + std::to_string(cx) + ", " + std::to_string(cy) + " не читается";
                return false;
            }
            const bool edited = w.edited(c);
            // What the game makes here: cells that differ from it were changed by the author (or an import).
            if (edited) level.generate(c, level.around(), made);
            // A chunk nobody changed is what is around after the import, outside the map's rectangle.
            if (!edited && level.around().empty_around != o.empty_around) level.generate(c, LevelWorld{o.empty_around}, after);
            else after = before;
            const world::Rect t = tiles_of(c).clipped(rect);
            for (i32 y = t.y0; y < t.y1; ++y)
                for (i32 x = t.x0; x < t.x1; ++x) {
                    const u32 li = world::local_index(x, y);
                    bool differs = false, author = false;
                    for (u32 l = 0; l < layers; ++l) {
                        const world::TileId v = l == o.walls ? p.at(0, x, y) : l == o.blocks ? p.at(1, x, y) : 0;
                        const usize i = static_cast<usize>(l) * world::kChunkTiles + li;
                        after[i] = v;
                        differs |= before[i] != v;
                        author |= edited && before[i] != made[i];
                    }
                    if (differs) {
                        ++out.cells;
                        if (author) ++out.author_cells;
                    }
                }
            if (after == before) continue;
            SetChunks::Change ch;
            ch.chunk = c;
            ch.edited = edited;
            if (keep) {
                ch.before = world::encode_chunk(before.data(), layers);
                ch.after = world::encode_chunk(after.data(), layers);
            }
            out.changes.push_back(std::move(ch));
        }
    return true;
}

// Every object with a LevelId that is loaded, by its id.
std::unordered_map<u64, flecs::entity> objects_by_id(Level& level) {
    std::unordered_map<u64, flecs::entity> out;
    level.scene().ecs().each([&](flecs::entity e, const LevelId& l) {
        if (l.id) out.emplace(l.id, e);
    });
    return out;
}

// A planned object as bytes: made from its template where it stands, packed, and taken away again.
bool make_snapshot(Level& level, const objects::Library& library, const Plan& p, const PlannedObject& po, ObjectSnapshot& out,
                   std::string& why) {
    const Picture& pic = p.pictures[po.picture];
    const objects::Template* t = library.find(std::string_view(pic.template_id));
    if (!t) {
        why = "нет шаблона " + q(pic.template_id);
        return false;
    }
    const objects::PropDef* prop = half_prop(library, *t);
    const f64 hh = half_height(prop, po.half_height);
    flecs::entity e = library.spawn(level.scene(), *t, po.x, po.y + hh);
    if (!e.is_valid()) {
        why = "место объекта " + q(po.name) + " не загрузилось";
        return false;
    }
    e.set<scene::Position>(scene::Position::at_tile(po.x, po.y));
    e.set<LevelId>({po.level_id});
    if (prop) {
        const f64 th = t->value("half_height") ? std::strtod(t->value("half_height")->c_str(), nullptr) : -1;
        if (std::fabs(th - hh) > 1e-9) library.set_value(level.scene(), e, *prop, number(hh));
    }
    out.id = po.level_id;
    out.x = po.x;
    out.y = po.y;
    out.bytes = level.scene().pack(e);
    e.destruct();
    return true;
}

// What differs between an object of an earlier import as it is and as the map has it; "" when nothing.
std::string difference(Level& level, const objects::Library& library, const Plan& p, const PlannedObject& po, flecs::entity e) {
    const Picture& pic = p.pictures[po.picture];
    const objects::Template* t = library.find(std::string_view(pic.template_id));
    const objects::ObjectRef* ref = e.try_get<objects::ObjectRef>();
    if (!t || !ref || ref->key != t->key) return "другая картинка";
    std::string out;
    if (const scene::Position* pos = e.try_get<scene::Position>();
        !pos || std::fabs(pos->tile_x() - po.x) > 1e-4 || std::fabs(pos->tile_y() - po.y) > 1e-4)
        out = "сдвинут";
    if (const objects::PropDef* prop = half_prop(library, *t)) {
        const f64 now = std::strtod(library.value(level.scene(), e, *prop).c_str(), nullptr);
        if (std::fabs(now - half_height(prop, po.half_height)) > 1e-4) out += out.empty() ? "другой размер" : ", другой размер";
    }
    return out;
}

std::string object_name(const PlannedObject& po, const Plan& p) {
    std::string n = po.name.empty() ? p.pictures[po.picture].name : po.name;
    return n + " (" + std::to_string(po.tiled_id) + ")";
}

} // namespace

std::string picture_file(const Picture& p) {
    u64 h = fnv1a(std::string_view(reinterpret_cast<const char*>(p.rgba.data()), p.rgba.size()));
    h = fnv1a_u64(p.w, h);
    h = fnv1a_u64(p.h, h);
    char b[20];
    std::snprintf(b, sizeof b, "%08x", static_cast<u32>(h ^ (h >> 32)));
    return p.template_id + "_" + b + ".png";
}

bool template_taken(const objects::Library& library, const std::string& id) {
    const objects::Template* t = library.find(std::string_view(id));
    return t && !made_by_import(*t);
}

void game_objects(Level& level, const world::Rect& rect, bool outside, std::vector<flecs::entity>& out) {
    out.clear();
    world::World& w = level.world();
    std::unordered_map<world::ChunkCoord, bool, world::ChunkCoordHash> edited;
    level.scene().ecs().each([&](flecs::entity e, const scene::Position& pos) {
        if (const LevelId* l = e.try_get<LevelId>(); l && l->id) return;
        const f64 x = pos.tile_x(), y = pos.tile_y();
        if (x >= rect.x0 && x < rect.x1 && y >= rect.y0 && y < rect.y1) {
            out.push_back(e);
            return;
        }
        if (!outside) return;
        const world::ChunkCoord c = pos.chunk();
        auto it = edited.find(c);
        if (it == edited.end()) it = edited.emplace(c, w.edited(c)).first;
        if (!it->second) out.push_back(e);
    });
}

Preview preview(Level& level, const objects::Library& library, const Plan& plan, const ApplyOptions& o) {
    Preview out;
    const world::Rect rect{plan.x0, plan.y0, plan.x1, plan.y1};
    Cells cells;
    if (!plan_cells(level, plan, o, false, cells, out.refusal)) return out;
    out.cells = cells.cells;
    out.author_cells = cells.author_cells;
    out.around = level.around().empty_around != o.empty_around;
    if (!rect.empty()) level.ensure_loaded(rect);
    level.load_objects();
    std::vector<flecs::entity> game;
    game_objects(level, rect, out.around && o.empty_around, game);
    out.game_objects = static_cast<u32>(game.size());
    const auto by_id = objects_by_id(level);
    for (const PlannedObject& po : plan.objects) {
        // Not in the record but in the level (an import made again after its tiled.json was lost): the same object.
        auto it = by_id.find(po.level_id);
        if (it == by_id.end() && !po.known) ++out.objects_new;
        else if (it == by_id.end()) out.objects_back.push_back(object_name(po, plan) + ": удалён, вернётся");
        else if (const std::string d = difference(level, library, plan, po, it->second); !d.empty())
            out.objects_back.push_back(object_name(po, plan) + ": " + d);
        else ++out.objects_same;
    }
    for (const u64 id : plan.remove_objects)
        if (by_id.count(id)) ++out.objects_removed;
    for (const Picture& p : plan.pictures) {
        const objects::Template* t = library.find(std::string_view(p.template_id));
        if (!t) ++out.templates_new;
        else if (t->picture != picture_file(p)) ++out.templates_updated;
    }
    return out;
}

bool write_resources(Plan& plan, objects::Library& library, const fs::path& sounds, Resources& made, std::string* error) {
    made = {};
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    const objects::KindDef* kind = library.kind(kPictureKind);
    if (!kind && !plan.pictures.empty()) return fail("в игре нет вида объектов «Картинка» (" + std::string(kPictureKind) + ")");
    const fs::path pictures = library.pictures_folder();
    std::error_code ec;

    // What was there, to take back.
    std::vector<fs::path> created; // files that were not there
    std::vector<objects::Template> replaced;
    std::vector<u64> added;
    auto undo = [&] {
        for (auto it = replaced.rbegin(); it != replaced.rend(); ++it) library.put(*it);
        for (const u64 key : added) library.remove(key);
        for (const fs::path& f : created) fs::remove(f, ec);
        made = {};
    };

    // --- the pictures: all into a folder aside first, then moved in ---
    struct Pic {
        std::string name;
        std::vector<u8> png;
        bool there = false; // the same file is there already
    };
    std::vector<Pic> pics;
    for (const Picture& p : plan.pictures) {
        Pic x;
        x.name = picture_file(p);
        assets::CookedTexture img;
        img.width = p.w;
        img.height = p.h;
        img.rgba8 = p.rgba;
        if (!assets::encode_image(img, ".png", x.png)) return fail("картинка шаблона " + q(p.name) + " не кодируется в PNG");
        std::vector<u8> old;
        x.there = fs::is_regular_file(pictures / utf8_path(x.name), ec) && read_file(pictures / utf8_path(x.name), old) && old == x.png;
        pics.push_back(std::move(x));
    }
    if (std::any_of(pics.begin(), pics.end(), [](const Pic& x) { return !x.there; })) {
        fs::create_directories(pictures, ec);
        if (ec) return fail("не создаётся папка картинок " + path_to_utf8(pictures) + ": " + ec.message());
        static std::mt19937_64 rng{std::random_device{}()};
        char tag[20];
        std::snprintf(tag, sizeof tag, "%08x", static_cast<u32>(rng()));
        const fs::path aside = pictures / utf8_path(std::string(".tiled-import-") + tag);
        fs::create_directories(aside, ec);
        if (ec) return fail("не создаётся временная папка " + path_to_utf8(aside) + ": " + ec.message());
        for (const Pic& x : pics) {
            if (x.there) continue;
            if (!write_file_atomic(aside / utf8_path(x.name), x.png)) {
                fs::remove_all(aside, ec);
                return fail("не записалась картинка " + q(x.name) + " в " + path_to_utf8(aside) + "; ничего не изменилось");
            }
        }
        for (const Pic& x : pics) {
            if (x.there) continue;
            const fs::path to = pictures / utf8_path(x.name);
            const bool existed = fs::exists(to, ec);
            fs::rename(aside / utf8_path(x.name), to, ec);
            if (ec) {
                const std::string why = ec.message();
                fs::remove_all(aside, ec);
                undo();
                return fail("картинка " + q(x.name) + " не перенеслась в " + path_to_utf8(pictures) + ": " + why + "; ничего не изменилось");
            }
            if (!existed) created.push_back(to);
            made.pictures.push_back(x.name);
        }
        fs::remove_all(aside, ec);
    }

    // --- the zones' music into the game's sounds ---
    for (auto& [file, name] : plan.music) {
        std::vector<u8> bytes;
        if (!read_file(file, bytes)) {
            undo();
            return fail("не читается музыка " + path_to_utf8(file) + "; ничего не изменилось");
        }
        // The same sound there already, or a name nobody has.
        std::string to_name = name;
        const fs::path stem = utf8_path(name).stem(), ext = utf8_path(name).extension();
        bool there = false;
        for (int n = 2;; ++n) {
            const fs::path to = sounds / utf8_path(to_name);
            if (!fs::exists(to, ec)) break;
            std::vector<u8> old;
            if (read_file(to, old) && old == bytes) {
                there = true;
                break;
            }
            to_name = path_to_utf8(stem) + " (" + std::to_string(n) + ")" + path_to_utf8(ext);
        }
        if (!there) {
            fs::create_directories(sounds, ec);
            const fs::path to = sounds / utf8_path(to_name);
            if (!write_file_atomic(to, bytes)) {
                undo();
                return fail("не записалась музыка " + q(to_name) + " в " + path_to_utf8(sounds) + "; ничего не изменилось");
            }
            created.push_back(to);
            made.sounds.push_back(to_name);
        }
        if (to_name != name) {
            for (Area& a : plan.areas.areas)
                if (a.music == name) a.music = to_name;
            name = to_name;
        }
    }

    // --- the templates ---
    std::vector<std::string> old_pictures;
    for (usize i = 0; i < plan.pictures.size(); ++i) {
        const Picture& p = plan.pictures[i];
        const objects::Template* had = library.find(std::string_view(p.template_id));
        objects::Template t;
        if (had) {
            t = *had;
        } else {
            std::optional<objects::Template> fresh = library.make(*kind, nullptr, p.name);
            if (!fresh) {
                undo();
                return fail("не делается шаблон " + q(p.name) + "; ничего не изменилось");
            }
            t = std::move(*fresh);
            t.id = p.template_id;
            t.about = "Картинка из карты Tiled " + q(plan.map_name) + ".";
        }
        t.picture = pics[i].name;
        const objects::PropDef* prop = library.prop_of(t, "half_height");
        const f64 hh = half_height(prop, static_cast<f64>(p.h) / plan.px * 0.5);
        t = library.with_value(t, "half_height", number(hh));
        if (had && had->picture == t.picture && had->values == t.values) continue;
        if (had) {
            if (had->picture != t.picture) old_pictures.push_back(had->picture);
            replaced.push_back(*had);
        }
        std::string why;
        if (!library.put(t, &why)) {
            if (had) replaced.pop_back();
            undo();
            return fail("не записался шаблон " + q(t.name) + ": " + why + "; ничего не изменилось");
        }
        if (!had) added.push_back(t.key);
        made.templates.push_back(t.id);
    }
    // Pictures of earlier imports no template shows any more.
    for (const std::string& name : old_pictures) {
        if (std::any_of(library.templates().begin(), library.templates().end(), [&](const objects::Template& t) { return t.picture == name; }))
            continue;
        fs::remove(pictures / utf8_path(name), ec);
    }
    return true;
}

bool apply(Level& level, editor::UndoStack& history, const objects::Library& library, const Plan& plan, const ApplyOptions& o,
           std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    const std::string label = label_of(plan);
    if (const std::string problem =
            tiles_problem(plan.tiles, level.world().layer_count(), level.module().liquids_layer());
        !problem.empty())
        return fail("тайлы уровня после импорта: " + problem);
    for (const Area& a : plan.areas.areas)
        if (const std::string problem = area_problem(a); !problem.empty()) return fail("зона " + q(a.name) + ": " + problem);
    const world::Rect rect{plan.x0, plan.y0, plan.x1, plan.y1};
    Cells cells;
    std::string why;
    if (!plan_cells(level, plan, o, true, cells, why)) return fail(why);

    // Objects: an earlier import's are found wherever they are.
    level.load_objects();
    std::vector<world::Rect> places;
    for (const PlannedObject& po : plan.objects) places.push_back(around(po.x, po.y));
    need(level, places);
    const auto by_id = objects_by_id(level);
    std::vector<ObjectSnapshot> gone, made;
    for (const PlannedObject& po : plan.objects) {
        auto it = by_id.find(po.level_id);
        if (it != by_id.end()) {
            if (difference(level, library, plan, po, it->second).empty()) continue;
            gone.push_back(snapshot(level, it->second));
        }
        ObjectSnapshot s;
        if (!make_snapshot(level, library, plan, po, s, why)) return fail(why);
        made.push_back(std::move(s));
    }
    for (const u64 id : plan.remove_objects)
        if (auto it = by_id.find(id); it != by_id.end()) gone.push_back(snapshot(level, it->second));

    const LevelWorld around_after{o.empty_around};
    const bool around_changes = level.around() != around_after;
    const bool tiles_change = level.own_tiles() != plan.tiles;
    const bool areas_change = level.areas() != plan.areas;
    const bool record_changes = level.tiled_record() != plan.record;
    if (!tiles_change && !around_changes && cells.changes.empty() && gone.empty() && made.empty() && !areas_change && !record_changes)
        return true;

    history.seal();
    history.begin_group(label);
    if (tiles_change) history.execute(std::make_unique<SetOwnTiles>(level, level.own_tiles(), plan.tiles, label));
    history.execute(std::make_unique<TakeGameObjects>(level, rect, around_changes && o.empty_around, label));
    if (around_changes) history.execute(std::make_unique<SetAround>(level, level.around(), around_after, label));
    if (!cells.changes.empty()) history.execute(std::make_unique<SetChunks>(level, std::move(cells.changes), label));
    if (!gone.empty() || !made.empty()) history.execute(std::make_unique<PutObjects>(level, std::move(gone), std::move(made), label));
    if (areas_change) history.execute(std::make_unique<SetAreas>(level, level.areas(), plan.areas, label));
    if (record_changes) history.execute(std::make_unique<SetRecord>(level, level.tiled_record(), plan.record, label));
    history.end_group();
    history.seal();
    return true;
}

// --- commands ---

SetChunks::SetChunks(Level& level, std::vector<Change> changes, std::string label)
    : level_(level), changes_(std::move(changes)), label_(std::move(label)) {}

void SetChunks::set(bool after) {
    if (changes_.empty()) return;
    std::vector<world::Rect> rects;
    for (const Change& c : changes_) rects.push_back(tiles_of(c.chunk));
    level_.ensure_loaded(rects);
    const u32 layers = level_.world().layer_count();
    std::vector<world::TileId> tiles(static_cast<usize>(layers) * world::kChunkTiles);
    for (const Change& c : changes_) {
        const std::vector<u8>& bytes = after ? c.after : c.before;
        if (!world::decode_chunk(bytes.data(), bytes.size(), tiles.data(), layers)) continue;
        // Back as it was: a chunk nobody had changed is the generator's again.
        if (!level_.set_chunk_tiles(c.chunk, tiles, after || c.edited))
            FORGE_WARN("Импорт Tiled: участок %d, %d не загрузился", c.chunk.x, c.chunk.y);
    }
}

SetAround::SetAround(Level& level, LevelWorld before, LevelWorld after, std::string label)
    : level_(level), before_(before), after_(after), label_(std::move(label)) {}

TakeGameObjects::TakeGameObjects(Level& level, world::Rect rect, bool outside, std::string label)
    : level_(level), rect_(rect), outside_(outside), label_(std::move(label)) {}

void TakeGameObjects::apply(editor::Document&) {
    gone_.clear();
    if (!rect_.empty()) level_.ensure_loaded(rect_);
    level_.load_objects();
    std::vector<flecs::entity> game;
    game_objects(level_, rect_, outside_, game);
    for (flecs::entity e : game) {
        ObjectSnapshot s;
        const scene::Position& p = *e.try_get<scene::Position>();
        s.x = p.tile_x();
        s.y = p.tile_y();
        s.bytes = level_.scene().pack(e);
        gone_.push_back(std::move(s));
    }
    for (flecs::entity e : game) e.destruct();
    if (!game.empty()) level_.touch_objects();
}

void TakeGameObjects::revert(editor::Document&) {
    if (gone_.empty()) return;
    std::vector<world::Rect> rects;
    for (const ObjectSnapshot& s : gone_) rects.push_back(around(s.x, s.y));
    need(level_, rects);
    for (const ObjectSnapshot& s : gone_) level_.scene().unpack(s.bytes);
    gone_.clear();
    level_.touch_objects();
}

PutObjects::PutObjects(Level& level, std::vector<ObjectSnapshot> gone, std::vector<ObjectSnapshot> made, std::string label)
    : level_(level), gone_(std::move(gone)), made_(std::move(made)), label_(std::move(label)) {}

void PutObjects::set(const std::vector<ObjectSnapshot>& out, const std::vector<ObjectSnapshot>& in) {
    std::vector<world::Rect> rects;
    for (const ObjectSnapshot& s : out) rects.push_back(around(s.x, s.y));
    for (const ObjectSnapshot& s : in) rects.push_back(around(s.x, s.y));
    need(level_, rects);
    auto by_id = objects_by_id(level_);
    for (const ObjectSnapshot& s : out)
        if (auto it = by_id.find(s.id); it != by_id.end()) {
            it->second.destruct();
            by_id.erase(it);
        }
    for (const ObjectSnapshot& s : in)
        if (!by_id.count(s.id)) level_.scene().unpack(s.bytes);
    level_.touch_objects();
}

SetRecord::SetRecord(Level& level, Record before, Record after, std::string label)
    : level_(level), before_(std::move(before)), after_(std::move(after)), label_(std::move(label)) {}

} // namespace forge::level::tiled
