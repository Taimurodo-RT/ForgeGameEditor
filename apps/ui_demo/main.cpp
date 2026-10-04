// UI demo: every Forge UI component in one window, drawn by the engine's
// own UI renderer from the files in ui/. Edit ui/demo/gallery.rml, any .rcss
// or ui/forge-ui/tokens.json while it runs: the window updates by itself.
//
//   forge_ui_demo [--ui DIR] [--theme dark|light|fantasy|parchment] [--scroll] [--no-vsync] [--msaa N]
//   forge_ui_demo --screenshot out.png [--theme NAME] [--frames N] [--scroll]   offscreen
//
// --scroll keeps scrolling the 50 000-row list, to measure it.
// F8 opens the element inspector.

#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/platform/app.h"
#include "forge/render/gpu.h"
#include "forge/render/offscreen.h"
#include "forge/core/jobs.h"
#include "forge/ui/ui.h"
#include "forge/ui/virtual_list.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#ifndef FORGE_UI_DIR
#define FORGE_UI_DIR "ui"
#endif

using namespace forge;

namespace {

// 50 000 rows that exist only as numbers: the list asks for the fields of the
// rows on screen.
class DemoRows final : public ui::ListSource {
public:
    static constexpr u32 kCount = 50'000;
    u32 count() const override { return kCount; }
    u64 version() const override { return version_; }
    std::string field(u32 row, std::string_view name) const override {
        static const char* icons[] = {"folder", "image", "lightbulb", "sensor_door", "person", "pets", "music_note", "texture"};
        static const char* kinds[] = {"Group", "Sprite", "Light", "Trigger", "Character", "Animal", "Sound", "Tilemap"};
        const u32 kind = row % 8 == 0 ? 0 : 1 + (row * 7) % 7;
        if (name == "name") return (kind == 0 ? "Группа " : "Объект ") + std::to_string(row + 1);
        if (name == "icon") return icons[kind];
        if (name == "kind") return kinds[kind];
        if (name == "indent") return std::to_string(kind == 0 ? 8 : 28) + "px";
        if (name == "selected") return row == selected_ ? "1" : "0";
        return {};
    }
    void on_row_event(u32 row, std::string_view event, int) override {
        if (event == "click") {
            selected_ = row;
            ++version_;
        }
    }

private:
    u32 selected_ = 2;
    u64 version_ = 1;
};

class Gallery {
public:
    bool init(SDL_GPUDevice* device, SDL_Window* window, u32 width, u32 height, const std::filesystem::path& ui_dir,
              const std::string& theme, u32 msaa) {
        ui::UiConfig config;
        config.root = ui_dir;
        config.theme = theme;
        config.msaa = msaa;
        config.hot_reload = window != nullptr;
        if (!ui_.init(device, window, config)) return false;
        context_ = ui_.create_context("gallery", width, height);
        if (!context_) return false;

        theme_ = ui_.theme();
        Rml::DataModelConstructor model = context_->CreateDataModel("gallery");
        model.Bind("theme", &theme_);
        model.Bind("clicks", &clicks_);
        model.Bind("volume", &volume_);
        model.Bind("music", &music_);
        model.Bind("quality", &quality_);
        model.Bind("name", &name_);
        model.Bind("health", &health_);
        model.Bind("rows", &rows_);
        model.BindEventCallback("set_theme", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (!args.empty()) pending_theme_ = args[0].Get<Rml::String>();
        });
        model.BindEventCallback("count_click", [this](Rml::DataModelHandle handle, Rml::Event&, const Rml::VariantList&) {
            ++clicks_;
            handle.DirtyVariable("clicks");
        });
        model.BindEventCallback("cycle", [this](Rml::DataModelHandle handle, Rml::Event&, const Rml::VariantList& args) {
            static const char* levels[] = {"Низкое", "Среднее", "Высокое", "Ультра"};
            const int dir = args.empty() ? 1 : args[0].Get<int>();
            quality_index_ = (quality_index_ + 4 + dir) % 4;
            quality_ = levels[quality_index_];
            handle.DirtyVariable("quality");
        });
        model_ = model.GetModelHandle();

        ui::register_list_source("demo-rows", &rows_source_);
        document_ = ui_.load_document(context_, "demo/gallery.rml");
        ui_.on_reload([this] { document_ = nullptr; });
        return document_ != nullptr;
    }

    void shutdown() {
        ui::register_list_source("demo-rows", nullptr);
        ui_.shutdown();
    }

    void update(f64 time, bool animate, bool scroll) {
        if (!pending_theme_.empty()) {
            if (ui_.set_theme(pending_theme_)) {
                theme_ = pending_theme_;
                model_.DirtyVariable("theme");
            }
            pending_theme_.clear();
        }
        if (animate) {
            const int health = 55 + static_cast<int>(25.0 * std::sin(time * 0.8));
            if (health != health_) {
                health_ = health;
                model_.DirtyVariable("health");
            }
        }
        if (scroll) {
            for (int i = 0; i < context_->GetNumDocuments(); ++i) {
                if (Rml::Element* list = context_->GetDocument(i)->GetElementById("rows")) {
                    float top = list->GetScrollTop() + 37.0f;
                    if (top >= list->GetScrollHeight() - list->GetClientHeight()) top = 0;
                    list->SetScrollTop(top);
                }
            }
        }
        ui_.update();
    }

    void render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, SDL_GPUTextureFormat format, u32 w, u32 h) {
        SDL_GPUColorTargetInfo clear{};
        clear.texture = target;
        clear.load_op = SDL_GPU_LOADOP_CLEAR;
        clear.store_op = SDL_GPU_STOREOP_STORE;
        clear.clear_color = {0, 0, 0, 1};
        SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &clear, 1, nullptr));
        if (context_->GetDimensions() != Rml::Vector2i(static_cast<int>(w), static_cast<int>(h)))
            context_->SetDimensions({static_cast<int>(w), static_cast<int>(h)});
        ui_.render(cmd, target, format, w, h);
    }

    bool handle_event(const SDL_Event& e) { return ui_.handle_event(context_, e); }
    ui::Ui& ui() { return ui_; }

private:
    ui::Ui ui_;
    Rml::Context* context_ = nullptr;
    Rml::ElementDocument* document_ = nullptr;
    Rml::DataModelHandle model_;
    DemoRows rows_source_;
    std::string theme_, pending_theme_;
    int clicks_ = 0;
    int volume_ = 70;
    bool music_ = true;
    int quality_index_ = 2;
    std::string quality_ = "Высокое";
    std::string name_ = "Борин";
    int health_ = 72;
    std::string rows_ = "50\u202f000"; // numbers grouped with a narrow space
};

class UiDemo final : public App {
public:
    std::filesystem::path ui_dir = utf8_path(FORGE_UI_DIR);
    std::string theme = "dark";
    bool scroll = false;
    u32 msaa = 2;

    bool on_init() override {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window(), &w, &h);
        if (!gallery_.init(gpu(), window(), static_cast<u32>(w), static_cast<u32>(h), ui_dir, theme, msaa)) {
            FORGE_ERROR("ui demo: start failed (UI folder: %s)", path_to_utf8(ui_dir).c_str());
            return false;
        }
        return true;
    }
    void on_event(const SDL_Event& e) override {
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) request_quit();
        gallery_.handle_event(e);
    }
    void on_frame(f64 dt) override {
        time_ += dt;
        gallery_.update(time_, true, scroll);
        const ui::UiStats& s = gallery_.ui().stats();
        char status[160];
        std::snprintf(status, sizeof(status), "ui update %.2f ms, render %.2f ms, %u draws, %u passes, MSAA x%u",
                      s.update_ms, s.render_ms, s.draws, s.passes, gallery_.ui().msaa_samples());
        set_status(status);
    }
    void on_render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, u32 w, u32 h) override {
        gallery_.render(cmd, target, swapchain_format(), w, h);
    }
    void on_shutdown() override {
        const FrameStats& s = frame_stats();
        FORGE_INFO("frame avg %.3f ms, worst %.3f ms (last %u frames)", s.avg_ms, s.worst_ms, FrameStats::kWindow);
        gallery_.shutdown();
    }

private:
    Gallery gallery_;
    f64 time_ = 0;
};

int run_screenshot(const std::filesystem::path& ui_dir, const std::string& theme, const char* path, u32 frames,
                   u32 msaa, bool scroll) {
    jobs::init();
    SDL_GPUDevice* device = render::create_offscreen_device();
    if (!device) {
        jobs::shutdown();
        return 1;
    }
    const u32 w = 1600, h = 900;
    SDL_GPUTexture* target = render::create_render_target(device, w, h);
    int result = 1;
    {
        Gallery gallery;
        if (target && gallery.init(device, nullptr, w, h, ui_dir, theme, msaa)) {
            f64 update_ms = 0, render_ms = 0, worst_ms = 0;
            u32 measured = 0;
            for (u32 f = 0; f < frames; ++f) {
                gallery.update(f / 60.0, false, scroll);
                SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
                gallery.render(cmd, target, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, w, h);
                SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
                SDL_WaitForGPUFences(device, true, &fence, 1);
                SDL_ReleaseGPUFence(device, fence);
                if (f < 2) continue; // the first frames load fonts and build everything
                const ui::UiStats& s = gallery.ui().stats();
                update_ms += s.update_ms;
                render_ms += s.render_ms;
                worst_ms = std::max(worst_ms, s.update_ms + s.render_ms);
                ++measured;
            }
            const ui::UiStats& s = gallery.ui().stats();
            measured = std::max(measured, 1u);
            FORGE_INFO("ui (CPU, %u frames%s): update avg %.3f ms, render avg %.3f ms, worst frame %.3f ms; %u draws, "
                       "%u passes, MSAA x%u",
                       measured, scroll ? ", list scrolling" : "", update_ms / measured, render_ms / measured,
                       worst_ms, s.draws, s.passes, gallery.ui().msaa_samples());
            result = render::save_png(device, target, w, h, path) ? 0 : 1;
        }
        gallery.shutdown();
    }
    if (target) SDL_ReleaseGPUTexture(device, target);
    render::destroy_offscreen_device(device);
    jobs::shutdown();
    return result;
}

} // namespace

int main(int argc, char** argv) {
    AppConfig config;
    config.title = "Forge UI";
    config.shader_formats = render::supported_shader_formats();
    UiDemo app;
    const char* screenshot = nullptr;
    u32 frames = 10;
    for (int i = 1; i < argc; ++i) {
        const bool has_value = i + 1 < argc;
        if (std::strcmp(argv[i], "--no-vsync") == 0) config.vsync = false;
        else if (std::strcmp(argv[i], "--ui") == 0 && has_value) app.ui_dir = utf8_path(argv[++i]);
        else if (std::strcmp(argv[i], "--theme") == 0 && has_value) app.theme = argv[++i];
        else if (std::strcmp(argv[i], "--scroll") == 0) app.scroll = true;
        else if (std::strcmp(argv[i], "--msaa") == 0 && has_value) app.msaa = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--screenshot") == 0 && has_value) screenshot = argv[++i];
        else if (std::strcmp(argv[i], "--frames") == 0 && has_value) frames = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
    }
    if (screenshot) return run_screenshot(app.ui_dir, app.theme, screenshot, frames, app.msaa, app.scroll);
    return app.run(config);
}
