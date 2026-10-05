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
constexpr f32 kNodeW = 250, kHead = 30, kRowH = 26, kFoot = 8;
constexpr f32 kFramePad = 24, kFrameHead = 34, kFrameGap = 40;
constexpr f32 kNoteH = 70; // a frame without a scheme: its note

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
    if (auto s = model.RegisterStruct<SegView>()) {
        s.RegisterMember("x", &SegView::x);
        s.RegisterMember("y", &SegView::y);
        s.RegisterMember("len", &SegView::len);
        s.RegisterMember("across", &SegView::across);
    }
    model.RegisterArray<std::vector<SegView>>();
    if (auto s = model.RegisterStruct<WireView>()) {
        s.RegisterMember("id", &WireView::id);
        s.RegisterMember("kind", &WireView::kind);
        s.RegisterMember("link", &WireView::link);
        s.RegisterMember("index", &WireView::index);
        s.RegisterMember("x", &WireView::x);
        s.RegisterMember("y", &WireView::y);
        s.RegisterMember("w", &WireView::w);
        s.RegisterMember("h", &WireView::h);
        s.RegisterMember("segs", &WireView::segs);
        s.RegisterMember("selected", &WireView::selected);
        s.RegisterMember("lit", &WireView::lit);
        s.RegisterMember("flow", &WireView::flow);
        s.RegisterMember("pending", &WireView::pending);
    }
    model.RegisterArray<std::vector<WireView>>();
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
    model.Bind("sc_wires", &m_wires_);
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
        if (ed_.sel_link_ != link) ed_.select_link(link);
        sel_node_ = 0;
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
    on("sc_wire", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        select_wire(static_cast<u32>(arg_int(a, 0, 0)), static_cast<usize>(arg_int(a, 1, 0)));
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
            for (const script::GraphNode& n : g->nodes) x = std::max(x, n.x + kNodeW + 40);
        open_palette(link, x, 0);
    });
    on("sc_reset", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        reset(static_cast<u32>(arg_int(a, 0, 0)));
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
        dirty("sc_choosing");
    });
}

// --- the scheme ----------------------------------------------------------------

const script::Graph* SchemeView::graph(u32 link) const {
    const auto it = shown_.find(link);
    return it == shown_.end() ? nullptr : &it->second;
}

script::Graph SchemeView::editable(u32 link) const {
    if (const script::Graph* g = graph(link)) return *g;
    const logic::Link* l = ed_.logic_.find(link);
    if (!l) return {};
    const logic::FindThing find = [this](std::string_view id) { return ed_.thing(id); };
    return logic::scheme_of(*l, ed_.verbs_, find);
}

void SchemeView::commit(u32 link, const script::Graph& g, const std::string& label, const std::string& merge) {
    shown_[link] = g;
    ed_.change_scheme(link, g, label, merge);
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
    if (!d || d->kind == script::NodeKind::Event || !ed_.logic_.find(link)) return 0;
    script::Graph g = editable(link);
    script::GraphNode& n = g.add(def, std::round(x), std::round(y));
    n.version = d->version;
    prefill(link, n);
    const u32 uid = n.uid;
    commit(link, g, "Нода «" + d->title.get() + "»");
    select_node(link, uid);
    return uid;
}

// A new node about the link's things knows them already.
void SchemeView::prefill(u32 link, script::GraphNode& n) const {
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
    n->x = std::round(x);
    n->y = std::round(y);
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
    if (ed_.sel_link_ != link) ed_.select_link(link);
    sel_link_node_ = link;
    sel_node_ = node;
    wire_selected_ = false;
    rebuild();
}

void SchemeView::select_wire(u32 link, usize wire) {
    if (ed_.sel_link_ != link) ed_.select_link(link);
    sel_wire_link_ = link;
    sel_wire_ = wire;
    wire_selected_ = true;
    sel_node_ = 0;
    rebuild();
}

bool SchemeView::reset(u32 link) {
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

bool SchemeView::pick(const std::string& value) {
    if (!m_choosing_) return false;
    m_choosing_ = false;
    dirty("sc_choosing");
    return set_value(pick_link_, pick_node_, pick_pin_, value);
}

std::string SchemeView::node_problem(u32 link, u32 node) const {
    const auto it = node_problems_.find({link, node});
    return it == node_problems_.end() ? std::string() : it->second.first;
}

// --- the list of nodes ---------------------------------------------------------

void SchemeView::open_palette(u32 link, f32 x, f32 y) {
    if (!ed_.logic_.find(link)) return;
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
        m_pal_y_ = p->second.y + kFrameHead + (y - p->second.min_y);
    }
    if (pane_w_ > 0) {
        m_pal_x_ = std::clamp(m_pal_x_, 8.0f, std::max(8.0f, pane_w_ - 300));
        m_pal_y_ = std::clamp(m_pal_y_, 8.0f, std::max(8.0f, pane_h_ - 400));
    }
    m_choosing_ = false;
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
    // The links' nodes first, then the library's groups as they come.
    std::vector<std::string> groups{"Связи"};
    for (const script::NodeDef& d : nodes_.all())
        if (std::find(groups.begin(), groups.end(), d.category) == groups.end()) groups.push_back(d.category);
    for (const std::string& group : groups) {
        bool first = true;
        for (const script::NodeDef& d : nodes_.all()) {
            if (d.category != group || d.hidden) continue;
            if (d.kind == script::NodeKind::Event || d.kind == script::NodeKind::Comment) continue;
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
    auto option_name = [&](const std::string& list, const std::string& id) -> std::string {
        const std::vector<OptionView>& opts = list == "thing" ? m_things_ : list == "action" ? m_actions_ : m_cues_;
        for (const OptionView& o : opts)
            if (o.id == id) return o.name;
        return id;
    };

    const logic::FindThing find = [this](std::string_view id) { return ed_.thing(id); };
    // Problems of the nodes: what the game would say.
    node_problems_.clear();
    for (const logic::Problem& p : logic::compile(ed_.logic_, ed_.verbs_, find, &nodes_).problems)
        if (p.node) node_problems_[{p.link, p.node}] = {p.text, p.warning};

    // The schemes shown: each link's, kept while a node is dragged.
    std::map<u32, script::Graph> shown;
    for (const logic::Link& l : ed_.logic_.links) {
        if (drag_ == Drag::Node && dragged_ && l.id == drag_link_ && shown_.contains(l.id)) {
            shown[l.id] = shown_[l.id];
            continue;
        }
        shown[l.id] = logic::scheme_of(l, ed_.verbs_, find);
    }
    shown_ = std::move(shown);
    if (pend_link_ && !shown_.contains(pend_link_)) pend_link_ = 0;
    if (wire_selected_ && (!shown_.contains(sel_wire_link_) || sel_wire_ >= shown_[sel_wire_link_].links.size()))
        wire_selected_ = false;
    if (sel_node_ && (!shown_.contains(sel_link_node_) || !shown_[sel_link_node_].find(sel_node_))) sel_node_ = 0;

    m_nodes_.clear();
    m_wires_.clear();
    m_frames_.clear();
    places_.clear();
    f32 top = 20 + pan_y_;
    const f32 left = 20 + pan_x_;
    for (const logic::Link& l : ed_.logic_.links) {
        const bool selected = l.id == ed_.sel_link_, on = ed_.lit(l.id);
        FrameView f;
        f.id = static_cast<int>(l.id);
        f.phrase = ed_.phrase_of(l.id);
        f.problem = ed_.problem_of(l.id);
        f.selected = selected;
        f.lit = on;
        f.custom = logic::own_scheme(l, ed_.verbs_, find) || !l.code.empty();
        f.x = left;
        f.y = top;
        if (!l.code.empty()) {
            // Its own code decides: the scheme is not used.
            f.note = "Связь задана своим кодом (" + std::to_string(logic::code_lines(l.code)) +
                     " стр.): он правится в режиме «Код». «Как в Связях» вернёт ей схему.";
            f.w = 520;
            f.h = kFrameHead + kNoteH;
            m_frames_.push_back(f);
            top += f.h + kFrameGap;
            continue;
        }
        const script::Graph& g = shown_[l.id];
        // Sizes first: the frame holds every node.
        std::map<u32, std::pair<usize, usize>> sizes; // rows of each node: left, right
        f32 min_x = 1e9f, min_y = 1e9f, max_x = -1e9f, max_y = -1e9f;
        std::map<u32, f32> heights;
        for (const script::GraphNode& n : g.nodes) {
            const script::NodeDef* d = nodes_.find(n.def);
            usize ins = 0, outs = 0;
            if (d) {
                ins = d->has_flow_in() ? 1 : 0;
                for (const script::PinDef& p : d->inputs) ins += !hidden_pin(p);
                outs = (d->has_next() ? 1 : 0) + d->slots.size() + d->outputs.size();
            }
            const usize rows = std::max<usize>(1, std::max(ins, outs));
            const f32 h = kHead + static_cast<f32>(rows) * kRowH + kFoot;
            heights[n.uid] = h;
            min_x = std::min(min_x, n.x);
            min_y = std::min(min_y, n.y);
            max_x = std::max(max_x, n.x + kNodeW);
            max_y = std::max(max_y, n.y + h);
        }
        if (g.nodes.empty()) min_x = min_y = max_x = max_y = 0;
        places_[l.id] = {left, top, min_x, min_y};
        const f32 ox = left + kFramePad - min_x, oy = top + kFrameHead - min_y;
        f.w = std::max(420.0f, max_x - min_x + kFramePad * 2);
        f.h = max_y - min_y + kFrameHead + kFramePad;
        m_frames_.push_back(f);

        // Where each pin is on screen, for the wires.
        std::map<std::tuple<u32, bool, std::string>, std::pair<f32, f32>> pins;
        std::map<std::tuple<u32, bool, std::string>, Rml::String> pin_kind;
        std::map<u32, std::pair<f32, f32>> spans; // each node's top and bottom on screen
        std::set<std::pair<u32, std::string>> wired_in, wired_out;
        for (const script::GraphLink& w : g.links) {
            wired_out.insert({w.from_node, w.from_pin});
            wired_in.insert({w.to_node, w.to_pin});
        }
        for (const script::GraphNode& n : g.nodes) {
            const script::NodeDef* d = nodes_.find(n.def);
            NodeView v;
            v.id = node_id(l.id, n.uid);
            v.link = static_cast<int>(l.id);
            v.uid = static_cast<int>(n.uid);
            v.title = node_title(l.id, n, d);
            v.icon = d && !d->icon.empty() ? d->icon : "circle";
            v.tone = tone_of(d);
            v.help = d ? d->help.get() : std::string();
            v.x = ox + n.x;
            v.y = oy + n.y;
            v.w = kNodeW;
            v.h = heights[n.uid];
            spans[n.uid] = {v.y, v.y + v.h};
            v.selected = sel_node_ == n.uid && sel_link_node_ == l.id;
            v.lit = on;
            v.fixed = n.def == "logic.when";
            if (const auto it = node_problems_.find({l.id, n.uid}); it != node_problems_.end()) {
                v.problem = it->second.first;
                v.warning = it->second.second;
            }
            std::vector<PinView> left_pins, right_pins;
            auto pend = [&](bool out, const std::string& pin) {
                return pend_link_ == l.id && pend_node_ == n.uid && pend_out_ == out && pend_pin_ == pin;
            };
            if (d) {
                if (d->has_flow_in()) {
                    PinView p;
                    p.id = script::kFlowIn;
                    p.kind = "flow";
                    p.on = true;
                    p.wired = wired_in.contains({n.uid, script::kFlowIn});
                    p.pending = pend(false, script::kFlowIn);
                    left_pins.push_back(p);
                }
                for (const script::PinDef& pd : d->inputs) {
                    if (hidden_pin(pd)) continue;
                    PinView p;
                    p.id = pd.id;
                    p.title = pd.title.get().empty() ? pd.id : pd.title.get();
                    p.kind = kind_of(pd.type);
                    p.on = !pd.constant;
                    p.wired = wired_in.contains({n.uid, pd.id});
                    p.pending = pend(false, pd.id);
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
                        } else if (pd.type != script::ValueType::Entity && pd.type != script::ValueType::Table &&
                                   pd.type != script::ValueType::Any) {
                            p.field = "text";
                        }
                    }
                    left_pins.push_back(p);
                }
                auto exit = [&](const std::string& id, const std::string& title) {
                    PinView p;
                    p.id = id;
                    p.title = title;
                    p.kind = "flow";
                    p.on = true;
                    p.wired = wired_out.contains({n.uid, id});
                    p.pending = pend(true, id);
                    right_pins.push_back(p);
                };
                if (d->has_next()) exit(script::kFlowNext, d->slots.empty() ? "" : "Далее");
                for (const script::SlotDef& s : d->slots) exit(s.id, s.title.get());
                for (const script::PinDef& pd : d->outputs) {
                    PinView p;
                    p.id = pd.id;
                    p.title = out_title(l.id, n, pd);
                    p.kind = kind_of(pd.type);
                    p.on = true;
                    p.wired = wired_out.contains({n.uid, pd.id});
                    p.pending = pend(true, pd.id);
                    right_pins.push_back(p);
                }
            }
            const usize rows = std::max<usize>(1, std::max(left_pins.size(), right_pins.size()));
            for (usize i = 0; i < rows; ++i) {
                RowView r;
                const f32 py = v.y + kHead + (static_cast<f32>(i) + 0.5f) * kRowH;
                if (i < left_pins.size()) {
                    r.in = left_pins[i];
                    pins[{n.uid, false, r.in.id}] = {v.x, py};
                    pin_kind[{n.uid, false, r.in.id}] = r.in.kind;
                }
                if (i < right_pins.size()) {
                    r.out = right_pins[i];
                    pins[{n.uid, true, r.out.id}] = {v.x + kNodeW, py};
                    pin_kind[{n.uid, true, r.out.id}] = r.out.kind;
                }
                v.rows.push_back(std::move(r));
            }
            m_nodes_.push_back(std::move(v));
        }

        // Wires: out of a pin, up or down halfway, into the other pin.
        // Out of a pin to the right, into a pin from the left: halfway across
        // when the second pin is further right; else round, between the rows.
        // from, to: the nodes' tops and bottoms, so a wire going round passes
        // between them, not through them.
        using Span = std::pair<f32, f32>;
        auto add_wire = [&](f32 x0, f32 y0, f32 x1, f32 y1, Span from, Span to, WireView w) {
            std::vector<std::pair<f32, f32>> pts;
            if (x1 >= x0 + 24) {
                const f32 mid = std::round(x0 + (x1 - x0) * 0.5f);
                pts = {{x0, y0}, {mid, y0}, {mid, y1}, {x1, y1}};
            } else {
                const f32 out = x0 + 16, in = x1 - 16;
                f32 ym = std::max(from.second, to.second) + 18; // under both
                if (to.first > from.second + 20) ym = std::round((from.second + to.first) * 0.5f);
                else if (from.first > to.second + 20) ym = std::round((to.second + from.first) * 0.5f);
                pts = {{x0, y0}, {out, y0}, {out, ym}, {in, ym}, {in, y1}, {x1, y1}};
            }
            f32 lo_x = 1e9f, lo_y = 1e9f, hi_x = -1e9f, hi_y = -1e9f;
            for (const auto& [px, py] : pts) {
                lo_x = std::min(lo_x, px), hi_x = std::max(hi_x, px);
                lo_y = std::min(lo_y, py), hi_y = std::max(hi_y, py);
            }
            // The box, a little bigger than the wire so its edges can be hit.
            w.x = lo_x - 6;
            w.y = lo_y - 6;
            w.w = hi_x - lo_x + 12;
            w.h = hi_y - lo_y + 12;
            for (usize k = 0; k + 1 < pts.size(); ++k) {
                const auto [ax, ay] = pts[k];
                const auto [bx, by] = pts[k + 1];
                SegView seg;
                seg.across = ay == by;
                seg.x = std::min(ax, bx) - w.x;
                seg.y = std::min(ay, by) - w.y;
                seg.len = seg.across ? std::fabs(bx - ax) : std::fabs(by - ay);
                if (seg.len > 0) w.segs.push_back(seg);
            }
            m_wires_.push_back(std::move(w));
        };
        for (usize i = 0; i < g.links.size(); ++i) {
            const script::GraphLink& gl = g.links[i];
            const auto a = pins.find({gl.from_node, true, gl.from_pin});
            const auto b = pins.find({gl.to_node, false, gl.to_pin});
            if (a == pins.end() || b == pins.end()) continue;
            WireView w;
            w.id = "sc-wire-" + std::to_string(l.id) + "-" + std::to_string(i);
            w.link = static_cast<int>(l.id);
            w.index = static_cast<int>(i);
            w.kind = pin_kind[{gl.from_node, true, gl.from_pin}];
            w.flow = gl.to_pin == script::kFlowIn;
            w.lit = on;
            w.selected = wire_selected_ && sel_wire_link_ == l.id && sel_wire_ == i;
            add_wire(a->second.first, a->second.second, b->second.first, b->second.second, spans[gl.from_node],
                     spans[gl.to_node], std::move(w));
        }
        // A wire on its way: from its pin to the mouse.
        if (drag_ == Drag::Wire && dragged_ && pend_link_ == l.id) {
            const auto a = pins.find({pend_node_, pend_out_, pend_pin_});
            if (a != pins.end()) {
                WireView w;
                w.id = "sc-wire-pending";
                w.link = static_cast<int>(l.id);
                w.index = -1;
                w.kind = pin_kind[{pend_node_, pend_out_, pend_pin_}];
                w.flow = w.kind == "flow";
                w.pending = true;
                const Span at = spans[pend_node_], mouse{mouse_y_, mouse_y_};
                if (pend_out_) add_wire(a->second.first, a->second.second, mouse_x_, mouse_y_, at, mouse, std::move(w));
                else add_wire(mouse_x_, mouse_y_, a->second.first, a->second.second, mouse, at, std::move(w));
            }
        }
        top += f.h + kFrameGap;
    }
    for (const char* name : {"sc_nodes", "sc_wires", "sc_frames", "sc_things", "sc_actions", "sc_cues"}) dirty(name);
}

bool SchemeView::to_scheme(u32 link, f32 sx, f32 sy, f32& x, f32& y) const {
    const auto it = places_.find(link);
    if (it == places_.end()) return false;
    x = std::round(sx - it->second.x - kFramePad + it->second.min_x);
    y = std::round(sy - it->second.y - kFrameHead + it->second.min_y);
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
            pane_w_ = pane->GetClientWidth();
            pane_h_ = pane->GetClientHeight();
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
