#pragma once

// The colour picker (13.4), one for the whole editor: «Интерфейс» opens it on a colour of its layers, «Изображения» on
// a colour of its brush. It stands beside the swatch it was opened from: a square of saturation and brightness, the hue,
// the alpha over a checkerboard, the HEX and the alpha in percent, the game's colours and an eyedropper. What it picks
// is shown at once by the one it was opened for, without being kept: «Готово» (Enter, or a click outside it) keeps it,
// «Отмена» and Esc put back what was there. While it is open it has the mouse and the keys: a click outside it closes it
// and goes no further.
//
// The markup is #ue-picker in ui/editor/editor.rml; its model is ue_cp (the picker) and ue_cp_colors (the game's
// colours).

#include "forge/editor/color_pick.h"
#include "forge/editor/ui_design.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <functional>
#include <string>
#include <vector>

namespace forge::editor_app {

class ColorPicker {
public:
    // The one the picker is opened for.
    class Client {
    public:
        virtual ~Client() = default;
        // The picker's colour is to be shown (link: the key of the game colour it follows, or none); nothing is kept.
        virtual void picker_show(editor::design::Color color, const std::string& link) = 0;
        // Closed: «Готово» keeps what is shown, «Отмена» puts back what was there. live: «Проверить» was on for the
        // eyedropper (it is off now).
        virtual void picker_closed(bool keep, bool live) = 0;
        // The eyedropper's click at a point of the window: false when the point is not on what the client shows (the
        // eyedropper is put away then). The client gives the colour there with take(), or says with hint() why not.
        virtual bool picker_sample(f32 x, f32 y) = 0;
        // The picker's title and what its colour is tied to, in words; what the eyedropper does (live: on the screen of
        // «Проверить»).
        virtual std::string picker_title() const = 0;
        virtual std::string picker_note() const = 0;
        virtual std::string picker_dropper_hint(bool live) const = 0;
        // «Проверить» for the eyedropper (only «Интерфейс»): false when it cannot be switched.
        virtual bool picker_checking(bool on) {
            (void)on;
            return false;
        }
    };

    struct Request {
        std::string field;              // what is picked, in the client's words ("fill.0.color", "image.left")
        editor::design::Color start{};  // the colour it opens with
        std::string link;               // the game colour that colour follows ("" none)
        bool linkable = false;          // a game colour picked is followed (a link); otherwise it is copied
        bool mixed = false;             // the colour shown is the first of several that differ
        bool can_live = false;          // «Проверить» for the eyedropper
        f32 x = 0, y = 0, w = 0, h = 0; // the swatch, window pixels
    };

    // The game's colours (those of «Компоненты» in «Интерфейс»).
    std::function<std::vector<editor::design::NamedColor>()> game_colors;

    void bind(Rml::DataModelConstructor& model);
    void set_model(Rml::DataModelHandle handle) { model_ = handle; }

    // Closes what is open (kept), then opens for client. False: nothing opened.
    bool open(Client& client, const Request& request, Rml::Context* context);
    // keep: «Готово»; otherwise «Отмена».
    void close(bool keep);
    bool is_open() const { return state_.open; }
    bool open_for(const Client& c) const { return state_.open && client_ == &c; }
    const std::string& field() const { return state_.field; }
    editor::design::Color color() const { return state_.color; }
    editor::design::Color start() const { return state_.start; }
    const std::string& link() const { return state_.link; }
    const std::string& start_link() const { return state_.start_link; }
    bool linkable() const { return state_.linkable; }
    bool picked() const { return state_.picked; }
    bool mixed() const { return state_.mixed; }
    // The game colour last picked (followed or copied): its key, "" after any other colour.
    const std::string& theme_key() const { return state_.theme; }

    // The square, hue and alpha as the mouse sets them (0..1 each).
    void square(f32 saturation, f32 brightness);
    void hue(f32 at);
    void alpha_at(f32 at);
    // The HEX field (Enter or leaving it): false, and nothing changed, for a part of a colour or a typo; the reason
    // is shown under the field.
    bool hex(const std::string& text);
    // The alpha field in percent.
    bool alpha(const std::string& percent);
    // A colour of the game's: followed where the client allows a link, copied otherwise.
    bool theme(const std::string& key);
    // The eyedropper: the next click takes the colour the client shows there; a click anywhere else, the right button
    // or Esc puts it away.
    void dropper(bool on);
    bool dropping() const { return state_.dropper; }
    bool live(bool on);
    bool living() const { return state_.live; }
    // From the client's picker_sample: the colour at the point (the eyedropper put away, `said` under it), or why there
    // is none (the eyedropper stays).
    void take(editor::design::Color c, const std::string& said);
    void hint(const std::string& said);
    // The texts again (the client's state changed).
    void refresh();

    // After the UI's layout: the open picker beside its swatch, inside the window. True when it moved (the UI is laid
    // out again to show it there).
    bool place(Rml::Context* context);
    // The mouse and Esc while it is open: what is under it gets nothing.
    bool handle_event(const SDL_Event& e, f32 density, Rml::Context* context);
    // The keys while it is open (no text field has them): Esc puts the eyedropper away, then cancels; Enter keeps.
    // Every other key is the picker's too: the tab under it does nothing.
    bool handle_key(const SDL_KeyboardEvent& k);

private:
    struct State {
        bool open = false, linkable = false, dropper = false, live = false, can_live = false;
        bool picked = false; // a colour was chosen (even the one it opened with)
        bool mixed = false;
        std::string field, link, start_link, theme;
        editor::design::Color start{}, color{};
        editor::design::Hsva hsva;
        f32 ax = 0, ay = 0, aw = 0, ah = 0; // the swatch it opened from (window pixels)
        f32 left = -1, top = -1;            // where it stands
        int drag = 0;                       // the mouse holds 1: the square, 2: the hue, 3: the alpha
    };
    struct View {
        bool open = false, dropper = false, live = false, can_live = false;
        Rml::String title, hex, alpha, hex_note, note, link, theme, hint; // theme: the game colour last picked (its chip marked)
        Rml::String old_swatch = "transparent", new_swatch = "transparent", hue_swatch = "#ff0000", alpha_bar = "none";
        float sv_x = 0, sv_y = 0, hue_x = 0, alpha_x = 0; // percent
    };
    struct Chip {
        int index = 0;
        Rml::String key, name, swatch;
    };

    void set(editor::design::Color c, const std::string& link);
    void changed(); // the colour moved: shown by the client, the view made again
    void drag_to(f32 mx, f32 my, Rml::Context* context);
    void dirty(const char* name) {
        if (model_) model_.DirtyVariable(name);
    }

    Client* client_ = nullptr;
    State state_;
    View view_;
    std::vector<Chip> chips_;
    f32 mouse_x_ = 0, mouse_y_ = 0;
    Rml::Context* context_ = nullptr;
    Rml::DataModelHandle model_;
};

} // namespace forge::editor_app
