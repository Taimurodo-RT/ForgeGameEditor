#include "forge/game/shell.h"

#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/ui/ui.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>
#include <yyjson.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace forge::game {

namespace fs = std::filesystem;

namespace {

struct SlotView {
    Rml::String id, title, location, playtime, date;
    bool autosave = false;
};
struct QuestView {
    Rml::String title, text;
    bool done = false;
};
struct ThemeView {
    Rml::String key, name;
};

struct Toast {
    std::string text;
    f64 left = 0;
};

const char* theme_name(const std::string& key) {
    if (key == "dark") return "Тёмная";
    if (key == "light") return "Светлая";
    if (key == "fantasy") return "Фэнтези";
    if (key == "parchment") return "Пергамент";
    return nullptr;
}

bool read_text(const fs::path& path, std::string& out) {
    std::vector<u8> bytes;
    if (!read_file(path, bytes)) return false;
    out.assign(bytes.begin(), bytes.end());
    return true;
}

std::string json_str(yyjson_val* v, const char* fallback = "") {
    return yyjson_is_str(v) ? std::string(yyjson_get_str(v), yyjson_get_len(v)) : std::string(fallback);
}

const char* screen_key(Screen s) {
    switch (s) {
    case Screen::Main: return "main";
    case Screen::Playing: return "playing";
    case Screen::Paused: return "paused";
    case Screen::Slots: return "slots";
    case Screen::Settings: return "settings";
    case Screen::Journal: return "journal";
    case Screen::Loading: return "loading";
    }
    return "main";
}

} // namespace

// What the documents see, plus the work waiting for the loading screen.
struct Shell::Model {
    Rml::DataModelHandle handle;
    Rml::String screen = "main";
    Rml::String title;
    bool has_continue = false, has_slots = false;
    Rml::String continue_text;
    std::vector<SlotView> slots;
    Rml::String slots_mode = "load";
    bool fullscreen = false, vsync = true, show_fps = false;
    int ui_scale = 100;
    Rml::String theme;
    std::vector<ThemeView> themes;
    std::vector<QuestView> journal;
    Rml::String tracker_title, tracker_text;
    bool in_dialogue = false, d_asks = false;
    Rml::String d_speaker, d_color, d_text, d_input;
    std::vector<Rml::String> d_choices;
    std::vector<Rml::String> toasts;
    Rml::String fps, playtime;
    bool own_menu = false; // the game's main menu (ui/) instead of this one

    std::vector<Toast> toast_list;
    u64 seen_vars = ~0ull;
    f64 fps_avg = 0, fps_timer = 0;
    // Work that waits until the loading screen has been drawn once.
    std::function<void()> pending;
    int pending_frames = 0;
    bool slots_dirty = true;
};

Shell::Shell() = default;
Shell::~Shell() { shutdown(); }

ui::Ui& Shell::ui() { return *ui_; }

bool Shell::init(Game& game, SDL_GPUDevice* device, SDL_Window* window, SDL_GPUTextureFormat format, u32 width,
                 u32 height, const ShellConfig& config) {
    game_ = &game;
    device_ = device;
    window_ = window;
    format_ = format;
    config_ = config;
    width_ = width;
    height_ = height;
    FORGE_INFO("экран игрока: %u×%u пикселей", width, height); // screens meet these pixels (forge-fit)
    model_ = std::make_unique<Model>();
    runner_ = std::make_unique<DialogueRunner>(vars_, [this](std::string_view n, const std::vector<Value>& a) { return call(n, a); });

    const GameInfo info = read_game_info(config_.game_dir);
    if (!info.ok) data_errors_.push_back("нет файла game.json или он не читается (" + path_to_utf8(config_.game_dir) + ")");
    title_ = info.title;
    org_ = info.org;
    autosave_s_ = info.autosave_minutes * 60.0;
    if (config_.theme.empty()) config_.theme = info.theme;
    user_dir_ = config.user_dir.empty() ? user_dir(org_, title_) : config.user_dir;
    slots_ = std::make_unique<SaveSlots>(user_dir_);
    settings_ = load_settings(user_dir_);

    ui::UiConfig uc;
    uc.root = config.ui_dir;
    // The command line wins, then the player's choice, then the game's own.
    uc.theme = !config.theme.empty()      ? config.theme
               : !settings_.theme.empty() ? settings_.theme
               : !config_.theme.empty()   ? config_.theme
                                          : uc.theme;
    uc.hot_reload = window != nullptr && config.dev;
    uc.debugger = window != nullptr && config.dev;
    ui_ = std::make_unique<ui::Ui>();
    if (!ui_->init(device, window, uc)) return false;
    context_ = ui_->create_context("game", width, height);
    if (!context_) return false;
    bind_model();
    if (!ui_->load_document(context_, "game/shell.rml")) return false;
    // The game's screens: made before the game, which tells them what its things are called.
    screens_ = std::make_unique<GameScreens>();
    screens_->on_action = [this](const ScreenAction& a, const std::string&) { screen_action(a); };
    screens_->set_quests(&quests_); // the journal's list
    if (!game.init(*this, device, format)) return false;
    load_game_data(); // after the game defined the functions dialogues may call
    screens_->load(context_, config_.game_dir, window != nullptr && config.dev);
    apply_settings(settings_);
    show(Screen::Main);
    return true;
}

void Shell::shutdown() {
    if (!game_) return;
    if (game_->running()) game_->end();
    game_->shutdown();
    game_ = nullptr;
    if (runner_) runner_->stop();
    screens_.reset(); // its pages live in the UI's context
    if (ui_) ui_->shutdown();
    context_ = nullptr;
}

GameInfo read_game_info(const fs::path& game_dir) {
    GameInfo info;
    std::string text;
    if (!read_text(game_dir / "game.json", text)) return info;
    yyjson_doc* doc = yyjson_read(text.data(), text.size(), 0);
    if (yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr) {
        info.ok = true;
        info.title = json_str(yyjson_obj_get(root, "title"), "Forge");
        info.org = json_str(yyjson_obj_get(root, "org"), "Forge");
        info.theme = json_str(yyjson_obj_get(root, "theme"));
        if (yyjson_val* a = yyjson_obj_get(root, "autosave_minutes"); yyjson_is_num(a)) info.autosave_minutes = yyjson_get_num(a);
    }
    if (doc) yyjson_doc_free(doc);
    return info;
}

void Shell::load_game_data() {
    std::string text;
    std::vector<std::string> known;
    for (const auto& [name, fn] : calls_) known.push_back(name);
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(config_.game_dir / "dialogues", ec)) {
        if (e.path().extension() != ".json" || !read_text(e.path(), text)) continue;
        const std::string id = path_to_utf8(e.path().stem());
        auto d = std::make_unique<Dialogue>();
        DialogueReport report;
        d->load(text, report, known);
        for (const std::string& w : report.warnings) FORGE_WARN("диалог %s: %s", id.c_str(), w.c_str());
        for (const std::string& err : report.errors) data_errors_.push_back("диалог " + id + ": " + err);
        if (report.ok()) dialogues_[id] = std::move(d);
    }
    if (read_text(config_.game_dir / "quests.json", text)) {
        std::vector<std::string> errors;
        quests_.load(text, errors);
        for (const std::string& err : errors) data_errors_.push_back("задания: " + err);
    }
    for (const std::string& err : data_errors_) FORGE_ERROR("%s", err.c_str());
}

void Shell::start_loading(std::function<void()> work) {
    show(Screen::Loading);
    model_->pending = std::move(work);
    model_->pending_frames = 1;
}

// What a button on one of the game's screens does, beyond showing and hiding
// screens (GameScreens does that itself).
void Shell::screen_action(const ScreenAction& a) {
    const std::string& t = a.target;
    if (a.what == "message") {
        if (on_message) on_message(t);
        else FORGE_INFO("сообщение логике: %s", t.c_str());
    } else if (a.what == "change") {
        std::string error;
        const Expr e = Expr::parse_actions(t, &error);
        if (!error.empty()) FORGE_WARN("кнопка: «%s» не читается: %s", t.c_str(), error.c_str());
        else e.run(vars_, [this](std::string_view n, const std::vector<Value>& args) { return call(n, args); });
    } else if (a.what == "talk") {
        if (!talk(t)) FORGE_WARN("разговор «%s» не начался", t.c_str());
    } else if (a.what == "pause") pause(true);
    else if (a.what == "resume") pause(false);
    else if (a.what == "menu") to_main_menu();
    else if (a.what == "quit") quit_ = true;
    else if (a.what == "new") start_loading([this] { new_game(); });
    else if (a.what == "continue") {
        if (!slots_->list().empty()) start_loading([this] { if (!continue_game()) show(Screen::Main); });
    } else if (a.what == "load" || a.what == "save") {
        if (a.what == "save" && !game_->running()) return;
        if (a.what == "load" && slots_->list().empty()) {
            toast("Сохранений пока нет");
            return;
        }
        model_->slots_mode = a.what;
        if (screen_ != Screen::Slots) back_ = screen_;
        show(Screen::Slots);
    } else if (a.what == "settings") {
        back_ = screen_;
        show(Screen::Settings);
    } else FORGE_WARN("кнопка: неизвестное действие «%s»", a.what.c_str());
}

void Shell::define(const std::string& name, CallFn fn) { calls_[name] = std::move(fn); }

Value Shell::call(std::string_view name, const std::vector<Value>& args) {
    auto it = calls_.find(std::string(name));
    if (it == calls_.end()) {
        FORGE_WARN("диалог вызвал неизвестную функцию %.*s", static_cast<int>(name.size()), name.data());
        return Value();
    }
    return it->second(name, args);
}

const Dialogue* Shell::dialogue(std::string_view id) const {
    auto it = dialogues_.find(std::string(id));
    return it == dialogues_.end() ? nullptr : it->second.get();
}

bool Shell::talk(std::string_view id) {
    const Dialogue* d = dialogue(id);
    if (!d || screen_ != Screen::Playing) return false;
    model_->d_input.clear();
    return runner_->start(*d);
}

bool Shell::in_dialogue() const { return runner_ && runner_->active(); }

void Shell::end_dialogue() { runner_->stop(); }

void Shell::toast(std::string text) {
    FORGE_INFO("%s", text.c_str());
    model_->toast_list.push_back({std::move(text), 3.0});
    if (model_->toast_list.size() > 4) model_->toast_list.erase(model_->toast_list.begin());
}

// --- flow ------------------------------------------------------------------

void Shell::show(Screen s) {
    screen_ = s;
    if (s == Screen::Main || s == Screen::Slots) model_->slots_dirty = true;
}

bool Shell::begin(std::string_view slot_id) {
    std::string error;
    if (game_->running()) game_->end();
    runner_->stop();
    vars_.clear();
    playtime_ = 0;
    if (!slots_->begin_session(slot_id, &error)) {
        toast("Не удалось начать: " + error);
        show(Screen::Main);
        return false;
    }
    std::string text;
    if (!slot_id.empty() && read_text(slots_->session() / "state.json", text)) {
        yyjson_doc* doc = yyjson_read(text.data(), text.size(), 0);
        yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
        if (root) {
            playtime_ = yyjson_get_num(yyjson_obj_get(root, "playtime"));
            if (yyjson_val* v = yyjson_obj_get(root, "vars")) {
                usize len = 0;
                char* json = yyjson_val_write(v, 0, &len);
                if (json) vars_.from_json({json, len});
                std::free(json);
            }
        }
        if (doc) yyjson_doc_free(doc);
    }
    if (!game_->begin(slots_->session(), slot_id.empty(), &error)) {
        toast("Не удалось начать: " + error);
        show(Screen::Main);
        return false;
    }
    slot_ = std::string(slot_id);
    since_autosave_ = 0;
    show(Screen::Playing);
    return true;
}

bool Shell::new_game() { return begin({}); }

bool Shell::load(std::string_view slot_id) {
    if (!slots_->exists(slot_id)) return false;
    const bool ok = begin(slot_id);
    if (ok) toast("Загружено");
    return ok;
}

bool Shell::continue_game() {
    const auto latest = slots_->latest();
    return latest && load(latest->id);
}

bool Shell::save(std::string_view slot_id, std::string title, bool autosave) {
    if (!game_->running()) return false;
    std::string error, location;
    if (!game_->save(slots_->session(), location, &error)) {
        toast("Не удалось сохранить: " + error);
        return false;
    }
    {
        yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
        yyjson_mut_val* root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);
        yyjson_mut_obj_add_real(doc, root, "playtime", playtime_);
        const std::string vars = vars_.to_json();
        yyjson_doc* vdoc = yyjson_read(vars.data(), vars.size(), 0);
        yyjson_mut_obj_add_val(doc, root, "vars", yyjson_val_mut_copy(doc, yyjson_doc_get_root(vdoc)));
        usize len = 0;
        char* json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY_TWO_SPACES, &len);
        const bool ok = json && write_file_atomic(slots_->session() / "state.json", {reinterpret_cast<const u8*>(json), len});
        std::free(json);
        yyjson_doc_free(vdoc);
        yyjson_mut_doc_free(doc);
        if (!ok) {
            toast("Не удалось сохранить: state.json");
            return false;
        }
    }
    SlotInfo info;
    info.id = std::string(slot_id);
    info.title = std::move(title);
    info.location = std::move(location);
    info.playtime_s = playtime_;
    info.autosave = autosave;
    if (!slots_->commit(info, &error)) {
        toast("Не удалось сохранить: " + error);
        return false;
    }
    if (!autosave) slot_ = info.id;
    since_autosave_ = 0;
    model_->slots_dirty = true;
    toast(autosave ? "Автосохранение" : "Сохранено");
    return true;
}

void Shell::pause(bool on) {
    if (on && screen_ == Screen::Playing) show(Screen::Paused);
    else if (!on && screen_ == Screen::Paused) show(Screen::Playing);
}

void Shell::to_main_menu() {
    model_->toast_list.clear(); // the game's messages stay with the game
    if (game_->running()) {
        save("autosave", "Автосохранение", true);
        game_->end();
    }
    runner_->stop();
    screens_->hide_commands();
    show(Screen::Main);
}

void Shell::apply_settings(const Settings& s) {
    settings_ = s;
    settings_.ui_scale = std::clamp(settings_.ui_scale, 0.5f, 3.0f);
    if (window_) {
        SDL_SetWindowFullscreen(window_, settings_.fullscreen);
        SDL_GPUPresentMode mode = SDL_GPU_PRESENTMODE_VSYNC;
        if (!settings_.vsync)
            mode = SDL_WindowSupportsGPUPresentMode(device_, window_, SDL_GPU_PRESENTMODE_MAILBOX) ? SDL_GPU_PRESENTMODE_MAILBOX
                   : SDL_WindowSupportsGPUPresentMode(device_, window_, SDL_GPU_PRESENTMODE_IMMEDIATE) ? SDL_GPU_PRESENTMODE_IMMEDIATE
                                                                                                       : SDL_GPU_PRESENTMODE_VSYNC;
        SDL_SetGPUSwapchainParameters(device_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode);
    }
    if (context_) context_->SetDensityIndependentPixelRatio(settings_.ui_scale);
    if (!settings_.theme.empty() && ui_ && settings_.theme != ui_->theme()) ui_->set_theme(settings_.theme);
    save_settings(user_dir_, settings_);
}

// --- input -----------------------------------------------------------------

bool Shell::handle_event(const SDL_Event& e) {
    if (e.type == SDL_EVENT_QUIT) {
        quit_ = true;
        return true;
    }
    // A held Enter or Space presses a screen's button once, not again with every repeat.
    if (e.type == SDL_EVENT_KEY_DOWN && e.key.repeat &&
        (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER || e.key.key == SDLK_SPACE) && screens_ &&
        screens_->has_focus(context_))
        return true;
    if (ui_->handle_event(context_, e)) {
        // Documents fill the screen; the mouse over a bare document body is
        // over the world, so the game gets the click or the wheel.
        const bool mouse = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP ||
                           e.type == SDL_EVENT_MOUSE_WHEEL || e.type == SDL_EVENT_MOUSE_MOTION;
        if (!mouse || !over_world()) return true;
    }
    if (e.type != SDL_EVENT_KEY_DOWN || e.key.repeat) return false;
    const SDL_Keycode k = e.key.key;

    if (screen_ == Screen::Playing && in_dialogue()) {
        const DialogueLine& line = runner_->line();
        if (k >= SDLK_1 && k <= SDLK_9) {
            runner_->choose(static_cast<u32>(k - SDLK_1));
            return true;
        }
        if ((k == SDLK_SPACE || k == SDLK_RETURN || k == SDLK_E) && line.choices.empty()) {
            runner_->advance();
            return true;
        }
        if (k == SDLK_ESCAPE) {
            end_dialogue();
            return true;
        }
        return true; // the hero does not move while talking
    }
    switch (screen_) {
    case Screen::Playing:
        if (k == SDLK_ESCAPE) {
            if (!screens_->close_top()) pause(true); // a window of the game's closes first
        }
        else if (k == SDLK_J) {
            back_ = Screen::Playing;
            show(Screen::Journal);
        } else if (k == SDLK_F5) save(slot_.empty() ? slots_->new_id() : slot_, slot_.empty() ? "Быстрое сохранение" : "Сохранение");
        else if (k == SDLK_F9) {
            if (!slot_.empty()) load(std::string(slot_));
            else continue_game();
        } else return false;
        return true;
    case Screen::Paused:
        if (k == SDLK_ESCAPE) pause(false);
        return true;
    case Screen::Slots:
    case Screen::Settings:
    case Screen::Journal:
        if (k == SDLK_ESCAPE || (k == SDLK_J && screen_ == Screen::Journal)) show(back_);
        return true;
    default: return false;
    }
}

// --- frame -----------------------------------------------------------------

void Shell::update(f64 dt) {
    Model& m = *model_;
    if (m.pending && m.pending_frames <= 0) {
        auto work = std::move(m.pending);
        m.pending = nullptr;
        work();
    }
    const bool playing = screen_ == Screen::Playing && game_->running();
    if (playing) {
        playtime_ += dt;
        since_autosave_ += dt;
        if (autosave_s_ > 0 && since_autosave_ >= autosave_s_ && !in_dialogue()) save("autosave", "Автосохранение", true);
    }
    // A window of the game's may stop the world while it is open.
    const bool stopped = playing && screens_->pauses();
    // A game that is not running draws its menu backdrop.
    game_->update(dt, (playing && !stopped) || !game_->running(), playing && !stopped && !in_dialogue());
    const bool in_game = game_->running() && screen_ != Screen::Main && screen_ != Screen::Loading;
    screens_->update(vars_, in_game, screen_ == Screen::Main, static_cast<int>(width_), static_cast<int>(height_),
                     [this](std::string_view n, const std::vector<Value>& args) { return call(n, args); });
    for (Toast& t : m.toast_list) t.left -= dt;
    std::erase_if(m.toast_list, [](const Toast& t) { return t.left <= 0; });
    m.fps_avg = m.fps_avg == 0 ? dt : m.fps_avg * 0.95 + dt * 0.05;
    refresh_model();
    ui_->update();
}

void Shell::render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, u32 width, u32 height) {
    if (context_->GetDimensions() != Rml::Vector2i(static_cast<int>(width), static_cast<int>(height)))
        context_->SetDimensions({static_cast<int>(width), static_cast<int>(height)});
    if (width != width_ || height != height_) FORGE_INFO("экран игрока: %u×%u пикселей", width, height);
    width_ = width;
    height_ = height;
    game_->render(cmd, target, width, height);
    ui_->render(cmd, target, format_, width, height);
    if (model_->pending) --model_->pending_frames;
}

bool Shell::over_world() const {
    Rml::Element* hover = context_ ? context_->GetHoverElement() : nullptr;
    return !hover || hover == hover->GetOwnerDocument();
}

Rml::Element* Shell::find_element(const char* id) {
    for (int i = 0; i < context_->GetNumDocuments(); ++i)
        if (Rml::Element* e = context_->GetDocument(i)->GetElementById(id)) return e;
    return nullptr;
}

// --- data model ------------------------------------------------------------

void Shell::bind_model() {
    Model& m = *model_;
    Rml::DataModelConstructor c = context_->CreateDataModel("shell");
    if (auto s = c.RegisterStruct<SlotView>()) {
        s.RegisterMember("id", &SlotView::id);
        s.RegisterMember("title", &SlotView::title);
        s.RegisterMember("location", &SlotView::location);
        s.RegisterMember("playtime", &SlotView::playtime);
        s.RegisterMember("date", &SlotView::date);
        s.RegisterMember("autosave", &SlotView::autosave);
    }
    if (auto s = c.RegisterStruct<QuestView>()) {
        s.RegisterMember("title", &QuestView::title);
        s.RegisterMember("text", &QuestView::text);
        s.RegisterMember("done", &QuestView::done);
    }
    if (auto s = c.RegisterStruct<ThemeView>()) {
        s.RegisterMember("key", &ThemeView::key);
        s.RegisterMember("name", &ThemeView::name);
    }
    c.RegisterArray<std::vector<SlotView>>();
    c.RegisterArray<std::vector<QuestView>>();
    c.RegisterArray<std::vector<ThemeView>>();
    c.RegisterArray<std::vector<Rml::String>>();

    c.Bind("screen", &m.screen);
    c.Bind("title", &m.title);
    c.Bind("has_continue", &m.has_continue);
    c.Bind("continue_text", &m.continue_text);
    c.Bind("has_slots", &m.has_slots);
    c.Bind("slots", &m.slots);
    c.Bind("slots_mode", &m.slots_mode);
    c.Bind("fullscreen", &m.fullscreen);
    c.Bind("vsync", &m.vsync);
    c.Bind("show_fps", &m.show_fps);
    c.Bind("ui_scale", &m.ui_scale);
    c.Bind("theme", &m.theme);
    c.Bind("themes", &m.themes);
    c.Bind("journal", &m.journal);
    c.Bind("tracker_title", &m.tracker_title);
    c.Bind("tracker_text", &m.tracker_text);
    c.Bind("in_dialogue", &m.in_dialogue);
    c.Bind("d_speaker", &m.d_speaker);
    c.Bind("d_color", &m.d_color);
    c.Bind("d_text", &m.d_text);
    c.Bind("d_choices", &m.d_choices);
    c.Bind("d_asks", &m.d_asks);
    c.Bind("d_input", &m.d_input);
    c.Bind("toasts", &m.toasts);
    c.Bind("fps", &m.fps);
    c.Bind("playtime", &m.playtime);
    c.Bind("own_menu", &m.own_menu);

    auto on = [&](const char* name, std::function<void(Rml::Event&, const Rml::VariantList&)> fn) {
        c.BindEventCallback(name, [fn](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& args) { fn(ev, args); });
    };
    auto arg_str = [](const Rml::VariantList& a, usize i) { return i < a.size() ? a[i].Get<Rml::String>() : Rml::String(); };
    auto loading = [this](std::function<void()> work) { start_loading(std::move(work)); };

    on("new_game", [this, loading](Rml::Event&, const Rml::VariantList&) { loading([this] { new_game(); }); });
    on("continue_game", [this, loading](Rml::Event&, const Rml::VariantList&) { loading([this] { if (!continue_game()) show(Screen::Main); }); });
    on("open_slots", [this, arg_str](Rml::Event&, const Rml::VariantList& a) {
        if (screen_ == Screen::Main && slots_->list().empty()) return;
        model_->slots_mode = arg_str(a, 0) == "save" ? "save" : "load";
        if (screen_ != Screen::Slots) back_ = screen_;
        show(Screen::Slots);
    });
    on("slot_click", [this, arg_str, loading](Rml::Event&, const Rml::VariantList& a) {
        const std::string id = arg_str(a, 0);
        if (model_->slots_mode == "save") {
            std::string title = "Сохранение";
            for (const SlotInfo& s : slots_->list())
                if (s.id == id) title = s.autosave ? "Сохранение" : s.title;
            if (save(id, title)) show(back_);
        } else {
            loading([this, id] { if (!load(id)) show(Screen::Main); });
        }
    });
    on("slot_delete", [this, arg_str](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation(); // not a click on the row
        slots_->remove(arg_str(a, 0));
        model_->slots_dirty = true;
    });
    on("save_new", [this](Rml::Event&, const Rml::VariantList&) {
        const std::string id = slots_->new_id();
        if (save(id, "Сохранение " + id.substr(id.find('-') + 1))) show(back_);
    });
    on("open_settings", [this](Rml::Event&, const Rml::VariantList&) {
        back_ = screen_;
        show(Screen::Settings);
    });
    on("open_journal", [this](Rml::Event&, const Rml::VariantList&) {
        back_ = screen_;
        show(Screen::Journal);
    });
    on("back", [this](Rml::Event&, const Rml::VariantList&) { show(back_); });
    on("resume", [this](Rml::Event&, const Rml::VariantList&) { pause(false); });
    on("to_main_menu", [this](Rml::Event&, const Rml::VariantList&) { to_main_menu(); });
    on("quit", [this](Rml::Event&, const Rml::VariantList&) { quit_ = true; });
    on("toggle", [this, arg_str](Rml::Event&, const Rml::VariantList& a) {
        Settings s = settings_;
        const std::string what = arg_str(a, 0);
        if (what == "fullscreen") s.fullscreen = !s.fullscreen;
        if (what == "vsync") s.vsync = !s.vsync;
        if (what == "show_fps") s.show_fps = !s.show_fps;
        apply_settings(s);
    });
    on("scale", [this](Rml::Event&, const Rml::VariantList& a) {
        Settings s = settings_;
        const int step = a.empty() ? 0 : a[0].Get<int>();
        s.ui_scale = std::round((s.ui_scale + 0.1f * static_cast<f32>(step)) * 10.0f) / 10.0f;
        apply_settings(s);
    });
    on("set_theme", [this, arg_str](Rml::Event&, const Rml::VariantList& a) {
        Settings s = settings_;
        s.theme = arg_str(a, 0);
        apply_settings(s);
    });
    on("choose", [this](Rml::Event&, const Rml::VariantList& a) {
        if (!a.empty()) runner_->choose(static_cast<u32>(a[0].Get<int>()));
    });
    on("advance", [this](Rml::Event&, const Rml::VariantList&) { runner_->advance(); });
    on("ask", [this](Rml::Event&, const Rml::VariantList&) {
        if (!model_->d_input.empty()) runner_->ask(model_->d_input);
        model_->d_input.clear();
    });
    on("ask_change", [this](Rml::Event& ev, const Rml::VariantList&) {
        if (!ev.GetParameter<bool>("linebreak", false)) return;
        const Rml::String typed = ev.GetParameter<Rml::String>("value", model_->d_input);
        if (!typed.empty()) runner_->ask(typed);
        model_->d_input.clear();
    });
    m.handle = c.GetModelHandle();

    for (const std::string& t : ui_->themes())
        if (const char* name = theme_name(t)) m.themes.push_back({t, name});
}

void Shell::refresh_model() {
    Model& m = *model_;
    // Menus are small: everything is compared and rewritten each frame, and
    // RmlUi redraws only what changed.
    bool dirty = false;
    auto set = [&](auto& field, auto value) {
        if (field != value) {
            field = std::move(value);
            dirty = true;
        }
    };
    set(m.screen, Rml::String(screen_key(screen_)));
    set(m.own_menu, screens_ && screens_->has_menu());
    set(m.title, Rml::String(title_));

    if (m.slots_dirty) {
        m.slots_dirty = false;
        const std::vector<SlotInfo> list = slots_->list();
        m.slots.clear();
        for (const SlotInfo& s : list)
            m.slots.push_back({s.id, s.title, s.location.empty() ? "—" : s.location, format_playtime(s.playtime_s),
                               format_date(s.saved_at), s.autosave});
        m.has_slots = !list.empty();
        m.has_continue = !list.empty();
        m.continue_text = list.empty() ? "" : list[0].title + " · " + (list[0].location.empty() ? "" : list[0].location + " · ") +
                                                  format_playtime(list[0].playtime_s);
        dirty = true;
    }

    set(m.fullscreen, settings_.fullscreen);
    set(m.vsync, settings_.vsync);
    set(m.show_fps, settings_.show_fps);
    set(m.ui_scale, static_cast<int>(std::lround(settings_.ui_scale * 100)));
    set(m.theme, Rml::String(ui_->theme()));

    if (m.seen_vars != vars_.version() || screen_ == Screen::Journal) {
        m.seen_vars = vars_.version();
        std::vector<QuestView> journal;
        Rml::String tracker_title, tracker_text;
        for (const JournalEntry& e : quests_.journal(vars_)) {
            journal.push_back({e.quest->title, e.text, e.state == QuestState::Done});
            if (tracker_text.empty() && e.state == QuestState::Active) {
                tracker_title = e.quest->title;
                tracker_text = e.text;
            }
        }
        if (journal.size() != m.journal.size() || tracker_text != m.tracker_text || tracker_title != m.tracker_title) dirty = true;
        else
            for (usize i = 0; i < journal.size(); ++i)
                if (journal[i].text != m.journal[i].text || journal[i].done != m.journal[i].done) dirty = true;
        m.journal = std::move(journal);
        m.tracker_title = tracker_title;
        m.tracker_text = tracker_text;
    }

    const bool talking = in_dialogue();
    set(m.in_dialogue, talking);
    if (talking) {
        const DialogueLine& l = runner_->line();
        set(m.d_speaker, Rml::String(l.speaker));
        set(m.d_color, Rml::String(l.color));
        set(m.d_text, Rml::String(l.text));
        set(m.d_asks, l.asks_keyword);
        std::vector<Rml::String> choices(l.choices.begin(), l.choices.end());
        set(m.d_choices, std::move(choices));
    }

    std::vector<Rml::String> toasts;
    for (const Toast& t : m.toast_list) toasts.push_back(t.text);
    set(m.toasts, std::move(toasts));

    m.fps_timer -= m.fps_avg;
    if (m.fps_timer <= 0) {
        m.fps_timer = 0.5;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.0f FPS · %.2f мс", m.fps_avg > 0 ? 1.0 / m.fps_avg : 0.0, m.fps_avg * 1000.0);
        set(m.fps, Rml::String(buf));
        set(m.playtime, Rml::String(format_playtime(playtime_)));
    }
    if (dirty) m.handle.DirtyAllVariables();
}

} // namespace forge::game
