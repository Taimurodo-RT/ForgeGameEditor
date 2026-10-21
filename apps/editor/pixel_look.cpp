// «Векторная», part 7а «Раскладка» of step 7 of docs/models/движение-модель.md: the three views, the panels' tabs,
// «Экспорт», «Пиксели» folded, the bottom panel's height and the code's share dragged, the vector's frame measured and
// the vector drawn to fill it (pixel_editor.h). None of it is written into files or the history.

#include "pixel_editor.h"
#include "pixel_files.h"

#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace forge::editor_app {

using namespace pixel_files;

namespace {

constexpr f64 kBottomLeast = 120;                  // the bottom panel's height at least (px of the interface)
constexpr f64 kUpperLeast = 160;                   // … and the height of the tab it leaves over it at least
constexpr f64 kShareLeast = 0.3, kShareMost = 0.7; // the code's share of the middle
// The bottom panel's share of the height under the bar when its height is not dragged: «Дизайн» and «Код», «Анимация».
constexpr f64 kBottomShare = 34, kBottomShareAnimation = 58;

const char* look_word(PixelEditor::Look l) {
    switch (l) {
    case PixelEditor::Look::Design: return "design";
    case PixelEditor::Look::Code: return "code";
    case PixelEditor::Look::Animation: return "animation";
    }
    return "code";
}

usize look_index(PixelEditor::Look l) { return static_cast<usize>(l); }

// Where an element's box is on the screen (the context's pixels); false when it is not shown.
bool box_of(Rml::Element* e, Rml::BoxArea area, f32& x, f32& y, f32& w, f32& h) {
    if (!e || !e->IsVisible(true)) return false;
    const Rml::Vector2f at = e->GetAbsoluteOffset(area), size = e->GetBox().GetSize(area);
    x = at.x, y = at.y, w = size.x, h = size.y;
    return true;
}

} // namespace

void PixelEditor::bind_look(Rml::DataModelConstructor& model) {
    model.Bind("px_look", &m_look_);
    model.Bind("px_left", &m_left_);
    model.Bind("px_side", &m_side_);
    model.Bind("px_bottom", &m_bottom_);
    model.Bind("px_export_open", &m_export_open_);
    model.Bind("px_pixels_folded", &m_pixels_folded_);
    model.Bind("px_tl_style", &m_tl_style_);
    model.Bind("px_code_style", &m_code_style_);
    model.Bind("px_vector_style", &m_vector_style_);
    model.Bind("px_pixel_style", &m_pixel_style_);
    model.Bind("px_one_style", &m_one_style_);
    model.Bind("px_problems", &m_problems_); // rows of the console's kind (bind_motion registers them)
    auto on = [&](const char* name, auto fn) {
        model.BindEventCallback(name, [fn](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) { fn(args); });
    };
    auto arg_str = [](const Rml::VariantList& a, usize i) { return i < a.size() ? a[i].Get<Rml::String>() : Rml::String(); };
    auto arg_int = [](const Rml::VariantList& a, usize i) { return i < a.size() ? a[i].Get<int>(0) : 0; };
    on("px_look", [this, arg_str](const Rml::VariantList& a) {
        const std::string w = arg_str(a, 0);
        for (Look l : {Look::Design, Look::Code, Look::Animation})
            if (w == look_word(l)) set_look(l);
    });
    on("px_left", [this, arg_str](const Rml::VariantList& a) { set_left(arg_str(a, 0)); });
    on("px_side", [this, arg_str](const Rml::VariantList& a) { set_side(arg_str(a, 0)); });
    on("px_bottom", [this, arg_str](const Rml::VariantList& a) { set_bottom(arg_str(a, 0)); });
    on("px_export_toggle", [this](const Rml::VariantList&) { set_export(!export_open_); });
    on("px_pixels_fold", [this](const Rml::VariantList&) { fold_pixels(!pixels_folded()); });
    on("px_problem", [this, arg_int](const Rml::VariantList& a) {
        if (const int i = arg_int(a, 0); i >= 0) problem_to_code(static_cast<usize>(i));
    });
    on("px_fit", [this](const Rml::VariantList&) { fit_view(); });
    on("px_zoom_by", [this, arg_int](const Rml::VariantList& a) { zoom_view_by(arg_int(a, 0) < 0 ? -1 : 1); });
    look_styles();
}

// --- the views and the panels' tabs ------------------------------------------------------------------------

void PixelEditor::set_look(Look l) {
    if (l == look_) return;
    look_ = l;
    m_look_ = look_word(l);
    dirty("px_look");
    // The code panel hidden does not keep the keyboard: the tab's keys are the tab's again.
    if (l != Look::Code && context_ && context_->GetFocusElement() && context_->GetFocusElement()->GetId() == "px-code-text")
        context_->GetFocusElement()->Blur();
    set_side(l == Look::Animation ? "anim" : "props");
    m_pixels_folded_ = folded_[look_index(l)];
    dirty("px_pixels_folded");
    look_styles();
    // The vector's frame is another size now: measured at the next update, the view resized keeping its middle.
}

void PixelEditor::set_left(const std::string& tab) {
    if ((tab != "layers" && tab != "files") || tab == left_) return;
    left_ = m_left_ = tab;
    dirty("px_left");
}

void PixelEditor::set_side(const std::string& tab) {
    if ((tab != "props" && tab != "anim") || tab == side_) return;
    side_ = m_side_ = tab;
    dirty("px_side");
}

void PixelEditor::set_bottom(const std::string& tab) {
    if ((tab != "timeline" && tab != "problems" && tab != "console") || tab == bottom_) return;
    bottom_ = m_bottom_ = tab;
    dirty("px_bottom");
}

void PixelEditor::set_export(bool open) {
    if (open == export_open_) return;
    export_open_ = m_export_open_ = open;
    dirty("px_export_open");
}

void PixelEditor::fold_pixels(bool folded) {
    folded_[look_index(look_)] = m_pixels_folded_ = folded;
    dirty("px_pixels_folded");
}

void PixelEditor::look_styles() {
    char buf[64];
    // A height dragged in a taller window is kept, shown no taller than this one leaves (the height − 160).
    f64 h = tl_height_[look_index(look_)];
    if (h > 0 && tab_h_ > 0) h = std::min(h, std::max(kBottomLeast, tab_h_ - kUpperLeast));
    // Each property said every time: RmlUi's style attribute keeps what it had until another value replaces it.
    if (h > 0) std::snprintf(buf, sizeof buf, "flex: 0 0 %.2fpx;", h);
    else std::snprintf(buf, sizeof buf, "flex: %g 1 0px;", look_ == Look::Animation ? kBottomShareAnimation : kBottomShare);
    if (m_tl_style_ != buf) {
        m_tl_style_ = buf;
        dirty("px_tl_style");
    }
    // The code's share once dragged; before, the view's own (48 %, 50 % in a narrow window).
    std::string code;
    if (code_share_ > 0) {
        std::snprintf(buf, sizeof buf, "flex: 0 0 %.3f%%;", code_share_ * 100);
        code = buf;
    }
    if (m_code_style_ != code) {
        m_code_style_ = code;
        dirty("px_code_style");
    }
}

bool PixelEditor::look_key(const SDL_KeyboardEvent& k) {
    if (!(k.mod & SDL_KMOD_CTRL) || (k.mod & (SDL_KMOD_ALT | SDL_KMOD_SHIFT))) return false;
    switch (k.key) {
    case SDLK_1: set_look(Look::Design); return true;
    case SDLK_2: set_look(Look::Code); return true;
    case SDLK_3: set_look(Look::Animation); return true;
    default: return false;
    }
}

// --- «Проблемы» ------------------------------------------------------------------------------------------------

void PixelEditor::rebuild_problems() {
    problems_.clear();
    for (const ConsoleLine& l : console_)
        if (l.error || l.warn) problems_.push_back(l);
    m_problems_.clear();
    for (const ConsoleLine& l : problems_) m_problems_.push_back({l.text, static_cast<int>(l.line), l.error, l.warn});
    dirty("px_problems");
}

bool PixelEditor::problem_to_code(usize index) {
    if (index >= problems_.size()) return false;
    set_look(Look::Code);
    return problems_[index].line > 0 && goto_line(problems_[index].line);
}

// --- the frames measured ----------------------------------------------------------------------------------------

void PixelEditor::measure_views(Rml::Context* context) {
    context_ = context;
    f32 x = 0, y = 0, w = 0, h = 0;
    // The height under the bar, which a dragged bottom panel's height is kept within.
    if (box_of(element("px-main"), Rml::BoxArea::Content, x, y, w, h) && std::fabs(h / dp_ - tab_h_) > 0.25) {
        tab_h_ = h / dp_;
        look_styles();
    }
    if (box_of(element("px-vector"), Rml::BoxArea::Content, x, y, w, h) && w >= 1 && h >= 1) {
        const u32 tw = static_cast<u32>(std::lround(w)), th = static_cast<u32>(std::lround(h));
        if (tw != view_tw_ || th != view_th_ || std::fabs(view_pw_ * dp_ - tw) > 1e-3 || std::fabs(view_ph_ * dp_ - th) > 1e-3)
            resize_view(tw / static_cast<f64>(dp_), th / static_cast<f64>(dp_), tw, th);
        view_x_ = x, view_y_ = y, view_w_ = static_cast<f32>(tw), view_h_ = static_cast<f32>(th);
    } else {
        view_w_ = view_h_ = 0;
        // Not shown: its points kept, its texels at the scale of the window now.
        const u32 tw = std::max(1u, static_cast<u32>(std::lround(view_pw_ * dp_))), th = std::max(1u, static_cast<u32>(std::lround(view_ph_ * dp_)));
        if (tw != view_texels_w() || th != view_texels_h()) resize_view(tw / static_cast<f64>(dp_), th / static_cast<f64>(dp_), tw, th);
    }
    // «Пиксели»: its column's room under its head, and its frame's size when shown; the big frame and «Через камеру»
    // are made to fit it.
    f32 hx = 0, hy = 0, hw = 0, hh = 0;
    if (box_of(element("px-pixel-col"), Rml::BoxArea::Content, x, y, w, h) && box_of(element("px-pixel-head"), Rml::BoxArea::Border, hx, hy, hw, hh) &&
        !pixels_folded()) {
        const f32 room_w = std::floor(w), room_h = std::floor(std::max(0.0f, h - hh - kPixelGap * dp_));
        if (room_w != pixel_room_w_ || room_h != pixel_room_h_) {
            pixel_room_w_ = room_w, pixel_room_h_ = room_h;
            view_dirty_ = true;
        }
    }
    if (box_of(element("px-pixel"), Rml::BoxArea::Content, x, y, w, h) && w >= 1 && h >= 1 &&
        (std::fabs(w - pixel_box_w_) >= 1 || std::fabs(h - pixel_box_h_) >= 1)) {
        pixel_box_w_ = std::floor(w), pixel_box_h_ = std::floor(h);
        view_dirty_ = true;
    }
}

void PixelEditor::resize_view(f64 pw, f64 ph, u32 tw, u32 th) {
    // The point of the picture in the view's middle stays there; the zoom's step stays.
    const bool keep = has_ && rig().loaded() && zoom_.width > 0 && zoom_.height > 0;
    vector::Point mid{};
    if (keep) mid = pixel::inverse(to_view()).apply({view_pw_ / 2, view_ph_ / 2});
    view_pw_ = pw, view_ph_ = ph, view_tw_ = tw, view_th_ = th;
    fit_box();
    if (keep) {
        const vector::Point now = to_view().apply(mid);
        zoom_.pan(pw / 2 - now.x, ph / 2 - now.y);
    }
    view_dirty_ = true;
}

// --- the grips ------------------------------------------------------------------------------------------------

bool PixelEditor::grip_press(Rml::Context* context, f32 x, f32 y) {
    context_ = context;
    bool bottom = false, code = false;
    for (const Rml::Element* el = context ? context->GetHoverElement() : nullptr; el; el = el->GetParentNode()) {
        bottom = bottom || el->GetId() == "px-tl-grip";
        code = code || el->GetId() == "px-code-split";
    }
    f32 bx = 0, by = 0, bw = 0, bh = 0, cx = 0, cy = 0, cw = 0, ch = 0;
    if (bottom && box_of(element("px-tl-panel"), Rml::BoxArea::Border, bx, by, bw, bh) &&
        box_of(element("px-body"), Rml::BoxArea::Content, cx, cy, cw, ch)) {
        f32 ax = 0, ay = 0, aw = 0, ah = 0;
        // The height of the tab under its bar: the panels' row.
        if (!box_of(element("px-main"), Rml::BoxArea::Content, ax, ay, aw, ah)) return false;
        grip_ = 1;
        grip_at_ = y, grip_from_ = bh, grip_whole_ = ah;
        play(false);
        return true;
    }
    if (code && look_ == Look::Code && box_of(element("px-code"), Rml::BoxArea::Border, bx, by, bw, bh) &&
        box_of(element("px-center-in"), Rml::BoxArea::Content, cx, cy, cw, ch)) {
        f32 sx = 0, sy = 0, sw = 0, sh = 0;
        // The code over the vector (a narrow window): the grip moves up and down.
        grip_across_ = box_of(element("px-stage"), Rml::BoxArea::Border, sx, sy, sw, sh) && sy >= by + bh - 0.5f;
        grip_ = 2;
        grip_at_ = grip_across_ ? y : x;
        grip_from_ = grip_across_ ? bh : bw;
        grip_whole_ = grip_across_ ? ch : cw;
        return true;
    }
    return false;
}

void PixelEditor::grip_drag(f32 x, f32 y) {
    const f64 dp = std::max(1e-3f, dp_);
    if (grip_ == 1) {
        // Up makes the bottom panel taller: 120 at least, the tab's height − 160 at most.
        const f64 most = std::max(kBottomLeast, grip_whole_ / dp - kUpperLeast);
        tl_height_[look_index(look_)] = std::clamp((grip_from_ + grip_at_ - y) / dp, kBottomLeast, most);
    } else if (grip_ == 2 && grip_whole_ > 0) {
        const f32 moved = (grip_across_ ? y : x) - grip_at_;
        code_share_ = std::clamp((grip_from_ + moved) / static_cast<f64>(grip_whole_), kShareLeast, kShareMost);
    }
    look_styles();
}

void PixelEditor::grip_release() { grip_ = 0; }

} // namespace forge::editor_app
