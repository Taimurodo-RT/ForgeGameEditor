// «Векторная», part 3б of step 3 of docs/models/движение-модель.md: the mouse writes into the code. The timeline's
// edges of blocks and pauses, the inspector's fields of the selected block, a part dragged in the vector, the selected
// part's values and «К коду», the tree's row under the mouse outlined (pixel_editor.h). The edits themselves are
// forge/pixel/motion_edit.h; here they are taken from the mouse and written as steps of the history.

#include "pixel_editor.h"
#include "pixel_files.h"

#include "forge/pixel/motion_edit.h"

#include <RmlUi/Core/Elements/ElementFormControl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace forge::editor_app {

using namespace pixel_files;

namespace {

constexpr f64 kEdgeReach = 4; // how near a block's or a pause's edge a press takes it (logical px)
constexpr i64 kCurveW = 64, kCurveH = 40; // a timing's curve in the list, texels (shown at half)

// "body · y"
std::string block_name(const pixel::Recording& rec, const pixel::Block& b) {
    return (b.part < rec.parts.size() ? rec.parts[b.part] : std::string("?")) + " · " + pixel::prop_word(b.prop);
}

std::string hex_of(const pixel::Rgba8& c) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c.r, c.g, c.b);
    return buf;
}

std::string thousandths(f64 v) { return number_words(std::round(v * 1000) / 1000); }

// A value as the inspector's field shows it and takes it: a number; #rrggbb or nothing (its own colour) of fill and
// stroke; a number or nothing (its own width) of stroke_width; «да» or «нет» of visible; the shape's id or nothing
// (its own shape) of morph; the order as its mark («над body»), not to be typed.
std::string field_of(pixel::Prop p, const pixel::PropValue& v) {
    if (pixel::prop_is_number(p)) return thousandths(v.v);
    if (p == pixel::Prop::Visible) return v.v > 0 ? "да" : "нет";
    if (p == pixel::Prop::Order) return pixel::value_words(p, v);
    if (!(v.v > 0)) return "";
    if (p == pixel::Prop::StrokeWidth) return thousandths(v.n);
    return p == pixel::Prop::Fill || p == pixel::Prop::Stroke ? hex_of(v.color) : v.target;
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
    });
    on("px_part_code", [this](Rml::Event&, const Rml::VariantList&) { part_to_code(); });
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
    std::string t = text;
    while (!t.empty() && t.back() == ' ') t.pop_back();
    while (!t.empty() && t.front() == ' ') t.erase(t.begin());
    if (pixel::prop_is_number(b->prop)) {
        if (!read_number(t, v.v)) return refuse_edit("Значение «" + what + "» — число");
    } else if (b->prop == pixel::Prop::Fill || b->prop == pixel::Prop::Stroke) {
        if (!t.empty() && !pixel::parse_color(t, v.color)) return refuse_edit("Цвет «" + what + "» — #rrggbb, пусто — свой цвет части");
        v.v = t.empty() ? 0 : 1;
    } else if (b->prop == pixel::Prop::StrokeWidth) {
        if (!t.empty() && (!read_number(t, v.n) || v.n < 0))
            return refuse_edit("Толщина «" + what + "» — число не меньше 0, пусто — своя толщина фигур");
        v.v = t.empty() ? 0 : 1;
    } else if (b->prop == pixel::Prop::Visible) {
        if (t != "да" && t != "нет") return refuse_edit("Видимость «" + what + "» — «да» или «нет»");
        v.v = t == "да" ? 1 : 0;
    } else if (b->prop == pixel::Prop::Order) {
        return refuse_edit("Порядок «" + what + "» правится в коде: to_top, to_bottom, above, below");
    } else {
        v.v = t.empty() ? 0 : 1;
        v.target = t;
    }
    std::string out, why;
    if (!pixel::edit_value(rec_, *b, b->prop, v, script_.code(), script_text_, out, why)) return refuse_edit("Значение «" + what + "» не записать: " + why);
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
    if (!pixel::edit_length(rec_, *b, length, script_.code(), script_text_, out, why)) return refuse_edit("Длину «" + what + "» не записать: " + why);
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
    if (!pixel::edit_timing(rec_, *b, timing, script_.code(), script_text_, out, why)) return refuse_edit("Сглаживание «" + what + "» не записать: " + why);
    if (out == script_text_) return rebuild(), false;
    return write_script(out, "«" + name() + "»: " + what + ", " + vector::timing_word(timing));
}

// --- the timeline's edges ---------------------------------------------------------------------------------

f64 PixelEditor::edge_snap(f64 t) const {
    const f64 fps = whole_.fps > 0 ? whole_.fps : 10;
    return pixel::snap_time(std::max(0.0, std::round(t * fps) / fps));
}

f64 PixelEditor::block_end(i32 index) const {
    const pixel::Block& b = rec_.blocks[static_cast<usize>(index)];
    if (b.to != pixel::Block::To::Binding && b.prop != pixel::Prop::Order) return pixel::snap_time(b.start + b.length);
    // A binding holds until its track's next block, and so does a call of the order (part 4а).
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
        if (eid.rfind("px-block-", 0) == 0 || eid.rfind("px-pause-", 0) == 0) id = eid;
    }
    if (id.empty()) {
        if (again) refuse_edit("Код сохранён; под мышью в новой записи нет края: тяните снова");
        return again;
    }
    const f64 reach = kEdgeReach * std::max(1.0f, dp_), at = x - tl_x_;
    const i32 index = std::atoi(id.c_str() + 9); // after "px-block-" and "px-pause-"
    Drag what = Drag::None;
    std::string why, label;
    bool ok = false;
    f64 edge = 0;
    if (id.rfind("px-block-", 0) == 0) {
        if (index < 0 || index >= static_cast<i32>(rec_.blocks.size())) return false;
        const pixel::Block& b = rec_.blocks[static_cast<usize>(index)];
        const f64 d0 = std::fabs(at - tv_.x(b.start)), d1 = std::fabs(at - tv_.x(block_end(index)));
        const bool own_start = b.start_site.site >= 0;
        if (d1 <= reach && (d1 <= d0 || !own_start)) what = Drag::BlockEnd;
        else if (d0 <= reach && own_start) what = Drag::BlockStart;
        else return false; // not at an edge: a click on the block
        ok = pixel::can_edit(rec_, b, what == Drag::BlockEnd ? pixel::BlockEdit::Length : pixel::BlockEdit::Start, script_.code(), why);
        label = (what == Drag::BlockEnd ? "Длину «" : "Начало «") + block_name(rec_, b) + "» мышью не изменить: ";
        edge = what == Drag::BlockEnd ? block_end(index) : b.start;
    } else {
        if (index < 0 || index >= static_cast<i32>(rec_.pauses.size())) return false;
        const pixel::Pause& p = rec_.pauses[static_cast<usize>(index)];
        edge = pixel::snap_time(p.start + p.length);
        if (std::fabs(at - tv_.x(edge)) > reach) return false;
        what = Drag::PauseEnd;
        ok = pixel::can_edit_pause(rec_, p, script_.code(), why);
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
    if (!pixel::edit_length(rec_, b, length, script_.code(), script_text_, out, why)) return refuse_edit("Длину «" + what + "» не записать: " + why);
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
    if (!pixel::edit_start(rec_, b, to, script_.code(), script_text_, out, why)) return refuse_edit("Начало «" + what + "» не записать: " + why);
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
    if (!pixel::edit_pause(rec_, p, length, script_.code(), script_text_, out, why)) return refuse_edit("Длину паузы не записать: " + why);
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
        const std::string best = part_at(move_x_, move_y_);
        if (best.empty()) {
            moving_ = false;
            refuse_edit("Код сохранён; под мышью в новой записи нет части: тяните снова");
            return;
        }
        select_part(best);
    }
    const i32 i = rig_.part(part_);
    if (i < 0) {
        moving_ = false;
        return;
    }
    move_inv_ = pixel::inverse(rig_.carried(i));
    move_from_ = view_to_picture(move_x_, move_y_);
    move_dx_ = move_dy_ = 0;
    move_started_ = true;
}

bool PixelEditor::move_part(f64 dx, f64 dy) {
    note_.clear();
    bool saved = false;
    if (!begin_edit(saved)) return false;
    if (part_.empty() || rig_.part(part_) < 0) return refuse_edit("Выберите часть");
    const f64 frame = whole_.fps > 0 ? 1 / static_cast<f64>(whole_.fps) : 0.1;
    std::string out, why;
    if (!pixel::edit_part_move(rec_, part_, t_, frame, dx, dy, script_.code(), script_text_, out, why))
        return refuse_edit("«" + part_ + "» не сдвинуть: " + why);
    if (out == script_text_) return rebuild(), false;
    return write_script(out, "«" + name() + "»: «" + part_ + "» сдвинута на " + number_words(dx) + "; " + number_words(dy));
}

// --- the inspector -----------------------------------------------------------------------------------------

void PixelEditor::make_curves() {
    if (curves_made_ || !ui_) return;
    curves_made_ = true;
    // 0 and 1 faint across, the curve over them; back and elastic go past both.
    const f64 lo = -0.45, hi = 1.45;
    auto row = [&](f64 v) { return std::clamp<i64>(std::lround((kCurveH - 1) - (v - lo) / (hi - lo) * (kCurveH - 1)), 0, kCurveH - 1); };
    for (usize i = 0; i < vector::kTimingCount; ++i) {
        std::vector<u8> px(static_cast<usize>(kCurveW) * kCurveH * 4, 0);
        auto put = [&](i64 x, i64 y, const u8 (&c)[4]) {
            if (x < 0 || y < 0 || x >= kCurveW || y >= kCurveH) return;
            std::copy(c, c + 4, &px[(static_cast<usize>(y) * kCurveW + static_cast<usize>(x)) * 4]);
        };
        const u8 faint[4] = {128, 128, 128, 110}, line[4] = {40, 120, 255, 255};
        for (i64 x = 0; x < kCurveW; ++x) put(x, row(0), faint), put(x, row(1), faint);
        i64 prev = -1;
        for (i64 x = 0; x < kCurveW; ++x) {
            const i64 y = row(vector::timing(static_cast<vector::Timing>(i), static_cast<f64>(x) / (kCurveW - 1)));
            const i64 a = prev < 0 ? y : std::min(prev, y), b = prev < 0 ? y : std::max(prev, y);
            for (i64 yy = a; yy <= b; ++yy) put(x, yy, line), put(x, yy + 1, line);
            prev = y;
        }
        ui_->set_image(curve_image(i), px.data(), static_cast<u32>(kCurveW), static_cast<u32>(kCurveH));
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
            const bool ok = pixel::can_edit(rec_, *b, what, script_.code(), why);
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
            const bool own = b->curve >= 0;
            m_block_timing_ = own ? "своя кривая" : vector::timing_word(b->timing);
            m_timing_icon_ = own ? "" : "/memory/" + curve_image(static_cast<usize>(b->timing));
            if (timings_open_)
                for (usize i = 0; i < vector::kTimingCount; ++i)
                    rows.push_back({vector::timing_word(static_cast<vector::Timing>(i)), "/memory/" + curve_image(i),
                                    !own && static_cast<usize>(b->timing) == i});
        }
    }
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
    const bool has = has_ && !part_.empty() && rig_.part(part_) >= 0;
    if (has) {
        pixel::PartPose pp;
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
        for (usize i = 0; i < pixel::kPropCount; ++i) {
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
            // The properties of part 4а show when the clip changes them.
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
            } else if (b.site.site >= 0 && b.site.site < static_cast<i32>(script_.code().sites().size())) {
                const pixel::CallSite& site = script_.code().sites()[static_cast<usize>(b.site.site)];
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
        for (const PartValue& v : part_values_) m_part_values_.push_back({v.name, v.value});
        dirty("px_part_values");
    }
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

// --- the tree's row under the mouse ------------------------------------------------------------------------

void PixelEditor::update_hover(Rml::Context* context) {
    std::string id;
    for (const Rml::Element* e = context ? context->GetHoverElement() : nullptr; e && id.empty(); e = e->GetParentNode())
        if (e->IsClassSet("px-part") && e->GetId().rfind("px-part-", 0) == 0) id = e->GetId().substr(8);
    if (!id.empty() && rig_.part(id) < 0) id.clear();
    if (id == hovered_) return;
    hovered_ = id;
    view_dirty_ = true;
}

} // namespace forge::editor_app
