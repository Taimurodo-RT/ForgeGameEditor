#include "forge/input/input.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <yyjson.h>

#include <algorithm>
#include <cstdlib>

FORGE_REFLECT(forge::input::Choice, 1) {
    t.field("action", &forge::input::Choice::action);
    t.field("binds", &forge::input::Choice::binds);
}

namespace forge::input {

namespace {

// A stick or a trigger counts as pressed past half way.
constexpr i32 kHalf = 16384;

struct Named {
    const char* text;
    i32 code;
    const char* words;
};

constexpr Named kMouse[] = {
    {"left", SDL_BUTTON_LEFT, "Левая кнопка мыши"},     {"right", SDL_BUTTON_RIGHT, "Правая кнопка мыши"},
    {"middle", SDL_BUTTON_MIDDLE, "Средняя кнопка мыши"}, {"x1", SDL_BUTTON_X1, "Кнопка мыши 4"},
    {"x2", SDL_BUTTON_X2, "Кнопка мыши 5"},
};

constexpr Named kPadButtons[] = {
    {"south", SDL_GAMEPAD_BUTTON_SOUTH, "Геймпад: A (нижняя)"},
    {"east", SDL_GAMEPAD_BUTTON_EAST, "Геймпад: B (правая)"},
    {"west", SDL_GAMEPAD_BUTTON_WEST, "Геймпад: X (левая)"},
    {"north", SDL_GAMEPAD_BUTTON_NORTH, "Геймпад: Y (верхняя)"},
    {"back", SDL_GAMEPAD_BUTTON_BACK, "Геймпад: Back"},
    {"guide", SDL_GAMEPAD_BUTTON_GUIDE, "Геймпад: Guide"},
    {"start", SDL_GAMEPAD_BUTTON_START, "Геймпад: Start"},
    {"ls", SDL_GAMEPAD_BUTTON_LEFT_STICK, "Геймпад: левый стик (нажатие)"},
    {"rs", SDL_GAMEPAD_BUTTON_RIGHT_STICK, "Геймпад: правый стик (нажатие)"},
    {"lb", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, "Геймпад: левый бампер"},
    {"rb", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "Геймпад: правый бампер"},
    {"dpup", SDL_GAMEPAD_BUTTON_DPAD_UP, "Геймпад: крестовина ↑"},
    {"dpdown", SDL_GAMEPAD_BUTTON_DPAD_DOWN, "Геймпад: крестовина ↓"},
    {"dpleft", SDL_GAMEPAD_BUTTON_DPAD_LEFT, "Геймпад: крестовина ←"},
    {"dpright", SDL_GAMEPAD_BUTTON_DPAD_RIGHT, "Геймпад: крестовина →"},
};

struct NamedAxis {
    const char* text;
    i32 axis;
    i8 dir;
    const char* words;
};

constexpr NamedAxis kPadAxes[] = {
    {"leftx-", SDL_GAMEPAD_AXIS_LEFTX, -1, "Геймпад: левый стик ←"},
    {"leftx+", SDL_GAMEPAD_AXIS_LEFTX, 1, "Геймпад: левый стик →"},
    {"lefty-", SDL_GAMEPAD_AXIS_LEFTY, -1, "Геймпад: левый стик ↑"},
    {"lefty+", SDL_GAMEPAD_AXIS_LEFTY, 1, "Геймпад: левый стик ↓"},
    {"rightx-", SDL_GAMEPAD_AXIS_RIGHTX, -1, "Геймпад: правый стик ←"},
    {"rightx+", SDL_GAMEPAD_AXIS_RIGHTX, 1, "Геймпад: правый стик →"},
    {"righty-", SDL_GAMEPAD_AXIS_RIGHTY, -1, "Геймпад: правый стик ↑"},
    {"righty+", SDL_GAMEPAD_AXIS_RIGHTY, 1, "Геймпад: правый стик ↓"},
    {"lt", SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1, "Геймпад: левый курок"},
    {"rt", SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1, "Геймпад: правый курок"},
};

// Keys whose SDL names are not what a player of a Russian game calls them.
constexpr Named kKeyWords[] = {
    {"Space", SDL_SCANCODE_SPACE, "Пробел"},          {"Return", SDL_SCANCODE_RETURN, "Enter"},
    {"Escape", SDL_SCANCODE_ESCAPE, "Esc"},           {"Left", SDL_SCANCODE_LEFT, "←"},
    {"Right", SDL_SCANCODE_RIGHT, "→"},               {"Up", SDL_SCANCODE_UP, "↑"},
    {"Down", SDL_SCANCODE_DOWN, "↓"},                 {"Left Shift", SDL_SCANCODE_LSHIFT, "Левый Shift"},
    {"Right Shift", SDL_SCANCODE_RSHIFT, "Правый Shift"}, {"Left Ctrl", SDL_SCANCODE_LCTRL, "Левый Ctrl"},
    {"Right Ctrl", SDL_SCANCODE_RCTRL, "Правый Ctrl"},  {"Left Alt", SDL_SCANCODE_LALT, "Левый Alt"},
    {"Right Alt", SDL_SCANCODE_RALT, "Правый Alt"},    {"Keypad Enter", SDL_SCANCODE_KP_ENTER, "Enter (цифровой блок)"},
};

bool pad_button_down(u32 buttons, i32 b) { return b >= 0 && b < 32 && (buttons >> b & 1u); }

// The key's place. An event made by a program (a self-test) may name only the key: its place is found from it.
i32 scancode_of(const SDL_KeyboardEvent& k) {
    if (k.scancode != SDL_SCANCODE_UNKNOWN || k.key == SDLK_UNKNOWN) return k.scancode;
    return SDL_GetScancodeFromKey(k.key, nullptr);
}

std::string json_text(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) < 0x20) continue;
        out += c;
    }
    return out + "\"";
}

std::vector<Bind> parse_all(const std::vector<std::string>& texts, std::string_view who, std::string_view action,
                            std::vector<std::string>* warnings) {
    std::vector<Bind> out;
    for (const std::string& t : texts) {
        Bind b;
        if (!parse(t, b)) {
            if (warnings) warnings->push_back(std::string(who) + ": у «" + std::string(action) + "» привязка «" + t + "» ничего не называет");
            continue;
        }
        if (std::find(out.begin(), out.end(), b) == out.end()) out.push_back(b);
    }
    return out;
}

} // namespace

std::vector<Bind> mouse_and_pad_binds() {
    std::vector<Bind> out;
    for (const Named& n : kMouse) out.push_back({Device::Mouse, n.code, 0});
    for (const Named& n : kPadButtons) out.push_back({Device::Pad, n.code, 0});
    for (const NamedAxis& n : kPadAxes) out.push_back({Device::PadAxis, n.axis, n.dir});
    return out;
}

bool parse(std::string_view text, Bind& out) {
    const usize colon = text.find(':');
    if (colon == std::string_view::npos) return false;
    const std::string_view kind = text.substr(0, colon);
    const std::string name(text.substr(colon + 1));
    if (name.empty()) return false;
    if (kind == "key") {
        const SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
        if (sc == SDL_SCANCODE_UNKNOWN) return false;
        out = {Device::Key, static_cast<i32>(sc), 0};
        return true;
    }
    if (kind == "mouse") {
        for (const Named& n : kMouse)
            if (name == n.text) {
                out = {Device::Mouse, n.code, 0};
                return true;
            }
        return false;
    }
    if (kind == "pad") {
        for (const Named& n : kPadButtons)
            if (name == n.text) {
                out = {Device::Pad, n.code, 0};
                return true;
            }
        for (const NamedAxis& n : kPadAxes)
            if (name == n.text) {
                out = {Device::PadAxis, n.axis, n.dir};
                return true;
            }
    }
    return false;
}

std::string format(const Bind& b) {
    switch (b.device) {
    case Device::Key: {
        const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(b.code));
        return name && *name ? std::string("key:") + name : std::string();
    }
    case Device::Mouse:
        for (const Named& n : kMouse)
            if (n.code == b.code) return std::string("mouse:") + n.text;
        return {};
    case Device::Pad:
        for (const Named& n : kPadButtons)
            if (n.code == b.code) return std::string("pad:") + n.text;
        return {};
    case Device::PadAxis:
        for (const NamedAxis& n : kPadAxes)
            if (n.axis == b.code && n.dir == b.dir) return std::string("pad:") + n.text;
        return {};
    }
    return {};
}

std::string words(const Bind& b) {
    switch (b.device) {
    case Device::Key: {
        for (const Named& n : kKeyWords)
            if (n.code == b.code) return n.words;
        const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(b.code));
        return name && *name ? name : "?";
    }
    case Device::Mouse:
        for (const Named& n : kMouse)
            if (n.code == b.code) return n.words;
        break;
    case Device::Pad:
        for (const Named& n : kPadButtons)
            if (n.code == b.code) return n.words;
        break;
    case Device::PadAxis:
        for (const NamedAxis& n : kPadAxes)
            if (n.axis == b.code && n.dir == b.dir) return n.words;
        break;
    }
    return "?";
}

const std::vector<ActionDef>& shell_actions() {
    static const std::vector<ActionDef> defs = {
        {"pause", "Пауза", {"key:Escape", "pad:start"}},
        {"journal", "Журнал", {"key:J", "pad:back"}},
        {"quicksave", "Быстрое сохранение", {"key:F5"}},
        {"quickload", "Быстрая загрузка", {"key:F9"}},
    };
    return defs;
}

bool read_choices(std::string_view json, Choices& out, std::string* error, std::vector<std::string>* warnings) {
    out.clear();
    yyjson_doc* doc = yyjson_read(json.data(), json.size(), 0);
    yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
    yyjson_val* list = root ? yyjson_obj_get(root, "actions") : nullptr;
    if (!yyjson_is_arr(list)) {
        if (error) *error = doc ? "нет списка «actions»" : "это не JSON";
        if (doc) yyjson_doc_free(doc);
        return false;
    }
    usize i, n;
    yyjson_val* item;
    yyjson_arr_foreach(list, i, n, item) {
        yyjson_val* id = yyjson_obj_get(item, "id");
        yyjson_val* binds = yyjson_obj_get(item, "binds");
        if (!yyjson_is_str(id) || !yyjson_is_arr(binds)) {
            if (warnings) warnings->push_back("действие " + std::to_string(i + 1) + ": нужны «id» и список «binds»");
            continue;
        }
        Choice c;
        c.action = yyjson_get_str(id);
        usize j, m;
        yyjson_val* b;
        yyjson_arr_foreach(binds, j, m, b) {
            if (yyjson_is_str(b)) c.binds.emplace_back(yyjson_get_str(b));
        }
        if (find(out, c.action)) {
            if (warnings) warnings->push_back("действие «" + c.action + "» дважды: берётся первое");
            continue;
        }
        out.push_back(std::move(c));
    }
    yyjson_doc_free(doc);
    return true;
}

std::string write_choices(const Choices& choices) {
    std::string out = "{\n  \"actions\": [";
    for (usize i = 0; i < choices.size(); ++i) {
        out += i ? ",\n    " : "\n    ";
        out += "{\"id\": " + json_text(choices[i].action) + ", \"binds\": [";
        for (usize j = 0; j < choices[i].binds.size(); ++j) out += (j ? ", " : "") + json_text(choices[i].binds[j]);
        out += "]}";
    }
    out += choices.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

const Choice* find(const Choices& choices, std::string_view action) {
    for (const Choice& c : choices)
        if (c.action == action) return &c;
    return nullptr;
}

void choose(Choices& choices, std::string_view action, std::vector<std::string> binds, const std::vector<std::string>& base) {
    auto it = std::find_if(choices.begin(), choices.end(), [&](const Choice& c) { return c.action == action; });
    if (binds == base) {
        if (it != choices.end()) choices.erase(it);
        return;
    }
    if (it != choices.end()) it->binds = std::move(binds);
    else choices.push_back({std::string(action), std::move(binds)});
}

std::vector<Action> resolve(const std::vector<ActionDef>& defs, const Choices& author, const Choices& player,
                            std::vector<std::string>* warnings) {
    std::vector<Action> out;
    for (const ActionDef& d : defs) {
        Action a{d.id, d.words, parse_all(d.defaults, "клавиши модуля", d.words, warnings)};
        if (const Choice* c = find(author, d.id)) a.binds = parse_all(c->binds, "клавиши автора", d.words, warnings);
        if (const Choice* c = find(player, d.id)) a.binds = parse_all(c->binds, "клавиши игрока", d.words, warnings);
        out.push_back(std::move(a));
    }
    const auto known = [&](const std::string& id) {
        return std::any_of(defs.begin(), defs.end(), [&](const ActionDef& d) { return d.id == id; });
    };
    if (warnings) {
        for (const Choice& c : author)
            if (!known(c.action)) warnings->push_back("клавиши автора: действия «" + c.action + "» нет в этой игре");
        for (const Choice& c : player)
            if (!known(c.action)) warnings->push_back("клавиши игрока: действия «" + c.action + "» нет в этой игре");
    }
    return out;
}

std::vector<Clash> clashes(const std::vector<Action>& actions) {
    std::vector<Clash> out;
    for (usize i = 0; i < actions.size(); ++i)
        for (usize j = i + 1; j < actions.size(); ++j)
            for (const Bind& b : actions[i].binds)
                if (std::find(actions[j].binds.begin(), actions[j].binds.end(), b) != actions[j].binds.end())
                    out.push_back({b, actions[i].words, actions[j].words});
    return out;
}

std::string clash_words(const Clash& c) { return words(c.bind) + " — у «" + c.a + "» и «" + c.b + "»"; }

std::optional<Bind> bind_of(const SDL_Event& e) {
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN: {
        const i32 sc = scancode_of(e.key);
        if (e.key.repeat || sc == SDL_SCANCODE_UNKNOWN) return std::nullopt;
        return Bind{Device::Key, sc, 0};
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: return Bind{Device::Mouse, e.button.button, 0};
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: return Bind{Device::Pad, e.gbutton.button, 0};
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (std::abs(static_cast<i32>(e.gaxis.value)) < kHalf) return std::nullopt;
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
            return e.gaxis.value > 0 ? std::optional<Bind>(Bind{Device::PadAxis, e.gaxis.axis, 1}) : std::nullopt;
        return Bind{Device::PadAxis, e.gaxis.axis, static_cast<i8>(e.gaxis.value < 0 ? -1 : 1)};
    default: return std::nullopt;
    }
}

// --- Input ---------------------------------------------------------------------

Input::~Input() {
    if (SDL_WasInit(SDL_INIT_GAMEPAD))
        for (auto& [id, p] : pads_) SDL_CloseGamepad(p.pad);
}

void Input::set(std::vector<Action> actions) { actions_ = std::move(actions); }

const Action* Input::action(std::string_view id) const {
    for (const Action& a : actions_)
        if (a.id == id) return &a;
    return nullptr;
}

void Input::handle_event(const SDL_Event& e) {
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN: {
        const i32 sc = scancode_of(e.key);
        if (e.key.repeat || sc == SDL_SCANCODE_UNKNOWN) break;
        if (std::find(keys_.begin(), keys_.end(), sc) == keys_.end()) keys_.push_back(sc);
        went_down_.push_back({Device::Key, sc, 0});
        break;
    }
    case SDL_EVENT_KEY_UP: std::erase(keys_, scancode_of(e.key)); break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e.button.button < 32) mouse_ |= 1u << e.button.button;
        went_down_.push_back({Device::Mouse, e.button.button, 0});
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e.button.button < 32) mouse_ &= ~(1u << e.button.button);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        keys_.clear();
        mouse_ = 0;
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!pads_.count(e.gdevice.which))
            if (SDL_Gamepad* g = SDL_OpenGamepad(e.gdevice.which)) pads_[e.gdevice.which].pad = g;
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (auto it = pads_.find(e.gdevice.which); it != pads_.end()) {
            SDL_CloseGamepad(it->second.pad);
            pads_.erase(it);
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (auto it = pads_.find(e.gbutton.which); it != pads_.end() && e.gbutton.button < 32) {
            const u32 bit = 1u << e.gbutton.button;
            if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                it->second.buttons |= bit;
                went_down_.push_back({Device::Pad, e.gbutton.button, 0});
            } else it->second.buttons &= ~bit;
        }
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (auto it = pads_.find(e.gaxis.which); it != pads_.end() && e.gaxis.axis < 8) {
            const i32 before = it->second.axes[e.gaxis.axis], now = e.gaxis.value;
            it->second.axes[e.gaxis.axis] = e.gaxis.value;
            for (i8 dir : {i8(-1), i8(1)})
                if (before * dir < kHalf && now * dir >= kHalf) went_down_.push_back({Device::PadAxis, e.gaxis.axis, dir});
        }
        break;
    default: break;
    }
}

bool Input::presses(const SDL_Event& e, std::string_view id) const {
    const Action* a = action(id);
    if (!a) return false;
    std::optional<Bind> b = bind_of(e);
    if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) // only crossing half way presses, as handle_event saw it
        b = !went_down_.empty() && went_down_.back().device == Device::PadAxis && went_down_.back().code == e.gaxis.axis
                ? std::optional<Bind>(went_down_.back())
                : std::nullopt;
    return b && std::find(a->binds.begin(), a->binds.end(), *b) != a->binds.end();
}

bool Input::held(const Bind& b) const {
    switch (b.device) {
    case Device::Key: return std::find(keys_.begin(), keys_.end(), b.code) != keys_.end();
    case Device::Mouse: return b.code >= 0 && b.code < 32 && (mouse_ >> b.code & 1u);
    case Device::Pad:
        for (const auto& [id, p] : pads_)
            if (pad_button_down(p.buttons, b.code)) return true;
        return false;
    case Device::PadAxis:
        if (b.code < 0 || b.code >= 8) return false;
        for (const auto& [id, p] : pads_)
            if (p.axes[b.code] * b.dir >= kHalf) return true;
        return false;
    }
    return false;
}

bool Input::down(std::string_view id) const {
    const Action* a = action(id);
    return a && std::any_of(a->binds.begin(), a->binds.end(), [&](const Bind& b) { return held(b); });
}

bool Input::pressed(std::string_view id) const {
    const Action* a = action(id);
    if (!a) return false;
    for (const Bind& b : went_down_)
        if (std::find(a->binds.begin(), a->binds.end(), b) != a->binds.end()) return true;
    return false;
}

void Input::end_frame() { went_down_.clear(); }

void Input::release_all() {
    keys_.clear();
    mouse_ = 0;
    for (auto& [id, p] : pads_) {
        p.buttons = 0;
        std::fill(std::begin(p.axes), std::end(p.axes), i16(0));
    }
    went_down_.clear();
}

std::string Input::first_words(std::string_view id) const {
    const Action* a = action(id);
    if (!a) return {};
    for (const Bind& b : a->binds)
        if (!is_pad(b)) return words(b);
    return a->binds.empty() ? std::string() : words(a->binds.front());
}

} // namespace forge::input
