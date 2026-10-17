#include "forge/pixel/motion_edit.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace forge::pixel {

namespace {

// The order of parts, the mask and the link have no value, length or start to drag: they are written in the code
// (parts 4а, 4б and 4в).
constexpr const char* kOrderInCode = "порядок частей правится в коде: to_top, to_bottom, above, below";
constexpr const char* kMaskInCode = "маска правится в коде: mask(\"lid\"), mask(nil)";
constexpr const char* kLinkInCode = "крепление правится в коде: attach(\"hand\"), detach()";
const char* in_code(Prop p) { return p == Prop::Mask ? kMaskInCode : p == Prop::Link ? kLinkInCode : kOrderInCode; }

// The call of s in the character's script; null with why for a module's call or none.
const CallSite* script_call(const Recording& rec, SiteRef s, const CodeSites& code, std::string& why) {
    if (s.site < 0) return why = "у блока нет вызова в коде", nullptr;
    if (s.file != 0) {
        const std::string name = s.file < rec.files.size() ? rec.files[s.file] : "?";
        return why = "блок из общего модуля «" + name + ".luau»: правка изменит всех персонажей, правьте его в коде",
               nullptr;
    }
    if (static_cast<usize>(s.site) >= code.sites().size()) return why = "код изменился: запишите клип снова", nullptr;
    return &code.sites()[static_cast<usize>(s.site)];
}

std::string value_text(Prop p, const PropValue& v) {
    if (prop_is_number(p)) return format_number(v.v);
    if (p == Prop::Visible) return v.v > 0 ? "true" : "false";
    if (!(v.v > 0)) return "nil"; // its own colour, width or shape
    if (p == Prop::StrokeWidth) return format_number(v.n);
    if (p == Prop::Morph) return quote_string(v.target);
    return quote_string(color_hex(v.color));
}

// Argument `index` of a call as the number `value` (it was `old`), added when it is one past the last.
bool number_arg(std::string_view source, const CallSite& c, i8 index, f64 old, f64 value, std::string& out,
                std::string& why) {
    if (index < 0) return why = "в вызове нет такого числа", false;
    const auto i = static_cast<usize>(index);
    if (i < c.args.size()) return rewrite_number(source, c.args[i], old, value, out, why);
    if (i == c.args.size()) return rewrite_arg(source, c, i, format_number(value), out, why);
    return why = "в вызове не хватает аргументов до этого", false;
}

bool has_value(const Recording& rec, const Block& b, std::string& why) {
    switch (b.to) {
    case Block::To::Value: break;
    case Block::To::Along: {
        const std::string path = b.ref < rec.alongs.size() && rec.alongs[b.ref].path < rec.paths.size() ? rec.paths[rec.alongs[b.ref].path].id : "?";
        return why = "путь правится в коде: along(\"" + path + "\", …)", false;
    }
    case Block::To::Snap:
        return why = "место считается от «" + (b.ref < rec.snaps.size() ? rec.snaps[b.ref] : std::string("?")) + "»: правится в коде", false;
    case Block::To::Back: return why = "блок возвращает туда, откуда начался переход: его значения в коде нет", false;
    case Block::To::Saved: return why = "блок возвращает сохранённое save(): его значения в коде нет", false;
    case Block::To::Rest: return why = "блок возвращает в покой: его значения в коде нет", false;
    case Block::To::Binding: return why = "значение блока считает функция связи: правьте её в коде", false;
    }
    if (!b.frames.empty()) return why = "значения tween(…) считает его функция: правьте её в коде", false;
    return true;
}

// The call whose argument holds block b's value; null with why.
const CallSite* value_call(const Recording& rec, const Block& b, const CodeSites& code, std::string& why) {
    if (prop_is_call(b.prop)) return why = in_code(b.prop), nullptr;
    const CallSite* c = script_call(rec, b.site, code, why);
    if (!c || !has_value(rec, b, why)) return nullptr;
    if (b.value_arg < 0 || static_cast<usize>(b.value_arg) >= c->args.size()) return why = "в вызове нет значения", nullptr;
    if (b.value_field && c->args[static_cast<usize>(b.value_arg)].kind != ArgKind::Table)
        return why = "значения вызова не в таблице { … }: правьте в коде", nullptr;
    return c;
}

// The call whose argument holds block b's length or timing; null with why.
const CallSite* timed_call(const Recording& rec, const Block& b, const CodeSites& code, bool timing, std::string& why) {
    if (prop_is_call(b.prop)) return why = in_code(b.prop), nullptr;
    const CallSite* c = script_call(rec, b.site, code, why);
    if (!c) return nullptr;
    if (b.to == Block::To::Binding)
        return why = timing ? "у связи нет сглаживания" : "у связи нет длины: она держится до следующего блока", nullptr;
    if (b.spring)
        return why = timing ? "у пружины нет сглаживания: её ход задают масса, жёсткость и затухание"
                            : "длину пружины задают её масса, жёсткость и затухание",
               nullptr;
    if (!timing) {
        if (b.length_arg < 0) return why = "в вызове нет такого числа", nullptr;
        if (static_cast<usize>(b.length_arg) > c->args.size()) return why = "в вызове не хватает аргументов до этого", nullptr;
        return c;
    }
    if (b.timing_arg < 0) return why = "в вызове нет сглаживания", nullptr;
    const auto i = static_cast<usize>(b.timing_arg);
    if (i <= c->args.size() || (i == c->args.size() + 1 && b.length_arg == static_cast<i8>(c->args.size()))) return c;
    return why = "в вызове не хватает аргументов до сглаживания", nullptr;
}

// The at(…) or delay(…) whose number is block b's start; null with why.
const CallSite* start_call(const Recording& rec, const Block& b, const CodeSites& code, std::string& why) {
    if (prop_is_call(b.prop)) return why = in_code(b.prop), nullptr;
    if (b.start_site.site < 0)
        return why = "блок идёт за предыдущим, своего начала в коде у него нет: оно есть у at(с, …) и delay(с, …)",
               nullptr;
    const CallSite* c = script_call(rec, b.start_site, code, why);
    if (c && c->args.empty()) return why = "в вызове нет начала", nullptr;
    return c;
}

const CallSite* pause_call(const Recording& rec, const Pause& pause, const CodeSites& code, std::string& why) {
    const CallSite* c = script_call(rec, pause.site, code, why);
    if (!c) return nullptr;
    if (pause.length_arg < 0) return why = "в вызове нет такого числа", nullptr;
    if (static_cast<usize>(pause.length_arg) > c->args.size()) return why = "в вызове не хватает аргументов до этого", nullptr;
    return c;
}

} // namespace

bool can_edit(const Recording& rec, const Block& b, BlockEdit what, const CodeSites& code, std::string& why) {
    why.clear();
    switch (what) {
    case BlockEdit::Value: return value_call(rec, b, code, why) != nullptr;
    case BlockEdit::Length: return timed_call(rec, b, code, false, why) != nullptr;
    case BlockEdit::Timing: return timed_call(rec, b, code, true, why) != nullptr;
    case BlockEdit::Start: return start_call(rec, b, code, why) != nullptr;
    }
    return false;
}

bool can_edit_pause(const Recording& rec, const Pause& pause, const CodeSites& code, std::string& why) {
    why.clear();
    return pause_call(rec, pause, code, why) != nullptr;
}

bool edit_value(const Recording& rec, const Block& b, Prop p, const PropValue& value, const CodeSites& code,
                std::string_view source, std::string& out, std::string& why) {
    out.clear();
    if (prop_is_call(p)) return why = in_code(p), false;
    const CallSite* c = value_call(rec, b, code, why);
    if (!c) return false;
    const CodeArg& arg = c->args[static_cast<usize>(b.value_arg)];
    if (b.value_field) {
        if (p == Prop::Morph) return why = "морфинг — отдельным вызовом part(\"…\"):morph(…)", false;
        if (!prop_is_number(p)) return rewrite_field(source, arg, prop_word(p), value_text(p, value), out, why);
        // The old value of another property of the same call: its block from the same step.
        f64 old = p == b.prop ? b.value.v : rest_value(p).v;
        if (p != b.prop)
            for (const Block& o : rec.blocks)
                if (o.site == b.site && o.prop == p && o.start == b.start && o.to == Block::To::Value) old = o.value.v;
        return rewrite_field_number(source, arg, prop_word(p), old, value.v, out, why);
    }
    if (p != b.prop)
        return why = std::string("в вызове одного свойства (") + prop_word(b.prop) + ") другое не дописать: оно ляжет " +
                     "новой строкой at(…)",
               false;
    if (prop_is_number(p)) return rewrite_number(source, arg, b.value.v, value.v, out, why);
    return rewrite_arg(source, *c, static_cast<usize>(b.value_arg), value_text(p, value), out, why);
}

bool edit_length(const Recording& rec, const Block& b, f64 length, const CodeSites& code, std::string_view source,
                 std::string& out, std::string& why) {
    out.clear();
    if (!std::isfinite(length) || length < 0) return why = "длина — число секунд не меньше 0", false;
    const CallSite* c = timed_call(rec, b, code, false, why);
    if (!c) return false;
    return number_arg(source, *c, b.length_arg, b.length, length, out, why);
}

bool edit_timing(const Recording& rec, const Block& b, vector::Timing timing, const CodeSites& code,
                 std::string_view source, std::string& out, std::string& why) {
    out.clear();
    const CallSite* c = timed_call(rec, b, code, true, why);
    if (!c) return false;
    const auto i = static_cast<usize>(b.timing_arg);
    const std::string word = quote_string(vector::timing_word(timing));
    if (i <= c->args.size()) return rewrite_arg(source, *c, i, word, out, why);
    // body.y(2) → body.y(2, 0, "out"): its length goes first.
    return rewrite_arg(source, *c, c->args.size(), format_number(b.length) + ", " + word, out, why);
}

bool edit_start(const Recording& rec, const Block& b, f64 start, const CodeSites& code, std::string_view source,
                std::string& out, std::string& why) {
    out.clear();
    if (!std::isfinite(start)) return why = "начало — число секунд", false;
    const CallSite* c = start_call(rec, b, code, why);
    if (!c) return false;
    // at's number is the start, delay's how long after; either moves by as much as the block.
    const CodeArg& arg = c->args[0];
    const f64 old = arg.kind == ArgKind::Number ? arg.number : 0;
    const f64 value = old + (start - b.start);
    if (arg.kind == ArgKind::Number && value < 0) return why = c->name + "(с, …): секунд не меньше 0", false;
    return rewrite_number(source, arg, old, value, out, why);
}

bool edit_pause(const Recording& rec, const Pause& pause, f64 length, const CodeSites& code, std::string_view source,
                std::string& out, std::string& why) {
    out.clear();
    if (!std::isfinite(length) || length < 0) return why = "длина — число секунд не меньше 0", false;
    const CallSite* c = pause_call(rec, pause, code, why);
    if (!c) return false;
    return number_arg(source, *c, pause.length_arg, pause.length, length, out, why);
}

bool add_at_line(const Recording& rec, f64 t, std::string_view part, Prop p, const PropValue& value, f64 length,
                 const CodeSites& code, std::string_view source, std::string& out, std::string& why) {
    out.clear();
    if (!std::isfinite(t) || t < 0 || !std::isfinite(length) || length < 0)
        return why = "начало и длина — числа секунд не меньше 0", false;
    std::string call = "part(" + quote_string(part) + ")";
    call += p == Prop::Morph ? std::string(":morph(") : std::string(".") + prop_word(p) + "(";
    call += value_text(p, value) + ", " + format_number(length) + ")";
    return insert_clip_line(code, source, rec.clip, "at(" + format_number(t) + ", " + call + ")", out, why);
}

bool add_at_table(const Recording& rec, f64 t, std::string_view part, const std::vector<std::pair<Prop, f64>>& values, f64 length,
                  const CodeSites& code, std::string_view source, std::string& out, std::string& why) {
    out.clear();
    if (!std::isfinite(t) || t < 0 || !std::isfinite(length) || length < 0)
        return why = "начало и длина — числа секунд не меньше 0", false;
    if (values.empty()) return why = "нечего ставить", false;
    std::string table;
    for (const auto& [p, v] : values) {
        if (!prop_is_number(p) || !std::isfinite(v)) return why = "в таблице — только числа: x, y, rot, scale, sx, sy, opacity", false;
        table += (table.empty() ? "{ " : ", ") + std::string(prop_word(p)) + " = " + format_number(v);
    }
    const std::string call = "part(" + quote_string(part) + "):to(" + table + " }, " + format_number(length) + ")";
    return insert_clip_line(code, source, rec.clip, "at(" + format_number(t) + ", " + call + ")", out, why);
}

bool edit_part_move(const Recording& rec, std::string_view part, f64 t, f64 frame, f64 dx, f64 dy, const CodeSites& code,
                    std::string_view source, std::string& out, std::string& why) {
    out.clear();
    if (!std::isfinite(t) || t < 0 || !std::isfinite(dx) || !std::isfinite(dy)) return why = "сдвиг — числа", false;
    const auto named = std::find(rec.parts.begin(), rec.parts.end(), part);
    const bool known = named != rec.parts.end();
    const u32 pi = known ? static_cast<u32>(named - rec.parts.begin()) : 0;
    // x and y: their deciding blocks in this frame, whether their transitions go, the move of each.
    struct Axis {
        Prop prop;
        f64 d;
        i32 track = -1, block = -1;
        bool going = false;
    };
    Axis axes[2] = {{Prop::X, dx}, {Prop::Y, dy}};
    for (Axis& a : axes) {
        a.track = known ? rec.track(pi, a.prop) : -1;
        a.block = a.track < 0 ? -1 : deciding_block(rec, static_cast<u32>(a.track), t);
        a.going = a.block >= 0 && block_going(rec.blocks[static_cast<usize>(a.block)], t, frame);
    }
    if (dx == 0 && dy == 0) return why = "часть не сдвинута", false;
    // Each edit on the text the last one left, its calls found in it again (a number or a field changes no call).
    std::string text(source);
    CodeSites now;
    const CodeSites* sites = &code;
    auto apply = [&](const std::string& next) {
        text = next;
        std::string w;
        u32 line = 0;
        if (!now.parse(text, w, line)) return why = "правка не разбирается: " + w, false;
        sites = &now;
        return true;
    };
    auto current = [&](const Axis& a, f64& v) {
        PropValue pv = rest_value(a.prop);
        if (a.track >= 0 && !sample(rec, static_cast<u32>(a.track), t, nullptr, pv, why)) return false;
        v = pv.v;
        return true;
    };
    std::vector<std::pair<Prop, f64>> line;
    std::string next;
    for (const Axis& a : axes) {
        if (a.d == 0) continue;
        if (a.going) {
            const Block& b = rec.blocks[static_cast<usize>(a.block)];
            if (!value_call(rec, b, *sites, why)) return false;
            PropValue v = b.value;
            v.v += a.d;
            if (!edit_value(rec, b, a.prop, v, *sites, text, next, why) || !apply(next)) return false;
            continue;
        }
        f64 v = 0;
        if (!current(a, v)) return false;
        // The other one's transition goes in a call of a table without this field, and this track has not started since.
        const Axis& o = axes[a.prop == Prop::X ? 1 : 0];
        if (o.going) {
            const Block& ob = rec.blocks[static_cast<usize>(o.block)];
            std::string no;
            const CallSite* c = ob.value_field ? value_call(rec, ob, *sites, no) : nullptr;
            bool field = c != nullptr;
            if (c)
                for (const CodeField& f : c->args[static_cast<usize>(ob.value_arg)].fields) field = field && f.name != prop_word(a.prop);
            if (field && a.track >= 0)
                for (u32 i : rec.tracks[static_cast<usize>(a.track)].blocks)
                    field = field && !(rec.blocks[i].start >= ob.start && rec.blocks[i].start <= snap_time(t));
            if (field) {
                PropValue pv = rest_value(a.prop);
                pv.v = v + a.d;
                if (!edit_value(rec, ob, a.prop, pv, *sites, text, next, why) || !apply(next)) return false;
                continue;
            }
        }
        line.emplace_back(a.prop, v + a.d);
    }
    if (!line.empty()) {
        // At the frame's moment to thousandths, not after it: the frame shows the part there.
        const f64 at = std::floor(t * 1000 + 1e-6) / 1000;
        if (!add_at_table(rec, at, part, line, 0, *sites, text, next, why)) return false;
        text = next;
    }
    out = std::move(text);
    return true;
}

} // namespace forge::pixel
