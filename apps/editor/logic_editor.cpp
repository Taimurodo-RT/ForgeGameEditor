#include "logic_editor.h"

#include "forge/core/file.h"
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

const char* part_label(std::string_view part) {
    if (part == "when") return "Когда";
    if (part == "if") return "Если";
    if (part == "then") return "Тогда";
    return "Иначе";
}

bool known_mode(std::string_view m) {
    return m == "links" || m == "ideas" || m == "steps" || m == "scheme" || m == "code";
}

constexpr u64 kLitMs = 1500; // how long a link that happened stays lit

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
    if (remember_) {
        std::vector<u8> bytes;
        if (read_file(settings_ / "logic_mode.txt", bytes)) {
            const std::string m(bytes.begin(), bytes.end());
            if (known_mode(m)) mode_ = m_mode_ = m;
        }
    }
    load();
    return true;
}

void LogicEditor::remember_mode() const {
    if (!remember_ || settings_.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(settings_, ec);
    write_file_atomic(settings_ / "logic_mode.txt", {reinterpret_cast<const u8*>(mode_.data()), mode_.size()});
}

bool LogicEditor::set_mode(const std::string& mode) {
    if (!known_mode(mode)) return false;
    if (mode == mode_) return true;
    close_picker();
    mode_ = m_mode_ = mode;
    adding_ = 0;
    if (mode != "code") cancel_code();
    m_gallery_ = m_choosing_ = false;
    choose_link_ = 0;
    if (model_)
        for (const char* name : {"lg_gallery", "lg_choosing"}) model_.DirtyVariable(name);
    remember_mode();
    if (model_) model_.DirtyVariable("lg_mode");
    rebuild();
    return true;
}

void LogicEditor::load() {
    std::string error;
    if (!verbs_.load(game_dir_ / "verbs.json", &error)) FORGE_ERROR("Связи: %s", error.c_str());
    if (!logic_.load(file_, &error)) FORGE_ERROR("Связи: %s", error.c_str());
    if (!ideas_.load(game_dir_ / "ideas.json", &error)) FORGE_ERROR("Идеи: %s", error.c_str());
    // A game without links starts with the hero on the board.
    if (logic_.board.empty() && logic_.links.empty()) logic_.set_spot(logic::kHero, 60, 60);
    std::error_code ec;
    file_time_ = std::filesystem::last_write_time(file_, ec);
    dirty_ = true;
}

void LogicEditor::bind(Rml::DataModelConstructor& model) {
    scheme_.bind(model);
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
        s.RegisterMember("lit", &LinkView::lit);
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
        s.RegisterMember("lit", &WordRow::lit);
    }
    model.RegisterArray<std::vector<WordRow>>();
    if (auto s = model.RegisterStruct<StepView>()) {
        s.RegisterMember("part", &StepView::part);
        s.RegisterMember("label", &StepView::label);
        s.RegisterMember("icon", &StepView::icon);
        s.RegisterMember("text", &StepView::text);
        s.RegisterMember("refine", &StepView::refine);
    }
    model.RegisterArray<std::vector<StepView>>();
    if (auto s = model.RegisterStruct<AddView>()) {
        s.RegisterMember("id", &AddView::id);
        s.RegisterMember("label", &AddView::label);
        s.RegisterMember("about", &AddView::about);
    }
    model.RegisterArray<std::vector<AddView>>();
    if (auto s = model.RegisterStruct<CardView>()) {
        s.RegisterMember("id", &CardView::id);
        s.RegisterMember("phrase", &CardView::phrase);
        s.RegisterMember("meaning", &CardView::meaning);
        s.RegisterMember("problem", &CardView::problem);
        s.RegisterMember("selected", &CardView::selected);
        s.RegisterMember("adding", &CardView::adding);
        s.RegisterMember("lit", &CardView::lit);
        s.RegisterMember("steps", &CardView::steps);
        s.RegisterMember("adds", &CardView::adds);
    }
    model.RegisterArray<std::vector<CardView>>();
    if (auto s = model.RegisterStruct<CodeLine>()) {
        s.RegisterMember("n", &CodeLine::n);
        s.RegisterMember("link", &CodeLine::link);
        s.RegisterMember("text", &CodeLine::text);
        s.RegisterMember("selected", &CodeLine::selected);
        s.RegisterMember("comment", &CodeLine::comment);
        s.RegisterMember("lit", &CodeLine::lit);
    }
    model.RegisterArray<std::vector<CodeLine>>();
    if (auto s = model.RegisterStruct<FieldView>()) {
        s.RegisterMember("side", &FieldView::side);
        s.RegisterMember("label", &FieldView::label);
        s.RegisterMember("thing", &FieldView::thing);
        s.RegisterMember("name", &FieldView::name);
        s.RegisterMember("icon", &FieldView::icon);
    }
    model.RegisterArray<std::vector<FieldView>>();
    if (auto s = model.RegisterStruct<IdeaCard>()) {
        s.RegisterMember("id", &IdeaCard::id);
        s.RegisterMember("name", &IdeaCard::name);
        s.RegisterMember("icon", &IdeaCard::icon);
        s.RegisterMember("phrase", &IdeaCard::phrase);
        s.RegisterMember("problem", &IdeaCard::problem);
        s.RegisterMember("selected", &IdeaCard::selected);
        s.RegisterMember("lit", &IdeaCard::lit);
        s.RegisterMember("fields", &IdeaCard::fields);
        s.RegisterMember("refine", &IdeaCard::refine);
    }
    model.RegisterArray<std::vector<IdeaCard>>();
    if (auto s = model.RegisterStruct<IdeaView>()) {
        s.RegisterMember("id", &IdeaView::id);
        s.RegisterMember("name", &IdeaView::name);
        s.RegisterMember("icon", &IdeaView::icon);
        s.RegisterMember("about", &IdeaView::about);
        s.RegisterMember("group", &IdeaView::group);
        s.RegisterMember("can", &IdeaView::can);
    }
    model.RegisterArray<std::vector<IdeaView>>();
    if (auto s = model.RegisterStruct<ChoiceView>()) {
        s.RegisterMember("id", &ChoiceView::id);
        s.RegisterMember("name", &ChoiceView::name);
        s.RegisterMember("icon", &ChoiceView::icon);
        s.RegisterMember("current", &ChoiceView::current);
    }
    model.RegisterArray<std::vector<ChoiceView>>();

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
    model.Bind("lg_sel_code", &m_sel_code_);
    model.Bind("lg_sel_scheme", &m_sel_scheme_);
    model.Bind("lg_sel_own", &m_sel_own_);
    model.Bind("lg_editing", &m_editing_);
    model.Bind("lg_edit_text", &m_edit_text_);
    model.Bind("lg_edit_title", &m_edit_title_);
    model.Bind("lg_edit_error", &m_edit_error_);
    model.Bind("lg_sel_name", &m_sel_name_);
    model.Bind("lg_sel_icon", &m_sel_icon_);
    model.Bind("lg_sel_links", &m_sel_links_);
    model.Bind("lg_count", &m_count_);
    model.Bind("lg_mode", &m_mode_);
    model.Bind("lg_cards", &m_cards_);
    model.Bind("lg_code", &m_code_);
    model.Bind("lg_idea_cards", &m_idea_cards_);
    model.Bind("lg_ideas", &m_ideas_);
    model.Bind("lg_choices", &m_choices_);
    model.Bind("lg_choose_title", &m_choose_title_);
    model.Bind("lg_gallery", &m_gallery_);
    model.Bind("lg_choosing", &m_choosing_);

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
    on("lg_mode", [this](Rml::Event&, const Rml::VariantList& a) {
        const std::string m = arg_str(a, 0);
        set_mode(m);
    });
    on("lg_gallery", [this](Rml::Event& ev, const Rml::VariantList&) {
        ev.StopPropagation();
        open_gallery(!m_gallery_);
    });
    on("lg_idea_add", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        add_idea(arg_str(a, 0));
    });
    on("lg_field", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        open_choices(static_cast<u32>(arg_int(a, 0, 0)), arg_str(a, 1));
    });
    on("lg_choose", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        choose(arg_str(a, 0));
    });
    on("lg_choose_cancel", [this](Rml::Event& ev, const Rml::VariantList&) {
        ev.StopPropagation();
        open_choices(0, {});
    });
    on("lg_idea_refine", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        const u32 id = static_cast<u32>(arg_int(a, 0, 0));
        const std::string what = arg_str(a, 1);
        const logic::Link* l = logic_.find(id);
        if (!l) return;
        const bool now = what == "night" ? l->night : what == "once" ? l->once : what == "sound" ? l->sound : l->hint;
        refine(id, what, !now);
    });
    on("lg_idea_steps", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        const u32 id = static_cast<u32>(arg_int(a, 0, 0));
        set_mode("steps");
        select_link(id);
    });
    on("lg_card", [this](Rml::Event&, const Rml::VariantList& a) {
        const u32 id = static_cast<u32>(arg_int(a, 0, 0));
        if (id != sel_link_) select_link(id);
    });
    on("lg_step_remove", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        refine(static_cast<u32>(arg_int(a, 0, 0)), arg_str(a, 1), false);
    });
    on("lg_step_adds", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        open_adds(static_cast<u32>(arg_int(a, 0, 0)));
    });
    on("lg_step_add", [this](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        const u32 id = static_cast<u32>(arg_int(a, 0, 0));
        adding_ = 0;
        refine(id, arg_str(a, 1), true);
    });
    on("lg_scheme_open", [this](Rml::Event& ev, const Rml::VariantList&) {
        ev.StopPropagation();
        set_mode("scheme");
    });
    on("lg_thing_scheme", [this](Rml::Event& ev, const Rml::VariantList&) {
        ev.StopPropagation();
        thing_scheme(sel_thing_);
    });
    on("lg_scheme_reset", [this](Rml::Event& ev, const Rml::VariantList&) {
        ev.StopPropagation();
        scheme_.reset(sel_link_);
    });
    on("lg_code_edit", [this](Rml::Event& ev, const Rml::VariantList&) {
        ev.StopPropagation();
        edit_code(sel_link_);
    });
    on("lg_code_save", [this](Rml::Event& ev, const Rml::VariantList&) {
        ev.StopPropagation();
        save_code();
    });
    on("lg_code_cancel", [this](Rml::Event& ev, const Rml::VariantList&) {
        ev.StopPropagation();
        cancel_code();
    });
    on("lg_code_reset", [this](Rml::Event& ev, const Rml::VariantList&) {
        ev.StopPropagation();
        reset_code(sel_link_);
    });
    on("lg_code_line", [this](Rml::Event&, const Rml::VariantList& a) {
        const u32 id = static_cast<u32>(arg_int(a, 0, 0));
        if (id) select_link(id);
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
    if (!keep_compiled_) compiled_ = logic::compile(logic_, verbs_, find, &scheme_.nodes()).problems;
    keep_compiled_ = false;
    for (const logic::Problem& p : compiled_)
        if (!p.warning && !problems_.contains(p.link)) problems_[p.link] = p.text;
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
        v.icon = !l.code.empty()                       ? "code"
                 : logic::own_scheme(l, verbs_, find)  ? "account_tree"
                 : verb && !verb->icon.empty()         ? verb->icon
                                                       : "link";
        v.phrase = phrase_of(l.id);
        v.refined = l.night + l.once + l.sound + l.hint;
        v.selected = l.id == sel_link_;
        v.broken = problems_.contains(l.id);
        v.lit = lit(l.id);
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
    m_hint_ = logic_.links.empty() && !hint_closed_ && mode_ == "links";
    rebuild_side();
    if (mode_ == "ideas") rebuild_ideas();
    if (mode_ == "steps") rebuild_steps();
    if (mode_ == "code") rebuild_code();
    if (mode_ == "scheme") scheme_.rebuild();
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
    m_sel_code_ = l && !l->code.empty();
    const logic::FindThing find = [this](std::string_view id) { return thing(id); };
    m_sel_scheme_ = l && l->code.empty() && logic::own_scheme(*l, verbs_, find);
    if (l) {
        m_sel_phrase_ = phrase_of(l->id);
        m_sel_meaning_ = meaning_of(l->id);
        m_sel_problem_ = problem_of(l->id);
        const logic::VerbDef* v = verbs_.find(l->verb);
        const logic::Thing* ta = thing(l->a);
        const logic::Thing* tb = thing(l->b);
        if (v && ta && tb)
            for (const logic::Refine& r : logic::refinements(*l, *v, *ta, *tb)) m_refine_.push_back({r.id, r.label, r.about, r.on});
    }
    // The thing: its picture and links.
    m_thing_links_.clear();
    m_sel_name_ = m_sel_icon_ = "";
    m_sel_links_ = 0;
    m_sel_own_ = m_has_thing_ && logic_.scheme_for(sel_thing_) != nullptr;
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
        m_words_.push_back({static_cast<int>(k.id), phrase_of(k.id), meaning_of(k.id), problem_of(k.id), k.id == sel_link_, lit(k.id)});
    if (model_)
        for (const char* name : {"lg_has_link", "lg_has_thing", "lg_refine", "lg_sel_phrase", "lg_sel_meaning", "lg_sel_problem", "lg_sel_code", "lg_sel_scheme", "lg_sel_own",
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
    scheme_.update(context);
    watch_fired();
    watch_file();
    const objects::Library* lib = module_.library();
    if (lib && lib->version() != built_lib_) dirty_ = true;
    if (dirty_) rebuild();
}

// --- links that happen in the running game -------------------------------------

void LogicEditor::light(u32 link) {
    if (!logic_.find(link)) return;
    const bool was = lit_.contains(link);
    lit_[link] = SDL_GetTicks() + kLitMs;
    if (!was) dirty_ = true;
}

void LogicEditor::watch_fired() {
    const u64 now = SDL_GetTicks();
    for (auto it = lit_.begin(); it != lit_.end();)
        if (it->second <= now) {
            it = lit_.erase(it);
            dirty_ = true;
        } else {
            ++it;
        }
    if (fired_file_.empty() || now - fired_checked_ < 100) return;
    fired_checked_ = now;
    std::error_code ec;
    const u64 size = std::filesystem::file_size(fired_file_, ec);
    if (ec) return;
    const auto time = std::filesystem::last_write_time(fired_file_, ec);
    if (ec || (size == fired_size_ && time == fired_time_)) return;
    fired_size_ = size;
    fired_time_ = time;
    std::vector<u8> bytes;
    if (!read_file(fired_file_, bytes)) return;
    // The first line names the run: another run starts from its beginning.
    const auto nl = std::find(bytes.begin(), bytes.end(), u8('\n'));
    if (nl == bytes.end()) return;
    std::string run(bytes.begin(), nl);
    if (run != fired_run_) {
        fired_run_ = std::move(run);
        fired_at_ = static_cast<u64>(nl - bytes.begin()) + 1;
    }
    // Whole lines only: the game may be writing the last one.
    usize end = bytes.size();
    while (end > fired_at_ && bytes[end - 1] != '\n') --end;
    u64 id = 0;
    bool digits = false;
    for (usize i = static_cast<usize>(fired_at_); i < end; ++i) {
        const u8 c = bytes[i];
        if (c >= '0' && c <= '9') {
            id = id * 10 + (c - '0');
            digits = true;
        } else {
            if (digits) light(static_cast<u32>(id));
            id = 0;
            digits = false;
        }
    }
    if (end > fired_at_) fired_at_ = end;
}

// --- changes -----------------------------------------------------------------

void LogicEditor::save() {
    std::string error;
    if (!logic_.save(file_, &error)) FORGE_ERROR("Связи не сохранились: %s", error.c_str());
    std::error_code ec;
    file_time_ = std::filesystem::last_write_time(file_, ec);
}

// The running game changed the links (F2 over the game): taken in as a change
// of its own, so Ctrl+Z takes it back. New things get places on the board.
void LogicEditor::watch_file() {
    const u64 now = SDL_GetTicks();
    if (now - file_checked_ < 300) return;
    file_checked_ = now;
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(file_, ec);
    if (ec || time == file_time_) return;
    logic::Logic after;
    if (!after.load(file_)) return; // half written: next time
    file_time_ = time;
    for (const logic::Link& l : after.links)
        for (const std::string& id : {l.a, l.b})
            if (!after.spot(id)) place_free(after, id);
    if (after.json() == logic_.json()) return;
    FORGE_INFO("Связи изменены в игре");
    change(after, "Связи изменены в игре");
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

void LogicEditor::change_scheme(u32 link, const script::Graph& graph, std::string label, std::string merge) {
    logic::Logic after = logic_;
    logic::Link* l = after.find(link);
    if (!l) return;
    const logic::FindThing find = [this](std::string_view id) { return thing(id); };
    logic::set_scheme(*l, graph, verbs_, find);
    change(after, std::move(label), std::move(merge));
}

void LogicEditor::change_thing_scheme(u32 id, const script::Graph& graph, std::string label, std::string merge) {
    logic::Logic after = logic_;
    logic::ThingScheme* t = after.find_scheme(id);
    if (!t) return;
    t->graph = graph.to_json();
    change(after, std::move(label), std::move(merge));
}

u32 LogicEditor::thing_scheme(const std::string& id) {
    if (id.empty() || id == logic::kHero || !thing(id)) return 0;
    u32 sid = 0;
    if (const logic::ThingScheme* had = logic_.scheme_for(id)) {
        sid = had->id;
    } else {
        logic::Logic after = logic_;
        logic::ThingScheme t;
        t.thing = id;
        t.graph = logic::new_thing_scheme(id).to_json();
        sid = after.add_scheme(std::move(t));
        if (!sid) return 0;
        change(after, "Своя схема: «" + thing(id)->name + "»");
    }
    set_mode("scheme");
    scheme_.select_frame(sid);
    scheme_.focus(sid);
    return sid;
}

bool LogicEditor::remove_thing_scheme(u32 id) {
    const logic::ThingScheme* t = logic_.find_scheme(id);
    if (!t) return false;
    const logic::Thing* th = thing(t->thing);
    const std::string name = th ? th->name : t->thing;
    logic::Logic after = logic_;
    after.remove_scheme(id);
    change(after, "Своя схема убрана: «" + name + "»");
    return true;
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
                if (!logic::suits(v, x, y)) continue;
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

bool LogicEditor::refine(const std::string& what, bool on) { return refine(sel_link_, what, on); }

bool LogicEditor::refine(u32 link, const std::string& what, bool on) {
    const logic::Link* l = logic_.find(link);
    if (!l) return false;
    logic::Logic after = logic_;
    logic::Link& m = *after.find(link);
    if (what == "night") m.night = on;
    else if (what == "once") m.once = on;
    else if (what == "sound") m.sound = on;
    else if (what == "hint") m.hint = on;
    else if (what == "code" && !on) return reset_code(link);
    else if (what == "graph" && !on) return scheme_.reset(link);
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

// --- «Шаги» and «Код» -----------------------------------------------------------

std::vector<logic::Step> LogicEditor::steps_of(u32 link) const {
    const logic::Link* l = logic_.find(link);
    const logic::VerbDef* v = l ? verbs_.find(l->verb) : nullptr;
    const logic::Thing* a = l ? thing(l->a) : nullptr;
    const logic::Thing* b = l ? thing(l->b) : nullptr;
    return v && a && b ? logic::steps(*l, *v, *a, *b) : std::vector<logic::Step>{};
}

void LogicEditor::open_adds(u32 link) {
    adding_ = adding_ == link ? 0 : link;
    if (adding_ && adding_ != sel_link_) sel_link_ = adding_, sel_thing_.clear();
    rebuild();
}

void LogicEditor::rebuild_steps() {
    m_cards_.clear();
    if (!logic_.find(adding_)) adding_ = 0;
    for (const logic::Link& l : logic_.links) {
        CardView c;
        c.id = static_cast<int>(l.id);
        c.phrase = phrase_of(l.id);
        c.meaning = meaning_of(l.id);
        c.problem = problem_of(l.id);
        c.selected = l.id == sel_link_;
        c.adding = l.id == adding_;
        c.lit = lit(l.id);
        std::string last;
        for (const logic::Step& st : steps_of(l.id)) {
            c.steps.push_back({st.part, st.part == last ? "" : part_label(st.part), st.icon, st.text, st.refine});
            last = st.part;
        }
        const logic::VerbDef* v = verbs_.find(l.verb);
        const logic::Thing* a = thing(l.a);
        const logic::Thing* b = thing(l.b);
        if (v && a && b)
            for (const logic::Refine& r : logic::refinements(l, *v, *a, *b))
                if (!r.on) c.adds.push_back({r.id, r.label, r.about});
        m_cards_.push_back(std::move(c));
    }
    if (model_) model_.DirtyVariable("lg_cards");
}

void LogicEditor::rebuild_code() {
    const logic::FindThing find = [this](std::string_view id) { return thing(id); };
    script::SourceMap map;
    const std::string text = logic::listing(logic_, verbs_, find, &map, &scheme_.nodes());
    m_code_.clear();
    usize start = 0;
    int n = 1;
    while (start <= text.size()) {
        usize end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        if (end == text.size() && start == end) break;
        CodeLine line;
        line.n = n;
        line.text = text.substr(start, end - start);
        const u32 index = map.node_at(n);
        if (index < logic_.links.size()) line.link = static_cast<int>(logic_.links[index].id);
        else if (index != script::SourceMap::kNoNode && index - logic_.links.size() < logic_.schemes.size())
            line.link = static_cast<int>(logic_.schemes[index - logic_.links.size()].id);
        line.selected = line.link != 0 && static_cast<u32>(line.link) == sel_link_;
        line.lit = line.link != 0 && lit(static_cast<u32>(line.link));
        const usize lead = line.text.find_first_not_of(' ');
        line.comment = lead != std::string::npos && line.text.compare(lead, 2, "--") == 0;
        m_code_.push_back(std::move(line));
        ++n;
        start = end + 1;
    }
    if (model_) model_.DirtyVariable("lg_code");
}

// --- «Код» block ---------------------------------------------------------------

bool LogicEditor::edit_code(u32 link) {
    const logic::Link* l = logic_.find(link);
    if (!l) return false;
    if (mode_ != "code") set_mode("code");
    sel_link_ = link;
    editing_ = link;
    const logic::FindThing find = [this](std::string_view id) { return thing(id); };
    m_edit_text_ = l->code.empty() ? logic::default_code(*l, verbs_, find) : l->code;
    m_edit_title_ = "Свой код: «" + phrase_of(link) + "»";
    m_edit_error_ = "";
    m_editing_ = true;
    if (model_)
        for (const char* name : {"lg_editing", "lg_edit_text", "lg_edit_title", "lg_edit_error"}) model_.DirtyVariable(name);
    rebuild();
    return true;
}

void LogicEditor::set_code_text(std::string text) {
    m_edit_text_ = std::move(text);
    if (model_) model_.DirtyVariable("lg_edit_text");
}

void LogicEditor::cancel_code() {
    if (!editing_ && !m_editing_) return;
    editing_ = 0;
    m_editing_ = false;
    m_edit_error_ = "";
    if (model_)
        for (const char* name : {"lg_editing", "lg_edit_error"}) model_.DirtyVariable(name);
}

bool LogicEditor::save_code(std::string* error) {
    const logic::Link* l = logic_.find(editing_);
    if (!l) return false;
    std::string text = m_edit_text_;
    std::replace(text.begin(), text.end(), '\t', ' ');
    if (!text.empty() && text.back() != '\n') text += '\n';
    // As the game will run it: inside its link, with self, hero and target.
    std::string message;
    if (!script::check_syntax("local self, hero, target, logic\n" + text, &message)) {
        m_edit_error_ = "Код не собирается: " + message;
        if (model_) model_.DirtyVariable("lg_edit_error");
        if (error) *error = m_edit_error_;
        return false;
    }
    const logic::FindThing find = [this](std::string_view id) { return thing(id); };
    const bool plain = text == logic::default_code(*l, verbs_, find);
    const u32 id = editing_;
    const std::string phrase = phrase_of(id);
    logic::Logic after = logic_;
    after.find(id)->code = plain ? std::string() : text;
    cancel_code();
    if (after.find(id)->code != l->code) change(after, (plain ? "Обычное действие: «" : "Свой код: «") + phrase + "»");
    select_link(id);
    return true;
}

bool LogicEditor::reset_code(u32 link) {
    const logic::Link* l = logic_.find(link);
    if (!l || l->code.empty()) return false;
    if (editing_ == link) cancel_code();
    logic::Logic after = logic_;
    after.find(link)->code.clear();
    change(after, "Обычное действие: «" + phrase_of(link) + "»");
    return true;
}

// --- «Идеи» --------------------------------------------------------------------

const logic::Idea* LogicEditor::idea_of(u32 link) const {
    const logic::Link* l = logic_.find(link);
    return l ? ideas_.of_verb(l->verb) : nullptr;
}

bool LogicEditor::pair_for(const logic::VerbDef& verb, std::string& a, std::string& b) const {
    // The board's things first, then the rest of the game's.
    std::vector<const logic::Thing*> order;
    for (int pass = 0; pass < 2; ++pass)
        for (const logic::Thing& t : things_)
            if ((logic_.spot(t.id) != nullptr) == (pass == 0)) order.push_back(&t);
    auto taken = [&](const std::string& x, const std::string& y) {
        for (const logic::Link& l : logic_.links)
            if (l.verb == verb.id && l.a == x && l.b == y) return true;
        return false;
    };
    // A pair that is not linked so already; else any pair.
    for (int pass = 0; pass < 2; ++pass)
        for (const logic::Thing* x : order)
            for (const logic::Thing* y : order)
                if (logic::suits(verb, *x, *y) && (pass == 1 || !taken(x->id, y->id))) {
                    a = x->id;
                    b = y->id;
                    return true;
                }
    return false;
}

void LogicEditor::open_gallery(bool open) {
    m_gallery_ = open;
    if (open) {
        m_choosing_ = false;
        choose_link_ = 0;
    }
    dirty_ = true;
    if (model_)
        for (const char* name : {"lg_gallery", "lg_choosing"}) model_.DirtyVariable(name);
}

bool LogicEditor::add_idea(const std::string& id) {
    const logic::Idea* idea = ideas_.find(id);
    const logic::VerbDef* verb = idea ? verbs_.find(idea->verb) : nullptr;
    if (!idea || !verb) return false;
    // The first fitting pair, things on the board first.
    logic::Link l;
    l.verb = verb->id;
    if (!pair_for(*verb, l.a, l.b)) {
        FORGE_WARN("Для идеи «%s» в игре нет подходящей вещи: сделайте её во вкладке «Объекты»", idea->name.c_str());
        return false;
    }
    l.night = idea->night;
    l.once = idea->once;
    l.sound = idea->sound && !verb->sound.empty();
    l.hint = idea->hint && !verb->fail.empty();
    logic::Logic after = logic_;
    const u32 link = after.add(l);
    m_gallery_ = false;
    if (model_) model_.DirtyVariable("lg_gallery");
    change(after, "Идея: «" + idea->name + "»");
    select_link(link);
    return true;
}

void LogicEditor::open_choices(u32 link, const std::string& side) {
    const logic::Link* l = logic_.find(link);
    const logic::VerbDef* v = l ? verbs_.find(l->verb) : nullptr;
    m_choices_.clear();
    choose_link_ = 0;
    choose_side_.clear();
    if (l && v && (side == "a" || side == "b")) {
        choose_link_ = link;
        choose_side_ = side;
        const logic::Thing* other = thing(side == "a" ? l->b : l->a);
        for (const logic::Thing& t : things_) {
            const bool ok = !other || (side == "a" ? logic::suits(*v, t, *other) : logic::suits(*v, *other, t));
            if (ok) m_choices_.push_back({t.id, t.name, icon_of(t.id), t.id == (side == "a" ? l->a : l->b)});
        }
        const logic::Idea* idea = ideas_.of_verb(l->verb);
        std::string label = side == "a" ? "Первая вещь" : "Вторая вещь";
        if (idea)
            for (const logic::IdeaField& f : idea->fields)
                if ((f.side == logic::Side::A) == (side == "a")) label = f.label;
        m_choose_title_ = label + ": что выбрать?";
        sel_link_ = link;
        sel_thing_.clear();
        m_gallery_ = false;
    }
    m_choosing_ = choose_link_ != 0;
    dirty_ = true;
    if (model_)
        for (const char* name : {"lg_choices", "lg_choose_title", "lg_choosing", "lg_gallery"}) model_.DirtyVariable(name);
}

bool LogicEditor::choose(const std::string& id) {
    const logic::Link* l = logic_.find(choose_link_);
    const logic::Thing* t = thing(id);
    if (!l || !t) return false;
    const bool a = choose_side_ == "a";
    const u32 link = choose_link_;
    open_choices(0, {});
    if ((a ? l->a : l->b) == id) return true;
    logic::Logic after = logic_;
    logic::Link& m = *after.find(link);
    (a ? m.a : m.b) = id;
    change(after, "Выбрать «" + t->name + "»");
    select_link(link);
    return true;
}

void LogicEditor::rebuild_ideas() {
    m_idea_cards_.clear();
    for (const logic::Link& l : logic_.links) {
        IdeaCard c;
        c.id = static_cast<int>(l.id);
        const logic::Idea* idea = ideas_.of_verb(l.verb);
        const logic::VerbDef* v = verbs_.find(l.verb);
        c.name = idea ? idea->name : "Связь";
        c.icon = idea ? idea->icon : (v && !v->icon.empty() ? v->icon : "link");
        c.phrase = phrase_of(l.id);
        c.problem = problem_of(l.id);
        c.selected = l.id == sel_link_;
        c.lit = lit(l.id);
        auto field = [&](logic::Side side, const std::string& label) {
            const std::string& id = side == logic::Side::A ? l.a : l.b;
            const logic::Thing* t = thing(id);
            c.fields.push_back({side == logic::Side::A ? "a" : "b", label, id, t ? t->name : id, icon_of(id)});
        };
        if (idea) {
            for (const logic::IdeaField& f : idea->fields) field(f.side, f.label);
        } else {
            field(logic::Side::A, "Первая вещь");
            field(logic::Side::B, "Вторая вещь");
        }
        const logic::Thing* a = thing(l.a);
        const logic::Thing* b = thing(l.b);
        if (v && a && b)
            for (const logic::Refine& r : logic::refinements(l, *v, *a, *b)) c.refine.push_back({r.id, r.label, r.about, r.on});
        m_idea_cards_.push_back(std::move(c));
    }
    m_ideas_.clear();
    for (const logic::Idea& d : ideas_.all()) {
        IdeaView v{d.id, d.name, d.icon, d.about, d.group, false};
        if (const logic::VerbDef* verb = verbs_.find(d.verb)) {
            std::string a, b;
            v.can = pair_for(*verb, a, b);
        }
        m_ideas_.push_back(std::move(v));
    }
    if (model_)
        for (const char* name : {"lg_idea_cards", "lg_ideas"}) model_.DirtyVariable(name);
}

// --- input -------------------------------------------------------------------

bool LogicEditor::handle_event(const SDL_Event& e, f32 density, bool) {
    if (mode_ == "scheme" && scheme_.handle_event(e, density)) return true;
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
    if (mode_ == "scheme" && scheme_.handle_key(k)) return true;
    if (k.key == SDLK_ESCAPE) {
        if (editing_) cancel_code();
        else if (m_choosing_) open_choices(0, {});
        else if (m_gallery_) open_gallery(false);
        else if (picking()) close_picker();
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
