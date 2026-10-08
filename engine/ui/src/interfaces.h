#pragma once

// RmlUi's file, system and input glue.

#include "forge/core/types.h"
#include "forge/ui/tokens.h"

#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/SystemInterface.h>
#include <SDL3/SDL_events.h>

#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

struct SDL_Window;
struct SDL_Cursor;

namespace Rml {
class Context;
}

namespace forge::ui {

// Reads UI files relative to the UI folder. Styles and documents (.rcss,
// .rml) pass through the token substitution on the way, so var(--token)
// works everywhere, including style attributes. Every file read is
// remembered for hot reload.
class FileInterface final : public Rml::FileInterface {
public:
    void set_root(std::filesystem::path root) { root_ = std::move(root); }
    void set_tokens(const Tokens* tokens) { tokens_ = tokens; }

    Rml::FileHandle Open(const Rml::String& path) override;
    void Close(Rml::FileHandle file) override;
    size_t Read(void* buffer, size_t size, Rml::FileHandle file) override;
    bool Seek(Rml::FileHandle file, long offset, int origin) override;
    size_t Tell(Rml::FileHandle file) override;
    size_t Length(Rml::FileHandle file) override;
    bool LoadFile(const Rml::String& path, Rml::String& out_data) override;

    std::filesystem::path resolve(const std::string& path) const;
    // Files that live in memory (pictures made by code): a document refers
    // to "/memory/<name>", which reaches here as "memory/<name>".
    void set_memory_file(const std::string& name, std::string bytes);
    void drop_memory_file(const std::string& name);
    // Files read so far and their modification times when read.
    std::unordered_map<std::string, std::filesystem::file_time_type> watched() const;

private:
    bool read(const std::string& path, std::string& out);

    std::filesystem::path root_;
    const Tokens* tokens_ = nullptr;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::filesystem::file_time_type> watched_;
    std::unordered_map<std::string, std::string> memory_;
};

// The time RmlUi reads while a context with its own clock is updated (Ui::set_clock); unset, the wall clock.
void set_clock_now(const double* seconds);

class SystemInterface final : public Rml::SystemInterface {
public:
    void set_window(SDL_Window* window) { window_ = window; }
    void destroy_cursors();

    double GetElapsedTime() override;
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;
    void SetMouseCursor(const Rml::String& cursor_name) override;
    void SetClipboardText(const Rml::String& text) override;
    void GetClipboardText(Rml::String& text) override;
    void ActivateKeyboard(Rml::Vector2f caret_position, float line_height) override;
    void DeactivateKeyboard() override;

private:
    SDL_Window* window_ = nullptr;
    std::unordered_map<std::string, SDL_Cursor*> cursors_;
};

// SDL event -> context. True when the UI consumed it.
bool process_event(Rml::Context* context, SDL_Window* window, const SDL_Event& event);
Rml::Input::KeyIdentifier convert_key(SDL_Keycode key);
int key_modifiers();

} // namespace forge::ui
