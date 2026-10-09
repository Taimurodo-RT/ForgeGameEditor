// The «Интерфейс» tab's template library (13.12): the window over the tab,
// the pictures of the templates, and taking one (a construction onto the
// open screen, a screen template as a new screen).

#include "ui_editor.h"

#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/game/dialogue.h"
#include "forge/game/vars.h"

#include <algorithm>
#include <cmath>

namespace forge::editor_app {

namespace d = editor::design;

namespace {

// Under a construction in its picture: the editor's dark, so every look reads on it.
constexpr d::Color kGround{0x23, 0x27, 0x2e, 255};

std::string kind_word(const std::string& section) {
    return section == "screen" ? "screen" : "construction";
}

// What a picture shows in its texts, bars and conditions: something to look at (a shop where two things of three
// are bought, a bar more than half full) rather than the zeros of a new game.
game::Vars picture_vars(const std::vector<std::string>& names) {
    game::Vars v;
    for (const std::string& name : names) {
        f64 value = 5;
        if (name == "inv.coins") value = 12;
        else if (name.find("max") != std::string::npos) value = 10;
        else if (name.rfind("hero.", 0) == 0) value = 7;
        else if (name.rfind("settings.", 0) == 0) value = 70;
        else if (name.rfind("quest.", 0) == 0 || name == "game.difficulty") value = name == "game.difficulty" ? 2 : 1;
        else if (name.rfind("inv.", 0) == 0) value = 3;
        v.set(name, value);
    }
    return v;
}

// «a», «b» and «c».
std::string quoted_list(const std::vector<std::string>& names) {
    std::string out;
    for (usize i = 0; i < names.size(); ++i) {
        if (i) out += i + 1 == names.size() ? " и " : ", ";
        out += "«" + names[i] + "»";
    }
    return out;
}

void add_once(std::vector<std::string>& list, const std::string& v) {
    if (!v.empty() && std::find(list.begin(), list.end(), v) == list.end()) list.push_back(v);
}

std::string trimmed(const std::string& s) {
    const usize a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

} // namespace

// --- the templates and their pictures ---------------------------------------

void UiEditor::load_templates() {
    if (!ui_) return;
    if (!tpl_.loaded) {
        tpl_.loaded = true;
        std::vector<std::string> errors;
        builtin_ = d::load_templates(ui_->root() / "templates", &errors);
        for (const std::string& e : errors) FORGE_WARN("Шаблоны: %s", e.c_str());
        builtin_count_ = builtin_.size();
    }
    // The game's own, read again each time: saved or taken out here, or the folder changed by hand.
    std::vector<d::Template> own;
    std::error_code ec;
    if (std::filesystem::exists(own_templates_dir() / "templates.json", ec)) {
        std::vector<std::string> errors;
        own = d::load_templates(own_templates_dir(), &errors);
        for (const std::string& e : errors) FORGE_WARN("Свои шаблоны: %s", e.c_str());
    }
    // An own template's picture is kept while nothing of it changed; the others are drawn anew, under new names.
    std::vector<TemplatePicture> pictures(builtin_count_ + own.size());
    for (usize i = 0; i < builtin_count_ && i < pictures_.size(); ++i) pictures[i] = std::move(pictures_[i]);
    for (usize j = 0; j < own.size(); ++j) {
        own[j].info.own = true;
        for (usize i = builtin_count_; i < templates_.size() && i < pictures_.size(); ++i)
            if (pictures_[i].context && templates_[i].info == own[j].info &&
                d::save_screen(templates_[i].screen) == d::save_screen(own[j].screen)) {
                pictures[builtin_count_ + j] = std::move(pictures_[i]);
                pictures_[i] = TemplatePicture{};
                break;
            }
    }
    for (usize i = builtin_count_; i < pictures_.size(); ++i) drop_picture(pictures_[i]);
    templates_ = builtin_;
    templates_.insert(templates_.end(), std::make_move_iterator(own.begin()), std::make_move_iterator(own.end()));
    pictures_ = std::move(pictures);
    tpl_.needs_for = -1;
}

void UiEditor::draw_picture(TemplatePicture& p, const d::Template& t) {
    if (p.context || !ui_) return;
    // A name never used before: the textures are kept by name, and an own template's picture is drawn again.
    p.image = "ui-tpl-" + std::to_string(++picture_serial_);
    p.context = ui_->create_context("ue-" + p.image, kTemplateThumbW, kTemplateThumbH);
    if (!p.context) return;
    p.context->SetDensityIndependentPixelRatio(1.0f);
    ui_->set_offscreen(p.context, p.image);
    p.page = std::make_unique<game::GameScreens>();
    p.page->set_items(tpl_.items);
    p.page->set_quests(game_quests ? game_quests() : nullptr);
    // At an address in the game's ui folder: its pictures are found as the game finds them (../pictures/...).
    const d::Screen s = d::template_preview(t, kGround);
    if (!p.page->load_page(p.context, "template", d::screen_html(s, html_options()), path_to_utf8(html_path("template")))) {
        FORGE_WARN("Шаблон %s не построился", t.info.file.c_str());
        return;
    }
    p.page->show("template", true);
    p.page->update(picture_vars(p.page->variables("template")), false, false, static_cast<int>(kTemplateThumbW),
                   static_cast<int>(kTemplateThumbH));
    // A few frames: laid out, pictures and letters loaded, drawn; then the context sleeps and keeps its picture.
    p.frames = 3;
    p.drawn = false;
}

void UiEditor::drop_picture(TemplatePicture& p) {
    if (p.page) p.page->unload();
    p.page.reset();
    if (p.context && ui_) ui_->destroy_context(p.context);
    p = TemplatePicture{};
}

void UiEditor::draw_template(usize index) { draw_picture(pictures_[index], templates_[index]); }

void UiEditor::update_templates() {
    if (!ui_) return;
    auto tick = [&](TemplatePicture& p) {
        if (!p.context || p.frames <= 0) return;
        if (--p.frames == 0) {
            ui_->set_active(p.context, false);
            p.drawn = true;
        }
    };
    for (TemplatePicture& p : pictures_) tick(p);
    tick(save_picture_);
    if (!tpl_.open) return;
    // New pictures a few at a time: the window opens at once, the cards fill in.
    int made = 0;
    for (usize i = 0; i < pictures_.size() && made < 6; ++i)
        if (!pictures_[i].context) {
            draw_template(i);
            ++made;
        }
    if (made) refresh_templates();
}

void UiEditor::drop_template_pictures() {
    for (TemplatePicture& p : pictures_) drop_picture(p);
    drop_picture(save_picture_);
}

bool UiEditor::template_drawn(usize index) const { return index < pictures_.size() && pictures_[index].drawn; }

bool UiEditor::read_template_pixel(usize index, u32 x, u32 y, u8 rgba[4]) const {
    return template_drawn(index) && ui_ && ui_->read_pixel(pictures_[index].context, x, y, rgba);
}

// --- the window --------------------------------------------------------------

void UiEditor::open_templates(const std::string& section) {
    if (checking_) return; // as «Создать»: the screen is the game's while it is checked
    close_picker(true);
    if (!menu_.empty()) open_menu("", 0, 0);
    if (!tpl_.open) tpl_.items = game_items ? game_items() : std::vector<game::ScreenItem>{};
    load_templates();
    tpl_.open = true;
    tpl_.saving = false;
    tpl_.confirm_remove = false;
    tpl_.note.clear();
    if (!section.empty()) {
        tpl_.section = kind_word(section);
        tpl_.group.clear();
        tpl_.search.clear();
    }
    // The pictures of the game's things as they are now: the lists' pictures are drawn again.
    for (usize i = 0; i < pictures_.size(); ++i)
        if (pictures_[i].context) {
            ui_->set_active(pictures_[i].context, true);
            pictures_[i].page->set_items(tpl_.items);
            pictures_[i].page->update(picture_vars(pictures_[i].page->variables("template")), false, false,
                                      static_cast<int>(kTemplateThumbW), static_cast<int>(kTemplateThumbH));
            pictures_[i].frames = 3;
        }
    const std::vector<usize> listed = templates_listed();
    if (std::find(listed.begin(), listed.end(), static_cast<usize>(std::max(tpl_.selected, 0))) == listed.end() || tpl_.selected < 0)
        tpl_.selected = listed.empty() ? -1 : static_cast<int>(listed.front());
    refresh_templates();
}

void UiEditor::close_templates() {
    if (!tpl_.open) return;
    tpl_.open = false;
    tpl_.saving = false; // a form left without saving: nothing written, nothing in the history
    tpl_.confirm_remove = false;
    // The window's fields are hidden with it, not gone: one would keep the keyboard (Ctrl+Z, Delete, the arrows).
    if (Rml::Element* f = context_ ? context_->GetFocusElement() : nullptr; f && (f->GetId() == "ue-tpl-search" || f->GetId() == "ue-tpl-save-title"))
        f->Blur();
    refresh_templates();
}

bool UiEditor::template_matches(const d::Template& t) const {
    if (tpl_.search.empty()) return d::template_kind_word(t.info.kind) == tpl_.section && (tpl_.group.empty() || t.info.group == tpl_.group);
    // Every word typed is found somewhere: in the title, the look, the group, what it is, its own words.
    std::string all = t.info.title + " " + t.info.variant + " " + t.info.group + " " + t.info.about;
    for (const std::string& w : t.info.words) all += " " + w;
    all = game::to_lower_utf8(all);
    for (const std::string& w : game::split_words(game::to_lower_utf8(tpl_.search)))
        if (all.find(w) == std::string::npos) return false;
    return true;
}

std::vector<usize> UiEditor::templates_listed() const {
    std::vector<usize> out;
    for (usize i = 0; i < templates_.size(); ++i)
        if (template_matches(templates_[i])) out.push_back(i);
    return out;
}

void UiEditor::templates_show(const std::string& section, const std::string& group) {
    tpl_.section = kind_word(section);
    tpl_.group = group;
    tpl_.search.clear();
    const std::vector<usize> listed = templates_listed();
    if (!listed.empty() && std::find(listed.begin(), listed.end(), static_cast<usize>(std::max(tpl_.selected, 0))) == listed.end())
        tpl_.selected = static_cast<int>(listed.front());
    refresh_templates();
}

void UiEditor::templates_search(const std::string& text) {
    if (text == tpl_.search) return;
    tpl_.search = text;
    const std::vector<usize> listed = templates_listed();
    if (!listed.empty() && (tpl_.selected < 0 || std::find(listed.begin(), listed.end(), static_cast<usize>(tpl_.selected)) == listed.end()))
        tpl_.selected = static_cast<int>(listed.front());
    refresh_templates();
}

bool UiEditor::select_template(usize index) {
    if (index >= templates_.size()) return false;
    tpl_.selected = static_cast<int>(index);
    tpl_.note.clear();
    tpl_.saving = false; // a card picked: its details in place of the form
    tpl_.confirm_remove = false;
    refresh_templates();
    return true;
}

u32 UiEditor::insert_template(usize index) {
    if (index >= templates_.size()) return 0;
    if (screen_.library) {
        tpl_.note = "Шаблоны ставятся на экраны игры, а это «Компоненты». Откройте экран слева или сделайте новый.";
        refresh_templates();
        return 0;
    }
    if (checking_) return 0;
    const std::vector<std::string> needs = template_needs(index);
    const d::Template& t = templates_[index];
    const std::string before = d::save_screen(screen_);
    d::Node n = d::template_layer(t, screen_, &library_);
    {
        // Copies of the game's components as the components are now, the game's colours and text styles as they are.
        d::Screen scratch;
        scratch.next_id = screen_.next_id;
        scratch.root.children.push_back(std::move(n));
        d::sync_instances(scratch, library_);
        d::apply_styles(scratch, library_);
        n = std::move(scratch.root.children.front());
        screen_.next_id = scratch.next_id;
    }
    const std::string label = "Из шаблонов: " + t.info.title;
    const std::string title = t.info.title;
    close_templates();
    u32 id = 0;
    if (n.w == screen_.width && n.h == screen_.height) {
        // A whole screen onto one of its size: where it stands on its own screen.
        n.x = 0;
        n.y = 0;
        id = n.id;
        screen_.root.children.push_back(std::move(n));
        selection_ = {id};
        page_dirty_ = true;
        commit(before, label);
        select({id});
    } else {
        id = place_new(std::move(n), before, label);
    }
    note_needs(needs, "«" + title + "» на экране.");
    return id;
}

std::string UiEditor::screen_from_template(usize index) {
    if (index >= templates_.size() || checking_) return {};
    const d::Template& t = templates_[index];
    // The file: the title in small letters ("главное_меню"), a number when taken; the title: a number when taken.
    const std::vector<std::string> all = screens();
    std::string base;
    for (char c : game::to_lower_utf8(t.info.title)) base += c == ' ' ? '_' : c;
    std::string name = base;
    for (u32 n = 2; std::find(all.begin(), all.end(), name) != all.end() || name == kLibrary; ++n) name = base + std::to_string(n);
    std::vector<std::string> titles;
    for (const std::string& other : all) {
        std::vector<u8> bytes;
        d::Screen s;
        if (read_file(json_path(other), bytes) &&
            d::load_screen(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), s))
            titles.push_back(s.title.empty() ? other : s.title);
    }
    std::string title = t.info.title;
    for (u32 n = 2; std::find(titles.begin(), titles.end(), title) != titles.end(); ++n) title = t.info.title + " " + std::to_string(n);
    const std::vector<std::string> needs = template_needs(index);
    d::Screen s = d::template_screen(t, title, &library_);
    std::string note;
    if (s.show == d::ScreenShow::Menu) {
        // One main menu: a second one comes as a window, the author decides.
        const std::string menu = menu_screen();
        if (!menu.empty()) {
            s.show = d::ScreenShow::Command;
            s.over = d::WindowOver::Any;
            note = "Главное меню у игры уже есть («" + menu + "»), поэтому «" + title +
                   "» сделан окном. Чтобы он стал главным меню, выберите справа «Когда видно» → «Главное меню игры».";
        }
    }
    close_templates();
    if (!write(name, s)) return {};
    open(name); // its copies of components synced with the game's library, as any screen opened
    tpl_.note = note;
    if (!note.empty()) show_move_note(note);
    note_needs(needs, note.empty() ? "Экран «" + title + "» готов." : note);
    return name;
}

bool UiEditor::take_template(usize index) {
    if (index >= templates_.size()) return false;
    if (templates_[index].info.kind == d::TemplateKind::Screen) return !screen_from_template(index).empty();
    return insert_template(index) != 0;
}

// --- what a template needs of the game -------------------------------------

std::vector<std::string> UiEditor::template_needs(usize index) {
    std::vector<std::string> out;
    if (index >= templates_.size()) return out;
    if (tpl_.needs_for == static_cast<int>(index)) return tpl_.needs;
    const d::Template& t = templates_[index];
    const bool whole = t.info.kind == d::TemplateKind::Screen;

    // The game's components, colours and text styles it is linked to.
    std::vector<std::string> dropped;
    {
        d::Node probe = t.screen.root;
        d::keep_links(probe, &library_, &dropped);
    }
    if (!dropped.empty()) {
        std::string list;
        for (usize i = 0; i < dropped.size(); ++i) list += (i ? ", " : "") + dropped[i];
        out.push_back("В этой игре нет " + list + ": такие слои станут обычными, вид останется.");
    }

    // What it names by name or path.
    std::vector<std::string> pictures, sounds, opens, messages, changed;
    const std::vector<std::string> have = screens();
    std::error_code ec;
    auto picture = [&](const std::string& path) {
        if (!path.empty() && !std::filesystem::exists(game_dir_ / utf8_path(path), ec)) add_once(pictures, path);
    };
    auto sound = [&](const std::string& name) {
        if (!name.empty() && name != "none" && !std::filesystem::exists(sounds_folder() / utf8_path(name), ec)) add_once(sounds, name);
    };
    auto visit = [&](auto&& self, const d::Node& n) -> void {
        for (const d::Paint& p : n.fills)
            if (p.kind == d::PaintKind::Image) picture(p.image);
        picture(n.frame.image);
        picture(n.mask.image);
        sound(n.click_sound);
        for (const d::Action& a : n.on_click) {
            if ((a.kind == d::ActionKind::Show || a.kind == d::ActionKind::Hide || a.kind == d::ActionKind::Toggle) &&
                std::find(have.begin(), have.end(), a.target) == have.end())
                add_once(opens, a.target);
            if (a.kind == d::ActionKind::Message) add_once(messages, a.target);
            if (a.kind == d::ActionKind::Change) {
                std::string error;
                game::Expr::parse_actions(a.target, &error).collect(nullptr, &changed);
            }
        }
        for (const d::Node& c : n.children) self(self, c);
    };
    visit(visit, t.screen.root);
    if (whole) {
        sound(t.screen.music);
        sound(t.screen.button_sound);
    }
    if (!pictures.empty())
        out.push_back("Нет картинок " + quoted_list(pictures) + ": пока их нет в папке игры, слои будут без них.");
    if (!sounds.empty()) out.push_back("Нет звуков " + quoted_list(sounds) + " в папке sounds: без них будет тихо.");
    if (!opens.empty())
        out.push_back("Кнопки открывают экраны " + quoted_list(opens) +
                      ", а их в игре нет: сделайте такие экраны или выберите другие в «При нажатии».");

    // The game's variables it reads or changes: things the game does not have, values nobody keeps.
    draw_template(index);
    std::vector<std::string> used = pictures_[index].page ? pictures_[index].page->variables("template") : std::vector<std::string>{};
    used.insert(used.end(), changed.begin(), changed.end());
    std::vector<std::string> known;
    if (game_values)
        for (const auto& [name, label] : game_values()) known.push_back(name);
    for (const game::ScreenItem& it : tpl_.items) known.push_back("inv." + it.id);
    std::vector<std::string> things, idle;
    for (const std::string& v : used) {
        if (std::find(known.begin(), known.end(), v) != known.end()) continue;
        if (v.rfind("inv.", 0) == 0) add_once(things, v.substr(4));
        else if (std::find(changed.begin(), changed.end(), v) == changed.end()) add_once(idle, v);
    }
    if (!things.empty())
        out.push_back("В игре нет вещей " + quoted_list(things) +
                      ": их счёт меняется, но в инвентаре их не будет. Поставьте в «При нажатии» вещи игры или добавьте такие в «Объекты».");
    if (!idle.empty())
        out.push_back("Значения " + quoted_list(idle) + " игра сама не ведёт: пока их не меняет «Логика» или кнопка, они 0.");
    if (!messages.empty())
        out.push_back("Кнопки посылают «Логике» сообщения " + quoted_list(messages) +
                      ": что они делают, задаёт схема «При сообщении».");
    tpl_.needs_for = static_cast<int>(index);
    tpl_.needs = out;
    return out;
}

void UiEditor::note_needs(const std::vector<std::string>& needs, const std::string& done) {
    if (needs.empty()) return;
    std::string note = done;
    for (const std::string& n : needs) note += " " + n;
    tpl_.note = note;
    show_move_note(note);
}

// --- the author's own («Сохранить как шаблон») ------------------------------

d::Template UiEditor::template_to_save() const {
    const d::Node* layer = tpl_.save_layer ? d::find(screen_.root, tpl_.save_layer) : nullptr;
    return d::own_template(screen_, layer, trimmed(tpl_.save_title), tpl_.save_group);
}

int UiEditor::own_template_titled(const std::string& title, d::TemplateKind kind) const {
    const std::string low = game::to_lower_utf8(trimmed(title));
    for (usize i = builtin_count_; i < templates_.size(); ++i)
        if (templates_[i].info.kind == kind && game::to_lower_utf8(templates_[i].info.title) == low) return static_cast<int>(i);
    return -1;
}

bool UiEditor::begin_save_template(u32 layer) {
    if (checking_ || screen_.library || !ui_) return false;
    const d::Node* n = layer ? d::find(screen_.root, layer) : nullptr;
    if (layer && (!n || layer == screen_.root.id)) return false;
    open_templates(n ? "construction" : "screen");
    // The author's own of the kind beside the form: what is there already, what a title would replace.
    tpl_.group = d::kOwnGroup;
    tpl_.saving = true;
    tpl_.save_layer = n ? layer : 0;
    tpl_.save_title = n ? n->name : screen_.title;
    tpl_.save_group = d::kOwnGroup;
    drop_picture(save_picture_);
    draw_picture(save_picture_, template_to_save());
    refresh_templates();
    return true;
}

void UiEditor::set_save_template(const std::string& title, const std::string& group) {
    if (!tpl_.saving) return;
    tpl_.save_title = title;
    const auto& groups = d::template_groups(tpl_.save_layer ? d::TemplateKind::Construction : d::TemplateKind::Screen);
    if (std::find(groups.begin(), groups.end(), group) != groups.end()) tpl_.save_group = group;
    tpl_.note.clear();
    refresh_templates();
}

int UiEditor::save_template(bool replace) {
    if (!tpl_.open || !tpl_.saving) return -1;
    const d::TemplateKind kind = tpl_.save_layer ? d::TemplateKind::Construction : d::TemplateKind::Screen;
    std::string title = trimmed(tpl_.save_title);
    auto refuse = [&](std::string why) {
        tpl_.note = std::move(why);
        refresh_templates();
        return -1;
    };
    if (title.empty()) return refuse("Назовите шаблон: по названию его найдёт поиск.");
    if (tpl_.save_layer && !d::find(screen_.root, tpl_.save_layer)) return refuse("Этого слоя на экране уже нет.");
    // The same title: in its place when asked, else beside it with a number («Витрина 2»), never over it unasked.
    std::string file;
    if (const int same = own_template_titled(title, kind); same >= 0) {
        if (replace) {
            file = templates_[static_cast<usize>(same)].info.file;
            title = templates_[static_cast<usize>(same)].info.title;
        } else {
            const std::string base = title;
            for (u32 n = 2; own_template_titled(title, kind) >= 0; ++n) title = base + " " + std::to_string(n);
        }
    }
    tpl_.save_title = title;
    const d::Template t = template_to_save();
    std::string error;
    const std::string saved = d::save_own_template(own_templates_dir(), t, file, &error);
    if (saved.empty()) return refuse("Шаблон не сохранился: " + error + ".");
    tpl_.saving = false;
    load_templates();
    int index = -1;
    for (usize i = builtin_count_; i < templates_.size(); ++i)
        if (templates_[i].info.file == saved) index = static_cast<int>(i);
    tpl_.section = d::template_kind_word(kind);
    tpl_.group = t.info.group;
    tpl_.search.clear();
    tpl_.selected = index;
    tpl_.note = "«" + title + "» " + (file.empty() ? "сохранён" : "заменён") + " в шаблонах игры, группа «" + t.info.group +
                "». Файл ui/templates/" + saved + ".json переносится вместе с проектом.";
    refresh_templates();
    return index;
}

bool UiEditor::remove_template(usize index) {
    if (index >= templates_.size() || !templates_[index].info.own) return false;
    const std::string title = templates_[index].info.title;
    if (!d::remove_own_template(own_templates_dir(), templates_[index].info.file)) {
        tpl_.note = "«" + title + "» не удалился: файлы ui/templates заняты или прочитаны не так.";
        refresh_templates();
        return false;
    }
    load_templates();
    const std::vector<usize> listed = templates_listed();
    tpl_.selected = listed.empty() ? -1 : static_cast<int>(listed.front());
    tpl_.confirm_remove = false;
    tpl_.note = "«" + title + "» больше нет в шаблонах. Экраны, где он уже стоит, не изменились.";
    refresh_templates();
    return true;
}

void UiEditor::templates_enter() {
    if (!tpl_.open) return;
    if (tpl_.saving) save_template(false);
    else if (tpl_.selected >= 0) take_template(static_cast<usize>(tpl_.selected));
}

void UiEditor::refresh_templates() {
    m_tpl_open_ = tpl_.open;
    m_tpl_section_ = tpl_.section;
    m_tpl_group_ = tpl_.group;
    m_tpl_search_ = tpl_.search;
    m_tpl_note_ = tpl_.note;
    // Closed, the lists stay as they were: the window's rows go with it, not before it (no row reads past its list).
    if (tpl_.open) {
        m_tpl_cards_.clear();
        m_tpl_groups_.clear();
        m_tpl_variants_.clear();
        m_tpl_sel_ = TplSelected{};
        m_tpl_count_c_ = m_tpl_count_s_ = 0;
        const std::vector<usize> listed = templates_listed();
        for (usize i : listed) {
            const d::Template& t = templates_[i];
            TplCard c;
            c.index = static_cast<int>(i);
            c.title = t.info.title;
            c.variant = t.info.variant;
            c.group = t.info.group;
            if (pictures_[i].context) c.thumb = "/gpu/" + pictures_[i].image;
            c.selected = tpl_.selected == static_cast<int>(i) && !tpl_.saving;
            c.own = t.info.own;
            m_tpl_cards_.push_back(std::move(c));
        }
        // The groups: how many there are (with a search, how many it found).
        for (d::TemplateKind kind : {d::TemplateKind::Construction, d::TemplateKind::Screen})
            for (const std::string& g : d::template_groups(kind)) {
                TplGroup row;
                row.section = d::template_kind_word(kind);
                row.name = g;
                for (usize i = 0; i < templates_.size(); ++i)
                    if (templates_[i].info.kind == kind && templates_[i].info.group == g &&
                        (tpl_.search.empty() || std::find(listed.begin(), listed.end(), i) != listed.end()))
                        ++row.count;
                row.selected = tpl_.search.empty() && tpl_.section == row.section && tpl_.group == g;
                (kind == d::TemplateKind::Construction ? m_tpl_count_c_ : m_tpl_count_s_) += row.count;
                m_tpl_groups_.push_back(std::move(row));
            }
        if (!tpl_.search.empty())
            m_tpl_heading_ = listed.empty() ? "Ничего не нашлось" : "Найдено: " + std::to_string(listed.size());
        else
            m_tpl_heading_ = tpl_.group.empty() ? (tpl_.section == "screen" ? "Экраны" : "Конструкции") : tpl_.group;
        m_tpl_empty_ = tpl_.search.empty() && tpl_.group == d::kOwnGroup
                           ? "Здесь будут ваши шаблоны. Правый щелчок по слою → «Сохранить как шаблон…», по пустому месту экрана → «Сохранить экран как шаблон…»."
                           : "Ничего не нашлось. Попробуйте другое слово: «меню», «кнопки», «предметы», «здоровье».";
        m_tpl_needs_.clear();
        if (tpl_.selected >= 0 && static_cast<usize>(tpl_.selected) < templates_.size()) {
            const d::Template& t = templates_[static_cast<usize>(tpl_.selected)];
            m_tpl_sel_.any = true;
            m_tpl_sel_.own = t.info.own;
            m_tpl_sel_.screen = t.info.kind == d::TemplateKind::Screen;
            m_tpl_sel_.can_insert = !screen_.library;
            m_tpl_sel_.title = t.info.title;
            m_tpl_sel_.variant = t.info.variant;
            m_tpl_sel_.group = t.info.group;
            m_tpl_sel_.about = t.info.about;
            m_tpl_sel_.kind = m_tpl_sel_.screen ? "Экран" : "Конструкция";
            if (pictures_[static_cast<usize>(tpl_.selected)].context)
                m_tpl_sel_.thumb = "/gpu/" + pictures_[static_cast<usize>(tpl_.selected)].image;
            // Its other looks: the same title of the same kind.
            for (usize i = 0; i < templates_.size(); ++i)
                if (templates_[i].info.kind == t.info.kind && templates_[i].info.title == t.info.title && !templates_[i].info.variant.empty())
                    m_tpl_variants_.push_back({static_cast<int>(i), templates_[i].info.variant, static_cast<int>(i) == tpl_.selected});
            if (m_tpl_variants_.size() < 2) m_tpl_variants_.clear();
            if (!tpl_.saving)
                for (const std::string& n : template_needs(static_cast<usize>(tpl_.selected))) m_tpl_needs_.push_back(n);
        }
        // The form: what is saved, under what title and group, and whether the title is taken.
        m_tpl_save_ = TplSave{};
        m_tpl_save_groups_.clear();
        if (tpl_.saving) {
            const d::TemplateKind kind = tpl_.save_layer ? d::TemplateKind::Construction : d::TemplateKind::Screen;
            const d::Node* layer = tpl_.save_layer ? d::find(screen_.root, tpl_.save_layer) : nullptr;
            m_tpl_save_.on = true;
            m_tpl_save_.screen = kind == d::TemplateKind::Screen;
            m_tpl_save_.title = tpl_.save_title;
            m_tpl_save_.group = tpl_.save_group;
            m_tpl_save_.what = layer ? "Конструкция: слой «" + layer->name + "» с экрана «" + screen_.title + "», как он выглядит сейчас."
                                     : "Экран «" + screen_.title + "» целиком: его слои и настройки (когда виден, пауза, Esc, затемнение).";
            m_tpl_save_.can = !trimmed(tpl_.save_title).empty() && (!tpl_.save_layer || layer);
            if (const int same = own_template_titled(tpl_.save_title, kind); same >= 0)
                m_tpl_save_.clash = "«" + templates_[static_cast<usize>(same)].info.title +
                                    "» уже есть среди ваших. «Заменить» запишет новый на его место (экраны, где он стоит, не изменятся), «Сохранить рядом» добавит «" +
                                    trimmed(tpl_.save_title) + " 2».";
            if (save_picture_.context) m_tpl_save_.thumb = "/gpu/" + save_picture_.image;
            for (const std::string& g : d::template_groups(kind)) m_tpl_save_groups_.push_back(g);
        }
        m_tpl_confirm_ = tpl_.confirm_remove;
    }
    for (const char* v : {"ue_tpl_open", "ue_tpl_section", "ue_tpl_group", "ue_tpl_search", "ue_tpl_note", "ue_tpl_cards",
                          "ue_tpl_groups", "ue_tpl_variants", "ue_tpl_sel", "ue_tpl_count_c", "ue_tpl_count_s", "ue_tpl_heading",
                          "ue_tpl_empty", "ue_tpl_needs", "ue_tpl_save", "ue_tpl_save_groups", "ue_tpl_confirm"})
        dirty(v);
}

void UiEditor::bind_templates(Rml::DataModelConstructor& model) {
    if (auto s = model.RegisterStruct<TplCard>()) {
        s.RegisterMember("index", &TplCard::index);
        s.RegisterMember("title", &TplCard::title);
        s.RegisterMember("variant", &TplCard::variant);
        s.RegisterMember("group", &TplCard::group);
        s.RegisterMember("thumb", &TplCard::thumb);
        s.RegisterMember("selected", &TplCard::selected);
        s.RegisterMember("own", &TplCard::own);
    }
    model.RegisterArray<std::vector<TplCard>>();
    if (auto s = model.RegisterStruct<TplGroup>()) {
        s.RegisterMember("section", &TplGroup::section);
        s.RegisterMember("name", &TplGroup::name);
        s.RegisterMember("count", &TplGroup::count);
        s.RegisterMember("selected", &TplGroup::selected);
    }
    model.RegisterArray<std::vector<TplGroup>>();
    if (auto s = model.RegisterStruct<TplVariant>()) {
        s.RegisterMember("index", &TplVariant::index);
        s.RegisterMember("name", &TplVariant::name);
        s.RegisterMember("selected", &TplVariant::selected);
    }
    model.RegisterArray<std::vector<TplVariant>>();
    if (auto s = model.RegisterStruct<TplSelected>()) {
        s.RegisterMember("any", &TplSelected::any);
        s.RegisterMember("screen", &TplSelected::screen);
        s.RegisterMember("can_insert", &TplSelected::can_insert);
        s.RegisterMember("own", &TplSelected::own);
        s.RegisterMember("title", &TplSelected::title);
        s.RegisterMember("variant", &TplSelected::variant);
        s.RegisterMember("group", &TplSelected::group);
        s.RegisterMember("about", &TplSelected::about);
        s.RegisterMember("thumb", &TplSelected::thumb);
        s.RegisterMember("kind", &TplSelected::kind);
    }
    if (auto s = model.RegisterStruct<TplSave>()) {
        s.RegisterMember("on", &TplSave::on);
        s.RegisterMember("screen", &TplSave::screen);
        s.RegisterMember("can", &TplSave::can);
        s.RegisterMember("title", &TplSave::title);
        s.RegisterMember("group", &TplSave::group);
        s.RegisterMember("what", &TplSave::what);
        s.RegisterMember("clash", &TplSave::clash);
        s.RegisterMember("thumb", &TplSave::thumb);
    }
    model.Bind("ue_tpl_open", &m_tpl_open_);
    model.Bind("ue_tpl_empty", &m_tpl_empty_);
    model.Bind("ue_tpl_needs", &m_tpl_needs_);
    model.Bind("ue_tpl_save", &m_tpl_save_);
    model.Bind("ue_tpl_save_groups", &m_tpl_save_groups_);
    model.Bind("ue_tpl_confirm", &m_tpl_confirm_);
    model.Bind("ue_tpl_section", &m_tpl_section_);
    model.Bind("ue_tpl_group", &m_tpl_group_);
    model.Bind("ue_tpl_search", &m_tpl_search_);
    model.Bind("ue_tpl_heading", &m_tpl_heading_);
    model.Bind("ue_tpl_note", &m_tpl_note_);
    model.Bind("ue_tpl_count_c", &m_tpl_count_c_);
    model.Bind("ue_tpl_count_s", &m_tpl_count_s_);
    model.Bind("ue_tpl_cards", &m_tpl_cards_);
    model.Bind("ue_tpl_groups", &m_tpl_groups_);
    model.Bind("ue_tpl_variants", &m_tpl_variants_);
    model.Bind("ue_tpl_sel", &m_tpl_sel_);

    auto on = [&](const char* name, auto fn) {
        model.BindEventCallback(name, [this, fn](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& args) {
            fn(ev, args);
        });
    };
    auto arg_str = [](const Rml::VariantList& a, usize i) { return i < a.size() ? a[i].Get<Rml::String>() : Rml::String(); };
    auto arg_int = [](const Rml::VariantList& a, usize i) { return i < a.size() ? a[i].Get<int>() : -1; };
    on("ue_tpl_open", [this, arg_str](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        open_templates(arg_str(a, 0));
    });
    on("ue_tpl_close", [this](Rml::Event&, const Rml::VariantList&) { close_templates(); });
    on("ue_tpl_show", [this, arg_str](Rml::Event&, const Rml::VariantList& a) { templates_show(arg_str(a, 0), arg_str(a, 1)); });
    on("ue_tpl_search", [this, arg_str](Rml::Event&, const Rml::VariantList& a) { templates_search(arg_str(a, 0)); });
    on("ue_tpl_pick", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
        if (const int i = arg_int(a, 0); i >= 0) select_template(static_cast<usize>(i));
    });
    // A double click takes it: a construction onto the screen, a screen as a new one.
    on("ue_tpl_take", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
        if (const int i = arg_int(a, 0); i >= 0) take_template(static_cast<usize>(i));
    });
    on("ue_tpl_insert", [this](Rml::Event&, const Rml::VariantList&) {
        if (tpl_.selected >= 0) insert_template(static_cast<usize>(tpl_.selected));
    });
    on("ue_tpl_new_screen", [this](Rml::Event&, const Rml::VariantList&) {
        if (tpl_.selected >= 0) screen_from_template(static_cast<usize>(tpl_.selected));
    });
    // «Сохранить как шаблон»: from the window, the one selected layer (none: the whole screen).
    on("ue_tpl_save_open", [this](Rml::Event&, const Rml::VariantList&) {
        begin_save_template(selection_.size() == 1 && selection_[0] != screen_.root.id ? selection_[0] : 0);
    });
    on("ue_tpl_save_title", [this, arg_str](Rml::Event&, const Rml::VariantList& a) {
        if (arg_str(a, 0) != tpl_.save_title) set_save_template(arg_str(a, 0), tpl_.save_group);
    });
    on("ue_tpl_save_group", [this, arg_str](Rml::Event&, const Rml::VariantList& a) { set_save_template(tpl_.save_title, arg_str(a, 0)); });
    on("ue_tpl_save", [this](Rml::Event&, const Rml::VariantList& a) { save_template(!a.empty() && a[0].Get<bool>()); });
    on("ue_tpl_remove", [this](Rml::Event&, const Rml::VariantList& a) {
        if (tpl_.selected < 0) return;
        if (!a.empty() && a[0].Get<bool>()) {
            remove_template(static_cast<usize>(tpl_.selected));
        } else {
            tpl_.confirm_remove = !tpl_.confirm_remove; // asked, or «Оставить»
            refresh_templates();
        }
    });
}

} // namespace forge::editor_app
