#pragma once

// The game of the module «Три в ряд» (step 14.3b, 14.3-три-в-ряд-модель.md): a level is a field the author drew,
// played with the mouse. The rules are match3_rules.h; this is what is around them: the levels in their order, the
// links of «Логика» on the module's events, the values the screens show, the saves, the animation of what the rules
// did, and the windows of a level passed, of the game won and lost.
//
// Each field played has a number of its own (its generation): «Дальше» and «Ещё раз» close it and start the next
// one, and whatever the old field left to happen later (a window after its last animation) is dropped, so nothing of
// it happens on the new one.

#include "match3_level.h"

#include "forge/audio/audio.h"
#include "forge/audio/screen_sounds.h"
#include "forge/game/shell.h"
#include "forge/level/levels.h"
#include "forge/logic/logic.h"
#include "forge/modules/modules.h"
#include "forge/render/camera.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace match3 {

// The module's events (verbs.json "when") and the values its screens show.
const std::vector<forge::modules::Event>& events();
std::vector<std::string> event_ids();
std::vector<forge::modules::Value> values(const forge::objects::Library* library);

// How long things take on screen (seconds), and the pull gems fall with (tiles / s²).
inline constexpr f64 kSwapTime = 0.15;
inline constexpr f64 kVanishTime = 0.2;
inline constexpr f64 kFallPull = 60;
// A level's end shows its window this long after the board settled: the player sees the last move first.
inline constexpr f64 kEndDelay = 0.4;

struct Options {
    // The level a new game starts from (a folder of the game's levels); empty: the start level of levels.json.
    std::filesystem::path level_dir;
    // No sound device (the self-test): sounds still "play" and are counted.
    bool silent = false;
};

class Match3Game final : public forge::game::Game {
public:
    // What the board on screen is doing.
    enum class Phase : u8 { Calm, Swap, SwapBack, Vanish, Fall };
    // How the game stands: playing, a level passed (its window up), the game won, lost.
    enum class Outcome : u8 { Playing, LevelPassed, Won, Lost };

    explicit Match3Game(const Options& options);
    ~Match3Game() override;

    bool init(forge::game::Shell& shell, SDL_GPUDevice* device, SDL_GPUTextureFormat format) override;
    bool begin(const std::filesystem::path& session, bool new_game, std::string* error) override;
    bool save(const std::filesystem::path& session, std::string& location, std::string* error) override;
    void end() override;
    bool running() const override { return running_; }
    // Between moves only: «Сохранить можно между ходами: фишки ещё падают». A level lost or the game won is not saved.
    bool can_save(std::string* why) const override;
    void update(f64 dt, bool playing, bool input) override;
    void render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, u32 width, u32 height) override;
    void handle_event(const SDL_Event& event) override;
    void shutdown() override;
    bool next_level() override;
    bool retry_level() override;

    // --- for the self-test ---
    struct Said {
        u64 generation = 0;
        Event event;
    };
    // The board's events as the game took them (of the field playing then), and the links that ran (their ids).
    const std::vector<Said>& said() const { return said_; }
    const std::vector<u32>& fired() const { return fired_; }
    // Things an old field left to happen later, dropped because a new field began (retry, next, a load).
    u32 stale() const { return stale_; }
    // Events of an old field that reached the game after it was closed (none, ever: what the checks look at).
    u32 stale_events() const { return stale_events_; }
    const Play* play() const;
    const Field* field() const;
    const GemSet& gems() const { return set_; }
    Phase phase() const;
    // How far the phase has gone, 0 … 1.
    f64 phase_progress() const;
    u64 generation() const;
    const std::string& level_id() const;
    u32 level_number() const;
    Outcome outcome() const { return outcome_; }
    // The cell picked by a click (-1: none).
    i32 picked() const;
    // Where a cell's middle is on the screen of the last frame (pixels), and a cell's side there.
    bool cell_on_screen(i32 cell, f32& sx, f32& sy) const;
    f32 cell_pixels() const { return camera_.zoom; }
    // The board as drawn this frame: each cell's gem (kNone, kHole) and where its middle is (tiles), how opaque.
    struct Drawn {
        u8 gem = kNone;
        f64 x = 0, y = 0;
        f32 alpha = 1, scale = 1;
        u32 frame = 0; // the gem's own: 0 stands, 1… vanish
    };
    const std::vector<Drawn>& drawn() const { return drawn_; }
    u32 frame_width() const { return width_; }
    u32 frame_height() const { return height_; }
    SDL_GPUDevice* device() const { return device_; }
    const Painter& painter() const { return painter_; }
    // Problems of the game's data (its objects, verbs, links, a level that would not start), in words.
    const std::vector<std::string>& problems() const { return problems_; }
    u32 sounds_played() const { return sounds_played_; }
    // Why the last begin() refused (a save that does not fit the game, a level that would not start).
    const std::string& refusal() const { return refusal_; }
    // How many times a level of this game ended (passed, the game won or lost): once a level, whatever links say.
    u32 ends() const { return ends_; }
    // The level a new game starts from (the self-test's levels).
    void set_level(const std::filesystem::path& dir) { options_.level_dir = dir; }
    // Breaks a rule on purpose, so a check is seen to fail without it (--break).
    enum class Break : u8 { None, StaleCall, RewardTwice, NoSeedSaved };
    void set_break(Break b) { break_ = b; }

private:
    struct Live; // a field being played
    // Starts the level `id` (its folder; id "" for a folder that is no level of the game) as a new field, the next
    // generation. False, why in words.
    bool start_level(const std::string& id, const std::filesystem::path& folder, std::string& why);
    bool read_level(const std::string& id, const std::filesystem::path& folder, Field& field, std::string& name, std::string& why);
    void on_board(u64 generation, const Event& e);
    void act(const forge::logic::Link& link, const forge::logic::VerbDef& verb);
    void later(f64 delay, std::function<void()> fn);
    void level_won();
    void finish(bool won, const std::string& why);
    void advance(f64 dt);
    void cascade();
    void try_swap(i32 a, i32 b);
    i32 cell_at(f32 sx, f32 sy) const;
    void set_values();
    void fit_camera(u32 width, u32 height);
    void push_board();
    void play_sound(const std::string& name);

    Options options_;
    Break break_ = Break::None;
    forge::game::Shell* shell_ = nullptr;
    SDL_GPUDevice* device_ = nullptr;
    Match3Level module_;
    GemSet set_;
    forge::logic::Verbs verbs_;
    forge::logic::Logic links_;
    forge::level::LevelList levels_;
    Painter painter_;
    forge::render::Camera2D camera_;
    std::unique_ptr<Live> field_;
    u64 generations_ = 0; // the last field's number
    struct Later {
        u64 generation = 0;
        f64 at = 0;
        std::function<void()> fn;
    };
    std::vector<Later> later_;
    f64 clock_ = 0; // the game's time while it plays (pauses stop it)
    Outcome outcome_ = Outcome::Playing;
    bool running_ = false;
    std::vector<Said> said_;
    std::vector<u32> fired_;
    u32 stale_ = 0, stale_events_ = 0;
    std::vector<std::string> problems_;
    std::string refusal_;
    u32 ends_ = 0;
    std::vector<Drawn> drawn_;
    u32 width_ = 0, height_ = 0;
    // The mouse: the cell pressed on and whether it is held (a drag swaps with the neighbour it comes onto).
    i32 pressed_ = -1;
    bool held_ = false;
    // Sound.
    forge::audio::Mixer mixer_;
    forge::audio::ScreenSounds screen_sounds_;
    std::unordered_map<std::string, forge::audio::SoundPtr> named_;
    u32 sounds_played_ = 0;
    std::unordered_map<std::string, bool> said_once_;
};

} // namespace match3
