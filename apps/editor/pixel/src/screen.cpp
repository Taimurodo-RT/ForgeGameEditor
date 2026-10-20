#include "forge/pixel/screen.h"

#include "forge/vector/path.h"
#include "forge/vector/svg.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace forge::pixel {

namespace {

using vector::MotionClip;
using vector::MotionFile;
using vector::MotionPiece;
using vector::MotionProp;
using vector::MotionValue;

constexpr usize kProps = vector::kMotionPropCount;

// Moments as the recording keeps them: whole nanoseconds.
i64 ns_of(f64 t) { return std::llround(t * 1e9); }
f64 seconds(i64 ns) { return static_cast<f64>(ns) / 1e9; }

vector::MotionColor color8(const Rgba8& c) { return {c.r, c.g, c.b, c.a}; }
MotionValue value_of(const PropValue& v) { return {v.v, color8(v.color), v.target, v.n}; }

Prop prop_of(MotionProp p) {
    Prop out = Prop::X;
    prop_from_word(vector::motion_prop_word(p), out);
    return out;
}
bool motion_prop_of(Prop p, MotionProp& out) { return vector::motion_prop_from_word(prop_word(p), out); }

// The moments a piece from `at` to `end` is looked at: 120 times a second from `at`, then the moment before `end`
// (as a frames piece keeps its values); `at` alone for a piece of no length.
std::vector<i64> moments(i64 at, i64 end) {
    if (end <= at) return {at};
    const usize n = vector::motion_frame_count(seconds(at), seconds(end));
    std::vector<i64> out;
    out.reserve(n);
    for (usize i = 0; i + 1 < n; ++i) out.push_back(ns_of(vector::motion_frame_moment(seconds(at), i)));
    out.push_back(end - 1);
    return out;
}

// The tab's frames of a clip, each moment made once: what the preview shows (the recording, start_from, pose(t)).
struct Frames {
    PoseScript& script;
    const Rig& rig;
    const PixelDoc& doc;
    std::string clip;
    std::map<i64, Pose> poses;

    const Pose* at(i64 t, std::string& why) {
        auto it = poses.find(t);
        if (it != poses.end()) return &it->second;
        Pose p;
        if (!script.pose(rig, seconds(t), clip, doc.params, doc.seed, p)) {
            why = "клип «" + clip + "», " + script.error();
            return nullptr;
        }
        return &poses.emplace(t, std::move(p)).first->second;
    }
};

// Whether a block's transition is the formula of an ease piece: not a binding, a path, a spring, tween's frames or an
// arc.
bool plain(const Block& b) {
    return b.to != Block::To::Binding && b.to != Block::To::Along && !b.spring && b.frames.empty() && b.arc < 0;
}

usize curve_index(MotionFile& file, const std::vector<f64>& curve) {
    for (usize i = 0; i < file.curves.size(); ++i)
        if (file.curves[i] == curve) return i;
    file.curves.push_back(curve);
    return file.curves.size() - 1;
}

// What a recording's track does that a screen does not draw yet.
const char* not_drawn_of(Prop p) {
    switch (p) {
    case Prop::Dash: return "dash";
    case Prop::DrawFrom:
    case Prop::DrawTo: return "draw";
    case Prop::Tint: return "tint";
    case Prop::Flash:
    case Prop::FlashColor: return "flash";
    case Prop::Blur: return "blur";
    case Prop::Shadow:
    case Prop::ShadowX:
    case Prop::ShadowY:
    case Prop::ShadowBlur: return "shadow";
    case Prop::Mask: return "mask";
    case Prop::Link: return "link";
    case Prop::Order: return "order";
    case Prop::Drawing: return "drawing";
    default: return nullptr;
    }
}

// The same of a frame.
void not_drawn_in(const Rig& rig, const Pose& pose, std::set<std::string>& out) {
    for (const auto& [id, pp] : pose.parts) {
        if (pp.dash != 0) out.insert("dash");
        if (pp.draw_from != 0 || pp.draw_to != 1) out.insert("draw");
        if (pp.has_tint && pp.tint_k > 0) out.insert("tint");
        if (pp.flash > 0) out.insert("flash");
        if (pp.blur > 0) out.insert("blur");
        if (pp.has_shadow && pp.shadow_k > 0) out.insert("shadow");
        if (!pp.mask.empty()) out.insert("mask");
        if (!pp.carry_by.empty() || pp.carry != Affine{}) out.insert("link");
    }
    if (!pose.copies.empty()) out.insert("copies");
    if (!pose.order.empty()) out.insert("order");
    if (!pose.drawing.empty() || pose.swap) out.insert("drawing");
    for (const auto& [id, m] : pose.morphs)
        if (m.k > 0 && !rig.holder_name(m.target).empty()) out.insert("morph_drawing");
}

vector::MotionMatrix matrix_of(const Affine& a) { return {a.a, a.b, a.c, a.d, a.e, a.f}; }

} // namespace

vector::MotionValue screen_value(const Pose& pose, const std::string& id, vector::MotionProp p) {
    MotionValue out = vector::motion_rest(p);
    if (p == MotionProp::Morph) {
        auto m = pose.morphs.find(id);
        if (m != pose.morphs.end() && m->second.k > 0) out = {m->second.k, {}, m->second.target, 0};
        return out;
    }
    auto it = pose.parts.find(id);
    if (it == pose.parts.end()) return out;
    const PartPose& pp = it->second;
    switch (p) {
    case MotionProp::X: out.v = pp.x; break;
    case MotionProp::Y: out.v = pp.y; break;
    case MotionProp::Rot: out.v = pp.rot; break;
    case MotionProp::Scale: out.v = pp.scale; break;
    case MotionProp::Sx: out.v = pp.sx; break;
    case MotionProp::Sy: out.v = pp.sy; break;
    case MotionProp::SkewX: out.v = pp.skew_x; break;
    case MotionProp::SkewY: out.v = pp.skew_y; break;
    case MotionProp::Opacity: out.v = pp.opacity; break;
    case MotionProp::Fill:
        if (pp.has_fill) out = {pp.fill_k, color8(pp.fill), {}, 0};
        break;
    case MotionProp::Stroke:
        if (pp.has_stroke) out = {pp.stroke_k, color8(pp.stroke), {}, 0};
        break;
    case MotionProp::StrokeWidth:
        if (pp.has_stroke_width) out = {pp.stroke_width_k, {}, {}, pp.stroke_width};
        break;
    case MotionProp::Visible: out.v = pp.visible ? 1 : 0; break;
    case MotionProp::Morph: break;
    }
    return vector::motion_plain(p, out);
}

bool screen_clip(PoseScript& script, const Rig& rig, const PixelDoc& doc, std::string_view clip, vector::MotionFile& file,
                 vector::MotionClip& out, std::string& why) {
    const Recording* rec = script.recording(rig, clip, doc.params, doc.seed);
    if (!rec) return why = "клип «" + std::string(clip) + "», " + script.error(), false;
    out = {};
    out.name = std::string(clip);
    out.length = rec->length;
    const auto spec = std::find_if(doc.clips.begin(), doc.clips.end(), [&](const ClipSpec& c) { return c.state == clip; });
    out.loop = spec != doc.clips.end() && spec->loop;
    const i64 length = ns_of(rec->length);
    Frames frames{script, rig, doc, std::string(clip), {}};

    auto rec_part = [&](const std::string& id) -> i32 {
        for (usize i = 0; i < rec->parts.size(); ++i)
            if (rec->parts[i] == id) return static_cast<i32>(i);
        return -1;
    };
    // The picture's parts only: the copies and the other drawings' parts are not in the SVG the screen shows.
    auto drawn_part = [&](const std::string& id) { return rig.part(id) >= 0 && copy_of(*rec, id) < 0; };

    // The tracks: each property of each part the clip ever takes from its rest, by blocks, start_from or pose(t).
    std::set<std::pair<i32, usize>> wanted; // the part in the rig, the property
    std::set<std::string> not_drawn;
    for (const Recording::Track& tr : rec->tracks) {
        const std::string& id = rec->parts[tr.part];
        if (const char* w = not_drawn_of(tr.prop)) not_drawn.insert(w);
        if (tr.prop == Prop::Morph)
            for (const u32 bi : tr.blocks)
                if (!rig.holder_name(rec->blocks[bi].value.target).empty()) not_drawn.insert("morph_drawing");
        MotionProp mp;
        if (motion_prop_of(tr.prop, mp) && drawn_part(id)) wanted.insert({rig.part(id), static_cast<usize>(mp)});
    }
    if (!rec->copies.empty()) not_drawn.insert("copies");
    if (!rec->start.order.empty()) not_drawn.insert("order");
    for (const StartValue& sv : rec->start.values) {
        if (const char* w = not_drawn_of(sv.prop)) not_drawn.insert(w);
        MotionProp mp;
        if (motion_prop_of(sv.prop, mp) && drawn_part(sv.part)) wanted.insert({rig.part(sv.part), static_cast<usize>(mp)});
    }
    std::vector<i64> whole = moments(0, length);
    if (length > 0) whole.push_back(length);
    for (const i64 t : whole) {
        const Pose* pose = frames.at(t, why);
        if (!pose) return false;
        not_drawn_in(rig, *pose, not_drawn);
        for (const auto& [id, pp] : pose->parts) {
            if (!drawn_part(id)) continue;
            for (usize p = 0; p < kProps; ++p) {
                const auto mp = static_cast<MotionProp>(p);
                if (mp != MotionProp::Morph && !vector::motion_same(mp, screen_value(*pose, id, mp), vector::motion_rest(mp)))
                    wanted.insert({rig.part(id), p});
            }
        }
        for (const auto& [id, m] : pose->morphs)
            if (m.k > 0 && drawn_part(id)) wanted.insert({rig.part(id), static_cast<usize>(MotionProp::Morph)});
    }

    for (const auto& [pi, p] : wanted) {
        const auto mp = static_cast<MotionProp>(p);
        const std::string& id = rig.parts()[static_cast<usize>(pi)].id;
        const i32 rp = rec_part(id);
        const i32 track = rp < 0 ? -1 : rec->track(static_cast<u32>(rp), prop_of(mp));
        // Where pieces start: 0, each deciding block's start, its end when the next one does not cut it.
        std::set<i64> cuts{0};
        if (track >= 0) {
            const std::vector<u32>& bs = rec->tracks[static_cast<usize>(track)].blocks;
            for (usize i = 0; i < bs.size(); ++i) {
                const Block& b = rec->blocks[bs[i]];
                const i64 s = ns_of(b.start), e = ns_of(b.start + b.length);
                if (s < length) cuts.insert(s);
                const bool cut = i + 1 < bs.size() && ns_of(rec->blocks[bs[i + 1]].start) < e;
                if (b.length > 0 && !cut && e < length) cuts.insert(e);
            }
        }
        vector::MotionTrack tr;
        tr.part = id;
        tr.prop = mp;
        const std::vector<i64> starts(cuts.begin(), cuts.end());
        for (usize k = 0; k < starts.size(); ++k) {
            const i64 s = starts[k], e = k + 1 < starts.size() ? starts[k + 1] : length;
            const std::vector<i64> ms = moments(s, e);
            std::vector<MotionValue> vs;
            vs.reserve(ms.size());
            for (const i64 m : ms) {
                const Pose* pose = frames.at(m, why);
                if (!pose) return false;
                vs.push_back(screen_value(*pose, id, mp));
            }
            MotionPiece pc;
            pc.at = seconds(s);
            if (std::all_of(vs.begin(), vs.end(), [&](const MotionValue& v) { return vector::motion_same(mp, v, vs.front()); })) {
                pc.kind = MotionPiece::Kind::Hold;
                pc.value = vs.front();
                tr.pieces.push_back(std::move(pc));
                continue;
            }
            // The deciding block's transition, when it starts here and its formula is the frame at every moment.
            const i32 d = track < 0 ? -1 : deciding_block(*rec, static_cast<u32>(track), seconds(s));
            if (d >= 0 && ns_of(rec->blocks[static_cast<usize>(d)].start) == s && plain(rec->blocks[static_cast<usize>(d)])) {
                const Block& b = rec->blocks[static_cast<usize>(d)];
                MotionPiece ease;
                ease.kind = MotionPiece::Kind::Ease;
                ease.at = pc.at;
                ease.length = b.length;
                ease.from = vector::motion_plain(mp, value_of(b.from));
                ease.to = vector::motion_plain(mp, value_of(b.end));
                MotionFile probe; // its own easing alone, so a curve that does not fit leaves the file as it was
                if (b.bezier >= 0 && static_cast<usize>(b.bezier) < rec->beziers.size()) {
                    ease.has_bezier = true;
                    ease.bezier = rec->beziers[static_cast<usize>(b.bezier)].curve;
                } else if (b.curve >= 0 && static_cast<usize>(b.curve) < rec->curves.size()) {
                    probe.curves.push_back(rec->curves[static_cast<usize>(b.curve)]);
                    ease.curve = 0;
                } else ease.timing = b.timing;
                bool same = true;
                for (usize i = 0; i < ms.size() && same; ++i)
                    same = vector::motion_same(mp, vector::motion_piece_value(probe, mp, ease, seconds(ms[i])), vs[i]);
                if (same) {
                    if (ease.curve >= 0) ease.curve = static_cast<i32>(curve_index(file, probe.curves.front()));
                    tr.pieces.push_back(std::move(ease));
                    continue;
                }
            }
            pc.kind = MotionPiece::Kind::Frames;
            pc.end = seconds(e);
            pc.frames = std::move(vs);
            tr.pieces.push_back(std::move(pc));
        }
        // A hold after a hold or a transition that ends on its value says nothing new.
        std::vector<MotionPiece> kept;
        for (MotionPiece& pc : tr.pieces) {
            if (!kept.empty() && pc.kind == MotionPiece::Kind::Hold) {
                const MotionPiece& last = kept.back();
                if ((last.kind == MotionPiece::Kind::Hold && vector::motion_same(mp, last.value, pc.value)) ||
                    (last.kind == MotionPiece::Kind::Ease && pc.at >= last.at + last.length && vector::motion_same(mp, last.to, pc.value)))
                    continue;
            }
            kept.push_back(std::move(pc));
        }
        tr.pieces = std::move(kept);
        const bool at_rest = tr.pieces.size() == 1 && tr.pieces.front().kind == MotionPiece::Kind::Hold &&
                             vector::motion_same(mp, tr.pieces.front().value, vector::motion_rest(mp));
        if (!at_rest) out.tracks.push_back(std::move(tr));
    }

    for (const Mark& m : rec->marks)
        if (ns_of(m.time) <= length) out.marks.push_back({m.kind == Mark::Kind::Sound, m.name, m.time, m.length});
    std::stable_sort(out.marks.begin(), out.marks.end(), [](const vector::MotionMark& a, const vector::MotionMark& b) { return a.at < b.at; });
    for (const char* w : vector::kMotionNotDrawn)
        if (not_drawn.count(w)) out.not_drawn.emplace_back(w);
    return true;
}

Drawing drawing_with_paths(const Drawing& d, const std::vector<std::string>& ids) {
    using Kind = DrawShape::Kind;
    Drawing out = d;
    for (DrawShape& s : out.shapes) {
        if (s.kind == Kind::Path || s.kind == Kind::Group || std::find(ids.begin(), ids.end(), s.id) == ids.end()) continue;
        vector::Path path;
        switch (s.kind) {
        case Kind::Rect: path = vector::rect(s.x, s.y, s.w, s.h, s.r, s.r); break;
        case Kind::Circle: path = vector::circle(s.x, s.y, s.r); break;
        case Kind::Ellipse: path = vector::ellipse(s.x, s.y, s.rx, s.ry); break;
        case Kind::Line: {
            const vector::Point pts[] = {{s.x1, s.y1}, {s.x2, s.y2}};
            path = vector::polyline(pts, false);
            if (s.stroke.empty()) s.stroke = "#000000"; // a line's own stroke, which a path has not
            break;
        }
        case Kind::Poly: {
            std::vector<vector::Point> pts;
            for (usize k = 0; k + 1 < s.points.size(); k += 2) pts.push_back({s.points[k], s.points[k + 1]});
            path = vector::polyline(pts, true);
            break;
        }
        case Kind::Path:
        case Kind::Group: break;
        }
        s.kind = Kind::Path;
        s.d = vector::to_d(path);
    }
    return out;
}

bool build_screen(PoseScript& script, const Rig& rig, const PixelDoc& doc, std::string_view character, std::string_view from,
                  std::string_view svg, const Drawing* drawn, ScreenGift& out, std::string& why) {
    out = {};
    if (script.clips().empty()) return why = "В коде нет клипов: экрану нечего играть", false;
    MotionFile& f = out.motion;
    f.character = std::string(character);
    f.from = std::string(from);
    f.picture = std::string(character) + ".svg";
    for (const std::string& name : script.clips()) {
        MotionClip c;
        if (!screen_clip(script, rig, doc, name, f, c, why)) return false;
        f.clips.push_back(std::move(c));
    }
    // The parts the clips move, and the ones they morph, in the picture's order.
    std::set<i32> moved, morphed;
    for (const MotionClip& c : f.clips)
        for (const vector::MotionTrack& t : c.tracks) {
            if (vector::motion_prop_moves(t.prop)) moved.insert(rig.part(t.part));
            if (t.prop == MotionProp::Morph) morphed.insert(rig.part(t.part));
        }
    for (const i32 pi : moved) {
        vector::MotionPart p;
        p.id = rig.parts()[static_cast<usize>(pi)].id;
        p.pivot = rig.pivot(pi);
        p.frame = matrix_of(rig.rest_frame(pi));
        p.own = matrix_of(rig.rest_local(pi));
        f.parts.push_back(std::move(p));
    }
    std::vector<std::string> morph_ids;
    for (const i32 pi : morphed) morph_ids.push_back(rig.parts()[static_cast<usize>(pi)].id);
    if (drawn) out.svg = drawing_svg(drawing_with_paths(*drawn, morph_ids), doc.frame_w, doc.frame_h);
    else {
        out.svg = std::string(svg);
        for (const i32 pi : morphed) {
            const Part& part = rig.parts()[static_cast<usize>(pi)];
            const std::string& tag = rig.outline().elements[static_cast<usize>(part.element)].tag;
            if (tag != "path")
                out.notes.push_back("морфинг «" + part.id + "» на экране не рисуется: в «" + f.picture + "» это <" + tag + ">");
        }
    }
    // What the screen does not draw yet, by word with its clips.
    std::string left;
    for (const char* w : vector::kMotionNotDrawn) {
        std::string clips;
        for (const MotionClip& c : f.clips)
            if (std::find(c.not_drawn.begin(), c.not_drawn.end(), w) != c.not_drawn.end()) clips += (clips.empty() ? "«" : ", «") + c.name + "»";
        if (!clips.empty()) left += (left.empty() ? "" : ", ") + std::string(vector::motion_not_drawn_words(w)) + " (" + clips + ")";
    }
    if (!left.empty()) out.notes.insert(out.notes.begin(), "На экране пока не рисуются: " + left);
    return true;
}

} // namespace forge::pixel
