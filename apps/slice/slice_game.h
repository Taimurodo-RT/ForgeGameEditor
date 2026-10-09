#pragma once

// The vertical slice: a small side-view game built the way a big one is.
// A streamed 64k × 64k world with saves, a hero who walks, jumps, swims,
// digs and builds, villagers with dialogues and two quests, critters,
// crates, water and sand, torches in the dark. Menus, saving and dialogues
// come from forge::game::Shell; this file is only the gameplay.

#include "demo_art.h"
#include "slice_level.h"
#include "slice_links.h"
#include "slice_sounds.h"
#include "slice_world.h"

#include "forge/game/shell.h"
#include "forge/logic/logic.h"
#include "forge/render/lighting.h"
#include "forge/render/particles.h"
#include "forge/render/sprite_renderer.h"
#include "forge/render/tilemap_renderer.h"
#include "forge/sim/simulation.h"

#include <memory>
#include <string>
#include <vector>

namespace slice {

// The hero's place and choices, next to the world in the save.
struct HeroSave {
    f64 x = 0, y = 0;
    u32 slot = 0;
    f32 zoom = 0;
};

struct Options {
    bool stress = false;          // the "all numbers at once" run
    u32 stress_critters = 200'000;
    u32 stress_particles = 1'000'000;
    // The level a new game starts from; empty: the game's own (data/level).
    std::filesystem::path level_dir;
    // Where a new game puts the hero (the tile point under its feet), for
    // «Играть отсюда» in the editor.
    bool at = false;
    f64 at_x = 0, at_y = 0;
    // No sound device (the self-test): sounds still "play" and are counted.
    bool silent = false;
    // Where to write the links that happen, one id a line (the editor's
    // «Логика» lights them up); empty: nowhere.
    std::filesystem::path fired_file;
    // «Связи» over the game (F2) may change the links: only when the editor
    // started it (--play) or in the self-test, never for players.
    bool edit_links = false;
    // The links' file; empty: the game's own logic.json.
    std::filesystem::path links_file;
};

// What the player does this frame: from the keyboard and mouse, or from a
// test (scripted).
struct Controls {
    bool left = false, right = false, jump = false;
    bool use = false;   // dig, break a crate
    bool place = false; // put the selected block
    f64 aim_x = 0, aim_y = 0; // the tile point under the cursor
};

// The hotbar: the tool, then blocks to place.
inline constexpr u32 kSlots = 6;

class SliceGame final : public forge::game::Game {
public:
    explicit SliceGame(const Options& options);
    ~SliceGame() override;

    bool init(forge::game::Shell& shell, SDL_GPUDevice* device, SDL_GPUTextureFormat format) override;
    bool begin(const std::filesystem::path& session, bool new_game, std::string* error) override;
    bool save(const std::filesystem::path& session, std::string& location, std::string* error) override;
    void end() override;
    bool running() const override { return running_; }
    void update(f64 dt, bool playing, bool input) override;
    void render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, u32 width, u32 height) override;
    void handle_event(const SDL_Event& event) override;
    void shutdown() override;

    // --- for the self-test ---
    // While scripted, the keyboard and mouse are ignored.
    void script(const Controls& c) {
        scripted_ = true;
        script_ = c;
    }
    void stop_script() { scripted_ = false; }
    // The level the next new game starts from, and where the hero stands
    // then (the self-test's own example levels).
    void set_level(const std::filesystem::path& dir, bool at = false, f64 x = 0, f64 y = 0) {
        options_.level_dir = dir;
        options_.at = at;
        options_.at_x = x;
        options_.at_y = y;
    }
    bool hero_alive() const;
    f64 hero_x() const;
    f64 hero_y() const;
    bool on_ground() const;
    // Moves the hero (the place is loaded first).
    void teleport(f64 x, f64 y);
    // Talks to the villager within reach; false when nobody is near.
    bool talk_nearest();
    void select(u32 slot) { slot_ = slot < kSlots ? slot : 0; }
    u32 slot() const { return slot_; }
    const SliceGenerator& generator() const { return *gen_; }
    forge::world::TileId tile(u32 layer, i32 x, i32 y) const;
    std::string location() const;
    u32 entities() const;
    u32 count_npcs() const;
    // Where a villager stands (who: 0 the miner, 1 the smith); NaN when not loaded.
    f64 npc_x(u8 who) const;
    u32 count_items(ItemKind kind) const;
    // The things the screens' lists know (names and pictures), as given to them.
    const std::vector<forge::game::ScreenItem>& screen_items() const { return screen_items_; }
    // A «Зверёк» copy moving by a scheme, and where it is now (NaN: gone).
    flecs::entity_t spawn_critter(f64 x, f64 feet_y, Scheme scheme);
    f64 critter_x(flecs::entity_t e) const;
    // Physics: a small body that falls and is pulled like anything else (a
    // probe), and what an entity felt in its last tick: where it is and the
    // pull on it (false when it is gone).
    flecs::entity_t spawn_probe(f64 x, f64 y);
    bool probe(flecs::entity_t e, f64& x, f64& y, f32& gx, f32& gy) const;
    // The world's pull this game runs with, the pull at a point now (world
    // and sources, as bodies feel it) and the gravity sources it found.
    void world_gravity(f32& x, f32& y) const;
    void pull_at(f64 x, f64 y, f32& gx, f32& gy) const;
    u32 gravity_sources() const;
    // The gravity points loaded now, with the editor's ids (0: none).
    struct Point {
        forge::u64 id = 0;
        f64 x = 0, y = 0;
        forge::sim::GravitySource source;
    };
    std::vector<Point> points() const;
    // The item nearest to a point within radius (0: none), and where an
    // entity is now (false: gone).
    flecs::entity_t nearest_item(f64 x, f64 y, f64 radius) const;
    bool position_of(flecs::entity_t e, f64& x, f64& y) const;
    // Gives an object a «Звук» block.
    bool set_sounds(flecs::entity_t e, const Sounds& sounds);
    f64 inventory(const char* item) const;
    u32 particles() const { return particles_.stats().slots_used; }
    f64 sim_ms() const { return sim_ms_; }
    const SliceSounds& sounds() const { return sounds_; }
    SliceSounds& sounds() { return sounds_; }
    const forge::sim::SimStats* sim_stats() const;
    // Links («Ключ открывает Дверь»): the last hint said to the hero, the
    // hero's hearts, the door nearest to a point (NaN-free: false when none).
    const std::string& last_hint() const { return last_hint_; }
    f64 hearts() const;
    bool door_open(f64 x, f64 y, bool& open) const;
    // Reads verbs.json and logic.json again and applies them (the editor
    // changed the links).
    bool reload_links(std::string* error = nullptr);
    const forge::logic::Logic& links() const { return links_; }
    // «Связи» over the game (F2).
    LinkOverlay& overlay() { return overlay_; }

private:
    friend class SliceLogic;
    struct Level;
    std::unique_ptr<Level> make_level(const std::filesystem::path& save_folder, std::string* error);
    void spawn_hero(f64 x, f64 y);
    void find_hero();
    void load_around(f64 x, f64 y);
    void spawn_stress();
    std::vector<forge::world::Rect> focus() const;

    void read_input(bool input);
    void hero_tick(const forge::sim::TickContext& ctx);
    void act(f64 dt);
    void dig(f64 dt, i32 tx, i32 ty);
    void place(i32 tx, i32 ty);
    bool break_crate(f64 x, f64 y);
    void pickups();
    void follow_camera(f64 dt);
    void drift_camera(f64 dt);
    void update_hud(bool playing);
    void build_sprites();
    void emit_effects(f64 dt);
    void hero_sounds(f64 dt);

    f64 inv(const std::string& item) const;
    void give(const std::string& item, f64 n, bool announce);
    bool take(const std::string& item, f64 n);
    i32 nearest_npc(f64 reach, flecs::entity* out) const;
    // What links asked for during a tick, done after it (they may destroy,
    // move the hero, start a dialogue).
    void do_deeds();
    // A link happened: into Options::fired_file (each link at most twice a
    // second, so «always» links do not flood it).
    void note_fired(u32 link);
    std::filesystem::path links_path() const;
    // Writes the links and plays by them at once (from «Связи» over the game).
    void change_links(const forge::logic::Logic& after, const std::string& said);
    // Takes in what the editor changed in the links' file meanwhile.
    void watch_links();
    std::vector<Seen> seen_things() const;
    void hurt_hero(f64 n);

    Options options_;
    forge::game::Shell* shell_ = nullptr;
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUTextureFormat format_{};
    std::shared_ptr<SliceGenerator> gen_;
    // Object kinds and templates: before the levels, whose scenes use it.
    forge::objects::Library library_;
    std::vector<forge::game::ScreenItem> screen_items_;

    std::unique_ptr<Level> level_;
    bool running_ = false;
    std::filesystem::path session_;

    std::vector<u8> atlas_;
    forge::demo::SheetImage sheet_;
    Pictures pictures_;
    SliceSounds sounds_;
    forge::render::TilemapRenderer tiles_;
    const forge::world::World* tiles_world_ = nullptr;
    forge::render::SpriteRenderer sprites_;
    forge::render::SpriteBatch batch_;
    forge::render::LightRenderer lights_;
    forge::render::ParticleSystem particles_;
    u32 sprite_capacity_ = 0;
    forge::render::Camera2D camera_;
    u32 width_ = 1600, height_ = 900;

    Controls controls_, script_, last_;
    bool scripted_ = false;
    u32 slot_ = 0;
    f64 hero_x_ = 0, hero_y_ = 0; // last known, for systems running in parallel
    i32 dig_x_ = 0, dig_y_ = 0;
    f64 dig_progress_ = 0, dig_dust_ = 0, place_wait_ = 0;
    bool dig_warned_ = false;
    u32 dig_ticks_ = 0;
    // The hero as last heard: for steps, landings and splashes.
    bool jumped_ = false, was_ground_ = true, was_wet_ = false;
    f64 fall_ = 0, walked_ = 0;
    flecs::entity_t talking_ = 0;
    std::string location_;
    f64 location_wait_ = 0;
    f64 backdrop_t_ = 0;
    std::vector<std::pair<f64, f64>> torches_;
    f64 frame_dt_ = 0, frame_ms_avg_ = 0, sim_ms_ = 0, stress_log_ = 0;
    std::vector<forge::world::Rect> stress_rects_;

    struct Hud;
    std::unique_ptr<Hud> hud_;

    // Links: the game's verbs and links, what they do here, and what they
    // asked for in the last tick.
    forge::logic::Verbs verbs_;
    forge::logic::Logic links_;
    std::unique_ptr<forge::logic::Game> logic_;
    std::unique_ptr<forge::script::GameBridge> bridge_; // scripts' way to the shell's variables and screens
    std::FILE* fired_ = nullptr;
    std::unordered_map<u32, u64> fired_last_; // link -> ms
    struct Deed {
        std::string action, thing;
        flecs::entity_t target = 0, other = 0;
    };
    std::vector<Deed> deeds_;
    std::vector<std::string> hints_;
    std::vector<std::pair<flecs::entity_t, std::string>> cues_;
    std::string last_hint_;
    // The things links name (the hero and the templates), «Связи» over the
    // game, and the links that have just happened (link -> until, ms).
    std::vector<forge::logic::Thing> things_;
    LinkOverlay overlay_;
    std::unordered_map<u32, u64> lit_;
    std::filesystem::file_time_type links_time_{};
    u64 links_checked_ = 0;
};

} // namespace slice

FORGE_REFLECT_DECLARE(slice::HeroSave)
