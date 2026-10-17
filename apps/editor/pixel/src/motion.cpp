#include "forge/pixel/motion.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace forge::pixel {

namespace {

constexpr const char* kPropWords[] = {"x",           "y",         "rot",         "scale",       "sx",
                                     "sy",          "skew_x",    "skew_y",      "opacity",     "fill",
                                     "stroke",      "stroke_width", "dash",     "draw_from",   "draw_to",
                                     "visible",     "tint",      "flash",       "flash_color", "blur",
                                     "shadow",      "shadow_x",  "shadow_y",    "shadow_blur", "morph",
                                     "маска",       "крепление",   "порядок"};
static_assert(std::size(kPropWords) == kPropCount);

u8 mix8(u8 a, u8 b, f64 k) { return static_cast<u8>(std::clamp(std::round(a + (b - a) * k), 0.0, 255.0)); }

f64 curve_at(const std::vector<f64>& c, f64 s) {
    if (c.empty()) return s;
    const f64 x = std::clamp(s, 0.0, 1.0) * static_cast<f64>(c.size() - 1);
    const auto i = static_cast<usize>(x);
    if (i + 1 >= c.size()) return c.back();
    return c[i] + (c[i + 1] - c[i]) * (x - static_cast<f64>(i));
}

bool before(const Block& a, const Block& b) { return a.start != b.start ? a.start < b.start : a.order < b.order; }

bool along_ok(const Recording& rec, const Block& b) {
    return b.ref < rec.alongs.size() && rec.alongs[b.ref].path < rec.paths.size() && rec.paths[rec.alongs[b.ref].path].points.size() >= 2;
}

// A block of along (part 4в) at share k of its timing: its x, y or rot.
f64 along_at(const Recording& rec, const Block& b, f64 k) {
    const Along& al = rec.alongs[b.ref];
    const PathLine& path = rec.paths[al.path];
    const f64 u = al.from + (al.to - al.from) * k;
    f64 angle = 0;
    const vector::Point p = path_point(path, u, &angle);
    if (b.prop == Prop::X || b.prop == Prop::Y) {
        const bool x = b.prop == Prop::X;
        if (al.relative) {
            const vector::Point p0 = path_point(path, al.from);
            return b.from.v + (x ? p.x - p0.x : p.y - p0.y);
        }
        return x ? p.x - al.pivot.x : p.y - al.pivot.y;
    }
    // rot: the way it goes, within −180 … 180 where it starts.
    f64 a0 = 0;
    path_point(path, al.from, &a0);
    if (al.to < al.from) angle += 180, a0 += 180;
    return angle - (a0 - std::remainder(a0, 360.0));
}

// Where a part stands by its bones (part 4в): links' carries are made sure of first by `ensure`, while resolve
// computes them in an order of its own.
struct World {
    const Recording& rec;
    BindingEval* eval;
    std::string& why;
    bool (*ensure)(void* self, u32 block) = nullptr;
    void* self = nullptr;

    // C · W(parent) · M, M its own move round its pivot.
    bool at(u32 bone, f64 t, Affine& out, u32 depth = 0) {
        Affine frame;
        if (!this->frame(bone, t, frame, depth)) return false;
        const Bone& bn = rec.bones[bone];
        PartPose pp;
        const auto named = std::find(rec.parts.begin(), rec.parts.end(), bn.id);
        if (named != rec.parts.end()) {
            const auto part = static_cast<u32>(named - rec.parts.begin());
            f64* fields[] = {&pp.x, &pp.y, &pp.rot, &pp.scale, &pp.sx, &pp.sy, &pp.skew_x, &pp.skew_y};
            for (usize i = 0; i < std::size(fields); ++i) {
                const i32 tr = rec.track(part, static_cast<Prop>(i));
                if (tr < 0) continue;
                PropValue v;
                if (!sample(rec, static_cast<u32>(tr), t, eval, v, why)) return false;
                *fields[i] = v.v;
            }
        }
        out = frame * pose_matrix(pp, bn.pivot);
        return true;
    }

    // C · W(parent): all but its own move.
    bool frame(u32 bone, f64 t, Affine& out, u32 depth = 0) {
        if (bone >= rec.bones.size()) return why = "у записи нет кости части", false;
        if (depth > 64) return why = "крепления по кругу", false;
        const Bone& bn = rec.bones[bone];
        Affine parent;
        if (bn.parent >= 0 && !at(static_cast<u32>(bn.parent), t, parent, depth + 1)) return false;
        Affine c;
        const auto named = std::find(rec.parts.begin(), rec.parts.end(), bn.id);
        if (named != rec.parts.end()) {
            const auto part = static_cast<u32>(named - rec.parts.begin());
            const i32 lt = rec.track(part, Prop::Link);
            const i32 d = lt < 0 ? -1 : deciding_block(rec, static_cast<u32>(lt), t);
            if (d >= 0) {
                if (ensure && !ensure(self, static_cast<u32>(d))) return false;
                const Block& lb = rec.blocks[static_cast<usize>(d)];
                c = lb.carry;
                if (lb.value.v > 0) {
                    const i32 tb = bone_of(rec, lb.value.target);
                    Affine w;
                    if (tb < 0) return why = "у записи нет кости части «" + lb.value.target + "»", false;
                    if (!at(static_cast<u32>(tb), t, w, depth + 1)) return false;
                    c = w * c;
                }
            }
        }
        out = c * parent;
        return true;
    }
};

// resolve()'s pass of links: each one's carry from where its target stood at its start, a link before the ones that
// read it.
struct LinkPass {
    Recording& rec;
    BindingEval* eval;
    std::string& why;
    std::vector<u8> state; // by block: 0 not yet, 1 under way, 2 done

    static bool ensure_fn(void* self, u32 block) { return static_cast<LinkPass*>(self)->ensure(block); }

    bool ensure(u32 i) {
        if (state[i] == 2) return true;
        if (state[i] == 1) return why = "крепления по кругу", false;
        state[i] = 1;
        Block& b = rec.blocks[i];
        World w{rec, eval, why, &ensure_fn, this};
        // Where the link before it had carried the part by now.
        Affine before;
        const i32 tr = rec.track(b.part, Prop::Link);
        const std::vector<u32>& on = rec.tracks[static_cast<usize>(tr)].blocks;
        const auto it = std::find(on.begin(), on.end(), i);
        if (it != on.begin()) {
            const u32 p = *(it - 1);
            if (!ensure(p)) return false;
            const Block& pb = rec.blocks[p];
            before = pb.carry;
            if (pb.value.v > 0) {
                const i32 tb = bone_of(rec, pb.value.target);
                Affine wt;
                if (tb < 0) return why = "у записи нет кости части «" + pb.value.target + "»", false;
                if (!w.at(static_cast<u32>(tb), b.start, wt)) return false;
                before = wt * before;
            }
        }
        if (b.value.v > 0) {
            const i32 tb = bone_of(rec, b.value.target);
            Affine wt;
            if (tb < 0) return why = "у записи нет кости части «" + b.value.target + "»", false;
            if (!w.at(static_cast<u32>(tb), b.start, wt)) return false;
            b.carry = inverse(wt) * before;
        } else {
            b.carry = before;
        }
        state[i] = 2;
        return true;
    }
};

} // namespace

const char* prop_word(Prop p) {
    const auto i = static_cast<usize>(p);
    return i < kPropCount ? kPropWords[i] : kPropWords[0];
}

bool prop_from_word(std::string_view word, Prop& out) {
    for (usize i = 0; i < kPropCount; ++i)
        if (word == kPropWords[i]) {
            out = static_cast<Prop>(i);
            return true;
        }
    return false;
}

bool prop_is_number(Prop p) {
    return !prop_is_color(p) && p != Prop::StrokeWidth && p != Prop::Visible && p != Prop::Morph && !prop_is_call(p);
}

bool prop_is_color(Prop p) {
    return p == Prop::Fill || p == Prop::Stroke || p == Prop::Tint || p == Prop::FlashColor || p == Prop::Shadow;
}

bool prop_is_call(Prop p) { return p == Prop::Mask || p == Prop::Link || p == Prop::Order; }

PropValue rest_value(Prop p) {
    PropValue v;
    v.v = (p == Prop::Scale || p == Prop::Sx || p == Prop::Sy || p == Prop::Opacity || p == Prop::DrawTo ||
           p == Prop::Visible)
              ? 1
              : 0;
    return v;
}

PropValue mix(Prop p, const PropValue& a, const PropValue& b, f64 k) {
    if (k == 0) return a;
    if (k == 1) return b;
    PropValue r;
    if (prop_is_number(p)) {
        r.v = a.v + (b.v - a.v) * k; // past 0 … 1 too: back and elastic overshoot
        return r;
    }
    if (p == Prop::Visible || prop_is_call(p)) return k >= 1 ? b : a; // no values between
    if (p == Prop::StrokeWidth && a.v == 1 && b.v == 1) {
        r.v = 1;
        r.n = a.n + (b.n - a.n) * k; // two widths given: as numbers, past them too
        return r;
    }
    k = std::clamp(k, 0.0, 1.0);
    if (k == 0) return a;
    if (k == 1) return b;
    if (a.v <= 0 && b.v <= 0) return a;
    if (p == Prop::StrokeWidth) {
        // Each shape's width is own · (1 − v) + n · v; mixed linearly for every own width at once.
        r.v = a.v + (b.v - a.v) * k;
        r.n = (std::max(a.v, 0.0) * a.n * (1 - k) + std::max(b.v, 0.0) * b.n * k) / r.v;
        return r;
    }
    if (prop_is_color(p)) {
        if (a.v <= 0) {
            r.color = b.color;
            r.v = b.v * k;
        } else if (b.v <= 0) {
            r.color = a.color;
            r.v = a.v * (1 - k);
        } else {
            r.color = {mix8(a.color.r, b.color.r, k), mix8(a.color.g, b.color.g, k), mix8(a.color.b, b.color.b, k),
                       mix8(a.color.a, b.color.a, k)};
            r.v = a.v + (b.v - a.v) * k;
        }
        return r;
    }
    if (a.v <= 0) {
        r.target = b.target;
        r.v = b.v * k;
    } else if (b.v <= 0) {
        r.target = a.target;
        r.v = a.v * (1 - k);
    } else if (a.target == b.target) {
        r.target = a.target;
        r.v = a.v + (b.v - a.v) * k;
    } else if (k < 0.5) {
        r.target = a.target;
        r.v = a.v * (1 - 2 * k);
    } else {
        r.target = b.target;
        r.v = b.v * (2 * k - 1);
    }
    return r;
}

f64 snap_time(f64 t) { return std::round(t * 1e9) / 1e9; }

i32 Recording::track(u32 part, Prop p) const {
    const usize i = part * kPropCount + static_cast<usize>(p);
    return i < track_of.size() ? track_of[i] : -1;
}

bool block_value(const Recording& rec, const Block& b, f64 t, BindingEval* eval, PropValue& out, std::string& why) {
    if (b.to == Block::To::Binding) {
        if (!eval) return why = "связь без скрипта", false;
        return eval->eval(rec, b.binding, b.prop, t, out, why);
    }
    const f64 local = snap_time(t - b.start); // on the recording's nanoseconds, so its end is its end
    if (b.to == Block::To::Along) {
        // On the path from its start: the part is at the path's point `from` at once, not where it was.
        if (!along_ok(rec, b)) return why = "along: нет пути", false;
        if (local < 0) {
            out = b.from;
            return true;
        }
        f64 k = 1;
        if (b.length > 0 && local < b.length) {
            const f64 s = local / b.length;
            k = b.curve >= 0 && static_cast<usize>(b.curve) < rec.curves.size() ? curve_at(rec.curves[static_cast<usize>(b.curve)], s)
                                                                               : vector::timing(b.timing, s);
        }
        out = {};
        out.v = along_at(rec, b, k);
        return true;
    }
    if (b.length <= 0) {
        out = local < 0 ? b.from : b.end;
        return true;
    }
    if (local <= 0) {
        out = b.from;
        return true;
    }
    if (local >= b.length) { // exactly where it goes, as Motion Canvas sets it at the end
        out = b.end;
        return true;
    }
    if (b.spring) {
        const f64 x = local * vector::kSpringRate;
        const auto i = static_cast<usize>(x);
        const f64 p0 = i == 0 ? b.from.v : (i - 1 < b.steps.size() ? b.steps[i - 1] : b.end.v);
        const f64 p1 = i < b.steps.size() ? b.steps[i] : b.end.v;
        out = {};
        out.v = p0 + (p1 - p0) * (x - static_cast<f64>(i));
        return true;
    }
    if (b.prop == Prop::Visible && b.frames.empty()) { // at its end, whatever its timing does on the way
        out = b.from;
        return true;
    }
    const f64 s = std::min(local / b.length, 1.0);
    if (!b.frames.empty()) {
        const f64 x = s * static_cast<f64>(b.frames.size() - 1);
        const auto i = static_cast<usize>(x);
        out = i + 1 >= b.frames.size() ? b.frames.back() : mix(b.prop, b.frames[i], b.frames[i + 1], x - static_cast<f64>(i));
        return true;
    }
    f64 k = b.curve >= 0 && static_cast<usize>(b.curve) < rec.curves.size() ? curve_at(rec.curves[static_cast<usize>(b.curve)], s)
                                                                           : vector::timing(b.timing, s);
    if (b.arc >= 0 && static_cast<usize>(b.arc) < rec.blocks.size()) {
        const Block& o = rec.blocks[static_cast<usize>(b.arc)];
        const Block& bx = b.prop == Prop::X ? b : o;
        const Block& by = b.prop == Prop::X ? o : b;
        f64 kx = k, ky = k;
        vector::arc_shares(k, b.arc_reverse, (bx.from.v - bx.end.v) / (by.from.v - by.end.v), kx, ky);
        k = b.prop == Prop::X ? kx : ky;
    }
    out = mix(b.prop, b.from, b.end, k);
    return true;
}

bool resolve(Recording& rec, BindingEval* eval, std::string& why) {
    why.clear();
    rec.tracks.clear();
    rec.track_of.assign(rec.parts.size() * kPropCount, -1);
    for (u32 i = 0; i < rec.blocks.size(); ++i) {
        const Block& b = rec.blocks[i];
        if (b.part >= rec.parts.size()) return why = "блок без части", false;
        i32& ti = rec.track_of[b.part * kPropCount + static_cast<usize>(b.prop)];
        if (ti < 0) {
            ti = static_cast<i32>(rec.tracks.size());
            rec.tracks.push_back({b.part, b.prop, {}});
        }
        rec.tracks[static_cast<usize>(ti)].blocks.push_back(i);
    }
    for (Recording::Track& t : rec.tracks)
        std::stable_sort(t.blocks.begin(), t.blocks.end(),
                         [&](u32 a, u32 b) { return before(rec.blocks[a], rec.blocks[b]); });

    // Blocks and saves in the order of time, each from where its track is by then.
    struct Event {
        f64 t;
        u32 order;
        bool save;
        u32 index;
    };
    std::vector<Event> events;
    events.reserve(rec.blocks.size() + rec.saves.size());
    for (u32 i = 0; i < rec.blocks.size(); ++i) events.push_back({rec.blocks[i].start, rec.blocks[i].order, false, i});
    for (u32 i = 0; i < rec.saves.size(); ++i) events.push_back({rec.saves[i].time, rec.saves[i].order, true, i});
    std::stable_sort(events.begin(), events.end(),
                     [](const Event& a, const Event& b) { return a.t != b.t ? a.t < b.t : a.order < b.order; });
    std::vector<i32> last(rec.tracks.size(), -1);
    for (const Event& e : events) {
        if (e.save) {
            Saved& s = rec.saves[e.index];
            for (usize p = 0; p < kPropCount; ++p) {
                const i32 ti = rec.track(s.part, static_cast<Prop>(p));
                const i32 li = ti >= 0 ? last[static_cast<usize>(ti)] : -1;
                if (li < 0) s.values[p] = rest_value(static_cast<Prop>(p));
                else if (!block_value(rec, rec.blocks[static_cast<usize>(li)], s.time, eval, s.values[p], why)) return false;
            }
            continue;
        }
        Block& b = rec.blocks[e.index];
        const i32 ti = rec.track(b.part, b.prop);
        const i32 li = last[static_cast<usize>(ti)];
        if (li < 0) b.from = rest_value(b.prop);
        else if (!block_value(rec, rec.blocks[static_cast<usize>(li)], b.start, eval, b.from, why)) return false;
        switch (b.to) {
        case Block::To::Value: b.end = b.value; break;
        case Block::To::Back:
            if (b.ref >= rec.blocks.size()) return why = "back: нет блока, к которому вернуться", false;
            b.end = rec.blocks[b.ref].from;
            break;
        case Block::To::Saved:
            if (b.ref >= rec.saves.size()) return why = "restore: нет сохранённого", false;
            b.end = rec.saves[b.ref].values[static_cast<usize>(b.prop)];
            break;
        case Block::To::Rest: b.end = rest_value(b.prop); break;
        case Block::To::Binding: b.end = {}; break;
        case Block::To::Snap: b.end = b.value; break;
        case Block::To::Along:
            if (!along_ok(rec, b)) return why = "along: нет пути", false;
            b.end = {};
            b.end.v = along_at(rec, b, 1);
            break;
        }
        if (b.spring) {
            const char* w = nullptr;
            if (!vector::spring_steps(b.spring_def, b.from.v, b.end.v, b.tolerance, b.steps, w,
                                      std::max(60.0, b.length + 1)))
                return why = w ? w : "пружина", false;
        }
        if (!b.frames.empty()) b.end = b.frames.back();
        last[static_cast<usize>(ti)] = static_cast<i32>(e.index);
    }
    // Links (part 4в): their carries, once every value is known.
    LinkPass pass{rec, eval, why, std::vector<u8>(rec.blocks.size(), 0)};
    for (u32 i = 0; i < rec.blocks.size(); ++i)
        if (rec.blocks[i].prop == Prop::Link && !pass.ensure(i)) return false;
    return true;
}

i32 deciding_block(const Recording& rec, u32 track, f64 t) {
    if (track >= rec.tracks.size()) return -1;
    const Recording::Track& tr = rec.tracks[track];
    // The last block (by start, then order) that started at t or before.
    const f64 at = snap_time(t);
    const auto it = std::upper_bound(tr.blocks.begin(), tr.blocks.end(), at,
                                     [&](f64 time, u32 i) { return time < rec.blocks[i].start; });
    return it == tr.blocks.begin() ? -1 : static_cast<i32>(*(it - 1));
}

bool block_going(const Block& b, f64 t, f64 frame) {
    const f64 local = snap_time(t - b.start); // as block_value reads it
    if (b.to == Block::To::Binding) return local >= 0;
    if (b.length <= 0) return local >= 0 && local < frame;
    return local > 0 && local <= b.length;
}

i32 latest_block(const Recording& rec, u32 part, f64 t) {
    i32 best = -1;
    for (usize p = 0; p < kPropCount; ++p) {
        const i32 tr = rec.track(part, static_cast<Prop>(p));
        const i32 d = tr < 0 ? -1 : deciding_block(rec, static_cast<u32>(tr), t);
        if (d >= 0 && (best < 0 || rec.blocks[static_cast<usize>(d)].start > rec.blocks[static_cast<usize>(best)].start)) best = d;
    }
    return best;
}

bool sample(const Recording& rec, u32 track, f64 t, BindingEval* eval, PropValue& out, std::string& why) {
    if (track >= rec.tracks.size()) return why = "нет такой дорожки", false;
    const i32 d = deciding_block(rec, track, t);
    if (d < 0) {
        out = rest_value(rec.tracks[track].prop);
        return true;
    }
    return block_value(rec, rec.blocks[static_cast<usize>(d)], snap_time(t), eval, out, why);
}

bool sample_pose(const Recording& rec, f64 t, BindingEval* eval, Pose& out, std::string& why) {
    for (u32 i = 0; i < rec.tracks.size(); ++i) {
        const Recording::Track& tr = rec.tracks[i];
        if (tr.prop == Prop::Order) continue; // all calls of all parts up to t, below
        PropValue v;
        if (!sample(rec, i, t, eval, v, why)) return false;
        const std::string& id = rec.parts[tr.part];
        if (tr.prop == Prop::Morph) {
            if (v.v > 0) out.morphs[id] = {v.target, v.v};
            continue;
        }
        PartPose& pp = out.parts[id];
        switch (tr.prop) {
        case Prop::X: pp.x = v.v; break;
        case Prop::Y: pp.y = v.v; break;
        case Prop::Rot: pp.rot = v.v; break;
        case Prop::Scale: pp.scale = v.v; break;
        case Prop::Sx: pp.sx = v.v; break;
        case Prop::Sy: pp.sy = v.v; break;
        case Prop::SkewX: pp.skew_x = v.v; break;
        case Prop::SkewY: pp.skew_y = v.v; break;
        case Prop::Opacity: pp.opacity = v.v; break;
        case Prop::Fill:
            pp.has_fill = v.v > 0;
            pp.fill = v.color;
            pp.fill_k = v.v;
            break;
        case Prop::Stroke:
            pp.has_stroke = v.v > 0;
            pp.stroke = v.color;
            pp.stroke_k = v.v;
            break;
        case Prop::StrokeWidth:
            pp.has_stroke_width = v.v > 0;
            pp.stroke_width = v.n;
            pp.stroke_width_k = v.v;
            break;
        case Prop::Dash: pp.dash = v.v; break;
        case Prop::DrawFrom: pp.draw_from = v.v; break;
        case Prop::DrawTo: pp.draw_to = v.v; break;
        case Prop::Visible: pp.visible = v.v > 0; break;
        case Prop::Tint:
            pp.has_tint = v.v > 0;
            pp.tint = v.color;
            pp.tint_k = v.v;
            break;
        case Prop::Flash: pp.flash = std::clamp(v.v, 0.0, 1.0); break; // back and springs overshoot: held in 0 … 1
        case Prop::FlashColor:
            pp.has_flash_color = v.v > 0;
            pp.flash_color = v.color;
            pp.flash_color_k = v.v;
            break;
        case Prop::Blur: pp.blur = std::max(v.v, 0.0); break;
        case Prop::Shadow:
            pp.has_shadow = v.v > 0;
            pp.shadow = v.color;
            pp.shadow_k = v.v;
            break;
        case Prop::ShadowX: pp.shadow_x = v.v; break;
        case Prop::ShadowY: pp.shadow_y = v.v; break;
        case Prop::ShadowBlur: pp.shadow_blur = std::max(v.v, 0.0); break;
        case Prop::Mask:
            pp.mask = v.v > 0 ? v.target : std::string();
            pp.mask_invert = v.v > 1;
            break;
        case Prop::Link: {
            const i32 d = deciding_block(rec, i, t);
            if (d < 0) break;
            const Block& lb = rec.blocks[static_cast<usize>(d)];
            pp.carry_by = lb.value.v > 0 ? lb.value.target : std::string();
            pp.carry = lb.carry;
            break;
        }
        case Prop::Morph:
        case Prop::Order: break;
        }
    }
    // The order: from the SVG's, every call up to t in the order of time (start, then order).
    std::vector<u32> calls;
    const f64 at = snap_time(t);
    for (u32 i = 0; i < rec.blocks.size(); ++i)
        if (rec.blocks[i].prop == Prop::Order && rec.blocks[i].start <= at) calls.push_back(i);
    std::stable_sort(calls.begin(), calls.end(), [&](u32 a, u32 b) { return before(rec.blocks[a], rec.blocks[b]); });
    for (u32 i : calls) {
        const Block& b = rec.blocks[i];
        OrderStep step;
        step.part = rec.parts[b.part];
        step.kind = static_cast<OrderStep::Kind>(std::clamp(static_cast<int>(b.value.v), 0, 3));
        step.other = b.value.target;
        out.order.push_back(std::move(step));
    }
    return true;
}

vector::Point path_point(const PathLine& path, f64 u, f64* angle) {
    if (angle) *angle = 0;
    const usize n = path.points.size();
    if (n == 0) return {};
    if (n < 2 || path.lengths.size() != n || path.angles.size() + 1 != n) return path.points[0];
    f64 laps = 0;
    if (!std::isfinite(u)) u = 0;
    if (path.closed) {
        laps = std::floor(u);
        u -= laps;
    } else {
        u = std::clamp(u, 0.0, 1.0);
    }
    const f64 d = u * path.length();
    const auto it = std::upper_bound(path.lengths.begin(), path.lengths.end(), d);
    const usize i = std::min<usize>(static_cast<usize>(std::max<std::ptrdiff_t>(it - path.lengths.begin() - 1, 0)), n - 2);
    const f64 seg = path.lengths[i + 1] - path.lengths[i];
    const f64 f = seg > 0 ? std::clamp((d - path.lengths[i]) / seg, 0.0, 1.0) : 0;
    const vector::Point a = path.points[i], b = path.points[i + 1];
    if (angle) {
        // Each time round a closed path its direction is on by the turn of one round.
        const f64 lap = path.closed ? path.angles.back() + std::remainder(path.angles.front() - path.angles.back(), 360.0) - path.angles.front() : 0;
        *angle = path.angles[i] + laps * lap;
    }
    return {a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f};
}

i32 bone_of(const Recording& rec, std::string_view id) {
    for (usize i = 0; i < rec.bones.size(); ++i)
        if (rec.bones[i].id == id) return static_cast<i32>(i);
    return -1;
}

bool bone_world(const Recording& rec, u32 bone, f64 t, BindingEval* eval, Affine& out, std::string& why) {
    World w{rec, eval, why};
    return w.at(bone, snap_time(t), out);
}

bool bone_frame(const Recording& rec, u32 bone, f64 t, BindingEval* eval, Affine& out, std::string& why) {
    World w{rec, eval, why};
    return w.frame(bone, snap_time(t), out);
}

void layer_pose(Pose& base, const Pose& over) {
    for (const auto& [id, o] : over.parts) {
        PartPose& b = base.parts[id];
        b.x += o.x;
        b.y += o.y;
        b.rot += o.rot;
        b.scale *= o.scale;
        b.sx *= o.sx;
        b.sy *= o.sy;
        b.opacity *= o.opacity;
        if (o.has_fill) {
            b.has_fill = true;
            b.fill = o.fill;
            b.fill_k = o.fill_k;
        }
    }
    for (const auto& [id, m] : over.morphs) base.morphs[id] = m;
}

} // namespace forge::pixel
