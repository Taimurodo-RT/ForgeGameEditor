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
#include "forge/level/levels.h"
#include "forge/logic/logic.h"
#include "forge/render/lighting.h"
#include "forge/render/particles.h"
#include "forge/render/sprite_renderer.h"
#include "forge/render/tilemap_renderer.h"
#include "forge/sim/simulation.h"

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace slice {

// An area of the level as the links name it: "area:" and its id.
std::string area_thing_id(forge::u64 id);

// The hero's place and choices, next to the world in the save.
struct HeroSave {
    f64 x = 0, y = 0;
    u32 slot = 0;
    f32 zoom = 0;
    // The level the game plays: its id in the game's list (levels.json); empty for a folder that is no level of it.
    // A save made before levels has no such field: it played game/level, so that is what it reads as.
    std::string level{forge::level::kFirstLevel};
    // Where the hero goes back to on this level after spikes or a pit (14.2c): the point under its feet where it came
    // into the level. A save made before 14.2c has none: the level's spawn point, else where the hero is.
    bool back = false;
    f64 back_x = 0, back_y = 0;
};

struct Options {
    bool stress = false;          // the "all numbers at once" run
    u32 stress_critters = 200'000;
    u32 stress_particles = 1'000'000;
    // The level a new game starts from; empty: the game's start level (levels.json; data/level without it).
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
    // An ended game (won or lost) is never saved: «Продолжить» goes on from before its end.
    bool can_save(std::string* why) const override;
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
    // The level this game plays: its id in the game's list; empty for a folder that is no level of it.
    const std::string& level_id() const { return level_id_; }
    bool hero_alive() const;
    // The hero's entity (0: none); a going refused keeps the same one.
    flecs::entity_t hero_entity() const;
    f64 hero_x() const;
    f64 hero_y() const;
    bool on_ground() const;
    // The hero's frame drawn last (0 stands, 1 and 2 the steps, 3 in the air) and whether by its picture.
    u32 hero_frame() const { return hero_frame_; }
    bool hero_pictured() const { return hero_pictured_; }
    // The frame of its picture drawn last (of its animation in its pose); without a picture, hero_frame().
    u32 hero_picture_frame() const { return hero_picture_frame_; }
    Pose hero_pose() const { return hero_look_.pose; }
    // The tick it took that pose (its animation plays from then), and the level's tick of the picture drawn last.
    u64 hero_pose_since() const { return hero_look_.since; }
    u64 drawn_tick() const { return drawn_tick_; }
    // Where it was drawn last (between two ticks: the frame's own place), in tiles.
    f64 hero_drawn_x() const { return hero_drawn_x_; }
    f64 hero_drawn_y() const { return hero_drawn_y_; }
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
    // A «Зверёк» copy moving by a scheme (id: a level's id of its own, as spawn_copy), and where it is now (NaN: gone).
    flecs::entity_t spawn_critter(f64 x, f64 feet_y, Scheme scheme, forge::u64 id = 0);
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
    // Light: the level's sources loaded now (with the editor's ids), the hour
    // the level is lit at, and the frame's light (the camera it was made with,
    // the sources with a radius at a point). The hero's own little light can
    // be put out to compare with the editor, which has no hero.
    struct Lamp {
        forge::u64 id = 0;
        f64 x = 0, y = 0;
        forge::level::LightSource source;
    };
    std::vector<Lamp> light_sources() const;
    f32 level_hour() const;
    forge::render::LightRenderer& lights() { return lights_; }
    forge::render::Camera2D& camera() { return camera_; }
    void set_hero_light(bool on) { hero_light_ = on; }
    SDL_GPUDevice* device() const { return device_; }
    // The size of the last frame drawn, and the world as loaded now.
    u32 frame_width() const { return width_; }
    u32 frame_height() const { return height_; }
    const forge::world::World* world() const;
    // The item nearest to a point within radius (0: none), and where an
    // entity is now (false: gone).
    flecs::entity_t nearest_item(f64 x, f64 y, f64 radius) const;
    bool position_of(flecs::entity_t e, f64& x, f64& y) const;
    // The whole width and height of an entity's body, in tiles (false: it has none).
    bool body_size(flecs::entity_t e, f64& w, f64& h) const;
    // Gives an object a «Звук» block.
    bool set_sounds(flecs::entity_t e, const Sounds& sounds);
    // The loaded copies of a template (by its id).
    std::vector<flecs::entity_t> copies_of(std::string_view template_id) const;
    // The game's templates, and the picture the copies of one are drawn with (null: their usual frame).
    const forge::objects::Library& library() const { return library_; }
    const Pictures::Picture* picture_of(std::string_view template_id) const;
    // A copy of a template with a level's id of its own, as one the author put on the level (the id stays with it
    // through saves), and the copy of that id in the level now (0: none).
    flecs::entity_t spawn_copy(std::string_view template_id, f64 x, f64 feet_y, forge::u64 id);
    flecs::entity_t copy_with_id(forge::u64 id) const;
    // The hero remembers that a link of an area happened («Только один раз»).
    bool hero_marked(u32 link) const;
    // Hurts the hero by n hearts (n < 0 heals), as a link does.
    void hurt(f64 n) {
        hurt_hero(n);
        settle();
    }
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
    // Areas («Зоны»): the level's (areas.json as it came into the game), the
    // ones the hero is in now, how often it came into and left each since the
    // game began or was loaded, and the entity an area's links run on.
    const forge::level::LevelAreas* areas() const;
    std::vector<forge::u64> areas_inside() const;
    u32 area_enters(forge::u64 id) const;
    u32 area_leaves(forge::u64 id) const;
    flecs::entity_t area_entity(forge::u64 id) const;
    // Reads verbs.json and logic.json again and applies them (the editor
    // changed the links).
    bool reload_links(std::string* error = nullptr);
    const forge::logic::Logic& links() const { return links_; }
    // The links' file the game plays by: Options::links_file, else the game's logic.json.
    std::filesystem::path links_path() const;
    // «Связи» over the game (F2).
    LinkOverlay& overlay() { return overlay_; }

    // Going to another level («уходит на уровень»): a link asks, the game goes after the tick
    // (14.2-платформер-модель.md, «Порядок перехода в игре»). go_to asks as a link does (link 0); the first ask
    // of a tick wins. A going not done (refused, or dropped) leaves the game and the session as they were and its
    // link's «Только один раз» unmarked (however long its code waited before asking). travels, travels_dropped,
    // travels_refused: how many went, were dropped, were refused since the game began or was loaded; travel_problem:
    // why the last one did not go ("" when it went).
    void go_to(std::string level, std::string arrive = {}) { ask_travel(std::move(level), std::move(arrive), 0); }
    u32 travels() const { return travels_; }
    const std::string& travel_problem() const { return travel_problem_; }
    u32 travels_dropped() const { return travels_dropped_; }
    u32 travels_refused() const { return travels_refused_; }
    // For the self-test: the next going fails at this step as if the disk refused (Save: the left level is not
    // written; Move: the session's world does not move aside; Make: the level gone to does not open; Back: the level
    // gone to does not move into the session, nor does the session's world move back).
    enum class TravelFault : u8 { None, Save, Move, Make, Back };
    void fail_next_travel(TravelFault f) { travel_fault_ = f; }
    // The screens the level's links and schemes opened and have not closed (a going closes them).
    const std::set<std::string>& level_screens() const { return level_screens_; }

    // The platformer's rules (14.2-платформер-модель.md, «Правила платформера (14.2c)»): the game's score
    // (hero.score), its end (won or lost: the world stands), the safe time left after a heart lost or the hero placed
    // (blinking: after a heart lost), the point it goes back to (under its feet), and how many enemies it beat, times
    // enemies and hazards took hearts, pits it fell into and times it went back since the game began or was loaded.
    f64 score() const;
    bool won() const { return ended_ == Ending::Won; }
    bool lost() const { return ended_ == Ending::Lost; }
    f64 safe_time() const { return safe_; }
    bool blinking() const { return blink_ && safe_ > 0; }
    bool back_point(f64& x, f64& y) const;
    u32 stomps() const { return stomps_; }
    // Enemies met in the tick of a stomp: they do not hurt while the hero stays in them.
    u32 spared() const { return static_cast<u32>(spared_.size()); }
    u32 enemy_hits() const { return enemy_hits_; }
    u32 hazard_hits() const { return hazard_hits_; }
    u32 falls() const { return falls_; }
    u32 backs() const { return backs_; }
    // Times a return found no place that would do (the hero stayed where it was).
    u32 nowheres() const { return nowheres_; }
    // How many times the game ended since it began or was loaded (at most once).
    u32 endings() const { return endings_; }

private:
    friend class SliceLogic;
    struct Level;
    std::unique_ptr<Level> make_level(const std::filesystem::path& save_folder, std::string* error);
    // The level's own tiles for the whole game (which stop the hero and light, how they look): make_level sets them
    // for the level it makes; a going that did not happen sets them back for the level played.
    void use_tiles(const Level& level);
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
    // Going to another level: asked during a tick (the first ask wins), done after the links' deeds.
    void ask_travel(std::string level, std::string arrive, u32 link);
    void travel();
    // Where the hero comes out on the level now loaded: an area's middle, else its spawn point, else the game's
    // start (both stood on the floor); the hero's feet.
    void arrival(const std::string& arrive, f64& x, f64& y);
    // The hero as it leaves a level, and back in one (its variables: «Только один раз» of areas).
    struct Carried {
        f64 x = 0, y = 0;
        f32 facing = 1;
        bool has_vars = false;
        forge::script::ScriptVars vars;
    };
    Carried carry_hero() const;
    void put_hero(const Carried& c, f64 x, f64 y);
    // A link happened: into Options::fired_file (each link at most twice a
    // second, so «always» links do not flood it).
    void note_fired(u32 link);
    // Writes the links and plays by them at once (from «Связи» over the game).
    void change_links(const forge::logic::Logic& after, const std::string& said);
    // Takes in what the editor changed in the links' file meanwhile.
    void watch_links();
    std::vector<Seen> seen_things() const;
    // n > 0 hurts, n < 0 heals; with no hearts left it asks settle for the end (or the old waking up).
    void hurt_hero(f64 n);
    // The platformer's rules: enemies and hazards touched this tick (a system before hero_tick); hearts lost to
    // them or a pit (the safe second after); what they leave for after the ticks (the end, or the old waking up; the
    // hero put back); the game's end; where the hero may come back to; an area links make a pit.
    void contacts_tick(const forge::sim::TickContext& ctx);
    void lose_hearts(f64 n);
    void settle();
    void wake_up(bool fell);
    void hearts_out(bool fell);
    bool go_back();
    const char* return_spot(bool waking, f64& x, f64& y);
    bool in_pit(f64 x, f64 y) const;
    void finish(bool won);
    bool back_spot(f64 x, f64 y, f64& out_x, f64& out_y, std::string* why);
    bool is_pit(forge::u64 area) const;
    void add_score(f64 n);
    // Where the hero came into the level (feet), and the safe time without blinking it starts there with.
    void came_in(f64 x, f64 feet_y);
    // Areas: who came in and went out this tick; the area a link names; where
    // a spawn point puts the hero (false: why, then the game's start).
    void areas_tick(const forge::sim::TickContext& ctx);
    const forge::level::Area* area_of(std::string_view thing) const;
    bool spawn_spot(f64 x, f64 y, f64& out_x, f64& out_y, std::string* why);
    // The game's name of the place at (x, y) ("Деревня"); "" where the level
    // has nothing around it.
    std::string place_name(f64 x, f64 y) const;

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
    std::string level_id_; // HeroSave::level

    TileArt atlas_; // the game's tiles and the level's own
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
    // Its frame (HeroLook), the tick of its last jump's push, the frame drawn last (0..3), whether by its picture and
    // where.
    HeroLook hero_look_;
    f32 zoom_ = 30; // a new game's view: pixels per tile
    u64 jump_tick_ = HeroLook::kNever;
    u32 hero_frame_ = 0;
    bool hero_pictured_ = false;
    u32 hero_picture_frame_ = 0;
    u64 drawn_tick_ = 0;
    f64 hero_drawn_x_ = 0, hero_drawn_y_ = 0;
    f64 fall_ = 0, walked_ = 0;
    flecs::entity_t talking_ = 0;
    std::string location_;
    f64 location_wait_ = 0;
    f64 backdrop_t_ = 0;
    std::vector<std::pair<f64, f64>> torches_;
    bool hero_light_ = true;
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
    struct Trip {
        std::string level, arrive;
        u32 link = 0;
        u64 call = 0; // the links' call that asked (logic::Runtime::asking)
    };
    std::optional<Trip> trip_;
    u32 travels_ = 0, travels_dropped_ = 0, travels_refused_ = 0;
    std::string travel_problem_;
    TravelFault travel_fault_ = TravelFault::None;
    std::set<std::string> level_screens_; // opened by this level's links and schemes
    std::string last_hint_;
    // The things links name (the hero and the templates), «Связи» over the
    // game, and the links that have just happened (link -> until, ms).
    std::vector<forge::logic::Thing> things_;
    LinkOverlay overlay_;
    std::unordered_map<u32, u64> lit_;
    std::unordered_map<forge::u64, u32> area_enters_, area_leaves_;
    std::filesystem::file_time_type links_time_{};
    u64 links_checked_ = 0;

    // The platformer's rules (14.2c).
    enum class Ending : u8 { None, Won, Lost };
    Ending ended_ = Ending::None;
    f64 safe_ = 0;      // game seconds the hero loses no heart
    bool blink_ = false; // that time came from a heart lost (the hero blinks)
    bool has_back_ = false;
    f64 back_x_ = 0, back_y_ = 0; // the return point: under the hero's feet
    bool back_asked_ = false;     // spikes or a pit: back after the ticks
    bool fell_ = false;           // that back is from a pit (nothing to stand on there)
    bool nowhere_ = false;        // no place to go back to for this touch of hazards: not looked for until off them
    bool has_ground_ = false;     // where the hero last stood safely on the floor of this level (not saved)
    f64 ground_x_ = 0, ground_y_ = 0;
    bool placed_ = false;         // teleported since the last tick: the body's contacts are from before
    bool out_of_hearts_ = false;  // the last heart went: the end (or the old waking up) after the ticks
    u32 stomps_ = 0, enemy_hits_ = 0, hazard_hits_ = 0, falls_ = 0, backs_ = 0, endings_ = 0, nowheres_ = 0;
    // Enemies the hero was in when it beat another: they do not hurt it until it is out of them.
    std::vector<flecs::entity_t> spared_;
};

} // namespace slice

FORGE_REFLECT_DECLARE(slice::HeroSave)
