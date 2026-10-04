#pragma once

// The UI engine shared by the editor and by game menus.
//
// Everything visible lives in files under one UI folder:
//   <root>/forge-ui/tokens.json   design tokens (colours of every theme, sizes, fonts)
//   <root>/forge-ui/*.rcss        Forge UI components
//   <root>/fonts/fonts.json       font files and their weights
//   <root>/...                    documents (.rml) and their styles (.rcss)
// Styles refer to tokens as var(--name); the active theme decides the value.
// Files are watched: saving one reloads the documents that use it.

#include "forge/core/types.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gpu.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct SDL_Window;

namespace Rml {
class Context;
class ElementDocument;
} // namespace Rml

namespace forge::ui {

class Tokens;

struct UiConfig {
    std::filesystem::path root; // the UI folder
    std::string theme = "dark";
    u32 msaa = 2;              // edge smoothing, falls back to what the GPU supports
    bool hot_reload = true;    // watch the folder for changes
    f32 dp_ratio = 1.0f;       // UI scale (1 = 100%)
};

// What happened during the last update(): for status lines and tests.
struct UiStats {
    u32 draws = 0;
    u32 passes = 0;
    u32 uploads = 0;
    u32 reloads = 0; // documents reloaded by hot reload, in total
    f64 update_ms = 0;
    f64 render_ms = 0; // CPU time to record and submit
};

class Ui {
public:
    Ui();
    ~Ui();
    Ui(const Ui&) = delete;
    Ui& operator=(const Ui&) = delete;

    // window may be null (offscreen rendering).
    bool init(SDL_GPUDevice* device, SDL_Window* window, const UiConfig& config);
    void shutdown();

    // One context per independent UI surface (the editor, a game's menus).
    Rml::Context* create_context(const std::string& name, u32 width, u32 height);
    // path is relative to the UI folder. The document is shown.
    Rml::ElementDocument* load_document(Rml::Context* context, const std::string& path);

    // Feeds an SDL event to the context. True when the UI used it (the mouse
    // is over UI, a text field has focus), so the game or editor should not.
    bool handle_event(Rml::Context* context, const SDL_Event& event);

    // Hot reload and context updates; call once per frame.
    void update();
    // Draws every context and blends it over target.
    void render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, SDL_GPUTextureFormat format, u32 width, u32 height);

    // Themes from tokens.json: "dark", "light", "fantasy", "parchment"...
    std::vector<std::string> themes() const;
    const std::string& theme() const;
    // Restyles every document; the documents keep their state.
    bool set_theme(const std::string& theme);
    // Reloads every document from its files (what hot reload does).
    void reload_documents();

    const Tokens& tokens() const;
    const std::filesystem::path& root() const;
    const UiStats& stats() const;
    u32 msaa_samples() const;

    // Called after documents were reloaded, so the app can re-attach
    // listeners to the new elements.
    void on_reload(std::function<void()> callback);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace forge::ui
