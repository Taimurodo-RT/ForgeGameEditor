#pragma once

// The «Интерфейс» tab: the game's screens (HUD, menus, windows), drawn the way
// Figma draws frames.
//
// Left: the game's screens and the layers of the open one (a tree; the eye
// hides a layer, the lock keeps it from being picked on the canvas; a double
// click renames). Middle: the canvas. The screen is drawn by the UI engine
// exactly as the game will draw it (the page design::screen_html makes), into
// a picture of its own that the canvas shows scaled. Around it: rulers (drag
// out of a ruler for a guide line, drag a guide back onto it to remove it),
// the pasteboard, the selection with its eight handles and size, and while
// moving or resizing the snapping lines: edges and middles of the screen,
// of the other layers and of guides, equal gaps between neighbours (pink
// lines and numbers, as in Figma). Right: the selected layer's design:
// place and size, constraints, auto layout, fills, stroke, corners, effects,
// opacity and blend, text; drawn art: a picture fill repeated as a texture,
// a frame picture cut into nine parts, a mask picture. With nothing selected
// the panel shows the screen's settings: background, default text, how it
// fits a player's screen of another size, the safe margin (dashed on the
// canvas, snapped to).
//
// Sample pictures for drawn interfaces (ui/art: frames, buttons, textures,
// masks) are copied into the game's pictures/интерфейс/ when the tab starts,
// unless the author already has them.
//
// Tools: V select, F frame, R rectangle, O ellipse, T text (drag to draw,
// or click for a default size). Wheel: scroll; Ctrl+wheel: zoom at the
// mouse; middle button or Space+drag: pan; Shift+0 / Shift+1: 100% / fit.
// Keys on the selection: arrows nudge (Shift: 10 px), Delete removes,
// Ctrl+D duplicates, Ctrl+C/V copy and paste, Ctrl+G puts it into a frame,
// Shift+A gives it auto layout, Ctrl+] / Ctrl+[ bring forward / send back,
// Esc selects the parent, Enter the first child.
//
// Components: a layer made a component lives in the game's library
// (ui/components.json, «Компоненты» above the screens); the screens get
// copies of it (instances, purple in the layer list) that follow every change
// of the component while keeping what was changed on the copy itself (its
// text, colours...). A component can have variants (a property and its
// values: «Состояние»: Обычная, Наведение, Нажата, Выключена); a copy picks
// one, and the variants named after states are what the button looks like
// when hovered, pressed or disabled in the game.
//
// Screens are the game's ui/<name>.json; every change writes it and the page
// ui/<name>.html at once (one step of the tab's history each; a drag is one
// step).

#include "forge/audio/screen_sounds.h"
#include "forge/editor/color_pick.h"
#include "forge/editor/document.h"
#include "forge/editor/ui_design.h"
#include "forge/editor/ui_simple.h"
#include "forge/editor/undo.h"
#include "forge/game/screens.h"
#include "forge/game/vars.h"
#include "forge/ui/drawing.h"
#include "forge/ui/ui.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace forge::editor_app {

class UiEditor {
public:
    // game_dir holds ui/ (made with a first screen when missing).
    bool init(ui::Ui& ui, const std::filesystem::path& game_dir);
    void shutdown();
    void bind(Rml::DataModelConstructor& model);
    void set_model(Rml::DataModelHandle handle) { model_ = handle; }

    // Shown or hidden with the tab: the screen is drawn only while shown.
    void set_shown(bool shown);
    void update(Rml::Context* context);
    // Mouse on the canvas and rulers. density: window pixels per event unit.
    bool handle_event(const SDL_Event& e, f32 density, Rml::Context* context);
    bool handle_key(const SDL_KeyboardEvent& k);

    editor::UndoStack& history() { return history_; }
    void undo();
    void redo();
    std::string status() const;

    // --- actions (clicks, keys, the self-test) ---
    std::vector<std::string> screens() const; // file names without .json
    bool open(const std::string& screen);
    const std::string& opened() const { return name_; }
    std::string new_screen();
    const editor::design::Screen& screen() const { return screen_; }

    // --- components ---
    // The library is ui/components.json, opened on the canvas like a screen.
    static constexpr const char* kLibrary = "components";
    bool open_library() { return open(kLibrary); }
    bool library_open() const { return name_ == kLibrary; }
    const editor::design::Screen& library() const { return library_; }
    // The selected layer becomes a component: on a screen it moves into the
    // library and an instance takes its place; in the library it is named one.
    bool make_component();
    // A copy of a component in the middle of the open screen; its id.
    u32 place_component(const std::string& component);
    // A new variant of the selected component (a copy beside it); states:
    // the usual look plus hover, pressed and disabled variants at once.
    bool add_variant(bool states);
    // The selected instance: its changes forgotten, or turned into plain layers.
    bool reset_instance();
    bool detach_instance();
    // Opens the library at the selected instance's variant.
    bool edit_component();

    // «Простой / Полный»: how the tab shows the screen. Simple: blocks a
    // beginner knows (a button, a text, a picture, a bar, a list) with a few
    // plain properties; what they do not show is kept and named. Choosing
    // changes nothing of the screen and makes no step of the history.
    void set_simple(bool on);
    bool simple() const { return simple_; }
    // A new block in the middle of the screen, selected; one step of the history.
    u32 add_block(editor::design::Block block);
    // A simple property of the selected block (the screen with nothing
    // selected): "name", "text", "size", "color" ("#rrggbb", or "@key" for a
    // colour of the game's), "text_color", "x", "y", "w", "h", "anchor_h",
    // "anchor_v", "action" ("none" or show, close, message, new, continue,
    // load, save, settings, pause, resume, menu, quit), "target", "picture",
    // "bar_value", "bar_max", "list", "picture_from", "show". One step of the
    // history however many layers it changes; false when it does not fit.
    bool set_simple_property(const std::string& field, const std::string& value);

    // --- the colour picker (13.4) ---
    // A click on a colour's swatch, in either panel, opens it beside the
    // swatch: a square of saturation and brightness, the hue, the alpha over
    // a checkerboard, the HEX, the game's colours and an eyedropper. What it
    // picks shows at once on the canvas (and in «Проверить») without being
    // written or kept: «Готово» (or a click outside it) keeps it as one step
    // of the history, none when nothing changed; «Отмена» and Esc put
    // everything back as it was, links to the game's colours and styles too.
    // field: a colour field of the full panel ("fill.0.color", "fill.0.color2"
    // (a gradient's last stop), "stroke.color", "effect.1.color",
    // "text_color", "screen.text_color", "screen.bars",
    // "motion.key.2.color", "color.0.color" and "textstyle.0.color" on the
    // library's page) or of the simple one ("simple.color",
    // "simple.text_color"). (x, y, w, h): the swatch, window pixels.
    bool open_picker(const std::string& field, f32 x = 0, f32 y = 0, f32 w = 0, f32 h = 0);
    bool picker_open() const { return picker_.open; }
    const std::string& picker_field() const { return picker_.field; }
    editor::design::Color picker_color() const { return picker_.color; }
    const std::string& picker_link() const { return picker_.link; }
    // The picker's square, hue and alpha as the mouse sets them (0..1 each).
    void picker_square(f32 saturation, f32 brightness);
    void picker_hue(f32 at);
    void picker_alpha_at(f32 at);
    // The HEX field (Enter or leaving it): false, and nothing changed, for a
    // part of a colour or a typo; the reason is shown under the field.
    bool picker_hex(const std::string& text);
    // The alpha field in percent.
    bool picker_alpha(const std::string& percent);
    // A colour of the game's: a fill follows it (a link, kept when the game's
    // colour changes); anything else gets a copy of it.
    bool picker_theme(const std::string& key);
    // The eyedropper: the next click on the screen on the canvas takes the
    // colour shown there; a click anywhere else, the right button or Esc
    // puts it away. It neither selects nor changes another layer.
    void picker_dropper(bool on);
    bool picker_dropping() const { return picker_.dropper; }
    // The eyedropper on the screen as «Проверить» runs it (the game's values,
    // the player's size): the canvas comes alive with the picker still open on
    // the selected layers' colour. Its clicks are the eyedropper's, never the
    // game's (no button is pressed). Closing the picker, «Готово» or «Отмена»,
    // ends that check and shows the layers again.
    bool picker_live(bool on);
    bool picker_living() const { return picker_.live; }
    // Takes the colour the canvas shows at a window point; false off the screen.
    bool picker_sample(f32 mx, f32 my);
    // keep: «Готово»; otherwise «Отмена».
    void close_picker(bool keep);
    // After the UI's layout: the open picker beside its swatch, inside the
    // window. True when it moved (the UI is laid out again to show it there).
    bool place_picker(Rml::Context* context);
    // After the UI's layout: a field of a key row that moved (its time typed) keeps the keyboard on
    // that key's row, not on the neighbour's that took its place. True when the focus moved.
    bool follow_moved_key(Rml::Context* context);

    enum class Tool : u8 { Select, Frame, Rectangle, Ellipse, Text };
    void set_tool(Tool tool);
    Tool tool() const { return tool_; }

    void select(const std::vector<u32>& ids);
    const std::vector<u32>& selection() const { return selection_; }
    // Adds a layer as the tool would on a click at (x, y) of the screen; its id.
    u32 add_layer(editor::design::NodeType type, f32 x, f32 y, f32 w, f32 h, u32 parent = 0);
    bool remove_selection();
    bool duplicate_selection();
    bool wrap_selection_in_frame();
    // A new list frame (selected) around the layer, which becomes its cell; one
    // step of the history.
    bool make_list(u32 cell, editor::design::ListSource source);
    bool add_auto_layout();
    bool move_selection(f32 dx, f32 dy);
    // Moving layers between frames (13.6): ids (a frame with all inside it) into
    // parent, before its index-th child of those that stay (past the end: on
    // top), each where it is on the screen (its place counted from the new
    // frame, its size kept; in a row or column it joins the flow there). One
    // step of the history; false (and move_note() says why) when refused, or
    // on a player's screen other than «Макет». Everything of the layers stays:
    // ids, children, clicks, links to the game's colours and styles, movement,
    // a copy's own changes.
    bool move_into(const std::vector<u32>& ids, u32 parent, usize index);
    // Why the last move was refused, or that a moved layer is hidden by its new frame's edge (empty: neither).
    const std::string& move_note() const { return m_move_note_; }
    // A layer carried in the layers' list or on the canvas, and where it would go.
    bool layer_dragging() const { return tree_.on && tree_.moved; }
    bool layer_pressed() const { return tree_.on; }
    bool canvas_moving() const { return drag_ == Drag::Move && dragged_; }
    u32 drop_parent() const { return tree_.on ? tree_.parent : drop_parent_; }
    usize drop_index() const { return tree_.on ? tree_.index : drop_index_; }
    static constexpr double drop_wait_seconds() { return kDropWait; } // how long the mouse rests over a frame to go in
    // The part of a layer the mouse can reach on the canvas: its box inside every frame around it that hides
    // what sticks out (a frame with «Обрезать», a list); nullopt when none of it shows.
    std::optional<editor::design::Rect> visible_box(u32 id) const;
    // Changes a field of the selected layers by its name in the design panel
    // ("x", "w", "fill.0.color", "text", "layout.gap"...); false when the
    // value does not fit.
    bool set_property(const std::string& field, const std::string& value);
    // The screen's box of a layer (the screen's pixels), after layout.
    std::optional<editor::design::Rect> layer_box(u32 id) const;

    // «Проверить»: the screen comes alive on the canvas. Buttons react to the
    // mouse (hover, pressed) and do what they do in the game: one that opens
    // a screen opens it here, «Изменить данные» changes the values in the
    // panel, the rest are written in the panel's list. Texts, bars and
    // conditions show the values the panel sets. Esc ends it.
    void set_checking(bool on);
    bool checking() const { return checking_; }
    // The screen is shown while playing with a solid background: it hides the world.
    bool covers_game() const;
    forge::game::Vars& check_vars() { return check_vars_; }
    const std::vector<std::string>& check_log() const { return check_log_; }
    // The page on the canvas (for tests: its elements).
    Rml::ElementDocument* page() const { return page_; }
    // Mouse on the screen's own pixels, as the check passes it to the page.
    void check_mouse(f32 x, f32 y, int button_down, int button_up);

    // The design panel's tab: "design" (how the layer looks), "game" (what
    // it does in the game: clicks, a bar, when it shows) or "motion" (how it
    // moves).
    void set_panel(const std::string& tab) {
        m_panel_ = tab == "game" || tab == "motion" ? tab : "design";
        dirty("ue_panel");
    }
    const Rml::String& panel() const { return m_panel_; }

    // --- The movement's timeline («Движение», «Своё, по ключам», one layer) ---
    // The key picked on the timeline or in its row (-1: none); picking changes nothing.
    int timeline_key() const { return selected_key_; }
    void pick_key(int index);
    bool timeline_shown() const;
    // Seconds the timeline spans: the delay, a pass, and a second pass when it repeats.
    f32 timeline_span() const;
    // A key carried along the timeline by the mouse: one step when let go, none on Esc.
    bool key_dragging() const { return key_drag_.on; }
    // The canvas showing the screen as the player sees it `seconds` after it
    // appears (its movements played by the same engine as the game's). Nothing
    // of it goes into the screen or the history; another layer or screen,
    // «Проверить», Esc or «К редактированию» end it.
    void preview_at(double seconds);
    void preview_play(bool on);
    void end_preview();
    bool motion_preview() const { return preview_.on; }
    bool preview_playing() const { return preview_.playing; }
    double preview_time() const { return preview_.t; }

    // The canvas view: zoom (1 = 100%) and where the screen's top left is
    // on the canvas (pixels of the canvas).
    f32 zoom() const { return zoom_; }
    void set_zoom(f32 zoom, f32 at_x, f32 at_y);
    void zoom_to_fit();
    void update_aspect();
    // Screen pixels to canvas pixels and back. A stretched screen shown on
    // a player's screen of other proportions is drawn stretched too: its
    // sides get their own zoom (zoom_x(), zoom_y()).
    f32 zoom_x() const { return zoom_ * aspect_x_; }
    f32 zoom_y() const { return zoom_ * aspect_y_; }
    f32 to_canvas_x(f32 x) const { return pan_x_ + x * zoom_x(); }
    f32 to_canvas_y(f32 y) const { return pan_y_ + y * zoom_y(); }
    f32 to_screen_x(f32 x) const { return (x - pan_x_) / zoom_x(); }
    f32 to_screen_y(f32 y) const { return (y - pan_y_) / zoom_y(); }

    // The player's screen the canvas and «Проверить» show. 0 «Макет»: the
    // screen's own size, where layers are drawn and moved; 1..5: the sizes
    // a player may have (1280×720, 1920×1080, 2560×1440, 21:9 2560×1080,
    // 4:3 1440×1080). The page meets them as in the game
    // (game::fit_screen); the screen's data stays as it is.
    struct ViewSize {
        const char* label; // on the switch
        f32 w, h;          // 0: the screen's own
    };
    static const std::vector<ViewSize>& view_sizes();
    void set_view(int view);
    int view() const { return view_; }
    f32 view_w() const;
    f32 view_h() const;
    // The fit of the screen on that player's screen.
    forge::game::ScreenFit view_fit() const;
    // A size other than «Макет»: layers are not moved or drawn by mouse.
    bool previewing() const { return view_ != 0; }
    const Rml::String& view_note() const { return m_view_note_; }
    // The canvas element's box in the window (for tests and input).
    f32 canvas_left() const { return canvas_x_; }
    f32 canvas_top() const { return canvas_y_; }

    // The game's values the panel offers for texts, bars and conditions
    // ({name, what the author reads}); asked when the panel refreshes.
    std::function<std::vector<std::pair<std::string, std::string>>()> game_values;
    // What lists show in «Проверить»: the game's things (inv.<id>) and its quests.
    std::function<std::vector<game::ScreenItem>()> game_items;
    std::function<const game::QuestBook*()> game_quests;
    // Sounds of «Ресурсы» the author can pick for a screen's music and its
    // buttons besides the game's own (a pick is copied into game/sounds).
    std::function<std::vector<std::filesystem::path>()> list_sounds;
    // «Проверить» plays without a device (tests); its sound, for tests.
    bool silent = false;
    std::filesystem::path sounds_folder() const { return game_dir_ / "sounds"; }
    const forge::audio::ScreenSounds& check_sound() const { return check_sound_; }
    forge::audio::Mixer& check_mixer() { return check_mixer_; }
    // The sounds the drop-downs offer: {value, what the author reads}.
    std::vector<std::pair<std::string, std::string>> sound_choices() const;
private:
    friend class UiCommand;

    // --- files ---
    std::filesystem::path ui_dir() const { return game_dir_ / "ui"; }
    std::filesystem::path json_path(const std::string& name) const { return editor::design::screen_file(ui_dir(), name, ".json"); }
    std::filesystem::path html_path(const std::string& name) const { return editor::design::screen_file(ui_dir(), name, ".html"); }
    bool write(const std::string& name, const editor::design::Screen& screen) const;
    void apply(const std::string& name, const std::string& json, const std::vector<u32>& selection);
    // Records a change: the screen as it is now against before.
    void commit(const std::string& before, std::string label, std::string merge = {});
    void remember_geometry(const std::string& before);

    // The library changed: every screen's instances follow it.
    void propagate_library();
    editor::design::HtmlOptions html_options() const;

    // --- the page on the canvas ---
    void rebuild_page();
    void read_boxes();
    // The game's pictures (pictures/..., for the panel's lists).
    void scan_pictures();
    void install_art(const std::filesystem::path& art_dir);
    std::array<f32, 4> art_slice(const std::string& picture) const;
    // The layer under a screen point: the deepest visible, unlocked one;
    // then the one to pick at the current selection's level (deep: the deepest).
    u32 hit(f32 x, f32 y, bool deep) const;
    u32 container_at(f32 x, f32 y) const;
    // The frame a carried layer would go into at a screen point: the deepest shown, unlocked frame there that
    // takes it (not one of the carried, not a copy of a component or a list), else the screen. Where among its
    // children: in a row or column by the point, otherwise on top.
    u32 drop_target(f32 x, f32 y, const std::vector<u32>& moving, usize& index) const;
    // The layers' list under the mouse while carrying: target and index (false: nowhere).
    bool tree_target(Rml::Context* context, f32 my);
    bool tree_event(const SDL_Event& e, f32 density, Rml::Context* context);
    // move_into with the boxes the layers are to keep (screen pixels) and the screen before the gesture.
    bool move_to(const std::vector<u32>& ids, u32 parent, usize index,
                 const std::unordered_map<u32, editor::design::Rect>& want, const std::string& before);
    void show_move_note(std::string note);
    void end_tree_drag();
    editor::design::Rect selection_box() const;
    editor::design::SnapTargets snap_targets() const;

    // --- view ---
    void read_canvas(Rml::Context* context);
    void refresh_view();
    void refresh_rulers();
    void refresh_overlay();
    void refresh_layers();
    void refresh_screens();
    void refresh_props();
    void dirty(const char* name);
    void dirty_all();

    // --- mouse ---
    enum class Drag : u8 { None, Pan, Move, Resize, Draw, Marquee, Guide };
    bool press(f32 mx, f32 my, u8 button, u8 clicks, Rml::Context* context);
    void drag_to(f32 mx, f32 my);
    void release();

    // «Проверить».
    void check_action(const forge::game::ScreenAction& a);
    void refresh_check();
    void seed_check_vars();

    // Field edits.
    bool set_on(editor::design::Node& n, const std::string& field, const std::string& value);
    // set_on, and inside a copy of a component the change is the copy's own.
    bool set_field(editor::design::Node& n, const std::string& field, const std::string& value);
    std::string simple_shown(const std::string& field) const;
    void refresh_simple(const editor::design::Node* n);

    // The colour picker.
    struct Picker {
        bool open = false, simple = false, linkable = false, dropper = false;
        bool picked = false; // a colour was chosen (even the one it opened with): it goes to every selected layer
        bool mixed = false;  // the selected layers differ in this colour (or its link)
        bool live = false;   // «Проверить» runs for the eyedropper (picker_live)
        std::string field, before, label, link, start_link;
        editor::design::Color start{}, color{};
        editor::design::Hsva hsva;
        f32 ax = 0, ay = 0, aw = 0, ah = 0; // the swatch it opened from (window pixels)
        f32 left = -1, top = -1;            // where it stands
        int drag = 0;                       // the mouse holds 1: the square, 2: the hue, 3: the alpha
    };
    Picker picker_;
    bool previewing_ = false; // commit() keeps nothing: the picker shows its colour
    Rml::Context* context_ = nullptr; // the editor's (the panels', the canvas's)
    struct PickerView {
        bool open = false, dropper = false, live = false, can_live = false;
        Rml::String title, hex, alpha, hex_note, note, link, hint;
        Rml::String old_swatch = "transparent", new_swatch = "transparent", hue_swatch = "#ff0000", alpha_bar = "none";
        float sv_x = 0, sv_y = 0, hue_x = 0, alpha_x = 0; // percent
    };
    PickerView m_cp_;
    std::optional<editor::design::Color> color_of(const std::string& field) const;
    std::optional<editor::design::Color> color_of(const std::string& field, u32 id) const;
    void picker_set(editor::design::Color c, const std::string& link);
    void picker_preview();
    void refresh_picker();
    bool picker_event(const SDL_Event& e, f32 density, Rml::Context* context);
    void picker_drag_to(f32 mx, f32 my, Rml::Context* context);
    void switch_checking(bool on);

    ui::Ui* ui_ = nullptr;
    std::filesystem::path game_dir_;
    Rml::DataModelHandle model_;
    Rml::Context* page_context_ = nullptr;
    Rml::ElementDocument* page_ = nullptr;
    bool shown_ = false;
    bool page_dirty_ = true;

    editor::Document doc_; // the history needs one; the screens live in their files
    editor::UndoStack history_{doc_};

    std::string name_;
    editor::design::Screen screen_;
    editor::design::Screen library_; // the components (the open screen when name_ is kLibrary)
    std::vector<u32> selection_;
    std::unordered_map<u32, editor::design::Rect> boxes_; // after the page's layout
    Tool tool_ = Tool::Select;
    std::vector<editor::design::Node> clipboard_;

    // View.
    f32 zoom_ = 0.5f, pan_x_ = 40, pan_y_ = 40;
    f32 aspect_x_ = 1, aspect_y_ = 1; // a stretched screen's proportions on the player's screen
    int view_ = 0;
    f32 canvas_x_ = 0, canvas_y_ = 0, canvas_w_ = 0, canvas_h_ = 0;
    bool fit_pending_ = true;
    f32 mouse_x_ = 0, mouse_y_ = 0; // window pixels
    bool space_down_ = false;

    // Drag.
    Drag drag_ = Drag::None;
    bool dragged_ = false;
    std::unordered_map<u32, editor::design::Rect> grab_boxes_; // each carried layer's box at the grab
    f32 grab_sx_ = 0, grab_sy_ = 0;                            // the carried layers' shift (screen pixels)
    u32 drop_parent_ = 0;                                      // the frame a carried layer goes into (0: its own)
    u32 drop_wait_ = 0;                                        // the frame under the mouse, waited on
    u64 drop_wait_ns_ = 0;
    static constexpr double kDropWait = 0.5;                   // seconds the mouse rests over a frame to go in
    usize drop_index_ = 0;
    // A layer carried in the layers' list: nothing changes until it is let go.
    struct TreeDrag {
        bool on = false, moved = false;
        u32 id = 0;                // the row pressed
        f32 down_x = 0, down_y = 0;
        std::vector<u32> moving;   // what goes (the selection, when the row is in it)
        u32 row = 0;               // the row under the mouse
        Rml::String where;         // "above", "below", "into", "no"
        u32 parent = 0;            // 0: nowhere
        usize index = 0;
    };
    TreeDrag tree_;
    f32 grab_mx_ = 0, grab_my_ = 0;       // window pixels
    f32 grab_pan_x_ = 0, grab_pan_y_ = 0; // view at the grab
    std::string grab_json_;               // the screen at the grab (undo)
    struct Grabbed {
        u32 id;
        f32 x, y, w, h;
    };
    std::vector<Grabbed> grabbed_;
    editor::design::Rect grab_box_{};     // the selection's box at the grab (screen pixels)
    int handle_ = -1;                     // 0..7: tl, t, tr, r, br, b, bl, l
    u32 drawn_ = 0;                       // the layer being drawn
    int guide_ = -1;                      // the guide being dragged
    bool guide_vertical_ = true;
    std::vector<editor::design::SnapLine> snap_lines_;
    std::vector<editor::design::SnapGap> snap_gaps_;
    editor::design::Rect marquee_{};
    u32 hover_ = 0;
    u64 last_click_ns_ = 0;
    f32 last_click_x_ = 0, last_click_y_ = 0;

    // Lines on the canvas (rulers, snapping).
    struct Lines final : ui::LineSource {
        std::vector<ui::Line> list;
        u64 v = 0;
        const std::vector<ui::Line>& lines() const override { return list; }
        u64 version() const override { return v; }
    };
    Lines ruler_x_, ruler_y_, overlay_lines_;

    // --- bound to the document ---
    struct ScreenRow {
        Rml::String name, title, size;
        bool selected = false;
    };
    struct LayerRow {
        int id = 0;
        Rml::String name, icon;
        int depth = 0;
        bool selected = false, visible = true, locked = false, container = false, open = true, hidden_by_parent = false,
             component = false; // an instance, or a component in the library
        Rml::String drop;           // a layer carried over the list: "above", "below", "into" or "no" (refused)
    };
    struct ComponentRow {
        Rml::String name, icon;
        int variants = 0;
    };
    struct VariantRow {
        Rml::String property, value;
        std::vector<Rml::String> values;
    };
    struct Tick {
        float at = 0;
        Rml::String label;
    };
    struct Box {
        float x = 0, y = 0, w = 0, h = 0;
    };
    struct Label {
        float x = 0, y = 0;
        Rml::String text;
    };
    struct FillRow {
        int index = 0;
        Rml::String kind, kind_name, hex, hex2, opacity, swatch, angle, image, fit, tile, offset_x, offset_y, style;
        Rml::String swatch2 = "transparent";
        bool visible = true;
    };
    struct EffectRow {
        int index = 0;
        Rml::String kind, kind_name, hex, x, y, blur, spread, swatch;
        bool visible = true, shadow = true;
    };
    struct Props {
        bool any = false, many = false, root = false, text = false, container = false, in_layout = false,
             has_layout = false, has_stroke = false;
        Rml::String name, type_name, icon;
        Rml::String x, y, w, h, rotation;
        Rml::String horizontal, vertical, width_sizing, height_sizing;
        Rml::String layout_mode, gap, pad_t, pad_r, pad_b, pad_l;
        int align = 0;
        bool space_between = false, clip = false, absolute = false;
        Rml::String radius, radius_tl, radius_tr, radius_br, radius_bl;
        bool radius_mixed = false;
        Rml::String opacity, blend;
        Rml::String stroke_hex, stroke_swatch = "transparent", stroke_width, stroke_align, stroke_style;
        Rml::String content, family, size, weight, line_height, letter_spacing, text_align, text_case, decoration,
            text_hex, text_swatch = "transparent";
        bool italic = false;
        // The screen.
        Rml::String screen_font, screen_size, screen_text_hex, screen_text_swatch = "transparent", screen_fit, bars_hex,
            bars_swatch = "transparent", safe;
        // Drawn art.
        bool has_frame = false, frame_visible = true, frame_fill = true, has_mask = false, mask_visible = true;
        Rml::String frame_image, frame_t, frame_r, frame_b, frame_l, frame_scale, frame_repeat, mask_image, mask_fit;
        // Components.
        bool can_make_component = false, lib_variant = false, instance = false, in_instance = false, changed_here = false;
        Rml::String component;
        Rml::String text_style; // the game's text style the text follows ("": its own)
        // The link to the game.
        Rml::String screen_show; // "playing", "command", "menu"
        bool pauses = false, esc_closes = true;
        bool covers_game = false; // shown while playing with a solid background: the world can't be seen
        Rml::String show_if;
        // Lists: the frame's own (list: "none", "items", "quests"), and the
        // list whose cell the layer is in (in_list: its fields can be shown).
        Rml::String list = "none", list_gap, picture_from, in_list;
        bool list_no_cell = false;
        bool has_bar = false;
        Rml::String bar_value, bar_max, bar_from;
        // Movement.
        Rml::String motion_kind = "none", motion_duration, motion_delay, motion_strength, motion_easing;
        bool motion_loop = true, motion_back = false;
        Rml::String smooth, smooth_easing;
        Rml::String appear = "none", appear_time;
        // Sound: the screen's music and its buttons' sound, a button's own
        // ("": as the screen's, "none": none).
        Rml::String music, button_sound, click_sound, sound_note;
    };
    // One key of the layer's own movement («Своё по ключам»).
    struct KeyRow {
        int index = 0;
        Rml::String at, x, y, scale, rotation, opacity, radius, blur, brightness;
        bool tint = false, selected = false;
        Rml::String hex, swatch;
    };
    // What a click on the layer does in the game, one row each.
    struct ClickRow {
        int index = 0;
        Rml::String kind, target;
        Rml::String needs; // "screen", "text", "talk" or "" (no target)
        Rml::String hint;  // the target field's placeholder
    };
    // A value of the game a screen can show («Монеты»: inv.coins).
    struct ValueRow {
        Rml::String name, label;
    };
    // The game's colours and text styles (shown on the library's page).
    struct GameColorRow {
        int index = 0;
        Rml::String key, name, hex, swatch;
    };
    struct GameTextRow {
        int index = 0;
        Rml::String key, name, family, size, weight, hex, swatch;
    };

    std::vector<ScreenRow> m_screens_;
    std::vector<ComponentRow> m_components_;
    std::vector<VariantRow> m_variants_; // the selected component variant's or instance's properties
    bool m_library_open_ = false;
    std::vector<GameColorRow> m_game_colors_;
    std::vector<ClickRow> m_clicks_;
    std::vector<KeyRow> m_keys_;
    // The timeline: a mark per key (and pale ones for what is not the author's to move).
    struct TimelineMark {
        int index = -1; // the key's (-1: the layer's own place at an end, or the second pass)
        float x = 0;    // percent of the track
        bool selected = false, tint = false;
        Rml::String swatch = "transparent", title;
    };
    struct TimelineView {
        bool show = false, preview = false, playing = false, repeat = false;
        Rml::String note, time, span;
        float delay_w = 0, pass_w = 100, head_x = 0; // percent
        std::vector<TimelineMark> keys, ghosts;
    };
    TimelineView m_tl_;
    int selected_key_ = -1;
    usize moved_key_ = 0; // where the last key moved by «motion.key.N.at» is now
    // A key moved by its typed time: until the panel is laid out again (the next frame), its row's fields
    // still name the old place, so their events (the blur after Enter) go to where the keys are now.
    struct KeyRemap {
        bool on = false;
        usize from = 0, to = 0;
    } key_remap_;
    std::string remap_key_field(const std::string& field) const;
    // The focused key field to move to once the panel is laid out: {its id now, its id then}.
    std::string focus_from_, focus_to_;
    struct KeyDrag {
        bool on = false, moved = false;
        int from = -1, index = -1; // where the key was, where it is now
        f32 down_x = 0, grab = 0;  // the mouse at the press; how far from the key's mark (track pixels)
        std::string before;
    };
    KeyDrag key_drag_;
    bool scrubbing_ = false;
    struct Preview {
        bool on = false, playing = false;
        double t = 0;     // seconds since the screen appeared
        double built = 0; // the time the page was built and played up to
        u64 last_ns = 0;
    };
    Preview preview_;
    void refresh_timeline();
    bool timeline_event(const SDL_Event& e, f32 density, Rml::Context* context);
    void key_drag_to(f32 mx);
    f32 timeline_time_at(f32 mx) const;
    bool timeline_wanted() const;
    bool checking_ = false;
    std::unique_ptr<forge::game::GameScreens> check_;
    // «Проверить»'s sound: the screen's music and its buttons.
    forge::audio::Mixer check_mixer_;
    bool check_mixer_open_ = false;
    u64 check_sound_ns_ = 0;
    forge::audio::ScreenSounds check_sound_;
    struct SoundRow {
        Rml::String value, name;
        bool operator==(const SoundRow&) const = default;
    };
    std::vector<SoundRow> m_sounds_;
    Rml::String sound_note_; // why the last pick did not take
    void scan_sounds();
    void sync_sound_selects();
    // A drop-down's sound: a name of game/sounds as it is, "add:<path>" a
    // file of «Ресурсы» copied there first. Empty when it cannot be used.
    std::optional<std::string> sound_pick(const std::string& value);
    bool page_from_check_ = false;
    forge::game::Vars check_vars_;
    u64 check_seen_ = ~0ull;
    std::vector<forge::game::ScreenAction> check_pending_; // clicks wait for the end of the page's event
    std::vector<std::string> check_back_; // screens opened by clicks, to go back to
    std::vector<std::string> check_log_;
    struct CheckVarRow {
        Rml::String name, label, value;
    };
    std::vector<CheckVarRow> m_check_vars_;
    std::vector<Rml::String> m_check_log_;
    bool m_checking_ = false;
    Rml::String m_panel_ = "design"; // the design panel's tab: "design" or "game"
    std::vector<ValueRow> m_values_;
    std::vector<Rml::String> m_screen_names_; // the other screens (a click's target)
    std::vector<GameTextRow> m_game_texts_;
    std::vector<LayerRow> m_layers_;
    std::vector<Tick> m_ticks_x_, m_ticks_y_;
    std::vector<Box> m_selected_;
    Box m_sel_box_;
    bool m_handles_ = false;
    Box m_hover_;
    bool m_hovering_ = false;
    Box m_drop_;                 // the frame a layer carried on the canvas goes into
    bool m_dropping_ = false;
    Rml::String m_drop_label_;   // «В рамку «Б»»
    std::string m_move_note_;    // why a move was refused
    Rml::String m_move_note_shown_;
    Box m_marquee_;
    bool m_marqueeing_ = false;
    std::vector<Label> m_measures_;
    Rml::String m_size_label_;
    Label m_frame_label_;
    Box m_frame_;
    std::vector<Box> m_guides_x_, m_guides_y_;
    Rml::String m_tool_ = "select";
    Rml::String m_zoom_text_;
    struct ViewRow {
        Rml::String label, title;
    };
    std::vector<ViewRow> m_views_;
    int m_view_ = 0;
    Rml::String m_view_note_; // what the canvas shows now, in words
    Rml::String m_title_;
    Rml::String m_renaming_; // a layer id or "screen:<name>" being renamed
    Rml::String m_rename_text_;
    Props m_p_;
    // «Простой»: the selected block's plain properties.
    bool simple_ = false;
    bool m_simple_ = false;
    struct SimpleProps {
        bool root = false, many = false, has_text = false, has_color = false, has_text_color = false, can_act = false,
             act_editable = true, picture = false, bar = false, list = false, in_list = false, has_hidden = false;
        Rml::String block, word, icon, name, text, size, hex, swatch = "transparent", style, text_hex,
            text_swatch = "transparent";
        Rml::String x, y, w, h, anchor_h, anchor_v;
        Rml::String action = "none", target, needs;
        Rml::String image, bar_value, bar_max, list_source, picture_from, show, inside;
        Rml::String hidden;
        Rml::String music, button_sound, click_sound;
    };
    SimpleProps m_s_;
    SimpleProps m_s_shown_; // as last shown: the drop-downs write into m_s_ before their change arrives
    std::vector<FillRow> m_fills_;
    std::vector<EffectRow> m_effects_;
    std::vector<Rml::String> m_families_;
    struct PictureRow {
        Rml::String path, name;
    };
    std::vector<PictureRow> m_pictures_;
    std::unordered_map<std::string, std::array<f32, 4>> art_slices_; // the samples' cuts, by file name
    Box m_safe_;
    bool m_has_safe_ = false;
    std::vector<int> m_nine_{0, 1, 2, 3, 4, 5, 6, 7, 8}; // the 3x3 align grid
    std::vector<u32> closed_; // frames folded in the layer list
};

} // namespace forge::editor_app
