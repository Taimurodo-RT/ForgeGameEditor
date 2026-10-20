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
#include <optional>
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
    // A px of a style is this many pixels of the window (the dp-ratio): the display's scale (the window's, from the
    // system) times scale. display_scale above 0 stands for the system's (tests, --ui-scale).
    f32 scale = 1.0f; // the game's «Масштаб интерфейса» (1 = 100%)
    f32 display_scale = 0.0f;
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
    // A w × h rectangle of it, rows top to bottom (the self-test compares pictures drawn at a scale with references).
    bool read_pixels(Rml::Context* context, u32 x, u32 y, u32 w, u32 h, std::vector<u8>& rgba);
    // A context's own clock (seconds), for showing a moment of its movements
    // (the editor's timeline): while it is set the context's animations see
    // this time, not the wall clock's. Moving it forward plays the way between
    // in steps, as RmlUi plays them (it moves at most 0.1 s at a time); moving
    // it back does not undo what played: load the document again first. nullopt
    // gives the context the wall clock again (load its documents again then).
    void set_clock(Rml::Context* context, std::optional<double> seconds);
    // Runs code that loads or shows documents of a context at its clock (RmlUi
    // starts a document's movements as it loads it): without the context's
    // clock set, just runs it.
    void at_clock(Rml::Context* context, const std::function<void()>& run);
    // Updates a context now (layout, its movements at its clock) outside update().
    void update_context(Rml::Context* context);
    // A context that is not active is neither updated nor drawn.
    void set_active(Rml::Context* context, bool active);
    // Removes a context made by create_context with its documents.
    void destroy_context(Rml::Context* context);

    // The scale of the UI: a px of a style is dp_ratio() pixels of the window, the display's scale times scale().
    // Contexts made by create_context follow it as it changes (the window moved to another display, the player's
    // choice), unless pin_dp_ratio gave them one of their own.
    f32 dp_ratio() const;
    f32 display_scale() const;
    f32 scale() const;
    void set_scale(f32 scale);
    // The display's scale taken instead of the system's (a test: the window moved to a display of another scale, which a
    // machine with one display cannot do). As the system's is, it is taken when the window says the display's scale
    // changed (SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED to handle_event), not before; 0: the system's again.
    void stand_in_display_scale(f32 scale);
    // A context drawn at a dp-ratio of its own (a page of the game shown at its design size).
    void pin_dp_ratio(Rml::Context* context, f32 dp_ratio);

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

    // Part 6г4 of docs/models/сцены-модель.md. The world drawn this frame, in a texture of the window's size the app
    // drew it into (the window's frame cannot be read): a <backdrop name="…"/> puts what the UI drew up to it over this
    // in /gpu/<name>, which the elements after it show; null (the default): black there.
    void set_world(SDL_GPUTexture* world);
    // A picture made by code straight into a texture (a scene's mask, made again as it changes), shown as
    // /gpu/<name>: RGBA with alpha premultiplied, rows top to bottom, w × h. A changed picture keeps its name (it goes
    // to the GPU with the next frame). drop_gpu_image forgets it, and a backdrop's picture too.
    void set_gpu_image(const std::string& name, const u8* rgba, u32 width, u32 height);
    void drop_gpu_image(const std::string& name);

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
