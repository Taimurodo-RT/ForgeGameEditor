#include "forge/editor/ui_templates.h"

#include "forge/core/file.h"
#include "forge/core/path.h"

#include <yyjson.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <span>

namespace forge::editor::design {

namespace {

const char* const kKinds[] = {"construction", "screen"};

std::string str(yyjson_val* o, const char* key) {
    const char* s = yyjson_get_str(yyjson_obj_get(o, key));
    return s ? s : "";
}

void say_once(std::vector<std::string>* list, std::string what) {
    if (list && std::find(list->begin(), list->end(), what) == list->end()) list->push_back(std::move(what));
}

// Small letters for a file name: Latin and Cyrillic (a file system may not tell «Пауза» from «пауза»).
std::string lower_letters(std::string_view text) {
    std::string out;
    for (usize i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c >= 'A' && c <= 'Z') {
            out += static_cast<char>(c - 'A' + 'a');
        } else if (c == 0xD0 && i + 1 < text.size()) {
            const unsigned char n = static_cast<unsigned char>(text[i + 1]);
            ++i;
            if (n >= 0x90 && n <= 0x9F) out += {'\xD0', static_cast<char>(n + 0x20)};       // А–П
            else if (n >= 0xA0 && n <= 0xAF) out += {'\xD1', static_cast<char>(n - 0x20)};  // Р–Я
            else if (n == 0x81) out += {'\xD1', '\x91'};                                   // Ё
            else out += {static_cast<char>(c), static_cast<char>(n)};
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

bool read_text_file(const std::filesystem::path& p, std::string& out) {
    std::vector<u8> bytes;
    if (!read_file(p, bytes)) return false;
    out.assign(bytes.begin(), bytes.end());
    return true;
}

bool write_text_file(const std::filesystem::path& p, const std::string& text) {
    return write_file_atomic(p, std::span(reinterpret_cast<const u8*>(text.data()), text.size()));
}

} // namespace

void keep_links(Node& n, const Screen* library, std::vector<std::string>* dropped) {
    const std::vector<Component> list = library ? components(*library) : std::vector<Component>{};
    // A layer from inside a copy of a component, taken alone: it follows no component any more.
    if (n.component.empty() && n.master) detach(n);
    auto visit = [&](auto&& self, Node& m) -> void {
        if (!m.component.empty() && !find_component(list, m.component)) {
            say_once(dropped, "компонента «" + m.component + "»");
            detach(m); // its layers as they look, plain (an instance inside it is looked at below)
        }
        for (Paint& p : m.fills)
            if (!p.style.empty() && !(library && std::any_of(library->colors.begin(), library->colors.end(),
                                                             [&](const NamedColor& c) { return c.key == p.style; }))) {
                say_once(dropped, "цвета игры");
                p.style.clear();
            }
        if (!m.text_style.style.empty() &&
            !(library && std::any_of(library->text_styles.begin(), library->text_styles.end(),
                                     [&](const NamedTextStyle& t) { return t.key == m.text_style.style; }))) {
            say_once(dropped, "стиля текста игры");
            m.text_style.style.clear();
        }
        for (Node& c : m.children) self(self, c);
    };
    visit(visit, n);
}

const char* template_kind_word(TemplateKind k) { return kKinds[static_cast<usize>(k)]; }

const std::vector<std::string>& template_groups(TemplateKind kind) {
    static const std::vector<std::string> constructions{kOwnGroup, "Сетки", "Формы", "Карточки", "Списки"};
    static const std::vector<std::string> screens{kOwnGroup, "Главное меню", "Над игрой", "Пауза", "Инвентарь", "Магазин", "Сохранения",
                                                  "Настройки"};
    return kind == TemplateKind::Construction ? constructions : screens;
}

std::string save_template_index(const std::vector<TemplateInfo>& list) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_uint(doc, root, "version", 1);
    yyjson_mut_val* a = yyjson_mut_arr(doc);
    for (const TemplateInfo& t : list) {
        yyjson_mut_val* o = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, o, "file", t.file.c_str());
        yyjson_mut_obj_add_str(doc, o, "kind", template_kind_word(t.kind));
        yyjson_mut_obj_add_strcpy(doc, o, "group", t.group.c_str());
        yyjson_mut_obj_add_strcpy(doc, o, "title", t.title.c_str());
        if (!t.variant.empty()) yyjson_mut_obj_add_strcpy(doc, o, "variant", t.variant.c_str());
        yyjson_mut_obj_add_strcpy(doc, o, "about", t.about.c_str());
        if (!t.words.empty()) {
            yyjson_mut_val* w = yyjson_mut_arr(doc);
            for (const std::string& s : t.words) yyjson_mut_arr_add_strcpy(doc, w, s.c_str());
            yyjson_mut_obj_add_val(doc, o, "words", w);
        }
        yyjson_mut_arr_append(a, o);
    }
    yyjson_mut_obj_add_val(doc, root, "templates", a);
    usize len = 0;
    char* text = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY_TWO_SPACES, &len);
    std::string out = text ? std::string(text, len) + "\n" : std::string();
    std::free(text);
    yyjson_mut_doc_free(doc);
    return out;
}

bool load_template_index(std::string_view json, std::vector<TemplateInfo>& out, std::string* error) {
    yyjson_read_err err{};
    yyjson_doc* doc = yyjson_read_opts(const_cast<char*>(json.data()), json.size(), 0, nullptr, &err);
    if (!doc) {
        if (error) *error = std::string("JSON: ") + (err.msg ? err.msg : "?") + " at " + std::to_string(err.pos);
        return false;
    }
    yyjson_val* a = yyjson_obj_get(yyjson_doc_get_root(doc), "templates");
    if (!yyjson_is_arr(a)) {
        if (error) *error = "нет списка templates";
        yyjson_doc_free(doc);
        return false;
    }
    out.clear();
    usize i, n;
    yyjson_val* o;
    yyjson_arr_foreach(a, i, n, o) {
        TemplateInfo t;
        t.file = str(o, "file");
        t.kind = str(o, "kind") == "screen" ? TemplateKind::Screen : TemplateKind::Construction;
        t.group = str(o, "group");
        t.title = str(o, "title");
        t.variant = str(o, "variant");
        t.about = str(o, "about");
        if (yyjson_val* w = yyjson_obj_get(o, "words"); yyjson_is_arr(w)) {
            usize j, m;
            yyjson_val* s;
            yyjson_arr_foreach(w, j, m, s) if (const char* c = yyjson_get_str(s)) t.words.emplace_back(c);
        }
        if (!t.file.empty()) out.push_back(std::move(t));
    }
    yyjson_doc_free(doc);
    return true;
}

std::vector<Template> load_templates(const std::filesystem::path& dir, std::vector<std::string>* errors) {
    std::vector<Template> out;
    std::vector<u8> bytes;
    if (!read_file(dir / "templates.json", bytes)) {
        if (errors) errors->push_back("нет " + path_to_utf8(dir / "templates.json"));
        return out;
    }
    std::vector<TemplateInfo> list;
    std::string error;
    if (!load_template_index(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), list, &error)) {
        if (errors) errors->push_back("templates.json: " + error);
        return out;
    }
    for (TemplateInfo& info : list) {
        Template t;
        if (!read_file(screen_file(dir, info.file, ".json"), bytes) ||
            !load_screen(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), t.screen, &error)) {
            if (errors) errors->push_back(info.file + ".json: " + (bytes.empty() ? std::string("не читается") : error));
            continue;
        }
        t.info = std::move(info);
        out.push_back(std::move(t));
    }
    return out;
}

Node template_layer(const Template& t, Screen& into, const Screen* library, std::vector<std::string>* dropped) {
    const Screen& s = t.screen;
    Node n;
    if (t.info.kind == TemplateKind::Construction && s.root.children.size() == 1) {
        n = s.root.children.front();
    } else {
        // The screen's content in a frame of its size, its background the frame's.
        n.name = t.info.title.empty() ? s.title : t.info.title;
        n.type = NodeType::Frame;
        n.w = s.width;
        n.h = s.height;
        n.fills = s.root.fills;
        n.strokes = s.root.strokes;
        n.effects = s.root.effects;
        n.layout = s.root.layout;
        n.clip = true;
        n.children = s.root.children;
    }
    keep_links(n, library, dropped);
    renumber(into, n);
    return n;
}

Screen template_screen(const Template& t, std::string title, const Screen* library, std::vector<std::string>* dropped) {
    Screen s = t.screen;
    s.title = std::move(title);
    s.root.name = s.title;
    s.library = false;
    s.guides.clear();
    s.colors.clear();
    s.text_styles.clear();
    keep_links(s.root, library, dropped);
    return s;
}

Screen template_preview(const Template& t, Color background) {
    Screen s = t.screen;
    Paint ground;
    ground.color = background;
    if (t.info.kind == TemplateKind::Construction && s.root.children.size() == 1) {
        Node& n = s.root.children.front();
        const f32 margin = std::max(48.0f, std::max(n.w, n.h) * 0.12f);
        f32 w = n.w + margin * 2, h = n.h + margin * 2;
        if (w * 9 < h * 16) w = h * 16 / 9;
        else h = w * 9 / 16;
        s.width = std::round(w);
        s.height = std::round(h);
        n.x = std::round((s.width - n.w) / 2);
        n.y = std::round((s.height - n.h) / 2);
        s.root.fills = {ground};
        s.root.strokes.clear();
        s.root.effects.clear();
        s.root.layout = {};
    } else if (s.root.fills.empty()) {
        s.root.fills = {ground}; // a window or what is over the game: on the ground, as over a game
    }
    s.root.w = s.width;
    s.root.h = s.height;
    s.show = ScreenShow::Command;
    s.over = WindowOver::Any;
    s.pauses = false;
    s.dim = false;
    s.esc_closes = false;
    s.appear = Appear::None;
    s.fit = ScreenFit::Fit;
    s.bars = background;
    s.safe = 0;
    s.music.clear();
    s.button_sound.clear();
    s.library = false;
    return s;
}

Template own_template(const Screen& from, const Node* layer, std::string title, std::string group) {
    Template t;
    t.info.own = true;
    t.info.title = std::move(title);
    t.info.group = std::move(group);
    if (layer) {
        t.info.kind = TemplateKind::Construction;
        t.info.about = "Своя конструкция: «" + layer->name + "» с экрана «" + from.title + "».";
        Screen s = make_screen(t.info.title, layer->w, layer->h);
        s.text = from.text;
        Node n = *layer;
        if (n.component.empty() && n.master) detach(n); // a part of a copy of a component: plain, as it shows
        n.x = 0;
        n.y = 0;
        n.horizontal = n.vertical = Constraint::Start;
        // As it shows on from: the screen's font and size written into the texts that take them from it.
        auto bake = [&](auto&& self, Node& m) -> void {
            if (m.type == NodeType::Text) {
                if (m.text_style.family.empty()) m.text_style.family = from.text.family;
                if (m.text_style.size <= 0) m.text_style.size = from.text.size;
            }
            for (Node& c : m.children) self(self, c);
        };
        bake(bake, n);
        renumber(s, n);
        s.root.children = {std::move(n)};
        t.screen = std::move(s);
    } else {
        t.info.kind = TemplateKind::Screen;
        t.info.about = "Свой экран, сохранён с экрана «" + from.title + "».";
        t.screen = from;
        t.screen.title = t.info.title;
        t.screen.root.name = t.info.title;
        t.screen.library = false;
        t.screen.guides.clear();
    }
    t.info.words = {lower_letters(from.title)};
    return t;
}

std::string safe_file_name(std::string_view title, std::string_view fallback) {
    std::string out;
    for (char ch : lower_letters(title)) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c >= 0x80 || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') out += ch; // letters of any language too
        else if ((c == ' ' || c == '\t' || c == '.' || c == '_') && !out.empty() && out.back() != '_') out += '_';
    }
    if (out.size() > 64) {
        usize cut = 64;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut; // not in the middle of a letter
        out.resize(cut);
    }
    while (!out.empty() && (out.back() == '_' || out.back() == '-')) out.pop_back();
    while (!out.empty() && out.front() == '-') out.erase(out.begin());
    if (out.empty()) out = fallback;
    // Windows' devices: «con.json» or «nul.html» is no file there, whatever the extension.
    static constexpr std::string_view kDevices[] = {"con", "prn", "aux", "nul"};
    const bool numbered = out.size() >= 4 && (out.starts_with("com") || out.starts_with("lpt")) &&
                          ((out.size() == 4 && out[3] >= '0' && out[3] <= '9') || out.substr(3) == "\xC2\xB9" ||
                           out.substr(3) == "\xC2\xB2" || out.substr(3) == "\xC2\xB3"); // COM1, LPT¹
    if (numbered || std::find(std::begin(kDevices), std::end(kDevices), out) != std::end(kDevices)) out += "_1";
    return out;
}

std::string free_file_name(const std::filesystem::path& dir, const std::string& name, std::initializer_list<std::string_view> exts,
                           const std::vector<std::string>& taken) {
    std::vector<std::string> there; // the folder's files, in small letters
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        there.push_back(lower_letters(path_to_utf8(it->path().filename())));
    auto used = [&](const std::string& n) {
        const std::string low = lower_letters(n);
        for (const std::string& t : taken)
            if (lower_letters(t) == low) return true;
        for (std::string_view ext : exts)
            if (std::find(there.begin(), there.end(), low + std::string(ext)) != there.end()) return true;
        return false;
    };
    std::string out = name;
    for (u32 n = 2; used(out); ++n) out = name + "_" + std::to_string(n);
    return out;
}

std::string template_file_name(std::string_view title) {
    std::string out = safe_file_name(title, "шаблон");
    if (out == "templates") out += "_1"; // not the index
    return out;
}

std::string save_own_template(const std::filesystem::path& dir, Template t, const std::string& replace, std::string* error) {
    std::vector<TemplateInfo> list;
    std::string text;
    if (read_text_file(dir / "templates.json", text) && !load_template_index(text, list, error)) return {};
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::string file = replace;
    const bool replacing = !replace.empty() &&
                           std::any_of(list.begin(), list.end(), [&](const TemplateInfo& i) { return i.file == replace; });
    if (!replacing) {
        // A name no other has, in the index or among the folder's files (any letters' size).
        std::vector<std::string> listed;
        for (const TemplateInfo& i : list) listed.push_back(i.file);
        file = free_file_name(dir, template_file_name(t.info.title), {".json"}, listed);
    }
    t.info.file = file;
    t.info.own = false; // where it lies says it
    if (!write_text_file(screen_file(dir, file, ".json"), save_screen(t.screen))) {
        if (error) *error = "не записался " + path_to_utf8(screen_file(dir, file, ".json"));
        return {};
    }
    if (replacing) {
        for (TemplateInfo& i : list)
            if (i.file == file) i = t.info;
    } else {
        list.push_back(t.info);
    }
    if (!write_text_file(dir / "templates.json", save_template_index(list))) {
        if (error) *error = "не записался " + path_to_utf8(dir / "templates.json");
        return {};
    }
    return file;
}

bool remove_own_template(const std::filesystem::path& dir, const std::string& file) {
    std::vector<TemplateInfo> list;
    std::string text;
    if (!read_text_file(dir / "templates.json", text) || !load_template_index(text, list)) return false;
    const auto it = std::find_if(list.begin(), list.end(), [&](const TemplateInfo& i) { return i.file == file; });
    if (it == list.end()) return false;
    list.erase(it);
    if (!write_text_file(dir / "templates.json", save_template_index(list))) return false;
    std::error_code ec;
    std::filesystem::remove(screen_file(dir, file, ".json"), ec);
    return true;
}

} // namespace forge::editor::design
