#include "forge/game/runner.h"

#include "forge/core/jobs.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/core/time.h"
#include "forge/platform/app.h"
#include "forge/render/gpu.h"
#include "forge/render/offscreen.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace forge::game {

namespace fs = std::filesystem;

namespace {

struct Options {
    ShellConfig shell;
    const char* screenshot = nullptr;
    u32 frames = 120;
    bool test = false;
    bool vsync = true;
};

// A packaged game has no console: everything logged also goes to log.txt
// in the user folder, so a player can send it.
struct LogFile {
    std::FILE* file = nullptr;
    std::mutex mutex;
};
LogFile g_log;

void log_to_file(LogLevel level, const char* message, void*) {
    static const char* names[] = {"trace", "info", "warn", "error"};
    std::lock_guard lock(g_log.mutex);
    if (!g_log.file) return;
    std::fprintf(g_log.file, "[%s] %s\n", names[static_cast<int>(level)], message);
    std::fflush(g_log.file);
}

void open_log(const fs::path& user_dir) {
#ifdef _WIN32
    g_log.file = _wfopen((user_dir / "log.txt").c_str(), L"w");
#else
    g_log.file = std::fopen((user_dir / "log.txt").c_str(), "w");
#endif
    if (g_log.file) log_set_sink(log_to_file, nullptr);
}

void close_log() {
    log_set_sink(nullptr, nullptr);
    std::lock_guard lock(g_log.mutex);
    if (g_log.file) std::fclose(g_log.file);
    g_log.file = nullptr;
}

void fatal(SDL_Window* window, const std::string& what) {
    FORGE_ERROR("%s", what.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Игра не запустилась", what.c_str(), window);
}

class GameApp final : public App {
public:
    GameApp(Game& game, const Options& options) : game_(game), options_(options) {}

    bool on_init() override {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window(), &w, &h);
        if (!shell_.init(game_, gpu(), window(), swapchain_format(), static_cast<u32>(w), static_cast<u32>(h), options_.shell)) {
            fatal(window(), "Не удалось загрузить файлы игры (папка интерфейса: " + path_to_utf8(options_.shell.ui_dir) +
                                ", папка игры: " + path_to_utf8(options_.shell.game_dir) + ").");
            return false;
        }
        SDL_SetWindowTitle(window(), shell_.title().c_str());
        return true;
    }
    void on_event(const SDL_Event& e) override {
        if (!shell_.handle_event(e) && shell_.screen() == Screen::Playing && !shell_.in_dialogue()) game_.handle_event(e);
    }
    void on_frame(f64 dt) override {
        shell_.update(std::min(dt, 0.1));
        if (shell_.quit_requested()) {
            if (shell_.screen() != Screen::Main) shell_.to_main_menu(); // autosaves
            request_quit();
        }
    }
    void on_render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, u32 w, u32 h) override {
        shell_.render(cmd, target, w, h);
    }
    void on_shutdown() override {
        const FrameStats& s = frame_stats();
        FORGE_INFO("frame avg %.3f ms, worst %.3f ms (last %u frames)", s.avg_ms, s.worst_ms, FrameStats::kWindow);
        shell_.shutdown();
    }

private:
    Game& game_;
    Options options_;
    Shell shell_;
};

int run_offscreen(Game& game, const GameMain& main, const Options& o) {
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
        Shell shell;
        if (target && shell.init(game, device, nullptr, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, w, h, o.shell)) {
            int failures = 0;
            bool testing = o.test && main.test;
            u32 frames = o.frames;
            f64 worst = 0, total = 0;
            for (u32 f = 0; f < frames || testing; ++f) {
                if (testing) testing = main.test(shell, f, failures);
                if (f > 100000) break;
                const u64 t0 = time_now_ns();
                shell.update(1.0 / 60.0);
                SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
                shell.render(cmd, target, w, h);
                SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
                SDL_WaitForGPUFences(device, true, &fence, 1);
                SDL_ReleaseGPUFence(device, fence);
                const f64 ms = ns_to_ms(time_now_ns() - t0);
                if (f > 10) worst = std::max(worst, ms);
                total += ms;
                frames = std::max(frames, f + 1);
            }
            FORGE_INFO("game (offscreen): %u frames, avg %.2f ms, worst %.2f ms (with the software GPU)", frames,
                       total / frames, worst);
            for (const std::string& e : shell.data_errors()) {
                FORGE_ERROR("game data: %s", e.c_str());
                ++failures;
            }
            const bool saved = !o.screenshot || render::save_png(device, target, w, h, o.screenshot);
            if (o.test) {
                if (failures == 0) FORGE_INFO("self-test passed");
                else FORGE_ERROR("self-test: %d checks failed", failures);
            }
            result = saved && failures == 0 ? 0 : 1;
        }
        shell.shutdown();
    }
    if (target) SDL_ReleaseGPUTexture(device, target);
    render::destroy_offscreen_device(device);
    jobs::shutdown();
    return result;
}

} // namespace

int run_game(Game& game, const GameMain& main, int argc, char** argv) {
    Options o;
    // A packaged game keeps its files in "data" next to the executable.
    std::error_code ec;
    const fs::path packaged = exe_dir() / "data";
    if (fs::is_directory(packaged / "ui", ec) && fs::is_directory(packaged / "game", ec)) {
        o.shell.ui_dir = packaged / "ui";
        o.shell.game_dir = packaged / "game";
        o.shell.dev = false;
    } else {
        o.shell.ui_dir = main.dev_ui_dir;
        o.shell.game_dir = main.dev_game_dir;
    }
    for (int i = 1; i < argc; ++i) {
        const bool has_value = i + 1 < argc;
        if (std::strcmp(argv[i], "--screenshot") == 0 && has_value) o.screenshot = argv[++i];
        else if (std::strcmp(argv[i], "--frames") == 0 && has_value) o.frames = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--test") == 0) o.test = true;
        else if (std::strcmp(argv[i], "--ui") == 0 && has_value) o.shell.ui_dir = utf8_path(argv[++i]);
        else if (std::strcmp(argv[i], "--data") == 0 && has_value) o.shell.game_dir = utf8_path(argv[++i]);
        else if (std::strcmp(argv[i], "--user") == 0 && has_value) o.shell.user_dir = utf8_path(argv[++i]);
        else if (std::strcmp(argv[i], "--theme") == 0 && has_value) o.shell.theme = argv[++i];
        else if (std::strcmp(argv[i], "--no-vsync") == 0) o.vsync = false;
    }

    if (o.screenshot || o.test) {
        // Offscreen runs never touch the player's saves.
        if (o.shell.user_dir.empty()) {
            o.shell.user_dir = fs::temp_directory_path() / "forge_game_offscreen";
            fs::remove_all(o.shell.user_dir, ec);
        }
        return run_offscreen(game, main, o);
    }

    AppConfig config;
    config.title = "Forge";
    config.shader_formats = render::supported_shader_formats();
    config.stats_in_title = false;
    config.vsync = o.vsync;
    GameApp app(game, o);
    // Settings that must be known before the window opens.
    const GameInfo info = read_game_info(o.shell.game_dir);
    if (o.shell.user_dir.empty()) o.shell.user_dir = user_dir(info.org, info.title);
    const Settings s = load_settings(o.shell.user_dir);
    config.fullscreen = s.fullscreen;
    if (!s.vsync) config.vsync = false;
    open_log(o.shell.user_dir);
    const int code = app.run(config);
    close_log();
    return code;
}

} // namespace forge::game
