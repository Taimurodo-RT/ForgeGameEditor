#include "interfaces.h"

#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/core/time.h"

#include <RmlUi/Core/Context.h>
#include <SDL3/SDL.h>

#include <cstring>

namespace forge::ui {

namespace {

struct OpenFile {
    std::string data;
    size_t pos = 0;
};

bool ends_with(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

const u64 g_start_ns = time_now_ns();

} // namespace

// ---------------------------------------------------------------------------
// Files

std::filesystem::path FileInterface::resolve(const std::string& path) const {
    std::filesystem::path p = utf8_path(path);
    if (p.is_absolute()) return p;
    return root_ / p;
}

bool FileInterface::read(const std::string& path, std::string& out) {
    const std::filesystem::path full = resolve(path);
    std::vector<u8> bytes;
    if (!read_file(full, bytes)) return false;
    out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());

    std::error_code ec;
    const auto time = std::filesystem::last_write_time(full, ec);
    {
        std::lock_guard lock(mutex_);
        watched_[path_to_utf8(full)] = time;
    }

    if (tokens_ && (ends_with(path, ".rcss") || ends_with(path, ".rml"))) {
        std::vector<std::string> missing;
        out = tokens_->substitute(out, &missing);
        for (const std::string& name : missing) FORGE_WARN("ui: %s uses unknown token --%s", path.c_str(), name.c_str());
    }
    return true;
}

Rml::FileHandle FileInterface::Open(const Rml::String& path) {
    auto* file = new OpenFile;
    if (!read(path, file->data)) {
        delete file;
        return {};
    }
    return reinterpret_cast<Rml::FileHandle>(file);
}

void FileInterface::Close(Rml::FileHandle file) { delete reinterpret_cast<OpenFile*>(file); }

size_t FileInterface::Read(void* buffer, size_t size, Rml::FileHandle handle) {
    auto* file = reinterpret_cast<OpenFile*>(handle);
    const size_t n = std::min(size, file->data.size() - file->pos);
    std::memcpy(buffer, file->data.data() + file->pos, n);
    file->pos += n;
    return n;
}

bool FileInterface::Seek(Rml::FileHandle handle, long offset, int origin) {
    auto* file = reinterpret_cast<OpenFile*>(handle);
    long base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? static_cast<long>(file->pos)
                                                            : static_cast<long>(file->data.size());
    const long pos = base + offset;
    if (pos < 0 || pos > static_cast<long>(file->data.size())) return false;
    file->pos = static_cast<size_t>(pos);
    return true;
}

size_t FileInterface::Tell(Rml::FileHandle handle) { return reinterpret_cast<OpenFile*>(handle)->pos; }

size_t FileInterface::Length(Rml::FileHandle handle) { return reinterpret_cast<OpenFile*>(handle)->data.size(); }

bool FileInterface::LoadFile(const Rml::String& path, Rml::String& out_data) { return read(path, out_data); }

std::unordered_map<std::string, std::filesystem::file_time_type> FileInterface::watched() const {
    std::lock_guard lock(mutex_);
    return watched_;
}

// ---------------------------------------------------------------------------
// System

double SystemInterface::GetElapsedTime() { return static_cast<double>(time_now_ns() - g_start_ns) / 1e9; }

bool SystemInterface::LogMessage(Rml::Log::Type type, const Rml::String& message) {
    switch (type) {
    case Rml::Log::LT_ERROR:
    case Rml::Log::LT_ASSERT: FORGE_ERROR("ui: %s", message.c_str()); break;
    case Rml::Log::LT_WARNING: FORGE_WARN("ui: %s", message.c_str()); break;
    case Rml::Log::LT_INFO: FORGE_TRACE("ui: %s", message.c_str()); break; // font loads and the like
    default: break;
    }
    return true;
}

void SystemInterface::SetMouseCursor(const Rml::String& name) {
    if (!window_) return;
    auto it = cursors_.find(name);
    if (it == cursors_.end()) {
        SDL_SystemCursor id = SDL_SYSTEM_CURSOR_DEFAULT;
        if (name == "pointer") id = SDL_SYSTEM_CURSOR_POINTER;
        else if (name == "text") id = SDL_SYSTEM_CURSOR_TEXT;
        else if (name == "move") id = SDL_SYSTEM_CURSOR_MOVE;
        else if (name == "crosshair") id = SDL_SYSTEM_CURSOR_CROSSHAIR;
        else if (name == "wait" || name == "progress") id = SDL_SYSTEM_CURSOR_PROGRESS;
        else if (name == "not-allowed") id = SDL_SYSTEM_CURSOR_NOT_ALLOWED;
        else if (name == "ew-resize" || name == "col-resize") id = SDL_SYSTEM_CURSOR_EW_RESIZE;
        else if (name == "ns-resize" || name == "row-resize") id = SDL_SYSTEM_CURSOR_NS_RESIZE;
        else if (name == "nwse-resize") id = SDL_SYSTEM_CURSOR_NWSE_RESIZE;
        else if (name == "nesw-resize") id = SDL_SYSTEM_CURSOR_NESW_RESIZE;
        it = cursors_.emplace(name, SDL_CreateSystemCursor(id)).first;
    }
    if (it->second) SDL_SetCursor(it->second);
}

void SystemInterface::destroy_cursors() {
    for (auto& [name, cursor] : cursors_)
        if (cursor) SDL_DestroyCursor(cursor);
    cursors_.clear();
}

void SystemInterface::SetClipboardText(const Rml::String& text) { SDL_SetClipboardText(text.c_str()); }

void SystemInterface::GetClipboardText(Rml::String& text) {
    char* s = SDL_GetClipboardText();
    text = s ? s : "";
    SDL_free(s);
}

void SystemInterface::ActivateKeyboard(Rml::Vector2f caret, float line_height) {
    if (!window_) return;
    const float density = SDL_GetWindowPixelDensity(window_);
    const SDL_Rect rect{static_cast<int>(caret.x / density), static_cast<int>(caret.y / density), 1,
                        static_cast<int>(line_height / density)};
    SDL_SetTextInputArea(window_, &rect, 0);
    SDL_StartTextInput(window_);
}

void SystemInterface::DeactivateKeyboard() {
    if (window_) SDL_StopTextInput(window_);
}

// ---------------------------------------------------------------------------
// Input

int key_modifiers() {
    const SDL_Keymod mods = SDL_GetModState();
    int out = 0;
    if (mods & SDL_KMOD_CTRL) out |= Rml::Input::KM_CTRL;
    if (mods & SDL_KMOD_SHIFT) out |= Rml::Input::KM_SHIFT;
    if (mods & SDL_KMOD_ALT) out |= Rml::Input::KM_ALT;
    if (mods & SDL_KMOD_GUI) out |= Rml::Input::KM_META;
    if (mods & SDL_KMOD_NUM) out |= Rml::Input::KM_NUMLOCK;
    if (mods & SDL_KMOD_CAPS) out |= Rml::Input::KM_CAPSLOCK;
    return out;
}

Rml::Input::KeyIdentifier convert_key(SDL_Keycode key) {
    using namespace Rml::Input;
    if (key >= SDLK_A && key <= SDLK_Z) return static_cast<KeyIdentifier>(KI_A + (key - SDLK_A));
    if (key >= SDLK_0 && key <= SDLK_9) return static_cast<KeyIdentifier>(KI_0 + (key - SDLK_0));
    if (key >= SDLK_F1 && key <= SDLK_F12) return static_cast<KeyIdentifier>(KI_F1 + (key - SDLK_F1));
    if (key >= SDLK_KP_1 && key <= SDLK_KP_9) return static_cast<KeyIdentifier>(KI_NUMPAD1 + (key - SDLK_KP_1));
    switch (key) {
    case SDLK_ESCAPE: return KI_ESCAPE;
    case SDLK_SPACE: return KI_SPACE;
    case SDLK_SEMICOLON: return KI_OEM_1;
    case SDLK_PLUS:
    case SDLK_EQUALS: return KI_OEM_PLUS;
    case SDLK_COMMA: return KI_OEM_COMMA;
    case SDLK_MINUS: return KI_OEM_MINUS;
    case SDLK_PERIOD: return KI_OEM_PERIOD;
    case SDLK_SLASH: return KI_OEM_2;
    case SDLK_GRAVE: return KI_OEM_3;
    case SDLK_LEFTBRACKET: return KI_OEM_4;
    case SDLK_BACKSLASH: return KI_OEM_5;
    case SDLK_RIGHTBRACKET: return KI_OEM_6;
    case SDLK_APOSTROPHE:
    case SDLK_DBLAPOSTROPHE: return KI_OEM_7;
    case SDLK_KP_0: return KI_NUMPAD0;
    case SDLK_KP_ENTER: return KI_NUMPADENTER;
    case SDLK_KP_MULTIPLY: return KI_MULTIPLY;
    case SDLK_KP_PLUS: return KI_ADD;
    case SDLK_KP_MINUS: return KI_SUBTRACT;
    case SDLK_KP_PERIOD: return KI_DECIMAL;
    case SDLK_KP_DIVIDE: return KI_DIVIDE;
    case SDLK_BACKSPACE: return KI_BACK;
    case SDLK_TAB: return KI_TAB;
    case SDLK_RETURN: return KI_RETURN;
    case SDLK_PAUSE: return KI_PAUSE;
    case SDLK_CAPSLOCK: return KI_CAPITAL;
    case SDLK_PAGEUP: return KI_PRIOR;
    case SDLK_PAGEDOWN: return KI_NEXT;
    case SDLK_END: return KI_END;
    case SDLK_HOME: return KI_HOME;
    case SDLK_LEFT: return KI_LEFT;
    case SDLK_UP: return KI_UP;
    case SDLK_RIGHT: return KI_RIGHT;
    case SDLK_DOWN: return KI_DOWN;
    case SDLK_INSERT: return KI_INSERT;
    case SDLK_DELETE: return KI_DELETE;
    case SDLK_LSHIFT: return KI_LSHIFT;
    case SDLK_RSHIFT: return KI_RSHIFT;
    case SDLK_LCTRL: return KI_LCONTROL;
    case SDLK_RCTRL: return KI_RCONTROL;
    case SDLK_LALT: return KI_LMENU;
    case SDLK_RALT: return KI_RMENU;
    case SDLK_LGUI: return KI_LMETA;
    case SDLK_RGUI: return KI_RMETA;
    default: return KI_UNKNOWN;
    }
}

bool process_event(Rml::Context* context, SDL_Window* window, const SDL_Event& ev) {
    // RmlUi returns true when the event was NOT used by any element.
    bool unused = true;
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    switch (ev.type) {
    case SDL_EVENT_MOUSE_MOTION:
        unused = context->ProcessMouseMove(static_cast<int>(ev.motion.x * density),
                                           static_cast<int>(ev.motion.y * density), key_modifiers());
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        const int button = ev.button.button == SDL_BUTTON_LEFT    ? 0
                           : ev.button.button == SDL_BUTTON_RIGHT ? 1
                           : ev.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                   : 3;
        unused = context->ProcessMouseButtonDown(button, key_modifiers());
        SDL_CaptureMouse(true);
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        SDL_CaptureMouse(false);
        const int button = ev.button.button == SDL_BUTTON_LEFT    ? 0
                           : ev.button.button == SDL_BUTTON_RIGHT ? 1
                           : ev.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                   : 3;
        unused = context->ProcessMouseButtonUp(button, key_modifiers());
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        unused = context->ProcessMouseWheel(Rml::Vector2f(-ev.wheel.x, -ev.wheel.y), key_modifiers());
        break;
    case SDL_EVENT_KEY_DOWN:
        unused = context->ProcessKeyDown(convert_key(ev.key.key), key_modifiers());
        if (ev.key.key == SDLK_RETURN || ev.key.key == SDLK_KP_ENTER) unused &= context->ProcessTextInput('\n');
        break;
    case SDL_EVENT_KEY_UP:
        unused = context->ProcessKeyUp(convert_key(ev.key.key), key_modifiers());
        break;
    case SDL_EVENT_TEXT_INPUT:
        unused = context->ProcessTextInput(Rml::String(ev.text.text));
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        context->SetDimensions({ev.window.data1, ev.window.data2});
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        context->ProcessMouseLeave();
        break;
    default: break;
    }
    return !unused;
}

} // namespace forge::ui
