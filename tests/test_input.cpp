// Actions and their binds (the tab «Ввод», editors/ввод-модель.md): a bind as text and in words, the module's
// defaults under the author's and the player's choices, what is held and what went down from SDL's events, a gamepad
// (SDL's virtual one) held and let go, a press a waiting scene took (swallowed) no action's until let go.

#include "forge/input/input.h"

#include "forge/core/types.h"
#include "forge/data/json.h"

#include <SDL3/SDL.h>
#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace forge;
using namespace forge::input;

namespace {

const std::vector<ActionDef> kDefs = {
    {"left", "Влево", {"key:A", "key:Left", "pad:dpleft", "pad:leftx-"}},
    {"jump", "Прыжок", {"key:Space", "key:W", "pad:south"}},
    {"talk", "Поговорить", {"key:E", "pad:west"}},
};

SDL_Event key(SDL_Scancode sc, bool down, bool repeat = false) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = sc;
    e.key.down = down;
    e.key.repeat = repeat;
    return e;
}

SDL_Event mouse(u8 button, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.button = button;
    e.button.down = down;
    return e;
}

Bind B(const char* text) {
    Bind b;
    REQUIRE_MESSAGE(parse(text, b), text);
    return b;
}

} // namespace

TEST_CASE("input: a bind is read and written back as the same text, and has words for the player") {
    for (const char* t : {"key:Space", "key:A", "key:F5", "key:Escape", "key:Left", "key:Left Shift", "mouse:left",
                          "mouse:x2", "pad:south", "pad:start", "pad:dpleft", "pad:leftx-", "pad:righty+", "pad:rt"}) {
        Bind b;
        REQUIRE_MESSAGE(parse(t, b), t);
        CHECK(format(b) == t);
        CHECK(words(b) != "?");
    }
    CHECK(words(B("key:Space")) == "Пробел");
    CHECK(words(B("key:Left")) == "←");
    CHECK(words(B("key:K")) == "K");
    CHECK(words(B("mouse:left")) == "Левая кнопка мыши");
    CHECK(words(B("pad:south")) == "Геймпад: A (нижняя)");
    CHECK(words(B("pad:leftx-")) == "Геймпад: левый стик ←");
    CHECK(words(B("pad:lt")) == "Геймпад: левый курок");
    CHECK(is_pad(B("pad:lt")));
    CHECK_FALSE(is_pad(B("mouse:right")));
    Bind b;
    for (const char* bad : {"", "Space", "key:", "key:Пробел", "key:NoSuchKey", "mouse:side", "pad:z", "pad:leftx", "joy:1"})
        CHECK_MESSAGE(!parse(bad, b), bad);
    // The key's place, not the letter: the same text whatever the layout.
    CHECK(B("key:W").code == SDL_SCANCODE_W);
}

TEST_CASE("input: the module's defaults, then the author's choice, then the player's") {
    std::vector<std::string> warnings;
    std::vector<Action> a = resolve(kDefs, {}, {}, &warnings);
    REQUIRE(a.size() == 3);
    CHECK(warnings.empty());
    CHECK(a[1].id == "jump");
    CHECK(a[1].binds == std::vector<Bind>{B("key:Space"), B("key:W"), B("pad:south")});

    const Choices author = {{"jump", {"key:K", "pad:south"}}};
    a = resolve(kDefs, author, {}, &warnings);
    CHECK(a[1].binds == std::vector<Bind>{B("key:K"), B("pad:south")});
    CHECK(a[0].binds.size() == 4); // the others as the module has them

    const Choices player = {{"jump", {"key:L"}}};
    a = resolve(kDefs, author, player, &warnings);
    CHECK(a[1].binds == std::vector<Bind>{B("key:L")});
    CHECK(warnings.empty());

    // An action the game has not and a bind that names nothing: left out, each said.
    a = resolve(kDefs, {{"fly", {"key:F"}}, {"talk", {"key:Nope", "key:T", "key:T"}}}, {}, &warnings);
    CHECK(a[2].binds == std::vector<Bind>{B("key:T")});
    REQUIRE(warnings.size() == 2);
    CHECK(warnings[0] == "клавиши автора: у «Поговорить» привязка «key:Nope» ничего не называет");
    CHECK(warnings[1] == "клавиши автора: действия «fly» нет в этой игре");

    // An action left without binds is the author's choice: it stays without.
    a = resolve(kDefs, {{"talk", {}}}, {}, nullptr);
    CHECK(a[2].binds.empty());
}

TEST_CASE("input: choices are written as input.json and read back; a choice equal to the base is no choice") {
    Choices c;
    choose(c, "jump", {"key:K", "pad:south"}, kDefs[1].defaults);
    choose(c, "talk", {"key:E", "pad:west"}, kDefs[2].defaults); // the defaults: nothing to write
    choose(c, "left", {"key:J"}, kDefs[0].defaults);
    REQUIRE(c.size() == 2);
    const std::string text = write_choices(c);
    CHECK(text ==
          "{\n  \"actions\": [\n    {\"id\": \"jump\", \"binds\": [\"key:K\", \"pad:south\"]},\n"
          "    {\"id\": \"left\", \"binds\": [\"key:J\"]}\n  ]\n}\n");
    Choices back;
    std::string error;
    REQUIRE(read_choices(text, back, &error));
    REQUIRE(back.size() == 2);
    CHECK(back[0].action == "jump");
    CHECK(back[0].binds == c[0].binds);
    CHECK(back[1].binds == std::vector<std::string>{"key:J"});
    choose(c, "jump", kDefs[1].defaults, kDefs[1].defaults); // back to the defaults: gone
    REQUIRE(c.size() == 1);
    CHECK(c[0].action == "left");
    CHECK(write_choices({}) == "{\n  \"actions\": []\n}\n");

    // Not a list of actions: refused; a bad entry: left out with a warning.
    CHECK_FALSE(read_choices("{\"actions\": 3}", back, &error));
    CHECK(error == "нет списка «actions»");
    CHECK_FALSE(read_choices("not json", back, &error));
    CHECK(error == "это не JSON");
    std::vector<std::string> warnings;
    REQUIRE(read_choices("{\"actions\": [{\"id\": 5}, {\"id\": \"jump\", \"binds\": [\"key:K\"]}, {\"id\": \"jump\", \"binds\": []}]}",
                         back, &error, &warnings));
    REQUIRE(back.size() == 1);
    CHECK(back[0].binds == std::vector<std::string>{"key:K"});
    CHECK(warnings == std::vector<std::string>{"действие 1: нужны «id» и список «binds»", "действие «jump» дважды: берётся первое"});
}

TEST_CASE("input: the player's choices go into settings.json and come back") {
    const Choices player = {{"jump", {"key:L", "pad:north"}}};
    const std::string json = data::to_json(player.front());
    Choice back;
    data::LoadReport report;
    REQUIRE(data::from_json(back, json, report));
    CHECK(back.action == "jump");
    CHECK(back.binds == player.front().binds);
}

TEST_CASE("input: one bind on two actions is said in words") {
    std::vector<Action> a = resolve(kDefs, {{"talk", {"key:Space", "key:E"}}}, {}, nullptr);
    std::vector<Clash> c = clashes(a);
    REQUIRE(c.size() == 1);
    CHECK(clash_words(c[0]) == "Пробел — у «Прыжок» и «Поговорить»");
    CHECK(clashes(resolve(kDefs, {}, {}, nullptr)).empty());
    CHECK(clashes(resolve(shell_actions(), {}, {}, nullptr)).empty());
}

TEST_CASE("input: held and pressed come from the window's events, not from the keyboard's state") {
    Input in;
    in.set(resolve(kDefs, {{"jump", {"key:K", "mouse:right"}}}, {}, nullptr));
    CHECK_FALSE(in.down("jump"));
    in.handle_event(key(SDL_SCANCODE_SPACE, true)); // the module's key, not the author's
    CHECK_FALSE(in.down("jump"));
    CHECK_FALSE(in.pressed("jump"));
    in.handle_event(key(SDL_SCANCODE_K, true));
    CHECK(in.down("jump"));
    CHECK(in.pressed("jump"));
    CHECK(in.presses(key(SDL_SCANCODE_K, true), "jump"));
    CHECK_FALSE(in.presses(key(SDL_SCANCODE_K, true, true), "jump")); // a repeat presses nothing
    in.end_frame();
    CHECK(in.down("jump"));
    CHECK_FALSE(in.pressed("jump")); // once, the frame it went down
    in.handle_event(key(SDL_SCANCODE_K, true, true));
    CHECK_FALSE(in.pressed("jump"));
    in.handle_event(key(SDL_SCANCODE_K, false));
    CHECK_FALSE(in.down("jump"));
    // Down and up within one frame: pressed all the same, not held.
    in.end_frame();
    in.handle_event(key(SDL_SCANCODE_K, true));
    in.handle_event(key(SDL_SCANCODE_K, false));
    CHECK(in.pressed("jump"));
    CHECK_FALSE(in.down("jump"));
    in.end_frame();
    // The mouse.
    in.handle_event(mouse(SDL_BUTTON_RIGHT, true));
    CHECK(in.down("jump"));
    in.handle_event(mouse(SDL_BUTTON_RIGHT, false));
    CHECK_FALSE(in.down("jump"));
    // The window loses the keyboard: nothing stays held.
    in.handle_event(key(SDL_SCANCODE_A, true));
    CHECK(in.down("left"));
    SDL_Event lost{};
    lost.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    in.handle_event(lost);
    CHECK_FALSE(in.down("left"));
    CHECK(in.first_words("jump") == "K");
    CHECK(in.first_words("left") == "A");
    CHECK(in.first_words("nope").empty());
    CHECK_FALSE(in.down("nope"));
    // bind_of: what a key, a button, a stick going past half way is.
    CHECK(bind_of(key(SDL_SCANCODE_K, true)) == B("key:K"));
    CHECK_FALSE(bind_of(key(SDL_SCANCODE_K, false)));
    CHECK_FALSE(bind_of(key(SDL_SCANCODE_K, true, true)));
    CHECK(bind_of(mouse(SDL_BUTTON_LEFT, true)) == B("mouse:left"));
    SDL_Event ax{};
    ax.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    ax.gaxis.axis = SDL_GAMEPAD_AXIS_LEFTX;
    ax.gaxis.value = -20000;
    CHECK(bind_of(ax) == B("pad:leftx-"));
    ax.gaxis.value = -10000;
    CHECK_FALSE(bind_of(ax));
}

TEST_CASE("input: a gamepad (SDL's virtual one) is opened when it comes, read, and let go when it goes") {
    REQUIRE(SDL_InitSubSystem(SDL_INIT_GAMEPAD));
    {
        Input in;
        in.set(resolve(kDefs, {}, {}, nullptr));
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = "Forge test pad";
        const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
        REQUIRE(id != 0);
        SDL_Joystick* joy = SDL_OpenJoystick(id);
        REQUIRE(joy);
        // The events SDL makes, as the game's loop takes them.
        const auto pump = [&] {
            SDL_UpdateJoysticks();
            SDL_Event e;
            while (SDL_PollEvent(&e)) in.handle_event(e);
        };
        pump();
        CHECK(in.gamepads() == 1);
        SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
        pump();
        CHECK(in.down("jump"));
        CHECK(in.pressed("jump"));
        in.end_frame();
        SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, false);
        pump();
        CHECK_FALSE(in.down("jump"));
        // The stick a little: not yet; past half way: held, pressed once.
        SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, -8000);
        pump();
        CHECK_FALSE(in.down("left"));
        SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, -30000);
        pump();
        CHECK(in.down("left"));
        CHECK(in.pressed("left"));
        in.end_frame();
        SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, -31000);
        pump();
        CHECK_FALSE(in.pressed("left")); // still past half way: not pressed again
        SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, 0);
        pump();
        CHECK_FALSE(in.down("left"));
        CHECK(in.first_words("jump") == "Пробел");
        // Gone: closed, nothing held.
        SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_WEST, true);
        pump();
        CHECK(in.down("talk"));
        SDL_CloseJoystick(joy);
        SDL_DetachVirtualJoystick(id);
        pump();
        CHECK(in.gamepads() == 0);
        CHECK_FALSE(in.down("talk"));
    }
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

TEST_CASE("input: a swallowed press is no action's until let go (a scene moved on by it, part 6г3)") {
    Input in;
    in.set(resolve(kDefs, {}, {}, nullptr));
    // E moves a waiting scene on: the world sees it neither pressed nor held, other keys as ever.
    in.handle_event(key(SDL_SCANCODE_A, true));
    const SDL_Event e = key(SDL_SCANCODE_E, true);
    in.handle_event(e);
    in.swallow(e);
    CHECK_FALSE(in.pressed("talk"));
    CHECK_FALSE(in.down("talk"));
    CHECK(in.pressed("left"));
    CHECK(in.down("left"));
    in.end_frame();
    in.handle_event(key(SDL_SCANCODE_E, true, true)); // its repeats too
    CHECK_FALSE(in.pressed("talk"));
    CHECK_FALSE(in.down("talk"));
    in.handle_event(key(SDL_SCANCODE_E, false));
    CHECK_FALSE(in.down("talk"));
    // Let go and pressed again: the world's.
    in.handle_event(e);
    CHECK(in.pressed("talk"));
    CHECK(in.down("talk"));
    in.handle_event(key(SDL_SCANCODE_E, false));
    in.end_frame();
    // Space swallowed, W of the same action held: the action is held by W.
    const SDL_Event space = key(SDL_SCANCODE_SPACE, true);
    in.handle_event(space);
    in.swallow(space);
    CHECK_FALSE(in.down("jump"));
    in.handle_event(key(SDL_SCANCODE_W, true));
    CHECK(in.down("jump"));
    CHECK(in.pressed("jump"));
    in.handle_event(key(SDL_SCANCODE_W, false));
    CHECK_FALSE(in.down("jump"));
    in.end_frame();
    // The window losing the keyboard forgets it: Space down again is pressed.
    SDL_Event lost{};
    lost.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    in.handle_event(lost);
    in.handle_event(space);
    CHECK(in.pressed("jump"));
    CHECK(in.down("jump"));
    in.release_all();
    // A mouse button, swallowed, until it is let go; a new game forgets it too.
    in.set(resolve(kDefs, {{"jump", {"mouse:left"}}}, {}, nullptr));
    const SDL_Event click = mouse(SDL_BUTTON_LEFT, true);
    in.handle_event(click);
    in.swallow(click);
    CHECK_FALSE(in.pressed("jump"));
    CHECK_FALSE(in.down("jump"));
    in.handle_event(mouse(SDL_BUTTON_LEFT, false));
    in.end_frame();
    in.handle_event(click);
    CHECK(in.down("jump"));
    in.swallow(click);
    CHECK_FALSE(in.down("jump"));
    in.release_all();
    in.handle_event(click);
    CHECK(in.down("jump"));
    // What presses nothing swallows nothing.
    in.swallow(key(SDL_SCANCODE_K, false));
    CHECK(in.down("jump"));
}

TEST_CASE("input: a swallowed gamepad button and stick (SDL's virtual pad)") {
    REQUIRE(SDL_InitSubSystem(SDL_INIT_GAMEPAD));
    {
        Input in;
        in.set(resolve(kDefs, {}, {}, nullptr));
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = "Forge test pad";
        const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
        REQUIRE(id != 0);
        SDL_Joystick* joy = SDL_OpenJoystick(id);
        REQUIRE(joy);
        // The events SDL makes; the shell swallows the ones `eat` says.
        const auto pump = [&](bool (*eat)(const SDL_Event&)) {
            SDL_UpdateJoysticks();
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                in.handle_event(e);
                if (eat && eat(e)) in.swallow(e);
            }
        };
        pump(nullptr);
        REQUIRE(in.gamepads() == 1);
        const auto south = [](const SDL_Event& e) { return e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN; };
        SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
        pump(south);
        CHECK_FALSE(in.pressed("jump"));
        CHECK_FALSE(in.down("jump"));
        in.end_frame();
        SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, false);
        pump(nullptr);
        SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
        pump(nullptr);
        CHECK(in.pressed("jump"));
        CHECK(in.down("jump"));
        SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, false);
        pump(nullptr);
        in.end_frame();
        // The stick past half way, swallowed: not held while it stays there, held again after it came back.
        const auto stick = [](const SDL_Event& e) { return e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && e.gaxis.value < -16384; };
        SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, -30000);
        pump(stick);
        CHECK_FALSE(in.pressed("left"));
        CHECK_FALSE(in.down("left"));
        in.end_frame();
        SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, -31000);
        pump(nullptr);
        CHECK_FALSE(in.down("left"));
        SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, 0);
        pump(nullptr);
        SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, -30000);
        pump(nullptr);
        CHECK(in.pressed("left"));
        CHECK(in.down("left"));
        SDL_CloseJoystick(joy);
        SDL_DetachVirtualJoystick(id);
        pump(nullptr);
    }
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}
