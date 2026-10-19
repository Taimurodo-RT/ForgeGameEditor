#include "forge/pixel/timeline.h"

#include "forge/pixel/scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace forge::pixel {

f64 TimeView::fit() const {
    const f64 shown = std::max(length, fps > 0 ? 1.0 / fps : 0.1);
    return std::max(1e-9, width) / shown;
}

f64 TimeView::most() const { return kMostPerFrame * std::max(1e-3f, fps) * std::max(1e-3, dp); }

void TimeView::zoom(f64 factor, f64 at_x) {
    if (!(factor > 0) || !std::isfinite(factor)) return;
    const f64 at = t(at_x);
    scale = std::clamp(per_second() * factor, fit(), std::max(fit(), most()));
    start = at - at_x / scale;
    clamp();
}

void TimeView::pan(f64 dx) {
    start += dx / per_second();
    clamp();
}

void TimeView::center(f64 at) {
    start = at - width / 2 / per_second();
    clamp();
}

void TimeView::clamp() {
    scale = std::clamp(per_second(), fit(), std::max(fit(), most()));
    const f64 shown = width / scale;
    start = std::clamp(start, 0.0, std::max(0.0, length - shown));
}

u32 ruler_step(f64 points_per_frame, f64 gap) {
    if (!(points_per_frame > 0)) return 1;
    for (u32 decade = 1;; decade *= 10) {
        for (const u32 m : {1u, 2u, 5u})
            if (m * decade * points_per_frame >= gap) return m * decade;
        if (decade > 100'000'000) return decade * 10;
    }
}

MotionLengths drag_flag(const Recording& rec, const MotionLengths& now, usize event, f64 to, bool only_this) {
    MotionLengths out = now;
    if (event >= rec.events.size()) return out;
    const TimeEvent& e = rec.events[event];
    auto thousandths = [](f64 s) { return std::max(0.0, std::round(s * 1000) / 1000); };
    auto set = [&](const TimeEvent& ev, f64 s) { (ev.wait ? out.waits : out.durations)[ev.name] = s; };
    const f64 length = thousandths(to - e.start);
    set(e, length);
    if (!only_this) return out;
    // The next event after this one's end, of another name: it gives up what this one took.
    const TimeEvent* next = nullptr;
    for (const TimeEvent& o : rec.events) {
        if ((o.name == e.name && o.wait == e.wait) || o.start < e.start + e.length - 1e-9) continue;
        if (!next || o.start < next->start) next = &o;
    }
    if (next) set(*next, thousandths(next->length - (length - e.length)));
    return out;
}

std::string value_words(Prop p, const PropValue& v) {
    if (prop_is_color(p)) {
        if (v.v <= 0) return p == Prop::Tint ? "без тона" : p == Prop::FlashColor ? "белый" : p == Prop::Shadow ? "без тени" : "свой цвет";
        return color_hex(v.color) + (v.v < 1 ? " " + format_number(v.v) : std::string());
    }
    if (p == Prop::StrokeWidth) {
        if (v.v <= 0) return "своя толщина";
        return format_number(v.n) + (v.v < 1 ? " " + format_number(v.v) : std::string());
    }
    if (p == Prop::Visible) return v.v > 0 ? "да" : "нет";
    if (p == Prop::Mask) {
        if (v.v <= 0) return "без маски";
        return "маска " + v.target + (v.v > 1 ? ", наоборот" : "");
    }
    if (p == Prop::Link) return v.v > 0 ? "с " + v.target : "отпущена";
    if (p == Prop::Order) {
        switch (static_cast<OrderStep::Kind>(std::clamp(static_cast<int>(v.v), 0, 3))) {
        case OrderStep::Kind::Top: return "наверх";
        case OrderStep::Kind::Bottom: return "вниз";
        case OrderStep::Kind::Above: return "над " + v.target;
        case OrderStep::Kind::Below: return "под " + v.target;
        }
    }
    if (p == Prop::Drawing) return v.target.empty() ? "draw" : v.target; // part 4д
    if (p == Prop::Morph) {
        if (v.v <= 0 || v.target.empty()) return "своя форма";
        return v.target + (v.v < 1 ? " " + format_number(v.v) : std::string());
    }
    return format_number(v.v);
}

std::string block_words(const Recording& rec, const Block& b) {
    // The camera's scale is its zoom (part 6б).
    const bool camera = b.part < rec.parts.size() && rec.parts[b.part] == kCameraPart;
    const std::string prop = camera && b.prop == Prop::Scale ? "zoom" : prop_word(b.prop);
    switch (b.to) {
    case Block::To::Binding: return "связь";
    case Block::To::Back: return prop + " → назад";
    case Block::To::Saved: return prop + " → restore";
    case Block::To::Rest: return prop + " → покой";
    case Block::To::Along: {
        const std::string path = b.ref < rec.alongs.size() && rec.alongs[b.ref].path < rec.paths.size() ? rec.paths[rec.alongs[b.ref].path].id : "?";
        return "по " + path + (b.prop == Prop::Rot ? ", поворот" : "");
    }
    case Block::To::Snap: return "к " + (b.ref < rec.snaps.size() ? rec.snaps[b.ref] : std::string("?"));
    case Block::To::Follow: return "следит за " + (b.ref < rec.follows.size() ? rec.follows[b.ref].target : std::string("?"));
    case Block::To::Hold: return "стоит";
    case Block::To::Value: break;
    }
    if (!b.frames.empty()) return "tween";
    const std::string to = value_words(b.prop, b.value);
    if (prop_is_call(b.prop)) return to; // «наверх», «маска lid», «с hand»: the track says what it is
    if (b.prop == Prop::Drawing) return "→ " + to; // «→ angry» on the track «рисунок» (part 4д)
    return b.spring ? "пружина → " + to : prop + " → " + to;
}

u64 other_seed(u64 seed) {
    u64 x = seed + 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x ^= x >> 31;
    const u64 next = x % 1'000'000;
    return next == seed ? (next + 1) % 1'000'000 : next;
}

void span_lines(std::string_view source, CodeSpan span, u32& first, u32& last) {
    const usize begin = std::min<usize>(span.begin, source.size());
    const usize end = std::min<usize>(std::max(span.end, span.begin), source.size());
    first = 1 + static_cast<u32>(std::count(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(begin), '\n'));
    last = first + static_cast<u32>(std::count(source.begin() + static_cast<std::ptrdiff_t>(begin),
                                               source.begin() + static_cast<std::ptrdiff_t>(end > begin ? end - 1 : begin), '\n'));
}

u32 char_index(std::string_view source, u32 byte) {
    u32 n = 0;
    for (usize i = 0; i < source.size() && i < byte; ++i)
        if ((static_cast<u8>(source[i]) & 0xC0) != 0x80) ++n;
    return n;
}

u32 byte_index(std::string_view source, u32 character) {
    u32 n = 0;
    for (usize i = 0; i < source.size(); ++i)
        if ((static_cast<u8>(source[i]) & 0xC0) != 0x80) {
            if (n == character) return static_cast<u32>(i);
            ++n;
        }
    return static_cast<u32>(source.size());
}

namespace {

// How many characters of the panel byte i of the file is: a UTF-8 continuation byte and "\r" none, a tab kTabSpaces.
u32 panel_width(std::string_view file, usize i) {
    const u8 c = static_cast<u8>(file[i]);
    if ((c & 0xC0) == 0x80 || c == '\r') return 0;
    return c == '\t' ? kTabSpaces : 1;
}

} // namespace

std::string panel_text(std::string_view file, bool& crlf) {
    usize pairs = 0, bare = 0;
    std::string out;
    out.reserve(file.size());
    for (usize i = 0; i < file.size(); ++i) {
        const char c = file[i];
        if (c == '\r') continue;
        if (c == '\n') ++(i > 0 && file[i - 1] == '\r' ? pairs : bare);
        if (c == '\t') out.append(kTabSpaces, ' ');
        else out += c;
    }
    crlf = pairs > bare;
    return out;
}

std::string file_text(std::string_view panel, bool crlf) {
    if (!crlf) return std::string(panel);
    std::string out;
    out.reserve(panel.size() + panel.size() / 16);
    for (const char c : panel) {
        if (c == '\n') out += '\r';
        out += c;
    }
    return out;
}

u32 panel_char(std::string_view file, u32 byte) {
    u32 n = 0;
    for (usize i = 0; i < file.size() && i < byte; ++i) n += panel_width(file, i);
    return n;
}

u32 file_byte(std::string_view file, u32 character) {
    u32 n = 0;
    for (usize i = 0; i < file.size(); ++i) {
        const u32 w = panel_width(file, i);
        if (w == 0) continue;
        if (n + w > character) return static_cast<u32>(i);
        n += w;
    }
    return static_cast<u32>(file.size());
}

u32 line_char(std::string_view text, u32 line) {
    u32 n = 0, at = 1;
    for (usize i = 0; i < text.size() && at < line; ++i) {
        if ((static_cast<u8>(text[i]) & 0xC0) != 0x80) ++n;
        if (text[i] == '\n') ++at;
    }
    return n;
}

i32 site_at(const CodeSites& code, u32 at) {
    i32 best = -1;
    u32 best_size = ~0u;
    for (usize i = 0; i < code.sites().size(); ++i) {
        const CodeSpan s = code.sites()[i].span;
        if (at < s.begin || at > s.end) continue;
        if (s.end - s.begin < best_size) {
            best_size = s.end - s.begin;
            best = static_cast<i32>(i);
        }
    }
    return best;
}

} // namespace forge::pixel
