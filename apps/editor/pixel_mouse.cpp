// «Векторная», part 3б of step 3 of docs/models/движение-модель.md: the mouse writes into the code. The timeline's
// edges of blocks and pauses, the inspector's fields of the selected block, a part dragged in the vector, the selected
// part's values and «К коду», the tree's row under the mouse outlined (pixel_editor.h); part 4ж: the part's fields and
// the graph of a block's easing with the handles of its bezier(…). The edits themselves are forge/pixel/motion_edit.h;
// here they are taken from the mouse and written as steps of the history.

#include "pixel_editor.h"
#include "pixel_files.h"

#include "forge/pixel/motion_edit.h"

#include <RmlUi/Core/Elements/ElementFormControl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace forge::editor_app {

using namespace pixel_files;

namespace {

constexpr f64 kEdgeReach = 4; // how near a block's or a pause's edge a press takes it (logical px)
constexpr i64 kCurveW = 64, kCurveH = 40; // a timing's curve in the list, texels (shown at half)

// "body · y"
std::string block_name(const pixel::Recording& rec, const pixel::Block& b) {
    if (b.prop == pixel::Prop::Drawing) return pixel::prop_word(b.prop); // the character's track, no part's (part 4д)
    const std::string part = b.part < rec.parts.size() ? rec.parts[b.part] : std::string("?");
    // A scene's camera (part 6б): its scale is its zoom.
    if (part == pixel::kCameraPart) return std::string("камера · ") + (b.prop == pixel::Prop::Scale ? "zoom" : pixel::prop_word(b.prop));
    return part + " · " + pixel::prop_word(b.prop);
}

constexpr const char* kMidSwap = "посреди смены рисунка часть не тянется: выберите кадр до или после";

std::string hex_of(const pixel::Rgba8& c) { return pixel::color_hex(c); }

std::string thousandths(f64 v) { return number_words(std::round(v * 1000) / 1000); }

// A value as the inspector's field shows it and takes it: a number; #rrggbb, #rrggbbaa or nothing (the rest) of a
// colour; a number or nothing (its own width) of stroke_width; «да» or «нет» of visible; the shape's id or nothing
// (its own shape) of morph; the mask and the order as their marks («маска lid», «над body»), not to be typed.
std::string field_of(pixel::Prop p, const pixel::PropValue& v) {
    if (pixel::prop_is_number(p)) return thousandths(v.v);
    if (p == pixel::Prop::Visible) return v.v > 0 ? "да" : "нет";
    if (pixel::prop_is_call(p)) return pixel::value_words(p, v);
    if (!(v.v > 0)) return "";
    if (p == pixel::Prop::StrokeWidth) return thousandths(v.n);
    return pixel::prop_is_color(p) ? hex_of(v.color) : v.target;
}

bool read_number(const std::string& text, f64& out) {
    std::string t = text;
    while (!t.empty() && t.back() == ' ') t.pop_back();
    while (!t.empty() && t.front() == ' ') t.erase(t.begin());
    std::replace(t.begin(), t.end(), ',', '.');
    char* end = nullptr;
    out = std::strtod(t.c_str(), &end);
    return !t.empty() && end && *end == '\0' && std::isfinite(out);
}

std::string curve_image(usize i) { return "px_curve_" + std::to_string(i); }
constexpr const char* kBezierIcon = "px_curve_bezier"; // the list's «кривая» (part 4ж)

std::string trimmed_text(const std::string& text) {
    std::string t = text;
    while (!t.empty() && t.back() == ' ') t.pop_back();
    while (!t.empty() && t.front() == ' ') t.erase(t.begin());
    return t;
}

// A text typed into a field as property p's value (the fields of a block, 3б, and of a part, 4ж): false with why;
// `what` names it ("body · y").
bool value_of_text(pixel::Prop p, const std::string& text, const std::string& what, pixel::PropValue& v, std::string& why) {
    const std::string t = trimmed_text(text);
    if (pixel::prop_is_number(p)) {
        if (!read_number(t, v.v)) return why = "Значение «" + what + "» — число", false;
        if (p == pixel::Prop::Flash && (v.v < 0 || v.v > 1)) return why = "Вспышка «" + what + "» — доля от 0 до 1", false;
        if ((p == pixel::Prop::Blur || p == pixel::Prop::ShadowBlur) && v.v < 0) return why = "Размытие «" + what + "» — число не меньше 0", false;
    } else if (pixel::prop_is_color(p)) {
        const char* rest = p == pixel::Prop::Tint ? "без тона" : p == pixel::Prop::FlashColor ? "белый" : p == pixel::Prop::Shadow ? "без тени" : "свой цвет части";
        if (!t.empty() && !pixel::parse_part_color(t, v.color)) return why = "Цвет «" + what + "» — #rrggbb или #rrggbbaa, пусто — " + rest, false;
        v.v = t.empty() ? 0 : 1;
    } else if (p == pixel::Prop::StrokeWidth) {
        if (!t.empty() && (!read_number(t, v.n) || v.n < 0))
            return why = "Толщина «" + what + "» — число не меньше 0, пусто — своя толщина фигур", false;
        v.v = t.empty() ? 0 : 1;
    } else if (p == pixel::Prop::Visible) {
        if (t != "да" && t != "нет") return why = "Видимость «" + what + "» — «да» или «нет»", false;
        v.v = t == "да" ? 1 : 0;
    } else if (p == pixel::Prop::Order) {
        return why = "Порядок «" + what + "» правится в коде: to_top, to_bottom, above, below", false;
    } else if (p == pixel::Prop::Mask) {
        return why = "Маска «" + what + "» правится в коде: mask(\"lid\"), mask(nil)", false;
    } else if (p == pixel::Prop::Link) {
        return why = "Крепление «" + what + "» правится в коде: attach(\"hand\"), detach()", false;
    } else if (p == pixel::Prop::Drawing) {
        return why = "Рисунок правится в коде: swap(…)", false;
    } else {
        v.v = t.empty() ? 0 : 1;
        v.target = t;
    }
    return true;
}

// The part's fields (part 4ж) in order, with the heading over the first of each section.
struct FieldProp {
    pixel::Prop prop;
    const char* section;
};
constexpr FieldProp kFieldProps[] = {
    {pixel::Prop::X, "Трансформация"}, {pixel::Prop::Y, ""}, {pixel::Prop::Rot, ""}, {pixel::Prop::Scale, ""}, {pixel::Prop::Sx, ""},
    {pixel::Prop::Sy, ""}, {pixel::Prop::SkewX, ""}, {pixel::Prop::SkewY, ""}, {pixel::Prop::Opacity, ""},
    {pixel::Prop::Stroke, "Контур"}, {pixel::Prop::StrokeWidth, ""}, {pixel::Prop::Fill, "Заливка"},
};

// The camera's fields (part 6б): its place, its zoom (its scale) and its turn.
constexpr FieldProp kCameraFields[] = {{pixel::Prop::X, "Камера"}, {pixel::Prop::Y, ""}, {pixel::Prop::Scale, ""}, {pixel::Prop::Rot, ""}};
std::span<const FieldProp> fields_of(bool camera) { return camera ? std::span<const FieldProp>(kCameraFields) : std::span<const FieldProp>(kFieldProps); }
std::string field_word(pixel::Prop p, bool camera) { return camera && p == pixel::Prop::Scale ? "zoom" : pixel::prop_word(p); }

bool is_field(std::string_view word) {
    for (const FieldProp& f : kFieldProps)
        if (word == pixel::prop_word(f.prop)) return true;
    return false;
}

// A field's property as the frame shows it, written as the part's values write it (3б): «свой», «своя» its own.
std::string frame_text(pixel::Prop p, const pixel::PartPose& pp) {
    auto color = [](bool has, const pixel::Rgba8& c, f64 k) { return has ? hex_of(c) + (k < 1 ? " · " + thousandths(k) : std::string()) : std::string("свой"); };
    switch (p) {
    case pixel::Prop::X: return thousandths(pp.x);
    case pixel::Prop::Y: return thousandths(pp.y);
    case pixel::Prop::Rot: return thousandths(pp.rot);
    case pixel::Prop::Scale: return thousandths(pp.scale);
    case pixel::Prop::Sx: return thousandths(pp.sx);
    case pixel::Prop::Sy: return thousandths(pp.sy);
    case pixel::Prop::SkewX: return thousandths(pp.skew_x);
    case pixel::Prop::SkewY: return thousandths(pp.skew_y);
    case pixel::Prop::Opacity: return thousandths(pp.opacity);
    case pixel::Prop::Stroke: return color(pp.has_stroke, pp.stroke, pp.stroke_k);
    case pixel::Prop::StrokeWidth:
        return pp.has_stroke_width ? thousandths(pp.stroke_width) + (pp.stroke_width_k < 1 ? " · " + thousandths(pp.stroke_width_k) : std::string()) : "своя";
    case pixel::Prop::Fill: return color(pp.has_fill, pp.fill, pp.fill_k);
    default: return {};
    }
}

// The frame's value of a field's property, for a clip without a recording.
pixel::PropValue frame_value(pixel::Prop p, const pixel::PartPose& pp) {
    pixel::PropValue v;
    switch (p) {
    case pixel::Prop::X: v.v = pp.x; break;
    case pixel::Prop::Y: v.v = pp.y; break;
    case pixel::Prop::Rot: v.v = pp.rot; break;
    case pixel::Prop::Scale: v.v = pp.scale; break;
    case pixel::Prop::Sx: v.v = pp.sx; break;
    case pixel::Prop::Sy: v.v = pp.sy; break;
    case pixel::Prop::SkewX: v.v = pp.skew_x; break;
    case pixel::Prop::SkewY: v.v = pp.skew_y; break;
    case pixel::Prop::Opacity: v.v = pp.opacity; break;
    case pixel::Prop::Stroke: v.v = pp.has_stroke ? 1 : 0, v.color = pp.stroke; break;
    case pixel::Prop::StrokeWidth: v.v = pp.has_stroke_width ? 1 : 0, v.n = pp.stroke_width; break;
    case pixel::Prop::Fill: v.v = pp.has_fill ? 1 : 0, v.color = pp.fill; break;
    default: break;
    }
    return v;
}

// The graph of a block's easing under «как» (part 4ж), in logical px: x 0 … 1 and y kEaseLo … kEaseHi within a
// margin; a press within kEaseReach of a handle takes it.
constexpr f64 kEaseW = 200, kEaseH = 180, kEasePad = 10, kEaseLo = -0.5, kEaseHi = 1.5, kEaseReach = 8;
f64 ease_gx(f64 x) { return kEasePad + x * (kEaseW - 2 * kEasePad); }
f64 ease_gy(f64 y) { return kEasePad + (kEaseHi - y) / (kEaseHi - kEaseLo) * (kEaseH - 2 * kEasePad); }

// "0,3; 0; 0,2; 1": the numbers have commas of their own.
std::string bezier_words(const vector::Bezier& c) {
    return thousandths(c.x1) + "; " + thousandths(c.y1) + "; " + thousandths(c.x2) + "; " + thousandths(c.y2);
}

} // namespace

// --- the model -------------------------------------------------------------------------------------

void PixelEditor::bind_mouse(Rml::DataModelConstructor& model) {
    if (auto s = model.RegisterStruct<TimingRow>()) {
        s.RegisterMember("word", &TimingRow::word);
        s.RegisterMember("icon", &TimingRow::icon);
        s.RegisterMember("selected", &TimingRow::selected);
    }
    model.RegisterArray<std::vector<TimingRow>>();
    if (auto s = model.RegisterStruct<PartValueRow>()) {
        s.RegisterMember("name", &PartValueRow::name);
        s.RegisterMember("value", &PartValueRow::value);
    }
    model.RegisterArray<std::vector<PartValueRow>>();
    if (auto s = model.RegisterStruct<PartFieldRow>()) {
        s.RegisterMember("name", &PartFieldRow::name);
        s.RegisterMember("label", &PartFieldRow::label);
        s.RegisterMember("section", &PartFieldRow::section);
        s.RegisterMember("value", &PartFieldRow::value);
        s.RegisterMember("now", &PartFieldRow::now);
    }
    model.RegisterArray<std::vector<PartFieldRow>>();
    model.Bind("px_part_fields", &m_part_fields_);
    model.Bind("px_has_ease", &m_has_ease_);
    model.Bind("px_ease_image", &m_ease_image_);
    model.Bind("px_ease_share", &m_ease_share_);
    model.Bind("px_ease_why", &m_ease_why_);
    model.Bind("px_timings", &m_timings_);
    model.Bind("px_part_values", &m_part_values_);
    model.Bind("px_block_value", &m_block_value_);
    model.Bind("px_block_length", &m_block_length_);
    model.Bind("px_block_timing", &m_block_timing_);
    model.Bind("px_timing_icon", &m_timing_icon_);
    model.Bind("px_block_why", &m_block_why_);
    model.Bind("px_part_name", &m_part_name_);
    model.Bind("px_part_code", &m_part_code_);
    model.Bind("px_can_value", &m_can_value_);
    model.Bind("px_can_length", &m_can_length_);
    model.Bind("px_can_timing", &m_can_timing_);
    model.Bind("px_timings_open", &m_timings_open_);
    model.Bind("px_has_part", &m_has_part_);
    model.Bind("px_part_can_code", &m_part_can_code_);

    auto on = [&](const char* name, auto fn) {
        model.BindEventCallback(name, [fn](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& args) { fn(ev, args); });
    };
    auto arg_int = [](const Rml::VariantList& a, usize i) { return i < a.size() ? a[i].Get<int>(-1) : -1; };
    auto arg_str = [](const Rml::VariantList& a, usize i) { return i < a.size() ? a[i].Get<Rml::String>() : Rml::String(); };
    auto arg_enter = [](const Rml::VariantList& a, usize i) { return i < a.size() && a[i].Get<bool>(); };
    auto field = [](Rml::Event& ev) -> std::optional<std::string> {
        Rml::Element* e = ev.GetTargetElement();
        if (!e || e->GetTagName() != "input") return std::nullopt;
        return static_cast<Rml::ElementFormControl*>(e)->GetValue();
    };
    // A field left as it shows writes nothing (and saves no panel).
    auto value = [this](const std::string& text) {
        if (text != m_block_value_) set_block_value(text);
    };
    auto length = [this](const std::string& text) {
        if (text == m_block_length_) return;
        f64 v = 0;
        if (read_number(text, v)) set_block_length(v);
        else refuse_edit("Длина — число секунд не меньше 0");
    };
    on("px_block_value", [value, arg_str, arg_enter](Rml::Event&, const Rml::VariantList& a) {
        if (arg_enter(a, 1)) value(arg_str(a, 0));
    });
    on("px_block_value_commit", [value, field](Rml::Event& ev, const Rml::VariantList&) {
        if (auto text = field(ev)) value(*text);
    });
    on("px_block_length", [length, arg_str, arg_enter](Rml::Event&, const Rml::VariantList& a) {
        if (arg_enter(a, 1)) length(arg_str(a, 0));
    });
    on("px_block_length_commit", [length, field](Rml::Event& ev, const Rml::VariantList&) {
        if (auto text = field(ev)) length(*text);
    });
    on("px_timing_open", [this](Rml::Event&, const Rml::VariantList&) {
        timings_open_ = !timings_open_;
        rebuild_inspector();
    });
    on("px_timing", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
        const int i = arg_int(a, 0);
        if (i >= 0 && i < static_cast<int>(vector::kTimingCount)) set_block_timing(static_cast<vector::Timing>(i));
        else if (i == static_cast<int>(vector::kTimingCount)) set_block_bezier_timing(); // the row after the words
    });
    on("px_part_code", [this](Rml::Event&, const Rml::VariantList&) { part_to_code(); });
    // Part 4ж: a part's field left as it showed when it took the keyboard writes nothing.
    auto part_field = [this](const std::string& name, const std::string& text) {
        bool same = name == field_focus_ && text == field_shown_;
        if (!same && name != field_focus_)
            for (const PartField& f : part_fields_)
                same = same || (f.name == name && f.value == text);
        if (!same) return void(set_part_field(name, text));
        // Nothing written: the field shows the code's value again (the playhead may have moved while it had the keyboard).
        field_fresh_ = name;
        rebuild_part();
    };
    on("px_part_field", [part_field, arg_str, arg_enter](Rml::Event&, const Rml::VariantList& a) {
        if (arg_enter(a, 2)) part_field(arg_str(a, 0), arg_str(a, 1));
    });
    on("px_part_field_commit", [part_field, arg_str, field](Rml::Event& ev, const Rml::VariantList& a) {
        if (auto text = field(ev)) part_field(arg_str(a, 0), *text);
    });
}

// --- an edit begins, is refused, is written ------------------------------------------------------------------

bool PixelEditor::refuse_edit(std::string why) {
    note_ = refused_ = std::move(why);
    rebuild();
    return false;
}

bool PixelEditor::code_records() {
    if (!script_problem_.empty() || !rec_error_.empty() || rec_stale_) {
        const u32 line = !script_problem_.empty() ? error_line_ : rec_error_line_;
        return refuse_edit("Код с ошибкой: правка мышью не начнётся, пока он не записывается" +
                           (line > 0 ? " (строка " + std::to_string(line) + ", см. консоль)" : std::string(" (см. консоль)")));
    }
    if (!rec_ok_) return refuse_edit("У клипа одной pose(t) нет записи: его движение правится в коде");
    return true;
}

bool PixelEditor::begin_edit(bool& saved) {
    saved = false;
    if (!has_) return false;
    // The timeline shows the saved code, and the numbers' places are its: the panel first, a step of its own.
    if (code_unsaved()) saved = save_code();
    return code_records();
}

const pixel::Block* PixelEditor::edited_block() {
    if (!rec_ok_ || block_ < 0 || block_ >= static_cast<i32>(rec_.blocks.size())) return refuse_edit("Выберите блок на таймлайне"), nullptr;
    // Its track and its number on it: so it is found again in the recording of a panel saved first.
    const pixel::Block& was = rec_.blocks[static_cast<usize>(block_)];
    const std::string part = rec_.parts[was.part];
    const pixel::Prop prop = was.prop;
    usize nth = 0;
    for (i32 i = 0; i < block_; ++i) nth += rec_.parts[rec_.blocks[static_cast<usize>(i)].part] == part && rec_.blocks[static_cast<usize>(i)].prop == prop;
    bool saved = false;
    if (!begin_edit(saved)) return nullptr;
    if (saved) {
        i32 found = -1;
        usize n = 0;
        for (usize i = 0; i < rec_.blocks.size() && found < 0; ++i)
            if (rec_.parts[rec_.blocks[i].part] == part && rec_.blocks[i].prop == prop && n++ == nth) found = static_cast<i32>(i);
        if (found < 0)
            return refuse_edit("Код сохранён, а в новой записи нет блока «" + part + " · " + pixel::prop_word(prop) + "» с тем же номером: выберите блок снова"),
                   nullptr;
        select_block(found);
    }
    return &rec_.blocks[static_cast<usize>(block_)];
}

// --- the selected block: its fields ------------------------------------------------------------------------

bool PixelEditor::set_block_value(const std::string& text) {
    note_.clear();
    const pixel::Block* b = edited_block();
    if (!b) return false;
    const std::string what = block_name(rec_, *b);
    pixel::PropValue v;
    std::string out, why;
    if (!value_of_text(b->prop, text, what, v, why)) return refuse_edit(why);
    if (!pixel::edit_value(rec_, *b, b->prop, v, script().code(), script_text_, out, why)) return refuse_edit("Значение «" + what + "» не записать: " + why);
    if (out == script_text_) return rebuild(), false; // the same
    const std::string shown = field_of(b->prop, v);
    return write_script(out, "«" + name() + "»: " + what + " = " + (shown.empty() ? std::string("своё") : shown));
}

bool PixelEditor::set_block_length(f64 length) {
    note_.clear();
    const pixel::Block* b = edited_block();
    if (!b) return false;
    const std::string what = block_name(rec_, *b);
    std::string out, why;
    if (!pixel::edit_length(rec_, *b, length, script().code(), script_text_, out, why)) return refuse_edit("Длину «" + what + "» не записать: " + why);
    if (out == script_text_) return rebuild(), false;
    return write_script(out, "«" + name() + "»: " + what + ", длина " + thousandths(length) + " с");
}

bool PixelEditor::set_block_timing(vector::Timing timing) {
    note_.clear();
    timings_open_ = false;
    const pixel::Block* b = edited_block();
    if (!b) return false;
    const std::string what = block_name(rec_, *b);
    std::string out, why;
    if (!pixel::edit_timing(rec_, *b, timing, script().code(), script_text_, out, why)) return refuse_edit("Сглаживание «" + what + "» не записать: " + why);
    if (out == script_text_) return rebuild(), false;
    return write_script(out, "«" + name() + "»: " + what + ", " + vector::timing_word(timing));
}

bool PixelEditor::set_block_bezier_timing() {
    note_.clear();
    timings_open_ = false;
    const pixel::Block* b = edited_block();
    if (!b) return false;
    if (b->bezier >= 0) return rebuild(), false; // a curve already: its numbers stay
    const std::string what = block_name(rec_, *b);
    const vector::Bezier ease; // CSS's ease
    std::string out, why;
    if (!pixel::edit_timing_bezier(rec_, *b, ease, script().code(), script_text_, out, why))
        return refuse_edit("Сглаживание «" + what + "» не записать: " + why);
    if (out == script_text_) return rebuild(), false;
    return write_script(out, "«" + name() + "»: " + what + ", кривая " + bezier_words(ease));
}

bool PixelEditor::set_block_bezier(const vector::Bezier& curve) {
    note_.clear();
    const pixel::Block* b = edited_block();
    if (!b) return false;
    const std::string what = block_name(rec_, *b);
    std::string out, why;
    if (!pixel::edit_bezier(rec_, *b, curve, script().code(), script_text_, out, why)) return refuse_edit("Кривую «" + what + "» не записать: " + why);
    if (out == script_text_) return rebuild(), false;
    return write_script(out, "«" + name() + "»: " + what + ", кривая " + bezier_words(curve));
}

// --- the selected part: its fields (part 4ж) ------------------------------------------------------------

bool PixelEditor::set_part_field(const std::string& field, const std::string& text) {
    note_.clear();
    // Whatever comes of it, the field shows the code's value again, though it has the keyboard.
    struct Fresh {
        PixelEditor* ed;
        std::string name;
        ~Fresh() {
            ed->field_fresh_ = name;
            ed->rebuild_part();
        }
    } fresh{this, field};
    const bool camera = camera_chosen(); // part 6б: x, y, zoom and rot of the camera
    const FieldProp* f = nullptr;
    for (const FieldProp& fp : fields_of(camera))
        if (field == field_word(fp.prop, camera)) f = &fp;
    if (!f) return refuse_edit(camera ? "У камеры нет поля «" + field + "»: есть x, y, zoom и rot" : "У части нет поля «" + field + "»");
    if (!has_ || part_.empty() || (!camera && rig().holder(part_).part(part_) < 0)) return refuse_edit("Выберите часть");
    const std::string id = part_, what = (camera ? std::string("камера") : id) + " · " + field;
    pixel::PropValue v;
    std::string out, why;
    if (!value_of_text(f->prop, text, what, v, why)) return refuse_edit(why);
    if (camera && f->prop == pixel::Prop::Scale && !(v.v > 0)) return refuse_edit("zoom камеры — число больше 0: во сколько раз камера приближает");
    bool saved = false;
    if (!begin_edit(saved)) return false;
    if (rig().swapping()) return refuse_edit("посреди смены рисунка поля части не пишутся: выберите кадр до или после");
    play(false);
    const f64 frame = whole_.fps > 0 ? 1 / static_cast<f64>(whole_.fps) : 0.1;
    bool going = false;
    pixel::PropValue was;
    if (pixel::part_prop_value(rec_, id, t_, frame, f->prop, was, going) && field_of(f->prop, was) == field_of(f->prop, v)) return rebuild(), false; // the same
    if (!pixel::edit_part_prop(rec_, id, t_, frame, f->prop, v, script().code(), script_text_, out, why))
        return refuse_edit("«" + what + "» не записать: " + why);
    if (out == script_text_) return rebuild(), false;
    const std::string shown = field_of(f->prop, v);
    return write_script(out, "«" + name() + "»: " + what + " = " + (shown.empty() ? std::string("своё") : shown));
}

// --- the timeline's edges ---------------------------------------------------------------------------------

f64 PixelEditor::edge_snap(f64 t) const {
    const f64 fps = whole_.fps > 0 ? whole_.fps : 10;
    return pixel::snap_time(std::max(0.0, std::round(t * fps) / fps));
}

f64 PixelEditor::block_end(i32 index) const {
    const pixel::Block& b = rec_.blocks[static_cast<usize>(index)];
    if (b.to != pixel::Block::To::Binding && b.to != pixel::Block::To::Follow && !pixel::prop_is_call(b.prop)) return pixel::snap_time(b.start + b.length);
    // A binding holds until its track's next block, and so does a call of the order, the mask or the link (parts 4а,
    // 4б and 4в) and the camera's follow (part 6б).
    const i32 tr = rec_.track(b.part, b.prop);
    if (tr >= 0) {
        const std::vector<u32>& on = rec_.tracks[static_cast<usize>(tr)].blocks;
        const auto it = std::find(on.begin(), on.end(), static_cast<u32>(index));
        if (it != on.end() && it + 1 != on.end()) return rec_.blocks[*(it + 1)].start;
    }
    return rec_.length;
}

bool PixelEditor::edge_press(f32 x, f32 y, Rml::Element* over, bool again) {
    if (!rec_ok_ || !selected_clip() || tl_w_ <= 0) return false;
    std::string id;
    for (Rml::Element* e = over; e && id.empty(); e = e->GetParentNode()) {
        const std::string& eid = e->GetId();
        if (eid.rfind("px-block-", 0) == 0 || eid.rfind("px-pause-", 0) == 0 || eid.rfind("px-play-", 0) == 0) id = eid;
    }
    if (id.empty()) {
        if (again) refuse_edit("Код сохранён; под мышью в новой записи нет края: тяните снова");
        return again;
    }
    const f64 reach = kEdgeReach * std::max(1.0f, dp_), at = x - tl_x_;
    const bool actor_clip = id.rfind("px-play-", 0) == 0;
    const i32 index = std::atoi(id.c_str() + (actor_clip ? 8 : 9)); // after "px-play-", "px-block-" and "px-pause-"
    Drag what = Drag::None;
    std::string why, label;
    bool ok = false;
    f64 edge = 0;
    if (actor_clip) {
        // An actor's clip in a scene (part 6а): its start is dragged, as a block's; its length is the character's.
        if (index < 0 || index >= static_cast<i32>(rec_.plays.size())) return false;
        const pixel::ActorPlay& p = rec_.plays[static_cast<usize>(index)];
        if (std::fabs(at - tv_.x(p.start)) > reach) return false; // not at the start: a click on the clip
        what = Drag::PlayStart;
        ok = pixel::can_edit_play_start(rec_, p, script().code(), why);
        label = "Начало клипа «" + p.actor + "» мышью не изменить: ";
        edge = p.start;
    } else if (id.rfind("px-block-", 0) == 0) {
        if (index < 0 || index >= static_cast<i32>(rec_.blocks.size())) return false;
        const pixel::Block& b = rec_.blocks[static_cast<usize>(index)];
        // A follow lasts to the camera's next place (part 6б): no edge of its own, a click on it.
        if (b.to == pixel::Block::To::Follow) return false;
        const f64 d0 = std::fabs(at - tv_.x(b.start)), d1 = std::fabs(at - tv_.x(block_end(index)));
        const bool own_start = b.start_site.site >= 0;
        if (d1 <= reach && (d1 <= d0 || !own_start)) what = Drag::BlockEnd;
        else if (d0 <= reach && own_start) what = Drag::BlockStart;
        else return false; // not at an edge: a click on the block
        ok = pixel::can_edit(rec_, b, what == Drag::BlockEnd ? pixel::BlockEdit::Length : pixel::BlockEdit::Start, script().code(), why);
        label = (what == Drag::BlockEnd ? "Длину «" : "Начало «") + block_name(rec_, b) + "» мышью не изменить: ";
        edge = what == Drag::BlockEnd ? block_end(index) : b.start;
    } else {
        if (index < 0 || index >= static_cast<i32>(rec_.pauses.size())) return false;
        const pixel::Pause& p = rec_.pauses[static_cast<usize>(index)];
        edge = pixel::snap_time(p.start + p.length);
        if (std::fabs(at - tv_.x(edge)) > reach) return false;
        what = Drag::PauseEnd;
        ok = pixel::can_edit_pause(rec_, p, script().code(), why);
        label = "Длину паузы мышью не изменить: ";
    }
    // The code that does not record has no places of numbers: that is why, not the call's.
    if (!code_records()) return true;
    if (!ok) return refuse_edit(label + why), true;
    if (!again) {
        bool saved = false;
        if (!begin_edit(saved)) return true;
        if (saved) {
            // The new recording laid out: the edge under the mouse is found in it again.
            if (context_) context_->Update();
            update_timeline_box();
            return edge_press(x, y, context_ ? context_->GetElementAtPoint(Rml::Vector2f(x, y)) : nullptr, true);
        }
    }
    play(false);
    tl_drag_ = what;
    drag_block_ = what == Drag::PauseEnd ? -1 : index;
    drag_pause_ = what == Drag::PauseEnd ? static_cast<usize>(index) : 0;
    drag_edge_ = edge;
    return true;
}

bool PixelEditor::set_block_end(i32 index, f64 to) {
    note_.clear();
    bool saved = false;
    if (!begin_edit(saved)) return false;
    if (index < 0 || index >= static_cast<i32>(rec_.blocks.size())) return refuse_edit("На таймлайне нет такого блока");
    const pixel::Block& b = rec_.blocks[static_cast<usize>(index)];
    const std::string what = block_name(rec_, b);
    const f64 length = pixel::snap_time(to - b.start);
    std::string out, why;
    if (!pixel::edit_length(rec_, b, length, script().code(), script_text_, out, why)) return refuse_edit("Длину «" + what + "» не записать: " + why);
    if (out == script_text_) return rebuild(), false;
    return write_script(out, "«" + name() + "»: " + what + ", длина " + thousandths(length) + " с");
}

bool PixelEditor::set_block_start(i32 index, f64 to) {
    note_.clear();
    bool saved = false;
    if (!begin_edit(saved)) return false;
    if (index < 0 || index >= static_cast<i32>(rec_.blocks.size())) return refuse_edit("На таймлайне нет такого блока");
    const pixel::Block& b = rec_.blocks[static_cast<usize>(index)];
    const std::string what = block_name(rec_, b);
    std::string out, why;
    if (!pixel::edit_start(rec_, b, to, script().code(), script_text_, out, why)) return refuse_edit("Начало «" + what + "» не записать: " + why);
    if (out == script_text_) return rebuild(), false;
    return write_script(out, "«" + name() + "»: " + what + ", начало " + thousandths(to) + " с");
}

bool PixelEditor::set_pause_end(usize index, f64 to) {
    note_.clear();
    bool saved = false;
    if (!begin_edit(saved)) return false;
    if (index >= rec_.pauses.size()) return refuse_edit("На таймлайне нет такой паузы");
    const pixel::Pause& p = rec_.pauses[index];
    const f64 length = pixel::snap_time(to - p.start);
    std::string out, why;
    if (!pixel::edit_pause(rec_, p, length, script().code(), script_text_, out, why)) return refuse_edit("Длину паузы не записать: " + why);
    if (out == script_text_) return rebuild(), false;
    return write_script(out, "«" + name() + "»: пауза " + thousandths(length) + " с");
}

// --- a part dragged in the vector --------------------------------------------------------------------------

void PixelEditor::start_move() {
    bool saved = false;
    if (!begin_edit(saved)) {
        moving_ = false;
        return;
    }
    play(false);
    redraw(); // the rig in the frame shown: of the new recording when the panel was saved
    if (saved) {
        const std::string best = scene_mode_ && camera_edge(move_x_, move_y_) ? std::string(pixel::kCameraPart) : part_at(move_x_, move_y_);
        if (best.empty()) {
            moving_ = false;
            refuse_edit("Код сохранён; под мышью в новой записи нет части: тяните снова");
            return;
        }
        if (rig().shown().copy(best) >= 0) {
            moving_ = false;
            refuse_edit("копия «" + best + "» правится в коде: copies(…)");
            return;
        }
        select_part(best);
    }
    // A scene's camera (part 6б): its x and y are the stage's units.
    if (camera_chosen()) {
        move_inv_ = pixel::Affine{};
        move_from_ = view_to_picture(move_x_, move_y_);
        move_dx_ = move_dy_ = 0;
        move_started_ = true;
        return;
    }
    // Part 4д: in a swap the frame is two drawings, and the move would be the new one's only. An actor's part in a
    // scene (part 6а) is in its actor's drawing, carried by the actor's place on the stage.
    const ShownPart s = shown_part(part_);
    if (s.rig && s.swapping) {
        moving_ = false;
        refuse_edit(kMidSwap);
        return;
    }
    if (!s.rig) {
        moving_ = false;
        return;
    }
    move_inv_ = pixel::inverse(s.outer * s.rig->carried(s.index));
    move_from_ = view_to_picture(move_x_, move_y_);
    move_dx_ = move_dy_ = 0;
    move_started_ = true;
}

bool PixelEditor::move_part(f64 dx, f64 dy) {
    note_.clear();
    bool saved = false;
    if (!begin_edit(saved)) return false;
    const bool camera = camera_chosen(); // part 6б
    if (part_.empty() || (!camera && rig().holder(part_).part(part_) < 0)) return refuse_edit("Выберите часть");
    if (shown_part(part_).swapping) return refuse_edit(kMidSwap);
    const f64 frame = whole_.fps > 0 ? 1 / static_cast<f64>(whole_.fps) : 0.1;
    std::string out, why;
    if (!pixel::edit_part_move(rec_, part_, t_, frame, dx, dy, script().code(), script_text_, out, why))
        return refuse_edit((camera ? std::string("Камеру") : "«" + part_ + "»") + " не сдвинуть: " + why);
    if (out == script_text_) return rebuild(), false;
    return write_script(out, "«" + name() + "»: " + (camera ? std::string("камера сдвинута") : "«" + part_ + "» сдвинута") + " на " + number_words(dx) +
                                 "; " + number_words(dy));
}

// --- the inspector -----------------------------------------------------------------------------------------

void PixelEditor::make_curves() {
    if (curves_made_ || !ui_) return;
    curves_made_ = true;
    // 0 and 1 faint across, the curve over them; back and elastic go past both.
    const f64 lo = -0.45, hi = 1.45;
    auto row = [&](f64 v) { return std::clamp<i64>(std::lround((kCurveH - 1) - (v - lo) / (hi - lo) * (kCurveH - 1)), 0, kCurveH - 1); };
    const vector::Bezier ease;
    for (usize i = 0; i <= vector::kTimingCount; ++i) {
        // After the words, the list's «кривая»: CSS's ease with the lines to its two points (part 4ж).
        const bool bezier = i == vector::kTimingCount;
        auto curve = [&](f64 x) { return bezier ? vector::bezier(ease, x) : vector::timing(static_cast<vector::Timing>(i), x); };
        std::vector<u8> px(static_cast<usize>(kCurveW) * kCurveH * 4, 0);
        auto put = [&](i64 x, i64 y, const u8 (&c)[4]) {
            if (x < 0 || y < 0 || x >= kCurveW || y >= kCurveH) return;
            std::copy(c, c + 4, &px[(static_cast<usize>(y) * kCurveW + static_cast<usize>(x)) * 4]);
        };
        const u8 faint[4] = {128, 128, 128, 110}, line[4] = {40, 120, 255, 255}, handle[4] = {150, 150, 160, 255};
        for (i64 x = 0; x < kCurveW; ++x) put(x, row(0), faint), put(x, row(1), faint);
        if (bezier) {
            // (0, 0) to (x1, y1) and (1, 1) to (x2, y2), a dot at each end
            auto segment = [&](f64 x0, f64 y0, f64 x1, f64 y1) {
                for (int k = 0; k <= 48; ++k) {
                    const f64 u = k / 48.0;
                    put(std::lround((x0 + (x1 - x0) * u) * (kCurveW - 1)), row(y0 + (y1 - y0) * u), handle);
                }
                const i64 cx = std::lround(x1 * (kCurveW - 1)), cy = row(y1);
                for (i64 dy = -2; dy <= 2; ++dy)
                    for (i64 dx = -2; dx <= 2; ++dx)
                        if (dx * dx + dy * dy <= 5) put(cx + dx, cy + dy, line);
            };
            segment(0, 0, ease.x1, ease.y1);
            segment(1, 1, ease.x2, ease.y2);
        }
        i64 prev = -1;
        for (i64 x = 0; x < kCurveW; ++x) {
            const i64 y = row(curve(static_cast<f64>(x) / (kCurveW - 1)));
            const i64 a = prev < 0 ? y : std::min(prev, y), b = prev < 0 ? y : std::max(prev, y);
            for (i64 yy = a; yy <= b; ++yy) put(x, yy, line), put(x, yy + 1, line);
            prev = y;
        }
        ui_->set_image(bezier ? std::string(kBezierIcon) : curve_image(i), px.data(), static_cast<u32>(kCurveW), static_cast<u32>(kCurveH));
    }
}

void PixelEditor::rebuild_inspector() {
    make_curves();
    m_can_value_ = m_can_length_ = m_can_timing_ = false;
    m_block_value_ = m_block_length_ = m_block_timing_ = m_timing_icon_ = m_block_why_ = "";
    std::vector<TimingRow> rows;
    const pixel::Block* b = rec_ok_ && block_ >= 0 && block_ < static_cast<i32>(rec_.blocks.size()) ? &rec_.blocks[static_cast<usize>(block_)] : nullptr;
    if (b && rec_stale_) {
        m_block_why_ = "Код с ошибкой: блок правится, когда код снова записывается";
    } else if (b) {
        // Each reason once: a module's call refuses all three alike.
        std::vector<std::string> whys;
        std::string why;
        auto can = [&](pixel::BlockEdit what) {
            const bool ok = pixel::can_edit(rec_, *b, what, script().code(), why);
            if (!ok && std::find(whys.begin(), whys.end(), why) == whys.end()) whys.push_back(why);
            return ok;
        };
        m_can_value_ = can(pixel::BlockEdit::Value);
        m_can_length_ = can(pixel::BlockEdit::Length);
        m_can_timing_ = can(pixel::BlockEdit::Timing);
        for (const std::string& w : whys) m_block_why_ += (m_block_why_.empty() ? "" : "\n") + w;
        if (m_can_value_) m_block_value_ = field_of(b->prop, b->value);
        if (m_can_length_) m_block_length_ = thousandths(b->length);
        if (m_can_timing_) {
            // bezier(…) is «кривая» (part 4ж), an author's function «своя кривая».
            const bool own = b->curve >= 0, bezier = b->bezier >= 0;
            m_block_timing_ = bezier ? "кривая" : own ? "своя кривая" : vector::timing_word(b->timing);
            m_timing_icon_ = bezier ? "/memory/" + std::string(kBezierIcon) : own ? "" : "/memory/" + curve_image(static_cast<usize>(b->timing));
            if (timings_open_) {
                for (usize i = 0; i < vector::kTimingCount; ++i)
                    rows.push_back({vector::timing_word(static_cast<vector::Timing>(i)), "/memory/" + curve_image(i),
                                    !own && !bezier && static_cast<usize>(b->timing) == i});
                rows.push_back({"кривая", "/memory/" + std::string(kBezierIcon), bezier});
            }
        }
    }
    rebuild_ease(m_can_timing_ ? b : nullptr);
    if (!m_can_timing_) timings_open_ = false;
    m_timings_open_ = timings_open_;
    m_timings_ = std::move(rows);
    for (const char* var : {"px_timings", "px_block_value", "px_block_length", "px_block_timing", "px_timing_icon", "px_block_why", "px_can_value",
                            "px_can_length", "px_can_timing", "px_timings_open"})
        dirty(var);
}

void PixelEditor::rebuild_part() {
    std::vector<PartValue> values;
    i32 block = -1;
    std::string code;
    bool can_code = false;
    const bool camera = camera_chosen(); // part 6б: its fields alone
    const bool has = has_ && !part_.empty() && (camera || rig().holder(part_).part(part_) >= 0);
    pixel::PartPose pp;
    if (has) {
        if (shown_ok_)
            if (const auto it = shown_pose_.parts.find(part_); it != shown_pose_.parts.end()) pp = it->second;
        std::string morph = "нет";
        if (shown_ok_)
            if (const auto it = shown_pose_.morphs.find(part_); it != shown_pose_.morphs.end() && it->second.k > 0)
                morph = "«" + it->second.target + "» · " + thousandths(it->second.k);
        auto color = [&](bool has_it, pixel::Rgba8 c, f64 k) { return has_it ? hex_of(c) + (k < 1 ? " · " + thousandths(k) : std::string()) : "свой"; };
        // The last order call of the part up to the playhead (part 4а).
        std::string order = "как в рисунке";
        if (shown_ok_)
            for (const pixel::OrderStep& o : shown_pose_.order)
                if (o.part == part_) {
                    pixel::PropValue v;
                    v.v = static_cast<f64>(o.kind), v.target = o.other;
                    order = pixel::value_words(pixel::Prop::Order, v);
                }
        const auto at = rec_ok_ ? std::find(rec_.parts.begin(), rec_.parts.end(), part_) : rec_.parts.end();
        const i32 index = rec_ok_ && at != rec_.parts.end() ? static_cast<i32>(at - rec_.parts.begin()) : -1;
        for (usize i = 0; i < pixel::kPropCount && !camera; ++i) {
            const pixel::Prop p = static_cast<pixel::Prop>(i);
            std::string value;
            switch (p) {
            case pixel::Prop::X: value = thousandths(pp.x); break;
            case pixel::Prop::Y: value = thousandths(pp.y); break;
            case pixel::Prop::Rot: value = thousandths(pp.rot); break;
            case pixel::Prop::Scale: value = thousandths(pp.scale); break;
            case pixel::Prop::Sx: value = thousandths(pp.sx); break;
            case pixel::Prop::Sy: value = thousandths(pp.sy); break;
            case pixel::Prop::Opacity: value = thousandths(pp.opacity); break;
            case pixel::Prop::Fill: value = color(pp.has_fill, pp.fill, pp.fill_k); break;
            case pixel::Prop::Morph: value = morph; break;
            default: break;
            }
            // The properties of parts 4а, 4б and 4в show when the clip changes them.
            auto rest_color = [&](bool has_it, pixel::Rgba8 c, f64 k, const char* none) {
                return has_it ? hex_of(c) + (k < 1 ? " · " + thousandths(k) : std::string()) : std::string(none);
            };
            if (value.empty() && index >= 0 && rec_.track(static_cast<u32>(index), p) >= 0) switch (p) {
                case pixel::Prop::SkewX: value = thousandths(pp.skew_x); break;
                case pixel::Prop::SkewY: value = thousandths(pp.skew_y); break;
                case pixel::Prop::Stroke: value = color(pp.has_stroke, pp.stroke, pp.stroke_k); break;
                case pixel::Prop::StrokeWidth:
                    value = pp.has_stroke_width ? thousandths(pp.stroke_width) + (pp.stroke_width_k < 1 ? " · " + thousandths(pp.stroke_width_k) : std::string())
                                                : "своя";
                    break;
                case pixel::Prop::Dash: value = thousandths(pp.dash); break;
                case pixel::Prop::DrawFrom: value = thousandths(pp.draw_from); break;
                case pixel::Prop::DrawTo: value = thousandths(pp.draw_to); break;
                case pixel::Prop::Visible: value = pp.visible ? "да" : "нет"; break;
                case pixel::Prop::Tint: value = rest_color(pp.has_tint, pp.tint, pp.tint_k, "без тона"); break;
                case pixel::Prop::Flash: value = thousandths(pp.flash); break;
                case pixel::Prop::FlashColor: value = rest_color(pp.has_flash_color, pp.flash_color, pp.flash_color_k, "белый"); break;
                case pixel::Prop::Blur: value = thousandths(pp.blur); break;
                case pixel::Prop::Shadow: value = rest_color(pp.has_shadow, pp.shadow, pp.shadow_k, "без тени"); break;
                case pixel::Prop::ShadowX: value = thousandths(pp.shadow_x); break;
                case pixel::Prop::ShadowY: value = thousandths(pp.shadow_y); break;
                case pixel::Prop::ShadowBlur: value = thousandths(pp.shadow_blur); break;
                case pixel::Prop::Mask: value = pp.mask.empty() ? "нет" : pp.mask + (pp.mask_invert ? ", наоборот" : ""); break;
                case pixel::Prop::Link: value = pp.carry_by.empty() ? "нет" : pp.carry_by; break;
                case pixel::Prop::Order: value = order; break;
                default: break;
                }
            if (!value.empty()) values.push_back({pixel::prop_word(p), value});
        }
        // «К коду»: the block of its tracks that started last at the playhead or before.
        if (index >= 0) block = pixel::latest_block(rec_, static_cast<u32>(index), t_);
        if (block >= 0) {
            const pixel::Block& b = rec_.blocks[static_cast<usize>(block)];
            if (b.site.file > 0 && b.site.file < rec_.files.size()) {
                code = "из модуля «" + rec_.files[b.site.file] + "»";
            } else if (b.site.site >= 0 && b.site.site < static_cast<i32>(script().code().sites().size())) {
                const pixel::CallSite& site = script().code().sites()[static_cast<usize>(b.site.site)];
                const usize from = std::min<usize>(site.span.begin, script_text_.size()), to = std::min<usize>(site.span.end, script_text_.size());
                code = "строка " + std::to_string(site.line) + ": " + call_words(std::string_view(script_text_).substr(from, to - from));
                can_code = true;
            }
        }
    }
    part_block_ = block;
    const bool same = values.size() == part_values_.size() &&
                      std::equal(values.begin(), values.end(), part_values_.begin(), [](const PartValue& a, const PartValue& b) { return a.name == b.name && a.value == b.value; });
    if (!same) {
        part_values_ = std::move(values);
        m_part_values_.clear();
        // The properties of the fields are shown by the fields (part 4ж).
        for (const PartValue& v : part_values_)
            if (!is_field(v.name)) m_part_values_.push_back({v.name, v.value});
        dirty("px_part_values");
    }
    rebuild_fields(has, pp);
    if (has != m_has_part_ || part_ != m_part_name_ || code != m_part_code_ || can_code != m_part_can_code_) {
        m_has_part_ = has;
        m_part_name_ = part_;
        m_part_code_ = code;
        m_part_can_code_ = can_code;
        for (const char* var : {"px_has_part", "px_part_name", "px_part_code", "px_part_can_code"}) dirty(var);
    }
}

bool PixelEditor::part_to_code() {
    if (part_block_ < 0 || part_block_ >= static_cast<i32>(rec_.blocks.size())) return false;
    const pixel::Block& b = rec_.blocks[static_cast<usize>(part_block_)];
    if (b.site.file > 0) return refuse_edit("Блок «" + block_name(rec_, b) + "» из модуля «" + rec_.files[b.site.file] + "»: его код не в панели");
    if (code_ != script_panel_) return refuse_edit("Код в панели не тот, что записан: сохраните его (Ctrl+S), и часть покажет свой вызов");
    select_call(b);
    return true;
}

// --- the part's fields and the graph of the easing (part 4ж) --------------------------------------------------

void PixelEditor::rebuild_fields(bool has, const pixel::PartPose& pp) {
    std::vector<PartField> fields;
    if (has) {
        const f64 frame = whole_.fps > 0 ? 1 / static_cast<f64>(whole_.fps) : 0.1;
        const bool camera = camera_chosen(); // part 6б
        for (const FieldProp& f : fields_of(camera)) {
            PartField row;
            row.name = field_word(f.prop, camera);
            row.section = f.section;
            // What the clip's code gives it; without a recording or with a binding deciding, what the frame shows.
            pixel::PropValue v;
            if (!rec_ok_ || !pixel::part_prop_value(rec_, part_, t_, frame, f.prop, v, row.going)) v = frame_value(f.prop, pp);
            row.value = field_of(f.prop, v);
            const std::string now = frame_text(f.prop, pp);
            if (now != row.value && !(row.value.empty() && (now == "свой" || now == "своя"))) row.now = "сейчас " + now;
            fields.push_back(std::move(row));
        }
    }
    // The field with the keyboard keeps its text while the playhead and the recording change (unless written).
    std::string focused;
    Rml::Element* focus = context_ ? context_->GetFocusElement() : nullptr;
    if (has && focus && focus->GetId().rfind("px-pf-", 0) == 0 && focus->GetTagName() == "input") focused = focus->GetId().substr(6);
    if (focused != field_focus_) {
        field_focus_ = focused;
        field_shown_.clear();
        for (const PartFieldRow& r : m_part_fields_)
            if (r.name == focused) field_shown_ = r.value; // the text it showed when it took the keyboard
    }
    const std::string fresh = std::exchange(field_fresh_, std::string());
    std::vector<PartFieldRow> rows;
    for (const PartField& f : fields) {
        PartFieldRow r{f.name, f.going ? f.name + " →" : f.name, f.section, f.value, f.now};
        if (f.name == focused) {
            if (f.name == fresh) field_shown_ = f.value;
            else r.value = static_cast<Rml::ElementFormControl*>(focus)->GetValue();
        }
        rows.push_back(std::move(r));
    }
    part_fields_ = std::move(fields);
    const bool same = rows.size() == m_part_fields_.size() &&
                      std::equal(rows.begin(), rows.end(), m_part_fields_.begin(), [](const PartFieldRow& a, const PartFieldRow& b) {
                          return a.name == b.name && a.label == b.label && a.section == b.section && a.value == b.value && a.now == b.now;
                      });
    if (!same || !fresh.empty()) {
        m_part_fields_ = std::move(rows);
        dirty("px_part_fields");
    }
}

void PixelEditor::rebuild_ease(const pixel::Block* b) {
    const i32 index = b ? static_cast<i32>(b - rec_.blocks.data()) : -1;
    bool has = false, can = false;
    std::string why, share;
    if (b && b->bezier >= 0 && static_cast<usize>(b->bezier) < rec_.beziers.size()) {
        has = true;
        can = pixel::can_edit_bezier(rec_, *b, script().code(), why);
        const usize n = pixel::bezier_users(rec_, *b);
        if (n > 1) share = "кривая общая: " + count_words(static_cast<u32>(n), "блок", "блока", "блоков");
    }
    // The block or its curve gone from under the hand: let go of it.
    if (ease_held_ && (index != ease_block_ || !has || !can)) ease_held_ = 0;
    if (!ease_held_) ease_live_ = has ? rec_.beziers[static_cast<usize>(b->bezier)].curve : vector::Bezier{};
    ease_has_ = has, ease_can_ = can, ease_block_ = index;
    if (m_has_ease_ != (b != nullptr) || m_ease_why_ != why || m_ease_share_ != share) {
        m_has_ease_ = b != nullptr;
        m_ease_why_ = why;
        m_ease_share_ = share;
        for (const char* var : {"px_has_ease", "px_ease_why", "px_ease_share"}) dirty(var);
    }
    ease_dirty_ = true;
}

void PixelEditor::draw_ease() {
    ease_dirty_ = false;
    if (!ui_) return;
    const pixel::Block* b = m_has_ease_ && ease_block_ >= 0 && ease_block_ < static_cast<i32>(rec_.blocks.size()) ? &rec_.blocks[static_cast<usize>(ease_block_)] : nullptr;
    if (!b) {
        ease_key_.clear();
        return;
    }
    const f64 s = std::max(1.0f, dp_);
    const i64 w = std::lround(kEaseW * s), h = std::lround(kEaseH * s);
    const bool handles = ease_has_ && ease_can_;
    // The curve as points of the texture: of bezier(…) by its parameter, else y of x.
    std::vector<f64> line;
    const i64 n = 4 * w;
    for (i64 k = 0; k <= n; ++k) {
        const f64 u = static_cast<f64>(k) / static_cast<f64>(n);
        f64 x = u, y = 0;
        if (ease_has_) {
            const vector::Bezier& c = ease_live_;
            x = 3 * (1 - u) * (1 - u) * u * c.x1 + 3 * (1 - u) * u * u * c.x2 + u * u * u;
            y = 3 * (1 - u) * (1 - u) * u * c.y1 + 3 * (1 - u) * u * u * c.y2 + u * u * u;
        } else {
            y = pixel::block_ease(rec_, *b, u);
        }
        line.push_back(ease_gx(x) * s), line.push_back(ease_gy(y) * s);
    }
    std::string key(reinterpret_cast<const char*>(line.data()), line.size() * sizeof(f64));
    key += std::to_string(w) + " " + std::to_string(h) + " " + std::to_string(handles) + " " + std::to_string(ease_held_) + " " + bezier_words(ease_live_);
    if (key == ease_key_) return;
    ease_key_ = std::move(key);

    std::vector<u8> px(static_cast<usize>(w * h) * 4, 0);
    auto put = [&](i64 x, i64 y, const u8 (&c)[4]) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        std::copy(c, c + 4, &px[(static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x)) * 4]);
    };
    auto disc = [&](f64 cx, f64 cy, f64 r, const u8 (&c)[4]) {
        for (i64 y = static_cast<i64>(std::floor(cy - r)); y <= static_cast<i64>(std::ceil(cy + r)); ++y)
            for (i64 x = static_cast<i64>(std::floor(cx - r)); x <= static_cast<i64>(std::ceil(cx + r)); ++x)
                if (std::hypot(static_cast<f64>(x) + 0.5 - cx, static_cast<f64>(y) + 0.5 - cy) <= r) put(x, y, c);
    };
    auto segment = [&](f64 x0, f64 y0, f64 x1, f64 y1, f64 r, const u8 (&c)[4]) {
        const i64 steps = std::max<i64>(1, static_cast<i64>(std::ceil(std::hypot(x1 - x0, y1 - y0) * 2)));
        for (i64 k = 0; k <= steps; ++k) {
            const f64 u = static_cast<f64>(k) / static_cast<f64>(steps);
            disc(x0 + (x1 - x0) * u, y0 + (y1 - y0) * u, r, c);
        }
    };
    const u8 square[4] = {128, 128, 128, 28}, faint[4] = {128, 128, 128, 110}, curve[4] = {40, 120, 255, 255}, arm[4] = {150, 150, 160, 255},
             white[4] = {255, 255, 255, 255};
    // The square 0 … 1, faint lines at y = 0 and y = 1.
    const i64 x0 = std::lround(ease_gx(0) * s), x1 = std::lround(ease_gx(1) * s), y1 = std::lround(ease_gy(1) * s), y0 = std::lround(ease_gy(0) * s);
    for (i64 y = y1; y <= y0; ++y)
        for (i64 x = x0; x <= x1; ++x) put(x, y, square);
    const i64 thin = std::max<i64>(1, std::lround(s));
    for (i64 x = 0; x < w; ++x)
        for (i64 t = 0; t < thin; ++t) put(x, y0 + t, faint), put(x, y1 + t, faint);
    if (handles) {
        const vector::Bezier& c = ease_live_;
        segment(ease_gx(0) * s, ease_gy(0) * s, ease_gx(c.x1) * s, ease_gy(c.y1) * s, 0.75 * s, arm);
        segment(ease_gx(1) * s, ease_gy(1) * s, ease_gx(c.x2) * s, ease_gy(c.y2) * s, 0.75 * s, arm);
    }
    for (usize k = 2; k < line.size(); k += 2) segment(line[k - 2], line[k - 1], line[k], line[k + 1], 1.1 * s, curve);
    if (handles) {
        // The points: hollow, the one held full.
        const vector::Bezier& c = ease_live_;
        const f64 pts[2][2] = {{c.x1, c.y1}, {c.x2, c.y2}};
        for (int k = 0; k < 2; ++k) {
            const f64 cx = ease_gx(pts[k][0]) * s, cy = ease_gy(pts[k][1]) * s;
            disc(cx, cy, 4.5 * s, curve);
            if (ease_held_ != k + 1) disc(cx, cy, 2.5 * s, white);
        }
    }
    if (!ease_images_[1].empty()) ui_->drop_image(ease_images_[1]);
    ease_images_[1] = ease_images_[0];
    ease_images_[0] = "px_ease_" + std::to_string(++image_serial_);
    ui_->set_image(ease_images_[0], px.data(), static_cast<u32>(w), static_cast<u32>(h));
    m_ease_image_ = "/memory/" + ease_images_[0];
    dirty("px_ease_image");
    ease_px_ = std::move(px);
    ease_w_ = static_cast<u32>(w), ease_h_ = static_cast<u32>(h);
}

bool PixelEditor::ease_texel(f64 x, f64 y, i64& tx, i64& ty) const {
    if (ease_w_ == 0) return false;
    const f64 s = static_cast<f64>(ease_w_) / kEaseW;
    tx = static_cast<i64>(std::floor(ease_gx(x) * s)), ty = static_cast<i64>(std::floor(ease_gy(y) * s));
    return true;
}

bool PixelEditor::ease_box(f32& x, f32& y, f32& w, f32& h) const {
    Rml::Element* graph = element("px-ease-graph");
    if (!graph || !graph->IsVisible(true)) return false;
    const Rml::Vector2f at = graph->GetAbsoluteOffset(Rml::BoxArea::Content), size = graph->GetBox().GetSize(Rml::BoxArea::Content);
    x = at.x, y = at.y, w = size.x, h = size.y;
    return w > 0 && h > 0;
}

bool PixelEditor::ease_screen(f64 x, f64 y, f32& sx, f32& sy) const {
    f32 bx = 0, by = 0, bw = 0, bh = 0;
    if (!m_has_ease_ || !ease_box(bx, by, bw, bh)) return false;
    sx = bx + static_cast<f32>(ease_gx(x) / kEaseW * bw);
    sy = by + static_cast<f32>(ease_gy(y) / kEaseH * bh);
    return true;
}

bool PixelEditor::ease_handle(int handle, f32& x, f32& y) const {
    if (!ease_has_ || !ease_can_ || (handle != 1 && handle != 2)) return false;
    return ease_screen(handle == 1 ? ease_live_.x1 : ease_live_.x2, handle == 1 ? ease_live_.y1 : ease_live_.y2, x, y);
}

bool PixelEditor::ease_press(f32 x, f32 y) {
    f32 bx = 0, by = 0, bw = 0, bh = 0;
    // The handle within reach, the nearer of two.
    auto nearest = [&]() {
        if (!ease_box(bx, by, bw, bh)) return 0;
        const f32 reach = static_cast<f32>(kEaseReach / kEaseW) * bw;
        int best = 0;
        f32 bd = 0;
        for (int k = 1; k <= 2; ++k) {
            f32 hx = 0, hy = 0;
            if (!ease_handle(k, hx, hy)) continue;
            const f32 d = std::hypot(x - hx, y - hy);
            if (d <= reach && (best == 0 || d < bd)) best = k, bd = d;
        }
        return best;
    };
    if (nearest() == 0) return false; // not at a point: a click on the graph
    // The panel saved first, a step of its own; the block found again in its recording.
    if (!edited_block()) return true;
    rebuild_inspector();
    const int k = nearest();
    if (k == 0) return refuse_edit("Код сохранён; у кривой в новой записи нет точки под мышью: тяните снова"), true;
    play(false);
    f32 hx = 0, hy = 0;
    ease_handle(k, hx, hy);
    ease_held_ = k;
    ease_from_ = ease_live_;
    ease_grab_x_ = x - hx, ease_grab_y_ = y - hy;
    ease_dirty_ = true;
    return true;
}

void PixelEditor::ease_drag(f32 x, f32 y) {
    f32 bx = 0, by = 0, bw = 0, bh = 0;
    if (!ease_held_ || !ease_box(bx, by, bw, bh)) return;
    const f64 gx = (x - ease_grab_x_ - bx) / bw * kEaseW, gy = (y - ease_grab_y_ - by) / bh * kEaseH;
    // x within 0 … 1, y within the graph's, both to hundredths.
    const f64 hx = std::round(std::clamp((gx - kEasePad) / (kEaseW - 2 * kEasePad), 0.0, 1.0) * 100) / 100;
    const f64 hy = std::round(std::clamp(kEaseHi - (gy - kEasePad) / (kEaseH - 2 * kEasePad) * (kEaseHi - kEaseLo), kEaseLo, kEaseHi) * 100) / 100;
    (ease_held_ == 1 ? ease_live_.x1 : ease_live_.x2) = hx;
    (ease_held_ == 1 ? ease_live_.y1 : ease_live_.y2) = hy;
    ease_dirty_ = true;
}

void PixelEditor::ease_release() {
    if (!ease_held_) return;
    ease_held_ = 0;
    const vector::Bezier to = ease_live_;
    ease_dirty_ = true; // the curve the code's again once written or refused: both rebuild
    if (to != ease_from_) set_block_bezier(to);
}

void PixelEditor::ease_cancel() {
    if (!ease_held_) return;
    ease_held_ = 0;
    ease_live_ = ease_from_;
    ease_dirty_ = true;
}

// --- the tree's row under the mouse ------------------------------------------------------------------------

void PixelEditor::update_hover(Rml::Context* context) {
    std::string id;
    for (const Rml::Element* e = context ? context->GetHoverElement() : nullptr; e && id.empty(); e = e->GetParentNode())
        if (e->IsClassSet("px-part") && e->GetId().rfind("px-part-", 0) == 0) id = e->GetId().substr(8);
    if (!id.empty() && !has_part(id)) id.clear(); // an actor's part in a scene too (part 6а)
    if (id == hovered_) return;
    hovered_ = id;
    view_dirty_ = true;
}

} // namespace forge::editor_app
