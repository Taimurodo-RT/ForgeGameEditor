#pragma once

// The template library of the «Интерфейс» tab (13.12): ready constructions
// (a card, a grid of things, a form) and whole screens (a pause, an
// inventory, a shop), each a screen of its own in a folder, listed by its
// templates.json.
//
// Taking a template copies it. The copy is the author's, as if drawn by
// hand: changing the template later does not change it (a construction the
// author wants to keep in step everywhere becomes a component). A
// construction goes onto the screen open now as one layer; a screen
// template makes a new screen with its settings (when it shows, pause, Esc,
// veil), or goes onto the screen open now as one frame of its size.
//
// A template's screen is written as the editor writes screens (save_screen),
// so the editor and the game read it as any other; the index says what it
// is for the library: its group, title, variant and the words it is found by.
//
// What a copy shares with the game instead of copying: the game's
// components, colours and text styles a layer is linked to (kept where the
// game has them, else made plain with the look kept), and what layers name
// by name or path: pictures and sounds of the game's folders, screens a
// button opens, variables. Nothing inside a template names another of its
// layers by id, so fresh ids keep it whole.
//
// The author's own templates («Сохранить как шаблон») live in the game's
// ui/templates folder, in the same form: they go with the project.

#include "forge/editor/ui_design.h"

#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace forge::editor::design {

enum class TemplateKind : u8 {
    Construction, // a part of a screen: one layer
    Screen,       // a whole screen
};
const char* template_kind_word(TemplateKind k); // "construction", "screen"

// The group the author's own templates go to unless another is picked.
inline constexpr const char* kOwnGroup = "Мои";

struct TemplateInfo {
    std::string file; // <file>.json next to templates.json
    TemplateKind kind = TemplateKind::Construction;
    std::string group;   // «Карточки», «Пауза»
    std::string title;   // «Карточка предмета»
    std::string variant; // «Дерево» ("": the only one)
    std::string about;   // what it is and what of the game it uses, a sentence or two
    std::vector<std::string> words; // more words search finds it by
    bool own = false; // the author's, from the game's folder (not written in the index: where it lies says it)
    bool operator==(const TemplateInfo&) const = default;
};

struct Template {
    TemplateInfo info;
    Screen screen;
};

// The groups of a kind, in the library's order (kOwnGroup first).
const std::vector<std::string>& template_groups(TemplateKind kind);

std::string save_template_index(const std::vector<TemplateInfo>& list);
bool load_template_index(std::string_view json, std::vector<TemplateInfo>& out, std::string* error = nullptr);
// The templates of a folder (templates.json and the screens it names), in its
// order; what cannot be read is left out and said in errors.
std::vector<Template> load_templates(const std::filesystem::path& dir, std::vector<std::string>* errors = nullptr);

// Links to the game kept where library (the game's components, colours and
// text styles) has what they name; the rest made plain, their look kept, and
// said once each in dropped («компонента «Кнопка»», «цвета игры», «стиля
// текста игры»). No library: all made plain.
void keep_links(Node& n, const Screen* library, std::vector<std::string>* dropped = nullptr);

// The layer a template puts onto a screen: a construction's one layer (its
// screen's only layer at the top; several are put into a frame of the
// screen's size), or a screen's content in a frame of its size named after
// it, its background the frame's fill. Its links as keep_links leaves them,
// with fresh ids from into.next_id.
Node template_layer(const Template& t, Screen& into, const Screen* library = nullptr, std::vector<std::string>* dropped = nullptr);
// A new screen from a template: its size, settings and layers, titled title.
Screen template_screen(const Template& t, std::string title, const Screen* library = nullptr,
                       std::vector<std::string>* dropped = nullptr);
// What the library's preview of a template draws: a screen template as it is,
// up at once as a window (no veil, no appearing, no sound) and fitted whole
// with background bars, on background when it has none of its own (a window,
// what is over the game); a construction alone in the middle of a screen of
// 16:9 around it, on background.
Screen template_preview(const Template& t, Color background);

// --- the author's own («Сохранить как шаблон») ---
// A template of a layer of from as it shows there (texts that take the
// screen's font or size get them written in, so it looks the same on any
// screen), or of the whole screen (layer nullptr). Its links stay.
Template own_template(const Screen& from, const Node* layer, std::string title, std::string group);
// A file name for title, one rule for an own template and for a screen made
// from a template: small letters; spaces, dots and _ as one _; letters,
// digits and - kept, every other sign left out (what a file system or a path
// takes for its own: "/\\:*?\"<>|", "..", and the like); no _ or - at the
// ends; at most 64 bytes; not a name Windows keeps for a device with any
// extension (CON, PRN, AUX, NUL, COM0–9, LPT0–9: "_1" after it); fallback
// when nothing is left. A name in a folder, never a path out of it.
std::string safe_file_name(std::string_view title, std::string_view fallback);
// The first of name, name_2, name_3… that no file of dir has with one of
// exts, small and capital letters alike (Windows does not tell «Меню.json»
// from «меню.json»), and that is none of taken.
std::string free_file_name(const std::filesystem::path& dir, const std::string& name, std::initializer_list<std::string_view> exts,
                           const std::vector<std::string>& taken = {});
// An own template's file name for title: safe_file_name, "шаблон" when
// nothing is left, never "templates" (the index).
std::string template_file_name(std::string_view title);
// Writes t into dir: its screen in its own file and the index. replace: the
// file of an own template it takes the place of ("": a new file, numbered
// when the name is taken, never another's). The file name, or "" (error says
// why).
std::string save_own_template(const std::filesystem::path& dir, Template t, const std::string& replace,
                              std::string* error = nullptr);
// Takes an own template out of dir (its file and its line in the index).
bool remove_own_template(const std::filesystem::path& dir, const std::string& file);

} // namespace forge::editor::design
