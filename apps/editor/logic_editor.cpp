#include "logic_editor.h"

#include "forge/core/log.h"
#include "forge/core/path.h"

#include <algorithm>
#include <cmath>

namespace forge::editor_app {

namespace {

// A thing on the board: its card's size in pixels.
constexpr f32 kCardW = 112, kCardH = 112;
constexpr f32 kLinkGap = 30; // between links of the same two things
constexpr u32 kHeroIconPx = 96;

int arg_int(const Rml::VariantList& a, usize i, int fallback) { return i < a.size() ? a[i].Get<int>() : fallback; }
Rml::String arg_str(const Rml::VariantList& a, usize i) { return i < a.size() ? a[i].Get<Rml::String>() : Rml::String(); }

bool fits(std::string_view rule, std::string_view id) {
    if (rule == "hero") return id == logic::kHero;
    if (rule == "thing") return id != logic::kHero;
    return true;
}

} // namespace

// Puts the links back as they were: the whole logic before and after, as
// its JSON (a few kilobytes even for a big game).
class LogicCommand final : public editor::Command {
public:
    LogicCommand(LogicEditor& ed, std::string before, std::string after, std::string label, std::string merge)
        : ed_(ed), before_(std::move(before)), after_(std::move(after)), label_(std::move(label)), merge_(std::move(merge)) {}
    void apply(editor::Document&) override { ed_.apply_json(after_); }
    void revert(editor::Document&) override { ed_.apply_json(before_); }
    std::string label() const override { return label_; }
    std::string merge_key() const override { return merge_; }
    bool try_merge(const editor::Command& next) override {
        const auto* n = static_cast<const LogicCommand*>(&next);
        if (merge_.empty() || n->merge_ != merge_) return false;
        after_ = n->after_;
        return true;
    }

private:
    LogicEditor& ed_;
    std::string before_, after_, label_, merge_;
};

LogicEditor::LogicEditor(level::LevelModule& module) : module_(module) {}

bool LogicEditor::init(ui::Ui& ui, const std::filesystem::path& game_dir, const std::filesystem::path& file) {
    ui_ = &ui;
    game_dir_ = game_dir;
    file_ = file;
    std::vector<u8> rgba;
    module_.hero_icon(kHeroIconPx, rgba);
    ui.set_image("logic_hero", rgba.data(), kHeroIconPx, kHeroIconPx);
    hero_icon_ = "/memory/logic_hero";
    load();
    return true;
}

void LogicEditor::load() {
    std::string error;
    if (!verbs_.load(game_dir_ / "verbs.json", &error)) FORGE_ERROR("Связи: %s", error.c_str());
    if (!logic_.load(file_, &error)) FORGE_ERROR("Связи: %s", error.c_str());
    // A game without links starts with the hero on the board.
    if (logic_.board.empty() && logic_.links.empty()) logic_.set_spot(logic::kHero, 60, 60);
    dirty_ = true;
}

void LogicEditor::bind(Rml::DataModelConstructor& model) {
    if (auto s = model.RegisterStruct<ThingView>()) {
        s.RegisterMember("id", &ThingView::id);
        s.RegisterMember("name", &ThingView::name);
        s.RegisterMember("icon", &ThingView::icon);
        s.RegisterMember("x", &ThingView::x);
        s.RegisterMember("y", &ThingView::y);
        s.RegisterMember("links", &ThingView::links);
        s.RegisterMember("selected", &ThingView::selected);
        s.RegisterMember("broken", &ThingView::broken);
    }
    model.RegisterArray<std::vector<ThingView>>();
    if (auto s = model.RegisterStruct<LinkView>()) {
        s.RegisterMember("id", &LinkView::id);
        s.RegisterMember("verb", &LinkView::verb);
        s.RegisterMember("phrase", &LinkView::phrase);
        s.RegisterMember("icon", &LinkView::icon);
        s.RegisterMember("lx", &LinkView::lx);
        s.RegisterMember("ly", &LinkView::ly);
        s.RegisterMember("len", &LinkView::len);
        s.RegisterMember("angle", &LinkView::angle);
        s.RegisterMember("hx", &LinkView::hx);
        s.RegisterMember("hy", &LinkView::hy);
        s.RegisterMember("cx", &LinkView::cx);
        s.RegisterMember("cy", &LinkView::cy);
        s.RegisterMember("refined", &LinkView::refined);
        s.RegisterMember("selected", &LinkView::selected);
        s.RegisterMember("broken", &LinkView::broken);
    }
    model.RegisterArray<std::vector<LinkView>>();
    if (auto s = model.RegisterStruct<NavRow>()) {
        s.RegisterMember("id", &NavRow::id);
        s.RegisterMember("name", &NavRow::name);
        s.RegisterMember("icon", &NavRow::icon);
        s.RegisterMember("about", &NavRow::about);
        s.RegisterMember("links", &NavRow::links);
        s.RegisterMember("selected", &NavRow::selected);
    }
    model.RegisterArray<std::vector<NavRow>>();
    if (auto s = model.RegisterStruct<VerbOption>()) {
        s.RegisterMember("verb", &VerbOption::verb);
        s.RegisterMember("phrase", &VerbOption::phrase);
        s.RegisterMember("about", &VerbOption::about);
        s.RegisterMember("icon", &VerbOption::icon);
        s.RegisterMember("reversed", &VerbOption::reversed);
    }
    model.RegisterArray<std::vector<VerbOption>>();
    if (auto s = model.RegisterStruct<Refinement>()) {
        s.RegisterMember("id", &Refinement::id);
        s.RegisterMember("label", &Refinement::label);
        s.RegisterMember("hint", &Refinement::hint);
        s.RegisterMember("on", &Refinement::on);
    }
    model.RegisterArray<std::vector<Refinement>>();
    if (auto s = model.RegisterStruct<WordRow>()) {
        s.RegisterMember("id", &WordRow::id);
        s.RegisterMember("phrase", &WordRow::phrase);
        s.RegisterMember("meaning", &WordRow::meaning);
        s.RegisterMember("problem", &WordRow::problem);
        s.RegisterMember("selected", &WordRow::selected);
    }
    model.RegisterArray<std::vector<WordRow>>();

    model.Bind("lg_things", &m_things_);
    model.Bind("lg_links", &m_links_);
    model.Bind("lg_board_rows", &m_board_rows_);
    model.Bind("lg_add_rows", &m_add_rows_);
    model.Bind("lg_verbs", &m_verbs_);
    model.Bind("lg_refine", &m_refine_);
    model.Bind("lg_words", &m_words_);
    model.Bind("lg_thing_links", &m_thing_links_);
    model.Bind("lg_pick_title", &m_pick_title_);
    model.Bind("lg_pick_x", &m_pick_x_);
    model.Bind("lg_pick_y", &m_pick_y_);
    model.Bind("lg_picking", &m_picking_);
    model.Bind("lg_has_link", &m_has_link_);
    model.Bind("lg_has_thing", &m_has_thing_);
    model.Bind("lg_hint", &m_hint_);
    model.Bind("lg_sel_phrase", &m_sel_phrase_);
    model.Bind("lg_sel_meaning", &m_sel_meaning_);
    model.Bind("lg_sel_problem", &m_sel_problem_);
    model.Bind("lg_sel_name", &m_sel_name_);
    model.Bind("lg_sel_icon", &m_sel_icon_);
    model.Bind("lg_sel_links", &m_sel_links_);
    model.Bind("lg_count", &m_count_);

    auto on = [&model](const char* name, auto fn) {
        model.BindEventCallback(name, [fn](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& a) { fn(ev, a); });
    };
    on("lg_thing_down", [this](Rml::Event& ev, const Rml::VariantList& a) {
        const int i = arg_int(a, 0, -1);
        if (i < 0 || i >= static_cast<int>(m_things_.size())) return;
        ev.StopPropagation();
        grabbing_ = true;
        dragged_ = false;
        grab_id_ = m_things_[static_cast<usize>(i)].id;
        grab_mx_ = ev.GetParameter<float>("mouse_x", 0);
        grab_my_ = ev.GetParameter<float>("mouse_y", 0);
        const logic::Spot* s = logic_.spot(grab_id_);
        grab_x_ = s ? s->x : 0;
        grab_y_ = s ? s->y : 0;
    });
    on("lg_board_down", [this](Rml::Event& ev, const Rml::VariantList&) {
        if (ev.GetParameter<int>("button", 0) != 0) return;
        grabbing_ = true;
        dragged_ = false;
        grab_id_.clear();
        grab_mx_ = ev.GetParameter<float>("mouse_x", 0);
        grab_my_ = ev.GetParameter<float>("mouse_y", 0);
        grab_x_ = pan_x_;
        grab_y_ = pan_y_;
    });
    on("lg_link", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        select_link(static_cast<u32>(arg_int(a, 0, 0)));
    });
    on("lg_stop", [](Rml::Event& ev, const Rml::VariantList&) { ev.StopPropagation(); });
    on("lg_pick", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        const int i = arg_int(a, 0, -1);
        if (i >= 0) pick(static_cast<usize>(i));
    });
    on("lg_pick_cancel", [this](Rml::Event& ev, const Rml::VariantList&) {
        ev.StopPropagation();
        close_picker();
    });
    on("lg_add", [this](Rml::Event&, const Rml::VariantList& a) { add_thing(arg_str(a, 0)); });
    on("lg_select_thing", [this](Rml::Event&, const Rml::VariantList& a) { select_thing(arg_str(a, 0)); });
    on("lg_remove_thing", [this](Rml::Event&, const Rml::VariantList&) { remove_thing(sel_thing_); });
    on("lg_remove_link", [this](Rml::Event&, const Rml::VariantList&) { remove_link(); });
    on("lg_refine", [this](Rml::Event&, const Rml::VariantList& a) {
        const std::string what = arg_str(a, 0);
        const logic::Link* l = logic_.find(sel_link_);
        if (!l) return;
        const bool now = what == "night" ? l->night : what == "once" ? l->once : what == "sound" ? l->sound : l->hint;
        refine(what, !now);
    });
    on("lg_hint_ok", [this](Rml::Event&, const Rml::VariantList&) {
        hint_closed_ = true;
        set(m_hint_, false, "lg_hint");
    });
}

// --- view --------------------------------------------------------------------

const logic::Thing* LogicEditor::thing(std::string_view id) const {
    for (const logic::Thing& t : things_)
        if (t.id == id) return &t;
    return nullptr;
}

std::string LogicEditor::icon_of(const std::string& id) {
    if (id == logic::kHero) return hero_icon_;
    const objects::Template* t = module_.library() ? module_.library()->find(id) : nullptr;
    return t && template_icon ? template_icon(*t) : std::string();
}

// A place where the card does not cover another one: along rows to the right
// of the things already there.
void LogicEditor::place_free(logic::Logic& l, const std::string& id) const {
    if (l.spot(id)) return;
    auto taken = [&](f32 x, f32 y) {
        for (const logic::Spot& s : l.board)
            if (std::fabs(s.x - x) < kCardW + 40 && std::fabs(s.y - y) < kCardH + 40) return true;
        return false;
    };
    for (int row = 0; row < 64; ++row)
        for (int col = 0; col < 6; ++col) {
            const f32 x = 60 + static_cast<f32>(col) * (kCardW + 120), y = 60 + static_cast<f32>(row) * (kCardH + 80);
            if (!taken(x, y)) {
                l.set_spot(id, x, y);
                return;
            }
        }
    l.set_spot(id, 60, 60);
}

void LogicEditor::rebuild() {
    dirty_ = false;
    objects::Library* lib = module_.library();
    built_lib_ = lib ? lib->version() : 0;
    things_.clear();
    things_.push_back(logic::hero_thing());
    if (lib)
        for (const objects::Template& t : lib->templates()) things_.push_back(logic::thing_of(*lib, t));
    const logic::FindThing find = [this](std::string_view id) { return thing(id); };
    problems_.clear();
    for (const logic::Problem& p : logic::compile(logic_, verbs_, find).problems) problems_[p.link] = p.text;
    // Every linked thing is on the board.
    for (const logic::Link& l : logic_.links) {
        place_free(logic_, l.a);
        place_free(logic_, l.b);
    }
    if (logic_.find(sel_link_) == nullptr) sel_link_ = 0;
    if (!sel_thing_.empty() && !logic_.spot(sel_thing_)) sel_thing_.clear();

    // Things.
    std::map<std::string, int> counts;
    for (const logic::Link& l : logic_.links) ++counts[l.a], ++counts[l.b];
    m_things_.clear();
    for (const logic::Spot& s : logic_.board) {
        const logic::Thing* t = thing(s.thing);
        ThingView v;
        v.id = s.thing;
        v.name = t ? t->name : s.thing;
        v.icon = icon_of(s.thing);
        v.x = s.x + pan_x_;
        v.y = s.y + pan_y_;
        v.links = counts[s.thing];
        v.selected = s.thing == sel_thing_ || s.thing == pick_a_;
        v.broken = !t;
        m_things_.push_back(v);
    }

    // Links: arrows from card to card; several between the same two things
    // lie side by side.
    std::map<std::pair<std::string, std::string>, std::vector<u32>> pairs;
    for (const logic::Link& l : logic_.links) pairs[std::minmax(l.a, l.b)].push_back(l.id);
    m_links_.clear();
    for (const logic::Link& l : logic_.links) {
        const logic::Spot* sa = logic_.spot(l.a);
        const logic::Spot* sb = logic_.spot(l.b);
        if (!sa || !sb) continue;
        LinkView v;
        v.id = static_cast<int>(l.id);
        const logic::VerbDef* verb = verbs_.find(l.verb);
        const logic::Thing* ta = thing(l.a);
        const logic::Thing* tb = thing(l.b);
        v.verb = verb ? (ta && ta->plural ? verb->plural : verb->name) : l.verb;
        v.icon = verb && !verb->icon.empty() ? verb->icon : "link";
        v.phrase = phrase_of(l.id);
        v.refined = l.night + l.once + l.sound + l.hint;
        v.selected = l.id == sel_link_;
        v.broken = problems_.contains(l.id);
        (void)tb;
        f32 ax = sa->x + pan_x_ + kCardW / 2, ay = sa->y + pan_y_ + kCardH / 2;
        f32 bx = sb->x + pan_x_ + kCardW / 2, by = sb->y + pan_y_ + kCardH / 2;
        f32 dx = bx - ax, dy = by - ay;
        const f32 dist = std::max(1.0f, std::hypot(dx, dy));
        dx /= dist;
        dy /= dist;
        // Side by side: the normal of the pair's own direction (the same for
        // A→B and B→A).
        const auto& same = pairs[std::minmax(l.a, l.b)];
        const usize k = static_cast<usize>(std::find(same.begin(), same.end(), l.id) - same.begin());
        const f32 sign = l.a < l.b ? 1.0f : -1.0f;
        const f32 off = (static_cast<f32>(k) - static_cast<f32>(same.size() - 1) * 0.5f) * kLinkGap;
        ax += -dy * sign * off, ay += dx * sign * off;
        bx += -dy * sign * off, by += dx * sign * off;
        // From the edge of one card to the edge of the other.
        auto edge = [&](f32 ux, f32 uy) {
            const f32 tx = std::fabs(ux) > 1e-4f ? (kCardW / 2 + 6) / std::fabs(ux) : 1e9f;
            const f32 ty = std::fabs(uy) > 1e-4f ? (kCardH / 2 + 6) / std::fabs(uy) : 1e9f;
            return std::min(tx, ty);
        };
        const f32 cut = edge(dx, dy);
        const f32 sx = ax + dx * cut, sy = ay + dy * cut;
        const f32 ex = bx - dx * (cut + 8), ey = by - dy * (cut + 8);
        v.lx = sx;
        v.ly = sy;
        v.len = std::max(0.0f, std::hypot(ex - sx, ey - sy));
        v.angle = std::atan2(dy, dx) * 57.29578f;
        v.hx = ex;
        v.hy = ey;
        v.cx = (sx + ex) * 0.5f;
        v.cy = (sy + ey) * 0.5f;
        m_links_.push_back(v);
    }

    // The left column: what is on the board, what can be added.
    m_board_rows_.clear();
    m_add_rows_.clear();
    for (const logic::Thing& t : things_) {
        NavRow r{t.id, t.name, icon_of(t.id), {}, counts[t.id], t.id == sel_thing_};
        if (t.id != logic::kHero)
            if (const objects::Template* tpl = lib ? lib->find(t.id) : nullptr) r.about = tpl->about;
        (logic_.spot(t.id) ? m_board_rows_ : m_add_rows_).push_back(r);
    }
    m_count_ = "Связей: " + std::to_string(logic_.links.size()) + ", вещей на доске: " + std::to_string(logic_.board.size());
    m_hint_ = logic_.links.empty() && !hint_closed_;
    rebuild_side();
    if (model_)
        for (const char* name : {"lg_things", "lg_links", "lg_board_rows", "lg_add_rows", "lg_count", "lg_hint"})
            model_.DirtyVariable(name);
}

void LogicEditor::rebuild_side() {
    const logic::Link* l = logic_.find(sel_link_);
    m_has_link_ = l != nullptr;
    m_has_thing_ = !l && !sel_thing_.empty();
    m_refine_.clear();
    m_sel_phrase_ = m_sel_meaning_ = m_sel_problem_ = "";
    if (l) {
        m_sel_phrase_ = phrase_of(l->id);
        m_sel_meaning_ = meaning_of(l->id);
        m_sel_problem_ = problem_of(l->id);
        const logic::VerbDef* v = verbs_.find(l->verb);
        const logic::Thing* ta = thing(l->a);
        const logic::Thing* tb = thing(l->b);
        m_refine_.push_back({"night", "Только ночью", "связь срабатывает, только когда в игре ночь", l->night});
        m_refine_.push_back({"once", "Только один раз", "у каждой копии вещи — один раз за игру", l->once});
        if (v && !v->sound.empty()) m_refine_.push_back({"sound", "Со звуком", "обычный звук игры для этого действия", l->sound});
        if (v && !v->fail.empty() && ta && tb && (!v->needs.empty() || l->night))
            m_refine_.push_back({"hint", "Подсказка, если не вышло", "«" + logic::fill(v->fail, *ta, *tb) + "»", l->hint});
    }
    // The thing: its picture and links.
    m_thing_links_.clear();
    m_sel_name_ = m_sel_icon_ = "";
    m_sel_links_ = 0;
    if (m_has_thing_) {
        const logic::Thing* t = thing(sel_thing_);
        m_sel_name_ = t ? t->name : sel_thing_;
        m_sel_icon_ = icon_of(sel_thing_);
        for (const logic::Link& k : logic_.links)
            if (k.a == sel_thing_ || k.b == sel_thing_)
                m_thing_links_.push_back({static_cast<int>(k.id), phrase_of(k.id), meaning_of(k.id), problem_of(k.id), false});
        m_sel_links_ = static_cast<int>(m_thing_links_.size());
    }
    // Everything in words.
    m_words_.clear();
    for (const logic::Link& k : logic_.links)
        m_words_.push_back({static_cast<int>(k.id), phrase_of(k.id), meaning_of(k.id), problem_of(k.id), k.id == sel_link_});
    if (model_)
        for (const char* name : {"lg_has_link", "lg_has_thing", "lg_refine", "lg_sel_phrase", "lg_sel_meaning", "lg_sel_problem",
                                 "lg_thing_links", "lg_sel_name", "lg_sel_icon", "lg_sel_links", "lg_words"})
            model_.DirtyVariable(name);
}

std::string LogicEditor::phrase_of(u32 link) const {
    const logic::Link* l = logic_.find(link);
    if (!l) return {};
    const logic::VerbDef* v = verbs_.find(l->verb);
    const logic::Thing* a = thing(l->a);
    const logic::Thing* b = thing(l->b);
    if (!v || !a || !b) return (a ? a->name : l->a) + " " + (v ? v->name : l->verb) + " " + (b ? b->name : l->b);
    return logic::phrase(*l, *v, *a, *b);
}

std::string LogicEditor::meaning_of(u32 link) const {
    const logic::Link* l = logic_.find(link);
    if (!l) return {};
    const logic::VerbDef* v = verbs_.find(l->verb);
    const logic::Thing* a = thing(l->a);
    const logic::Thing* b = thing(l->b);
    return v && a && b ? logic::meaning(*l, *v, *a, *b) : std::string();
}

std::string LogicEditor::problem_of(u32 link) const {
    const auto it = problems_.find(link);
    return it == problems_.end() ? std::string() : "Не работает: " + it->second + ".";
}

bool LogicEditor::thing_at(const std::string& id, f32& x, f32& y) const {
    const logic::Spot* s = logic_.spot(id);
    if (!s) return false;
    x = s->x + pan_x_ + kCardW / 2;
    y = s->y + pan_y_ + kCardH / 2;
    return true;
}

void LogicEditor::update(Rml::Context* context) {
    // The board's size, so the picker stays on it.
    for (int i = 0; context && i < context->GetNumDocuments(); ++i)
        if (Rml::Element* board = context->GetDocument(i)->GetElementById("lg-board")) {
            const f32 w = board->GetClientWidth(), h = board->GetClientHeight();
            if (w != board_w_ || h != board_h_) {
                board_w_ = w;
                board_h_ = h;
                if (picking()) place_picker();
            }
            break;
        }
    const objects::Library* lib = module_.library();
    if (lib && lib->version() != built_lib_) dirty_ = true;
    if (dirty_) rebuild();
}

// --- changes -----------------------------------------------------------------

void LogicEditor::save() {
    std::string error;
    if (!logic_.save(file_, &error)) FORGE_ERROR("Связи не сохранились: %s", error.c_str());
}

void LogicEditor::apply_json(const std::string& json) {
    logic::Logic l;
    std::string error;
    if (!l.parse(json, &error)) {
        FORGE_ERROR("Связи: %s", error.c_str());
        return;
    }
    logic_ = std::move(l);
    save();
    dirty_ = true;
}

void LogicEditor::change(const logic::Logic& after, std::string label, std::string merge) {
    history_.execute(std::make_unique<LogicCommand>(*this, logic_.json(), after.json(), std::move(label), std::move(merge)));
    rebuild();
}

bool LogicEditor::add_thing(const std::string& id) {
    if (id.empty() || logic_.spot(id) || (id != logic::kHero && !thing(id))) return false;
    logic::Logic after = logic_;
    place_free(after, id);
    const logic::Thing* t = thing(id);
    change(after, "На доску: «" + (t ? t->name : id) + "»");
    select_thing(id);
    return true;
}

bool LogicEditor::remove_thing(const std::string& id) {
    if (!logic_.spot(id)) return false;
    logic::Logic after = logic_;
    std::erase_if(after.links, [&](const logic::Link& l) { return l.a == id || l.b == id; });
    std::erase_if(after.board, [&](const logic::Spot& s) { return s.thing == id; });
    const logic::Thing* t = thing(id);
    sel_thing_.clear();
    close_picker();
    change(after, "С доски: «" + (t ? t->name : id) + "»");
    return true;
}

bool LogicEditor::move_thing(const std::string& id, f32 x, f32 y) {
    const logic::Spot* s = logic_.spot(id);
    if (!s) return false;
    logic::Logic after = logic_;
    after.set_spot(id, std::round(x), std::round(y));
    const logic::Thing* t = thing(id);
    change(after, "Подвинуть «" + (t ? t->name : id) + "»");
    return true;
}

void LogicEditor::select_thing(const std::string& id) {
    sel_thing_ = logic_.spot(id) ? id : std::string();
    sel_link_ = 0;
    rebuild();
}

void LogicEditor::select_link(u32 id) {
    sel_link_ = logic_.find(id) ? id : 0;
    sel_thing_.clear();
    close_picker();
    rebuild();
}

void LogicEditor::click_thing(const std::string& id) {
    if (picking()) close_picker();
    if (!sel_thing_.empty() && sel_thing_ != id) {
        open_picker(sel_thing_, id);
        return;
    }
    select_thing(id);
}

void LogicEditor::click_board() {
    close_picker();
    sel_thing_.clear();
    sel_link_ = 0;
    rebuild();
}

// What the first thing can do with the second (and, after them, what the
// second can do with the first).
void LogicEditor::open_picker(const std::string& a, const std::string& b) {
    const logic::Thing* ta = thing(a);
    const logic::Thing* tb = thing(b);
    m_verbs_.clear();
    if (ta && tb) {
        for (int reversed = 0; reversed < 2; ++reversed) {
            const logic::Thing& x = reversed ? *tb : *ta;
            const logic::Thing& y = reversed ? *ta : *tb;
            for (const logic::VerbDef& v : verbs_.all()) {
                if (!fits(v.a_is, x.id) || !fits(v.b_is, y.id)) continue;
                if (v.always && x.id == logic::kHero) continue;
                logic::Link l{0, x.id, v.id, y.id};
                m_verbs_.push_back({v.id, logic::phrase(l, v, x, y), logic::fill(v.about, x, y), v.icon.empty() ? "link" : v.icon,
                                    reversed != 0});
            }
        }
    }
    pick_a_ = a;
    pick_b_ = b;
    sel_thing_.clear();
    sel_link_ = 0;
    m_pick_title_ = ta && tb ? "Что «" + ta->name + "» делает с «" + tb->name + "»?" : "Связать";
    place_picker();
    m_picking_ = true;
    rebuild();
    if (model_)
        for (const char* name : {"lg_verbs", "lg_pick_title", "lg_pick_x", "lg_pick_y", "lg_picking"}) model_.DirtyVariable(name);
}

// Right of the second thing; left of it when the board ends there.
void LogicEditor::place_picker() {
    const logic::Spot* sb = logic_.spot(pick_b_);
    constexpr f32 kPickW = 340, kPickH = 460, kMargin = 8;
    f32 x = sb ? sb->x + pan_x_ + kCardW + 16 : kMargin, y = sb ? sb->y + pan_y_ : kMargin;
    if (board_w_ > 0 && x + kPickW > board_w_ - kMargin) {
        x = sb ? sb->x + pan_x_ - kPickW - 16 : kMargin;
        if (x < kMargin) x = std::max(kMargin, board_w_ - kPickW - kMargin);
    }
    if (board_h_ > 0) y = std::min(y, board_h_ - std::min(kPickH, board_h_) - kMargin);
    m_pick_x_ = x;
    m_pick_y_ = std::max(kMargin, y);
    if (model_) {
        model_.DirtyVariable("lg_pick_x");
        model_.DirtyVariable("lg_pick_y");
    }
}

void LogicEditor::close_picker() {
    if (pick_a_.empty() && !m_picking_) return;
    pick_a_.clear();
    pick_b_.clear();
    m_verbs_.clear();
    m_picking_ = false;
    dirty_ = true;
    if (model_)
        for (const char* name : {"lg_verbs", "lg_picking"}) model_.DirtyVariable(name);
}

bool LogicEditor::pick(usize i) {
    if (i >= m_verbs_.size() || pick_a_.empty()) return false;
    const VerbOption& o = m_verbs_[i];
    logic::Link l;
    l.a = o.reversed ? pick_b_ : pick_a_;
    l.b = o.reversed ? pick_a_ : pick_b_;
    l.verb = o.verb;
    // What most links want: the usual sound, and a hint when the hero lacks
    // what the link needs.
    if (const logic::VerbDef* v = verbs_.find(l.verb)) {
        l.sound = !v->sound.empty();
        l.hint = !v->fail.empty() && !v->needs.empty();
    }
    const std::string phrase = o.phrase;
    logic::Logic after = logic_;
    const u32 id = after.add(l);
    close_picker();
    change(after, "Связь: «" + phrase + "»");
    select_link(id);
    return true;
}

bool LogicEditor::refine(const std::string& what, bool on) {
    const logic::Link* l = logic_.find(sel_link_);
    if (!l) return false;
    logic::Logic after = logic_;
    logic::Link& m = *after.find(sel_link_);
    if (what == "night") m.night = on;
    else if (what == "once") m.once = on;
    else if (what == "sound") m.sound = on;
    else if (what == "hint") m.hint = on;
    else return false;
    const char* name = what == "night" ? "только ночью" : what == "once" ? "один раз" : what == "sound" ? "со звуком" : "подсказка";
    change(after, std::string(on ? "Уточнить: " : "Убрать: ") + name);
    return true;
}

bool LogicEditor::remove_link() {
    if (!logic_.find(sel_link_)) return false;
    logic::Logic after = logic_;
    const std::string phrase = phrase_of(sel_link_);
    after.remove(sel_link_);
    sel_link_ = 0;
    change(after, "Удалить связь «" + phrase + "»");
    return true;
}

// --- input -------------------------------------------------------------------

bool LogicEditor::handle_event(const SDL_Event& e, f32 density, bool) {
    if (!grabbing_) return false;
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        const f32 dx = e.motion.x * density - grab_mx_, dy = e.motion.y * density - grab_my_;
        if (!dragged_ && std::hypot(dx, dy) > 4) dragged_ = true;
        if (!dragged_) return true;
        if (grab_id_.empty()) {
            pan_x_ = grab_x_ + dx;
            pan_y_ = grab_y_ + dy;
        } else {
            // Live while dragging; one step of the history when let go.
            logic_.set_spot(grab_id_, grab_x_ + dx, grab_y_ + dy);
        }
        close_picker();
        rebuild();
        return true;
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
        grabbing_ = false;
        const std::string id = grab_id_;
        if (dragged_ && !id.empty()) {
            const logic::Spot* s = logic_.spot(id);
            const f32 x = s ? s->x : grab_x_, y = s ? s->y : grab_y_;
            logic_.set_spot(id, grab_x_, grab_y_);
            move_thing(id, x, y);
        } else if (!dragged_) {
            if (id.empty()) click_board();
            else click_thing(id);
        }
        dragged_ = false;
        return true;
    }
    return false;
}

bool LogicEditor::handle_key(const SDL_KeyboardEvent& k) {
    if (k.key == SDLK_ESCAPE) {
        if (picking()) close_picker();
        else if (sel_link_ || !sel_thing_.empty()) click_board();
        else return false;
        rebuild();
        return true;
    }
    if (k.key == SDLK_DELETE) {
        if (sel_link_) return remove_link();
        if (!sel_thing_.empty()) return remove_thing(sel_thing_);
    }
    return false;
}

void LogicEditor::undo() {
    if (history_.undo()) FORGE_INFO("Отменено");
    rebuild();
}

void LogicEditor::redo() {
    if (history_.redo()) FORGE_INFO("Повторено");
    rebuild();
}

std::string LogicEditor::status() const {
    std::string s = m_count_ + " · " + path_to_utf8(file_.filename());
    if (picking()) s += " · выберите, что делает первая вещь со второй (Esc — отмена)";
    else if (!sel_thing_.empty()) s += " · нажмите на другую вещь, чтобы связать их";
    return s;
}

} // namespace forge::editor_app
