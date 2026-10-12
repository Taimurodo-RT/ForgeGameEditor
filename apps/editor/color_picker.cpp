#include "color_picker.h"

#include <RmlUi/Core/Elements/ElementFormControl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace forge::editor_app {

namespace d = editor::design;

namespace {

std::string swatch(d::Color c) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "rgba(%d, %d, %d, %d)", c.r, c.g, c.b, c.a);
    return buf;
}

std::string hex_of(d::Color c) {
    std::string h = d::color_hex(c);
    for (char& ch : h) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return h.substr(1);
}

Rml::Element* element_in(Rml::Context* context, const char* id) {
    for (int i = 0; context && i < context->GetNumDocuments(); ++i)
        if (Rml::Element* e = context->GetDocument(i)->GetElementById(id)) return e;
    return nullptr;
}

} // namespace

void ColorPicker::bind(Rml::DataModelConstructor& model) {
    if (auto s = model.RegisterStruct<View>()) {
        s.RegisterMember("open", &View::open);
        s.RegisterMember("dropper", &View::dropper);
        s.RegisterMember("live", &View::live);
        s.RegisterMember("can_live", &View::can_live);
        s.RegisterMember("title", &View::title);
        s.RegisterMember("hex", &View::hex);
        s.RegisterMember("alpha", &View::alpha);
        s.RegisterMember("old_swatch", &View::old_swatch);
        s.RegisterMember("new_swatch", &View::new_swatch);
        s.RegisterMember("hue_swatch", &View::hue_swatch);
        s.RegisterMember("alpha_bar", &View::alpha_bar);
        s.RegisterMember("hex_note", &View::hex_note);
        s.RegisterMember("note", &View::note);
        s.RegisterMember("link", &View::link);
        s.RegisterMember("theme", &View::theme);
        s.RegisterMember("hint", &View::hint);
        s.RegisterMember("sv_x", &View::sv_x);
        s.RegisterMember("sv_y", &View::sv_y);
        s.RegisterMember("hue_x", &View::hue_x);
        s.RegisterMember("alpha_x", &View::alpha_x);
    }
    model.Bind("ue_cp", &view_);
    if (auto s = model.RegisterStruct<Chip>()) {
        s.RegisterMember("index", &Chip::index);
        s.RegisterMember("key", &Chip::key);
        s.RegisterMember("name", &Chip::name);
        s.RegisterMember("swatch", &Chip::swatch);
    }
    model.RegisterArray<std::vector<Chip>>();
    model.Bind("ue_cp_colors", &chips_);

    auto on = [&](const char* name, auto fn) {
        model.BindEventCallback(name, [fn](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& args) { fn(ev, args); });
    };
    auto arg_str = [](const Rml::VariantList& a, usize i) { return i < a.size() ? a[i].Get<Rml::String>() : Rml::String(); };
    auto input_value = [](Rml::Event& ev) -> std::string {
        if (auto* input = rmlui_dynamic_cast<Rml::ElementFormControl*>(ev.GetTargetElement())) return input->GetValue();
        return {};
    };
    on("ue_cp_outside", [this](Rml::Event& ev, const Rml::VariantList&) {
        ev.StopPropagation();
        if (!state_.open || state_.dropper) return; // the eyedropper's click is the canvas's to give
        close(true); // the cover under the picker has the click: the canvas under it gets nothing
    });
    on("ue_cp_close", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        close(!a.empty() && a[0].Get<bool>());
    });
    on("ue_cp_dropper", [this](Rml::Event& ev, const Rml::VariantList&) {
        ev.StopPropagation();
        dropper(!state_.dropper);
    });
    on("ue_cp_live", [this](Rml::Event& ev, const Rml::VariantList&) {
        ev.StopPropagation();
        live(!state_.live);
    });
    on("ue_cp_theme", [this, arg_str](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        theme(arg_str(a, 0));
    });
    // HEX and alpha: kept on Enter and on leaving the field; leaving it as it was shown changes nothing.
    // Enter on the shown colour of several that differ gives it to all of them.
    // (Taken, the field shows the colour as the picker writes it: "36c" becomes "3366CC".)
    on("ue_cp_hex", [this, arg_str](Rml::Event& ev, const Rml::VariantList& a) {
        if (state_.open && a.size() > 1 && a[1].Get<bool>() && (arg_str(a, 0) != view_.hex || state_.mixed) && hex(arg_str(a, 0)))
            if (auto* input = rmlui_dynamic_cast<Rml::ElementFormControl*>(ev.GetTargetElement())) input->SetValue(view_.hex);
    });
    on("ue_cp_hex_done", [this, input_value](Rml::Event& ev, const Rml::VariantList&) {
        const std::string value = input_value(ev);
        if (!state_.open || value == view_.hex) return;
        if (!hex(value))
            if (auto* input = rmlui_dynamic_cast<Rml::ElementFormControl*>(ev.GetTargetElement())) input->SetValue(view_.hex);
    });
    on("ue_cp_alpha", [this, arg_str](Rml::Event& ev, const Rml::VariantList& a) {
        if (state_.open && a.size() > 1 && a[1].Get<bool>() && (arg_str(a, 0) != view_.alpha || state_.mixed) && alpha(arg_str(a, 0)))
            if (auto* input = rmlui_dynamic_cast<Rml::ElementFormControl*>(ev.GetTargetElement())) input->SetValue(view_.alpha);
    });
    on("ue_cp_alpha_done", [this, input_value](Rml::Event& ev, const Rml::VariantList&) {
        const std::string value = input_value(ev);
        if (!state_.open || value == view_.alpha) return;
        if (!alpha(value))
            if (auto* input = rmlui_dynamic_cast<Rml::ElementFormControl*>(ev.GetTargetElement())) input->SetValue(view_.alpha);
    });
}

bool ColorPicker::open(Client& client, const Request& r, Rml::Context* context) {
    close(true);
    context_ = context;
    client_ = &client;
    state_ = State{};
    state_.field = r.field;
    state_.start = state_.color = r.start;
    state_.hsva = d::color_to_hsva(r.start);
    state_.start_link = state_.link = r.link;
    state_.linkable = r.linkable;
    state_.mixed = r.mixed;
    state_.can_live = r.can_live;
    state_.ax = r.x;
    state_.ay = r.y;
    state_.aw = r.w;
    state_.ah = r.h;
    state_.open = true;
    // Where it first stands, before its own size is known (place puts it right).
    state_.left = state_.top = -1;
    if (Rml::Element* p = element_in(context_, "ue-picker")) p->SetProperty("visibility", "hidden");
    view_.hex_note.clear();
    view_.hint.clear();
    refresh();
    return true;
}

void ColorPicker::close(bool keep) {
    if (!state_.open) return;
    Client* client = client_;
    const bool live = state_.live;
    state_.open = false;
    state_.dropper = false;
    state_.live = false;
    state_.drag = 0;
    view_.hex_note.clear();
    view_.hint.clear();
    if (client) client->picker_closed(keep, live);
    if (!state_.open) client_ = nullptr; // (the client may have opened it again)
    refresh();
}

void ColorPicker::set(d::Color c, const std::string& link) {
    if (!state_.open) return;
    state_.color = c;
    state_.link = link;
    state_.picked = true;
    state_.hsva = d::color_to_hsva(c, state_.hsva);
    view_.hex_note.clear();
    changed();
}

void ColorPicker::changed() {
    if (client_) client_->picker_show(state_.color, state_.link);
    refresh();
}

void ColorPicker::square(f32 saturation, f32 brightness) {
    if (!state_.open) return;
    state_.hsva.s = std::clamp(saturation, 0.0f, 1.0f);
    state_.hsva.v = std::clamp(brightness, 0.0f, 1.0f);
    state_.picked = true;
    state_.color = d::hsva_to_color(state_.hsva);
    state_.link.clear();
    state_.theme.clear();
    view_.hex_note.clear();
    changed();
}

void ColorPicker::hue(f32 at) {
    if (!state_.open) return;
    state_.hsva.h = std::clamp(at, 0.0f, 1.0f) * 360.0f;
    if (state_.hsva.h >= 360.0f) state_.hsva.h = 359.99f; // the bar's right end: red, as its left
    state_.picked = true;
    state_.color = d::hsva_to_color(state_.hsva);
    state_.link.clear();
    state_.theme.clear();
    view_.hex_note.clear();
    changed();
}

void ColorPicker::alpha_at(f32 at) {
    if (!state_.open) return;
    state_.hsva.a = std::clamp(at, 0.0f, 1.0f);
    state_.picked = true;
    state_.color = d::hsva_to_color(state_.hsva);
    state_.link.clear();
    state_.theme.clear();
    view_.hex_note.clear();
    changed();
}

bool ColorPicker::hex(const std::string& text) {
    if (!state_.open) return false;
    const std::optional<d::Color> c = d::parse_hex_input(text);
    if (!c) {
        // A part of a colour or a typo: nothing changes, the reason shows under the field.
        view_.hex_note = d::hex_problem(text);
        dirty("ue_cp");
        return false;
    }
    state_.theme.clear();
    set(*c, "");
    return true;
}

bool ColorPicker::alpha(const std::string& percent) {
    if (!state_.open) return false;
    const std::optional<u8> a = d::parse_alpha_input(percent);
    if (!a) {
        view_.hex_note = "Прозрачность: число от 0 до 100";
        dirty("ue_cp");
        return false;
    }
    d::Color c = state_.color;
    c.a = *a;
    state_.theme.clear();
    set(c, "");
    return true;
}

bool ColorPicker::theme(const std::string& key) {
    if (!state_.open || !game_colors) return false;
    for (const d::NamedColor& c : game_colors())
        if (c.key == key) {
            state_.theme = key;
            set(c.color, state_.linkable ? key : std::string());
            return true;
        }
    return false;
}

void ColorPicker::dropper(bool on) {
    if (!state_.open || state_.dropper == on) return;
    state_.dropper = on;
    state_.drag = 0;
    view_.hint = on && client_ ? client_->picker_dropper_hint(state_.live) : std::string();
    refresh();
}

bool ColorPicker::live(bool on) {
    if (!state_.open || state_.live == on || !client_ || (on && !state_.can_live)) return false;
    state_.live = on;
    if (!client_->picker_checking(on)) {
        state_.live = !on;
        return false;
    }
    dropper(on);
    refresh();
    return true;
}

void ColorPicker::take(d::Color c, const std::string& said) {
    if (!state_.open) return;
    state_.dropper = false;
    view_.hint = said;
    state_.theme.clear();
    set(c, "");
}

void ColorPicker::hint(const std::string& said) {
    view_.hint = said;
    dirty("ue_cp");
}

void ColorPicker::refresh() {
    view_.open = state_.open;
    view_.dropper = state_.dropper;
    view_.live = state_.live;
    view_.can_live = state_.open && state_.can_live;
    std::vector<Chip> chips;
    if (state_.open) {
        const d::Color c = state_.color;
        view_.title = client_ ? client_->picker_title() : std::string();
        view_.hex = hex_of(c);
        const f32 pct = std::round(c.a / 2.55f);
        view_.alpha = std::to_string(static_cast<int>(pct)) + "%";
        view_.old_swatch = swatch(state_.start);
        view_.new_swatch = swatch(c);
        view_.hue_swatch = swatch(d::hsva_to_color({state_.hsva.h, 1, 1, 1}));
        char bar[160];
        std::snprintf(bar, sizeof(bar), "linear-gradient(90deg, rgba(%d, %d, %d, 0) 0%%, rgba(%d, %d, %d, 255) 100%%)", c.r, c.g, c.b, c.r,
                      c.g, c.b);
        view_.alpha_bar = bar;
        view_.sv_x = state_.hsva.s * 100;
        view_.sv_y = (1 - state_.hsva.v) * 100;
        view_.hue_x = state_.hsva.h / 360 * 100;
        view_.alpha_x = state_.hsva.a * 100;
        view_.link = state_.link;
        view_.theme = state_.theme;
        view_.note = client_ ? client_->picker_note() : std::string();
        if (game_colors) {
            const std::vector<d::NamedColor> all = game_colors();
            for (usize i = 0; i < all.size(); ++i) chips.push_back({static_cast<int>(i), all[i].key, all[i].name, swatch(all[i].color)});
        }
    }
    auto same = [](const Chip& a, const Chip& b) { return a.index == b.index && a.key == b.key && a.name == b.name && a.swatch == b.swatch; };
    if (!std::equal(chips.begin(), chips.end(), chips_.begin(), chips_.end(), same)) {
        chips_ = std::move(chips);
        dirty("ue_cp_colors");
    }
    dirty("ue_cp");
}

// Beside the swatch it opened from, inside the window: left of the swatch, or right of it when there is no room; never
// past an edge (a small window scrolls the picker itself).
bool ColorPicker::place(Rml::Context* context) {
    Rml::Element* p = element_in(context, "ue-picker");
    if (!state_.open || !p || !context || p->GetComputedValues().display() == Rml::Style::Display::None) return false;
    const Rml::Vector2f size = p->GetBox().GetSize(Rml::BoxArea::Border);
    if (size.x <= 0 || size.y <= 0) return false; // not laid out yet
    const Rml::Vector2i window = context->GetDimensions();
    const f32 W = static_cast<f32>(window.x), H = static_cast<f32>(window.y), m = 8;
    f32 x = state_.ax - size.x - 12;
    if (x < m) x = state_.ax + state_.aw + 12;
    x = std::clamp(x, m, std::max(m, W - size.x - m));
    f32 y = std::clamp(state_.ay + state_.ah * 0.5f - 60, m, std::max(m, H - size.y - m));
    const f32 tall = std::max(120.0f, H - 2 * m);
    if (x != state_.left || y != state_.top || p->GetProperty<float>("max-height") != tall ||
        p->GetComputedValues().visibility() != Rml::Style::Visibility::Visible) {
        state_.left = x;
        state_.top = y;
        p->SetProperty("left", std::to_string(static_cast<int>(x)) + "px");
        p->SetProperty("top", std::to_string(static_cast<int>(y)) + "px");
        p->SetProperty("max-height", std::to_string(static_cast<int>(tall)) + "px");
        p->SetProperty("visibility", "visible"); // shown once it stands in its place
        return true;
    }
    return false;
}

void ColorPicker::drag_to(f32 mx, f32 my, Rml::Context* context) {
    const char* id = state_.drag == 1 ? "ue-cp-sv" : state_.drag == 2 ? "ue-cp-hue" : "ue-cp-alpha";
    Rml::Element* e = element_in(context, id);
    if (!e) return;
    const Rml::Vector2f at = e->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f size = e->GetBox().GetSize(Rml::BoxArea::Border);
    const f32 u = size.x > 0 ? std::clamp((mx - at.x) / size.x, 0.0f, 1.0f) : 0;
    const f32 v = size.y > 0 ? std::clamp((my - at.y) / size.y, 0.0f, 1.0f) : 0;
    if (state_.drag == 1) square(u, 1 - v);
    else if (state_.drag == 2) hue(u);
    else alpha_at(u);
}

bool ColorPicker::handle_event(const SDL_Event& e, f32 density, Rml::Context* context) {
    if (!state_.open) return false;
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
        // (Without a text field's focus the keys came by handle_key already.)
        if (e.key.key != SDLK_ESCAPE) return false;
        if (state_.dropper) dropper(false);
        else close(false);
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        mouse_x_ = e.button.x * density;
        mouse_y_ = e.button.y * density;
        if (state_.dropper) {
            if (e.button.button != SDL_BUTTON_LEFT || !client_ || !client_->picker_sample(mouse_x_, mouse_y_)) dropper(false);
            return true;
        }
        if (e.button.button != SDL_BUTTON_LEFT) return true;
        for (Rml::Element* el = context ? context->GetHoverElement() : nullptr; el; el = el->GetParentNode()) {
            const Rml::String& id = el->GetId();
            if (id == "ue-cp-sv") state_.drag = 1;
            else if (id == "ue-cp-hue") state_.drag = 2;
            else if (id == "ue-cp-alpha") state_.drag = 3;
            if (state_.drag || id == "ue-picker") break;
        }
        if (state_.drag) drag_to(mouse_x_, mouse_y_, context);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION:
        mouse_x_ = e.motion.x * density;
        mouse_y_ = e.motion.y * density;
        if (state_.drag) drag_to(mouse_x_, mouse_y_, context);
        return true;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        state_.drag = 0;
        return true;
    case SDL_EVENT_MOUSE_WHEEL: return true;
    default: return false;
    }
}

bool ColorPicker::handle_key(const SDL_KeyboardEvent& k) {
    if (!state_.open) return false;
    if (k.key == SDLK_ESCAPE) {
        if (state_.dropper) dropper(false);
        else close(false);
    } else if (k.key == SDLK_RETURN || k.key == SDLK_KP_ENTER) {
        close(true);
    }
    return true;
}

} // namespace forge::editor_app
