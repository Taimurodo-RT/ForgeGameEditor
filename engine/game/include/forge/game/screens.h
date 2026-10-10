#pragma once

// The game's own screens, drawn in the editor's «Интерфейс» tab: the pages
// game/ui/<name>.html, shown over the world and tied to the game's data.
//
// What a page says about itself (design::screen_html writes it):
//   on its top layer: forge-screen  when it is shown: "playing" (over the
//                                    world while the player plays, a HUD),
//                                    "command" (when a button or the logic
//                                    shows it), "menu" (the main menu)
//                     forge-fit, forge-size, forge-bars
//                                    how it meets a player's screen of
//                                    another size (expand, fit, stretch)
//                     forge-pauses   the world stops while it is shown
//                     forge-esc="0"  Esc does not close it
//                     forge-music    the music it plays while it is up (a
//                                    file of the game's sounds folder)
//                     forge-ending   a window the game shows itself when it
//                                    ends: "win" (won), "lose" (lost)
//                     forge-button-sound
//                                    its buttons' sound when pressed
//   on any layer:     forge-text     a text with {variables} put in
//                     forge-show-if  shown only while this holds
//                     forge-bar-value, -max, -from
//                                    cut to value / max of its length
//                     forge-click    what a click does: [["show", "Магазин"], ...]
//                     forge-click-sound
//                                    its own sound when pressed ("none": none;
//                                    without it, the screen's forge-button-sound)
//                     forge-disabled a button that is off («Выключена»): a
//                                    click on it does nothing and makes no sound
//                     forge-picture  a picture from the data: "item.icon"
//                     forge-list     a list of the game's: "items" (what the
//                                    hero carries) or "quests" (the journal).
//                                    Its first layer is the cell, repeated once
//                                    per element (forge-cell: its "x y w h",
//                                    forge-list-gap: the space between cells,
//                                    in rows that wrap); the other layers show
//                                    only while the list is empty. Inside a
//                                    cell, item.<field> reads the element:
//                                    items: id, name, count, icon, about, index;
//                                    quests: id, title, text, done, index.
//                                    Only the cells in sight exist: scrolling
//                                    10 000 elements moves a few dozen cells.
//
// The same class runs the editor's «Проверить»: it shows one page and its
// clicks, with variables the author sets by hand.
//
// Sound (forge::audio::ScreenSounds plays it): one music at a time, the
// music of the topmost screen up that has one: the newest window shown by
// command, else the menu, else the music of the place the hero is in (the
// game says it: set_place_music), else the screens over the world. A screen
// without music lets the one under it play; none up with music and no
// place's: silence. on_music
// says when that changes (a page loaded again with the same music changes
// nothing). A button's sound goes to on_sound once per press, with its
// actions: a click or Enter / Space on a button with the keyboard's focus.

#include "forge/game/quests.h"
#include "forge/game/vars.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Rml {
class Context;
class Element;
class ElementDocument;
} // namespace Rml

namespace forge::game {

enum class ScreenRole : u8 { Playing, Command, Menu };

// What the game says about one kind of thing the hero carries (inv.<id>).
struct ScreenItem {
    std::string id;
    std::string name;    // shown as item.name (the id when there is none)
    std::string picture; // item.icon: a path in the game folder (pictures/...), may be empty
    std::string about;
};

// How a page drawn for page_w × page_h meets a player's screen of
// view_w × view_h pixels (forge-fit). The one rule for the game, the
// editor's canvas and its «Проверить»:
//   expand   one scale for both sides (the smaller side's), the page grows
//            on the other side: layers stuck to an edge follow it;
//   fit      the whole page at one scale, centred, bars around it;
//   stretch  each side scaled on its own.
struct ScreenFit {
    f32 sx = 1, sy = 1;      // the scale of a page pixel on the player's screen
    f32 left = 0, top = 0;   // where the page's corner is (player's pixels)
    f32 width = 0, height = 0; // the page's size it lays out at (page pixels)
};
ScreenFit fit_screen(std::string_view mode, f32 page_w, f32 page_h, f32 view_w, f32 view_h);
// The same in page pixels: a point of the player's screen, and back.
inline f32 fit_to_view_x(const ScreenFit& f, f32 x) { return f.left + x * f.sx; }
inline f32 fit_to_view_y(const ScreenFit& f, f32 y) { return f.top + y * f.sy; }
inline f32 fit_to_page_x(const ScreenFit& f, f32 x) { return (x - f.left) / f.sx; }
inline f32 fit_to_page_y(const ScreenFit& f, f32 y) { return (y - f.top) / f.sy; }
// Puts a page's top layer where the fit says (size, place, scale); bars:
// the colour around a fitted page ("" leaves the page's own).
void apply_screen_fit(Rml::Element* root, const ScreenFit& f, const std::string& bars = {});
// Its transform alone ("scale(...)"), for movements to build on.
std::string screen_fit_transform(const ScreenFit& f);

struct ScreenAction {
    std::string what;   // "show", "hide", "toggle", "close", "message", "change", "talk",
                        // "pause", "resume", "menu", "quit", "new", "continue", "load", "save", "settings"
    std::string target; // a screen, a message, statements, a dialogue
};

class GameScreens {
public:
    GameScreens();
    ~GameScreens();
    GameScreens(const GameScreens&) = delete;
    GameScreens& operator=(const GameScreens&) = delete;

    // Loads every page of game_dir/ui (hidden). ua_sheet: the browser
    // defaults' href for html_to_rml. watch: pages saved again (by the
    // editor) load again while the game runs.
    void load(Rml::Context* context, const std::filesystem::path& game_dir, bool watch);
    // One page from memory (the editor's «Проверить»); name: what show() calls it.
    bool load_page(Rml::Context* context, const std::string& name, const std::string& html, const std::string& url);
    void unload();
    void remove(std::string_view name);

    // Every frame: which pages show (playing: the player plays; menu: the
    // main menu is up), their fit to the window, and their game data.
    void update(const Vars& vars, bool playing, bool menu, int width, int height, const CallFn& call = {});

    bool show(std::string_view name, bool on);
    bool toggle(std::string_view name);
    bool shown(std::string_view name) const;
    bool exists(std::string_view name) const;
    void hide_commands(); // back to the main menu: windows opened in the game close
    // Where windows come up now: over the main menu (true) or over the game.
    // update() keeps it; the editor's «Проверить» says it for its screen.
    void set_over_menu(bool menu);
    // A window set to come up only over the game (or only over the main
    // menu) fits only there: show() does not open it elsewhere.
    bool fits(std::string_view name) const;
    // Its «Где появляется»: "game", "menu" or "" (anywhere).
    std::string over(std::string_view name) const;
    // Esc: hides the newest shown window when Esc closes it; false when no
    // window is up or the newest one stays (the ones under it stay too).
    bool close_top();
    // The windows up now (Command pages shown), the newest last.
    std::vector<std::string> windows() const;
    // The windows (Command pages) the game shows itself when it ends this way ("win", "lose": forge-ending), by
    // name; and showing them. show_ending returns how many came up (0: the game has none for it).
    std::vector<std::string> endings(std::string_view kind) const;
    u32 show_ending(std::string_view kind);
    // A shown page stops the world.
    bool pauses() const;
    // The game's own main menu is there (the shell's is not shown).
    bool has_menu() const;
    std::vector<std::string> names() const;

    // A click on a layer that does something: the actions, in order, and the
    // page it was on. Set by the shell (or the editor).
    std::function<void(const ScreenAction& action, const std::string& page)> on_action;
    // Every action goes to on_action, the pages' own too (the editor's
    // «Проверить» opens the other screen on its canvas).
    bool actions_to_caller = false;
    // Runs one action that only touches the pages (show, hide, toggle,
    // close); false for anything else.
    bool run_page_action(const ScreenAction& action, const std::string& page);

    // Clicks give the actions of the nearest layer up the tree that has any
    // (and its sound). Returns true when the element (or a parent) had
    // actions; a button that is off (forge-disabled) takes the click and
    // does nothing.
    bool click(Rml::Element* element);

    // The music of the screens up now ("": none), as update() last saw it.
    const std::string& music() const;
    // The music of the place the hero is in ("": none), from the game: under
    // a window's or a menu's, over the screens' over the world. update()
    // takes it in.
    void set_place_music(std::string name);
    const std::string& place_music() const;
    // Says the music changed (update(); "" when none is wanted).
    std::function<void(const std::string& name)> on_music;
    // Leaving (the editor's «Проверить», the game): on_music(""), and the
    // next update() starts the music again.
    void stop_music();
    // A button's sound, once per press.
    std::function<void(const std::string& name)> on_sound;
    // The keyboard's focus is on a button of these pages (a held Enter or
    // Space must not press it again with every repeat).
    bool has_focus(Rml::Context* context) const;
    // The variables a page reads (for the editor's «Проверить» panel).
    std::vector<std::string> variables(std::string_view name) const;

    Rml::ElementDocument* document(std::string_view name) const;

    // The lists' data: the names and pictures of the things inv.<id> counts
    // (in the order lists show them; others follow by id), and the quests the
    // journal list shows.
    void set_items(std::vector<ScreenItem> items);
    void set_quests(const QuestBook* quests);
    // Cells a page's lists hold now (made once, reused while scrolling).
    usize list_cells(std::string_view name) const;
    // Time the last update() spent on lists (milliseconds).
    f64 lists_ms() const;

private:
    struct Page;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Parses forge-click's JSON list.
std::vector<ScreenAction> parse_actions(std::string_view json);

} // namespace forge::game
