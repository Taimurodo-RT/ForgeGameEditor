#include "slice_game.h"

#include "slice_art.h"

#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/time.h"
#include "forge/data/json.h"
#include "forge/script/host.h"
#include "forge/ui/ui.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

FORGE_REFLECT(slice::HeroSave, 1) {
    t.field("x", &slice::HeroSave::x);
    t.field("y", &slice::HeroSave::y);
    t.field("slot", &slice::HeroSave::slot);
    t.field("zoom", &slice::HeroSave::zoom);
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
constexpr f32 kGravity = 40;
constexpr f32 kWalk = 8.5f;
constexpr f32 kJump = 15.5f;
constexpr f32 kReach = 5.5f;     // tiles from the hero's centre to dig or build
constexpr f32 kTalkReach = 2.6f; // to a villager
constexpr f32 kZoom = 30;
constexpr f64 kHearts = 3;

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
SliceGame::~SliceGame() = default;

std::unique_ptr<SliceGame::Level> SliceGame::make_level(const fs::path& save_folder, std::string* error) {
    auto L = std::make_unique<Level>();
    L->world = std::make_unique<World>(world_desc(), gen_);
    if (!save_folder.empty() && !L->world->open_save(save_folder, error)) return nullptr;
    L->scene = std::make_unique<scene::Scene>(*L->world);
    register_components(*L->scene);
    attach_objects(library_, *L->scene);
    if (!save_folder.empty() && !L->scene->open_save(save_folder, error)) return nullptr;
    L->scene->set_populator([this](ChunkCoord c, scene::Scene& s) { populate(*gen_, library_, c, s); });

    SimDesc sd;
    sd.gravity_y = kGravity;
    sd.liquid_layer = kLiquids;
    L->sim = std::make_unique<Simulation>(*L->world, *L->scene, sd);
    for (TileId t = 1; t < TileSliceCount; ++t)
        if (is_solid(t)) L->sim->collision().set(t, TileShape::Solid);
    CellSim& cells = *L->sim->cells();
    LiquidKind water;
    const u8 w = cells.add_liquid(water);
    FORGE_ASSERT(w == kWater);
    (void)w;
    cells.set_falling(TileSand, true);

    flecs::world& ecs = L->scene->ecs();
    L->objects.init(ecs);

    Level* level = L.get();
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
    L->scripts = std::make_unique<script::ScriptHost>(*L->sim, *L->scene);
    L->links = std::make_unique<logic::Runtime>(*L->scripts, library_, *logic_);
    L->links->load(links_, verbs_);
    L->links->attach(*L->scene);
    return L;
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
        static constexpr std::string_view known[] = {"collect", "open", "close", "toggle", "hurt", "heal", "coin", "talk", "follow", "flee"};
        if (std::find(std::begin(known), std::end(known), action) == std::end(known)) return false;
        g_.deeds_.push_back({std::string(action), std::string(thing), target, other});
        return true;
    }
    void hint(flecs::entity_t, std::string_view text) override { g_.hints_.emplace_back(text); }
    void sound(flecs::entity_t at, std::string_view cue) override { g_.cues_.emplace_back(at, std::string(cue)); }
    bool night() override { return false; } // no nights in the slice yet

private:
    SliceGame& g_;
};

void SliceGame::do_deeds() {
    if (!level_) return;
    flecs::world& ecs = level_->scene->ecs();
    auto alive = [&](flecs::entity_t e) { return e && ecs.is_alive(e); };
    std::vector<Deed> deeds;
    deeds.swap(deeds_);
    for (const Deed& d : deeds) {
        if (d.action == "hurt" || d.action == "heal") {
            if (d.target == level_->hero.id()) hurt_hero(d.action == "hurt" ? 1 : -1);
            continue;
        }
        if (d.action == "coin") {
            give("coins", 1, true);
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
            if (const Item* item = e.try_get<Item>()) give(kind_item(static_cast<ItemKind>(item->kind)), item->count, true);
            else give(d.thing, 1, true);
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

f64 SliceGame::hearts() const {
    return shell_->vars().has("hero.hearts") ? shell_->vars().get("hero.hearts").number() : kHearts;
}

// n > 0 hurts, n < 0 heals. With no hearts left the hero wakes up in the
// village.
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
    shell_->vars().set("hero.hearts", kHearts);
    shell_->toast("Герой очнулся в деревне");
    teleport(gen_->spawn_x(), gen_->spawn_y() - kHeroHalfH);
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

bool SliceGame::reload_links(std::string* error) {
    const fs::path dir = shell_->game_dir();
    bool ok = verbs_.load(dir / "verbs.json", error);
    if (ok) ok = links_.load(dir / "logic.json", error);
    if (!ok) return false;
    if (level_) level_->links->load(links_, verbs_);
    return true;
}

// --- lifetime ----------------------------------------------------------------

bool SliceGame::init(game::Shell& shell, SDL_GPUDevice* device, SDL_GPUTextureFormat format) {
    shell_ = &shell;
    if (std::string error; !load_objects(library_, shell.game_dir(), &error)) FORGE_ERROR("%s", error.c_str());
    device_ = device;
    format_ = format;
    atlas_ = make_atlas();
    // The drawn frames and the templates' own pictures under them.
    pictures_.update(library_, make_sheet(), sheet_);
    sounds_.init(library_.sounds_folder(), options_.silent);
    logic_ = std::make_unique<SliceLogic>(*this);
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
    c.BindEventCallback("select", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a) {
        if (!a.empty()) select(static_cast<u32>(a[0].Get<int>()));
    });
    h.handle = c.GetModelHandle();
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
    // A new game starts from the level as the author left it in the editor.
    if (new_game) {
        const fs::path level = options_.level_dir.empty() ? shell_->game_dir() / "level" : options_.level_dir;
        if (!forge::level::copy_level(level, session / "world", error)) return false;
    }
    level_ = make_level(session / "world", error);
    if (!level_) return false;

    HeroSave hs;
    hs.x = gen_->spawn_x();
    hs.y = gen_->spawn_y() - kHeroHalfH;
    hs.zoom = kZoom;
    if (new_game && options_.at) {
        hs.x = options_.at_x;
        hs.y = options_.at_y - kHeroHalfH;
    }
    if (new_game) {
        shell_->vars().set("hero.hearts", kHearts);
        shell_->vars().set("inv.torch", 5);
        shell_->vars().set("hero.dig_speed", 1);
    } else {
        std::vector<u8> bytes;
        data::LoadReport report;
        if (!read_file(session / "hero.json", bytes) ||
            !data::from_json(hs, {reinterpret_cast<const char*>(bytes.data()), bytes.size()}, report)) {
            if (error) *error = "в сохранении нет героя (hero.json)";
            level_.reset();
            return false;
        }
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
    location_ = gen_->location(hero_x_, hero_y_);
    dig_progress_ = 0;
    talking_ = 0;
    return true;
}

bool SliceGame::save(const fs::path& session, std::string& location, std::string* error) {
    if (!level_) return false;
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
    const std::string json = data::to_json(hs);
    if (!write_file_atomic(session / "hero.json", {reinterpret_cast<const u8*>(json.data()), json.size()})) {
        if (error) *error = "hero.json";
        return false;
    }
    location = gen_->location(hs.x, hs.y);
    FORGE_INFO("slice: saved %u chunks of tiles and %u of objects in %.1f ms", w.chunks, s.chunks, w.ms + s.ms);
    return true;
}

void SliceGame::end() {
    if (!running_) return;
    running_ = false;
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
f64 SliceGame::hero_x() const { return hero_alive() ? level_->hero.get<Position>().tile_x() : 0; }
f64 SliceGame::hero_y() const { return hero_alive() ? level_->hero.get<Position>().tile_y() : 0; }
bool SliceGame::on_ground() const { return hero_alive() && (level_->hero.get<Body>().contacts & OnGround); }
TileId SliceGame::tile(u32 layer, i32 x, i32 y) const { return level_ ? level_->world->tile(layer, x, y) : kEmptyTile; }
std::string SliceGame::location() const { return hero_alive() ? gen_->location(hero_x(), hero_y()) : ""; }
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

flecs::entity_t SliceGame::spawn_critter(f64 x, f64 feet_y, Scheme scheme) {
    const objects::Template* t = library_.find("critter");
    if (!level_ || !t) return 0;
    flecs::entity e = library_.spawn(*level_->scene, *t, x, feet_y);
    if (!e.is_valid()) return 0;
    if (Critter* c = e.try_get_mut<Critter>()) c->scheme = static_cast<u8>(scheme);
    return e.id();
}

f64 SliceGame::critter_x(flecs::entity_t id) const {
    if (!level_ || !id) return std::nan("");
    const flecs::entity e(level_->scene->ecs(), id);
    const Position* p = e.is_alive() ? e.try_get<Position>() : nullptr;
    return p ? p->tile_x() : std::nan("");
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
    camera_.x = x;
    camera_.y = y - 2;
    hero_x_ = x;
    hero_y_ = y;
    load_around(x, y);
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
    }
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
    if (shell_->over_world()) {
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
    read_input(input && playing);
    if (!level_->hero.is_alive()) find_hero();
    hero_x_ = hero_x();
    hero_y_ = hero_y();

    const u64 t0 = time_now_ns();
    sync_doors(*level_->scene);
    level_->sim->update(playing ? dt : 0.0, focus());
    sim_ms_ = ns_to_ms(time_now_ns() - t0);
    do_deeds();
    sync_doors(*level_->scene);
    if (!level_->hero.is_alive()) find_hero();
    hero_x_ = hero_x();
    hero_y_ = hero_y();

    sounds_.pause(!playing);
    sounds_.set_listener(hero_x_, hero_y_);
    if (playing) {
        hero_sounds(dt);
        sounds_.update_objects(*level_->scene, hero_x_, hero_y_, dt);
        act(dt);
        pickups();
        location_wait_ -= dt;
        if (location_wait_ <= 0) {
            location_wait_ = 0.5;
            const std::string here = gen_->location(hero_x_, hero_y_);
            if (here != location_) {
                location_ = here;
                shell_->toast(here);
            }
        }
    }
    follow_camera(dt);
    emit_effects(playing ? dt : 0.0);
    build_sprites();
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

    push_objects(batch_, level_->objects, *gen_, camera_.x, camera_.y, alpha, sim.clock().tick(), &pictures_);
    if (running_ && level_->hero.is_alive()) {
        const Position& p = level_->hero.get<Position>();
        const Body& b = level_->hero.get<Body>();
        const Hero& h = level_->hero.get<Hero>();
        f64 x, y;
        draw_position(p, b, alpha, x, y);
        u32 frame = FrameHero;
        if (!(b.contacts & OnGround) && b.liquid == 0) frame = FrameHero + 3;
        else if (std::fabs(b.vx) > 0.5f) frame = FrameHero + 1 + (static_cast<u32>(sim.clock().tick() / 6) & 1u);
        at(x, y, h.facing < 0 ? -1.0f : 1.0f, 2.0f, frame, 4);
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
}

void SliceGame::render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, u32 width, u32 height) {
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
    tiles_.prepare(cmd, camera_, width, height);
    sprites_.prepare(cmd, batch_, camera_, width, height);
    for (const auto& [x, y] : torches_) lights_.add({x, y, kTorchLight.r, kTorchLight.g, kTorchLight.b});
    if (running_ && hero_alive()) lights_.add({hero_x_, hero_y_ - 0.5, 0.55f, 0.5f, 0.45f}); // a little light to see by
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
