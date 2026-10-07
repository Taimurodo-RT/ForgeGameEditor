#include "ui_editor.h"

#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/core/time.h"
#include "forge/ui/html.h"

#include <yyjson.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace forge::editor_app {

namespace d = editor::design;

namespace {

constexpr const char* kImage = "ui-screen"; // the canvas's picture of the screen: <img src="/gpu/ui-screen"/>
constexpr f32 kSnap = 6;                     // canvas pixels
constexpr f32 kHandle = 7;                   // canvas pixels: how near a handle a press grabs it

std::string fmt(f32 v) {
    if (std::fabs(v - std::round(v)) < 0.005f) return std::to_string(static_cast<long long>(std::lround(v)));
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

// A number typed into a field: "12", "12,5", "12px", "50%"; also "+10" and
// "-10" after a value (adds to it), and simple sums "100/2", "8*3".
std::optional<f32> parse_number(std::string text, f32 current = 0) {
    std::string s;
    for (char c : text)
        if (c != ' ' && c != '\t') s += c == ',' ? '.' : c;
    for (const char* unit : {"px", "%", "°"}) {
        const usize n = std::strlen(unit);
        if (s.size() > n && s.compare(s.size() - n, n, unit) == 0) s.resize(s.size() - n);
    }
    if (s.empty()) return std::nullopt;
    // Left to right, no precedence: what people type in a field.
    const char* p = s.c_str();
    char* end = nullptr;
    f32 value = 0;
    bool relative = false;
    if ((*p == '+' || *p == '*' || *p == '/') && p[1]) {
        value = current;
        relative = true;
    } else {
        value = std::strtof(p, &end);
        if (end == p) return std::nullopt;
        p = end;
    }
    while (*p) {
        const char op = *p++;
        const f32 rhs = std::strtof(p, &end);
        if (end == p) return std::nullopt;
        p = end;
        switch (op) {
        case '+': value += rhs; break;
        case '-': value -= rhs; break;
        case '*': value *= rhs; break;
        case '/':
            if (rhs == 0) return std::nullopt;
            value /= rhs;
            break;
        default: return std::nullopt;
        }
    }
    (void)relative;
    if (!std::isfinite(value)) return std::nullopt;
    return value;
}

const char* type_icon(d::NodeType t) {
    switch (t) {
    case d::NodeType::Frame: return "tag";
    case d::NodeType::Rectangle: return "rectangle";
    case d::NodeType::Ellipse: return "circle";
    case d::NodeType::Text: return "title";
    case d::NodeType::Image: return "image";
    }
    return "crop_free";
}

const char* type_word(d::NodeType t) {
    switch (t) {
    case d::NodeType::Frame: return "Рамка";
    case d::NodeType::Rectangle: return "Прямоугольник";
    case d::NodeType::Ellipse: return "Эллипс";
    case d::NodeType::Text: return "Текст";
    case d::NodeType::Image: return "Картинка";
    }
    return "";
}

const char* const kConstraintWords[] = {"start", "end", "both", "center", "scale"};
const char* const kSizingWords[] = {"fixed", "hug", "fill"};
const char* const kLayoutWords[] = {"none", "row", "column", "wrap"};
const char* const kKindWords[] = {"solid", "linear", "radial", "image"};
const char* const kKindNames[] = {"Цвет", "Линейный градиент", "Радиальный градиент", "Картинка"};
const char* const kFitWords[] = {"fill", "fit", "tile", "stretch"};
const char* const kArtRepeatWords[] = {"stretch", "repeat", "round", "space"};
const char* const kScreenFitWords[] = {"expand", "fit", "stretch"};
const char* const kScreenShowWords[] = {"playing", "command", "menu"};
const char* const kBarFromWords[] = {"left", "right", "bottom", "top"};
// Movement (the file's words, design::Motion and friends).
const char* const kMotionWords[] = {"none", "pulse", "float", "swing", "spin", "shake", "blink", "custom"};
const char* const kEasingWords[] = {"smooth", "linear", "in", "out", "back", "bounce", "elastic"};
const char* const kAppearWords[] = {"none", "fade", "rise", "drop", "zoom", "left", "right"};
// Where the sample pictures for drawn interfaces go in a game.
const char* const kArtFolder = "pictures/интерфейс";
const char* const kEffectWords[] = {"drop-shadow", "inner-shadow", "layer-blur", "background-blur"};
const char* const kEffectNames[] = {"Тень", "Внутренняя тень", "Размытие слоя", "Размытие фона"};
const char* const kAlignWords[] = {"left", "center", "right", "justify"};
const char* const kCaseWords[] = {"none", "upper", "lower", "title"};
const char* const kDecorationWords[] = {"none", "underline", "strike"};
const char* const kStrokeAlignWords[] = {"inside", "center", "outside"};
const char* const kStrokeStyleWords[] = {"solid", "dashed", "dotted"};

template <usize N> int index_of(const std::string& value, const char* const (&words)[N]) {
    for (usize i = 0; i < N; ++i)
        if (value == words[i]) return static_cast<int>(i);
    return -1;
}

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

d::Rect bounds(const std::vector<d::Rect>& rects) {
    if (rects.empty()) return {};
    f32 x0 = rects[0].x, y0 = rects[0].y, x1 = rects[0].right(), y1 = rects[0].bottom();
    for (const d::Rect& r : rects) {
        x0 = std::min(x0, r.x);
        y0 = std::min(y0, r.y);
        x1 = std::max(x1, r.right());
        y1 = std::max(y1, r.bottom());
    }
    return {x0, y0, x1 - x0, y1 - y0};
}

d::Paint solid(d::Color c) {
    d::Paint p;
    p.color = c;
    return p;
}

bool inside(const d::Rect& r, f32 x, f32 y) { return x >= r.x && y >= r.y && x < r.right() && y < r.bottom(); }
bool intersects(const d::Rect& a, const d::Rect& b) {
    return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}

u32 rgba(u8 r, u8 g, u8 b, u8 a = 255) {
    return (u32(r) << 24) | (u32(g) << 16) | (u32(b) << 8) | u32(a);
}

// The first screen of a game that has none: a main menu to start from.
d::Screen sample_screen() {
    d::Screen s = d::make_screen("Главное меню", 1920, 1080);
    d::Paint bg;
    bg.kind = d::PaintKind::Linear;
    bg.angle = 160;
    bg.stops = {{d::Color{0x1a, 0x24, 0x30, 255}, 0}, {d::Color{0x2a, 0x1f, 0x18, 255}, 0.6f}, {d::Color{0x0d, 0x0b, 0x09, 255}, 1}};
    s.root.fills.push_back(bg);

    d::Node title;
    title.id = s.next_id++;
    title.name = "Название игры";
    title.type = d::NodeType::Text;
    title.text = "Старая шахта";
    title.x = 140;
    title.y = 150;
    title.w = 900;
    title.h = 150;
    title.width_sizing = d::Sizing::Hug;
    title.height_sizing = d::Sizing::Hug;
    title.text_style.family = "Exo 2";
    title.text_style.size = 120;
    title.text_style.weight = 800;
    title.text_style.color = {0xe8, 0xb0, 0x4a, 255};
    title.effects.push_back({d::EffectKind::DropShadow, d::Color{0, 0, 0, 150}, 0, 4, 24, 0, true});
    s.root.children.push_back(title);

    d::Node menu;
    menu.id = s.next_id++;
    menu.name = "Меню";
    menu.type = d::NodeType::Frame;
    menu.x = 140;
    menu.y = 420;
    menu.w = 520;
    menu.h = 324;
    menu.vertical = d::Constraint::Center;
    menu.layout.mode = d::LayoutMode::Column;
    menu.layout.gap = 24;
    menu.height_sizing = d::Sizing::Hug;
    // The game's main menu in place of its usual one: the buttons start a
    // game, open the settings, leave.
    s.show = d::ScreenShow::Menu;
    const std::pair<const char*, d::ActionKind> buttons[] = {
        {"Новая игра", d::ActionKind::NewGame}, {"Настройки", d::ActionKind::Settings}, {"Выход", d::ActionKind::Quit}};
    for (const auto& [label, action] : buttons) {
        d::Node button;
        button.id = s.next_id++;
        button.name = std::string("Кнопка «") + label + "»";
        button.type = d::NodeType::Frame;
        button.w = 520;
        button.h = 92;
        button.width_sizing = d::Sizing::Fill;
        button.layout.mode = d::LayoutMode::Row;
        button.layout.padding = {0, 28, 0, 28};
        button.layout.align = 3; // middle left
        button.radius = {14, 14, 14, 14};
        button.fills.push_back(solid(d::Color{0x2a, 0x20, 0x18, 240}));
        button.strokes.push_back(d::Stroke{d::Color{0xe8, 0xb0, 0x4a, 0x55}, 2, d::StrokeAlign::Inside});
        button.effects.push_back({d::EffectKind::DropShadow, d::Color{0, 0, 0, 120}, 0, 12, 40, 0, true});
        button.on_click.push_back({action, ""});
        d::Node text;
        text.id = s.next_id++;
        text.name = label;
        text.type = d::NodeType::Text;
        text.text = label;
        text.width_sizing = d::Sizing::Hug;
        text.height_sizing = d::Sizing::Hug;
        text.text_style.family = "Exo 2";
        text.text_style.size = 40;
        text.text_style.weight = 700;
        text.text_style.color = {0xf4, 0xea, 0xd8, 255};
        button.children.push_back(text);
        menu.children.push_back(button);
    }
    s.root.children.push_back(menu);
    return s;
}

} // namespace

// One step of the history: a screen's file before and after.
class UiCommand final : public editor::Command {
public:
    UiCommand(UiEditor& ed, std::string screen, std::string before, std::string after, std::vector<u32> sel_before,
              std::vector<u32> sel_after, std::string label, std::string merge)
        : ed_(ed), screen_(std::move(screen)), before_(std::move(before)), after_(std::move(after)),
          sel_before_(std::move(sel_before)), sel_after_(std::move(sel_after)), label_(std::move(label)),
          merge_(std::move(merge)) {}
    void apply(editor::Document&) override { ed_.apply(screen_, after_, sel_after_); }
    void revert(editor::Document&) override { ed_.apply(screen_, before_, sel_before_); }
    std::string label() const override { return label_; }
    std::string merge_key() const override { return merge_.empty() ? std::string() : screen_ + "/" + merge_; }
    bool try_merge(const editor::Command& next) override {
        // Only this tab's commands are in its history.
        const auto* n = static_cast<const UiCommand*>(&next);
        if (n->screen_ != screen_) return false;
        after_ = n->after_;
        sel_after_ = n->sel_after_;
        return true;
    }

private:
    UiEditor& ed_;
    std::string screen_, before_, after_;
    std::vector<u32> sel_before_, sel_after_;
    std::string label_, merge_;
};

// --- files ------------------------------------------------------------------

bool UiEditor::init(ui::Ui& ui, const std::filesystem::path& game_dir) {
    ui_ = &ui;
    game_dir_ = game_dir;
    std::error_code ec;
    std::filesystem::create_directories(ui_dir(), ec);
    if (screens().empty()) write("main_menu", sample_screen());
    // The components: their own file, made empty the first time.
    {
        std::vector<u8> bytes;
        d::Screen lib;
        if (!read_file(json_path(kLibrary), bytes) ||
            !d::load_screen(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), lib)) {
            lib = d::make_screen("Компоненты", 1920, 1080);
            lib.root.name = "Компоненты";
            lib.root.clip = false;
            lib.root.fills.push_back(solid(d::Color{0x2c, 0x2f, 0x36, 255}));
        }
        lib.library = true;
        library_ = std::move(lib);
        if (!std::filesystem::exists(json_path(kLibrary), ec)) write(kLibrary, library_);
    }

    // Font families the screens can use: the UI's own fonts.
    std::vector<u8> bytes;
    if (read_file(ui.root() / "fonts" / "fonts.json", bytes)) {
        if (yyjson_doc* doc = yyjson_read(reinterpret_cast<const char*>(bytes.data()), bytes.size(), 0)) {
            yyjson_val* fonts = yyjson_obj_get(yyjson_doc_get_root(doc), "fonts");
            usize i, n;
            yyjson_val* f;
            yyjson_arr_foreach(fonts, i, n, f) {
                yyjson_val* family = yyjson_obj_get(f, "family");
                if (!yyjson_is_str(family)) continue;
                const Rml::String name = yyjson_get_str(family);
                if (name.find("Material") != Rml::String::npos) continue;
                if (std::find(m_families_.begin(), m_families_.end(), name) == m_families_.end()) m_families_.push_back(name);
            }
            yyjson_doc_free(doc);
        }
    }
    if (m_families_.empty()) m_families_.push_back("Onest");
    install_art(ui.root() / "art");
    scan_pictures();

    page_context_ = ui.create_context("ui-design", 1920, 1080);
    if (!page_context_) return false;
    page_context_->SetDensityIndependentPixelRatio(1.0f);
    ui.set_offscreen(page_context_, kImage);
    ui.set_active(page_context_, false);

    ui::register_line_source("ue-ruler-x", &ruler_x_);
    ui::register_line_source("ue-ruler-y", &ruler_y_);
    ui::register_line_source("ue-overlay", &overlay_lines_);

    const std::vector<std::string> all = screens();
    if (!all.empty()) open(all.front());
    return true;
}

void UiEditor::shutdown() {
    // The check's pages live in the UI's contexts: let them go while those still exist.
    checking_ = false;
    if (page_from_check_) page_ = nullptr;
    page_from_check_ = false;
    check_.reset();
    ui::register_line_source("ue-ruler-x", nullptr);
    ui::register_line_source("ue-ruler-y", nullptr);
    ui::register_line_source("ue-overlay", nullptr);
}

std::vector<std::string> UiEditor::screens() const {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(ui_dir(), ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
        std::string name = path_to_utf8(entry.path().stem());
        if (name != kLibrary) out.push_back(std::move(name));
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool UiEditor::write(const std::string& name, const d::Screen& screen) const {
    const std::string json = d::save_screen(screen);
    const std::string html = d::screen_html(screen, html_options());
    const bool ok = write_file_atomic(json_path(name), {reinterpret_cast<const u8*>(json.data()), json.size()}) &&
                    write_file_atomic(html_path(name), {reinterpret_cast<const u8*>(html.data()), html.size()});
    if (!ok) FORGE_ERROR("Не удалось сохранить экран %s", name.c_str());
    return ok;
}

bool UiEditor::open(const std::string& name) {
    std::vector<u8> bytes;
    if (!read_file(json_path(name), bytes)) return false;
    d::Screen s;
    std::string error;
    if (!d::load_screen(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), s, &error)) {
        FORGE_ERROR("Экран %s не открылся: %s", name.c_str(), error.c_str());
        return false;
    }
    if (s.title.empty()) s.title = name;
    if (name == kLibrary) {
        s.library = true;
        library_ = s;
    } else if (d::sync_instances(s, library_) | d::apply_styles(s, library_)) {
        write(name, s); // its components or the game's styles changed since
    }
    if (name != name_) {
        fit_pending_ = true;
        closed_.clear();
    }
    name_ = name;
    screen_ = std::move(s);
    selection_.clear();
    page_dirty_ = true;
    dirty_all();
    return true;
}

std::string UiEditor::new_screen() {
    const std::vector<std::string> all = screens();
    std::string name = "screen";
    for (u32 n = 2; std::find(all.begin(), all.end(), name) != all.end(); ++n) name = "screen" + std::to_string(n);
    d::Screen s = d::make_screen("Экран " + std::to_string(all.size() + 1), 1920, 1080);
    // A window over the game: the world shows dimmed behind it.
    s.show = d::ScreenShow::Command;
    s.root.fills.push_back(solid(d::Color{0x1b, 0x21, 0x27, 0xb0}));
    write(name, s);
    open(name);
    return name;
}

bool UiEditor::covers_game() const {
    return screen_.show == d::ScreenShow::Playing &&
           std::any_of(screen_.root.fills.begin(), screen_.root.fills.end(), [](const d::Paint& f) {
               return f.visible && f.opacity >= 1 && (f.kind != d::PaintKind::Solid || f.color.a == 255);
           });
}

void UiEditor::apply(const std::string& name, const std::string& json, const std::vector<u32>& selection) {
    d::Screen s;
    if (!d::load_screen(json, s)) return;
    if (s.title.empty()) s.title = name;
    if (name == kLibrary) {
        s.library = true;
        d::sync_instances(s, s);
        d::apply_styles(s, s);
        library_ = s;
        write(name, s);
        propagate_library();
        if (name_ != kLibrary) {
            // A step of a change made from a screen (Сделать компонентом): stay there.
            const std::vector<u32> keep = selection_;
            select(keep);
            page_dirty_ = true;
            dirty_all();
            return;
        }
    } else {
        d::sync_instances(s, library_);
        d::apply_styles(s, library_);
        // The page and its file at once: the game reads the page.
        write(name, s);
    }
    if (name != name_) fit_pending_ = true;
    name_ = name;
    screen_ = std::move(s);
    selection_.clear();
    for (u32 id : selection)
        if (d::find(screen_.root, id)) selection_.push_back(id);
    page_dirty_ = true;
    dirty_all();
}

void UiEditor::remember_geometry(const std::string& before) {
    if (screen_.library) return;
    d::Screen previous;
    if (!d::load_screen(before, previous)) return;
    for (u32 id : selection_) {
        d::Node* n = d::find(screen_.root, id);
        const d::Node* was = d::find(previous.root, id);
        const d::Node* instance = d::instance_of(screen_.root, id);
        if (!n || !was || !n->master || !instance) continue;
        auto keep = [&](const char* field) {
            if (std::find(n->overrides.begin(), n->overrides.end(), field) == n->overrides.end())
                n->overrides.push_back(field);
        };
        if (n->w != was->w || n->h != was->h || n->width_sizing != was->width_sizing || n->height_sizing != was->height_sizing)
            keep("size");
        if (instance->id != id && (n->x != was->x || n->y != was->y || n->absolute != was->absolute)) keep("place");
        if (n->layout != was->layout || n->clip != was->clip) keep("layout");
    }
}

void UiEditor::commit(const std::string& before, std::string label, std::string merge) {
    std::string after = d::save_screen(screen_);
    if (after == before) return; // nothing changed (a field given its own value)
    // The selection before the change is not known here any more; keep what
    // is selected now for both (undo selects what the change touched).
    history_.execute(std::make_unique<UiCommand>(*this, name_, before, std::move(after), selection_, selection_,
                                                 std::move(label), std::move(merge)));
}

void UiEditor::undo() {
    history_.seal();
    history_.undo();
}

void UiEditor::redo() {
    history_.seal();
    history_.redo();
}

std::string UiEditor::status() const {
    std::string s = "Экран «" + screen_.title + "» " + fmt(screen_.width) + "×" + fmt(screen_.height) + " · " +
                    fmt(std::round(zoom_ * 100)) + "%";
    if (selection_.size() == 1)
        if (const d::Node* n = d::find(screen_.root, selection_[0])) s += " · выделено: " + n->name;
    if (selection_.size() > 1) s += " · выделено: " + std::to_string(selection_.size());
    return s;
}

// --- the page on the canvas -------------------------------------------------

void UiEditor::set_shown(bool shown) {
    if (shown == shown_) return;
    shown_ = shown;
    if (ui_ && page_context_) ui_->set_active(page_context_, shown);
    if (shown) {
        page_dirty_ = true;
        scan_pictures();
    }
}

d::HtmlOptions UiEditor::html_options() const {
    d::HtmlOptions options;
    options.library = &library_;
    return options;
}

void UiEditor::propagate_library() {
    for (const std::string& name : screens()) {
        std::vector<u8> bytes;
        d::Screen s;
        if (!read_file(json_path(name), bytes) ||
            !d::load_screen(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), s))
            continue;
        if (s.title.empty()) s.title = name;
        bool has_instances = false;
        auto visit = [&](auto&& self, const d::Node& n) -> void {
            if (!n.component.empty() && n.master) has_instances = true;
            for (const d::Node& c : n.children) self(self, c);
        };
        visit(visit, s.root);
        const bool styled = d::apply_styles(s, library_);
        if (!has_instances && !styled) continue;
        d::sync_instances(s, library_);
        d::apply_styles(s, library_);
        write(name, s); // the page too: the states' looks may have changed
        if (name == name_) {
            screen_ = std::move(s);
            page_dirty_ = true;
        }
    }
}

void UiEditor::rebuild_page() {
    page_dirty_ = false;
    if (!page_context_) return;
    update_aspect();
    // The page is laid out on the player's screen (the screen's own size in «Макет»).
    const Rml::Vector2i size{static_cast<int>(std::lround(view_w())), static_cast<int>(std::lround(view_h()))};
    if (page_context_->GetDimensions() != size) page_context_->SetDimensions(size);
    if (page_) {
        if (page_from_check_) check_->unload();
        else page_context_->UnloadDocument(page_);
        page_ = nullptr;
        page_from_check_ = false;
    }
    if (checking_ && !screen_.library) {
        // The page as the game runs it: its clicks and its data.
        if (!check_) {
            check_ = std::make_unique<game::GameScreens>();
            check_->actions_to_caller = true;
            check_->on_action = [this](const game::ScreenAction& a, const std::string&) { check_pending_.push_back(a); };
        }
        // Lists show the game's things and quests.
        check_->set_items(game_items ? game_items() : std::vector<game::ScreenItem>{});
        check_->set_quests(game_quests ? game_quests() : nullptr);
        if (check_->load_page(page_context_, name_, d::screen_html(screen_, html_options()),
                              path_to_utf8(html_path(name_.empty() ? std::string("screen") : name_)))) {
            page_ = check_->document(name_);
            page_from_check_ = true;
            check_->show(name_, true);
            seed_check_vars();
            check_->update(check_vars_, screen_.show != d::ScreenShow::Menu, screen_.show == d::ScreenShow::Menu, size.x, size.y);
            page_context_->Update();
            read_boxes();
            refresh_check();
            return;
        }
    }
    // On the canvas things stand where they are placed: movements play in «Проверить» and the game.
    d::HtmlOptions still = html_options();
    still.motion = false;
    const std::string rml = ui::html_to_rml(d::screen_html(screen_, still), "/web/html.rcss");
    // The page's own address: pictures are found next to it (../pictures/...).
    std::string url = path_to_utf8(html_path(name_.empty() ? std::string("screen") : name_));
    std::replace(url.begin(), url.end(), '\\', '/');
    std::replace(url.begin(), url.end(), ':', '|');
    page_ = page_context_->LoadDocumentFromMemory(rml, url);
    if (!page_) {
        FORGE_ERROR("Экран %s не построился", name_.c_str());
        return;
    }
    page_->Show();
    // Meets the player's screen as in the game: the root's own forge-fit and forge-size.
    if (Rml::Element* root = page_->GetElementById("n" + std::to_string(screen_.root.id)))
        game::apply_screen_fit(root, view_fit(), root->GetAttribute<Rml::String>("forge-bars", ""));
    page_context_->Update(); // lay it out now: the canvas reads its boxes
    read_boxes();
}

void UiEditor::install_art(const std::filesystem::path& art_dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path to = game_dir_ / utf8_path(kArtFolder);
    for (const fs::directory_entry& e : fs::directory_iterator(art_dir, ec)) {
        if (!e.is_regular_file(ec) || e.path().extension() != ".png") continue;
        const fs::path mine = to / e.path().filename();
        if (fs::exists(mine, ec)) continue;
        fs::create_directories(to, ec);
        fs::copy_file(e.path(), mine, ec);
    }
    // The cuts of the frame pictures (art.json: {"pictures": {"name.png": {"slice": [t, r, b, l]}}}).
    std::vector<u8> bytes;
    if (!read_file(art_dir / "art.json", bytes)) return;
    yyjson_doc* doc = yyjson_read(reinterpret_cast<const char*>(bytes.data()), bytes.size(), 0);
    if (!doc) return;
    yyjson_val* pictures = yyjson_obj_get(yyjson_doc_get_root(doc), "pictures");
    usize i, n;
    yyjson_val *key, *val;
    yyjson_obj_foreach(pictures, i, n, key, val) {
        yyjson_val* slice = yyjson_obj_get(val, "slice");
        if (!yyjson_is_arr(slice) || yyjson_arr_size(slice) != 4) continue;
        std::array<f32, 4> cut{};
        for (usize k = 0; k < 4; ++k) cut[k] = static_cast<f32>(yyjson_get_num(yyjson_arr_get(slice, k)));
        art_slices_[yyjson_get_str(key)] = cut;
    }
    yyjson_doc_free(doc);
}

std::array<f32, 4> UiEditor::art_slice(const std::string& picture) const {
    const usize slash = picture.find_last_of('/');
    auto it = art_slices_.find(slash == std::string::npos ? picture : picture.substr(slash + 1));
    return it != art_slices_.end() ? it->second : std::array<f32, 4>{16, 16, 16, 16};
}

void UiEditor::scan_pictures() {
    namespace fs = std::filesystem;
    std::vector<PictureRow> rows;
    std::error_code ec;
    const fs::path root = game_dir_ / "pictures";
    for (fs::recursive_directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string ext = path_to_utf8(it->path().extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".svg" && ext != ".tga") continue;
        std::string path = path_to_utf8(it->path().lexically_relative(game_dir_));
        std::replace(path.begin(), path.end(), '\\', '/');
        // Shown as the picture's own name, then its folder: «рамка_дерево (интерфейс)».
        std::string name = path_to_utf8(it->path().stem());
        std::string folder = path_to_utf8(it->path().parent_path().lexically_relative(root));
        std::replace(folder.begin(), folder.end(), '\\', '/');
        if (!folder.empty() && folder != ".") name += " (" + folder + ")";
        rows.push_back({path, name});
        if (rows.size() >= 2000) break;
    }
    std::sort(rows.begin(), rows.end(), [](const PictureRow& a, const PictureRow& b) { return a.path < b.path; });
    // A picture a layer uses but the folder lacks stays listed, so the lists show it.
    auto keep = [&](const std::string& path) {
        if (!path.empty() && std::none_of(rows.begin(), rows.end(), [&](const PictureRow& r) { return r.path == path; }))
            rows.push_back({path, path + " (нет файла)"});
    };
    auto visit = [&](auto&& self, const d::Node& n) -> void {
        for (const d::Paint& p : n.fills)
            if (p.kind == d::PaintKind::Image) keep(p.image);
        keep(n.frame.image);
        keep(n.mask.image);
        for (const d::Node& c : n.children) self(self, c);
    };
    visit(visit, screen_.root);
    m_pictures_ = std::move(rows);
    dirty("ue_pictures");
}

void UiEditor::read_boxes() {
    boxes_.clear();
    if (!page_) return;
    // Boxes are in the page's pixels: the fit places the root on the player's screen.
    const game::ScreenFit fit = view_fit();
    // Layout boxes leave out transforms: a centred layer that hugs its
    // content is moved back by half its size (design::node_css), and so is
    // everything inside it.
    auto visit = [&](auto&& self, const d::Node& n, const d::Node* parent, f32 ox, f32 oy) -> void {
        if (!n.visible) return;
        if (Rml::Element* e = page_->GetElementById("n" + std::to_string(n.id))) {
            const Rml::Vector2f at = e->GetAbsoluteOffset(Rml::BoxArea::Border);
            const Rml::Vector2f size = e->GetBox().GetSize(Rml::BoxArea::Border);
            const bool in_flow = parent && parent->layout.mode != d::LayoutMode::None && !n.absolute;
            if (parent && !in_flow) {
                if (n.horizontal == d::Constraint::Center && n.width_sizing == d::Sizing::Hug) ox -= size.x * 0.5f;
                if (n.vertical == d::Constraint::Center && n.height_sizing == d::Sizing::Hug) oy -= size.y * 0.5f;
            }
            boxes_[n.id] = {at.x + ox - fit.left, at.y + oy - fit.top, size.x, size.y};
        }
        for (const d::Node& c : n.children) self(self, c, &n, ox, oy);
    };
    visit(visit, screen_.root, nullptr, 0.f, 0.f);
    // The root is the whole screen (wider or taller than drawn when it grows to the player's).
    boxes_[screen_.root.id] = {0, 0, fit.width, fit.height};
}

std::optional<d::Rect> UiEditor::layer_box(u32 id) const {
    auto it = boxes_.find(id);
    if (it == boxes_.end()) return std::nullopt;
    return it->second;
}

u32 UiEditor::hit(f32 x, f32 y, bool deep) const {
    // The deepest layer under the point, the topmost of siblings first.
    std::vector<u32> path;
    auto visit = [&](auto&& self, const d::Node& n, std::vector<u32>& at) -> bool {
        if (!n.visible) return false;
        for (auto it = n.children.rbegin(); it != n.children.rend(); ++it) {
            at.push_back(it->id);
            if (self(self, *it, at)) return true;
            at.pop_back();
        }
        if (n.id == screen_.root.id || n.locked) return false;
        auto box = boxes_.find(n.id);
        return box != boxes_.end() && inside(box->second, x, y);
    };
    if (!visit(visit, screen_.root, path) || path.empty()) return 0;
    if (deep) return path.back();
    // At the level of the selection: a child of the root, or of a frame the
    // selection is in (Figma's way: a click picks the outer layer, a double
    // click goes in).
    std::set<u32> context{screen_.root.id};
    for (u32 sel : selection_)
        for (u32 id : d::path_to(screen_.root, sel)) context.insert(id);
    u32 pick = path.front();
    u32 parent = screen_.root.id;
    for (u32 id : path) {
        if (context.count(parent)) pick = id;
        parent = id;
    }
    return pick;
}

u32 UiEditor::container_at(f32 x, f32 y) const {
    u32 found = screen_.root.id;
    auto visit = [&](auto&& self, const d::Node& n) -> void {
        for (const d::Node& c : n.children) {
            if (!c.visible || c.locked || !c.is_container()) continue;
            auto box = boxes_.find(c.id);
            if (box == boxes_.end() || !inside(box->second, x, y)) continue;
            found = c.id;
            self(self, c);
        }
    };
    visit(visit, screen_.root);
    return found;
}

d::Rect UiEditor::selection_box() const {
    std::vector<d::Rect> rects;
    for (u32 id : selection_)
        if (auto b = layer_box(id)) rects.push_back(*b);
    return bounds(rects);
}

d::SnapTargets UiEditor::snap_targets() const {
    d::SnapTargets t;
    t.frame = {0, 0, screen_.width, screen_.height};
    std::set<u32> skip(selection_.begin(), selection_.end());
    // Layers that move with the selection are not targets.
    auto visit = [&](auto&& self, const d::Node& n, bool moving) -> void {
        const bool m = moving || skip.count(n.id);
        if (!n.visible) return;
        if (!m && n.id != screen_.root.id)
            if (auto b = layer_box(n.id)) t.boxes.push_back(*b);
        for (const d::Node& c : n.children) self(self, c, m);
    };
    visit(visit, screen_.root, false);
    for (const d::Guide& g : screen_.guides) (g.vertical ? t.xs : t.ys).push_back(g.position);
    if (screen_.safe > 0) {
        t.xs.push_back(screen_.safe);
        t.xs.push_back(screen_.width - screen_.safe);
        t.ys.push_back(screen_.safe);
        t.ys.push_back(screen_.height - screen_.safe);
    }
    return t;
}

// --- view -------------------------------------------------------------------

void UiEditor::dirty(const char* name) {
    if (model_) model_.DirtyVariable(name);
}

void UiEditor::dirty_all() {
    m_components_.clear();
    for (const d::Component& c : d::components(library_))
        m_components_.push_back({c.name, c.properties.empty() ? "widgets" : "view_cozy", static_cast<int>(c.variants.size())});
    dirty("ue_components");
    m_library_open_ = library_open();
    dirty("ue_library_open");
    m_game_colors_.clear();
    for (usize i = 0; i < library_.colors.size(); ++i) {
        const d::NamedColor& c = library_.colors[i];
        m_game_colors_.push_back({static_cast<int>(i), c.key, c.name, hex_of(c.color), swatch(c.color)});
    }
    m_game_texts_.clear();
    for (usize i = 0; i < library_.text_styles.size(); ++i) {
        const d::NamedTextStyle& t = library_.text_styles[i];
        m_game_texts_.push_back({static_cast<int>(i), t.key, t.name, t.style.family, fmt(t.style.size),
                                 std::to_string(t.style.weight), hex_of(t.style.color), swatch(t.style.color)});
    }
    dirty("ue_game_colors");
    dirty("ue_game_texts");
    refresh_screens();
    refresh_layers();
    refresh_props();
    refresh_view();
}

void UiEditor::read_canvas(Rml::Context* context) {
    if (!context) return;
    for (int i = 0; i < context->GetNumDocuments(); ++i) {
        Rml::Element* canvas = context->GetDocument(i)->GetElementById("ue-canvas");
        if (!canvas) continue;
        const Rml::Vector2f at = canvas->GetAbsoluteOffset(Rml::BoxArea::Padding);
        const Rml::Vector2f size = canvas->GetBox().GetSize(Rml::BoxArea::Padding);
        if (at.x != canvas_x_ || at.y != canvas_y_ || size.x != canvas_w_ || size.y != canvas_h_) {
            canvas_x_ = at.x;
            canvas_y_ = at.y;
            canvas_w_ = size.x;
            canvas_h_ = size.y;
            refresh_view();
        }
        return;
    }
}

const std::vector<UiEditor::ViewSize>& UiEditor::view_sizes() {
    static const std::vector<ViewSize> sizes = {
        {"Макет", 0, 0}, {"1280×720", 1280, 720}, {"1920×1080", 1920, 1080}, {"2560×1440", 2560, 1440},
        {"21:9 · 2560×1080", 2560, 1080}, {"4:3 · 1440×1080", 1440, 1080}};
    return sizes;
}

f32 UiEditor::view_w() const {
    const ViewSize& v = view_sizes()[static_cast<usize>(view_)];
    return v.w > 0 ? v.w : screen_.width;
}

f32 UiEditor::view_h() const {
    const ViewSize& v = view_sizes()[static_cast<usize>(view_)];
    return v.h > 0 ? v.h : screen_.height;
}

game::ScreenFit UiEditor::view_fit() const {
    return game::fit_screen(d::screen_fit_word(screen_.fit), screen_.width, screen_.height, view_w(), view_h());
}

void UiEditor::set_view(int view) {
    view = std::clamp(view, 0, static_cast<int>(view_sizes().size()) - 1);
    if (view == view_) return;
    view_ = view;
    m_view_ = view;
    dirty("ue_view");
    if (drag_ != Drag::None) drag_ = Drag::None;
    snap_lines_.clear();
    snap_gaps_.clear();
    page_dirty_ = true; // laid out again on that screen
    rebuild_page();
    zoom_to_fit();
    refresh_props();
}

// A stretched screen on a player's screen of other proportions: its sides'
// own zooms, and the words over the canvas.
void UiEditor::update_aspect() {
    const game::ScreenFit fit = view_fit();
    const f32 s = std::min(fit.sx, fit.sy);
    aspect_x_ = fit.sx / s;
    aspect_y_ = fit.sy / s;
    if (!previewing()) {
        m_view_note_ = "";
        return;
    }
    const char* how = screen_.fit == d::ScreenFit::Fit       ? "целиком, по краям полосы"
                      : screen_.fit == d::ScreenFit::Stretch ? "растянут по сторонам"
                                                             : "растёт по стороне, слои держатся своих краёв";
    m_view_note_ = "Экран игрока " + fmt(view_w()) + "×" + fmt(view_h()) + ": экран " + fmt(screen_.width) + "×" +
                   fmt(screen_.height) + " " + how + ", масштаб " +
                   (fit.sx == fit.sy ? fmt(std::round(fit.sx * 1000) / 10) + "%"
                                     : fmt(std::round(fit.sx * 1000) / 10) + "% × " + fmt(std::round(fit.sy * 1000) / 10) + "%") +
                   ". Слои двигаются в «Макете».";
}

void UiEditor::set_zoom(f32 zoom, f32 at_x, f32 at_y) {
    zoom = std::clamp(zoom, 0.02f, 64.0f);
    // Keep the screen point under (at_x, at_y) (canvas pixels) in place.
    const f32 sx = to_screen_x(at_x), sy = to_screen_y(at_y);
    zoom_ = zoom;
    pan_x_ = at_x - sx * zoom_x();
    pan_y_ = at_y - sy * zoom_y();
    refresh_view();
}

void UiEditor::zoom_to_fit() {
    if (canvas_w_ <= 0 || canvas_h_ <= 0) {
        fit_pending_ = true;
        return;
    }
    fit_pending_ = false;
    update_aspect();
    const f32 margin = 48;
    // The player's whole screen, in page pixels.
    const game::ScreenFit fit = view_fit();
    const f32 x0 = game::fit_to_page_x(fit, 0), y0 = game::fit_to_page_y(fit, 0);
    const f32 w = view_w() / fit.sx, h = view_h() / fit.sy;
    const f32 k = std::min((canvas_w_ - 2 * margin) / (w * aspect_x_), (canvas_h_ - 2 * margin) / (h * aspect_y_));
    zoom_ = std::clamp(k, 0.02f, 1.0f);
    pan_x_ = std::round((canvas_w_ - w * zoom_x()) * 0.5f - x0 * zoom_x());
    pan_y_ = std::round((canvas_h_ - h * zoom_y()) * 0.5f - y0 * zoom_y());
    refresh_view();
}

void UiEditor::refresh_view() {
    // The frame is the player's whole screen (bars around a fitted page included).
    update_aspect();
    const game::ScreenFit fit = view_fit();
    const f32 x0 = game::fit_to_page_x(fit, 0), y0 = game::fit_to_page_y(fit, 0);
    const f32 fw = view_w() / fit.sx, fh = view_h() / fit.sy;
    m_frame_ = {to_canvas_x(x0), to_canvas_y(y0), fw * zoom_x(), fh * zoom_y()};
    m_frame_label_ = {m_frame_.x, m_frame_.y - 22,
                      previewing() ? screen_.title + "  на экране игрока " + fmt(view_w()) + " × " + fmt(view_h())
                                   : screen_.title + "  " + fmt(screen_.width) + " × " + fmt(screen_.height)};
    m_zoom_text_ = fmt(std::round(zoom_ * 100)) + "%";
    m_guides_x_.clear();
    m_guides_y_.clear();
    for (const d::Guide& g : screen_.guides) {
        if (g.vertical) m_guides_x_.push_back({to_canvas_x(g.position), 0, 1, canvas_h_});
        else m_guides_y_.push_back({0, to_canvas_y(g.position), canvas_w_, 1});
    }
    m_has_safe_ = screen_.safe > 0;
    // Along the player's screen's edges.
    m_safe_ = {to_canvas_x(x0 + screen_.safe), to_canvas_y(y0 + screen_.safe), (fw - 2 * screen_.safe) * zoom_x(),
               (fh - 2 * screen_.safe) * zoom_y()};
    for (const char* name : {"ue_frame", "ue_frame_label", "ue_zoom", "ue_guides_x", "ue_guides_y", "ue_safe", "ue_has_safe", "ue_view_note"})
        dirty(name);
    refresh_overlay();
    refresh_rulers();
}

void UiEditor::refresh_rulers() {
    // A step between labelled ticks that leaves at least ~70 pixels.
    static const f32 steps[] = {1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000};
    f32 step = steps[std::size(steps) - 1];
    for (f32 s : steps)
        if (s * zoom_ >= 70) {
            step = s;
            break;
        }
    const f32 minor = step / ((static_cast<int>(step) % 5 == 0 || step < 2) ? 5 : 4);
    const u32 tick = rgba(0x8a, 0x96, 0xa0, 200), major = rgba(0xa9, 0xb4, 0xbd, 255);
    const f32 ruler = 20;
    m_ticks_x_.clear();
    m_ticks_y_.clear();
    ruler_x_.list.clear();
    ruler_y_.list.clear();
    auto axis = [&](bool horizontal) {
        const f32 length = horizontal ? canvas_w_ : canvas_h_;
        const f32 from = horizontal ? to_screen_x(0) : to_screen_y(0);
        const f32 to = horizontal ? to_screen_x(length) : to_screen_y(length);
        Lines& lines = horizontal ? ruler_x_ : ruler_y_;
        std::vector<Tick>& ticks = horizontal ? m_ticks_x_ : m_ticks_y_;
        const f32 first = std::floor(from / minor) * minor;
        for (f32 v = first; v <= to && ticks.size() < 400; v += minor) {
            const f32 at = std::round(horizontal ? to_canvas_x(v) : to_canvas_y(v)) + 0.5f;
            const bool big = std::fabs(std::fmod(std::fabs(v) + step * 0.5f, step) - step * 0.5f) < minor * 0.25f;
            const f32 len = big ? ruler * 0.5f : ruler * 0.25f;
            ui::Line l;
            l.width = 1;
            l.color = big ? major : tick;
            if (horizontal) l.points = {at, ruler - len, at, ruler};
            else l.points = {ruler - len, at, ruler, at};
            lines.list.push_back(std::move(l));
            if (big) ticks.push_back({at, fmt(std::round(v))});
        }
        ++lines.v;
    };
    axis(true);
    axis(false);
    dirty("ue_ticks_x");
    dirty("ue_ticks_y");
}

void UiEditor::refresh_overlay() {
    // Selection: each layer's outline, the whole selection's box with its handles.
    m_selected_.clear();
    for (u32 id : selection_)
        if (auto b = layer_box(id)) m_selected_.push_back({to_canvas_x(b->x), to_canvas_y(b->y), b->w * zoom_x(), b->h * zoom_y()});
    const d::Rect sb = selection_box();
    m_sel_box_ = {to_canvas_x(sb.x), to_canvas_y(sb.y), sb.w * zoom_x(), sb.h * zoom_y()};
    m_handles_ = !selection_.empty() && !(selection_.size() == 1 && selection_[0] == screen_.root.id);
    m_size_label_ = selection_.empty() ? Rml::String() : fmt(std::round(sb.w * 100) / 100) + " × " + fmt(std::round(sb.h * 100) / 100);
    // Hover.
    m_hovering_ = false;
    if (hover_ && drag_ == Drag::None && std::find(selection_.begin(), selection_.end(), hover_) == selection_.end())
        if (auto b = layer_box(hover_)) {
            m_hover_ = {to_canvas_x(b->x), to_canvas_y(b->y), b->w * zoom_x(), b->h * zoom_y()};
            m_hovering_ = true;
        }
    m_marqueeing_ = drag_ == Drag::Marquee && dragged_;
    if (m_marqueeing_)
        m_marquee_ = {to_canvas_x(marquee_.x), to_canvas_y(marquee_.y), marquee_.w * zoom_x(), marquee_.h * zoom_y()};

    // Snapping lines and gaps (pink), with the gaps' lengths.
    overlay_lines_.list.clear();
    m_measures_.clear();
    const u32 pink = rgba(0xff, 0x24, 0x7c);
    for (const d::SnapLine& l : snap_lines_) {
        ui::Line line;
        line.width = 1;
        line.color = pink;
        if (l.vertical) {
            const f32 x = std::round(to_canvas_x(l.at)) + 0.5f;
            line.points = {x, to_canvas_y(l.from), x, to_canvas_y(l.to)};
        } else {
            const f32 y = std::round(to_canvas_y(l.at)) + 0.5f;
            line.points = {to_canvas_x(l.from), y, to_canvas_x(l.to), y};
        }
        overlay_lines_.list.push_back(std::move(line));
    }
    for (const d::SnapGap& g : snap_gaps_) {
        ui::Line line;
        line.width = 1;
        line.color = pink;
        if (g.horizontal) {
            const f32 y = std::round(to_canvas_y(g.at)) + 0.5f;
            line.points = {to_canvas_x(g.from), y, to_canvas_x(g.to), y};
            m_measures_.push_back({(to_canvas_x(g.from) + to_canvas_x(g.to)) * 0.5f, y + 4, fmt(std::round(g.to - g.from))});
        } else {
            const f32 x = std::round(to_canvas_x(g.at)) + 0.5f;
            line.points = {x, to_canvas_y(g.from), x, to_canvas_y(g.to)};
            m_measures_.push_back({x + 4, (to_canvas_y(g.from) + to_canvas_y(g.to)) * 0.5f, fmt(std::round(g.to - g.from))});
        }
        overlay_lines_.list.push_back(std::move(line));
    }
    ++overlay_lines_.v;
    for (const char* name : {"ue_selected", "ue_sel_box", "ue_handles", "ue_size_label", "ue_hover", "ue_hovering",
                             "ue_marquee", "ue_marqueeing", "ue_measures"})
        dirty(name);
}

void UiEditor::refresh_screens() {
    m_screens_.clear();
    for (const std::string& name : screens()) {
        ScreenRow row;
        row.name = name;
        row.title = name;
        if (name == name_) {
            row.title = screen_.title;
            row.size = fmt(screen_.width) + "×" + fmt(screen_.height);
        } else {
            std::vector<u8> bytes;
            d::Screen s;
            if (read_file(json_path(name), bytes) &&
                d::load_screen(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), s)) {
                if (!s.title.empty()) row.title = s.title;
                row.size = fmt(s.width) + "×" + fmt(s.height);
            }
        }
        row.selected = name == name_;
        m_screens_.push_back(std::move(row));
    }
    m_title_ = screen_.title;
    dirty("ue_screens");
    dirty("ue_title");
}

void UiEditor::refresh_layers() {
    m_layers_.clear();
    auto visit = [&](auto&& self, const d::Node& n, int depth, bool parent_hidden) -> void {
        // Top of the list is the top of the drawing: children last to first.
        for (auto it = n.children.rbegin(); it != n.children.rend(); ++it) {
            const d::Node& c = *it;
            LayerRow row;
            row.id = static_cast<int>(c.id);
            row.name = c.name.empty() ? type_word(c.type) : c.name;
            row.icon = c.is_container() && c.layout.mode != d::LayoutMode::None
                           ? (c.layout.mode == d::LayoutMode::Column ? "view_agenda" : "view_column")
                           : type_icon(c.type);
            row.depth = depth;
            row.component = !c.component.empty() && (c.master || screen_.library);
            if (row.component) row.icon = screen_.library ? "view_cozy" : "widgets";
            if (row.component && screen_.library && depth == 0) {
                // A variant: the component's name and its values («Кнопка · Наведение»).
                std::string name = c.component;
                for (const auto& [property, value] : c.variant) name += " · " + value;
                row.name = name;
            }
            row.selected = std::find(selection_.begin(), selection_.end(), c.id) != selection_.end();
            row.visible = c.visible;
            row.locked = c.locked;
            row.container = !c.children.empty();
            row.open = std::find(closed_.begin(), closed_.end(), c.id) == closed_.end();
            row.hidden_by_parent = parent_hidden;
            m_layers_.push_back(row);
            if (row.container && row.open) self(self, c, depth + 1, parent_hidden || !c.visible);
        }
    };
    visit(visit, screen_.root, 0, false);
    dirty("ue_layers");
}

void UiEditor::refresh_props() {
    Props p;
    m_fills_.clear();
    m_effects_.clear();
    // Nothing selected: the screen itself, with its settings.
    const d::Node* n = selection_.empty() ? &screen_.root : d::find(screen_.root, selection_[0]);
    if (n) {
        const d::Node* parent = d::parent_of(screen_.root, n->id);
        p.any = true;
        p.many = selection_.size() > 1;
        p.root = n->id == screen_.root.id;
        p.text = n->type == d::NodeType::Text;
        p.container = n->is_container();
        p.in_layout = parent && parent->layout.mode != d::LayoutMode::None;
        p.absolute = n->absolute;
        p.has_layout = n->layout.mode != d::LayoutMode::None;
        p.name = p.many ? std::to_string(selection_.size()) + " слоя(ёв)" : n->name;
        p.type_name = p.root ? "Экран" : type_word(n->type);
        p.icon = type_icon(n->type);
        const d::Rect box = layer_box(n->id).value_or(d::Rect{n->x, n->y, n->w, n->h});
        const d::Rect pbox = parent ? layer_box(parent->id).value_or(d::Rect{}) : d::Rect{};
        p.x = fmt(p.in_layout && !n->absolute ? box.x - pbox.x : n->x);
        p.y = fmt(p.in_layout && !n->absolute ? box.y - pbox.y : n->y);
        p.w = fmt(n->width_sizing == d::Sizing::Fixed ? n->w : box.w);
        p.h = fmt(n->height_sizing == d::Sizing::Fixed ? n->h : box.h);
        if (p.root) {
            p.w = fmt(screen_.width);
            p.h = fmt(screen_.height);
        }
        p.rotation = fmt(n->rotation);
        p.horizontal = kConstraintWords[static_cast<int>(n->horizontal)];
        p.vertical = kConstraintWords[static_cast<int>(n->vertical)];
        p.width_sizing = kSizingWords[static_cast<int>(n->width_sizing)];
        p.height_sizing = kSizingWords[static_cast<int>(n->height_sizing)];
        p.layout_mode = kLayoutWords[static_cast<int>(n->layout.mode)];
        p.gap = fmt(n->layout.gap);
        p.pad_t = fmt(n->layout.padding[0]);
        p.pad_r = fmt(n->layout.padding[1]);
        p.pad_b = fmt(n->layout.padding[2]);
        p.pad_l = fmt(n->layout.padding[3]);
        p.align = n->layout.align;
        p.space_between = n->layout.space_between;
        p.clip = n->clip;
        p.radius = fmt(n->radius[0]);
        p.radius_mixed = !(n->radius[0] == n->radius[1] && n->radius[1] == n->radius[2] && n->radius[2] == n->radius[3]);
        if (p.radius_mixed) p.radius = "Разные";
        p.radius_tl = fmt(n->radius[0]);
        p.radius_tr = fmt(n->radius[1]);
        p.radius_br = fmt(n->radius[2]);
        p.radius_bl = fmt(n->radius[3]);
        p.opacity = fmt(std::round(n->opacity * 100)) + "%";
        p.blend = d::blend_css(n->blend);
        if (!n->strokes.empty()) {
            const d::Stroke& s = n->strokes[0];
            p.has_stroke = true;
            p.stroke_hex = hex_of(s.color);
            p.stroke_swatch = swatch(s.color);
            p.stroke_width = fmt(s.width);
            p.stroke_align = kStrokeAlignWords[static_cast<int>(s.align)];
            p.stroke_style = kStrokeStyleWords[static_cast<int>(s.style)];
        }
        const d::TextStyle& t = n->text_style;
        p.content = n->text;
        p.family = t.family;
        p.text_style = t.style;
        p.size = t.size > 0 ? fmt(t.size) : "Экран";
        p.weight = std::to_string(t.weight);
        p.italic = t.italic;
        p.line_height = t.line_height > 0 ? fmt(t.line_height) : "Авто";
        p.letter_spacing = fmt(t.letter_spacing);
        p.text_align = kAlignWords[static_cast<int>(t.align)];
        p.text_case = kCaseWords[static_cast<int>(t.text_case)];
        p.decoration = kDecorationWords[static_cast<int>(t.decoration)];
        p.text_hex = hex_of(t.color);
        p.text_swatch = swatch(t.color);
        // Fills top first, as Figma lists them.
        for (int i = static_cast<int>(n->fills.size()) - 1; i >= 0; --i) {
            const d::Paint& f = n->fills[static_cast<usize>(i)];
            FillRow row;
            row.index = i;
            row.kind = kKindWords[static_cast<int>(f.kind)];
            row.kind_name = kKindNames[static_cast<int>(f.kind)];
            row.visible = f.visible;
            row.opacity = fmt(std::round(f.opacity * 100)) + "%";
            row.angle = fmt(f.angle);
            row.image = f.image;
            row.fit = kFitWords[static_cast<int>(f.fit)];
            row.tile = f.tile > 0 ? fmt(f.tile) : "Своя";
            row.offset_x = fmt(f.offset_x);
            row.offset_y = fmt(f.offset_y);
            row.style = f.style;
            if (f.kind == d::PaintKind::Solid) {
                row.hex = hex_of(f.color);
                row.swatch = swatch(f.color);
            } else if (f.kind == d::PaintKind::Image) {
                // The picture's name, without its folder and type.
                const usize slash = f.image.find_last_of('/');
                row.hex = slash == std::string::npos ? f.image : f.image.substr(slash + 1);
                const usize dot = row.hex.find_last_of('.');
                if (dot != std::string::npos && dot > 0) row.hex.resize(dot);
                row.swatch = "#808080";
            } else {
                const d::Color a = f.stops.empty() ? d::Color{} : f.stops.front().color;
                const d::Color b = f.stops.empty() ? d::Color{} : f.stops.back().color;
                row.hex = hex_of(a);
                row.hex2 = hex_of(b);
                row.swatch = swatch(a);
            }
            m_fills_.push_back(std::move(row));
        }
        if (p.root) {
            p.screen_font = screen_.text.family;
            p.screen_size = fmt(screen_.text.size);
            p.screen_text_hex = hex_of(screen_.text.color);
            p.screen_text_swatch = swatch(screen_.text.color);
            p.screen_fit = kScreenFitWords[static_cast<int>(screen_.fit)];
            p.bars_hex = hex_of(screen_.bars);
            p.bars_swatch = swatch(screen_.bars);
            p.safe = fmt(screen_.safe);
            p.screen_show = kScreenShowWords[static_cast<int>(screen_.show)];
            p.pauses = screen_.pauses;
            p.esc_closes = screen_.esc_closes;
            p.covers_game = covers_game();
        }
        p.show_if = n->show_if;
        p.list = n->list == d::ListSource::None ? "none" : d::list_word(n->list);
        p.list_gap = fmt(n->list_gap);
        p.list_no_cell = n->list != d::ListSource::None && n->children.empty();
        p.picture_from = n->picture_from;
        p.in_list.clear();
        {
            // In a list's cell (the list's first layer, or inside it)?
            const std::vector<u32> path = d::path_to(screen_.root, n->id);
            for (usize i = 0; i + 1 < path.size(); ++i) {
                const d::Node* up = d::find(screen_.root, path[i]);
                if (up && up->list != d::ListSource::None && !up->children.empty() && up->children.front().id == path[i + 1])
                    p.in_list = d::list_word(up->list);
            }
        }
        p.has_bar = !n->bar.value.empty();
        p.bar_value = n->bar.value;
        p.bar_max = n->bar.max;
        p.bar_from = kBarFromWords[static_cast<int>(n->bar.from)];
        {
            const d::Motion& m = n->motion;
            p.motion_kind = kMotionWords[static_cast<int>(m.kind)];
            p.motion_duration = fmt(m.duration);
            p.motion_delay = fmt(m.delay);
            p.motion_strength = fmt(std::round(m.strength * 100)) + "%";
            p.motion_easing = kEasingWords[static_cast<int>(m.easing)];
            p.motion_loop = m.loop;
            p.motion_back = m.back;
            p.smooth = fmt(n->smooth);
            p.smooth_easing = kEasingWords[static_cast<int>(n->smooth_easing)];
            m_keys_.clear();
            if (m.kind == d::MotionKind::Custom)
                for (usize i = 0; i < m.keys.size(); ++i) {
                    const d::MotionKey& k = m.keys[i];
                    KeyRow row;
                    row.index = static_cast<int>(i);
                    row.at = fmt(std::round(k.at * 100)) + "%";
                    row.x = fmt(k.x);
                    row.y = fmt(k.y);
                    row.scale = fmt(std::round(k.scale * 100)) + "%";
                    row.rotation = fmt(k.rotation);
                    row.opacity = fmt(std::round(k.opacity * 100)) + "%";
                    row.radius = k.radius >= 0 ? fmt(k.radius) : "";
                    row.blur = fmt(k.blur);
                    row.brightness = fmt(std::round(k.brightness * 100)) + "%";
                    row.tint = k.tint;
                    row.hex = hex_of(k.color);
                    row.swatch = swatch(k.color);
                    m_keys_.push_back(std::move(row));
                }
            if (p.root) {
                p.appear = kAppearWords[static_cast<int>(screen_.appear)];
                p.appear_time = fmt(screen_.appear_time);
            }
        }
        m_clicks_.clear();
        for (usize i = 0; i < n->on_click.size(); ++i) {
            const d::Action& a = n->on_click[i];
            ClickRow row;
            row.index = static_cast<int>(i);
            row.kind = d::action_word(a.kind);
            row.target = a.target;
            switch (a.kind) {
            case d::ActionKind::Show:
            case d::ActionKind::Hide:
            case d::ActionKind::Toggle: row.needs = "screen"; break;
            case d::ActionKind::Message: row.needs = "text"; row.hint = "имя сообщения: открыть_дверь"; break;
            case d::ActionKind::Change: row.needs = "text"; row.hint = "inv.coins -= 5; shop.bought = 1"; break;
            case d::ActionKind::Talk: row.needs = "text"; row.hint = "имя разговора"; break;
            default: break;
            }
            m_clicks_.push_back(std::move(row));
        }
        p.has_frame = !n->frame.image.empty();
        if (p.has_frame) {
            const d::FrameArt& f = n->frame;
            p.frame_visible = f.visible;
            p.frame_fill = f.fill;
            p.frame_image = f.image;
            p.frame_t = fmt(f.slice[0]);
            p.frame_r = fmt(f.slice[1]);
            p.frame_b = fmt(f.slice[2]);
            p.frame_l = fmt(f.slice[3]);
            p.frame_scale = fmt(std::round(f.scale * 100)) + "%";
            p.frame_repeat = kArtRepeatWords[static_cast<int>(f.repeat)];
        }
        p.has_mask = !n->mask.image.empty();
        if (p.has_mask) {
            p.mask_visible = n->mask.visible;
            p.mask_image = n->mask.image;
            p.mask_fit = kFitWords[static_cast<int>(n->mask.fit)];
        }
        for (usize i = 0; i < n->effects.size(); ++i) {
            const d::Effect& e = n->effects[i];
            EffectRow row;
            row.index = static_cast<int>(i);
            row.kind = kEffectWords[static_cast<int>(e.kind)];
            row.kind_name = kEffectNames[static_cast<int>(e.kind)];
            row.shadow = e.kind == d::EffectKind::DropShadow || e.kind == d::EffectKind::InnerShadow;
            row.hex = hex_of(e.color);
            row.swatch = swatch(e.color);
            row.x = fmt(e.x);
            row.y = fmt(e.y);
            row.blur = fmt(e.blur);
            row.spread = fmt(e.spread);
            row.visible = e.visible;
            m_effects_.push_back(std::move(row));
        }
    }
    // Components.
    m_variants_.clear();
    if (n && !p.many && !p.root) {
        const d::Node* inst = screen_.library ? nullptr : d::instance_of(screen_.root, n->id);
        const bool top_of_library = screen_.library && d::parent_of(screen_.root, n->id) == &screen_.root;
        p.instance = inst && inst->id == n->id;
        p.in_instance = inst && inst->id != n->id;
        p.changed_here = inst && !n->overrides.empty();
        p.lib_variant = top_of_library && !n->component.empty();
        p.can_make_component = !inst && (screen_.library ? top_of_library && n->component.empty() : true);
        const d::Node* named = p.lib_variant ? n : inst;
        if (named) {
            p.component = named->component;
            const std::vector<d::Component> list = d::components(library_);
            if (const d::Component* c = d::find_component(list, named->component))
                for (const d::ComponentProperty& prop : c->properties) {
                    VariantRow row;
                    row.property = prop.name;
                    row.value = d::variant_value(named->variant, prop.name);
                    for (const std::string& v : prop.values) row.values.push_back(v);
                    m_variants_.push_back(std::move(row));
                }
        }
        if (p.lib_variant) {
            // Its own values (a property the others lack too).
            for (const auto& [property, value] : n->variant)
                if (std::none_of(m_variants_.begin(), m_variants_.end(), [&](const VariantRow& r) { return r.property == property; }))
                    m_variants_.push_back({property, value, {value}});
        }
    }
    dirty("ue_variants");
    dirty("ue_clicks");
    dirty("ue_keys");
    // The values a screen can show, and the screens a click can open.
    m_values_.clear();
    if (!p.in_list.empty()) {
        const std::string in = p.in_list;
        const d::ListSource src = in == "quests" ? d::ListSource::Quests : in == "items" ? d::ListSource::Items : d::ListSource::None;
        for (const auto& [name, label] : d::list_fields(src)) m_values_.push_back({name, "Элемент списка: " + label});
    }
    if (game_values)
        for (const auto& [name, label] : game_values()) m_values_.push_back({name, label});
    dirty("ue_values");
    m_screen_names_.clear();
    for (const std::string& s : screens())
        if (s != name_) m_screen_names_.push_back(s);
    dirty("ue_screen_names");
    m_p_ = std::move(p);
    dirty("ue_p");
    dirty("ue_fills");
    dirty("ue_effects");
}

// --- changes ----------------------------------------------------------------

// The history's name for a change of a panel field, in the panel's words.
std::string change_label(const std::string& field) {
    auto starts = [&](const char* p) { return field.rfind(p, 0) == 0; };
    static const std::pair<const char*, const char*> words[] = {
        {"x", "Место"}, {"y", "Место"}, {"w", "Размер"}, {"h", "Размер"}, {"rotation", "Поворот"}, {"radius", "Скругление"},
        {"opacity", "Прозрачность"}, {"blend", "Наложение"}, {"text_style", "Стиль текста"}, {"text.insert", "Данные в тексте"},
        {"text", "Текст"}, {"family", "Шрифт"}, {"size", "Размер текста"}, {"weight", "Жирность"}, {"italic", "Курсив"},
        {"line_height", "Высота строки"}, {"letter_spacing", "Межбуквенный интервал"}, {"text_align", "Выравнивание текста"},
        {"text_case", "Регистр"}, {"decoration", "Подчёркивание"}, {"text_color", "Цвет текста"}, {"horizontal", "Прилипание"},
        {"vertical", "Прилипание"}, {"width_sizing", "Ширина"}, {"height_sizing", "Высота"}, {"absolute", "Вне раскладки"},
        {"clip", "Обрезка"}, {"show_if", "Условие показа"}, {"name", "Имя"}, {"list", "Список"}, {"list_gap", "Расстояние в списке"},
        {"picture_from", "Картинка из данных"}};
    for (const auto& [f, word] : words)
        if (field == f) return std::string("Изменено: ") + word;
    static const std::pair<const char*, const char*> groups[] = {
        {"fill", "Заливка"}, {"stroke", "Обводка"}, {"effect", "Эффекты"}, {"layout", "Автораскладка"}, {"frame.", "Рамка-картинка"},
        {"mask.", "Маска"}, {"click", "При нажатии"}, {"bar.", "Полоска"}, {"screen.", "Настройки экрана"}, {"color.", "Цвета игры"},
        {"textstyle.", "Стили текста"}, {"variant.", "Вариант"}, {"instance.", "Вариант копии"}, {"property.", "Свойство компонента"},
        {"component.", "Компонент"}, {"motion.", "Движение"}, {"smooth", "Плавная смена вида"}};
    for (const auto& [p, word] : groups)
        if (starts(p)) return std::string("Изменено: ") + word;
    return "Изменено";
}

bool UiEditor::set_on(d::Node& n, const std::string& field, const std::string& value) {
    const bool root = n.id == screen_.root.id;
    auto number = [&](f32 current) { return parse_number(value, current); };
    auto set_num = [&](f32& target, f32 lo = -1e6f, f32 hi = 1e6f) {
        const std::optional<f32> v = number(target);
        if (!v) return false;
        target = std::clamp(*v, lo, hi);
        return true;
    };
    auto color = [&](d::Color& target) {
        std::string v = value;
        if (!v.empty() && v[0] != '#') v = "#" + v;
        const std::optional<d::Color> c = d::parse_color(v);
        if (!c) return false;
        target = *c;
        return true;
    };
    auto percent = [&](f32& target) {
        const std::optional<f32> v = number(target * 100);
        if (!v) return false;
        target = std::clamp(*v / 100, 0.0f, 1.0f);
        return true;
    };
    // A percentage with no top (a size of 150%).
    auto percent_any = [&](f32& target) {
        const std::optional<f32> v = number(target * 100);
        if (!v) return false;
        target = std::clamp(*v / 100, 0.0f, 100.0f);
        return true;
    };
    // "fill.2.color" -> index 2, rest "color"
    auto indexed = [&](const char* prefix, usize& index, std::string& rest) {
        const std::string p = std::string(prefix) + ".";
        if (field.rfind(p, 0) != 0) return false;
        const std::string tail = field.substr(p.size());
        const usize dot = tail.find('.');
        if (dot == std::string::npos) return false;
        index = static_cast<usize>(std::atoi(tail.substr(0, dot).c_str()));
        rest = tail.substr(dot + 1);
        return true;
    };

    // The game's colours and text styles (the library's settings).
    if (root && screen_.library && (field.rfind("color.", 0) == 0 || field.rfind("textstyle.", 0) == 0)) {
        const bool is_color = field.rfind("color.", 0) == 0;
        const std::string rest = field.substr(is_color ? 6 : 10);
        if (rest == "add") {
            if (is_color) screen_.colors.push_back({d::fresh_style_key(screen_, true), "Цвет " + std::to_string(screen_.colors.size() + 1), {232, 176, 74, 255}});
            else {
                d::NamedTextStyle t;
                t.key = d::fresh_style_key(screen_, false);
                t.name = "Текст " + std::to_string(screen_.text_styles.size() + 1);
                t.style.family = m_families_.empty() ? std::string("Onest") : std::string(m_families_.front());
                t.style.size = 32;
                t.style.weight = 700;
                screen_.text_styles.push_back(std::move(t));
            }
            return true;
        }
        const usize dot = rest.find('.');
        if (dot == std::string::npos) return false;
        const usize i = static_cast<usize>(std::atoi(rest.substr(0, dot).c_str()));
        const std::string what = rest.substr(dot + 1);
        if (is_color) {
            if (i >= screen_.colors.size()) return false;
            d::NamedColor& c = screen_.colors[i];
            if (what == "remove") {
                screen_.colors.erase(screen_.colors.begin() + static_cast<std::ptrdiff_t>(i));
                return true;
            }
            if (what == "name") {
                if (value.empty()) return false;
                c.name = value;
                return true;
            }
            if (what == "color") return color(c.color);
            return false;
        }
        if (i >= screen_.text_styles.size()) return false;
        d::NamedTextStyle& t = screen_.text_styles[i];
        if (what == "remove") {
            screen_.text_styles.erase(screen_.text_styles.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
        if (what == "name") {
            if (value.empty()) return false;
            t.name = value;
            return true;
        }
        if (what == "family") {
            if (value.empty()) return false;
            t.style.family = value;
            return true;
        }
        if (what == "size") return set_num(t.style.size, 1, 2000);
        if (what == "weight") {
            f32 w = t.style.weight;
            if (!set_num(w, 100, 900)) return false;
            t.style.weight = static_cast<u16>(std::round(w / 100) * 100);
            return true;
        }
        if (what == "color") return color(t.style.color);
        return false;
    }

    // A component variant's value (library), an instance's choice (screens).
    if (field.rfind("variant.", 0) == 0) {
        if (!screen_.library || n.component.empty() || value.empty()) return false;
        d::set_variant_value(n.variant, field.substr(8), value);
        return true;
    }
    if (field.rfind("instance.", 0) == 0) {
        if (screen_.library || n.component.empty() || !n.master || value.empty()) return false;
        d::set_variant_value(n.variant, field.substr(9), value);
        d::sync_instance(screen_, n, library_);
        return true;
    }

    if (field == "name") {
        if (value.empty()) return false;
        n.name = value;
        if (root) screen_.title = value;
        return true;
    }
    if (field == "x" || field == "y") {
        if (root) return false;
        f32& target = field == "x" ? n.x : n.y;
        return set_num(target);
    }
    if (field == "w" || field == "h") {
        if (root) {
            f32& target = field == "w" ? screen_.width : screen_.height;
            if (!set_num(target, 16, 16384)) return false;
            n.w = screen_.width;
            n.h = screen_.height;
            return true;
        }
        f32& target = field == "w" ? n.w : n.h;
        if (!set_num(target, 0, 1e5f)) return false;
        (field == "w" ? n.width_sizing : n.height_sizing) = d::Sizing::Fixed;
        return true;
    }
    if (field == "rotation") return !root && set_num(n.rotation, -360, 360);
    if (field == "horizontal" || field == "vertical") {
        const int i = index_of(value, kConstraintWords);
        if (i < 0 || root) return false;
        (field == "horizontal" ? n.horizontal : n.vertical) = static_cast<d::Constraint>(i);
        return true;
    }
    if (field == "width_sizing" || field == "height_sizing") {
        const int i = index_of(value, kSizingWords);
        if (i < 0 || root) return false;
        (field == "width_sizing" ? n.width_sizing : n.height_sizing) = static_cast<d::Sizing>(i);
        return true;
    }
    if (field == "absolute") {
        n.absolute = !n.absolute;
        return !root;
    }
    if (field == "clip") {
        n.clip = !n.clip;
        return n.is_container();
    }
    if (field == "layout.mode") {
        const int i = index_of(value, kLayoutWords);
        if (i < 0 || !n.is_container()) return false;
        n.layout.mode = static_cast<d::LayoutMode>(i);
        return true;
    }
    if (field == "layout.gap") return set_num(n.layout.gap, -1e4f, 1e4f);
    if (field == "layout.pad") {
        f32 all = n.layout.padding[0];
        if (!set_num(all, 0, 1e4f)) return false;
        n.layout.padding = {all, all, all, all};
        return true;
    }
    if (field.rfind("layout.pad_", 0) == 0) {
        const std::string side = field.substr(11);
        const int i = side == "t" ? 0 : side == "r" ? 1 : side == "b" ? 2 : side == "l" ? 3 : -1;
        return i >= 0 && set_num(n.layout.padding[static_cast<usize>(i)], 0, 1e4f);
    }
    if (field == "layout.align") {
        const int a = std::atoi(value.c_str());
        if (a < 0 || a > 8) return false;
        n.layout.align = static_cast<u8>(a);
        return true;
    }
    if (field == "layout.space_between") {
        n.layout.space_between = !n.layout.space_between;
        return true;
    }
    if (field == "radius") {
        f32 r = n.radius[0];
        if (!set_num(r, 0, 1e5f)) return false;
        n.radius = {r, r, r, r};
        return true;
    }
    if (field.rfind("radius.", 0) == 0) {
        const int i = std::atoi(field.c_str() + 7);
        return i >= 0 && i < 4 && set_num(n.radius[static_cast<usize>(i)], 0, 1e5f);
    }
    if (field == "opacity") return percent(n.opacity);
    if (field == "blend") {
        for (int i = 0; i <= static_cast<int>(d::Blend::PlusLighter); ++i)
            if (value == d::blend_css(static_cast<d::Blend>(i))) {
                n.blend = static_cast<d::Blend>(i);
                return true;
            }
        return false;
    }

    // The link to the game: a text's values, a bar, a condition, clicks.
    if (field == "text.insert") { // a value of the game at the end of the text
        if (n.type != d::NodeType::Text || value.empty()) return false;
        n.text += (n.text.empty() || n.text.back() == ' ' ? "{" : " {") + value + "}";
        return true;
    }
    if (field == "show_if") {
        if (root || value == n.show_if) return false;
        n.show_if = value;
        return true;
    }
    if (field == "list") { // repeats the first layer for every element of a list of the game's
        if (root || !n.is_container()) return false;
        const d::ListSource s = value == "items" ? d::ListSource::Items : value == "quests" ? d::ListSource::Quests : d::ListSource::None;
        if (s == n.list) return false;
        n.list = s;
        return true;
    }
    if (field == "list_gap") return n.list != d::ListSource::None && set_num(n.list_gap, 0, 1e4f);
    if (field == "picture_from") {
        if (root || value == n.picture_from) return false;
        n.picture_from = value;
        return true;
    }
    if (field.rfind("bar.", 0) == 0) {
        if (root) return false;
        const std::string what = field.substr(4);
        if (what == "add") {
            if (!n.bar.value.empty()) return false;
            n.bar = d::Bar{};
            n.bar.value = "hero.hearts";
            n.bar.max = "hero.hearts_max";
            return true;
        }
        if (n.bar.value.empty()) return false;
        if (what == "remove") n.bar = d::Bar{};
        else if (what == "value") {
            if (value.empty() || value == n.bar.value) return false;
            n.bar.value = value;
        } else if (what == "max") {
            if (value.empty() || value == n.bar.max) return false;
            n.bar.max = value;
        } else if (what == "from") {
            const int i = index_of(value, kBarFromWords);
            if (i < 0) return false;
            n.bar.from = static_cast<d::BarFrom>(i);
        } else return false;
        return true;
    }
    if (field == "click.add") {
        if (root) return false;
        d::Action a;
        // A first guess that is often right: a button opens another screen.
        a.kind = d::ActionKind::Show;
        const std::vector<std::string> list = screens();
        for (const std::string& s : list)
            if (s != name_) {
                a.target = s;
                break;
            }
        if (a.target.empty()) a.kind = d::ActionKind::Close;
        n.on_click.push_back(a);
        return true;
    }
    {
        usize i = 0;
        std::string rest;
        if (indexed("click", i, rest)) {
            if (root || i >= n.on_click.size()) return false;
            d::Action& a = n.on_click[i];
            if (rest == "remove") n.on_click.erase(n.on_click.begin() + static_cast<std::ptrdiff_t>(i));
            else if (rest == "kind") {
                const std::optional<d::ActionKind> k = d::parse_action(value);
                if (!k || *k == a.kind) return false;
                a.kind = *k;
                if (*k == d::ActionKind::Show || *k == d::ActionKind::Hide || *k == d::ActionKind::Toggle) {
                    if (std::find(m_screen_names_.begin(), m_screen_names_.end(), a.target) == m_screen_names_.end())
                        a.target = m_screen_names_.empty() ? std::string() : std::string(m_screen_names_.front());
                } else a.target.clear();
            } else if (rest == "target") {
                if (value == a.target) return false;
                a.target = value;
            } else return false;
            return true;
        }
    }

    // Movement: the layer's own, and how smoothly its look changes.
    if (field == "smooth") return !root && set_num(n.smooth, 0, 10);
    if (field == "smooth.easing") {
        const int i = index_of(value, kEasingWords);
        if (i < 0 || n.smooth_easing == static_cast<d::Easing>(i)) return false;
        n.smooth_easing = static_cast<d::Easing>(i);
        return true;
    }
    if (field.rfind("motion.", 0) == 0) {
        if (root) return false; // the screen comes and goes (screen.appear) instead
        d::Motion& m = n.motion;
        const std::string what = field.substr(7);
        if (what == "kind") {
            const int i = index_of(value, kMotionWords);
            if (i < 0 || m.kind == static_cast<d::MotionKind>(i)) return false;
            const d::MotionKind was = m.kind;
            m.kind = static_cast<d::MotionKind>(i);
            if (m.kind == d::MotionKind::Custom && m.keys.empty()) {
                // Starts from what the preset did, to change from there.
                d::Motion from = m;
                from.kind = was == d::MotionKind::None ? d::MotionKind::Pulse : was;
                m.keys = d::motion_keys(from);
            }
            if (m.kind == d::MotionKind::Spin && was == d::MotionKind::None) m.easing = d::Easing::Linear;
            return true;
        }
        if (m.kind == d::MotionKind::None) return false;
        if (what == "duration") return set_num(m.duration, 0.05f, 600);
        if (what == "delay") return set_num(m.delay, 0, 600);
        if (what == "strength") return percent_any(m.strength);
        if (what == "loop") {
            m.loop = !m.loop;
            return true;
        }
        if (what == "back") {
            m.back = !m.back;
            return true;
        }
        if (what == "easing") {
            const int i = index_of(value, kEasingWords);
            if (i < 0 || m.easing == static_cast<d::Easing>(i)) return false;
            m.easing = static_cast<d::Easing>(i);
            return true;
        }
        if (m.kind != d::MotionKind::Custom) return false;
        if (what == "key.add") {
            // Halfway between the last two keys, or at the end.
            d::MotionKey k;
            std::vector<d::MotionKey> keys = d::motion_keys(m);
            k.at = keys.size() >= 2 ? (keys[keys.size() - 2].at + keys.back().at) * 0.5f : 0.5f;
            m.keys.push_back(k);
            std::stable_sort(m.keys.begin(), m.keys.end(), [](const d::MotionKey& a, const d::MotionKey& b) { return a.at < b.at; });
            return true;
        }
        usize i = 0;
        std::string rest;
        if (!indexed("motion.key", i, rest) || i >= m.keys.size()) return false;
        d::MotionKey& k = m.keys[i];
        if (rest == "remove") {
            m.keys.erase(m.keys.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
        if (rest == "at") {
            if (!percent(k.at)) return false;
            std::stable_sort(m.keys.begin(), m.keys.end(), [](const d::MotionKey& a, const d::MotionKey& b) { return a.at < b.at; });
            return true;
        }
        if (rest == "x") return set_num(k.x);
        if (rest == "y") return set_num(k.y);
        if (rest == "scale") return percent_any(k.scale);
        if (rest == "rotation") return set_num(k.rotation, -3600, 3600);
        if (rest == "opacity") return percent(k.opacity);
        if (rest == "radius") {
            if (value.empty()) {
                if (k.radius < 0) return false;
                k.radius = -1;
                return true;
            }
            f32 r = std::max(k.radius, 0.0f);
            if (!set_num(r, 0, 10000)) return false;
            k.radius = r;
            return true;
        }
        if (rest == "blur") return set_num(k.blur, 0, 500);
        if (rest == "brightness") return percent_any(k.brightness);
        if (rest == "color") {
            if (value.empty()) {
                if (!k.tint) return false;
                k.tint = false;
                return true;
            }
            if (!color(k.color)) return false;
            k.tint = true;
            return true;
        }
        if (rest == "color.add") {
            if (k.tint) return false;
            k.tint = true;
            k.color = n.type == d::NodeType::Text ? n.text_style.color : d::Color{232, 176, 74, 255};
            return true;
        }
        return false;
    }

    // The screen's settings.
    if (field.rfind("screen.", 0) == 0) {
        if (!root) return false;
        const std::string f = field.substr(7);
        if (f == "show") {
            const int i = index_of(value, kScreenShowWords);
            if (i < 0 || screen_.show == static_cast<d::ScreenShow>(i)) return false;
            screen_.show = static_cast<d::ScreenShow>(i);
            return true;
        }
        if (f == "pauses") {
            screen_.pauses = !screen_.pauses;
            return true;
        }
        if (f == "esc") {
            screen_.esc_closes = !screen_.esc_closes;
            return true;
        }
        if (f == "appear") {
            const int i = index_of(value, kAppearWords);
            if (i < 0 || screen_.appear == static_cast<d::Appear>(i)) return false;
            screen_.appear = static_cast<d::Appear>(i);
            return true;
        }
        if (f == "appear_time") return set_num(screen_.appear_time, 0.05f, 10);
        if (f == "font") {
            if (value.empty()) return false;
            screen_.text.family = value;
            return true;
        }
        if (f == "font_size") return set_num(screen_.text.size, 1, 2000);
        if (f == "text_color") return color(screen_.text.color);
        if (f == "fit") {
            const int i = index_of(value, kScreenFitWords);
            if (i < 0) return false;
            screen_.fit = static_cast<d::ScreenFit>(i);
            return true;
        }
        if (f == "bars") return color(screen_.bars);
        if (f == "safe") return set_num(screen_.safe, 0, std::min(screen_.width, screen_.height) * 0.45f);
        return false;
    }

    // The frame picture.
    if (field.rfind("frame.", 0) == 0) {
        if (root || n.type == d::NodeType::Text) return false;
        d::FrameArt& f = n.frame;
        const std::string what = field.substr(6);
        if (what == "add") {
            if (!f.image.empty()) return false;
            std::string pick = std::string(kArtFolder) + "/рамка_дерево.png";
            if (std::none_of(m_pictures_.begin(), m_pictures_.end(), [&](const PictureRow& r) { return r.path == pick; }))
                pick = m_pictures_.empty() ? std::string() : std::string(m_pictures_.front().path);
            if (pick.empty()) return false;
            f = d::FrameArt{};
            f.image = pick;
            f.slice = art_slice(pick);
            return true;
        }
        if (f.image.empty()) return false;
        if (what == "remove") {
            f = d::FrameArt{};
            return true;
        }
        if (what == "visible") {
            f.visible = !f.visible;
            return true;
        }
        if (what == "fill") {
            f.fill = !f.fill;
            return true;
        }
        if (what == "image") {
            if (value.empty()) return false;
            f.image = value;
            f.slice = art_slice(value);
            return true;
        }
        if (what == "scale") {
            f32 pct = f.scale * 100;
            if (!set_num(pct, 1, 10000)) return false;
            f.scale = pct / 100;
            return true;
        }
        if (what == "repeat") {
            const int i = index_of(value, kArtRepeatWords);
            if (i < 0) return false;
            f.repeat = static_cast<d::ArtRepeat>(i);
            return true;
        }
        if (what.rfind("slice.", 0) == 0) {
            const int i = std::atoi(what.substr(6).c_str());
            return i >= 0 && i < 4 && set_num(f.slice[static_cast<usize>(i)], 0, 4096);
        }
        return false;
    }

    // The mask picture.
    if (field.rfind("mask.", 0) == 0) {
        if (root) return false;
        d::MaskArt& m = n.mask;
        const std::string what = field.substr(5);
        if (what == "add") {
            if (!m.image.empty()) return false;
            std::string pick = std::string(kArtFolder) + "/клякса.png";
            if (std::none_of(m_pictures_.begin(), m_pictures_.end(), [&](const PictureRow& r) { return r.path == pick; }))
                pick = m_pictures_.empty() ? std::string() : std::string(m_pictures_.front().path);
            if (pick.empty()) return false;
            m = d::MaskArt{};
            m.image = pick;
            return true;
        }
        if (m.image.empty()) return false;
        if (what == "remove") {
            m = d::MaskArt{};
            return true;
        }
        if (what == "visible") {
            m.visible = !m.visible;
            return true;
        }
        if (what == "image") {
            if (value.empty()) return false;
            m.image = value;
            return true;
        }
        if (what == "fit") {
            const int i = index_of(value, kFitWords);
            if (i < 0) return false;
            m.fit = static_cast<d::ImageFit>(i);
            return true;
        }
        return false;
    }

    // Fills.
    if (field == "fill.add") {
        d::Paint p;
        p.color = n.fills.empty() && n.type != d::NodeType::Frame ? d::Color{0xd9, 0xd9, 0xd9, 255} : d::Color{255, 255, 255, 255};
        if (!n.fills.empty()) p = n.fills.back();
        n.fills.push_back(p);
        return true;
    }
    usize index = 0;
    std::string rest;
    if (indexed("fill", index, rest)) {
        if (index >= n.fills.size()) return false;
        d::Paint& f = n.fills[index];
        if (rest == "remove") {
            n.fills.erase(n.fills.begin() + static_cast<std::ptrdiff_t>(index));
            return true;
        }
        if (rest == "visible") {
            f.visible = !f.visible;
            return true;
        }
        if (rest == "opacity") return percent(f.opacity);
        if (rest == "angle") return set_num(f.angle, -3600, 3600);
        if (rest == "image") {
            f.image = value;
            return true;
        }
        if (rest == "tile") {
            if (value.empty() || value == "Своя") {
                f.tile = 0;
                return true;
            }
            return set_num(f.tile, 0, 1e4f);
        }
        if (rest == "offset_x") return set_num(f.offset_x, -1e4f, 1e4f);
        if (rest == "offset_y") return set_num(f.offset_y, -1e4f, 1e4f);
        if (rest == "fit") {
            const int i = index_of(value, kFitWords);
            if (i < 0) return false;
            f.fit = static_cast<d::ImageFit>(i);
            return true;
        }
        if (rest == "kind") {
            const int i = index_of(value, kKindWords);
            if (i < 0) return false;
            const d::PaintKind kind = static_cast<d::PaintKind>(i);
            if ((kind == d::PaintKind::Linear || kind == d::PaintKind::Radial) && f.stops.empty()) {
                d::Color end = f.color;
                end.a = 0;
                f.stops = {{f.color, 0}, {end, 1}};
            }
            if (kind == d::PaintKind::Solid && f.kind != d::PaintKind::Solid && !f.stops.empty()) f.color = f.stops.front().color;
            f.kind = kind;
            return true;
        }
        if (rest == "style") {
            // A colour of the game's, or "" for the layer's own.
            if (value.empty()) {
                f.style.clear();
                return true;
            }
            for (const d::NamedColor& c : library_.colors)
                if (c.key == value) {
                    f.kind = d::PaintKind::Solid;
                    f.color = c.color;
                    f.style = c.key;
                    return true;
                }
            return false;
        }
        if (rest == "color") {
            f.style.clear(); // its own colour now
            if (f.kind == d::PaintKind::Solid) return color(f.color);
            if (f.stops.empty()) f.stops = {{f.color, 0}, {f.color, 1}};
            return color(f.stops.front().color);
        }
        if (rest == "color2") {
            if (f.stops.size() < 2) f.stops = {{f.color, 0}, {f.color, 1}};
            return color(f.stops.back().color);
        }
        return false;
    }

    // Stroke (one).
    if (field == "stroke.add") {
        if (!n.strokes.empty()) return false;
        n.strokes.push_back(d::Stroke{});
        return true;
    }
    if (field.rfind("stroke.", 0) == 0) {
        if (n.strokes.empty()) return false;
        d::Stroke& s = n.strokes[0];
        const std::string what = field.substr(7);
        if (what == "remove") {
            n.strokes.clear();
            return true;
        }
        if (what == "visible") {
            s.visible = !s.visible;
            return true;
        }
        if (what == "color") return color(s.color);
        if (what == "width") return set_num(s.width, 0, 1000);
        if (what == "align") {
            const int i = index_of(value, kStrokeAlignWords);
            if (i < 0) return false;
            s.align = static_cast<d::StrokeAlign>(i);
            return true;
        }
        if (what == "style") {
            const int i = index_of(value, kStrokeStyleWords);
            if (i < 0) return false;
            s.style = static_cast<d::StrokeStyle>(i);
            return true;
        }
        return false;
    }

    // Effects.
    if (field == "effect.add") {
        n.effects.push_back(d::Effect{});
        return true;
    }
    if (indexed("effect", index, rest)) {
        if (index >= n.effects.size()) return false;
        d::Effect& e = n.effects[index];
        if (rest == "remove") {
            n.effects.erase(n.effects.begin() + static_cast<std::ptrdiff_t>(index));
            return true;
        }
        if (rest == "visible") {
            e.visible = !e.visible;
            return true;
        }
        if (rest == "kind") {
            const int i = index_of(value, kEffectWords);
            if (i < 0) return false;
            e.kind = static_cast<d::EffectKind>(i);
            return true;
        }
        if (rest == "color") return color(e.color);
        if (rest == "x") return set_num(e.x);
        if (rest == "y") return set_num(e.y);
        if (rest == "blur") return set_num(e.blur, 0, 1000);
        if (rest == "spread") return set_num(e.spread, -1000, 1000);
        return false;
    }

    // Text.
    if (n.type != d::NodeType::Text) return false;
    d::TextStyle& t = n.text_style;
    if (field == "text") {
        n.text = value;
        return true;
    }
    if (field == "text_style") {
        if (value.empty()) {
            t.style.clear();
            return true;
        }
        for (const d::NamedTextStyle& named : library_.text_styles)
            if (named.key == value) {
                const d::TextAlign align = t.align;
                t = named.style;
                t.style = named.key;
                t.align = align;
                return true;
            }
        return false;
    }
    // A change of the look by hand: the text no longer follows its style.
    if (field != "text_align") t.style.clear();
    if (field == "family") {
        t.family = value; // empty: the screen's font
        return true;
    }
    if (field == "size") {
        if (value.empty() || value == "Экран") {
            t.size = 0;
            return true;
        }
        if (t.size <= 0) t.size = screen_.text.size;
        return set_num(t.size, 1, 2000);
    }
    if (field == "weight") {
        f32 w = t.weight;
        if (!set_num(w, 100, 900)) return false;
        t.weight = static_cast<u16>(std::lround(w / 100) * 100);
        return true;
    }
    if (field == "italic") {
        t.italic = !t.italic;
        return true;
    }
    if (field == "line_height") {
        if (value.empty() || value == "Авто" || value == "auto") {
            t.line_height = 0;
            return true;
        }
        return set_num(t.line_height, 0, 4000);
    }
    if (field == "letter_spacing") return set_num(t.letter_spacing, -200, 200);
    if (field == "text_align") {
        const int i = index_of(value, kAlignWords);
        if (i < 0) return false;
        t.align = static_cast<d::TextAlign>(i);
        return true;
    }
    if (field == "text_case") {
        const int i = index_of(value, kCaseWords);
        if (i < 0) return false;
        t.text_case = static_cast<d::TextCase>(i);
        return true;
    }
    if (field == "decoration") {
        const int i = index_of(value, kDecorationWords);
        if (i < 0) return false;
        t.decoration = static_cast<d::TextDecoration>(i);
        return true;
    }
    if (field == "text_color") return color(t.color);
    return false;
}

bool UiEditor::set_property(const std::string& field, const std::string& value) {
    const std::string before = d::save_screen(screen_);
    bool any = false;
    // Nothing selected: the screen's settings.
    const std::vector<u32> targets = selection_.empty() ? std::vector<u32>{screen_.root.id} : selection_;
    // A component's name and its properties' names: on all its variants.
    if (screen_.library && (field == "component.name" || field.rfind("property.", 0) == 0)) {
        const d::Node* n = targets.empty() ? nullptr : d::find(screen_.root, targets[0]);
        if (!n || n->component.empty() || value.empty()) return false;
        const std::string component = n->component;
        const std::string old = field == "component.name" ? std::string() : field.substr(9);
        const std::vector<d::Component> list = d::components(screen_);
        if (field == "component.name" && value != component && d::find_component(list, value)) return false; // taken
        for (d::Node& v : screen_.root.children) {
            if (v.component != component) continue;
            if (field == "component.name") v.component = value;
            else
                for (auto& [property, val] : v.variant)
                    if (property == old) property = value;
            any = true;
        }
        if (!any) return false;
        commit(before, "Переименовано");
        return true;
    }
    // A layer that is not a plain frame (a component's copy, a rectangle...)
    // becomes the cell of a new list around it.
    if (field == "list" && targets.size() == 1 && value != "none" && !screen_.library) {
        const d::Node* n = d::find(screen_.root, targets[0]);
        if (n && n->id != screen_.root.id && (!n->is_container() || !n->component.empty()) &&
            !d::instance_of(screen_.root, d::parent_of(screen_.root, n->id)->id))
            return make_list(n->id, value == "quests" ? d::ListSource::Quests : d::ListSource::Items);
    }
    for (u32 id : targets)
        if (d::Node* n = d::find(screen_.root, id)) {
            const bool copy = !screen_.library && n->master && d::instance_of(screen_.root, n->id);
            const std::string was = copy ? d::save_screen(screen_) : std::string();
            if (!set_on(*n, field, value)) continue;
            any = true;
            // Inside a copy of a component: this copy's own change, kept when the component changes
            // (a field given the value it had is no change).
            if (copy && d::save_screen(screen_) != was) {
                const std::string kept = d::override_of(field);
                const bool top = !n->component.empty();
                if (!kept.empty() && !(top && kept == "place") &&
                    std::find(n->overrides.begin(), n->overrides.end(), kept) == n->overrides.end())
                    n->overrides.push_back(kept);
            }
        }
    if (!any) {
        // Back to what it was (a field that did not take the value shows the old one).
        d::load_screen(before, screen_);
        refresh_props();
        return false;
    }
    commit(before, change_label(field));
    return true;
}

void UiEditor::select(const std::vector<u32>& ids) {
    selection_.clear();
    for (u32 id : ids)
        if (d::find(screen_.root, id) && std::find(selection_.begin(), selection_.end(), id) == selection_.end())
            selection_.push_back(id);
    // Show it in the layer list: open the frames it is in.
    for (u32 id : selection_)
        for (u32 a : d::path_to(screen_.root, id))
            if (a != id) closed_.erase(std::remove(closed_.begin(), closed_.end(), a), closed_.end());
    refresh_layers();
    refresh_props();
    refresh_overlay();
}

void UiEditor::set_tool(Tool tool) {
    tool_ = tool;
    static const char* names[] = {"select", "frame", "rectangle", "ellipse", "text"};
    m_tool_ = names[static_cast<int>(tool)];
    dirty("ue_tool");
}

u32 UiEditor::add_layer(d::NodeType type, f32 x, f32 y, f32 w, f32 h, u32 parent_id) {
    d::Node* parent = parent_id ? d::find(screen_.root, parent_id) : &screen_.root;
    if (!parent || !parent->is_container()) parent = &screen_.root;
    if (!screen_.library && d::instance_of(screen_.root, parent->id)) parent = &screen_.root; // a copy of a component keeps its layers
    const d::Rect pbox = layer_box(parent->id).value_or(d::Rect{});
    d::Node n;
    n.id = screen_.next_id++;
    n.type = type;
    n.name = d::fresh_name(screen_, type);
    n.x = std::round(x - pbox.x);
    n.y = std::round(y - pbox.y);
    n.w = std::max(std::round(w), 1.0f);
    n.h = std::max(std::round(h), 1.0f);
    switch (type) {
    case d::NodeType::Frame: n.fills.push_back(solid(d::Color{255, 255, 255, 255})); break;
    case d::NodeType::Rectangle:
    case d::NodeType::Ellipse: n.fills.push_back(solid(d::Color{0xd9, 0xd9, 0xd9, 255})); break;
    case d::NodeType::Text:
        n.text = "Текст";
        n.width_sizing = d::Sizing::Hug;
        n.height_sizing = d::Sizing::Hug;
        n.text_style.family.clear(); // the screen's font and size
        n.text_style.size = 0;
        n.text_style.color = screen_.text.color;
        break;
    case d::NodeType::Image: {
        d::Paint p;
        p.kind = d::PaintKind::Image;
        n.fills.push_back(p);
        break;
    }
    }
    const u32 id = n.id;
    parent->children.push_back(std::move(n));
    selection_ = {id};
    page_dirty_ = true;
    return id;
}

bool UiEditor::remove_selection() {
    if (selection_.empty()) return false;
    const std::string before = d::save_screen(screen_);
    bool any = false;
    for (u32 id : selection_) {
        // A layer of a copy of a component stays (it can be hidden).
        const d::Node* inst = screen_.library ? nullptr : d::instance_of(screen_.root, id);
        if (inst && inst->id != id) continue;
        if (id != screen_.root.id && d::remove(screen_.root, id)) any = true;
    }
    if (!any) return false;
    selection_.clear();
    commit(before, "Удалено");
    return true;
}

bool UiEditor::duplicate_selection() {
    if (selection_.empty()) return false;
    const std::string before = d::save_screen(screen_);
    std::vector<u32> fresh;
    for (u32 id : selection_) {
        if (id == screen_.root.id) continue;
        if (const d::Node* inst = screen_.library ? nullptr : d::instance_of(screen_.root, id); inst && inst->id != id) continue;
        d::Node* parent = d::parent_of(screen_.root, id);
        if (!parent) continue;
        auto it = std::find_if(parent->children.begin(), parent->children.end(), [&](const d::Node& c) { return c.id == id; });
        if (it == parent->children.end()) continue;
        d::Node copy = *it;
        d::renumber(screen_, copy);
        if (parent->layout.mode == d::LayoutMode::None || copy.absolute) {
            copy.x += 10;
            copy.y += 10;
        }
        fresh.push_back(copy.id);
        parent->children.insert(it + 1, std::move(copy));
    }
    if (fresh.empty()) return false;
    selection_ = fresh;
    commit(before, "Создана копия");
    return true;
}

bool UiEditor::make_list(u32 cell_id, d::ListSource source) {
    d::Node* parent = d::parent_of(screen_.root, cell_id);
    if (!parent || source == d::ListSource::None) return false;
    const std::string before = d::save_screen(screen_);
    auto it = std::find_if(parent->children.begin(), parent->children.end(), [&](const d::Node& c) { return c.id == cell_id; });
    if (it == parent->children.end()) return false;
    d::Node cell = std::move(*it);
    const usize at = static_cast<usize>(it - parent->children.begin());
    parent->children.erase(it);
    // The list stands where the cell was: room for a few of them under each other.
    d::Node list;
    list.id = screen_.next_id++;
    list.type = d::NodeType::Frame;
    list.name = source == d::ListSource::Quests ? "Список заданий" : "Список вещей";
    list.list = source;
    list.x = cell.x;
    list.y = cell.y;
    list.w = std::round(cell.w + list.list_gap * 2);
    list.h = std::round(cell.h * 5 + list.list_gap * 6);
    list.horizontal = cell.horizontal;
    list.vertical = cell.vertical;
    cell.x = list.list_gap;
    cell.y = list.list_gap;
    cell.absolute = false;
    cell.horizontal = d::Constraint::Start;
    cell.vertical = d::Constraint::Start;
    // What the list shows while it is empty.
    d::Node empty;
    empty.id = screen_.next_id++;
    empty.type = d::NodeType::Text;
    empty.name = "Пока пусто";
    empty.text = source == d::ListSource::Quests ? "Заданий пока нет" : "Пока ничего нет";
    empty.x = list.list_gap;
    empty.y = list.list_gap;
    empty.w = std::max(cell.w, 120.0f);
    empty.h = 32;
    empty.text_style.size = 20;
    list.children.push_back(std::move(cell));
    list.children.push_back(std::move(empty));
    const u32 id = list.id;
    parent->children.insert(parent->children.begin() + static_cast<std::ptrdiff_t>(std::min(at, parent->children.size())), std::move(list));
    selection_ = {id};
    commit(before, "Сделан список");
    return true;
}

bool UiEditor::wrap_selection_in_frame() {
    if (selection_.empty() || selection_[0] == screen_.root.id) return false;
    d::Node* parent = d::parent_of(screen_.root, selection_[0]);
    if (!parent) return false;
    // Only siblings go into one frame, and not inside a copy of a component.
    for (u32 id : selection_)
        if (d::parent_of(screen_.root, id) != parent) return false;
    if (!screen_.library && d::instance_of(screen_.root, parent->id)) return false;
    const std::string before = d::save_screen(screen_);
    const d::Rect box = selection_box();
    const d::Rect pbox = layer_box(parent->id).value_or(d::Rect{});
    d::Node frame;
    frame.id = screen_.next_id++;
    frame.type = d::NodeType::Frame;
    frame.name = d::fresh_name(screen_, d::NodeType::Frame);
    frame.x = std::round(box.x - pbox.x);
    frame.y = std::round(box.y - pbox.y);
    frame.w = std::round(box.w);
    frame.h = std::round(box.h);
    // In the parent's order, where the topmost of them was.
    usize at = parent->children.size();
    for (usize i = 0; i < parent->children.size();) {
        d::Node& c = parent->children[i];
        if (std::find(selection_.begin(), selection_.end(), c.id) == selection_.end()) {
            ++i;
            continue;
        }
        if (const auto b = layer_box(c.id)) {
            c.x = std::round(b->x - box.x);
            c.y = std::round(b->y - box.y);
        }
        c.absolute = false;
        frame.children.push_back(std::move(c));
        parent->children.erase(parent->children.begin() + static_cast<std::ptrdiff_t>(i));
        at = i;
    }
    at = std::min(at, parent->children.size());
    const u32 id = frame.id;
    parent->children.insert(parent->children.begin() + static_cast<std::ptrdiff_t>(at), std::move(frame));
    selection_ = {id};
    commit(before, "Рамка вокруг выделенного");
    return true;
}

bool UiEditor::add_auto_layout() {
    if (selection_.empty()) return false;
    d::Node* n = d::find(screen_.root, selection_[0]);
    if (!n) return false;
    if (!n->is_container() || (selection_.size() > 1)) {
        // Like Figma: Shift+A on layers puts them into a frame with auto layout.
        if (!wrap_selection_in_frame()) return false;
        n = d::find(screen_.root, selection_[0]);
        if (!n) return false;
    }
    const std::string before = d::save_screen(screen_);
    // Row or column: the way the children already go.
    bool column = false;
    if (n->children.size() >= 2) {
        f32 sx = 0, sy = 0;
        for (usize i = 1; i < n->children.size(); ++i) {
            sx += std::fabs(n->children[i].x - n->children[i - 1].x);
            sy += std::fabs(n->children[i].y - n->children[i - 1].y);
        }
        column = sy > sx;
        // In reading order.
        std::stable_sort(n->children.begin(), n->children.end(), [&](const d::Node& a, const d::Node& b) {
            return column ? a.y < b.y : a.x < b.x;
        });
    }
    n->layout.mode = column ? d::LayoutMode::Column : d::LayoutMode::Row;
    if (n->layout.gap == 0) n->layout.gap = 10;
    if (n->layout.padding == std::array<f32, 4>{}) n->layout.padding = {10, 10, 10, 10};
    n->width_sizing = d::Sizing::Hug;
    n->height_sizing = d::Sizing::Hug;
    remember_geometry(before);
    commit(before, "Автораскладка");
    return true;
}

bool UiEditor::move_selection(f32 dx, f32 dy) {
    if (selection_.empty()) return false;
    const std::string before = d::save_screen(screen_);
    for (u32 id : selection_) {
        if (id == screen_.root.id) continue;
        d::Node* n = d::find(screen_.root, id);
        d::Node* parent = d::parent_of(screen_.root, id);
        if (!n || !parent || (parent->layout.mode != d::LayoutMode::None && !n->absolute)) continue;
        n->x += dx;
        n->y += dy;
    }
    remember_geometry(before);
    commit(before, "Сдвинуто", "nudge");
    return true;
}

// --- frame and input --------------------------------------------------------

// --- components -------------------------------------------------------------

bool UiEditor::make_component() {
    if (selection_.size() != 1) return false;
    d::Node* n = d::find(screen_.root, selection_[0]);
    if (!n || n->id == screen_.root.id) return false;
    if (!screen_.library && d::instance_of(screen_.root, n->id)) return false;
    const std::vector<d::Component> list = d::components(library_);
    const std::string base = n->name.empty() ? std::string("Компонент") : n->name;
    std::string name = base;
    for (int k = 2; d::find_component(list, name); ++k) name = base + " " + std::to_string(k);
    if (screen_.library) {
        // In the library: a layer at the top is named a component.
        if (d::parent_of(screen_.root, n->id) != &screen_.root || !n->component.empty()) return false;
        const std::string before = d::save_screen(screen_);
        n->component = name;
        commit(before, "Сделан компонент «" + name + "»");
        return true;
    }
    // On a screen: the layer goes into the library, a copy of it takes its place.
    const std::string label = "Сделан компонент «" + name + "»";
    const d::Rect box = layer_box(n->id).value_or(d::Rect{n->x, n->y, n->w, n->h});
    d::Screen lib = library_;
    d::Node master = *n;
    d::renumber(lib, master);
    master.component = name;
    master.variant.clear();
    master.master = 0;
    master.overrides.clear();
    master.horizontal = master.vertical = d::Constraint::Start;
    master.absolute = false;
    master.locked = false;
    master.visible = true;
    if (master.width_sizing == d::Sizing::Fill) {
        master.width_sizing = d::Sizing::Fixed;
        master.w = std::round(box.w);
    }
    if (master.height_sizing == d::Sizing::Fill) {
        master.height_sizing = d::Sizing::Fixed;
        master.h = std::round(box.h);
    }
    f32 right = 80;
    for (const d::Node& c : lib.root.children) right = std::max(right, c.x + c.w + 80);
    master.x = right;
    master.y = 80;
    lib.root.children.push_back(std::move(master));
    history_.seal();
    history_.begin_group(label);
    history_.execute(std::make_unique<UiCommand>(*this, kLibrary, d::save_screen(library_), d::save_screen(lib), selection_,
                                                 selection_, label, std::string()));
    // The screen: the copy where the layer was.
    const std::string before = d::save_screen(screen_);
    n = d::find(screen_.root, selection_.empty() ? 0 : selection_[0]);
    std::optional<d::Node> inst = n ? d::make_instance(screen_, library_, name) : std::nullopt;
    if (inst) {
        inst->name = n->name;
        inst->x = n->x;
        inst->y = n->y;
        inst->rotation = n->rotation;
        inst->horizontal = n->horizontal;
        inst->vertical = n->vertical;
        inst->width_sizing = n->width_sizing;
        inst->height_sizing = n->height_sizing;
        inst->absolute = n->absolute;
        inst->visible = n->visible;
        const u32 id = inst->id;
        *n = std::move(*inst);
        selection_ = {id};
        commit(before, label);
    }
    history_.end_group();
    history_.seal();
    return inst.has_value();
}

u32 UiEditor::place_component(const std::string& component) {
    if (screen_.library) {
        // In the library: show it.
        for (const d::Node& v : screen_.root.children)
            if (v.component == component) {
                select({v.id});
                return v.id;
            }
        return 0;
    }
    const std::string before = d::save_screen(screen_);
    std::optional<d::Node> inst = d::make_instance(screen_, library_, component);
    if (!inst) return 0;
    inst->x = std::round((screen_.width - inst->w) * 0.5f);
    inst->y = std::round((screen_.height - inst->h) * 0.5f);
    const u32 id = inst->id;
    screen_.root.children.push_back(std::move(*inst));
    selection_ = {id};
    commit(before, "Добавлен компонент «" + component + "»");
    return id;
}

bool UiEditor::add_variant(bool states) {
    if (!screen_.library || selection_.size() != 1) return false;
    d::Node* v = d::find(screen_.root, selection_[0]);
    if (!v || v->component.empty() || d::parent_of(screen_.root, v->id) != &screen_.root) return false;
    const std::string before = d::save_screen(screen_);
    const std::string component = v->component;
    auto exists = [&](const std::vector<std::pair<std::string, std::string>>& values) {
        for (const d::Node& o : screen_.root.children)
            if (o.component == component && o.variant == values) return true;
        return false;
    };
    std::vector<d::Node> made;
    if (states) {
        const std::string property = "Состояние";
        if (d::variant_value(v->variant, property).empty()) d::set_variant_value(v->variant, property, "Обычная");
        const char* const looks[] = {"Наведение", "Нажата", "Выключена"};
        f32 y = v->y;
        for (const char* look : looks) {
            std::vector<std::pair<std::string, std::string>> values = v->variant;
            d::set_variant_value(values, property, look);
            if (exists(values)) continue;
            d::Node copy = *v;
            d::renumber(screen_, copy);
            copy.variant = values;
            y += v->h + 40;
            copy.y = y;
            if (std::string(look) == "Выключена") copy.opacity = 0.5f; // a start: dimmed
            made.push_back(std::move(copy));
        }
    } else {
        if (v->variant.empty()) d::set_variant_value(v->variant, "Вид", "1");
        const std::string property = v->variant.back().first;
        std::vector<std::pair<std::string, std::string>> values = v->variant;
        for (int k = 2;; ++k) {
            d::set_variant_value(values, property, std::to_string(k));
            if (!exists(values)) break;
        }
        d::Node copy = *v;
        d::renumber(screen_, copy);
        copy.variant = values;
        f32 right = v->x;
        for (const d::Node& o : screen_.root.children)
            if (o.component == component && std::abs(o.y - v->y) < 1) right = std::max(right, o.x + o.w);
        copy.x = right + 40;
        made.push_back(std::move(copy));
    }
    std::vector<u32> fresh;
    for (d::Node& m : made) {
        fresh.push_back(m.id);
        screen_.root.children.push_back(std::move(m));
    }
    if (!fresh.empty()) selection_ = {fresh.front()};
    commit(before, states ? "Добавлены состояния" : "Добавлен вариант");
    return true;
}

bool UiEditor::reset_instance() {
    if (screen_.library || selection_.size() != 1) return false;
    d::Node* n = d::find(screen_.root, selection_[0]);
    if (!n || n->component.empty() || !n->master) return false;
    const std::string before = d::save_screen(screen_);
    auto forget = [](auto&& self, d::Node& node) -> void {
        node.overrides.clear();
        for (d::Node& c : node.children) self(self, c);
    };
    forget(forget, *n);
    d::sync_instance(screen_, *n, library_);
    commit(before, "Сброшены изменения копии");
    return true;
}

bool UiEditor::detach_instance() {
    if (screen_.library || selection_.size() != 1) return false;
    d::Node* n = d::find(screen_.root, selection_[0]);
    if (!n || n->component.empty() || !n->master) return false;
    const std::string before = d::save_screen(screen_);
    d::detach(*n);
    commit(before, "Копия отвязана от компонента");
    return true;
}

bool UiEditor::edit_component() {
    if (screen_.library || selection_.size() != 1) return false;
    const d::Node* inst = d::instance_of(screen_.root, selection_[0]);
    if (!inst) return false;
    const u32 master = inst->master;
    if (!open_library()) return false;
    if (d::find(screen_.root, master)) select({master});
    return true;
}

void UiEditor::update(Rml::Context* context) {
    read_canvas(context);
    if (fit_pending_ && canvas_w_ > 0) zoom_to_fit();
    if (checking_) {
        // Clicks of the last events, now that the page is done with them.
        std::vector<game::ScreenAction> actions;
        actions.swap(check_pending_);
        for (const game::ScreenAction& a : actions) check_action(a);
        if (check_ && page_from_check_) {
            const Rml::Vector2i size = page_context_->GetDimensions();
            check_->update(check_vars_, screen_.show != d::ScreenShow::Menu, screen_.show == d::ScreenShow::Menu, size.x, size.y);
            if (check_seen_ != check_vars_.version()) refresh_check();
        }
    }
    if (page_dirty_) {
        rebuild_page();
        refresh_overlay();
        refresh_props();
    }
}

bool UiEditor::handle_event(const SDL_Event& e, f32 density, Rml::Context* context) {
    if (checking_ && drag_ == Drag::None) {
        // The canvas is the game's screen: the mouse goes to the page.
        auto on_canvas = [&](f32 mx, f32 my) {
            const f32 cx = mx - canvas_x_, cy = my - canvas_y_;
            return cx >= 0 && cy >= 0 && cx < canvas_w_ && cy < canvas_h_;
        };
        auto pass = [&](f32 mx, f32 my, int down, int up) {
            mouse_x_ = mx;
            mouse_y_ = my;
            // Page pixels on the canvas, the player's pixels on the page's context.
            const game::ScreenFit fit = view_fit();
            check_mouse(game::fit_to_view_x(fit, to_screen_x(mx - canvas_x_)), game::fit_to_view_y(fit, to_screen_y(my - canvas_y_)),
                        down, up);
        };
        if (e.type == SDL_EVENT_MOUSE_MOTION) {
            pass(e.motion.x * density, e.motion.y * density, -1, -1);
            return false;
        }
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
            const f32 mx = e.button.x * density, my = e.button.y * density;
            Rml::Element* over = context ? context->GetHoverElement() : nullptr;
            bool on_stage = false;
            for (Rml::Element* el = over; el; el = el->GetParentNode()) {
                if (el->GetId() == "ue-canvas") on_stage = true;
                if (on_stage || el->IsClassSet("ue-panel")) break;
            }
            if (!on_stage || !on_canvas(mx, my)) return false;
            pass(mx, my, 0, -1);
            return true;
        }
        if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
            pass(e.button.x * density, e.button.y * density, -1, 0);
            return false;
        }
        // The wheel over the player's screen scrolls the page as in the game
        // (Ctrl+wheel still zooms the canvas; off the screen it pans).
        if (e.type == SDL_EVENT_MOUSE_WHEEL && !(SDL_GetModState() & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) && on_canvas(mouse_x_, mouse_y_) &&
            ui_ && page_context_) {
            const game::ScreenFit fit = view_fit();
            const f32 vx = game::fit_to_view_x(fit, to_screen_x(mouse_x_ - canvas_x_));
            const f32 vy = game::fit_to_view_y(fit, to_screen_y(mouse_y_ - canvas_y_));
            if (vx >= 0 && vy >= 0 && vx < view_w() && vy < view_h()) {
                pass(mouse_x_, mouse_y_, -1, -1);
                ui_->handle_event(page_context_, e); // the game's own way: ProcessMouseWheel
                return true;
            }
        }
    }
    switch (e.type) {
    case SDL_EVENT_KEY_UP:
        if (e.key.key == SDLK_SPACE) space_down_ = false;
        return false;
    case SDL_EVENT_MOUSE_MOTION: {
        mouse_x_ = e.motion.x * density;
        mouse_y_ = e.motion.y * density;
        if (drag_ != Drag::None) {
            drag_to(mouse_x_, mouse_y_);
            return true;
        }
        const f32 cx = mouse_x_ - canvas_x_, cy = mouse_y_ - canvas_y_;
        u32 over = 0;
        if (cx >= 0 && cy >= 0 && cx < canvas_w_ && cy < canvas_h_ && tool_ == Tool::Select)
            over = hit(to_screen_x(cx), to_screen_y(cy), SDL_GetModState() & SDL_KMOD_CTRL);
        if (over != hover_) {
            hover_ = over;
            refresh_overlay();
        }
        return false;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        mouse_x_ = e.button.x * density;
        mouse_y_ = e.button.y * density;
        // Only presses on the canvas or the rulers (not on a menu over them).
        Rml::Element* over = context ? context->GetHoverElement() : nullptr;
        bool on_stage = false;
        for (Rml::Element* el = over; el; el = el->GetParentNode()) {
            const Rml::String& id = el->GetId();
            if (id == "ue-canvas" || id == "ue-ruler-x" || id == "ue-ruler-y") {
                on_stage = true;
                break;
            }
            if (el->IsClassSet("ue-panel")) break;
        }
        if (!on_stage) return false;
        return press(mouse_x_, mouse_y_, e.button.button, e.button.clicks, context);
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (drag_ == Drag::None) return false;
        mouse_x_ = e.button.x * density;
        mouse_y_ = e.button.y * density;
        release();
        return true;
    case SDL_EVENT_MOUSE_WHEEL: {
        const f32 cx = mouse_x_ - canvas_x_, cy = mouse_y_ - canvas_y_;
        if (cx < 0 || cy < 0 || cx >= canvas_w_ || cy >= canvas_h_) return false;
        const SDL_Keymod mods = SDL_GetModState();
        if (mods & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) {
            set_zoom(zoom_ * std::pow(1.15f, e.wheel.y), cx, cy);
        } else if (mods & SDL_KMOD_SHIFT) {
            pan_x_ += e.wheel.y * 60;
            refresh_view();
        } else {
            pan_x_ -= e.wheel.x * 60;
            pan_y_ += e.wheel.y * 60;
            refresh_view();
        }
        return true;
    }
    default: return false;
    }
}

bool UiEditor::press(f32 mx, f32 my, u8 button, u8 clicks, Rml::Context* /*context*/) {
    const f32 cx = mx - canvas_x_, cy = my - canvas_y_;
    const f32 sx = to_screen_x(cx), sy = to_screen_y(cy);
    grab_mx_ = mx;
    grab_my_ = my;
    grab_pan_x_ = pan_x_;
    grab_pan_y_ = pan_y_;
    dragged_ = false;
    snap_lines_.clear();
    snap_gaps_.clear();
    if (button == SDL_BUTTON_MIDDLE || (button == SDL_BUTTON_LEFT && space_down_)) {
        drag_ = Drag::Pan;
        return true;
    }
    if (button != SDL_BUTTON_LEFT) return true;
    grab_json_ = d::save_screen(screen_);

    // Out of a ruler: a new guide (the top ruler gives a horizontal one).
    if (cy < 0 || cx < 0) {
        guide_vertical_ = cx < 0 && cy >= 0;
        screen_.guides.push_back({guide_vertical_, guide_vertical_ ? std::round(sx) : std::round(sy)});
        guide_ = static_cast<int>(screen_.guides.size()) - 1;
        drag_ = Drag::Guide;
        refresh_view();
        return true;
    }
    // A guide line.
    for (usize i = 0; i < screen_.guides.size(); ++i) {
        const d::Guide& g = screen_.guides[i];
        const f32 at = g.vertical ? to_canvas_x(g.position) : to_canvas_y(g.position);
        if (std::fabs((g.vertical ? cx : cy) - at) <= 3 && tool_ == Tool::Select) {
            guide_ = static_cast<int>(i);
            guide_vertical_ = g.vertical;
            drag_ = Drag::Guide;
            return true;
        }
    }

    // On a player's screen other than «Макет» layers are looked at, not moved:
    // where a drag would land depends on the screen.
    if (previewing() && tool_ != Tool::Select) return true;
    // Drawing a new layer.
    if (tool_ != Tool::Select) {
        static const d::NodeType types[] = {d::NodeType::Frame, d::NodeType::Frame, d::NodeType::Rectangle,
                                            d::NodeType::Ellipse, d::NodeType::Text};
        const d::NodeType type = types[static_cast<int>(tool_)];
        const u32 parent = container_at(sx, sy);
        drawn_ = add_layer(type, std::round(sx), std::round(sy), 1, 1, parent);
        grab_box_ = {std::round(sx), std::round(sy), 0, 0};
        drag_ = Drag::Draw;
        refresh_layers();
        refresh_props();
        return true;
    }

    // A handle of the selection.
    if (m_handles_ && !previewing()) {
        const Box& b = m_sel_box_;
        const f32 hx[8] = {b.x, b.x + b.w * 0.5f, b.x + b.w, b.x + b.w, b.x + b.w, b.x + b.w * 0.5f, b.x, b.x};
        const f32 hy[8] = {b.y, b.y, b.y, b.y + b.h * 0.5f, b.y + b.h, b.y + b.h, b.y + b.h, b.y + b.h * 0.5f};
        for (int i = 0; i < 8; ++i) {
            // Edge middles only when the box is big enough to tell them apart.
            if ((i % 2 == 1) && (b.w < 24 || b.h < 24) && (i == 1 || i == 5 ? b.w < 24 : b.h < 24)) continue;
            if (std::fabs(cx - hx[i]) <= kHandle && std::fabs(cy - hy[i]) <= kHandle) {
                handle_ = i;
                drag_ = Drag::Resize;
                grab_box_ = selection_box();
                grabbed_.clear();
                for (u32 id : selection_)
                    if (const d::Node* n = d::find(screen_.root, id)) {
                        const d::Rect box = layer_box(id).value_or(d::Rect{});
                        grabbed_.push_back({id, box.x, box.y, box.w, box.h});
                        (void)n;
                    }
                return true;
            }
        }
    }

    // A layer: pick it (double click: go into a frame), then move.
    const bool deep = SDL_GetModState() & (SDL_KMOD_CTRL | SDL_KMOD_GUI);
    const bool shift = SDL_GetModState() & SDL_KMOD_SHIFT;
    u32 id = hit(sx, sy, deep);
    if (clicks >= 2 && id && std::find(selection_.begin(), selection_.end(), id) != selection_.end()) {
        // One level deeper than the selected frame.
        const std::vector<u32> path = d::path_to(screen_.root, hit(sx, sy, true));
        for (usize i = 0; i + 1 < path.size(); ++i)
            if (path[i] == id) {
                id = path[i + 1];
                break;
            }
    }
    if (id) {
        if (shift) {
            auto it = std::find(selection_.begin(), selection_.end(), id);
            std::vector<u32> sel = selection_;
            if (it != selection_.end()) sel.erase(sel.begin() + (it - selection_.begin()));
            else sel.push_back(id);
            select(sel);
        } else if (std::find(selection_.begin(), selection_.end(), id) == selection_.end()) {
            select({id});
        }
        if (previewing()) return true;
        drag_ = Drag::Move;
        grab_box_ = selection_box();
        grabbed_.clear();
        for (u32 sel : selection_)
            if (const d::Node* n = d::find(screen_.root, sel)) grabbed_.push_back({sel, n->x, n->y, n->w, n->h});
        return true;
    }
    // Empty canvas: a selection rectangle.
    if (!shift) select({});
    drag_ = Drag::Marquee;
    marquee_ = {sx, sy, 0, 0};
    return true;
}

void UiEditor::drag_to(f32 mx, f32 my) {
    const f32 dxw = mx - grab_mx_, dyw = my - grab_my_;
    if (!dragged_ && std::hypot(dxw, dyw) < 3) return;
    dragged_ = true;
    const SDL_Keymod mods = SDL_GetModState();
    const bool no_snap = mods & (SDL_KMOD_CTRL | SDL_KMOD_GUI);
    const bool shift = mods & SDL_KMOD_SHIFT;
    const bool alt = mods & SDL_KMOD_ALT;
    const f32 threshold = kSnap / zoom_;
    snap_lines_.clear();
    snap_gaps_.clear();
    const f32 sx = to_screen_x(mx - canvas_x_), sy = to_screen_y(my - canvas_y_);

    switch (drag_) {
    case Drag::Pan:
        pan_x_ = grab_pan_x_ + dxw;
        pan_y_ = grab_pan_y_ + dyw;
        refresh_view();
        return;
    case Drag::Guide: {
        if (guide_ < 0 || guide_ >= static_cast<int>(screen_.guides.size())) return;
        screen_.guides[static_cast<usize>(guide_)].position = std::round(guide_vertical_ ? sx : sy);
        refresh_view();
        return;
    }
    case Drag::Marquee: {
        const f32 x0 = std::min(marquee_.x, sx), y0 = std::min(marquee_.y, sy);
        const f32 x1 = std::max(to_screen_x(grab_mx_ - canvas_x_), sx), y1 = std::max(to_screen_y(grab_my_ - canvas_y_), sy);
        const f32 gx = to_screen_x(grab_mx_ - canvas_x_), gy = to_screen_y(grab_my_ - canvas_y_);
        marquee_ = {std::min(gx, sx), std::min(gy, sy), std::fabs(sx - gx), std::fabs(sy - gy)};
        (void)x0;
        (void)y0;
        (void)x1;
        (void)y1;
        // The screen's own layers the rectangle touches.
        std::vector<u32> sel;
        for (const d::Node& c : screen_.root.children) {
            if (!c.visible || c.locked) continue;
            if (auto b = layer_box(c.id); b && intersects(*b, marquee_)) sel.push_back(c.id);
        }
        if (sel != selection_) select(sel);
        refresh_overlay();
        return;
    }
    case Drag::Move: {
        f32 dx = dxw / zoom_, dy = dyw / zoom_;
        if (shift) (std::fabs(dx) > std::fabs(dy) ? dy : dx) = 0;
        d::Rect moved = grab_box_;
        moved.x = std::round(moved.x + dx);
        moved.y = std::round(moved.y + dy);
        if (!no_snap) {
            const d::SnapResult r = d::snap_box(moved, snap_targets(), threshold);
            moved.x += r.dx;
            moved.y += r.dy;
            snap_lines_ = r.lines;
            snap_gaps_ = r.gaps;
        }
        dx = moved.x - grab_box_.x;
        dy = moved.y - grab_box_.y;
        for (const Grabbed& g : grabbed_) {
            d::Node* n = d::find(screen_.root, g.id);
            d::Node* parent = d::parent_of(screen_.root, g.id);
            if (!n || !parent || n->locked) continue;
            if (parent->layout.mode != d::LayoutMode::None && !n->absolute) {
                // In an auto layout a drag changes the order.
                const bool row = parent->layout.mode != d::LayoutMode::Column;
                const f32 at = row ? sx : sy;
                usize index = 0;
                for (const d::Node& c : parent->children) {
                    if (c.id == n->id) continue;
                    const auto b = layer_box(c.id);
                    if (b && at > (row ? b->cx() : b->cy())) ++index;
                }
                auto it = std::find_if(parent->children.begin(), parent->children.end(),
                                       [&](const d::Node& c) { return c.id == g.id; });
                if (it == parent->children.end()) continue;
                const usize from = static_cast<usize>(it - parent->children.begin());
                if (from != index) {
                    d::Node moving = std::move(*it);
                    parent->children.erase(it);
                    parent->children.insert(parent->children.begin() + static_cast<std::ptrdiff_t>(index), std::move(moving));
                    page_dirty_ = true;
                }
                snap_lines_.clear();
                snap_gaps_.clear();
                continue;
            }
            n->x = std::round(g.x + dx);
            n->y = std::round(g.y + dy);
            page_dirty_ = true;
        }
        break;
    }
    case Drag::Resize: {
        // Which edges the handle moves: 0 tl, 1 t, 2 tr, 3 r, 4 br, 5 b, 6 bl, 7 l.
        const bool left = handle_ == 0 || handle_ == 6 || handle_ == 7;
        const bool right = handle_ == 2 || handle_ == 3 || handle_ == 4;
        const bool top = handle_ == 0 || handle_ == 1 || handle_ == 2;
        const bool bottom = handle_ == 4 || handle_ == 5 || handle_ == 6;
        const f32 dx = dxw / zoom_, dy = dyw / zoom_;
        d::Rect b = grab_box_;
        if (left) {
            b.x += dx;
            b.w -= dx;
        }
        if (right) b.w += dx;
        if (top) {
            b.y += dy;
            b.h -= dy;
        }
        if (bottom) b.h += dy;
        b.x = std::round(b.x);
        b.y = std::round(b.y);
        b.w = std::round(b.w);
        b.h = std::round(b.h);
        if (!no_snap && !shift && !alt) {
            const d::SnapResult r = d::snap_edges(b, left, right, top, bottom, snap_targets(), threshold);
            if (left) {
                b.x += r.dx;
                b.w -= r.dx;
            } else if (right) {
                b.w += r.dx;
            }
            if (top) {
                b.y += r.dy;
                b.h -= r.dy;
            } else if (bottom) {
                b.h += r.dy;
            }
            snap_lines_ = r.lines;
        }
        if (shift && grab_box_.w > 0 && grab_box_.h > 0) {
            // Keep the proportions.
            const f32 ratio = grab_box_.w / grab_box_.h;
            if ((left || right) && (top || bottom)) {
                if (std::fabs(b.w / grab_box_.w) > std::fabs(b.h / grab_box_.h)) b.h = std::round(b.w / ratio);
                else b.w = std::round(b.h * ratio);
                if (left) b.x = grab_box_.right() - b.w;
                if (top) b.y = grab_box_.bottom() - b.h;
            } else if (left || right) {
                b.h = std::round(b.w / ratio);
                b.y = std::round(grab_box_.cy() - b.h * 0.5f);
            } else {
                b.w = std::round(b.h * ratio);
                b.x = std::round(grab_box_.cx() - b.w * 0.5f);
            }
        }
        if (alt) {
            // From the middle.
            const f32 gw = b.w - grab_box_.w, gh = b.h - grab_box_.h;
            if (left || right) {
                b.w = grab_box_.w + 2 * gw;
                b.x = grab_box_.x - gw;
            }
            if (top || bottom) {
                b.h = grab_box_.h + 2 * gh;
                b.y = grab_box_.y - gh;
            }
        }
        // Dragged past the other side: flip, as Figma does.
        if (b.w < 0) {
            b.x += b.w;
            b.w = -b.w;
        }
        if (b.h < 0) {
            b.y += b.h;
            b.h = -b.h;
        }
        b.w = std::max(b.w, 1.0f);
        b.h = std::max(b.h, 1.0f);
        // Every selected layer scales inside the selection's box.
        const f32 kx = grab_box_.w > 0 ? b.w / grab_box_.w : 1, ky = grab_box_.h > 0 ? b.h / grab_box_.h : 1;
        for (const Grabbed& g : grabbed_) {
            d::Node* n = d::find(screen_.root, g.id);
            if (!n || g.id == screen_.root.id) continue;
            const f32 nx = b.x + (g.x - grab_box_.x) * kx, ny = b.y + (g.y - grab_box_.y) * ky;
            const f32 nw = std::max(std::round(g.w * kx), 1.0f), nh = std::max(std::round(g.h * ky), 1.0f);
            d::Node* parent = d::parent_of(screen_.root, g.id);
            const d::Rect pbox = parent ? layer_box(parent->id).value_or(d::Rect{}) : d::Rect{};
            const bool flow = parent && parent->layout.mode != d::LayoutMode::None && !n->absolute;
            if (!flow) {
                n->x = std::round(nx - pbox.x);
                n->y = std::round(ny - pbox.y);
            }
            if (left || right) {
                n->w = nw;
                n->width_sizing = d::Sizing::Fixed;
            }
            if (top || bottom) {
                n->h = nh;
                n->height_sizing = d::Sizing::Fixed;
            }
        }
        page_dirty_ = true;
        break;
    }
    case Drag::Draw: {
        d::Node* n = d::find(screen_.root, drawn_);
        if (!n) return;
        f32 x0 = grab_box_.x, y0 = grab_box_.y, x1 = std::round(sx), y1 = std::round(sy);
        if (shift) {
            const f32 side = std::max(std::fabs(x1 - x0), std::fabs(y1 - y0));
            x1 = x0 + (x1 < x0 ? -side : side);
            y1 = y0 + (y1 < y0 ? -side : side);
        }
        if (alt) {
            x0 = 2 * grab_box_.x - x1;
            y0 = 2 * grab_box_.y - y1;
        }
        d::Rect b{std::min(x0, x1), std::min(y0, y1), std::max(std::fabs(x1 - x0), 1.0f), std::max(std::fabs(y1 - y0), 1.0f)};
        if (!no_snap && !shift && !alt) {
            const d::SnapResult r = d::snap_edges(b, x1 < x0, x1 >= x0, y1 < y0, y1 >= y0, snap_targets(), threshold);
            if (x1 < x0) {
                b.x += r.dx;
                b.w -= r.dx;
            } else {
                b.w += r.dx;
            }
            if (y1 < y0) {
                b.y += r.dy;
                b.h -= r.dy;
            } else {
                b.h += r.dy;
            }
            snap_lines_ = r.lines;
        }
        d::Node* parent = d::parent_of(screen_.root, drawn_);
        const d::Rect pbox = parent ? layer_box(parent->id).value_or(d::Rect{}) : d::Rect{};
        n->x = std::round(b.x - pbox.x);
        n->y = std::round(b.y - pbox.y);
        n->w = std::max(std::round(b.w), 1.0f);
        n->h = std::max(std::round(b.h), 1.0f);
        if (n->type == d::NodeType::Text) {
            n->width_sizing = d::Sizing::Fixed;
            n->height_sizing = d::Sizing::Fixed;
        }
        page_dirty_ = true;
        break;
    }
    case Drag::None: return;
    }
    refresh_overlay();
}

void UiEditor::release() {
    const Drag was = drag_;
    drag_ = Drag::None;
    const bool moved = dragged_;
    dragged_ = false;
    snap_lines_.clear();
    snap_gaps_.clear();
    switch (was) {
    case Drag::Pan: break;
    case Drag::Guide: {
        // Dropped back on a ruler (or never moved out of it): gone.
        const f32 cx = mouse_x_ - canvas_x_, cy = mouse_y_ - canvas_y_;
        if (guide_ >= 0 && guide_ < static_cast<int>(screen_.guides.size()) && (guide_vertical_ ? cx < 0 : cy < 0))
            screen_.guides.erase(screen_.guides.begin() + guide_);
        guide_ = -1;
        commit(grab_json_, "Направляющая");
        history_.seal();
        break;
    }
    case Drag::Marquee: break;
    case Drag::Move:
        if (moved) {
            remember_geometry(grab_json_);
            commit(grab_json_, selection_.size() > 1 ? "Сдвинуты слои" : "Сдвинуто");
            history_.seal();
        } else if (!(SDL_GetModState() & SDL_KMOD_SHIFT) && selection_.size() > 1) {
            // A click on one of several selected layers selects only it.
            const u32 id = hit(to_screen_x(grab_mx_ - canvas_x_), to_screen_y(grab_my_ - canvas_y_),
                               SDL_GetModState() & SDL_KMOD_CTRL);
            if (id) select({id});
        }
        break;
    case Drag::Resize:
        if (moved) {
            remember_geometry(grab_json_);
            commit(grab_json_, "Размер");
            history_.seal();
        }
        break;
    case Drag::Draw: {
        if (!moved) {
            // A click: a default size, centred where clicked (text: its own size).
            if (d::Node* n = d::find(screen_.root, drawn_)) {
                if (n->type == d::NodeType::Text) {
                    n->width_sizing = d::Sizing::Hug;
                    n->height_sizing = d::Sizing::Hug;
                } else {
                    n->x -= 50;
                    n->y -= 50;
                    n->w = 100;
                    n->h = 100;
                }
            }
        }
        const d::Node* n = d::find(screen_.root, drawn_);
        commit(grab_json_, std::string("Новый слой: ") + (n ? n->name : ""));
        history_.seal();
        set_tool(Tool::Select);
        break;
    }
    case Drag::None: break;
    }
    refresh_overlay();
}

bool UiEditor::handle_key(const SDL_KeyboardEvent& k) {
    if (checking_) {
        // Keys do not edit while checking; Esc ends it.
        if (k.key == SDLK_ESCAPE) set_checking(false);
        return k.key == SDLK_ESCAPE || k.key == SDLK_DELETE || k.key == SDLK_BACKSPACE;
    }
    const bool ctrl = k.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI);
    const bool shift = k.mod & SDL_KMOD_SHIFT;
    const bool alt = k.mod & SDL_KMOD_ALT;
    if (k.key == SDLK_SPACE) {
        space_down_ = true;
        return true;
    }
    if (!ctrl && !alt) {
        switch (k.key) {
        case SDLK_V: set_tool(Tool::Select); return true;
        case SDLK_F: set_tool(Tool::Frame); return true;
        case SDLK_R: set_tool(Tool::Rectangle); return true;
        case SDLK_O: set_tool(Tool::Ellipse); return true;
        case SDLK_T: set_tool(Tool::Text); return true;
        case SDLK_A:
            if (shift) return add_auto_layout();
            break;
        case SDLK_0:
            if (shift) {
                set_zoom(1, canvas_w_ * 0.5f, canvas_h_ * 0.5f);
                return true;
            }
            break;
        case SDLK_1:
            if (shift) {
                zoom_to_fit();
                return true;
            }
            break;
        case SDLK_EQUALS:
        case SDLK_KP_PLUS: set_zoom(zoom_ * 1.25f, canvas_w_ * 0.5f, canvas_h_ * 0.5f); return true;
        case SDLK_MINUS:
        case SDLK_KP_MINUS: set_zoom(zoom_ / 1.25f, canvas_w_ * 0.5f, canvas_h_ * 0.5f); return true;
        case SDLK_DELETE:
        case SDLK_BACKSPACE: return remove_selection();
        case SDLK_ESCAPE: {
            if (tool_ != Tool::Select) {
                set_tool(Tool::Select);
                return true;
            }
            if (selection_.empty()) return false;
            const d::Node* parent = d::parent_of(screen_.root, selection_[0]);
            select(parent && parent->id != screen_.root.id ? std::vector<u32>{parent->id} : std::vector<u32>{});
            return true;
        }
        case SDLK_RETURN: {
            if (selection_.size() != 1) return false;
            const d::Node* n = d::find(screen_.root, selection_[0]);
            if (!n || n->children.empty()) return false;
            std::vector<u32> kids;
            for (const d::Node& c : n->children) kids.push_back(c.id);
            select(kids);
            return true;
        }
        case SDLK_LEFT: return move_selection(shift ? -10.f : -1.f, 0);
        case SDLK_RIGHT: return move_selection(shift ? 10.f : 1.f, 0);
        case SDLK_UP: return move_selection(0, shift ? -10.f : -1.f);
        case SDLK_DOWN: return move_selection(0, shift ? 10.f : 1.f);
        default: break;
        }
        return false;
    }
    if (ctrl) {
        switch (k.key) {
        case SDLK_D: return duplicate_selection();
        case SDLK_G: return wrap_selection_in_frame();
        case SDLK_A: {
            // Everything beside the selection (or on the screen).
            const d::Node* parent = selection_.empty() ? &screen_.root : d::parent_of(screen_.root, selection_[0]);
            if (!parent) parent = &screen_.root;
            std::vector<u32> all;
            for (const d::Node& c : parent->children)
                if (c.visible && !c.locked) all.push_back(c.id);
            select(all);
            return true;
        }
        case SDLK_C:
        case SDLK_X: {
            clipboard_.clear();
            for (u32 id : selection_)
                if (id != screen_.root.id)
                    if (const d::Node* n = d::find(screen_.root, id)) clipboard_.push_back(*n);
            if (k.key == SDLK_X) remove_selection();
            return !clipboard_.empty();
        }
        case SDLK_V: {
            if (clipboard_.empty()) return false;
            const std::string before = d::save_screen(screen_);
            // Into the selected frame, else beside the selection, else on the screen.
            d::Node* target = &screen_.root;
            if (!selection_.empty()) {
                d::Node* sel = d::find(screen_.root, selection_[0]);
                if (sel && sel->is_container() && sel->id != screen_.root.id && !shift) target = sel;
                else if (d::Node* p = d::parent_of(screen_.root, selection_[0])) target = p;
            }
            // Not into a copy of a component: beside it.
            if (const d::Node* inst = screen_.library ? nullptr : d::instance_of(screen_.root, target->id))
                if (d::Node* p = d::parent_of(screen_.root, inst->id)) target = p;
            std::vector<u32> fresh;
            for (d::Node n : clipboard_) {
                d::renumber(screen_, n);
                n.x += 10;
                n.y += 10;
                fresh.push_back(n.id);
                target->children.push_back(std::move(n));
            }
            selection_ = fresh;
            commit(before, "Вставлено");
            return true;
        }
        case SDLK_RIGHTBRACKET:
        case SDLK_LEFTBRACKET: {
            // Forward / back among the siblings (Shift: to the top / bottom).
            if (selection_.empty()) return false;
            const std::string before = d::save_screen(screen_);
            const bool up = k.key == SDLK_RIGHTBRACKET;
            for (u32 id : selection_) {
                d::Node* parent = d::parent_of(screen_.root, id);
                if (!parent) continue;
                auto& kids = parent->children;
                auto it = std::find_if(kids.begin(), kids.end(), [&](const d::Node& c) { return c.id == id; });
                if (it == kids.end()) continue;
                const usize i = static_cast<usize>(it - kids.begin());
                const usize to = shift ? (up ? kids.size() - 1 : 0) : (up ? std::min(i + 1, kids.size() - 1) : (i ? i - 1 : 0));
                if (to == i) continue;
                d::Node n = std::move(kids[i]);
                kids.erase(kids.begin() + static_cast<std::ptrdiff_t>(i));
                kids.insert(kids.begin() + static_cast<std::ptrdiff_t>(to), std::move(n));
            }
            commit(before, up ? "Выше" : "Ниже");
            return true;
        }
        default: break;
        }
    }
    return false;
}

// --- the document -----------------------------------------------------------

void UiEditor::bind(Rml::DataModelConstructor& model) {
    if (auto s = model.RegisterStruct<ScreenRow>()) {
        s.RegisterMember("name", &ScreenRow::name);
        s.RegisterMember("title", &ScreenRow::title);
        s.RegisterMember("size", &ScreenRow::size);
        s.RegisterMember("selected", &ScreenRow::selected);
    }
    model.RegisterArray<std::vector<ScreenRow>>();
    if (auto s = model.RegisterStruct<LayerRow>()) {
        s.RegisterMember("id", &LayerRow::id);
        s.RegisterMember("name", &LayerRow::name);
        s.RegisterMember("icon", &LayerRow::icon);
        s.RegisterMember("depth", &LayerRow::depth);
        s.RegisterMember("selected", &LayerRow::selected);
        s.RegisterMember("visible", &LayerRow::visible);
        s.RegisterMember("locked", &LayerRow::locked);
        s.RegisterMember("container", &LayerRow::container);
        s.RegisterMember("open", &LayerRow::open);
        s.RegisterMember("hidden_by_parent", &LayerRow::hidden_by_parent);
        s.RegisterMember("component", &LayerRow::component);
    }
    model.RegisterArray<std::vector<LayerRow>>();
    model.RegisterArray<std::vector<Rml::String>>();
    if (auto s = model.RegisterStruct<ComponentRow>()) {
        s.RegisterMember("name", &ComponentRow::name);
        s.RegisterMember("icon", &ComponentRow::icon);
        s.RegisterMember("variants", &ComponentRow::variants);
    }
    model.RegisterArray<std::vector<ComponentRow>>();
    if (auto s = model.RegisterStruct<VariantRow>()) {
        s.RegisterMember("property", &VariantRow::property);
        s.RegisterMember("value", &VariantRow::value);
        s.RegisterMember("values", &VariantRow::values);
    }
    model.RegisterArray<std::vector<VariantRow>>();
    if (auto s = model.RegisterStruct<Tick>()) {
        s.RegisterMember("at", &Tick::at);
        s.RegisterMember("label", &Tick::label);
    }
    model.RegisterArray<std::vector<Tick>>();
    if (auto s = model.RegisterStruct<Box>()) {
        s.RegisterMember("x", &Box::x);
        s.RegisterMember("y", &Box::y);
        s.RegisterMember("w", &Box::w);
        s.RegisterMember("h", &Box::h);
    }
    model.RegisterArray<std::vector<Box>>();
    if (auto s = model.RegisterStruct<Label>()) {
        s.RegisterMember("x", &Label::x);
        s.RegisterMember("y", &Label::y);
        s.RegisterMember("text", &Label::text);
    }
    model.RegisterArray<std::vector<Label>>();
    if (auto s = model.RegisterStruct<FillRow>()) {
        s.RegisterMember("index", &FillRow::index);
        s.RegisterMember("kind", &FillRow::kind);
        s.RegisterMember("kind_name", &FillRow::kind_name);
        s.RegisterMember("hex", &FillRow::hex);
        s.RegisterMember("hex2", &FillRow::hex2);
        s.RegisterMember("opacity", &FillRow::opacity);
        s.RegisterMember("swatch", &FillRow::swatch);
        s.RegisterMember("angle", &FillRow::angle);
        s.RegisterMember("image", &FillRow::image);
        s.RegisterMember("fit", &FillRow::fit);
        s.RegisterMember("tile", &FillRow::tile);
        s.RegisterMember("offset_x", &FillRow::offset_x);
        s.RegisterMember("offset_y", &FillRow::offset_y);
        s.RegisterMember("style", &FillRow::style);
        s.RegisterMember("visible", &FillRow::visible);
    }
    model.RegisterArray<std::vector<FillRow>>();
    if (auto s = model.RegisterStruct<EffectRow>()) {
        s.RegisterMember("index", &EffectRow::index);
        s.RegisterMember("kind", &EffectRow::kind);
        s.RegisterMember("kind_name", &EffectRow::kind_name);
        s.RegisterMember("hex", &EffectRow::hex);
        s.RegisterMember("swatch", &EffectRow::swatch);
        s.RegisterMember("x", &EffectRow::x);
        s.RegisterMember("y", &EffectRow::y);
        s.RegisterMember("blur", &EffectRow::blur);
        s.RegisterMember("spread", &EffectRow::spread);
        s.RegisterMember("visible", &EffectRow::visible);
        s.RegisterMember("shadow", &EffectRow::shadow);
    }
    model.RegisterArray<std::vector<EffectRow>>();
    if (auto s = model.RegisterStruct<Props>()) {
        s.RegisterMember("any", &Props::any);
        s.RegisterMember("many", &Props::many);
        s.RegisterMember("root", &Props::root);
        s.RegisterMember("text", &Props::text);
        s.RegisterMember("container", &Props::container);
        s.RegisterMember("in_layout", &Props::in_layout);
        s.RegisterMember("has_layout", &Props::has_layout);
        s.RegisterMember("has_stroke", &Props::has_stroke);
        s.RegisterMember("name", &Props::name);
        s.RegisterMember("type_name", &Props::type_name);
        s.RegisterMember("icon", &Props::icon);
        s.RegisterMember("x", &Props::x);
        s.RegisterMember("y", &Props::y);
        s.RegisterMember("w", &Props::w);
        s.RegisterMember("h", &Props::h);
        s.RegisterMember("rotation", &Props::rotation);
        s.RegisterMember("horizontal", &Props::horizontal);
        s.RegisterMember("vertical", &Props::vertical);
        s.RegisterMember("width_sizing", &Props::width_sizing);
        s.RegisterMember("height_sizing", &Props::height_sizing);
        s.RegisterMember("layout_mode", &Props::layout_mode);
        s.RegisterMember("gap", &Props::gap);
        s.RegisterMember("pad_t", &Props::pad_t);
        s.RegisterMember("pad_r", &Props::pad_r);
        s.RegisterMember("pad_b", &Props::pad_b);
        s.RegisterMember("pad_l", &Props::pad_l);
        s.RegisterMember("align", &Props::align);
        s.RegisterMember("space_between", &Props::space_between);
        s.RegisterMember("clip", &Props::clip);
        s.RegisterMember("absolute", &Props::absolute);
        s.RegisterMember("radius", &Props::radius);
        s.RegisterMember("radius_tl", &Props::radius_tl);
        s.RegisterMember("radius_tr", &Props::radius_tr);
        s.RegisterMember("radius_br", &Props::radius_br);
        s.RegisterMember("radius_bl", &Props::radius_bl);
        s.RegisterMember("radius_mixed", &Props::radius_mixed);
        s.RegisterMember("opacity", &Props::opacity);
        s.RegisterMember("blend", &Props::blend);
        s.RegisterMember("stroke_hex", &Props::stroke_hex);
        s.RegisterMember("stroke_swatch", &Props::stroke_swatch);
        s.RegisterMember("stroke_width", &Props::stroke_width);
        s.RegisterMember("stroke_align", &Props::stroke_align);
        s.RegisterMember("stroke_style", &Props::stroke_style);
        s.RegisterMember("content", &Props::content);
        s.RegisterMember("family", &Props::family);
        s.RegisterMember("size", &Props::size);
        s.RegisterMember("weight", &Props::weight);
        s.RegisterMember("line_height", &Props::line_height);
        s.RegisterMember("letter_spacing", &Props::letter_spacing);
        s.RegisterMember("text_align", &Props::text_align);
        s.RegisterMember("text_case", &Props::text_case);
        s.RegisterMember("decoration", &Props::decoration);
        s.RegisterMember("text_hex", &Props::text_hex);
        s.RegisterMember("text_swatch", &Props::text_swatch);
        s.RegisterMember("italic", &Props::italic);
        s.RegisterMember("screen_font", &Props::screen_font);
        s.RegisterMember("screen_size", &Props::screen_size);
        s.RegisterMember("screen_text_hex", &Props::screen_text_hex);
        s.RegisterMember("screen_text_swatch", &Props::screen_text_swatch);
        s.RegisterMember("screen_fit", &Props::screen_fit);
        s.RegisterMember("bars_hex", &Props::bars_hex);
        s.RegisterMember("bars_swatch", &Props::bars_swatch);
        s.RegisterMember("safe", &Props::safe);
        s.RegisterMember("has_frame", &Props::has_frame);
        s.RegisterMember("frame_visible", &Props::frame_visible);
        s.RegisterMember("frame_fill", &Props::frame_fill);
        s.RegisterMember("frame_image", &Props::frame_image);
        s.RegisterMember("frame_t", &Props::frame_t);
        s.RegisterMember("frame_r", &Props::frame_r);
        s.RegisterMember("frame_b", &Props::frame_b);
        s.RegisterMember("frame_l", &Props::frame_l);
        s.RegisterMember("frame_scale", &Props::frame_scale);
        s.RegisterMember("frame_repeat", &Props::frame_repeat);
        s.RegisterMember("has_mask", &Props::has_mask);
        s.RegisterMember("mask_visible", &Props::mask_visible);
        s.RegisterMember("mask_image", &Props::mask_image);
        s.RegisterMember("mask_fit", &Props::mask_fit);
        s.RegisterMember("can_make_component", &Props::can_make_component);
        s.RegisterMember("lib_variant", &Props::lib_variant);
        s.RegisterMember("instance", &Props::instance);
        s.RegisterMember("in_instance", &Props::in_instance);
        s.RegisterMember("changed_here", &Props::changed_here);
        s.RegisterMember("component", &Props::component);
        s.RegisterMember("text_style", &Props::text_style);
        s.RegisterMember("screen_show", &Props::screen_show);
        s.RegisterMember("covers_game", &Props::covers_game);
        s.RegisterMember("pauses", &Props::pauses);
        s.RegisterMember("esc_closes", &Props::esc_closes);
        s.RegisterMember("show_if", &Props::show_if);
        s.RegisterMember("list", &Props::list);
        s.RegisterMember("list_gap", &Props::list_gap);
        s.RegisterMember("list_no_cell", &Props::list_no_cell);
        s.RegisterMember("picture_from", &Props::picture_from);
        s.RegisterMember("in_list", &Props::in_list);
        s.RegisterMember("has_bar", &Props::has_bar);
        s.RegisterMember("bar_value", &Props::bar_value);
        s.RegisterMember("bar_max", &Props::bar_max);
        s.RegisterMember("bar_from", &Props::bar_from);
        s.RegisterMember("motion_kind", &Props::motion_kind);
        s.RegisterMember("motion_duration", &Props::motion_duration);
        s.RegisterMember("motion_delay", &Props::motion_delay);
        s.RegisterMember("motion_strength", &Props::motion_strength);
        s.RegisterMember("motion_easing", &Props::motion_easing);
        s.RegisterMember("motion_loop", &Props::motion_loop);
        s.RegisterMember("motion_back", &Props::motion_back);
        s.RegisterMember("smooth", &Props::smooth);
        s.RegisterMember("smooth_easing", &Props::smooth_easing);
        s.RegisterMember("appear", &Props::appear);
        s.RegisterMember("appear_time", &Props::appear_time);
    }
    if (auto s = model.RegisterStruct<KeyRow>()) {
        s.RegisterMember("index", &KeyRow::index);
        s.RegisterMember("at", &KeyRow::at);
        s.RegisterMember("x", &KeyRow::x);
        s.RegisterMember("y", &KeyRow::y);
        s.RegisterMember("scale", &KeyRow::scale);
        s.RegisterMember("rotation", &KeyRow::rotation);
        s.RegisterMember("opacity", &KeyRow::opacity);
        s.RegisterMember("radius", &KeyRow::radius);
        s.RegisterMember("blur", &KeyRow::blur);
        s.RegisterMember("brightness", &KeyRow::brightness);
        s.RegisterMember("tint", &KeyRow::tint);
        s.RegisterMember("hex", &KeyRow::hex);
        s.RegisterMember("swatch", &KeyRow::swatch);
    }
    model.RegisterArray<std::vector<KeyRow>>();
    model.Bind("ue_keys", &m_keys_);
    if (auto s = model.RegisterStruct<ClickRow>()) {
        s.RegisterMember("index", &ClickRow::index);
        s.RegisterMember("kind", &ClickRow::kind);
        s.RegisterMember("target", &ClickRow::target);
        s.RegisterMember("needs", &ClickRow::needs);
        s.RegisterMember("hint", &ClickRow::hint);
    }
    model.RegisterArray<std::vector<ClickRow>>();
    if (auto s = model.RegisterStruct<ValueRow>()) {
        s.RegisterMember("name", &ValueRow::name);
        s.RegisterMember("label", &ValueRow::label);
    }
    model.RegisterArray<std::vector<ValueRow>>();
    model.Bind("ue_clicks", &m_clicks_);
    model.Bind("ue_panel", &m_panel_);
    if (auto s = model.RegisterStruct<CheckVarRow>()) {
        s.RegisterMember("name", &CheckVarRow::name);
        s.RegisterMember("label", &CheckVarRow::label);
        s.RegisterMember("value", &CheckVarRow::value);
    }
    model.RegisterArray<std::vector<CheckVarRow>>();
    model.Bind("ue_check_vars", &m_check_vars_);
    model.Bind("ue_check_log", &m_check_log_);
    model.Bind("ue_checking", &m_checking_);
    model.Bind("ue_values", &m_values_);
    model.Bind("ue_screen_names", &m_screen_names_);
    if (auto s = model.RegisterStruct<PictureRow>()) {
        s.RegisterMember("path", &PictureRow::path);
        s.RegisterMember("name", &PictureRow::name);
    }
    model.RegisterArray<std::vector<PictureRow>>();
    model.RegisterArray<std::vector<int>>();

    model.Bind("ue_screens", &m_screens_);
    model.Bind("ue_layers", &m_layers_);
    model.Bind("ue_ticks_x", &m_ticks_x_);
    model.Bind("ue_ticks_y", &m_ticks_y_);
    model.Bind("ue_selected", &m_selected_);
    model.Bind("ue_sel_box", &m_sel_box_);
    model.Bind("ue_handles", &m_handles_);
    model.Bind("ue_hover", &m_hover_);
    model.Bind("ue_hovering", &m_hovering_);
    model.Bind("ue_marquee", &m_marquee_);
    model.Bind("ue_marqueeing", &m_marqueeing_);
    model.Bind("ue_measures", &m_measures_);
    model.Bind("ue_size_label", &m_size_label_);
    model.Bind("ue_frame_label", &m_frame_label_);
    model.Bind("ue_frame", &m_frame_);
    model.Bind("ue_guides_x", &m_guides_x_);
    model.Bind("ue_guides_y", &m_guides_y_);
    model.Bind("ue_tool", &m_tool_);
    model.Bind("ue_zoom", &m_zoom_text_);
    if (auto v = model.RegisterStruct<ViewRow>()) {
        v.RegisterMember("label", &ViewRow::label);
        v.RegisterMember("title", &ViewRow::title);
    }
    model.RegisterArray<std::vector<ViewRow>>();
    m_views_.clear();
    for (const ViewSize& v : view_sizes())
        m_views_.push_back({v.label, v.w > 0 ? "Экран игрока " + fmt(v.w) + "×" + fmt(v.h) : "Размер, в котором экран нарисован: слои двигаются здесь"});
    model.Bind("ue_views", &m_views_);
    model.Bind("ue_view", &m_view_);
    model.Bind("ue_view_note", &m_view_note_);
    model.Bind("ue_title", &m_title_);
    model.Bind("ue_renaming", &m_renaming_);
    model.Bind("ue_rename_text", &m_rename_text_);
    model.Bind("ue_p", &m_p_);
    model.Bind("ue_fills", &m_fills_);
    model.Bind("ue_effects", &m_effects_);
    model.Bind("ue_families", &m_families_);
    model.Bind("ue_nine", &m_nine_);
    model.Bind("ue_pictures", &m_pictures_);
    model.Bind("ue_safe", &m_safe_);
    model.Bind("ue_has_safe", &m_has_safe_);
    if (auto s = model.RegisterStruct<GameColorRow>()) {
        s.RegisterMember("index", &GameColorRow::index);
        s.RegisterMember("key", &GameColorRow::key);
        s.RegisterMember("name", &GameColorRow::name);
        s.RegisterMember("hex", &GameColorRow::hex);
        s.RegisterMember("swatch", &GameColorRow::swatch);
    }
    model.RegisterArray<std::vector<GameColorRow>>();
    if (auto s = model.RegisterStruct<GameTextRow>()) {
        s.RegisterMember("index", &GameTextRow::index);
        s.RegisterMember("key", &GameTextRow::key);
        s.RegisterMember("name", &GameTextRow::name);
        s.RegisterMember("family", &GameTextRow::family);
        s.RegisterMember("size", &GameTextRow::size);
        s.RegisterMember("weight", &GameTextRow::weight);
        s.RegisterMember("hex", &GameTextRow::hex);
        s.RegisterMember("swatch", &GameTextRow::swatch);
    }
    model.RegisterArray<std::vector<GameTextRow>>();
    model.Bind("ue_game_colors", &m_game_colors_);
    model.Bind("ue_game_texts", &m_game_texts_);
    model.Bind("ue_components", &m_components_);
    model.Bind("ue_variants", &m_variants_);
    model.Bind("ue_library_open", &m_library_open_);

    auto on = [&](const char* name, auto fn) {
        model.BindEventCallback(name, [this, fn](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& args) {
            fn(ev, args);
        });
    };
    auto arg_str = [](const Rml::VariantList& a, usize i) { return i < a.size() ? a[i].Get<Rml::String>() : Rml::String(); };
    auto arg_int = [](const Rml::VariantList& a, usize i) { return i < a.size() ? a[i].Get<int>() : 0; };
    auto input_value = [](Rml::Event& ev) -> std::string {
        if (auto* input = rmlui_dynamic_cast<Rml::ElementFormControl*>(ev.GetTargetElement())) return input->GetValue();
        return {};
    };

    on("ue_screen", [this, arg_str](Rml::Event&, const Rml::VariantList& a) { open(arg_str(a, 0)); });
    on("ue_new_screen", [this](Rml::Event&, const Rml::VariantList&) { new_screen(); });
    on("ue_library", [this](Rml::Event&, const Rml::VariantList&) { open_library(); });
    on("ue_place", [this, arg_str](Rml::Event&, const Rml::VariantList& a) { place_component(arg_str(a, 0)); });
    on("ue_component", [this, arg_str](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        const std::string what = arg_str(a, 0);
        if (what == "make") make_component();
        else if (what == "variant") add_variant(false);
        else if (what == "states") add_variant(true);
        else if (what == "reset") reset_instance();
        else if (what == "detach") detach_instance();
        else if (what == "edit") edit_component();
    });
    on("ue_tool", [this, arg_str](Rml::Event&, const Rml::VariantList& a) {
        const std::string t = arg_str(a, 0);
        set_tool(t == "frame" ? Tool::Frame : t == "rectangle" ? Tool::Rectangle : t == "ellipse" ? Tool::Ellipse
                 : t == "text" ? Tool::Text : Tool::Select);
    });
    on("ue_zoom", [this, arg_str](Rml::Event&, const Rml::VariantList& a) {
        const std::string what = arg_str(a, 0);
        if (what == "in") set_zoom(zoom_ * 1.25f, canvas_w_ * 0.5f, canvas_h_ * 0.5f);
        else if (what == "out") set_zoom(zoom_ / 1.25f, canvas_w_ * 0.5f, canvas_h_ * 0.5f);
        else if (what == "100") set_zoom(1, canvas_w_ * 0.5f, canvas_h_ * 0.5f);
        else zoom_to_fit();
    });
    on("ue_view", [this, arg_int](Rml::Event&, const Rml::VariantList& a) { set_view(arg_int(a, 0)); });
    on("ue_select_screen", [this](Rml::Event&, const Rml::VariantList&) { select({screen_.root.id}); });
    on("ue_layer", [this, arg_int](Rml::Event& ev, const Rml::VariantList& a) {
        const u32 id = static_cast<u32>(arg_int(a, 0));
        if (ev.GetParameter<int>("shift_key", 0)) {
            std::vector<u32> sel = selection_;
            auto it = std::find(sel.begin(), sel.end(), id);
            if (it != sel.end()) sel.erase(it);
            else sel.push_back(id);
            select(sel);
        } else {
            select({id});
        }
    });
    on("ue_layer_eye", [this, arg_int](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        if (d::Node* n = d::find(screen_.root, static_cast<u32>(arg_int(a, 0)))) {
            const std::string before = d::save_screen(screen_);
            n->visible = !n->visible;
            commit(before, n->visible ? "Показан слой" : "Скрыт слой");
        }
    });
    on("ue_layer_lock", [this, arg_int](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        if (d::Node* n = d::find(screen_.root, static_cast<u32>(arg_int(a, 0)))) {
            const std::string before = d::save_screen(screen_);
            n->locked = !n->locked;
            commit(before, n->locked ? "Слой закреплён" : "Слой откреплён");
        }
    });
    on("ue_layer_fold", [this, arg_int](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        const u32 id = static_cast<u32>(arg_int(a, 0));
        auto it = std::find(closed_.begin(), closed_.end(), id);
        if (it != closed_.end()) closed_.erase(it);
        else closed_.push_back(id);
        refresh_layers();
    });
    on("ue_rename", [this, arg_str](Rml::Event&, const Rml::VariantList& a) {
        m_renaming_ = arg_str(a, 0);
        if (m_renaming_.rfind("screen", 0) == 0) m_rename_text_ = screen_.title;
        else if (const d::Node* n = d::find(screen_.root, static_cast<u32>(std::atoi(m_renaming_.c_str())))) m_rename_text_ = n->name;
        dirty("ue_renaming");
        dirty("ue_rename_text");
    });
    auto finish_rename = [this](const std::string& text) {
        const std::string what = m_renaming_;
        m_renaming_.clear();
        dirty("ue_renaming");
        if (what.empty() || text.empty()) return;
        const std::string before = d::save_screen(screen_);
        if (what.rfind("screen", 0) == 0) {
            screen_.title = text;
            screen_.root.name = text;
        } else if (d::Node* n = d::find(screen_.root, static_cast<u32>(std::atoi(what.c_str())))) {
            n->name = text;
        }
        commit(before, "Переименовано");
    };
    on("ue_rename_text", [finish_rename, arg_str](Rml::Event&, const Rml::VariantList& a) {
        if (a.size() > 1 && a[1].Get<bool>()) finish_rename(arg_str(a, 0));
    });
    on("ue_rename_done", [finish_rename, input_value](Rml::Event& ev, const Rml::VariantList&) { finish_rename(input_value(ev)); });
    on("ue_check", [this](Rml::Event&, const Rml::VariantList&) { set_checking(!checking_); });
    on("ue_check_var", [this, arg_str](Rml::Event&, const Rml::VariantList& a) {
        const std::string name = arg_str(a, 0), text = arg_str(a, 1);
        char* end = nullptr;
        const double v = std::strtod(text.c_str(), &end);
        if (!text.empty() && end && *end == 0) check_vars_.set(name, v);
        else check_vars_.set(name, text);
    });
    on("ue_panel_tab", [this, arg_str](Rml::Event&, const Rml::VariantList& a) { set_panel(arg_str(a, 0)); });
    on("ue_set", [this, arg_str](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        set_property(arg_str(a, 0), arg_str(a, 1));
    });
    // Fields: Enter keeps the value at once, leaving the field keeps it too.
    on("ue_text", [this, arg_str](Rml::Event&, const Rml::VariantList& a) {
        if (a.size() > 2 && a[2].Get<bool>()) set_property(arg_str(a, 0), arg_str(a, 1));
    });
    on("ue_commit", [this, arg_str, input_value](Rml::Event& ev, const Rml::VariantList& a) {
        const std::string value = input_value(ev);
        // Unchanged fields do not make a step of the history.
        const std::string before = d::save_screen(screen_);
        if (!set_property(arg_str(a, 0), value)) return;
        (void)before;
    });
    on("ue_pick", [this, arg_str](Rml::Event& ev, const Rml::VariantList& a) {
        // A <select>: its value is the new one.
        if (auto* input = rmlui_dynamic_cast<Rml::ElementFormControl*>(ev.GetTargetElement()))
            set_property(arg_str(a, 0), input->GetValue());
    });
}

} // namespace forge::editor_app

namespace forge::editor_app {

// --- «Проверить» ---------------------------------------------------------------

void UiEditor::set_checking(bool on) {
    if (on == checking_) return;
    if (on && screen_.library) return; // the components are not a screen of the game
    checking_ = on;
    m_checking_ = on;
    check_back_.clear();
    check_log_.clear();
    check_pending_.clear();
    if (on) {
        select({});
        set_tool(Tool::Select);
        hover_ = 0;
    }
    page_dirty_ = true;
    dirty("ue_checking");
    refresh_check();
    refresh_overlay();
}

void UiEditor::check_mouse(f32 x, f32 y, int down, int up) {
    if (!page_context_ || !checking_) return;
    const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
    page_context_->ProcessMouseMove(ix, iy, 0);
    if (down >= 0) page_context_->ProcessMouseButtonDown(down, 0);
    if (up >= 0) page_context_->ProcessMouseButtonUp(up, 0);
}

// What a value is called in the panel: its name in the game's words when known.
void UiEditor::seed_check_vars() {
    if (!check_) return;
    for (const std::string& name : check_->variables(name_)) {
        if (check_vars_.has(name)) continue;
        // Something to look at: bars half full, a few coins, quests begun.
        const bool max = name.find("max") != std::string::npos;
        check_vars_.set(name, max ? 10.0 : name.rfind("quest.", 0) == 0 ? 1.0 : 5.0);
    }
}

void UiEditor::refresh_check() {
    m_check_vars_.clear();
    m_check_log_.clear();
    if (checking_ && check_) {
        std::vector<std::pair<std::string, std::string>> known;
        if (game_values) known = game_values();
        for (const std::string& name : check_->variables(name_)) {
            CheckVarRow row;
            row.name = name;
            row.label = name;
            for (const auto& [n, label] : known)
                if (n == name) row.label = label.substr(0, label.rfind(" · "));
            row.value = check_vars_.get(name).text();
            m_check_vars_.push_back(std::move(row));
        }
        for (auto it = check_log_.rbegin(); it != check_log_.rend() && m_check_log_.size() < 8; ++it) m_check_log_.push_back(*it);
    }
    check_seen_ = check_vars_.version();
    dirty("ue_check_vars");
    dirty("ue_check_log");
}

void UiEditor::check_action(const game::ScreenAction& a) {
    const std::string& t = a.target;
    std::string said;
    if (a.what == "show" || a.what == "toggle") {
        if (t == name_ && a.what == "toggle") {
            said = "Скрыт экран «" + screen_.title + "»";
            if (!check_back_.empty()) {
                const std::string back = check_back_.back();
                check_back_.pop_back();
                open(back);
            }
        } else if (std::find(m_screen_names_.begin(), m_screen_names_.end(), t) != m_screen_names_.end() || t == name_) {
            if (t != name_) {
                check_back_.push_back(name_);
                open(t);
            }
            said = "Открыт экран «" + screen_.title + "»";
        } else said = "Нет экрана «" + t + "»";
    } else if (a.what == "hide" || a.what == "close") {
        const bool self = a.what == "close" || t.empty() || t == name_;
        said = self ? "Экран закрыт" : "Скрыт экран «" + t + "»";
        if (self && !check_back_.empty()) {
            const std::string back = check_back_.back();
            check_back_.pop_back();
            open(back);
        }
    } else if (a.what == "change") {
        std::string error;
        const game::Expr e = game::Expr::parse_actions(t, &error);
        if (!error.empty()) said = "Не читается: " + t;
        else {
            e.run(check_vars_);
            said = "Данные: " + t;
        }
    } else if (a.what == "message") said = "Сообщение «Логике»: " + t;
    else if (a.what == "talk") said = "Разговор «" + t + "»";
    else {
        static const std::pair<const char*, const char*> words[] = {
            {"pause", "Пауза"}, {"resume", "Вернуться в игру"}, {"menu", "В главное меню"}, {"quit", "Выйти из игры"},
            {"new", "Новая игра"}, {"continue", "Продолжить"}, {"load", "Загрузить"}, {"save", "Сохранить"},
            {"settings", "Настройки"}};
        said = a.what;
        for (const auto& [w, ru] : words)
            if (a.what == w) said = std::string("Кнопка: ") + ru;
    }
    check_log_.push_back(said);
    if (check_log_.size() > 32) check_log_.erase(check_log_.begin());
    refresh_check();
}

} // namespace forge::editor_app
