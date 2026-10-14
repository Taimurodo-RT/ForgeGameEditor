#include "forge/pixel/motion_edit.h"

#include <cmath>
#include <cstdio>

namespace forge::pixel {

namespace {

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
    if (!(v.v > 0)) return "nil"; // its own colour or shape
    if (p == Prop::Morph) return quote_string(v.target);
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", v.color.r, v.color.g, v.color.b);
    return quote_string(buf);
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

bool has_value(const Block& b, std::string& why) {
    switch (b.to) {
    case Block::To::Value: break;
    case Block::To::Back: return why = "блок возвращает туда, откуда начался переход: его значения в коде нет", false;
    case Block::To::Saved: return why = "блок возвращает сохранённое save(): его значения в коде нет", false;
    case Block::To::Rest: return why = "блок возвращает в покой: его значения в коде нет", false;
    case Block::To::Binding: return why = "значение блока считает функция связи: правьте её в коде", false;
    }
    if (!b.frames.empty()) return why = "значения tween(…) считает его функция: правьте её в коде", false;
    return true;
}

} // namespace

bool edit_value(const Recording& rec, const Block& b, Prop p, const PropValue& value, const CodeSites& code,
                std::string_view source, std::string& out, std::string& why) {
    out.clear();
    const CallSite* c = script_call(rec, b.site, code, why);
    if (!c || !has_value(b, why)) return false;
    if (b.value_arg < 0 || static_cast<usize>(b.value_arg) >= c->args.size()) return why = "в вызове нет значения", false;
    const CodeArg& arg = c->args[static_cast<usize>(b.value_arg)];
    if (b.value_field) {
        if (arg.kind != ArgKind::Table) return why = "значения вызова не в таблице { … }: правьте в коде", false;
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
    const CallSite* c = script_call(rec, b.site, code, why);
    if (!c) return false;
    if (b.to == Block::To::Binding) return why = "у связи нет длины: она держится до следующего блока", false;
    if (b.spring) return why = "длину пружины задают её масса, жёсткость и затухание", false;
    return number_arg(source, *c, b.length_arg, b.length, length, out, why);
}

bool edit_timing(const Recording& rec, const Block& b, vector::Timing timing, const CodeSites& code,
                 std::string_view source, std::string& out, std::string& why) {
    out.clear();
    const CallSite* c = script_call(rec, b.site, code, why);
    if (!c) return false;
    if (b.to == Block::To::Binding) return why = "у связи нет сглаживания", false;
    if (b.spring) return why = "у пружины нет сглаживания: её ход задают масса, жёсткость и затухание", false;
    if (b.timing_arg < 0) return why = "в вызове нет сглаживания", false;
    const auto i = static_cast<usize>(b.timing_arg);
    const std::string word = quote_string(vector::timing_word(timing));
    if (i <= c->args.size()) return rewrite_arg(source, *c, i, word, out, why);
    // body.y(2) → body.y(2, 0, "out"): its length goes first.
    if (i == c->args.size() + 1 && b.length_arg == static_cast<i8>(c->args.size()))
        return rewrite_arg(source, *c, c->args.size(), format_number(b.length) + ", " + word, out, why);
    return why = "в вызове не хватает аргументов до сглаживания", false;
}

bool edit_start(const Recording& rec, const Block& b, f64 start, const CodeSites& code, std::string_view source,
                std::string& out, std::string& why) {
    out.clear();
    if (!std::isfinite(start)) return why = "начало — число секунд", false;
    if (b.start_site.site < 0)
        return why = "блок идёт за предыдущим, своего начала в коде у него нет: оно есть у at(с, …) и delay(с, …)",
               false;
    const CallSite* c = script_call(rec, b.start_site, code, why);
    if (!c) return false;
    if (c->args.empty()) return why = "в вызове нет начала", false;
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
    const CallSite* c = script_call(rec, pause.site, code, why);
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

} // namespace forge::pixel
