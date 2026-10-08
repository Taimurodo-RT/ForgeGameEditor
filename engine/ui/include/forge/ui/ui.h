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
    bool debugger = true;      // F8 opens RmlUi's inspector; a beacon shows UI warnings
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

    // Draws the context into a texture of its own size instead of over the
    // window; documents show it as <img src="/gpu/<image>"/> (the editor's
    // view of a game screen). An empty image name draws it over the window
    // again. Offscreen contexts are drawn before the others.
    void set_offscreen(Rml::Context* context, const std::string& image);
    // One pixel of what an offscreen context drew last (RGBA, alpha
    // premultiplied as drawn); false without such a picture or outside it.
    // Waits for the GPU: for a click (the colour picker's eyedropper), not
    // for every frame.
    bool read_pixel(Rml::Context* context, u32 x, u32 y, u8 rgba[4]);
    // A context that is not active is neither updated nor drawn.
    void set_active(Rml::Context* context, bool active);
    // Removes a context made by create_context with its documents.
    void destroy_context(Rml::Context* context);

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

    // A picture made by code (palette icons, previews), shown by documents as
    // <img src="/memory/<name>"/>. Pixels are RGBA, rows top to bottom.
    // Give a changed picture a new name: shown pictures are cached by name.
    void set_image(const std::string& name, const u8* rgba, u32 width, u32 height);
    // Forgets a picture made by set_image (after no element shows it).
    void drop_image(const std::string& name);

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
