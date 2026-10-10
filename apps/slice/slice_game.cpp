#include "slice_game.h"

#include "slice_art.h"

#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/core/time.h"
#include "forge/data/json.h"
#include "forge/level/areas.h"
#include "forge/level/levels.h"
#include "forge/level/light.h"
#include "forge/script/host.h"
#include "forge/ui/ui.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

FORGE_REFLECT(slice::HeroSave, 1) {
    t.field("x", &slice::HeroSave::x);
    t.field("y", &slice::HeroSave::y);
    t.field("slot", &slice::HeroSave::slot);
    t.field("zoom", &slice::HeroSave::zoom);
    t.field("level", &slice::HeroSave::level);
    t.field("back", &slice::HeroSave::back);
    t.field("back_x", &slice::HeroSave::back_x);
    t.field("back_y", &slice::HeroSave::back_y);
}

namespace slice {

using namespace forge;
using namespace forge::world;
using namespace forge::sim;
using forge::game::Value;
using render::Sprite;
using scene::Position;
namespace fs = std::filesystem;

namespace {

constexpr u64 kSeed = 1;
constexpr f32 kWalk = 8.5f;
constexpr f32 kJump = 15.5f;
constexpr f32 kReach = 5.5f;     // tiles from the hero's centre to dig or build
constexpr f32 kTalkReach = 2.6f; // to a villager
constexpr f32 kZoom = 30;
constexpr f64 kHearts = 3;
// The platformer's rules (14.2-платформер-модель.md, «Касание врага»): the safe time after a heart lost, and after the
// hero is placed (a new game, a save loaded, a level come into); how a hurting enemy pushes the hero away.
constexpr f64 kSafe = 1.0;
constexpr f32 kKnockX = 6, kKnockY = 6;
// The places a hero is put back in (return_spot), as the log names them.
constexpr const char* kEntryPlace = "точка входа на уровень";
constexpr const char* kSpawnPlace = "точка появления уровня";
constexpr const char* kStartPlace = "старт игры";
constexpr const char* kGroundPlace = "место, где герой последний раз стоял на полу";

struct SlotDef {
    const char* item; // also the icon's colour class in hud.rcss: tone-<item>
    const char* icon;
    TileId tile;
    u32 layer;
};
const SlotDef kSlotDefs[kSlots] = {
    {"pickaxe", "hardware", TileAir, kBlocks},
    {"dirt", "landscape", TileDirt, kBlocks},
    {"stone", "hexagon", TileStone, kBlocks},
    {"sand", "grain", TileSand, kBlocks},
    {"wood", "carpenter", TilePlanks, kBlocks},
    {"torch", "local_fire_department", TileTorch, kWalls},
};

const char* item_title(const std::string& item) {
    if (item == "pickaxe") return "Кирка";
    if (item == "coins") return "Монеты";
    if (item == "dirt") return "Земля";
    if (item == "stone") return "Камень";
    if (item == "sand") return "Песок";
    if (item == "wood") return "Доски";
    if (item == "torch") return "Факелы";
    if (item == "copper") return "Медь";
    if (item == "iron") return "Железо";
    if (item == "gold") return "Золото";
    if (item == "key") return "Ключ";
    return "Предмет";
}

const char* kind_item(ItemKind k) {
    switch (k) {
    case ItemKind::Pickaxe: return "pickaxe";
    case ItemKind::Coins: return "coins";
    case ItemKind::Copper: return "copper";
    case ItemKind::Wood: return "wood";
    case ItemKind::Torch: return "torch";
    case ItemKind::Key: return "key";
    }
    return "";
}

const char* npc_dialogue(u8 who) { return who == 0 ? "miner" : "smith"; }
const char* npc_name(u8 who) { return who == 0 ? "Борис" : "Кузнец Мирон"; }

std::string with_spaces(u64 n) {
    std::string digits = std::to_string(n), out;
    for (usize i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += ' ';
        out += digits[i];
    }
    return out;
}

} // namespace

// --- level -------------------------------------------------------------------

struct SliceGame::Level {
    std::unique_ptr<World> world;
    std::unique_ptr<scene::Scene> scene;
    std::unique_ptr<Simulation> sim;
    // Scripts run after the game's systems; links are scripts too.
    std::unique_ptr<script::ScriptHost> scripts;
    std::unique_ptr<logic::Runtime> links;
    flecs::entity hero;
    // The time of day the author gave the level (light.json came with it into the save).
    forge::level::LevelLight light;
    // Its areas and spawn point (areas.json, the same way), and which areas
    // the hero is in.
    forge::level::LevelAreas areas;
    forge::level::AreaWatch watch;
    // What is around it (world.json) and its own tiles (tiles.json and
    // tiles.png), the same way.
    forge::level::LevelWorld around;
    forge::level::LevelTiles own;
    // Queries belong to the ECS world: declared last, released first.
    Objects objects;
};

struct SlotView {
    Rml::String icon, tone, count, key;
    bool selected = false, empty = false;
    bool operator==(const SlotView&) const = default;
};

struct SliceGame::Hud {
    Rml::DataModelHandle handle;
    bool visible = false;
    std::vector<SlotView> slots;
    Rml::String hint, location, coins, copper, stress;
    bool has_hint = false, stress_on = false;
};

SliceGame::SliceGame(const Options& options) : options_(options), gen_(std::make_shared<SliceGenerator>(kSeed)) {}
SliceGame::~SliceGame() {
    if (fired_) std::fclose(fired_);
}

std::unique_ptr<SliceGame::Level> SliceGame::make_level(const fs::path& save_folder, std::string* error) {
    auto L = std::make_unique<Level>();
    if (!save_folder.empty()) {
        if (std::string why; !forge::level::load_world(save_folder, L->around, nullptr, &why)) {
            FORGE_WARN("slice: %s; the game's world is around the level", why.c_str());
            L->around = {};
        }
        if (std::string why; !forge::level::load_tiles(save_folder, L->own, world_desc().layer_count, kLiquids, nullptr, &why)) {
            FORGE_WARN("slice: %s; the level has no tiles of its own", why.c_str());
            L->own = {};
        }
    }
    // Nothing around: the empty world, and nobody comes to live in it.
    std::shared_ptr<const Generator> around = gen_;
    if (L->around.empty_around) around = std::make_shared<EmptyGenerator>();
    L->world = std::make_unique<World>(world_desc(), std::move(around));
    if (!save_folder.empty() && !L->world->open_save(save_folder, error)) return nullptr;
    L->scene = std::make_unique<scene::Scene>(*L->world);
    register_components(*L->scene);
    // The editor's ids of the level's objects stay with them in the game and its saves.
    L->scene->register_component<forge::level::LevelId>();
    // And its light sources, placed in the «Свет» mode.
    L->scene->register_component<forge::level::LightSource>();
    attach_objects(library_, *L->scene);
    if (!save_folder.empty() && !L->scene->open_save(save_folder, error)) return nullptr;
    if (!L->around.empty_around)
        L->scene->set_populator([this](ChunkCoord c, scene::Scene& s) { populate(*gen_, library_, c, s); });
    // The level's own tiles: which stop the hero and light, how they look.
    use_tiles(*L);

    SimDesc sd;
    sd.gravity_y = kGravity;
    sd.liquid_layer = kLiquids;
    L->sim = std::make_unique<Simulation>(*L->world, *L->scene, sd);
    setup_cells(L->sim->collision(), *L->sim->cells());
    // The level's world pull, as the author set it in the editor (physics.json
    // came with the level into the save); without it the game's own.
    if (!save_folder.empty()) {
        forge::level::LevelPhysics p{0, kGravity};
        if (std::string why; !forge::level::load_physics(save_folder, p, nullptr, &why))
            FORGE_WARN("slice: %s; the game's gravity is used", why.c_str());
        L->sim->set_gravity(p.gravity_x, p.gravity_y);
        if (std::string why; !forge::level::load_light(save_folder, L->light, nullptr, &why)) {
            FORGE_WARN("slice: %s; noon is used", why.c_str());
            L->light = {};
        }
        if (std::string why; !forge::level::load_areas(save_folder, L->areas, nullptr, &why)) {
            FORGE_WARN("slice: %s; the level has no areas", why.c_str());
            L->areas = {};
        }
    }

    flecs::world& ecs = L->scene->ecs();
    L->objects.init(ecs);

    Level* level = L.get();
    // Enemies and hazards the hero touches, from where both were before the last move: before the hero moves.
    L->sim->add_system([this](const TickContext& ctx) { contacts_tick(ctx); });
    L->sim->add_system([this](const TickContext& ctx) { hero_tick(ctx); });
    // Villagers stroll around their homes and turn to the hero while talking.
    L->sim->add_system([this, level](const TickContext& ctx) {
        const flecs::entity_t talking = talking_;
        const f64 hx = hero_x_;
        each_due(ctx, level->objects.npcs, [&](flecs::entity_t e, f32 dt, Position& p, Body& b, Npc& n) {
            if (e == talking) {
                b.vx = 0;
                n.dir = 0;
                n.facing = hx > p.tile_x() ? 1.0f : -1.0f;
                return;
            }
            n.timer -= dt;
            if (n.timer <= 0) {
                n.seed = hash32(n.seed, static_cast<u32>(ctx.tick));
                n.timer = 1.5f + static_cast<f32>(n.seed % 300) / 100.0f;
                n.dir = (n.seed >> 9) % 5 < 2 ? 0.0f : ((n.seed >> 12) & 1 ? 1.0f : -1.0f);
            }
            const f32 off = static_cast<f32>(p.tile_x()) - n.home_x;
            if (off > 3.0f && n.dir > 0) n.dir = -1;
            if (off < -3.0f && n.dir < 0) n.dir = 1;
            if ((n.dir > 0 && (b.contacts & HitRight)) || (n.dir < 0 && (b.contacts & HitLeft))) n.dir = -n.dir;
            if (n.dir != 0) n.facing = n.dir;
            b.vx = n.dir * 2.2f;
        });
    });
    // Critters move by their «Управление» scheme and hop over steps.
    L->sim->add_system([this, level](const TickContext& ctx) {
        const Controls keys = controls_;
        const f64 hx = hero_x_, hy = hero_y_;
        each_due(ctx, level->objects.critters, [&](flecs::entity_t, f32, Position& p, Body& b, Critter& c) {
            c.seed = hash32(c.seed, static_cast<u32>(ctx.tick));
            const bool ground = b.contacts & OnGround;
            // Which way it wants to go: -1, 0 or 1.
            f32 want = 0;
            switch (static_cast<Scheme>(c.scheme)) {
            case Scheme::Player:
                want = (keys.right ? 1.0f : 0.0f) - (keys.left ? 1.0f : 0.0f);
                if (keys.jump && ground) b.vy = -13.0f;
                break;
            case Scheme::Stand: break;
            case Scheme::Follow:
            case Scheme::Flee: {
                const f64 dx = hx - p.tile_x(), dy = hy - p.tile_y();
                const f64 d = std::hypot(dx, dy);
                const f32 to_hero = dx > 0 ? 1.0f : -1.0f;
                if (c.scheme == static_cast<u8>(Scheme::Follow)) {
                    if (d > 1.5 && d < 40) want = to_hero;
                } else if (d < 8) {
                    want = -to_hero;
                }
                break;
            }
            case Scheme::Wander:
            default:
                if ((c.seed & 511) == 0) c.dir = -c.dir;
                want = c.dir;
                break;
            }
            if (want != 0) c.dir = want;
            if (!ground) return;
            if (want == 0) {
                b.vx = 0;
                return;
            }
            const bool blocked = (want > 0 && (b.contacts & HitRight)) || (want < 0 && (b.contacts & HitLeft));
            if (blocked) {
                // Wanderers sometimes turn back instead; the others always try.
                if (c.scheme != static_cast<u8>(Scheme::Wander) || (c.seed & 3) != 0) {
                    b.vy = -13.0f;
                    b.vx = want * c.speed;
                    return;
                }
                c.dir = -c.dir;
                want = c.dir;
            }
            b.vx = want * c.speed;
        });
    });
    // Which areas the hero came into or left: their links run with this tick's scripts.
    L->sim->add_system([this](const TickContext& ctx) { areas_tick(ctx); });
    L->scripts = std::make_unique<script::ScriptHost>(*L->sim, *L->scene);
    L->scripts->set_user(script::kGameBridge, bridge_.get());
    L->links = std::make_unique<logic::Runtime>(*L->scripts, library_, *logic_);
    std::vector<logic::Thing> areas;
    for (const forge::level::Area& a : L->areas.areas) areas.push_back(logic::area_thing(area_thing_id(a.id), a.name, level_id_));
    // The game's other levels' areas: «Логика» is the game's, its links to them are not broken here, only idle.
    // And its levels, where links may send the hero: the list as read (an entry not read is no level).
    std::vector<logic::Thing> others, levels;
    const fs::path game = shell_ && !save_folder.empty() ? shell_->game_dir() : fs::path();
    const forge::level::LevelList list = game.empty() ? forge::level::LevelList{} : forge::level::read_levels(game);
    for (const forge::level::LevelEntry& e : list.levels) {
        levels.push_back(logic::level_thing(e.id, e.name));
        forge::level::LevelAreas there;
        if (!forge::level::load_areas(forge::level::level_folder(game, e.id), there, nullptr, nullptr)) continue;
        for (const forge::level::Area& a : there.areas)
            if (!L->areas.find(a.id))
                others.push_back(logic::area_thing(area_thing_id(a.id), a.name + " (" + e.name + ")", e.id));
    }
    L->links->set_areas(std::move(areas));
    L->links->set_other_areas(std::move(others));
    L->links->set_levels(std::move(levels));
    L->links->load(links_, verbs_);
    L->links->attach(*L->scene);
    return L;
}

void SliceGame::use_tiles(const Level& level) {
    set_own_solid(level.own);
    atlas_ = make_tile_art(level.own);
    lights_.set_rules(light_rules());
}

// --- links -------------------------------------------------------------------

// What links do in «Старой шахте». Everything that changes the game waits
// for the end of the tick (SliceGame::do_deeds).
class SliceLogic final : public logic::Game {
public:
    explicit SliceLogic(SliceGame& game) : g_(game) {}

    bool is_hero(flecs::entity_t e) override { return e && g_.level_ && g_.level_->hero.id() == e; }
    flecs::entity_t hero() override { return g_.level_ && g_.level_->hero.is_alive() ? g_.level_->hero.id() : 0; }
    // A pickup is carried as its item ("key"), anything else by its own id.
    bool has(flecs::entity_t, std::string_view thing) override {
        if (const objects::Template* t = g_.library_.find(thing))
            if (const objects::PropDef* what = g_.library_.prop_of(*t, "what"); what && g_.library_.has_block(*t, "pickup")) {
                std::string item = g_.library_.value(*t, *what);
                if (item.size() >= 2 && item.front() == '"') item = item.substr(1, item.size() - 2);
                return g_.inv(item) > 0;
            }
        return g_.inv(std::string(thing)) > 0;
    }
    bool act(std::string_view action, flecs::entity_t target, std::string_view thing, flecs::entity_t other,
             flecs::entity_t) override {
        static constexpr std::string_view known[] = {"collect", "open",   "close", "toggle", "hurt", "heal", "coin",
                                                     "talk",    "follow", "flee",  "arrive", "fall", "win"};
        if (std::find(std::begin(known), std::end(known), action) == std::end(known)) return false;
        g_.deeds_.push_back({std::string(action), std::string(thing), target, other});
        return true;
    }
    bool go(flecs::entity_t, std::string_view level, std::string_view arrive, u32 link) override {
        g_.ask_travel(std::string(level), std::string(arrive), link);
        return true;
    }
    void hint(flecs::entity_t, std::string_view text) override { g_.hints_.emplace_back(text); }
    void sound(flecs::entity_t at, std::string_view cue) override { g_.cues_.emplace_back(at, std::string(cue)); }
    // The level's hour (light.json; it does not run in the game), never the editor's «Просмотр».
    bool night() override { return g_.level_ && is_night(g_.level_->light.time); }
    void fired(u32 link) override { g_.note_fired(link); }

private:
    SliceGame& g_;
};

// Scripts and schemes reach the game's variables and screens through this.
class ShellBridge final : public script::GameBridge {
public:
    ShellBridge(game::Shell& shell, std::set<std::string>& opened) : s_(shell), opened_(opened) {}
    f64 var(std::string_view name, std::string* text) override {
        const game::Value v = s_.vars().get(name);
        if (text && v.is_text()) *text = v.text();
        return v.number();
    }
    void set_var(std::string_view name, f64 number) override { s_.vars().set(name, number); }
    void set_text(std::string_view name, std::string_view text) override { s_.vars().set(name, std::string(text)); }
    // The level's links and schemes open and close windows: the ones they leave open close when the hero leaves.
    void screen(std::string_view what, std::string_view name) override {
        if (what == "toggle") s_.screens().toggle(name);
        else s_.screens().show(name, what == "show");
        if (s_.screens().shown(name)) opened_.insert(std::string(name));
        else opened_.erase(std::string(name));
    }

private:
    game::Shell& s_;
    std::set<std::string>& opened_;
};

void SliceGame::note_fired(u32 link) {
    const u64 now = SDL_GetTicks();
    lit_[link] = now + 1500; // for «Связи» over the game
    if (options_.fired_file.empty()) return;
    u64& last = fired_last_[link];
    if (last && now - last < 500) return;
    last = now;
    if (!fired_) {
        // A new file for each run.
        std::error_code ec;
        std::filesystem::create_directories(options_.fired_file.parent_path(), ec);
#ifdef _WIN32
        fired_ = _wfopen(options_.fired_file.c_str(), L"wb");
#else
        fired_ = std::fopen(options_.fired_file.c_str(), "wb");
#endif
        if (!fired_) return;
        // Which run this is: the editor starts over when it changes.
        std::fprintf(fired_, "run %llu\n", static_cast<unsigned long long>(SDL_GetPerformanceCounter()));
    }
    std::fprintf(fired_, "%u\n", link);
    std::fflush(fired_);
}

void SliceGame::do_deeds() {
    if (!level_) return;
    flecs::world& ecs = level_->scene->ecs();
    auto alive = [&](flecs::entity_t e) { return e && ecs.is_alive(e); };
    std::vector<Deed> deeds;
    deeds.swap(deeds_);
    for (const Deed& d : deeds) {
        // An ended game does nothing more: the links' deeds after its end are dropped.
        if (ended_ != Ending::None) break;
        if (d.action == "hurt" || d.action == "heal") {
            if (d.target == level_->hero.id()) {
                hurt_hero(d.action == "hurt" ? 1 : -1);
                settle(); // the last heart: the end now, before a deed after it (a goal reached in the same step)
            }
            continue;
        }
        if (d.action == "fall") { // into a pit: a heart (not in the safe time) and back where it came into the level
            if (d.target == level_->hero.id()) {
                ++falls_;
                if (safe_ <= 0) lose_hearts(1);
                back_asked_ = fell_ = true;
                settle();
            }
            continue;
        }
        if (d.action == "win") {
            if (d.target == level_->hero.id()) finish(true);
            continue;
        }
        if (d.action == "coin") {
            give("coins", 1, true);
            continue;
        }
        if (d.action == "arrive") { // the hero came into an area: its name
            if (const forge::level::Area* a = area_of(d.thing)) {
                shell_->toast(a->name);
                last_hint_ = a->name;
            }
            continue;
        }
        if (!alive(d.target)) continue;
        flecs::entity e = ecs.entity(d.target);
        if (d.action == "open" || d.action == "close" || d.action == "toggle") {
            if (Door* door = e.try_get_mut<Door>()) {
                const bool was = door->open;
                door->open = d.action == "open" ? true : d.action == "close" ? false : !door->open;
                if (door->open != was) e.modified<Door>();
            }
        } else if (d.action == "collect") {
            if (const Item* item = e.try_get<Item>()) {
                give(kind_item(static_cast<ItemKind>(item->kind)), item->count, true);
                add_score(item->score);
            } else {
                give(d.thing, 1, true);
            }
            e.destruct();
        } else if (d.action == "talk") {
            if (const Npc* n = e.try_get<Npc>(); n && !shell_->in_dialogue() && shell_->talk(npc_dialogue(n->who))) {
                talking_ = e.id();
                sounds_.play(Cue::Talk, hero_x_, hero_y_);
            }
        } else if (d.action == "follow" || d.action == "flee") {
            if (Critter* c = e.try_get_mut<Critter>())
                c->scheme = static_cast<u8>(d.action == "follow" ? Scheme::Follow : Scheme::Flee);
        }
    }
    for (const std::string& h : hints_) {
        shell_->toast(h);
        last_hint_ = h;
    }
    hints_.clear();
    for (const auto& [at, cue] : cues_) {
        f64 x = hero_x_, y = hero_y_;
        if (alive(at))
            if (const Position* p = ecs.entity(at).try_get<Position>()) x = p->tile_x(), y = p->tile_y();
        const Cue c = cue == "pickup" ? Cue::Pickup : cue == "coins" ? Cue::Coins : cue == "open" ? Cue::Crate : Cue::Place;
        sounds_.play(c, x, y);
    }
    cues_.clear();
}

// --- going to another level ----------------------------------------------------
// 14.2-платформер-модель.md, «Порядок перехода в игре»: everything the going writes is made beside the session's
// folders, and the level gone to is made from its copy while the left one still plays; only then the folders take
// their places, and the left level is destroyed whole (SliceGame::end without leaving the game). A going refused
// before that leaves the game and the session as they were.

void SliceGame::ask_travel(std::string level, std::string arrive, u32 link) {
    if (!running_ || !level_) return;
    const u64 call = level_->links->asking();
    if (ended_ != Ending::None) { // the game is over: nobody goes anywhere
        level_->links->take_back_first(call, link);
        ++travels_dropped_;
        return;
    }
    if (trip_) {
        ++travels_dropped_;
        // Its «Только один раз» does not stand: it did not send the hero anywhere.
        level_->links->take_back_first(call, link);
        FORGE_INFO("slice: переход на «%s» (связь %u) не нужен: в этом шаге герой уже уходит на «%s»", level.c_str(), link,
                   trip_->level.c_str());
        return;
    }
    trip_ = Trip{std::move(level), std::move(arrive), link, call};
    // This tick is the level's last: the ticks left of the frame do not run in it.
    level_->sim->stop_ticks();
}

SliceGame::Carried SliceGame::carry_hero() const {
    Carried c;
    c.x = hero_x();
    c.y = hero_y();
    if (!hero_alive()) return c;
    if (const Hero* h = level_->hero.try_get<Hero>()) c.facing = h->facing;
    if (const script::ScriptVars* v = level_->hero.try_get<script::ScriptVars>()) {
        c.has_vars = true;
        c.vars = *v;
    }
    return c;
}

void SliceGame::put_hero(const Carried& c, f64 x, f64 y) {
    load_around(x, y);
    spawn_hero(x, y);
    if (!hero_alive()) return;
    level_->hero.set<Hero>({c.facing});
    if (c.has_vars) level_->hero.set<script::ScriptVars>(c.vars);
    hero_x_ = x;
    hero_y_ = y;
    camera_.x = x;
    camera_.y = y - 2;
    load_around(x, y);
    // Where it comes out it is already in: no area there is entered now (the way back would send it back at once).
    level_->watch.settle(level_->areas, x, y);
    location_ = place_name(x, y);
}

void SliceGame::arrival(const std::string& arrive, f64& x, f64& y) {
    // The game's start, as a new game without a spawn point.
    x = gen_->spawn_x();
    y = gen_->spawn_y() - kHeroHalfH;
    f64 px = 0, py = 0;
    std::string what;
    if (!arrive.empty()) {
        const forge::level::Area* a = area_of(arrive);
        if (!a) return; // checked before going
        px = (a->x0 + a->x1) * 0.5;
        py = (a->y0 + a->y1) * 0.5;
        what = "зона «" + a->name + "»";
    } else if (level_->areas.spawn) {
        px = level_->areas.spawn_x;
        py = level_->areas.spawn_y;
        what = "точка появления";
    } else {
        FORGE_WARN("slice: у уровня «%s» нет точки появления: герой приходит на старт игры", level_id_.c_str());
        return;
    }
    f64 fx = 0, fy = 0;
    std::string why;
    if (spawn_spot(px, py, fx, fy, &why)) {
        x = fx;
        y = fy - kHeroHalfH;
    } else {
        FORGE_WARN("slice: %s %.2f, %.2f: %s; герой приходит на старт игры", what.c_str(), px, py, why.c_str());
    }
}

void SliceGame::travel() {
    const Trip trip = std::move(*trip_);
    trip_.reset();
    const TravelFault fault = std::exchange(travel_fault_, TravelFault::None);
    const fs::path game = shell_->game_dir();
    const forge::level::LevelList list = forge::level::read_levels(game);
    const forge::level::LevelEntry* to = list.find(trip.level);
    const std::string name = to ? to->name : trip.level;
    auto refuse = [&](const std::string& why) {
        // The link's «Только один раз» stands only if the hero went.
        level_->links->take_back_first(trip.call, trip.link);
        ++travels_refused_;
        travel_problem_ = why;
        FORGE_WARN("slice: переход на «%s» не сделан: %s", name.c_str(), why.c_str());
        shell_->toast("Перехода не будет: " + why);
    };
    // 2. The level gone to and where the hero comes out there, nothing changed yet.
    if (!to) return refuse("уровня «" + trip.level + "» нет в списке уровней игры");
    u64 area_id = 0;
    if (!trip.arrive.empty() && (!trip.arrive.starts_with(logic::kAreaPrefix) ||
                                 !forge::level::parse_area_id(std::string_view(trip.arrive).substr(logic::kAreaPrefix.size()), area_id)))
        return refuse("где появиться — не зона: «" + trip.arrive + "»");
    if (to->id == level_id_) { // the same level: the hero only moves
        if (area_id && !level_->areas.find(area_id)) return refuse("на уровне «" + name + "» нет зоны, где появиться");
        const Carried c = carry_hero();
        f64 x = 0, y = 0;
        arrival(trip.arrive, x, y);
        if (hero_alive()) {
            teleport(x, y);
            level_->watch.settle(level_->areas, x, y);
            location_ = place_name(x, y);
        } else {
            put_hero(c, x, y);
        }
        came_in(x, y + kHeroHalfH);
        ++travels_;
        travel_problem_.clear();
        FORGE_INFO("slice: герой перешёл в другое место уровня «%s»", name.c_str());
        return;
    }
    std::error_code ec;
    const fs::path levels = session_ / "levels";
    const fs::path world = session_ / "world";
    const fs::path been = levels / to->id;
    const fs::path source = forge::level::level_folder(game, to->id);
    const bool visited = fs::exists(been, ec);
    if (visited && !fs::is_directory(been, ec)) return refuse("в сохранении на месте уровня «" + name + "» файл, а не папка");
    if (!visited && fs::exists(source, ec) && !fs::is_directory(source, ec))
        return refuse("папка уровня «" + name + "» — файл: " + path_to_utf8(source));
    const fs::path from = visited ? been : source;
    if (area_id) {
        forge::level::LevelAreas there;
        std::string why;
        if (!forge::level::load_areas(from, there, nullptr, &why)) return refuse("зоны уровня «" + name + "» не читаются: " + why);
        if (!there.find(area_id)) return refuse("на уровне «" + name + "» нет зоны, где появиться");
    }
    // 3. Everything the going writes, beside the session's folders: the level gone to (its own copy, the first time),
    // the left one as it is now (its folder, then what changed in it, without the hero). A level that is no level of
    // the list cannot be come back to: nothing of it is kept.
    const std::string left = level_id_;
    const forge::level::LevelEntry* here = list.find(left);
    const std::string left_name = here ? here->name : left.empty() ? std::string("вне списка") : left;
    const bool keep_left = !left.empty();
    const bool made_levels = !fs::exists(levels, ec);
    const fs::path target = visited ? been : levels / ("." + to->id + ".tmp");
    const fs::path stage = levels / ("." + left + ".leave.tmp");
    const fs::path stale = levels / ("." + left + ".stale.tmp");
    const fs::path old_world = levels / ".world.old.tmp";
    auto clean = [&] {
        if (!visited) fs::remove_all(target, ec);
        if (keep_left) fs::remove_all(stage, ec);
        if (made_levels) fs::remove(levels, ec); // only when nothing is left in it
    };
    if (!visited) {
        fs::create_directories(levels, ec);
        if (ec || !fs::is_directory(levels, ec)) {
            clean();
            return refuse("не создать папку уровней в сохранении: " + path_to_utf8(levels));
        }
        fs::remove_all(target, ec);
        std::string error;
        bool ok = forge::level::copy_level(source, target, &error);
        if (ok) {
            fs::create_directories(target, ec); // a level of the list with no folder yet: an empty one
            ok = !ec;
        }
        if (!ok) {
            clean();
            return refuse("уровень «" + name + "» не скопировался в сохранение" + (error.empty() ? "" : ": " + error));
        }
    }
    if (keep_left) {
        fs::remove_all(stage, ec);
        fs::copy(world, stage, fs::copy_options::recursive, ec);
        bool ok = !ec;
        if (ok) {
            // The scene's index first: what the links of this step destroyed is gone from it.
            level_->scene->update();
            ok = level_->world->save_copy(stage).ok &&
                 level_->scene->save_copy(stage, hero_alive() ? level_->hero.id() : 0).ok;
        }
        if (!ok || fault == TravelFault::Save) {
            clean();
            return refuse("уровень «" + left_name + "» не записался на диск");
        }
    }
    // 4. The level gone to, made from its copy while the left one still plays (the areas are named by their level).
    level_id_ = to->id;
    std::string error;
    std::unique_ptr<Level> next;
    if (fault != TravelFault::Make) next = make_level(target, &error);
    level_id_ = left;
    auto drop = [&](const std::string& why) {
        next.reset();
        use_tiles(*level_);
        clean();
        refuse(why);
    };
    if (!next) return drop("уровень «" + name + "» не открылся" + (error.empty() ? std::string() : ": " + error));
    // 5. The folders take their places: the session's world aside, the level gone to into it, the left one into
    // levels/<id>. Each move undone in reverse when the next one fails.
    fs::remove_all(old_world, ec);
    if (fs::exists(stale, ec)) fs::remove_all(stale, ec);
    if (fault != TravelFault::Move) fs::rename(world, old_world, ec); // the fault for the test: it did not move
    if (ec || fault == TravelFault::Move) return drop("папка уровня в сохранении не переместилась");
    const fs::path keep = levels / left;
    bool had_keep = false, published = false;
    std::string why;
    if (fault != TravelFault::Back) fs::rename(target, world, ec); // the fault for the test: it did not move
    if (!ec && fault != TravelFault::Back) {
        published = true;
        // A folder of the left level already there (none should be: its folder was the session's world) goes.
        if (keep_left && fs::exists(keep, ec)) {
            fs::rename(keep, stale, ec);
            had_keep = !ec;
            if (ec) why = "в сохранении уже есть папка уровня «" + left_name + "», она не убирается";
        }
        if (why.empty() && keep_left) {
            fs::rename(stage, keep, ec);
            if (ec) why = "уровень «" + left_name + "» не встал на место в сохранении";
        }
        if (why.empty()) {
            // 6. Done: the session holds the going whole. The level gone to reads and writes its folder where it is now.
            next->world->moved_save(world);
            next->scene->moved_save(world);
            fs::remove_all(old_world, ec);
            if (ec) FORGE_WARN("slice: не удалить %s", path_to_utf8(old_world).c_str());
            if (had_keep) {
                FORGE_WARN("slice: в сохранении уже была папка %s: она заменена уровнем, с которого ушёл герой",
                           path_to_utf8(keep).c_str());
                fs::remove_all(stale, ec);
            }
            if (!keep_left) FORGE_INFO("slice: уровень вне списка игры не сохраняется: вернуться в него нельзя");
        }
    } else {
        why = "папка уровня «" + name + "» в сохранении не переместилась";
    }
    if (!why.empty()) {
        // Undone in reverse; when that fails too, the session is no game to go on with: the main menu, the saves
        // untouched (the game is not running, nothing is written).
        bool undone = fault != TravelFault::Back;
        if (undone && had_keep) {
            fs::rename(stale, keep, ec);
            undone = !ec;
        }
        if (undone && published) {
            fs::rename(world, target, ec);
            undone = !ec;
        }
        if (undone) {
            fs::rename(old_world, world, ec);
            undone = !ec;
        }
        if (!undone) {
            next.reset();
            FORGE_ERROR("slice: переход не сделан (%s), и папки сохранения не вернулись на место: игра — в главное меню",
                        why.c_str());
            travel_problem_ = why + "; папки сохранения не вернулись на место";
            end();
            shell_->to_main_menu(); // no autosave: the game is not running
            shell_->toast("Перехода не будет: " + why + ". Игра вернулась в главное меню, сохранения не тронуты");
            return;
        }
        return drop(why);
    }
    // 7. The left level goes, as SliceGame::end has it go, but the game goes on; the hero comes into the other.
    const Carried hero = carry_hero();
    for (const std::string& screen : level_screens_)
        if (shell_->screens().shown(screen)) shell_->screens().show(screen, false);
    level_screens_.clear();
    if (shell_->in_dialogue()) shell_->dialogue_runner().stop();
    talking_ = 0;
    sounds_.stop_objects();
    shell_->screens().set_place_music({});
    tiles_.shutdown();
    tiles_world_ = nullptr;
    level_.reset();
    deeds_.clear();
    hints_.clear();
    cues_.clear();
    level_ = std::move(next);
    level_id_ = to->id;
    f64 x = 0, y = 0;
    arrival(trip.arrive, x, y);
    put_hero(hero, x, y);
    came_in(x, y + kHeroHalfH);
    ++travels_;
    travel_problem_.clear();
    FORGE_INFO("slice: герой ушёл с уровня «%s» на уровень «%s»%s", left.c_str(), to->id.c_str(),
               trip.arrive.empty() ? "" : (" в зону " + trip.arrive).c_str());
}

f64 SliceGame::hearts() const {
    return shell_->vars().has("hero.hearts") ? shell_->vars().get("hero.hearts").number() : kHearts;
}

// n > 0 hurts, n < 0 heals. With no hearts left: settle, after the ticks (from a link's deed, at once) — the end when
// the game has a window for it, else the hero wakes up as before.
void SliceGame::hurt_hero(f64 n) {
    const f64 now = std::clamp(hearts() - n, 0.0, kHearts);
    shell_->vars().set("hero.hearts", now);
    if (n < 0) {
        shell_->toast("Сердца: " + std::to_string(static_cast<i32>(now)));
        return;
    }
    if (now > 0) {
        shell_->toast("Ранен! Сердец осталось: " + std::to_string(static_cast<i32>(now)));
        sounds_.play(Cue::Land, hero_x_, hero_y_);
        return;
    }
    out_of_hearts_ = true;
    // The world stops at this tick: the ticks left of the frame do not run.
    if (level_) level_->sim->stop_ticks();
}

// With no hearts left and no window «при поражении» in the game: as before 14.2c, full hearts and the hero wakes up in
// the village (a level with nothing around: at its spawn point).
void SliceGame::wake_up() {
    shell_->vars().set("hero.hearts", kHearts);
    // The village's start first (or the spawn point of a level with nothing around), checked as a return is; none
    // will do: where it is.
    f64 x = 0, y = 0;
    const char* where = return_spot(true, x, y);
    shell_->toast(where == kStartPlace && !level_->around.empty_around ? "Герой очнулся в деревне" : "Герой очнулся");
    if (where) teleport(x, y - kHeroHalfH);
}

// --- the platformer's rules ----------------------------------------------------------
// 14.2-платформер-модель.md, «Правила платформера (14.2c)»: enemies beaten from above and hurting otherwise, hazards
// and pits that take a heart and put the hero back where it came into the level, the score, the end of the game.

f64 SliceGame::score() const { return shell_->vars().get("hero.score").number(); }

void SliceGame::add_score(f64 n) {
    if (n > 0) shell_->vars().set("hero.score", score() + n);
}

bool SliceGame::back_point(f64& x, f64& y) const {
    x = back_x_;
    y = back_y_;
    return has_back_;
}

bool SliceGame::can_save(std::string* why) const {
    if (ended_ == Ending::None) return true;
    if (why) *why = "игра окончена";
    return false;
}

void SliceGame::came_in(f64 x, f64 feet_y) {
    has_back_ = true;
    back_x_ = x;
    // Standing on a cell's top, the hero's place (in floats) has its feet a hair above it: on it.
    back_y_ = std::fabs(feet_y - std::round(feet_y)) < 1e-3 ? std::round(feet_y) : feet_y;
    has_ground_ = nowhere_ = false;
    // A second the hero loses no heart where it is placed (in an enemy's box, say), not blinking.
    if (safe_ < kSafe) {
        safe_ = kSafe;
        blink_ = false;
    }
}

// Once a tick, before the hero moves: where the hero and each enemy were before their last move and are now tells
// how they met (forge::sim::touch_side). Enemies beaten first, each once (it is gone at once), one bounce; then, only
// if none was beaten this tick and the hero is not safe, the most hearts of the others, once a tick. Then hazards.
void SliceGame::contacts_tick(const TickContext& ctx) {
    if (!level_ || ctx.rewinding || ended_ != Ending::None || !level_->hero.is_alive()) return;
    const f32 dt = ctx.dt * ctx.time_scale;
    if (dt <= 0) return;
    safe_ = std::max(0.0, safe_ - dt);
    if (safe_ <= 0) blink_ = false;
    const Position& hp = level_->hero.get<Position>();
    const Body& hb = level_->hero.get<Body>();
    const Box hero{hp.tile_x(), hp.tile_y(), kHeroHalfW, kHeroHalfH};
    const Box hero_before{hero.x - hb.last_dx, hero.y - hb.last_dy, kHeroHalfW, kHeroHalfH};
    auto apart = [&](const Box& b) {
        return std::fabs(b.x - hero.x) >= b.half_w + hero.half_w || std::fabs(b.y - hero.y) >= b.half_h + hero.half_h;
    };
    struct Met {
        flecs::entity_t e = 0;
        Box box;
        Enemy enemy;
        Touch side = Touch::None;
    };
    std::vector<Met> met;
    level_->objects.enemies.each([&](flecs::entity e, const Position& p, const Body& b, const Enemy& en) {
        const Box now{p.tile_x(), p.tile_y(), b.half_w, b.half_h};
        if (apart(now)) return;
        const Box before{now.x - b.last_dx, now.y - b.last_dy, b.half_w, b.half_h};
        const Touch side = touch_side(hero_before, hero, before, now);
        if (side != Touch::None) met.push_back({e.id(), now, en, side});
    });
    // Spared enemies stay so while the hero is in them.
    std::erase_if(spared_, [&](flecs::entity_t e) {
        return std::none_of(met.begin(), met.end(), [e](const Met& m) { return m.e == e; });
    });
    flecs::world& ecs = level_->scene->ecs();
    f32 bounce = 0;
    for (const Met& m : met) {
        if (m.side != Touch::Top || !m.enemy.stomp) continue;
        flecs::entity e = ecs.entity(m.e);
        if (!e.is_alive()) continue;
        add_score(m.enemy.score);
        const Sounds* own = e.try_get<Sounds>();
        sounds_.play(own ? own->hit : std::string(), Cue::Crate, m.box.x, m.box.y, own ? own->volume : 1.0f,
                     own ? own->range : 16.0f);
        render::ParticleEmit dust;
        dust.x = m.box.x;
        dust.y = m.box.y;
        dust.count = 16;
        dust.radius = static_cast<f32>(m.box.half_w);
        dust.speed_min = 1;
        dust.speed_max = 5;
        dust.life_min = 0.3f;
        dust.life_max = 0.7f;
        dust.size_start = 0.3f;
        dust.size_end = 0.1f;
        dust.color = render::pack_color(200, 190, 170);
        dust.frame = FrameDust;
        particles_.emit(dust);
        e.destruct(); // gone at once: the next tick of the frame does not meet it again, nor do the saves keep it
        bounce = std::max(bounce, m.enemy.bounce);
        ++stomps_;
    }
    if (bounce > 0) {
        level_->hero.get_mut<Body>().vy = -bounce;
        // The others it is in now do not hurt it on its way up and out of them.
        for (const Met& m : met)
            if (ecs.is_alive(m.e) && std::find(spared_.begin(), spared_.end(), m.e) == spared_.end()) spared_.push_back(m.e);
    } else if (safe_ <= 0) {
        const Met* nearest = nullptr;
        u8 hearts = 0;
        for (const Met& m : met) {
            if (m.enemy.hearts == 0 || std::find(spared_.begin(), spared_.end(), m.e) != spared_.end()) continue;
            hearts = std::max(hearts, m.enemy.hearts);
            if (!nearest || std::fabs(m.box.x - hero.x) < std::fabs(nearest->box.x - hero.x)) nearest = &m;
        }
        if (nearest) {
            Body& b = level_->hero.get_mut<Body>();
            b.vx = hero.x < nearest->box.x ? -kKnockX : kKnockX;
            b.vy = -kKnockY;
            ++enemy_hits_;
            lose_hearts(hearts);
        }
    }
    // Hazards: the boxes overlapping; back after the ticks, a heart only when not safe.
    u8 hurts = 0;
    bool on = false;
    level_->objects.hazards.each([&](const Position& p, const Body& b, const Hazard& h) {
        if (apart({p.tile_x(), p.tile_y(), b.half_w, b.half_h})) return;
        on = true;
        hurts = std::max(hurts, h.hearts);
    });
    // The contacts are of the last move, from where the hero was if it has been placed since.
    const bool placed = std::exchange(placed_, false);
    if (!on) {
        nowhere_ = false; // off them: the next touch looks for a place to go back to again
        // Standing safely on the floor: the last place to go back to, when no other will do.
        if (!placed && (level_->hero.get<Body>().contacts & OnGround) && !in_pit(hero.x, hero.y)) {
            const f64 feet = hero.y + kHeroHalfH;
            has_ground_ = true;
            ground_x_ = hero.x;
            ground_y_ = std::fabs(feet - std::round(feet)) < 1e-3 ? std::round(feet) : feet;
        }
        return;
    }
    // With no place to go back to found for this touch the hero stays on them, the keys its own; they hurt it again
    // once it is not safe, as a long touch of an enemy does.
    if (!nowhere_) back_asked_ = true;
    if (safe_ <= 0 && hurts > 0) {
        ++hazard_hits_;
        lose_hearts(hurts);
    }
    if (back_asked_) level_->sim->stop_ticks(); // back at once, after this tick
}

void SliceGame::lose_hearts(f64 n) {
    safe_ = kSafe;
    blink_ = true;
    hurt_hero(n);
}

// What the ticks and the links' deeds left for after them: no hearts left (the end when the game has a window «при
// поражении», else the old waking up), the hero put back (spikes, a pit).
void SliceGame::settle() {
    if (!level_ || !running_ || ended_ != Ending::None) {
        out_of_hearts_ = back_asked_ = false;
        return;
    }
    if (out_of_hearts_) {
        out_of_hearts_ = back_asked_ = fell_ = false;
        hearts_out();
        return;
    }
    if (back_asked_) {
        back_asked_ = false;
        const bool fell = std::exchange(fell_, false);
        if (go_back() || !fell) return;
        // In a pit there is nothing to stand on: with nowhere to go back to, as the last heart.
        FORGE_WARN("slice: из зоны «падает в» вернуть героя некуда: как последнее сердце");
        shell_->vars().set("hero.hearts", 0.0);
        hearts_out();
    }
}

// No hearts left: the end when the game has a window «при поражении», else the old waking up.
void SliceGame::hearts_out() {
    if (!shell_->screens().endings("lose").empty()) finish(false);
    else wake_up();
}

bool SliceGame::in_pit(f64 x, f64 y) const {
    for (const forge::level::Area& a : level_->areas.areas)
        if (a.contains(x, y) && is_pit(a.id)) return true;
    return false;
}

bool SliceGame::is_pit(u64 area) const {
    const std::string id = area_thing_id(area);
    for (const logic::Link& l : links_.links)
        if (l.verb == "fall" && l.b == id) return true;
    return false;
}

bool SliceGame::back_spot(f64 x, f64 y, f64& out_x, f64& out_y, std::string* why) {
    if (!spawn_spot(x, y, out_x, out_y, why)) return false;
    const f64 cx = out_x, cy = out_y - kHeroHalfH;
    for (const forge::level::Area& a : level_->areas.areas)
        if (a.contains(cx, cy) && is_pit(a.id)) {
            if (why) *why = "там зона «" + a.name + "», куда герой падает";
            return false;
        }
    bool hazard = false;
    level_->objects.hazards.each([&](const Position& p, const Body& b, const Hazard&) {
        hazard = hazard || (std::fabs(p.tile_x() - cx) < b.half_w + kHeroHalfW && std::fabs(p.tile_y() - cy) < b.half_h + kHeroHalfH);
    });
    if (hazard) {
        if (why) *why = "там опасность";
        return false;
    }
    return true;
}

// The places the hero is put back in, in order, each checked the same way (back_spot): where it came into the level,
// the level's spawn point, the game's start, where it last stood safely on the floor here. Waking up (no hearts, no
// window «при поражении») begins where the game always woke it: the village's start, or the spawn point of a level
// with nothing around. Returns which place it found (x, y: under its feet), nullptr when none will do.
const char* SliceGame::return_spot(bool waking, f64& x, f64& y) {
    struct Place {
        const char* what;
        bool has;
        f64 x, y;
    };
    const Place entry{kEntryPlace, has_back_, back_x_, back_y_};
    const Place spawn{kSpawnPlace, level_->areas.spawn, level_->areas.spawn_x, level_->areas.spawn_y};
    const Place start{kStartPlace, true, gen_->spawn_x(), gen_->spawn_y()};
    const Place ground{kGroundPlace, has_ground_, ground_x_, ground_y_};
    const Place* order[4] = {&entry, &spawn, &start, &ground};
    if (waking && !level_->around.empty_around) order[0] = &start, order[2] = &entry; // start, spawn, entry, ground
    else if (waking) order[0] = &spawn, order[1] = &entry;                         // spawn, entry, start, ground
    for (const Place* p : order) {
        if (!p->has) continue;
        std::string why;
        if (back_spot(p->x, p->y, x, y, &why)) return p->what;
        FORGE_WARN("slice: %s %.2f, %.2f не годится для возврата: %s", p->what, p->x, p->y, why.c_str());
    }
    FORGE_WARN("slice: вернуть героя некуда: он остаётся, где был (%.2f, %.2f)", hero_x(), hero_y() + kHeroHalfH);
    ++nowheres_;
    return nullptr;
}

// Back after spikes or a pit, to the first place that will do (return_spot); none will: it stays where it is, its
// speed and the keys its own, and the hazards do not ask for it again until the hero is off them (false).
bool SliceGame::go_back() {
    if (!hero_alive()) return false;
    f64 x = 0, y = 0;
    if (!return_spot(false, x, y)) {
        nowhere_ = true;
        return false;
    }
    nowhere_ = false;
    const f64 cy = y - kHeroHalfH;
    teleport(x, cy);
    // It is in the areas there already: no area there is entered now (a «Вход» would send it back).
    level_->watch.settle(level_->areas, x, cy);
    location_ = place_name(x, cy);
    ++backs_;
    if (safe_ < kSafe) safe_ = kSafe;
    FORGE_INFO("slice: герой вернулся к точке %.2f, %.2f", x, y);
    return true;
}

void SliceGame::finish(bool won) {
    if (!running_ || ended_ != Ending::None) return;
    ended_ = won ? Ending::Won : Ending::Lost;
    ++endings_;
    out_of_hearts_ = back_asked_ = false;
    if (trip_) { // a going asked in the same step does not happen
        level_->links->take_back_first(trip_->call, trip_->link);
        ++travels_dropped_;
        trip_.reset();
    }
    deeds_.clear();
    controls_ = {};
    shell_->vars().set("game.result", std::string(won ? "победа" : "поражение"));
    const u32 shown = shell_->screens().show_ending(won ? "win" : "lose");
    if (won && shown == 0) shell_->toast("Победа!");
    // The world stands: no object sounds, no place music (the windows may play their own).
    sounds_.stop_objects();
    shell_->screens().set_place_music({});
    FORGE_INFO("slice: игра окончена: %s, очков %g, окон «%s» показано %u", won ? "победа" : "поражение", score(),
               won ? "при победе" : "при поражении", shown);
}

std::string SliceGame::place_name(f64 x, f64 y) const {
    // The game's places are where its generator made them.
    return level_ && level_->around.empty_around ? std::string() : gen_->location(x, y);
}

bool SliceGame::door_open(f64 x, f64 y, bool& open) const {
    if (!level_) return false;
    f64 best = 1e300;
    bool found = false;
    level_->scene->ecs().each([&](const Position& p, const Door& d) {
        const f64 dist = std::hypot(p.tile_x() - x, p.tile_y() - y);
        if (dist < best) best = dist, open = d.open, found = true;
    });
    return found;
}

fs::path SliceGame::links_path() const {
    return options_.links_file.empty() ? shell_->game_dir() / "logic.json" : options_.links_file;
}

bool SliceGame::reload_links(std::string* error) {
    // Into new ones first: a file half written keeps the old links.
    logic::Verbs verbs;
    logic::Logic links;
    const fs::path file = links_path();
    std::error_code ec;
    const auto time = fs::last_write_time(file, ec);
    if (!verbs.load(shell_->game_dir() / "verbs.json", error) || !links.load(file, error)) return false;
    verbs_ = std::move(verbs);
    links_ = std::move(links);
    links_time_ = time;
    if (level_) level_->links->load(links_, verbs_);
    return true;
}

void SliceGame::change_links(const logic::Logic& after, const std::string& said) {
    std::string error;
    if (!after.save(links_path(), &error) || !reload_links(&error)) {
        FORGE_ERROR("Связи: %s", error.c_str());
        shell_->toast("Связи не сохранились");
        return;
    }
    shell_->toast(said);
}

void SliceGame::watch_links() {
    const u64 now = SDL_GetTicks();
    if (now - links_checked_ < 500) return;
    links_checked_ = now;
    std::error_code ec;
    const auto time = fs::last_write_time(links_path(), ec);
    if (ec || time == links_time_) return;
    // Only the links matter here, not where the editor's board keeps them.
    auto said = [this] {
        std::string s;
        for (const logic::Link& l : links_.links)
            s += std::to_string(l.id) + l.a + "|" + l.verb + "|" + l.b + (l.night ? "n" : "") + (l.once ? "o" : "") +
                 (l.sound ? "s" : "") + (l.hint ? "h" : "") + ";";
        return s;
    };
    const std::string before = said();
    if (std::string error; !reload_links(&error)) return; // half written: next time
    if (said() != before) {
        shell_->toast("Связи обновлены из редактора");
    }
}

// The things on screen links can name: the hero, and of each template the
// copy nearest to the hero.
std::vector<Seen> SliceGame::seen_things() const {
    std::vector<Seen> seen;
    if (!level_) return seen;
    const Rect view = camera_.visible_tiles(width_, height_);
    if (hero_alive()) seen.push_back({std::string(logic::kHero), hero_x_, hero_y_, kHeroHalfW, kHeroHalfH});
    std::unordered_map<u64, usize> at; // template key -> index in seen
    std::unordered_map<u64, f64> best;
    level_->scene->ecs().each([&](flecs::entity e, const objects::ObjectRef& r, const Position& p) {
        Seen s;
        s.x = p.tile_x();
        s.y = p.tile_y();
        if (const Door* d = e.try_get<Door>()) {
            i32 x, top, bottom;
            door_cells(p, *d, x, top, bottom);
            s.x = x + 0.5;
            s.y = (top + bottom + 1) * 0.5;
            s.half_h = (bottom - top + 1) * 0.5;
        } else if (const Body* b = e.try_get<Body>()) {
            s.half_w = b->half_w;
            s.half_h = b->half_h;
        } else if (const RigidBody* rb = e.try_get<RigidBody>()) {
            s.half_w = rb->half_w;
            s.half_h = rb->half_h;
        }
        if (s.x < view.x0 || s.x > view.x1 || s.y < view.y0 || s.y > view.y1) return;
        const f64 dist = std::hypot(s.x - hero_x_, s.y - hero_y_);
        if (const auto it = best.find(r.key); it != best.end() && it->second <= dist) return;
        const objects::Template* t = library_.find(r.key);
        if (!t) return;
        s.id = t->id;
        best[r.key] = dist;
        if (const auto it = at.find(r.key); it != at.end()) seen[it->second] = std::move(s);
        else {
            at[r.key] = seen.size();
            seen.push_back(std::move(s));
        }
    });
    return seen;
}

// --- lifetime ----------------------------------------------------------------

bool SliceGame::init(game::Shell& shell, SDL_GPUDevice* device, SDL_GPUTextureFormat format) {
    shell_ = &shell;
    if (std::string error; !load_objects(library_, shell.game_dir(), &error)) FORGE_ERROR("%s", error.c_str());
    device_ = device;
    format_ = format;
    atlas_ = make_tile_art({});
    // The drawn frames and the templates' own pictures under them.
    pictures_.update(library_, make_sheet(), sheet_);
    sounds_.init(library_.sounds_folder(), options_.silent);
    // The game's screens: the music of the one up, a button's sound when pressed.
    shell.screens().on_music = [this](const std::string& name) { sounds_.screens().music(name); };
    shell.screens().on_sound = [this](const std::string& name) { sounds_.screens().click(name); };
    logic_ = std::make_unique<SliceLogic>(*this);
    bridge_ = std::make_unique<ShellBridge>(shell, level_screens_);
    // A button's «Сообщение логике» goes to every scheme listening nearby.
    shell.on_message = [this](const std::string& message) {
        if (level_ && level_->scripts) level_->scripts->send(0, message);
    };
    things_.push_back(logic::hero_thing());
    for (const objects::Template& t : library_.templates()) things_.push_back(logic::thing_of(library_, t));
    overlay_.set_allowed(options_.edit_links);
    overlay_.on_add = [this](const logic::Link& l) {
        logic::Logic after = links_;
        after.add(l);
        const logic::VerbDef* v = verbs_.find(l.verb);
        const logic::Thing* a = nullptr;
        const logic::Thing* b = nullptr;
        for (const logic::Thing& t : things_) {
            if (t.id == l.a) a = &t;
            if (t.id == l.b) b = &t;
        }
        change_links(after, v && a && b ? "Связь: «" + logic::phrase(l, *v, *a, *b) + "»" : "Связь добавлена");
    };
    overlay_.on_remove = [this](u32 id) {
        logic::Logic after = links_;
        if (after.remove(id)) change_links(after, "Связь убрана");
    };
    if (std::string error; !reload_links(&error)) FORGE_ERROR("Связи: %s", error.c_str());
    else FORGE_INFO("Связи: действий %zu, связей %zu", verbs_.all().size(), links_.links.size());
    sprite_capacity_ = options_.stress ? options_.stress_critters + 65536 : 65536;
    if (!sprites_.init(device, format, sheet_.sheet(), sprite_capacity_)) return false;
    if (!lights_.init(device, format)) return false;
    lights_.set_rules(light_rules());
    render::ParticleLook look;
    look.gravity_y = 12;
    look.drag = 1.2f;
    if (!particles_.init(device, format, sheet_.sheet(), options_.stress ? options_.stress_particles : 65536, 0,
                         gen_->village_y(), look))
        return false;

    // Inventory lives in variables (inv.dirt...), so dialogues can check and
    // change it and saves carry it.
    shell.define("has", [this](std::string_view, const std::vector<Value>& a) {
        return Value(!a.empty() && inv(a[0].text()) >= (a.size() > 1 ? a[1].number() : 1.0));
    });
    shell.define("count", [this](std::string_view, const std::vector<Value>& a) { return Value(a.empty() ? 0.0 : inv(a[0].text())); });
    shell.define("give", [this](std::string_view, const std::vector<Value>& a) {
        if (!a.empty()) give(a[0].text(), a.size() > 1 ? a[1].number() : 1.0, true);
        return Value(true);
    });
    shell.define("take", [this](std::string_view, const std::vector<Value>& a) {
        return Value(!a.empty() && take(a[0].text(), a.size() > 1 ? a[1].number() : 1.0));
    });

    // What the screens' lists call the things the hero carries, in their
    // order; item.icon is the picture of the pickup that gives it, if any.
    std::vector<game::ScreenItem> items;
    for (const char* id : {"coins", "key", "pickaxe", "torch", "wood", "dirt", "stone", "sand", "copper", "iron", "gold"}) {
        game::ScreenItem& i = items.emplace_back(game::ScreenItem{id, item_title(id), "", ""});
        for (const objects::Template& t : library_.templates())
            if (library_.item_of(t) == id) {
                i.picture = library_.picture_in(t, shell.game_dir());
                i.about = t.about;
                if (!i.picture.empty()) break;
            }
    }
    screen_items_ = items;
    shell.screens().set_items(std::move(items));

    // The HUD: its own document in the shell's context.
    hud_ = std::make_unique<Hud>();
    Rml::Context* ctx = shell.context();
    Rml::DataModelConstructor c = ctx->CreateDataModel("hud");
    if (!c) return false;
    if (auto s = c.RegisterStruct<SlotView>()) {
        s.RegisterMember("icon", &SlotView::icon);
        s.RegisterMember("tone", &SlotView::tone);
        s.RegisterMember("count", &SlotView::count);
        s.RegisterMember("key", &SlotView::key);
        s.RegisterMember("selected", &SlotView::selected);
        s.RegisterMember("empty", &SlotView::empty);
    }
    c.RegisterArray<std::vector<SlotView>>();
    Hud& h = *hud_;
    c.Bind("visible", &h.visible);
    c.Bind("slots", &h.slots);
    c.Bind("hint", &h.hint);
    c.Bind("has_hint", &h.has_hint);
    c.Bind("location", &h.location);
    c.Bind("coins", &h.coins);
    c.Bind("copper", &h.copper);
    c.Bind("stress", &h.stress);
    c.Bind("stress_on", &h.stress_on);
    overlay_.bind(c);
    c.BindEventCallback("select", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a) {
        if (!a.empty()) select(static_cast<u32>(a[0].Get<int>()));
    });
    h.handle = c.GetModelHandle();
    overlay_.set_handle(h.handle);
    if (!shell.ui().load_document(ctx, "slice/hud.rml")) return false;

    camera_.zoom = kZoom;
    return true;
}

void SliceGame::shutdown() {
    end();
    sounds_.shutdown();
    tiles_.shutdown();
    tiles_world_ = nullptr;
    level_.reset();
    particles_.shutdown();
    lights_.shutdown();
    sprites_.shutdown();
}

bool SliceGame::begin(const fs::path& session, bool new_game, std::string* error) {
    tiles_.shutdown();
    tiles_world_ = nullptr;
    level_.reset(); // the menu's backdrop
    session_ = session;
    // A new game starts from the level as the author left it in the editor: the one given (--level), else the
    // game's start level. A level of the list with no folder yet is an empty one (the game's world around).
    if (new_game) {
        const fs::path game = shell_->game_dir();
        const forge::level::LevelList list = forge::level::read_levels(game);
        if (list.broken) FORGE_WARN("slice: %s; новая игра — с уровня «level»", list.problem.c_str());
        for (const std::string& n : list.notes) FORGE_WARN("slice: %s", n.c_str());
        fs::path level = options_.level_dir;
        const forge::level::LevelEntry* entry = nullptr;
        if (level.empty()) {
            entry = &list.start_level();
            level = forge::level::level_folder(game, entry->id);
        } else {
            entry = forge::level::level_of_folder(game, list, level);
        }
        level_id_ = entry ? entry->id : std::string();
        if (entry) FORGE_INFO("slice: новая игра с уровня «%s» (%s)", entry->name.c_str(), path_to_utf8(level).c_str());
        else FORGE_INFO("slice: новая игра с уровня в %s", path_to_utf8(level).c_str());
        if (!forge::level::copy_level(level, session / "world", error)) return false;
    }
    // A save says which level it played before that level is made (its areas are that level's for the links).
    HeroSave saved;
    if (!new_game) {
        std::vector<u8> bytes;
        data::LoadReport report;
        if (!read_file(session / "hero.json", bytes) ||
            !data::from_json(saved, {reinterpret_cast<const char*>(bytes.data()), bytes.size()}, report)) {
            if (error) *error = "в сохранении нет героя (hero.json)";
            return false;
        }
        level_id_ = saved.level;
    }
    trip_.reset();
    travels_ = travels_dropped_ = travels_refused_ = 0;
    travel_problem_.clear();
    ended_ = Ending::None;
    safe_ = 0;
    blink_ = back_asked_ = fell_ = out_of_hearts_ = has_back_ = has_ground_ = nowhere_ = false;
    stomps_ = enemy_hits_ = hazard_hits_ = falls_ = backs_ = endings_ = nowheres_ = 0;
    spared_.clear();
    level_screens_.clear();
    deeds_.clear();
    hints_.clear();
    cues_.clear();
    level_ = make_level(session / "world", error);
    if (!level_) return false;

    HeroSave hs;
    hs.x = gen_->spawn_x();
    hs.y = gen_->spawn_y() - kHeroHalfH;
    hs.zoom = kZoom;
    // Where a new game puts the hero: «Играть отсюда», else the level's spawn
    // point, else the game's start.
    if (new_game && options_.at) {
        hs.x = options_.at_x;
        hs.y = options_.at_y - kHeroHalfH;
    } else if (new_game && level_->areas.spawn) {
        f64 x = 0, y = 0;
        std::string why;
        if (spawn_spot(level_->areas.spawn_x, level_->areas.spawn_y, x, y, &why)) {
            hs.x = x;
            hs.y = y - kHeroHalfH;
        } else {
            FORGE_WARN("slice: точка появления уровня %.2f, %.2f: %s; герой начинает со старта игры", level_->areas.spawn_x,
                       level_->areas.spawn_y, why.c_str());
        }
    }
    shell_->vars().set("hero.hearts_max", kHearts); // for a screen's bar of hearts
    // The things the hero may carry count 0 until found (a save keeps what it had): a screen's «{inv.coins}»
    // shows 0 in a new game, not its braces.
    for (const game::ScreenItem& it : screen_items_)
        if (!shell_->vars().has("inv." + it.id)) shell_->vars().set("inv." + it.id, 0);
    // The score the same way: 0 in a new game and in a save from before it (14.2c).
    if (!shell_->vars().has("hero.score")) shell_->vars().set("hero.score", 0);
    if (new_game) {
        shell_->vars().set("hero.hearts", kHearts);
        shell_->vars().set("inv.torch", 5);
        shell_->vars().set("hero.dig_speed", 1);
    } else {
        hs = saved;
    }
    slot_ = hs.slot < kSlots ? hs.slot : 0;
    camera_.zoom = hs.zoom > 0 ? hs.zoom : kZoom;
    camera_.x = hs.x;
    camera_.y = hs.y - 2;
    if (options_.stress) {
        stress_rects_.clear();
        for (i32 x = -2048; x < 2048; x += 256) {
            i32 lo = INT32_MAX, hi = INT32_MIN;
            for (i32 sx = x; sx < x + 256; sx += 8) {
                lo = std::min(lo, gen_->surface(sx));
                hi = std::max(hi, gen_->surface(sx));
            }
            stress_rects_.push_back({x, lo - 16, x + 256, hi + 8});
        }
    }
    load_around(hs.x, hs.y);
    if (new_game) spawn_hero(hs.x, hs.y);
    else find_hero();
    if (!level_->hero.is_alive()) {
        FORGE_WARN("slice: the hero was not in the save, placing a new one");
        spawn_hero(hs.x, hs.y);
    }
    if (options_.stress && new_game) spawn_stress();
    running_ = true;
    hero_x_ = hero_x();
    hero_y_ = hero_y();
    // A new game in an area comes into it at the first step; a loaded save
    // was in its areas already.
    if (new_game) level_->watch.clear();
    else level_->watch.settle(level_->areas, hero_x_, hero_y_);
    area_enters_.clear();
    area_leaves_.clear();
    location_ = place_name(hero_x_, hero_y_);
    dig_progress_ = 0;
    talking_ = 0;
    // Where the hero goes back to after spikes and pits: where a new game put it; a save's own, else (a save from
    // before 14.2c) the level's spawn point, else where it is now.
    f64 back_x = hero_x_, back_y = hero_y_ + kHeroHalfH;
    if (!new_game && saved.back) {
        back_x = saved.back_x;
        back_y = saved.back_y;
    } else if (!new_game && level_->areas.spawn) {
        if (f64 x = 0, y = 0; spawn_spot(level_->areas.spawn_x, level_->areas.spawn_y, x, y, nullptr)) back_x = x, back_y = y;
    }
    came_in(back_x, back_y);
    return true;
}

bool SliceGame::save(const fs::path& session, std::string& location, std::string* error) {
    if (!level_) return false;
    level_->scene->update(); // what the frame destroyed after its last tick (a pickup, a link's deed) out of the index
    const SaveReport w = level_->world->save();
    const scene::SceneSaveReport s = level_->scene->save();
    if (!w.ok || !s.ok) {
        if (error) *error = "мир не записался на диск";
        return false;
    }
    HeroSave hs;
    hs.x = hero_x();
    hs.y = hero_y();
    hs.slot = slot_;
    hs.zoom = camera_.zoom;
    hs.level = level_id_;
    hs.back = has_back_;
    hs.back_x = back_x_;
    hs.back_y = back_y_;
    const std::string json = data::to_json(hs);
    if (!write_file_atomic(session / "hero.json", {reinterpret_cast<const u8*>(json.data()), json.size()})) {
        if (error) *error = "hero.json";
        return false;
    }
    location = place_name(hs.x, hs.y);
    FORGE_INFO("slice: saved %u chunks of tiles and %u of objects in %.1f ms", w.chunks, s.chunks, w.ms + s.ms);
    return true;
}

void SliceGame::end() {
    if (!running_) return;
    running_ = false;
    trip_.reset();
    level_screens_.clear();
    shell_->screens().set_place_music({});
    sounds_.stop_objects();
    tiles_.shutdown();
    tiles_world_ = nullptr;
    level_.reset();
    talking_ = 0;
}

// Loads the place around a point (and keeps the current one) before
// anything moves there.
void SliceGame::load_around(f64 x, f64 y) {
    std::vector<Rect> rects = focus();
    const i32 tx = static_cast<i32>(std::floor(x)), ty = static_cast<i32>(std::floor(y));
    rects.push_back({tx - 48, ty - 32, tx + 48, ty + 32});
    for (u32 last = ~0u, rounds = 0; rounds < 64; ++rounds) {
        level_->sim->update(0, rects);
        level_->world->finish_loading();
        const u32 now = level_->world->stats().resident;
        if (now == last) break;
        last = now;
    }
}

std::vector<Rect> SliceGame::focus() const {
    std::vector<Rect> rects;
    Rect view = camera_.visible_tiles(width_, height_);
    view.x0 -= 8;
    view.x1 += 8;
    view.y0 -= 8;
    view.y1 += 8;
    rects.push_back(view);
    rects.insert(rects.end(), stress_rects_.begin(), stress_rects_.end());
    return rects;
}

void SliceGame::spawn_hero(f64 x, f64 y) {
    flecs::entity e = level_->scene->spawn(Position::at_tile(x, y));
    if (!e.is_valid()) return;
    Body b;
    b.half_w = kHeroHalfW;
    b.half_h = kHeroHalfH;
    e.set<Body>(b);
    e.set<Hero>({1});
    e.set<KeepAwake>({12});
    level_->hero = e;
}

void SliceGame::find_hero() {
    level_->hero = flecs::entity();
    level_->scene->ecs().each([&](flecs::entity e, const Hero&) { level_->hero = e; });
}

// The "all numbers at once" run: 200 000 critters along 4096 tiles of
// surface, all simulated, while the hero plays.
void SliceGame::spawn_stress() {
    for (u32 last = ~0u, rounds = 0; rounds < 64; ++rounds) {
        level_->sim->update(0, focus());
        level_->world->finish_loading();
        const u32 now = level_->world->stats().resident;
        if (now == last) break;
        last = now;
    }
    scene::Scene& scene = *level_->scene;
    u32 spawned = 0;
    for (u32 i = 0; spawned < options_.stress_critters && i < options_.stress_critters * 2; ++i) {
        const i32 x = -2048 + static_cast<i32>(hash32(i, 1) % 4096);
        if (std::abs(x) < 30) continue; // not inside the houses
        const f64 y = gen_->surface(x) - 1.0 - static_cast<f64>(hash32(i, 2) % 6);
        flecs::entity e = scene.spawn(Position::at_tile(x + 0.5, y));
        if (!e.is_valid()) continue;
        Body b;
        b.half_w = 0.35f;
        b.half_h = 0.4f;
        e.set<Body>(b);
        e.set<Critter>({1.5f + static_cast<f32>(hash32(i, 3) % 300) / 100.0f, (i & 1) ? 1.0f : -1.0f, i});
        ++spawned;
    }
    FORGE_INFO("stress: %u critters spawned over %zu stretches of surface", spawned, stress_rects_.size());
}

// --- queries for tests -----------------------------------------------------

bool SliceGame::hero_alive() const { return level_ && level_->hero.is_alive(); }
flecs::entity_t SliceGame::hero_entity() const { return hero_alive() ? level_->hero.id() : 0; }
f64 SliceGame::hero_x() const { return hero_alive() ? level_->hero.get<Position>().tile_x() : 0; }
f64 SliceGame::hero_y() const { return hero_alive() ? level_->hero.get<Position>().tile_y() : 0; }
bool SliceGame::on_ground() const { return hero_alive() && (level_->hero.get<Body>().contacts & OnGround); }
TileId SliceGame::tile(u32 layer, i32 x, i32 y) const { return level_ ? level_->world->tile(layer, x, y) : kEmptyTile; }
std::string SliceGame::location() const { return hero_alive() ? place_name(hero_x(), hero_y()) : ""; }
u32 SliceGame::entities() const { return level_ ? level_->scene->stats().entities : 0; }
f64 SliceGame::inventory(const char* item) const { return inv(item); }
const SimStats* SliceGame::sim_stats() const { return level_ ? &level_->sim->stats() : nullptr; }

u32 SliceGame::count_npcs() const {
    u32 n = 0;
    if (level_) level_->objects.npcs.each([&](const Position&, const Body&, const Npc&) { ++n; });
    return n;
}

f64 SliceGame::npc_x(u8 who) const {
    f64 x = std::nan("");
    if (level_) level_->objects.npcs.each([&](const Position& p, const Body&, const Npc& n) { if (n.who == who) x = p.tile_x(); });
    return x;
}

u32 SliceGame::count_items(ItemKind kind) const {
    u32 n = 0;
    if (level_) level_->objects.items.each([&](const Position&, const Body&, const Item& i) { n += i.kind == static_cast<u8>(kind); });
    return n;
}

flecs::entity_t SliceGame::spawn_critter(f64 x, f64 feet_y, Scheme scheme, u64 id) {
    const objects::Template* t = library_.find("critter");
    if (!level_ || !t) return 0;
    flecs::entity e = library_.spawn(*level_->scene, *t, x, feet_y);
    if (!e.is_valid()) return 0;
    if (Critter* c = e.try_get_mut<Critter>()) c->scheme = static_cast<u8>(scheme);
    if (id) e.set<forge::level::LevelId>({id});
    return e.id();
}

f64 SliceGame::critter_x(flecs::entity_t id) const {
    if (!level_ || !id) return std::nan("");
    const flecs::entity e(level_->scene->ecs(), id);
    const Position* p = e.is_alive() ? e.try_get<Position>() : nullptr;
    return p ? p->tile_x() : std::nan("");
}

std::vector<flecs::entity_t> SliceGame::copies_of(std::string_view template_id) const {
    std::vector<flecs::entity_t> out;
    const objects::Template* t = library_.find(template_id);
    if (!level_ || !t) return out;
    level_->scene->ecs().each([&](flecs::entity e, const objects::ObjectRef& ref) {
        if (ref.key == t->key) out.push_back(e.id());
    });
    return out;
}

flecs::entity_t SliceGame::spawn_copy(std::string_view template_id, f64 x, f64 feet_y, u64 id) {
    const objects::Template* t = library_.find(template_id);
    if (!level_ || !t || !id) return 0;
    flecs::entity e = library_.spawn(*level_->scene, *t, x, feet_y);
    if (!e.is_valid()) return 0;
    e.set<forge::level::LevelId>({id});
    return e.id();
}

flecs::entity_t SliceGame::copy_with_id(u64 id) const {
    flecs::entity_t out = 0;
    if (!level_ || !id) return out;
    level_->scene->ecs().each([&](flecs::entity e, const forge::level::LevelId& l) {
        if (l.id == id) out = e.id();
    });
    return out;
}

bool SliceGame::hero_marked(u32 link) const {
    if (!hero_alive()) return false;
    const script::ScriptVars* vars = level_->hero.try_get<script::ScriptVars>();
    const script::ScriptVar* v = vars ? vars->find("связь " + std::to_string(link)) : nullptr;
    return v && v->kind == script::VarKind::Bool && v->x != 0;
}

flecs::entity_t SliceGame::spawn_probe(f64 x, f64 y) {
    if (!level_) return 0;
    flecs::entity e = level_->scene->spawn(Position::at_tile(x, y));
    if (!e.is_valid()) return 0;
    Body b;
    b.half_w = b.half_h = 0.25f;
    e.set<Body>(b);
    return e.id();
}

bool SliceGame::probe(flecs::entity_t id, f64& x, f64& y, f32& gx, f32& gy) const {
    if (!level_ || !id) return false;
    const flecs::entity e(level_->scene->ecs(), id);
    if (!e.is_alive()) return false;
    const Position* p = e.try_get<Position>();
    const Body* b = e.try_get<Body>();
    if (!p || !b) return false;
    x = p->tile_x();
    y = p->tile_y();
    gx = b->gx;
    gy = b->gy;
    return true;
}

void SliceGame::world_gravity(f32& x, f32& y) const {
    x = y = std::nanf("");
    if (!level_) return;
    x = level_->sim->gravity().world_x();
    y = level_->sim->gravity().world_y();
}

void SliceGame::pull_at(f64 x, f64 y, f32& gx, f32& gy) const {
    gx = gy = std::nanf("");
    if (level_) level_->sim->gravity().at(x, y, gx, gy);
}

u32 SliceGame::gravity_sources() const { return level_ ? static_cast<u32>(level_->sim->gravity().source_count()) : 0; }

std::vector<SliceGame::Point> SliceGame::points() const {
    std::vector<Point> out;
    if (!level_) return out;
    level_->scene->ecs().each([&](flecs::entity e, const Position& p, const GravitySource& g) {
        const forge::level::LevelId* id = e.try_get<forge::level::LevelId>();
        out.push_back({id ? id->id : 0, p.tile_x(), p.tile_y(), g});
    });
    return out;
}

std::vector<SliceGame::Lamp> SliceGame::light_sources() const {
    std::vector<Lamp> out;
    if (!level_) return out;
    level_->scene->ecs().each([&](flecs::entity e, const Position& p, const forge::level::LightSource& s) {
        const forge::level::LevelId* id = e.try_get<forge::level::LevelId>();
        out.push_back({id ? id->id : 0, p.tile_x(), p.tile_y(), s});
    });
    return out;
}

f32 SliceGame::level_hour() const { return level_ ? level_->light.time : std::nanf(""); }
const world::World* SliceGame::world() const { return level_ ? level_->world.get() : nullptr; }

flecs::entity_t SliceGame::nearest_item(f64 x, f64 y, f64 radius) const {
    if (!level_) return 0;
    flecs::entity_t best = 0;
    f64 best_d = radius;
    level_->scene->ecs().each([&](flecs::entity e, const Position& p, const Item&) {
        const f64 d = std::hypot(p.tile_x() - x, p.tile_y() - y);
        if (d <= best_d) {
            best = e.id();
            best_d = d;
        }
    });
    return best;
}

bool SliceGame::position_of(flecs::entity_t id, f64& x, f64& y) const {
    if (!level_ || !id) return false;
    const flecs::entity e(level_->scene->ecs(), id);
    const Position* p = e.is_alive() ? e.try_get<Position>() : nullptr;
    if (!p) return false;
    x = p->tile_x();
    y = p->tile_y();
    return true;
}

bool SliceGame::set_sounds(flecs::entity_t id, const Sounds& sounds) {
    if (!level_) return false;
    flecs::entity e = level_->scene->ecs().entity(id);
    if (!e.is_alive()) return false;
    e.set<Sounds>(sounds);
    return true;
}

void SliceGame::teleport(f64 x, f64 y) {
    if (!hero_alive() || std::isnan(x) || std::isnan(y)) return;
    load_around(x, y);
    level_->hero.set<Position>(Position::at_tile(x, y));
    Body& b = level_->hero.get_mut<Body>();
    b.vx = b.vy = 0;
    b.last_dx = b.last_dy = 0;
    placed_ = true; // its contacts are from where it was
    // The hero moves to its new chunk while the old place is still loaded:
    // otherwise the old place could unload with the hero still filed there.
    load_around(x, y);
    camera_.x = x;
    camera_.y = y - 2;
    hero_x_ = x;
    hero_y_ = y;
    load_around(x, y);
}

// --- areas -------------------------------------------------------------------

std::string area_thing_id(u64 id) { return std::string(logic::kAreaPrefix) + forge::level::area_id_text(id); }

const forge::level::Area* SliceGame::area_of(std::string_view thing) const {
    u64 id = 0;
    if (!level_ || !thing.starts_with(logic::kAreaPrefix) || !forge::level::parse_area_id(thing.substr(logic::kAreaPrefix.size()), id))
        return nullptr;
    return level_->areas.find(id);
}

// Once a tick, before the scripts: the hero's centre against the areas.
void SliceGame::areas_tick(const TickContext& ctx) {
    if (!level_ || ctx.rewinding || !level_->hero.is_alive()) return;
    const Position* p = level_->hero.try_get<Position>();
    if (!p) return;
    std::vector<forge::level::AreaEvent> events;
    level_->watch.step(level_->areas, p->tile_x(), p->tile_y(), events);
    for (const forge::level::AreaEvent& e : events) {
        if (e.entered) ++area_enters_[e.area];
        else ++area_leaves_[e.area];
        level_->links->area_event(area_thing_id(e.area), level_->hero.id(), e.entered);
    }
}

bool SliceGame::spawn_spot(f64 x, f64 y, f64& out_x, f64& out_y, std::string* why) {
    if (!in_world(x, y)) {
        if (why) *why = "вне мира";
        return false;
    }
    // The column around it loaded, up and down as far as the ground is looked for.
    std::vector<Rect> rects = focus();
    const i32 tx = static_cast<i32>(std::floor(x)), ty = static_cast<i32>(std::floor(y));
    rects.push_back({tx - 1, ty - kGroundReach - 3, tx + 2, ty + kGroundReach + 1});
    for (u32 last = ~0u, rounds = 0; rounds < 64; ++rounds) {
        level_->sim->update(0, rects);
        level_->world->finish_loading();
        const u32 now = level_->world->stats().resident;
        if (now == last) break;
        last = now;
    }
    if (!hero_ground(*level_->world, x, y, kGroundReach, out_x, out_y)) {
        if (why) *why = "рядом нет пола с местом для героя";
        return false;
    }
    if (out_x != tx + 0.5 || out_y != std::floor(y))
        FORGE_INFO("slice: точка появления %.2f, %.2f в стене или в воздухе: герой встаёт на пол в %.1f, %.0f", x, y, out_x, out_y);
    return true;
}

const forge::level::LevelAreas* SliceGame::areas() const { return level_ ? &level_->areas : nullptr; }

std::vector<u64> SliceGame::areas_inside() const { return level_ ? level_->watch.inside() : std::vector<u64>{}; }

u32 SliceGame::area_enters(u64 id) const {
    const auto it = area_enters_.find(id);
    return it == area_enters_.end() ? 0 : it->second;
}

u32 SliceGame::area_leaves(u64 id) const {
    const auto it = area_leaves_.find(id);
    return it == area_leaves_.end() ? 0 : it->second;
}

flecs::entity_t SliceGame::area_entity(u64 id) const {
    return level_ && level_->links ? level_->links->area_entity(area_thing_id(id)) : 0;
}

// --- inventory ---------------------------------------------------------------

f64 SliceGame::inv(const std::string& item) const { return shell_->vars().get("inv." + item).number(); }

void SliceGame::give(const std::string& item, f64 n, bool announce) {
    if (n <= 0) return;
    shell_->vars().set("inv." + item, inv(item) + n);
    if (announce) {
        char text[96];
        std::snprintf(text, sizeof(text), "+%g %s", n, item_title(item));
        shell_->toast(text);
    }
}

bool SliceGame::take(const std::string& item, f64 n) {
    const f64 have = inv(item);
    if (have < n) return false;
    shell_->vars().set("inv." + item, have - n);
    return true;
}

// --- play --------------------------------------------------------------------

i32 SliceGame::nearest_npc(f64 reach, flecs::entity* out) const {
    if (!level_ || !hero_alive()) return -1;
    f64 best = reach;
    i32 who = -1;
    level_->objects.npcs.each([&](flecs::entity e, const Position& p, const Body&, const Npc& n) {
        const f64 d = std::hypot(p.tile_x() - hero_x_, p.tile_y() - hero_y_);
        if (d < best) {
            best = d;
            who = n.who;
            if (out) *out = e;
        }
    });
    return who;
}

bool SliceGame::talk_nearest() {
    flecs::entity e;
    const i32 who = nearest_npc(kTalkReach, &e);
    if (who < 0) return false;
    if (!shell_->talk(npc_dialogue(static_cast<u8>(who)))) return false;
    talking_ = e.id();
    sounds_.play(Cue::Talk, hero_x_, hero_y_);
    return true;
}

void SliceGame::handle_event(const SDL_Event& e) {
    if (!running_) return;
    if (e.type == SDL_EVENT_MOUSE_WHEEL && e.wheel.y != 0)
        camera_.zoom = std::clamp(camera_.zoom * (e.wheel.y > 0 ? 1.15f : 1.0f / 1.15f), 14.0f, 72.0f);
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
        if (e.key.key >= SDLK_1 && e.key.key < SDLK_1 + static_cast<SDL_Keycode>(kSlots)) select(static_cast<u32>(e.key.key - SDLK_1));
        if (e.key.key == SDLK_E) talk_nearest();
        if (e.key.key == SDLK_F2 && overlay_.allowed()) overlay_.show(!overlay_.on());
    }
    // A click on the world, not on a thing: nothing picked any more.
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && overlay_.on()) overlay_.cancel();
}

void SliceGame::read_input(bool input) {
    if (scripted_) {
        controls_ = input ? script_ : Controls{};
        return;
    }
    controls_ = {};
    SDL_Window* window = SDL_GetKeyboardFocus();
    if (!input || !window) return;
    const bool* keys = SDL_GetKeyboardState(nullptr);
    controls_.left = keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT];
    controls_.right = keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT];
    controls_.jump = keys[SDL_SCANCODE_SPACE] || keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP];
    f32 mx = 0, my = 0;
    const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&mx, &my);
    const f32 density = SDL_GetWindowPixelDensity(window);
    camera_.screen_to_tile(mx * density, my * density, width_, height_, controls_.aim_x, controls_.aim_y);
    if (shell_->over_world() && !overlay_.on()) { // over «Связи» clicks pick things
        controls_.use = buttons & SDL_BUTTON_LMASK;
        controls_.place = buttons & SDL_BUTTON_RMASK;
    }
}

void SliceGame::hero_tick(const TickContext& ctx) {
    if (!level_ || !level_->hero.is_alive()) return;
    const f32 dt = ctx.dt * ctx.time_scale;
    if (dt <= 0) return;
    Body& b = level_->hero.get_mut<Body>();
    Position& p = level_->hero.get_mut<Position>();
    Hero& h = level_->hero.get_mut<Hero>();
    const Controls& c = controls_;
    const f32 want = (c.right ? kWalk : 0.0f) - (c.left ? kWalk : 0.0f);
    const bool ground = b.contacts & OnGround;
    const f32 acc = (ground ? 70.0f : 30.0f) * dt;
    b.vx = b.vx < want ? std::min(want, b.vx + acc) : std::max(want, b.vx - acc);
    if (want != 0) h.facing = want > 0 ? 1.0f : -1.0f;
    if (c.jump) {
        if (ground) {
            b.vy = -kJump;
            jumped_ = true;
        } else if (b.liquid != 0) {
            // Swim up; at the surface, a kick that clears the bank.
            const i32 hx = static_cast<i32>(std::floor(p.tile_x()));
            const i32 head = static_cast<i32>(std::floor(p.tile_y() - kHeroHalfH));
            if (liquid_amount(level_->world->tile(kLiquids, hx, head)) < kFull / 4) b.vy = -12.0f;
            else b.vy = std::max(b.vy - 70.0f * dt, -7.0f);
        }
    }
    // Steps one tile high are walked up, not jumped.
    const bool blocked = (want > 0 && (b.contacts & HitRight)) || (want < 0 && (b.contacts & HitLeft));
    if (ground && blocked) {
        const World& w = *level_->world;
        auto free = [&](i32 x, i32 y) { return !is_solid(w.tile(kBlocks, x, y)); };
        const f64 x = p.tile_x(), y = p.tile_y();
        const f64 dir = want > 0 ? 1.0 : -1.0;
        const i32 feet = static_cast<i32>(std::floor(y + kHeroHalfH - 0.05));
        const i32 front = static_cast<i32>(std::floor(x + dir * (kHeroHalfW + 0.1)));
        bool ok = !free(front, feet);
        const i32 c0 = static_cast<i32>(std::floor(std::min(x - kHeroHalfW, x + dir * (kHeroHalfW + 0.1))));
        const i32 c1 = static_cast<i32>(std::floor(std::max(x + kHeroHalfW, x + dir * (kHeroHalfW + 0.1))));
        for (i32 cx = c0; cx <= c1 && ok; ++cx) ok = free(cx, feet - 1) && free(cx, feet - 2);
        if (ok) p = Position::at_tile(x + dir * 0.12, y - 1.0);
    }
}

void SliceGame::act(f64 dt) {
    const Controls& c = controls_;
    place_wait_ = std::max(0.0, place_wait_ - dt);
    const f64 dx = c.aim_x - hero_x_, dy = c.aim_y - hero_y_;
    const bool reach = dx * dx + dy * dy <= static_cast<f64>(kReach * kReach);
    const i32 tx = static_cast<i32>(std::floor(c.aim_x)), ty = static_cast<i32>(std::floor(c.aim_y));
    if (c.use && reach) {
        if (!last_.use && break_crate(c.aim_x, c.aim_y)) {
            // a crate under the cursor took this click
        } else {
            dig(dt, tx, ty);
        }
    } else {
        dig_progress_ = 0;
    }
    if (c.place && reach && place_wait_ <= 0) {
        place(tx, ty);
        place_wait_ = 0.18;
    }
    last_ = c;
}

void SliceGame::dig(f64 dt, i32 tx, i32 ty) {
    World& w = *level_->world;
    const TileId t = w.tile(kBlocks, tx, ty);
    if (t == TileAir) {
        // A torch on the wall comes off with a click.
        if (w.tile(kWalls, tx, ty) == TileTorch && !last_.use) {
            w.set_tile(kWalls, tx, ty, TilePlankWall);
            give("torch", 1, false);
            sounds_.play(Cue::Torch, tx + 0.5, ty + 0.5);
        }
        dig_progress_ = 0;
        return;
    }
    const bool pickaxe = inv("pickaxe") > 0;
    const f64 speed = std::max(0.25, shell_->vars().get("hero.dig_speed").number());
    const f64 need = pickaxe ? pickaxe_time(t) / speed : hand_time(t);
    if (need <= 0) {
        if (!dig_warned_) shell_->toast("Без кирки это не разбить");
        dig_warned_ = true;
        return;
    }
    if (tx != dig_x_ || ty != dig_y_) {
        dig_x_ = tx;
        dig_y_ = ty;
        dig_progress_ = 0;
    }
    dig_progress_ += dt;
    dig_dust_ -= dt;
    const u32 color = t == TileSand ? render::pack_color(220, 200, 140) : (t == TileDirt || t == TileGrass ? render::pack_color(140, 100, 60) : render::pack_color(150, 150, 160));
    if (dig_dust_ <= 0) {
        dig_dust_ = 0.08;
        if (dig_ticks_++ % 3 == 0) sounds_.play(Cue::Dig, tx + 0.5, ty + 0.5, 1, t == TileSand || t == TileDirt || t == TileGrass ? 0.8f : 1.1f);
        render::ParticleEmit e;
        e.x = tx + 0.5;
        e.y = ty + 0.5;
        e.count = 3;
        e.radius = 0.4f;
        e.speed_min = 1;
        e.speed_max = 4;
        e.life_min = 0.3f;
        e.life_max = 0.7f;
        e.size_start = 0.25f;
        e.size_end = 0.1f;
        e.color = color;
        e.frame = FrameDust;
        particles_.emit(e);
    }
    if (dig_progress_ < need) return;
    dig_progress_ = 0;
    dig_ticks_ = 0;
    w.set_tile(kBlocks, tx, ty, TileAir);
    give(drop_of(t), 1, false);
    sounds_.play(Cue::Break, tx + 0.5, ty + 0.5, 1, t == TileSand || t == TileDirt || t == TileGrass ? 0.85f : 1.0f);
    render::ParticleEmit e;
    e.x = tx + 0.5;
    e.y = ty + 0.5;
    e.count = 14;
    e.radius = 0.5f;
    e.speed_min = 2;
    e.speed_max = 7;
    e.life_min = 0.4f;
    e.life_max = 0.9f;
    e.size_start = 0.22f;
    e.size_end = 0.08f;
    e.spin = 6;
    e.color = color;
    e.frame = FrameSpark;
    particles_.emit(e);
}

void SliceGame::place(i32 tx, i32 ty) {
    const SlotDef& s = kSlotDefs[slot_];
    if (s.tile == TileAir) return;
    if (inv(s.item) < 1) {
        shell_->toast(std::string("Нет: ") + item_title(s.item));
        return;
    }
    World& w = *level_->world;
    if (w.tile(kBlocks, tx, ty) != TileAir) return;
    if (s.layer == kWalls) {
        if (w.tile(kWalls, tx, ty) == TileTorch) return;
        w.set_tile(kWalls, tx, ty, TileTorch);
        take(s.item, 1);
        sounds_.play(Cue::Torch, tx + 0.5, ty + 0.5);
        return;
    }
    // Not inside the hero, and next to something to hold on to.
    const f64 hx0 = hero_x_ - kHeroHalfW, hx1 = hero_x_ + kHeroHalfW;
    const f64 hy0 = hero_y_ - kHeroHalfH, hy1 = hero_y_ + kHeroHalfH;
    if (tx + 1 > hx0 && tx < hx1 && ty + 1 > hy0 && ty < hy1) return;
    const bool held = is_solid(w.tile(kBlocks, tx - 1, ty)) || is_solid(w.tile(kBlocks, tx + 1, ty)) ||
                      is_solid(w.tile(kBlocks, tx, ty - 1)) || is_solid(w.tile(kBlocks, tx, ty + 1)) ||
                      w.tile(kWalls, tx, ty) != TileAir;
    if (!held) return;
    w.set_tile(kLiquids, tx, ty, 0);
    w.set_tile(kBlocks, tx, ty, s.tile);
    take(s.item, 1);
    sounds_.play(Cue::Place, tx + 0.5, ty + 0.5);
}

bool SliceGame::break_crate(f64 x, f64 y) {
    std::vector<flecs::entity_t> near;
    level_->scene->query_radius(x, y, 0.9f, near);
    flecs::world& ecs = level_->scene->ecs();
    for (flecs::entity_t id : near) {
        flecs::entity e = ecs.entity(id);
        if (!e.is_alive() || !e.has<RigidBody>()) continue;
        const Position p = e.get<Position>();
        const u32 dice = hash32(static_cast<u32>(id), static_cast<u32>(time_now_ns()));
        const Sounds* own = e.try_get<Sounds>();
        sounds_.play(own ? own->hit : std::string(), Cue::Crate, p.tile_x(), p.tile_y(), own ? own->volume : 1.0f,
                     own ? own->range : 16.0f);
        e.destruct();
        give("wood", 2, true);
        give("coins", 1 + dice % 4, true);
        render::ParticleEmit em;
        em.x = p.tile_x();
        em.y = p.tile_y();
        em.count = 24;
        em.radius = 0.5f;
        em.speed_min = 3;
        em.speed_max = 8;
        em.life_min = 0.5f;
        em.life_max = 1.0f;
        em.size_start = 0.3f;
        em.size_end = 0.1f;
        em.spin = 8;
        em.color = render::pack_color(176, 124, 70);
        em.frame = FrameSpark;
        particles_.emit(em);
        return true;
    }
    return false;
}

void SliceGame::pickups() {
    std::vector<flecs::entity_t> near;
    level_->scene->query_radius(hero_x_, hero_y_, 2.0f, near);
    flecs::world& ecs = level_->scene->ecs();
    for (flecs::entity_t id : near) {
        flecs::entity e = ecs.entity(id);
        if (!e.is_alive() || !e.has<Item>()) continue;
        // Touching the hero's box.
        const Position& ip = e.get<Position>();
        if (std::fabs(ip.tile_x() - hero_x_) > kHeroHalfW + 0.7 || std::fabs(ip.tile_y() - hero_y_) > kHeroHalfH + 0.6) continue;
        const Item item = e.get<Item>();
        const ItemKind kind = static_cast<ItemKind>(item.kind);
        const Sounds* own = e.try_get<Sounds>();
        sounds_.play(own ? own->pickup : std::string(), kind == ItemKind::Coins ? Cue::Coins : Cue::Pickup, ip.tile_x(),
                     ip.tile_y(), own ? own->volume : 1.0f, own ? own->range : 16.0f);
        e.destruct();
        add_score(item.score);
        if (kind == ItemKind::Pickaxe) {
            give("pickaxe", 1, false);
            shell_->toast("Найдена кирка Бориса");
            if (shell_->vars().get("quest.pickaxe").number() < 2) shell_->vars().set("quest.pickaxe", 2);
            dig_warned_ = false;
        } else {
            give(kind_item(kind), item.count, true);
        }
    }
}

void SliceGame::follow_camera(f64 dt) {
    const f64 k = 1.0 - std::exp(-dt * 8.0);
    camera_.x += (hero_x_ - camera_.x) * k;
    camera_.y += (hero_y_ - 2.0 - camera_.y) * k;
}

// Menu backdrop: the camera floats over the village.
void SliceGame::drift_camera(f64 dt) {
    backdrop_t_ += dt;
    camera_.zoom = 26;
    camera_.x = -6.0 + std::sin(backdrop_t_ * 0.04) * 30.0;
    camera_.y = gen_->village_y() - 7.0;
}

void SliceGame::update(f64 dt, bool playing, bool input) {
    frame_dt_ = dt;
    if (!level_) {
        if (running_) return;
        level_ = make_level({}, nullptr);
        backdrop_t_ = 0;
        drift_camera(0);
        load_around(camera_.x, camera_.y);
    }
    const game::Settings& settings = shell_->settings();
    sounds_.set_volumes(settings.master_volume, settings.sound_volume, settings.music_volume);
    sounds_.tick(dt);
    if (!running_) {
        drift_camera(dt);
        level_->sim->update(dt, focus());
        build_sprites();
        update_hud(false);
        return;
    }
    if (!shell_->in_dialogue()) talking_ = 0;
    // An ended game (won or lost): the world stands and the keys lead nobody; the camera and the screens go on.
    const bool live = playing && ended_ == Ending::None;
    read_input(input && live);
    if (!level_->hero.is_alive()) find_hero();
    hero_x_ = hero_x();
    hero_y_ = hero_y();

    if (trip_) { // asked between frames (go_to)
        travel();
        if (!level_) return;
    }
    const u64 t0 = time_now_ns();
    sync_doors(*level_->scene);
    level_->sim->update(live ? dt : 0.0, focus());
    sim_ms_ = ns_to_ms(time_now_ns() - t0);
    settle(); // what the ticks left: no hearts, back after spikes
    do_deeds();
    if (trip_) {
        travel();
        if (!level_) return; // could not go, nor put the session back: the main menu
    }
    // The links' «Только один раз» stand once their call is over and a going it asked for is done or refused; a
    // call still waiting may yet ask to go.
    level_->links->keep_firsts();
    sync_doors(*level_->scene);
    if (!level_->hero.is_alive()) find_hero();
    hero_x_ = hero_x();
    hero_y_ = hero_y();

    sounds_.pause(!playing);
    sounds_.set_listener(hero_x_, hero_y_);
    // The music of the place the hero is in: the screens decide over it.
    const forge::level::Area* place =
        hero_alive() && ended_ == Ending::None ? forge::level::music_area(level_->areas, hero_x_, hero_y_) : nullptr;
    shell_->screens().set_place_music(place ? place->music : std::string());
    // The game may have ended in this frame's ticks or deeds: from then on nothing more is done, sounded or taken.
    const bool going = live && ended_ == Ending::None;
    if (going) {
        hero_sounds(dt);
        sounds_.update_objects(*level_->scene, hero_x_, hero_y_, dt);
        act(dt);
        pickups();
        location_wait_ -= dt;
        if (location_wait_ <= 0) {
            location_wait_ = 0.5;
            const std::string here = place_name(hero_x_, hero_y_);
            if (here != location_) {
                location_ = here;
                if (!here.empty()) shell_->toast(here);
            }
        }
    }
    follow_camera(dt);
    emit_effects(going ? dt : 0.0);
    build_sprites();
    watch_links();
    update_hud(playing);

    if (options_.stress) {
        const f64 ms = dt * 1000.0;
        frame_ms_avg_ = frame_ms_avg_ == 0 ? ms : frame_ms_avg_ * 0.95 + ms * 0.05;
        stress_log_ += dt;
        if (stress_log_ >= 5.0) {
            stress_log_ = 0;
            const SimStats& s = level_->sim->stats();
            FORGE_INFO("stress: entities %u, chunks active %u near %u, particles %u, frame %.2f ms, sim %.2f ms (systems %.2f, bodies %.2f, liquids %.2f)",
                       entities(), s.zones.active, s.zones.near, particles_.stats().slots_used, frame_ms_avg_, sim_ms_,
                       s.systems_ms, s.bodies_ms, s.cells_ms);
        }
    }
}

// Steps, jumps, landings and splashes of the hero, from how it moved.
void SliceGame::hero_sounds(f64 dt) {
    if (!hero_alive()) return;
    const Body& b = level_->hero.get<Body>();
    const bool ground = b.contacts & OnGround, wet = b.liquid != 0;
    const f64 feet = hero_y_ + kHeroHalfH;
    if (jumped_) sounds_.play(Cue::Jump, hero_x_, feet, 0.8f);
    jumped_ = false;
    if (ground && !was_ground_ && fall_ > 8 && !wet) sounds_.play(Cue::Land, hero_x_, feet, std::min(1.0f, static_cast<f32>(fall_ / 25.0)));
    if (wet && !was_wet_ && fall_ > 3) sounds_.play(Cue::Splash, hero_x_, feet, std::min(1.0f, static_cast<f32>(fall_ / 15.0) + 0.3f));
    if (ground && !wet && std::fabs(b.vx) > 1) {
        walked_ += std::fabs(b.vx) * dt;
        if (walked_ >= 1.6) {
            walked_ = 0;
            sounds_.play(Cue::Step, hero_x_, feet, 0.7f);
        }
    } else if (!ground) {
        walked_ = 1.2; // the first step comes soon after landing
    }
    fall_ = ground ? 0.0 : std::max(fall_ * (wet ? 0.0 : 1.0), static_cast<f64>(b.vy));
    was_ground_ = ground;
    was_wet_ = wet;
}

void SliceGame::emit_effects(f64 dt) {
    if (dt <= 0) return;
    // Embers over torches now and then.
    for (const auto& [x, y] : torches_) {
        if (hash32(static_cast<u32>(x * 131 + y), static_cast<u32>(time_now_ns() >> 20)) % 40 != 0) continue;
        render::ParticleEmit e;
        e.x = x;
        e.y = y - 0.3;
        e.count = 1;
        e.speed_min = 0.5f;
        e.speed_max = 1.5f;
        e.angle = -1.5708f;
        e.spread = 1.0f;
        e.life_min = 0.4f;
        e.life_max = 0.9f;
        e.size_start = 0.12f;
        e.size_end = 0.02f;
        e.color = render::pack_color(255, 190, 80);
        e.frame = FrameSpark;
        particles_.emit(e);
    }
    if (options_.stress) {
        // Falling ash over a wide area, enough to keep the particle pool full.
        const f32 life = 4.0f;
        const u32 count = static_cast<u32>(static_cast<f64>(options_.stress_particles) / life * dt);
        render::ParticleEmit e;
        e.x = camera_.x;
        e.y = camera_.y - 10;
        e.count = count;
        e.radius = 80;
        e.speed_min = 0.5f;
        e.speed_max = 3;
        e.angle = 1.5708f;
        e.spread = 2.0f;
        e.life_min = life * 0.75f;
        e.life_max = life * 1.25f;
        e.size_start = 0.12f;
        e.size_end = 0.06f;
        e.color = render::pack_color(230, 230, 240, 160);
        e.frame = FrameDust;
        particles_.emit(e);
    }
}

void SliceGame::build_sprites() {
    batch_.begin(camera_.x, camera_.y, sprite_capacity_);
    const Simulation& sim = *level_->sim;
    const f32 alpha = sim.clock().alpha();
    auto at = [&](f64 x, f64 y, f32 w, f32 h, u32 frame, u32 order, u32 color = 0xffffffffu, f32 angle = 0) {
        Sprite s;
        s.x = static_cast<f32>(x - camera_.x);
        s.y = static_cast<f32>(y - camera_.y);
        s.w = w;
        s.h = h;
        s.frame = frame;
        s.order = order;
        s.color = color;
        s.angle = angle;
        batch_.push(s);
    };

    push_objects(batch_, level_->objects, level_->around.empty_around ? nullptr : gen_.get(), camera_.x, camera_.y, alpha,
                 sim.clock().tick(), &pictures_);
    if (running_ && level_->hero.is_alive()) {
        const Position& p = level_->hero.get<Position>();
        const Body& b = level_->hero.get<Body>();
        const Hero& h = level_->hero.get<Hero>();
        f64 x, y;
        draw_position(p, b, alpha, x, y);
        u32 frame = FrameHero;
        if (!(b.contacts & OnGround) && b.liquid == 0) frame = FrameHero + 3;
        else if (std::fabs(b.vx) > 0.5f) frame = FrameHero + 1 + (static_cast<u32>(sim.clock().tick() / 6) & 1u);
        // Blinking in the safe second after a heart lost.
        const bool faint = blinking() && (sim.clock().tick() / 5) % 2 == 1;
        at(x, y, h.facing < 0 ? -1.0f : 1.0f, 2.0f, frame, 4, faint ? render::pack_color(255, 255, 255, 70) : 0xffffffffu);
        // The tool in hand while digging.
        if (controls_.use && inv("pickaxe") > 0) {
            const f32 swing = std::sin(static_cast<f32>(sim.clock().tick()) * 0.5f) * 0.9f;
            at(x + h.facing * 0.55, y - 0.1, h.facing * 0.8f, 0.8f, FramePickaxe, 5, 0xffffffffu, h.facing * swing);
        }
        // Cracks on the block being dug, darkening as it gives.
        if (dig_progress_ > 0) at(dig_x_ + 0.5, dig_y_ + 0.5, 1.0f, 1.0f, FrameSpark, 6, render::pack_color(0, 0, 0, static_cast<u8>(std::min(1.0, dig_progress_ * 2.0) * 150)));
    }
    // Torches: a flame sprite (not dimmed with the wall) and a light.
    push_torches(batch_, *level_->world, camera_.visible_tiles(width_, height_), camera_.x, camera_.y, sim.clock().tick(),
                 torches_);
}

void SliceGame::update_hud(bool playing) {
    Hud& h = *hud_;
    const bool visible = running_ && playing;
    std::vector<SlotView> slots(kSlots);
    for (u32 i = 0; i < kSlots; ++i) {
        const SlotDef& d = kSlotDefs[i];
        SlotView& v = slots[i];
        v.key = std::to_string(i + 1);
        v.selected = i == slot_;
        v.tone = std::string("tone-") + d.item;
        v.icon = d.icon;
        if (i == 0) {
            const bool pick = running_ && inv("pickaxe") > 0;
            v.icon = pick ? "hardware" : "back_hand";
            if (!pick) v.tone = "tone-hand";
        } else {
            const f64 n = running_ ? inv(d.item) : 0;
            v.count = n > 0 ? std::to_string(static_cast<i64>(n)) : "";
            v.empty = n <= 0;
        }
    }
    std::string hint;
    if (visible && !shell_->in_dialogue()) {
        const i32 who = nearest_npc(kTalkReach, nullptr);
        if (who >= 0) hint = std::string("E — поговорить: ") + npc_name(static_cast<u8>(who));
    }
    const Rml::String coins = running_ ? std::to_string(static_cast<i64>(inv("coins"))) : "0";
    const Rml::String copper = running_ ? std::to_string(static_cast<i64>(inv("copper"))) : "0";
    std::string stress;
    if (options_.stress && running_) {
        char text[256];
        std::snprintf(text, sizeof(text), "сущностей %s · частиц %s · кадр %.2f мс · симуляция %.2f мс",
                      with_spaces(entities()).c_str(), with_spaces(particles_.stats().slots_used).c_str(), frame_ms_avg_, sim_ms_);
        stress = text;
    }
    auto dirty = [&](auto& field, const auto& value, const char* name) {
        if (field == value) return;
        field = value;
        h.handle.DirtyVariable(name);
    };
    dirty(h.visible, visible, "visible");
    dirty(h.slots, slots, "slots");
    dirty(h.hint, Rml::String(hint), "hint");
    dirty(h.has_hint, !hint.empty(), "has_hint");
    dirty(h.location, Rml::String(location_), "location");
    dirty(h.coins, coins, "coins");
    dirty(h.copper, copper, "copper");
    dirty(h.stress, Rml::String(stress), "stress");
    dirty(h.stress_on, !stress.empty(), "stress_on");

    // The HUD over the menus' documents, so its hotbar and «Связи» take
    // clicks (its bare body lets them through to the world).
    if (Rml::Context* ctx = shell_->context(); ctx && visible) {
        const int n = ctx->GetNumDocuments();
        for (int i = 0; i + 1 < n; ++i)
            if (Rml::ElementDocument* d = ctx->GetDocument(i); d->GetTitle() == "HUD") {
                d->PullToFront();
                break;
            }
    }
    const u64 now = SDL_GetTicks();
    std::erase_if(lit_, [now](const auto& l) { return l.second <= now; });
    overlay_.update(overlay_.on() ? seen_things() : std::vector<Seen>{}, camera_, width_, height_, links_, verbs_, things_,
                    [this](u32 link) { return lit_.contains(link); });
}

void SliceGame::render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, u32 width, u32 height) {
    // The torches drawn and lit are the ones in view (build_sprites, in update): a window of another size since
    // then gets them for this frame's view, not the last one's.
    const bool resized = width != width_ || height != height_;
    width_ = width;
    height_ = height;
    SDL_GPUColorTargetInfo info{};
    info.texture = target;
    info.clear_color = SDL_FColor{0.47f, 0.68f, 0.90f, 1.0f};
    info.load_op = SDL_GPU_LOADOP_CLEAR;
    info.store_op = SDL_GPU_STOREOP_STORE;
    if (!level_) {
        SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &info, 1, nullptr));
        return;
    }
    if (tiles_world_ != level_->world.get()) {
        tiles_.shutdown();
        if (!init_tiles(tiles_, device_, format_, *level_->world, atlas_)) {
            FORGE_ERROR("slice: the tile renderer did not start");
            SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &info, 1, nullptr));
            return;
        }
        tiles_world_ = level_->world.get();
    }
    if (resized) build_sprites();
    tiles_.prepare(cmd, camera_, width, height);
    sprites_.prepare(cmd, batch_, camera_, width, height);
    lights_.rules().sky_color = sky_light(level_->light.time);
    for (const auto& [x, y] : torches_) lights_.add({x, y, kTorchLight.r, kTorchLight.g, kTorchLight.b});
    if (running_ && hero_alive() && hero_light_) lights_.add({hero_x_, hero_y_ - 0.5, 0.55f, 0.5f, 0.45f}); // a little light to see by
    forge::level::add_light_sources(*level_->scene, lights_);
    lights_.prepare(cmd, *level_->world, camera_, width, height);
    particles_.simulate(cmd, static_cast<f32>(frame_dt_));

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &info, 1, nullptr);
    tiles_.draw_layers(cmd, pass, 0, 2);
    sprites_.draw(cmd, pass);
    tiles_.draw_layers(cmd, pass, kLiquids, 1); // water over whoever swims in it
    particles_.draw(cmd, pass, camera_, width, height);
    lights_.draw(cmd, pass);
    SDL_EndGPURenderPass(pass);
}

} // namespace slice
