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

// 0.35 → "0,35".
std::string seconds_words(f64 v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    std::string out = buf;
    std::replace(out.begin(), out.end(), '.', ',');
    return out;
}

vector::MotionMatrix matrix_of(const Affine& a) { return {a.a, a.b, a.c, a.d, a.e, a.f}; }

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

// --- parameters from the game (part 5ж) ---------------------------------------------------------

bool same_moment(f64 a, f64 b) { return ns_of(a) == ns_of(b); }

// The first way clip `o` (recorded at another value of a parameter) is another clip than `z`, not in its numbers: what
// a game cannot make of `z` by moving them. Null when they differ only there. The curves are each one's file's.
const char* how_other(const MotionFile& fz, const MotionClip& z, const MotionFile& fo, const MotionClip& o) {
    if (!same_moment(z.length, o.length)) return "длина клипа";
    if (z.tracks.size() != o.tracks.size()) return "число переходов";
    for (usize t = 0; t < z.tracks.size(); ++t) {
        const vector::MotionTrack &a = z.tracks[t], &b = o.tracks[t];
        if (a.part != b.part || a.prop != b.prop || a.pieces.size() != b.pieces.size()) return "число переходов";
        for (usize k = 0; k < a.pieces.size(); ++k)
            if (a.pieces[k].kind != b.pieces[k].kind) return "число переходов";
    }
    for (usize t = 0; t < z.tracks.size(); ++t)
        for (usize k = 0; k < z.tracks[t].pieces.size(); ++k) {
            const MotionPiece &a = z.tracks[t].pieces[k], &b = o.tracks[t].pieces[k];
            if (!same_moment(a.at, b.at) || !same_moment(a.length, b.length) || !same_moment(a.end, b.end) || a.frames.size() != b.frames.size())
                return "время переходов";
        }
    for (usize t = 0; t < z.tracks.size(); ++t)
        for (usize k = 0; k < z.tracks[t].pieces.size(); ++k) {
            const MotionPiece &a = z.tracks[t].pieces[k], &b = o.tracks[t].pieces[k];
            if (a.kind != MotionPiece::Kind::Ease) continue;
            const bool ca = a.curve >= 0 && static_cast<usize>(a.curve) < fz.curves.size(), cb = b.curve >= 0 && static_cast<usize>(b.curve) < fo.curves.size();
            if (a.has_bezier != b.has_bezier || (a.has_bezier && !(a.bezier == b.bezier)) || ca != cb ||
                (ca && fz.curves[static_cast<usize>(a.curve)] != fo.curves[static_cast<usize>(b.curve)]) || (!a.has_bezier && !ca && a.timing != b.timing))
                return "сглаживание";
        }
    auto same_marks = [](const vector::MotionMark& a, const vector::MotionMark& b) {
        return a.sound == b.sound && a.name == b.name && same_moment(a.at, b.at) && same_moment(a.length, b.length);
    };
    if (!std::equal(z.marks.begin(), z.marks.end(), o.marks.begin(), o.marks.end(), same_marks)) return "метки";
    auto same_copies = [](const vector::MotionCopy& a, const vector::MotionCopy& b) {
        return a.id == b.id && a.source == b.source && a.number == b.number && same_moment(a.born, b.born);
    };
    if (!std::equal(z.copies.begin(), z.copies.end(), o.copies.begin(), o.copies.end(), same_copies)) return "копии";
    auto same_steps = [](const vector::MotionOrderStep& a, const vector::MotionOrderStep& b) {
        return same_moment(a.at, b.at) && a.part == b.part && a.kind == b.kind && a.other == b.other;
    };
    if (!std::equal(z.order.begin(), z.order.end(), o.order.begin(), o.order.end(), same_steps)) return "порядок частей";
    if (z.not_drawn != o.not_drawn) return "что не рисуется";
    // What a value has besides its numbers.
    auto other_than_numbers = [](MotionProp p, const MotionValue& a, const MotionValue& b) -> const char* {
        switch (p) {
        case MotionProp::Fill:
        case MotionProp::Stroke:
        case MotionProp::Tint:
        case MotionProp::FlashColor: return a.color == b.color ? nullptr : "цвет";
        case MotionProp::Morph: return a.target == b.target ? nullptr : "фигура морфинга";
        case MotionProp::Mask: return a.v == b.v && a.target == b.target ? nullptr : "маска";
        case MotionProp::Link: return a.target == b.target ? nullptr : "крепление";
        case MotionProp::Drawing: return a.target == b.target ? nullptr : "рисунок";
        case MotionProp::Visible: return a.v == b.v ? nullptr : "видимость";
        default: return nullptr;
        }
    };
    for (usize t = 0; t < z.tracks.size(); ++t) {
        const MotionProp p = z.tracks[t].prop;
        for (usize k = 0; k < z.tracks[t].pieces.size(); ++k) {
            const MotionPiece &a = z.tracks[t].pieces[k], &b = o.tracks[t].pieces[k];
            const char* w = other_than_numbers(p, a.value, b.value);
            if (!w) w = other_than_numbers(p, a.from, b.from);
            if (!w) w = other_than_numbers(p, a.to, b.to);
            for (usize i = 0; !w && i < a.frames.size(); ++i) w = other_than_numbers(p, a.frames[i], b.frames[i]);
            if (w) return w;
        }
    }
    return nullptr;
}

// A value's numbers, as a parameter changes them: its number or share, a stroke width's width, a link's matrix.
std::vector<f64*> numbers_of(MotionProp p, MotionValue& v) {
    if (!vector::motion_prop_has_numbers(p)) return {};
    if (p == MotionProp::Link) return {&v.m.a, &v.m.b, &v.m.c, &v.m.d, &v.m.e, &v.m.f};
    if (p == MotionProp::StrokeWidth) return {&v.v, &v.n};
    return {&v.v};
}
std::vector<f64*> numbers_of(MotionProp p, vector::MotionDelta& d) {
    if (!vector::motion_prop_has_numbers(p)) return {};
    if (p == MotionProp::Link) return {&d.m[0], &d.m[1], &d.m[2], &d.m[3], &d.m[4], &d.m[5]};
    if (p == MotionProp::StrokeWidth) return {&d.v, &d.n};
    return {&d.v};
}

// A value's change per unit from its values at one less (`m`) and one more (`p`): false when its numbers do not move by
// as much either way.
bool change_of(MotionProp prop, MotionValue z, MotionValue m, MotionValue p, vector::MotionDelta& out) {
    out = {};
    const std::vector<f64*> zs = numbers_of(prop, z), ms = numbers_of(prop, m), ps = numbers_of(prop, p), ds = numbers_of(prop, out);
    for (usize i = 0; i < zs.size(); ++i) {
        const f64 a = *ms[i], b = *zs[i], c = *ps[i];
        if (std::abs((c - b) - (b - a)) > 1e-9 * std::max({1.0, std::abs(a), std::abs(b), std::abs(c)})) return false;
        const f64 d = (c - a) / 2;
        *ds[i] = std::abs(d) <= 1e-9 * std::max(1.0, std::abs(b)) ? 0 : d;
    }
    return true;
}

// How clip `base` changes with parameter `prm` (part 5ж): recorded again at one less and one more of it, it is the same
// clip but for its numbers, each moving by as much either way; then each piece gets its change per unit. Else false
// with why a game cannot play it at another value.
bool clip_by(PoseScript& script, const Rig& rig, const PixelDoc& doc, const MotionFile& file, MotionClip& base, const vector::MotionParam& prm,
             std::string& reason) {
    MotionClip side[2];
    MotionFile curves[2]; // each one's easings: the file's are the clip's at the parameter's own value
    for (int k = 0; k < 2; ++k) {
        PixelDoc at = doc;
        const f64 v = prm.value + (k ? 1 : -1);
        at.params[prm.name] = v;
        std::string why;
        if (!screen_clip(script, rig, at, base.name, curves[k], side[k], why)) {
            reason = "клип не записывается при " + prm.name + " = " + seconds_words(v) + ": " + why;
            return false;
        }
    }
    for (int k = 0; k < 2; ++k)
        if (const char* w = how_other(file, base, curves[k], side[k])) return reason = w, false;
    // Each number's change, the piece's in the order of the file's parameters.
    std::vector<std::vector<vector::MotionBy>> by(base.tracks.size());
    for (usize t = 0; t < base.tracks.size(); ++t) {
        const MotionProp p = base.tracks[t].prop;
        for (usize k = 0; k < base.tracks[t].pieces.size(); ++k) {
            const MotionPiece &z = base.tracks[t].pieces[k], &m = side[0].tracks[t].pieces[k], &q = side[1].tracks[t].pieces[k];
            vector::MotionBy b;
            b.param = prm.name;
            bool ok = true;
            switch (z.kind) {
            case MotionPiece::Kind::Hold: ok = change_of(p, z.value, m.value, q.value, b.value); break;
            case MotionPiece::Kind::Ease: ok = change_of(p, z.from, m.from, q.from, b.from) && change_of(p, z.to, m.to, q.to, b.to); break;
            case MotionPiece::Kind::Frames:
                b.frames.resize(z.frames.size());
                for (usize i = 0; i < z.frames.size() && ok; ++i) ok = change_of(p, z.frames[i], m.frames[i], q.frames[i], b.frames[i]);
                if (std::all_of(b.frames.begin(), b.frames.end(), [](const vector::MotionDelta& d) { return d.zero(); })) b.frames.clear();
                break;
            }
            if (!ok) return reason = "не прямо", false;
            by[t].push_back(std::move(b));
        }
    }
    for (usize t = 0; t < base.tracks.size(); ++t)
        for (usize k = 0; k < base.tracks[t].pieces.size(); ++k) {
            vector::MotionBy& b = by[t][k];
            if (!b.value.zero() || !b.from.zero() || !b.to.zero() || !b.frames.empty()) base.tracks[t].pieces[k].by.push_back(std::move(b));
        }
    return true;
}

} // namespace

vector::MotionShown screen_shown(const Pose& pose) {
    vector::MotionShown out;
    out.drawing = file_drawing(pose.drawing);
    if (pose.swap) {
        out.swap = true;
        out.from = file_drawing(pose.swap_from);
        out.k = pose.swap_k;
    }
    return out;
}

bool screen_same_shown(const vector::MotionShown& a, const vector::MotionShown& b) {
    return a.drawing == b.drawing && a.swap == b.swap && a.from == b.from &&
           std::abs(a.k - b.k) <= 1e-9 * std::max({1.0, std::abs(a.k), std::abs(b.k)});
}

// Whether a name makes a file name everywhere: not empty, none of / \ : * ? " < > | or a control character,
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
    case MotionProp::Drawing: break; // the character's, not a part's: screen_shown
    }
    return vector::motion_plain(p, out);
}

bool screen_clip(PoseScript& script, const Rig& rig, const PixelDoc& doc, std::string_view clip, vector::MotionFile& file,
                 vector::MotionClip& out, std::string& why, std::span<const std::string> more) {
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
    for (const std::string& id : more)
        if (std::find(drawn_parts.begin(), drawn_parts.end(), id) == drawn_parts.end()) drawn_parts.push_back(id);
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
                vs.push_back(screen_shown(*pose));
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
                while (wrong < ms.size() && screen_same_shown(vector::motion_shown(probe, one, seconds(ms[wrong])), vs[wrong])) ++wrong;
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
            const vector::MotionShown last = screen_shown(*pose);
            vector::MotionClip one;
            one.length = out.length;
            one.tracks.push_back(tr);
            if (!screen_same_shown(vector::motion_shown(file, one, out.length), last)) {
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
    // The code's parameters (part 5ж), at the values the tab gives the clips: how each clip changes with each, or why it
    // does not. A picture the code draws from one the game cannot draw again: no clip changes with that one.
    for (const ScriptParam& sp : script.params()) {
        const auto it = doc.params.find(sp.name);
        f.params.push_back({sp.name, it != doc.params.end() ? it->second : sp.value});
    }
    auto picture_with = [&](const vector::MotionParam& prm, std::string& reason) {
        std::vector<std::pair<std::string, Drawing>> was;
        for (int k = -1; k <= 1; ++k) {
            std::map<std::string, f64> values = doc.params;
            values[prm.name] = prm.value + k;
            std::vector<std::pair<std::string, Drawing>> now;
            if (drawn) now.push_back({std::string(vector::kMotionMainDrawing), {}});
            for (const NamedDrawing& d : drawings) now.push_back({d.name, {}});
            for (auto& [name, d] : now)
                if (!(name == std::string(vector::kMotionMainDrawing) ? script.draw(values, doc.seed, d) : script.draw_named(name, values, doc.seed, d)))
                    return reason = "картинка не рисуется при " + prm.name + " = " + seconds_words(values[prm.name]) + ": " + script.error(), true;
            if (k == -1) was = now;
            for (usize i = 0; k != -1 && i < now.size(); ++i)
                if (!(now[i].second == was[i].second))
                    return reason = now[i].first == std::string(vector::kMotionMainDrawing) ? std::string("картинка") : "рисунок «" + now[i].first + "»", true;
        }
        return false;
    };
    for (const vector::MotionParam& prm : f.params) {
        std::string reason;
        if (picture_with(prm, reason)) {
            for (MotionClip& c : f.clips) c.fixed[prm.name] = reason;
            continue;
        }
        for (MotionClip& c : f.clips)
            if (!clip_by(script, rig, doc, f, c, prm, reason)) c.fixed[prm.name] = reason;
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
    // Which parameters a game changes the clips by, and which not where (part 5ж).
    std::string changing, fixed;
    for (const vector::MotionParam& prm : f.params) {
        bool changes = false;
        for (const MotionClip& c : f.clips) {
            if (const auto it = c.fixed.find(prm.name); it != c.fixed.end())
                fixed += (fixed.empty() ? "" : ", ") + prm.name + " в «" + c.name + "» (" + it->second + ")";
            for (const vector::MotionTrack& t : c.tracks)
                for (const MotionPiece& pc : t.pieces)
                    changes = changes || std::any_of(pc.by.begin(), pc.by.end(), [&](const vector::MotionBy& b) { return b.param == prm.name; });
        }
        if (changes) changing += (changing.empty() ? "" : ", ") + prm.name;
    }
    if (!changing.empty() || !fixed.empty())
        out.notes.push_back((changing.empty() ? std::string() : "Из игры меняются: " + changing) + (!changing.empty() && !fixed.empty() ? ". " : "") +
                            (fixed.empty() ? std::string() : "Не меняются: " + fixed));
    return true;
}

} // namespace forge::pixel
