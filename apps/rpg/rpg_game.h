#pragma once

// The game of the module «RPG сверху» (step 14.4a, 14.4-rpg-модель.md): the hero walks a map in eight directions,
// talks to villagers (the shell's dialogues), picks things up into the inventory (the variables inv.*), comes to
// places; the quest is the game's data (quests.json and the dialogues). The rules of walking are rpg_rules.h; this is
// what is around them: the maps in their order, the links of «Логика» on the module's events, the values the screens
// show, the saves, the drawing, and the windows of a map passed, of the game won and lost.

#include "rpg_level.h"

#include "forge/audio/audio.h"
#include "forge/audio/screen_sounds.h"
#include "forge/game/shell.h"
#include "forge/level/levels.h"
#include "forge/logic/logic.h"
#include "forge/modules/modules.h"
#include "forge/render/camera.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace rpg {

// The module's events (verbs.json "when"), the values its screens show and the actions of its player.
const std::vector<forge::modules::Event>& events();
std::vector<std::string> event_ids();
std::vector<forge::modules::Value> values(const forge::objects::Library* library);
const std::vector<forge::input::ActionDef>& actions();

// Pixels of the screen a cell of the map is, unless game.json says ("zoom").
inline constexpr f32 kZoom = 48;

struct Options {
    // The map a new game starts on (a folder of the game's levels); empty: the start level of levels.json.
    std::filesystem::path level_dir;
    // No sound device (the self-test).
    bool silent = false;
};

class RpgGame final : public forge::game::Game {
public:
    // How the game stands: playing, a map passed (its window up), the game won, lost.
    enum class Outcome : u8 { Playing, LevelPassed, Won, Lost };

    explicit RpgGame(const Options& options);
    ~RpgGame() override;

    bool init(forge::game::Shell& shell, SDL_GPUDevice* device, SDL_GPUTextureFormat format) override;
    bool begin(const std::filesystem::path& session, bool new_game, std::string* error) override;
    bool save(const std::filesystem::path& session, std::string& location, std::string* error) override;
    void end() override;
    bool running() const override { return running_; }
    // A game won or lost is not saved.
    bool can_save(std::string* why) const override;
    void update(f64 dt, bool playing, bool input) override;
    void render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, u32 width, u32 height) override;
    void handle_event(const SDL_Event& event) override;
    void shutdown() override;
    bool next_level() override;
    std::vector<forge::input::ActionDef> actions() const override { return rpg::actions(); }

    // --- for the self-test ---
    // The hero now: feet, the way he looks, whether he walked this frame.
    f64 hero_x() const { return hero_x_; }
    f64 hero_y() const { return hero_y_; }
    Dir facing() const { return facing_; }
    bool walking() const { return walking_; }
    const Moved& last_move() const { return last_move_; }
    // The map being played (null: none) and its things as they are now.
    const MapData* map() const { return map_ ? &*map_ : nullptr; }
    const std::string& level_id() const { return level_; }
    const std::string& level_name() const { return level_name_; }
    // Whether a pickup of the map is gone (by its id), whether the hero stands in a place.
    bool gone(std::string_view id) const;
    bool inside(std::string_view id) const;
    // The villager the hero would talk to now (-1: none near), and the one he last talked to.
    i32 near_villager() const;
    const std::string& talked_to() const { return talked_; }
    // The module's events as they came: "pick:key", "reach:gate"; and the links that ran (their ids).
    const std::vector<std::string>& said() const { return said_; }
    const std::vector<forge::u32>& fired() const { return fired_; }
    // What the game said to the player at the top of the screen, in order.
    const std::vector<std::string>& toasts() const { return toasts_; }
    // The sounds of things that played (their files), in order. Without a sound device (the self-test) they still
    // "play" when their file is read.
    const std::vector<std::string>& sounds_played() const { return sounds_played_; }
    // Marks of the animations shown that fired (part 5а of docs/models/движение-модель.md): "name@who", who "hero" or
    // the thing's id, in order, the first kMarksKept of them. Their sounds are in sounds_played (RPG catches no emit).
    static constexpr forge::usize kMarksKept = 4096;
    const std::vector<std::string>& marks_fired() const { return marks_fired_; }
    Outcome outcome() const { return outcome_; }
    u32 ends() const { return ends_; }
    // Problems of the game's data, and why the last begin() refused.
    const std::vector<std::string>& problems() const { return problems_; }
    const std::string& refusal() const { return refusal_; }
    // Where a point of the map is on the screen of the last frame (pixels), and a cell's side there.
    void screen_of(f64 x, f64 y, f32& sx, f32& sy) const;
    f32 cell_pixels() const { return camera_.zoom; }
    u32 frame_width() const { return width_; }
    u32 frame_height() const { return height_; }
    SDL_GPUDevice* device() const { return device_; }
    const Painter& painter() const { return painter_; }
    const forge::objects::Library& library() const { return module_.library_ref(); }
    // The hero's frame of his picture drawn last, and whether he was drawn mirrored.
    u32 hero_frame() const { return hero_frame_; }
    bool hero_mirrored() const { return hero_mirrored_; }
    // Breaks a rule on purpose, so a check is seen to fail without it (--break).
    enum class Break : u8 { None, DiagonalFast, NoWalls, GoneNotSaved, PickTwice };
    void set_break(Break b) { break_ = b; }

private:
    bool start_level(const std::string& id, const std::filesystem::path& folder, std::string& why);
    bool read_level(const std::string& id, const std::filesystem::path& folder, MapData& out, std::string& name, std::string& why);
    std::vector<Box> bodies() const;
    void walk(f64 dt, bool input);
    void touch();
    void talk();
    void on_event(const std::string& kind, const Thing& b);
    void level_won();
    void finish(bool won);
    void set_values();
    void say(std::string text);
    void play_sound(const std::string& name);
    // After the ticks of a frame: the marks the hero's animation and the things' came to (anim_marks), each fired.
    void anim_marks(bool turned, u64 clock_was);
    void fire_mark(const forge::objects::ClipEvent& e, const Thing& who);
    void fit_camera(u32 width, u32 height);
    f64 inv(const std::string& item) const;
    void give(const std::string& item, f64 n, bool tell);
    bool take(const std::string& item, f64 n);
    usize level_index() const;

    Options options_;
    Break break_ = Break::None;
    forge::game::Shell* shell_ = nullptr;
    SDL_GPUDevice* device_ = nullptr;
    RpgLevel module_;
    forge::logic::Verbs verbs_;
    forge::logic::Logic links_;
    forge::level::LevelList levels_;
    Painter painter_;
    forge::render::Camera2D camera_;
    f32 zoom_ = kZoom;
    bool running_ = false;
    std::optional<MapData> map_;
    std::string level_, level_name_;
    std::filesystem::path folder_;
    // Grows with each map started: what happened on a map is answered on that map only.
    u64 map_gen_ = 0;
    f64 hero_x_ = 0, hero_y_ = 0;
    Dir facing_ = Dir::S;
    bool walking_ = false;
    Moved last_move_;
    u64 walk_ticks_ = 0, stand_ticks_ = 0; // since the hero began walking / standing (60 a second)
    f64 tick_left_ = 0;
    std::vector<std::string> gone_;   // pickups taken on this map
    std::vector<std::string> inside_; // places the hero stands in
    std::string talked_;
    std::vector<std::string> said_;
    std::vector<forge::u32> fired_;
    std::vector<std::string> toasts_;
    Outcome outcome_ = Outcome::Playing;
    // An end a dialogue asked for (win(), lose()): it comes when the dialogue is over.
    std::optional<bool> pending_end_;
    u32 ends_ = 0;
    std::vector<std::string> problems_;
    std::string refusal_;
    u32 width_ = 0, height_ = 0;
    u32 hero_frame_ = 0;
    bool hero_mirrored_ = false;
    u64 clock_ticks_ = 0;
    forge::audio::Mixer mixer_;
    forge::audio::ScreenSounds screen_sounds_;
    std::unordered_map<std::string, forge::audio::SoundPtr> named_; // a file that does not read: null
    std::vector<std::string> sounds_played_;
    // Where the marks were taken to: the ticks of the hero's animation (kTake: none yet on this map, the next look
    // takes them), and whether the things' were taken on this map.
    static constexpr forge::i64 kTake = -2;
    forge::i64 hero_marks_ = kTake;
    bool thing_marks_ = false;
    std::vector<std::string> marks_fired_;
    std::vector<std::string> marks_silent_; // sounds of marks not there: said once each
};

} // namespace rpg
