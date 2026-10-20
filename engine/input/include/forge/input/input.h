#pragma once

// What the player does, by name (the editor's tab «Ввод», editors/ввод-модель.md): «Прыжок», «Пауза». A game asks
// down("jump") and pressed("talk"), not which key is held; which keys, mouse buttons and gamepad buttons make an action
// is the module's default, then the author's (the game's input.json), then the player's («Настройки», settings.json).
//
//   forge::input::Input input;
//   input.set(forge::input::resolve(defs, author, player));
//   input.handle_event(e);           // every SDL event, before the UI takes it
//   if (input.down("left")) ...      // held
//   if (input.pressed("talk")) ...   // went down since the last end_frame()
//   input.end_frame();
//
// A bind is written as text: "key:Space" (the key's place on the keyboard, as SDL names it, not the letter of the
// layout), "mouse:left", "pad:south", "pad:leftx-" (a stick or a trigger past half way).

#include "forge/core/types.h"
#include "forge/data/reflect.h"

#include <SDL3/SDL_events.h>

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct SDL_Gamepad;

namespace forge::input {

enum class Device : u8 { Key, Mouse, Pad, PadAxis };

struct Bind {
    Device device = Device::Key;
    i32 code = 0; // SDL_Scancode, SDL_BUTTON_LEFT…, SDL_GamepadButton or SDL_GamepadAxis
    i8 dir = 0;   // a stick: -1 or +1; a trigger: +1; 0 otherwise
    bool operator==(const Bind&) const = default;
};

// "key:Space" … false when the text names nothing.
bool parse(std::string_view text, Bind& out);
std::string format(const Bind& b);
// In the player's words: «Пробел», «←», «Левая кнопка мыши», «Геймпад: A (нижняя)», «Левый стик ←».
std::string words(const Bind& b);
// Whether a bind is the player's keyboard or mouse (else a gamepad's).
inline bool is_pad(const Bind& b) { return b.device == Device::Pad || b.device == Device::PadAxis; }
// Every mouse button, gamepad button, stick direction and trigger, in that order: what an author picks from a list
// without a gamepad at hand.
std::vector<Bind> mouse_and_pad_binds();

// An action a module or the shell has, with its default binds (as text).
struct ActionDef {
    std::string id;    // "jump"
    std::string words; // "Прыжок"
    std::vector<std::string> defaults;
};
// The shell's own, in every game: pause, journal, quicksave, quickload.
const std::vector<ActionDef>& shell_actions();

// An action's binds as someone chose them (the author's input.json, the player's settings.json "keys").
struct Choice {
    std::string action;
    std::vector<std::string> binds;
};
using Choices = std::vector<Choice>;

// input.json: {"actions": [{"id": "jump", "binds": ["key:K"]}]}. False and why when it is not that (the file stays
// unread); a bad entry in it is left out with a warning.
bool read_choices(std::string_view json, Choices& out, std::string* error, std::vector<std::string>* warnings = nullptr);
std::string write_choices(const Choices& choices);
// The choice for action (null: none).
const Choice* find(const Choices& choices, std::string_view action);
// Sets the choice for an action, or removes it when binds are what the action has without it (base: the module's
// defaults for the author, the author's binds for the player); keeps the others in order.
void choose(Choices& choices, std::string_view action, std::vector<std::string> binds, const std::vector<std::string>& base);

// An action as it plays: its binds after the defaults, the author's and the player's choices.
struct Action {
    std::string id;
    std::string words;
    std::vector<Bind> binds;
};
// defs in their order; each layer replaces an action's binds. A choice for an action defs have not and a bind that
// names nothing are left out, each with a warning in words.
std::vector<Action> resolve(const std::vector<ActionDef>& defs, const Choices& author, const Choices& player,
                            std::vector<std::string>* warnings = nullptr);

// One bind on two actions.
struct Clash {
    Bind bind;
    std::string a, b; // the actions' words, in their order
};
std::vector<Clash> clashes(const std::vector<Action>& actions);
// «Пробел — у «Прыжок» и «Поговорить»».
std::string clash_words(const Clash& c);

// The bind an event is: a key, a mouse button or a gamepad button going down, a stick or a trigger going past half
// way. None for anything else (a repeat, a key going up, the mouse moving).
std::optional<Bind> bind_of(const SDL_Event& e);

class Input {
public:
    Input() = default;
    ~Input();
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;

    void set(std::vector<Action> actions);
    const std::vector<Action>& actions() const { return actions_; }
    const Action* action(std::string_view id) const;

    // Every event: what is held, gamepads connected and gone (opened and closed here), the window losing the
    // keyboard (all keys let go).
    void handle_event(const SDL_Event& e);
    // Whether this event presses the action (going down, not a repeat).
    bool presses(const SDL_Event& e, std::string_view action) const;
    bool down(std::string_view action) const;
    bool pressed(std::string_view action) const;
    void end_frame();
    // Nothing held (a new game, a lost window).
    void release_all();
    // The button this event pressed is neither held nor pressed for any action until it is let go: a press that moves a
    // scene on is the scene's, not the world's (part 6г3 of docs/models/сцены-модель.md).
    void swallow(const SDL_Event& e);
    u32 gamepads() const { return static_cast<u32>(pads_.size()); }
    // The action's first keyboard or mouse bind in words, or its first gamepad bind; "" when it has none.
    std::string first_words(std::string_view action) const;

private:
    bool held(const Bind& b) const;
    struct Pad {
        SDL_Gamepad* pad = nullptr;
        u32 buttons = 0;     // bit per SDL_GamepadButton
        i16 axes[8] = {};    // SDL_GamepadAxis
    };
    std::vector<Action> actions_;
    std::vector<i32> keys_;    // scancodes held
    u32 mouse_ = 0;            // bit per SDL_BUTTON_*
    std::unordered_map<u32, Pad> pads_; // by SDL_JoystickID
    std::vector<Bind> went_down_;
    std::vector<Bind> swallowed_; // held, but no action's until let go
};

} // namespace forge::input

FORGE_REFLECT_DECLARE(forge::input::Choice)
