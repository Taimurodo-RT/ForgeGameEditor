#include "scheme_view.h"

#include "logic_editor.h"

#include "forge/core/log.h"
#include "forge/script/api.h"

#include <RmlUi/Core/Elements/ElementFormControl.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <string>
#include <tuple>

namespace forge::editor_app {

namespace {

// Sizes on screen (px): they match logic.rcss.
constexpr f32 kGrid = 16; // nodes snap to it
constexpr f32 kNodeMinW = 160, kNodeMaxW = 384, kHead = 30, kRowH = 24, kFoot = 8;
constexpr f32 kPickW = 150, kNumW = 52, kTextW = 110; // fields in a row
constexpr f32 kFramePad = 32, kFrameHead = 36, kFrameGap = 48;
// Auto layout: between steps (room for wires), between a value and what it
// goes into, a value's width in its column, between lanes.
constexpr f32 kGapX = 80, kGapY = 32, kGapValue = 48, kValueW = 176, kGapLane = 48;
constexpr f32 kNoteH = 70; // a frame without a scheme: its note
constexpr f32 kTop = 60;   // the first frame: under «+ Своя схема вещи»

int arg_int(const Rml::VariantList& a, usize i, int fallback) { return i < a.size() ? a[i].Get<int>() : fallback; }
Rml::String arg_str(const Rml::VariantList& a, usize i) { return i < a.size() ? a[i].Get<Rml::String>() : Rml::String(); }
bool arg_bool(const Rml::VariantList& a, usize i) { return i < a.size() && a[i].Get<bool>(); }

// Lower case for search: ASCII and Russian letters.
std::string lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (usize i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out += static_cast<char>(std::tolower(c));
            continue;
        }
        if (c == 0xD0 && i + 1 < s.size()) {
            const unsigned char d = static_cast<unsigned char>(s[i + 1]);
            ++i;
            if (d >= 0x90 && d <= 0x9F) { // А..П
                out += static_cast<char>(0xD0);
                out += static_cast<char>(d + 0x20);
            } else if (d >= 0xA0 && d <= 0xAF) { // Р..Я
                out += static_cast<char>(0xD1);
                out += static_cast<char>(d - 0x20);
            } else if (d == 0x81) { // Ё
                out += static_cast<char>(0xD1);
                out += static_cast<char>(0x91);
            } else {
                out += static_cast<char>(c);
                out += static_cast<char>(d);
            }
            continue;
        }
        out += static_cast<char>(c);
    }
    return out;
}

const char* kind_of(script::ValueType t) {
    using script::ValueType;
    switch (t) {
    case ValueType::Bool: return "bool";
    case ValueType::Number:
    case ValueType::Integer: return "number";
    case ValueType::String:
    case ValueType::Enum: return "string";
    case ValueType::Vec2: return "vec2";
    case ValueType::Color: return "color";
    case ValueType::Entity: return "entity";
    case ValueType::Asset: return "asset";
    case ValueType::Table: return "table";
    case ValueType::Any: break;
    }
    return "any";
}

// A wire may carry `from` into `to` (as the compiler converts).
bool types_fit(script::ValueType from, script::ValueType to) {
    using script::ValueType;
    if (from == to || from == ValueType::Any || to == ValueType::Any) return true;
    const auto num = [](ValueType t) { return t == ValueType::Number || t == ValueType::Integer; };
    if (num(from) && num(to)) return true;
    if (to == ValueType::Bool && from == ValueType::Entity) return true;
    if (to == ValueType::String && (num(from) || from == ValueType::Bool || from == ValueType::Enum)) return true;
    if (from == ValueType::String && (to == ValueType::Enum || to == ValueType::Asset)) return true;
    return false;
}

std::string tone_of(const script::NodeDef* d) {
    if (!d) return "missing";
    if (d->kind == script::NodeKind::Event) return "event";
    if (d->category == "Связи") return d->kind == script::NodeKind::Pure ? "links-pure" : "links";
    switch (d->kind) {
    case script::NodeKind::Pure: return "pure";
    case script::NodeKind::Flow: return "flow";
    default: return "action";
    }
}

bool hidden_pin(const script::PinDef& p) { return !p.id.empty() && p.id[0] == '_'; }

std::string node_id(u32 link, u32 uid) { return std::to_string(link) + "-" + std::to_string(uid); }

} // namespace

SchemeView::SchemeView(LogicEditor& editor) : ed_(editor) {
    script::ScriptApi api;
    script::register_core_api(api);
    nodes_ = logic::node_library(api);
    ui::register_line_source("sc-wires", &wires_);
    ui::register_line_source("sc-grid", &grid_);
}

SchemeView::~SchemeView() {
    ui::register_line_source("sc-wires", nullptr);
    ui::register_line_source("sc-grid", nullptr);
}

void SchemeView::bind(Rml::DataModelConstructor& model) {
    if (auto s = model.RegisterStruct<PinView>()) {
        s.RegisterMember("id", &PinView::id);
        s.RegisterMember("title", &PinView::title);
        s.RegisterMember("kind", &PinView::kind);
        s.RegisterMember("field", &PinView::field);
        s.RegisterMember("value", &PinView::value);
        s.RegisterMember("list", &PinView::list);
        s.RegisterMember("on", &PinView::on);
        s.RegisterMember("wired", &PinView::wired);
        s.RegisterMember("pending", &PinView::pending);
        s.RegisterMember("check", &PinView::check);
    }
    if (auto s = model.RegisterStruct<RowView>()) {
        s.RegisterMember("in", &RowView::in);
        s.RegisterMember("out", &RowView::out);
    }
    model.RegisterArray<std::vector<RowView>>();
    if (auto s = model.RegisterStruct<NodeView>()) {
        s.RegisterMember("id", &NodeView::id);
        s.RegisterMember("title", &NodeView::title);
        s.RegisterMember("icon", &NodeView::icon);
        s.RegisterMember("tone", &NodeView::tone);
        s.RegisterMember("problem", &NodeView::problem);
        s.RegisterMember("help", &NodeView::help);
        s.RegisterMember("link", &NodeView::link);
        s.RegisterMember("uid", &NodeView::uid);
        s.RegisterMember("x", &NodeView::x);
        s.RegisterMember("y", &NodeView::y);
        s.RegisterMember("w", &NodeView::w);
        s.RegisterMember("h", &NodeView::h);
        s.RegisterMember("selected", &NodeView::selected);
        s.RegisterMember("lit", &NodeView::lit);
        s.RegisterMember("fixed", &NodeView::fixed);
        s.RegisterMember("warning", &NodeView::warning);
        s.RegisterMember("rows", &NodeView::rows);
    }
    model.RegisterArray<std::vector<NodeView>>();
    if (auto s = model.RegisterStruct<FrameView>()) {
        s.RegisterMember("id", &FrameView::id);
        s.RegisterMember("phrase", &FrameView::phrase);
        s.RegisterMember("problem", &FrameView::problem);
        s.RegisterMember("note", &FrameView::note);
        s.RegisterMember("x", &FrameView::x);
        s.RegisterMember("y", &FrameView::y);
        s.RegisterMember("w", &FrameView::w);
        s.RegisterMember("h", &FrameView::h);
        s.RegisterMember("selected", &FrameView::selected);
        s.RegisterMember("lit", &FrameView::lit);
        s.RegisterMember("custom", &FrameView::custom);
        s.RegisterMember("own", &FrameView::own);
    }
    model.RegisterArray<std::vector<FrameView>>();
    if (auto s = model.RegisterStruct<OptionView>()) {
        s.RegisterMember("id", &OptionView::id);
        s.RegisterMember("name", &OptionView::name);
    }
    model.RegisterArray<std::vector<OptionView>>();
    if (auto s = model.RegisterStruct<PaletteItem>()) {
        s.RegisterMember("id", &PaletteItem::id);
        s.RegisterMember("title", &PaletteItem::title);
        s.RegisterMember("icon", &PaletteItem::icon);
        s.RegisterMember("help", &PaletteItem::help);
        s.RegisterMember("group", &PaletteItem::group);
        s.RegisterMember("tone", &PaletteItem::tone);
        s.RegisterMember("first", &PaletteItem::first);
    }
    model.RegisterArray<std::vector<PaletteItem>>();

    model.Bind("sc_nodes", &m_nodes_);
    model.Bind("sc_frames", &m_frames_);
    model.Bind("sc_things", &m_things_);
    model.Bind("sc_actions", &m_actions_);
    model.Bind("sc_cues", &m_cues_);
    model.Bind("sc_palette", &m_palette_);
    model.Bind("sc_pal_items", &m_palette_items_);
    model.Bind("sc_pal_x", &m_pal_x_);
    model.Bind("sc_pal_y", &m_pal_y_);
    model.Bind("sc_search", &m_search_);
    model.Bind("sc_tip", &m_tip_);
    model.Bind("sc_choosing", &m_choosing_);
    model.Bind("sc_choices", &m_choices_);
    model.Bind("sc_choose_x", &m_choose_x_);
    model.Bind("sc_choose_y", &m_choose_y_);

    auto on = [&model](const char* name, auto fn) {
        model.BindEventCallback(name, [fn](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& a) { fn(ev, a); });
    };
    // The empty pane: drags as a whole; a right click lists nodes.
    on("sc_down", [this](Rml::Event& ev, const Rml::VariantList&) {
        const int button = ev.GetParameter<int>("button", 0);
        const f32 mx = ev.GetParameter<float>("mouse_x", 0), my = ev.GetParameter<float>("mouse_y", 0);
        if (button == 1) {
            const u32 link = frame_at(mx - pane_x_, my - pane_y_);
            f32 x = 0, y = 0;
            if (link && to_scheme(link, mx - pane_x_, my - pane_y_, x, y)) open_palette(link, x, y);
            return;
        }
        if (button != 0) return;
        close_palette();
        u32 wl = 0;
        if (const int wi = wire_at(mx - pane_x_, my - pane_y_, wl); wi >= 0) {
            select_wire(wl, static_cast<usize>(wi));
            return;
        }
        drag_ = Drag::Pan;
        dragged_ = false;
        grab_mx_ = mx;
        grab_my_ = my;
        grab_x_ = pan_x_;
        grab_y_ = pan_y_;
    });
    // A frame's body: a click selects its link; a right click lists nodes.
    on("sc_frame_down", [this](Rml::Event& ev, const Rml::VariantList& a) {
        const u32 link = static_cast<u32>(arg_int(a, 0, 0));
        const int button = ev.GetParameter<int>("button", 0);
        const f32 mx = ev.GetParameter<float>("mouse_x", 0), my = ev.GetParameter<float>("mouse_y", 0);
        ev.StopPropagation();
        if (button == 1) {
            f32 x = 0, y = 0;
            if (to_scheme(link, mx - pane_x_, my - pane_y_, x, y)) open_palette(link, x, y);
            return;
        }
        if (button != 0) return;
        close_palette();
        u32 wl = 0;
        if (const int wi = wire_at(mx - pane_x_, my - pane_y_, wl); wi >= 0) {
            select_wire(wl, static_cast<usize>(wi));
            return;
        }
        sel_node_ = 0;
        select_frame(link);
        wire_selected_ = false;
        drag_ = Drag::Pan;
        dragged_ = false;
        grab_mx_ = mx;
        grab_my_ = my;
        grab_x_ = pan_x_;
        grab_y_ = pan_y_;
    });
    on("sc_node_down", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        if (ev.GetParameter<int>("button", 0) != 0) return;
        const u32 link = static_cast<u32>(arg_int(a, 0, 0)), node = static_cast<u32>(arg_int(a, 1, 0));
        const script::Graph* g = graph(link);
        const script::GraphNode* n = g ? g->find(node) : nullptr;
        if (!n) return;
        close_palette();
        drag_ = Drag::Node;
        dragged_ = false;
        drag_link_ = link;
        drag_node_ = node;
        grab_mx_ = ev.GetParameter<float>("mouse_x", 0);
        grab_my_ = ev.GetParameter<float>("mouse_y", 0);
        grab_x_ = n->x;
        grab_y_ = n->y;
    });
    on("sc_pin_down", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        if (ev.GetParameter<int>("button", 0) != 0) return;
        const u32 link = static_cast<u32>(arg_int(a, 0, 0)), node = static_cast<u32>(arg_int(a, 1, 0));
        const bool out = arg_bool(a, 2);
        const std::string pin = arg_str(a, 3);
        close_palette();
        if (ev.GetParameter<int>("alt_key", 0)) {
            unplug(link, node, out, pin);
            return;
        }
        // The second end of a pending wire, or the first one.
        if (pend_link_ && !(pend_link_ == link && pend_node_ == node && pend_out_ == out && pend_pin_ == pin)) {
            click_pin(link, node, out, pin);
            return;
        }
        pend_link_ = link;
        pend_node_ = node;
        pend_out_ = out;
        pend_pin_ = pin;
        drag_ = Drag::Wire;
        dragged_ = false;
        grab_mx_ = ev.GetParameter<float>("mouse_x", 0);
        grab_my_ = ev.GetParameter<float>("mouse_y", 0);
        mouse_x_ = grab_mx_ - pane_x_;
        mouse_y_ = grab_my_ - pane_y_;
        rebuild();
    });
    // A click (not a press) on a pin: what the self-test sends.
    on("sc_pin", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        if (drag_ != Drag::None) return;
        click_pin(static_cast<u32>(arg_int(a, 0, 0)), static_cast<u32>(arg_int(a, 1, 0)), arg_bool(a, 2), arg_str(a, 3));
    });
    on("sc_node", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        if (drag_ != Drag::None) return;
        select_node(static_cast<u32>(arg_int(a, 0, 0)), static_cast<u32>(arg_int(a, 1, 0)));
    });
    on("sc_stop", [](Rml::Event& ev, const Rml::VariantList&) { ev.StopPropagation(); });
    on("sc_remove", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        remove_node(static_cast<u32>(arg_int(a, 0, 0)), static_cast<u32>(arg_int(a, 1, 0)));
    });
    on("sc_add", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        const u32 link = static_cast<u32>(arg_int(a, 0, 0));
        // Right of everything in the frame.
        f32 x = 0;
        if (const script::Graph* g = graph(link))
            for (const script::GraphNode& n : g->nodes) x = std::max(x, n.x + box_of(link, *g, n).w + kGapX);
        open_palette(link, x, 0);
    });
    on("sc_arrange", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        arrange(static_cast<u32>(arg_int(a, 0, 0)));
    });
    on("sc_reset", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        reset(static_cast<u32>(arg_int(a, 0, 0)));
    });
    on("sc_own_add", [this](Rml::Event& ev, const Rml::VariantList&) {
        ev.StopPropagation();
        open_things();
    });
    on("sc_search", [this](Rml::Event& ev, const Rml::VariantList&) { set_search(ev.GetParameter<Rml::String>("value", "")); });
    on("sc_pal_pick", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        palette_pick(arg_str(a, 0));
    });
    on("sc_pal_close", [this](Rml::Event& ev, const Rml::VariantList&) {
        ev.StopPropagation();
        close_palette();
    });
    // Values typed into a node: kept on Enter or when the field is left.
    on("sc_text", [this](Rml::Event&, const Rml::VariantList& a) {
        if (!arg_bool(a, 4)) return;
        set_value(static_cast<u32>(arg_int(a, 0, 0)), static_cast<u32>(arg_int(a, 1, 0)), arg_str(a, 2), arg_str(a, 3));
    });
    on("sc_text_done", [this](Rml::Event& ev, const Rml::VariantList& a) {
        auto* field = rmlui_dynamic_cast<Rml::ElementFormControl*>(ev.GetTargetElement());
        const std::string value = field ? field->GetValue() : std::string();
        set_value(static_cast<u32>(arg_int(a, 0, 0)), static_cast<u32>(arg_int(a, 1, 0)), arg_str(a, 2), value);
    });
    on("sc_check", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        set_value(static_cast<u32>(arg_int(a, 0, 0)), static_cast<u32>(arg_int(a, 1, 0)), arg_str(a, 2),
                  arg_bool(a, 3) ? "false" : "true");
    });
    on("sc_pick_open", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        open_pick(static_cast<u32>(arg_int(a, 0, 0)), static_cast<u32>(arg_int(a, 1, 0)), arg_str(a, 2));
    });
    on("sc_pick", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        pick(arg_str(a, 0));
    });
    on("sc_pick_close", [this](Rml::Event& ev, const Rml::VariantList&) {
        ev.StopPropagation();
        m_choosing_ = false;
        picking_thing_ = false;
        dirty("sc_choosing");
    });
}

// --- the scheme ----------------------------------------------------------------

const script::Graph* SchemeView::graph(u32 link) const {
    const auto it = shown_.find(link);
    return it == shown_.end() ? nullptr : &it->second;
}

bool SchemeView::own(u32 frame) const { return frame && ed_.logic_.find_scheme(frame) != nullptr; }

bool SchemeView::known(u32 frame) const { return frame && (ed_.logic_.find(frame) || own(frame)); }

void SchemeView::select_frame(u32 frame) {
    if (own(frame)) {
        sel_own_ = frame;
        if (ed_.sel_link_) ed_.select_link(0);
        else rebuild();
        return;
    }
    sel_own_ = 0;
    if (ed_.sel_link_ != frame) ed_.select_link(frame);
}

script::Graph SchemeView::editable(u32 link) const {
    if (const script::Graph* g = graph(link)) return *g;
    if (const logic::ThingScheme* t = ed_.logic_.find_scheme(link)) {
        script::Graph g;
        g.from_json(t->graph);
        return g;
    }
    const logic::Link* l = ed_.logic_.find(link);
    if (!l) return {};
    const logic::FindThing find = [this](std::string_view id) { return ed_.thing(id); };
    return logic::scheme_of(*l, ed_.verbs_, find);
}

void SchemeView::commit(u32 link, const script::Graph& g, const std::string& label, const std::string& merge) {
    shown_[link] = g;
    if (own(link)) ed_.change_thing_scheme(link, g, label, merge);
    else ed_.change_scheme(link, g, label, merge);
}

const script::PinDef* SchemeView::in_pin(const script::Graph& g, u32 node, const std::string& pin) const {
    const script::GraphNode* n = g.find(node);
    const script::NodeDef* d = n ? nodes_.find(n->def) : nullptr;
    return d ? d->input(pin) : nullptr;
}

const script::PinDef* SchemeView::out_pin(const script::Graph& g, u32 node, const std::string& pin) const {
    const script::GraphNode* n = g.find(node);
    const script::NodeDef* d = n ? nodes_.find(n->def) : nullptr;
    return d ? d->output(pin) : nullptr;
}

bool SchemeView::is_exit(const script::Graph& g, u32 node, const std::string& pin) const {
    const script::GraphNode* n = g.find(node);
    const script::NodeDef* d = n ? nodes_.find(n->def) : nullptr;
    if (!d) return false;
    return (pin == script::kFlowNext && d->has_next()) || d->slot(pin);
}

bool SchemeView::fits(const script::Graph& g, u32 from, const std::string& from_pin, u32 to, const std::string& to_pin) const {
    if (from == to) return false;
    const script::GraphNode* tn = g.find(to);
    const script::NodeDef* td = tn ? nodes_.find(tn->def) : nullptr;
    if (!td) return false;
    if (to_pin == script::kFlowIn) return td->has_flow_in() && is_exit(g, from, from_pin);
    const script::PinDef* out = out_pin(g, from, from_pin);
    const script::PinDef* in = td->input(to_pin);
    return out && in && !in->constant && types_fit(out->type, in->type);
}

u32 SchemeView::add_node(u32 link, const std::string& def, f32 x, f32 y) {
    const script::NodeDef* d = nodes_.find(def);
    if (!d || !known(link)) return 0;
    // Links start at «Когда»; a thing's scheme at the events.
    if (d->kind == script::NodeKind::Event && !(own(link) && logic::thing_event(def))) return 0;
    script::Graph g = editable(link);
    script::GraphNode& n = g.add(def, std::round(x / kGrid) * kGrid, std::round(y / kGrid) * kGrid);
    n.version = d->version;
    prefill(link, n);
    const u32 uid = n.uid;
    commit(link, g, "Нода «" + d->title.get() + "»");
    select_node(link, uid);
    return uid;
}

// A new node about the link's things knows them already.
void SchemeView::prefill(u32 link, script::GraphNode& n) const {
    if (const logic::ThingScheme* t = ed_.logic_.find_scheme(link)) {
        if (n.def == "logic.act") n.set_value("thing", t->thing);
        return;
    }
    const logic::Link* l = ed_.logic_.find(link);
    if (!l) return;
    const logic::VerbDef* v = ed_.verbs_.find(l->verb);
    if (n.def == "logic.act") {
        if (v) n.set_value("action", v->action);
        n.set_value("thing", v && v->target == logic::Side::A ? l->a : l->b);
    } else if (n.def == "logic.has" || n.def == "logic.nearest") {
        n.set_value("thing", l->a == logic::kHero ? l->b : l->a);
    } else if (n.def == "logic.sound" && v && !v->sound.empty()) {
        n.set_value("cue", v->sound);
    }
}

bool SchemeView::remove_node(u32 link, u32 node) {
    script::Graph g = editable(link);
    const script::GraphNode* n = g.find(node);
    if (!n || n->def == "logic.when") return false;
    const script::NodeDef* d = nodes_.find(n->def);
    const std::string title = d ? d->title.get() : n->def;
    g.remove(node);
    if (sel_node_ == node) sel_node_ = 0;
    wire_selected_ = false;
    commit(link, g, "Убрана нода «" + title + "»");
    return true;
}

bool SchemeView::move_node(u32 link, u32 node, f32 x, f32 y) {
    script::Graph g = editable(link);
    script::GraphNode* n = g.find(node);
    if (!n) return false;
    n->x = std::round(x / kGrid) * kGrid;
    n->y = std::round(y / kGrid) * kGrid;
    commit(link, g, "Нода сдвинута");
    return true;
}

bool SchemeView::connect(u32 link, u32 from, const std::string& from_pin, u32 to, const std::string& to_pin) {
    script::Graph g = editable(link);
    if (!fits(g, from, from_pin, to, to_pin)) return false;
    // One wire leaves a flow exit, one wire comes into a value.
    const bool flow = to_pin == script::kFlowIn;
    std::erase_if(g.links, [&](const script::GraphLink& w) {
        if (flow) return w.from_node == from && w.from_pin == from_pin && w.to_pin == script::kFlowIn;
        return w.to_node == to && w.to_pin == to_pin;
    });
    g.link(from, from_pin, to, to_pin);
    wire_selected_ = false;
    commit(link, g, flow ? "Провод потока" : "Провод значения");
    return true;
}

bool SchemeView::disconnect(u32 link, usize wire) {
    script::Graph g = editable(link);
    if (wire >= g.links.size()) return false;
    g.links.erase(g.links.begin() + static_cast<std::ptrdiff_t>(wire));
    wire_selected_ = false;
    commit(link, g, "Провод убран");
    return true;
}

bool SchemeView::unplug(u32 link, u32 node, bool out, const std::string& pin) {
    script::Graph g = editable(link);
    const usize before = g.links.size();
    std::erase_if(g.links, [&](const script::GraphLink& w) {
        return out ? w.from_node == node && w.from_pin == pin : w.to_node == node && w.to_pin == pin;
    });
    if (g.links.size() == before) return false;
    commit(link, g, "Провода убраны");
    return true;
}

bool SchemeView::set_value(u32 link, u32 node, const std::string& pin, const std::string& value) {
    script::Graph g = editable(link);
    script::GraphNode* n = g.find(node);
    if (!n || !in_pin(g, node, pin)) return false;
    const std::string* now = n->value(pin);
    if ((now ? *now : std::string()) == value) return false;
    n->set_value(pin, value);
    commit(link, g, "Значение ноды", "value-" + node_id(link, node) + "-" + pin);
    return true;
}

void SchemeView::click_pin(u32 link, u32 node, bool out, const std::string& pin) {
    if (!pend_link_ || pend_link_ != link || (pend_node_ == node && pend_out_ == out)) {
        // The first end (another frame's pin starts over).
        pend_link_ = link;
        pend_node_ = node;
        pend_out_ = out;
        pend_pin_ = pin;
        m_tip_ = "Теперь нажмите на вход или выход другой ноды (Esc — отмена)";
        dirty("sc_tip");
        rebuild();
        return;
    }
    const u32 a = pend_node_;
    const bool a_out = pend_out_;
    const std::string a_pin = pend_pin_;
    pend_link_ = 0;
    m_tip_ = "";
    dirty("sc_tip");
    bool ok = false;
    if (a_out && !out) ok = connect(link, a, a_pin, node, pin);
    else if (!a_out && out) ok = connect(link, node, pin, a, a_pin);
    if (!ok) {
        FORGE_INFO("Эти выход и вход не соединить: поток идёт в поток, значения — в подходящие значения");
        rebuild();
    }
}

void SchemeView::select_node(u32 link, u32 node) {
    sel_link_node_ = link;
    sel_node_ = node;
    wire_selected_ = false;
    select_frame(link);
    rebuild();
}

void SchemeView::select_wire(u32 link, usize wire) {
    sel_wire_link_ = link;
    sel_wire_ = wire;
    wire_selected_ = true;
    sel_node_ = 0;
    select_frame(link);
    rebuild();
}

bool SchemeView::reset(u32 link) {
    if (own(link)) return ed_.remove_thing_scheme(link);
    logic::Logic after = ed_.logic_;
    logic::Link* l = after.find(link);
    if (!l || (l->graph.empty() && l->code.empty())) return false;
    l->graph.clear();
    l->code.clear();
    shown_.erase(link);
    sel_node_ = 0;
    ed_.change(after, "Схема связи как в «Связях»: «" + ed_.phrase_of(link) + "»");
    return true;
}

void SchemeView::open_pick(u32 link, u32 node, const std::string& pin) {
    const script::Graph* g = graph(link);
    const script::PinDef* p = g ? in_pin(*g, node, pin) : nullptr;
    if (!p) return;
    close_palette();
    pick_link_ = link;
    pick_node_ = node;
    pick_pin_ = pin;
    m_choices_ = p->enum_type == "thing" ? m_things_ : p->enum_type == "action" ? m_actions_ : m_cues_;
    // Under the field.
    const std::string id = node_id(link, node);
    for (const NodeView& n : m_nodes_)
        if (n.id == id) {
            usize row = 0;
            for (usize i = 0; i < n.rows.size(); ++i)
                if (n.rows[i].in.id == pin) row = i;
            m_choose_x_ = n.x + 16;
            m_choose_y_ = n.y + kHead + static_cast<f32>(row + 1) * kRowH;
        }
    m_choosing_ = true;
    for (const char* name : {"sc_choosing", "sc_choices", "sc_choose_x", "sc_choose_y"}) dirty(name);
}

void SchemeView::open_things() {
    close_palette();
    m_choices_.clear();
    for (const logic::Thing& t : ed_.things_)
        if (t.id != logic::kHero && !ed_.logic_.scheme_for(t.id)) m_choices_.push_back({t.id, t.name});
    picking_thing_ = true;
    // Under the button, at the pane's top right.
    m_choose_x_ = std::max(8.0f, pane_w_ - 236);
    m_choose_y_ = 44;
    m_choosing_ = true;
    for (const char* name : {"sc_choosing", "sc_choices", "sc_choose_x", "sc_choose_y"}) dirty(name);
}

void SchemeView::focus(u32 frame) {
    for (const FrameView& f : m_frames_)
        if (static_cast<u32>(f.id) == frame) {
            pan_y_ -= f.y - kTop;
            rebuild();
            return;
        }
}

bool SchemeView::pick(const std::string& value) {
    if (!m_choosing_) return false;
    m_choosing_ = false;
    dirty("sc_choosing");
    if (picking_thing_) {
        picking_thing_ = false;
        return ed_.thing_scheme(value) != 0;
    }
    return set_value(pick_link_, pick_node_, pick_pin_, value);
}

std::string SchemeView::node_problem(u32 link, u32 node) const {
    const auto it = node_problems_.find({link, node});
    return it == node_problems_.end() ? std::string() : it->second.first;
}

// --- the list of nodes ---------------------------------------------------------

void SchemeView::open_palette(u32 link, f32 x, f32 y) {
    if (!known(link)) return;
    pal_link_ = link;
    pal_x_ = x;
    pal_y_ = y;
    search_.clear();
    m_search_ = "";
    m_palette_ = true;
    // On screen: where it was asked for, inside the pane.
    const auto p = places_.find(link);
    if (p != places_.end()) {
        m_pal_x_ = p->second.x + kFramePad + (x - p->second.min_x);
        m_pal_y_ = p->second.y + kFrameHead + kFramePad + (y - p->second.min_y);
    }
    if (pane_w_ > 0) {
        m_pal_x_ = std::clamp(m_pal_x_, 8.0f, std::max(8.0f, pane_w_ - 300));
        m_pal_y_ = std::clamp(m_pal_y_, 8.0f, std::max(8.0f, pane_h_ - 400));
    }
    m_choosing_ = false;
    picking_thing_ = false;
    dirty("sc_choosing");
    rebuild_palette();
    for (const char* name : {"sc_palette", "sc_search", "sc_pal_x", "sc_pal_y"}) dirty(name);
}

void SchemeView::close_palette() {
    if (!m_palette_) return;
    m_palette_ = false;
    pal_link_ = 0;
    dirty("sc_palette");
}

void SchemeView::set_search(const std::string& text) {
    search_ = text;
    rebuild_palette();
}

usize SchemeView::palette_items() const { return m_palette_items_.size(); }

void SchemeView::rebuild_palette() {
    m_palette_items_.clear();
    const std::string want = lower(search_);
    // The links' nodes first (a thing's scheme: its events), then the
    // library's groups as they come.
    const bool events = own(pal_link_);
    std::vector<std::string> groups{events ? "События" : "Связи"};
    for (const script::NodeDef& d : nodes_.all())
        if (std::find(groups.begin(), groups.end(), d.category) == groups.end()) groups.push_back(d.category);
    for (const std::string& group : groups) {
        bool first = true;
        for (const script::NodeDef& d : nodes_.all()) {
            if (d.category != group || d.hidden) continue;
            if (d.kind == script::NodeKind::Event && !(events && logic::thing_event(d.id))) continue;
            if (d.kind == script::NodeKind::Comment) continue;
            if (d.id == "std.graph.entry" || d.id == "std.graph.return") continue;
            if (!want.empty() && lower(d.title.get()).find(want) == std::string::npos &&
                lower(d.category).find(want) == std::string::npos)
                continue;
            PaletteItem it{d.id, d.title.get(), d.icon.empty() ? "circle" : d.icon, d.help.get(), group, tone_of(&d), first};
            first = false;
            m_palette_items_.push_back(std::move(it));
        }
    }
    dirty("sc_pal_items");
}

bool SchemeView::palette_pick(const std::string& def) {
    const u32 link = pal_link_;
    if (!link) return false;
    const f32 x = pal_x_, y = pal_y_;
    close_palette();
    // A wire let go here: the new node takes it.
    const bool joining = pend_link_ == link;
    const u32 from = pend_node_;
    const bool from_out = pend_out_;
    const std::string from_pin = pend_pin_;
    pend_link_ = 0;
    const u32 uid = add_node(link, def, x, y);
    if (!uid) return false;
    if (joining) {
        const script::Graph* g = graph(link);
        const script::NodeDef* d = nodes_.find(def);
        if (g && d) {
            if (from_out) {
                if (is_exit(*g, from, from_pin) && d->has_flow_in()) connect(link, from, from_pin, uid, script::kFlowIn);
                else
                    for (const script::PinDef& p : d->inputs)
                        if (!p.constant && fits(*g, from, from_pin, uid, p.id)) {
                            connect(link, from, from_pin, uid, p.id);
                            break;
                        }
            } else if (from_pin == script::kFlowIn) {
                if (d->has_next()) connect(link, uid, script::kFlowNext, from, from_pin);
            } else {
                for (const script::PinDef& p : d->outputs)
                    if (fits(*g, uid, p.id, from, from_pin)) {
                        connect(link, uid, p.id, from, from_pin);
                        break;
                    }
            }
        }
    }
    return true;
}

// --- view ----------------------------------------------------------------------

std::string SchemeView::node_title(u32 link, const script::GraphNode& n, const script::NodeDef* d) const {
    if (!d) return "Нет ноды «" + n.def + "»";
    if (n.def == "logic.when") {
        const std::vector<logic::Step> st = ed_.steps_of(link);
        return st.empty() ? d->title.get() : st[0].text;
    }
    return d->title.get();
}

std::string SchemeView::out_title(u32 link, const script::GraphNode& n, const script::PinDef& p) const {
    if (n.def == "logic.when" && (p.id == "a" || p.id == "b")) {
        const logic::Link* l = ed_.logic_.find(link);
        const logic::Thing* t = l ? ed_.thing(p.id == "a" ? l->a : l->b) : nullptr;
        if (t) return t->name;
    }
    return p.title.get().empty() ? p.id : p.title.get();
}

std::string SchemeView::option_name(const std::string& list, const std::string& id) const {
    const std::vector<OptionView>& opts = list == "thing" ? m_things_ : list == "action" ? m_actions_ : m_cues_;
    for (const OptionView& o : opts)
        if (o.id == id) return o.name;
    return id;
}

namespace {

// How wide text is drawn (px), near enough: the editor's font at 12 px.
f32 text_w(std::string_view s, f32 per = 6.9f) {
    usize n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return static_cast<f32>(n) * per;
}

} // namespace

SchemeView::NodeBox SchemeView::box_of(u32 link, const script::Graph& g, const script::GraphNode& n) const {
    NodeBox b;
    const script::NodeDef* d = nodes_.find(n.def);
    b.title = node_title(link, n, d);
    if (d) {
        std::set<std::pair<u32, std::string>> wired_in;
        for (const script::GraphLink& w : g.links)
            if (w.to_node == n.uid) wired_in.insert({w.to_node, w.to_pin});
        if (d->has_flow_in()) {
            PinView p;
            p.id = script::kFlowIn;
            p.kind = "flow";
            p.on = true;
            b.left.push_back(p);
        }
        for (const script::PinDef& pd : d->inputs) {
            if (hidden_pin(pd)) continue;
            PinView p;
            p.id = pd.id;
            p.title = pd.title.get().empty() ? pd.id : pd.title.get();
            p.kind = kind_of(pd.type);
            p.on = !pd.constant;
            p.wired = wired_in.contains({n.uid, pd.id});
            const std::string* val = n.value(pd.id);
            p.value = val ? *val : pd.value;
            if (!p.wired) {
                if (pd.enum_type == "thing" || pd.enum_type == "action" || pd.enum_type == "cue") {
                    p.field = "pick";
                    p.list = pd.enum_type;
                    p.title = (p.title.empty() ? "" : p.title + ": ") + option_name(pd.enum_type, p.value);
                } else if (pd.type == script::ValueType::Bool) {
                    p.field = "check";
                    p.check = p.value == "true" || p.value == "1" || p.value == "да";
                } else if (pd.type == script::ValueType::Number || pd.type == script::ValueType::Integer) {
                    p.field = "number";
                } else if (pd.type != script::ValueType::Entity && pd.type != script::ValueType::Table) {
                    // Any: a number, yes/no or text, as typed.
                    p.field = "text";
                }
            }
            b.left.push_back(p);
        }
        auto exit = [&](const std::string& id, const std::string& title) {
            PinView p;
            p.id = id;
            p.title = title;
            p.kind = "flow";
            p.on = true;
            b.right.push_back(p);
        };
        if (d->has_next()) exit(script::kFlowNext, d->slots.empty() ? "" : "Далее");
        for (const script::SlotDef& sl : d->slots) exit(sl.id, sl.title.get());
        for (const script::PinDef& pd : d->outputs) {
            PinView p;
            p.id = pd.id;
            p.title = out_title(link, n, pd);
            p.kind = kind_of(pd.type);
            p.on = true;
            b.right.push_back(p);
        }
    }
    // Width: the title, or the widest row (its two sides and the gap).
    const usize rows = std::max<usize>(1, std::max(b.left.size(), b.right.size()));
    f32 w = std::max(kNodeMinW, text_w(b.title, 7.6f) + 96); // the icon, a flag and «×»
    for (usize i = 0; i < rows; ++i) {
        f32 lw = 0, rw = 0;
        if (i < b.left.size()) {
            const PinView& p = b.left[i];
            lw = 22;
            if (p.field == "pick") lw += std::min(kPickW, text_w(p.title) + 34);
            else if (!p.title.empty()) lw += text_w(p.title) + 6;
            if (p.field == "number") lw += kNumW + 6;
            else if (p.field == "text") lw += kTextW + 6;
            else if (p.field == "check") lw += 22;
        }
        if (i < b.right.size()) rw = 22 + text_w(b.right[i].title);
        w = std::max(w, lw + rw + 24);
    }
    b.w = std::min(kNodeMaxW, std::ceil(w / kGrid) * kGrid);
    b.h = kHead + static_cast<f32>(rows) * kRowH + kFoot;
    return b;
}

// The order Unreal's graphs are read in: what happens goes left to right
// (each step a column, a branch a lane under it), the values a step needs
// lie under and before it, next to the pin they go into.
void SchemeView::lay_out(u32 link, script::Graph& g) const {
    if (g.nodes.empty()) return;
    std::map<u32, NodeBox> box;
    for (const script::GraphNode& n : g.nodes) box[n.uid] = box_of(link, g, n);
    auto def = [&](u32 uid) {
        const script::GraphNode* n = g.find(uid);
        return n ? nodes_.find(n->def) : nullptr;
    };
    auto pure = [&](u32 uid) {
        const script::NodeDef* d = def(uid);
        return d && d->kind == script::NodeKind::Pure;
    };
    // Flow: exits in order (next first, then the slots: «Тогда», «Иначе»).
    std::map<u32, std::vector<u32>> exits;
    for (const script::GraphNode& n : g.nodes) {
        const script::NodeDef* d = def(n.uid);
        if (!d) continue;
        std::vector<std::string> order;
        for (const script::SlotDef& sl : d->slots) order.push_back(sl.id);
        order.push_back(script::kFlowNext);
        for (const std::string& pin : order)
            for (const script::GraphLink& w : g.links)
                if (w.from_node == n.uid && w.from_pin == pin && w.to_pin == script::kFlowIn) exits[n.uid].push_back(w.to_node);
    }
    std::set<u32> has_in;
    for (const auto& [_, to] : exits)
        for (u32 t : to) has_in.insert(t);
    std::vector<u32> roots;
    for (const script::GraphNode& n : g.nodes) {
        const script::NodeDef* d = def(n.uid);
        if (d && d->kind == script::NodeKind::Event) roots.push_back(n.uid);
    }
    for (const script::GraphNode& n : g.nodes)
        if (!pure(n.uid) && !has_in.contains(n.uid) && std::find(roots.begin(), roots.end(), n.uid) == roots.end())
            roots.push_back(n.uid);
    // Columns (the longest way to a step) and lanes (a new one per branch).
    std::map<u32, int> col, lane;
    int lanes = 0;
    std::function<void(u32, int, int, int)> walk = [&](u32 uid, int c, int l, int depth) {
        if (depth > 64) return;
        const auto it = col.find(uid);
        if (it != col.end() && it->second >= c) return;
        col[uid] = c;
        if (!lane.contains(uid)) lane[uid] = l;
        bool first = true;
        for (u32 to : exits[uid]) {
            const int next_lane = first || lane.contains(to) ? lane[uid] : lanes++;
            first = false;
            walk(to, c + 1, lane.contains(to) ? lane[to] : next_lane, depth + 1);
        }
    };
    for (u32 r : roots) {
        if (lane.contains(r)) continue;
        walk(r, 0, lanes++, 0);
    }
    // Values: under the step they go into (the first one, through other
    // values too), one column of them further left for each step back.
    std::map<u32, std::pair<u32, int>> feeds; // value -> (step, how far back)
    std::function<void(u32, u32, int)> feed = [&](u32 step, u32 into, int back) {
        for (const script::GraphLink& w : g.links) {
            if (w.to_node != into || w.to_pin == script::kFlowIn || !pure(w.from_node)) continue;
            if (feeds.contains(w.from_node) || back > 16) continue;
            feeds[w.from_node] = {step, back};
            feed(step, w.from_node, back + 1);
        }
    };
    for (const auto& [uid, _] : col) feed(uid, uid, 1);
    // Values nothing uses: a lane of their own.
    const int spare = lanes;
    for (const script::GraphNode& n : g.nodes)
        if (!col.contains(n.uid) && !feeds.contains(n.uid)) {
            col[n.uid] = 0;
            lane[n.uid] = spare;
        }
    // Column x: the widest step in each; before a column, room for wires and
    // for the values its steps take (a slot per step back, as wide as the
    // widest of them).
    int cols = 0;
    for (const auto& [_, c] : col) cols = std::max(cols, c + 1);
    std::vector<f32> col_w(static_cast<usize>(cols), 0), col_x(static_cast<usize>(cols), 0);
    std::vector<f32> val_w(static_cast<usize>(cols), 0);
    std::vector<int> depth(static_cast<usize>(cols), 0);
    for (const auto& [uid, c] : col)
        if (!feeds.contains(uid)) col_w[static_cast<usize>(c)] = std::max(col_w[static_cast<usize>(c)], box[uid].w);
    for (const auto& [uid, f] : feeds) {
        const usize c = static_cast<usize>(col[f.first]);
        val_w[c] = std::max(val_w[c], box[uid].w);
        depth[c] = std::max(depth[c], f.second);
    }
    auto room = [&](usize c) { return static_cast<f32>(depth[c]) * (val_w[c] + kGapValue); };
    col_x[0] = room(0);
    for (usize c = 1; c < static_cast<usize>(cols); ++c)
        col_x[c] = col_x[c - 1] + col_w[c - 1] + std::max(kGapX, room(c) + kGapValue);
    // Lanes top to bottom. Values lie in the room before their step, under
    // the lane's steps (so no wire between steps runs behind them), each
    // nearer step first.
    int all_lanes = 0;
    for (const auto& [_, l] : lane) all_lanes = std::max(all_lanes, l + 1);
    f32 y = 0;
    struct Rect {
        f32 x, y, w, h;
    };
    for (int l = 0; l < all_lanes; ++l) {
        f32 bottom = y;
        for (const auto& [uid, c] : col)
            if (lane[uid] == l && !feeds.contains(uid)) {
                script::GraphNode* n = g.find(uid);
                n->x = col_x[static_cast<usize>(c)];
                n->y = y;
                bottom = std::max(bottom, y + box[uid].h);
            }
        const f32 steps_bottom = bottom;
        std::vector<std::pair<u32, std::pair<u32, int>>> mine;
        for (const auto& f : feeds)
            if (lane[f.second.first] == l) mine.push_back(f);
        std::sort(mine.begin(), mine.end(), [&](const auto& a, const auto& b) {
            if (col[a.second.first] != col[b.second.first]) return col[a.second.first] < col[b.second.first];
            if (a.second.second != b.second.second) return a.second.second < b.second.second;
            return a.first < b.first;
        });
        std::vector<Rect> placed;
        for (const auto& [uid, f] : mine) {
            const script::GraphNode* step = g.find(f.first);
            const usize c = static_cast<usize>(col[f.first]);
            const f32 w = box[uid].w, h = box[uid].h;
            const f32 right = step->x - kGapValue - static_cast<f32>(f.second - 1) * (val_w[c] + kGapValue);
            const f32 x = right - w;
            f32 at = steps_bottom + kGapY;
            for (bool moved = true; moved;) {
                moved = false;
                for (const Rect& r : placed)
                    if (x < r.x + r.w + 16 && x + w + 16 > r.x && at < r.y + r.h + 16 && at + h + 16 > r.y) {
                        at = r.y + r.h + 16;
                        moved = true;
                    }
            }
            placed.push_back({x, at, w, h});
            script::GraphNode* n = g.find(uid);
            n->x = x;
            n->y = at;
            bottom = std::max(bottom, at + h);
        }
        y = bottom + kGapLane;
    }
    for (script::GraphNode& n : g.nodes) {
        n.x = std::round(n.x / kGrid) * kGrid;
        n.y = std::round(n.y / kGrid) * kGrid;
    }
}

bool SchemeView::arrange(u32 link) {
    if (!known(link)) return false;
    script::Graph g = editable(link);
    lay_out(link, g);
    commit(link, g, "Ноды упорядочены");
    return true;
}

void SchemeView::rebuild() {
    // Lists for the pick fields.
    m_things_.clear();
    for (const logic::Thing& t : ed_.things_) m_things_.push_back({t.id, t.name});
    m_actions_.clear();
    m_cues_.clear();
    std::set<std::string> acts, cues;
    for (const logic::VerbDef& v : ed_.verbs_.all()) {
        if (!v.action.empty() && acts.insert(v.action).second) {
            std::string name = v.step.empty() ? v.action : v.step.substr(0, v.step.find(' '));
            m_actions_.push_back({v.action, name});
        }
        if (!v.sound.empty() && cues.insert(v.sound).second) m_cues_.push_back({v.sound, "как «" + v.name + "»"});
    }

    const logic::FindThing find = [this](std::string_view id) { return ed_.thing(id); };
    // Problems of the nodes: what the game would say.
    node_problems_.clear();
    for (const logic::Problem& p : logic::compile(ed_.logic_, ed_.verbs_, find, &nodes_).problems)
        if (p.node) node_problems_[{p.link, p.node}] = {p.text, p.warning};

    // The schemes shown: each link's, kept while a node is dragged. A link
    // whose nodes were never placed by hand is laid out in order.
    std::map<u32, script::Graph> shown;
    for (const logic::Link& l : ed_.logic_.links) {
        if (drag_ == Drag::Node && dragged_ && l.id == drag_link_ && shown_.contains(l.id)) {
            shown[l.id] = shown_[l.id];
            continue;
        }
        shown[l.id] = logic::scheme_of(l, ed_.verbs_, find);
        if (l.graph.empty()) lay_out(l.id, shown[l.id]);
    }
    for (const logic::ThingScheme& t : ed_.logic_.schemes) {
        if (drag_ == Drag::Node && dragged_ && t.id == drag_link_ && shown_.contains(t.id)) {
            shown[t.id] = shown_[t.id];
            continue;
        }
        script::Graph g;
        g.from_json(t.graph);
        shown[t.id] = std::move(g);
    }
    shown_ = std::move(shown);
    if (sel_own_ && !own(sel_own_)) sel_own_ = 0;
    if (pend_link_ && !shown_.contains(pend_link_)) pend_link_ = 0;
    if (wire_selected_ && (!shown_.contains(sel_wire_link_) || sel_wire_ >= shown_[sel_wire_link_].links.size()))
        wire_selected_ = false;
    if (sel_node_ && (!shown_.contains(sel_link_node_) || !shown_[sel_link_node_].find(sel_node_))) sel_node_ = 0;

    m_nodes_.clear();
    m_wires_.clear();
    m_frames_.clear();
    places_.clear();
    wires_.list.clear();
    f32 top = kTop + pan_y_;
    const f32 left = 20 + pan_x_;
    std::vector<u32> frames;
    for (const logic::Link& l : ed_.logic_.links) frames.push_back(l.id);
    for (const logic::ThingScheme& t : ed_.logic_.schemes) frames.push_back(t.id);
    for (const u32 fid : frames) {
        const logic::Link* lk = ed_.logic_.find(fid);
        const logic::ThingScheme* ts = lk ? nullptr : ed_.logic_.find_scheme(fid);
        const bool selected = lk ? fid == ed_.sel_link_ : fid == sel_own_, on = lk && ed_.lit(fid);
        FrameView f;
        f.id = static_cast<int>(fid);
        f.problem = ed_.problem_of(fid);
        f.selected = selected;
        f.lit = on;
        f.x = left;
        f.y = top;
        if (ts) {
            const logic::Thing* t = ed_.thing(ts->thing);
            f.phrase = "«" + (t ? t->name : ts->thing) + "» сама по себе";
            f.own = true;
        } else {
            f.phrase = ed_.phrase_of(fid);
            f.custom = logic::own_scheme(*lk, ed_.verbs_, find) || !lk->code.empty();
        }
        if (lk && !lk->code.empty()) {
            // Its own code decides: the scheme is not used.
            f.note = "Связь задана своим кодом (" + std::to_string(logic::code_lines(lk->code)) +
                     " стр.): он правится в режиме «Код». «Как в Связях» вернёт ей схему.";
            f.w = 520;
            f.h = kFrameHead + kNoteH;
            m_frames_.push_back(f);
            top += f.h + kFrameGap;
            continue;
        }
        const script::Graph& g = shown_[fid];
        // Sizes first: the frame holds every node.
        std::map<u32, NodeBox> boxes;
        f32 min_x = 1e9f, min_y = 1e9f, max_x = -1e9f, max_y = -1e9f;
        for (const script::GraphNode& n : g.nodes) {
            NodeBox b = box_of(fid, g, n);
            min_x = std::min(min_x, n.x);
            min_y = std::min(min_y, n.y);
            max_x = std::max(max_x, n.x + b.w);
            max_y = std::max(max_y, n.y + b.h);
            boxes[n.uid] = std::move(b);
        }
        if (g.nodes.empty()) min_x = min_y = max_x = max_y = 0;
        places_[fid] = {left, top, min_x, min_y};
        const f32 ox = left + kFramePad - min_x, oy = top + kFrameHead + kFramePad - min_y;
        f.w = std::max(460.0f, max_x - min_x + kFramePad * 2);
        f.h = max_y - min_y + kFrameHead + kFramePad * 2;
        m_frames_.push_back(f);

        // Where each pin is on screen, for the wires.
        std::map<std::tuple<u32, bool, std::string>, std::pair<f32, f32>> pins;
        std::map<std::tuple<u32, bool, std::string>, Rml::String> pin_kind;
        std::set<std::pair<u32, std::string>> wired_in, wired_out;
        for (const script::GraphLink& w : g.links) {
            wired_out.insert({w.from_node, w.from_pin});
            wired_in.insert({w.to_node, w.to_pin});
        }
        for (const script::GraphNode& n : g.nodes) {
            const script::NodeDef* d = nodes_.find(n.def);
            NodeBox& b = boxes[n.uid];
            NodeView v;
            v.id = node_id(fid, n.uid);
            v.link = static_cast<int>(fid);
            v.uid = static_cast<int>(n.uid);
            v.title = b.title;
            v.icon = d && !d->icon.empty() ? d->icon : "circle";
            v.tone = tone_of(d);
            v.help = d ? d->help.get() : std::string();
            v.x = ox + n.x;
            v.y = oy + n.y;
            v.w = b.w;
            v.h = b.h;
            v.selected = sel_node_ == n.uid && sel_link_node_ == fid;
            v.lit = on;
            v.fixed = n.def == "logic.when";
            if (const auto it = node_problems_.find({fid, n.uid}); it != node_problems_.end()) {
                v.problem = it->second.first;
                v.warning = it->second.second;
            }
            auto pend = [&](bool out, const std::string& pin) {
                return pend_link_ == fid && pend_node_ == n.uid && pend_out_ == out && pend_pin_ == pin;
            };
            for (PinView& p : b.left) {
                p.wired = wired_in.contains({n.uid, p.id});
                p.pending = pend(false, p.id);
            }
            for (PinView& p : b.right) {
                p.wired = wired_out.contains({n.uid, p.id});
                p.pending = pend(true, p.id);
            }
            const usize rows = std::max<usize>(1, std::max(b.left.size(), b.right.size()));
            for (usize i = 0; i < rows; ++i) {
                RowView r;
                const f32 py = v.y + kHead + (static_cast<f32>(i) + 0.5f) * kRowH;
                if (i < b.left.size()) {
                    r.in = b.left[i];
                    pins[{n.uid, false, r.in.id}] = {v.x, py};
                    pin_kind[{n.uid, false, r.in.id}] = r.in.kind;
                }
                if (i < b.right.size()) {
                    r.out = b.right[i];
                    pins[{n.uid, true, r.out.id}] = {v.x + v.w, py};
                    pin_kind[{n.uid, true, r.out.id}] = r.out.kind;
                }
                v.rows.push_back(std::move(r));
            }
            m_nodes_.push_back(std::move(v));
        }

        // Wires: curves from pin to pin, under the nodes.
        for (usize i = 0; i < g.links.size(); ++i) {
            const script::GraphLink& gl = g.links[i];
            const auto a = pins.find({gl.from_node, true, gl.from_pin});
            const auto b = pins.find({gl.to_node, false, gl.to_pin});
            if (a == pins.end() || b == pins.end()) continue;
            WireView w;
            w.id = "sc-wire-" + std::to_string(fid) + "-" + std::to_string(i);
            w.link = static_cast<int>(fid);
            w.index = static_cast<int>(i);
            w.kind = pin_kind[{gl.from_node, true, gl.from_pin}];
            w.flow = gl.to_pin == script::kFlowIn;
            w.lit = on;
            w.selected = wire_selected_ && sel_wire_link_ == fid && sel_wire_ == i;
            ui::wire_curve(a->second.first, a->second.second, b->second.first, b->second.second, w.points);
            m_wires_.push_back(std::move(w));
        }
        // A wire on its way: from its pin to the mouse.
        if (drag_ == Drag::Wire && dragged_ && pend_link_ == fid) {
            const auto a = pins.find({pend_node_, pend_out_, pend_pin_});
            if (a != pins.end()) {
                WireView w;
                w.id = "sc-wire-pending";
                w.link = static_cast<int>(fid);
                w.index = -1;
                w.kind = pin_kind[{pend_node_, pend_out_, pend_pin_}];
                w.flow = w.kind == "flow";
                w.pending = true;
                if (pend_out_) ui::wire_curve(a->second.first, a->second.second, mouse_x_, mouse_y_, w.points);
                else ui::wire_curve(mouse_x_, mouse_y_, a->second.first, a->second.second, w.points);
                m_wires_.push_back(std::move(w));
            }
        }
        top += f.h + kFrameGap;
    }
    for (const WireView& w : m_wires_) {
        ui::Line line;
        line.points = w.points;
        line.width = w.flow ? 3.0f : 2.0f;
        line.color = w.selected ? 0xffb74dffu : w.lit ? 0x66d17affu : wire_color(w.kind);
        if (w.pending) line.color = (line.color & 0xffffff00u) | 0xb0u;
        wires_.list.push_back(std::move(line));
    }
    ++wires_.counter;
    rebuild_grid();
    for (const char* name : {"sc_nodes", "sc_frames", "sc_things", "sc_actions", "sc_cues"}) dirty(name);
}

// The paper under the schemes: a fine grid, every eighth line stronger,
// moving with the view.
void SchemeView::rebuild_grid() {
    grid_.list.clear();
    const f32 w = std::max(pane_w_, 400.0f), h = std::max(pane_h_, 300.0f);
    const f32 big = kGrid * 8;
    auto lines = [&](f32 step, u32 color) {
        const f32 sx = std::fmod(pan_x_, step), sy = std::fmod(pan_y_, step);
        for (f32 x = sx < 0 ? sx + step : sx; x < w; x += step) grid_.list.push_back({{x, 0, x, h}, color, 1.0f});
        for (f32 y = sy < 0 ? sy + step : sy; y < h; y += step) grid_.list.push_back({{0, y, w, y}, color, 1.0f});
    };
    lines(kGrid, 0xffffff0au);
    lines(big, 0xffffff1au);
    ++grid_.counter;
}

u32 SchemeView::wire_color(const std::string& kind) {
    if (kind == "flow") return 0xf2f2f2ffu;
    if (kind == "bool") return 0xc2302fffu;
    if (kind == "number") return 0x9ee24bffu;
    if (kind == "string") return 0xf04fd2ffu;
    if (kind == "entity") return 0x2f9df2ffu;
    if (kind == "vec2") return 0xf2c22fffu;
    return 0xa0a8b8ffu;
}

int SchemeView::wire_at(f32 x, f32 y, u32& link) const {
    f32 best = 6; // px: near enough to a wire
    int found = -1;
    for (const WireView& w : m_wires_) {
        if (w.pending) continue;
        const f32 d = ui::distance_to(w.points, x, y);
        if (d < best) {
            best = d;
            found = w.index;
            link = static_cast<u32>(w.link);
        }
    }
    return found;
}

bool SchemeView::to_scheme(u32 link, f32 sx, f32 sy, f32& x, f32& y) const {
    const auto it = places_.find(link);
    if (it == places_.end()) return false;
    x = std::round(sx - it->second.x - kFramePad + it->second.min_x);
    y = std::round(sy - it->second.y - kFrameHead - kFramePad + it->second.min_y);
    return true;
}

u32 SchemeView::frame_at(f32 sx, f32 sy) const {
    for (const FrameView& f : m_frames_)
        if (sx >= f.x && sx <= f.x + f.w && sy >= f.y && sy <= f.y + f.h) return static_cast<u32>(f.id);
    return 0;
}

// --- input -----------------------------------------------------------------------

void SchemeView::update(Rml::Context* context) {
    context_ = context;
    for (int i = 0; context && i < context->GetNumDocuments(); ++i)
        if (Rml::Element* pane = context->GetDocument(i)->GetElementById("lg-scheme")) {
            const Rml::Vector2f at = pane->GetAbsoluteOffset(Rml::BoxArea::Padding);
            pane_x_ = at.x;
            pane_y_ = at.y;
            const f32 w = pane->GetClientWidth(), h = pane->GetClientHeight();
            if (w != pane_w_ || h != pane_h_) {
                pane_w_ = w;
                pane_h_ = h;
                rebuild_grid();
            }
            break;
        }
}

bool SchemeView::handle_event(const SDL_Event& e, f32 density) {
    if (drag_ == Drag::None) return false;
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        const f32 mx = e.motion.x * density, my = e.motion.y * density;
        const f32 dx = mx - grab_mx_, dy = my - grab_my_;
        if (!dragged_ && std::hypot(dx, dy) > 4) dragged_ = true;
        if (!dragged_) return true;
        if (drag_ == Drag::Pan) {
            pan_x_ = grab_x_ + dx;
            pan_y_ = grab_y_ + dy;
        } else if (drag_ == Drag::Node) {
            // Live while dragging; one step of the history when let go.
            if (script::Graph* g = shown_.contains(drag_link_) ? &shown_[drag_link_] : nullptr)
                if (script::GraphNode* n = g->find(drag_node_)) {
                    n->x = std::round(grab_x_ + dx);
                    n->y = std::round(grab_y_ + dy);
                }
        } else if (drag_ == Drag::Wire) {
            mouse_x_ = mx - pane_x_;
            mouse_y_ = my - pane_y_;
        }
        rebuild();
        return true;
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
        const Drag was = drag_;
        drag_ = Drag::None;
        const bool moved = dragged_;
        dragged_ = false;
        if (was == Drag::Pan) {
            if (!moved) {
                m_choosing_ = false;
                dirty("sc_choosing");
                sel_node_ = 0;
                wire_selected_ = false;
                pend_link_ = 0;
                m_tip_ = "";
                dirty("sc_tip");
                rebuild();
            }
        } else if (was == Drag::Node) {
            if (moved) {
                const script::Graph* g = graph(drag_link_);
                const script::GraphNode* n = g ? g->find(drag_node_) : nullptr;
                if (n) {
                    const f32 x = n->x, y = n->y;
                    // The history starts from where it was.
                    shown_[drag_link_].find(drag_node_)->x = grab_x_;
                    shown_[drag_link_].find(drag_node_)->y = grab_y_;
                    move_node(drag_link_, drag_node_, x, y);
                }
            } else {
                select_node(drag_link_, drag_node_);
            }
        } else if (was == Drag::Wire && moved) {
            // Let go on a pin: joined; on the empty frame: the list of nodes.
            const u32 link = pend_link_;
            Rml::Element* hover = context_ ? context_->GetHoverElement() : nullptr;
            while (hover && hover->GetId().rfind("sc-pin-", 0) != 0) hover = hover->GetParentNode();
            if (hover) {
                const std::string id = hover->GetId(); // sc-pin-<link>-<node>-<i|o>-<pin>
                const std::string rest = id.substr(7);
                const usize a = rest.find('-'), b = rest.find('-', a + 1), c = rest.find('-', b + 1);
                if (a != std::string::npos && b != std::string::npos && c != std::string::npos) {
                    const u32 l = static_cast<u32>(std::stoul(rest.substr(0, a)));
                    const u32 node = static_cast<u32>(std::stoul(rest.substr(a + 1, b - a - 1)));
                    click_pin(l, node, rest.substr(b + 1, c - b - 1) == "o", rest.substr(c + 1));
                }
            } else if (link) {
                f32 x = 0, y = 0;
                if (frame_at(mouse_x_, mouse_y_) == link && to_scheme(link, mouse_x_, mouse_y_, x, y)) {
                    open_palette(link, x, y);
                    rebuild();
                } else {
                    pend_link_ = 0;
                    rebuild();
                }
            }
        } else if (was == Drag::Wire) {
            // A click: the pin waits for its other end.
            m_tip_ = "Теперь нажмите на вход или выход другой ноды (Esc — отмена)";
            dirty("sc_tip");
            rebuild();
        }
        return true;
    }
    return false;
}

bool SchemeView::handle_key(const SDL_KeyboardEvent& k) {
    if (k.key == SDLK_ESCAPE) {
        if (m_choosing_) m_choosing_ = false, dirty("sc_choosing");
        else if (m_palette_) close_palette();
        else if (pend_link_) pend_link_ = 0;
        else if (sel_node_ || wire_selected_) sel_node_ = 0, wire_selected_ = false;
        else return false;
        m_tip_ = "";
        dirty("sc_tip");
        rebuild();
        return true;
    }
    if (k.key == SDLK_DELETE || k.key == SDLK_BACKSPACE) {
        if (sel_node_) return remove_node(sel_link_node_, sel_node_);
        if (wire_selected_) return disconnect(sel_wire_link_, sel_wire_);
    }
    return false;
}

} // namespace forge::editor_app
