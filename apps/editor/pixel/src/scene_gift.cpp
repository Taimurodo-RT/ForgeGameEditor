#include "forge/pixel/scene_gift.h"

#include "forge/pixel/screen.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <span>

namespace forge::pixel {

namespace {

using vector::MotionClip;
using vector::MotionFile;
using vector::MotionPart;
using vector::MotionPartPose;
using vector::MotionPiece;
using vector::MotionProp;
using vector::MotionTrack;
using vector::MotionValue;

constexpr usize kProps = vector::kMotionPropCount;
constexpr f64 kCheckRate = 30; // the check's moments a second
constexpr f64 kCheckWait = 0.5; // how long the check stands at each stop

i64 ns_of(f64 t) { return std::llround(t * 1e9); }

vector::MotionMatrix matrix_of(const Affine& a) { return {a.a, a.b, a.c, a.d, a.e, a.f}; }

// Seconds as the tab writes them: to thousandths, "3,2".
std::string seconds_words(f64 v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", std::round(v * 1000) / 1000);
    std::string out = buf;
    std::replace(out.begin(), out.end(), '.', ',');
    return out;
}

// A property in the author's words.
const char* prop_words(MotionProp p) {
    static constexpr const char* kWords[] = {"место по x", "место по y", "поворот", "масштаб", "масштаб по x", "масштаб по y",
                                             "наклон по x", "наклон по y", "прозрачность", "заливка", "обводка", "толщина обводки",
                                             "видимость", "морфинг", "пунктир", "прорисовка от", "прорисовка до", "тон",
                                             "вспышка", "цвет вспышки", "маска", "крепление", "рисунок"};
    static_assert(std::size(kWords) == kProps);
    return kWords[static_cast<usize>(p)];
}

// An id under an actor's name, as prefix_pose puts it: none stays none.
std::string under(const std::string& pre, const std::string& id) { return id.empty() ? id : pre + id; }

// What a value names under the actor's name: a mask's part, a link's carrier, a morph's shape.
void prefix_value(MotionProp p, MotionValue& v, const std::string& pre) {
    if (p == MotionProp::Mask || p == MotionProp::Link || p == MotionProp::Morph) v.target = under(pre, v.target);
}

// A clip of an actor's character with its ids under the actor's name, as the tab poses it (prefix_pose): its tracks
// and what their values name, its copies and its order. The drawing's track keeps its part and drawings their names.
void prefix_clip(MotionClip& c, const std::string& name) {
    const std::string pre = name + "/";
    for (MotionTrack& t : c.tracks) {
        if (t.prop == MotionProp::Drawing) continue;
        t.part = pre + t.part;
        for (MotionPiece& pc : t.pieces) {
            prefix_value(t.prop, pc.value, pre);
            prefix_value(t.prop, pc.from, pre);
            prefix_value(t.prop, pc.to, pre);
            for (MotionValue& v : pc.frames) prefix_value(t.prop, v, pre);
        }
    }
    for (vector::MotionCopy& k : c.copies) {
        k.id = pre + k.id;
        k.source = pre + k.source;
    }
    for (vector::MotionOrderStep& s : c.order) {
        s.part = under(pre, s.part);
        s.other = under(pre, s.other);
    }
}

MotionPart record(const Rig& r, i32 i, std::string id) {
    MotionPart p;
    p.id = std::move(id);
    p.pivot = r.pivot(i);
    p.frame = matrix_of(r.rest_frame(i));
    p.own = matrix_of(r.rest_local(i));
    return p;
}

// The part a copy of a clip is of; the id itself for none.
std::string source_of(const MotionClip& c, const std::string& id) {
    for (const vector::MotionCopy& k : c.copies)
        if (k.id == id) return k.source;
    return id;
}

// The actor whose part «ёжик/head» is, by the stage's items; empty for none.
std::string owner_of(const Stage& stage, std::string_view id) {
    const usize slash = id.find('/');
    if (slash == std::string_view::npos) return {};
    const StageItem* it = stage.item(id.substr(0, slash));
    return it && it->kind == StageItem::Kind::Actor ? it->id : std::string();
}

const MotionTrack* track_of(const MotionClip& c, std::string_view part, MotionProp p) {
    for (const MotionTrack& t : c.tracks)
        if (t.prop == p && t.part == part) return &t;
    return nullptr;
}

// Whether the file keeps track (part, p) of clip `c` at t exactly: no track (the part at rest), or its piece then a hold,
// a transition, or a frames piece at one of its moments (between two it mixes them, as on a screen of the game).
bool exact(const MotionClip& c, std::string_view part, MotionProp p, f64 t) {
    const MotionTrack* tr = track_of(c, part, p);
    if (!tr || tr->pieces.empty()) return true;
    const i64 n = ns_of(std::clamp(t, 0.0, std::max(c.length, 0.0)));
    const MotionPiece* pc = &tr->pieces.front();
    for (const MotionPiece& x : tr->pieces)
        if (ns_of(x.at) <= n) pc = &x;
    if (pc->kind != MotionPiece::Kind::Frames || pc->frames.size() < 2 || n <= ns_of(pc->at) || n >= ns_of(pc->end) - 1) return true;
    const auto i = static_cast<usize>(std::max(0.0, std::round((static_cast<f64>(n) / 1e9 - pc->at) * vector::kMotionRate)));
    return i + 1 < pc->frames.size() && ns_of(vector::motion_frame_moment(pc->at, i)) == n;
}

// A part's value as the game poses it: its track's, else at rest.
MotionValue game_value(const std::vector<MotionPartPose>& parts, std::string_view id, MotionProp p) {
    for (const MotionPartPose& pp : parts)
        if (pp.id == id && pp.has[static_cast<usize>(p)]) return vector::motion_plain(p, pp.values[static_cast<usize>(p)]);
    return vector::motion_rest(p);
}

std::vector<std::string> copy_ids(const std::vector<PartCopy>& copies) {
    std::vector<std::string> out;
    for (const PartCopy& c : copies) out.push_back(c.id);
    std::sort(out.begin(), out.end());
    return out;
}
std::vector<std::string> copy_ids(const std::vector<vector::MotionCopy>& copies) {
    std::vector<std::string> out;
    for (const vector::MotionCopy& c : copies) out.push_back(c.id);
    std::sort(out.begin(), out.end());
    return out;
}

bool same_order(const std::vector<OrderStep>& tab, const std::vector<vector::MotionOrderStep>& game) {
    if (tab.size() != game.size()) return false;
    for (usize i = 0; i < tab.size(); ++i) {
        const OrderStep& a = tab[i];
        const vector::MotionOrderStep& b = game[i];
        const bool beside = a.kind == OrderStep::Kind::Above || a.kind == OrderStep::Kind::Below;
        if (a.part != b.part || static_cast<u8>(a.kind) != static_cast<u8>(b.kind) || (beside && a.other != b.other)) return false;
    }
    return true;
}

bool near(f64 a, f64 b) { return std::abs(a - b) <= 1e-9 * std::max({1.0, std::abs(a), std::abs(b)}); }

} // namespace

namespace {

// Where each actor's drawing reaches beyond its canvas (part 6г2 of docs/models/сцены-модель.md): its pictures posed as
// the tab poses them at every 1/30 s and at each start and end of a clip, a stop and a transition, with no waits and
// with 0.5 s at each stop; in its picture's units, whole and outward, its canvas within. w 0: within its canvas.
bool measure_reach(Scene& scene, const Recording& rec, std::vector<vector::SceneActor>& actors, std::string& why) {
    std::set<i64> moments;
    auto add = [&](f64 t) {
        if (t >= 0 && t <= rec.length) moments.insert(ns_of(t));
    };
    for (i64 k = 0; static_cast<f64>(k) / kCheckRate <= rec.length; ++k) add(static_cast<f64>(k) / kCheckRate);
    add(rec.length);
    for (const ActorPlay& p : rec.plays) {
        add(p.start);
        add(p.start + p.length);
    }
    for (const SceneStop& s : rec.stops) add(s.time);
    for (const SceneTransition& t : rec.transitions) {
        add(t.start);
        add(t.start + t.length);
    }
    std::vector<std::array<f64, 4>> box(actors.size());
    for (usize a = 0; a < actors.size(); ++a) box[a] = {1e300, 1e300, -1e300, -1e300};
    auto at = [&](f64 t, std::span<const f64> held) {
        if (!scene.frame(t, held)) return why = scene.error(), false;
        for (usize a = 0; a < actors.size(); ++a) {
            f64 r[4];
            if (!scene.actor_reach(actors[a].id, r)) continue;
            box[a] = {std::min(box[a][0], r[0]), std::min(box[a][1], r[1]), std::max(box[a][2], r[2]), std::max(box[a][3], r[3])};
        }
        return true;
    };
    const std::vector<f64> waited(rec.stops.size(), kCheckWait);
    for (const i64 n : moments) {
        const f64 t = static_cast<f64>(n) / 1e9;
        if (!at(t, {})) return false;
        if (!rec.stops.empty() && !at(t, waited)) return false;
    }
    for (usize a = 0; a < actors.size(); ++a) {
        vector::SceneActor& it = actors[a];
        const f64 cx0 = it.origin.x, cy0 = it.origin.y, cx1 = it.origin.x + it.w, cy1 = it.origin.y + it.h;
        const std::array<f64, 4>& b = box[a];
        constexpr f64 kEps = 1e-6;
        if (b[0] > b[2] || (b[0] >= cx0 - kEps && b[1] >= cy0 - kEps && b[2] <= cx1 + kEps && b[3] <= cy1 + kEps)) continue;
        const f64 x0 = std::floor(std::min(b[0], cx0)), y0 = std::floor(std::min(b[1], cy0));
        const f64 x1 = std::ceil(std::max(b[2], cx1)), y1 = std::ceil(std::max(b[3], cy1));
        it.reach = {x0, y0, x1 - x0, y1 - y0};
    }
    return true;
}

bool build(Scene& scene, std::string_view name, std::string_view from, const SceneFiles& files, SceneGift& out, std::string& why) {
    if (!scene.loaded()) return why = scene.error().empty() ? "сцена не загружена" : scene.error(), false;
    if (!file_name_ok(name)) return why = "имя сцены «" + std::string(name) + "» не годится для имени папки", false;
    if (!scene.record()) return why = scene.error(), false;
    const Recording& rec = *scene.recording();
    const Stage& stage = scene.stage();
    const std::string scene_name(name);

    // The folder's files, one a name as Windows keeps them.
    std::map<std::string, std::string> taken; // folded path → whose
    auto take = [&](const std::string& path, const std::string& whose) {
        const auto [it, fresh] = taken.emplace(folded(path), whose);
        if (!fresh) why = it->second + " и " + whose + " дали бы один файл «" + path + "»";
        return fresh;
    };
    auto put = [&](std::string path, std::string bytes) { out.files.push_back({std::move(path), std::move(bytes)}); };
    // The stage's ids are letters, digits, _ and - (draw.cpp): each makes a file's name, two of them one file only by
    // their capitals.
    std::vector<const StageItem*> actors;
    for (const StageItem& it : stage.items)
        if (it.kind == StageItem::Kind::Actor) actors.push_back(&it);

    // The stage's clip: its shapes, its items' groups, the camera, the transitions' frames and its moves of the actors'
    // parts, as a screen's clip of the stage's rig.
    std::vector<std::string> more{kCameraPart};
    for (const char* l : kLayerParts) more.emplace_back(l);
    for (const StageItem* a : actors) {
        const Rig* r = scene.actor_rig(a->id);
        if (!r) return why = "актёр «" + a->id + "» не стоит на сцене", false;
        auto add = [&](const Rig& d) {
            for (const Part& p : d.parts())
                if (std::find(more.begin(), more.end(), p.id) == more.end()) more.push_back(p.id);
        };
        add(*r);
        for (const std::string& d : r->drawings()) add(*r->drawing(d));
    }
    PoseScript& script = scene.script();
    script.set_lengths("scene", scene.doc().lengths);
    PixelDoc sd;
    sd.params = scene.doc().params;
    sd.seed = scene.doc().seed;
    MotionFile& sf = out.motion.stage;
    sf.character = scene_name;
    sf.from = std::string(from);
    sf.picture = "stage.svg";
    {
        MotionClip c;
        if (!screen_clip(script, scene.rig(), sd, "scene", sf, c, why, more)) return why = "сцена: " + why, false;
        sf.clips.push_back(std::move(c));
    }
    const MotionClip& stage_clip = sf.clips.front();

    // Each actor's clips: each play's clip recorded with the values it plays it at, the same clip at other values another
    // clip of the file, «walk 2», by its first play.
    vector::SceneFile& file = out.motion.file;
    std::vector<MotionFile>& actor_files = out.motion.actors;
    actor_files.resize(actors.size());
    struct Played {
        std::string actor, clip, as;
        std::map<std::string, f64> values;
    };
    std::vector<Played> played;
    auto actor_index = [&](const std::string& id) -> i32 {
        for (usize i = 0; i < actors.size(); ++i)
            if (actors[i]->id == id) return static_cast<i32>(i);
        return -1;
    };
    for (const ActorPlay& p : rec.plays) {
        vector::ScenePlay sp;
        sp.kind = static_cast<vector::ScenePlay::Kind>(p.kind);
        sp.actor = p.actor;
        sp.at = p.start;
        sp.speed = p.speed;
        sp.order = p.order;
        if (p.kind != ActorPlay::Kind::Stop) sp.blend = p.blend;
        const i32 a = actor_index(p.actor);
        if (a < 0) return why = "на сцене нет актёра «" + p.actor + "»", false;
        if (p.kind != ActorPlay::Kind::Stop) {
            std::map<std::string, f64> values;
            PoseScript* s = scene.actor_script(p, values, why);
            if (!s) return why = "актёр «" + p.actor + "», клип «" + p.clip + "»: " + why, false;
            auto same = std::find_if(played.begin(), played.end(), [&](const Played& o) { return o.actor == p.actor && o.clip == p.clip && o.values == values; });
            if (same == played.end()) {
                MotionFile& af = actor_files[static_cast<usize>(a)];
                const std::vector<std::string>& theirs = s->clips();
                std::string as = p.clip;
                for (u32 n = 2; af.clip(as) || (as != p.clip && std::find(theirs.begin(), theirs.end(), as) != theirs.end()); ++n)
                    as = p.clip + " " + std::to_string(n);
                const SceneCharacter* c = scene.character(p.actor);
                PixelDoc cd;
                cd.params = values;
                cd.seed = c->doc.seed;
                cd.clips = c->doc.clips;
                MotionClip clip;
                if (!screen_clip(*s, c->rig, cd, p.clip, af, clip, why)) return why = "актёр «" + p.actor + "», " + why, false;
                clip.name = as;
                prefix_clip(clip, p.actor);
                af.clips.push_back(std::move(clip));
                same = played.insert(played.end(), {p.actor, p.clip, as, std::move(values)});
            }
            sp.clip = same->as;
        }
        file.plays.push_back(std::move(sp));
    }

    // The parts the clips move or link and every copy, with where they stand at rest.
    auto moved_of = [](const MotionClip& c, std::vector<std::string>& moved) {
        for (const MotionTrack& t : c.tracks)
            if ((vector::motion_prop_moves(t.prop) || t.prop == MotionProp::Link) && std::find(moved.begin(), moved.end(), t.part) == moved.end())
                moved.push_back(t.part);
    };
    std::vector<std::string> stage_moved;
    moved_of(stage_clip, stage_moved);
    const vector::Point middle{stage.width() / 2, stage.height() / 2};
    // A part of the stage's clip where it stands at rest: the camera and the frames round the canvas's middle, an
    // actor's part in its rig, in its main drawing or else the first that has it.
    auto stage_record = [&](const std::string& id, const std::string& as, MotionPart& p) {
        if (camera_part(id)) {
            p = {as, middle, {}, {}};
            return true;
        }
        if (const i32 i = scene.rig().part(id); i >= 0) return p = record(scene.rig(), i, as), true;
        if (const std::string a = owner_of(stage, id); !a.empty()) {
            const Rig& r = *scene.actor_rig(a);
            if (const i32 i = r.part(id); i >= 0) return p = record(r, i, as), true;
            for (const std::string& d : r.drawings())
                if (const i32 i = r.drawing(d)->part(id); i >= 0) return p = record(*r.drawing(d), i, as), true;
        }
        return false;
    };
    for (const std::string& id : stage_moved) {
        if (source_of(stage_clip, id) != id) continue; // a copy: below, by its source
        MotionPart p;
        if (!stage_record(id, id, p)) return why = "сцена: часть «" + id + "» не стоит на рисунке сцены", false;
        sf.parts.push_back(std::move(p));
    }
    for (const vector::MotionCopy& k : stage_clip.copies) {
        MotionPart p;
        if (!stage_record(k.source, k.id, p)) return why = "сцена: часть «" + k.source + "» копии «" + k.id + "» не стоит на рисунке сцены", false;
        sf.parts.push_back(std::move(p));
    }

    // What the game will not draw: the clips' words, and a mask by an actor or a picture (its own layer is not the shape
    // a mask needs, part 6г).
    std::set<std::string> not_drawn(stage_clip.not_drawn.begin(), stage_clip.not_drawn.end());
    // (A part of an actor masks its own actor's parts only: the recording refuses the rest.)
    auto by_item = [&](const MotionValue& v) { return v.v != 0 && stage.item(v.target); };
    for (const MotionTrack& t : stage_clip.tracks)
        if (t.prop == MotionProp::Mask)
            for (const MotionPiece& pc : t.pieces)
                if (by_item(pc.value) || std::any_of(pc.frames.begin(), pc.frames.end(), by_item)) not_drawn.insert("mask_by_item");

    // The actors' files.
    std::vector<std::string> stage_morphs;
    for (const MotionTrack& t : stage_clip.tracks)
        if (t.prop == MotionProp::Morph && owner_of(stage, t.part).empty()) stage_morphs.push_back(source_of(stage_clip, t.part));
    for (usize a = 0; a < actors.size(); ++a) {
        const StageItem& it = *actors[a];
        const std::string pre = it.id + "/";
        const SceneCharacter& c = *scene.character(it.id);
        const Rig& ar = *scene.actor_rig(it.id);
        MotionFile& af = actor_files[a];
        af.character = it.id;
        af.from = it.file + ".pixel.json";
        af.picture = it.id + ".svg";
        std::vector<std::string> moved;
        std::set<std::string> morphed; // the character's ids
        std::vector<vector::MotionCopy> copies;
        for (const MotionClip& clip : af.clips) {
            moved_of(clip, moved);
            for (const MotionTrack& t : clip.tracks)
                if (t.prop == MotionProp::Morph) morphed.insert(source_of(clip, t.part).substr(pre.size()));
            for (const vector::MotionCopy& k : clip.copies)
                if (std::none_of(copies.begin(), copies.end(), [&](const vector::MotionCopy& o) { return o.id == k.id; })) copies.push_back(k);
            not_drawn.insert(clip.not_drawn.begin(), clip.not_drawn.end());
        }
        // The scene's moves of the actor's parts go over its clips' (scene_over): their matrices are the actor's.
        for (const std::string& id : stage_moved)
            if (owner_of(stage, id) == it.id && std::find(moved.begin(), moved.end(), id) == moved.end()) moved.push_back(id);
        for (const MotionTrack& t : stage_clip.tracks)
            if (t.prop == MotionProp::Morph && owner_of(stage, t.part) == it.id) morphed.insert(source_of(stage_clip, t.part).substr(pre.size()));
        auto parts_of = [&](const Rig& r) {
            std::vector<MotionPart> ps;
            for (const std::string& id : moved)
                if (const i32 i = r.part(id); i >= 0) ps.push_back(record(r, i, id));
            for (const vector::MotionCopy& k : copies)
                if (const i32 i = r.part(k.source); i >= 0) ps.push_back(record(r, i, k.id));
            return ps;
        };
        af.parts = parts_of(ar);
        // Its pictures as its tab draws them, the shapes it morphs written as <path>, its ids under its name.
        const std::vector<std::string> morph_ids(morphed.begin(), morphed.end());
        const std::string main = c.by_code ? drawing_svg(drawing_with_paths(c.drawn, morph_ids), c.doc.frame_w, c.doc.frame_h) : c.svg;
        const std::string whose = "актёр «" + it.id + "»";
        if (!take("actors/" + af.picture, whose)) return false;
        put("actors/" + af.picture, prefix_svg_ids(main, it.id));
        // An actor that plays no clip has no file of clips (a file of none does not read) nor its other drawings (only a
        // clip shows them): the scene's moves of its parts stand by stage.motion.json's records.
        if (af.clips.empty()) {
            file.actors.push_back({it.id, it.file, "actors/" + af.picture, "", it.origin, it.w, it.h, {}});
            continue;
        }
        std::vector<const NamedDrawing*> named;
        for (const NamedDrawing& d : c.drawings) named.push_back(&d);
        std::sort(named.begin(), named.end(), [](const NamedDrawing* x, const NamedDrawing* y) { return x->name < y->name; });
        for (const NamedDrawing* d : named) {
            if (!file_name_ok(d->name)) return why = whose + ": рисунок «" + d->name + "» — такое имя не годится для файла", false;
            const Rig* dr = ar.drawing(d->name);
            if (!dr || dr == &ar) return why = whose + ": рисунок «" + d->name + "» не стоит на актёре", false;
            vector::MotionDrawing md;
            md.name = d->name;
            md.picture = it.id + "." + d->name + ".svg";
            md.parts = parts_of(*dr);
            if (!take("actors/" + md.picture, "рисунок «" + d->name + "» актёра «" + it.id + "»")) return false;
            put("actors/" + md.picture,
                prefix_svg_ids(with_root_of(drawing_svg(drawing_with_paths(d->drawing, morph_ids), c.doc.frame_w, c.doc.frame_h), main), it.id));
            af.drawings.push_back(std::move(md));
        }
        if (!take("actors/" + it.id + ".motion.json", whose)) return false;
        put("actors/" + it.id + ".motion.json", vector::write_motion_file(af));
        file.actors.push_back({it.id, it.file, "actors/" + af.picture, "actors/" + it.id + ".motion.json", it.origin, it.w, it.h, {}});
    }

    // The pictures: each file of «Ресурсы» once, by the first picture of it.
    std::map<std::string, std::string> pictures; // its path in «Ресурсы» → in the folder
    for (const StageItem& it : stage.items) {
        if (it.kind != StageItem::Kind::Picture) continue;
        auto p = pictures.find(it.file);
        if (p == pictures.end()) {
            const std::string path = "pictures/" + it.id + ".png";
            if (!take(path, "картинка «" + it.id + "»")) return false;
            std::vector<u8> bytes;
            std::string err;
            if (!files.read || !files.read(it.file, bytes, err)) return why = err.empty() ? "нет картинки «" + it.file + "»" : err, false;
            put(path, std::string(bytes.begin(), bytes.end()));
            p = pictures.emplace(it.file, path).first;
        }
        file.pictures.push_back({it.id, p->second, it.x, it.y, it.w, it.h});
    }

    // The stage's picture: its shapes and the items' groups, the background in scene.json.
    Stage bare = stage;
    bare.background.clear();
    bare.drawing = drawing_with_paths(stage.drawing, stage_morphs);
    take("stage.svg", "сцена");
    put("stage.svg", stage_svg(bare));
    take("stage.motion.json", "сцена");
    put("stage.motion.json", vector::write_motion_file(sf));

    file.name = scene_name;
    file.from = std::string(from);
    file.width = stage.width();
    file.height = stage.height();
    file.background = stage.background;
    file.length = rec.length;
    for (const SceneMusic& m : rec.music) file.music.push_back({m.start, m.order, m.name, m.fade, m.volume});
    for (const SceneStop& s : rec.stops) file.stops.push_back({s.time, s.order, s.kind == SceneStop::Kind::Talk, s.name});
    for (const SceneTransition& t : rec.transitions) file.transitions.push_back({t.leave, t.kind, t.from, t.start, t.length});
    std::string left;
    for (const char* w : vector::kMotionNotDrawn)
        if (not_drawn.count(w)) {
            file.not_drawn.emplace_back(w);
            left += (left.empty() ? "" : ", ") + std::string(vector::motion_not_drawn_words(w));
        }
    if (not_drawn.count("mask_by_item")) {
        file.not_drawn.emplace_back("mask_by_item");
        left += (left.empty() ? "" : ", ") + std::string("маска по актёру или картинке");
    }
    if (!left.empty()) out.notes.push_back("В игре не будет: " + left);
    if (!measure_reach(scene, rec, file.actors, why)) return false;
    take("scene.json", "сцена");
    put("scene.json", vector::write_scene_file(file));
    std::sort(out.files.begin(), out.files.end(), [](const SceneGift::File& x, const SceneGift::File& y) { return x.path < y.path; });

    // The files as a game reads them, checked against the tab.
    vector::SceneMotion back;
    auto text = [&](std::string_view path) -> const std::string& {
        for (const SceneGift::File& f : out.files)
            if (f.path == path) return f.bytes;
        static const std::string none;
        return none;
    };
    std::string err;
    if (!vector::read_scene_file(text("scene.json"), back.file, err)) return why = "scene.json не читается: " + err, false;
    if (!vector::read_motion_file(text("stage.motion.json"), back.stage, err)) return why = "stage.motion.json не читается: " + err, false;
    for (const vector::SceneActor& a : back.file.actors) {
        back.actors.emplace_back();
        if (!a.motion.empty() && !vector::read_motion_file(text(a.motion), back.actors.back(), err))
            return why = a.motion + " не читается: " + err, false;
    }
    out.motion = std::move(back);
    return check_scene_gift(scene, out.motion, why);
}

} // namespace

bool build_scene_gift(Scene& scene, std::string_view name, std::string_view from, const SceneFiles& files, SceneGift& out, std::string& why) {
    out = {};
    if (build(scene, name, from, files, out, why)) return true;
    out = {};
    return false;
}

bool check_scene_gift(Scene& scene, const vector::SceneMotion& motion, std::string& why) {
    const Recording* rec = scene.recording();
    if (!rec) return why = "сцена не записана", false;
    if (!vector::check_scene(motion, why)) return false;
    const vector::SceneFile& f = motion.file;
    const MotionClip& stage_clip = *motion.stage.clip("scene");

    std::set<i64> moments;
    auto add = [&](f64 t) {
        if (t >= 0 && t <= f.length) moments.insert(ns_of(t));
    };
    for (i64 k = 0; static_cast<f64>(k) / kCheckRate <= f.length; ++k) add(static_cast<f64>(k) / kCheckRate);
    add(f.length);
    for (usize i = 0; i < f.plays.size(); ++i) {
        const vector::ScenePlay& p = f.plays[i];
        add(p.at);
        const i32 a = [&] {
            for (usize k = 0; k < f.actors.size(); ++k)
                if (f.actors[k].id == p.actor) return static_cast<i32>(k);
            return -1;
        }();
        const MotionClip* c = a >= 0 && p.kind != vector::ScenePlay::Kind::Stop ? motion.actors[static_cast<usize>(a)].clip(p.clip) : nullptr;
        if (c && p.kind == vector::ScenePlay::Kind::Play) add(p.at + c->length / p.speed);
        // Part 6г5а: a blend's start, middle and end in the scene's seconds (no waits).
        if (c && p.blend > 0 && c->length > 0) {
            const f64 lasts = (p.kind == vector::ScenePlay::Kind::Loop ? p.blend : std::min(p.blend, c->length)) / p.speed;
            add(p.at + lasts / 2);
            add(p.at + lasts);
        }
    }
    for (const vector::SceneStop& s : f.stops) add(s.at);
    for (const vector::SceneTransition& t : f.transitions) {
        add(t.at);
        add(t.at + t.length);
    }
    for (const vector::SceneMusicCall& m : f.music) add(m.at);

    // One moment, with these waits: false with why at the first thing that differs.
    auto one = [&](f64 t, std::span<const f64> held, i32 waiting, const std::string& waits) {
        ScenePoses tab;
        if (!scene.poses(t, held, waiting, tab)) return why = scene.error(), false;
        const vector::ScenePose game = vector::scene_pose(motion, t, held, waiting);
        auto differs = [&](const std::string& what) {
            why = "в игре сцена разошлась бы со вкладкой: " + seconds_words(t) + " с" + waits + ", " + what;
            return false;
        };
        if (tab.phase != game.phase) return differs("вход, сцена или уход");
        const std::vector<MusicVoice> heard = music_at(*rec, t, held, waiting);
        const std::vector<vector::SceneMusicVoice> played = vector::scene_music_at(f, t, held, waiting);
        for (usize i = 0; i < std::max(heard.size(), played.size()); ++i) {
            const bool same = i < heard.size() && i < played.size() && heard[i].name == played[i].name && heard[i].call == played[i].call &&
                              near(heard[i].since, played[i].since) && near(heard[i].volume, played[i].volume);
            if (!same) return differs("музыка «" + (i < heard.size() ? heard[i].name : played[i].name) + "»");
        }
        // Each value of a part, where the files keep it exactly (the stage's clip and each clip at its moment); the parts
        // placed in `rig`'s drawing `drawing` with their records in `file`.
        using Clips = std::vector<std::pair<const MotionClip*, f64>>;
        auto values = [&](const Pose& pose, const std::vector<MotionPartPose>& parts, const std::vector<std::string>& ids, const MotionFile* file,
                          const Rig* rig, const std::vector<vector::MotionCopy>& copies, const std::string& drawing, const Clips& clips) {
            for (const std::string& id : ids)
                for (usize p = 0; p < kProps; ++p) {
                    const auto mp = static_cast<MotionProp>(p);
                    if (mp == MotionProp::Drawing || !exact(stage_clip, id, mp, t) ||
                        std::any_of(clips.begin(), clips.end(), [&](const auto& c) { return !exact(*c.first, id, mp, c.second); }))
                        continue;
                    if (!vector::motion_same(mp, screen_value(pose, id, mp), game_value(parts, id, mp))) return differs("«" + id + "», " + prop_words(mp));
                }
            // A part the clip moves or links stands by its record in the file, where the drawing has it (without one the game
            // leaves it where it is: motion_part_pose).
            const Rig* r = rig ? rig->drawing(drawing == vector::kMotionMainDrawing ? "" : drawing) : nullptr;
            for (const MotionPartPose& pp : parts) {
                bool placed = pp.has[static_cast<usize>(MotionProp::Link)];
                for (usize p = 0; p < kProps; ++p) placed = placed || (pp.has[p] && vector::motion_prop_moves(static_cast<MotionProp>(p)));
                if (!placed || !file || !r) continue;
                std::string source = pp.id;
                for (const vector::MotionCopy& k : copies)
                    if (k.id == pp.id) source = k.source;
                if (r->part(source) >= 0 && !file->part(pp.id, drawing)) return differs("«" + pp.id + "»: в файле нет того, где часть стоит");
            }
            return true;
        };
        auto ids_of = [](const Pose& pose, const std::vector<MotionPartPose>& parts) {
            std::vector<std::string> ids;
            for (const auto& [id, pp] : pose.parts) ids.push_back(id);
            for (const auto& [id, m] : pose.morphs) ids.push_back(id);
            for (const MotionPartPose& pp : parts) ids.push_back(pp.id);
            std::sort(ids.begin(), ids.end());
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
            return ids;
        };
        // The camera and the frames, then the stage.
        std::vector<MotionPartPose> frames{game.camera};
        frames.insert(frames.end(), std::begin(game.frames), std::end(game.frames));
        std::vector<std::string> frame_ids{kCameraPart};
        frame_ids.insert(frame_ids.end(), std::begin(kLayerParts), std::end(kLayerParts));
        if (!values(tab.scene, frames, frame_ids, nullptr, nullptr, {}, vector::kMotionMainDrawing, {})) return false;
        if (!values(tab.stage, game.stage, ids_of(tab.stage, game.stage), &motion.stage, &scene.rig(), stage_clip.copies, vector::kMotionMainDrawing, {}))
            return false;
        if (copy_ids(tab.stage.copies) != copy_ids(game.copies)) return differs("копии на сцене");
        if (!same_order(tab.stage.order, game.order)) return differs("порядок частей сцены");
        for (usize a = 0; a < f.actors.size(); ++a) {
            const std::string& id = f.actors[a].id;
            const vector::SceneActorPose& ap = game.actors[a];
            const ActorMoment m = actor_moment(*rec, id, t, held, waiting);
            const i32 play = m.play ? static_cast<i32>(m.play - rec->plays.data()) : -1;
            if (play != ap.moment.play || (play >= 0 && !near(m.time, ap.moment.time))) return differs("«" + id + "», клип и его миг");
            // Part 6г5а: in a blend, the same steps, each at its moment and share.
            bool same_blend = m.blend.size() == ap.moment.blend.size();
            for (usize i = 0; same_blend && i < m.blend.size(); ++i) {
                const ActorBlendStep& x = m.blend[i];
                const vector::SceneBlendStep& y = ap.moment.blend[i];
                same_blend = (x.play ? static_cast<i32>(x.play - rec->plays.data()) : -1) == y.play && near(x.time, y.time) && near(x.k, y.k);
            }
            if (!same_blend) return differs("«" + id + "», смена клипов");
            const Pose& pose = tab.actors[id];
            Clips clips;
            auto clip_of = [&](i32 at, f64 time) {
                if (const MotionClip* c = at >= 0 ? motion.actors[a].clip(f.plays[static_cast<usize>(at)].clip) : nullptr) clips.push_back({c, time});
            };
            if (ap.moment.blend.empty()) clip_of(play, ap.moment.time);
            for (const vector::SceneBlendStep& st : ap.moment.blend) clip_of(st.play, st.time);
            // The records of its parts: its file's, or the stage's for an actor with none.
            const MotionFile& file = f.actors[a].motion.empty() ? motion.stage : motion.actors[a];
            const bool drawing_exact =
                std::all_of(clips.begin(), clips.end(), [&](const auto& c) { return exact(*c.first, kDrawingPart, MotionProp::Drawing, c.second); });
            if (drawing_exact && !screen_same_shown(screen_shown(pose), ap.shown)) return differs("«" + id + "», рисунок");
            if (!values(pose, ap.parts, ids_of(pose, ap.parts), &file, scene.actor_rig(id), ap.copies, ap.shown.drawing, clips)) return false;
            if (copy_ids(pose.copies) != copy_ids(ap.copies)) return differs("«" + id + "», копии");
            if (!same_order(pose.order, ap.order)) return differs("«" + id + "», порядок частей");
        }
        return true;
    };
    const std::vector<f64> waited(f.stops.size(), kCheckWait);
    for (const i64 n : moments) {
        const f64 t = static_cast<f64>(n) / 1e9;
        if (!one(t, {}, -1, "")) return false;
        if (!f.stops.empty() && !one(t, waited, -1, " с ожиданиями по " + seconds_words(kCheckWait) + " с")) return false;
    }
    for (usize i = 0; i < f.stops.size(); ++i)
        for (const f64 h : {0.0, kCheckWait / 2, kCheckWait}) {
            std::vector<f64> held(i + 1, kCheckWait);
            held[i] = h;
            if (!one(f.stops[i].at, held, static_cast<i32>(i), " у ожидания " + std::to_string(i + 1) + " через " + seconds_words(h) + " с")) return false;
        }
    return true;
}

} // namespace forge::pixel
