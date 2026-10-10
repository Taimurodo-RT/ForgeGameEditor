#include "object_library.h"

#include "forge/assets/image.h"
#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/game/dialogue.h"

#include <RmlUi/Core/Elements/ElementFormControlInput.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>

namespace forge::editor_app {

namespace {

constexpr u32 kIconPx = 96;
constexpr const char* kAnyGame = "Для любой игры";
constexpr f32 kMenuW = 290, kMenuH = 420; // about the right-click menu's size, to keep it inside the tab

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

Rml::String input_value(Rml::Event& ev) {
    Rml::Element* e = ev.GetTargetElement();
    return e && e->GetTagName() == "input" ? static_cast<Rml::ElementFormControl*>(e)->GetValue() : Rml::String();
}

// The mouse of an event in the tab's own pixels (the menus are placed
// inside #objects), with the menu kept inside the tab.
void menu_point(Rml::Event& ev, f32& x, f32& y) {
    x = ev.GetParameter<float>("mouse_x", 0);
    y = ev.GetParameter<float>("mouse_y", 0);
    Rml::Element* t = ev.GetTargetElement();
    Rml::Element* tab = t && t->GetOwnerDocument() ? t->GetOwnerDocument()->GetElementById("objects") : nullptr;
    if (!tab) return;
    const Rml::Vector2f at = tab->GetAbsoluteOffset(Rml::BoxArea::Border);
    x = std::clamp(x - at.x, 0.0f, std::max(0.0f, tab->GetOffsetWidth() - kMenuW));
    y = std::clamp(y - at.y, 0.0f, std::max(0.0f, tab->GetOffsetHeight() - kMenuH));
}

// A property as the editor shows it: its kind of field and value in words.
template <typename View>
View make_prop_view(const objects::Library& lib, const objects::Template& t, const objects::PropDef& p,
                    const std::string& block) {
    using reflect::Kind;
    const Kind kind = p.info->type->kind;
    const std::string json = lib.value(t, p);
    View v;
    v.block = block;
    v.label = p.name;
    v.hint = p.hint;
    v.advanced = p.advanced;
    v.value = kind == Kind::Bool ? json : objects::Library::display(p, json);
    if (p.asset == "sound") {
        v.kind = "sound";
        v.filled = json != "\"\"" && !json.empty();
    } else if (!p.choices.empty()) v.kind = "enum";
    else if (kind == Kind::Bool) v.kind = "bool";
    else if (kind >= Kind::I8 && kind <= Kind::F64 && p.has_range) {
        v.kind = "slider";
        v.min = static_cast<float>(p.min);
        v.max = static_cast<float>(p.max);
        v.step = kind <= Kind::U64 ? 1.0f : static_cast<float>((p.max - p.min) / 200.0);
    } else {
        v.kind = "text";
    }
    return v;
}

} // namespace

ObjectLibrary::ObjectLibrary(level::LevelModule& module) : module_(module) {}

bool ObjectLibrary::init(ui::Ui& ui) {
    ui_ = &ui;
    if (!module_.library()) return false;
    // The shared library knows the same kinds as the game.
    if (!shared_folder_.empty()) {
        shared_.set_pictures_folder(shared_folder_ / "pictures");
        shared_.set_sounds_folder(shared_folder_ / "sounds");
        std::string error;
        shared_ready_ = shared_.load(library().kinds_file(), shared_folder_ / "objects", &error);
        if (!shared_ready_) FORGE_WARN("Общая библиотека объектов недоступна: %s", error.c_str());
    }
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
    if (auto s = model.RegisterStruct<NavRow>()) {
        s.RegisterMember("place", &NavRow::place);
        s.RegisterMember("name", &NavRow::name);
        s.RegisterMember("icon", &NavRow::icon);
        s.RegisterMember("about", &NavRow::about);
        s.RegisterMember("count", &NavRow::count);
        s.RegisterMember("selected", &NavRow::selected);
    }
    model.RegisterArray<std::vector<NavRow>>();
    if (auto s = model.RegisterStruct<Card>()) {
        s.RegisterMember("name", &Card::name);
        s.RegisterMember("kind", &Card::kind);
        s.RegisterMember("genre", &Card::genre);
        s.RegisterMember("icon", &Card::icon);
        s.RegisterMember("about", &Card::about);
        s.RegisterMember("badge", &Card::badge);
        s.RegisterMember("selected", &Card::selected);
        s.RegisterMember("renaming", &Card::renaming);
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
    if (auto s = model.RegisterStruct<GenreItem>()) {
        s.RegisterMember("name", &GenreItem::name);
        s.RegisterMember("label", &GenreItem::label);
        s.RegisterMember("checked", &GenreItem::checked);
    }
    model.RegisterArray<std::vector<GenreItem>>();
    if (auto s = model.RegisterStruct<PropView>()) {
        s.RegisterMember("kind", &PropView::kind);
        s.RegisterMember("block", &PropView::block);
        s.RegisterMember("label", &PropView::label);
        s.RegisterMember("value", &PropView::value);
        s.RegisterMember("hint", &PropView::hint);
        s.RegisterMember("min", &PropView::min);
        s.RegisterMember("max", &PropView::max);
        s.RegisterMember("step", &PropView::step);
        s.RegisterMember("advanced", &PropView::advanced);
        s.RegisterMember("filled", &PropView::filled);
        s.RegisterMember("index", &PropView::index);
    }
    model.RegisterArray<std::vector<PropView>>();
    if (auto s = model.RegisterStruct<BlockView>()) {
        s.RegisterMember("id", &BlockView::id);
        s.RegisterMember("name", &BlockView::name);
        s.RegisterMember("icon", &BlockView::icon);
        s.RegisterMember("about", &BlockView::about);
        s.RegisterMember("note", &BlockView::note);
        s.RegisterMember("removable", &BlockView::removable);
        s.RegisterMember("props", &BlockView::props);
    }
    model.RegisterArray<std::vector<BlockView>>();
    if (auto s = model.RegisterStruct<PicView>()) {
        s.RegisterMember("name", &PicView::name);
        s.RegisterMember("folder", &PicView::folder);
        s.RegisterMember("icon", &PicView::icon);
    }
    model.RegisterArray<std::vector<PicView>>();
    if (auto s = model.RegisterStruct<SndView>()) {
        s.RegisterMember("name", &SndView::name);
        s.RegisterMember("folder", &SndView::folder);
        s.RegisterMember("format", &SndView::format);
        s.RegisterMember("playable", &SndView::playable);
    }
    model.RegisterArray<std::vector<SndView>>();

    model.Bind("ol_all", &m_all_);
    model.Bind("ol_shared", &m_shared_);
    model.Bind("ol_in_shared", &m_in_shared_);
    model.Bind("ol_sel_twin", &m_sel_twin_);
    model.Bind("ol_shared_note", &m_shared_note_);
    model.Bind("ol_genres", &m_genres_);
    model.Bind("ol_kinds", &m_kinds_);
    model.Bind("ol_cards", &m_cards_);
    model.Bind("ol_create", &m_create_);
    model.Bind("ol_genre_items", &m_genre_items_);
    model.Bind("ol_props", &m_props_);
    model.Bind("ol_search", &m_search_);
    model.Bind("ol_count", &m_count_);
    model.Bind("ol_total", &m_total_);
    model.Bind("ol_menu", &m_menu_);
    model.Bind("ol_menu_x", &m_menu_x_);
    model.Bind("ol_menu_y", &m_menu_y_);
    model.Bind("ol_details", &m_details_);
    model.Bind("ol_has_sel", &m_has_sel_);
    model.Bind("ol_editing", &m_editing_);
    model.Bind("ol_sel_name", &m_sel_name_);
    model.Bind("ol_sel_kind", &m_sel_kind_);
    model.Bind("ol_sel_kind_icon", &m_sel_kind_icon_);
    model.Bind("ol_sel_kind_about", &m_sel_kind_about_);
    model.Bind("ol_sel_genre", &m_sel_genre_);
    model.Bind("ol_sel_about", &m_sel_about_);
    model.Bind("ol_sel_icon", &m_sel_icon_);
    model.Bind("ol_sel_file", &m_sel_file_);
    model.Bind("ol_sel_picture", &m_sel_picture_);
    model.Bind("ol_sel_frames", &m_sel_frames_);
    model.Bind("ol_sel_hero", &m_sel_hero_);
    model.Bind("ol_sel_placed", &m_sel_placed_);
    model.Bind("ol_pics", &m_pics_);
    model.Bind("ol_blocks", &m_blocks_);
    model.Bind("ol_add_blocks", &m_add_blocks_);
    model.Bind("ol_pics_open", &m_pics_open_);
    model.Bind("ol_pics_search", &m_pics_search_);
    model.Bind("ol_pics_note", &m_pics_note_);
    model.Bind("ol_snds", &m_snds_);
    model.Bind("ol_snds_open", &m_snds_open_);
    model.Bind("ol_snds_search", &m_snds_search_);
    model.Bind("ol_snds_note", &m_snds_note_);
    model.Bind("ol_snds_title", &m_snds_title_);

    // Any action closes an open menu.
    auto on = [&](const char* name, auto fn) {
        model.BindEventCallback(name, [this, fn](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& args) {
            set_menu("");
            fn(ev, args);
        });
    };
    auto arg_str = [](const Rml::VariantList& a, usize i) { return i < a.size() ? a[i].Get<Rml::String>() : Rml::String(); };
    auto arg_int = [](const Rml::VariantList& a, usize i, int fallback = -1) {
        return i < a.size() ? a[i].Get<int>(fallback) : fallback;
    };
    auto card_key = [this](int i) { return i >= 0 && i < static_cast<int>(card_keys_.size()) ? card_keys_[static_cast<usize>(i)] : 0; };

    on("ol_show", [this, arg_str](Rml::Event&, const Rml::VariantList& a) { show(arg_str(a, 0)); });
    // Cards: a click selects, a double click opens the editor, the right
    // button opens the menu.
    on("ol_card_down", [this, arg_int, card_key](Rml::Event& ev, const Rml::VariantList& a) {
        const int i = arg_int(a, 0);
        const u64 key = card_key(i);
        if (key && !(renaming_ && key == selected_)) select(key);
        if (ev.GetParameter<int>("button", 0) == 1) {
            f32 x, y;
            menu_point(ev, x, y);
            context_menu(i, x, y);
        }
    });
    on("ol_card_open", [this, arg_int, card_key](Rml::Event&, const Rml::VariantList& a) {
        if (const u64 key = card_key(arg_int(a, 0))) {
            select(key);
            open_editor();
        }
    });
    // Empty space between the cards: the right button offers to create.
    // (It also hears the cards' presses, which bubble up: those are left alone.)
    model.BindEventCallback("ol_space_down", [this](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList&) {
        Rml::Element* t = ev.GetTargetElement();
        if (!t || (t->GetId() != "ol-grid" && t->GetId() != "ol-grid-wrap")) return;
        set_menu("");
        if (ev.GetParameter<int>("button", 0) == 1) {
            f32 x, y;
            menu_point(ev, x, y);
            context_menu(-1, x, y);
        }
    });
    model.BindEventCallback("ol_menu", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        set_menu(m_menu_ == "new" ? "" : "new");
    });
    on("ol_menu_close", [](Rml::Event&, const Rml::VariantList&) {});
    on("ol_create", [this, arg_str, arg_int](Rml::Event&, const Rml::VariantList& a) { create(arg_str(a, 0), arg_int(a, 1)); });
    model.BindEventCallback("ol_create_menu", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        set_menu("new");
    });
    on("ol_duplicate", [this](Rml::Event&, const Rml::VariantList&) { duplicate(); });
    on("ol_delete", [this](Rml::Event&, const Rml::VariantList&) { remove_selected(); });
    on("ol_rename_start", [this](Rml::Event&, const Rml::VariantList&) { start_rename(); });
    on("ol_place", [this](Rml::Event&, const Rml::VariantList&) { place_selected(); });
    on("ol_share", [this](Rml::Event&, const Rml::VariantList&) { share_selected(); });
    on("ol_take", [this](Rml::Event&, const Rml::VariantList&) { take_selected(); });
    on("ol_open", [this](Rml::Event&, const Rml::VariantList&) { open_editor(); });
    on("ol_back", [this](Rml::Event&, const Rml::VariantList&) { close_editor(); });
    on("ol_genre", [this, arg_str](Rml::Event&, const Rml::VariantList& a) { set_genre(arg_str(a, 0)); });
    on("ol_block_add", [this, arg_str](Rml::Event&, const Rml::VariantList& a) { add_block(arg_str(a, 0)); });
    on("ol_block_remove", [this, arg_str](Rml::Event&, const Rml::VariantList& a) { remove_block(arg_str(a, 0)); });
    model.BindEventCallback("ol_blocks_menu", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        set_menu(m_menu_ == "blocks" ? "" : "blocks");
    });
    on("ol_picture_pick", [this](Rml::Event&, const Rml::VariantList&) { open_pictures(); });
    on("ol_picture_clear", [this](Rml::Event&, const Rml::VariantList&) { clear_picture(); });
    on("ol_frames", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
        if (const objects::Template* t = selected()) set_frames(static_cast<int>(t->frames) + arg_int(a, 0));
    });
    on("ol_picture_choose", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
        const int i = arg_int(a, 0);
        if (i >= 0) choose_picture(static_cast<usize>(i));
    });
    on("ol_pics_close", [this](Rml::Event&, const Rml::VariantList&) { close_pictures(); });
    on("ol_sound_pick", [this, arg_int](Rml::Event&, const Rml::VariantList& a) { open_sounds(arg_int(a, 0)); });
    on("ol_sound_clear", [this, arg_int](Rml::Event&, const Rml::VariantList& a) { clear_sound(arg_int(a, 0)); });
    on("ol_sound_play", [this, arg_int](Rml::Event&, const Rml::VariantList& a) { play_prop_sound(arg_int(a, 0)); });
    on("ol_snd_choose", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
        const int i = arg_int(a, 0);
        if (i >= 0) choose_sound(static_cast<usize>(i));
    });
    // The ▶ on a row: listen without choosing (the row's click must not choose).
    model.BindEventCallback("ol_snd_preview", [this, arg_int](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        const int i = arg_int(a, 0);
        if (i >= 0) preview_sound(static_cast<usize>(i));
    });
    on("ol_snds_close", [this](Rml::Event&, const Rml::VariantList&) { close_sounds(); });
    model.BindEventCallback("ol_snds_search", [this](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList&) {
        if (!ui_updating_) set_sound_search(input_value(ev));
    });
    model.BindEventCallback("ol_pics_search", [this](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList&) {
        if (!ui_updating_) set_picture_search(input_value(ev));
    });
    on("ol_details", [this](Rml::Event&, const Rml::VariantList&) { set(m_details_, !m_details_, "ol_details"); });
    model.BindEventCallback("ol_search", [this](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList&) {
        if (!ui_updating_) set_search(input_value(ev));
    });
    // The name typed on the card: taken on Enter or when the field is left.
    model.BindEventCallback("ol_rename_done", [this](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& a) {
        if (ui_updating_ || !renaming_ || (!a.empty() && !a[0].Get<bool>())) return;
        if (!rename(input_value(ev))) rebuild();
        // Enter ends the typing: the keys (Ctrl+Z) are the tab's again, not the hidden field's.
        if (Rml::Element* e = ev.GetTargetElement()) e->Blur();
    });
    // The note in the editor: written when the field is left or Enter pressed.
    model.BindEventCallback("ol_about", [this](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& a) {
        if (ui_updating_ || (!a.empty() && !a[0].Get<bool>())) return;
        set_about(input_value(ev));
    });
    on("ol_prop_text", [this, arg_int, arg_str](Rml::Event&, const Rml::VariantList& a) {
        if (a.size() > 2 && a[2].Get<bool>()) set_prop(arg_int(a, 0), arg_str(a, 1), false);
    });
    on("ol_prop_commit", [this, arg_int](Rml::Event& ev, const Rml::VariantList& a) {
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

std::string ObjectLibrary::icon_path(const objects::Template& t, bool from_shared) {
    const std::string name = (from_shared ? "shr_" : "tpl_") + t.id + "_" + std::to_string(t.look());
    auto& revs = from_shared ? shared_icon_revs_ : icon_revs_;
    auto it = revs.find(t.key);
    if (it != revs.end() && it->second == t.look()) return "/memory/" + name;
    std::vector<u8> rgba;
    std::vector<u8> bytes;
    assets::CookedTexture image;
    if (from_shared && !t.picture.empty() && read_file(shared_.picture_file(t), bytes) && assets::decode_image(bytes, image)) {
        // Its own picture, from the shared library's folder, centred: of a strip of frames the first.
        if (t.frames > 1 && image.width % t.frames == 0) {
            const u32 fw = image.width / t.frames;
            std::vector<u8> first(static_cast<usize>(fw) * image.height * 4);
            for (u32 y = 0; y < image.height; ++y)
                std::copy_n(&image.rgba8[static_cast<usize>(y) * image.width * 4], static_cast<usize>(fw) * 4,
                            &first[static_cast<usize>(y) * fw * 4]);
            image.rgba8 = std::move(first);
            image.width = fw;
        }
        const assets::CookedTexture fit = assets::fit_image(image, kIconPx);
        rgba.assign(static_cast<usize>(kIconPx) * kIconPx * 4, 0);
        const u32 ox = (kIconPx - std::min(fit.width, kIconPx)) / 2, oy = (kIconPx - std::min(fit.height, kIconPx)) / 2;
        for (u32 y = 0; y < std::min(fit.height, kIconPx); ++y)
            std::copy_n(&fit.rgba8[static_cast<usize>(y) * fit.width * 4], static_cast<usize>(std::min(fit.width, kIconPx)) * 4,
                        &rgba[(static_cast<usize>(oy + y) * kIconPx + ox) * 4]);
    } else {
        level::ObjectDef def{t.id, t.name, "", "", t.key};
        if (from_shared) def.tmpl = &t;
        module_.object_icon(def, kIconPx, rgba);
    }
    ui_->set_image(name, rgba.data(), kIconPx, kIconPx);
    revs[t.key] = t.look();
    return "/memory/" + name;
}

std::string ObjectLibrary::twin(const objects::Template& t) const {
    if (!shared_ready_) return {};
    const objects::Library& other = showing_shared() ? *module_.library() : shared_;
    if (!other.find(t.key)) return {};
    return other.same_as(shown(), t) ? "same" : "differs";
}

void ObjectLibrary::rebuild() {
    objects::Library& lib = library();
    built_ = lib.version();
    shared_built_ = shared_.version();
    const bool in_shared = showing_shared();
    const auto& game_all = lib.templates();
    const auto& all = shown().templates();
    auto count_if = [&](auto pred) { return static_cast<int>(std::count_if(game_all.begin(), game_all.end(), pred)); };

    // Navigation: everything, the genres, the kinds; the shared library.
    m_all_ = {"", "Все объекты", "category", "", static_cast<int>(game_all.size()), place_.empty()};
    m_shared_ = {"s:", "Общие объекты", "share", "объекты для всех игр: берите их в игру копией",
                 static_cast<int>(shared_.templates().size()), in_shared};
    set(m_in_shared_, in_shared, "ol_in_shared");
    m_shared_note_ = shared_ready_ ? "Общие объекты хранятся на этом компьютере (" + path_to_utf8(shared_folder_) +
                                         ") и видны в любой игре. В игру объект попадает копией: игра работает и без них."
                                   : "Общая библиотека недоступна.";
    m_genres_.clear();
    for (const std::string& g : lib.genres())
        m_genres_.push_back({"g:" + g, g, "sports_esports", "",
                             count_if([&](const objects::Template& t) { return t.genre == g; }), place_ == "g:" + g});
    m_genres_.push_back({"g:", kAnyGame, "public", "объекты без жанра",
                         count_if([](const objects::Template& t) { return t.genre.empty(); }), place_ == "g:"});
    m_kinds_.clear();
    for (const objects::KindDef& k : lib.kinds())
        m_kinds_.push_back({"k:" + k.id, k.name, k.icon, k.about,
                            count_if([&](const objects::Template& t) { return t.kind == k.id; }), place_ == "k:" + k.id});

    // The cards of this place, as the search narrows them.
    const std::string needle = game::to_lower_utf8(search_);
    const bool by_genre = place_.starts_with("g:"), by_kind = place_.starts_with("k:");
    const std::string what = place_.size() > 2 ? place_.substr(2) : std::string();
    m_cards_.clear();
    card_keys_.clear();
    for (const objects::Template& t : all) {
        if (by_genre && t.genre != what) continue;
        if (by_kind && t.kind != what) continue;
        if (!needle.empty() && game::to_lower_utf8(t.name).find(needle) == std::string::npos) continue;
        const objects::KindDef* k = lib.kind_of(t);
        const std::string tw = twin(t);
        const std::string badge = tw.empty() ? std::string()
                                  : in_shared ? (tw == "same" ? "в игре" : "в игре, другой")
                                              : (tw == "same" ? "общий" : "общий, изменён");
        m_cards_.push_back({t.name, k ? k->name : t.kind, t.genre, icon_path(t, in_shared), t.about, badge,
                            t.key == selected_, renaming_ && t.key == selected_});
        card_keys_.push_back(t.key);
    }
    m_total_ = static_cast<int>(all.size());
    m_count_ = m_cards_.size() == all.size() ? "Объектов: " + std::to_string(all.size())
                                             : "Показано " + std::to_string(m_cards_.size()) + " из " + std::to_string(all.size());
    if (selected_ && !shown().find(selected_)) selected_ = 0;
    if (editing_ && !lib.find(editing_)) close_editor();
    if (model_)
        for (const char* name : {"ol_all", "ol_shared", "ol_shared_note", "ol_genres", "ol_kinds", "ol_cards", "ol_count", "ol_total"}) model_.DirtyVariable(name);
    rebuild_side();
}

void ObjectLibrary::rebuild_side() {
    objects::Library& lib = library();
    const objects::Template* t = selected();
    const objects::KindDef* k = t ? lib.kind_of(*t) : nullptr;
    set(m_has_sel_, t != nullptr, "ol_has_sel");
    m_props_.clear();
    prop_refs_.clear();
    m_blocks_.clear();
    m_add_blocks_.clear();
    m_genre_items_.clear();
    m_sel_twin_ = t ? twin(*t) : std::string();
    if (t) {
        m_sel_name_ = t->name;
        m_sel_about_ = t->about;
        m_sel_icon_ = icon_path(*t, showing_shared());
        m_sel_kind_ = k ? k->name : "неизвестный вид «" + t->kind + "»";
        m_sel_kind_icon_ = k ? k->icon : "help";
        m_sel_kind_about_ = k ? k->about : "";
        m_sel_genre_ = t->genre.empty() ? kAnyGame : t->genre;
        m_sel_file_ = path_to_utf8(t->file.filename());
        m_sel_picture_ = t->picture;
        m_sel_frames_ = static_cast<int>(t->frames);
        m_sel_placed_ = !k || k->placed;
        m_sel_hero_ = hero_note(*t);
        for (const std::string& g : lib.genres()) m_genre_items_.push_back({g, g, t->genre == g});
        m_genre_items_.push_back({"", kAnyGame, t->genre.empty()});
        // The editor: the object's blocks and their properties.
        if (k && editing_ == t->key) {
            auto names = [&](const std::vector<std::string>& ids) {
                std::string out;
                for (const std::string& id : ids)
                    if (const objects::BlockDef* b = lib.block(id)) out += (out.empty() ? "«" : ", «") + b->name + "»";
                return out;
            };
            auto ids_of = [&](const objects::Template& x) {
                std::vector<std::string> out;
                for (const objects::BlockDef* b : lib.blocks_of(x)) out.push_back(b->id);
                return out;
            };
            const std::vector<std::string> mine = ids_of(*t);
            auto add_props = [&](const std::string& block, const std::vector<const objects::PropDef*>& props) {
                for (const objects::PropDef* p : props) {
                    if (std::find(prop_refs_.begin(), prop_refs_.end(), p) != prop_refs_.end()) continue;
                    m_props_.push_back(make_prop_view<PropView>(lib, *t, *p, block));
                    m_props_.back().index = static_cast<int>(m_props_.size() - 1);
                    prop_refs_.push_back(p);
                    m_blocks_.back().props.push_back(m_props_.back());
                }
            };
            for (const objects::BlockDef* b : lib.blocks_of(*t)) {
                // What else goes when this one is taken away.
                std::vector<std::string> gone, after = ids_of(lib.with_block(*t, b->id, false));
                for (const std::string& id : mine)
                    if (id != b->id && std::find(after.begin(), after.end(), id) == after.end()) gone.push_back(id);
                // An object keeps at least one block.
                m_blocks_.push_back({b->id, b->name, b->icon, b->about,
                                     gone.empty() ? std::string() : "вместе с ним уйдёт " + names(gone), !after.empty()});
                std::vector<const objects::PropDef*> props;
                for (const objects::PropDef& p : b->props) props.push_back(&p);
                add_props(b->id, props);
            }
            if (m_blocks_.empty()) {
                // A kind not made of blocks: its properties as one.
                m_blocks_.push_back({"", "Свойства", "tune", "", "", false});
                add_props("", lib.props_of(*t));
            }
            for (const objects::BlockDef& b : lib.blocks()) {
                if (std::find(mine.begin(), mine.end(), b.id) != mine.end()) continue;
                const std::vector<std::string> after = ids_of(lib.with_block(*t, b.id, true));
                std::vector<std::string> added, gone;
                for (const std::string& id : after)
                    if (id != b.id && std::find(mine.begin(), mine.end(), id) == mine.end()) added.push_back(id);
                for (const std::string& id : mine)
                    if (std::find(after.begin(), after.end(), id) == after.end()) gone.push_back(id);
                std::string note;
                if (!added.empty()) note = "добавит и " + names(added);
                if (!gone.empty()) note += (note.empty() ? "заменит " : "; заменит ") + names(gone);
                m_add_blocks_.push_back({b.id, b.name, b.icon, b.about, note, true});
            }
        }
    }
    if (model_)
        for (const char* name : {"ol_props", "ol_blocks", "ol_add_blocks", "ol_genre_items", "ol_sel_name", "ol_sel_about", "ol_sel_icon", "ol_sel_kind",
                                 "ol_sel_kind_icon", "ol_sel_kind_about", "ol_sel_genre", "ol_sel_file", "ol_sel_picture", "ol_sel_frames", "ol_sel_hero", "ol_sel_placed",
                                 "ol_sel_twin"})
            model_.DirtyVariable(name);
}

void ObjectLibrary::update(Rml::Context* context) {
    const Rml::Element* focus = context ? context->GetFocusElement() : nullptr;
    // Do not rewrite a field while the user types in it.
    const bool typing = focus && focus->GetTagName() == "input" && focus->GetAttribute<Rml::String>("type", "text") == "text" &&
                        focus->GetId() != "ol-search";
    if ((library().version() != built_ || shared_.version() != shared_built_) && !typing) rebuild();
    if (rename_focus_ && context) {
        // The name field on the card appears with this update: focus it. Every card has one (data-if only hides it),
        // the shown one is the selected card's.
        Rml::ElementList fields;
        for (int i = 0; i < context->GetNumDocuments(); ++i) context->GetDocument(i)->QuerySelectorAll(fields, "#ol-name-field");
        for (Rml::Element* field : fields)
            if (auto* e = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(field); e && e->IsVisible(true)) {
                e->Focus();
                e->Select();
                rename_focus_ = false;
                break;
            }
    }
}

// --- actions ----------------------------------------------------------------

const objects::Template* ObjectLibrary::selected() const {
    return selected_ ? shown().find(selected_) : nullptr;
}

void ObjectLibrary::select(u64 key) {
    if (key == selected_) return;
    selected_ = key;
    renaming_ = false;
    for (usize i = 0; i < m_cards_.size(); ++i) {
        m_cards_[i].selected = card_keys_[i] == key;
        m_cards_[i].renaming = false;
    }
    if (model_) model_.DirtyVariable("ol_cards");
    rebuild_side();
}

void ObjectLibrary::show(const std::string& place) {
    // Another game may have shared something meanwhile.
    if (place == "s:" && shared_ready_) shared_.reload_templates();
    place_ = place;
    close_editor();
    rebuild();
}

void ObjectLibrary::set_search(const std::string& text) {
    if (text == search_) return;
    search_ = text;
    m_search_ = text;
    rebuild();
}

void ObjectLibrary::set_menu(const std::string& menu) { set(m_menu_, Rml::String(menu), "ol_menu"); }

void ObjectLibrary::context_menu(int card, f32 x, f32 y) {
    if (card >= 0 && card < static_cast<int>(card_keys_.size())) select(card_keys_[static_cast<usize>(card)]);
    set(m_menu_x_, x, "ol_menu_x");
    set(m_menu_y_, y, "ol_menu_y");
    set_menu(card >= 0 && selected() ? "card" : "empty");
}

void ObjectLibrary::change(objects::Template after, std::string label, std::string merge) {
    change(library(), std::move(after), std::move(label), std::move(merge));
}

void ObjectLibrary::change(objects::Library& lib, objects::Template after, std::string label, std::string merge) {
    const objects::Template* before = lib.find(after.key);
    std::optional<objects::Template> b;
    if (before) b = *before;
    history_.execute(std::make_unique<TemplateCommand>(lib, std::move(b), std::move(after), std::move(label), std::move(merge)));
}

bool ObjectLibrary::create(const std::string& kind, int preset) {
    const objects::KindDef* k = library().kind(kind);
    if (!k) return false;
    const objects::Preset* p = preset >= 0 && preset < static_cast<int>(k->presets.size()) ? &k->presets[static_cast<usize>(preset)] : nullptr;
    std::optional<objects::Template> t = library().make(*k, p, "");
    if (!t) return false;
    // An empty one made while a genre is shown belongs to it.
    if (place_.starts_with("g:") && !p) t->genre = place_.substr(2);
    const u64 key = t->key;
    const std::string name = t->name, genre = t->genre;
    change(std::move(*t), "Создать: " + name);
    history_.seal();
    // Show it: in the place shown if it belongs there, else among all.
    if (showing_shared() || (place_.starts_with("k:") && place_ != "k:" + kind) || (place_.starts_with("g:") && place_ != "g:" + genre))
        place_.clear();
    search_.clear();
    m_search_.clear();
    if (model_) model_.DirtyVariable("ol_search");
    selected_ = key;
    rebuild();
    FORGE_INFO("Создан объект «%s» (%s)", name.c_str(), k->name.c_str());
    return true;
}

bool ObjectLibrary::duplicate() {
    if (showing_shared()) return false; // shared objects are changed in a game, then shared again
    const objects::Template* src = selected();
    const objects::KindDef* k = src ? library().kind_of(*src) : nullptr;
    if (!k) return false;
    std::optional<objects::Template> t = library().make(*k, nullptr, src->name + " (копия)");
    if (!t) return false;
    t->about = src->about;
    t->genre = src->genre;
    t->values = src->values;
    t->blocks = src->blocks;
    t->picture = src->picture;
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
    const bool in_shared = showing_shared();
    history_.execute(std::make_unique<TemplateCommand>(shown(), *t, std::nullopt,
                                                       (in_shared ? "Убрать из общих: " : "Удалить: ") + name, ""));
    history_.seal();
    selected_ = next;
    renaming_ = false;
    rebuild();
    if (in_shared) FORGE_INFO("Объект «%s» убран из общих (Ctrl+Z вернёт). В играх его копии остаются.", name.c_str());
    else FORGE_INFO("Объект «%s» удалён из библиотеки (Ctrl+Z вернёт). Его копии на уровне остаются как были.", name.c_str());
    return true;
}

void ObjectLibrary::start_rename() {
    if (!selected() || showing_shared()) return;
    close_editor();
    renaming_ = true;
    rename_focus_ = true;
    rebuild();
}

bool ObjectLibrary::rename(const std::string& name) {
    renaming_ = false;
    if (showing_shared()) return false; // shared objects are changed in a game, then shared again
    const objects::Template* t = selected();
    if (!t || name.empty() || name == t->name) return false;
    objects::Template after = *t;
    after.name = library().free_name(name);
    // The file follows the name, so it is easy to find among the resources.
    if (const objects::KindDef* k = library().kind_of(*t))
        if (std::optional<objects::Template> probe = library().make(*k, nullptr, after.name)) after.file = probe->file;
    change(std::move(after), "Переименовать: " + t->name);
    history_.seal();
    rebuild();
    return true;
}

bool ObjectLibrary::set_genre(const std::string& genre) {
    if (showing_shared()) return false; // shared objects are changed in a game, then shared again
    const objects::Template* t = selected();
    if (!t || t->genre == genre) return false;
    objects::Template after = *t;
    after.genre = genre;
    change(std::move(after), "«" + t->name + "»: " + (genre.empty() ? std::string(kAnyGame) : "жанр " + genre));
    history_.seal();
    return true;
}

bool ObjectLibrary::place_selected() {
    if (!selected_ || !on_place) return false;
    // Its kind is not put on levels (the hero's picture: the game puts the hero).
    if (const objects::Template* t = selected(); t && library().kind_of(*t) && !library().kind_of(*t)->placed) return false;
    // A shared object is placed as the game's copy of it.
    if (showing_shared() && twin(*selected()) != "same" && !take_selected()) return false;
    if (!library().find(selected_)) return false;
    on_place(selected_);
    return true;
}

bool ObjectLibrary::share_selected() {
    const objects::Template* t = selected();
    if (!t || showing_shared() || !shared_ready_ || twin(*t) == "same") return false;
    std::string error;
    std::optional<objects::Template> copy = shared_.copy_from(library(), *t, &error);
    if (!copy) {
        FORGE_WARN("«%s» не делается общим: %s", t->name.c_str(), error.c_str());
        return false;
    }
    const bool update = shared_.find(t->key) != nullptr;
    const std::string name = t->name;
    change(shared_, std::move(*copy), (update ? "Обновить в общих: " : "Сделать общим: ") + name);
    history_.seal();
    rebuild();
    FORGE_INFO(update ? "Общий объект «%s» обновлён по этой игре" : "Объект «%s» теперь среди общих: его можно взять в любую игру",
               name.c_str());
    return true;
}

bool ObjectLibrary::take_selected() {
    const objects::Template* t = selected();
    if (!t || !showing_shared() || twin(*t) == "same") return false;
    std::string error;
    std::optional<objects::Template> copy = library().copy_from(shared_, *t, &error);
    if (!copy) {
        FORGE_WARN("«%s» не берётся в игру: %s", t->name.c_str(), error.c_str());
        return false;
    }
    const bool update = library().find(t->key) != nullptr;
    const std::string name = t->name;
    change(std::move(*copy), (update ? "Обновить из общих: " : "Взять в игру: ") + name);
    history_.seal();
    rebuild();
    FORGE_INFO(update ? "Объект «%s» в игре обновлён по общему" : "Объект «%s» взят в игру копией", name.c_str());
    return true;
}

void ObjectLibrary::open_editor() {
    if (!selected() || showing_shared()) return;
    renaming_ = false;
    editing_ = selected_;
    set(m_editing_, true, "ol_editing");
    rebuild_side();
}

void ObjectLibrary::close_editor() {
    if (!editing_) return;
    close_pictures();
    close_sounds();
    editing_ = 0;
    set(m_editing_, false, "ol_editing");
    rebuild_side();
}

bool ObjectLibrary::add_block(const std::string& block) {
    if (showing_shared()) return false; // shared objects are changed in a game, then shared again
    const objects::Template* t = selected();
    const objects::BlockDef* b = library().block(block);
    set_menu("");
    if (!t || !b || library().has_block(*t, block)) return false;
    change(library().with_block(*t, block, true), "«" + t->name + "»: блок «" + b->name + "»");
    history_.seal();
    FORGE_INFO("«%s»: добавлен блок «%s»", t->name.c_str(), b->name.c_str());
    return true;
}

bool ObjectLibrary::remove_block(const std::string& block) {
    if (showing_shared()) return false; // shared objects are changed in a game, then shared again
    const objects::Template* t = selected();
    const objects::BlockDef* b = library().block(block);
    if (!t || !b || !library().has_block(*t, block)) return false;
    objects::Template after = library().with_block(*t, block, false);
    if (after.blocks.empty()) return false; // nothing would be left of it
    const std::string name = t->name;
    change(std::move(after), "«" + name + "»: без блока «" + b->name + "»");
    history_.seal();
    FORGE_INFO("«%s»: убран блок «%s»", name.c_str(), b->name.c_str());
    return true;
}

bool ObjectLibrary::set_about(const std::string& about) {
    if (showing_shared()) return false; // shared objects are changed in a game, then shared again
    const objects::Template* t = selected();
    if (!t || about == t->about) return false;
    objects::Template after = *t;
    after.about = about;
    change(std::move(after), "«" + t->name + "»: заметка");
    history_.seal();
    return true;
}

void ObjectLibrary::set_prop(int i, const std::string& text, bool dragging) {
    if (ui_updating_ || showing_shared() || i < 0 || i >= static_cast<int>(prop_refs_.size())) return;
    const objects::Template* t = selected();
    if (!t) return;
    const objects::PropDef& p = *prop_refs_[static_cast<usize>(i)];
    if (text == m_props_[static_cast<usize>(i)].value) return;
    const std::optional<std::string> json = objects::Library::parse(p, text);
    if (!json) {
        FORGE_WARN("«%s»: не понимаю «%s»", p.name.c_str(), text.c_str());
        rebuild_side();
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
    if (k.key == SDLK_ESCAPE) {
        if (menu_open()) set_menu("");
        else if (m_pics_open_) close_pictures();
        else if (m_snds_open_) close_sounds();
        else if (renaming_) {
            renaming_ = false;
            rebuild();
        } else if (editing_) close_editor();
        else return false;
        return true;
    }
    if (editing_) return false; // in the editor the keys belong to its fields
    if (k.key == SDLK_DELETE) return remove_selected();
    if (k.key == SDLK_F2) {
        start_rename();
        return true;
    }
    if (ctrl && k.key == SDLK_D) return duplicate();
    if (ctrl && k.key == SDLK_N) {
        set_menu("new");
        return true;
    }
    if (k.key == SDLK_RETURN || k.key == SDLK_KP_ENTER) {
        open_editor();
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
    if (editing_ && t) return "Редактор объекта «" + t->name + "» · " + path_to_utf8(t->file.filename()) + " · Esc — к библиотеке";
    if (t) s += " · «" + t->name + "»: " + path_to_utf8(t->file.filename());
    return s;
}

} // namespace forge::editor_app

// --- the object's picture ---

namespace forge::editor_app {

// A file into the game's folder (pictures, sounds), under its own name; the
// same file already there is used as it is, another one of that name gets a
// number. The name it has there; empty when it cannot be copied.
std::string copy_into(const std::filesystem::path& source, const std::filesystem::path& folder, const char* what,
                      const CopyIn& via) {
    namespace fs = std::filesystem;
    if (via)
        if (std::optional<std::string> name = via(source, folder)) return *name;
    std::error_code ec;
    fs::create_directories(folder, ec);
    std::vector<u8> bytes;
    if (!read_file(source, bytes)) {
        FORGE_WARN("%s %s не читается", what, path_to_utf8(source).c_str());
        return {};
    }
    const std::string stem = path_to_utf8(source.stem()), ext = path_to_utf8(source.extension());
    fs::path target = folder / source.filename();
    for (int n = 2;; ++n) {
        std::vector<u8> there;
        if (!fs::exists(target, ec)) {
            if (!write_file_atomic(target, bytes)) {
                FORGE_WARN("%s не копируется в %s", what, path_to_utf8(target).c_str());
                return {};
            }
            break;
        }
        if (fs::equivalent(target, source, ec) || (read_file(target, there) && there == bytes)) break;
        target = folder / utf8_path(stem + " " + std::to_string(n) + ext);
    }
    return path_to_utf8(target.filename());
}

bool ObjectLibrary::set_picture(const std::filesystem::path& source) {
    const objects::Template* t = selected();
    if (!t || showing_shared()) return false;
    const std::string name = copy_into(source, library().pictures_folder(), "картинка", copy_in);
    if (name.empty()) return false;
    close_pictures();
    if (name == t->picture) return false;
    objects::Template after = *t;
    after.picture = name;
    change(std::move(after), "«" + t->name + "»: картинка");
    history_.seal();
    return true;
}

bool ObjectLibrary::set_frames(int n) {
    const objects::Template* t = selected();
    if (!t || showing_shared() || t->picture.empty()) return false;
    const u32 frames = static_cast<u32>(std::clamp(n, 1, static_cast<int>(objects::kMaxFrames)));
    if (frames == t->frames) return false;
    objects::Template after = *t;
    after.frames = frames;
    change(std::move(after), "«" + t->name + "»: кадров в картинке " + std::to_string(frames));
    history_.seal();
    return true;
}

std::string ObjectLibrary::hero_note(const objects::Template& t) {
    const objects::Library& lib = library();
    if (!lib.has_block(t, "hero")) return {};
    if (t.picture.empty()) return "Без картинки: герой рисуется как прежде.";
    if (t.frames != 1 && t.frames != 4)
        return "Кадров в картинке " + std::to_string(t.frames) + ", а у героя их 4 (стоит, шаг, шаг, в воздухе) или 1: герой рисуется как прежде.";
    const objects::Template* first = nullptr;
    usize n = 0;
    for (const objects::Template& o : lib.templates())
        if (lib.has_block(o, "hero") && !o.picture.empty() && (o.frames == 1 || o.frames == 4)) {
            ++n;
            if (!first || o.id < first->id) first = &o;
        }
    if (n < 2) return {};
    if (first->id == t.id) return "Объектов вида «Герой» с картинкой " + std::to_string(n) + ": героя рисует этот, первый по id.";
    return "Объектов вида «Герой» с картинкой " + std::to_string(n) + ": героя рисует «" + first->name + "», первый по id, а не этот.";
}

bool ObjectLibrary::clear_picture() {
    const objects::Template* t = selected();
    if (!t || t->picture.empty()) return false;
    objects::Template after = *t;
    after.picture.clear();
    change(std::move(after), "«" + t->name + "»: обычная картинка");
    history_.seal();
    return true;
}

void ObjectLibrary::open_pictures() {
    if (!selected()) return;
    set_menu("");
    pic_search_.clear();
    set(m_pics_search_, Rml::String(), "ol_pics_search");
    set(m_pics_open_, true, "ol_pics_open");
    rebuild_pictures();
}

void ObjectLibrary::close_pictures() { set(m_pics_open_, false, "ol_pics_open"); }

void ObjectLibrary::set_picture_search(const std::string& text) {
    if (text == pic_search_) return;
    pic_search_ = text;
    m_pics_search_ = text;
    rebuild_pictures();
}

bool ObjectLibrary::choose_picture(usize i) { return i < pic_files_.size() && set_picture(pic_files_[i]); }

void ObjectLibrary::rebuild_pictures() {
    // The images whose name or folder has every word of the search; the
    // first ones get thumbnails (decoded once).
    constexpr usize kShown = 60;
    constexpr u32 kThumb = 96;
    pic_files_.clear();
    m_pics_.clear();
    std::vector<std::filesystem::path> all = list_images ? list_images() : std::vector<std::filesystem::path>();
    const std::string needle = game::to_lower_utf8(pic_search_);
    usize matching = 0;
    for (const std::filesystem::path& file : all) {
        const std::string text = path_to_utf8(file);
        if (!needle.empty() && game::to_lower_utf8(path_to_utf8(file.parent_path().filename() / file.filename())).find(needle) == std::string::npos) continue;
        ++matching;
        if (pic_files_.size() >= kShown) continue;
        std::string& icon = pic_icons_[text];
        if (icon.empty()) {
            icon = "pic_" + std::to_string(pic_icons_.size());
            std::vector<u8> bytes;
            assets::CookedTexture image;
            if (read_file(file, bytes) && assets::decode_image(bytes, image)) {
                const assets::CookedTexture thumb = assets::fit_image(image, kThumb);
                ui_->set_image(icon, thumb.rgba8.data(), thumb.width, thumb.height);
            }
        }
        pic_files_.push_back(file);
        m_pics_.push_back({path_to_utf8(file.stem()), path_to_utf8(file.parent_path().filename()), "/memory/" + icon});
    }
    m_pics_note_ = all.empty() ? "В «Ресурсах» пока нет картинок: перетащите их в окно редактора на вкладке «Ресурсы»."
                   : matching == 0 ? "Ничего не нашлось."
                   : matching > pic_files_.size()
                       ? "Показаны первые " + std::to_string(pic_files_.size()) + " из " + std::to_string(matching) +
                             ": уточните поиск."
                       : "Картинка скопируется в папку игры и будет у этого объекта и всех его копий.";
    if (model_)
        for (const char* name : {"ol_pics", "ol_pics_note", "ol_pics_search"}) model_.DirtyVariable(name);
}

} // namespace forge::editor_app

// --- the object's sounds ---

namespace forge::editor_app {

namespace {

std::string lower_ext(const std::filesystem::path& file) {
    std::string ext = path_to_utf8(file.extension());
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

} // namespace

bool ObjectLibrary::set_sound(int prop, const std::filesystem::path& source) {
    const objects::Template* t = selected();
    if (!t || showing_shared() || prop < 0 || prop >= static_cast<int>(prop_refs_.size())) return false;
    const objects::PropDef& p = *prop_refs_[static_cast<usize>(prop)];
    if (p.asset != "sound") return false;
    if (!audio::readable(source)) {
        set(m_snds_note_, Rml::String("Игра читает WAV и OGG. Этот файл переведите в «Ресурсах»: правая кнопка → «Конвертировать…» → OGG."),
            "ol_snds_note");
        FORGE_WARN("«%s»: игра читает только WAV и OGG — переведите его через «Конвертировать…»", path_to_utf8(source.filename()).c_str());
        return false;
    }
    if (std::string error; !audio::load(source, &error)) {
        FORGE_WARN("«%s» не звучит: %s", path_to_utf8(source.filename()).c_str(), error.c_str());
        return false;
    }
    const std::string name = copy_into(source, library().sounds_folder(), "звук", copy_in);
    if (name.empty()) return false;
    close_sounds();
    const std::string json = objects::Library::parse(p, name).value_or("\"\"");
    if (json == library().value(*t, p)) return false;
    change(library().with_value(*t, p.id, json), "«" + t->name + "»: " + p.name);
    history_.seal();
    return true;
}

bool ObjectLibrary::clear_sound(int prop) {
    const objects::Template* t = selected();
    if (!t || showing_shared() || prop < 0 || prop >= static_cast<int>(prop_refs_.size())) return false;
    const objects::PropDef& p = *prop_refs_[static_cast<usize>(prop)];
    if (p.asset != "sound" || library().value(*t, p) == "\"\"") return false;
    change(library().with_value(*t, p.id, "\"\""), "«" + t->name + "»: " + p.name + " — " + (p.empty.empty() ? "нет" : p.empty));
    history_.seal();
    return true;
}

void ObjectLibrary::open_sounds(int prop) {
    if (!selected() || prop < 0 || prop >= static_cast<int>(prop_refs_.size())) return;
    set_menu("");
    snd_prop_ = prop;
    snd_search_.clear();
    set(m_snds_search_, Rml::String(), "ol_snds_search");
    set(m_snds_title_, Rml::String("Звук: " + prop_refs_[static_cast<usize>(prop)]->name), "ol_snds_title");
    set(m_snds_open_, true, "ol_snds_open");
    rebuild_sounds();
}

void ObjectLibrary::close_sounds() { set(m_snds_open_, false, "ol_snds_open"); }

void ObjectLibrary::set_sound_search(const std::string& text) {
    if (text == snd_search_) return;
    snd_search_ = text;
    m_snds_search_ = text;
    rebuild_sounds();
}

bool ObjectLibrary::choose_sound(usize i) { return i < snd_files_.size() && set_sound(snd_prop_, snd_files_[i]); }

bool ObjectLibrary::play_file(const std::filesystem::path& file) {
    std::string error;
    audio::ClipPtr clip = audio::load(file, &error);
    if (!clip) {
        FORGE_WARN("«%s» не звучит: %s", path_to_utf8(file.filename()).c_str(), error.c_str());
        return false;
    }
    // The device opens on the first listen; without one, nothing is heard
    // but the sound still counts as played.
    if (!preview_open_) {
        preview_open_ = true;
        if (!silent && !preview_.open()) FORGE_WARN("Звука нет: не открылось устройство вывода");
    }
    preview_.stop_all();
    preview_.play(clip);
    return true;
}

bool ObjectLibrary::play_prop_sound(int prop) {
    const objects::Template* t = selected();
    if (!t || prop < 0 || prop >= static_cast<int>(prop_refs_.size())) return false;
    const objects::PropDef& p = *prop_refs_[static_cast<usize>(prop)];
    const objects::Library& lib = shown();
    std::string json = lib.value(*t, p);
    if (json.size() < 3) return false;
    return play_file(lib.sound_file(json.substr(1, json.size() - 2)));
}

bool ObjectLibrary::preview_sound(usize i) { return i < snd_files_.size() && play_file(snd_files_[i]); }

void ObjectLibrary::rebuild_sounds() {
    constexpr usize kShown = 80;
    snd_files_.clear();
    m_snds_.clear();
    const std::vector<std::filesystem::path> all = list_sounds ? list_sounds() : std::vector<std::filesystem::path>();
    const std::string needle = game::to_lower_utf8(snd_search_);
    usize matching = 0;
    for (const std::filesystem::path& file : all) {
        if (!needle.empty() && game::to_lower_utf8(path_to_utf8(file.parent_path().filename() / file.filename())).find(needle) == std::string::npos)
            continue;
        ++matching;
        if (snd_files_.size() >= kShown) continue;
        std::string format = lower_ext(file);
        if (!format.empty()) format = format.substr(1);
        for (char& c : format) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        snd_files_.push_back(file);
        m_snds_.push_back({path_to_utf8(file.stem()), path_to_utf8(file.parent_path().filename()), format, audio::readable(file)});
    }
    m_snds_note_ = all.empty() ? "В «Ресурсах» пока нет звуков: перетащите их в окно редактора на вкладке «Ресурсы»."
                   : matching == 0 ? "Ничего не нашлось."
                   : matching > snd_files_.size()
                       ? "Показаны первые " + std::to_string(snd_files_.size()) + " из " + std::to_string(matching) + ": уточните поиск."
                       : "Кнопка слева от названия — послушать. Серые игра пока не читает: их переводят в OGG через «Конвертировать…». Выбранный звук скопируется в папку игры и будет у этого объекта и всех его копий.";
    if (model_)
        for (const char* name : {"ol_snds", "ol_snds_note", "ol_snds_search", "ol_snds_title"}) model_.DirtyVariable(name);
}

} // namespace forge::editor_app
