#include "slice_links.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>

#include <algorithm>
#include <cmath>
#include <map>

namespace logic = forge::logic;

namespace slice {

namespace {

constexpr float kLinkGap = 26.0f; // between the arrows of one pair
constexpr float kPickW = 340.0f, kChoiceH = 52.0f, kMargin = 12.0f;

const logic::Thing* find_thing(const std::vector<logic::Thing>& things, std::string_view id) {
    for (const logic::Thing& t : things)
        if (t.id == id) return &t;
    return nullptr;
}

} // namespace

void LinkOverlay::bind(Rml::DataModelConstructor& c) {
    if (auto s = c.RegisterStruct<MarkView>()) {
        s.RegisterMember("id", &MarkView::id);
        s.RegisterMember("name", &MarkView::name);
        s.RegisterMember("x", &MarkView::x);
        s.RegisterMember("y", &MarkView::y);
        s.RegisterMember("w", &MarkView::w);
        s.RegisterMember("h", &MarkView::h);
        s.RegisterMember("picked", &MarkView::picked);
        s.RegisterMember("hero", &MarkView::hero);
        s.RegisterMember("linked", &MarkView::linked);
    }
    c.RegisterArray<std::vector<MarkView>>();
    if (auto s = c.RegisterStruct<ArrowView>()) {
        s.RegisterMember("id", &ArrowView::id);
        s.RegisterMember("verb", &ArrowView::verb);
        s.RegisterMember("icon", &ArrowView::icon);
        s.RegisterMember("phrase", &ArrowView::phrase);
        s.RegisterMember("lx", &ArrowView::lx);
        s.RegisterMember("ly", &ArrowView::ly);
        s.RegisterMember("len", &ArrowView::len);
        s.RegisterMember("angle", &ArrowView::angle);
        s.RegisterMember("hx", &ArrowView::hx);
        s.RegisterMember("hy", &ArrowView::hy);
        s.RegisterMember("cx", &ArrowView::cx);
        s.RegisterMember("cy", &ArrowView::cy);
        s.RegisterMember("lit", &ArrowView::lit);
    }
    c.RegisterArray<std::vector<ArrowView>>();
    if (auto s = c.RegisterStruct<RowView>()) {
        s.RegisterMember("id", &RowView::id);
        s.RegisterMember("phrase", &RowView::phrase);
        s.RegisterMember("icon", &RowView::icon);
        s.RegisterMember("lit", &RowView::lit);
        s.RegisterMember("here", &RowView::here);
    }
    c.RegisterArray<std::vector<RowView>>();
    if (auto s = c.RegisterStruct<ChoiceView>()) {
        s.RegisterMember("phrase", &ChoiceView::phrase);
        s.RegisterMember("about", &ChoiceView::about);
        s.RegisterMember("icon", &ChoiceView::icon);
    }
    c.RegisterArray<std::vector<ChoiceView>>();
    c.Bind("ln_on", &m_on_);
    c.Bind("ln_allowed", &m_allowed_);
    c.Bind("ln_marks", &m_marks_);
    c.Bind("ln_arrows", &m_arrows_);
    c.Bind("ln_rows", &m_rows_);
    c.Bind("ln_choices", &m_choices_);
    c.Bind("ln_picking", &m_picking_);
    c.Bind("ln_first", &m_first_);
    c.Bind("ln_title", &m_title_);
    c.Bind("ln_tip", &m_tip_);
    c.Bind("ln_pick_x", &m_pick_x_);
    c.Bind("ln_pick_y", &m_pick_y_);
    c.BindEventCallback("ln_mark", [this](Rml::DataModelHandle, Rml::Event& e, const Rml::VariantList& a) {
        if (!a.empty()) click(a[0].Get<Rml::String>());
        e.StopPropagation();
    });
    c.BindEventCallback("ln_choose", [this](Rml::DataModelHandle, Rml::Event& e, const Rml::VariantList& a) {
        if (!a.empty()) choose(static_cast<usize>(a[0].Get<int>()));
        e.StopPropagation();
    });
    c.BindEventCallback("ln_cancel", [this](Rml::DataModelHandle, Rml::Event& e, const Rml::VariantList&) {
        cancel();
        e.StopPropagation();
    });
    c.BindEventCallback("ln_remove", [this](Rml::DataModelHandle, Rml::Event& e, const Rml::VariantList& a) {
        if (!a.empty() && on_remove) on_remove(static_cast<u32>(a[0].Get<int>()));
        e.StopPropagation();
    });
    c.BindEventCallback("ln_close", [this](Rml::DataModelHandle, Rml::Event& e, const Rml::VariantList&) {
        show(false);
        e.StopPropagation();
    });
}

void LinkOverlay::show(bool on) {
    on_ = on && allowed_;
    if (!on_) cancel();
}

void LinkOverlay::cancel() {
    pick_a_.clear();
    pick_b_.clear();
    options_.clear();
    set(m_choices_, {}, "ln_choices");
    set(m_picking_, false, "ln_picking");
}

void LinkOverlay::click(const std::string& id) {
    if (!on_) return;
    if (pick_a_.empty() || picking()) {
        cancel();
        pick_a_ = id;
        return;
    }
    if (id == pick_a_) { // the same again: let go of it
        cancel();
        return;
    }
    pick_b_ = id;
    open_choices();
}

// What the first can do with the second, and the second with the first.
void LinkOverlay::open_choices() {
    options_.clear();
    std::vector<ChoiceView> views;
    const logic::Thing* ta = things_ ? find_thing(*things_, pick_a_) : nullptr;
    const logic::Thing* tb = things_ ? find_thing(*things_, pick_b_) : nullptr;
    if (ta && tb && verbs_)
        for (int reversed = 0; reversed < 2; ++reversed) {
            const logic::Thing& x = reversed ? *tb : *ta;
            const logic::Thing& y = reversed ? *ta : *tb;
            for (const logic::VerbDef& v : verbs_->all()) {
                if (!logic::suits(v, x, y)) continue;
                const logic::Link l{0, x.id, v.id, y.id};
                options_.push_back({x.id, v.id, y.id});
                views.push_back({logic::phrase(l, v, x, y), logic::fill(v.about, x, y), v.icon.empty() ? "link" : v.icon});
            }
        }
    set(m_title_, Rml::String(ta && tb ? "Что «" + ta->name + "» делает с «" + tb->name + "»?" : "Связать"), "ln_title");
    set(m_choices_, views, "ln_choices");
    set(m_picking_, true, "ln_picking");
}

bool LinkOverlay::choose(usize i) {
    if (i >= options_.size()) return false;
    logic::Link l;
    l.a = options_[i].a;
    l.verb = options_[i].verb;
    l.b = options_[i].b;
    // As on the editor's board: the usual sound, and a hint when the hero
    // lacks what the link needs.
    if (const logic::VerbDef* v = verbs_ ? verbs_->find(l.verb) : nullptr) {
        l.sound = !v->sound.empty();
        l.hint = !v->fail.empty() && !v->needs.empty();
    }
    cancel();
    if (on_add) on_add(l);
    return true;
}

void LinkOverlay::update(const std::vector<Seen>& seen, const forge::render::Camera2D& camera, u32 width, u32 height,
                         const logic::Logic& logic, const logic::Verbs& verbs, const std::vector<logic::Thing>& things,
                         const std::function<bool(u32)>& lit) {
    verbs_ = &verbs;
    things_ = &things;
    width_ = width;
    height_ = height;
    set(m_allowed_, allowed_, "ln_allowed");
    set(m_on_, on_, "ln_on");
    if (!on_) return; // the marks stay as they were, hidden
    const f64 cx = camera.snapped_x(), cy = camera.snapped_y(), zoom = camera.zoom;
    auto sx = [&](f64 x) { return static_cast<float>((x - cx) * zoom + width * 0.5); };
    auto sy = [&](f64 y) { return static_cast<float>((y - cy) * zoom + height * 0.5); };

    std::map<std::string, const Seen*> at;
    for (const Seen& s : seen) at[s.id] = &s;
    std::map<std::string, int> links_of;
    for (const logic::Link& l : logic.links) ++links_of[l.a], ++links_of[l.b];

    std::vector<MarkView> marks;
    for (const Seen& s : seen) {
        const logic::Thing* t = find_thing(things, s.id);
        if (!t) continue;
        MarkView m;
        m.id = s.id;
        m.name = t->name;
        m.x = sx(s.x);
        m.y = sy(s.y);
        m.w = std::max(28.0f, static_cast<float>(s.half_w * 2 * zoom) + 14);
        m.h = std::max(28.0f, static_cast<float>(s.half_h * 2 * zoom) + 14);
        m.picked = s.id == pick_a_ || s.id == pick_b_;
        m.hero = s.id == logic::kHero;
        m.linked = links_of.contains(s.id);
        marks.push_back(m);
    }
    // A thing picked but gone from the screen: start over.
    if (!pick_a_.empty() && !at.contains(pick_a_) && !picking()) cancel();

    // The arrows between things on screen; a pair's arrows side by side.
    std::map<std::pair<std::string, std::string>, std::vector<u32>> pairs;
    for (const logic::Link& l : logic.links)
        if (at.contains(l.a) && at.contains(l.b) && l.a != l.b) pairs[std::minmax(l.a, l.b)].push_back(l.id);
    std::vector<ArrowView> arrows;
    std::vector<RowView> rows;
    for (const logic::Link& l : logic.links) {
        const logic::VerbDef* v = verbs.find(l.verb);
        const logic::Thing* ta = find_thing(things, l.a);
        const logic::Thing* tb = find_thing(things, l.b);
        RowView r;
        r.id = static_cast<int>(l.id);
        r.phrase = v && ta && tb ? logic::phrase(l, *v, *ta, *tb) : l.a + " " + l.verb + " " + l.b;
        r.icon = v && !v->icon.empty() ? v->icon : "link";
        r.lit = lit && lit(l.id);
        r.here = at.contains(l.a) && at.contains(l.b);
        rows.push_back(r);
        if (!r.here || l.a == l.b) continue;
        const Seen& a = *at[l.a];
        const Seen& b = *at[l.b];
        ArrowView w;
        w.id = r.id;
        w.verb = v ? (ta && ta->plural ? v->plural : v->name) : l.verb;
        w.icon = r.icon;
        w.phrase = r.phrase;
        w.lit = r.lit;
        float ax = sx(a.x), ay = sy(a.y), bx = sx(b.x), by = sy(b.y);
        float dx = bx - ax, dy = by - ay;
        const float dist = std::max(1.0f, std::hypot(dx, dy));
        dx /= dist;
        dy /= dist;
        const auto& same = pairs[std::minmax(l.a, l.b)];
        const usize k = static_cast<usize>(std::find(same.begin(), same.end(), l.id) - same.begin());
        const float sign = l.a < l.b ? 1.0f : -1.0f;
        const float off = (static_cast<float>(k) - static_cast<float>(same.size() - 1) * 0.5f) * kLinkGap;
        ax += -dy * sign * off, ay += dx * sign * off;
        bx += -dy * sign * off, by += dx * sign * off;
        // From the ring of one thing to the ring of the other.
        auto cut = [&](const Seen& s) {
            const float hw = std::max(14.0f, static_cast<float>(s.half_w * zoom) + 7);
            const float hh = std::max(14.0f, static_cast<float>(s.half_h * zoom) + 7);
            const float tx = std::fabs(dx) > 1e-4f ? hw / std::fabs(dx) : 1e9f;
            const float ty = std::fabs(dy) > 1e-4f ? hh / std::fabs(dy) : 1e9f;
            return std::min(tx, ty);
        };
        const float ca = cut(a), cb = cut(b) + 10;
        const float x0 = ax + dx * ca, y0 = ay + dy * ca;
        const float x1 = bx - dx * cb, y1 = by - dy * cb;
        w.lx = x0;
        w.ly = y0;
        w.len = std::max(0.0f, (x1 - x0) * dx + (y1 - y0) * dy);
        w.angle = std::atan2(dy, dx) * 57.29578f;
        w.hx = x1;
        w.hy = y1;
        w.cx = (x0 + x1) * 0.5f;
        w.cy = (y0 + y1) * 0.5f;
        arrows.push_back(w);
    }

    // The choices next to the second thing, on the screen.
    if (picking())
        if (const auto it = at.find(pick_b_); it != at.end()) {
            const float h = 64.0f + static_cast<float>(std::max<usize>(1, m_choices_.size())) * kChoiceH;
            const float bx = sx(it->second->x), by = sy(it->second->y);
            float x = bx + 40;
            if (x + kPickW > width - kMargin) x = bx - 40 - kPickW;
            x = std::clamp(x, kMargin, std::max(kMargin, width - kPickW - kMargin));
            const float y = std::clamp(by - h * 0.5f, kMargin + 40, std::max(kMargin + 40, height - h - 110));
            set(m_pick_x_, x, "ln_pick_x");
            set(m_pick_y_, y, "ln_pick_y");
        }

    std::string tip;
    if (picking()) tip = "Выберите, что будет происходить, или нажмите «Отмена»";
    else if (!pick_a_.empty()) {
        const logic::Thing* t = find_thing(things, pick_a_);
        tip = "Теперь нажмите на вторую вещь — что «" + (t ? t->name : pick_a_) + "» будет с ней делать?";
    } else tip = "Нажмите на вещь, потом на другую — и выберите, что первая делает со второй";
    set(m_tip_, Rml::String(tip), "ln_tip");
    set(m_first_, !pick_a_.empty() && !picking(), "ln_first");
    set(m_marks_, marks, "ln_marks");
    set(m_arrows_, arrows, "ln_arrows");
    set(m_rows_, rows, "ln_rows");
}

const std::vector<forge::modules::Event>& verb_events() {
    static const std::vector<forge::modules::Event> events = {
        {"touch", "касается: герой касается вещи (в зону входит)"},
        {"always", "всегда: всё время, пока первая вещь есть"},
    };
    return events;
}

std::vector<std::string> verb_event_ids() {
    std::vector<std::string> out;
    for (const forge::modules::Event& e : verb_events()) out.push_back(e.id);
    return out;
}

} // namespace slice
