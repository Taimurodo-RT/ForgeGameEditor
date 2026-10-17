#include "forge/pixel/motion.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace forge::pixel {

namespace {

constexpr const char* kPropWords[] = {"x", "y", "rot", "scale", "sx", "sy", "opacity", "fill", "morph"};
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

bool prop_is_number(Prop p) { return p != Prop::Fill && p != Prop::Morph; }

PropValue rest_value(Prop p) {
    PropValue v;
    v.v = (p == Prop::Scale || p == Prop::Sx || p == Prop::Sy || p == Prop::Opacity) ? 1 : 0;
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
    k = std::clamp(k, 0.0, 1.0);
    if (k == 0) return a;
    if (k == 1) return b;
    if (a.v <= 0 && b.v <= 0) return a;
    if (p == Prop::Fill) {
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
        case Prop::Opacity: pp.opacity = v.v; break;
        case Prop::Fill:
            pp.has_fill = v.v > 0;
            pp.fill = v.color;
            pp.fill_k = v.v;
            break;
        case Prop::Morph: break;
        }
    }
    return true;
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
