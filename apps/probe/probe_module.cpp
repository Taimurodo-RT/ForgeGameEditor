#include "probe_module.h"

#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/game/runner.h"
#include "forge/game/saves.h"
#include "forge/level/level.h"
#include "forge/logic/logic.h"
#include "forge/objects/library.h"
#include "forge/world/generators.h"

#include <SDL3/SDL_gpu.h>
#include <yyjson.h>

#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef FORGE_UI_DIR
#define FORGE_UI_DIR "ui"
#endif
#ifndef PROBE_GAME_DIR
#define PROBE_GAME_DIR "tests/data/probe/game"
#endif
#ifndef PROBE_MODULE_DIR
#define PROBE_MODULE_DIR "tests/data/probe/module"
#endif

namespace probe {

namespace fs = std::filesystem;
using namespace forge;

namespace {

const std::vector<modules::Event>& events() {
    static const std::vector<modules::Event> list = {{"probe_ping", "пинг пробы"}};
    return list;
}

std::vector<std::string> event_ids() {
    std::vector<std::string> out;
    for (const modules::Event& e : events()) out.push_back(e.id);
    return out;
}

std::vector<modules::Value> values(const objects::Library*) { return {{"probe.ticks", "Тики пробы"}, {"probe.mark", "Метка пробы"}}; }

// --- the game -------------------------------------------------------------------------------------------------------

// Counts the frames it plays (probe.ticks) and keeps a number a test sets (probe.mark); the screen is its colour.
class ProbeGame final : public game::Game {
public:
    bool init(game::Shell& shell, SDL_GPUDevice*, SDL_GPUTextureFormat) override {
        shell_ = &shell;
        // Its verbs with its own events (the module's): the same file the editor's «Логика» reads.
        const fs::path file = shell.game_dir() / "verbs.json";
        std::error_code ec;
        if (fs::exists(file, ec) && !verbs_.load(file, &verbs_error_, event_ids())) FORGE_ERROR("Проба: %s", verbs_error_.c_str());
        return true;
    }
    bool begin(const fs::path& session, bool new_game, std::string* error) override {
        ticks_ = 0;
        if (!new_game) {
            std::vector<u8> text;
            if (!read_file(session / "probe.json", text)) {
                if (error) *error = "нет probe.json в сохранении";
                return false;
            }
            yyjson_doc* doc = yyjson_read(reinterpret_cast<const char*>(text.data()), text.size(), 0);
            ticks_ = static_cast<u64>(yyjson_get_num(yyjson_obj_get(doc ? yyjson_doc_get_root(doc) : nullptr, "ticks")));
            yyjson_doc_free(doc);
        }
        if (!shell_->vars().has("probe.mark")) shell_->vars().set("probe.mark", 0);
        shell_->vars().set("probe.ticks", static_cast<f64>(ticks_));
        running_ = true;
        return true;
    }
    bool save(const fs::path& session, std::string& location, std::string* error) override {
        location = "Проба";
        const std::string text = "{\"ticks\": " + std::to_string(ticks_) + "}\n";
        if (write_file_atomic(session / "probe.json", {reinterpret_cast<const u8*>(text.data()), text.size()})) return true;
        if (error) *error = "probe.json не записан";
        return false;
    }
    void end() override { running_ = false; }
    bool running() const override { return running_; }
    void update(f64, bool playing, bool) override {
        if (!running_ || !playing) return;
        ++ticks_;
        shell_->vars().set("probe.ticks", static_cast<f64>(ticks_));
    }
    void render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, u32, u32) override {
        SDL_GPUColorTargetInfo info{};
        info.texture = target;
        info.clear_color = SDL_FColor{kColor[0] / 255.0f, kColor[1] / 255.0f, kColor[2] / 255.0f, 1.0f};
        info.load_op = SDL_GPU_LOADOP_CLEAR;
        info.store_op = SDL_GPU_STOREOP_STORE;
        SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &info, 1, nullptr));
    }
    void shutdown() override { running_ = false; }

    const logic::Verbs& verbs() const { return verbs_; }
    const std::string& verbs_error() const { return verbs_error_; }
    u64 ticks() const { return ticks_; }

private:
    game::Shell* shell_ = nullptr;
    logic::Verbs verbs_;
    std::string verbs_error_;
    u64 ticks_ = 0;
    bool running_ = false;
};

// --- its self-test ----------------------------------------------------------------------------------------------------

// The files of the session folder, by their paths in it: what a refused load must leave as it was.
std::map<std::string, std::vector<u8>> session_files(const fs::path& dir) {
    std::map<std::string, std::vector<u8>> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::vector<u8> bytes;
        read_file(it->path(), bytes);
        out[path_to_utf8(it->path().lexically_relative(dir))] = std::move(bytes);
    }
    return out;
}

// --scene probe: a new game plays, its values and its verbs are its module's. probe_save: probe.mark 42 saved as the
// slot probe-slot (another process plays on with it). probe_foreign: the slots of «Старая шахта» that slice's scene
// module_slots left in the same --user folder (slice-slot, old-slot: a slot without "module") are refused, the game
// and its session as they were; then probe-slot, saved by another process, loads with its mark.
class SelfTest {
public:
    SelfTest(ProbeGame& game, std::string scene) : g_(game), scene_(std::move(scene)) {}

    bool frame(game::Shell& s, u32 f, int& failures) {
        failures_ = &failures;
        if (f < 3) return true;
        if (f == 3) {
            check(scene_ == "probe" || scene_ == "probe_save" || scene_ == "probe_foreign", "сцена пробы «" + scene_ + "» есть");
            check(s.screen() == game::Screen::Playing || s.new_game(), "новая игра пробы начинается");
            return true;
        }
        if (f < 15) return true; // it plays a few frames
        check(g_.running() && g_.ticks() >= 5, "игра пробы идёт: тиков " + std::to_string(g_.ticks()));
        check(s.vars().get("probe.ticks").number() == static_cast<f64>(g_.ticks()), "её тики в её значении probe.ticks");
        for (const modules::Value& v : values(nullptr))
            check(s.vars().has(v.var), "значение модуля «" + v.words + "» (" + v.var + ") есть в игре");
        check(!s.vars().has("hero.hearts") && !s.vars().has("inv.coins"), "значений «Старой шахты» в игре пробы нет");
        const logic::VerbDef* ping = g_.verbs().find("ping");
        check(g_.verbs_error().empty() && ping && ping->when == "probe_ping",
              "её действие «пингует» читается с её событием probe_ping " + g_.verbs_error());
        for (const std::string& e : s.data_errors()) check(false, "данные игры пробы: " + e);
        if (scene_ == "probe_save") save(s);
        if (scene_ == "probe_foreign") foreign(s);
        return false;
    }

private:
    void check(bool ok, const std::string& what) {
        if (ok) FORGE_INFO("self-test: ok   %s", what.c_str());
        else {
            ++*failures_;
            FORGE_ERROR("self-test: FAIL %s", what.c_str());
        }
    }
    void save(game::Shell& s) {
        s.vars().set("probe.mark", 42);
        check(s.save("probe-slot", "Проба: метка 42"), "игра пробы сохраняется в probe-slot");
        const std::optional<game::SlotInfo> info = s.slots().info("probe-slot");
        check(info && info->module == kModuleId && info->module_error.empty(), "слот пробы записан с модулем «probe»");
    }
    void foreign(game::Shell& s) {
        s.vars().set("probe.mark", 7);
        const fs::path session = s.slots().session();
        for (const char* slot : {"slice-slot", "old-slot"}) {
            const std::optional<game::SlotInfo> info = s.slots().info(slot);
            check(info.has_value(), std::string("слот «Старой шахты» ") + slot +
                                        " есть (его оставила сцена slice module_slots в той же папке --user)");
            if (!info) continue;
            check(info->module == "slice", std::string(slot) + ": модуль slice (" + (info->module.empty() ? info->module_error : info->module) + ")");
            check(s.save("probe-before", "Проба до отказа"), "сессия пробы записана перед загрузкой чужого слота");
            const auto before = session_files(session);
            const u64 ticks = g_.ticks();
            check(!s.load(slot), std::string("чужой слот ") + slot + " не загружается в игру пробы");
            check(session_files(session) == before, std::string("после отказа ") + slot + " сессия пробы та же, файл в файл");
            check(g_.running() && s.screen() == game::Screen::Playing && g_.ticks() == ticks && s.vars().get("probe.mark").number() == 7,
                  std::string("и игра пробы идёт как шла: метка 7, тиков ") + std::to_string(ticks));
        }
        check(s.load("probe-slot"), "свой слот probe-slot (его записал другой процесс) загружается");
        check(s.vars().get("probe.mark").number() == 42 && g_.running(), "с меткой 42 из того процесса");
    }

    ProbeGame& g_;
    std::string scene_;
    int* failures_ = nullptr;
};

int run(int argc, char** argv) {
    std::string scene = "probe";
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--scene") == 0) scene = argv[i + 1];
    ProbeGame game;
    SelfTest test(game, scene);
    game::GameMain m;
    m.dev_ui_dir = FORGE_UI_DIR;
    m.dev_game_dir = PROBE_GAME_DIR;
    m.module = kModuleId;
    m.test = [&](game::Shell& shell, u32 frame, int& failures) { return test.frame(shell, frame, failures); };
    return game::run_game(game, m, argc, argv);
}

// --- its level in the editor ----------------------------------------------------------------------------------------

// One layer «Проба» with one tile, nothing around: what the editor's tabs need of a module, and nothing of slice's.
class ProbeLevel final : public level::LevelModule {
public:
    ProbeLevel() {
        tiles_.push_back({"probe_tile", "Плитка пробы", "Проба", "тестовый модуль", 0, 1, "1"});
    }
    std::string title() const override { return "Проба"; }
    world::WorldDesc world_desc() const override {
        world::WorldDesc d;
        d.layer_count = 1;
        return d;
    }
    std::shared_ptr<const world::Generator> generator() const override { return gen_; }
    void setup_scene(scene::Scene&) override {}
    const std::vector<std::string>& layer_names() const override { return layers_; }
    const std::vector<level::TileDef>& tiles() const override { return tiles_; }
    void tile_icon(const level::TileDef&, u32 size, std::vector<u8>& rgba) const override {
        rgba.clear();
        for (u32 i = 0; i < size * size; ++i) rgba.insert(rgba.end(), {kColor[0], kColor[1], kColor[2], 255});
    }
    void start(f64& x, f64& y) const override { x = y = 0; }
    Color background() const override { return {kColor[0] / 255.0f, kColor[1] / 255.0f, kColor[2] / 255.0f, 1.0f}; }
    bool init_view(SDL_GPUDevice*, SDL_GPUTextureFormat) override { return true; }
    void shutdown_view() override {}
    void prepare_view(SDL_GPUCommandBuffer*, level::Level&, const render::Camera2D&, u32, u32, const level::ViewOptions&, f64) override {}
    void draw_view(SDL_GPUCommandBuffer*, SDL_GPURenderPass*) override {}
    objects::Library* library() override { return &library_; }
    u64 objects_version() const override { return library_.version(); }

private:
    std::shared_ptr<const world::Generator> gen_ = std::make_shared<world::EmptyGenerator>();
    std::vector<std::string> layers_ = {"Проба"};
    std::vector<level::TileDef> tiles_;
    objects::Library library_;
};

} // namespace

modules::ModuleDef module_def() {
    modules::ModuleDef m;
    m.id = kModuleId;
    m.name = "Проба";
    m.files = utf8_path(PROBE_MODULE_DIR);
    m.events = events();
    m.values = values;
    m.run = run;
    m.make_level_module = [] { return std::make_unique<ProbeLevel>(); };
    m.test_only = true;
    return m;
}

} // namespace probe
