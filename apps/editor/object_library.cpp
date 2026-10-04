#include "object_library.h"

#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/game/dialogue.h"

#include <RmlUi/Core/Elements/ElementFormControl.h>

#include <algorithm>
#include <optional>

namespace forge::editor_app {

namespace fs = std::filesystem;

namespace {

constexpr u32 kIconPx = 96;

// Puts a template back as it was, or takes it away (before/after empty).
class TemplateCommand final : public editor::Command {
public:
    TemplateCommand(objects::Library& lib, std::optional<objects::Template> before,
                    std::optional<objects::Template> after, std::string label, std::string merge)
        : lib_(lib), before_(std::move(before)), after_(std::move(after)), label_(std::move(label)),
          merge_(std::move(merge)) {}
    void apply(editor::Document&) override { set(after_, before_); }
    void revert(editor::Document&) override { set(before_, after_); }
    std::string label() const override { return label_; }
    std::string merge_key() const override { return merge_; }
    bool try_merge(const editor::Command& next) override {
        const auto* n = static_cast<const TemplateCommand*>(&next);
        if (!n || n->merge_ != merge_) return false;
        after_ = n->after_;
        return true;
    }

private:
    void set(const std::optional<objects::Template>& to, const std::optional<objects::Template>& from) {
        std::string error;
        if (to) {
            if (!lib_.put(*to, &error)) FORGE_ERROR("Шаблон «%s»: %s", to->name.c_str(), error.c_str());
        } else if (from) {
            lib_.remove(from->key);
        }
    }
    objects::Library& lib_;
    std::optional<objects::Template> before_, after_;
    std::string label_, merge_;
};

std::string icon_name(const objects::Template& t) { return "tpl_" + t.id + "_" + std::to_string(t.rev); }

} // namespace

ObjectLibrary::ObjectLibrary(level::LevelModule& module) : module_(module) {}

bool ObjectLibrary::init(ui::Ui& ui) {
    ui_ = &ui;
    if (!module_.library()) return false;
    // «Создать»: every kind with its presets.
    for (const objects::KindDef& k : library().kinds()) {
        CreateGroup g{k.id, k.name, k.icon, k.about, {}};
        for (usize i = 0; i < k.presets.size(); ++i) {
            const objects::Preset& p = k.presets[i];
            g.presets.push_back({k.id, p.name, p.genre, p.about, static_cast<int>(i)});
        }
        g.presets.push_back({k.id, "Пустой: " + k.name, "", "все свойства как у вида", -1});
        m_create_.push_back(std::move(g));
    }
    rebuild();
    if (!library().templates().empty()) select(library().templates()[0].key);
    return true;
}

void ObjectLibrary::bind(Rml::DataModelConstructor& model) {
    if (auto s = model.RegisterStruct<KindRow>()) {
        s.RegisterMember("id", &KindRow::id);
        s.RegisterMember("name", &KindRow::name);
        s.RegisterMember("icon", &KindRow::icon);
        s.RegisterMember("about", &KindRow::about);
        s.RegisterMember("count", &KindRow::count);
        s.RegisterMember("selected", &KindRow::selected);
    }
    model.RegisterArray<std::vector<KindRow>>();
    if (auto s = model.RegisterStruct<Card>()) {
        s.RegisterMember("name", &Card::name);
        s.RegisterMember("kind", &Card::kind);
        s.RegisterMember("icon", &Card::icon);
        s.RegisterMember("about", &Card::about);
        s.RegisterMember("selected", &Card::selected);
    }
    model.RegisterArray<std::vector<Card>>();
    if (auto s = model.RegisterStruct<PresetRow>()) {
        s.RegisterMember("kind", &PresetRow::kind);
        s.RegisterMember("name", &PresetRow::name);
        s.RegisterMember("genre", &PresetRow::genre);
        s.RegisterMember("about", &PresetRow::about);
        s.RegisterMember("index", &PresetRow::index);
    }
    model.RegisterArray<std::vector<PresetRow>>();
    if (auto s = model.RegisterStruct<CreateGroup>()) {
        s.RegisterMember("kind", &CreateGroup::kind);
        s.RegisterMember("name", &CreateGroup::name);
        s.RegisterMember("icon", &CreateGroup::icon);
        s.RegisterMember("about", &CreateGroup::about);
        s.RegisterMember("presets", &CreateGroup::presets);
    }
    model.RegisterArray<std::vector<CreateGroup>>();
    if (auto s = model.RegisterStruct<PropView>()) {
        s.RegisterMember("kind", &PropView::kind);
        s.RegisterMember("label", &PropView::label);
        s.RegisterMember("value", &PropView::value);
        s.RegisterMember("hint", &PropView::hint);
        s.RegisterMember("min", &PropView::min);
        s.RegisterMember("max", &PropView::max);
        s.RegisterMember("step", &PropView::step);
        s.RegisterMember("advanced", &PropView::advanced);
    }
    model.RegisterArray<std::vector<PropView>>();

    model.Bind("ol_kinds", &m_kinds_);
    model.Bind("ol_cards", &m_cards_);
    model.Bind("ol_create", &m_create_);
    model.Bind("ol_props", &m_props_);
    model.Bind("ol_kind", &m_kind_);
    model.Bind("ol_search", &m_search_);
    model.Bind("ol_count", &m_count_);
    model.Bind("ol_total", &m_total_);
    model.Bind("ol_menu", &m_menu_);
    model.Bind("ol_details", &m_details_);
    model.Bind("ol_has_sel", &m_has_sel_);
    model.Bind("ol_sel_name", &m_sel_name_);
    model.Bind("ol_sel_kind", &m_sel_kind_);
    model.Bind("ol_sel_kind_icon", &m_sel_kind_icon_);
    model.Bind("ol_sel_kind_about", &m_sel_kind_about_);
    model.Bind("ol_sel_about", &m_sel_about_);
    model.Bind("ol_sel_icon", &m_sel_icon_);
    model.Bind("ol_sel_file", &m_sel_file_);

    auto on = [&](const char* name, auto fn) {
        model.BindEventCallback(name, [this, fn](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& args) {
            fn(ev, args);
        });
    };
    auto arg_str = [](const Rml::VariantList& a, usize i) { return i < a.size() ? a[i].Get<Rml::String>() : Rml::String(); };
    auto arg_int = [](const Rml::VariantList& a, usize i, int fallback = -1) {
        return i < a.size() ? a[i].Get<int>(fallback) : fallback;
    };
    auto input_value = [](Rml::Event& ev) {
        Rml::Element* e = ev.GetTargetElement();
        return e && e->GetTagName() == "input" ? static_cast<Rml::ElementFormControl*>(e)->GetValue() : Rml::String();
    };

    on("ol_kind", [this, arg_str](Rml::Event&, const Rml::VariantList& a) { show_kind(arg_str(a, 0)); });
    on("ol_select", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
        const int i = arg_int(a, 0);
        if (i >= 0 && i < static_cast<int>(card_keys_.size())) select(card_keys_[static_cast<usize>(i)]);
    });
    on("ol_place_card", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
        const int i = arg_int(a, 0);
        if (i >= 0 && i < static_cast<int>(card_keys_.size()) && on_place) on_place(card_keys_[static_cast<usize>(i)]);
    });
    on("ol_menu", [this](Rml::Event&, const Rml::VariantList&) { set(m_menu_, !m_menu_, "ol_menu"); });
    on("ol_menu_close", [this](Rml::Event&, const Rml::VariantList&) { set(m_menu_, false, "ol_menu"); });
    on("ol_create", [this, arg_str, arg_int](Rml::Event&, const Rml::VariantList& a) {
        set(m_menu_, false, "ol_menu");
        create(arg_str(a, 0), arg_int(a, 1));
    });
    on("ol_duplicate", [this](Rml::Event&, const Rml::VariantList&) { duplicate(); });
    on("ol_delete", [this](Rml::Event&, const Rml::VariantList&) { remove_selected(); });
    on("ol_place", [this](Rml::Event&, const Rml::VariantList&) {
        if (selected_ && on_place) on_place(selected_);
    });
    on("ol_details", [this](Rml::Event&, const Rml::VariantList&) { set(m_details_, !m_details_, "ol_details"); });
    on("ol_search", [this, input_value](Rml::Event& ev, const Rml::VariantList&) {
        if (!ui_updating_) set_search(input_value(ev));
    });
    // Name and description: written when the field is left or Enter pressed.
    on("ol_name", [this, input_value](Rml::Event& ev, const Rml::VariantList& a) {
        if (ui_updating_ || (a.size() > 0 && !a[0].Get<bool>())) return;
        rename(input_value(ev));
    });
    on("ol_about", [this, input_value](Rml::Event& ev, const Rml::VariantList& a) {
        if (ui_updating_ || (a.size() > 0 && !a[0].Get<bool>())) return;
        set_about(input_value(ev));
    });
    on("ol_prop_text", [this, arg_int, arg_str](Rml::Event&, const Rml::VariantList& a) {
        if (a.size() > 2 && a[2].Get<bool>()) set_prop(arg_int(a, 0), arg_str(a, 1), false);
    });
    on("ol_prop_commit", [this, arg_int, input_value](Rml::Event& ev, const Rml::VariantList& a) {
        set_prop(arg_int(a, 0), input_value(ev), false);
    });
    on("ol_prop_slide", [this, arg_int, arg_str](Rml::Event&, const Rml::VariantList& a) {
        set_prop(arg_int(a, 0), arg_str(a, 1), true);
    });
    on("ol_prop_toggle", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
        const int i = arg_int(a, 0);
        if (i >= 0 && i < static_cast<int>(m_props_.size()))
            set_prop(i, m_props_[static_cast<usize>(i)].value == "true" ? "false" : "true", false);
    });
    on("ol_prop_cycle", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
        const int i = arg_int(a, 0);
        if (i < 0 || i >= static_cast<int>(prop_refs_.size())) return;
        const auto& choices = prop_refs_[static_cast<usize>(i)]->choices;
        if (choices.empty()) return;
        auto it = std::find_if(choices.begin(), choices.end(),
                               [&](const objects::Choice& c) { return c.name == m_props_[static_cast<usize>(i)].value; });
        const i64 at = it == choices.end() ? 0 : it - choices.begin();
        const i64 n = static_cast<i64>(choices.size());
        set_prop(i, choices[static_cast<usize>(((at + arg_int(a, 1, 1)) % n + n) % n)].name, false);
    });
}

// --- lists ------------------------------------------------------------------

void ObjectLibrary::icon_of(const objects::Template& t) {
    auto it = icon_revs_.find(t.key);
    if (it != icon_revs_.end() && it->second == t.rev) return;
    std::vector<u8> rgba;
    module_.object_icon({t.id, t.name, "", "", t.key}, kIconPx, rgba);
    ui_->set_image(icon_name(t), rgba.data(), kIconPx, kIconPx);
    icon_revs_[t.key] = t.rev;
}

void ObjectLibrary::rebuild() {
    objects::Library& lib = library();
    built_ = lib.version();
    const std::string needle = game::to_lower_utf8(search_);
    m_kinds_.clear();
    m_kinds_.push_back({"", "Все объекты", "category", "", static_cast<int>(lib.templates().size()), kind_.empty()});
    for (const objects::KindDef& k : lib.kinds()) {
        const int n = static_cast<int>(std::count_if(lib.templates().begin(), lib.templates().end(),
                                                     [&](const objects::Template& t) { return t.kind == k.id; }));
        m_kinds_.push_back({k.id, k.name, k.icon, k.about, n, kind_ == k.id});
    }
    m_cards_.clear();
    card_keys_.clear();
    for (const objects::Template& t : lib.templates()) {
        if (!kind_.empty() && t.kind != kind_) continue;
        if (!needle.empty() && game::to_lower_utf8(t.name).find(needle) == std::string::npos) continue;
        icon_of(t);
        const objects::KindDef* k = lib.kind_of(t);
        m_cards_.push_back({t.name, k ? k->name : t.kind, "/memory/" + icon_name(t), t.about, t.key == selected_});
        card_keys_.push_back(t.key);
    }
    m_total_ = static_cast<int>(lib.templates().size());
    m_count_ = m_cards_.size() == lib.templates().size()
                   ? "Шаблонов: " + std::to_string(m_cards_.size())
                   : "Показано " + std::to_string(m_cards_.size()) + " из " + std::to_string(lib.templates().size());
    if (selected_ && !lib.find(selected_)) selected_ = 0;
    if (model_) {
        for (const char* name : {"ol_kinds", "ol_cards", "ol_count", "ol_total", "ol_kind"}) model_.DirtyVariable(name);
    }
    m_kind_ = kind_;
    props_built_ = 0;
    rebuild_props();
}

void ObjectLibrary::rebuild_props() {
    objects::Library& lib = library();
    const objects::Template* t = selected();
    const objects::KindDef* k = t ? lib.kind_of(*t) : nullptr;
    set(m_has_sel_, t != nullptr, "ol_has_sel");
    m_props_.clear();
    prop_refs_.clear();
    if (t) {
        icon_of(*t);
        m_sel_name_ = t->name;
        m_sel_about_ = t->about;
        m_sel_icon_ = "/memory/" + icon_name(*t);
        m_sel_kind_ = k ? k->name : "неизвестный вид «" + t->kind + "»";
        m_sel_kind_icon_ = k ? k->icon : "help";
        m_sel_kind_about_ = k ? k->about : "";
        m_sel_file_ = path_to_utf8(t->file.filename());
        if (k)
            for (const objects::PropDef& p : k->props) {
                using reflect::Kind;
                const Kind kind = p.info->type->kind;
                const std::string json = lib.value(*t, p);
                PropView v;
                v.label = p.name;
                v.hint = p.hint;
                v.advanced = p.advanced;
                v.value = kind == Kind::Bool ? json : objects::Library::display(p, json);
                if (!p.choices.empty()) v.kind = "enum";
                else if (kind == Kind::Bool) v.kind = "bool";
                else if (kind >= Kind::I8 && kind <= Kind::F64 && p.has_range) {
                    v.kind = "slider";
                    v.min = static_cast<float>(p.min);
                    v.max = static_cast<float>(p.max);
                    v.step = kind <= Kind::U64 ? 1.0f : static_cast<float>((p.max - p.min) / 200.0);
                } else {
                    v.kind = "text";
                }
                m_props_.push_back(std::move(v));
                prop_refs_.push_back(&p);
            }
    }
    if (model_) {
        for (const char* name : {"ol_props", "ol_sel_name", "ol_sel_about", "ol_sel_icon", "ol_sel_kind", "ol_sel_kind_icon",
                                 "ol_sel_kind_about", "ol_sel_file"})
            model_.DirtyVariable(name);
    }
}

void ObjectLibrary::update(Rml::Context* context) {
    if (library().version() != built_) {
        // Do not rewrite a field while the user types in it.
        const Rml::Element* focus = context ? context->GetFocusElement() : nullptr;
        if (!(focus && focus->GetTagName() == "input" && focus->GetAttribute<Rml::String>("type", "text") == "text" &&
              focus->GetId() != "ol-search"))
            rebuild();
    }
}

// --- actions ----------------------------------------------------------------

const objects::Template* ObjectLibrary::selected() const {
    return selected_ ? module_.library()->find(selected_) : nullptr;
}

void ObjectLibrary::select(u64 key) {
    if (key == selected_) return;
    selected_ = key;
    for (usize i = 0; i < m_cards_.size(); ++i) m_cards_[i].selected = card_keys_[i] == key;
    if (model_) model_.DirtyVariable("ol_cards");
    rebuild_props();
}

void ObjectLibrary::show_kind(const std::string& kind) {
    kind_ = kind;
    rebuild();
}

void ObjectLibrary::set_search(const std::string& text) {
    if (text == search_) return;
    search_ = text;
    m_search_ = text;
    rebuild();
}

void ObjectLibrary::change(objects::Template after, std::string label, std::string merge) {
    const objects::Template* before = library().find(after.key);
    std::optional<objects::Template> b;
    if (before) b = *before;
    history_.execute(std::make_unique<TemplateCommand>(library(), std::move(b), std::move(after), std::move(label),
                                                       std::move(merge)));
}

bool ObjectLibrary::create(const std::string& kind, int preset) {
    const objects::KindDef* k = library().kind(kind);
    if (!k) return false;
    const objects::Preset* p = preset >= 0 && preset < static_cast<int>(k->presets.size()) ? &k->presets[static_cast<usize>(preset)] : nullptr;
    std::optional<objects::Template> t = library().make(*k, p, "");
    if (!t) return false;
    const u64 key = t->key;
    const std::string name = t->name;
    change(std::move(*t), "Создать: " + name);
    history_.seal();
    // Show it: its kind (or all), no search hiding it.
    if (!kind_.empty() && kind_ != kind) kind_ = kind;
    search_.clear();
    m_search_.clear();
    if (model_) model_.DirtyVariable("ol_search");
    selected_ = key;
    rebuild();
    FORGE_INFO("Создан шаблон «%s» (%s)", name.c_str(), k->name.c_str());
    return true;
}

bool ObjectLibrary::duplicate() {
    const objects::Template* src = selected();
    const objects::KindDef* k = src ? library().kind_of(*src) : nullptr;
    if (!k) return false;
    std::optional<objects::Template> t = library().make(*k, nullptr, src->name + " (копия)");
    if (!t) return false;
    t->about = src->about;
    t->values = src->values;
    const u64 key = t->key;
    change(std::move(*t), "Копия: " + src->name);
    history_.seal();
    selected_ = key;
    rebuild();
    return true;
}

bool ObjectLibrary::remove_selected() {
    const objects::Template* t = selected();
    if (!t) return false;
    const std::string name = t->name;
    // The next card gets the selection.
    u64 next = 0;
    auto it = std::find(card_keys_.begin(), card_keys_.end(), t->key);
    if (it != card_keys_.end()) {
        if (it + 1 != card_keys_.end()) next = *(it + 1);
        else if (it != card_keys_.begin()) next = *(it - 1);
    }
    history_.execute(std::make_unique<TemplateCommand>(library(), *t, std::nullopt, "Удалить: " + name, ""));
    history_.seal();
    selected_ = next;
    rebuild();
    FORGE_INFO("Шаблон «%s» удалён (Ctrl+Z вернёт). Его копии на уровне остаются как были.", name.c_str());
    return true;
}

bool ObjectLibrary::rename(const std::string& name) {
    const objects::Template* t = selected();
    if (!t || name.empty() || name == t->name) {
        rebuild_props();
        return false;
    }
    objects::Template after = *t;
    after.name = library().free_name(name);
    // The file follows the name, so it is easy to find among the resources.
    if (const objects::KindDef* k = library().kind_of(*t))
        if (std::optional<objects::Template> probe = library().make(*k, nullptr, after.name)) after.file = probe->file;
    change(std::move(after), "Переименовать: " + t->name);
    history_.seal();
    return true;
}

bool ObjectLibrary::set_about(const std::string& about) {
    const objects::Template* t = selected();
    if (!t || about == t->about) return false;
    objects::Template after = *t;
    after.about = about;
    change(std::move(after), "«" + t->name + "»: описание");
    history_.seal();
    return true;
}

void ObjectLibrary::set_prop(int i, const std::string& text, bool dragging) {
    if (ui_updating_ || i < 0 || i >= static_cast<int>(prop_refs_.size())) return;
    const objects::Template* t = selected();
    if (!t) return;
    const objects::PropDef& p = *prop_refs_[static_cast<usize>(i)];
    if (text == m_props_[static_cast<usize>(i)].value) return;
    const std::optional<std::string> json = objects::Library::parse(p, text);
    if (!json) {
        FORGE_WARN("«%s»: не понимаю «%s»", p.name.c_str(), text.c_str());
        rebuild_props();
        return;
    }
    if (*json == library().value(*t, p)) return;
    change(library().with_value(*t, p.id, *json), "«" + t->name + "»: " + p.name, "tpl:" + t->id + ":" + p.id);
    if (!dragging) history_.seal();
}

void ObjectLibrary::undo() {
    if (history_.undo()) FORGE_INFO("Отменено");
}

void ObjectLibrary::redo() {
    if (history_.redo()) FORGE_INFO("Повторено");
}

bool ObjectLibrary::handle_key(const SDL_KeyboardEvent& k) {
    const bool ctrl = (k.mod & SDL_KMOD_CTRL) != 0;
    if (k.key == SDLK_DELETE) return remove_selected();
    if (ctrl && k.key == SDLK_D) return duplicate();
    if (ctrl && k.key == SDLK_N) {
        set(m_menu_, true, "ol_menu");
        return true;
    }
    if (k.key == SDLK_ESCAPE && m_menu_) {
        set(m_menu_, false, "ol_menu");
        return true;
    }
    if ((k.key == SDLK_RETURN || k.key == SDLK_KP_ENTER) && selected_ && on_place) {
        on_place(selected_);
        return true;
    }
    if (k.key == SDLK_UP || k.key == SDLK_DOWN || k.key == SDLK_LEFT || k.key == SDLK_RIGHT) {
        if (card_keys_.empty()) return false;
        auto it = std::find(card_keys_.begin(), card_keys_.end(), selected_);
        i64 at = it == card_keys_.end() ? -1 : it - card_keys_.begin();
        at += (k.key == SDLK_UP || k.key == SDLK_LEFT) ? -1 : 1;
        at = std::clamp<i64>(at, 0, static_cast<i64>(card_keys_.size()) - 1);
        select(card_keys_[static_cast<usize>(at)]);
        return true;
    }
    return false;
}

std::string ObjectLibrary::status() const {
    const objects::Template* t = selected();
    std::string s = m_count_;
    if (t) s += " · «" + t->name + "»: " + path_to_utf8(t->file.filename());
    return s;
}

} // namespace forge::editor_app
