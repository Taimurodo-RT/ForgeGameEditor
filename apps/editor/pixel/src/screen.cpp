#include "forge/pixel/screen.h"

#include "forge/vector/path.h"
#include "forge/vector/svg.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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
MotionValue value_of(const PropValue& v) { return {v.v, color8(v.color), v.target, v.n, {}}; }

// The same property in the file and in the recording: by its word, the mask's and the link's by name (the code has no
// field of theirs, so their words differ).
Prop prop_of(MotionProp p) {
    if (p == MotionProp::Mask) return Prop::Mask;
    if (p == MotionProp::Link) return Prop::Link;
    Prop out = Prop::X;
    prop_from_word(vector::motion_prop_word(p), out);
    return out;
}
bool motion_prop_of(Prop p, MotionProp& out) {
    if (p == Prop::Mask) return out = MotionProp::Mask, true;
    if (p == Prop::Link) return out = MotionProp::Link, true;
    return vector::motion_prop_from_word(prop_word(p), out);
}

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

// What a recording's track does that a screen does not draw (since part 5д: the blur and the shadow).
const char* not_drawn_of(Prop p) {
    switch (p) {
    case Prop::Blur: return "blur";
    case Prop::Shadow:
    case Prop::ShadowX:
    case Prop::ShadowY:
    case Prop::ShadowBlur: return "shadow";
    default: return nullptr;
    }
}

// The same of a frame.
void not_drawn_in(const Pose& pose, std::set<std::string>& out) {
    for (const auto& [id, pp] : pose.parts) {
        if (pp.blur > 0) out.insert("blur");
        if (pp.has_shadow && pp.shadow_k > 0) out.insert("shadow");
    }
}

// A drawing's name as the file has it: "draw" for the main one (a frame's "", a value's "draw" or "").
std::string file_drawing(const std::string& name) { return name.empty() ? vector::kMotionMainDrawing : name; }

// What a frame shows of the drawings, as the file's motion_shown has it.
vector::MotionShown shown_of(const Pose& pose) {
    vector::MotionShown out;
    out.drawing = file_drawing(pose.drawing);
    if (pose.swap) {
        out.swap = true;
        out.from = file_drawing(pose.swap_from);
        out.k = pose.swap_k;
    }
    return out;
}

bool same_shown(const vector::MotionShown& a, const vector::MotionShown& b) {
    return a.drawing == b.drawing && a.swap == b.swap && a.from == b.from &&
           std::abs(a.k - b.k) <= 1e-9 * std::max({1.0, std::abs(a.k), std::abs(b.k)});
}

// 0.35 → "0,35".
std::string seconds_words(f64 v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    std::string out = buf;
    std::replace(out.begin(), out.end(), '.', ',');
    return out;
}

vector::MotionMatrix matrix_of(const Affine& a) { return {a.a, a.b, a.c, a.d, a.e, a.f}; }

// Whether a drawing's name makes a file name everywhere: not empty, none of / \ : * ? " < > | or a control character,
// not ending in a dot or a space.
bool file_name_ok(std::string_view name) {
    if (name.empty() || name.back() == '.' || name.back() == ' ') return false;
    for (const char ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7f || std::string_view("/\\:*?\"<>|").find(ch) != std::string_view::npos) return false;
    }
    return true;
}

// A name with its Latin and Cyrillic capitals small: two names Windows keeps as one file.
std::string folded(std::string_view name) {
    std::string out;
    for (usize i = 0; i < name.size(); ++i) {
        const auto c = static_cast<unsigned char>(name[i]);
        const auto n = i + 1 < name.size() ? static_cast<unsigned char>(name[i + 1]) : 0;
        if (c >= 'A' && c <= 'Z') out += static_cast<char>(c - 'A' + 'a');
        else if (c == 0xd0 && n >= 0x90 && n <= 0x9f) out += "\xd0", out += static_cast<char>(n + 0x20), ++i; // А … П
        else if (c == 0xd0 && n >= 0xa0 && n <= 0xaf) out += "\xd1", out += static_cast<char>(n - 0x20), ++i; // Р … Я
        else if (c == 0xd0 && n == 0x81) out += "\xd1\x91", ++i;                                           // Ё
        else out += static_cast<char>(c);
    }
    return out;
}

std::string xml_escaped(std::string_view s) {
    std::string out;
    for (const char c : s) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

// A drawing's text with the main picture's root size (part 5д): its width, height, viewBox and preserveAspectRatio. The
// tab draws every drawing in the main picture's units, so the drawing stands on the screen where it stands in the tab,
// and a layer that shows it keeps its size.
std::string with_root_of(const std::string& text, std::string_view main) {
    const vector::SvgOutline mo = vector::scan_svg(main), o = vector::scan_svg(text);
    if (mo.elements.empty() || o.elements.empty()) return text;
    std::string tag = "<svg xmlns=\"http://www.w3.org/2000/svg\"";
    for (const char* name : {"width", "height", "viewBox", "preserveAspectRatio"})
        if (const std::string* v = mo.elements.front().attr(name)) tag += std::string(" ") + name + "=\"" + xml_escaped(*v) + "\"";
    tag += '>';
    return text.substr(0, o.elements.front().begin) + tag + text.substr(o.elements.front().open_end);
}

} // namespace

vector::MotionValue screen_value(const Pose& pose, const std::string& id, vector::MotionProp p) {
    MotionValue out = vector::motion_rest(p);
    if (p == MotionProp::Morph) {
        auto m = pose.morphs.find(id);
        if (m != pose.morphs.end() && m->second.k > 0) out = {m->second.k, {}, m->second.target, 0, {}};
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
        if (pp.has_fill) out = {pp.fill_k, color8(pp.fill), {}, 0, {}};
        break;
    case MotionProp::Stroke:
        if (pp.has_stroke) out = {pp.stroke_k, color8(pp.stroke), {}, 0, {}};
        break;
    case MotionProp::StrokeWidth:
        if (pp.has_stroke_width) out = {pp.stroke_width_k, {}, {}, pp.stroke_width, {}};
        break;
    case MotionProp::Visible: out.v = pp.visible ? 1 : 0; break;
    case MotionProp::Morph: break;
    case MotionProp::Dash: out.v = pp.dash; break;
    case MotionProp::DrawFrom: out.v = pp.draw_from; break;
    case MotionProp::DrawTo: out.v = pp.draw_to; break;
    case MotionProp::Tint:
        if (pp.has_tint) out = {pp.tint_k, color8(pp.tint), {}, 0, {}};
        break;
    case MotionProp::Flash: out.v = pp.flash; break;
    case MotionProp::FlashColor:
        if (pp.has_flash_color) out = {pp.flash_color_k, color8(pp.flash_color), {}, 0, {}};
        break;
    case MotionProp::Mask:
        if (!pp.mask.empty()) out = {pp.mask_invert ? 2.0 : 1.0, {}, pp.mask, 0, {}};
        break;
    case MotionProp::Link:
        out.target = pp.carry_by;
        out.m = matrix_of(pp.carry);
        break;
    case MotionProp::Drawing: break; // the character's, not a part's: shown_of
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
    // The parts of the character's drawings (part 5д): the main picture's, then those only other drawings have, by the
    // drawings' names and their order in each.
    std::vector<std::string> drawn_parts;
    for (const Part& p : rig.parts()) drawn_parts.push_back(p.id);
    for (const std::string& name : rig.drawings())
        for (const Part& p : rig.drawing(name)->parts())
            if (std::find(drawn_parts.begin(), drawn_parts.end(), p.id) == drawn_parts.end()) drawn_parts.push_back(p.id);
    auto drawn_part = [&](const std::string& id) { return std::find(drawn_parts.begin(), drawn_parts.end(), id) != drawn_parts.end(); };
    // The copies the screen draws (part 5г): of parts of a drawing, born within the clip.
    std::vector<char> drawn_copy(rec->copies.size(), 0);
    for (usize i = 0; i < rec->copies.size(); ++i) {
        const Copy& c = rec->copies[i];
        drawn_copy[i] = drawn_part(c.source) && ns_of(c.born) <= length;
        if (drawn_copy[i]) out.copies.push_back({c.id, c.source, c.number, c.born});
    }
    std::stable_sort(out.copies.begin(), out.copies.end(), [](const vector::MotionCopy& a, const vector::MotionCopy& b) { return a.born < b.born; });
    // The parts the screen's pictures have, by rank: the drawings' parts, then the copies by their calls. -1 for none of
    // them.
    const auto picture = static_cast<i32>(drawn_parts.size());
    auto rank_of = [&](const std::string& id) -> i32 {
        if (const auto it = std::find(drawn_parts.begin(), drawn_parts.end(), id); it != drawn_parts.end())
            return static_cast<i32>(it - drawn_parts.begin());
        const i32 ci = copy_of(*rec, id);
        return ci >= 0 && drawn_copy[static_cast<usize>(ci)] ? picture + ci : -1;
    };
    auto id_of = [&](i32 rank) -> const std::string& {
        return rank < picture ? drawn_parts[static_cast<usize>(rank)] : rec->copies[static_cast<usize>(rank - picture)].id;
    };

    // The tracks: each property of each part the clip ever takes from its rest, by blocks, start_from or pose(t).
    std::set<std::pair<i32, usize>> wanted; // the part's rank, the property
    std::set<std::string> not_drawn;
    for (const Recording::Track& tr : rec->tracks) {
        const std::string& id = rec->parts[tr.part];
        if (const char* w = not_drawn_of(tr.prop)) not_drawn.insert(w);
        MotionProp mp;
        if (motion_prop_of(tr.prop, mp) && rank_of(id) >= 0) wanted.insert({rank_of(id), static_cast<usize>(mp)});
    }
    for (const StartValue& sv : rec->start.values) {
        if (const char* w = not_drawn_of(sv.prop)) not_drawn.insert(w);
        MotionProp mp;
        if (motion_prop_of(sv.prop, mp) && rank_of(sv.part) >= 0) wanted.insert({rank_of(sv.part), static_cast<usize>(mp)});
    }
    std::vector<i64> whole = moments(0, length);
    if (length > 0) whole.push_back(length);
    bool drawings = false; // a frame shows another drawing than the main one, or a swap
    for (const i64 t : whole) {
        const Pose* pose = frames.at(t, why);
        if (!pose) return false;
        not_drawn_in(*pose, not_drawn);
        drawings = drawings || !pose->drawing.empty() || pose->swap;
        for (const auto& [id, pp] : pose->parts) {
            const i32 rank = rank_of(id);
            if (rank < 0) continue;
            for (usize p = 0; p < kProps; ++p) {
                const auto mp = static_cast<MotionProp>(p);
                if (mp != MotionProp::Morph && !vector::motion_same(mp, screen_value(*pose, id, mp), vector::motion_rest(mp)))
                    wanted.insert({rank, p});
            }
        }
        for (const auto& [id, m] : pose->morphs)
            if (m.k > 0 && rank_of(id) >= 0) wanted.insert({rank_of(id), static_cast<usize>(MotionProp::Morph)});
    }

    for (const auto& [rank, p] : wanted) {
        const auto mp = static_cast<MotionProp>(p);
        const std::string& id = id_of(rank);
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
            // The mask and the link change by a call, at a moment: a hold from each moment it is another.
            if (vector::motion_prop_held(mp)) {
                for (usize i = 0; i < ms.size(); ++i) {
                    if (i > 0 && vector::motion_same(mp, vs[i], vs[i - 1])) continue;
                    MotionPiece h;
                    h.kind = MotionPiece::Kind::Hold;
                    h.at = i == 0 ? pc.at : seconds(ms[i]);
                    h.value = vs[i];
                    tr.pieces.push_back(std::move(h));
                }
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
        // The clip's last moment, where a clip that does not loop stays: a call at its very end changes the frame there
        // only, and the pieces look at the moments before their ends. A hold of it when they give another value there.
        if (length > 0) {
            const Pose* pose = frames.at(length, why);
            if (!pose) return false;
            const MotionValue last = screen_value(*pose, id, mp);
            if (!vector::motion_same(mp, vector::motion_track_value(file, tr, out.length, out.length), last)) {
                MotionPiece h;
                h.kind = MotionPiece::Kind::Hold;
                h.at = std::min(seconds(length), out.length);
                h.value = last;
                tr.pieces.push_back(std::move(h));
            }
        }
        const bool at_rest = tr.pieces.size() == 1 && tr.pieces.front().kind == MotionPiece::Kind::Hold &&
                             vector::motion_same(mp, tr.pieces.front().value, vector::motion_rest(mp));
        if (!at_rest) out.tracks.push_back(std::move(tr));
    }

    // The drawing's track (part 5д), first: a hold of the drawing shown, or a swap's transition, checked at each moment
    // against the frame as the other transitions are.
    if (drawings) {
        const i32 dp = rec_part(kDrawingPart);
        const i32 track = dp < 0 ? -1 : rec->track(static_cast<u32>(dp), Prop::Drawing);
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
        tr.part = kDrawingPart;
        tr.prop = MotionProp::Drawing;
        const std::vector<i64> starts(cuts.begin(), cuts.end());
        for (usize k = 0; k < starts.size(); ++k) {
            const i64 s = starts[k], e = k + 1 < starts.size() ? starts[k + 1] : length;
            const std::vector<i64> ms = moments(s, e);
            std::vector<vector::MotionShown> vs;
            for (const i64 m : ms) {
                const Pose* pose = frames.at(m, why);
                if (!pose) return false;
                vs.push_back(shown_of(*pose));
            }
            MotionPiece pc;
            pc.at = seconds(s);
            if (std::all_of(vs.begin(), vs.end(), [&](const vector::MotionShown& v) { return !v.swap && v.drawing == vs.front().drawing; })) {
                // A hold after a hold or a transition that ends on its drawing says nothing new.
                const MotionPiece* last = tr.pieces.empty() ? nullptr : &tr.pieces.back();
                if (last && ((last->kind == MotionPiece::Kind::Hold && last->value.target == vs.front().drawing) ||
                             (last->kind == MotionPiece::Kind::Ease && s >= ns_of(last->at + last->length) && last->to.target == vs.front().drawing)))
                    continue;
                pc.value.target = vs.front().drawing;
                tr.pieces.push_back(std::move(pc));
                continue;
            }
            const i32 d = track < 0 ? -1 : deciding_block(*rec, static_cast<u32>(track), seconds(s));
            const Block* b = d >= 0 && ns_of(rec->blocks[static_cast<usize>(d)].start) == s ? &rec->blocks[static_cast<usize>(d)] : nullptr;
            usize wrong = 0;
            if (b) {
                pc.kind = MotionPiece::Kind::Ease;
                pc.length = b->length;
                pc.from.target = file_drawing(b->from.target);
                pc.to.target = file_drawing(b->end.target);
                MotionFile probe; // its own easing alone, so a curve that does not fit leaves the file as it was
                if (b->bezier >= 0 && static_cast<usize>(b->bezier) < rec->beziers.size()) {
                    pc.has_bezier = true;
                    pc.bezier = rec->beziers[static_cast<usize>(b->bezier)].curve;
                } else if (b->curve >= 0 && static_cast<usize>(b->curve) < rec->curves.size()) {
                    probe.curves.push_back(rec->curves[static_cast<usize>(b->curve)]);
                    pc.curve = 0;
                } else pc.timing = b->timing;
                vector::MotionClip one;
                one.length = out.length;
                one.tracks.push_back({std::string(kDrawingPart), MotionProp::Drawing, {pc}});
                while (wrong < ms.size() && same_shown(vector::motion_shown(probe, one, seconds(ms[wrong])), vs[wrong])) ++wrong;
                if (wrong == ms.size()) {
                    if (pc.curve >= 0) pc.curve = static_cast<i32>(curve_index(file, probe.curves.front()));
                    tr.pieces.push_back(std::move(pc));
                    continue;
                }
            } else {
                while (wrong < ms.size() && !vs[wrong].swap) ++wrong;
            }
            why = "клип «" + out.name + "»: смена рисунка в " + seconds_words(seconds(ms[std::min(wrong, ms.size() - 1)])) +
                  " с не записывается переходом";
            return false;
        }
        // The clip's last moment, as for the parts: a hold of the drawing it shows when the pieces end on another.
        if (length > 0) {
            const Pose* pose = frames.at(length, why);
            if (!pose) return false;
            const vector::MotionShown last = shown_of(*pose);
            vector::MotionClip one;
            one.length = out.length;
            one.tracks.push_back(tr);
            if (!same_shown(vector::motion_shown(file, one, out.length), last)) {
                if (last.swap)
                    return why = "клип «" + out.name + "»: смена рисунка в " + seconds_words(out.length) + " с не записывается переходом", false;
                MotionPiece h;
                h.kind = MotionPiece::Kind::Hold;
                h.at = std::min(seconds(length), out.length);
                h.value.target = last.drawing;
                tr.pieces.push_back(std::move(h));
            }
        }
        // Not a hold of the main drawing alone: the frames (with the last moment's) showed another or a swap.
        out.tracks.insert(out.tracks.begin(), std::move(tr));
    }

    // The order (part 5г): what start_from left, at 0, then the clip's calls by time (start, then as made), of the parts
    // the screen draws and their neighbours in the picture.
    auto step_of = [&](const OrderStep& o, f64 at) {
        vector::MotionOrderStep st;
        st.at = at;
        st.part = o.part;
        st.kind = static_cast<vector::MotionOrder>(o.kind);
        if (o.kind == OrderStep::Kind::Above || o.kind == OrderStep::Kind::Below) st.other = o.other;
        return st;
    };
    auto drawn_step = [&](const OrderStep& o) {
        return rank_of(o.part) >= 0 && ((o.kind != OrderStep::Kind::Above && o.kind != OrderStep::Kind::Below) || drawn_part(o.other));
    };
    for (const OrderStep& o : rec->start.order)
        if (drawn_step(o)) out.order.push_back(step_of(o, 0));
    std::vector<u32> calls;
    for (u32 i = 0; i < rec->blocks.size(); ++i)
        if (rec->blocks[i].prop == Prop::Order && ns_of(rec->blocks[i].start) <= length) calls.push_back(i);
    std::stable_sort(calls.begin(), calls.end(), [&](u32 a, u32 b) {
        const Block &x = rec->blocks[a], &y = rec->blocks[b];
        return x.start != y.start ? x.start < y.start : x.order < y.order;
    });
    for (const u32 i : calls) {
        const Block& b = rec->blocks[i];
        OrderStep o;
        o.part = rec->parts[b.part];
        o.kind = static_cast<OrderStep::Kind>(std::clamp(static_cast<int>(b.value.v), 0, 3));
        o.other = b.value.target;
        if (drawn_step(o)) out.order.push_back(step_of(o, b.start));
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
                  std::string_view svg, const Drawing* drawn, const std::vector<NamedDrawing>& drawings, ScreenGift& out, std::string& why) {
    out = {};
    if (script.clips().empty()) return why = "В коде нет клипов: экрану нечего играть", false;
    // The other drawings' files (part 5д): Ёжик.angry.svg, a name that makes a file and one file a name.
    std::map<std::string, std::string> folds;
    for (const NamedDrawing& d : drawings) {
        if (!file_name_ok(d.name)) return why = "рисунок «" + d.name + "»: такое имя не годится для имени файла", false;
        if (!rig.drawing(d.name) || rig.drawing(d.name) == &rig) return why = "рисунок «" + d.name + "» не стоит на персонаже: откройте его снова", false;
        auto [it, fresh] = folds.emplace(folded(d.name), d.name);
        if (!fresh) {
            const auto [a, b] = std::minmax(it->second, d.name);
            return why = "рисунки «" + a + "» и «" + b + "» дали бы один файл", false;
        }
    }
    for (const std::string& name : rig.drawings())
        if (std::none_of(drawings.begin(), drawings.end(), [&](const NamedDrawing& d) { return d.name == name; }))
            return why = "у персонажа есть рисунок «" + name + "», а его текста нет: откройте персонажа снова", false;
    MotionFile& f = out.motion;
    f.character = std::string(character);
    f.from = std::string(from);
    f.picture = std::string(character) + ".svg";
    for (const std::string& name : script.clips()) {
        MotionClip c;
        if (!screen_clip(script, rig, doc, name, f, c, why)) return false;
        f.clips.push_back(std::move(c));
    }
    // The parts the clips move or link, then every copy (a copy's numbers are its source's), as each drawing has them
    // and in its order; the shapes they morph, a copy's by its source.
    std::set<std::string> moved, morphed;
    std::vector<vector::MotionCopy> copies;
    auto source_of = [&](const MotionClip& c, const std::string& id) {
        for (const vector::MotionCopy& k : c.copies)
            if (k.id == id) return k.source;
        return id;
    };
    for (const MotionClip& c : f.clips) {
        for (const vector::MotionTrack& t : c.tracks) {
            if (vector::motion_prop_moves(t.prop) || t.prop == MotionProp::Link) moved.insert(t.part);
            if (t.prop == MotionProp::Morph) morphed.insert(source_of(c, t.part));
        }
        for (const vector::MotionCopy& k : c.copies)
            if (std::none_of(copies.begin(), copies.end(), [&](const vector::MotionCopy& o) { return o.id == k.id; })) copies.push_back(k);
    }
    auto parts_of = [&](const Rig& r) {
        std::vector<vector::MotionPart> ps;
        auto part_of = [&](const std::string& id, i32 pi) {
            vector::MotionPart p;
            p.id = id;
            p.pivot = r.pivot(pi);
            p.frame = matrix_of(r.rest_frame(pi));
            p.own = matrix_of(r.rest_local(pi));
            ps.push_back(std::move(p));
        };
        for (const Part& p : r.parts())
            if (moved.count(p.id)) part_of(p.id, r.part(p.id));
        for (const vector::MotionCopy& k : copies)
            if (r.part(k.source) >= 0) part_of(k.id, r.part(k.source));
        return ps;
    };
    f.parts = parts_of(rig);
    const std::vector<std::string> morph_ids(morphed.begin(), morphed.end());
    // A morphed shape that is no path in the picture of «Ресурсы» the screen's stage writes as one (part 5г).
    out.svg = drawn ? drawing_svg(drawing_with_paths(*drawn, morph_ids), doc.frame_w, doc.frame_h) : std::string(svg);
    // The other drawings (part 5д), by name: their parts as they stand in them, their text with the main root's size.
    std::vector<const NamedDrawing*> named;
    for (const NamedDrawing& d : drawings) named.push_back(&d);
    std::sort(named.begin(), named.end(), [](const NamedDrawing* a, const NamedDrawing* b) { return a->name < b->name; });
    for (const NamedDrawing* d : named) {
        vector::MotionDrawing md;
        md.name = d->name;
        md.picture = std::string(character) + "." + d->name + ".svg";
        md.parts = parts_of(*rig.drawing(d->name));
        out.drawings.push_back({md.picture, with_root_of(drawing_svg(drawing_with_paths(d->drawing, morph_ids), doc.frame_w, doc.frame_h), out.svg)});
        f.drawings.push_back(std::move(md));
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
