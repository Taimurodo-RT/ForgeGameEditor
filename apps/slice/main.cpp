// The vertical slice: «Старая шахта», a small side-view game made the way a
// big one is (see docs/vertical-slice.md).
//
// The game of the module "slice" (step 14.3a): the launcher (apps/game: forge_game, built also as forge_slice and
// packaged as OldMine) reads the game's module in its game.json (none: a game from before, "slice") and hands its
// command line over to slice::run, as forge_slice's main() had it. The lines below work with any of those names.
//
//   forge_slice                     play
//   forge_slice --stress            play with 200 000 critters and a million particles
//   forge_slice --play --level DIR --at X,Y [--fired FILE]
//                                   a new game from a level folder, the hero at X,Y
//                                   (the level editor's «Играть отсюда»; FILE gets the links
//                                   that happen, for its «Логика» tab; F2 shows the links
//                                   over the game and draws new ones into logic.json)
//   forge_slice --test --screenshot out.png [--scene village|mine|door|links|menu|windows|templates|volumes|physics|light|zones|own_tiles
//                                                    |tiled|tiled_update|levels|levels_continue|platformer
//                                                    |platformer_continue|platformer_edges|platformer_template
//                                                    |platformer_template_continue|module_slots|project]
//                                   offscreen: plays the game through and checks it
//                                   (volumes: over a settings.json of music and sounds at 0;
//                                   physics, light, zones: the level of games/examples/physics, light or zones as a
//                                   new game and «Играть отсюда» start it; light also draws the editor's view of it
//                                   to compare; own_tiles: a level with tiles of its own and nothing around it, made
//                                   by the scene, over a copy of the game's data with a «Картинка» template;
//                                   tiled: the level imported from the map of Tiled in games/examples/tiled, over a
//                                   copy of the game's data with what the import wrote, with nothing of Tiled, its
//                                   frame compared with tmxrasterizer's; tiled_update: the same after two pictures of
//                                   the map changed and it was imported again, from the build only, which has the map;
//                                   levels: going between two levels of a copy of the game's data, by links «уходит
//                                   через», and what stays of each, saved in the second; levels_continue, another
//                                   process: «Продолжить» from that save, both levels as they were left;
//                                   platformer: the platformer's rules (enemies, coins and the score, spikes, a pit,
//                                   the end of the game) on a game of two levels with nothing around them, played by
//                                   the keys and saved; platformer_continue, another process: «Продолжить», a win;
//                                   platformer_edges, a third: a link «собирает», the frame a game ends in, no place
//                                   to put the hero back in, with the window «при поражении» and without;
//                                   platformer_template: the template «Платформер» (games/platformer) played through
//                                   «Луг» to «Холмы» by the keys, the hero's frames by the pixels, saved there, lost;
//                                   platformer_template_continue, another process: «Продолжить» and on to the flag;
//                                   module_slots (step 14.3a), with the --user DIR of the test module's probe_save: the
//                                   module's values in the game, the slots of another module or of none refused, a slot
//                                   from before step 14.3a loaded)
//   forge_slice --test --scene project [--play] --data GAME [--level DIR --at X,Y --user DIR] [--edits FILE]
//                                   a game the editor made of a template, as its «Играть» starts it: that game's data,
//                                   title and links, the hero at X,Y, the player's files in DIR; with FILE, what the
//                                   author made in it (apps/common/project_edits.h), met as a player meets it, or, for another game
//                                   of the template, that none of it is there; without --play the game starts at its
//                                   main menu (FILE says to take «Продолжить»)
//   forge_slice --test --window --no-vsync --scene inventory
//                                   10 000 things in a list scrolled to the end and back
//                                   in a real window; the frame times while scrolling go
//                                   to the log and to inventory-scroll.txt
//
// Controls: A/D walk, Space/W jump (and swim), left mouse digs or breaks a
// crate, right mouse builds with the selected slot (1-6), E talks, the
// wheel zooms. Esc pauses, J opens the journal, F5 saves, F9 loads.

#include "slice_art.h"
#include "project_edits.h"
#include "slice_game.h"
#include "slice_level.h"
#include "slice_module.h"

#include "forge/assets/image.h"
#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/core/time.h"
#include "forge/data/json.h"
#include "forge/editor/document.h"
#include "forge/editor/project.h"
#include "forge/audio/screen_sounds.h"
#include "forge/game/runner.h"
#include "forge/game/game_module.h"
#include "forge/game/saves.h"
#include "forge/level/level.h"
#include "forge/level/light.h"
#include "forge/level/tiled_apply.h"
#include "forge/sim/bodies.h"
#include "forge/render/offscreen.h"
#include "forge/world/region_store.h"
#include "forge/script/graph.h"
#include "forge/ui/ui.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/SystemInterface.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Transform.h>
#include <RmlUi/Core/TransformPrimitive.h>

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "simple_pages.h" // the screens of games/examples/simple-mode

using namespace forge;
using namespace forge::game;
using namespace slice;
using forge::edits::ProjectEdits;
using forge::edits::read_edits;

namespace {

// The files of the session folder, by their paths in it: what a refused load must leave as it was (step 14.3a).
std::map<std::string, std::vector<u8>> session_files(const std::filesystem::path& dir) {
    std::map<std::string, std::vector<u8>> out;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::vector<u8> bytes;
        read_file(it->path(), bytes);
        out[path_to_utf8(it->path().lexically_relative(dir))] = std::move(bytes);
    }
    return out;
}

// --scene levels and levels_continue (step 14.2b): a game of two levels, «Деревня» and «Пещера», with links that send
// the hero from one to the other, made by the scene levels in this run's own folder (its data and the player's
// files); levels_continue plays on there in another process, as a player's «Продолжить».
namespace trip {
inline std::filesystem::path root() { return std::filesystem::temp_directory_path() / utf8_path("forge_slice_уровни"); }
// What the first process leaves for the second: the save it made in «Пещера» and what it saw then.
inline std::filesystem::path notes() { return root() / utf8_path("заметки.txt"); }
// The areas: «Двор» (its music; coming into it opens a window), «Метка» (once), «Выход» (to «Пещера», «Вход»),
// «Калитка» (to «Пещера», once) and «Тропа» (to «Пещера», once, its scheme waiting 0.3 s first) on «Деревня»;
// «Вход» (its music; back to «Деревня») and «Грот» on «Пещера».
constexpr u64 kYard = 0x14b2000000000001, kMark = 0x14b2000000000002, kExit = 0x14b2000000000003,
              kEntry = 0x14b2000000000004, kGrotto = 0x14b2000000000005, kGate = 0x14b2000000000006,
              kPath = 0x14b2000000000007;
// The copies the scene puts on «Деревня», by their level ids: «Часы» counting seconds with a sound near them, two
// «Зверька» (one wanders and steps with a sound, one goes by the keys), a falling «Ящик», «Родник» and «Монеты».
constexpr u64 kTicker = 0x14b2000000000101, kCritter = 0x14b2000000000102, kCrate = 0x14b2000000000103,
              kSpring = 0x14b2000000000104, kCoin = 0x14b2000000000105, kPlayer = 0x14b2000000000106,
              kTicker2 = 0x14b2000000000107; // another «Часы», put while the goings are refused

// The links the scene adds to the game's.
constexpr u32 kYardLink = 101, kMarkLink = 102, kGoLink = 103, kGoTooLink = 104, kLateLink = 105, kBackLink = 106,
              kGrottoLink = 107, kGrottoWait = 108, kTickerLink = 109, kSpringLink = 110, kGateLink = 112, kPathLink = 113;
// «Часы» count twice: test.ticks by a link «При старте»: wait a second, +1, again (its waiting is not saved: after a
// load or a coming back it does not go on, as «При старте» does not run again), and test.frames by their own scheme,
// +1 «Каждый шаг».
constexpr u32 kTickerScheme = 111;
} // namespace trip

// --scene platformer and platformer_continue (step 14.2c): the platformer's rules on a game of two levels with nothing
// around them, «Луг» (the start) and «Холм», made by the scene platformer in this run's own folder; platformer_continue
// plays on there in another process, as a player's «Продолжить».
namespace plat {
inline std::filesystem::path root() { return std::filesystem::temp_directory_path() / utf8_path("forge_slice_платформер"); }
inline std::filesystem::path notes() { return root() / utf8_path("заметки.txt"); }
// The areas: «Тропа» (to «Холм», «Вход») and «Возврат» (where the hero comes back) on «Луг»; «Вход» (back to «Луг»,
// «Возврат»), «Яма» (the hero falls into it) and «Финиш» (it reaches it: the game is won) on «Холм».
constexpr u64 kTrail = 0x142c000000000001, kReturn = 0x142c000000000002, kEntry = 0x142c000000000003, kPit = 0x142c000000000004,
              kFinish = 0x142c000000000005;
// The copies on «Луг», by their level ids: a «Монетка», a «Жук» to jump onto, a «Ёж» against a wall, two «Жука» side
// by side, a «Жук» beside a «Ёж»; and the «Жук» the scene drops onto the hero's head.
constexpr u64 kCoin = 0x142c000000000101, kBeetle = 0x142c000000000102, kHedge = 0x142c000000000103, kPairA = 0x142c000000000104,
              kPairB = 0x142c000000000105, kBeside = 0x142c000000000106, kSpiny = 0x142c000000000107, kDropper = 0x142c000000000108;
// On «Холм»: a «Монетка», a «Жук с голосом» (its own «Удар»), a «Ряд шипов»; and the «Бродяга» platformer_continue puts
// in the pen by «Финиш» (it walks, its steps sound).
constexpr u64 kCoin2 = 0x142c000000000201, kLoud = 0x142c000000000202, kSpikes = 0x142c000000000203, kWanderer = 0x142c000000000204;
// The links the scene adds to the game's.
constexpr u32 kTrailLink = 201, kEntryLink = 202, kPitLink = 203, kFinishLink = 204;
// Where they stand: the middles; their feet on the floor.
constexpr f64 kCoinX = 6.5, kBeetleX = 22.5, kHedgeX = 40.5, kPairAX = 55.5, kPairBX = 56.2, kPairX = 55.85, kBesideX = 70.5,
              kSpinyX = 71.4, kDropX = 100.5;
constexpr f64 kCoin2X = 8.5, kLoudX = 18.5, kSpikesX = 30.5, kWandererX = 64.5;
// What platformer_edges puts on them, by their level ids: a «Самоцвет» the link «Герой собирает Самоцвет» takes, a
// «Гудящий зверёк» (a sound near it), a «Монетка» and a «Ёж» where the game is lost, a «Монетка» in «Финиш», a «Ряд
// шипов» over the places the hero is put back in. kGemLink: that link.
constexpr u64 kGem = 0x142c000000000301, kHum = 0x142c000000000302, kEndCoin = 0x142c000000000303, kEndHedge = 0x142c000000000304,
              kWinCoin = 0x142c000000000305, kWinHum = 0x142c000000000306, kOver = 0x142c000000000307,
              kOverGround = 0x142c000000000308;
constexpr u32 kGemLink = 205;
constexpr f64 kGemX = 12.5, kOverX = 1.5; // «Ряд шипов» at kOverX: from 0 to 3
// How far before an enemy the running hero jumps to come down onto it (a jump of 15.5 tiles / s at 8.5 tiles / s).
constexpr f64 kJumpAhead = 6.1;

// The two levels as the level editor writes them (its Level and the game's module): cells, areas, the spawn point,
// the copies with their ids; nothing around (world.json). feet: the floor's top.
// hill_spawn_x: the spawn point of «Холм» (platformer_edges puts it where there is no floor).
inline bool write_levels(const std::filesystem::path& game, f64 feet, std::string& why, f64 hill_spawn_x = 1.5) {
    namespace fs = std::filesystem;
    using namespace forge::world;
    const i32 v = static_cast<i32>(feet);
    struct Put {
        const char* id;
        f64 x;
        u64 level_id;
    };
    auto area = [v](u64 id, const char* name, i32 x0, i32 x1, i32 y0, i32 y1) {
        forge::level::Area a;
        a.id = id;
        a.name = name;
        a.x0 = x0;
        a.x1 = x1;
        a.y0 = v + y0;
        a.y1 = v + y1;
        return a;
    };
    auto one = [&](const char* id, const std::function<void(forge::level::Level&)>& paint, const forge::level::LevelAreas& areas,
                   std::initializer_list<Put> objects) {
        const fs::path dir = forge::level::level_folder(game, id);
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        if (!forge::level::save_world(dir, forge::level::LevelWorld{true}, &why)) return false;
        SliceLevel module;
        if (!module.load_objects(game, &why)) return false;
        forge::level::Level level(module);
        if (!level.open(dir, &why)) return false;
        level.ensure_loaded({-20, v - 30, 170, v + 30});
        paint(level);
        level.set_areas(areas);
        const auto& defs = module.objects();
        for (const Put& p : objects) {
            usize i = 0;
            while (i < defs.size() && defs[i].id != p.id) ++i;
            flecs::entity e = i < defs.size() ? module.place_object(level, i, p.x, feet) : flecs::entity();
            if (!e.is_valid()) {
                why = std::string("не поставлен «") + p.id + "»";
                return false;
            }
            e.set<forge::level::LevelId>({p.level_id});
        }
        level.touch_objects();
        const forge::level::Level::SaveReport r = level.save();
        if (!r.ok) why = std::string(id) + ": " + r.error;
        return r.ok;
    };
    auto column = [](forge::level::Level& l, i32 x, i32 y0, i32 y1) {
        for (i32 y = y0; y < y1; ++y) l.set_tile(kBlocks, x, y, TileStone);
    };
    forge::level::LevelAreas meadow;
    meadow.areas = {area(kReturn, "Возврат", 118, 121, -6, 2), area(kTrail, "Тропа", 128, 131, -6, 2)};
    meadow.spawn = true;
    meadow.spawn_x = 0.5;
    meadow.spawn_y = feet;
    forge::level::LevelAreas hill;
    hill.areas = {area(kEntry, "Вход", 0, 3, -6, 2), area(kPit, "Яма", 40, 43, 1, 14), area(kFinish, "Финиш", 55, 58, -6, 2)};
    hill.spawn = true;
    hill.spawn_x = hill_spawn_x;
    hill.spawn_y = feet;
    return one(
               "level",
               [&](forge::level::Level& l) {
                   for (i32 x = -12; x < 140; ++x)
                       for (i32 y = v; y < v + 6; ++y) l.set_tile(kBlocks, x, y, y == v ? TileGrass : TileDirt);
                   column(l, -12, v - 6, v);
                   column(l, 139, v - 6, v);
                   column(l, 41, v - 4, v); // behind «Ёж»: a hero walking into it stays in it
               },
               meadow,
               {{"coin", kCoinX, kCoin},
                {"beetle", kBeetleX, kBeetle},
                {"hedgehog", kHedgeX, kHedge},
                {"beetle", kPairAX, kPairA},
                {"beetle", kPairBX, kPairB},
                {"beetle", kBesideX, kBeside},
                {"hedgehog", kSpinyX, kSpiny}}) &&
           one(
               "hill",
               [&](forge::level::Level& l) {
                   for (i32 x = -6; x < 80; ++x)
                       for (i32 y = v; y < v + 6; ++y)
                           if (x < 40 || x >= 43) l.set_tile(kBlocks, x, y, y == v ? TileGrass : TileDirt);
                   for (i32 x = 38; x < 45; ++x)
                       for (i32 y = v + 14; y < v + 17; ++y) l.set_tile(kBlocks, x, y, TileStone); // the pit's bottom
                   column(l, -6, v - 6, v);
                   column(l, 79, v - 6, v);
                   column(l, 60, v - 4, v); // the pen of «Бродяга»
                   column(l, 68, v - 4, v);
               },
               hill,
               {{"coin", kCoin2X, kCoin2}, {"loud_beetle", kLoudX, kLoud}, {"spikes_row", kSpikesX, kSpikes}});
}
} // namespace plat

// --scene platformer_template and platformer_template_continue (step 14.2d): the template «Платформер» (games/platformer,
// in a package data/examples/platformer) as a player meets it, a copy of it in this run's own folder; the second
// plays on there in another process, as a player's «Продолжить».
namespace tpl {
inline std::filesystem::path root() { return std::filesystem::temp_directory_path() / utf8_path("forge_slice_шаблон_платформер"); }
inline std::filesystem::path notes() { return root() / utf8_path("заметки.txt"); }
// The template's ids (apps/editor/platformer_template.cpp): copy n of level l («Луг» 0, «Холмы» 1, «Вершина» 2) in
// the order the template puts them, area n of level l.
constexpr u64 id(u64 level, u64 n) { return 0x142d000000000000ull | ((level + 1) << 16) | (n + 1); }
constexpr u64 area(u64 level, u64 n) { return 0x142d00a000000000ull | ((level + 1) << 8) | (n + 1); }
// A «Жук» the scene puts next to the hero, to see it walk.
constexpr u64 kTestBeetle = 0x142d0000000f0001ull;
// The tiles «Трава» and «Мост» of the template's tiles.png.
constexpr world::TileId kGrass = 256, kBridge = 263;
} // namespace tpl

// The self-test: steps run one after another, each over as many frames as
// it needs (a step returns true when it is done).
struct Step {
    const char* name;
    u32 max_frames;
    std::function<bool(u32 frame)> run;
};

class SelfTest {
public:
    SelfTest(SliceGame& game, std::string scene) : g_(game), scene_(std::move(scene)) {}

    // --scene tiled and tiled_update: games/examples/tiled where the game has it, and the folder main() put this
    // run's copy of the game's data (data/) and of the example's level (level/) in.
    std::filesystem::path tiled_example, tiled_root;
    std::string tiled_error; // what main() could not make of them
    // --scene project: the data of the game the editor made (--data), where the editor's «Играть» put the hero
    // (--at) and the folder of the player's files it gave (--user).
    std::filesystem::path project_data, project_user;
    bool project_at = false;
    f64 project_at_x = 0, project_at_y = 0;
    // --edits FILE: what the author made in that game (or that another game has none of it); why it was not read.
    bool project_has_edits = false;
    ProjectEdits project_edits;
    std::string project_edits_error;

    bool frame(Shell& shell, int& failures) {
        if (steps_.empty()) build(shell);
        failures_ = &failures;
        if (index_ >= steps_.size()) return false;
        Step& s = steps_[index_];
        bool done = false;
        if (frames_ > s.max_frames) {
            fail(std::string("шаг «") + s.name + "» не закончился вовремя");
            done = true;
        } else {
            done = s.run(frames_);
        }
        ++frames_;
        if (done) {
            FORGE_INFO("self-test: «%s» (%u frames)", s.name, frames_);
            g_.stop_script();
            ++index_;
            frames_ = 0;
        }
        return true;
    }

private:
    void check(bool ok, const std::string& what) {
        if (!ok) fail(what);
    }
    bool check_ok(bool ok, const std::string& what) {
        check(ok, what);
        return ok;
    }
    void fail(const std::string& what) {
        ++*failures_;
        FORGE_ERROR("self-test: %s (hero at %.1f, %.1f)", what.c_str(), g_.hero_x(), g_.hero_y());
    }
    f64 var(Shell& s, const char* name) { return s.vars().get(name).number(); }
    // Clicks a button with the mouse, the way a player does: whatever lies
    // over it (the HUD, another document) must let the click through.
    bool click(Shell& s, const char* id) { return click(s, s.find_element(id)); }
    bool click(Shell& s, Rml::Element* e) {
        if (!e) return false;
        return click_at(s, e->GetAbsoluteOffset(Rml::BoxArea::Border) + e->GetBox().GetSize(Rml::BoxArea::Border) * 0.5f);
    }
    // At a point of the player's screen (the context's pixels).
    bool click_at(Shell& s, Rml::Vector2f p) {
        SDL_Event ev{};
        ev.type = SDL_EVENT_MOUSE_MOTION;
        ev.motion.x = p.x;
        ev.motion.y = p.y;
        s.handle_event(ev);
        for (const SDL_EventType t : {SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
            ev = {};
            ev.type = t;
            ev.button.button = SDL_BUTTON_LEFT;
            ev.button.down = t == SDL_EVENT_MOUSE_BUTTON_DOWN;
            ev.button.x = p.x;
            ev.button.y = p.y;
            s.handle_event(ev);
        }
        return true;
    }
    // --- the screens' sound (games/examples/screen-sound) ---
    // The example's sounds, made here and written as files with their Russian names (the package does not carry
    // them); the screens' sound reads them from that folder for the step.
    std::filesystem::path sound_files() {
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / utf8_path("forge_slice_звуки_экранов");
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        auto tune = [](std::initializer_list<f32> notes, f32 step) {
            std::vector<audio::Tone> tones;
            f32 at = 0;
            for (f32 hz : notes) {
                tones.push_back({audio::Wave::Triangle, hz, hz, step * 0.95f, 0.01f, 3, 0.35f, at});
                at += step;
            }
            return audio::synth(tones);
        };
        const audio::Tone click[] = {{audio::Wave::Square, 1200, 900, 0.05f, 0.001f, 40, 0.3f}};
        const audio::Tone ring[] = {{audio::Wave::Sine, 1320, 1320, 0.35f, 0.002f, 9, 0.4f}};
        const std::pair<const char*, audio::ClipPtr> files[] = {{"мелодия меню.wav", tune({392, 494, 587, 494}, 0.2f)},
                                                                {"мелодия игры.wav", tune({262, 330, 392, 330}, 0.2f)},
                                                                {"мелодия окна.wav", tune({523, 440, 349, 440}, 0.3f)},
                                                                {"щелчок.wav", audio::synth(click)},
                                                                {"звон.wav", audio::synth(ring)}};
        for (const auto& [name, clip] : files) {
            std::vector<u8> wav;
            check(clip && audio::encode_wav(*clip, wav) && write_file_atomic(dir / utf8_path(name), wav), std::string("звук записан: ") + name);
        }
        return dir;
    }
    // A press of the mouse at a point of a 1920x1080 page, where its fit shows it.
    void press_page(Shell& s, f32 x, f32 y) {
        const Rml::Vector2i size = s.context()->GetDimensions();
        const game::ScreenFit fit = game::fit_screen("expand", 1920, 1080, static_cast<f32>(size.x), static_cast<f32>(size.y));
        press_window(s, game::fit_to_view_x(fit, x), game::fit_to_view_y(fit, y));
    }
    void press_window(Shell& s, f32 x, f32 y) {
        SDL_Event ev{};
        ev.type = SDL_EVENT_MOUSE_MOTION;
        ev.motion.x = x;
        ev.motion.y = y;
        s.handle_event(ev);
        for (const SDL_EventType t : {SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
            ev = {};
            ev.type = t;
            ev.button.button = SDL_BUTTON_LEFT;
            ev.button.down = t == SDL_EVENT_MOUSE_BUTTON_DOWN;
            ev.button.x = x;
            ev.button.y = y;
            s.handle_event(ev);
        }
    }
    void key(Shell& s, SDL_Keycode k, bool down, bool repeat = false) {
        SDL_Event ev{};
        ev.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        ev.key.key = k;
        ev.key.down = down;
        ev.key.repeat = repeat;
        s.handle_event(ev);
    }
    // The button with that layer name on a page.
    static Rml::Element* titled(Rml::Element* e, const char* title) {
        if (!e) return nullptr;
        if (e->HasAttribute("forge-click") && e->GetAttribute<Rml::String>("title", "") == title) return e;
        for (int i = 0; i < e->GetNumChildren(); ++i)
            if (Rml::Element* found = titled(e->GetChild(i), title)) return found;
        return nullptr;
    }
    // Only what plays on the music bus is heard (the world's and the buttons' sounds are muted for one block).
    static f32 music_peak(audio::Mixer& m) {
        m.set_volume(audio::Bus::Sound, 0);
        m.set_volume(audio::Bus::Ui, 0);
        std::vector<f32> buf(4096);
        f32 p = 0;
        for (int i = 0; i < 2; ++i) { // the gains ramp across the first block
            m.mix(buf.data(), 2048);
            p = 0;
            for (f32 v : buf) p = std::max(p, std::fabs(v));
        }
        return p;
    }

    // Goes to the village, then next to a villager; true once there.
    bool near_npc(SliceGame& g, u8 who, u32 f) {
        const f64 v = g.generator().village_y();
        if (f == 0) g.teleport(who == 0 ? 10.5 : -24.5, v - 0.93);
        if (f == 5) {
            const f64 x = g.npc_x(who);
            check(!std::isnan(x), who == 0 ? "Борис дома" : "кузнец дома");
            if (!std::isnan(x)) g.teleport(x + (who == 0 ? -1.0 : 1.0), v - 0.93);
        }
        return f >= 10;
    }

    // A game the editor made of a template and started with «Играть» (step 14.1): the game plays that game's data,
    // its title and links, with the hero where the editor said, and keeps the player's files where it was told.
    void build_project(Shell& s) {
        SliceGame& g = g_;
        steps_.push_back({"игра из папки автора", 5, [&s, this](u32 f) {
            if (f < 2) return false;
            std::error_code ec;
            check(!project_data.empty() && std::filesystem::equivalent(s.game_dir(), project_data, ec),
                  "игра читает данные из " + path_to_utf8(project_data) + ", а не из " + path_to_utf8(s.game_dir()));
            const std::string title = editor::project::game_title(project_data);
            check(!title.empty() && s.title() == title, "название игры из её game.json: «" + s.title() + "», ждали «" + title + "»");
            check(s.data_errors().empty(), "данные игры читаются без ошибок");
            // «Продолжить» (step 14.2b): the game started as a player starts it, at its main menu, and goes on from
            // the newest save.
            const bool resume = project_has_edits && project_edits.go_continue;
            check(s.screen() == (resume ? Screen::Main : Screen::Playing), resume ? "игра начинается с главного меню" : "«Играть» сразу начинает игру");
            if (resume) {
                const std::optional<SlotInfo> latest = s.slots().latest();
                check(latest && latest->id == project_edits.go_save, "последнее сохранение — «" + project_edits.go_save + "»: «" +
                                                                         (latest ? latest->id : std::string()) + "»");
                check(s.continue_game(), "«Продолжить»");
            }
            std::vector<u8> mine, links;
            check(read_file(project_data / "logic.json", mine) && read_file(g_.links_path(), links) && mine == links,
                  "связи «Логики» — из logic.json этой игры");
            if (!project_user.empty())
                check(std::filesystem::equivalent(s.user_folder(), project_user, ec),
                      "файлы игрока — в папке, которую дал редактор: " + path_to_utf8(s.user_folder()));
            return true;
        }});
        steps_.push_back({"герой там, где сказал редактор", 120, [&g, this](u32 f) {
            if (!project_at) return true;
            if (f < 30 && !g.on_ground()) return false;
            check(g.hero_alive() && std::fabs(g.hero_x() - project_at_x) < 1.5 && std::fabs(g.hero_y() - project_at_y) < 3,
                  "герой у точки «Играть»: " + std::to_string(project_at_x) + ", " + std::to_string(project_at_y));
            return true;
        }});
        steps_.push_back({"игра идёт", 60, [&g, this](u32 f) {
            if (f < 60) return false;
            check(g.hero_alive(), "герой жив через секунду игры");
            return true;
        }});
        if (project_has_edits) build_edits(s);
    }

    // The frame the player sees now (the world and the screens over it): its pixels, as wide and tall as the game's.
    bool frame_pixels(Shell& s, std::vector<u8>& rgba, u32& w, u32& h) {
        w = g_.frame_width();
        h = g_.frame_height();
        SDL_GPUDevice* device = g_.device();
        SDL_GPUTexture* target = w && h ? render::create_render_target(device, w, h) : nullptr;
        if (!target) return false;
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
        s.render(cmd, target, w, h);
        SDL_SubmitGPUCommandBuffer(cmd);
        const bool ok = render::read_pixels(device, target, w, h, rgba);
        SDL_ReleaseGPUTexture(device, target);
        return ok;
    }
    static std::array<int, 3> pixel_at(const std::vector<u8>& rgba, u32 w, u32 h, f32 x, f32 y) {
        const i32 px = static_cast<i32>(std::floor(x)), py = static_cast<i32>(std::floor(y));
        if (px < 0 || py < 0 || px >= static_cast<i32>(w) || py >= static_cast<i32>(h) || rgba.size() < static_cast<usize>(w) * h * 4)
            return {-1, -1, -1};
        const usize i = (static_cast<usize>(py) * w + static_cast<usize>(px)) * 4;
        return {rgba[i], rgba[i + 1], rgba[i + 2]};
    }
    static std::string rgb_text(const std::array<int, 3>& p) {
        return std::to_string(p[0]) + "," + std::to_string(p[1]) + "," + std::to_string(p[2]);
    }
    static bool near_rgb(const std::array<int, 3>& a, const std::vector<int>& b, int most) {
        if (b.size() != 3) return false;
        for (int c = 0; c < 3; ++c)
            if (std::abs(a[c] - b[static_cast<usize>(c)]) > most) return false;
        return true;
    }
    // A layer of a screen's page (#n<id>).
    static Rml::Element* layer(Shell& s, const std::string& screen, u32 id) {
        Rml::ElementDocument* doc = s.screens().document(screen);
        return doc ? doc->GetElementById("n" + std::to_string(id)) : nullptr;
    }
    // Where the player sees the middle of a layer of a screen: its place on the page (its own size, 1920 × 1080),
    // through the page's fit onto the player's screen, in the context's pixels.
    static std::optional<Rml::Vector2f> on_screen(Shell& s, const std::string& screen, u32 id) {
        Rml::Element* e = layer(s, screen, id);
        Rml::Element* root = e;
        while (root && !root->HasAttribute("forge-screen")) root = root->GetParentNode();
        if (!e || !root) return std::nullopt;
        f32 w = 1920, h = 1080;
        std::sscanf(root->GetAttribute<Rml::String>("forge-size", "").c_str(), "%f %f", &w, &h);
        const Rml::Vector2f c = e->GetAbsoluteOffset(Rml::BoxArea::Border) - root->GetAbsoluteOffset(Rml::BoxArea::Border) +
                                e->GetBox().GetSize(Rml::BoxArea::Border) * 0.5f;
        const Rml::Vector2i size = s.context()->GetDimensions();
        const game::ScreenFit fit =
            game::fit_screen(root->GetAttribute<Rml::String>("forge-fit", ""), w, h, static_cast<f32>(size.x), static_cast<f32>(size.y));
        return Rml::Vector2f(game::fit_to_view_x(fit, c.x), game::fit_to_view_y(fit, c.y));
    }
    static std::string text_of(Rml::Element* e) {
        if (!e) return {};
        std::string out = e->GetInnerRML();
        // The words without the markup around them.
        std::string plain;
        bool tag = false;
        for (char c : out) {
            if (c == '<') tag = true;
            else if (c == '>') tag = false;
            else if (!tag) plain += c;
        }
        return plain;
    }

    // --scene project --edits FILE (step 14.1b, «Находка»): what the author made through the editor's tabs, met as a
    // player meets it. The own object stands on the level drawn with its picture; the hero touches it: the link adds
    // to the game's value and shows the window, whose text says the value, whose picture is the same and moves by its
    // keys, whose button plays the chosen sound (that file, its length) and closes it; a second touch does nothing.
    // The game is saved and a setting changed: into this game's player's folder. absent: another game of the same
    // template, with none of it and a player's folder of its own.
    // Going between the levels of a game the editor made (step 14.2b), by its links «уходит через»: from the level
    // the game is on, into each area of the plan, and where the hero must be after it; a crate put on a level is found
    // there when the hero comes back, in this process or after «Продолжить» in another; then a save.
    void build_going(Shell& s) {
        namespace fs = std::filesystem;
        SliceGame& g = g_;
        struct State {
            usize leg = 0;
            u32 at = 0;      // the frame the hero came into the area
            std::string was; // the level it came into it on
        };
        auto st = std::make_shared<State>();
        constexpr u64 kMarker = 0x14b2000000000201;
        auto in = [&g](const std::string& thing) {
            u64 id = 0;
            if (!thing.starts_with(logic::kAreaPrefix) || !forge::level::parse_area_id(std::string_view(thing).substr(logic::kAreaPrefix.size()), id))
                return false;
            const std::vector<u64> now = g.areas_inside();
            return std::find(now.begin(), now.end(), id) != now.end();
        };
        auto area = [&g](const std::string& thing) -> const forge::level::Area* {
            u64 id = 0;
            if (!g.areas() || !thing.starts_with(logic::kAreaPrefix) ||
                !forge::level::parse_area_id(std::string_view(thing).substr(logic::kAreaPrefix.size()), id))
                return nullptr;
            return g.areas()->find(id);
        };
        auto mark = [&g, this]() {
            const ProjectEdits& e = project_edits;
            if (g.level_id() == e.go_mark_put && !g.copy_with_id(kMarker) && g.areas() && g.areas()->spawn)
                check(g.spawn_copy("crate", g.areas()->spawn_x + 6.5, g.areas()->spawn_y - 3, kMarker) != 0,
                      "ящик игры поставлен на уровень «" + g.level_id() + "»");
            if (g.level_id() == e.go_mark_find)
                check(g.copy_with_id(kMarker) != 0, "на уровне «" + g.level_id() + "» ящик, который игра там оставила: уровень такой, каким его оставили");
        };
        steps_.push_back({"переходы: с какого уровня", 20, [&s, &g, this, mark](u32 f) {
            if (f < 10) return false;
            const ProjectEdits& e = project_edits;
            check(g.level_id() == e.go_start, "игра на уровне «" + e.go_start + "», а не «" + g.level_id() + "»");
            check(e.go_level.size() == e.go_through.size() && e.go_arrive.size() == e.go_through.size(), "план переходов: по три строки");
            if (e.go_continue) {
                // Where the save has the hero.
                std::vector<u8> bytes;
                HeroSave saved;
                data::LoadReport report;
                check(read_file(s.slots().folder(e.go_save) / "hero.json", bytes) &&
                          data::from_json(saved, {reinterpret_cast<const char*>(bytes.data()), bytes.size()}, report) &&
                          saved.level == e.go_start && std::fabs(g.hero_x() - saved.x) < 0.01 && std::fabs(g.hero_y() - saved.y) < 0.05,
                      "герой там, где его сохранили: " + std::to_string(saved.x) + ", " + std::to_string(saved.y) + " на «" + saved.level + "»");
            }
            mark();
            return true;
        }});
        steps_.push_back({"переходы по связям «уходит через»", 2000, [&s, &g, this, st, in, area, mark](u32 f) {
            const ProjectEdits& e = project_edits;
            if (st->leg >= e.go_through.size() || e.go_level.size() != e.go_through.size() || e.go_arrive.size() != e.go_through.size())
                return true;
            const std::string& through = e.go_through[st->leg];
            if (st->at == 0) {
                // From the level's spawn point (out of the area), then into the area's middle.
                const forge::level::Area* a = area(through);
                if (!check_ok(a != nullptr && g.areas()->spawn, "на уровне «" + g.level_id() + "» зона " + through + " и точка появления"))
                    return true;
                if (f % 20 == 0) {
                    g.teleport(g.areas()->spawn_x, g.areas()->spawn_y - kHeroHalfH - 0.1);
                    return false;
                }
                if (f % 20 == 10) {
                    if (!check_ok(!in(through), "герой не в зоне " + through + " перед тем, как в неё войти")) return true;
                    st->was = g.level_id();
                    st->at = f;
                    g.teleport((a->x0 + a->x1) * 0.5, (a->y0 + a->y1) * 0.5);
                }
                return false;
            }
            if (g.level_id() == st->was && f < st->at + 120) return false;
            const std::string& to = e.go_level[st->leg], &arrive = e.go_arrive[st->leg];
            check(g.level_id() == to && g.travel_problem().empty(),
                  "из зоны " + through + " — на уровень «" + to + "», а герой на «" + g.level_id() + "»: «" + g.travel_problem() + "»");
            if (arrive.empty()) {
                const forge::level::LevelAreas* here = g.areas();
                check(here && here->spawn && std::fabs(g.hero_x() - here->spawn_x) <= 1.0 &&
                          std::fabs(g.hero_y() + kHeroHalfH - here->spawn_y) <= 3.0,
                      "герой у точки появления уровня «" + g.level_id() + "»: " + std::to_string(g.hero_x()) + ", " + std::to_string(g.hero_y()));
            } else {
                check(area(arrive) && in(arrive), "герой в зоне " + arrive + " уровня «" + g.level_id() + "»: " + std::to_string(g.hero_x()) +
                                                      ", " + std::to_string(g.hero_y()));
            }
            mark();
            ++st->leg;
            st->at = 0;
            return st->leg >= e.go_through.size();
        }});
        steps_.push_back({"сохранение после переходов", 10, [&s, &g, this](u32 f) {
            const ProjectEdits& e = project_edits;
            if (f < 5) return false;
            if (e.go_save.empty() || e.go_continue) return true;
            std::error_code ec;
            SDL_Delay(1100); // the newest save by a second
            check(s.save(e.go_save, "После переходов"), "игра сохраняется на уровне «" + g.level_id() + "»");
            for (const std::string& id : e.go_level)
                if (id != g.level_id())
                    check(fs::is_directory(s.slots().folder(e.go_save) / "levels" / utf8_path(id), ec),
                          "в сохранении и уровень «" + id + "», каким его оставили");
            return true;
        }});
    }

    // The platformer the author made through the editor's tabs (step 14.2c), met as a player meets it: the copies
    // where the editor put them, the goal drawn with the author's picture, the author's HUD. The coin gives its «Очки»;
    // the enemy, walked into with the keys, takes its «Урон», once for a touch; a jump onto it gives its «Очки» and it is
    // gone; the zone of «падает в» takes the last heart: the author's window «при поражении», whose «Ещё раз» plays a
    // clean game; the trap hurts as wide as its «Полширины» and no wider and brings the hero back; the goal of «доходит
    // до» wins: the window «при победе», the world stands.
    void build_platform_edits(Shell& s) {
        SliceGame& g = g_;
        struct State {
            f64 ex = 0, ey = 0, cx = 0, cy = 0, tx = 0, ty = 0, fx = 0, fy = 0;
            f64 coins0 = 0, score0 = 0;
            u32 hits0 = 0, stomps0 = 0, hazards0 = 0, falls0 = 0, endings0 = 0, cue0 = 0, at = 0;
        };
        auto st = std::make_shared<State>();
        const f64 v = g.generator().village_y();
        auto put = [&g](f64 feet_x, f64 feet_y) { g.teleport(feet_x, feet_y - kHeroHalfH); };
        auto keys = [&g](bool left, bool right, bool jump) {
            Controls c;
            c.left = left;
            c.right = right;
            c.jump = jump;
            g.script(c);
        };
        auto said = [&s](const std::string& screen, u32 node) { return text_of(layer(s, screen, node)); };
        auto ticks = [&g] { return g.sim_stats() ? g.sim_stats()->ticks : 0u; };
        auto state = [&g, &s, this] {
            auto n = [](f64 x) { return std::to_string(static_cast<i64>(std::llround(x))); };
            return " (очки " + n(g.score()) + ", сердца " + n(g.hearts()) + ", монеты " + n(g.inventory("coins")) + ", герой " +
                   std::to_string(g.hero_x()) + ", " + std::to_string(g.hero_y()) +
                   (s.screens().shown(project_edits.pl_lose) ? ", окно поражения" : "") + (s.screens().shown(project_edits.pl_win) ? ", окно победы" : "") +
                   ")";
        };
        auto score_text = [](f64 n) { return "Очки: " + std::to_string(static_cast<i64>(n)); };
        steps_.push_back({"платформер автора: всё на месте", 150, [&s, &g, this, st, said, put, v](u32 f) {
            const ProjectEdits& e = project_edits;
            if (!check_ok(e.pl_at.size() == 8, "где стоят враг, монетка, ловушка и цель: восемь чисел")) return true;
            if (f == 0) {
                put(e.pl_at[6] - 5, v); // in view of the goal, away from all the rest
                return false;
            }
            if (f < 60 || (!g.on_ground() && f < 120)) return false;
            check(one_of(g, e.pl_enemy, st->ex, st->ey) && one_of(g, e.pl_coin, st->cx, st->cy) && one_of(g, e.pl_trap, st->tx, st->ty) &&
                      one_of(g, e.pl_goal, st->fx, st->fy),
                  "на уровне по одному врагу, монетке, ловушке и цели автора: " + std::to_string(g.copies_of(e.pl_enemy).size()) + ", " +
                      std::to_string(g.copies_of(e.pl_coin).size()) + ", " + std::to_string(g.copies_of(e.pl_trap).size()) + ", " +
                      std::to_string(g.copies_of(e.pl_goal).size()));
            const f64 have[8] = {st->ex, st->ey, st->cx, st->cy, st->tx, st->ty, st->fx, st->fy};
            const char* what[4] = {"враг", "монетка", "ловушка", "цель"};
            for (usize i = 0; i < 4; ++i)
                check(std::fabs(have[i * 2] - e.pl_at[i * 2]) < 0.05 && std::fabs(have[i * 2 + 1] - e.pl_at[i * 2 + 1]) < 0.1,
                      std::string(what[i]) + " там, где поставил редактор: " + std::to_string(have[i * 2]) + ", " + std::to_string(have[i * 2 + 1]) +
                          "; в редакторе " + std::to_string(e.pl_at[i * 2]) + ", " + std::to_string(e.pl_at[i * 2 + 1]));
            check(s.screens().shown(e.pl_hud) && said(e.pl_hud, e.pl_hud_text) == "Очки: 0", "HUD автора поверх игры: «" + said(e.pl_hud, e.pl_hud_text) + "»");
            check(g.hearts() == 3 && g.score() == 0 && !g.won() && !g.lost() && !s.screens().shown(e.pl_lose) && !s.screens().shown(e.pl_win),
                  "новая игра: три сердца, ноль очков, окон конца нет");
            // The goal drawn with the author's picture, from «Ресурсы».
            std::vector<u8> px;
            u32 w = 0, h = 0;
            check(frame_pixels(s, px, w, h), "кадр игры снят");
            const render::Camera2D& c = g.camera();
            const std::array<int, 3> at = pixel_at(px, w, h, static_cast<f32>((st->fx - c.snapped_x()) * c.zoom + w * 0.5),
                                                   static_cast<f32>((st->fy - c.snapped_y()) * c.zoom + h * 0.5));
            check(near_rgb(at, e.pl_goal_color, 24), "цель нарисована картинкой автора: " + rgb_text(at));
            return true;
        }});
        steps_.push_back({"монетка автора: её «Очки», одна монета, один звук", 120, [&g, this, st, said, put, v, state, score_text](u32 f) {
            const ProjectEdits& e = project_edits;
            if (f == 0) {
                st->coins0 = g.inventory("coins");
                st->score0 = g.score();
                st->cue0 = g.sounds().played(Cue::Coins);
                st->at = 0;
                put(st->cx, v);
                return false;
            }
            if (!g.copies_of(e.pl_coin).empty()) return f >= 60 && check_ok(false, "монетка не подобрана" + state());
            if (st->at == 0) st->at = f;
            if (f < st->at + 30) return false; // nothing more comes of it
            check(g.score() == st->score0 + e.pl_coin_score && g.inventory("coins") == st->coins0 + 1,
                  "монетка дала свои " + std::to_string(static_cast<i64>(e.pl_coin_score)) + " очков и одну монету" + state());
            check(g.sounds().played(Cue::Coins) == st->cue0 + 1, "звук монеты один раз: " + std::to_string(g.sounds().played(Cue::Coins) - st->cue0));
            check(said(e.pl_hud, e.pl_hud_text) == score_text(e.pl_coin_score), "HUD: «" + said(e.pl_hud, e.pl_hud_text) + "»");
            return true;
        }});
        steps_.push_back({"враг автора сбоку: его «Урон», отброс, за одно касание один урон", 300, [&g, this, st, put, keys, v, state](u32 f) {
            const ProjectEdits& e = project_edits;
            if (f == 0) {
                st->hits0 = g.enemy_hits();
                st->at = 0;
                put(st->ex - 3, v);
                return false;
            }
            if (st->at == 0) {
                // The keys: right, into it.
                if (f < 10 || g.enemy_hits() == st->hits0) {
                    keys(false, f >= 10, false);
                    return f >= 200 && check_ok(false, "герой дошёл до врага и не ранен" + state());
                }
                keys(false, false, false);
                st->at = f;
                check(g.hearts() == 3 - e.pl_damage, "касание сбоку сняло «Урон» врага: " + std::to_string(static_cast<i64>(e.pl_damage)) + state());
                check(g.hero_x() < st->ex && g.blinking(), "героя отбросило от врага, он мигает" + state());
                return false;
            }
            // Pushed back, then standing in it again while it blinks: that touch hurts no more.
            if (f == st->at + 10) put(st->ex - 0.5, v);
            if (f < st->at + 45) return false;
            check(g.enemy_hits() == st->hits0 + 1 && g.hearts() == 3 - e.pl_damage && g.blinking(), "пока герой мигает, касание не ранит" + state());
            check(g.copies_of(e.pl_enemy).size() == 1, "враг на месте");
            put(st->ex - 6, v);
            return true;
        }});
        steps_.push_back({"прыжок сверху на врага: его «Очки», врага нет", 200, [&s, &g, this, st, said, state, score_text](u32 f) {
            const ProjectEdits& e = project_edits;
            if (f == 0) {
                st->stomps0 = g.stomps();
                st->score0 = g.score();
                return false;
            }
            if (f == 40) g.teleport(st->ex, st->ey - 4); // the safe time over
            if (f <= 40 || (g.stomps() == st->stomps0 && f < 150)) return false;
            check(g.stomps() == st->stomps0 + 1 && g.copies_of(e.pl_enemy).empty(), "прыжок сверху побеждает врага, его больше нет" + state());
            check(g.score() == st->score0 + e.pl_enemy_score && g.hearts() == 3 - e.pl_damage,
                  "его «Очки» в счёт: " + std::to_string(static_cast<i64>(e.pl_enemy_score)) + ", сердца те же" + state());
            const std::string want = score_text(e.pl_coin_score + e.pl_enemy_score);
            check(s.screens().shown(e.pl_hud) && said(e.pl_hud, e.pl_hud_text) == want, "HUD: «" + said(e.pl_hud, e.pl_hud_text) + "», ждали «" + want + "»");
            return true;
        }});
        steps_.push_back({"зона «падает в»: последнее сердце, окно автора «при поражении»", 200, [&s, &g, this, st, said, put, v, state](u32 f) {
            const ProjectEdits& e = project_edits;
            u64 id = 0;
            const bool named = e.pl_pit.starts_with(logic::kAreaPrefix) &&
                               forge::level::parse_area_id(std::string_view(e.pl_pit).substr(logic::kAreaPrefix.size()), id);
            const forge::level::Area* a = named && g.areas() ? g.areas()->find(id) : nullptr;
            if (!check_ok(a != nullptr, "на уровне зона автора " + e.pl_pit)) return true;
            if (f == 0) {
                check(g.hearts() == 1, "у героя одно сердце" + state());
                st->falls0 = g.falls();
                st->endings0 = g.endings();
                st->at = 0;
                put((a->x0 + a->x1) * 0.5, v);
                return false;
            }
            if (st->at == 0) {
                if (!g.lost()) return f >= 60 && check_ok(false, "в зоне игра не проиграна" + state());
                st->at = f;
                check(g.falls() == st->falls0 + 1 && g.hearts() == 0 && g.endings() == st->endings0 + 1,
                      "в зоне герой теряет последнее сердце: игра проиграна, один раз" + state());
                return false;
            }
            if (f < st->at + 30) return false; // the window laid out
            check(g.endings() == st->endings0 + 1 && s.screens().shown(e.pl_lose) && !s.screens().shown(e.pl_win) &&
                      said(e.pl_lose, e.pl_lose_text) == "Итог: поражение",
                  "окно автора «при поражении» показано само: «" + said(e.pl_lose, e.pl_lose_text) + "»" + state());
            std::string why;
            check(!g.can_save(&why) && why == "игра окончена", "игра сохраняться не даёт: «" + why + "»");
            return true;
        }});
        steps_.push_back({"«Ещё раз» в окне автора: чистая новая игра", 300, [&s, &g, this, said, state](u32 f) {
            const ProjectEdits& e = project_edits;
            if (f == 0) {
                const std::optional<Rml::Vector2f> at = on_screen(s, e.pl_lose, e.pl_lose_again);
                check(at && click_at(s, *at), "кнопка «Ещё раз» нажата мышью");
                return false;
            }
            if ((g.lost() || s.screen() != Screen::Playing || !g.on_ground()) && f < 250) return false;
            if (f < 30) return false;
            f64 x = 0, y = 0;
            check(!g.lost() && !g.won() && s.screen() == Screen::Playing && !s.screens().shown(e.pl_lose), "новая игра идёт, окна нет" + state());
            check(g.hearts() == 3 && g.score() == 0 && one_of(g, e.pl_enemy, x, y) && one_of(g, e.pl_coin, x, y),
                  "в ней три сердца, ноль очков, враг и монетка снова на месте" + state());
            check(s.screens().shown(e.pl_hud) && said(e.pl_hud, e.pl_hud_text) == "Очки: 0", "HUD: «" + said(e.pl_hud, e.pl_hud_text) + "»");
            return true;
        }});
        steps_.push_back({"ловушка автора: ранит в своих «Полширины» и не дальше, герой возвращается", 200, [&g, this, st, put, v, state](u32 f) {
            const ProjectEdits& e = project_edits;
            const f64 outside = e.pl_trap_half_w + kHeroHalfW + 0.4, inside = e.pl_trap_half_w + kHeroHalfW - 0.3;
            if (f == 0) {
                st->hazards0 = g.hazard_hits();
                put(st->tx + outside, v);
                return false;
            }
            if (f < 40) return false;
            if (f == 40) {
                check(g.hazard_hits() == st->hazards0 && g.hearts() == 3, "в " + std::to_string(outside) + " от середины ловушки она не ранит" + state());
                put(st->tx + inside, v);
                return false;
            }
            if (g.hazard_hits() == st->hazards0 && f < 100) return false;
            if (f < 140) return false; // back, standing
            f64 bx = 0, by = 0;
            check(g.hazard_hits() == st->hazards0 + 1 && g.hearts() == 3 - e.pl_trap_damage,
                  "в " + std::to_string(inside) + " — ранит на свой «Урон» " + std::to_string(static_cast<i64>(e.pl_trap_damage)) + state());
            check(g.back_point(bx, by) && std::fabs(g.hero_x() - bx) < 0.1 && std::fabs(g.hero_x() - st->tx) > outside,
                  "герой вернулся к точке возвращения " + std::to_string(bx) + state());
            return true;
        }});
        steps_.push_back({"цель автора («доходит до»): победа, окно автора «при победе», мир стоит", 200,
                          [&s, &g, this, st, said, put, v, ticks, state](u32 f) {
            const ProjectEdits& e = project_edits;
            if (f == 0) {
                st->endings0 = g.endings();
                put(st->fx, v);
                return false;
            }
            if (!g.won()) return f >= 60 && check_ok(false, "у цели игра не выиграна" + state());
            if (f < 120) return false;
            check(g.endings() == st->endings0 + 1 && ticks() == 0, "цель — победа, один раз; мир стоит: тиков за кадр " + std::to_string(ticks()) + state());
            check(s.screens().shown(e.pl_win) && !s.screens().shown(e.pl_lose) && said(e.pl_win, e.pl_win_text) == "Итог: победа",
                  "окно автора «при победе» показано само: «" + said(e.pl_win, e.pl_win_text) + "»");
            return true;
        }});
    }
    // The template «Платформер» (step 14.2d) as the author changed it in the tabs of a game made of it, met as a player
    // meets it in a new game: the hero drawn with the picture the author put into «Ресурсы», frame by frame (stands, a
    // step, in the air); the cells the author made solid over a pit hold the hero; a beetle walks at its new «Скорость»;
    // the coin the author put gives the coin's new «Очки» and a coin, its sound once, and the HUD says so; the start
    // level's exit leads where its link now says; there three falls lose the game, and the window's «Ещё раз» plays a
    // new one, where a beetle walked into takes its new «Урон»; through the exit again the flag wins: the window with
    // the author's title and button, the world standing; the button plays a new game.
    void build_template_edits(Shell& s) {
        SliceGame& g = g_;
        struct State {
            u32 at = 0, cue0 = 0, falls0 = 0, hits0 = 0;
            f64 score0 = 0, coins0 = 0, fastest = 0;
            std::map<flecs::entity_t, f64> was;
            std::string level0;
            std::array<u32, 2> seen{}; // the frames of the author's «Копия» seen walking
        };
        auto st = std::make_shared<State>();
        auto put = [&g](f64 feet_x, f64 feet_y) { g.teleport(feet_x, feet_y - kHeroHalfH); };
        auto said = [&s](const std::string& screen, u32 node) { return text_of(layer(s, screen, node)); };
        auto raw = [&s](const std::string& screen) {
            Rml::ElementDocument* doc = s.screens().document(screen);
            return !doc || text_of(doc).find('{') != std::string::npos;
        };
        auto whole = [](f64 x) { return std::to_string(static_cast<i64>(std::llround(x))); };
        auto state = [&g, whole] {
            return " (уровень «" + g.level_id() + "», очки " + whole(g.score()) + ", сердца " + whole(g.hearts()) + ", монеты " +
                   whole(g.inventory("coins")) + ", герой " + std::to_string(g.hero_x()) + ", " + std::to_string(g.hero_y()) + ")";
        };
        auto area = [&g](const std::string& thing) -> const forge::level::Area* {
            u64 id = 0;
            if (!g.areas() || !thing.starts_with(logic::kAreaPrefix) ||
                !forge::level::parse_area_id(std::string_view(thing).substr(logic::kAreaPrefix.size()), id))
                return nullptr;
            return g.areas()->find(id);
        };
        auto in = [&g](const std::string& thing) {
            u64 id = 0;
            if (!thing.starts_with(logic::kAreaPrefix) || !forge::level::parse_area_id(std::string_view(thing).substr(logic::kAreaPrefix.size()), id))
                return false;
            const std::vector<u64> now = g.areas_inside();
            return std::find(now.begin(), now.end(), id) != now.end();
        };
        // The author's coin where it was put (none: false).
        auto coin = [&g, this](f64& x, f64& y) {
            const ProjectEdits& e = project_edits;
            for (flecs::entity_t c : g.copies_of(e.tp_coin))
                if (e.tp_coin_at.size() == 2 && g.position_of(c, x, y) && std::fabs(x - e.tp_coin_at[0]) < 0.05 && std::fabs(y - e.tp_coin_at[1]) < 0.1)
                    return true;
            return false;
        };
        // The middle of the hero as the screen shows it now (drawn in this frame), and the author's colour of a frame.
        auto hero_seen = [&s, &g, this](std::array<int, 3>& at) {
            std::vector<u8> px;
            u32 w = 0, h = 0;
            if (!frame_pixels(s, px, w, h)) return false;
            const render::Camera2D& c = g.camera();
            at = pixel_at(px, w, h, static_cast<f32>((g.hero_drawn_x() - c.snapped_x()) * c.zoom + w * 0.5),
                          static_cast<f32>((g.hero_drawn_y() - c.snapped_y()) * c.zoom + h * 0.5));
            return true;
        };
        auto colour = [this](u32 frame) {
            const std::vector<int>& h = project_edits.tp_hero;
            return h.size() == 12 && frame < 4 ? std::vector<int>(h.begin() + frame * 3, h.begin() + frame * 3 + 3) : std::vector<int>{};
        };
        auto frame_seen = [&g, hero_seen, colour](const char* what) {
            std::array<int, 3> at{};
            const u32 k = g.hero_frame();
            return std::make_pair(hero_seen(at) && near_rgb(at, colour(k), 24),
                                  std::string(what) + ": кадр " + std::to_string(k) + " новой картинки автора на экране: " + rgb_text(at));
        };
        steps_.push_back({"шаблон автора: новая игра, герой картинкой из «Ресурсов», HUD", 200, [&s, &g, this, said, raw, frame_seen, state](u32 f) {
            const ProjectEdits& e = project_edits;
            if (!check_ok(e.tp_hero.size() == 12, "цвета четырёх кадров героя: двенадцать чисел")) return true;
            if (f == 0) {
                check(s.new_game(), "новая игра");
                return false;
            }
            if (f < 30 || (!g.on_ground() && f < 150)) return false;
            check(g.level_id() == e.level && g.hearts() == 3 && g.score() == 0 && !g.won() && !g.lost(),
                  "новая игра на «" + e.level_name + "»: три сердца, ноль очков" + state());
            check(g.hero_pictured() && g.hero_frame() == 0, "герой нарисован своей картинкой, кадр «стоит»");
            const auto [seen, why] = frame_seen("стоит");
            check(seen, why);
            check(s.screens().shown(e.tp_hud) && said(e.tp_hud, e.tp_hud_score) == "Очки: 0" && !raw(e.tp_hud),
                  "HUD поверх игры, без сырых {…}: «" + said(e.tp_hud, e.tp_hud_score) + "»");
            return true;
        }});
        steps_.push_back({"кадры новой картинки: шаг и прыжок", 200, [&g, this, st, frame_seen](u32 f) {
            if (f < 12) {
                Controls c;
                c.left = true;
                g.script(c);
                return false;
            }
            if (f == 12) {
                g.script(Controls{});
                check(g.on_ground() && (g.hero_frame() == 1 || g.hero_frame() == 2), "идёт: кадр шага, а не " + std::to_string(g.hero_frame()));
                const auto [seen, why] = frame_seen("шаг");
                check(seen, why);
                return false;
            }
            if (f == 50) {
                g.teleport(g.hero_x(), g.hero_y() - 3);
                return false;
            }
            if (f == 56) {
                check(!g.on_ground() && g.hero_frame() == 3, "в воздухе: кадр «в воздухе», а не " + std::to_string(g.hero_frame()));
                const auto [seen, why] = frame_seen("в воздухе");
                check(seen, why);
            }
            return f > 56 && (g.on_ground() || f >= 190);
        }});
        steps_.push_back({"мостки автора над ямой держат героя", 120, [&g, this, st, put, state](u32 f) {
            const ProjectEdits& e = project_edits;
            const std::vector<int>& b = e.tp_bridge;
            if (!check_ok(b.size() == 3, "мостки автора: x0, x1, y")) return true;
            if (f == 0) {
                st->falls0 = g.falls();
                put((b[0] + b[1] + 1) * 0.5, b[2] - 1.0);
                return false;
            }
            if (f < 60) return false;
            check(g.on_ground() && std::fabs(g.hero_y() + kHeroHalfH - b[2]) < 0.05 && g.falls() == st->falls0 && g.level_id() == e.level,
                  "герой стоит на мостках автора над ямой, не падает" + state());
            return true;
        }});
        steps_.push_back({"«Жук» автора ходит со своей «Скоростью»", 120, [&g, this, st](u32 f) {
            const ProjectEdits& e = project_edits;
            if (f == 0) {
                st->was.clear();
                st->fastest = 0;
            }
            for (flecs::entity_t c : g.copies_of(e.tp_enemy)) {
                f64 x = 0, y = 0;
                if (!g.position_of(c, x, y)) continue;
                const auto was = st->was.find(c);
                if (was != st->was.end() && std::fabs(x - was->second) < 0.5) st->fastest = std::max(st->fastest, std::fabs(x - was->second) * 60);
                st->was[c] = x;
            }
            if (f < 90) return false;
            check(!st->was.empty() && std::fabs(st->fastest - e.tp_enemy_speed) < 0.1,
                  "«Жук» идёт со «Скоростью» автора " + std::to_string(e.tp_enemy_speed) + " клетки в секунду: " + std::to_string(st->fastest));
            return true;
        }});
        steps_.push_back({"монетка автора: новые «Очки», монета, звук, HUD", 150, [&g, this, st, put, said, whole, state, coin](u32 f) {
            const ProjectEdits& e = project_edits;
            f64 x = 0, y = 0;
            if (f == 0) {
                if (!check_ok(coin(x, y), "монетка автора там, где её поставили: " + std::to_string(g.copies_of(e.tp_coin).size()) + " монеток"))
                    return true;
                st->score0 = g.score();
                st->coins0 = g.inventory("coins");
                st->cue0 = g.sounds().played(Cue::Coins);
                st->at = 0;
                put(x, std::ceil(y));
                return false;
            }
            if (coin(x, y)) return f >= 60 && check_ok(false, "монетка автора не подобрана" + state());
            if (!st->at) st->at = f;
            if (f < st->at + 30) return false; // nothing more comes of it
            check(g.score() == st->score0 + e.tp_coin_score && g.inventory("coins") == st->coins0 + 1,
                  "монетка дала новые " + whole(e.tp_coin_score) + " очков и одну монету" + state());
            check(g.sounds().played(Cue::Coins) == st->cue0 + 1, "звук монеты один раз: " + std::to_string(g.sounds().played(Cue::Coins) - st->cue0));
            check(said(e.tp_hud, e.tp_hud_score) == "Очки: " + whole(g.score()), "HUD: «" + said(e.tp_hud, e.tp_hud_score) + "»");
            return true;
        }});
        // Into the start level's exit: the level and the area its link now names.
        auto through = [&g, this, st, area, in, state](u32 f, u32 most) {
            const ProjectEdits& e = project_edits;
            if (f == 0) {
                const forge::level::Area* a = area(e.tp_exit);
                if (!check_ok(a != nullptr, "на уровне «" + g.level_id() + "» выход " + e.tp_exit)) return true;
                st->level0 = g.level_id();
                g.teleport((a->x0 + a->x1) * 0.5, (a->y0 + a->y1) * 0.5);
                return false;
            }
            if (g.level_id() == st->level0 && f < most) return false;
            check(g.level_id() == e.tp_exit_level && g.travel_problem().empty() && in(e.tp_exit_arrive),
                  "через выход — на «" + e.tp_exit_level + "», в зону " + e.tp_exit_arrive + state());
            return true;
        };
        steps_.push_back({"выход ведёт, куда указал автор", 200, [through](u32 f) { return through(f, 150); }});
        steps_.push_back({"три падения в «Пропасть»: поражение, окно «при поражении»", 700, [&s, &g, this, st, area, raw, state](u32 f) {
            const ProjectEdits& e = project_edits;
            const forge::level::Area* p = area(e.tp_pit);
            if (!check_ok(p != nullptr, "на уровне «" + g.level_id() + "» пропасть " + e.tp_pit)) return true;
            if (f == 0) {
                st->falls0 = g.falls();
                st->at = 0;
            }
            if (!g.lost()) {
                g.script(Controls{});
                if (g.safe_time() <= 0 && g.on_ground() && f >= st->at + 10) {
                    g.teleport((p->x0 + p->x1) * 0.5, p->y0 + 1.5);
                    st->at = f;
                }
                return f >= 650 && check_ok(false, "поражения нет" + state());
            }
            if (f < st->at + 40) return false; // the window comes up
            check(g.falls() == st->falls0 + 3 && g.hearts() == 0 && g.endings() == 1, "три падения, сердец нет" + state());
            check(s.screens().shown(e.tp_lose) && !raw(e.tp_lose), "окно «при поражении» без сырых {…}");
            std::string why;
            check(!g.can_save(&why), "оконченную игру не сохранить: " + why);
            return true;
        }});
        steps_.push_back({"«Ещё раз»: новая игра на стартовом уровне", 120, [&s, &g, this, state, coin](u32 f) {
            const ProjectEdits& e = project_edits;
            if (f == 0) {
                const std::optional<Rml::Vector2f> at = on_screen(s, e.tp_lose, e.tp_again);
                check(at && click_at(s, *at), "«Ещё раз» нажата мышью");
                return false;
            }
            if (f < 40) return false;
            f64 x = 0, y = 0;
            check(g.running() && !g.lost() && g.endings() == 0 && !s.screens().shown(e.tp_lose), "новая игра, окна нет" + state());
            check(g.level_id() == e.level && g.hearts() == 3 && g.score() == 0 && coin(x, y), "на «" + e.level_name + "», три сердца, монетка автора снова лежит" + state());
            return true;
        }});
        steps_.push_back({"«Жук» автора сбоку: его новый «Урон»", 300, [&g, this, st, put, state](u32 f) {
            const ProjectEdits& e = project_edits;
            // The beetle ahead of the hero: the nearest one.
            auto beetle = [&g, &e](f64& x, f64& y) {
                bool any = false;
                f64 best = 1e9;
                for (flecs::entity_t c : g.copies_of(e.tp_enemy)) {
                    f64 cx = 0, cy = 0;
                    if (g.position_of(c, cx, cy) && std::fabs(cx - g.hero_x()) < best) {
                        best = std::fabs(cx - g.hero_x());
                        x = cx;
                        y = cy;
                        any = true;
                    }
                }
                return any;
            };
            f64 x = 0, y = 0;
            if (f == 0) {
                if (!check_ok(beetle(x, y), "на уровне «" + g.level_id() + "» есть «Жук» автора")) return true;
                st->hits0 = g.enemy_hits();
                st->at = 0;
                put(x - 3, std::ceil(y));
                return false;
            }
            Controls c;
            if (!st->at) {
                if (f < 10 || g.enemy_hits() == st->hits0) {
                    c.right = f >= 10;
                    g.script(c);
                    return f >= 250 && check_ok(false, "герой дошёл до «Жука» и не ранен" + state());
                }
                g.script(c);
                st->at = f;
                check(g.enemy_hits() == st->hits0 + 1 && g.hearts() == 3 - e.tp_enemy_damage && g.blinking(),
                      "касание сбоку сняло новый «Урон» «Жука» " + std::to_string(static_cast<i64>(e.tp_enemy_damage)) + ", герой мигает" + state());
                return false;
            }
            return f >= st->at + 5;
        }});
        steps_.push_back({"снова через выход", 200, [through](u32 f) { return through(f, 150); }});
        steps_.push_back({"«Флаг»: победа, окно автора «при победе»", 200, [&s, &g, this, st, said, raw, put, state](u32 f) {
            const ProjectEdits& e = project_edits;
            const std::vector<flecs::entity_t> flags = g.copies_of(e.tp_goal);
            f64 x = 0, y = 0;
            if (f == 0) {
                if (!check_ok(flags.size() == 1 && g.position_of(flags[0], x, y), "на «" + g.level_id() + "» один «Флаг»")) return true;
                put(x, std::ceil(y));
                st->at = 0;
                return false;
            }
            if (!g.won()) return f >= 100 && check_ok(false, "у «Флага» победы нет" + state());
            if (!st->at) st->at = f;
            if (f < st->at + 60) return false;
            const u32 ticks = g.sim_stats() ? g.sim_stats()->ticks : 0u;
            check(g.endings() == 1 && ticks == 0, "победа один раз, мир стоит: тиков за кадр " + std::to_string(ticks) + state());
            check(s.screens().shown(e.tp_win) && said(e.tp_win, e.tp_win_title) == e.tp_win_text && !raw(e.tp_win),
                  "окно «при победе» с заголовком автора: «" + said(e.tp_win, e.tp_win_title) + "»");
            check(said(e.tp_win, e.tp_win_again_label) == e.tp_win_again_text, "и кнопкой автора: «" + said(e.tp_win, e.tp_win_again_label) + "»");
            return true;
        }});
        steps_.push_back({"кнопка автора после победы: новая игра", 120, [&s, &g, this, state, coin](u32 f) {
            const ProjectEdits& e = project_edits;
            if (f == 0) {
                const std::optional<Rml::Vector2f> at = on_screen(s, e.tp_win, e.tp_win_again);
                check(at && click_at(s, *at), "«" + e.tp_win_again_text + "» нажата мышью");
                return false;
            }
            if (f < 40) return false;
            f64 x = 0, y = 0;
            check(g.running() && !g.won() && g.endings() == 0 && !s.screens().shown(e.tp_win), "новая игра, окна нет" + state());
            check(g.level_id() == e.level && g.hearts() == 3 && g.score() == 0 && coin(x, y), "на «" + e.level_name + "», три сердца, монетка автора лежит" + state());
            return true;
        }});
        // The author's «Копия» of «Жук»: its own template, the same strip of two frames, drawn so on the level.
        steps_.push_back({"«Копия» «Жука» автора: те же два кадра на уровне", 200, [&s, &g, this, st](u32 f) {
            const ProjectEdits& e = project_edits;
            if (e.tp_twin.empty()) return true;
            constexpr u64 kTwin = 0x142d7e57000001ull;
            if (f == 0) {
                const Pictures::Picture *twin = g.picture_of(e.tp_twin), *beetle = g.picture_of(e.tp_enemy);
                check(twin && beetle && twin->frames == 2 && beetle->frames == 2 && twin->aspect == beetle->aspect,
                      "«Копия» рисуется полосой «Жука»: кадров " + std::to_string(twin ? twin->frames : 0));
                check(g.spawn_copy(e.tp_twin, g.hero_x() + 13, 0, kTwin) != 0, "«Копия» на «Луге»");
                st->was.clear();
                st->at = 0;
                return false;
            }
            const flecs::entity_t c = g.copy_with_id(kTwin);
            f64 x = 0, y = 0;
            if (!c || !g.position_of(c, x, y)) return check_ok(false, "«Копии» на уровне нет");
            if (f < 10) {
                st->was[c] = x;
                return false;
            }
            const bool walking = std::fabs(x - st->was[c]) > 0.01;
            st->was[c] = x;
            if (walking) {
                std::vector<u8> px;
                u32 w = 0, h = 0;
                std::vector<u8> bytes;
                assets::CookedTexture strip;
                const objects::Template* t = g.library().find(e.tp_twin);
                if (t && read_file(g.library().picture_file(*t), bytes) && assets::decode_image(bytes, strip) && frame_pixels(s, px, w, h)) {
                    const Match m = drawn_frame(px, w, h, g.camera(), x, y, 1, 1, strip, 2, 4);
                    if (m.score > 0.85) ++st->seen[m.frame];
                }
            }
            if (f < 150 && (st->seen[0] < 3 || st->seen[1] < 3)) return false;
            check(st->seen[0] >= 3 && st->seen[1] >= 3,
                  "идёт: на экране оба кадра, а не вся полоса: " + std::to_string(st->seen[0]) + " и " + std::to_string(st->seen[1]));
            return true;
        }});
    }
    // The tab «Анимация»: the author's animations as the game plays them. The game's own clips of the templates are
    // the author's, and the states the author left are the game's rule. The hero stands, walks and jumps; a copy of the
    // object that stands and one of the walker put beside it: each drawn with the frame its animation has at the tick
    // it is drawn at. The game says that frame, and the screen shows it: its pixels there match that frame of the strip
    // well, and at least as well as any other frame of it. Every frame of each animation is seen so.
    void build_anim_edits(Shell& s) {
        SliceGame& g = g_;
        struct Look {
            u32 looks = 0, bad = 0;
            std::array<u32, objects::kMaxClipFrames> seen{};
            std::string first_bad;
            u64 longest = 0; // ticks into the pose, the most looked at
        };
        struct State {
            Look stand, walk, air, still, walker;
            std::map<flecs::entity_t, f64> was;
        };
        constexpr u64 kStill = 0x14a1a000000001ull, kWalker = 0x14a1a000000002ull; // the copies' level ids
        auto st = std::make_shared<State>();
        auto pose_named = [](const std::string& id, Pose& out) {
            for (usize i = 0; i < kPoses; ++i)
                if (id == pose_id(static_cast<Pose>(i))) {
                    out = static_cast<Pose>(i);
                    return true;
                }
            return false;
        };
        // The author's animation of a template in a state (false: they gave it none).
        auto authored = [this](const std::string& of, const std::string& state, objects::Clip& out) {
            const ProjectEdits& e = project_edits;
            usize from = 0;
            for (usize i = 0; i < e.an_of.size() && i < e.an_state.size() && i < e.an_count.size() && i < e.an_fps.size() && i < e.an_loop.size(); ++i) {
                const usize n = static_cast<usize>(std::max(0, e.an_count[i]));
                if (e.an_of[i] == of && e.an_state[i] == state && from + n <= e.an_frames.size()) {
                    out = {};
                    for (usize k = from; k < from + n; ++k) out.frames.push_back(static_cast<u32>(e.an_frames[k]));
                    out.fps = static_cast<f32>(e.an_fps[i]);
                    out.loop = e.an_loop[i] != 0;
                    return true;
                }
                from += n;
            }
            return false;
        };
        // What a template plays in a pose by the author: their animation, else the game's rule for its frames.
        auto expected = [&g, authored](const std::string& of, Pose pose) {
            objects::Clip c;
            const objects::Template* t = g.library().find(of);
            if (authored(of, pose_id(pose), c) || !t) return c;
            return pose_rule(mover_of(g.library(), *t), pose, t->frames);
        };
        // One look: the frame the game says it drew (frame of the strip) against the author's (want), and the screen
        // there (a box of w × h tiles around x, y) against the picture's strip.
        auto look = [&s, &g, this](Look& l, const std::string& what, const std::string& of, u32 said, u32 want, u64 into, f64 x, f64 y, f64 w, f64 h) {
            ++l.looks;
            l.longest = std::max(l.longest, into);
            std::string bad;
            const Pictures::Picture* pic = g.picture_of(of);
            assets::CookedTexture strip;
            std::vector<u8> bytes, px;
            u32 sw = 0, sh = 0;
            const objects::Template* t = g.library().find(of);
            if (said != want) bad = "игра рисует кадр " + std::to_string(said + 1) + ", а по анимации автора " + std::to_string(want + 1);
            else if (!pic || !t || !read_file(g.library().picture_file(*t), bytes) || !assets::decode_image(bytes, strip) || !frame_pixels(s, px, sw, sh))
                bad = "картинка или кадр экрана не читаются";
            else {
                f64 mine = 0, other = 0;
                u32 rival = 0;
                for (u32 k = 0; k < pic->frames; ++k) {
                    const Match m = drawn_frame(px, sw, sh, g.camera(), x, y, w, h, strip, pic->frames, 3, 40, static_cast<i32>(k));
                    if (k == want) mine = m.score;
                    else if (m.score > other) other = m.score, rival = k;
                }
                char buf[96];
                std::snprintf(buf, sizeof buf, "на экране кадр %u совпал на %.2f, кадр %u на %.2f", want + 1, mine, rival + 1, other);
                if (mine < 0.9 || mine < other) bad = buf;
            }
            if (bad.empty()) {
                if (want < l.seen.size()) ++l.seen[want];
                return;
            }
            if (!l.bad++) l.first_bad = what + ", тик позы " + std::to_string(into) + ": " + bad;
            if (l.bad <= 5) FORGE_WARN("%s, тик позы %llu: %s", what.c_str(), static_cast<unsigned long long>(into), bad.c_str());
        };
        // Every frame of the clip seen on the screen, none wrong.
        auto all_seen = [this](const Look& l, const std::string& what, const objects::Clip& c, u32 at_least) {
            std::string missing, counts;
            for (u32 k : c.frames)
                if (k >= l.seen.size() || l.seen[k] == 0) missing += " " + std::to_string(k + 1);
            for (u32 k = 0; k < l.seen.size(); ++k)
                if (l.seen[k]) counts += " " + std::to_string(k + 1) + "×" + std::to_string(l.seen[k]);
            check(l.bad == 0, what + ": не тот кадр " + std::to_string(l.bad) + " раз из " + std::to_string(l.looks) + ", первый: " + l.first_bad);
            check(l.looks >= at_least && missing.empty(), what + ": взглядов " + std::to_string(l.looks) + ", кадры на экране:" + counts +
                                                              (missing.empty() ? std::string() : ", не видно:" + missing));
            FORGE_INFO("%s: взглядов %u, не тот кадр %u, кадры на экране:%s", what.c_str(), l.looks, l.bad, counts.c_str());
        };

        steps_.push_back({"анимации автора: игра играет их, остальное по своему правилу", 3, [&g, this, pose_named, authored, expected](u32 f) {
            const ProjectEdits& e = project_edits;
            if (f < 1) return false;
            usize frames = 0;
            for (int n : e.an_count) frames += static_cast<usize>(std::max(0, n));
            if (!check_ok(!e.an_of.empty() && e.an_state.size() == e.an_of.size() && e.an_count.size() == e.an_of.size() &&
                              e.an_fps.size() == e.an_of.size() && e.an_loop.size() == e.an_of.size() && frames == e.an_frames.size(),
                          "анимации автора: по шаблону, состоянию, числу кадров, кадрам, кадрам в секунду и повтору"))
                return true;
            for (const std::string* of : {&e.an_hero, &e.an_walker, &e.an_still}) {
                const objects::Template* t = g.library().find(*of);
                const Pictures::Picture* pic = g.picture_of(*of);
                if (!check_ok(t && pic, "шаблон «" + *of + "» с картинкой в игре")) continue;
                for (Pose pose : poses_of(mover_of(g.library(), *t))) {
                    objects::Clip mine;
                    const bool own = authored(*of, pose_id(pose), mine);
                    const objects::Clip want = expected(*of, pose), have = pic->clips[static_cast<usize>(pose)];
                    check(have == want, "«" + t->name + "», «" + pose_name(pose) + "»: " + (own ? "анимация автора" : "правило игры") + ", кадров " +
                                            std::to_string(have.frames.size()) + " (" + std::to_string(want.frames.size()) + "), " +
                                            std::to_string(have.fps) + " к/с (" + std::to_string(want.fps) + ")");
                }
            }
            for (usize i = 0; i < e.an_of.size(); ++i) {
                Pose pose{};
                const objects::Template* t = g.library().find(e.an_of[i]);
                const std::vector<Pose> poses = t ? poses_of(mover_of(g.library(), *t)) : std::vector<Pose>{};
                check(pose_named(e.an_state[i], pose) && std::find(poses.begin(), poses.end(), pose) != poses.end(),
                      "«" + e.an_state[i] + "» — состояние, в котором игра рисует «" + e.an_of[i] + "»");
            }
            return true;
        }});
        steps_.push_back({"новая игра: герой стоит кадром своего состояния", 200, [&s, &g, this, st, look, expected](u32 f) {
            const ProjectEdits& e = project_edits;
            if (f == 0) {
                check(s.new_game(), "новая игра");
                return false;
            }
            if (f < 30 || (!g.on_ground() && f < 150)) return false;
            if (g.hero_pose() == Pose::Stand && !g.blinking()) {
                const objects::Clip c = expected(e.an_hero, Pose::Stand);
                const u64 into = g.drawn_tick() - g.hero_pose_since();
                look(st->stand, "стоит", e.an_hero, g.hero_picture_frame(), objects::clip_frame(c, into), into, g.hero_drawn_x(), g.hero_drawn_y(),
                     1, 2);
            }
            if (f < 50) return false;
            check(g.hero_pictured() && st->stand.looks >= 10 && st->stand.bad == 0,
                  "герой стоит кадром «Стоит»: взглядов " + std::to_string(st->stand.looks) + ", не тот " + std::to_string(st->stand.bad) + " " +
                      st->stand.first_bad);
            return true;
        }});
        steps_.push_back({"герой идёт кадрами анимации автора", 70, [&g, this, st, look, expected, all_seen](u32 f) {
            const ProjectEdits& e = project_edits;
            // Right, left, right: on «Луг» from its start, clear of the wall and of what lies further.
            Controls c;
            c.right = f < 20 || (f >= 40 && f < 60);
            c.left = f >= 20 && f < 40;
            g.script(f < 60 ? c : Controls{});
            const objects::Clip clip = expected(e.an_hero, Pose::Walk);
            if (g.on_ground() && g.hero_pose() == Pose::Walk && !g.blinking()) {
                const u64 into = g.drawn_tick() - g.hero_pose_since();
                look(st->walk, "идёт", e.an_hero, g.hero_picture_frame(), objects::clip_frame(clip, into), into, g.hero_drawn_x(), g.hero_drawn_y(),
                     1, 2);
            }
            if (f < 62) return false;
            all_seen(st->walk, "идёт", clip, 30);
            return true;
        }});
        steps_.push_back({"герой в воздухе кадрами анимации автора, без повтора стоит на последнем", 200, [&g, this, st, look, expected, all_seen](u32 f) {
            const ProjectEdits& e = project_edits;
            const objects::Clip clip = expected(e.an_hero, Pose::Air);
            if (f == 0) {
                g.teleport(g.hero_x(), g.hero_y() - 6);
                return false;
            }
            if (!g.on_ground() && g.hero_pose() == Pose::Air && !g.blinking()) {
                const u64 into = g.drawn_tick() - g.hero_pose_since();
                look(st->air, "в воздухе", e.an_hero, g.hero_picture_frame(), objects::clip_frame(clip, into), into, g.hero_drawn_x(),
                     g.hero_drawn_y(), 1, 2);
            }
            if (f < 4 || (!g.on_ground() && f < 190)) return false;
            all_seen(st->air, "в воздухе", clip, 15);
            const u64 whole = static_cast<u64>(objects::clip_ticks(clip.fps)) * clip.frames.size();
            check(clip.loop || st->air.longest >= whole,
                  "в воздухе дольше всей анимации (взгляд на тике позы " + std::to_string(st->air.longest) + ", в ней " + std::to_string(whole) +
                      " тиков): после неё её последний кадр");
            return true;
        }});
        steps_.push_back({"копия того, что стоит, кадрами анимации автора", 160, [&g, this, st, look, expected, all_seen](u32 f) {
            const ProjectEdits& e = project_edits;
            const objects::Clip clip = expected(e.an_still, Pose::Idle);
            if (f == 0) {
                check(g.spawn_copy(e.an_still, g.hero_x() - 3, g.hero_y() + kHeroHalfH, kStill) != 0, "копия «" + e.an_still + "» рядом с героем");
                return false;
            }
            const flecs::entity_t c = g.copy_with_id(kStill);
            f64 x = 0, y = 0, w = 0, h = 0;
            const Pictures::Picture* pic = g.picture_of(e.an_still);
            if (!c || !pic || !g.position_of(c, x, y) || !g.body_size(c, w, h)) return check_ok(false, "копии «" + e.an_still + "» нет");
            if (f >= 3) {
                const u64 into = g.drawn_tick();
                const u32 want = objects::clip_frame(clip, into);
                // What the game draws it with: the frame of its picture by the level's clock.
                const u32 said = pic->at(Pose::Idle, into) - pic->frame;
                look(st->still, "стоит", e.an_still, said, want, into, x, y, h * pic->aspect, h);
            }
            if (f < 150) return false;
            all_seen(st->still, "«" + e.an_still + "» стоит", clip, 100);
            return true;
        }});
        steps_.push_back({"копия того, кто ходит, кадрами анимации автора", 200, [&g, this, st, look, expected, all_seen](u32 f) {
            const ProjectEdits& e = project_edits;
            const objects::Clip clip = expected(e.an_walker, Pose::Walk);
            if (f == 0) {
                check(g.spawn_copy(e.an_walker, g.hero_x() + 8, g.hero_y() + kHeroHalfH, kWalker) != 0, "копия «" + e.an_walker + "» рядом с героем");
                st->was.clear();
                return false;
            }
            const flecs::entity_t c = g.copy_with_id(kWalker);
            f64 x = 0, y = 0;
            const Pictures::Picture* pic = g.picture_of(e.an_walker);
            if (!c || !pic || !g.position_of(c, x, y)) return check_ok(false, "копии «" + e.an_walker + "» нет");
            const auto was = st->was.find(c);
            // It walks while it goes (the game: on the ground, faster than 0.3 of a tile a second).
            const bool walking = was != st->was.end() && std::fabs(x - was->second) > 0.01 && std::fabs(x - was->second) < 0.5;
            st->was[c] = x;
            if (walking && f >= 10) {
                const u64 into = g.drawn_tick();
                look(st->walker, "идёт", e.an_walker, pic->at(Pose::Walk, into) - pic->frame, objects::clip_frame(clip, into), into, x, y, pic->aspect, 1);
            }
            if (f < 190) return false;
            all_seen(st->walker, "«" + e.an_walker + "» идёт", clip, 60);
            return true;
        }});
    }
    static bool one_of(SliceGame& g, const std::string& id, f64& x, f64& y) {
        const std::vector<flecs::entity_t> c = g.copies_of(id);
        return c.size() == 1 && g.position_of(c[0], x, y);
    }

    void build_edits(Shell& s) {
        SliceGame& g = g_;
        if (!project_edits_error.empty()) {
            steps_.push_back({"правки автора", 1, [this](u32) {
                check(false, project_edits_error);
                return true;
            }});
            return;
        }
        // The level: the one the author meant, with what they put into it; and so again from the game's save. A save
        // made before levels (hero.json without "level") plays game/level, and is saved so; one of a folder that is
        // no level of the game ("level": "") stays so.
        if (!project_edits.level.empty() || !project_edits.cells.empty())
            steps_.push_back({"игра играет нужный уровень", 60, [&s, &g, this](u32 f) {
                const ProjectEdits& e = project_edits;
                auto look = [&](const std::string& when) {
                    if (!e.level.empty())
                        check(g.level_id() == e.level,
                              "уровень «" + e.level_name + "» (" + e.level + "), а играет «" + g.level_id() + "»" + when);
                    for (usize i = 0; i + 3 < e.cells.size(); i += 4) {
                        const u32 layer = static_cast<u32>(e.cells[i]);
                        const i32 x = e.cells[i + 1], y = e.cells[i + 2];
                        const world::TileId want = static_cast<world::TileId>(e.cells[i + 3]), have = g.tile(layer, x, y);
                        check(have == want, "клетка " + std::to_string(x) + ", " + std::to_string(y) + " слоя " + std::to_string(layer) +
                                                ": " + std::to_string(have) + ", автор оставил " + std::to_string(want) + when);
                    }
                };
                if (f == 2) {
                    check(e.cells.size() % 4 == 0, "клетки уровня — по четыре числа");
                    look("");
                    check(s.save("levels", "Уровни"), "игра сохраняется");
                    return false;
                }
                // The save "levels" as another game wrote it: hero.json without "level" (null), or with the level given.
                auto save_as = [&](const char* to, const std::string* level) {
                    namespace fs = std::filesystem;
                    std::error_code ec;
                    const fs::path into = s.slots().folder(to);
                    fs::remove_all(into, ec);
                    fs::copy(s.slots().folder("levels"), into, fs::copy_options::recursive, ec);
                    std::vector<u8> bytes;
                    if (ec || !read_file(into / "hero.json", bytes)) return false;
                    std::string text(bytes.begin(), bytes.end());
                    const usize at = text.find("\"level\""), comma = text.rfind(',', at);
                    const usize open = text.find('"', text.find(':', at)), close = text.find('"', open + 1);
                    if (at == std::string::npos || comma == std::string::npos || close == std::string::npos) return false;
                    if (level) text.replace(open, close + 1 - open, "\"" + *level + "\"");
                    else text.erase(comma, close + 1 - comma);
                    return (level || text.find("\"level\"") == std::string::npos) &&
                           write_file_atomic(into / "hero.json", {reinterpret_cast<const u8*>(text.data()), text.size()});
                };
                auto hero_text = [&](const char* slot) {
                    std::vector<u8> bytes;
                    read_file(s.slots().folder(slot) / "hero.json", bytes);
                    return std::string(bytes.begin(), bytes.end());
                };
                if (f == 5) {
                    check(s.load("levels"), "сохранение загружается");
                    return false;
                }
                if (f == 10) {
                    look(" (после загрузки сохранения)");
                    check(save_as("levels-old", nullptr), "сохранение, как до уровней: в hero.json нет «level»");
                    check(s.load("levels-old"), "старое сохранение загружается");
                    return false;
                }
                if (f == 15) {
                    check(g.level_id() == "level", "старое сохранение (без «level») играет «level», а не «" + g.level_id() + "»");
                    check(s.save("levels-old-again", "Снова"), "старое сохранение сохраняется заново");
                    check(hero_text("levels-old-again").find("\"level\": \"level\"") != std::string::npos,
                          "в новом сохранении «level»: «level»: " + hero_text("levels-old-again"));
                    check(s.load("levels-old-again"), "и оно загружается");
                    return false;
                }
                if (f == 20) {
                    check(g.level_id() == "level", "сохранённое заново играет «level», а не «" + g.level_id() + "»");
                    const std::string none;
                    check(save_as("levels-outside", &none), "сохранение папки не из списка уровней: «level»: \"\"");
                    check(s.load("levels-outside"), "оно загружается");
                    return false;
                }
                if (f == 25) {
                    check(g.level_id().empty(), "«level»: \"\" так и остаётся пустым, а не «" + g.level_id() + "»");
                    check(s.load("levels"), "сохранение «levels» загружается снова");
                    return false;
                }
                if (f < 30) return false;
                look(" (после загрузки сохранения)");
                return true;
            }});
        if (!project_edits.go_through.empty() || project_edits.go_continue) build_going(s);
        if (!project_edits.pl_enemy.empty()) build_platform_edits(s);
        if (!project_edits.tp_hero.empty()) build_template_edits(s);
        if (!project_edits.an_of.empty()) build_anim_edits(s);
        if (project_edits.object.empty() && !project_edits.absent) return;
        if (project_edits.absent) {
            steps_.push_back({"в этой игре ничего из другой игры того же шаблона", 5, [&s, &g, this](u32 f) {
                if (f < 2) return false;
                const ProjectEdits& e = project_edits;
                std::error_code ec;
                check(g.copies_of(e.object).empty(), "объекта «" + e.object_name + "» (" + e.object + ") здесь нет");
                bool linked = false;
                for (const logic::Link& l : g.links().links) linked = linked || l.a == e.object || l.b == e.object;
                check(!linked, "связей с ним нет");
                check(!s.screens().exists(e.screen), "экрана «" + e.screen + "» нет");
                check(!s.vars().has(e.var), "значения «" + e.var + "» нет");
                check(!std::filesystem::exists(s.game_dir() / "sounds" / utf8_path(e.sound), ec), "звука «" + e.sound + "» в игре нет");
                check(!std::filesystem::exists(s.game_dir() / "sources.json", ec), "копий из «Ресурсов» нет (нет sources.json)");
                check(s.slots().list().empty(), "сохранений нет: папка игрока своя (" + path_to_utf8(s.user_folder()) + ")");
                check(std::fabs(s.settings().music_volume - Settings{}.music_volume) < 1e-4f,
                      "настройки свои: громкость музыки " + std::to_string(s.settings().music_volume));
                return true;
            }});
            return;
        }
        struct State {
            f64 ox = 0, oy = 0;
            u32 clicks = 0;
            u64 since = 0; // when the window opened (its appearing and its picture's keys go by the clock)
        };
        auto st = std::make_shared<State>();
        // The hero a few steps from where the editor put it (wherever the game started it): its place is loaded and
        // in view; then the frame is looked at.
        steps_.push_back({"находка стоит на уровне своей картинкой", 90, [&s, &g, this, st](u32 f) {
            const ProjectEdits& e = project_edits;
            if (f == 0) {
                g.teleport(e.x - 3, e.y - 0.43);
                return false;
            }
            if (f < 60) return false; // the hero lands, the view follows
            const std::vector<flecs::entity_t> copies = g.copies_of(e.object);
            check(copies.size() == 1, "«" + e.object_name + "» на уровне одна: " + std::to_string(copies.size()));
            if (copies.empty() || !g.position_of(copies[0], st->ox, st->oy)) return true;
            check(std::fabs(st->ox - e.x) < 0.01 && std::fabs(st->oy - e.y) < 0.01,
                  "она там, где её поставил редактор: " + std::to_string(st->ox) + ", " + std::to_string(st->oy));
            check(!s.vars().has(e.var) && !s.screens().shown(e.screen), "герой её ещё не касался: значения и окна нет");
            std::vector<u8> px;
            u32 w = 0, h = 0;
            check(frame_pixels(s, px, w, h), "кадр игры снят");
            const render::Camera2D& c = g.camera();
            const std::array<int, 3> at = pixel_at(px, w, h, static_cast<f32>((st->ox - c.snapped_x()) * c.zoom + w * 0.5),
                                                   static_cast<f32>((st->oy - c.snapped_y()) * c.zoom + h * 0.5));
            check(near_rgb(at, e.color, 24), "«" + e.object_name + "» нарисована своей картинкой: " + rgb_text(at) + ", ждали " +
                                                 std::to_string(e.color.size() == 3 ? e.color[0] : -1) + "," +
                                                 std::to_string(e.color.size() == 3 ? e.color[1] : -1) + "," +
                                                 std::to_string(e.color.size() == 3 ? e.color[2] : -1));
            return true;
        }});
        steps_.push_back({"герой касается находки: значение и окно", 120, [&s, &g, this, st](u32 f) {
            const ProjectEdits& e = project_edits;
            if (f == 0) {
                g.teleport(st->ox, st->oy - 0.43);
                return false;
            }
            if (!s.screens().shown(e.screen) && f < 100) return false;
            check(s.screens().shown(e.screen), "касание открывает окно «" + e.screen + "»");
            check(s.vars().has(e.var) && var(s, e.var.c_str()) == e.value,
                  "значение «" + e.var + "» = " + s.vars().get(e.var).text() + ", ждали " + std::to_string(e.value));
            return true;
        }});
        steps_.push_back({"окно: значение, та же картинка, она движется", 120, [&s, this, st](u32 f) {
            if (f == 0) st->since = SDL_GetTicks();
            if (SDL_GetTicks() - st->since < 600 && f < 100) {
                SDL_Delay(5); // laid out, appeared
                return false;
            }
            const ProjectEdits& e = project_edits;
            const std::string said = text_of(layer(s, e.screen, e.text_node));
            check(said.find(e.text) != std::string::npos, "в окне «" + said + "», ждали «" + e.text + "»");
            Rml::Element* pic = layer(s, e.screen, e.picture_node);
            if (!check_ok(pic != nullptr, "в окне картинка (слой " + std::to_string(e.picture_node) + ")")) return true;
            std::vector<u8> px;
            u32 w = 0, h = 0;
            check(frame_pixels(s, px, w, h), "кадр с окном снят");
            const std::optional<Rml::Vector2f> at = on_screen(s, e.screen, e.picture_node);
            const Rml::Vector2i size = s.context()->GetDimensions();
            const std::array<int, 3> c = at && size.x > 0 && size.y > 0
                                             ? pixel_at(px, w, h, at->x * static_cast<f32>(w) / static_cast<f32>(size.x),
                                                        at->y * static_cast<f32>(h) / static_cast<f32>(size.y))
                                             : std::array<int, 3>{-1, -1, -1};
            check(near_rgb(c, e.color, 24), "картинка в окне та же: " + rgb_text(c));
            // Its keys play: 100 %, e.motion_scale at the middle key, 100 % again, a pass in e.motion_seconds, over and
            // over. Not two looks by the wall clock: RmlUi moves at most 0.1 s between updates, and a pass that comes
            // back the way it went looks the same 0.05 s before its end and 0.05 s after it. The context's clock takes
            // over at the moment the wall clock is at (the movement goes on from where it is) and goes a pass in steps
            // of a twentieth: whatever moment of the pass it started at, one of them is within a fortieth of a pass of
            // each end and of the middle key, and the last is the first again.
            const f64 pass = e.motion_seconds, middle = e.motion_scale;
            if (!check_ok(pass > 0 && middle > 0 && std::abs(middle - 1) > 0.1,
                          "ожидания: движение по ключам (" + std::to_string(middle) + ", " + std::to_string(pass) + " с)"))
                return true;
            auto scale = [pic]() -> f64 {
                const Rml::Property* p = pic->GetProperty("transform");
                const Rml::TransformPtr t = p ? p->Get<Rml::TransformPtr>() : nullptr;
                if (t)
                    for (const Rml::TransformPrimitive& prim : t->GetPrimitives())
                        if (prim.type == Rml::TransformPrimitive::SCALE2D) return prim.scale_2d.values[0];
                return t ? -1 : 1; // no movement: as it stands
            };
            constexpr int kLooks = 20;
            const double from = Rml::GetSystemInterface()->GetElapsedTime();
            std::vector<f64> seen;
            for (int i = 0; i <= kLooks; ++i) {
                s.ui().set_clock(s.context(), from + pass * i / kLooks);
                s.ui().update_context(s.context());
                seen.push_back(scale());
            }
            s.ui().set_clock(s.context(), std::nullopt);
            std::string said_scales;
            for (const f64 v : seen) said_scales += (said_scales.empty() ? "" : " ") + std::to_string(v).substr(0, 5);
            FORGE_INFO("scene: картинка в окне за проход по часам контекста: %s", said_scales.c_str());
            const auto [lo, hi] = std::minmax_element(seen.begin(), seen.end());
            const f64 low = std::min(1.0, middle), high = std::max(1.0, middle), near = 0.1 * (high - low);
            check(*lo > low - 0.001 && *hi < high + 0.001, "картинка в окне не выходит за свои ключи: " + said_scales);
            check(std::abs((middle > 1 ? *hi : *lo) - middle) < near,
                  "картинка в окне доходит до среднего ключа " + std::to_string(middle) + ": " + said_scales);
            check(std::abs((middle > 1 ? *lo : *hi) - 1) < near, "и возвращается к 100 %: " + said_scales);
            check(std::abs(seen.back() - seen.front()) < 0.002, "проход за " + std::to_string(pass) + " с, снова: " + said_scales);
            return true;
        }});
        steps_.push_back({"«Забрать»: тот самый звук, окно закрывается", 30, [&s, &g, this, st](u32 f) {
            const ProjectEdits& e = project_edits;
            audio::ScreenSounds& snd = g.sounds().screens();
            if (f == 0) {
                st->clicks = snd.clicks();
                const std::optional<Rml::Vector2f> at = on_screen(s, e.screen, e.button_node);
                check(at && click_at(s, *at), "кнопка окна нажата мышью");
                return false;
            }
            check(snd.clicks() == st->clicks + 1 && snd.last_click() == e.sound,
                  "кнопка сыграла «" + snd.last_click() + "», ждали «" + e.sound + "»");
            const audio::ClipPtr clip = snd.clip(e.sound);
            check(clip && clip->frames() == e.sound_frames, "звук — тот самый файл: " + std::to_string(clip ? clip->frames() : 0) +
                                                                " кадров, ждали " + std::to_string(e.sound_frames));
            check(!s.screens().shown(e.screen), "окно закрылось");
            return true;
        }});
        steps_.push_back({"второе касание ничего не делает («Только один раз»)", 150, [&s, &g, this, st](u32 f) {
            const ProjectEdits& e = project_edits;
            if (f == 0) g.teleport(st->ox - 5, st->oy - 0.43);
            if (f == 40) g.teleport(st->ox, st->oy - 0.43);
            if (f < 120) return false;
            check(var(s, e.var.c_str()) == e.value && !s.screens().shown(e.screen),
                  "значение осталось " + s.vars().get(e.var).text() + ", окно не открылось");
            return true;
        }});
        steps_.push_back({"сохранение и настройка — в папке игрока этой игры", 5, [&s, this](u32 f) {
            if (f < 1) return false;
            Settings mine = s.settings();
            mine.music_volume = 0.3f;
            s.apply_settings(mine);
            check(s.save("находка", "Находка"), "игра сохраняется");
            std::error_code ec;
            check(std::filesystem::is_regular_file(s.user_folder() / "settings.json", ec) && s.slots().exists("находка"),
                  "настройки и сохранение — в " + path_to_utf8(s.user_folder()));
            return true;
        }});
    }

    void build(Shell& s) {
        if (scene_ == "project") {
            build_project(s);
            return;
        }
        if (scene_ == "stress") {
            build_stress(s);
            return;
        }
        if (scene_ == "inventory") {
            build_inventory(s);
            return;
        }
        if (scene_ == "windows") {
            build_windows(s);
            return;
        }
        if (scene_ == "templates" || scene_ == "volumes") {
            build_templates(s);
            return;
        }
        if (scene_ == "physics") {
            build_physics(s);
            return;
        }
        if (scene_ == "light") {
            build_light(s);
            return;
        }
        if (scene_ == "zones") {
            build_zones(s);
            return;
        }
        if (scene_ == "levels" || scene_ == "levels_continue") {
            build_levels(s, scene_ == "levels_continue");
            return;
        }
        if (scene_ == "platformer" || scene_ == "platformer_continue" || scene_ == "platformer_edges") {
            build_platformer(s, scene_ == "platformer_continue", scene_ == "platformer_edges");
            return;
        }
        if (scene_ == "platformer_template" || scene_ == "platformer_template_continue") {
            build_template(s, scene_ == "platformer_template_continue");
            return;
        }
        if (scene_ == "own_tiles") {
            build_own_tiles(s);
            return;
        }
        if (scene_ == "tiled" || scene_ == "tiled_update") {
            build_tiled(s, scene_ == "tiled_update");
            return;
        }
        if (scene_ == "module_slots") {
            build_module_slots(s);
            return;
        }
        SliceGame& g = g_;
        const f64 v = g.generator().village_y();
        const f64 stand = 0.93; // centre above the floor
        const f64 gy = g.generator().gallery_y();
        auto controls = [](bool left, bool right, bool jump) {
            Controls c;
            c.left = left;
            c.right = right;
            c.jump = jump;
            return c;
        };
        auto aim = [](f64 x, f64 y, bool use, bool place) {
            Controls c;
            c.aim_x = x;
            c.aim_y = y;
            c.use = use;
            c.place = place;
            return c;
        };
        DialogueRunner& talk = s.dialogue_runner();

        // The main menu of games/examples/screen-sound: its music starts once, its buttons sound as set (the
        // screen's click, a ring of its own, none) once per press, by mouse and by keyboard; an empty place
        // makes no sound. Then the page goes and its music stops.
        steps_.push_back({"музыка и звук кнопок главного меню", 40, [&s, &g, this](u32 f) {
            GameScreens& sc = s.screens();
            audio::ScreenSounds& snd = g.sounds().screens();
            static std::filesystem::path kept;
            static u32 starts = 0, clicks = 0;
            auto num = [&](const char* name) { return s.vars().get(name).number(); };
            if (f == 0) {
                check(std::size(kSoundPages) == 3, "в игру встроены экраны примера звука");
                kept = snd.folder();
                snd.attach(&g.sounds().mixer(), sound_files());
                for (const SimplePage& p : kSoundPages)
                    check(sc.load_page(s.context(), p.name, p.html, path_to_utf8(s.game_dir() / "ui" / (std::string(p.name) + ".html"))),
                          std::string("экран примера строится: ") + p.name);
                for (const char* v : {"demo.presses", "demo.rings", "demo.quiet"}) s.vars().set(v, 0);
                starts = snd.music_starts();
                clicks = snd.clicks();
            }
            if (f == 3) {
                check(s.screen() == Screen::Main && sc.shown("звук_меню"), "меню примера вместо меню игры");
                check(sc.music() == "мелодия меню.wav" && snd.music_name() == "мелодия меню.wav", "играет музыка меню: " + snd.music_name());
                check(snd.music_starts() == starts + 1 && snd.music_playing(), "музыка меню начата один раз");
                check(g.sounds().mixer().voices(audio::Bus::Music) == 1, "на шине музыки один голос");
                check(music_peak(g.sounds().mixer()) > 0.05f, "музыку меню слышно: в смеси есть звук");
                press_page(s, 960, 345); // «Нажми»: the screen's click
            }
            if (f == 4) {
                check(num("demo.presses") == 1 && snd.clicks() == clicks + 1 && snd.last_click() == "щелчок.wav",
                      "«Нажми»: действие один раз и щелчок экрана");
                press_page(s, 960, 465); // «Звон»: its own
            }
            if (f == 5) {
                check(num("demo.rings") == 1 && snd.clicks() == clicks + 2 && snd.last_click() == "звон.wav", "«Звон»: свой звук");
                press_page(s, 960, 585); // «Тихо»: none
            }
            if (f == 6) {
                check(num("demo.quiet") == 1 && snd.clicks() == clicks + 2, "«Тихо»: действие есть, звука нет");
                // The mouse leaves the keyboard's focus on the button it pressed (not on its label).
                check(s.context()->GetFocusElement() == titled(sc.document("звук_меню"), "Тихо") && sc.has_focus(s.context()),
                      "нажатая мышью кнопка берёт фокус клавиатуры");
                key(s, SDLK_SPACE, true);
                key(s, SDLK_SPACE, false);
            }
            if (f == 7) {
                check(num("demo.quiet") == 2 && snd.clicks() == clicks + 2, "пробел после мыши нажимает ту же кнопку, без звука");
                press_page(s, 200, 950); // the background: nothing to press
            }
            if (f == 8) {
                check(snd.clicks() == clicks + 2 && num("demo.presses") == 1 && num("demo.quiet") == 2, "пустое место не звучит и не нажимает");
                // Tab: the first of the menu's buttons takes the keyboard.
                key(s, SDLK_TAB, true);
                key(s, SDLK_TAB, false);
                check(s.context()->GetFocusElement() == titled(sc.document("звук_меню"), "Нажми"), "Tab даёт фокус первой кнопке меню");
                key(s, SDLK_RETURN, true);
                key(s, SDLK_RETURN, false);
            }
            if (f == 9) {
                check(num("demo.presses") == 2 && snd.clicks() == clicks + 3, "Enter нажимает кнопку один раз, со звуком");
                key(s, SDLK_RETURN, true);
                key(s, SDLK_RETURN, true, true); // held: repeats
                key(s, SDLK_RETURN, true, true);
                key(s, SDLK_RETURN, false);
            }
            if (f == 10) {
                check(num("demo.presses") == 3 && snd.clicks() == clicks + 4, "удержание и отпускание Enter не нажимают снова");
                key(s, SDLK_SPACE, true);
                key(s, SDLK_SPACE, false);
            }
            if (f == 11) {
                check(num("demo.presses") == 4 && snd.clicks() == clicks + 5, "пробел нажимает один раз");
                // Loaded again (saved in the editor while the game runs): the music goes on, not twice.
                for (const SimplePage& p : kSoundPages)
                    if (std::string(p.name) == "звук_меню") sc.load_page(s.context(), p.name, p.html, "test/ui/звук_меню.html");
            }
            if (f == 14) {
                check(snd.music_starts() == starts + 1 && g.sounds().mixer().voices(audio::Bus::Music) == 1,
                      "страница загружена снова: музыка та же, не вторая");
                for (const SimplePage& p : kSoundPages) sc.remove(p.name);
            }
            if (f == 16) {
                check(sc.music().empty() && !snd.music_playing() && g.sounds().mixer().voices(audio::Bus::Music) == 0,
                      "меню примера ушло: его музыка остановлена");
                snd.attach(&g.sounds().mixer(), kept);
                for (const char* v : {"demo.presses", "demo.rings", "demo.quiet"}) s.vars().set(v, 0);
                return true;
            }
            return false;
        }});
        // A window a button of the game's own menu opens: over the menu, closed by «В главное меню» and Esc,
        // and by the game starting.
        steps_.push_back({"окно из главного меню поверх меню", 20, [&s, this](u32 f) {
            GameScreens& sc = s.screens();
            auto shown = [&](const char* id) {
                Rml::Element* e = s.find_element(id);
                return e && e->IsVisible(true);
            };
            if (f == 0) {
                const Rml::Vector2i size = s.context()->GetDimensions();
                const std::string dims = std::to_string(size.x) + " " + std::to_string(size.y);
                // The window's button stands where the menu's is: a press there goes to the one on top.
                const std::string place = "position: absolute; left: 300px; top: 200px; width: 200px; height: 60px;";
                const std::string menu =
                    "<html><head><style>body, #m-root { pointer-events: none; } #m-root > div { pointer-events: auto; }</style></head>"
                    "<body><div id=\"m-root\" forge-screen=\"menu\" forge-size=\"" + dims + "\">"
                    "<div id=\"m-open\" style=\"" + place + " background: #335;\" "
                    "forge-click=\"[[&quot;show&quot;,&quot;тест_из_меню&quot;]]\">Об игре</div></div></body></html>";
                const std::string window =
                    "<html><head><style>body, #w-root { pointer-events: none; } #w-root > div { pointer-events: auto; }</style></head>"
                    "<body><div id=\"w-root\" forge-screen=\"command\" forge-size=\"" + dims + "\">"
                    "<div id=\"w-back\" style=\"" + place + " background: #533;\" "
                    "forge-click=\"[[&quot;menu&quot;,&quot;&quot;]]\">В главное меню</div></div></body></html>";
                check(s.screen() == Screen::Main, "главное меню открыто");
                check(sc.load_page(s.context(), "тест_меню", menu, "test/ui/тест_меню.html"), "меню строится");
                check(sc.load_page(s.context(), "тест_из_меню", window, "test/ui/тест_из_меню.html"), "окно строится");
            }
            if (f == 2) {
                check(shown("m-open") && !shown("w-back"), "меню видно, окно пока закрыто");
                check(click(s, "m-open"), "кнопка меню нажимается");
            }
            if (f == 4) {
                check(sc.shown("тест_из_меню") && shown("w-back"), "окно открылось поверх главного меню, а не под ним");
                check(click(s, "w-back"), "нажатие там, где обе кнопки");
            }
            if (f == 6) {
                check(!shown("w-back") && shown("m-open") && s.screen() == Screen::Main,
                      "«В главное меню» в окне: окно закрыто, меню на месте");
                check(click(s, "m-open"), "окно ещё раз");
            }
            if (f == 8) {
                check(shown("w-back"), "окно снова поверх меню");
                SDL_Event ev{};
                ev.type = SDL_EVENT_KEY_DOWN;
                ev.key.key = SDLK_ESCAPE;
                ev.key.down = true;
                s.handle_event(ev);
            }
            if (f == 10) {
                check(!shown("w-back") && shown("m-open"), "Esc закрывает окно над меню");
                // Left open over the game's own menu: «Новая игра» there closes it (checked in «старт»).
                sc.remove("тест_меню");
                check(sc.show("тест_из_меню", true), "окно открыто перед новой игрой");
                return true;
            }
            return false;
        }});
        steps_.push_back({"меню", 30, [&s, &g, this](u32 f) {
            if (f < 5) return false;
            if (f == 5) {
                check(s.screen() == Screen::Main, "игра начинается с главного меню");
                check(g.count_npcs() >= 2, "за меню видна деревня с жителями");
                check(click(s, "menu-new"), "в меню есть «Новая игра»");
            }
            if (f == 25) check(false, "«Новая игра» не нажимается мышью");
            return s.screen() == Screen::Playing || f >= 25;
        }});
        steps_.push_back({"старт", 180, [&s, &g, this](u32 f) {
            if (f == 0) {
                check(!s.screens().shown("тест_из_меню"), "новая игра закрыла окно, открытое из меню");
                s.screens().remove("тест_из_меню");
                check(g.running() && g.hero_alive(), "герой появился");
                check(s.screen() == Screen::Playing, "идёт игра");
                check(g.location() == "Деревня", "герой в деревне, а не в «" + g.location() + "»");
                check(var(s, "inv.torch") == 5, "в начале 5 факелов");
            }
            return g.on_ground();
        }});
        static f64 x0 = 0, y0 = 0;
        steps_.push_back({"ходьба", 60, [&g, controls, this](u32 f) {
            if (f == 0) {
                x0 = g.hero_x();
                g.script(controls(false, true, false));
            }
            if (f < 40) return false;
            check(g.hero_x() > x0 + 3, "герой идёт вправо");
            return true;
        }});
        steps_.push_back({"разговор с Борисом", 30, [&s, &g, &talk, this](u32 f) {
            if (!near_npc(g, 0, f)) return false;
            check(g.talk_nearest(), "с Борисом можно заговорить");
            check(talk.node_id() == "hello", "Борис здоровается");
            talk.advance();
            check(talk.line().choices.size() == 3, "у Бориса три ответа");
            check(talk.choose(0), "можно согласиться помочь");
            check(var(s, "quest.pickaxe") == 1, "задание взято");
            talk.advance();
            check(!s.in_dialogue(), "разговор закончился");
            check(g.talk_nearest(), "заговорить снова");
            check(talk.node_id() == "not_yet", "Борис спрашивает про кирку");
            check(talk.choose(0), "задать вопрос");
            check(talk.line().asks_keyword, "можно написать вопрос");
            check(talk.ask("А где тут медь?"), "вопрос про медь понят");
            check(talk.node_id() == "k_copper", "ответ про медь");
            talk.stop();
            return true;
        }});
        steps_.push_back({"копать руками и строить", 120, [&s, &g, aim, v, stand, this](u32 f) {
            if (f == 0) {
                g.teleport(40.5, v - stand);
                g.select(1);
            }
            if (f < 5) return false;
            if (f == 5) {
                x0 = s.vars().get("inv.dirt").number();
                check(g.tile(kBlocks, 42, static_cast<i32>(v)) == world::TileGrass, "у ног трава");
            }
            if (f < 70) {
                g.script(aim(42.5, v + 0.5, true, false));
                return false;
            }
            if (f == 70) {
                check(g.tile(kBlocks, 42, static_cast<i32>(v)) == world::TileAir, "дёрн выкопан руками");
                check(var(s, "inv.dirt") == x0 + 1, "земля попала в сумку");
                g.script(aim(42.5, v + 0.5, false, true));
                return false;
            }
            check(g.tile(kBlocks, 42, static_cast<i32>(v)) == world::TileDirt, "земля поставлена обратно");
            check(var(s, "inv.dirt") == x0, "земля ушла из сумки");
            return true;
        }});
        steps_.push_back({"подъём по лестнице", 150, [&g, controls, this](u32 f) {
            const SliceGenerator& gen = g.generator();
            if (f == 0) g.teleport(gen.mine_x() - 40 + 0.5, gen.mine_y() + 41 - 0.93);
            if (f < 5) return false;
            if (f == 5) {
                y0 = g.hero_y();
                g.script(controls(false, true, false));
            }
            if (f < 100) return false;
            check(g.hero_y() < y0 - 5, "герой поднимается по ступеням");
            return true;
        }});
        steps_.push_back({"штольня без кирки", 60, [&g, aim, gy, stand, this](u32 f) {
            const SliceGenerator& gen = g.generator();
            const i32 fx = gen.gallery_x0() - 3;
            if (f == 0) g.teleport(gen.gallery_x0() - 1.5, gy + 1 - stand);
            if (f < 5) return false;
            if (f == 5) check(g.location() == "Старая шахта", "место: старая шахта, а не «" + g.location() + "»");
            if (f < 50) {
                g.script(aim(fx + 0.5, gy + 1.5, true, false));
                return false;
            }
            check(g.tile(kBlocks, fx, static_cast<i32>(gy) + 1) != world::TileAir, "камень без кирки не копается");
            return true;
        }});
        // «Ключ открывает Дверь» (games/slice/logic.json): without the key
        // the door holds and says what it needs.
        steps_.push_back({"дверь без ключа", 100, [&g, controls, gy, stand, this](u32 f) {
            const SliceGenerator& gen = g.generator();
            const i32 dx = gen.gallery_x1();
            if (f == 0) g.teleport(dx + 3.5, gy + 1 - stand);
            if (f < 5) return false;
            if (f < 80) {
                g.script(controls(true, false, false));
                return false;
            }
            g.script(Controls{});
            bool open = true;
            check(g.door_open(dx + 0.5, gy + 0.5, open), "дверь в конце штольни");
            check(!open, "без ключа дверь закрыта");
            check(g.tile(kBlocks, dx, static_cast<i32>(gy)) == TileDoor && g.tile(kBlocks, dx, static_cast<i32>(gy) - 4) == TileDoor,
                  "закрытая дверь занимает проход");
            check(g.hero_x() > dx + 1.0, "герой не прошёл сквозь дверь");
            check(g.last_hint() == "Нужен предмет «Ключ»", "подсказка: нужен ключ, а не «" + g.last_hint() + "»");
            g.teleport(gen.gallery_x0() - 1.5, gy + 1 - stand);
            return true;
        }});
        steps_.push_back({"до кирки через пруд", 1200, [&s, &g, controls, this](u32) {
            const f64 x = g.hero_x();
            g.script(controls(true, false, x > g.generator().pool_x0() - 3 && x < g.generator().pool_x1() + 3));
            if (g.inventory("pickaxe") < 1) return false;
            check(var(s, "quest.pickaxe") == 2, "задание: вернуть кирку");
            check(var(s, "inv.coins") >= 12, "монеты у кирки подобраны");
            check(var(s, "inv.torch") == 8, "факелы в штольне подобраны");
            check(var(s, "inv.key") == 1, "ключ подобран по пути");
            bool open = false;
            check(g.door_open(g.generator().gallery_x1() + 0.5, g.generator().gallery_y() + 0.5, open) && open, "ключ открыл дверь");
            std::vector<u8> fired;
            read_file(std::filesystem::temp_directory_path() / "forge_slice_test_fired.txt", fired);
            const std::string said(fired.begin(), fired.end());
            check(said.starts_with("run ") && said.find("\n1\n") != std::string::npos, "игра записала, что связь «Ключ открывает Дверь» сработала");
            check(g.tile(kBlocks, g.generator().gallery_x1(), static_cast<i32>(g.generator().gallery_y())) == world::TileAir,
                  "открытая дверь пропускает");
            return true;
        }});
        steps_.push_back({"копать камень киркой", 120, [&s, &g, aim, gy, this](u32 f) {
            static i32 tx = 0;
            if (f == 0) g.script(Controls{});
            if (f == 9) { // standing still by now
                tx = static_cast<i32>(std::floor(g.hero_x())) + 2;
                x0 = var(s, "inv.stone");
                g.select(2);
            }
            if (f < 10) return false;
            if (f < 60) {
                g.script(aim(tx + 0.5, gy + 1.5, true, false));
                return false;
            }
            if (f == 60) {
                check(g.tile(kBlocks, tx, static_cast<i32>(gy) + 1) == world::TileAir, "камень выкопан киркой");
                check(var(s, "inv.stone") >= x0 + 1, "камень в сумке");
                g.script(aim(tx + 0.5, gy + 1.5, false, true));
                return false;
            }
            if (f == 61) check(g.tile(kBlocks, tx, static_cast<i32>(gy) + 1) == world::TileStone, "камень поставлен");
            g.script(aim(tx + 0.5, gy + 1.5, true, false)); // and dug again, to check the save
            return f >= 100;
        }});
        static f64 saved_x = 0, saved_y = 0, saved_coins = 0;
        static i32 hole_x = 0;
        steps_.push_back({"сохранение и загрузка", 40, [&s, &g, gy, this](u32 f) {
            if (f == 0) {
                g.script(Controls{});
                hole_x = static_cast<i32>(std::floor(g.hero_x())) + 2;
                check(g.tile(kBlocks, hole_x, static_cast<i32>(gy) + 1) == world::TileAir, "яма перед сохранением");
                check(s.save("slot-1", "Проверка"), "игра сохраняется");
                saved_x = g.hero_x();
                saved_y = g.hero_y();
                saved_coins = var(s, "inv.coins");
                s.vars().set("inv.coins", 999);
                g.teleport(g.generator().spawn_x(), g.generator().spawn_y());
                return false;
            }
            if (f == 5) {
                check(s.load("slot-1"), "сохранение загружается");
                return false;
            }
            if (f < 10) return false;
            check(std::fabs(g.hero_x() - saved_x) < 0.5 && std::fabs(g.hero_y() - saved_y) < 0.5, "герой там же, где сохранился");
            check(var(s, "inv.coins") == saved_coins, "сумка как при сохранении");
            check(var(s, "quest.pickaxe") == 2, "задание как при сохранении");
            check(g.tile(kBlocks, hole_x, static_cast<i32>(gy) + 1) == world::TileAir, "выкопанное осталось выкопанным");
            check(g.count_items(ItemKind::Pickaxe) == 0, "кирка не появилась снова");
            check(s.slots().exists("slot-1"), "слот в списке");
            return true;
        }});
        steps_.push_back({"вернуть кирку", 30, [&s, &g, &talk, this](u32 f) {
            if (!near_npc(g, 0, f)) return false;
            const f64 coins = var(s, "inv.coins");
            check(g.talk_nearest(), "заговорить с Борисом");
            check(talk.node_id() == "give_back", "Борис узнаёт кирку");
            check(var(s, "quest.pickaxe") == 3, "задание выполнено");
            check(var(s, "inv.coins") == coins + 30, "награда 30 монет");
            check(var(s, "inv.pickaxe") == 1, "кирка осталась у героя");
            talk.advance();
            return true;
        }});
        steps_.push_back({"медь для кузнеца", 30, [&s, &g, &talk, this](u32 f) {
            if (!near_npc(g, 1, f)) return false;
            check(g.talk_nearest(), "заговорить с кузнецом");
            check(talk.node_id() == "hello", "кузнец здоровается");
            check(talk.choose(0), "взять заказ");
            check(var(s, "quest.copper") == 1, "заказ взят");
            talk.advance();
            s.vars().set("inv.copper", 10);
            check(g.talk_nearest(), "прийти с медью");
            check(talk.node_id() == "reward", "кузнец берёт медь");
            check(var(s, "hero.dig_speed") == 2, "кирка копает вдвое быстрее");
            check(var(s, "inv.copper") == 0, "медь отдана");
            talk.advance();
            return true;
        }});
        steps_.push_back({"ящик", 40, [&s, &g, aim, v, this](u32 f) {
            const f64 cx = g.generator().smith_house().x1 + 4.5;
            if (f == 0) {
                g.teleport(cx - 3.0, v - 0.93);
                x0 = var(s, "inv.wood");
            }
            if (f < 20) return false;
            if (f == 20) {
                g.script(aim(cx, v - 0.5, true, false));
                return false;
            }
            g.script(Controls{});
            check(var(s, "inv.wood") >= x0 + 2, "из ящика выпали доски");
            return true;
        }});
        steps_.push_back({"песок за досками", 150, [&g, aim, gy, stand, this](u32 f) {
            const SliceGenerator& gen = g.generator();
            const i32 x = gen.gallery_x0() - 40;
            if (f == 0) g.teleport(x - 2.5, gy + 1 - stand);
            if (f < 10) return false;
            if (f == 10) check(g.tile(kBlocks, x, static_cast<i32>(gy) - 5) == TilePlanks, "над штольней доски");
            if (f < 60) {
                g.script(aim(x + 0.5, gy - 4.5, true, false));
                return false;
            }
            g.script(Controls{});
            if (f < 140) return false;
            bool sand = false;
            for (i32 y = static_cast<i32>(gy) - 4; y <= static_cast<i32>(gy); ++y)
                sand = sand || g.tile(kBlocks, x, y) == world::TileSand || g.tile(kBlocks, x - 1, y) == world::TileSand ||
                       g.tile(kBlocks, x + 1, y) == world::TileSand;
            check(sand, "песок высыпался в штольню");
            return true;
        }});
        // The «Управление» schemes move critters each their own way.
        steps_.push_back({"схемы управления", 200, [&g, controls, v, stand, this](u32 f) {
            static flecs::entity_t follow = 0, flee = 0, player = 0, still = 0;
            static f64 hx = 0, follow0 = 0, flee0 = 0, player0 = 0, still0 = 0;
            if (f == 0) {
                g.script(Controls{});
                g.teleport(0.5, v - stand);
                return false;
            }
            if (f == 10) {
                hx = g.hero_x();
                follow = g.spawn_critter(hx + 6, v, Scheme::Follow);
                flee = g.spawn_critter(hx - 2, v, Scheme::Flee);
                player = g.spawn_critter(hx + 2, v, Scheme::Player);
                still = g.spawn_critter(hx - 4, v, Scheme::Stand);
                check(follow && flee && player && still, "зверьки с разными схемами появились");
                return false;
            }
            if (f == 15) {
                follow0 = g.critter_x(follow);
                flee0 = g.critter_x(flee);
                player0 = g.critter_x(player);
                still0 = g.critter_x(still);
            }
            if (f < 100) return false;
            if (f == 100) {
                check(std::fabs(g.critter_x(follow) - hx) < std::fabs(follow0 - hx) - 2, "«идёт за героем» подошёл к герою");
                check(g.critter_x(flee) < flee0 - 2, "«убегает» убежал от героя");
                check(std::fabs(g.critter_x(player) - player0) < 0.5, "«игрок» стоит, пока клавиши не нажаты");
                check(std::fabs(g.critter_x(still) - still0) < 0.5, "«стоит» стоит на месте");
                player0 = g.critter_x(player);
                g.script(controls(false, true, false));
                return false;
            }
            if (f < 160) return false;
            g.script(Controls{});
            check(g.critter_x(player) > player0 + 1.5, "«игрок» идёт вправо по стрелке");
            return true;
        }});
        // Everything above made its sounds; an object's own ones play too.
        steps_.push_back({"звуки", 200, [&g, v, stand, this](u32 f) {
            static flecs::entity_t pet = 0;
            const SliceSounds& snd = g.sounds();
            if (f == 0) {
                check(snd.played(Cue::Step) > 5, "шаги героя звучат");
                check(snd.played(Cue::Jump) > 0, "прыжок звучит");
                check(snd.played(Cue::Dig) > 0 && snd.played(Cue::Break) > 0, "копание звучит");
                check(snd.played(Cue::Place) > 0, "постройка звучит");
                check(snd.played(Cue::Pickup) > 0, "подбор кирки звучит");
                check(snd.played(Cue::Crate) > 0, "ящик разбивается со звуком");
                check(snd.played(Cue::Splash) > 0, "всплеск в пруду");
                check(snd.played(Cue::Talk) > 0, "разговор начинается со звука");
                // A sound file of its own: a short tone.
                const std::filesystem::path file = std::filesystem::temp_directory_path() / forge::utf8_path("forge_slice_мурлык.wav");
                const audio::Tone tone[] = {{audio::Wave::Sine, 200, 220, 0.3f}};
                const audio::ClipPtr clip = audio::synth(tone);
                std::vector<u8> wav;
                check(clip && audio::encode_wav(*clip, wav) && write_file_atomic(file, wav), "звуковой файл записан");
                g.script(Controls{});
                g.teleport(0.5, v - stand);
                pet = g.spawn_critter(g.hero_x() + 6, v, Scheme::Follow);
                Sounds own;
                own.near = path_to_utf8(file);
                own.step = path_to_utf8(file);
                check(g.set_sounds(pet, own), "у зверька свой звук");
                return false;
            }
            if (f < 90) return false;
            if (f == 90) {
                check(snd.loops() == 1, "звук «рядом» играет около зверька");
                check(snd.played_named() > 0, "шаги зверька своим звуком");
                g.teleport(0.5 + 200, v - stand);
            }
            return f >= 100 && snd.loops() == 0;
        }});
        // «Связи» over the game (F2): the things on screen get names; a click
        // on the hero, then on a critter, offers what they can do, and the
        // link chosen is written and played at once.
        steps_.push_back({"связи поверх игры", 200, [&s, &g, v, stand, this](u32 f) {
            static flecs::entity_t pet = 0;
            static f64 x0 = 0;
            static u32 made = 0;
            static usize count0 = 0;
            LinkOverlay& o = g.overlay();
            if (f == 0) {
                g.script(Controls{});
                g.teleport(-30.5, v - stand);
                check(!o.on(), "связи над игрой скрыты, пока не нажата F2");
                return false;
            }
            if (f == 5) {
                check(s.find_element("ln-tag") != nullptr, "в углу подсказка «F2 Связи»");
                pet = g.spawn_critter(g.hero_x() + 3.5, v, Scheme::Stand);
                SDL_Event ev{};
                ev.type = SDL_EVENT_KEY_DOWN;
                ev.key.key = SDLK_F2;
                ev.key.down = true;
                g.handle_event(ev);
                return false;
            }
            if (f == 10) {
                check(o.on(), "F2 показывает связи");
                bool hero = false, critter = false;
                for (const LinkOverlay::MarkView& m : o.marks()) {
                    hero = hero || m.id == "hero";
                    critter = critter || m.id == "critter";
                }
                check(hero && critter, "над героем и зверьком их имена");
                check(s.find_element("ln-panel") != nullptr, "список связей игры");
                count0 = g.links().links.size();
                x0 = g.critter_x(pet);
                check(click(s, "ln-ring-hero"), "герой нажимается");
                return false;
            }
            if (f == 12) {
                check(o.picked() == "hero", "герой выбран первым");
                check(click(s, "ln-ring-critter"), "зверёк нажимается");
                return false;
            }
            if (f == 14) {
                check(o.picking(), "после второй вещи — выбор, что будет");
                i32 flee = -1;
                for (usize i = 0; i < o.choices().size(); ++i)
                    if (o.choices()[i].phrase.find("убегает") != std::string::npos) flee = static_cast<i32>(i);
                check(flee >= 0, "среди вариантов «Зверёк убегает от героя»");
                check(click(s, ("ln-choice-" + std::to_string(flee)).c_str()), "вариант нажимается");
                return false;
            }
            if (f == 16) {
                const auto& links = g.links().links;
                check(links.size() == count0 + 1, "связь добавилась");
                if (links.size() == count0 + 1) {
                    const logic::Link& l = links.back();
                    made = l.id;
                    check(l.a == "critter" && l.verb == "flee" && l.b == "hero", "связь: зверёк убегает от героя");
                }
                std::vector<u8> bytes;
                read_file(std::filesystem::temp_directory_path() / "forge_slice_test_logic.json", bytes);
                check(std::string(bytes.begin(), bytes.end()).find("\"flee\"") != std::string::npos, "связь записана в файл связей");
                check(!o.picking(), "выбор закрылся");
                return false;
            }
            if (f == 20) { // «всё время» links happen as the copies start
                bool lit = false;
                for (const LinkOverlay::RowView& r : o.rows()) lit = lit || (static_cast<u32>(r.id) == made && r.lit);
                check(lit, "сработавшая связь подсвечена");
            }
            if (f < 110) return false;
            if (f == 110) {
                check(g.critter_x(pet) > x0 + 2, "зверёк сразу убегает по новой связи");
                if (scene_ != "links") check(click(s, ("ln-remove-" + std::to_string(made)).c_str()), "связь убирается");
                return false;
            }
            if (f == 112) {
                if (scene_ != "links") check(g.links().links.size() == count0, "связь убрана");
                SDL_Event ev{};
                ev.type = SDL_EVENT_KEY_DOWN;
                ev.key.key = SDLK_F2;
                ev.key.down = true;
                g.handle_event(ev);
                return false;
            }
            if (f < 114) return false;
            check(!o.on(), "F2 ещё раз — обратно к игре");
            return true;
        }});
        // Screens drawn in «Интерфейс» (written here by hand, as the editor
        // writes them): a HUD with data in it and a window a button opens.
        steps_.push_back({"экраны игры", 60, [&s, this](u32 f) {
            static int messages = 0;
            GameScreens& sc = s.screens();
            auto shown = [&](const char* id) {
                Rml::Element* e = s.find_element(id);
                return e && e->IsVisible(true);
            };
            if (f == 0) {
                const Rml::Vector2i size = s.context()->GetDimensions();
                const std::string dims = std::to_string(size.x) + " " + std::to_string(size.y); // scale 1: clicks land where the boxes are
                const std::string hud =
                    "<html><head><style>body, #t-root { pointer-events: none; } #t-root > div { pointer-events: auto; }</style></head>"
                    "<body><div id=\"t-root\" forge-screen=\"playing\" forge-fit=\"expand\" forge-size=\"" + dims + "\">"
                    "<div id=\"t-coins\" forge-text=\"Монеты: {inv.coins}\">?</div>"
                    "<div id=\"t-bar\" style=\"position: absolute; left: 0; top: 40px; width: 200px; height: 20px; background: red;\" "
                    "forge-bar-value=\"hero.hearts\" forge-bar-max=\"hero.hearts_max\" forge-bar-from=\"left\"></div>"
                    "<div id=\"t-key\" forge-show-if=\"inv.key &gt; 0\">ключ</div>"
                    "<div id=\"t-open\" style=\"position: absolute; left: 300px; top: 100px; width: 120px; height: 40px; background: #333;\" "
                    "forge-click=\"[[&quot;show&quot;,&quot;тест_окно&quot;]]\">Открыть</div>"
                    "</div></body></html>";
                const std::string window =
                    "<html><head><style>body, #t-window { pointer-events: none; } #t-window > div { pointer-events: auto; }</style></head>"
                    "<body><div id=\"t-window\" forge-screen=\"command\" forge-pauses=\"1\" forge-appear=\"rise\" forge-appear-time=\"0.3\" forge-size=\"" + dims + "\">"
                    "<div id=\"t-buy\" style=\"position: absolute; left: 300px; top: 300px; width: 120px; height: 40px; background: #353;\" "
                    "forge-click=\"[[&quot;change&quot;,&quot;inv.coins += 5&quot;],[&quot;message&quot;,&quot;купил&quot;],[&quot;close&quot;,&quot;&quot;]]\">Купить</div>"
                    "</div></body></html>";
                check(sc.load_page(s.context(), "тест_hud", hud, "test/ui/тест_hud.html"), "экран HUD строится");
                check(sc.load_page(s.context(), "тест_окно", window, "test/ui/тест_окно.html"), "окно строится");
                s.vars().set("inv.coins", 7);
                s.vars().set("inv.key", 0);
                s.vars().set("hero.hearts", 5);
                s.on_message = [](const std::string& m) { if (m == "купил") ++messages; };
            }
            if (f == 3) {
                Rml::Element* coins = s.find_element("t-coins");
                check(coins && coins->GetInnerRML() == "Монеты: 7", "текст показывает монеты: " + (coins ? coins->GetInnerRML() : std::string("нет")));
                check(shown("t-bar") && !shown("t-key"), "полоска видна, ключа без ключа нет");
                check(!shown("t-buy"), "окно пока скрыто");
                s.vars().set("hero.hearts", 1);
                s.vars().set("inv.key", 1);
            }
            if (f == 5) {
                Rml::Element* bar = s.find_element("t-bar");
                check(bar && bar->GetLocalProperty("mask-image"), "полоска обрезана по сердцам");
                check(shown("t-key"), "с ключом ключ виден");
                check(click(s, "t-open"), "кнопка нажимается");
            }
            if (f == 7) {
                check(sc.shown("тест_окно") && shown("t-buy"), "кнопка открыла окно");
                check(sc.pauses(), "окно ставит мир на паузу");
                Rml::Element* w = s.find_element("t-window");
                check(w && w->GetLocalProperty("opacity") && w->GetProperty<float>("opacity") < 1, "окно появляется плавно");
                SDL_Delay(350); // let it finish rising, or the click lands below the moving button on a slow machine
            }
            if (f == 9) check(click(s, "t-buy"), "кнопка в окне нажимается");
            if (f == 11) {
                check(s.vars().get("inv.coins").number() == 12, "покупка изменила монеты");
                check(messages == 1, "логика получила сообщение");
                check(!sc.shown("тест_окно"), "окно закрылось");
                check(sc.document("тест_окно") && sc.document("тест_окно")->IsVisible(), "и уходит плавно");
                Rml::Element* coins = s.find_element("t-coins");
                check(coins && coins->GetInnerRML() == "Монеты: 12", "текст следит за монетами");
                sc.show("тест_окно", true);
            }
            if (f == 13) {
                SDL_Event ev{};
                ev.type = SDL_EVENT_KEY_DOWN;
                ev.key.key = SDLK_ESCAPE;
                ev.key.down = true;
                s.handle_event(ev);
                check(!sc.shown("тест_окно") && s.screen() == forge::game::Screen::Playing, "Esc закрывает окно, а не игру");
            }
            if (f == 14) SDL_Delay(350); // the window's going away (started by the frame after Esc) takes 0.3 s
            if (f == 16) check(sc.document("тест_окно") && !sc.document("тест_окно")->IsVisible(), "ушедшее окно скрыто");
            if (f < 17) return false;
            sc.remove("тест_hud");
            sc.remove("тест_окно");
            s.on_message = nullptr;
            return true;
        }});
        // Windows' behaviour (13.11): a window that darkens what is under it takes the clicks off the HUD and the
        // world; Esc closes only the window on top, and one that Esc does not close keeps the ones under it (Esc
        // opens the pause); a window set to come up only over the main menu does not open in the game.
        steps_.push_back({"поведение окон", 40, [&s, this](u32 f) {
            GameScreens& sc = s.screens();
            auto key = [&](SDL_Keycode k) {
                SDL_Event ev{};
                ev.type = SDL_EVENT_KEY_DOWN;
                ev.key.key = k;
                ev.key.down = true;
                s.handle_event(ev);
            };
            auto hovered = [&]() {
                Rml::Element* h = s.context()->GetHoverElement();
                return h ? h->GetId() : Rml::String("нет");
            };
            if (f == 0) {
                const Rml::Vector2i size = s.context()->GetDimensions();
                const std::string dims = std::to_string(size.x) + " " + std::to_string(size.y);
                auto page = [&](const std::string& root, const std::string& attrs, bool veil, const std::string& inside) {
                    // As pages were written before 13.11: the page itself is not said to let clicks through.
                    return "<html><head><style>body, #" + root + " { pointer-events: none; } #" + root +
                           " { position: relative; width: 100%; height: 100%; } #" + root + " > div { pointer-events: auto; }" +
                           (veil ? " #forge-dim { position: absolute; left: 0px; top: 0px; width: 100%; height: 100%; "
                                   "background-color: rgba(0, 0, 0, 0.5); pointer-events: auto; }"
                                 : "") +
                           "</style></head><body>" + (veil ? "<div id=\"forge-dim\"></div>" : "") + "<div id=\"" + root +
                           "\" " + attrs + " forge-size=\"" + dims + "\">" + inside + "</div></body></html>";
                };
                auto button = [](const char* id, int x, int y, const char* actions, const char* label) {
                    return std::string("<div id=\"") + id + "\" style=\"position: absolute; left: " + std::to_string(x) +
                           "px; top: " + std::to_string(y) + "px; width: 120px; height: 40px; background: #335;\" forge-click=\"" +
                           actions + "\">" + label + "</div>";
                };
                check(sc.load_page(s.context(), "тест_hud2",
                                   page("o-hud", "forge-screen=\"playing\"", false,
                                        button("o-open", 100, 100, "[[&quot;show&quot;,&quot;тест_затемнение&quot;]]", "Окно") +
                                            button("o-count", 100, 500, "[[&quot;change&quot;,&quot;test.hud += 1&quot;]]", "Счёт")),
                                   "test/ui/тест_hud2.html"),
                      "HUD строится");
                check(sc.load_page(s.context(), "тест_затемнение",
                                   page("o-dim", "forge-screen=\"command\" forge-pauses=\"1\" forge-dim=\"1\" forge-appear=\"fade\" forge-appear-time=\"0.3\"", true,
                                        button("o-more", 600, 300, "[[&quot;show&quot;,&quot;тест_без_esc&quot;]]", "Ещё")),
                                   "test/ui/тест_затемнение.html"),
                      "окно с затемнением строится");
                check(sc.load_page(s.context(), "тест_без_esc",
                                   page("o-stay", "forge-screen=\"command\" forge-esc=\"0\"", false,
                                        button("o-close", 600, 400, "[[&quot;close&quot;,&quot;&quot;]]", "Закрыть")),
                                   "test/ui/тест_без_esc.html"),
                      "окно без Esc строится");
                check(sc.load_page(s.context(), "тест_только_меню",
                                   page("o-menu", "forge-screen=\"command\" forge-over=\"menu\"", false, ""), "test/ui/тест_только_меню.html"),
                      "окно только для меню строится");
                s.vars().set("test.hud", 0);
            }
            if (f == 2) {
                check(sc.over("тест_только_меню") == "menu" && !sc.fits("тест_только_меню"), "окно только для меню не подходит игре");
                check(!sc.show("тест_только_меню", true) && !sc.shown("тест_только_меню"), "в игре оно не открывается");
                check(click(s, "o-count"), "кнопка HUD нажимается");
            }
            if (f == 4) {
                check(s.vars().get("test.hud").number() == 1, "без окна щелчок доходит до HUD");
                check(click(s, "o-open"), "кнопка открывает окно");
            }
            if (f == 6) {
                check(sc.shown("тест_затемнение") && sc.pauses(), "окно открыто и ставит игру на паузу");
                Rml::Element* veil = s.find_element("forge-dim");
                check(veil && veil->IsVisible(true), "затемнение видно");
                check(veil && veil->GetLocalProperty("opacity") && veil->GetProperty<float>("opacity") < 1, "затемнение проявляется вместе с окном");
                check(click(s, "o-count"), "щелчок по кнопке HUD под затемнением");
                check(hovered() == "forge-dim" && !s.over_world(), "мышь над затемнением, не над миром: " + hovered());
                SDL_Delay(350);
            }
            if (f == 8) {
                Rml::Element* veil = s.find_element("forge-dim");
                check(veil && !veil->GetLocalProperty("opacity"), "окно появилось: затемнение целиком");
                check(s.vars().get("test.hud").number() == 1, "затемнение не пропустило щелчок к HUD");
                check(sc.shown("тест_затемнение"), "щелчок по затемнению окно не закрывает");
                check(click(s, "o-more"), "кнопка в окне открывает второе окно");
            }
            if (f == 10) {
                const std::vector<std::string> up = sc.windows();
                check(up.size() == 2 && up.back() == "тест_без_esc", "второе окно сверху");
                key(SDLK_ESCAPE);
                check(sc.shown("тест_без_esc") && sc.shown("тест_затемнение"), "Esc не закрыл ни верхнее окно без Esc, ни окно под ним");
                check(s.screen() == forge::game::Screen::Paused, "Esc открыл паузу игры");
                key(SDLK_ESCAPE);
                check(s.screen() == forge::game::Screen::Playing, "Esc ещё раз — обратно в игру");
            }
            if (f == 12) check(click(s, "o-close"), "второе окно закрывается своей кнопкой");
            if (f == 14) {
                check(!sc.shown("тест_без_esc") && sc.shown("тест_затемнение"), "закрылось только второе окно");
                key(SDLK_ESCAPE);
                check(!sc.shown("тест_затемнение") && s.screen() == forge::game::Screen::Playing, "Esc закрыл окно с затемнением");
                check(sc.windows().empty() && !sc.pauses(), "окон нет, игра идёт");
            }
            if (f == 15) SDL_Delay(350); // the window and its veil fade away
            if (f == 17) check(click(s, "o-count"), "кнопка HUD снова нажимается");
            if (f == 19) {
                check(s.vars().get("test.hud").number() == 2, "без окна щелчок снова доходит до HUD");
                // A window without the veil: a click beside its buttons goes to the HUD under it.
                check(sc.show("тест_без_esc", true), "окно без затемнения открыто над HUD");
            }
            if (f == 21) check(click(s, "o-count"), "щелчок мимо кнопок окна без затемнения");
            if (f == 23) {
                check(s.vars().get("test.hud").number() == 3 && sc.shown("тест_без_esc"), "дошёл до кнопки HUD под окном, окно осталось");
                check(click(s, "o-close"), "окно закрывается своей кнопкой");
            }
            if (f == 25) check(sc.windows().empty(), "окон нет");
            if (f < 26) return false;
            for (const char* n : {"тест_hud2", "тест_затемнение", "тест_без_esc", "тест_только_меню"}) sc.remove(n);
            return true;
        }});
        // The example screens made of «Простой»'s blocks (games/examples/simple-mode, built in): the HUD's
        // coins, hearts and button, the bag its button opens, with a cell per thing and a button that closes it.
        steps_.push_back({"экраны из простых блоков", 30, [&s, this](u32 f) {
            static std::vector<std::pair<std::string, f64>> kept;
            static f64 hearts0 = 0, max0 = 0;
            GameScreens& sc = s.screens();
            auto in = [&](const char* page, const char* id) -> Rml::Element* {
                Rml::ElementDocument* d = sc.document(page);
                return d ? d->GetElementById(id) : nullptr;
            };
            // A click where the layer shows: its place on the drawn 1920 × 1080 screen, by the fit.
            auto press = [&](const char* page, const char* id) {
                Rml::Element* root = in(page, "n1");
                Rml::Element* e = in(page, id);
                if (!root || !e) return false;
                const Rml::Vector2i size = s.context()->GetDimensions();
                const game::ScreenFit fit = game::fit_screen("expand", 1920, 1080, static_cast<f32>(size.x), static_cast<f32>(size.y));
                const Rml::Vector2f c = e->GetAbsoluteOffset(Rml::BoxArea::Border) - root->GetAbsoluteOffset(Rml::BoxArea::Border) +
                                        e->GetBox().GetSize(Rml::BoxArea::Border) * 0.5f;
                const f32 x = game::fit_to_view_x(fit, c.x), y = game::fit_to_view_y(fit, c.y);
                SDL_Event ev{};
                ev.type = SDL_EVENT_MOUSE_MOTION;
                ev.motion.x = x;
                ev.motion.y = y;
                s.handle_event(ev);
                for (const SDL_EventType t : {SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
                    ev = {};
                    ev.type = t;
                    ev.button.button = SDL_BUTTON_LEFT;
                    ev.button.down = t == SDL_EVENT_MOUSE_BUTTON_DOWN;
                    ev.button.x = x;
                    ev.button.y = y;
                    s.handle_event(ev);
                }
                return true;
            };
            auto cells = [&]() {
                std::string out;
                Rml::Element* list = in("простой_сумка", "n4");
                Rml::Element* content = list ? list->GetFirstChild() : nullptr;
                for (int i = 0; content && i < content->GetNumChildren(); ++i) {
                    Rml::Element* c = content->GetChild(i);
                    if (c->GetComputedValues().display() == Rml::Style::Display::None || !c->GetChild(0)) continue;
                    out += (out.empty() ? "" : " | ") + c->GetChild(0)->GetInnerRML();
                }
                return out;
            };
            if (f == 0) {
                check(std::size(kSimplePages) == 2, "в игру встроены 2 экрана примера");
                for (const SimplePage& p : kSimplePages)
                    check(sc.load_page(s.context(), p.name, p.html, path_to_utf8(s.game_dir() / "ui" / (std::string(p.name) + ".html"))),
                          std::string("экран примера строится: ") + p.name);
                kept.clear();
                for (const auto& [name, value] : s.vars().all())
                    if (name.rfind("inv.", 0) == 0 && value.number() != 0) kept.emplace_back(name, value.number());
                for (const auto& [name, value] : kept) s.vars().set(name, 0);
                hearts0 = s.vars().get("hero.hearts").number();
                max0 = s.vars().get("hero.hearts_max").number();
                s.vars().set("inv.coins", 4);
                s.vars().set("inv.key", 1);
                s.vars().set("hero.hearts", 2);
                s.vars().set("hero.hearts_max", 4);
            }
            if (f == 3) {
                Rml::Element* coins = in("простой_hud", "n2");
                check(sc.shown("простой_hud") && coins && coins->GetInnerRML() == "Монеты: 4",
                      "текст из блока показывает монеты: " + (coins ? coins->GetInnerRML() : std::string("нет")));
                Rml::Element* bar = in("простой_hud", "n4");
                check(bar && bar->GetLocalProperty("mask-image"), "полоска из блока обрезана по сердцам");
                check(!sc.shown("простой_сумка"), "сумка пока закрыта");
                check(press("простой_hud", "n5"), "кнопка «Сумка» нажимается");
            }
            if (f == 6) {
                check(sc.shown("простой_сумка"), "кнопка из блока открыла сумку");
                check(cells() == "Монеты 4 | Ключ 1", "в списке из блока ячейка на каждую вещь: " + cells());
                check(press("простой_сумка", "n8"), "кнопка «Закрыть» нажимается");
            }
            if (f == 9) check(!sc.shown("простой_сумка"), "кнопка «Закрыть» закрыла сумку");
            if (f < 10) return false;
            sc.remove("простой_hud");
            sc.remove("простой_сумка");
            s.vars().set("inv.coins", 0);
            s.vars().set("inv.key", 0);
            for (const auto& [name, value] : kept) s.vars().set(name, value);
            s.vars().set("hero.hearts", hearts0);
            s.vars().set("hero.hearts_max", max0);
            return true;
        }});
        // The screen of games/examples/color-picker: colours with alpha (a see-through panel, its shadow, a title,
        // a gradient fading out, a stroke) reach the game's own screens as the editor wrote them.
        steps_.push_back({"цвета палитры с прозрачностью", 6, [&s, this](u32 f) {
            GameScreens& sc = s.screens();
            const char* const name = "палитра_пример";
            auto in = [&](const char* id) -> Rml::Element* {
                Rml::ElementDocument* d = sc.document(name);
                return d ? d->GetElementById(id) : nullptr;
            };
            auto rgba = [&](const char* id, const char* property) {
                Rml::Element* e = in(id);
                if (!e) return std::string("нет");
                const Rml::Colourb c = e->GetProperty<Rml::Colourb>(property);
                return std::to_string(c.red) + " " + std::to_string(c.green) + " " + std::to_string(c.blue) + " " + std::to_string(c.alpha);
            };
            if (f == 0) {
                check(std::size(kColorPages) == 1, "в игру встроен экран примера палитры");
                for (const SimplePage& p : kColorPages)
                    check(sc.load_page(s.context(), p.name, p.html, path_to_utf8(s.game_dir() / "ui" / (std::string(p.name) + ".html"))),
                          std::string("экран примера строится: ") + p.name);
                sc.show(name, true);
            }
            if (f == 3) {
                check(sc.shown(name), "экран примера палитры показан");
                check(rgba("n2", "background-color") == "27 33 39 153", "панель #1B2127 на 60 %: " + rgba("n2", "background-color"));
                check(in("n2") && in("n2")->GetLocalProperty("box-shadow"), "у панели тень");
                check(rgba("n3", "color") == "255 255 255 204", "заголовок белый на 80 %: " + rgba("n3", "color"));
                check(in("n4") && in("n4")->GetLocalProperty("decorator"), "полоса с градиентом до прозрачного");
                check(rgba("n5", "background-color") == "232 176 74 255", "кнопка #E8B04A сплошная: " + rgba("n5", "background-color"));
                check(rgba("n5", "outline-color") == "255 255 255 128", "обводка белая на 50 %: " + rgba("n5", "outline-color"));
            }
            if (f < 5) return false;
            sc.remove(name);
            return true;
        }});
        // The screen of games/examples/key-timeline: the coin where the editor's timeline shows it at a moment,
        // in the game's own screens. The context's clock is set (as the editor's preview sets the canvas's), so
        // each moment is exact: before the delay, between keys, the colour at a key, the second pass backwards.
        steps_.push_back({"движение по ключам в заданный момент", 8, [&s, this](u32 f) {
            GameScreens& sc = s.screens();
            const char* const name = "шкала_пример";
            Rml::ElementDocument* doc = sc.document(name);
            Rml::Element* coin = doc ? doc->GetElementById("n3") : nullptr;
            auto shift = [&]() -> f32 {
                const Rml::Property* p = coin ? coin->GetProperty("transform") : nullptr;
                const Rml::TransformPtr t = p ? p->Get<Rml::TransformPtr>() : nullptr;
                if (t)
                    for (const Rml::TransformPrimitive& prim : t->GetPrimitives())
                        if (prim.type == Rml::TransformPrimitive::TRANSLATE2D) return prim.translate_2d.values[0].number;
                return 0;
            };
            // Seconds since the screen appeared; where the coin is then (3 s a pass after 0.5 s, keys at 0, 30, 60, 100 %;
            // at 60 % it is red). Only forward: what played is not unplayed.
            static const f32 moments[][3] = {{0.25f, 0, 0}, {0.95f, 200, 0}, {1.85f, 600, 0}, {2.3f, 800, 1}, {5.0f, 666.667f, 0}};
            if (f == 0) {
                check(std::size(kTimelinePages) == 1, "в игру встроен экран примера шкалы");
                s.ui().set_clock(s.context(), 0.0);
                s.ui().at_clock(s.context(), [&]() { // its movements start at the context's clock
                    for (const SimplePage& p : kTimelinePages)
                        check(sc.load_page(s.context(), p.name, p.html, path_to_utf8(s.game_dir() / "ui" / (std::string(p.name) + ".html"))),
                              std::string("экран примера строится: ") + p.name);
                    sc.show(name, true);
                    // Shown now, not by the screens' next update on the wall clock: RmlUi starts movements as it shows.
                    if (Rml::ElementDocument* d = sc.document(name)) d->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
                });
                return false;
            }
            if (f >= 2 && f <= 6) {
                const f32* m = moments[f - 2];
                const f32 got = shift();
                check(std::abs(got - m[1]) < 0.5f,
                      "монета в " + std::to_string(m[0]) + " с: сдвиг " + std::to_string(got) + " (ждали " + std::to_string(m[1]) + ")");
                if (m[2] > 0) {
                    const Rml::Colourb c = coin ? coin->GetProperty<Rml::Colourb>("background-color") : Rml::Colourb();
                    check(std::abs(c.red - 255) <= 1 && std::abs(c.green - 64) <= 1 && std::abs(c.blue - 64) <= 1,
                          "монета на третьем ключе красная: " + std::to_string(c.red) + " " + std::to_string(c.green) + " " + std::to_string(c.blue));
                }
            }
            if (f >= 1 && f <= 5) s.ui().set_clock(s.context(), static_cast<double>(moments[f - 1][0]));
            if (f < 7) return false;
            s.ui().set_clock(s.context(), std::nullopt);
            sc.remove(name);
            return true;
        }});
        // The finished screen of games/examples/layer-move: the button the author carried from frame A into frame B
        // takes the click at its new place and does what it did before; its old place in A is now empty.
        steps_.push_back({"перенесённая кнопка нажимается на новом месте", 6, [&s, this](u32 f) {
            GameScreens& sc = s.screens();
            const char* const name = "перенос_готово";
            auto presses = [&]() { return s.vars().get("demo.presses").number(); };
            // A click at a point of the 1920x1080 page, where the fit shows it; what is under the pointer.
            auto press = [&](f32 x, f32 y) {
                const Rml::Vector2i size = s.context()->GetDimensions();
                const game::ScreenFit fit = game::fit_screen("expand", 1920, 1080, static_cast<f32>(size.x), static_cast<f32>(size.y));
                const f32 wx = game::fit_to_view_x(fit, x), wy = game::fit_to_view_y(fit, y);
                SDL_Event ev{};
                ev.type = SDL_EVENT_MOUSE_MOTION;
                ev.motion.x = wx;
                ev.motion.y = wy;
                s.handle_event(ev);
                Rml::Element* hover = s.context()->GetHoverElement();
                for (const SDL_EventType t : {SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
                    ev = {};
                    ev.type = t;
                    ev.button.button = SDL_BUTTON_LEFT;
                    ev.button.down = t == SDL_EVENT_MOUSE_BUTTON_DOWN;
                    ev.button.x = wx;
                    ev.button.y = wy;
                    s.handle_event(ev);
                }
                return hover ? hover->GetId() : Rml::String();
            };
            static f64 before = 0;
            if (f == 0) {
                check(std::size(kMovePages) == 1, "в игру встроен экран примера переноса");
                for (const SimplePage& p : kMovePages)
                    check(sc.load_page(s.context(), p.name, p.html, path_to_utf8(s.game_dir() / "ui" / (std::string(p.name) + ".html"))),
                          std::string("экран примера строится: ") + p.name);
                s.vars().set("demo.presses", 0);
                sc.show(name, true);
            }
            if (f == 2) {
                Rml::ElementDocument* doc = sc.document(name);
                Rml::Element* button = doc ? doc->GetElementById("n4") : nullptr;
                Rml::Element* parent = button ? button->GetParentNode() : nullptr;
                check(sc.shown(name) && parent && parent->GetId() == "n7", "кнопка на экране лежит в «Рамке Б»");
                // In B at 100, 100 (B at 1000, 200), 240 by 80: its middle at 1220, 340 of the page.
                before = presses();
                const Rml::String got = press(1220, 340);
                check(got == "n4" || got == "n5", "под мышью на новом месте кнопка: " + got);
            }
            if (f == 3) {
                check(presses() == before + 1, "кнопка на новом месте делает прежнее: demo.presses " + std::to_string(presses()));
                // Where it was in A (A at 160, 200, the button at 60, 80): its middle at 340, 320 is A's empty fill now.
                const Rml::String got = press(340, 320);
                check(got != "n4" && got != "n5", "на старом месте кнопки нет: " + got);
            }
            if (f == 4) check(presses() == before + 1, "старое место не нажимает: demo.presses " + std::to_string(presses()));
            if (f < 5) return false;
            sc.remove(name);
            s.vars().set("demo.presses", 0);
            return true;
        }});
        // The example's screens in the game: the music of the screen up, one at a time; a window over the
        // game with its own music, and back; a button's sound once per press by mouse and keys; the keys stay
        // the game's over a running game; the volume from the settings; a button that is off, a click in the
        // clipped part, missing files.
        steps_.push_back({"музыка и звук кнопок экранов в игре", 60, [&s, &g, this](u32 f) {
            GameScreens& sc = s.screens();
            audio::ScreenSounds& snd = g.sounds().screens();
            audio::Mixer& mixer = g.sounds().mixer();
            static std::filesystem::path kept;
            static u32 starts = 0, clicks = 0;
            static audio::Voice first;
            static Settings settings;
            static usize problems = 0;
            auto num = [&](const char* name) { return s.vars().get(name).number(); };
            const SimplePage* hud = nullptr;
            for (const SimplePage& p : kSoundPages)
                if (std::string(p.name) == "звук_игра") hud = &p;
            if (f == 0) {
                check(s.screen() == Screen::Playing, "идёт игра");
                kept = snd.folder();
                snd.attach(&mixer, sound_files());
                for (const SimplePage& p : kSoundPages)
                    check(sc.load_page(s.context(), p.name, p.html, path_to_utf8(s.game_dir() / "ui" / (std::string(p.name) + ".html"))),
                          std::string("экран примера строится: ") + p.name);
                s.vars().set("demo.rings", 0);
                starts = snd.music_starts();
                clicks = snd.clicks();
            }
            if (f == 3) {
                check(!sc.shown("звук_меню") && sc.shown("звук_игра"), "в игре виден экран поверх игры, меню нет");
                check(snd.music_name() == "мелодия игры.wav" && snd.music_starts() == starts + 1, "играет музыка игры, начата один раз");
                check(mixer.voices(audio::Bus::Music) == 1 && music_peak(mixer) > 0.05f, "музыку игры слышно, один голос");
                first = snd.music_voice();
                sc.load_page(s.context(), hud->name, hud->html, "test/ui/звук_игра.html"); // saved again in the editor
            }
            if (f == 5) {
                check(snd.music_starts() == starts + 1 && snd.music_voice() == first && mixer.playing(first) &&
                          mixer.voices(audio::Bus::Music) == 1,
                      "страница загружена снова: музыка не начинается заново и не двоится");
                // Over a running game Enter is the game's: a focused button over the world is not pressed by it.
                Rml::Element* open = titled(sc.document("звук_игра"), "Окно");
                if (open) open->Focus();
                key(s, SDLK_RETURN, true);
                key(s, SDLK_RETURN, false);
            }
            if (f == 6) {
                check(!sc.shown("звук_окно") && snd.clicks() == clicks, "Enter над идущей игрой не нажимает кнопку экрана");
                press_page(s, 160, 85); // «Окно» with the mouse
            }
            if (f == 7) check(sc.shown("звук_окно") && snd.clicks() == clicks + 1 && snd.last_click() == "щелчок.wav", "«Окно» открыло окно, щелчок один");
            if (f == 9) {
                check(snd.music_name() == "мелодия окна.wav" && snd.music_starts() == starts + 2 && !mixer.playing(first) &&
                          mixer.voices(audio::Bus::Music) == 1,
                      "окно со своей музыкой: музыка игры остановлена, играет музыка окна");
                check(sc.pauses(), "окно остановило мир");
                // Tab in the window that has just opened: its first button takes the keyboard.
                key(s, SDLK_TAB, true);
                key(s, SDLK_TAB, false);
                Rml::Element* ring = titled(sc.document("звук_окно"), "Звон");
                check(ring && s.context()->GetFocusElement() == ring, "Tab даёт фокус первой кнопке окна");
                key(s, SDLK_RETURN, true);
                key(s, SDLK_RETURN, true, true);
                key(s, SDLK_RETURN, false);
            }
            if (f == 10) {
                check(num("demo.rings") == 1 && snd.clicks() == clicks + 2 && snd.last_click() == "звон.wav",
                      "Enter на «Звон»: одно действие и один звон, без повторов");
                press_page(s, 960, 505); // «Звон» with the mouse
            }
            if (f == 11) {
                check(num("demo.rings") == 2 && snd.clicks() == clicks + 3, "мышь на «Звон»: одно действие и один звон");
                press_page(s, 960, 645); // «Закрыть»: as the screen's buttons, which have none
            }
            if (f == 12) check(!sc.shown("звук_окно") && snd.clicks() == clicks + 3, "«Закрыть» закрыло окно без звука");
            if (f == 14) {
                check(snd.music_name() == "мелодия игры.wav" && snd.music_starts() == starts + 3 && mixer.voices(audio::Bus::Music) == 1,
                      "окно закрыто: снова музыка игры, одна");
                // The settings: music at 0.
                settings = s.settings();
                Settings quiet = settings;
                quiet.music_volume = 0;
                s.apply_settings(quiet);
            }
            if (f == 16) {
                check(snd.music_playing() && music_peak(mixer) == 0, "громкость музыки 0 в настройках: музыку не слышно");
                s.apply_settings(settings);
            }
            if (f == 18) {
                check(music_peak(mixer) > 0.05f, "громкость вернули: музыку снова слышно");
                for (const SimplePage& p : kSoundPages) sc.remove(p.name);
                // A button that is off, a click in what a frame clips, files that are not there.
                const Rml::Vector2i size = s.context()->GetDimensions();
                const std::string page =
                    "<html><head><style>html, body { margin: 0; width: 100%; height: 100%; overflow: hidden; }"
                    " .root { position: relative; width: 100%; height: 100%; } body, .root { pointer-events: none; }"
                    " .root div { pointer-events: auto; position: absolute; height: 60px; background: #c63; }</style></head><body>"
                    "<div class=\"root\" forge-screen=\"command\" forge-size=\"" + std::to_string(size.x) + " " + std::to_string(size.y) +
                    "\" forge-music=\"нет такой.ogg\" forge-button-sound=\"щелчок.wav\">"
                    "<div id=\"t-off\" style=\"left: 100px; top: 100px; width: 200px;\" forge-disabled=\"1\""
                    " forge-click=\"[[&quot;change&quot;,&quot;probe.off += 1&quot;]]\"></div>"
                    "<div id=\"t-clip\" style=\"left: 400px; top: 100px; width: 100px; overflow: hidden; clip: always; border-radius: 12px;\">"
                    "<div id=\"t-clipped\" style=\"left: 0px; top: 0px; width: 300px;\""
                    " forge-click=\"[[&quot;change&quot;,&quot;probe.clip += 1&quot;]]\"></div></div>"
                    "<div id=\"t-missing\" style=\"left: 700px; top: 100px; width: 200px;\" forge-click-sound=\"нет звука.wav\""
                    " forge-click=\"[[&quot;change&quot;,&quot;probe.missing += 1&quot;]]\"></div>"
                    "</div></body></html>";
                check(sc.load_page(s.context(), "звук_проверка", page, "test/ui/звук_проверка.html"), "страница проверки строится");
                sc.show("звук_проверка", true);
                for (const char* v : {"probe.off", "probe.clip", "probe.missing"}) s.vars().set(v, 0);
            }
            if (f == 21) {
                check(snd.music_name() == "нет такой.ogg" && !snd.music_playing() && mixer.voices(audio::Bus::Music) == 0,
                      "музыки нет в папке: тишина, игра идёт");
                problems = snd.problems().size();
                const u32 before = snd.clicks();
                press_window(s, 200, 130); // off
                press_window(s, 600, 130); // the clipped part of a button
                check(num("probe.off") == 0 && num("probe.clip") == 0 && snd.clicks() == before,
                      "выключенная кнопка и обрезанная часть не нажимаются и не звучат: " + std::to_string(num("probe.off")) + " " +
                          std::to_string(num("probe.clip")) + " " + std::to_string(snd.clicks() - before));
                press_window(s, 450, 130); // its part in sight
                check(num("probe.clip") == 1 && snd.clicks() == before + 1,
                      "видимая часть кнопки нажимается со звуком экрана: " + std::to_string(num("probe.clip")) + " " + std::to_string(snd.clicks() - before));
                press_window(s, 800, 130);
                press_window(s, 800, 130);
                check(num("probe.missing") == 2 && snd.clicks() == before + 1, "звука кнопки нет в папке: действие есть, звука нет");
            }
            if (f == 40) {
                check(std::count(snd.problems().begin(), snd.problems().end(), "нет звука.wav") == 1 &&
                          std::count(snd.problems().begin(), snd.problems().end(), "нет такой.ogg") == 1 &&
                          snd.problems().size() == problems + 1,
                      "о каждом недостающем файле сказано один раз, файлы не читаются каждый кадр");
                sc.remove("звук_проверка");
            }
            if (f == 42) {
                check(sc.music().empty() && snd.music_name().empty() && mixer.voices(audio::Bus::Music) == 0, "экраны ушли: тишина");
                snd.attach(&mixer, kept);
                for (const char* v : {"probe.off", "probe.clip", "probe.missing", "demo.rings"}) s.vars().set(v, 0);
                return true;
            }
            return false;
        }});
        // A page drawn bigger than the window, scaled down to it: every button up to the window's right and
        // bottom edges takes the click where it shows, the pointer over it finds it, and the empty bars beside a
        // fitted page take none. (RmlUi checked the clipping of a
        // scaled page in the window's coordinates against the page's, and lost the right and bottom.)
        steps_.push_back({"нажатия у краёв уменьшенной страницы", 12, [&s, this](u32 f) {
            GameScreens& sc = s.screens();
            static const char* const spots[][2] = {{"left: 0px; top: 0px;", "лево-верх"},      {"right: 0px; top: 0px;", "право-верх"},
                                                   {"left: 0px; bottom: 0px;", "лево-низ"},     {"right: 0px; bottom: 0px;", "право-низ"},
                                                   {"right: 0px; top: 45%;", "правый край"},    {"left: 45%; bottom: 0px;", "нижний край"}};
            constexpr usize kSpots = std::size(spots);
            const Rml::Vector2i size = s.context()->GetDimensions();
            const f32 vw = static_cast<f32>(size.x), vh = static_cast<f32>(size.y);
            // Drawn a quarter bigger than the window: scale 0.8. The window-shaped one grows; the wide one fits with bars.
            const f32 ew = std::round(vw * 1.25f), eh = std::round(vh * 1.25f), fw = ew, fh = std::round(vh * 0.625f);
            const game::ScreenFit grow = game::fit_screen("expand", ew, eh, vw, vh), bars = game::fit_screen("fit", fw, fh, vw, vh);
            auto count = [&](const std::string& name) { return s.vars().get(name).number(); };
            auto total = [&]() {
                f64 n = count("probe.half");
                for (usize i = 0; i < kSpots; ++i) n += count("probe.e" + std::to_string(i));
                return n;
            };
            // The pointer and a click at a point of the page, where the fit shows it; what is under the pointer.
            auto press = [&](const game::ScreenFit& fit, f32 x, f32 y) {
                const f32 wx = game::fit_to_view_x(fit, x), wy = game::fit_to_view_y(fit, y);
                SDL_Event ev{};
                ev.type = SDL_EVENT_MOUSE_MOTION;
                ev.motion.x = wx;
                ev.motion.y = wy;
                s.handle_event(ev);
                Rml::Element* hover = s.context()->GetHoverElement();
                for (const SDL_EventType t : {SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
                    ev = {};
                    ev.type = t;
                    ev.button.button = SDL_BUTTON_LEFT;
                    ev.button.down = t == SDL_EVENT_MOUSE_BUTTON_DOWN;
                    ev.button.x = wx;
                    ev.button.y = wy;
                    s.handle_event(ev);
                }
                return hover ? hover->GetId() : Rml::String();
            };
            const std::string style = "<style>html, body { margin: 0; width: 100%; height: 100%; overflow: hidden; }"
                                      " .root { position: relative; width: 100%; height: 100%; overflow: hidden; }"
                                      " body, .root { pointer-events: none; } .root > div { pointer-events: auto; position: absolute;"
                                      " width: 160px; height: 60px; background: #c63; }</style>";
            auto dims = [](f32 w, f32 h) { return std::to_string(static_cast<int>(w)) + " " + std::to_string(static_cast<int>(h)); };
            if (f == 0) {
                std::string page = "<html><head>" + style + "</head><body><div class=\"root\" forge-screen=\"command\" forge-fit=\"expand\" forge-size=\"" +
                                   dims(ew, eh) + "\">";
                for (usize i = 0; i < kSpots; ++i)
                    page += "<div id=\"t-e" + std::to_string(i) + "\" style=\"" + spots[i][0] + "\" forge-click=\"[[&quot;change&quot;,&quot;probe.e" +
                            std::to_string(i) + " += 1&quot;]]\"></div>";
                page += "</div></body></html>";
                // A layer half out of the page's bottom: its outer half is in the bars.
                const std::string wide = "<html><head>" + style + "</head><body><div class=\"root\" forge-screen=\"command\" forge-fit=\"fit\" forge-size=\"" +
                                         dims(fw, fh) + "\" forge-bars=\"#102030\"><div id=\"t-half\" style=\"left: 200px; bottom: -30px;\" "
                                         "forge-click=\"[[&quot;change&quot;,&quot;probe.half += 1&quot;]]\"></div></div></body></html>";
                check(sc.load_page(s.context(), "тест_края", page, "test/ui/тест_края.html") &&
                          sc.load_page(s.context(), "тест_полосы", wide, "test/ui/тест_полосы.html"),
                      "уменьшенные страницы строятся");
                for (usize i = 0; i < kSpots; ++i) s.vars().set("probe.e" + std::to_string(i), 0);
                s.vars().set("probe.half", 0);
                sc.show("тест_края", true);
            }
            if (f == 3) {
                check(grow.sx < 0.81f && grow.sx > 0.79f, "страница уменьшена до 0,8: " + std::to_string(grow.sx));
                Rml::ElementDocument* doc = sc.document("тест_края");
                for (usize i = 0; i < kSpots; ++i) {
                    Rml::Element* b = doc ? doc->GetElementById("t-e" + std::to_string(i)) : nullptr;
                    Rml::Element* root = b ? b->GetParentNode() : nullptr;
                    if (!b || !root) {
                        check(false, std::string("кнопка есть: ") + spots[i][1]);
                        continue;
                    }
                    // Its middle and a point 4 px (page) inside its outer corner, where the window edge is nearest.
                    const Rml::Vector2f at = b->GetAbsoluteOffset(Rml::BoxArea::Border) - root->GetAbsoluteOffset(Rml::BoxArea::Border);
                    const Rml::Vector2f box = b->GetBox().GetSize(Rml::BoxArea::Border);
                    const f64 before = total();
                    const std::string id = "t-e" + std::to_string(i);
                    const std::string under = press(grow, at.x + box.x * 0.5f, at.y + box.y * 0.5f);
                    const f32 cx = at.x + box.x * 0.5f > grow.width * 0.5f ? at.x + box.x - 4 : at.x + 4;
                    const f32 cy = at.y + box.y * 0.5f > grow.height * 0.5f ? at.y + box.y - 4 : at.y + 4;
                    const std::string corner = press(grow, cx, cy);
                    check(under == id && corner == id, std::string(spots[i][1]) + ": указатель над кнопкой находит её (" + under + ", у края " + corner + ")");
                    check(count("probe.e" + std::to_string(i)) == 2 && total() == before + 2,
                          std::string(spots[i][1]) + ": два нажатия попали в эту кнопку и только в неё");
                }
                sc.show("тест_края", false);
                sc.show("тест_полосы", true);
            }
            if (f == 6) {
                check(bars.top > 1 && bars.sx < 0.81f, "широкая страница вписана с полосами сверху и снизу");
                Rml::ElementDocument* doc = sc.document("тест_полосы");
                Rml::Element* half = doc ? doc->GetElementById("t-half") : nullptr;
                const f64 before = total();
                // Inside the page: the half of the layer that shows.
                check(half && press(bars, 280, fh - 10) == "t-half" && count("probe.half") == 1, "видимая половина слоя нажимается");
                // In the bars, where nothing is drawn: above the page, and below it away from the layer. (The layer's
                // outer half shows in the bar below, as RmlUi draws a placed layer sticking out of an unscrolled page.)
                press(bars, 280, -10);
                press(bars, 10, -bars.top / bars.sy * 0.5f);
                press(bars, fw - 20, fh + 10);
                press(bars, fw * 0.5f, fh + bars.top / bars.sy * 0.5f);
                check(total() == before + 1, "щелчки по полосам вне страницы ничего не делают: " + std::to_string(total() - before - 1));
            }
            if (f < 7) return false;
            sc.remove("тест_края");
            sc.remove("тест_полосы");
            return true;
        }});
        // Clipping in hit testing, as RmlUi draws it: a layer under «clip: none» leaves the clipping of the
        // layers around it (as a drop-down's list does), «clip: N» leaves N of them, and without either a layer
        // outside a clipping parent takes no pointer.
        steps_.push_back({"отсечение и нажатия", 6, [&s, this](u32 f) {
            GameScreens& sc = s.screens();
            struct Probe {
                const char* id;   // the layer looked for under the pointer
                f32 x, y;         // the point (window pixels)
                bool found;       // whether it is found there
                const char* what;
            };
            // Boxes at 100 px steps down the window; the buttons stick out to the right of their 100 × 100 parent.
            static const Probe probes[] = {
                {"c-none", 160, 140, true, "clip: none на предке выводит из отсечения внешнего"},
                {"c-plain", 160, 240, false, "без clip: none слой за краем отсекающего родителя не нажимается"},
                {"c-plain", 60, 240, true, "та же кнопка внутри родителя нажимается"},
                {"c-one", 160, 340, false, "clip: 1 пропускает одну область, внешний родитель всё равно отсекает"},
                {"c-two", 160, 440, true, "clip: 2 пропускает обе области"},
                {"c-auto", 160, 540, false, "clip: auto во вложенных отсекающих слоях"},
            };
            if (f == 0) {
                const Rml::Vector2i size = s.context()->GetDimensions();
                const std::string dims = std::to_string(size.x) + " " + std::to_string(size.y); // scale 1
                auto box = [](int top, const std::string& inner_style, const std::string& id, const std::string& button_style) {
                    // The inner layer is in the flow, so the outer one's content overflows it and it clips (RmlUi,
                    // as it draws, clips only what makes a layer scroll: absolutely placed layers do not).
                    return "<div style=\"position: absolute; left: 0px; top: " + std::to_string(top) +
                           "px; width: 100px; height: 100px; overflow: hidden;\">"
                           "<div style=\"position: relative; width: 300px; height: 100px; " + inner_style + "\">"
                           "<div id=\"" + id + "\" style=\"position: absolute; left: 40px; top: 20px; width: 140px; height: 40px; "
                           "background: #f00; " + button_style + "\"></div></div></div>";
                };
                const std::string page =
                    "<html><head><style>html, body { margin: 0; width: 100%; height: 100%; overflow: hidden; }"
                    " .root { position: relative; width: 100%; height: 100%; } body, .root { pointer-events: none; }"
                    " .root div { pointer-events: auto; }</style></head><body><div class=\"root\" forge-screen=\"command\" forge-size=\"" +
                    dims + "\">" +
                    box(100, "clip: none;", "c-none", "") + box(200, "", "c-plain", "") +
                    box(300, "overflow: hidden;", "c-one", "clip: 1;") + box(400, "overflow: hidden;", "c-two", "clip: 2;") +
                    box(500, "overflow: hidden;", "c-auto", "") + "</div></body></html>";
                check(sc.load_page(s.context(), "тест_отсечение", page, "test/ui/тест_отсечение.html"), "страница отсечения строится");
                sc.show("тест_отсечение", true);
            }
            if (f == 3) {
                for (const Probe& p : probes) {
                    SDL_Event ev{};
                    ev.type = SDL_EVENT_MOUSE_MOTION;
                    ev.motion.x = p.x;
                    ev.motion.y = p.y;
                    s.handle_event(ev);
                    Rml::Element* hover = s.context()->GetHoverElement();
                    const std::string id = hover ? hover->GetId() : std::string();
                    check((id == p.id) == p.found, std::string(p.what) + " (под указателем «" + id + "»)");
                }
            }
            if (f < 4) return false;
            sc.remove("тест_отсечение");
            return true;
        }});
        // Lists on a screen: the hero's things and the journal, a cell per
        // element; only the cells in sight are made.
        steps_.push_back({"списки на экране", 40, [&s, this](u32 f) {
            static std::vector<std::pair<std::string, f64>> kept;
            static std::vector<std::string> said;
            GameScreens& sc = s.screens();
            // The cells shown now, top to bottom: their texts.
            auto cells = [&](const char* list) {
                std::vector<std::string> out;
                Rml::Element* box = s.find_element(list);
                Rml::Element* content = box ? box->GetFirstChild() : nullptr;
                if (!content) return out;
                std::vector<std::pair<float, std::string>> seen;
                for (int i = 0; i < content->GetNumChildren(); ++i) {
                    Rml::Element* c = content->GetChild(i);
                    if (c->GetComputedValues().display() == Rml::Style::Display::None) continue;
                    seen.emplace_back(c->GetOffsetTop(), c->GetChild(0) ? c->GetChild(0)->GetInnerRML() : "");
                }
                std::sort(seen.begin(), seen.end());
                for (auto& [top, text] : seen) out.push_back(text);
                return out;
            };
            auto columns = [&](const char* list) {
                std::vector<float> lefts;
                Rml::Element* box = s.find_element(list);
                Rml::Element* content = box ? box->GetFirstChild() : nullptr;
                for (int i = 0; content && i < content->GetNumChildren(); ++i) {
                    Rml::Element* c = content->GetChild(i);
                    if (c->GetComputedValues().display() == Rml::Style::Display::None) continue;
                    if (std::find(lefts.begin(), lefts.end(), c->GetOffsetLeft()) == lefts.end()) lefts.push_back(c->GetOffsetLeft());
                }
                return lefts.size();
            };
            auto empty_shown = [&]() {
                Rml::Element* e = s.find_element("t-empty");
                return e && e->GetComputedValues().visibility() == Rml::Style::Visibility::Visible;
            };
            auto joined = [](const std::vector<std::string>& v) {
                std::string out;
                for (const std::string& x : v) out += (out.empty() ? "" : " | ") + x;
                return out;
            };
            if (f == 0) {
                // The hero's things put aside: the list starts from what the test gives.
                kept.clear();
                said.clear();
                for (const auto& [name, value] : s.vars().all())
                    if (name.rfind("inv.", 0) == 0 && value.number() != 0) kept.emplace_back(name, value.number());
                for (const auto& [name, value] : kept) s.vars().set(name, 0);
                s.vars().set("inv.coins", 3);
                s.vars().set("inv.key", 1);
                const Rml::Vector2i size = s.context()->GetDimensions();
                const std::string dims = std::to_string(size.x) + " " + std::to_string(size.y);
                const std::string page =
                    "<html><head><style>body, #t-bag { pointer-events: none; } #t-items, #t-journal { pointer-events: auto; }"
                    " .cell { position: absolute; left: 0px; top: 0px; width: 380px; height: 40px; background: #333; }</style></head>"
                    "<body><div id=\"t-bag\" forge-screen=\"command\" forge-size=\"" + dims + "\">"
                    "<div id=\"t-items\" style=\"position: absolute; left: 100px; top: 100px; width: 400px; height: 300px; overflow-y: auto;\" "
                    "forge-list=\"items\" forge-list-gap=\"4\" forge-cell=\"0 0 380 40\">"
                    "<div class=\"cell\" forge-click=\"[[&quot;message&quot;,&quot;взял {item.id}&quot;]]\">"
                    "<div forge-text=\"{item.name}: {item.count}\">?</div><div forge-picture=\"item.icon\"></div></div>"
                    "<div id=\"t-empty\">Пусто</div></div>"
                    "<div id=\"t-journal\" style=\"position: absolute; left: 600px; top: 100px; width: 400px; height: 300px; overflow-y: auto;\" "
                    "forge-list=\"quests\" forge-list-gap=\"4\" forge-cell=\"0 0 380 40\">"
                    "<div class=\"cell\"><div forge-text=\"{item.title}\">?</div></div></div>"
                    // A grid: 3 cells a row (a short last row), then 2 when narrower.
                    "<div id=\"t-grid\" style=\"position: absolute; left: 100px; top: 450px; width: 400px; height: 200px; overflow-y: auto;\" "
                    "forge-list=\"items\" forge-list-gap=\"4\" forge-cell=\"0 0 120 40\">"
                    "<div class=\"cell\" style=\"width: 120px;\"><div forge-text=\"{item.name}\">?</div></div></div>"
                    // The cell's own condition: only what the hero has more than one of
                    // (written with line breaks, as the editor writes pages).
                    "<div id=\"t-cond\" style=\"position: absolute; left: 600px; top: 450px; width: 400px; height: 300px; overflow-y: auto;\" "
                    "forge-list=\"items\" forge-list-gap=\"4\" forge-cell=\"0 0 380 40\">\n    "
                    "<div class=\"cell\" forge-show-if=\"item.count &gt; 1\"><div forge-text=\"{item.name}\">?</div></div>\n  </div>"
                    "</div></body></html>";
                // At the game's own ui/ (not written there): item.icon finds ../pictures/ as a game screen does.
                check(sc.load_page(s.context(), "тест_сумка", page, path_to_utf8(s.game_dir() / "ui" / "тест_сумка.html")),
                      "экран со списками строится");
                sc.show("тест_сумка", true);
                s.on_message = [](const std::string& m) { said.push_back(m); };
            }
            if (f == 3) {
                check(joined(cells("t-items")) == "Монеты: 3 | Ключ: 1", "список вещей: " + joined(cells("t-items")));
                check(!empty_shown(), "пока есть вещи, «Пусто» не видно");
                // item.icon: the picture of the pickup that gives the thing, or none.
                Rml::Element* box = s.find_element("t-items");
                Rml::Element* content = box ? box->GetFirstChild() : nullptr;
                int pictured = 0;
                for (int i = 0; content && i < content->GetNumChildren(); ++i) {
                    Rml::Element* c = content->GetChild(i);
                    Rml::Element* icon = c->GetNumChildren() > 1 ? c->GetChild(1) : nullptr;
                    if (!icon || c->GetComputedValues().display() == Rml::Style::Display::None) continue;
                    const std::string name = c->GetChild(0)->GetInnerRML();
                    std::string want;
                    for (const game::ScreenItem& t : g_.screen_items())
                        if (name.rfind(t.name + ":", 0) == 0) want = t.picture;
                    const Rml::Property* p = icon->GetLocalProperty("decorator");
                    const std::string look = p ? p->ToString() : std::string("none");
                    check(want.empty() ? look == "none" : look.find("../" + want) != std::string::npos,
                          name + ": картинка «" + want + "», показано " + look);
                    pictured += !want.empty();
                }
                FORGE_INFO("списки: у %d вещей из видимых своя картинка", pictured);
                check(cells("t-grid").size() == 2, "2 вещи в сетке по 3: видно 2 ячейки, а не " + std::to_string(cells("t-grid").size()));
                check(joined(cells("t-cond")) == "Монеты", "условие ячейки прячет то, чего одна штука: " + joined(cells("t-cond")));
                usize journal = s.quests().journal(s.vars()).size();
                check(cells("t-journal").size() == journal, "в списке заданий " + std::to_string(cells("t-journal").size()) +
                                                                " из " + std::to_string(journal));
                s.vars().set("inv.torch", 2); // what picking up does
            }
            if (f == 5) {
                check(joined(cells("t-items")) == "Монеты: 3 | Ключ: 1 | Факелы: 2", "подобранное появилось: " + joined(cells("t-items")));
                check(cells("t-grid").size() == 3 && columns("t-grid") == 3, "3 вещи: полный ряд из 3");
                s.vars().set("inv.coins", 7);
            }
            if (f == 7) {
                check(joined(cells("t-items")) == "Монеты: 7 | Ключ: 1 | Факелы: 2", "количество обновилось: " + joined(cells("t-items")));
                s.vars().set("inv.coins", 0);
                s.vars().set("inv.key", 0);
            }
            if (f == 9) {
                check(joined(cells("t-items")) == "Факелы: 2", "убранное исчезло: " + joined(cells("t-items")));
                check(cells("t-grid").size() == 1, "в сетке осталась 1 ячейка, а не " + std::to_string(cells("t-grid").size()));
                s.vars().set("inv.torch", 0);
            }
            if (f == 11) {
                check(cells("t-items").empty(), "последний предмет убран, ячеек нет: " + joined(cells("t-items")));
                check(empty_shown(), "пустой список показывает «Пусто»");
                check(cells("t-grid").empty() && cells("t-cond").empty(), "в пустых сетке и списке с условием ячеек не видно: " +
                                                                              std::to_string(cells("t-grid").size()) + ", " +
                                                                              std::to_string(cells("t-cond").size()));
                char id[16];
                for (int i = 1; i <= 500; ++i) {
                    std::snprintf(id, sizeof(id), "inv.t%04d", i);
                    s.vars().set(id, i);
                }
            }
            if (f == 13) {
                check(!empty_shown(), "с вещами «Пусто» снова скрыто");
                check(sc.list_cells("тест_сумка") < 80, "ячеек сделано только для видимого (4 списка по 500): " + std::to_string(sc.list_cells("тест_сумка")));
                check(cells("t-items").size() >= 6 && cells("t-items").front() == "t0001: 1", "500 вещей: " + cells("t-items").front());
                // 200 px high, 44 a row: 5 rows of 3 in sight.
                check(cells("t-grid").size() == 15 && columns("t-grid") == 3, "сетка снова заполнена: " + std::to_string(cells("t-grid").size()));
                // Cells hidden while the list was empty come back under their own condition: t0001 (one) stays hidden.
                const std::vector<std::string> cond = cells("t-cond");
                check(!cond.empty() && cond.front() == "t0002" && std::find(cond.begin(), cond.end(), "t0001") == cond.end(),
                      "после пустого списка условие ячейки снова действует: " + joined(cond));
                if (Rml::Element* grid = s.find_element("t-grid")) grid->SetProperty("width", "260px");
                if (Rml::Element* box = s.find_element("t-items")) box->SetScrollTop(4000); // 4000 / 44: the 91st first
            }
            if (f == 15) {
                const std::vector<std::string> now = cells("t-items");
                check(!now.empty() && now.front() == "t0091: 91", "после прокрутки сверху t0091: " + (now.empty() ? std::string() : now.front()));
                check(sc.list_cells("тест_сумка") < 80, "после прокрутки ячеек всё так же мало: " + std::to_string(sc.list_cells("тест_сумка")));
                check(cells("t-grid").size() == 10 && columns("t-grid") == 2, "уже сетка: 5 рядов по 2, а не " +
                                                                                 std::to_string(cells("t-grid").size()));
                // Scrolled to the end: the last row is short (500 = 249 rows of 2 and 2), no extra cells.
                if (Rml::Element* grid = s.find_element("t-grid")) grid->SetScrollTop(100000);
                // A click on a cell acts on the element it shows now.
                Rml::Element* box = s.find_element("t-items");
                Rml::Element* hit = box ? s.context()->GetElementAtPoint(box->GetAbsoluteOffset(Rml::BoxArea::Border) + Rml::Vector2f(190, 60)) : nullptr;
                check(click(s, hit), "ячейка нажимается");
            }
            if (f == 17) {
                // 4000 + 60 = 4060 / 44: the 93rd (t0093).
                check(said.size() == 1 && said[0] == "взял t0093", "нажатие пришло от своей ячейки: " + (said.empty() ? std::string("ничего") : said[0]));
                const std::vector<std::string> end = cells("t-grid");
                check(!end.empty() && end.back() == "t0500" && std::count(end.begin(), end.end(), "t0500") == 1,
                      "в конце сетки последняя вещь одна и последняя: " + joined(end));
                s.vars().set("inv.t0500", 0); // the last row gets one cell shorter
            }
            if (f == 19) {
                const std::vector<std::string> end = cells("t-grid");
                check(!end.empty() && end.back() == "t0499" && std::find(end.begin(), end.end(), "t0500") == end.end(),
                      "неполный последний ряд: лишней ячейки нет: " + joined(end));
            }
            if (f < 20) return false;
            char id[16];
            for (int i = 1; i <= 500; ++i) {
                std::snprintf(id, sizeof(id), "inv.t%04d", i);
                s.vars().set(id, 0);
            }
            for (const auto& [name, value] : kept) s.vars().set(name, value);
            sc.remove("тест_сумка");
            s.on_message = nullptr;
            return true;
        }});
        // A screen drawn for another size than the player's: it meets this
        // screen by game::fit_screen (the rule «Интерфейс» shows), a layer
        // stuck to a corner stays there and a click lands where the layer shows.
        steps_.push_back({"экран другого размера", 30, [&s, this](u32 f) {
            static f64 coins0 = 0;
            GameScreens& sc = s.screens();
            struct Kind {
                const char* fit;
                f32 w, h; // drawn for
            };
            // 16:9 grown, 21:9 whole with bars, 4:3 stretched (the editor's 21:9 and 4:3 sizes).
            static const Kind kinds[] = {{"expand", 1920, 1080}, {"fit", 2560, 1080}, {"stretch", 1440, 1080}};
            const u32 k = f / 8, at = f % 8;
            if (k >= std::size(kinds)) {
                s.vars().set("inv.coins", coins0);
                return true;
            }
            const Kind& kind = kinds[k];
            const std::string name = std::string("тест_размер_") + kind.fit;
            const Rml::Vector2i size = s.context()->GetDimensions();
            const game::ScreenFit fit = game::fit_screen(kind.fit, kind.w, kind.h, static_cast<f32>(size.x), static_cast<f32>(size.y));
            if (at == 0) {
                if (k == 0) coins0 = s.vars().get("inv.coins").number();
                char dims[64];
                std::snprintf(dims, sizeof(dims), "%g %g", static_cast<double>(kind.w), static_cast<double>(kind.h));
                const std::string page =
                    "<html><head><style>html, body { margin: 0; width: 100%; height: 100%; overflow: hidden; }"
                    " #t-size { position: relative; width: 100%; height: 100%; overflow: hidden; }" // as the editor writes a screen
                    " body, #t-size { pointer-events: none; } #t-size > div { pointer-events: auto; }</style></head>"
                    "<body><div id=\"t-size\" forge-screen=\"command\" forge-fit=\"" + std::string(kind.fit) + "\" forge-size=\"" + dims +
                    "\" forge-bars=\"#102030\">"
                    "<div id=\"t-corner\" style=\"position: absolute; right: 20px; bottom: 20px; width: 160px; height: 60px; background: #c33;\"></div>"
                    "<div id=\"t-press\" style=\"position: absolute; left: 200px; top: 300px; width: 140px; height: 50px; background: #3c3;\" "
                    "forge-click=\"[[&quot;change&quot;,&quot;inv.coins += 1&quot;]]\"></div>"
                    "</div></body></html>";
                check(sc.load_page(s.context(), name, page, "test/ui/" + name + ".html"), name + " строится");
                sc.show(name, true);
                s.vars().set("inv.coins", 0);
            }
            if (at == 3) {
                // Layout pixels: the root's corner is where the fit puts it.
                Rml::Element* root = s.find_element("t-size");
                Rml::Element* corner = s.find_element("t-corner");
                const Rml::Vector2f r = root ? root->GetAbsoluteOffset(Rml::BoxArea::Border) : Rml::Vector2f(-1, -1);
                const Rml::Vector2f c = corner ? corner->GetAbsoluteOffset(Rml::BoxArea::Border) - r : Rml::Vector2f(-1, -1);
                check(std::fabs(r.x - fit.left) < 0.5f && std::fabs(r.y - fit.top) < 0.5f,
                      name + ": экран стоит в " + std::to_string(r.x) + ", " + std::to_string(r.y));
                check(std::fabs(c.x - (fit.width - 180)) < 0.5f && std::fabs(c.y - (fit.height - 80)) < 0.5f,
                      name + ": угловой слой у своего угла: " + std::to_string(c.x) + ", " + std::to_string(c.y));
                // Its corner on the player's screen: inside it, by the fit's scale.
                const f32 right = game::fit_to_view_x(fit, c.x + 160), bottom = game::fit_to_view_y(fit, c.y + 60);
                check(right <= static_cast<f32>(size.x) + 0.5f && bottom <= static_cast<f32>(size.y) + 0.5f,
                      name + ": угол на экране игрока");
                // The click where the layer shows (not where its layout box is).
                const f32 x = game::fit_to_view_x(fit, 270), y = game::fit_to_view_y(fit, 325);
                SDL_Event ev{};
                ev.type = SDL_EVENT_MOUSE_MOTION;
                ev.motion.x = x;
                ev.motion.y = y;
                s.handle_event(ev);
                for (const SDL_EventType t : {SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
                    ev = {};
                    ev.type = t;
                    ev.button.button = SDL_BUTTON_LEFT;
                    ev.button.down = t == SDL_EVENT_MOUSE_BUTTON_DOWN;
                    ev.button.x = x;
                    ev.button.y = y;
                    s.handle_event(ev);
                }
            }
            if (at == 6) {
                check(s.vars().get("inv.coins").number() == 1, name + ": нажатие попало в слой там, где он виден");
                sc.remove(name);
            }
            return false;
        }});
        // Where the picture is taken.
        steps_.push_back({"кадр", 200, [&s, &g, &talk, v, stand, this](u32 f) {
            const SliceGenerator& gen = g.generator();
            if (scene_ == "links") { // «Связи» over the village: the hero picked, then Boris
                if (f == 0) {
                    g.teleport(6.5, v - stand);
                    g.spawn_critter(2.5, v, Scheme::Stand);
                    g.overlay().show(true);
                }
                if (f == 60) click(s, "ln-ring-hero");
                if (f == 62) click(s, "ln-ring-miner");
                return f >= 90;
            }
            if (scene_ == "menu") {
                if (f == 0) s.to_main_menu();
                return f >= 120;
            }
            if (scene_ == "mine") {
                if (f == 0) g.teleport(gen.pool_x1() + 6.5, gen.gallery_y() + 1 - 0.93);
                return f >= 90;
            }
            if (scene_ == "door") { // the door at the end of the gallery (the key has opened it)
                if (f == 0) g.teleport(gen.gallery_x1() + 5.5, gen.gallery_y() + 1 - 0.93);
                return f >= 90;
            }
            // The village, talking to Boris.
            if (f == 0) {
                s.vars().set("quest.pickaxe", 0);
                s.vars().set("inv.pickaxe", 0);
            }
            if (!near_npc(g, 0, f)) return false;
            if (f == 60) {
                g.talk_nearest();
                talk.advance();
            }
            return f >= 90;
        }});
    }

    // All the numbers at once: 200 000 critters living along the surface, a
    // million particles, liquids and the hero playing, in one world.
    void build_stress(Shell& s) {
        SliceGame& g = g_;
        steps_.push_back({"меню", 30, [&s, this](u32 f) {
            if (f < 5) return false;
            check(s.new_game(), "новая игра начинается");
            return true;
        }});
        steps_.push_back({"200 000 существ и миллион частиц", 400, [&g, this](u32 f) {
            static f64 sim_total = 0, sim_worst = 0;
            if (f == 0) {
                check(g.entities() >= 200'000, "в мире 200 000 существ, а не " + std::to_string(g.entities()));
                Controls c;
                c.right = true;
                c.jump = true;
                g.script(c);
            }
            if (f >= 60) {
                sim_total += g.sim_ms();
                sim_worst = std::max(sim_worst, g.sim_ms());
            }
            if (f < 300) return false;
            const forge::sim::SimStats* st = g.sim_stats();
            FORGE_INFO("stress: %u entities, %u particles, sim avg %.2f ms, worst %.2f ms; chunks active %u near %u",
                       g.entities(), g.particles(), sim_total / 240.0, sim_worst, st ? st->zones.active : 0, st ? st->zones.near : 0);
            check(g.entities() >= 200'000, "существа на месте");
            check(g.particles() >= 900'000, "частиц около миллиона, а не " + std::to_string(g.particles()));
            return true;
        }});
    }

    // An inventory of 10 000 things in a window, scrolled to the end and back
    // at an even speed; what the frames took while it scrolled is the
    // number. With --window it runs on the real GPU (add --no-vsync to see
    // past the monitor's rate); offscreen the picture is drawn without a GPU.
    // --scene module_slots (step 14.3a), with --user DIR of the test module's scene probe_save (its slot probe-slot
    // there): a new game's values are all the module's (and the module offers no value the game has not); the slot of
    // another module, and slots whose module is no module ("" and 5), are refused before anything changes, the game
    // and its session file for file as they were; a slot from before step 14.3a (slot.json without "module", as the
    // game wrote it then) loads as «Старая шахта»'s. It saves slice-slot and leaves old-slot for probe_foreign.
    void build_module_slots(Shell& s) {
        steps_.push_back({"новая игра", 30, [&s, this](u32 f) {
            if (f < 5) return false;
            check(s.new_game(), "новая игра начинается");
            return true;
        }});
        steps_.push_back({"значения модуля", 10, [&s, this](u32 f) {
            if (f < 3) return false;
            const modules::ModuleDef def = module_def();
            const std::vector<modules::Value> values = def.values(&g_.library());
            check(values.size() >= 5, "у модуля есть значения: " + std::to_string(values.size()));
            for (const modules::Value& v : values)
                check(s.vars().has(v.var), "значение модуля «" + v.words + "» (" + v.var + ") есть в игре");
            check(!s.vars().has("probe.ticks") && !s.vars().has("probe.mark"), "значений модуля «Проба» в игре нет");
            check(s.save("slice-slot", "Шахта до проверки слотов"), "игра сохраняется в slice-slot");
            const std::optional<SlotInfo> info = s.slots().info("slice-slot");
            check(info && info->module == kModuleId && info->module_error.empty(), "слот записан с модулем «slice»");
            return true;
        }});
        steps_.push_back({"слоты чужого модуля и сломанные", 10, [&s, this](u32 f) {
            if (f < 2) return false;
            const std::optional<SlotInfo> probe_slot = s.slots().info("probe-slot");
            check(probe_slot && probe_slot->module == "probe",
                  "слот probe-slot тестового модуля есть (его оставила сцена probe_save с той же папкой --user)");
            check(write_slot(s, "empty-module", "\"\"") && write_slot(s, "number-module", "5"), "слоты с «module»: \"\" и 5 записаны");
            for (const char* slot : {"probe-slot", "empty-module", "number-module"}) {
                const std::optional<SlotInfo> info = s.slots().info(slot);
                std::string why;
                check(info && !slot_fits(*info, kModuleId, &why), std::string(slot) + ": не слот «Старой шахты»: " + why);
                if (std::string(slot) != "probe-slot")
                    check(info && info->module.empty() && !info->module_error.empty(),
                          std::string(slot) + ": модуль не читается, и это не slice: " + (info ? info->module_error : ""));
                s.vars().set("test.mark", 7);
                check(s.save("slice-before", "Шахта до отказа"), "сессия записана перед загрузкой");
                const auto before = session_files(s.slots().session());
                const f64 x = g_.hero_x(), y = g_.hero_y();
                check(!s.load(slot), std::string("слот ") + slot + " не загружается");
                check(session_files(s.slots().session()) == before, std::string("после отказа ") + slot + " сессия та же, файл в файл");
                check(g_.running() && s.screen() == Screen::Playing && var(s, "test.mark") == 7 && g_.hero_x() == x && g_.hero_y() == y,
                      std::string("и игра идёт как шла: метка 7, герой на месте после ") + slot);
            }
            return true;
        }});
        steps_.push_back({"слот до шага 14.3a", 30, [&s, this](u32 f) {
            if (f == 0) {
                check(write_slot(s, "old-slot", nullptr), "слот без «module», как его писала игра до шага 14.3a, записан");
                const std::optional<SlotInfo> info = s.slots().info("old-slot");
                check(info && info->module == kModuleId && info->module_error.empty(), "слот без «module» читается как слот «Старой шахты»");
                s.vars().set("test.mark", 8);
                check(s.load("old-slot"), "и загружается");
                return false;
            }
            if (f < 10) return false;
            check(g_.running() && s.screen() == Screen::Playing && var(s, "test.mark") == 7,
                  "игра из старого слота идёт, с тем, что в нём было (метка 7)");
            return true;
        }});
    }
    // A slot made of slice-before (saved by the steps above) with slot.json as the game wrote it before step 14.3a:
    // the reflected fields only, then "module" with this JSON value when module_json is given ("", 5).
    bool write_slot(Shell& s, const char* id, const char* module_json) {
        std::optional<SlotInfo> info = s.slots().info("slice-before");
        if (!info && !s.save("slice-before", "Шахта до отказа")) return false;
        info = s.slots().info("slice-before");
        if (!info) return false;
        std::error_code ec;
        const std::filesystem::path to = s.slots().folder(id);
        std::filesystem::remove_all(to, ec);
        std::filesystem::copy(s.slots().folder("slice-before"), to, std::filesystem::copy_options::recursive, ec);
        if (ec) return false;
        info->module.clear();
        std::string text = data::to_json(*info);
        if (module_json) {
            const usize end = text.rfind('}');
            if (end == std::string::npos) return false;
            text = text.substr(0, end) + ",\n  \"module\": " + module_json + "\n}\n";
            std::string named;
            const GameModule kind = game_module_of(text, named);
            if (kind != GameModule::Broken) return false; // what the test means: a module that is none
        }
        return write_file_atomic(to / "slot.json", {reinterpret_cast<const u8*>(text.data()), text.size()});
    }
    void build_inventory(Shell& s) {
        steps_.push_back({"меню", 30, [&s, this](u32 f) {
            if (f < 5) return false;
            check(s.new_game(), "новая игра начинается");
            return true;
        }});
        steps_.push_back({"10 000 вещей", 30, [&s, this](u32 f) {
            if (f == 0) {
                char id[24];
                for (int i = 1; i <= kInventoryItems; ++i) {
                    std::snprintf(id, sizeof(id), "inv.item%05d", i);
                    s.vars().set(id, i % 99 + 1);
                }
                const Rml::Vector2i size = s.context()->GetDimensions();
                const std::string dims = std::to_string(size.x) + " " + std::to_string(size.y);
                const std::string box = "left: 40px; top: 40px; width: " + std::to_string(size.x - 80) + "px; height: " +
                                        std::to_string(size.y - 80) + "px;";
                const std::string page =
                    "<html><head><style>body, #inv-root { pointer-events: none; } #inv-list { pointer-events: auto; }"
                    " .cell { position: absolute; left: 8px; top: 8px; width: 180px; height: 64px; background: #2a3138; border-radius: 8px; }"
                    " .name { position: absolute; left: 10px; top: 8px; color: #e3e8ec; font-size: 16px; }"
                    " .count { position: absolute; right: 10px; bottom: 8px; color: #ffb77a; font-size: 14px; }</style></head>"
                    "<body><div id=\"inv-root\" forge-screen=\"command\" forge-size=\"" + dims + "\">"
                    "<div id=\"inv-list\" style=\"position: absolute; " + box + " overflow-y: auto; background: #12161ae0;\" "
                    "forge-list=\"items\" forge-list-gap=\"8\" forge-cell=\"8 8 180 64\">"
                    "<div class=\"cell\" forge-click=\"[[&quot;message&quot;,&quot;{item.id}&quot;]]\">"
                    "<div class=\"name\" forge-text=\"{item.name}\">?</div><div class=\"count\" forge-text=\"×{item.count}\">?</div></div>"
                    "<div>Пусто</div></div></div></body></html>";
                check(s.screens().load_page(s.context(), "инвентарь", page, "test/ui/инвентарь.html"), "инвентарь строится");
                s.screens().show("инвентарь", true);
            }
            return f >= 5;
        }});
        steps_.push_back({"прокрутка 10 000 вещей", 1400, [&s, this](u32 f) {
            static std::vector<f64> frame_ms, list_ms;
            static u64 last = 0;
            static usize most_cells = 0;
            constexpr u32 kPass = 600; // frames down, then as many back up
            Rml::Element* box = s.find_element("inv-list");
            if (!box) {
                check(false, "нет списка");
                return true;
            }
            const u64 now = time_now_ns();
            if (f == 0) {
                frame_ms.clear();
                list_ms.clear();
                most_cells = 0;
            } else if (f > 1) { // the first frames lay it out
                frame_ms.push_back(static_cast<f64>(now - last) * 1e-6);
                list_ms.push_back(s.screens().lists_ms());
            }
            last = now;
            most_cells = std::max(most_cells, s.screens().list_cells("инвентарь"));
            const f32 max = std::max(box->GetScrollHeight() - box->GetClientHeight(), 0.0f);
            const f32 t = f <= kPass ? static_cast<f32>(f) / kPass : static_cast<f32>(2 * kPass - f) / kPass;
            // Every so often: the cell at the top shows what is really there.
            if (f % 100 == 50) {
                Rml::Element* content = box->GetFirstChild();
                const f32 scroll = box->GetScrollTop();
                const int row = static_cast<int>((scroll - 8) / 72);
                const int cols = std::max(1, static_cast<int>((box->GetClientWidth() - 8 + 8) / 188));
                char want[24];
                std::snprintf(want, sizeof(want), "item%05d", std::max(row, 0) * cols + 1);
                bool found = false;
                for (int i = 0; content && i < content->GetNumChildren(); ++i) {
                    Rml::Element* c = content->GetChild(i);
                    if (c->GetComputedValues().display() == Rml::Style::Display::None) continue;
                    if (c->GetChild(0) && c->GetChild(0)->GetInnerRML() == want) found = true;
                }
                check(found, std::string("после прокрутки на ") + std::to_string(static_cast<int>(scroll)) + " видна ячейка " + want);
                // A wide list puts several cells in a row.
                std::vector<float> lefts;
                for (int i = 0; content && i < content->GetNumChildren(); ++i) {
                    Rml::Element* c = content->GetChild(i);
                    if (c->GetComputedValues().display() == Rml::Style::Display::None) continue;
                    if (std::find(lefts.begin(), lefts.end(), c->GetOffsetLeft()) == lefts.end()) lefts.push_back(c->GetOffsetLeft());
                }
                check(static_cast<int>(lefts.size()) == cols && cols >= 5,
                      "в ряду " + std::to_string(lefts.size()) + " ячеек, рядов по " + std::to_string(cols));
            }
            if (f < 2 * kPass) {
                box->SetScrollTop(max * t);
                return false;
            }
            std::vector<f64> sorted = frame_ms;
            std::sort(sorted.begin(), sorted.end());
            f64 sum = 0, lsum = 0, lworst = 0;
            for (f64 v : frame_ms) sum += v;
            for (f64 v : list_ms) {
                lsum += v;
                lworst = std::max(lworst, v);
            }
            const f64 n = std::max<f64>(static_cast<f64>(frame_ms.size()), 1.0);
            const f64 avg = sum / n, p99 = sorted.empty() ? 0 : sorted[sorted.size() * 99 / 100],
                      worst = sorted.empty() ? 0 : sorted.back();
            char line[512];
            std::snprintf(line, sizeof(line),
                          "inventory scroll: %d items, %zu frames, frame avg %.2f ms (%.0f FPS), p99 %.2f ms, worst %.2f ms; "
                          "lists avg %.3f ms, worst %.3f ms; cells made %zu",
                          kInventoryItems, frame_ms.size(), avg, avg > 0 ? 1000.0 / avg : 0.0, p99, worst, lsum / n, lworst, most_cells);
            FORGE_INFO("%s", line);
            if (std::FILE* out = std::fopen("inventory-scroll.txt", "w")) {
                std::fprintf(out, "%s\n", line);
                std::fclose(out);
            }
            check(most_cells < 200, "ячеек сделано только для видимого: " + std::to_string(most_cells));
            return true;
        }});
    }
    static constexpr int kInventoryItems = 10000;

    // games/examples/window-behaviour (13.11), the pages as the editor saved them, read from disk by the game's own
    // GameScreens::load (the package carries the folder in data/examples): a main menu, a screen over the game and
    // two windows. With the player's mouse and keys, as the game's window gives them: where a window may come up,
    // the veil and clicks under a window, the world standing while a window's buttons work, Esc and the keyboard's
    // focus, and the game getting its input back.
    void build_windows(Shell& s) {
        SliceGame& g = g_;
        static const char* const kPages[] = {"окна_меню", "окна_игра", "окна_настройки", "окна_справка"};
        static f64 x0 = 0, x1 = 0;
        static Rml::Element* hud_focus = nullptr;
        // An event as the game's window passes it: to the screens and the shell, then what they leave to the world.
        auto send = [&s, &g](const SDL_Event& e) {
            if (!s.handle_event(e) && s.screen() == Screen::Playing && !s.in_dialogue()) g.handle_event(e);
        };
        auto key = [send](SDL_Keycode k, bool down, bool repeat = false) {
            SDL_Event ev{};
            ev.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            ev.key.key = k;
            ev.key.down = down;
            ev.key.repeat = repeat;
            send(ev);
        };
        auto press = [key](SDL_Keycode k) {
            key(k, true);
            key(k, false);
        };
        // The mouse at a point of a page's 1920 × 1080, where the page's fit shows it on the player's screen.
        auto at = [&s](f32 x, f32 y) {
            const Rml::Vector2i size = s.context()->GetDimensions();
            const game::ScreenFit fit = game::fit_screen("expand", 1920, 1080, static_cast<f32>(size.x), static_cast<f32>(size.y));
            return Rml::Vector2f(game::fit_to_view_x(fit, x), game::fit_to_view_y(fit, y));
        };
        auto mouse = [send](Rml::Vector2f p, bool click) {
            SDL_Event ev{};
            ev.type = SDL_EVENT_MOUSE_MOTION;
            ev.motion.x = p.x;
            ev.motion.y = p.y;
            send(ev);
            for (const SDL_EventType t : {SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
                if (!click) break;
                ev = {};
                ev.type = t;
                ev.button.button = SDL_BUTTON_LEFT;
                ev.button.down = t == SDL_EVENT_MOUSE_BUTTON_DOWN;
                ev.button.x = p.x;
                ev.button.y = p.y;
                send(ev);
            }
        };
        // The middle of a button of a page, on the player's screen.
        auto spot = [&s, at](const char* page, const char* title) -> std::optional<Rml::Vector2f> {
            Rml::ElementDocument* doc = s.screens().document(page);
            Rml::Element* root = doc ? doc->GetElementById("n1") : nullptr;
            Rml::Element* e = titled(doc, title);
            if (!root || !e) return std::nullopt;
            const Rml::Vector2f c = e->GetAbsoluteOffset(Rml::BoxArea::Border) - root->GetAbsoluteOffset(Rml::BoxArea::Border) +
                                    e->GetBox().GetSize(Rml::BoxArea::Border) * 0.5f;
            return at(c.x, c.y);
        };
        auto click = [spot, mouse](const char* page, const char* title) {
            const auto p = spot(page, title);
            if (p) mouse(*p, true);
            return p.has_value();
        };
        auto focus = [&s](const char* page, const char* title) {
            Rml::Element* f = s.context()->GetFocusElement();
            return f && f == titled(s.screens().document(page), title);
        };
        auto num = [&s](const char* name) { return s.vars().get(name).number(); };
        // The game's own menus (game/shell.rml): the middle of an element on the screen, what the mouse is over,
        // a button of a part by its label, where a document stands among the context's (back to front).
        auto middle = [](Rml::Element* e) { return e->GetAbsoluteOffset(Rml::BoxArea::Border) + e->GetBox().GetSize(Rml::BoxArea::Border) * 0.5f; };
        auto under_mouse = [&s](Rml::Element* e) {
            for (Rml::Element* h = s.context()->GetHoverElement(); h; h = h->GetParentNode())
                if (h == e) return true;
            return false;
        };
        auto hovered = [&s]() -> std::string {
            Rml::Element* h = s.context()->GetHoverElement();
            if (!h) return "ничего";
            Rml::ElementDocument* d = h->GetOwnerDocument();
            return "«" + h->GetTagName() + "#" + h->GetId() + "» в «" + (d ? d->GetTitle() : std::string()) + "»";
        };
        auto shell_item = [&s](const char* part, const char* label) -> Rml::Element* {
            Rml::Element* box = s.find_element(part);
            Rml::ElementList labels;
            if (box) box->GetElementsByClassName(labels, "label");
            for (Rml::Element* e : labels)
                if (e->GetInnerRML() == label) return e->GetParentNode();
            return nullptr;
        };
        auto shell_close = [&s](const char* part) -> Rml::Element* {
            Rml::Element* box = s.find_element(part);
            Rml::ElementList buttons;
            if (box) box->GetElementsByClassName(buttons, "icon-button");
            return buttons.empty() ? nullptr : buttons.front();
        };
        auto depth = [&s](const Rml::ElementDocument* d) {
            for (int i = 0; i < s.context()->GetNumDocuments(); ++i)
                if (s.context()->GetDocument(i) == d) return i;
            return -1;
        };

        steps_.push_back({"пример поведения окон с диска", 20, [&s, this](u32 f) {
            GameScreens& sc = s.screens();
            if (f == 0) {
                const std::filesystem::path dir = s.game_dir().parent_path() / "examples" / "window-behaviour";
                std::error_code ec;
                for (const char* name : kPages)
                    check(std::filesystem::is_regular_file(dir / "ui" / utf8_path(std::string(name) + ".html"), ec) &&
                              std::filesystem::is_regular_file(dir / "ui" / utf8_path(std::string(name) + ".json"), ec),
                          std::string("файлы примера на диске: ") + name + " в " + path_to_utf8(dir / "ui"));
                // As the game reads its own game/ui when it starts.
                FORGE_INFO("пример окон читается из %s", path_to_utf8(dir / "ui").c_str());
                sc.load(s.context(), dir, false);
                const std::vector<std::string> names = sc.names();
                for (const char* name : kPages) {
                    Rml::ElementDocument* doc = sc.document(name);
                    std::string url = doc ? doc->GetSourceURL() : std::string();
                    std::replace(url.begin(), url.end(), '\\', '/');
                    check(std::find(names.begin(), names.end(), name) != names.end() &&
                              url.ends_with("window-behaviour/ui/" + std::string(name) + ".html"),
                          std::string("страница прочитана из файла: ") + name + " (" + url + ")");
                }
                check(names.size() == std::size(kPages), "других экранов нет: " + std::to_string(names.size()));
                check(sc.over("окна_справка") == "game" && sc.over("окна_настройки").empty(), "где появляются окна: справка только над игрой");
                for (const char* v : {"demo.volume", "demo.coins"}) s.vars().set(v, 0);
            }
            if (f < 3) return false;
            check(s.screen() == Screen::Main && sc.shown("окна_меню"), "меню примера вместо меню игры");
            check(sc.windows().empty(), "окон пока нет");
            return true;
        }});
        steps_.push_back({"окна над главным меню", 40, [&s, press, key, focus, num, this](u32 f) {
            GameScreens& sc = s.screens();
            const std::vector<std::string> settings{"окна_настройки"};
            if (f == 0) {
                press(SDLK_TAB);
                check(focus("окна_меню", "Настройки"), "Tab: клавиатура у «Настройки» меню");
                press(SDLK_RETURN);
            }
            if (f == 2) {
                check(sc.windows() == settings && s.screen() == Screen::Main && sc.shown("окна_меню"),
                      "Enter: окно открылось поверх главного меню, меню на месте");
                Rml::Element* fe = s.context()->GetFocusElement();
                check(fe && fe->GetOwnerDocument() == sc.document("окна_настройки"), "окно с паузой взяло клавиатуру");
                press(SDLK_TAB);
                check(focus("окна_настройки", "Громче"), "Tab: «Громче» в окне");
                press(SDLK_RETURN);
            }
            if (f == 4) {
                check(num("demo.volume") == 1, "Enter нажал кнопку окна");
                press(SDLK_TAB);
                check(focus("окна_настройки", "Справка"), "Tab: «Справка»");
                press(SDLK_RETURN);
            }
            if (f == 6) {
                check(sc.windows() == settings && !sc.shown("окна_справка"), "справка только над игрой: над меню не открылась");
                // Esc held: it closes the window once; its repeats do nothing more.
                key(SDLK_ESCAPE, true);
                key(SDLK_ESCAPE, true, true);
                key(SDLK_ESCAPE, true, true);
                key(SDLK_ESCAPE, false);
                check(sc.windows().empty() && s.screen() == Screen::Main && sc.shown("окна_меню"), "Esc закрыл окно, меню осталось");
            }
            if (f == 8) {
                check(focus("окна_меню", "Настройки"), "клавиатура вернулась к кнопке, открывшей окно");
                press(SDLK_TAB);
                check(focus("окна_меню", "Играть"), "Tab: «Играть»");
                press(SDLK_RETURN);
            }
            if (f < 10) return false;
            return s.screen() == Screen::Playing && g_.running() && g_.hero_alive();
        }});
        steps_.push_back({"окна в игре", 140, [&s, &g, press, key, at, mouse, click, spot, focus, num, middle, under_mouse, hovered,
                                                shell_item, shell_close, depth, this](u32 f) {
            GameScreens& sc = s.screens();
            auto right = [&g]() {
                Controls c;
                c.right = true;
                g.script(c);
            };
            if (f == 0) {
                check(sc.shown("окна_игра") && !sc.shown("окна_меню") && sc.windows().empty(), "в игре экран примера над игрой");
                for (const char* v : {"demo.volume", "demo.coins"}) s.vars().set(v, 0);
            }
            if (f < 30 && !g.on_ground()) return false; // the hero lands first
            if (f == 30) {
                right();
                x0 = g.hero_x();
            }
            if (f == 40) {
                check(g.hero_x() > x0 + 0.1, "мир идёт: герой идёт вправо, " + std::to_string(x0) + " → " + std::to_string(g.hero_x()));
                mouse(at(1000, 500), false);
                check(s.over_world(), "мышь на пустом месте над миром (мимо кнопок экрана)");
                check(click("окна_игра", "Монета"), "щелчок по «Монета»");
            }
            if (f == 42) {
                check(num("demo.coins") == 1, "без окна щелчок доходит до кнопки над игрой");
                check(click("окна_игра", "Настройки"), "щелчок по «Настройки» над игрой");
                // Over the running game the keyboard is the game's: the click leaves the focus where it pressed.
                hud_focus = s.context()->GetFocusElement();
                check(hud_focus && hud_focus->GetOwnerDocument() == sc.document("окна_игра"), "фокус на экране над игрой");
            }
            if (f == 44) {
                check(sc.windows() == std::vector<std::string>{"окна_настройки"} && sc.pauses(), "окно открылось над игрой и ставит её на паузу");
                Rml::ElementDocument* doc = sc.document("окна_настройки");
                Rml::Element* veil = doc ? doc->GetElementById("forge-dim") : nullptr;
                check(veil && veil->IsVisible(true), "затемнение видно");
                x1 = g.hero_x();
                // Under the veil: the button over the game does not get the click, nor does the world.
                const auto coin = spot("окна_игра", "Монета");
                if (coin) mouse(*coin, true);
                Rml::Element* h = s.context()->GetHoverElement();
                check(h && h->GetId() == "forge-dim" && !s.over_world(), "мышь над затемнением, а не над миром и кнопками");
                // A key of the world's while the window stops the game: the slot stays.
                press(SDLK_2);
                check(g.slot() == 0, "клавиша игры при окне с паузой до мира не дошла: ячейка " + std::to_string(g.slot()));
            }
            if (f == 50) {
                check(std::fabs(g.hero_x() - x1) < 1e-9, "мир стоит: герой не сдвинулся, хотя «вправо» держат");
                check(num("demo.coins") == 1 && sc.shown("окна_настройки"), "затемнение не пропустило щелчок; окно не закрылось");
                check(click("окна_настройки", "Громче"), "щелчок по «Громче» в окне");
            }
            if (f == 52) {
                check(num("demo.volume") == 1, "кнопки окна работают, пока мир стоит");
                // The keyboard is the window's: the button pressed with the mouse has it, Space presses it again and
                // the hero does not jump.
                check(focus("окна_настройки", "Громче"), "клавиатура у «Громче», нажатой мышью");
                press(SDLK_SPACE);
            }
            if (f == 54) {
                check(num("demo.volume") == 2 && std::fabs(g.hero_x() - x1) < 1e-9, "пробел нажал «Громче», мир всё ещё стоит");
                check(click("окна_настройки", "Справка"), "щелчок по «Справка»");
            }
            if (f == 56) {
                check(sc.windows() == std::vector<std::string>{"окна_настройки", "окна_справка"}, "справка открылась над окном (над игрой можно)");
                check(click("окна_настройки", "Громче"), "щелчок по «Громче» рядом со справкой");
            }
            if (f == 58) {
                check(num("demo.volume") == 3 && sc.shown("окна_справка"), "справка без затемнения: щелчок рядом с ней дошёл до окна под ней");
                key(SDLK_ESCAPE, true);
                check(sc.windows().size() == 2 && s.screen() == Screen::Paused, "Esc: справка без Esc и окно под ней остались, открылась пауза");
                key(SDLK_ESCAPE, true, true);
                key(SDLK_ESCAPE, false);
                check(s.screen() == Screen::Paused, "удержанный Esc паузу не закрыл");
            }
            // The pause, drawn a frame later, is over both windows (the lower one was clicked last): the player
            // sees it, the mouse is on it, and what is under it does not get the click.
            const std::vector<std::string> both{"окна_настройки", "окна_справка"};
            if (f == 60) {
                Rml::Element* resume = s.find_element("pause-resume");
                check(resume && resume->IsVisible(true), "пауза видна");
                if (resume) mouse(middle(resume), false);
                check(resume && under_mouse(resume), "мышь на «Продолжить» паузы, а не на окне под ней: " + hovered());
                // «Понятно» stands beside the pause's panel, under its veil.
                if (const auto ok = spot("окна_справка", "Понятно")) mouse(*ok, true);
                check(under_mouse(s.find_element("pause-menu")) && sc.windows() == both && s.screen() == Screen::Paused,
                      "под паузой кнопка окна не срабатывает: мышь " + hovered());
                Rml::Element* journal = shell_item("pause-menu", "Журнал");
                check(journal != nullptr, "«Журнал» в паузе");
                if (journal) mouse(middle(journal), true);
            }
            if (f == 62) {
                Rml::Element* close = shell_close("journal");
                check(s.screen() == Screen::Journal && close && close->IsVisible(true), "«Журнал» паузы открыл журнал над окнами");
                if (close) mouse(middle(close), false);
                check(close && under_mouse(close), "мышь на кнопке журнала, а не на окне под ним: " + hovered());
                if (close) mouse(middle(close), true);
            }
            if (f == 64) {
                check(s.screen() == Screen::Paused, "журнал закрылся, снова пауза");
                Rml::Element* settings = shell_item("pause-menu", "Настройки");
                check(settings != nullptr, "«Настройки» в паузе");
                if (settings) mouse(middle(settings), true);
            }
            if (f == 66) {
                Rml::Element* full = s.find_element("set-fullscreen");
                check(s.screen() == Screen::Settings && full && full->IsVisible(true), "«Настройки» паузы открыли настройки игры над окнами");
                if (full) mouse(middle(full), false);
                check(full && under_mouse(full), "мышь на переключателе настроек игры, а не на окне: " + hovered());
                press(SDLK_ESCAPE);
                check(s.screen() == Screen::Paused, "Esc: из настроек обратно в паузу");
            }
            if (f == 68) {
                Rml::Element* resume = s.find_element("pause-resume");
                if (resume) mouse(middle(resume), true);
                check(s.screen() == Screen::Playing, "щелчок по «Продолжить» вернул в игру");
                check(sc.windows() == both && num("demo.volume") == 3, "оба окна на месте, их кнопки под паузой не нажимались");
            }
            if (f == 70) {
                const int w1 = depth(sc.document("окна_настройки")), w2 = depth(sc.document("окна_справка"));
                Rml::Element* pause = s.find_element("pause-menu");
                const int menus = pause ? depth(pause->GetOwnerDocument()) : -1;
                check(w1 >= 0 && w1 < w2 && w2 < menus, "порядок: справка над окном, меню игры над обоими (" + std::to_string(w1) + ", " +
                                                            std::to_string(w2) + ", " + std::to_string(menus) + ")");
                check(focus("окна_настройки", "Громче"), "клавиатура вернулась к окну, к «Громче», а не к скрытой кнопке паузы");
                press(SDLK_J);
            }
            if (f == 72) {
                Rml::Element* close = shell_close("journal");
                check(s.screen() == Screen::Journal && close && close->IsVisible(true), "J: журнал открылся над окнами");
                if (close) mouse(middle(close), false);
                check(close && under_mouse(close), "мышь на кнопке журнала, открытого клавишей: " + hovered());
                press(SDLK_J);
                check(s.screen() == Screen::Playing && sc.windows() == both, "J ещё раз: обратно в игру, окна на месте");
            }
            if (f == 74) {
                check(click("окна_справка", "Понятно"), "щелчок по «Понятно»");
            }
            if (f == 76) {
                {
                    std::string up;
                    for (const std::string& w : sc.windows()) up += w + " ";
                    check(sc.windows() == std::vector<std::string>{"окна_настройки"}, "«Понятно» закрыл только справку: " + up);
                }
                check(std::fabs(g.hero_x() - x1) < 1e-9, "окно с паузой открыто: мир по-прежнему стоит");
                // The only window clicked: the pages stay in order, and it is still under the game's menus.
                check(click("окна_настройки", "Громче"), "щелчок по «Громче» единственного окна");
            }
            if (f == 78) {
                check(num("demo.volume") == 4, "«Громче» нажалось");
                press(SDLK_J);
            }
            if (f == 80) {
                Rml::Element* close = shell_close("journal");
                check(s.screen() == Screen::Journal && close && close->IsVisible(true), "J: журнал открылся над щёлкнутым окном");
                if (close) mouse(middle(close), false);
                check(close && under_mouse(close), "мышь на кнопке журнала, а не на щёлкнутом окне под ним: " + hovered());
                press(SDLK_J);
                check(s.screen() == Screen::Playing && sc.windows() == std::vector<std::string>{"окна_настройки"}, "J ещё раз: обратно в игру, окно на месте");
                key(SDLK_ESCAPE, true);
                key(SDLK_ESCAPE, true, true);
                key(SDLK_ESCAPE, false);
                check(sc.windows().empty() && !sc.pauses() && s.screen() == Screen::Playing, "Esc закрыл окно, игра идёт");
            }
            if (f == 81) SDL_Delay(350); // the window and its veil fade away
            if (f == 83) {
                check(hud_focus && s.context()->GetFocusElement() == hud_focus, "фокус вернулся туда, где был до окна: на «Настройки» над игрой");
                check(click("окна_игра", "Монета"), "щелчок по «Монета» снова");
                press(SDLK_2);
                check(g.slot() == 1, "клавиша игры снова доходит до мира: ячейка " + std::to_string(g.slot()));
                mouse(at(1000, 500), false);
                check(s.over_world(), "мышь на пустом месте снова над миром");
            }
            if (f == 93) {
                check(num("demo.coins") == 2, "щелчок снова доходит до кнопки над игрой");
                check(g.hero_x() > x1 + 0.1, "мир снова идёт: герой идёт вправо, " + std::to_string(x1) + " → " + std::to_string(g.hero_x()));
                g.select(0);
                // The game's own screens again (games/slice has none).
                sc.load(s.context(), s.game_dir(), false);
                return true;
            }
            return false;
        }});
    }

    // The volumes a player saved, in the game made of templates (--scene volumes): music and sounds at 0, all at
    // 100, as settings.json had them when the game started (main writes it into this run's own folder). A saved 0
    // is the player's silence, kept: not «no value», not a new player's 70; the template's −10 and +10 stop at 0 and
    // 100; what is heard is what settings.json keeps; a save made at another volume does not change it when loaded.
    template <typename Click, typename Num, typename Near>
    void build_volumes(Shell& s, Click click, Num num, Near near) {
        static f64 music_in_save = -1;
        auto three = [](f64 a, f64 b, f64 c) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%g, %g, %g", a, b, c);
            return std::string(buf);
        };
        // The whole, the music, the sounds: in the settings, in the variables, in the mixer, in settings.json.
        auto at = [&s, num, near, three, this](f64 master, f64 music, f64 sound, const std::string& what) {
            const Settings& v = s.settings();
            const audio::Mixer& m = g_.sounds().mixer();
            const Settings file = game::load_settings(s.user_folder());
            check(near(v.master_volume, static_cast<f32>(master / 100)) && near(v.music_volume, static_cast<f32>(music / 100)) &&
                      near(v.sound_volume, static_cast<f32>(sound / 100)),
                  what + ": настройки " + three(v.master_volume * 100.0, v.music_volume * 100.0, v.sound_volume * 100.0));
            check(num("settings.master") == master && num("settings.music") == music && num("settings.sound") == sound,
                  what + ": переменные " + three(num("settings.master"), num("settings.music"), num("settings.sound")));
            check(near(m.master(), static_cast<f32>(master / 100)) && near(m.volume(audio::Bus::Music), static_cast<f32>(music / 100)) &&
                      near(m.volume(audio::Bus::Sound), static_cast<f32>(sound / 100)) && near(m.volume(audio::Bus::Ui), static_cast<f32>(sound / 100)),
                  what + ": микшер " + three(m.master() * 100.0, m.volume(audio::Bus::Music) * 100.0, m.volume(audio::Bus::Sound) * 100.0));
            check(near(file.master_volume, static_cast<f32>(master / 100)) && near(file.music_volume, static_cast<f32>(music / 100)) &&
                      near(file.sound_volume, static_cast<f32>(sound / 100)),
                  what + ": settings.json " + three(file.master_volume * 100.0, file.music_volume * 100.0, file.sound_volume * 100.0));
        };
        // What the music's scale writes under it.
        auto numbers = [&s]() {
            Rml::ElementDocument* doc = s.screens().document("шаблоны_настройки");
            Rml::Element* row = nullptr;
            Rml::ElementList all;
            if (doc) doc->QuerySelectorAll(all, "[title]");
            for (Rml::Element* e : all)
                if (e->GetAttribute<Rml::String>("title", "") == "Строка «Музыка»") row = e;
            Rml::Element* text = row ? row->QuerySelector("[title=Числа]") : nullptr;
            return text ? text->GetInnerRML() : std::string("нет слоя");
        };
        steps_.push_back({"громкость, сохранённая игроком: 0 и 100", 40, [&s, at, click, numbers, this](u32 f) {
            GameScreens& sc = s.screens();
            if (f == 0) {
                at(100, 0, 0, "игра снова запущена: музыка и звуки 0, как игрок их оставил, а не 70 и 100 новой игры");
                check(s.settings().vsync && s.settings().ui_scale == 1.0f, "остальное как было");
                check(click("шаблоны_меню", nullptr, "Кнопка «Настройки»"), "щелчок по «Настройки» меню");
            }
            if (f == 1) {
                check(sc.windows() == std::vector<std::string>{"шаблоны_настройки"}, "окно настроек открылось");
                SDL_Delay(300);
            }
            if (f == 3) {
                check(numbers() == "0", "шкала музыки пишет 0: " + numbers());
                check(click("шаблоны_настройки", "Строка «Музыка»", "Кнопка «−»"), "«−» у музыки при 0");
            }
            if (f == 5) {
                at(100, 0, 0, "«−» при 0: музыка 0, а не −10");
                check(numbers() == "0", "шкала музыки пишет 0: " + numbers());
                check(click("шаблоны_настройки", "Строка «Общая»", "Кнопка «+»"), "«+» у общей при 100");
            }
            if (f == 7) {
                at(100, 0, 0, "«+» при 100: общая 100, а не 110");
                check(click("шаблоны_настройки", "Строка «Музыка»", "Кнопка «+»"), "«+» у музыки");
            }
            if (f == 9) {
                at(100, 10, 0, "«+»: музыка 10, слышна и записана");
                check(numbers() == "10", "шкала музыки пишет 10: " + numbers());
                check(click("шаблоны_настройки", "Строка «Музыка»", "Кнопка «−»"), "«−» у музыки");
            }
            if (f == 11) {
                at(100, 0, 0, "«−»: снова 0, и это записано");
                // What «Логика» or a button's «Изменить данные» may set: past the ends, brought in. The game's
                // mixer takes the settings in its own update, which comes before the variables' in a frame: heard
                // from the next one.
                s.vars().set("settings.sound", 250);
            }
            if (f == 13) {
                at(100, 0, 100, "settings.sound = 250: звуки 100");
                s.vars().set("settings.sound", -40);
            }
            if (f == 15) {
                at(100, 0, 0, "settings.sound = −40: звуки 0");
                // What the game's code may ask of the settings: past the ends, brought in.
                Settings loud = s.settings();
                loud.music_volume = 1.5f;
                s.apply_settings(loud);
            }
            if (f == 17) {
                at(100, 100, 0, "игра просит громкость музыки 1,5: 100");
                Settings quiet = s.settings();
                quiet.music_volume = 0;
                s.apply_settings(quiet);
            }
            if (f == 19) {
                at(100, 0, 0, "и снова 0");
                for (const bool down : {true, false}) {
                    SDL_Event ev{};
                    ev.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                    ev.key.key = SDLK_ESCAPE;
                    ev.key.down = down;
                    s.handle_event(ev);
                }
                check(sc.windows().empty() && s.screen() == Screen::Main, "Esc закрыл окно настроек");
            }
            if (f == 21) check(click("шаблоны_меню", nullptr, "Кнопка «Новая игра»"), "щелчок по «Новая игра»");
            if (f < 22) return false;
            return s.screen() == Screen::Playing && g_.running();
        }});
        steps_.push_back({"сохранение игры не меняет громкость", 40, [&s, at, num, this](u32 f) {
            if (f == 0) {
                at(100, 0, 0, "новая игра: громкость та же");
                check(s.save("volumes", "Громкость"), "игра сохраняется при музыке 0");
                std::vector<u8> bytes;
                read_file(s.slots().folder("volumes") / "state.json", bytes);
                Vars in_save;
                const std::string text(bytes.begin(), bytes.end());
                const usize from = text.find("\"vars\"");
                const usize open = from == std::string::npos ? from : text.find('{', from);
                const usize close = open == std::string::npos ? open : text.find('}', open);
                if (close != std::string::npos) in_save.from_json(std::string_view(text).substr(open, close - open + 1));
                music_in_save = in_save.get("settings.music").number();
                check(in_save.has("settings.music") && music_in_save == 0, "в сохранении settings.music 0");
                // «Логика» turns the music up after the save.
                s.vars().set("settings.music", 50);
            }
            if (f == 2) {
                at(100, 50, 0, "«Логика» прибавила музыку до 50 после сохранения");
                check(s.load("volumes"), "сохранение загружается");
            }
            if (f == 4) {
                check(s.screen() == Screen::Playing && g_.running(), "игра идёт после загрузки");
                at(100, 50, 0, "загружено сохранение с settings.music " + std::to_string(static_cast<int>(music_in_save)) +
                                   ": громкость игрока 50 осталась");
                check(num("settings.music") == 50, "переменная показывает громкость игрока, а не сохранения");
            }
            return f >= 6;
        }});
    }

    // games/examples/templates (13.12): a game made of the library's templates, as the editor saved it, read from
    // disk (the package carries it in data/examples). What the templates' buttons do in the game: the main menu's
    // «Настройки» opens the author's settings over it, whose buttons change the game's volume (Settings, the
    // mixer, settings.json); two copies of the game's own template «Находка» give their own things; the HUD's
    // pause button opens the author's pause, which stops the world, darkens, grows in and sounds its buttons; the
    // settings over it; «Продолжить» closes it and the world goes on; Esc is still the game's pause.
    // --scene volumes: the same game started again over the volumes a player saved (main writes them): music and
    // sounds at 0, all at 100.
    void build_templates(Shell& s) {
        SliceGame& g = g_;
        static const char* const kPages[] = {"шаблоны_меню", "шаблоны_игра", "шаблоны_пауза", "шаблоны_настройки"};
        static f64 x0 = 0, x1 = 0, torches = 0;
        static u32 clicks = 0;
        static std::filesystem::path kept;
        auto send = [&s, &g](const SDL_Event& e) {
            if (!s.handle_event(e) && s.screen() == Screen::Playing && !s.in_dialogue()) g.handle_event(e);
        };
        auto key = [send](SDL_Keycode k, bool down) {
            SDL_Event ev{};
            ev.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            ev.key.key = k;
            ev.key.down = down;
            send(ev);
        };
        auto press = [key](SDL_Keycode k) {
            key(k, true);
            key(k, false);
        };
        // The mouse at a point of a page's 1920 × 1080, where the page's fit shows it on the player's screen.
        auto at = [&s](f32 x, f32 y) {
            const Rml::Vector2i size = s.context()->GetDimensions();
            const game::ScreenFit fit = game::fit_screen("expand", 1920, 1080, static_cast<f32>(size.x), static_cast<f32>(size.y));
            return Rml::Vector2f(game::fit_to_view_x(fit, x), game::fit_to_view_y(fit, y));
        };
        // A button of a page (in the layer titled within, when two have its name).
        auto button = [&s](const char* page, const char* within, const char* title) -> Rml::Element* {
            Rml::ElementDocument* doc = s.screens().document(page);
            Rml::Element* scope = doc;
            if (within && doc) {
                Rml::ElementList all;
                doc->QuerySelectorAll(all, "[title]");
                scope = nullptr;
                for (Rml::Element* e : all)
                    if (e->GetAttribute<Rml::String>("title", "") == within) scope = e;
            }
            return titled(scope, title);
        };
        // Clicked in its middle, where the page's fit shows it.
        auto click = [&s, at, send, button](const char* page, const char* within, const char* title) {
            Rml::ElementDocument* doc = s.screens().document(page);
            Rml::Element* root = doc ? doc->GetElementById("n1") : nullptr;
            Rml::Element* e = button(page, within, title);
            if (!root || !e) return false;
            const Rml::Vector2f c = e->GetAbsoluteOffset(Rml::BoxArea::Border) - root->GetAbsoluteOffset(Rml::BoxArea::Border) +
                                    e->GetBox().GetSize(Rml::BoxArea::Border) * 0.5f;
            const Rml::Vector2f p = at(c.x, c.y);
            SDL_Event ev{};
            ev.type = SDL_EVENT_MOUSE_MOTION;
            ev.motion.x = p.x;
            ev.motion.y = p.y;
            send(ev);
            for (const SDL_EventType t : {SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
                ev = {};
                ev.type = t;
                ev.button.button = SDL_BUTTON_LEFT;
                ev.button.down = t == SDL_EVENT_MOUSE_BUTTON_DOWN;
                ev.button.x = p.x;
                ev.button.y = p.y;
                send(ev);
            }
            return true;
        };
        auto focus = [&s, button](const char* page, const char* within, const char* title) {
            Rml::Element* f = s.context()->GetFocusElement();
            return f && f == button(page, within, title);
        };
        auto num = [&s](const char* name) { return s.vars().get(name).number(); };
        // A window still growing in: its root's opacity is set while it moves (GameScreens::place).
        auto appearing = [&s](const char* page) {
            Rml::ElementDocument* doc = s.screens().document(page);
            Rml::Element* root = doc ? doc->GetElementById("n1") : nullptr;
            return root && root->GetLocalProperty("opacity") != nullptr;
        };
        // A window closed and still drawn while it goes away: its page shown, its veil too, marked forge-leaving (the
        // mouse and the keyboard already go to what is under it). Not the root's opacity: when the frame that starts
        // the going away reads a coarse clock in the same tick as the start (Windows' counts in 100 ns), the page is
        // drawn whole there and has none, though it is still going.
        auto leaving = [&s](const char* page) {
            Rml::ElementDocument* doc = s.screens().document(page);
            Rml::Element* veil = doc ? doc->GetElementById("forge-dim") : nullptr;
            return doc && doc->IsVisible() && doc->HasAttribute("forge-leaving") && (!veil || veil->IsVisible(true));
        };
        // The page the mouse is over now.
        auto hovered_in = [&s](const char* page) {
            Rml::Element* h = s.context()->GetHoverElement();
            return h && h->GetOwnerDocument() == s.screens().document(page);
        };
        auto near = [](f32 a, f32 b) { return std::fabs(a - b) < 1e-4f; };

        steps_.push_back({"пример шаблонов с диска", 20, [&s, &g, this](u32 f) {
            GameScreens& sc = s.screens();
            if (f == 0) {
                const std::filesystem::path dir = s.game_dir().parent_path() / "examples" / "templates";
                std::error_code ec;
                for (const char* name : kPages)
                    check(std::filesystem::is_regular_file(dir / "ui" / utf8_path(std::string(name) + ".html"), ec),
                          std::string("файлы примера на диске: ") + name + " в " + path_to_utf8(dir / "ui"));
                check(std::filesystem::is_regular_file(dir / "ui" / "templates" / utf8_path("находка.json"), ec) &&
                          std::filesystem::is_regular_file(dir / "sounds" / utf8_path("щелчок.wav"), ec) &&
                          std::filesystem::is_regular_file(dir / "pictures" / utf8_path("интерфейс/рамка_дерево.png"), ec),
                      "с ними свой шаблон, звук и картинки");
                FORGE_INFO("пример шаблонов читается из %s", path_to_utf8(dir / "ui").c_str());
                sc.load(s.context(), dir, false);
                const std::vector<std::string> names = sc.names();
                for (const char* name : kPages) {
                    Rml::ElementDocument* doc = sc.document(name);
                    std::string url = doc ? doc->GetSourceURL() : std::string();
                    std::replace(url.begin(), url.end(), '\\', '/');
                    check(std::find(names.begin(), names.end(), name) != names.end() &&
                              url.ends_with("templates/ui/" + std::string(name) + ".html"),
                          std::string("страница прочитана из файла: ") + name + " (" + url + ")");
                }
                check(names.size() == std::size(kPages), "других экранов нет (свои шаблоны игры не экраны): " + std::to_string(names.size()));
                audio::ScreenSounds& snd = g.sounds().screens();
                kept = snd.folder();
                snd.attach(&g.sounds().mixer(), dir / "sounds");
            }
            if (f < 3) return false;
            check(s.screen() == Screen::Main && sc.shown("шаблоны_меню"), "меню из шаблона вместо меню игры");
            check(sc.windows().empty(), "окон пока нет");
            return true;
        }});
        if (scene_ == "volumes") {
            build_volumes(s, click, num, near);
            return;
        }
        steps_.push_back({"настройки из шаблона над главным меню", 40, [&s, press, key, click, focus, num, appearing, near, this](u32 f) {
            GameScreens& sc = s.screens();
            if (f == 0) {
                check(near(s.settings().music_volume, 0.7f), "громкость музыки как у новой игры: " + std::to_string(s.settings().music_volume));
                check(num("settings.music") == 70, "и в переменной settings.music: " + std::to_string(num("settings.music")));
                check(click("шаблоны_меню", nullptr, "Кнопка «Настройки»"), "щелчок по «Настройки» меню");
            }
            if (f == 1) {
                check(sc.windows() == std::vector<std::string>{"шаблоны_настройки"} && s.screen() == Screen::Main && sc.shown("шаблоны_меню"),
                      "окно настроек открылось поверх главного меню, меню на месте");
                check(appearing("шаблоны_настройки"), "окно появляется (растёт)");
                SDL_Delay(300);
            }
            if (f == 3) {
                check(!appearing("шаблоны_настройки"), "окно появилось");
                check(click("шаблоны_настройки", "Строка «Музыка»", "Кнопка «+»"), "«+» у музыки");
            }
            if (f == 5) {
                check(near(s.settings().music_volume, 0.8f) && num("settings.music") == 80, "громкость музыки 80: игра её меняет");
                check(near(g_.sounds().mixer().volume(audio::Bus::Music), 0.8f), "микшер играет музыку тише: " +
                                                                                      std::to_string(g_.sounds().mixer().volume(audio::Bus::Music)));
                check(near(game::load_settings(s.user_folder()).music_volume, 0.8f), "и запоминает её (settings.json)");
                // The button pressed with the mouse has the keyboard (13.11): Enter presses it again.
                check(focus("шаблоны_настройки", "Строка «Музыка»", "Кнопка «+»"), "клавиатура у нажатой «+»");
                press(SDLK_RETURN);
            }
            if (f == 7) {
                check(num("settings.music") == 90 && near(s.settings().music_volume, 0.9f), "Enter: ещё +10");
                check(click("шаблоны_настройки", "Строка «Общая»", "Кнопка «−»"), "«−» у общей");
            }
            if (f == 9) {
                check(near(s.settings().master_volume, 0.9f) && near(g_.sounds().mixer().master(), 0.9f), "общая громкость 90, микшер тоже");
                key(SDLK_ESCAPE, true);
                key(SDLK_ESCAPE, false);
                check(sc.windows().empty() && s.screen() == Screen::Main && sc.shown("шаблоны_меню"), "Esc закрыл окно, меню осталось");
            }
            if (f == 11) {
                check(focus("шаблоны_меню", nullptr, "Кнопка «Настройки»"), "клавиатура вернулась к «Настройки» меню");
                check(click("шаблоны_меню", nullptr, "Кнопка «Новая игра»"), "щелчок по «Новая игра»");
            }
            if (f < 13) return false;
            return s.screen() == Screen::Playing && g_.running() && g_.hero_alive();
        }});
        steps_.push_back({"шаблоны в игре: находки, пауза, настройки", 140, [&s, &g, press, key, click, num, appearing, leaving, hovered_in, near,
                                                                          this](u32 f) {
            GameScreens& sc = s.screens();
            audio::ScreenSounds& snd = g.sounds().screens();
            auto right = [&g]() {
                Controls c;
                c.right = true;
                g.script(c);
            };
            const std::vector<std::string> pause{"шаблоны_пауза"};
            if (f == 0) {
                check(sc.shown("шаблоны_игра") && !sc.shown("шаблоны_меню") && sc.windows().empty(), "в игре экран из шаблона над игрой");
                check(num("inv.key") == 0 && num("inv.torch") == 5, "новая игра: ключа нет, факелов 5");
                torches = num("inv.torch");
            }
            if (f == 2) {
                // The template's texts show the game's values from the first frame: a new game's 0 coins, not «{inv.coins}».
                Rml::ElementDocument* doc = sc.document("шаблоны_игра");
                Rml::Element* coins = doc ? doc->QuerySelector("[title=Сколько]") : nullptr;
                Rml::Element* hearts = doc ? doc->QuerySelector("[title=Сердца]") : nullptr;
                check(coins && coins->GetInnerRML() == "0", "монет над игрой: 0 (" + (coins ? coins->GetInnerRML() : std::string("нет слоя")) + ")");
                check(hearts && hearts->GetInnerRML() == "3/3", "сердца над игрой: " + (hearts ? hearts->GetInnerRML() : std::string("нет слоя")));
            }
            if (f < 30 && !g.on_ground()) return false; // the hero lands first
            if (f == 30) {
                right();
                x0 = g.hero_x();
            }
            if (f == 40) {
                check(g.hero_x() > x0 + 0.1, "мир идёт: герой идёт вправо");
                check(click("шаблоны_игра", "Карточка «Ключ»", "Кнопка «Взять»"), "«Взять» у первой «Находки» («Ключ»)");
            }
            if (f == 42) {
                check(num("inv.key") == 1 && num("inv.torch") == torches, "она даёт ключ");
                check(click("шаблоны_игра", "Карточка «Факел»", "Кнопка «Взять»"), "«Взять» у второй, изменённой («Факел»)");
            }
            if (f == 44) {
                check(num("inv.torch") == torches + 1 && num("inv.key") == 1, "она даёт факел: копии шаблона не связаны");
                check(click("шаблоны_игра", nullptr, "Кнопка «Пауза»"), "кнопка паузы над игрой");
            }
            if (f == 45) {
                check(sc.windows() == pause && sc.pauses() && s.screen() == Screen::Playing, "открылась пауза из шаблона, игра стоит");
                Rml::ElementDocument* doc = sc.document("шаблоны_пауза");
                Rml::Element* veil = doc ? doc->GetElementById("forge-dim") : nullptr;
                check(veil && veil->IsVisible(true), "затемнение видно");
                check(appearing("шаблоны_пауза"), "окно появляется (растёт)");
                x1 = g.hero_x();
                SDL_Delay(300);
            }
            if (f == 50) {
                check(!appearing("шаблоны_пауза"), "окно появилось");
                check(std::fabs(g.hero_x() - x1) < 1e-9, "мир стоит, хотя «вправо» держат");
                check(click("шаблоны_пауза", nullptr, "Кнопка «Настройки»"), "«Настройки» паузы");
            }
            if (f == 51) {
                check(sc.windows() == std::vector<std::string>{"шаблоны_пауза", "шаблоны_настройки"}, "настройки открылись над паузой");
                SDL_Delay(300);
            }
            if (f == 53) check(click("шаблоны_настройки", "Строка «Звуки»", "Кнопка «−»"), "«−» у звуков");
            if (f == 55) {
                check(near(s.settings().sound_volume, 0.9f) && num("settings.sound") == 90, "громкость звуков 90");
                check(near(g.sounds().mixer().volume(audio::Bus::Sound), 0.9f) && near(g.sounds().mixer().volume(audio::Bus::Ui), 0.9f),
                      "микшер: звуки мира и кнопок тише");
                check(click("шаблоны_настройки", nullptr, "Кнопка «Готово»"), "«Готово»");
            }
            // Closed, a window is still drawn while it goes away; the mouse goes to what is under it at once. The click
            // comes on the first frame after the closing one: the going away has started (in that frame's update) and,
            // however slow the frames, has not ended (only a later update ends it).
            if (f == 56) {
                check(sc.windows() == pause && sc.pauses(), "«Готово» закрыл настройки, пауза осталась");
                check(leaving("шаблоны_настройки"), "настройки ещё уходят: видны, с затемнением");
                clicks = snd.clicks();
                check(click("шаблоны_пауза", nullptr, "Кнопка «Продолжить»"), "«Продолжить» сразу, пока настройки уходят");
                check(hovered_in("шаблоны_пауза"), "мышь над паузой, а не над уходящими настройками");
            }
            if (f == 57) {
                check(snd.clicks() == clicks + 1 && snd.last_click() == "щелчок.wav", "кнопка паузы звучит звуком экрана: " + snd.last_click());
                check(sc.windows().empty() && !sc.pauses() && s.screen() == Screen::Playing, "«Продолжить» закрыл паузу из шаблона");
                check(leaving("шаблоны_пауза"), "пауза ещё уходит: видна, с затемнением");
                torches = num("inv.torch");
                check(click("шаблоны_игра", "Карточка «Факел»", "Кнопка «Взять»"), "«Взять» сразу, пока пауза уходит");
                check(hovered_in("шаблоны_игра"), "мышь над карточкой, а не над уходящей паузой");
            }
            if (f == 58) {
                check(num("inv.torch") == torches + 1, "щелчок дошёл до карточки, а не до уходящей паузы");
                SDL_Delay(350);
            }
            if (f == 70) {
                check(g.hero_x() > x1 + 0.1, "мир снова идёт");
                // Esc over the game is still the game's own pause (13.11).
                key(SDLK_ESCAPE, true);
                key(SDLK_ESCAPE, false);
                check(s.screen() == Screen::Paused, "Esc: пауза самой игры");
                press(SDLK_ESCAPE);
                check(s.screen() == Screen::Playing, "Esc ещё раз — обратно в игру");
            }
            if (f == 72) {
                g.select(0);
                snd.attach(&g.sounds().mixer(), kept);
                sc.load(s.context(), s.game_dir(), false);
                return true;
            }
            return false;
        }});
    }

    // The level editor's «Физика» as the game plays it: games/examples/physics/level, made in the editor with the
    // mouse (its self-test makes it again and compares), read from the files the way «Играть отсюда» starts it
    // (in a package from the package's own data/examples). An old level keeps the game's pull; the example's
    // world pull, gravity point, water and sand are the author's; a body is pulled by the point, the water and
    // the sand fall along the world's pull (also when it goes left, in a copy); the example's files stay as they were.
    void build_physics(Shell& s) {
        SliceGame& g = g_;
        struct State {
            std::filesystem::path dir, left;
            std::vector<std::pair<std::string, std::vector<u8>>> files;
            flecs::entity_t inside = 0, outside = 0, coins = 0, far = 0;
            f64 coins_x = 0, water_x = 0, water_y = 0, sand_x = 0, sand_y = 0;
            u64 id = 0;
        };
        auto st = std::make_shared<State>();
        const i32 v = g.generator().village_y();
        const f64 cx = 68.5, cy = v - 12.5; // the example's point: radius 10, strength 60
        // Every file of a folder with its bytes, by name.
        auto files_of = [](const std::filesystem::path& dir) {
            std::vector<std::pair<std::string, std::vector<u8>>> out;
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
                if (!e.is_regular_file()) continue;
                std::vector<u8> bytes;
                read_file(e.path(), bytes);
                out.emplace_back(path_to_utf8(e.path().filename()), std::move(bytes));
            }
            std::sort(out.begin(), out.end());
            return out;
        };
        // Water (in full cells) or sand in a rectangle of tiles (both ends in), and where it is on average.
        auto water = [&g](i32 x0, i32 y0, i32 x1, i32 y1, f64* mx = nullptr, f64* my = nullptr) {
            f64 n = 0, sx = 0, sy = 0;
            for (i32 y = y0; y <= y1; ++y)
                for (i32 x = x0; x <= x1; ++x) {
                    const world::TileId t = g.tile(kLiquids, x, y);
                    if (forge::sim::liquid_kind(t) != kWater) continue;
                    const f64 a = forge::sim::liquid_amount(t) / static_cast<f64>(forge::sim::kFull);
                    n += a;
                    sx += a * x;
                    sy += a * y;
                }
            if (mx) *mx = n > 0 ? sx / n : std::nan("");
            if (my) *my = n > 0 ? sy / n : std::nan("");
            return n;
        };
        auto sand = [&g](i32 x0, i32 y0, i32 x1, i32 y1, f64* mx = nullptr, f64* my = nullptr) {
            u32 n = 0;
            f64 sx = 0, sy = 0;
            for (i32 y = y0; y <= y1; ++y)
                for (i32 x = x0; x <= x1; ++x)
                    if (g.tile(kBlocks, x, y) == world::TileSand) {
                        ++n;
                        sx += x;
                        sy += y;
                    }
            if (mx) *mx = n ? sx / n : std::nan("");
            if (my) *my = n ? sy / n : std::nan("");
            return n;
        };
        auto pull = [&g](f64 x, f64 y, f32 ex, f32 ey) {
            f32 gx = 0, gy = 0;
            g.pull_at(x, y, gx, gy);
            return std::fabs(gx - ex) < 1e-4f && std::fabs(gy - ey) < 1e-4f;
        };
        // The pull a probe felt in its last tick, near (ex, ey) (it moves meanwhile, and the way to a centre turns
        // with it); what it felt goes into what.
        auto felt = [&g](flecs::entity_t e, f32 ex, f32 ey, f32 near, std::string& what) {
            f64 x = 0, y = 0;
            f32 gx = 0, gy = 0;
            const bool ok = g.probe(e, x, y, gx, gy);
            char t[96];
            std::snprintf(t, sizeof(t), "%.2f, %.2f у тела в %.2f, %.2f", static_cast<f64>(gx), static_cast<f64>(gy), x, y);
            what = t;
            return ok && std::fabs(gx - ex) <= near && std::fabs(gy - ey) <= near;
        };

        steps_.push_back({"меню", 30, [&s, &g, this](u32 f) {
            if (f < 5) return false;
            g.set_level({});
            check(s.new_game(), "новая игра со своего уровня игры");
            return true;
        }});
        steps_.push_back({"старый уровень: тянет вниз, как в игре", 20, [&s, &g, felt, this, st](u32 f) {
            if (f == 0) {
                std::error_code ec;
                check(!std::filesystem::exists(s.game_dir() / "level" / "physics.json", ec), "у уровня игры нет physics.json");
                f32 gx = 0, gy = 0;
                g.world_gravity(gx, gy);
                check(gx == 0 && gy == kGravity, "гравитация мира 0, 40: " + std::to_string(gx) + ", " + std::to_string(gy));
                st->far = g.spawn_probe(g.hero_x(), g.hero_y() - 6);
                check(st->far != 0, "тело-проба в воздухе");
            }
            if (f < 5) return false;
            std::string what;
            const bool ok = felt(st->far, 0, kGravity, 0, what);
            check(ok, "тело чувствует только мировую: " + what);
            check(g.gravity_sources() == 0 && g.points().empty(), "точек гравитации нет");
            return true;
        }});
        steps_.push_back({"пример «Физика» из файлов", 140, [&s, &g, cx, cy, v, files_of, water, sand, pull, felt, this, st](u32 f) {
            if (f == 0) {
                st->dir = s.game_dir().parent_path() / "examples" / "physics" / "level";
                std::error_code ec;
                FORGE_INFO("пример «Физика» читается из %s", path_to_utf8(st->dir).c_str());
                check(std::filesystem::is_regular_file(st->dir / "physics.json", ec), "physics.json примера на диске: " + path_to_utf8(st->dir));
                st->files = files_of(st->dir);
                check(st->files.size() > 2, "с ним файлы участков: " + std::to_string(st->files.size()));
                forge::level::LevelPhysics p{0, kGravity};
                check(forge::level::load_physics(st->dir, p) && p == forge::level::LevelPhysics{0, 30}, "в файле: вниз 30");
                // «Играть отсюда»: a new game from the level folder, the hero where the author asked.
                g.set_level(st->dir, true, 58.5, v);
                check(s.new_game(), "новая игра с уровня примера");
                f32 gx = 0, gy = 0;
                g.world_gravity(gx, gy);
                check(gx == 0 && gy == 30, "игра тянет вниз с силой 30: " + std::to_string(gx) + ", " + std::to_string(gy));
                const std::vector<SliceGame::Point> pts = g.points();
                check(pts.size() == 1, "одна точка гравитации: " + std::to_string(pts.size()));
                if (!pts.empty()) {
                    const SliceGame::Point& pt = pts[0];
                    st->id = pt.id;
                    check(pt.id != 0 && pt.x == cx && pt.y == cy && pt.source.radius == 10 && pt.source.strength == 60 &&
                              pt.source.toward_center && !pt.source.fade && !pt.source.replace,
                          "точка автора: свой id, центр 68.5, v-12.5, радиус 10, сила 60");
                }
                check(water(40, v - 9, 46, v - 6) == 28 && water(48, v - 1, 50, v - 1) == 3, "вода автора в воздухе (28) и у земли (3)");
                check(sand(52, v - 12, 55, v - 10) == 12, "песок автора в воздухе (12)");
                st->coins = g.nearest_item(64.5, cy, 1.5);
                f64 y = 0;
                check(st->coins != 0 && g.position_of(st->coins, st->coins_x, y), "монеты автора у точки");
                st->inside = g.spawn_probe(cx + 4, cy);
                st->outside = g.spawn_probe(cx + 12, cy);
            }
            if (f == 3) {
                check(g.gravity_sources() == 1, "симуляция нашла точку");
                // GravityField's rules: in the centre only the world's pull, inside (the circle too) the point's
                // strength toward the centre added to it, outside only the world's.
                check(pull(cx, cy, 0, 30), "в самом центре: только мировая");
                check(pull(cx + 6, cy, -60, 30) && pull(cx, cy - 6, 0, 30 + 60), "внутри: 60 к центру плюс мировая");
                check(pull(cx + 10, cy, -60, 30), "на окружности ещё действует");
                check(pull(cx + 10.25, cy, 0, 30), "за ней только мировая");
                std::string what;
                bool ok = felt(st->inside, -60, 30, 1, what);
                check(ok, "тело внутри: 60 к центру плюс мировая: " + what);
                ok = felt(st->outside, 0, 30, 0, what);
                check(ok, "тело снаружи: только мировая: " + what);
            }
            if (f == 30) {
                f64 x = 0, y = 0, xc = 0, yc = 0;
                const bool probe = g.position_of(st->inside, x, y), coins = g.position_of(st->coins, xc, yc);
                check(probe && x < cx + 3.5, "тело внутри летит к центру: x " + std::to_string(x));
                check(coins && xc > st->coins_x + 0.3, "монеты тянет к центру: x " + std::to_string(st->coins_x) + " → " + std::to_string(xc));
            }
            if (f < 130) return false;
            f64 wy = 0;
            const f64 all = water(-64, v - 30, 127, v + 8, nullptr, &wy), below = water(-64, v - 4, 127, v + 8);
            // A thin film on a floor dries up as the water spreads (CellSim): most of the 31 cells stay.
            check(water(40, v - 9, 46, v - 6) < 0.5 && all > 25 && below > all - 0.5,
                  "вода упала на землю: из " + std::to_string(all) + " у земли " + std::to_string(below) + ", в среднем y " +
                      std::to_string(wy));
            check(sand(52, v - 12, 55, v - 10) == 0 && sand(44, v - 4, 62, v - 1) == 12, "песок упал, весь");
            return true;
        }});
        steps_.push_back({"сохранение игры: файлы примера не тронуты", 10, [&s, &g, files_of, this, st](u32 f) {
            if (f == 0) {
                check(s.save("physics", "Физика"), "игра сохраняется");
                std::vector<u8> mine, author;
                read_file(s.slots().folder("physics") / "world" / "physics.json", mine);
                read_file(st->dir / "physics.json", author);
                check(!mine.empty() && mine == author, "в сохранении physics.json автора");
                check(files_of(st->dir) == st->files, "файлы примера как были");
                check(s.load("physics"), "сохранение загружается");
            }
            if (f < 3) return false;
            f32 gx = 0, gy = 0;
            g.world_gravity(gx, gy);
            const std::vector<SliceGame::Point> pts = g.points();
            check(gx == 0 && gy == 30 && pts.size() == 1 && pts[0].id == st->id, "загруженная игра: вниз 30 и та же точка");
            return true;
        }});
        steps_.push_back({"сила мира влево: вода и песок туда же", 80, [&s, &g, v, water, sand, felt, this, st](u32 f) {
            if (f == 0) {
                st->left = std::filesystem::temp_directory_path() / "forge_slice_physics_left";
                std::error_code ec;
                std::filesystem::remove_all(st->left, ec);
                std::string why;
                check(forge::level::copy_level(st->dir, st->left, &why) &&
                          forge::level::save_physics(st->left, {-30, 0}, &why),
                      "копия примера с силой мира влево " + path_to_utf8(st->left) + why);
                g.set_level(st->left, true, 58.5, v);
                check(s.new_game(), "новая игра с копии");
                f32 gx = 0, gy = 0;
                g.world_gravity(gx, gy);
                check(gx == -30 && gy == 0, "игра тянет влево: " + std::to_string(gx) + ", " + std::to_string(gy));
                water(-128, v - 30, 127, v + 8, &st->water_x, &st->water_y);
                sand(-128, v - 30, 127, v + 8, &st->sand_x, &st->sand_y);
                st->far = g.spawn_probe(30.5, v - 20);
            }
            if (f == 3) {
                std::string what;
                const bool ok = felt(st->far, -30, 0, 0, what);
                check(ok, "тело чувствует мировую влево: " + what);
            }
            if (f < 60) return false;
            f64 wx = 0, wy = 0, sx = 0, sy = 0;
            water(-128, v - 30, 127, v + 8, &wx, &wy);
            const u32 n = sand(-128, v - 30, 127, v + 8, &sx, &sy);
            check(n == 12, "песок весь на месте: " + std::to_string(n));
            check(wx < st->water_x - 2 && std::fabs(wy - st->water_y) < 2,
                  "вода течёт влево, а не вниз: x " + std::to_string(st->water_x) + " → " + std::to_string(wx) + ", y " +
                      std::to_string(st->water_y) + " → " + std::to_string(wy));
            check(sx < st->sand_x - 2 && std::fabs(sy - st->sand_y) < 2,
                  "песок сыплется влево, а не вниз: x " + std::to_string(st->sand_x) + " → " + std::to_string(sx) + ", y " +
                      std::to_string(st->sand_y) + " → " + std::to_string(sy));
            return true;
        }});
        steps_.push_back({"обратно в меню", 5, [&s, &g, files_of, this, st](u32 f) {
            if (f == 0) {
                s.to_main_menu();
                g.set_level({});
                check(files_of(st->dir) == st->files, "файлы примера как были");
                std::error_code ec;
                std::filesystem::remove_all(st->left, ec);
            }
            return f >= 2;
        }});
    }

    // The level editor's «Зоны» as the game plays them: games/examples/zones, made in the editor with the mouse, the
    // panel and «Логика» (its self-test makes it again and compares), read from the files the way a new game and
    // «Играть отсюда» start it (in a package from the package's own data/examples). An old level has no areas. The
    // example's spawn point puts the hero east of the mine's entrance. Walking in, the hero comes into «Шахта» once,
    // at its right edge x = -149 (the edge is not in it): the link «Герой входит в Шахту» («Только один раз») shows the
    // place's name and starts «Потерянная кирка» in the HUD. Staying in it, a chunk border, the chamber «Дальний зал»
    // inside it say nothing more of «Шахта»; leaving and coming back does not run the once link again, nor after a
    // save is loaded (a load is not a coming in); «Играть отсюда» inside it comes in at the first step. The place's
    // music: шахта.wav from the example's sounds/, started once, kept in the chamber, stopped outside, under a
    // window's music and over a screen's over the game, through a pause; at master or music 0 not heard; a file that
    // is not there said once. A spawn point in rock or out of the world, a broken areas.json; the example's files
    // stay as they were.
    void build_zones(Shell& s) {
        SliceGame& g = g_;
        struct State {
            std::filesystem::path dir, level, copy, sounds;
            std::vector<std::pair<std::string, std::vector<u8>>> files;
            forge::level::LevelAreas areas;
            u64 mine = 0, hall = 0;
            u32 mine_link = 0, hall_link = 0;
            std::filesystem::path kept; // the screens' sounds folder of the game
            u32 starts = 0, frames = 0, exact = 0;
            f64 quest = 0; // quest.pickaxe when saved
            audio::Voice voice;
            Settings settings;
            f64 x = 0, y = 0;  // the hero after the last frame
            bool border = false; // a chunk border crossed inside «Шахта»
        };
        auto st = std::make_shared<State>();
        const SliceGenerator& gen = g.generator();
        const i32 sy = gen.mine_y(), gy = gen.gallery_y(), mx = gen.mine_x();
        // On the stairs (their floor at x is the row sy + (mx - x) + 1), in the gallery, in the chamber, east of the
        // entrance on the ground: tile points under the hero's feet.
        const f64 stairs_x = mx - 40.5, stairs_y = sy + 41 + 1;
        const f64 gallery_x = gen.gallery_x1() + 25.5, room_x = gen.gallery_x1() - 6.5, floor_y = gy + 1;
        const f64 east_x = mx + 9.5;
        // Every file of a folder and under it with its bytes, by its path in it.
        auto files_of = [](const std::filesystem::path& dir) {
            std::vector<std::pair<std::string, std::vector<u8>>> out;
            std::error_code ec;
            for (const auto& e : std::filesystem::recursive_directory_iterator(dir, ec)) {
                if (!e.is_regular_file()) continue;
                std::vector<u8> bytes;
                read_file(e.path(), bytes);
                out.emplace_back(path_to_utf8(std::filesystem::relative(e.path(), dir)), std::move(bytes));
            }
            std::sort(out.begin(), out.end());
            return out;
        };
        auto in = [&g](u64 id) {
            const std::vector<u64> now = g.areas_inside();
            return std::find(now.begin(), now.end(), id) != now.end();
        };
        // Where the hero is, and where it stands: the centre over the feet.
        auto put = [&g](f64 feet_x, f64 feet_y) { g.teleport(feet_x, feet_y - kHeroHalfH); };
        auto var = [&s](const char* name) { return s.vars().get(name).number(); };
        auto hud = [&s] {
            Rml::Element* t = s.find_element("tracker");
            return t && t->IsVisible(true) ? std::string(t->GetInnerRML()) : std::string();
        };
        // The place's music heard: its peak on the music bus (the other buses back as the settings say).
        auto heard = [&s, &g] {
            const f32 p = music_peak(g.sounds().mixer());
            s.apply_settings(s.settings());
            return p;
        };

        steps_.push_back({"меню", 30, [&s, &g, this](u32 f) {
            if (f < 5) return false;
            g.set_level({});
            check(s.new_game(), "новая игра со своего уровня игры");
            return true;
        }});
        steps_.push_back({"старый уровень: зон нет", 10, [&s, &g, &gen, this](u32 f) {
            if (f < 3) return false;
            std::error_code ec;
            check(!std::filesystem::exists(s.game_dir() / "level" / "areas.json", ec), "у уровня игры нет areas.json");
            check(g.areas() && g.areas()->areas.empty() && !g.areas()->spawn && g.areas_inside().empty(), "зон и точки появления нет");
            check(g.hero_x() == gen.spawn_x(), "герой на старте игры: x " + std::to_string(g.hero_x()));
            check(s.screens().place_music().empty() && g.sounds().screens().music_name().empty(), "музыки места нет");
            return true;
        }});
        steps_.push_back({"пример «Зоны» из файлов: новая игра у точки появления", 20,
                          [&s, &g, &gen, sy, gy, mx, files_of, in, var, this, st](u32 f) {
            if (f == 0) {
                st->dir = s.game_dir().parent_path() / "examples" / "zones";
                st->level = st->dir / "level";
                std::error_code ec;
                FORGE_INFO("пример «Зоны» читается из %s", path_to_utf8(st->dir).c_str());
                check(std::filesystem::is_regular_file(st->level / "areas.json", ec), "areas.json примера на диске: " + path_to_utf8(st->level));
                st->files = files_of(st->dir);
                check(st->files.size() == 4, "в примере 4 файла (README, logic.json, areas.json, музыка): " + std::to_string(st->files.size()));
                std::string why;
                check(forge::level::load_areas(st->level, st->areas, nullptr, &why) && st->areas.areas.size() == 2,
                      "в файле две зоны " + why);
                for (const forge::level::Area& a : st->areas.areas) {
                    if (a.name == "Шахта") st->mine = a.id;
                    if (a.name == "Дальний зал") st->hall = a.id;
                }
                const forge::level::Area* mine = st->areas.find(st->mine);
                const forge::level::Area* hall = st->areas.find(st->hall);
                check(mine && mine->x0 == mx - 153 && mine->x1 == mx + 1 && mine->y0 == sy - 5 && mine->y1 == gy + 3 &&
                          mine->music == "шахта.wav",
                      "«Шахта»: x -303…-149, от входа до пола штольни, музыка шахта.wav");
                check(hall && hall->x0 == mx - 153 && hall->x1 == gen.gallery_x1() + 2 && hall->y0 == gy - 8 && hall->y1 == gy + 2 &&
                          hall->music.empty() && st->hall != st->mine,
                      "«Дальний зал» внутри неё, без своей музыки, со своим id");
                check(st->areas.spawn && st->areas.spawn_x == mx + 9.5 && st->areas.spawn_y == sy + 3,
                      "точка появления на земле в 9 клетках к востоку от входа: " + std::to_string(st->areas.spawn_y));
                // The example's links: the game's, and the two of its areas.
                logic::Logic links;
                check(links.load(st->dir / "logic.json", &why), "связи примера читаются " + why);
                for (const logic::Link& l : links.links) {
                    if (l.a == "hero" && l.verb == "enter" && l.b == area_thing_id(st->mine)) st->mine_link = l.once ? l.id : 0;
                    if (l.a == "hero" && l.verb == "enter" && l.b == area_thing_id(st->hall)) st->hall_link = l.once ? 0 : l.id;
                }
                check(st->mine_link && st->hall_link, "«Герой входит в Шахту» (один раз) и «Герой входит в Дальний зал»");
                // The game reads them as its links (the run's links file), and the screens' sounds from the example.
                const std::filesystem::path file = std::filesystem::temp_directory_path() / "forge_slice_test_logic.json";
                std::filesystem::copy_file(st->dir / "logic.json", file, std::filesystem::copy_options::overwrite_existing, ec);
                check(!ec && g.reload_links(&why), "связи примера — связи игры " + why);
                audio::ScreenSounds& snd = g.sounds().screens();
                st->kept = snd.folder();
                snd.attach(&g.sounds().mixer(), st->dir / "sounds");
                // A new game from the level folder: the hero at its spawn point.
                g.set_level(st->level);
                check(s.new_game(), "новая игра с уровня примера");
                check(g.hero_x() == st->areas.spawn_x && g.hero_y() == scene::Position::at_tile(0, st->areas.spawn_y - kHeroHalfH).tile_y(),
                      "герой в точке появления: " + std::to_string(g.hero_x()) + ", " + std::to_string(g.hero_y()));
                check(g.areas() && *g.areas() == st->areas, "в игре зоны из файла");
                check(var("quest.pickaxe") == 0, "задание ещё не взято");
            }
            if (f < 10) return false;
            check(g.areas_inside().empty() && g.area_enters(st->mine) == 0 && !in(st->mine), "у входа герой ни в одной зоне");
            check(s.screens().place_music().empty() && g.sounds().screens().music_name().empty(), "музыки места нет");
            st->starts = g.sounds().screens().music_starts();
            return true;
        }});
        // Walking west into the mine and down the stairs: each frame of one tick, the hero is in «Шахта» as the
        // point where the frame before left it says (the areas look at the hero before it moves in a tick).
        steps_.push_back({"пешком в «Шахту»: вход один раз, на краю x = -149", 900, [&s, &g, mx, sy, gy, in, var, hud, heard, this, st](u32 f) {
            const forge::level::Area* mine = st->areas.find(st->mine);
            if (!mine) return true;
            if (f == 0) {
                st->x = g.hero_x();
                st->y = g.hero_y();
                st->frames = st->exact = 0;
            } else {
                const forge::sim::SimStats* stats = g.sim_stats();
                const bool one = stats && stats->ticks == 1;
                ++st->frames;
                if (one) {
                    ++st->exact;
                    check(in(st->mine) == mine->contains(st->x, st->y),
                          "в «Шахте» ровно тогда, когда в ней точка героя: x " + std::to_string(st->x) + ", y " + std::to_string(st->y));
                }
                if (std::floor(st->x / 64) != std::floor(g.hero_x() / 64) && in(st->mine) && mine->contains(st->x, st->y))
                    st->border = true;
                st->x = g.hero_x();
                st->y = g.hero_y();
            }
            check(g.area_enters(st->mine) <= 1 && g.area_leaves(st->mine) == 0, "в «Шахту» входят один раз и не выходят");
            if (g.hero_x() > mx - 60.0) {
                Controls c;
                c.left = true;
                g.script(c);
                return false;
            }
            g.stop_script();
            FORGE_INFO("пешком от %.1f до %.1f: кадров %u, из них с одним шагом мира (проверены точно) %u", mx + 9.5, g.hero_x(), st->frames,
                       st->exact);
            check(st->exact * 10 >= st->frames * 9, "почти все кадры с одним шагом мира");
            check(in(st->mine) && !in(st->hall) && g.area_enters(st->mine) == 1, "герой на лестнице в «Шахте», вошёл один раз");
            check(st->border, "граница участков x = -192 пройдена внутри «Шахты» без выхода и входа");
            check(g.last_hint() == "Шахта", "игра показала название места: " + g.last_hint());
            check(var("quest.pickaxe") == 1, "связь задала quest.pickaxe = 1");
            check(hud().find("Потерянная кирка") != std::string::npos, "в HUD задание «Потерянная кирка»: " + hud());
            audio::ScreenSounds& snd = g.sounds().screens();
            check(s.screens().place_music() == "шахта.wav" && snd.music_name() == "шахта.wav" && snd.music_playing() &&
                      snd.music_starts() == st->starts + 1 && g.sounds().mixer().voices(audio::Bus::Music) == 1,
                  "играет шахта.wav из папки примера, начата один раз: " + snd.music_name());
            check(heard() > 0.05f, "музыку места слышно: в смеси есть звук");
            st->voice = snd.music_voice();
            (void)sy;
            (void)gy;
            return true;
        }});
        steps_.push_back({"край зоны: x = -149 не в ней, левее — в ней; повторный вход не повторяет «один раз»", 40,
                          [&s, &g, mx, sy, in, put, var, this, st](u32 f) {
            // Standing still on the ground over the entrance: the hero's point stays where it was put.
            if (f == 0) put(mx + 1.0, sy);
            if (f == 10) {
                check(g.hero_x() == mx + 1.0 && !in(st->mine) && g.area_leaves(st->mine) == 1,
                      "в x = -149 (правый край) герой уже не в «Шахте»: x " + std::to_string(g.hero_x()));
                check(s.screens().place_music().empty() && g.sounds().screens().music_name().empty() &&
                          g.sounds().mixer().voices(audio::Bus::Music) == 0,
                      "вне зоны музыка места остановлена");
                s.vars().set("quest.pickaxe", 0); // so a second run of the once link would show
                put(mx + 0.75, sy);
            }
            if (f < 20) return false;
            check(g.hero_x() == mx + 0.75 && in(st->mine) && g.area_enters(st->mine) == 2, "в x = -149.25 герой снова в «Шахте»: вход второй");
            check(var("quest.pickaxe") == 0, "связь «Только один раз» второй раз не сработала");
            check(g.sounds().screens().music_name() == "шахта.wav" && g.sounds().screens().music_starts() == st->starts + 2,
                  "музыка места начата снова, один раз");
            s.vars().set("quest.pickaxe", 1);
            return true;
        }});
        steps_.push_back({"«Дальний зал» внутри «Шахты»: свой вход, музыка шахты не начинается заново", 40,
                          [&s, &g, room_x, gallery_x, floor_y, in, put, this, st](u32 f) {
            audio::ScreenSounds& snd = g.sounds().screens();
            if (f == 0) {
                st->voice = snd.music_voice();
                put(room_x, floor_y);
            }
            if (f == 10) {
                const std::vector<u64> now = g.areas_inside();
                check(now == std::vector<u64>{st->mine, st->hall}, "герой в обеих зонах, по порядку списка");
                check(g.area_enters(st->hall) == 1 && g.area_enters(st->mine) == 2 && g.area_leaves(st->mine) == 1,
                      "вход только в «Дальний зал»: из «Шахты» герой не выходил");
                check(g.last_hint() == "Дальний зал", "игра показала «Дальний зал»: " + g.last_hint());
                check(snd.music_name() == "шахта.wav" && snd.music_voice() == st->voice && snd.music_playing() &&
                          snd.music_starts() == st->starts + 2,
                      "у зала своей музыки нет: играет та же музыка шахты, не заново");
                put(gallery_x, floor_y);
            }
            if (f < 20) return false;
            check(in(st->mine) && !in(st->hall) && g.area_leaves(st->hall) == 1 && g.area_enters(st->mine) == 2,
                  "из зала в штольню: выход из «Дальнего зала», «Шахта» та же");
            check(snd.music_voice() == st->voice && snd.music_starts() == st->starts + 2, "музыка та же");
            return true;
        }});
        steps_.push_back({"по штольне через границу участков x = -256", 200, [&g, in, this, st](u32 f) {
            if (f == 0) st->x = g.hero_x();
            if (g.hero_x() < -253.0 && f < 190) {
                Controls c;
                c.right = true;
                g.script(c);
                return false;
            }
            g.stop_script();
            check(st->x < -256 && g.hero_x() > -256 && in(st->mine), "граница пройдена: x " + std::to_string(st->x) + " → " + std::to_string(g.hero_x()));
            check(g.area_enters(st->mine) == 2 && g.area_leaves(st->mine) == 1 && g.area_enters(st->hall) == 1, "ни входа, ни выхода");
            check(g.sounds().screens().music_starts() == st->starts + 2, "музыка не начиналась заново");
            return true;
        }});
        steps_.push_back({"далеко и обратно: участки шахты выгружены, зоны те же", 60, [&g, mx, sy, stairs_x, stairs_y, in, put, this, st](u32 f) {
            if (f == 0) put(3000.5, g.generator().surface(3000));
            if (f == 20) {
                check(g.world() && g.world()->find_chunk(world::chunk_of(static_cast<i32>(stairs_x), static_cast<i32>(stairs_y))) == nullptr,
                      "участки шахты выгружены");
                check(g.areas() && *g.areas() == st->areas && g.areas_inside().empty() && g.area_leaves(st->mine) == 2,
                      "зоны те же, герой вышел из «Шахты»");
                put(stairs_x, stairs_y);
            }
            if (f < 30) return false;
            check(in(st->mine) && g.area_enters(st->mine) == 3 && g.areas() && *g.areas() == st->areas, "обратно на лестницу: снова в «Шахте»");
            (void)mx;
            (void)sy;
            return true;
        }});
        steps_.push_back({"сохранение и загрузка: загрузка — не вход, «один раз» помнится", 60,
                          [&s, &g, mx, sy, in, put, var, files_of, this, st](u32 f) {
            if (f == 0) {
                // The chamber's pickaxe was picked up on the way (quest.pickaxe 2, «несёт кирку»): the game's own quest.
                st->quest = var("quest.pickaxe");
                check(st->quest >= 1 && s.save("zones", "Зоны"), "игра сохраняется в «Шахте», задание взято");
                std::vector<u8> mine, author;
                read_file(s.slots().folder("zones") / "world" / "areas.json", mine);
                read_file(st->level / "areas.json", author);
                check(!mine.empty() && mine == author, "в сохранении areas.json автора");
                check(files_of(st->dir) == st->files, "файлы примера как были");
                check(s.load("zones"), "сохранение загружается");
            }
            if (f == 10) {
                check(in(st->mine) && g.area_enters(st->mine) == 0 && g.area_leaves(st->mine) == 0,
                      "загруженная игра: герой в «Шахте», входа не было");
                check(var("quest.pickaxe") == st->quest, "задание как при сохранении: quest.pickaxe = " + std::to_string(var("quest.pickaxe")));
                check(g.sounds().screens().music_name() == "шахта.wav" && g.sounds().screens().music_playing(), "музыка места играет");
                put(mx + 9.5, sy);
            }
            if (f == 20) {
                check(!in(st->mine) && g.area_leaves(st->mine) == 1, "вышел");
                s.vars().set("quest.pickaxe", 0);
                put(mx + 0.75, sy);
            }
            if (f < 30) return false;
            check(in(st->mine) && g.area_enters(st->mine) == 1 && var("quest.pickaxe") == 0,
                  "вошёл снова: связь «Только один раз» после загрузки не сработала");
            s.vars().set("quest.pickaxe", st->quest);
            return true;
        }});
        steps_.push_back({"«Играть отсюда» в «Шахте»: вход в первом шаге", 20, [&s, &g, stairs_x, stairs_y, in, var, hud, this, st](u32 f) {
            if (f == 0) {
                g.set_level(st->level, true, stairs_x, stairs_y);
                check(s.new_game(), "новая игра с уровня примера, герой на лестнице");
                check(g.hero_x() == stairs_x && var("quest.pickaxe") == 0, "герой там, задание не взято");
            }
            if (f < 5) return false;
            check(in(st->mine) && g.area_enters(st->mine) == 1, "новая игра в зоне — это вход");
            check(var("quest.pickaxe") == 1 && g.last_hint() == "Шахта", "связь сработала: «Шахта», quest.pickaxe = 1");
            check(hud().find("Потерянная кирка") != std::string::npos, "в HUD задание: " + hud());
            check(g.sounds().screens().music_name() == "шахта.wav" && g.sounds().screens().music_playing(), "играет музыка места");
            return true;
        }});
        // The order of the musics: a window's (by a button) over the place's, the place's over a screen's over the
        // game; a pause keeps it. The screens are those of games/examples/screen-sound, their sounds made here.
        steps_.push_back({"музыка места среди музыки экранов", 60, [&s, &g, east_x, stairs_x, stairs_y, sy, put, heard, this, st](u32 f) {
            GameScreens& sc = s.screens();
            audio::ScreenSounds& snd = g.sounds().screens();
            audio::Mixer& mixer = g.sounds().mixer();
            if (f == 0) {
                st->sounds = sound_files();
                std::error_code ec;
                std::filesystem::copy_file(st->dir / "sounds" / utf8_path("шахта.wav"), st->sounds / utf8_path("шахта.wav"), ec);
                check(!ec, "шахта.wav рядом со звуками экранов");
                snd.attach(&mixer, st->sounds);
                for (const SimplePage& p : kSoundPages)
                    if (std::string(p.name) != "звук_меню")
                        check(sc.load_page(s.context(), p.name, p.html, path_to_utf8(s.game_dir() / "ui" / (std::string(p.name) + ".html"))),
                              std::string("экран примера звука строится: ") + p.name);
                st->starts = snd.music_starts();
            }
            if (f == 3) {
                check(sc.shown("звук_игра") && sc.music() == "шахта.wav" && snd.music_name() == "шахта.wav" && snd.music_playing() &&
                          mixer.voices(audio::Bus::Music) == 1,
                      "в «Шахте» музыка места главнее музыки экрана поверх игры: " + sc.music());
                st->voice = snd.music_voice();
                put(east_x, sy);
            }
            if (f == 8)
                check(snd.music_name() == "мелодия игры.wav" && mixer.voices(audio::Bus::Music) == 1, "вне зоны — музыка экрана: " + snd.music_name());
            if (f == 9) put(stairs_x, stairs_y);
            if (f == 14) {
                check(snd.music_name() == "шахта.wav" && mixer.voices(audio::Bus::Music) == 1, "снова в зоне — снова музыка места");
                st->starts = snd.music_starts();
                press_page(s, 160, 85); // «Окно»
            }
            if (f == 18) {
                check(sc.shown("звук_окно") && snd.music_name() == "мелодия окна.wav" && snd.music_starts() == st->starts + 1 &&
                          mixer.voices(audio::Bus::Music) == 1,
                      "окно со своей музыкой главнее музыки места: " + snd.music_name());
                press_page(s, 960, 645); // «Закрыть»
            }
            if (f == 22) {
                check(!sc.shown("звук_окно") && snd.music_name() == "шахта.wav" && snd.music_starts() == st->starts + 2 &&
                          mixer.voices(audio::Bus::Music) == 1,
                      "окно закрыто: снова музыка места, одна");
                st->voice = snd.music_voice();
                s.pause(true);
            }
            if (f == 26) {
                check(s.screen() == Screen::Paused && snd.music_voice() == st->voice && snd.music_playing() &&
                          snd.music_starts() == st->starts + 2,
                      "пауза: музыка места играет дальше, не заново");
                s.pause(false);
            }
            if (f == 30) {
                check(snd.music_voice() == st->voice && snd.music_starts() == st->starts + 2, "после паузы та же");
                // The volumes: music at 0, then all at 0 — playing, not heard; back — heard.
                st->settings = s.settings();
                Settings quiet = st->settings;
                quiet.music_volume = 0;
                s.apply_settings(quiet);
            }
            if (f == 33) {
                check(snd.music_playing() && music_peak(mixer) == 0, "«Музыка» 0: музыку места не слышно");
                Settings quiet = st->settings;
                quiet.master_volume = 0;
                s.apply_settings(quiet);
            }
            if (f == 36) {
                check(snd.music_playing() && music_peak(mixer) == 0, "общая громкость 0: не слышно");
                s.apply_settings(st->settings);
            }
            if (f == 39) {
                check(heard() > 0.05f && snd.music_starts() == st->starts + 2, "громкость вернули: слышно, музыка та же");
                for (const SimplePage& p : kSoundPages) sc.remove(p.name);
            }
            if (f < 42) return false;
            check(snd.music_name() == "шахта.wav", "экраны ушли: музыка места");
            return true;
        }});
        // A copy where «Дальний зал» has music of its own, the same file: from one place to the other it plays on.
        steps_.push_back({"две зоны с одной музыкой: из одной в другую она не начинается заново", 40,
                          [&s, &g, room_x, gallery_x, floor_y, in, put, this, st](u32 f) {
            audio::ScreenSounds& snd = g.sounds().screens();
            if (f == 0) {
                snd.attach(&g.sounds().mixer(), st->dir / "sounds");
                st->copy = std::filesystem::temp_directory_path() / "forge_slice_zones_copy";
                std::error_code ec;
                std::filesystem::remove_all(st->copy, ec);
                forge::level::LevelAreas a = st->areas;
                a.find(st->hall)->music = "шахта.wav";
                std::string why;
                check(forge::level::copy_level(st->level, st->copy, &why) && forge::level::save_areas(st->copy, a, &why),
                      "копия примера: у «Дальнего зала» музыка шахта.wav " + why);
                g.set_level(st->copy, true, gallery_x, floor_y);
                check(s.new_game(), "новая игра с копии, герой в штольне");
            }
            if (f == 10) {
                check(in(st->mine) && !in(st->hall) && snd.music_name() == "шахта.wav" && snd.music_playing(), "в штольне музыка «Шахты»");
                st->voice = snd.music_voice();
                st->starts = snd.music_starts();
                put(room_x, floor_y);
            }
            if (f == 20) {
                check(in(st->hall) && g.area_enters(st->hall) == 1 && s.screens().place_music() == "шахта.wav", "герой в «Дальнем зале», его музыка");
                check(snd.music_voice() == st->voice && snd.music_playing() && snd.music_starts() == st->starts,
                      "тот же файл: музыка играет дальше, не заново");
                put(gallery_x, floor_y);
            }
            if (f < 30) return false;
            check(!in(st->hall) && g.area_leaves(st->hall) == 1 && snd.music_voice() == st->voice && snd.music_starts() == st->starts,
                  "обратно в штольню: та же музыка");
            return true;
        }});
        steps_.push_back({"нет файла музыки: сказано один раз, игра идёт", 50, [&s, &g, stairs_x, stairs_y, this, st](u32 f) {
            audio::ScreenSounds& snd = g.sounds().screens();
            static usize problems = 0;
            if (f == 0) {
                st->copy = std::filesystem::temp_directory_path() / "forge_slice_zones_copy";
                std::error_code ec;
                std::filesystem::remove_all(st->copy, ec);
                forge::level::LevelAreas a = st->areas;
                a.find(st->mine)->music = "нет такой.wav";
                std::string why;
                check(forge::level::copy_level(st->level, st->copy, &why) && forge::level::save_areas(st->copy, a, &why),
                      "копия примера с музыкой, которой нет " + why);
                problems = snd.problems().size();
                st->starts = snd.music_starts();
                g.set_level(st->copy, true, stairs_x, stairs_y);
                check(s.new_game(), "новая игра с копии");
            }
            if (f == 10) {
                check(s.screens().place_music() == "нет такой.wav" && snd.music_name() == "нет такой.wav" && !snd.music_playing() &&
                          g.sounds().mixer().voices(audio::Bus::Music) == 0,
                      "музыки нет в папке: тишина");
                check(snd.problems().size() == problems + 1 && snd.problems().back() == "нет такой.wav", "о файле сказано");
            }
            if (f < 45) return false;
            check(snd.problems().size() == problems + 1 && snd.music_starts() == st->starts, "один раз: файл не читается каждый кадр");
            check(g.hero_alive() && s.screen() == Screen::Playing, "игра идёт");
            return true;
        }});
        steps_.push_back({"точка появления в камне, вне мира; испорченный areas.json", 30, [&s, &g, &gen, mx, sy, var, this, st](u32 f) {
            static f64 ex = 0, ey = 0;
            const std::filesystem::path rock = std::filesystem::temp_directory_path() / "forge_slice_zones_rock";
            if (f == 0) {
                std::error_code ec;
                std::filesystem::remove_all(rock, ec);
                forge::level::LevelAreas a = st->areas;
                a.spawn_y = sy + 6; // six rows down into the ground east of the entrance
                std::string why;
                check(forge::level::copy_level(st->level, rock, &why) && forge::level::save_areas(rock, a, &why), "копия с точкой в камне " + why);
                g.set_level(rock);
                check(s.new_game(), "новая игра");
                check(g.world() && hero_ground(*g.world(), a.spawn_x, a.spawn_y, kGroundReach, ex, ey),
                      "у точки есть пол с местом для героя");
                check(g.hero_x() == ex && g.hero_y() == scene::Position::at_tile(0, ey - kHeroHalfH).tile_y() && ey != a.spawn_y,
                      "герой не в камне, а на ближайшем полу: " + std::to_string(g.hero_x()) + ", " + std::to_string(g.hero_y()) +
                          " (пол " + std::to_string(ey) + ")");
            }
            if (f == 5) {
                check(g.hero_alive() && g.hero_x() == ex, "стоит там");
                forge::level::LevelAreas a = st->areas;
                a.spawn_y = -1e6; // over the world's top
                std::string why;
                check(forge::level::save_areas(rock, a, &why), "точка над миром " + why);
                g.set_level(rock);
                check(s.new_game() && g.hero_x() == gen.spawn_x(), "вне мира: герой на старте игры");
            }
            if (f == 10) {
                const std::string text = "{\"areas\": [{\"id\": 5}]}";
                check(write_file_atomic(rock / "areas.json", {reinterpret_cast<const u8*>(text.data()), text.size()}), "areas.json испорчен");
                g.set_level(rock);
                check(s.new_game(), "уровень с испорченным areas.json играется");
                check(g.areas() && g.areas()->areas.empty() && !g.areas()->spawn && g.hero_x() == gen.spawn_x(),
                      "зон нет, герой на старте игры");
                std::vector<u8> bytes;
                read_file(rock / "areas.json", bytes);
                check(std::string(bytes.begin(), bytes.end()) == text, "файл автора не тронут");
            }
            if (f < 15) return false;
            check(g.areas_inside().empty() && var("quest.pickaxe") == 0, "входов нет");
            (void)mx;
            return true;
        }});
        steps_.push_back({"обратно в меню", 5, [&s, &g, files_of, this, st](u32 f) {
            if (f == 0) {
                s.to_main_menu();
                g.set_level({});
                check(s.screens().place_music().empty() && g.sounds().screens().music_name().empty() &&
                          g.sounds().mixer().voices(audio::Bus::Music) == 0,
                      "в главном меню музыки места нет");
                g.sounds().screens().attach(&g.sounds().mixer(), st->kept);
                check(files_of(st->dir) == st->files, "файлы примера как были");
                std::error_code ec;
                std::filesystem::remove_all(st->copy, ec);
                std::filesystem::remove_all(std::filesystem::temp_directory_path() / "forge_slice_zones_rock", ec);
            }
            return f >= 2;
        }});
    }

    // Going from level to level (step 14.2b, 14.2-платформер-модель.md, «Порядок перехода в игре»), in the game of
    // trip::root() that main() made. The level the hero left is gone while it is away: its seconds do not count, its
    // critters and falling crate stand still, its sounds are silent, the keys move nothing in it; back there,
    // everything is as it was left. The hero carries its hearts, the bag and the game's values; «Только один раз» of
    // a copy stays with the copy, of an area with the hero. A link sends the hero (two in one step: the first wins; a
    // link's code still waiting when its level goes never goes on), a save made in «Пещера» keeps both levels, a new
    // game starts clean, saves made before levels go on, and a going refused loses nothing: a level not in the list,
    // its folder a file, an area not there, the disk refusing at each step (the game goes on where it was, or, when
    // even the level left does not open again, to the main menu with the saves as they were). continued: the save
    // made in «Пещера», by another process («Продолжить»).
    void build_levels(Shell& s, bool continued) {
        namespace fs = std::filesystem;
        SliceGame& g = g_;
        using Files = std::vector<std::pair<std::string, std::vector<u8>>>;
        struct State {
            f64 v = 0;                  // the top of the village's floor: where feet stand
            f64 ky = 0, cx = 0, px = 0; // «Ящик»'s y, «Зверёк»'s x and the keys' «Зверёк»'s x when «Деревня» was left
            f64 ticks = 0, frames = 0, hearts = 0, coins = 0, hx = 0;
            u32 named = 0, travels = 0, dropped = 0, refused = 0, arrived = 0;
            bool said = false;
            std::map<std::string, Files> slots; // every save's files, as they were before the refusals
            Files tree;                         // the session's files and folders just before a going refused
            flecs::entity_t hero = 0;           // the hero then
            std::vector<std::pair<fs::path, fs::path>> aside; // region files moved away (where, from) for a write that fails
            std::map<std::string, f64> notes;   // what the first process saw
        };
        auto st = std::make_shared<State>();
        auto files_of = [](const fs::path& dir) {
            Files out;
            std::error_code ec;
            for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
                if (!e.is_regular_file()) continue;
                std::vector<u8> bytes;
                read_file(e.path(), bytes);
                out.emplace_back(path_to_utf8(fs::relative(e.path(), dir)), std::move(bytes));
            }
            std::sort(out.begin(), out.end());
            return out;
        };
        // A folder's files with their bytes and its folders (a "/" after the name, no bytes).
        auto tree_of = [files_of](const fs::path& dir) {
            Files out = files_of(dir);
            std::error_code ec;
            for (const auto& e : fs::recursive_directory_iterator(dir, ec))
                if (e.is_directory()) out.emplace_back(path_to_utf8(fs::relative(e.path(), dir)) + "/", std::vector<u8>{});
            std::sort(out.begin(), out.end());
            return out;
        };
        auto tree_diff = [](const Files& a, const Files& b) {
            std::string out;
            std::map<std::string, const std::vector<u8>*> was;
            for (const auto& [name, bytes] : a) was[name] = &bytes;
            for (const auto& [name, bytes] : b) {
                auto it = was.find(name);
                if (it == was.end()) out += " +" + name;
                else if (*it->second != bytes) out += " ~" + name;
                if (it != was.end()) was.erase(it);
            }
            for (const auto& [name, bytes] : was) out += " -" + name;
            return out;
        };
        // A folder where a region file of the level's objects is, around a point of the session's world: a write
        // of that region fails (the file there, if any, moved away; aside says where, to put it back).
        auto block_region = [st](const fs::path& world, f64 x, f64 y) {
            namespace w = forge::world;
            const i32 rx = (static_cast<i32>(std::floor(x)) >> w::kChunkShift) >> w::kRegionShift;
            const i32 ry = (static_cast<i32>(std::floor(y)) >> w::kChunkShift) >> w::kRegionShift;
            const fs::path file = world / ("e." + std::to_string(rx) + "." + std::to_string(ry) + ".fwr");
            const fs::path away = trip::root() / utf8_path("в сторону");
            std::error_code ec;
            fs::create_directories(away, ec);
            st->aside.emplace_back(away / file.filename(), file);
            if (fs::is_regular_file(file, ec)) fs::rename(file, away / file.filename(), ec);
            fs::create_directories(file / utf8_path("занято"), ec);
            return fs::is_directory(file, ec);
        };
        auto unblock = [st] {
            std::error_code ec;
            for (const auto& [to, from] : st->aside) {
                fs::remove_all(from, ec);
                if (fs::exists(to, ec)) fs::rename(to, from, ec);
            }
            st->aside.clear();
        };
        auto saves = [&s, files_of] {
            std::map<std::string, Files> out;
            for (const SlotInfo& i : s.slots().list()) out[i.id] = files_of(s.slots().folder(i.id));
            return out;
        };
        auto put = [&g](f64 feet_x, f64 feet_y) { g.teleport(feet_x, feet_y - kHeroHalfH); };
        auto in = [&g](u64 id) {
            const std::vector<u64> now = g.areas_inside();
            return std::find(now.begin(), now.end(), id) != now.end();
        };
        auto var = [&s](const char* name) { return s.vars().get(name).number(); };
        auto num = [](f64 x) {
            char b[32];
            std::snprintf(b, sizeof b, "%.2f", x);
            return std::string(b);
        };
        auto at_spawn = [&g, st] { return g.hero_x() == 2.5 && std::fabs(g.hero_y() - (st->v - kHeroHalfH)) < 0.05; };
        auto at_entry = [&g, st] { return g.hero_x() == -47.5 && std::fabs(g.hero_y() - (st->v - kHeroHalfH)) < 0.05; };
        auto where = [&g, num] { return " (уровень «" + g.level_id() + "», герой " + num(g.hero_x()) + ", " + num(g.hero_y()) + ")"; };
        auto same_areas = [](const fs::path& a, const fs::path& b) {
            std::vector<u8> x, y;
            return read_file(a / "areas.json", x) && read_file(b / "areas.json", y) && x == y;
        };
        // hero.json of a save as a game before levels wrote it (no "level"), or with the level given.
        auto hero_level = [&s](const char* slot, const std::string* level) {
            const fs::path file = s.slots().folder(slot) / "hero.json";
            std::vector<u8> bytes;
            if (!read_file(file, bytes)) return false;
            std::string text(bytes.begin(), bytes.end());
            const usize at = text.find("\"level\""), comma = text.rfind(',', at);
            const usize open = text.find('"', text.find(':', at)), close = text.find('"', open + 1);
            if (at == std::string::npos || comma == std::string::npos || close == std::string::npos) return false;
            if (level) text.replace(open, close + 1 - open, "\"" + *level + "\"");
            else text.erase(comma, close + 1 - comma);
            return write_file_atomic(file, {reinterpret_cast<const u8*>(text.data()), text.size()});
        };
        // The keys' critter, the wandering one, the crate: where they are now.
        auto look = [&g](f64& cx, f64& px, f64& ky) {
            f64 x = 0;
            cx = g.critter_x(g.copy_with_id(trip::kCritter));
            px = g.critter_x(g.copy_with_id(trip::kPlayer));
            return g.position_of(g.copy_with_id(trip::kCrate), x, ky) && !std::isnan(cx) && !std::isnan(px);
        };
        auto walk = [&g](bool right) {
            Controls c;
            c.right = right;
            c.left = !right;
            g.script(c);
        };

        if (continued) {
            steps_.push_back({"«Продолжить» другим процессом: «Пещера», где сохранились", 40, [&s, &g, this, st, in, var, num, where](u32 f) {
                std::error_code ec;
                if (f < 5) return false;
                if (f == 5) {
                    st->v = g.generator().village_y();
                    std::vector<u8> bytes;
                    check(read_file(trip::notes(), bytes), "заметки первого процесса: " + path_to_utf8(trip::notes()));
                    const std::string text(bytes.begin(), bytes.end());
                    for (usize start = 0; start < text.size();) {
                        usize end = text.find('\n', start);
                        if (end == std::string::npos) end = text.size();
                        const std::string line = text.substr(start, end - start);
                        if (const usize sp = line.find(' '); sp != std::string::npos)
                            st->notes[line.substr(0, sp)] = std::strtod(line.c_str() + sp + 1, nullptr);
                        start = end + 1;
                    }
                    FORGE_INFO("рабочая папка: %s", path_to_utf8(fs::current_path(ec)).c_str());
                    check(fs::equivalent(s.user_folder(), trip::root() / "user", ec), "файлы игрока — в " + path_to_utf8(s.user_folder()));
                    check(s.slots().latest() && s.slots().latest()->id == "уровни", "последнее сохранение — «уровни»");
                    check(s.continue_game(), "«Продолжить»");
                    return false;
                }
                if (f < 15) return false;
                std::map<std::string, f64>& n = st->notes;
                check(g.level_id() == "cave" && std::fabs(g.hero_x() - n["x"]) < 0.01 && std::fabs(g.hero_y() - n["y"]) < 0.05,
                      "герой в «Пещере», где сохранился: " + num(n["x"]) + ", " + num(n["y"]) + where());
                check(g.areas() && g.areas()->find(trip::kEntry) && g.areas()->find(trip::kGrotto) && in(trip::kGrotto),
                      "зоны «Пещеры», герой в «Гроте»");
                check(g.hearts() == n["hearts"] && var("inv.coins") == n["coins"] && var("test.ticks") == n["ticks"] &&
                          var("test.frames") == n["frames"],
                      "сердца, сумка и значения игры — как при сохранении");
                check(g.hero_marked(trip::kMarkLink), "герой помнит «Метку»");
                check(!g.copy_with_id(trip::kTicker) && g.copies_of("ticker").empty(), "«Часов» «Деревни» здесь нет");
                return true;
            }});
            steps_.push_back({"«Деревня» не идёт, пока героя там нет", 130, [&g, this, st, var](u32 f) {
                if (f < 120) return false;
                check(g.level_id() == "cave" && var("test.ticks") == st->notes["ticks"] && var("test.frames") == st->notes["frames"],
                      "«Часы» «Деревни» не считают: test.ticks " + std::to_string(var("test.ticks")) + ", test.frames " +
                          std::to_string(var("test.frames")));
                return true;
            }});
            steps_.push_back({"в «Деревню»: она такая, какой её оставили", 200, [&s, &g, this, st, put, in, var, look, where](u32 f) {
                std::map<std::string, f64>& n = st->notes;
                if (f == 0) {
                    g.go_to("level");
                    return false;
                }
                if (f == 1) {
                    f64 cx = 0, px = 0, ky = 0;
                    const bool seen = look(cx, px, ky);
                    check(g.level_id() == "level" && g.travel_problem().empty(), "герой в «Деревне»" + where());
                    check(seen && std::fabs(ky - n["ky"]) < 1.0 && std::fabs(cx - n["cx"]) < 0.5 && std::fabs(px - n["px"]) < 0.5,
                          "«Ящик» и оба «Зверька» там, где их оставили: " + std::to_string(ky) + ", " + std::to_string(cx) + ", " +
                              std::to_string(px));
                    check(g.copy_with_id(trip::kTicker) && g.copy_with_id(trip::kSpring) && !g.copy_with_id(trip::kCoin),
                          "«Часы» и «Родник» на месте, подобранных монет нет");
                    check(s.screens().place_music() == "двор.wav", "музыка «Двора»: «" + s.screens().place_music() + "»");
                    g.hurt(1);
                    put(34.5, st->v);
                    return false;
                }
                if (f == 30) {
                    check(g.hearts() == n["hearts"] - 1, "«Родник» помнит, что уже лечил: сердец " + std::to_string(g.hearts()));
                    put(25.5, st->v);
                }
                if (f == 50) check(in(trip::kMark) && g.last_hint() != "Метка", "«Метка» себя не называет: «" + g.last_hint() + "»");
                if (f < 150) return false;
                check(var("test.frames") >= n["frames"] + 100, "схема «Часов» снова считает шаги: test.frames " + std::to_string(var("test.frames")));
                return true;
            }});
            steps_.push_back({"и снова в «Пещеру»; сохранение", 30, [&s, &g, this, at_entry, where](u32 f) {
                if (f == 0) {
                    g.go_to("cave", area_thing_id(trip::kEntry));
                    return false;
                }
                if (f < 5) return false;
                check(g.level_id() == "cave" && at_entry(), "герой в середине «Входа»" + where());
                check(s.save("уровни-2", "Уровни 2"), "игра сохраняется снова");
                return true;
            }});
            return;
        }

        steps_.push_back({"игра двух уровней в своей папке: новая игра", 30, [&s, &g, this, st](u32 f) {
            if (f < 5) return false;
            std::error_code ec;
            check(fs::equivalent(s.game_dir(), trip::root() / "data", ec), "игра читает данные из " + path_to_utf8(s.game_dir()));
            check(fs::equivalent(s.user_folder(), trip::root() / "user", ec), "файлы игрока — в " + path_to_utf8(s.user_folder()));
            check(s.data_errors().empty(), "данные игры читаются без ошибок");
            const forge::level::LevelList list = forge::level::read_levels(s.game_dir());
            check(list.from_file && list.writable() && list.levels.size() == 3 && list.start == "level",
                  "levels.json: «Деревня» (старт), «Пещера» и «Файл»");
            check(s.slots().list().empty(), "сохранений ещё нет");
            // The areas, on the village's floor (the game's world is around both levels).
            st->v = g.generator().village_y();
            const i32 v = g.generator().village_y();
            auto area = [v](u64 id, const char* name, i32 x0, i32 x1, const char* music) {
                forge::level::Area a;
                a.id = id;
                a.name = name;
                a.x0 = x0;
                a.x1 = x1;
                a.y0 = v - 6;
                a.y1 = v + 2;
                a.music = music;
                return a;
            };
            forge::level::LevelAreas village, cave;
            village.areas = {area(trip::kYard, "Двор", -6, 8, "двор.wav"), area(trip::kMark, "Метка", 24, 27, ""),
                             area(trip::kExit, "Выход", 40, 43, ""), area(trip::kGate, "Калитка", -16, -12, ""),
                             area(trip::kPath, "Тропа", -22, -18, "")};
            village.spawn = true;
            village.spawn_x = 2.5;
            village.spawn_y = v;
            cave.areas = {area(trip::kEntry, "Вход", -50, -46, "пещера.wav"), area(trip::kGrotto, "Грот", -62, -59, "")};
            cave.spawn = true;
            cave.spawn_x = -40.5;
            cave.spawn_y = v;
            std::string why;
            for (const auto& [id, areas] : {std::pair{"level", &village}, std::pair{"cave", &cave}}) {
                const fs::path dir = forge::level::level_folder(s.game_dir(), id);
                fs::create_directories(dir, ec);
                check(forge::level::save_areas(dir, *areas, &why), std::string("зоны уровня ") + id + " записаны " + why);
            }
            g.set_level({});
            check(s.new_game(), "новая игра");
            return true;
        }});
        steps_.push_back({"новая игра — на стартовом уровне «Деревня»", 40, [&s, &g, this, st, var, at_spawn, where](u32 f) {
            if (f < 20) return false;
            std::error_code ec;
            check(g.level_id() == "level" && at_spawn(), "герой в точке появления «Деревни»" + where());
            check(!fs::exists(s.slots().session() / "levels", ec), "других уровней в игре ещё нет (session/levels)");
            check(g.travels() == 0 && g.travel_problem().empty(), "переходов ещё не было");
            check(s.screens().shown("окно_уровня") && g.level_screens().count("окно_уровня"), "«Двор» открыл окно уровня");
            check(s.screens().place_music() == "двор.wav", "музыка «Двора»: «" + s.screens().place_music() + "»");
            check(g.hearts() == 3 && var("inv.coins") == 0, "три сердца, монет нет");
            return true;
        }});
        steps_.push_back({"«Деревня» живёт: секунды, зверьки, звук, клавиши", 200, [&s, &g, this, st, var](u32 f) {
            const f64 v = st->v;
            if (f == 0) {
                const flecs::entity_t t = g.spawn_copy("ticker", -3.5, v, trip::kTicker);
                Sounds near;
                near.near = "ручей.wav";
                near.range = 24;
                check(t && g.set_sounds(t, near), "«Часы» стоят, рядом с ними журчит");
                const flecs::entity_t c = g.spawn_critter(-8.5, v, Scheme::Wander, trip::kCritter);
                Sounds steps;
                steps.step = "шаг.wav";
                steps.range = 24;
                check(c && g.set_sounds(c, steps), "«Зверёк» бегает и стучит шагами");
                check(g.spawn_critter(-1.5, v, Scheme::Player, trip::kPlayer) != 0, "«Зверёк» по клавишам стоит рядом с героем");
                check(g.spawn_copy("spring", 34.5, v, trip::kSpring) && g.spawn_copy("coins", 20.5, v, trip::kCoin), "«Родник» и «Монеты»");
                check(s.screens().show("окно_игрока", true), "игрок открыл своё окно"); // as a button of the HUD opens it
                st->named = g.sounds().played_named();
                st->cx = g.critter_x(c);
                return false;
            }
            if (f == 60) {
                st->px = g.critter_x(g.copy_with_id(trip::kPlayer));
                Controls c;
                c.right = true;
                g.script(c);
            }
            if (f == 80) g.stop_script();
            if (f < 180) return false;
            check(var("test.ticks") >= 2, "«Часы» считают секунды: test.ticks = " + s.vars().get("test.ticks").text());
            check(var("test.frames") >= 100, "и шаги своей схемой: test.frames = " + s.vars().get("test.frames").text());
            check(g.sounds().loops() == 1, "у «Часов» журчит: петель звука " + std::to_string(g.sounds().loops()));
            check(g.sounds().played_named() > st->named, "шаги «Зверька» звучат: " + std::to_string(g.sounds().played_named()));
            check(std::fabs(g.critter_x(g.copy_with_id(trip::kCritter)) - st->cx) > 0.5, "«Зверёк» бегает");
            check(g.critter_x(g.copy_with_id(trip::kPlayer)) > st->px + 0.5, "клавиши ведут «Зверька» по клавишам");
            return true;
        }});
        steps_.push_back({"сердца, монеты, «Родник» и «Метка»", 100, [&g, this, st, put, in, var](u32 f) {
            if (f == 0) {
                g.hurt(1);
                put(20.5, st->v); // onto the coins
            }
            if (f == 15) {
                check(var("inv.coins") == 10 && !g.copy_with_id(trip::kCoin), "монеты подобраны: в сумке " + std::to_string(var("inv.coins")));
                put(25.5, st->v); // into «Метка»
            }
            if (f == 30) {
                check(in(trip::kMark) && g.last_hint() == "Метка" && g.hero_marked(trip::kMarkLink),
                      "«Метка» назвала себя, «один раз» запомнил герой: «" + g.last_hint() + "»");
                put(34.5, st->v); // onto «Родник»
            }
            if (f == 60) {
                check(g.hearts() == 3, "«Родник» вылечил: сердец " + std::to_string(g.hearts()));
                put(2.5, st->v);
            }
            return f >= 80;
        }});
        steps_.push_back({"в «Пещеру» между шагами: «Деревня» замерла целиком", 260, [&s, &g, this, st, put, in, var, look, at_entry, where,
                                                                                  same_areas](u32 f) {
            std::error_code ec;
            if (f == 0) {
                check(g.spawn_copy("crate", 5.5, st->v - 25, trip::kCrate) != 0, "«Ящик» высоко над землёй");
                return false;
            }
            if (f < 25) return false;
            if (f == 25) {
                check(look(st->cx, st->px, st->ky) && st->ky < st->v - 15, "«Ящик» падает: y " + std::to_string(st->ky));
                st->ticks = var("test.ticks");
                st->frames = var("test.frames");
                st->named = g.sounds().played_named();
                st->hearts = g.hearts();
                st->coins = var("inv.coins");
                st->travels = g.travels();
                st->said = false;
                g.go_to("cave", area_thing_id(trip::kEntry));
                return false;
            }
            if (f == 26) {
                const fs::path session = s.slots().session();
                check(g.level_id() == "cave" && g.travels() == st->travels + 1 && g.travel_problem().empty(),
                      "герой в «Пещере»: «" + g.travel_problem() + "»" + where());
                check(at_entry(), "герой в середине «Входа», на полу" + where());
                check(g.areas() && g.areas()->find(trip::kEntry) && !g.areas()->find(trip::kYard), "зоны — «Пещеры»");
                check(in(trip::kEntry) && g.area_enters(trip::kEntry) == 0, "герой уже во «Входе», в него не входил: обратно не уходит");
                for (const u64 id : {trip::kTicker, trip::kCritter, trip::kCrate, trip::kSpring, trip::kPlayer})
                    check(!g.copy_with_id(id), "вещи «Деревни» в «Пещере» нет: " + forge::level::area_id_text(id));
                check(g.copies_of("ticker").empty() && g.copies_of("spring").empty(), "«Часов» и «Родника» здесь нет");
                check(g.sounds().loops() == 0, "петли звука «Деревни» остановлены: " + std::to_string(g.sounds().loops()));
                check(!s.screens().shown("окно_уровня") && g.level_screens().empty(), "окно, открытое «Деревней», закрыто");
                check(s.screens().shown("окно_игрока"), "окно, открытое игроком, осталось");
                check(s.screens().place_music() == "пещера.wav", "музыка места — «Входа»: «" + s.screens().place_music() + "»");
                check(g.hearts() == st->hearts && var("inv.coins") == st->coins && var("test.ticks") == st->ticks &&
                          var("test.frames") == st->frames,
                      "сердца, сумка и значения игры — те же");
                check(g.hero_marked(trip::kMarkLink), "герой помнит «Метку»");
                check(fs::is_directory(session / "levels" / "level", ec), "«Деревня» записана в session/levels/level");
                check(same_areas(session / "world", forge::level::level_folder(s.game_dir(), "cave")), "мир игры — копия «Пещеры» автора");
                std::string temps;
                for (const auto& e : fs::directory_iterator(session / "levels", ec))
                    if (path_to_utf8(e.path().filename()) != "level") temps += " " + path_to_utf8(e.path().filename());
                check(!fs::exists(session / "levels" / "cave", ec) && temps.empty(),
                      "копия «Пещеры» стала миром игры, лишних и временных папок нет:" + temps);
                Controls c; // the keys: here, not in «Деревня»
                c.right = true;
                g.script(c);
                return false;
            }
            if (f == 60) g.stop_script();
            if (f == 100) put(-60.5, st->v); // into «Грот»
            // Every frame there: «Деревня» counts nothing and sounds nothing.
            if (!st->said && (var("test.ticks") != st->ticks || var("test.frames") != st->frames ||
                              g.sounds().played_named() != st->named || g.sounds().loops() != 0)) {
                st->said = true;
                check(false, "в «Пещере» «Деревня» не идёт: test.ticks " + std::to_string(var("test.ticks")) + " (было " +
                                 std::to_string(st->ticks) + "), test.frames " + std::to_string(var("test.frames")) + " (было " +
                                 std::to_string(st->frames) + "), звуков по имени " + std::to_string(g.sounds().played_named()) +
                                 " (было " + std::to_string(st->named) + "), петель " + std::to_string(g.sounds().loops()));
            }
            if (f < 26 + 200) return false;
            check(g.level_id() == "cave" && in(trip::kGrotto), "герой всё ещё в «Пещере», в «Гроте»" + where());
            check(g.last_hint() == "Грот" && var("test.grot") == 1, "«Грот»: название и связь, ждавшая 0,3 с, сделаны: test.grot " +
                                                                      std::to_string(var("test.grot")));
            return true;
        }});
        steps_.push_back({"обратно между шагами: «Деревня» такая, какой её оставили", 200, [&s, &g, this, st, var, look, at_spawn, where](u32 f) {
            std::error_code ec;
            if (f == 0) {
                st->travels = g.travels();
                g.go_to("level");
                return false;
            }
            f64 cx = 0, px = 0, ky = 0;
            const bool seen = look(cx, px, ky);
            if (f == 1) {
                const fs::path session = s.slots().session();
                check(g.level_id() == "level" && g.travels() == st->travels + 1 && at_spawn(), "герой в точке появления «Деревни»" + where());
                check(seen && std::fabs(ky - st->ky) < 1.0, "«Ящик» висит, где его оставили: y " + std::to_string(ky) + ", был " + std::to_string(st->ky));
                check(seen && std::fabs(cx - st->cx) < 0.5, "«Зверёк» там, где его оставили: x " + std::to_string(cx) + ", был " + std::to_string(st->cx));
                check(seen && std::fabs(px - st->px) < 0.5, "клавиши в «Пещере» «Зверька» «Деревни» не двигали: x " + std::to_string(px) +
                                                                ", был " + std::to_string(st->px));
                check(g.copy_with_id(trip::kTicker) && g.copy_with_id(trip::kSpring) && !g.copy_with_id(trip::kCoin),
                      "«Часы» и «Родник» на месте, подобранных монет нет");
                check(var("test.ticks") == st->ticks && var("test.frames") - st->frames <= 4,
                      "пока героя не было, «Часы» не считали: test.ticks " + std::to_string(var("test.ticks")) + ", test.frames " +
                          std::to_string(var("test.frames")) + " (было " + std::to_string(st->frames) + ")");
                check(!s.screens().shown("окно_уровня") && s.screens().shown("окно_игрока"), "окно уровня закрыто, окно игрока открыто");
                check(s.screens().place_music() == "двор.wav", "музыка «Двора»: «" + s.screens().place_music() + "»");
                check(!fs::exists(session / "levels" / "level", ec) && fs::is_directory(session / "levels" / "cave", ec),
                      "«Пещера» ждёт в session/levels/cave");
                return false;
            }
            if (f < 150) return false;
            check(var("test.frames") >= st->frames + 100, "схема «Часов» снова считает шаги: test.frames " + std::to_string(var("test.frames")));
            // Their «При старте» does not run again, as after a load: its waiting a second is gone (a limit of 14.2b).
            FORGE_INFO("«Часы» «При старте» после возвращения: test.ticks %.0f (было %.0f)", var("test.ticks"), st->ticks);
            check(g.sounds().loops() == 1, "и журчат: петель звука " + std::to_string(g.sounds().loops()));
            check(seen && ky > st->v - 2, "«Ящик» упал на землю: y " + std::to_string(ky));
            s.screens().show("окно_игрока", false);
            return true;
        }});
        steps_.push_back({"«Только один раз»: «Родник» помнит сам, «Метку» помнит герой", 80, [&g, this, st, put, in](u32 f) {
            if (f == 0) {
                g.hurt(1);
                put(34.5, st->v);
            }
            if (f == 30) {
                check(g.hearts() == 2, "«Родник» второй раз не лечит: сердец " + std::to_string(g.hearts()));
                put(25.5, st->v);
            }
            if (f < 50) return false;
            check(in(trip::kMark) && g.last_hint() == "Грот", "«Метка» второй раз себя не называет: подсказка «" + g.last_hint() + "»");
            put(2.5, st->v);
            return true;
        }});
        steps_.push_back({"по связи: «Выход» → «Пещера», «Вход»; вторая связь того же шага не нужна", 300,
                          [&g, this, st, put, in, var, walk, at_entry, where](u32 f) {
            if (f == 0) {
                st->travels = g.travels();
                st->dropped = g.travels_dropped();
                st->arrived = 0;
                put(37.5, st->v);
                return false;
            }
            if (f < 10) return false;
            if (!st->arrived && g.level_id() == "level" && f < 250) {
                walk(true);
                return false;
            }
            g.stop_script();
            if (!st->arrived) {
                st->arrived = f;
                check(g.level_id() == "cave" && g.travels() == st->travels + 1, "герой вошёл в «Выход» и ушёл в «Пещеру»" + where());
                check(at_entry() && in(trip::kEntry), "в середину «Входа», как сказала первая связь" + where());
                check(g.travels_dropped() == st->dropped + 1, "вторая связь того же шага («в точку появления») не нужна: " +
                                                                  std::to_string(g.travels_dropped() - st->dropped));
                check(!g.hero_marked(trip::kGoTooLink), "её «Только один раз» не отмечен: она никуда не отправила");
            }
            if (f < st->arrived + 60) return false;
            check(g.level_id() == "cave" && g.travels() == st->travels + 1 && at_entry(), "через секунду герой там же: обратно его не отправило" + where());
            check(var("test.after") == 0, "код связи, ждавший в «Деревне», после ухода не продолжился: test.after " + std::to_string(var("test.after")));
            return true;
        }});
        steps_.push_back({"по связи обратно: «Вход» → «Деревня», точка появления", 300, [&g, this, st, put, in, walk, at_spawn, where](u32 f) {
            if (f == 0) {
                st->travels = g.travels();
                st->arrived = 0;
                put(-43.5, st->v);
                return false;
            }
            if (f < 10) return false;
            if (f == 10) check(!in(trip::kEntry), "герой вышел из «Входа»");
            if (!st->arrived && g.level_id() == "cave" && f < 250) {
                walk(false);
                return false;
            }
            g.stop_script();
            if (!st->arrived) {
                st->arrived = f;
                check(g.level_id() == "level" && g.travels() == st->travels + 1 && at_spawn(), "герой вернулся в точку появления «Деревни»" + where());
                check(g.copy_with_id(trip::kTicker) && !g.copy_with_id(trip::kCoin), "«Деревня» та же");
            }
            if (f < st->arrived + 60) return false;
            check(g.level_id() == "level" && g.travels() == st->travels + 1, "через секунду герой там же" + where());
            return true;
        }});
        steps_.push_back({"новая игра начинается с начала; отказы до первого шага в «Пещеру», «Только один раз»", 140,
                          [&s, &g, this, st, var, put, at_spawn, where, tree_of, tree_diff, block_region, unblock](u32 f) {
            std::error_code ec;
            const fs::path session = s.slots().session();
            auto same_session = [&](const std::string& what) {
                const std::string diff = tree_diff(st->tree, tree_of(session));
                check(diff.empty(), what + ": session та же, файл в файл и папка в папку:" + diff);
            };
            auto refused = [&](const std::string& what, const char* says) {
                check(g.level_id() == "level" && g.travels_refused() == st->refused + 1 && g.hero_entity() == st->hero &&
                          g.travel_problem().find(says) != std::string::npos,
                      what + ": «Пещеры» не будет, «Деревня» играет, герой тот же: «" + g.travel_problem() + "»");
                same_session(what);
                st->refused = g.travels_refused();
            };
            if (f == 0) {
                check(s.save("перед-новой", "Перед новой"), "игра сохраняется в «Деревне»");
                check(fs::is_directory(s.slots().folder("перед-новой") / "levels" / "cave", ec), "в сохранении и «Пещера»");
                check(s.new_game(), "новая игра");
                return false;
            }
            if (f == 10) {
                check(g.level_id() == "level" && g.travels() == 0, "новая игра — на «Деревне»");
                check(!fs::exists(session / "levels", ec), "уровней прошлой игры нет (session/levels)");
                check(!g.copy_with_id(trip::kTicker) && !g.hero_marked(trip::kMarkLink), "«Деревня» — как у автора, герой ничего не помнит");
                check(g.hearts() == 3 && var("inv.coins") == 0 && var("test.ticks") == 0 && var("test.frames") == 0, "сердца, сумка и значения — новые");
                // session/levels is a file: the level left cannot be kept, so the hero does not go.
                const std::string_view text = "файл на месте папки уровней";
                check(write_file_atomic(session / "levels", {reinterpret_cast<const u8*>(text.data()), text.size()}), "session/levels — файл");
                st->tree = tree_of(session);
                st->hero = g.hero_entity();
                st->refused = g.travels_refused();
                put(-14.5, st->v); // into «Калитка»: once, to «Пещера», at once
                return false;
            }
            if (f == 14) {
                refused("«Калитка», а session/levels — файл", "не создать папку уровней");
                check(!g.hero_marked(trip::kGateLink), "«Только один раз» «Калитки» не отмечен: перехода не было");
                put(-20.5, st->v); // into «Тропа»: once, its scheme waits 0.3 s, then to «Пещера»
                return false;
            }
            if (f == 20) {
                check(g.level_id() == "level" && g.travels_refused() == st->refused && g.hero_marked(trip::kPathLink),
                      "схема «Тропы» ждёт: «Только один раз» уже отмечен, перехода ещё не просили");
                return false;
            }
            if (f == 45) {
                refused("«Тропа» после ожидания, а session/levels — файл", "не создать папку уровней");
                check(!g.hero_marked(trip::kPathLink), "«Только один раз» «Тропы» снят: её переход не состоялся");
                fs::remove(session / "levels", ec);
                put(2.5, st->v);
                return false;
            }
            if (f == 48) {
                // The disk refuses for real while «Пещера» was never gone to: the left level's objects are not written.
                check(block_region(session / "world", g.hero_x(), g.hero_y()), "на месте файла объектов «Деревни» у героя — папка");
                st->tree = tree_of(session);
                st->hero = g.hero_entity();
                g.go_to("cave");
                return false;
            }
            if (f == 49) {
                refused("диск не пишет «Деревню»", "не записался на диск");
                check(!fs::exists(session / "levels", ec), "ни копии «Пещеры», ни папки уровней в session");
                unblock();
                put(-20.5, st->v);
                return false;
            }
            if (f == 80) {
                check(g.level_id() == "cave" && g.travels() == 1 && g.travel_problem().empty(), "файл вернули: «Тропа» после ожидания уводит в «Пещеру»" + where());
                check(g.hero_marked(trip::kPathLink), "и теперь её «Только один раз» отмечен");
                g.go_to("level");
                return false;
            }
            if (f == 81) {
                check(g.level_id() == "level" && at_spawn(), "обратно в точку появления «Деревни»" + where());
                put(-14.5, st->v);
                return false;
            }
            if (f == 85) {
                check(g.level_id() == "cave" && g.travels() == 3 && g.hero_marked(trip::kGateLink), "и «Калитка» уводит, теперь отмечена" + where());
                g.go_to("level");
                return false;
            }
            if (f == 86) {
                st->travels = g.travels();
                put(-20.5, st->v);
                return false;
            }
            if (f == 120) {
                check(g.level_id() == "level" && g.travels() == st->travels, "в «Тропу» снова: она больше не уводит" + where());
                put(-14.5, st->v);
                return false;
            }
            if (f < 126) return false;
            check(g.level_id() == "level" && g.travels() == st->travels, "в «Калитку» снова: она больше не уводит" + where());
            check(s.load("перед-новой"), "сохранение «перед-новой» загружается");
            return true;
        }});
        steps_.push_back({"сохранения прежних версий: без «level» и «level»: \"\"", 30, [&s, &g, this, hero_level](u32 f) {
            std::error_code ec;
            const fs::path session = s.slots().session();
            if (f == 0) {
                check(g.level_id() == "level" && g.copy_with_id(trip::kTicker), "«перед-новой»: «Деревня» с «Часами»");
                check(s.save("старое", "Старое") && hero_level("старое", nullptr), "сохранение, как до уровней: в hero.json нет «level»");
                const std::string none;
                check(s.save("вне-списка", "Вне списка") && hero_level("вне-списка", &none), "сохранение уровня не из списка: «level»: \"\"");
                check(s.load("старое"), "старое сохранение загружается");
                return false;
            }
            if (f == 5) {
                check(g.level_id() == "level", "без «level» — «level», а не «" + g.level_id() + "»");
                g.go_to("cave");
                return false;
            }
            if (f == 6) {
                check(g.level_id() == "cave", "из старого сохранения — в «Пещеру»");
                g.go_to("level");
                return false;
            }
            if (f == 7) {
                check(g.level_id() == "level" && g.copy_with_id(trip::kTicker), "и обратно: «Деревня» та же, с «Часами»");
                check(s.load("вне-списка"), "сохранение уровня не из списка загружается");
                return false;
            }
            if (f == 12) {
                check(g.level_id().empty() && g.copy_with_id(trip::kTicker), "«level»: \"\" — уровень не из списка («" + g.level_id() + "»)");
                g.go_to("cave");
                return false;
            }
            if (f == 13) {
                check(g.level_id() == "cave", "из него — в «Пещеру»");
                std::string left;
                for (const auto& e : fs::directory_iterator(session / "levels", ec)) left += " " + path_to_utf8(e.path().filename());
                check(left.empty(), "уровень не из списка не сохраняется (вернуться в него нельзя), временных папок нет:" + left);
                g.go_to("level");
                return false;
            }
            if (f == 14) {
                check(g.level_id() == "level" && !g.copy_with_id(trip::kTicker), "«Деревня» после него — как у автора");
                check(s.load("перед-новой"), "сохранение «перед-новой» загружается");
                return false;
            }
            return f >= 20;
        }});
        steps_.push_back({"отказы: «Деревня» живёт, как жила, session и сохранения не тронуты", 170,
                          [&s, &g, this, st, saves, same_areas, var, put, tree_of, tree_diff, block_region, unblock](u32 f) {
            std::error_code ec;
            const fs::path session = s.slots().session();
            // Refused: the same level with the same hero where it stood, the window its «Двор» opened, the session as it
            // was just before (files and folders: nothing staged is left), and the reason.
            auto refused = [&](const std::string& what, const char* says) {
                check(g.level_id() == "level" && g.running() && g.hero_entity() == st->hero && std::fabs(g.hero_x() - st->hx) < 0.01 &&
                          g.copy_with_id(trip::kTicker) && g.copy_with_id(trip::kTicker2) && g.travel_problem().find(says) != std::string::npos,
                      what + ": «Деревня» играет дальше, герой тот же и на месте; почему — «" + g.travel_problem() + "»");
                check(s.screens().shown("окно_уровня") && g.level_screens().count("окно_уровня"), what + ": окно «Двора» открыто");
                const std::string diff = tree_diff(st->tree, tree_of(session));
                check(diff.empty(), what + ": session та же, файл в файл и папка в папку:" + diff);
            };
            // The next going: the session and the hero as they are just before it.
            auto go = [&](const std::string& level, const std::string& arrive = {}) {
                st->tree = tree_of(session);
                st->hero = g.hero_entity();
                g.go_to(level, arrive);
            };
            if (f == 0) {
                check(g.level_id() == "level" && g.copy_with_id(trip::kTicker), "«перед-новой»: «Деревня» с «Часами»");
                // Other «Часы», put now: their «При старте» waits a second, again and again (the loaded ones' does not
                // go on after a load). A going refused must not stop it.
                check(g.spawn_copy("ticker", -2.5, st->v, trip::kTicker2) != 0, "ещё одни «Часы»: их ожидание идёт");
                put(12.5, st->v); // out of «Двор», then into it: its link opens the level's window
                return false;
            }
            if (f == 3) put(2.5, st->v);
            if (f < 10) return false;
            const u32 k = f - 10;
            switch (k) {
            case 0:
                check(s.screens().shown("окно_уровня") && g.level_screens().count("окно_уровня"), "«Двор» открыл окно уровня");
                st->slots = saves();
                st->hx = g.hero_x();
                st->ticks = var("test.ticks");
                st->frames = var("test.frames");
                go("nowhere");
                break;
            case 1:
                refused("уровня нет в списке", "нет в списке уровней");
                go("file");
                break;
            case 2:
                refused("папка уровня — файл", "— файл");
                go("cave", "area:00000000000000ff");
                break;
            case 3:
                refused("зоны, где появиться, нет", "нет зоны");
                go("cave", "spawn");
                break;
            case 4:
                refused("где появиться — не зона", "не зона");
                g.fail_next_travel(SliceGame::TravelFault::Save);
                go("cave");
                break;
            case 5:
                refused("«Деревня» не записалась", "не записался на диск");
                g.fail_next_travel(SliceGame::TravelFault::Move);
                go("cave");
                break;
            case 6:
                refused("папка «Деревни» не переместилась", "не переместилась");
                g.fail_next_travel(SliceGame::TravelFault::Make);
                go("cave");
                break;
            case 7: {
                refused("«Пещера» не открылась", "не открылся");
                check(fs::is_directory(session / "levels" / "cave", ec) && same_areas(session / "world", forge::level::level_folder(s.game_dir(), "level")),
                      "папки на месте: «Деревня» — мир игры, «Пещера» — в session/levels/cave");
                // The disk refuses for real: where the file of the level's objects around the hero was, a folder.
                check(block_region(session / "world", g.hero_x(), g.hero_y()) && fs::exists(st->aside.back().first, ec),
                      "файл объектов уровня у героя заменён папкой");
                go("cave");
                break;
            }
            case 8:
                refused("диск не пишет файлы уровня", "не записался на диск");
                unblock();
                break;
            case 130:
                // All that while «Деревня» went on: its waiting «Часы» counted, the scheme stepped, the window stayed.
                check(var("test.ticks") >= st->ticks + 2, "ожидание «Часов» шло и после отказов: test.ticks " + std::to_string(var("test.ticks")) +
                                                              " (было " + std::to_string(st->ticks) + ")");
                check(var("test.frames") >= st->frames + 100, "схема «Часов» считала шаги: test.frames " + std::to_string(var("test.frames")));
                check(g.level_id() == "level" && g.hero_entity() == st->hero && s.screens().shown("окно_уровня") &&
                          g.level_screens().count("окно_уровня"),
                      "герой тот же, окно «Двора» открыто");
                g.go_to("cave");
                break;
            case 131:
                check(g.level_id() == "cave" && g.travel_problem().empty(), "файлы вернули — переход есть: «" + g.travel_problem() + "»");
                g.go_to("level");
                break;
            case 132:
                check(g.level_id() == "level" && g.copy_with_id(trip::kTicker) && g.copy_with_id(trip::kTicker2), "и обратно: «Деревня» с обоими «Часами»");
                check(saves() == st->slots, "сохранения те же, байт в байт");
                return true;
            default: break;
            }
            return false;
        }});
        steps_.push_back({"папки не встали на место и не вернулись: главное меню, сохранения не тронуты", 20, [&s, &g, this, st, saves](u32 f) {
            if (f == 0) {
                g.fail_next_travel(SliceGame::TravelFault::Back);
                g.go_to("cave");
                return false;
            }
            if (f == 1) {
                check(!g.running() && s.screen() == Screen::Main, "папки сохранения не вернулись на место: игра в главном меню");
                check(g.travel_problem().find("не вернулись на место") != std::string::npos, "почему: «" + g.travel_problem() + "»");
                check(saves() == st->slots, "сохранения те же, байт в байт, автосохранения нет");
                return false;
            }
            if (f < 5) return false;
            check(s.load("перед-новой") && g.level_id() == "level" && g.copy_with_id(trip::kTicker), "сохранение «перед-новой» играется дальше");
            return true;
        }});
        steps_.push_back({"сохранение в «Пещере»: оба уровня", 90, [&s, &g, this, st, put, in, var, look, same_areas](u32 f) {
            std::error_code ec;
            if (f == 0) {
                check(look(st->cx, st->px, st->ky), "«Деревня»: оба «Зверька» и «Ящик»");
                g.go_to("cave", area_thing_id(trip::kEntry));
                return false;
            }
            if (f == 30) put(-60.5, st->v); // into «Грот»
            if (f < 60) return false;
            check(g.level_id() == "cave" && in(trip::kGrotto), "герой в «Пещере», в «Гроте»");
            SDL_Delay(1100); // the newest save by a second: «Продолжить» takes it
            check(s.save("уровни", "Уровни"), "игра сохраняется в «Пещере»");
            const fs::path slot = s.slots().folder("уровни");
            std::vector<u8> bytes;
            read_file(slot / "hero.json", bytes);
            const std::string hero(bytes.begin(), bytes.end());
            check(hero.find("\"level\": \"cave\"") != std::string::npos, "hero.json: «level»: «cave»: " + hero);
            check(fs::is_directory(slot / "levels" / "level", ec) && !fs::exists(slot / "levels" / "cave", ec) &&
                      same_areas(slot / "world", forge::level::level_folder(s.game_dir(), "cave")),
                  "в сохранении «Пещера» — мир игры, «Деревня» — levels/level");
            for (const auto& e : fs::directory_iterator(slot / "levels", ec))
                check(path_to_utf8(e.path().filename())[0] != '.', "временных папок нет: " + path_to_utf8(e.path().filename()));
            check(s.slots().latest() && s.slots().latest()->id == "уровни", "оно последнее: его продолжит «Продолжить»");
            std::string text;
            char line[96];
            for (const auto& [key, value] : std::initializer_list<std::pair<const char*, f64>>{
                     {"x", g.hero_x()}, {"y", g.hero_y()}, {"ticks", var("test.ticks")}, {"frames", var("test.frames")}, {"hearts", g.hearts()},
                     {"coins", var("inv.coins")}, {"ky", st->ky}, {"cx", st->cx}, {"px", st->px}}) {
                std::snprintf(line, sizeof line, "%s %.17g\n", key, value);
                text += line;
            }
            check(write_file_atomic(trip::notes(), {reinterpret_cast<const u8*>(text.data()), text.size()}), "заметки для второго процесса");
            return true;
        }});
    }

    // --scene platformer and platformer_continue (step 14.2c, 14.2-платформер-модель.md, «Как проверяется 14.2c»): the
    // platformer's rules on the game of two levels in plat::root(), «Луг» and «Холм», nothing around them. The keys lead
    // the hero (walking, jumping onto enemies); a new game ends in a loss, «Ещё раз» plays a clean one over to «Холм»
    // and back, where it is saved; platformer_continue goes on from there in another process and wins; platformer_edges,
    // a third process on the same game, plays a link «собирает», the frames games end in and returns with no place to go.
    void build_platformer(Shell& s, bool continued, bool edges) {
        namespace fs = std::filesystem;
        SliceGame& g = g_;
        using Files = std::vector<std::pair<std::string, std::vector<u8>>>;
        struct State {
            f64 v = 0; // the floor's top on both levels: where feet stand
            u32 phase = 0, at = 0, hit1 = 0;
            f64 x0 = 0, y0 = 0, top = 0, score0 = 0, wx = 0;
            u32 stomps0 = 0, hits0 = 0, hazards0 = 0, backs0 = 0, falls0 = 0, travels0 = 0, coins_cue = 0, crate_cue = 0, named0 = 0;
            u32 last = 0, nowheres0 = 0, pickup_cue = 0;
            f64 coins0 = 0, gx = 0;
            Files saves;
            std::map<std::string, f64> notes; // what the first process saw
        };
        auto st = std::make_shared<State>();
        st->v = g.generator().village_y();
        auto files_of = [](const fs::path& dir) {
            Files out;
            std::error_code ec;
            for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
                if (!e.is_regular_file()) continue;
                std::vector<u8> bytes;
                read_file(e.path(), bytes);
                out.emplace_back(path_to_utf8(fs::relative(e.path(), dir)), std::move(bytes));
            }
            std::sort(out.begin(), out.end());
            return out;
        };
        auto saves = [&s, files_of] {
            Files out;
            for (const SlotInfo& i : s.slots().list())
                for (auto& f : files_of(s.slots().folder(i.id))) out.emplace_back(i.id + "/" + f.first, std::move(f.second));
            return out;
        };
        auto put = [&g](f64 feet_x, f64 feet_y) { g.teleport(feet_x, feet_y - kHeroHalfH); };
        auto keys = [&g](bool left, bool right, bool jump) {
            Controls c;
            c.left = left;
            c.right = right;
            c.jump = jump;
            g.script(c);
        };
        auto in = [&g](u64 id) {
            const std::vector<u64> now = g.areas_inside();
            return std::find(now.begin(), now.end(), id) != now.end();
        };
        auto there = [&g](u64 id) { return g.copy_with_id(id) != 0; };
        auto var = [&s](const char* name) { return s.vars().get(name).number(); };
        auto num = [](f64 x) {
            char b[32];
            std::snprintf(b, sizeof b, "%.2f", x);
            return std::string(b);
        };
        auto where = [&g, num] { return " (уровень «" + g.level_id() + "», герой " + num(g.hero_x()) + ", " + num(g.hero_y()) + ")"; };
        auto at = [&g, st](f64 feet_x) { return std::fabs(g.hero_x() - feet_x) < 0.01 && std::fabs(g.hero_y() - (st->v - kHeroHalfH)) < 0.05; };
        // The author's HUD over the game, as the player reads it.
        auto hud = [&s](const char* id) {
            Rml::Element* e = s.find_element(id);
            return e ? std::string(e->GetInnerRML()) : std::string("нет");
        };
        auto hud_says = [hud](f64 score, f64 coins, f64 hearts) {
            auto n = [](f64 x) { return std::to_string(static_cast<i64>(x)); };
            return hud("pl-score") == "Очки: " + n(score) && hud("pl-coins") == "Монеты: " + n(coins) && hud("pl-hearts") == "Сердца: " + n(hearts);
        };
        auto hud_text = [hud] { return "«" + hud("pl-score") + "», «" + hud("pl-coins") + "», «" + hud("pl-hearts") + "»"; };
        auto ticks = [&g] { return g.sim_stats() ? g.sim_stats()->ticks : 0u; };

        // Runs right from where the hero is, takes what lies on the way, and jumps when it is plat::kJumpAhead before
        // an enemy standing at x; done once it is beaten (the hero stops), the checks after that left to the step.
        auto run_and_stomp = [&g, st, keys](f64 x, u32 f) {
            if (f == 0) {
                st->phase = 0;
                st->stomps0 = g.stomps();
                st->top = 1e9;
            }
            if (st->phase == 0) {
                const bool jump = g.hero_x() >= x - plat::kJumpAhead && g.on_ground();
                keys(false, true, jump);
                if (jump) st->phase = 1;
                return false;
            }
            if (st->phase == 1) {
                keys(false, true, false);
                if (g.stomps() == st->stomps0) return false;
                st->phase = 2;
                st->at = f;
                st->y0 = g.hero_y();
                g.script(Controls{});
            }
            st->top = std::min(st->top, g.hero_y());
            return true;
        };

        if (edges) {
            // The same game in a third process; «Холм» written again with its spawn point where there is no floor
            // (x −20.5): a place to put the hero back in that will not do.
            steps_.push_back({"края: та же игра, точка появления «Холма» там, где нет пола", 40, [&s, &g, this, st](u32 f) {
                if (f < 5) return false;
                std::error_code ec;
                FORGE_INFO("рабочая папка: %s", path_to_utf8(fs::current_path(ec)).c_str());
                check(fs::equivalent(s.game_dir(), plat::root() / "data", ec), "игра читает данные из " + path_to_utf8(s.game_dir()));
                check(s.data_errors().empty(), "данные игры читаются без ошибок");
                logic::Logic links;
                std::string why;
                check(links.load(s.game_dir() / "logic.json", &why), "связи игры читаются: " + why);
                const auto gem = std::find_if(links.links.begin(), links.links.end(), [](const logic::Link& l) { return l.id == plat::kGemLink; });
                check(gem != links.links.end() && gem->a == "hero" && gem->verb == "collect" && gem->b == "gem",
                      "в logic.json связь «Герой собирает Самоцвет»");
                check(plat::write_levels(s.game_dir(), st->v, why, -20.5), "уровни записаны: " + why);
                g.set_level({});
                check(s.new_game(), "новая игра");
                return true;
            }});
            // A real link «собирает»: the hero runs to the «Самоцвет»; the link's touch (1.4 tiles round it) meets it
            // before the hero's own taking (its box) does: the link's sound, not the coins'.
            steps_.push_back({"«Герой собирает Самоцвет»: его очки и монеты один раз", 90, [&s, &g, this, st, put, keys, there, var](u32 f) {
                if (f < 20) return false; // the new game laid out
                if (f == 20) {
                    check(g.level_id() == "level" && g.score() == 0, "«Луг», 0 очков");
                    st->score0 = g.score();
                    st->coins0 = var("inv.coins");
                    st->coins_cue = g.sounds().played(Cue::Coins);
                    st->pickup_cue = g.sounds().played(Cue::Pickup);
                    st->at = 0;
                    check(g.spawn_copy("gem", plat::kGemX, st->v, plat::kGem) != 0, "«Самоцвет» на «Луге»");
                    return false;
                }
                if (f == 22) {
                    put(plat::kGemX - 4, st->v);
                    keys(false, true, false);
                }
                if (f < 23) return false;
                if (!st->at) {
                    if (there(plat::kGem)) {
                        if (f < 70) return false;
                        check(false, "«Самоцвет» не собран");
                        return true;
                    }
                    g.script(Controls{});
                    st->at = f;
                    check(g.score() == st->score0 + 30 && var("inv.coins") == st->coins0 + 2,
                          "«Самоцвет» дал свои 30 очков и 2 монеты: очки " + std::to_string(g.score()) + ", монеты " +
                              std::to_string(var("inv.coins")));
                    check(g.sounds().played(Cue::Pickup) == st->pickup_cue + 1 && g.sounds().played(Cue::Coins) == st->coins_cue,
                          "собрала его связь (её звук), а не касание (звук монет)");
                    check(s.vars().get("hero.score").number() == 30, "hero.score: " + s.vars().get("hero.score").text());
                    return false;
                }
                if (f < st->at + 30) return false;
                check(g.score() == st->score0 + 30 && var("inv.coins") == st->coins0 + 2 && g.sounds().played(Cue::Pickup) == st->pickup_cue + 1,
                      "и через полсекунды столько же: выдача одна");
                return true;
            }});
            steps_.push_back({"«Холм» и обратно: «Самоцвета» нет, очки не прибавились", 40, [&g, this, st, there, var, at, where](u32 f) {
                if (f == 0) {
                    g.go_to("hill", area_thing_id(plat::kEntry));
                    return false;
                }
                if (f == 5) {
                    check(g.level_id() == "hill", "на «Холме»" + where());
                    g.go_to("level");
                    return false;
                }
                if (f < 12) return false;
                check(g.level_id() == "level" && at(0.5), "снова на «Луге»" + where());
                check(!there(plat::kGem) && g.score() == st->score0 + 30 && var("inv.coins") == st->coins0 + 2,
                      "«Самоцвета» нет, очки и монеты те же: " + std::to_string(g.score()));
                return true;
            }});
            steps_.push_back({"сохранение и загрузка: «Самоцвета» нет, очки не прибавились", 40, [&s, &g, this, st, there, var, where](u32 f) {
                if (f == 0) {
                    check(s.save("самоцвет", "Самоцвет"), "сохранено");
                    return false;
                }
                if (f == 3) {
                    check(s.load("самоцвет"), "сохранение загружается");
                    return false;
                }
                if (f < 15) return false;
                check(g.level_id() == "level", "на «Луге»" + where());
                check(!there(plat::kGem) && g.score() == st->score0 + 30 && var("inv.coins") == st->coins0 + 2,
                      "«Самоцвета» нет, очки и монеты те же: " + std::to_string(g.score()));
                return true;
            }});
            // The frame a game ends in does nothing after the end: what lies under the hero stays there, the sounds of
            // objects stopped by the end do not start again.
            steps_.push_back({"кадр поражения: «Монетка» под героем цела, звук «Гудящего зверька» не начат снова", 200,
                              [&s, &g, this, st, put, there, var, num](u32 f) {
                if (f == 0) {
                    g.script(Controls{});
                    put(94.5, st->v);
                    check(g.spawn_copy("hum", 96.5, st->v, plat::kHum) != 0 && g.spawn_copy("coin", plat::kDropX, st->v, plat::kEndCoin) != 0 &&
                              g.spawn_copy("hedgehog", plat::kDropX, st->v, plat::kEndHedge) != 0,
                          "«Гудящий зверёк», «Монетка» и «Ёж» на «Луге»");
                    g.hurt(g.hearts() - 1);
                    st->at = 0;
                    return false;
                }
                if (!st->at) {
                    if (g.safe_time() > 0 || g.sounds().loops() == 0) {
                        if (f < 150) return false;
                        check(false, "не дождался: неуязвимость " + num(g.safe_time()) + ", звуков рядом " + std::to_string(g.sounds().loops()));
                        return true;
                    }
                    check(g.hearts() == 1 && g.sounds().loops() == 1, "одно сердце, звук «Гудящего зверька» звучит");
                    st->at = f;
                    st->score0 = g.score();
                    st->coins0 = var("inv.coins");
                    st->coins_cue = g.sounds().played(Cue::Coins);
                    st->named0 = g.sounds().played_named();
                    put(plat::kDropX, st->v); // into «Ёж» and onto «Монетка» at once
                    return false;
                }
                if (!st->last) {
                    if (!g.lost()) {
                        if (f < st->at + 5) return false;
                        check(false, "касание «Ежа» не окончило игру");
                        return true;
                    }
                    st->last = f;
                    check(g.endings() == 1 && g.hearts() == 0, "кадр окончил игру поражением");
                    check(there(plat::kEndCoin) && g.score() == st->score0 && var("inv.coins") == st->coins0 && g.sounds().played(Cue::Coins) == st->coins_cue,
                          "в этом кадре «Монетка» не подобрана: очки " + std::to_string(g.score()) + ", монеты " + std::to_string(var("inv.coins")));
                    check(g.sounds().loops() == 0, "звук «Гудящего зверька» снят и в этом кадре не начат снова: " + std::to_string(g.sounds().loops()));
                    return false;
                }
                if (f < st->last + 30) return false;
                check(there(plat::kEndCoin) && g.score() == st->score0 && var("inv.coins") == st->coins0 && g.sounds().loops() == 0 &&
                          g.sounds().played_named() == st->named0,
                      "и после: «Монетка» цела, звуков объектов нет");
                st->last = 0;
                press_page(s, 800, 530); // «Ещё раз»
                return true;
            }});
            steps_.push_back({"кадр победы: «Монетка» в «Финише» цела, звук не начат снова", 120, [&s, &g, this, st, put, there, var, where](u32 f) {
                if (f < 30) return false; // the new game after «Ещё раз»
                if (f == 30) {
                    check(g.running() && !g.lost() && g.endings() == 0, "«Ещё раз»: новая игра");
                    g.go_to("hill", area_thing_id(plat::kEntry));
                    st->at = 0;
                    return false;
                }
                if (f < 35) return false;
                if (f == 35) {
                    check(g.level_id() == "hill", "на «Холме»" + where());
                    put(52.5, st->v);
                    check(g.spawn_copy("hum", 51.5, st->v, plat::kWinHum) != 0 && g.spawn_copy("coin", 56.5, st->v, plat::kWinCoin) != 0,
                          "«Гудящий зверёк» у «Финиша», «Монетка» в нём");
                    return false;
                }
                if (!st->at) {
                    if (g.sounds().loops() == 0) {
                        if (f < 90) return false;
                        check(false, "звук «Гудящего зверька» не звучит");
                        return true;
                    }
                    st->at = f;
                    st->score0 = g.score();
                    st->coins0 = var("inv.coins");
                    st->coins_cue = g.sounds().played(Cue::Coins);
                    put(56.5, st->v); // into «Финиш» and onto «Монетка» at once
                    return false;
                }
                if (!g.won()) {
                    if (f < st->at + 5) return false;
                    check(false, "«Финиш» не дал победы" + where());
                    return true;
                }
                check(g.endings() == 1, "кадр окончил игру победой");
                check(there(plat::kWinCoin) && g.score() == st->score0 && var("inv.coins") == st->coins0 && g.sounds().played(Cue::Coins) == st->coins_cue,
                      "в этом кадре «Монетка» не подобрана: очки " + std::to_string(g.score()));
                check(g.sounds().loops() == 0, "звук «Гудящего зверька» снят и не начат снова: " + std::to_string(g.sounds().loops()));
                press_page(s, 800, 530); // «Ещё раз»
                return true;
            }});
            // No place to put the hero back in: where it came into the level, the level's spawn point, the game's start
            // (x 2.5) and where it last stood on the floor all under «Ряд шипов». It stays where it is, the keys its own.
            steps_.push_back({"возвращать некуда, окно поражения есть: герой стоит, где был, сердце раз в секунду, потом поражение", 400,
                              [&s, &g, this, st, at, where, num](u32 f) {
                if (f < 30) return false; // the new game after «Ещё раз»
                if (f == 30) {
                    check(g.running() && !g.won() && g.endings() == 0 && g.level_id() == "level" && at(0.5), "«Ещё раз»: новая игра на «Луге»" + where());
                    check(g.spawn_copy("spikes_row", plat::kOverX, st->v, plat::kOver) != 0,
                          "«Ряд шипов» от 0 до 3: под точкой появления (0,5) и стартом игры (2,5)");
                    st->x0 = g.hero_x();
                    st->backs0 = g.backs();
                    st->hazards0 = g.hazard_hits();
                    st->nowheres0 = g.nowheres();
                    st->hit1 = 0;
                    st->last = 0;
                    return false;
                }
                if (g.hero_x() != st->x0) {
                    check(false, "героя перенесли: " + num(st->x0) + " → " + num(g.hero_x()) + where());
                    return true;
                }
                const u32 hits = g.hazard_hits() - st->hazards0;
                if (hits > st->hit1) {
                    if (st->hit1 > 0)
                        check(f - st->last >= 59 && f - st->last <= 75, "снова, когда кончилась неуязвимость: через " + std::to_string(f - st->last) + " кадров");
                    st->hit1 = hits;
                    st->last = f;
                }
                if (!g.lost()) return false;
                check(hits == 3 && g.hearts() == 0, "три раза по сердцу: " + std::to_string(hits));
                check(g.backs() == st->backs0, "ни одного переноса: " + std::to_string(g.backs() - st->backs0));
                check(g.nowheres() == st->nowheres0 + 1, "место искалось один раз, пока герой на шипах: " + std::to_string(g.nowheres() - st->nowheres0));
                check(g.endings() == 1 && s.screens().shown("поражение"), "поражение, его окно");
                st->last = 0;
                press_page(s, 800, 530); // «Ещё раз»
                return true;
            }});
            steps_.push_back({"возвращать некуда: кнопки уводят героя с шипов, а встав на пол, он возвращается туда", 400,
                              [&g, this, st, keys, at, where](u32 f) {
                if (f < 30) return false;
                if (f == 30) {
                    check(g.running() && g.endings() == 0 && at(0.5), "«Ещё раз»: новая игра" + where());
                    check(g.spawn_copy("spikes_row", plat::kOverX, st->v, plat::kOver) != 0, "«Ряд шипов» от 0 до 3");
                    st->backs0 = g.backs();
                    st->hazards0 = g.hazard_hits();
                    st->phase = 0;
                    return false;
                }
                if (st->phase == 0) { // on them until a heart goes
                    if (g.hazard_hits() == st->hazards0) return false;
                    check(g.hearts() == 2 && g.backs() == st->backs0, "минус сердце, героя не перенесли");
                    st->phase = 1;
                    st->at = f;
                    keys(false, true, false);
                    return false;
                }
                if (st->phase == 1) { // off them by the keys
                    if (g.hero_x() < 6.5) {
                        if (f < st->at + 90) return false;
                        check(false, "кнопки не увели героя с шипов" + where());
                        return true;
                    }
                    g.script(Controls{});
                    st->phase = 2;
                    st->at = f;
                    return false;
                }
                if (st->phase == 2) { // standing on the floor; not safe any more, back onto them
                    if (f < st->at + 20 || g.safe_time() > 0) return false;
                    keys(true, false, false);
                    st->phase = 3;
                    st->at = f;
                    return false;
                }
                if (g.hazard_hits() == st->hazards0 + 1) {
                    if (f < st->at + 120) return false;
                    check(false, "шипы не ранили снова" + where());
                    return true;
                }
                g.script(Controls{});
                check(g.hearts() == 1 && g.backs() == st->backs0 + 1, "минус сердце, и героя вернули: возвратов " + std::to_string(g.backs() - st->backs0));
                check(at(3.5), "туда, где он последний раз стоял на полу (клетка 3), не на шипы" + where());
                return true;
            }});
            steps_.push_back({"из «Ямы» некуда вернуть, окно поражения есть: как последнее сердце", 200, [&s, &g, this, st, put, at, where](u32 f) {
                if (f == 0) {
                    g.hurt(-2);
                    g.go_to("hill", area_thing_id(plat::kEntry));
                    return false;
                }
                if (f < 10) return false;
                if (f == 10) {
                    check(g.level_id() == "hill" && at(1.5) && g.hearts() == 3, "на «Холме», во «Входе», три сердца" + where());
                    check(g.spawn_copy("spikes_row", plat::kOverX, st->v, plat::kOver) != 0,
                          "«Ряд шипов» под «Входом» (1,5), стартом игры (2,5) и местом, где герой стоял (1,5)");
                    put(41.5, st->v - 2); // over «Яма»
                    st->falls0 = g.falls();
                    st->backs0 = g.backs();
                    st->nowheres0 = g.nowheres();
                    return false;
                }
                if (!g.lost()) {
                    if (f < 180) return false;
                    check(false, "нет поражения" + where());
                    return true;
                }
                check(g.falls() == st->falls0 + 1 && g.backs() == st->backs0, "упал в «Яму», переноса нет");
                check(g.nowheres() == st->nowheres0 + 1, "вернуть некуда: ни «Вход», ни точка появления без пола, ни старт, ни место на шипах");
                check(g.hearts() == 0 && g.endings() == 1 && s.screens().shown("поражение"), "как последнее сердце: поражение, его окно");
                press_page(s, 800, 530); // «Ещё раз»
                return true;
            }});
            // A game without a window «при поражении» (as «Старая шахта»): the last heart wakes the hero up, with no
            // place for it where it is.
            steps_.push_back({"без окна поражения, возвращать некуда: «Герой очнулся», где стоял, кнопки ведут его", 500,
                              [&s, &g, this, st, keys, at, where, num](u32 f) {
                if (f < 30) return false;
                if (f == 30) {
                    check(g.running() && g.endings() == 0 && at(0.5), "«Ещё раз»: новая игра" + where());
                    s.screens().remove("поражение");
                    check(s.screens().endings("lose").empty(), "окна «при поражении» больше нет");
                    check(g.spawn_copy("spikes_row", plat::kOverX, st->v, plat::kOver) != 0, "«Ряд шипов» от 0 до 3");
                    st->backs0 = g.backs();
                    st->hazards0 = g.hazard_hits();
                    st->phase = 0;
                    keys(false, true, false);
                    return false;
                }
                if (st->phase == 0) { // a step along them, still on them: not at the spawn point
                    if (g.hero_x() < 1.4) return false;
                    g.script(Controls{});
                    st->phase = 1;
                    st->at = f;
                    return false;
                }
                if (st->phase == 1) {
                    if (f < st->at + 10) return false;
                    st->x0 = g.hero_x();
                    check(std::fabs(st->x0 - plat::kOverX) < 1.5 + kHeroHalfW, "герой на шипах, не в точке появления" + where());
                    st->phase = 2;
                    return false;
                }
                if (st->phase == 2) {
                    if (g.hero_x() != st->x0) {
                        check(false, "героя перенесли: " + num(st->x0) + " → " + num(g.hero_x()) + where());
                        return true;
                    }
                    if (g.hazard_hits() < st->hazards0 + 3) return false;
                    check(g.hearts() == 3 && !g.lost() && g.endings() == 0, "последнее сердце: «Герой очнулся», три сердца, игра идёт");
                    check(g.backs() == st->backs0, "ни одного переноса, и очнулся он там же: " + num(g.hero_x()));
                    keys(false, true, false);
                    st->phase = 3;
                    st->at = f;
                    return false;
                }
                if (g.hero_x() < 4) {
                    if (f < st->at + 60) return false;
                    check(false, "кнопки не ведут героя" + where());
                    return true;
                }
                g.script(Controls{});
                return true;
            }});
            steps_.push_back({"без окна поражения: из «Ямы» — туда, где герой последний раз стоял на полу", 300, [&g, this, st, put, keys, at, where](u32 f) {
                if (f == 0) {
                    g.go_to("hill", area_thing_id(plat::kEntry));
                    return false;
                }
                if (f < 10) return false;
                if (f == 10) {
                    check(g.level_id() == "hill" && at(1.5), "на «Холме», во «Входе»" + where());
                    keys(false, true, false);
                    st->phase = 0;
                    return false;
                }
                if (st->phase == 0) {
                    if (g.hero_x() < 10) return false;
                    g.script(Controls{});
                    st->phase = 1;
                    st->at = f;
                    return false;
                }
                if (st->phase == 1) {
                    if (f < st->at + 20) return false;
                    st->gx = g.hero_x();
                    check(g.spawn_copy("spikes_row", plat::kOverX, st->v, plat::kOver) != 0, "«Ряд шипов» под «Входом» и стартом игры");
                    put(41.5, st->v - 2); // over «Яма»
                    st->falls0 = g.falls();
                    st->backs0 = g.backs();
                    st->phase = 2;
                    st->at = f;
                    return false;
                }
                if (g.falls() == st->falls0) {
                    if (f < st->at + 100) return false;
                    check(false, "герой не упал в «Яму»" + where());
                    return true;
                }
                check(g.backs() == st->backs0 + 1 && at(std::floor(st->gx) + 0.5), "вернулся туда, где стоял" + where());
                check(!g.lost() && g.endings() == 0, "игра идёт");
                return true;
            }});
            // And with the last place on the floor under spikes too: from «Яма» nowhere to go back to and nowhere to
            // wake up in, and in a pit there is nothing to stand on: the game is lost without a window. The world
            // stands (the keys move nothing, no ticks), saving is refused, loading goes on.
            steps_.push_back({"без окна поражения: из «Ямы» некуда вернуть и негде очнуться — поражение, загрузка выводит", 300,
                              [&s, &g, this, st, put, keys, where, num, ticks](u32 f) {
                if (f == 0) {
                    check(g.level_id() == "hill" && !g.lost(), "на «Холме», игра идёт" + where());
                    check(g.spawn_copy("spikes_row", g.hero_x(), st->v, plat::kOverGround) != 0,
                          "«Ряд шипов» и над местом, где герой последний раз стоял на полу");
                    g.hurt(g.hearts() - 3);
                    put(41.5, st->v - 2); // over «Яма»
                    st->falls0 = g.falls();
                    st->backs0 = g.backs();
                    st->nowheres0 = g.nowheres();
                    st->at = 0;
                    return false;
                }
                if (!st->at) {
                    if (!g.lost()) {
                        if (f < 120) return false;
                        check(false, "нет поражения: сердец " + num(g.hearts()) + where());
                        return true;
                    }
                    check(g.falls() == st->falls0 + 1 && g.backs() == st->backs0, "упал в «Яму», переноса нет");
                    check(g.nowheres() == st->nowheres0 + 2,
                          "вернуть некуда и очнуться негде: искалось " + std::to_string(g.nowheres() - st->nowheres0) + " раза");
                    check(g.hearts() == 0 && g.endings() == 1 && s.screens().endings("lose").empty(), "поражение без окна, сердец 0");
                    std::string why;
                    check(!g.can_save(&why) && why == "игра окончена", "игра сохраняться не даёт: «" + why + "»");
                    st->x0 = g.hero_x();
                    st->y0 = g.hero_y();
                    st->at = f;
                    keys(false, true, true);
                    return false;
                }
                if (f < st->at + 60) {
                    if (f > st->at + 1 && ticks() != 0) {
                        check(false, "мир идёт: тиков в кадре " + std::to_string(ticks()));
                        return true;
                    }
                    return false;
                }
                if (f == st->at + 60) {
                    check(g.hero_x() == st->x0 && g.hero_y() == st->y0 && g.lost() && g.endings() == 1,
                          "герой стоит, хоть кнопки нажаты, поражение одно" + where());
                    g.script(Controls{});
                    check(s.load("самоцвет"), "сохранение загружается (F9)");
                    return false;
                }
                if (f < st->at + 75) return false;
                check(g.running() && !g.lost() && g.endings() == 0 && g.can_save(nullptr), "после загрузки игра идёт, сохранять можно");
                check(g.level_id() == "level" && g.hearts() > 0, "на «Луге», где сохранились" + where());
                return true;
            }});
            // The same with the last heart taken by «Яма» itself: no hearts, no window, nowhere to wake up in.
            steps_.push_back({"без окна поражения: последнее сердце снимает «Яма», очнуться негде — поражение", 300,
                              [&s, &g, this, st, put, at, where, num](u32 f) {
                if (f == 0) {
                    g.go_to("hill", area_thing_id(plat::kEntry));
                    return false;
                }
                if (f < 10) return false;
                if (f == 10) {
                    check(g.level_id() == "hill" && at(1.5), "на «Холме», во «Входе»" + where());
                    g.hurt(g.hearts() - 1);
                    st->at = 0;
                    return false;
                }
                if (!st->at) {
                    if (g.safe_time() > 0) return false;
                    check(g.hearts() == 1 && at(1.5), "одно сердце, неуязвимости нет, во «Входе»" + where());
                    check(g.spawn_copy("spikes_row", plat::kOverX, st->v, plat::kOver) != 0,
                          "«Ряд шипов» под «Входом» (1,5), стартом игры (2,5) и местом, где герой стоял (1,5)");
                    put(41.5, st->v - 2); // over «Яма»
                    st->falls0 = g.falls();
                    st->backs0 = g.backs();
                    st->nowheres0 = g.nowheres();
                    st->at = f;
                    return false;
                }
                if (!g.lost()) {
                    if (f < st->at + 120) return false;
                    check(false, "нет поражения: сердец " + num(g.hearts()) + where());
                    return true;
                }
                check(g.falls() == st->falls0 + 1 && g.backs() == st->backs0, "упал в «Яму», переноса нет");
                check(g.nowheres() == st->nowheres0 + 1,
                      "очнуться негде: искалось " + std::to_string(g.nowheres() - st->nowheres0) + " раз");
                check(g.hearts() == 0 && g.endings() == 1 && s.screens().endings("lose").empty(), "поражение без окна, сердец 0");
                return true;
            }});
            return;
        }
        if (continued) {
            steps_.push_back({"«Продолжить» другим процессом: «Холм», где сохранились", 40, [&s, &g, this, st, there, num, where, hud_says, hud_text](u32 f) {
                std::error_code ec;
                if (f < 5) return false;
                if (f == 5) {
                    std::vector<u8> bytes;
                    check(read_file(plat::notes(), bytes), "заметки первого процесса: " + path_to_utf8(plat::notes()));
                    const std::string text(bytes.begin(), bytes.end());
                    for (usize start = 0; start < text.size();) {
                        usize end = text.find('\n', start);
                        if (end == std::string::npos) end = text.size();
                        const std::string line = text.substr(start, end - start);
                        if (const usize sp = line.find(' '); sp != std::string::npos)
                            st->notes[line.substr(0, sp)] = std::strtod(line.c_str() + sp + 1, nullptr);
                        start = end + 1;
                    }
                    FORGE_INFO("рабочая папка: %s", path_to_utf8(fs::current_path(ec)).c_str());
                    check(fs::equivalent(s.game_dir(), plat::root() / "data", ec), "игра читает данные из " + path_to_utf8(s.game_dir()));
                    check(fs::equivalent(s.user_folder(), plat::root() / "user", ec), "файлы игрока — в " + path_to_utf8(s.user_folder()));
                    check(s.slots().list().size() == 1 && s.slots().latest() && s.slots().latest()->id == "платформер",
                          "сохранение одно — «платформер»; ни поражения, ни автосохранения в слотах");
                    check(s.continue_game(), "«Продолжить»");
                    return false;
                }
                if (f < 15) return false;
                std::map<std::string, f64>& n = st->notes;
                check(g.level_id() == "hill" && std::fabs(g.hero_x() - n["x"]) < 0.01 && std::fabs(g.hero_y() - n["y"]) < 0.05,
                      "герой на «Холме», где сохранился: " + num(n["x"]) + ", " + num(n["y"]) + where());
                check(g.score() == n["score"] && s.vars().get("inv.coins").number() == n["coins"] && g.hearts() == n["hearts"],
                      "очки, монеты, сердца — как при сохранении: " + std::to_string(g.score()));
                check(hud_says(n["score"], n["coins"], n["hearts"]), "HUD: " + hud_text());
                check(!there(plat::kCoin2) && !there(plat::kLoud) && there(plat::kSpikes),
                      "на «Холме» нет подобранной «Монетки» и побеждённого «Жука с голосом», «Ряд шипов» на месте");
                f64 bx = 0, by = 0;
                const bool back = g.back_point(bx, by);
                check(back && bx == 1.5 && std::fabs(by - st->v) < 1e-4, "точка возврата из сохранения: середина «Входа» " + num(bx) + ", " + num(by));
                check(g.safe_time() > 0.5 && !g.blinking(), "после загрузки секунда без урона, не мигает");
                check(!g.won() && !g.lost() && g.can_save(nullptr), "игра идёт, сохранять можно");
                return true;
            }});
            steps_.push_back({"«Луг» без подобранной «Монетки» и побеждённых врагов", 20, [&g, this, st, there, at, where](u32 f) {
                if (f == 0) {
                    g.go_to("level");
                    return false;
                }
                if (f == 2) {
                    check(g.level_id() == "level" && at(0.5), "на «Луге», в точке появления" + where());
                    check(!there(plat::kCoin) && !there(plat::kBeetle) && there(plat::kHedge) && there(plat::kPairA) && there(plat::kSpiny),
                          "«Монетки» и «Жука» нет, «Ёж» и нетронутые враги на месте");
                    g.go_to("hill", area_thing_id(plat::kEntry));
                }
                if (f < 5) return false;
                check(g.level_id() == "hill" && at(1.5), "снова на «Холме», во «Входе»" + where());
                return true;
            }});
            steps_.push_back({"«Бродяга» ходит и стучит шагами", 100, [&g, this, st, put](u32 f) {
                if (f == 0) {
                    put(52.5, st->v); // near enough to hear it (its «Слышно на»: 16 tiles)
                    check(g.spawn_copy("wanderer", plat::kWandererX, st->v, plat::kWanderer) != 0, "«Бродяга» в загоне у «Финиша»");
                    return false;
                }
                const flecs::entity_t w = g.copy_with_id(plat::kWanderer);
                if (f == 5) {
                    st->wx = g.critter_x(w);
                    st->named0 = g.sounds().played_named();
                }
                if (f < 90) return false;
                const f64 x = g.critter_x(w);
                check(w && !std::isnan(x) && std::fabs(x - st->wx) > 0.3, "«Бродяга» ходит: " + std::to_string(st->wx) + " → " + std::to_string(x));
                check(g.sounds().played_named() > st->named0, "его шаги звучат: " + std::to_string(g.sounds().played_named() - st->named0));
                return true;
            }});
            steps_.push_back({"«Финиш»: победа", 80, [&s, &g, this, st, keys, var](u32 f) {
                if (f == 0) {
                    st->score0 = g.score();
                    st->saves = Files{};
                }
                if (!g.won()) {
                    keys(false, true, false);
                    return false;
                }
                check(g.endings() == 1 && !g.lost(), "игра окончена победой");
                check(s.vars().get("game.result").text() == "победа", "game.result: «" + s.vars().get("game.result").text() + "»");
                check(s.screens().shown("победа") && !s.screens().shown("поражение"), "окно «победа» показано, «поражение» — нет");
                check(g.score() == st->score0 && var("hero.hearts") > 0, "очки те же, сердца есть");
                return true;
            }});
            steps_.push_back({"победа: мир стоит, ключи никого не ведут, окно одно, сохранить нельзя", 90, [&s, &g, this, st, keys, saves, ticks, hud](u32 f) {
                const flecs::entity_t w = g.copy_with_id(plat::kWanderer);
                if (f == 0) {
                    st->x0 = g.hero_x();
                    st->y0 = g.hero_y();
                    st->wx = g.critter_x(w);
                    st->named0 = g.sounds().played_named();
                    st->saves = saves();
                }
                keys(false, true, true);
                if (f > 1 && ticks() != 0) check(false, "мир идёт: тиков в кадре " + std::to_string(ticks()));
                if (f < 70) return false;
                check(g.hero_x() == st->x0 && g.hero_y() == st->y0, "герой стоит, хоть кнопки нажаты");
                check(g.critter_x(w) == st->wx, "«Бродяга» стоит");
                check(g.sounds().played_named() == st->named0, "его шаги не звучат");
                check(g.endings() == 1 && s.screens().shown("победа"), "победа одна, окно показано один раз");
                check(!s.screens().pauses(), "окно победы мир не ставит на паузу, а мир всё равно стоит");
                check(s.screens().place_music().empty(), "музыки места нет");
                check(hud("pl-win-text") == "Итог: победа", "окно: «" + hud("pl-win-text") + "»");
                std::string why;
                check(!g.can_save(&why) && why == "игра окончена", "игра сохраняться не даёт: «" + why + "»");
                check(!s.save("после-победы", "После победы") && !s.save("autosave", "Автосохранение", true), "ни сохранения, ни автосохранения");
                check(saves() == st->saves, "слоты те же, байт в байт");
                g.script(Controls{});
                press_page(s, 800, 530); // «Ещё раз»
                return true;
            }});
            steps_.push_back({"«Ещё раз» после победы: чистая новая игра", 60, [&s, &g, this, st, there, at, where, hud_says, hud_text](u32 f) {
                if (f < 40) return false;
                std::error_code ec;
                check(g.running() && !g.won() && !g.lost() && g.endings() == 0, "новая игра идёт");
                check(g.level_id() == "level" && at(0.5), "на «Луге», в точке появления" + where());
                check(g.hearts() == 3 && g.score() == 0 && s.vars().get("inv.coins").number() == 0 && !s.vars().has("game.result"),
                      "три сердца, 0 очков, 0 монет, итога нет");
                check(hud_says(0, 0, 3), "HUD: " + hud_text());
                check(there(plat::kCoin) && there(plat::kBeetle) && there(plat::kHedge), "«Монетка» и «Жук» снова на «Луге»");
                check(!fs::exists(s.slots().session() / "levels", ec), "уровней прошлой игры нет (session/levels)");
                check(!s.screens().shown("победа"), "окно победы закрыто");
                return true;
            }});
            return;
        }

        steps_.push_back({"игра платформера в своей папке: уровни «Луг» и «Холм»", 30, [&s, &g, this, st](u32 f) {
            if (f < 5) return false;
            std::error_code ec;
            check(fs::equivalent(s.game_dir(), plat::root() / "data", ec), "игра читает данные из " + path_to_utf8(s.game_dir()));
            check(fs::equivalent(s.user_folder(), plat::root() / "user", ec), "файлы игрока — в " + path_to_utf8(s.user_folder()));
            check(s.data_errors().empty(), "данные игры читаются без ошибок");
            const forge::level::LevelList list = forge::level::read_levels(s.game_dir());
            check(list.from_file && list.levels.size() == 2 && list.start == "level", "levels.json: «Луг» (старт) и «Холм»");
            check(s.slots().list().empty(), "сохранений ещё нет");
            std::string why;
            check(plat::write_levels(s.game_dir(), st->v, why), "уровни записаны: " + why);
            g.set_level({});
            check(s.new_game(), "новая игра");
            return true;
        }});
        steps_.push_back({"новая игра: «Луг», три сердца, 0 очков, всё на местах", 40, [&s, &g, this, st, there, at, where, num, hud_says, hud_text](u32 f) {
            if (f < 20) return false;
            std::error_code ec;
            check(g.level_id() == "level" && at(0.5), "герой в точке появления «Луга»" + where());
            check(g.hearts() == 3 && g.score() == 0 && s.vars().get("inv.coins").number() == 0, "три сердца, 0 очков, 0 монет");
            check(s.screens().shown("платформер_hud") && hud_says(0, 0, 3), "HUD автора: " + hud_text());
            check(g.safe_time() > 0.5 && !g.blinking(), "в начале секунда без урона, герой не мигает");
            f64 bx = 0, by = 0;
            const bool back = g.back_point(bx, by);
            check(back && bx == 0.5 && std::fabs(by - st->v) < 1e-4, "точка возврата — старт: " + num(bx) + ", " + num(by));
            for (const u64 id : {plat::kCoin, plat::kBeetle, plat::kHedge, plat::kPairA, plat::kPairB, plat::kBeside, plat::kSpiny})
                check(there(id), "копия " + std::to_string(id & 0xfff) + " на «Луге»");
            check(!g.won() && !g.lost() && g.can_save(nullptr), "игра идёт, сохранять можно");
            check(!fs::exists(s.slots().session() / "levels", ec), "других уровней ещё нет (session/levels)");
            return true;
        }});
        // Walking and jumping by the keys: a coin on the way, a beetle jumped onto.
        auto coin_and_beetle = [&s, &g, this, st, there, run_and_stomp, hud_says, hud_text](const char* title) {
            steps_.push_back({title, 320, [&s, &g, this, st, there, run_and_stomp, hud_says, hud_text](u32 f) {
                if (f == 0) {
                    st->coins_cue = g.sounds().played(Cue::Coins);
                    st->crate_cue = g.sounds().played(Cue::Crate);
                    st->score0 = g.score();
                    st->at = 0;
                    st->hit1 = 0;
                }
                if (st->phase < 2 || f == 0) {
                    if (!run_and_stomp(plat::kBeetleX, f)) {
                        // The coin as the hero runs over it.
                        if (!there(plat::kCoin) && st->hit1 != 1) {
                            st->hit1 = 1;
                            check(s.vars().get("inv.coins").number() == 1 && g.score() == st->score0 + 10,
                                  "«Монетка»: +1 монета, +10 очков: " + std::to_string(g.score()));
                            check(g.sounds().played(Cue::Coins) == st->coins_cue + 1, "звук монеты один раз");
                        }
                        return false;
                    }
                }
                st->top = std::min(st->top, g.hero_y());
                if (f < st->at + 45) return false;
                st->hit1 = 0;
                check(!there(plat::kCoin) && !there(plat::kBeetle), "«Монетки» и «Жука» больше нет");
                check(g.stomps() == st->stomps0 + 1 && g.score() == st->score0 + 110, "«Жук» побеждён один раз: +100 очков, всего " +
                                                                                     std::to_string(g.score()));
                check(g.hearts() == 3 && g.enemy_hits() == 0, "сердца не сняты: прыжок сверху не ранит");
                check(g.sounds().played(Cue::Coins) == st->coins_cue + 1 && g.sounds().played(Cue::Crate) == st->crate_cue + 1,
                      "звук монеты и удара — по одному разу, не каждый тик");
                check(st->top < st->y0 - 1.0, "после победы — отскок вверх: " + std::to_string(st->y0 - st->top) + " клетки");
                check(hud_says(st->score0 + 110, 1, 3), "HUD: " + hud_text());
                return true;
            }});
        };
        coin_and_beetle("кнопками: «Монетка» по пути и прыжок на «Жука»");
        steps_.push_back({"два «Жука» рядом под героем: два раза очки, один отскок, без урона", 90, [&g, this, st, put, there](u32 f) {
            if (f == 0) {
                st->stomps0 = g.stomps();
                st->score0 = g.score();
                st->top = 1e9;
                st->at = 0;
                put(plat::kPairX, st->v - 3);
                return false;
            }
            if (g.stomps() > st->stomps0) {
                if (!st->at) st->at = f, st->y0 = g.hero_y();
                st->top = std::min(st->top, g.hero_y());
            }
            if (f < 70) return false;
            check(g.stomps() == st->stomps0 + 2 && !there(plat::kPairA) && !there(plat::kPairB), "оба побеждены");
            check(g.score() == st->score0 + 200, "очки два раза: +200, всего " + std::to_string(g.score()));
            const f64 rise = st->y0 - st->top;
            check(rise > 1.0 && rise < 1.75, "отскок один (11 клеток в секунду, около 1,5 клетки вверх): " + std::to_string(rise));
            check(g.hearts() == 3 && g.enemy_hits() == 0, "урона нет");
            return true;
        }});
        // The stomp's tick also met «Ёж»: it does not hurt while the hero stays in it, and hurts once the hero has left
        // it and comes back.
        steps_.push_back({"прыжок на «Жука» рядом с «Ежом»: победа; «Ёж» не ранит, пока герой в нём", 240, [&g, this, st, put, there](u32 f) {
            if (f == 0) {
                g.script(Controls{});
                st->stomps0 = g.stomps();
                st->score0 = g.score();
                st->hits0 = g.enemy_hits();
                st->at = 0;
                st->hit1 = 0;
                put(plat::kBesideX + 0.3, st->v - 3);
                return false;
            }
            if (!st->at) {
                if (g.stomps() == st->stomps0) {
                    if (f < 60) return false;
                    check(false, "«Жук» не побеждён");
                    return true;
                }
                st->at = f;
                check(g.stomps() == st->stomps0 + 1 && !there(plat::kBeside) && there(plat::kSpiny), "«Жук» побеждён, «Ёж» на месте");
                check(g.score() == st->score0 + 100, "очки: +100, всего " + std::to_string(g.score()));
                check(g.spared() == 1, "«Ёж» коснулся героя в тике победы и не ранит: " + std::to_string(g.spared()));
                put(plat::kSpinyX - 0.2, st->v); // stays in «Ёж»
                return false;
            }
            const u32 inside = f - st->at;
            if (inside < 80) {
                if (g.enemy_hits() != st->hits0) {
                    check(false, "«Ёж» ранил героя, который в нём с тика победы: через " + std::to_string(inside) + " кадров");
                    return true;
                }
                if (inside == 79) {
                    check(g.spared() == 1 && g.hearts() == 3, "80 кадров в «Еже»: урона нет, «Ёж» всё ещё не ранит");
                    put(plat::kSpinyX - 3, st->v); // out of it
                }
                return false;
            }
            if (inside == 81) {
                check(g.spared() == 0 && g.enemy_hits() == st->hits0, "герой вышел из «Ежа»: он снова ранит");
                put(plat::kSpinyX - 0.2, st->v); // and back in
                return false;
            }
            if (!st->hit1) {
                if (g.enemy_hits() == st->hits0 + 1) {
                    st->hit1 = f;
                    check(inside <= 84 && g.hearts() == 2, "снова в «Еже»: минус сердце сразу, через " + std::to_string(inside - 81) + " кадров");
                    put(plat::kSpinyX - 3, st->v); // out of it again,
                    g.hurt(-1);                    // and the next steps count from three hearts
                } else if (inside > 90) {
                    check(false, "снова в «Еже», но урона нет");
                    return true;
                }
                return false;
            }
            if (g.safe_time() > 0) return false; // the next step counts from no safe time
            check(g.hearts() == 3 && g.enemy_hits() == st->hits0 + 1, "одна рана, сердце возвращено: " + std::to_string(g.hearts()));
            return true;
        }});
        steps_.push_back({"сбоку «Ёж»: минус сердце, отталкивание, секунда без урона, потом снова", 220, [&g, this, st, put, keys](u32 f) {
            if (f == 0) {
                st->hits0 = g.enemy_hits();
                st->hit1 = 0;
                put(34.5, st->v);
                return false;
            }
            keys(false, st->hit1 == 0 || g.enemy_hits() < st->hits0 + 2, false);
            if (g.enemy_hits() == st->hits0 + 1 && !st->hit1) {
                st->hit1 = f;
                st->x0 = g.hero_x();
                check(g.hearts() == 2, "минус сердце: " + std::to_string(g.hearts()));
                check(g.safe_time() > 0.9 && g.blinking(), "неуязвимость, герой мигает");
            }
            if (st->hit1 && f == st->hit1 + 4) check(g.hero_x() < st->x0 - 0.1, "отталкивание назад: " + std::to_string(st->x0) + " → " + std::to_string(g.hero_x()));
            if (st->hit1 && f == st->hit1 + 45) {
                check(std::fabs(g.hero_x() - plat::kHedgeX) < 0.73, "герой всё ещё у «Ежа», прижат к стене: " + std::to_string(g.hero_x()));
                check(g.enemy_hits() == st->hits0 + 1 && g.hearts() == 2, "в течение секунды сердце не снимается");
            }
            if (g.enemy_hits() < st->hits0 + 2) return false;
            const u32 after = f - st->hit1;
            check(after >= 59 && after <= 75, "снова — когда кончилась неуязвимость: через " + std::to_string(after) + " кадров");
            check(g.hearts() == 1, "ещё минус сердце: " + std::to_string(g.hearts()));
            g.script(Controls{});
            return true;
        }});
        steps_.push_back({"враг падает на голову: снизу урон, враг жив; последнее сердце — поражение", 160, [&s, &g, this, st, put, there](u32 f) {
            if (f == 0) {
                put(plat::kDropX, st->v);
                st->hits0 = g.enemy_hits();
                return false;
            }
            if (f == 70) check(g.spawn_copy("beetle", plat::kDropX, st->v - 5, plat::kDropper) != 0, "«Жук» над головой героя");
            if (!g.lost()) return false;
            check(g.enemy_hits() == st->hits0 + 1 && g.hearts() == 0, "снизу — урон: сердец 0");
            check(there(plat::kDropper), "упавший «Жук» жив: снизу его не победить");
            check(g.endings() == 1 && s.vars().get("game.result").text() == "поражение", "поражение: game.result «" +
                                                                                           s.vars().get("game.result").text() + "»");
            check(s.screens().shown("поражение") && !s.screens().shown("победа"), "окно «поражение» показано");
            return true;
        }});
        steps_.push_back({"поражение: мир стоит, окно одно, сохранить нельзя", 90, [&s, &g, this, st, keys, saves, ticks, hud](u32 f) {
            if (f == 0) {
                st->x0 = g.hero_x();
                st->y0 = g.hero_y();
                g.position_of(g.copy_with_id(plat::kDropper), st->wx, st->top);
                st->saves = saves();
            }
            keys(false, true, true);
            if (f > 1 && ticks() != 0) check(false, "мир идёт: тиков в кадре " + std::to_string(ticks()));
            if (f < 70) return false;
            f64 x = 0, y = 0;
            g.position_of(g.copy_with_id(plat::kDropper), x, y);
            check(g.hero_x() == st->x0 && g.hero_y() == st->y0 && x == st->wx && y == st->top, "герой и «Жук» стоят, хоть кнопки нажаты");
            check(g.endings() == 1 && s.screens().shown("поражение") && s.screens().pauses(), "поражение одно, окно показано и ставит паузу");
            check(hud("pl-lose-text") == "Итог: поражение", "окно: «" + hud("pl-lose-text") + "»");
            std::string why;
            check(!g.can_save(&why) && why == "игра окончена", "игра сохраняться не даёт: «" + why + "»");
            check(!s.save("после-поражения", "После поражения") && !s.save("autosave", "Автосохранение", true), "ни сохранения, ни автосохранения");
            check(saves() == st->saves && s.slots().list().empty(), "слотов нет, как и было");
            g.script(Controls{});
            press_page(s, 800, 530); // «Ещё раз», where the page's fit shows it
            return true;
        }});
        steps_.push_back({"«Ещё раз»: чистая новая игра", 60, [&s, &g, this, st, there, at, where, hud_says, hud_text](u32 f) {
            if (f < 40) return false;
            std::error_code ec;
            check(g.running() && !g.lost() && g.endings() == 0 && !s.screens().shown("поражение"), "новая игра идёт, окно закрыто");
            check(g.level_id() == "level" && at(0.5), "на «Луге», в точке появления" + where());
            check(g.hearts() == 3 && g.score() == 0 && s.vars().get("inv.coins").number() == 0 && !s.vars().has("game.result"),
                  "три сердца, 0 очков, 0 монет, итога нет");
            check(hud_says(0, 0, 3), "HUD: " + hud_text());
            for (const u64 id : {plat::kCoin, plat::kBeetle, plat::kHedge, plat::kPairA, plat::kPairB, plat::kBeside, plat::kSpiny})
                check(there(id), "копия " + std::to_string(id & 0xfff) + " снова на «Луге»");
            check(!there(plat::kDropper), "упавшего «Жука» нет: его ставила прошлая игра");
            check(!fs::exists(s.slots().session() / "levels", ec), "уровней прошлой игры нет (session/levels)");
            check(g.stomps() == 0 && g.enemy_hits() == 0, "счёт побед и ран с начала");
            return true;
        }});
        coin_and_beetle("снова кнопками: «Монетка» и «Жук»");
        steps_.push_back({"по «Тропе» на «Холм»: во «Вход»", 80, [&s, &g, this, st, put, keys, in, at, where, num, hud_says, hud_text](u32 f) {
            if (f == 0) {
                put(124.5, st->v);
                st->travels0 = g.travels();
                return false;
            }
            if (g.level_id() != "hill") {
                keys(false, true, false);
                return false;
            }
            g.script(Controls{});
            check(g.travels() == st->travels0 + 1 && at(1.5) && in(plat::kEntry), "герой на «Холме», в середине «Входа»" + where());
            f64 bx = 0, by = 0;
            const bool back = g.back_point(bx, by);
            check(back && bx == 1.5 && std::fabs(by - st->v) < 1e-4, "точка возврата — место прибытия: " + num(bx) + ", " + num(by));
            check(g.hearts() == 3 && g.score() == 110 && s.vars().get("inv.coins").number() == 1, "сердца, очки, монеты перешли с героем");
            check(g.safe_time() > 0.9 && !g.blinking(), "после прихода секунда без урона, не мигает");
            check(hud_says(110, 1, 3), "HUD: " + hud_text());
            return true;
        }});
        steps_.push_back({"«Холм»: «Монетка» и «Жук с голосом» — его «Удар»", 320, [&s, &g, this, st, there, run_and_stomp](u32 f) {
            if (f == 0) {
                st->named0 = g.sounds().played_named();
                st->crate_cue = g.sounds().played(Cue::Crate);
                st->score0 = g.score();
            }
            if (st->phase < 2 || f == 0)
                if (!run_and_stomp(plat::kLoudX, f)) return false;
            if (f < st->at + 40) return false;
            check(!there(plat::kCoin2) && !there(plat::kLoud), "«Монетки» и «Жука с голосом» нет");
            check(g.score() == st->score0 + 110 && s.vars().get("inv.coins").number() == 2, "+10 и +100 очков: " + std::to_string(g.score()));
            check(g.sounds().played_named() >= st->named0 + 1 && g.sounds().played(Cue::Crate) == st->crate_cue,
                  "звучит его «Удар» (удар.wav), не обычный");
            check(g.hearts() == 3, "сердца не сняты");
            return true;
        }});
        steps_.push_back({"«Ряд шипов»: минус сердце, назад во «Вход», скорость 0, «Вход» не увёл", 120, [&g, this, st, put, keys, in, at, where](u32 f) {
            if (f == 0) {
                st->hazards0 = g.hazard_hits();
                st->backs0 = g.backs();
                st->travels0 = g.travels();
                st->at = 0;
                put(24.5, st->v);
                return false;
            }
            if (!st->at) {
                if (g.hazard_hits() == st->hazards0) {
                    keys(false, true, false);
                    return false;
                }
                g.script(Controls{});
                st->at = f;
                check(g.hearts() == 2 && g.backs() == st->backs0 + 1, "минус сердце, и герой возвращён: сердец " + std::to_string(g.hearts()));
                check(at(1.5) && g.level_id() == "hill" && in(plat::kEntry), "в точке возврата, во «Входе»" + where());
                check(g.safe_time() > 0.9 && g.blinking(), "неуязвимость, мигает");
            }
            if (f < st->at + 20) return false;
            check(at(1.5) && g.on_ground(), "стоит в точке возврата: скорость обнулена" + where());
            check(g.level_id() == "hill" && g.travels() == st->travels0, "«Вход» не увёл на «Луг»: возврат — не вход в зону");
            return true;
        }});
        steps_.push_back({"шипы в неуязвимости: назад, сердце не снято", 20, [&g, this, st, put, at, where](u32 f) {
            if (f == 0) {
                st->backs0 = g.backs();
                put(plat::kSpikesX, st->v);
                return false;
            }
            if (f < 3) return false;
            check(g.backs() == st->backs0 + 1 && at(1.5), "назад во «Вход»" + where());
            check(g.hearts() == 2 && g.hazard_hits() == st->hazards0 + 1, "сердце не снято: " + std::to_string(g.hearts()));
            return true;
        }});
        steps_.push_back({"«Яма»: минус сердце, назад", 180, [&g, this, st, put, keys, at, in, where](u32 f) {
            if (f < 70) return false; // the safe second over
            if (f == 70) {
                st->falls0 = g.falls();
                st->backs0 = g.backs();
                put(36.5, st->v);
                return false;
            }
            if (g.falls() == st->falls0) {
                keys(false, true, false);
                return false;
            }
            g.script(Controls{});
            check(g.hearts() == 1 && g.backs() == st->backs0 + 1, "минус сердце, назад: сердец " + std::to_string(g.hearts()));
            check(at(1.5) && g.level_id() == "hill" && in(plat::kEntry), "во «Входе»" + where());
            return true;
        }});
        steps_.push_back({"из «Входа» и снова в него: на «Луг», в «Возврат»", 160, [&g, this, st, keys, there, at, where, num](u32 f) {
            if (f == 0) {
                st->travels0 = g.travels();
                st->phase = 0;
            }
            if (g.level_id() == "hill") {
                if (g.hero_x() > 4.5) st->phase = 1;
                keys(st->phase == 1, st->phase == 0, false);
                return false;
            }
            g.script(Controls{});
            check(g.travels() == st->travels0 + 1 && at(119.5), "на «Луге», в середине «Возврата»" + where());
            f64 bx = 0, by = 0;
            const bool back = g.back_point(bx, by);
            check(back && bx == 119.5 && std::fabs(by - st->v) < 1e-4, "точка возврата «Луга» — «Возврат»: " + num(bx) + ", " + num(by));
            check(!there(plat::kCoin) && !there(plat::kBeetle), "подобранной «Монетки» и побеждённого «Жука» нет");
            check(there(plat::kHedge) && there(plat::kPairA) && there(plat::kBeside), "нетронутые враги на месте");
            check(g.hearts() == 1 && g.score() == 220, "сердце одно, очков 220");
            return true;
        }});
        steps_.push_back({"снова на «Холм»: подобранное и побеждённое не вернулись, счёт не удвоился", 80, [&s, &g, this, keys, there, at, where, hud_says, hud_text](u32) {
            if (g.level_id() != "hill") {
                keys(false, true, false);
                return false;
            }
            g.script(Controls{});
            check(at(1.5), "на «Холме», во «Входе»" + where());
            check(!there(plat::kCoin2) && !there(plat::kLoud) && there(plat::kSpikes), "«Монетки» и «Жука с голосом» нет, шипы на месте");
            check(g.score() == 220 && s.vars().get("inv.coins").number() == 2 && g.hearts() == 1, "очки 220, монет 2, сердце одно");
            check(hud_says(220, 2, 1), "HUD: " + hud_text());
            return true;
        }});
        steps_.push_back({"сохранение на «Холме»", 30, [&s, &g, this, st, var](u32 f) {
            if (f < 10) return false;
            check(s.save("платформер", "Платформер"), "игра сохраняется");
            std::vector<u8> bytes;
            read_file(s.slots().folder("платформер") / "hero.json", bytes);
            const std::string hero(bytes.begin(), bytes.end());
            check(hero.find("\"back\": true") != std::string::npos && hero.find("\"back_x\": 1.5") != std::string::npos,
                  "hero.json: точка возврата: " + hero);
            check(s.slots().list().size() == 1, "сохранение одно");
            std::string text;
            char line[96];
            for (const auto& [key, value] : std::initializer_list<std::pair<const char*, f64>>{
                     {"x", g.hero_x()}, {"y", g.hero_y()}, {"score", g.score()}, {"coins", var("inv.coins")}, {"hearts", g.hearts()}}) {
                std::snprintf(line, sizeof line, "%s %.17g\n", key, value);
                text += line;
            }
            check(write_file_atomic(plat::notes(), {reinterpret_cast<const u8*>(text.data()), text.size()}), "заметки для второго процесса");
            return true;
        }});
    }

    // The level editor's «Свет» as the game plays it: games/examples/light/level, made in the editor with the mouse
    // and the panel (its self-test makes it again and compares), read from the files the way «Играть отсюда» starts
    // it (in a package from the package's own data/examples). An old level keeps the light it always had; the
    // example's hour (21:30) and its two sources are the author's. The game's frame and the editor's view «как в
    // игре» are made with the same camera, size (the game's, --size) and world under the light's grid, the hero's
    // own light put out: their light alone is compared pixel by pixel, again in a window of another size, and the
    // sources' light where it reaches (nothing past the radius). A copy at noon has a brighter sky and the same
    // lamps; a save keeps light.json; links «Только ночью» go by the level's hour; the example's files stay as they
    // were.
    struct Shot {
        std::vector<u8> full, light; // the frame, and its light alone (white under it)
    };
    // The game's world (or the editor's view) and its light, w × h, with a command buffer of its own.
    static bool shoot(SDL_GPUDevice* device, u32 w, u32 h, const std::function<void(SDL_GPUCommandBuffer*, SDL_GPUTexture*)>& frame,
                      render::LightRenderer& lights, Shot& out) {
        SDL_GPUTexture* full = render::create_render_target(device, w, h);
        SDL_GPUTexture* light = render::create_render_target(device, w, h);
        bool ok = false;
        if (full && light) {
            SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
            frame(cmd, full);
            SDL_GPUColorTargetInfo info{};
            info.texture = light;
            info.clear_color = SDL_FColor{1, 1, 1, 1};
            info.load_op = SDL_GPU_LOADOP_CLEAR;
            info.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &info, 1, nullptr);
            lights.draw(cmd, pass);
            SDL_EndGPURenderPass(pass);
            SDL_SubmitGPUCommandBuffer(cmd);
            ok = render::read_pixels(device, full, w, h, out.full) && render::read_pixels(device, light, w, h, out.light);
        }
        if (full) SDL_ReleaseGPUTexture(device, full);
        if (light) SDL_ReleaseGPUTexture(device, light);
        return ok;
    }
    void build_light(Shell& s) {
        SliceGame& g = g_;
        struct State {
            std::filesystem::path dir, noon, out;
            std::vector<std::pair<std::string, std::vector<u8>>> files;
            std::vector<SliceGame::Lamp> lamps;
            render::Camera2D cam;
            u32 w = 0, h = 0; // the game's frame
            u32 link = 0;     // «Ключ открывает Дверь»
            Shot game, editor, hero, at_noon, resized_game, resized_editor, preview;
        };
        auto st = std::make_shared<State>();
        const i32 v = g.generator().village_y();
        const f32 zoom = 22;
        // The example's sources (games/examples/light/README.md) and the hero's place in the room, between them.
        const f64 ax = 42.5, ay = v + 12.5, bx = 66.5, by = v + 14.5, hx = 56.5;
        auto files_of = [](const std::filesystem::path& dir) {
            std::vector<std::pair<std::string, std::vector<u8>>> out;
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
                if (!e.is_regular_file()) continue;
                std::vector<u8> bytes;
                read_file(e.path(), bytes);
                out.emplace_back(path_to_utf8(e.path().filename()), std::move(bytes));
            }
            std::sort(out.begin(), out.end());
            return out;
        };
        // A pixel of an image at a tile point (as every renderer places it around the snapped camera).
        auto pixel = [st](const std::vector<u8>& rgba, const render::Camera2D& c, f64 tx, f64 ty) {
            const u32 w = st->w, h = st->h;
            const i32 px = static_cast<i32>(std::floor((tx - c.snapped_x()) * c.zoom + w * 0.5));
            const i32 py = static_cast<i32>(std::floor((ty - c.snapped_y()) * c.zoom + h * 0.5));
            std::array<int, 3> out{-1, -1, -1};
            if (px < 0 || py < 0 || px >= static_cast<i32>(w) || py >= static_cast<i32>(h) || rgba.size() < w * h * 4) return out;
            const usize i = (static_cast<usize>(py) * w + static_cast<usize>(px)) * 4;
            return std::array<int, 3>{rgba[i], rgba[i + 1], rgba[i + 2]};
        };
        auto rgb = [](const std::array<int, 3>& p) {
            return std::to_string(p[0]) + "," + std::to_string(p[1]) + "," + std::to_string(p[2]);
        };
        // Pixels that differ between two images, and by how much at most.
        auto differ = [](const std::vector<u8>& a, const std::vector<u8>& b, int& most) {
            usize n = 0;
            most = 0;
            if (a.size() != b.size()) return a.size() + b.size();
            for (usize i = 0; i < a.size(); i += 4) {
                int d = 0;
                for (usize c = 0; c < 3; ++c) d = std::max(d, std::abs(static_cast<int>(a[i + c]) - static_cast<int>(b[i + c])));
                n += d > 0;
                most = std::max(most, d);
            }
            return n;
        };
        auto lit = [](const Color& c) { return c.r > 0 || c.g > 0 || c.b > 0; };

        steps_.push_back({"меню", 30, [&s, &g, this](u32 f) {
            if (f < 5) return false;
            g.set_level({});
            check(s.new_game(), "новая игра со своего уровня игры");
            return true;
        }});
        steps_.push_back({"старый уровень: свет как раньше", 10, [&s, &g, this](u32 f) {
            if (f < 3) return false;
            std::error_code ec;
            check(!std::filesystem::exists(s.game_dir() / "level" / "light.json", ec), "у уровня игры нет light.json");
            check(g.level_hour() == 12.0f && g.light_sources().empty(), "полдень, источников света нет");
            const Color day = light_rules().sky_color, now = g.lights().rules().sky_color;
            check(now.r == day.r && now.g == day.g && now.b == day.b, "небо светит, как всегда светило в игре");
            return true;
        }});
        steps_.push_back({"пример «Свет» из файлов", 20, [&s, &g, v, zoom, hx, ax, ay, bx, by, files_of, this, st](u32 f) {
            if (f == 0) {
                st->dir = s.game_dir().parent_path() / "examples" / "light" / "level";
                st->out = std::filesystem::temp_directory_path() / "forge_slice_light";
                std::error_code ec;
                std::filesystem::remove_all(st->out, ec);
                std::filesystem::create_directories(st->out, ec);
                FORGE_INFO("пример «Свет» читается из %s", path_to_utf8(st->dir).c_str());
                check(std::filesystem::is_regular_file(st->dir / "light.json", ec), "light.json примера на диске: " + path_to_utf8(st->dir));
                st->files = files_of(st->dir);
                check(st->files.size() > 2, "с ним файлы участков: " + std::to_string(st->files.size()));
                forge::level::LevelLight l;
                check(forge::level::load_light(st->dir, l) && l.time == 21.5f, "в файле: 21:30");
                // «Играть отсюда»: a new game from the level folder, the hero on the room's floor.
                g.set_level(st->dir, true, hx, v + 18);
                check(s.new_game(), "новая игра с уровня примера");
                g.camera().zoom = zoom;
                check(g.level_hour() == 21.5f, "игра светит в 21:30: " + std::to_string(g.level_hour()));
                st->lamps = g.light_sources();
                std::sort(st->lamps.begin(), st->lamps.end(), [](const auto& a, const auto& b) { return a.x < b.x; });
                check(st->lamps.size() == 2, "два источника света: " + std::to_string(st->lamps.size()));
                if (st->lamps.size() == 2) {
                    const SliceGame::Lamp& a = st->lamps[0];
                    const SliceGame::Lamp& b = st->lamps[1];
                    check(a.id != 0 && b.id != 0 && a.id != b.id, "у каждого свой id из редактора");
                    check(a.x == ax && a.y == ay && a.source.r == 1 && std::fabs(a.source.g - 0x9A / 255.0f) < 1e-5f &&
                              std::fabs(a.source.b - 0x40 / 255.0f) < 1e-5f && a.source.brightness == 2 && a.source.radius == 10,
                          "тёплый: центр 42.5, v+12.5, #FF9A40, яркость 2, радиус 10");
                    check(b.x == bx && b.y == by && std::fabs(b.source.r - 0x50 / 255.0f) < 1e-5f &&
                              std::fabs(b.source.g - 0x80 / 255.0f) < 1e-5f && b.source.b == 1 && b.source.brightness == 1.5f &&
                              b.source.radius == 6,
                          "синий: центр 66.5, v+14.5 (перенесён через x = 64), #5080FF, яркость 1.5, радиус 6");
                }
            }
            return f >= 15; // the camera settles on the hero
        }});
        steps_.push_back({"игра и редактор светят одинаково", 5, [&s, &g, v, ax, ay, bx, by, hx, pixel, rgb, differ, lit, this, st](u32 f) {
            if (f > 0) return true;
            // The size the game is drawn at (--size), and another, as when its window is made bigger or smaller.
            const u32 w = g.frame_width(), h = g.frame_height();
            const bool full_hd = w == 1920 && h == 1080;
            const u32 w2 = full_hd ? 1600 : 1920, h2 = full_hd ? 900 : 1080;
            st->w = w;
            st->h = h;
            FORGE_INFO("кадр игры %ux%u; окно другого размера — %ux%u", w, h, w2, h2);
            st->cam = g.camera();
            check(std::fabs(st->cam.x - hx) < 1 && st->cam.zoom == 22, "камера у героя: " + std::to_string(st->cam.x) + ", " +
                                                                           std::to_string(st->cam.y));
            // The game's frame with the hero's own light put out, as the editor has no hero.
            auto game_shot = [&g](u32 sw, u32 sh, Shot& out) {
                return shoot(g.device(), sw, sh, [&g, sw, sh](SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* t) { g.render(cmd, t, sw, sh); },
                             g.lights(), out);
            };
            g.set_hero_light(false);
            check(game_shot(w, h, st->game), "кадр игры без света героя");
            // The editor's view «как в игре» of the same folder: the same camera and size, the same world under the
            // light's grid (the view and kLightMargin tiles around it; the game loads chunks farther than that).
            SliceLevel module;
            std::string why;
            check(module.load_objects(s.game_dir(), &why), "шаблоны игры для редактора " + why);
            check(module.init_view(g.device(), SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM), "вид редактора");
            {
                forge::level::Level level(module);
                check(level.open(st->dir), "редактор открывает пример");
                check(level.light().time == 21.5f, "в редакторе тоже 21:30");
                const render::Camera2D cam = st->cam;
                auto editor_shot = [&](u32 sw, u32 sh, const forge::level::ViewOptions& options, Shot& out) {
                    const world::Rect grid = cam.visible_tiles(sw, sh).expanded(render::kLightMargin);
                    level.ensure_loaded(grid);
                    u32 chunks = 0, in_game = 0, in_editor = 0;
                    for (i32 cy = grid.y0 >> world::kChunkShift; cy <= (grid.y1 - 1) >> world::kChunkShift; ++cy)
                        for (i32 cx = grid.x0 >> world::kChunkShift; cx <= (grid.x1 - 1) >> world::kChunkShift; ++cx) {
                            ++chunks;
                            in_game += g.world() && g.world()->find_chunk({cx, cy}) != nullptr;
                            in_editor += level.world().find_chunk({cx, cy}) != nullptr;
                        }
                    FORGE_INFO("под сеткой света %ux%u участков %u: загружены в игре %u, в редакторе %u", sw, sh, chunks, in_game, in_editor);
                    check(in_game == chunks && in_editor == chunks, "под сеткой света " + std::to_string(sw) + "x" + std::to_string(sh) +
                                                                        " участков " + std::to_string(chunks) + ": в игре загружено " +
                                                                        std::to_string(in_game) + ", в редакторе " + std::to_string(in_editor));
                    return shoot(g.device(), sw, sh,
                                 [&](SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* t) {
                                     module.prepare_view(cmd, level, cam, sw, sh, options, 0);
                                     SDL_GPUColorTargetInfo info{};
                                     info.texture = t;
                                     const Color bg = module.background();
                                     info.clear_color = SDL_FColor{bg.r, bg.g, bg.b, 1};
                                     info.load_op = SDL_GPU_LOADOP_CLEAR;
                                     info.store_op = SDL_GPU_STOREOP_STORE;
                                     SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &info, 1, nullptr);
                                     module.draw_view(cmd, pass);
                                     SDL_EndGPURenderPass(pass);
                                 },
                                 module.lights(), out);
                };
                forge::level::ViewOptions options;
                options.game_light = true;
                check(editor_shot(w, h, options, st->editor), "вид редактора «как в игре»");
                // The light in numbers: the sources' own light where it reaches, in the game and the editor.
                render::LightRenderer& gl = g.lights();
                render::LightRenderer& el = module.lights();
                u32 cells = 0, same = 0, past = 0, game_a = 0, game_b = 0;
                for (i32 y = static_cast<i32>(ay) - 14; y <= static_cast<i32>(ay) + 14; ++y)
                    for (i32 x = static_cast<i32>(ax) - 14; x <= static_cast<i32>(bx) + 14; ++x) {
                        const Color a = gl.lamp_at(x + 0.5, y + 0.5), b = el.lamp_at(x + 0.5, y + 0.5);
                        ++cells;
                        same += a.r == b.r && a.g == b.g && a.b == b.b;
                        const f64 da = std::hypot(x + 0.5 - ax, y + 0.5 - ay), db = std::hypot(x + 0.5 - bx, y + 0.5 - by);
                        if (lit(a) && da >= 10 && db >= 6) ++past;
                        game_a += lit(a) && da < 10;
                        game_b += lit(a) && db < 6;
                    }
                check(same == cells, "свет источников в игре и в редакторе тот же в " + std::to_string(same) + " из " +
                                         std::to_string(cells) + " клеток");
                check(past == 0, "за окружностями радиусов света нет: " + std::to_string(past) + " клеток");
                FORGE_INFO("освещено: тёплым %u клеток (круг радиуса 10, стены его режут), синим %u (радиус 6)", game_a, game_b);
                check(game_a > 50 && game_b > 20, "оба источника светят: " + std::to_string(game_a) + " и " + std::to_string(game_b));
                const Color ca = gl.lamp_at(ax, ay), cb = gl.lamp_at(bx, by);
                check(ca.r > ca.g && ca.g > ca.b && ca.r > 1.5f, "в центре тёплого: тёплый свет 2 × #FF9A40");
                check(cb.b > cb.g && cb.g > cb.r && cb.b > 1.2f, "в центре синего: синий свет 1.5 × #5080FF");
                // The window of another size: the game draws its next frame at it (the torches in view are picked again),
                // the editor its view of the same size; the light is the same again.
                check(game_shot(w2, h2, st->resized_game), "кадр игры в окне " + std::to_string(w2) + "x" + std::to_string(h2));
                check(editor_shot(w2, h2, options, st->resized_editor), "вид редактора того же размера");
                // «Просмотр» of another hour in the editor: its view only, the level's hour (the game's night) as it was.
                options.preview_time = 12;
                check(editor_shot(w, h, options, st->preview), "вид редактора с просмотром 12:00");
                check(level.light().time == 21.5f && g.level_hour() == 21.5f, "после просмотра в уровне и в игре 21:30");
                module.level_closing(level);
            }
            module.shutdown_view();
            int most = 0;
            usize n = differ(st->game.light, st->editor.light, most);
            FORGE_INFO("свет кадра %ux%u: игра и редактор различаются в %zu пикселях из %u, не больше чем на %d", w, h, n, w * h, most);
            check(n == 0, "свет игры и вида редактора совпадает попиксельно: разных " + std::to_string(n) + ", до " + std::to_string(most));
            n = differ(st->resized_game.light, st->resized_editor.light, most);
            FORGE_INFO("свет кадра %ux%u: игра и редактор различаются в %zu пикселях из %u, не больше чем на %d", w2, h2, n, w2 * h2, most);
            check(n == 0, "в окне " + std::to_string(w2) + "x" + std::to_string(h2) + " тоже попиксельно: разных " + std::to_string(n) +
                              ", до " + std::to_string(most));
            const auto pa = pixel(st->game.light, st->cam, ax, ay), pb = pixel(st->game.light, st->cam, bx, by);
            const auto mid = pixel(st->game.light, st->cam, hx, by), sky = pixel(st->game.light, st->cam, hx, v - 3);
            FORGE_INFO("свет в пикселях: тёплый %s, синий %s, между ними %s, небо %s", rgb(pa).c_str(), rgb(pb).c_str(), rgb(mid).c_str(),
                       rgb(sky).c_str());
            check(pa[0] == 255 && pa[0] >= pa[1] && pa[1] > pa[2] + 60, "тёплый в кадре: " + rgb(pa));
            check(pb[2] == 255 && pb[2] > pb[1] && pb[1] > pb[0], "синий в кадре: " + rgb(pb));
            check(mid[0] < 40 && mid[1] < 40 && mid[2] < 40, "между ними, вне обоих кругов, темно: " + rgb(mid));
            check(sky[2] > sky[0] + 20 && sky[2] < 100, "ночное небо тёмно-синее: " + rgb(sky));
            const auto noon_sky = pixel(st->preview.light, st->cam, hx, v - 3);
            check(noon_sky[0] > sky[0] + 150, "просмотр 12:00 светлее в виде редактора: " + rgb(sky) + " → " + rgb(noon_sky));
            // The hero's own light again: brighter around him, the same elsewhere.
            g.set_hero_light(true);
            check(game_shot(w, h, st->hero), "кадр игры со светом героя");
            const auto lit_hero = pixel(st->hero.light, st->cam, hx, by), at_a = pixel(st->hero.light, st->cam, ax, ay);
            check(lit_hero[0] > mid[0] + 40, "у героя светлее, чем без его света: " + rgb(mid) + " → " + rgb(lit_hero));
            check(at_a == pa, "у тёплого источника так же: " + rgb(at_a));
            for (const auto& [name, shot] : {std::pair<const char*, const Shot*>{"game", &st->game}, {"editor", &st->editor}, {"hero", &st->hero},
                                             {"preview", &st->preview}}) {
                render::write_png(path_to_utf8(st->out / (std::string(name) + ".png")).c_str(), w, h, shot->full);
                render::write_png(path_to_utf8(st->out / (std::string(name) + "-light.png")).c_str(), w, h, shot->light);
            }
            for (const auto& [name, shot] : {std::pair<const char*, const Shot*>{"resized-game", &st->resized_game},
                                             {"resized-editor", &st->resized_editor}}) {
                render::write_png(path_to_utf8(st->out / (std::string(name) + ".png")).c_str(), w2, h2, shot->full);
                render::write_png(path_to_utf8(st->out / (std::string(name) + "-light.png")).c_str(), w2, h2, shot->light);
            }
            FORGE_INFO("картинки: %s (game, editor, hero, preview, resized-game, resized-editor и их -light)", path_to_utf8(st->out).c_str());
            return true;
        }});
        steps_.push_back({"сохранение игры: файлы примера не тронуты", 10, [&s, &g, files_of, this, st](u32 f) {
            if (f == 0) {
                check(s.save("light", "Свет"), "игра сохраняется");
                std::vector<u8> mine, author;
                read_file(s.slots().folder("light") / "world" / "light.json", mine);
                read_file(st->dir / "light.json", author);
                check(!mine.empty() && mine == author, "в сохранении light.json автора");
                check(files_of(st->dir) == st->files, "файлы примера как были");
                check(s.load("light"), "сохранение загружается");
            }
            if (f < 3) return false;
            std::vector<SliceGame::Lamp> lamps = g.light_sources();
            std::sort(lamps.begin(), lamps.end(), [](const auto& a, const auto& b) { return a.x < b.x; });
            check(g.level_hour() == 21.5f && lamps.size() == 2 && st->lamps.size() == 2 && lamps[0].id == st->lamps[0].id &&
                      lamps[1].id == st->lamps[1].id,
                  "загруженная игра: 21:30 и те же источники");
            return true;
        }});
        steps_.push_back({"копия в полдень: небо светлее, источники те же", 20, [&s, &g, v, zoom, ax, ay, hx, pixel, rgb, this, st](u32 f) {
            if (f == 0) {
                st->noon = std::filesystem::temp_directory_path() / "forge_slice_light_noon";
                std::error_code ec;
                std::filesystem::remove_all(st->noon, ec);
                std::string why;
                check(forge::level::copy_level(st->dir, st->noon, &why) && forge::level::save_light(st->noon, {12}, &why),
                      "копия примера в полдень " + path_to_utf8(st->noon) + why);
                g.set_level(st->noon, true, hx, v + 18);
                check(s.new_game(), "новая игра с копии");
                g.camera().zoom = zoom;
                check(g.level_hour() == 12.0f, "игра светит в полдень");
            }
            if (f < 15) return false;
            const u32 w = st->w, h = st->h;
            check(g.frame_width() == w && g.frame_height() == h, "кадр того же размера");
            g.set_hero_light(false);
            check(shoot(g.device(), w, h, [&g, w, h](SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* t) { g.render(cmd, t, w, h); }, g.lights(),
                        st->at_noon),
                  "кадр в полдень");
            g.set_hero_light(true);
            const render::Camera2D cam = g.camera();
            const auto night = pixel(st->game.light, st->cam, hx, v - 3), day = pixel(st->at_noon.light, cam, hx, v - 3);
            const auto lamp_night = pixel(st->game.light, st->cam, ax, ay), lamp_day = pixel(st->at_noon.light, cam, ax, ay);
            check(day[0] > night[0] + 150 && day[1] > night[1] + 150, "небо в полдень светлее: " + rgb(night) + " → " + rgb(day));
            check(lamp_day == lamp_night, "тёплый источник светит так же: " + rgb(lamp_day));
            render::write_png(path_to_utf8(st->out / "noon.png").c_str(), w, h, st->at_noon.full);
            return true;
        }});
        // «Только ночью» goes by the level's hour (light.json), night from 21:00 till 4:30 (slice::is_night). The
        // game's own link «Ключ открывает Дверь» (its logic.json; in a package, the package's) is made night-only the
        // way «Логика» makes it and written to this run's file of links; the game reads it from there. The door at the
        // end of the gallery opens to the hero with the key at night and holds by day: the example at 21:30 and copies
        // of it at the hours around 21:00, 4:30 and midnight, in a new game and in a game loaded from a save.
        steps_.push_back({"связь «Только ночью» из файла связей", 5, [&s, &g, this, st](u32 f) {
            if (f > 0) return true;
            logic::Logic links;
            std::string why;
            check(links.load(s.game_dir() / "logic.json", &why), "связи игры читаются " + why);
            logic::Link* door = nullptr;
            for (logic::Link& l : links.links)
                if (l.a == "key" && l.verb == "open" && l.b == "door") door = &l;
            check(door && !door->night, "в игре связь «Ключ открывает Дверь», без «Только ночью»");
            if (!door) return true;
            st->link = door->id;
            door->night = true; // the switch «Только ночью» in «Логика»
            const std::filesystem::path file = std::filesystem::temp_directory_path() / "forge_slice_test_logic.json";
            check(links.save(file, &why) && g.reload_links(&why), "связь записана в файл связей прогона, игра его перечитала " + why);
            std::vector<u8> bytes;
            read_file(file, bytes);
            const std::string text(bytes.begin(), bytes.end());
            check(text.find("\"night\"") != std::string::npos, "в файле связей: night");
            bool night = false;
            for (const logic::Link& l : g.links().links) night = night || (l.id == st->link && l.night);
            check(night, "в игре связь «Ключ открывает Дверь» — только ночью");
            return true;
        }});
        struct Hour {
            f32 time;
            bool night, from_save;
        };
        for (const Hour& at : {Hour{21.5f, true, false}, Hour{12.0f, false, false}, Hour{20.0f + 59.0f / 60.0f, false, true},
                               Hour{21.0f, true, true}, Hour{0.0f, true, false}, Hour{4.0f + 29.0f / 60.0f, true, true},
                               Hour{4.5f, false, false}}) {
            const std::string& name = names_.emplace_back(std::string("«Только ночью» в ") + forge::level::clock_text(at.time) +
                                                          (at.from_save ? ", игра из сохранения" : ", новая игра"));
            steps_.push_back({name.c_str(), 140, [&s, &g, at, this, st](u32 f) {
                const i32 dx = g.generator().gallery_x1();
                const f64 gy = g.generator().gallery_y();
                const std::string when = forge::level::clock_text(at.time);
                if (f == 0) {
                    std::filesystem::path dir = st->dir; // 21:30: the example itself
                    if (at.time != 21.5f) {
                        dir = std::filesystem::temp_directory_path() / "forge_slice_light_hour";
                        std::error_code ec;
                        std::filesystem::remove_all(dir, ec);
                        std::string why;
                        check(forge::level::copy_level(st->dir, dir, &why) && forge::level::save_light(dir, {at.time}, &why),
                              "копия примера в " + when + " " + why);
                    }
                    // The hero by the door, with the key.
                    g.set_level(dir, true, dx + 2.5, gy + 1);
                    check(s.new_game(), "новая игра в " + when);
                    s.vars().set("inv.key", 1);
                    g.script(Controls{});
                    return false;
                }
                if (at.from_save && f == 3) {
                    check(s.save("night", "Ночь"), "игра сохраняется");
                    s.vars().set("inv.key", 0);
                }
                if (at.from_save && f == 6) check(s.load("night"), "сохранение загружается");
                if (f == 9) {
                    check(g.level_hour() == at.time && g.inventory("key") == 1,
                          "в игре " + forge::level::clock_text(g.level_hour()) + ", ключ у героя");
                    check(is_night(g.level_hour()) == at.night, when + (at.night ? " — ночь" : " — не ночь"));
                }
                if (f < 12) return false;
                if (f < 110) {
                    Controls left;
                    left.left = true;
                    g.script(left);
                    return false;
                }
                g.script(Controls{});
                bool open = !at.night;
                check(g.door_open(dx + 0.5, gy + 0.5, open), "дверь в конце штольни");
                if (at.night) {
                    check(open && g.hero_x() < dx + 0.5, "в " + when + " ключ открыл дверь, герой прошёл");
                } else {
                    check(!open && g.hero_x() > dx + 1.0, "в " + when + " дверь закрыта и с ключом, герой у двери");
                    FORGE_INFO("подсказка у закрытой двери: «%s»", g.last_hint().c_str());
                }
                return true;
            }});
        }
        steps_.push_back({"обратно в меню", 5, [&s, &g, files_of, this, st](u32 f) {
            if (f == 0) {
                s.to_main_menu();
                g.set_level({});
                check(files_of(st->dir) == st->files, "файлы примера как были");
                std::error_code ec;
                std::filesystem::remove_all(st->noon, ec);
                std::filesystem::remove_all(std::filesystem::temp_directory_path() / "forge_slice_light_hour", ec);
            }
            return f >= 2;
        }});
    }

    // --- the template «Платформер» (step 14.2d) ---
    // A strip of frames (a template's picture) the game draws in a box of tw × th tiles around (cx, cy): which frame
    // the screen shows there, mirrored or not, and how well (the share of the frame's opaque pixels the screen has,
    // each channel within `most`), the box looked for up to `slack` pixels around where it should be (a body moving
    // is drawn between two ticks). only: that frame of the strip alone (-1: the best of all).
    struct Match {
        i32 frame = -1;
        bool flip = false;
        f64 score = 0;
        i32 dx = 0, dy = 0;
    };
    static Match drawn_frame(const std::vector<u8>& screen, u32 sw, u32 sh, const render::Camera2D& c, f64 cx, f64 cy, f64 tw,
                             f64 th, const assets::CookedTexture& strip, u32 frames, i32 slack, i32 most = 40, i32 only = -1) {
        Match best;
        if (!frames || strip.width % frames || screen.size() < static_cast<usize>(sw) * sh * 4) return best;
        const u32 fw = strip.width / frames, fh = strip.height;
        const f64 left = (cx - tw / 2 - c.snapped_x()) * c.zoom + sw * 0.5, top = (cy - th / 2 - c.snapped_y()) * c.zoom + sh * 0.5;
        const f64 pw = tw * c.zoom, ph = th * c.zoom;
        for (u32 k = 0; k < frames; ++k)
            for (int flip = 0; flip < 2 && (only < 0 || static_cast<u32>(only) == k); ++flip)
                for (i32 dy = -slack; dy <= slack; ++dy)
                    for (i32 dx = -slack; dx <= slack; ++dx) {
                        u32 opaque = 0, same = 0;
                        for (i32 y = static_cast<i32>(std::floor(top)); y < static_cast<i32>(std::ceil(top + ph)); ++y)
                            for (i32 x = static_cast<i32>(std::floor(left)); x < static_cast<i32>(std::ceil(left + pw)); ++x) {
                                const f64 u = (x + 0.5 - left) / pw, v = (y + 0.5 - top) / ph;
                                if (u < 0 || u >= 1 || v < 0 || v >= 1) continue;
                                u32 px = std::min(fw - 1, static_cast<u32>(u * fw));
                                const u32 py = std::min(fh - 1, static_cast<u32>(v * fh));
                                if (flip) px = fw - 1 - px;
                                const u8* p = &strip.rgba8[(static_cast<usize>(py) * strip.width + k * fw + px) * 4];
                                if (p[3] < 128) continue;
                                ++opaque;
                                const i32 X = x + dx, Y = y + dy;
                                if (X < 0 || Y < 0 || X >= static_cast<i32>(sw) || Y >= static_cast<i32>(sh)) continue;
                                const u8* q = &screen[(static_cast<usize>(Y) * sw + static_cast<usize>(X)) * 4];
                                if (std::abs(q[0] - p[0]) <= most && std::abs(q[1] - p[1]) <= most && std::abs(q[2] - p[2]) <= most) ++same;
                            }
                        const f64 score = opaque ? static_cast<f64>(same) / opaque : 0;
                        if (score > best.score) best = {static_cast<i32>(k), flip != 0, score, dx, dy};
                    }
        return best;
    }
    // A layer of a screen's page by its name (its title).
    static Rml::Element* named(Rml::Element* e, const std::string& title) {
        if (!e) return nullptr;
        if (e->GetAttribute<Rml::String>("title", "") == title) return e;
        for (int i = 0; i < e->GetNumChildren(); ++i)
            if (Rml::Element* found = named(e->GetChild(i), title)) return found;
        return nullptr;
    }

    // The template «Платформер» as a player meets it, in this run's copy of games/platformer (main): a bot plays it by
    // the keys, looking at the level as a player does (its tiles, enemies and spikes): it runs, jumps over pits and
    // spikes, onto beetles and over hedgehogs, through «Луг» to «Холмы», where the game is saved. The hero's frame is
    // checked against its state all along and against the screen's pixels now and then (mirrored when it faces left),
    // its feet on the grass and on the bridge to the pixel; a beetle walks by the two frames of its picture. Three
    // falls lose the game: the window «при поражении», «Ещё раз» a new game, the save as it was.
    // continued (another process, another working folder): «Продолжить» on «Холмы», nothing given twice, the bot on
    // through «Вершина» to the flag: the window «при победе», «Ещё раз», the save still loads.
    void build_template(Shell& s, bool continued) {
        namespace fs = std::filesystem;
        SliceGame& g = g_;
        struct Thing {
            f64 x, feet, half;
            bool stomp;
            f64 vx; // tiles / s, as it went the frame before
        };
        struct State {
            // The bot: where the hero and the enemies were the frame before.
            f64 bot_x = 0, bot_feet = 0, run_up = 1e9; // run_up: back to there first
            std::map<flecs::entity_t, f64> enemy_x;
            // The hero's frames against its state: the last frames' (on the ground, x), latest last.
            std::deque<std::pair<bool, f64>> history;
            u32 last_frame = 0, before_last = 0, seen[4] = {}, bad = 0, looks = 0, bad_looks = 0, flipped = 0;
            std::string first_bad, seen_on;
            u32 hurt = 0, at = 0, still = 0, beetle_frames[2] = {}, beetle_looks = 0;
            f64 x0 = 0, score0 = 0, coins0 = 0;
            assets::CookedTexture hero, beetle, tiles;
            std::map<std::string, f64> notes;
        };
        auto st = std::make_shared<State>();
        auto var = [&s](const char* name) { return s.vars().get(name).number(); };
        auto num = [](f64 x) {
            char b[32];
            std::snprintf(b, sizeof b, "%.2f", x);
            return std::string(b);
        };
        auto where = [&g, num] { return " (уровень «" + g.level_id() + "», герой " + num(g.hero_x()) + ", " + num(g.hero_y()) + ")"; };
        auto there = [&g](u64 id) { return g.copy_with_id(id) != 0; };
        auto in = [&g](u64 id) {
            const std::vector<u64> now = g.areas_inside();
            return std::find(now.begin(), now.end(), id) != now.end();
        };
        auto picture = [](const fs::path& file, assets::CookedTexture& out) {
            std::vector<u8> bytes;
            return read_file(file, bytes) && assets::decode_image(bytes, out);
        };
        // The author's HUD and windows, as the player reads them.
        auto said = [&s](const char* screen, const char* layer) {
            Rml::ElementDocument* doc = s.screens().document(screen);
            Rml::Element* e = doc ? named(doc, layer) : nullptr;
            return e ? text_of(e) : std::string("нет");
        };
        auto whole = [](f64 x) { return std::to_string(static_cast<i64>(std::llround(x))); };
        auto hud_says = [said, whole, var, &g] {
            return said("hud", "Сердца") == whole(g.hearts()) && said("hud", "Монеты") == whole(var("inv.coins")) &&
                   said("hud", "Очки") == "Очки: " + whole(g.score());
        };
        auto hud_text = [said] { return "«" + said("hud", "Сердца") + "», «" + said("hud", "Монеты") + "», «" + said("hud", "Очки") + "»"; };
        auto press = [&s, this](const char* screen, const char* button) {
            Rml::ElementDocument* doc = s.screens().document(screen);
            Rml::Element* e = doc ? named(doc, button) : nullptr;
            u32 id = 0;
            if (e) std::sscanf(e->GetId().c_str(), "n%u", &id);
            const std::optional<Rml::Vector2f> at = id ? on_screen(s, screen, id) : std::nullopt;
            return at && click_at(s, *at);
        };

        // What the bot sees: the blocks, the enemies (going as they went the frame before) and the spikes now.
        auto solid = [&g](i32 x, i32 y) { return g.tile(kBlocks, x, y) != world::TileAir; };
        auto things = [&g, st](std::initializer_list<const char*> ids, f64 half, bool enemy) {
            std::vector<Thing> out;
            for (const char* id : ids)
                for (flecs::entity_t e : g.copies_of(id)) {
                    f64 x = 0, y = 0;
                    if (!g.position_of(e, x, y)) continue;
                    f64 vx = 0;
                    if (enemy) {
                        const auto was = st->enemy_x.find(e);
                        if (was != st->enemy_x.end() && std::fabs(x - was->second) < 0.5) vx = (x - was->second) * 60;
                        st->enemy_x[e] = x;
                    }
                    out.push_back({x, enemy ? y + 0.4 : y + 0.5, std::string_view(id) == "spikes_row" ? 1.5 : half,
                                   enemy && std::string_view(id) == "beetle", vx});
                }
            return out;
        };
        // The bot plays as a careful player does: it plays each choice out in its head first, the hero moving as the
        // game moves it (8.5 tiles / s, sped up by 70 on the ground and 30 in the air, a jump of 15.5, the pull of 40,
        // a one-tile step walked up), against the blocks, the spikes and the enemies. dir(t): the keys at tick t; down:
        // till it comes down and stops (no keys) after that, else for `ticks` (and till it is on the ground). Unsafe: an
        // enemy met from the side, spikes, the pit, a wall it stays at, coming down next to an enemy. x: where it went
        // wrong or ended, land: where it came down.
        struct Plan {
            bool safe = true, stomp = false;
            f64 x = 0, land = -1e9;
        };
        auto play_out = [solid](f64 x, f64 feet, f64 vx, f64 vy, bool ground, bool jump, const std::function<i32(u32)>& dir, u32 ticks,
                                bool down, const std::vector<Thing>& enemies, const std::vector<Thing>& spikes) {
            Plan out;
            constexpr f64 dt = 1.0 / 60, hw = kHeroHalfW, tall = 2 * kHeroHalfH;
            auto cell = [](f64 v) { return static_cast<i32>(std::floor(v)); };
            auto any_solid = [&](f64 x0, f64 x1, f64 y0, f64 y1) {
                for (i32 cx = cell(x0 + 1e-3); cx <= cell(x1 - 1e-3); ++cx)
                    for (i32 cy = cell(y0 + 1e-3); cy <= cell(y1 - 1e-3); ++cy)
                        if (solid(cx, cy)) return true;
                return false;
            };
            auto wrong = [&out](f64 at) {
                out.safe = false;
                out.x = at;
                return out;
            };
            u32 stuck = 0, landed = ~0u;
            for (u32 t = 0;; ++t) {
                if (down ? (landed != ~0u && t >= landed + 30) || t >= 240 : (t >= ticks && ground) || t >= ticks + 120) break;
                const i32 d = landed != ~0u ? 0 : dir(t);
                const f64 want = d * 8.5, acc = (ground ? 70.0 : 30.0) * dt;
                vx = vx < want ? std::min(want, vx + acc) : std::max(want, vx - acc);
                if (jump && ground) vy = -15.5;
                jump = false;
                const bool was_ground = ground;
                vy += 40 * dt;
                f64 nx = x + vx * dt;
                if (any_solid(nx - hw, nx + hw, feet - tall, feet)) {
                    nx = x;
                    vx = 0;
                    const f64 up = x + d * 0.12;
                    if (was_ground && d != 0 && !any_solid(up - hw, up + hw, feet - 1 - tall, feet - 1)) {
                        nx = up;
                        feet -= 1;
                        stuck = 0;
                    } else if (was_ground && d != 0 && ++stuck > 3) {
                        return wrong(x);
                    }
                }
                const f64 was_x = x;
                x = nx;
                const f64 before = feet, next = feet + vy * dt;
                ground = false;
                if (vy > 0) {
                    for (i32 r = cell(feet + 1e-3); r <= cell(next - 1e-3) && !ground; ++r)
                        if (r >= feet - 1e-3 && any_solid(x - hw, x + hw, r, r + 1)) {
                            feet = r;
                            vy = 0;
                            ground = true;
                        }
                    if (!ground) feet = next;
                } else if (any_solid(x - hw, x + hw, next - tall, feet - tall)) {
                    vy = 0;
                } else {
                    feet = next;
                }
                if (feet > 1.5) return wrong(x); // into the pit
                const f64 now = (t + 1) * dt;
                for (const Thing& e : enemies) {
                    // Met as the game meets it (forge::sim::touch_side: through its top, it is beaten); 0.1 of a tile
                    // short of that at its side counts as met.
                    const f64 ex = e.x + e.vx * now;
                    const forge::sim::Box hero_now{x, feet - kHeroHalfH, hw, kHeroHalfH}, hero_was{was_x, before - kHeroHalfH, hw, kHeroHalfH},
                        it_now{ex, e.feet - 0.4, e.half, 0.4}, it_was{ex - e.vx * dt, e.feet - 0.4, e.half, 0.4};
                    const forge::sim::Touch side = forge::sim::touch_side(hero_was, hero_now, it_was, it_now);
                    if (side == forge::sim::Touch::None) {
                        if (std::fabs(ex - x) < e.half + hw + 0.1 && feet > e.feet - 0.8 && feet - tall < e.feet) return wrong(x);
                        continue;
                    }
                    if (side != forge::sim::Touch::Top || !e.stomp) return wrong(x);
                    out.stomp = true;
                    out.x = out.land = x;
                    return out;
                }
                for (const Thing& h : spikes)
                    if (std::fabs(h.x - x) < h.half + hw + 0.05 && feet > h.feet - 1.0 && feet - tall < h.feet) return wrong(x);
                if (ground && !was_ground && landed == ~0u) {
                    for (const Thing& e : enemies) {
                        const f64 ex = e.x + e.vx * now;
                        if (std::fabs(e.feet - feet) < 1.2 && ex - x > -1.0 && ex - x < 1.5) return wrong(x); // no room to jump it
                    }
                    out.land = x;
                    if (down) landed = t;
                }
            }
            out.x = x;
            return out;
        };
        // One frame of the bot. On the ground: a jump onto a beetle if there is one; right while that is safe a little
        // ahead; else a jump that comes down safe past where running goes wrong (letting go of the key or turning back
        // on the way if it takes that: a short bridge); else, if one at full speed would, a run-up (3 tiles back);
        // else it waits, else it backs off. In the air: right, no key or left, whichever comes down safe, onto a beetle
        // first.
        auto drive = [&g, st, things, play_out] {
            const f64 hx = g.hero_x(), feet = g.hero_y() + kHeroHalfH;
            const bool near = std::fabs(hx - st->bot_x) < 1.0 && std::fabs(feet - st->bot_feet) < 1.0;
            const f64 vx = near ? (hx - st->bot_x) * 60 : 0, vy = near && !g.on_ground() ? (feet - st->bot_feet) * 60 : 0;
            st->bot_x = hx;
            st->bot_feet = feet;
            const std::vector<Thing> enemies = things({"beetle", "hedgehog"}, 0.35, true);
            const std::vector<Thing> spikes = things({"spikes", "spikes_row"}, 0.5, false);
            auto keys = [](i32 d) { return [d](u32) { return d; }; };
            auto then = [](i32 d1, u32 k, i32 d2) { return [=](u32 t) { return t < k ? d1 : d2; }; };
            Controls c;
            if (g.on_ground()) {
                std::vector<std::function<i32(u32)>> jumps = {keys(1)};
                for (u32 k = 6; k <= 48; k += 6) {
                    jumps.push_back(then(1, k, 0));
                    jumps.push_back(then(1, k, -1));
                }
                auto jump_from = [&](f64 speed, f64 past) {
                    for (const auto& d : jumps) {
                        const Plan p = play_out(hx, feet, speed, 0, true, true, d, 0, true, enemies, spikes);
                        if (p.safe && (p.stomp || p.land > past)) return true;
                    }
                    return false;
                };
                if (hx > st->run_up) { // taking a run-up
                    if (play_out(hx, feet, vx, 0, true, false, keys(-1), 24, false, enemies, spikes).safe) {
                        c.left = true;
                        return c;
                    }
                }
                st->run_up = 1e9;
                if (jump_from(vx, 1e9)) { // onto a beetle
                    c.right = c.jump = true;
                    return c;
                }
                // A sixth of a second ahead (and on till it is down): time enough to stop or jump.
                const Plan run = play_out(hx, feet, vx, 0, true, false, keys(1), 10, false, enemies, spikes);
                if (run.safe) {
                    c.right = true;
                    return c;
                }
                if (jump_from(vx, run.x)) {
                    c.right = c.jump = true;
                    return c;
                }
                if (vx < 6 && jump_from(8.5, run.x)) {
                    st->run_up = hx - 3;
                    c.left = true;
                    return c;
                }
                if (play_out(hx, feet, vx, 0, true, false, keys(0), 24, false, enemies, spikes).safe) return c; // waits
                c.left = true; // backs off
                return c;
            }
            i32 pick = 1;
            bool found = false, stomp = false;
            for (i32 d : {1, 0, -1}) {
                const Plan p = play_out(hx, feet, vx, vy, false, false, keys(d), 0, true, enemies, spikes);
                if (p.safe && (!found || (p.stomp && !stomp))) {
                    pick = d;
                    found = true;
                    stomp = p.stomp;
                }
            }
            c.right = pick > 0;
            c.left = pick < 0;
            return c;
        };
        // The hero's frame against its state, every frame: in the air (5 frames) 3, walking on the ground 1 or 2,
        // standing 0, a walk starting with 1. A look at the screen every `every` frames: the frame drawn is the one
        // the game says, as its picture's pixels.
        auto watch = [&s, &g, st, this](u32 f, u32 every) {
            // Another level, or the hero put elsewhere: what was seen before is not its walk.
            if (g.level_id() != st->seen_on || (!st->history.empty() && std::fabs(st->history.back().second - g.hero_x()) > 1.0)) {
                st->history.clear();
                st->last_frame = st->before_last = g.hero_frame();
                st->seen_on = g.level_id();
            }
            st->history.push_back({g.on_ground(), g.hero_x()});
            if (st->history.size() > 6) st->history.pop_front();
            if (const u32 hurt = g.enemy_hits() + g.hazard_hits(); hurt != st->hurt) {
                if (hurt > st->hurt) FORGE_INFO("бот ранен на кадре теста %u (%s, %.2f, %.2f)", f, g.level_id().c_str(), g.hero_x(), g.hero_y());
                for (const char* id : {"beetle", "hedgehog"})
                    for (flecs::entity_t e : g.copies_of(id)) {
                        f64 x = 0, y = 0;
                        if (g.position_of(e, x, y) && std::fabs(x - g.hero_x()) < 4) FORGE_INFO("  рядом %s в %.2f, %.2f", id, x, y);
                    }
                st->hurt = hurt;
            }
            const u32 frame = g.hero_frame();
            if (frame < 4) ++st->seen[frame];
            const auto& h = st->history;
            auto all_ground = [&](usize k, bool want) {
                if (h.size() < k) return false;
                for (usize i = h.size() - k; i < h.size(); ++i)
                    if (h[i].first != want) return false;
                return true;
            };
            // Walking: 0.05 tiles a frame and more (3 tiles / s, the frame's rule is 0.5); standing: not at all.
            auto moved = [&](usize i) { return std::fabs(h[i].second - h[i - 1].second); };
            std::string bad;
            if (all_ground(5, false) && frame != 3) bad = "в воздухе кадр " + std::to_string(frame);
            if (all_ground(3, true) && moved(h.size() - 1) > 0.05 && moved(h.size() - 2) > 0.05 && frame != 1 && frame != 2)
                bad = "идёт, а кадр " + std::to_string(frame);
            if (all_ground(4, true) && moved(h.size() - 1) < 1e-6 && moved(h.size() - 2) < 1e-6 && moved(h.size() - 3) < 1e-6 && frame != 0)
                bad = "стоит, а кадр " + std::to_string(frame);
            if ((frame == 1 || frame == 2) && st->last_frame != 1 && st->last_frame != 2 && frame != 1)
                bad = "ходьба начата кадром " + std::to_string(frame);
            // A frame of standing between steps is a flash (a step walked up, a bump).
            auto walk = [](u32 k) { return k == 1 || k == 2; };
            if (walk(frame) && st->last_frame == 0 && walk(st->before_last)) bad = "кадр «стоит» мигнул посреди ходьбы";
            st->before_last = st->last_frame;
            st->last_frame = frame;
            if (!bad.empty() && !st->bad++) st->first_bad = bad + " на кадре теста " + std::to_string(f);
            if (!bad.empty() && st->bad <= 5)
                FORGE_WARN("кадр героя: %s на кадре теста %u (%s, %.3f, %.3f)", bad.c_str(), f, g.level_id().c_str(), g.hero_x(), g.hero_y());
            // The look at the screen; not while the hero blinks after a heart lost.
            if (every == 0 || f % every != 0 || g.blinking()) return;
            std::vector<u8> px;
            u32 w = 0, hgt = 0;
            if (!frame_pixels(s, px, w, hgt)) return;
            const Match m = drawn_frame(px, w, hgt, g.camera(), g.hero_drawn_x(), g.hero_drawn_y(), 1, 2, st->hero, 4, 2);
            ++st->looks;
            if (m.score < 0.9 || m.frame != static_cast<i32>(g.hero_frame())) {
                if (st->bad_looks++ < 5)
                    FORGE_WARN("кадр героя на экране %d (совпало %.2f), игра говорит %u, кадр теста %u", m.frame, m.score, g.hero_frame(), f);
            }
            st->flipped += m.flip;
        };
        auto frames_ok = [st, this](const std::string& part) {
            check(st->bad == 0, part + ": кадр героя не по состоянию " + std::to_string(st->bad) + " раз, первый: " + st->first_bad);
            check(st->bad_looks == 0, part + ": на экране не тот кадр героя " + std::to_string(st->bad_looks) + " раз из " +
                                          std::to_string(st->looks));
            FORGE_INFO("%s: кадры героя стоит %u, шаг %u и %u, в воздухе %u; снимков %u", part.c_str(), st->seen[0], st->seen[1],
                       st->seen[2], st->seen[3], st->looks);
        };
        // The hero standing on the floor: frame 0 on the screen where it was drawn (mirrored when it faces left), its
        // feet on the tile's top edge to the pixel (the brief: ±1): the screen's row 2 pixels above the edge is the
        // picture's sole, the row 2 below it the tile's own (its row 1) with nothing of the hero.
        auto feet_on_edge = [&s, &g, st, this, num](world::TileId tile, bool left) {
            std::vector<u8> px;
            u32 w = 0, h = 0;
            if (!check_ok(frame_pixels(s, px, w, h), "кадр снят")) return;
            const render::Camera2D& c = g.camera();
            const f64 hx = g.hero_drawn_x(), hy = g.hero_drawn_y();
            const Match m = drawn_frame(px, w, h, c, hx, hy, 1, 2, st->hero, 4, 2);
            check(m.frame == 0 && m.score > 0.9 && m.flip == left && std::abs(m.dx) <= 1 && std::abs(m.dy) <= 1,
                  std::string("герой стоит кадром 0") + (left ? ", отражён" : "") + ", где нарисован: кадр " + std::to_string(m.frame) +
                      (m.flip ? " отражён" : "") + ", совпало " + num(m.score) + ", сдвиг " + std::to_string(m.dx) + ", " + std::to_string(m.dy));
            const i32 row = static_cast<i32>(std::lround(g.hero_y() + kHeroHalfH));
            check(g.on_ground() && g.tile(kBlocks, static_cast<i32>(std::floor(hx)), row) == tile,
                  "под героем тайл " + std::to_string(tile) + ": " + std::to_string(g.tile(kBlocks, static_cast<i32>(std::floor(hx)), row)));
            // The sole: the lowest row of frame 0 with an opaque pixel, and a column near the middle opaque in it and in
            // the two rows above.
            const assets::CookedTexture& pic = st->hero;
            const u32 fw = pic.width / 4, fh = pic.height;
            auto opaque = [&pic](i32 x, i32 y) {
                return x >= 0 && y >= 0 && pic.rgba8[(static_cast<usize>(y) * pic.width + static_cast<usize>(x)) * 4 + 3] >= 128;
            };
            i32 bottom = -1, col = -1;
            for (i32 y = 0; y < static_cast<i32>(fh); ++y)
                for (i32 x = 0; x < static_cast<i32>(fw); ++x)
                    if (opaque(x, y)) bottom = y;
            for (i32 x = 0; x < static_cast<i32>(fw); ++x)
                if (opaque(x, bottom) && opaque(x, bottom - 1) && opaque(x, bottom - 2) &&
                    (col < 0 || std::abs(2 * x + 1 - static_cast<i32>(fw)) < std::abs(2 * col + 1 - static_cast<i32>(fw))))
                    col = x;
            if (!check_ok(bottom > 2 && col >= 0, "у кадра 0 героя есть подошва в три строки")) return;
            const f64 top = (hy - 1 - c.snapped_y()) * c.zoom + h * 0.5 + m.dy, scale = 2.0 * c.zoom / fh;
            const f64 feet_px = top + (bottom + 1) * scale, edge_px = (row - c.snapped_y()) * c.zoom + h * 0.5;
            check(std::fabs(feet_px - edge_px) <= 1.0, "ступни героя на краю тайла: низ ступней " + num(feet_px) + ", край " + num(edge_px) +
                                                          " (пикселей экрана, ±1)");
            const i32 pic_col = left ? static_cast<i32>(fw) - 1 - col : col;
            const f64 sx = (hx - 0.5 - c.snapped_x()) * c.zoom + w * 0.5 + m.dx + (pic_col + 0.5) * (static_cast<f64>(c.zoom) / fw);
            const f64 wx = (sx - w * 0.5) / c.zoom + c.snapped_x();
            const f64 ya = edge_px - 1.5, yb = edge_px + 1.5;
            const i32 ra = static_cast<i32>(std::floor((ya - top) / scale)), rb = static_cast<i32>(std::floor((yb - top) / scale));
            const world::TileId under = g.tile(kBlocks, static_cast<i32>(std::floor(wx)), row);
            const u32 cell = (under - forge::level::kFirstOwnTile) * 32 + static_cast<u32>(std::clamp((wx - std::floor(wx)) * 32, 0.0, 31.0));
            if (!check_ok(under >= forge::level::kFirstOwnTile && under < forge::level::kFirstOwnTile + 16 && st->tiles.width == 512,
                          "под ступнёй тайл шаблона: " + std::to_string(under)))
                return;
            const u8* sole = &pic.rgba8[(static_cast<usize>(std::clamp(ra, 0, static_cast<i32>(fh) - 1)) * pic.width + static_cast<usize>(col)) * 4];
            const u8* ground = &st->tiles.rgba8[(static_cast<usize>(st->tiles.width) + cell) * 4]; // its row 1
            const std::array<int, 3> above = pixel_at(px, w, h, static_cast<f32>(sx), static_cast<f32>(ya)),
                                     below = pixel_at(px, w, h, static_cast<f32>(sx), static_cast<f32>(yb));
            check(ra >= 0 && ra <= bottom && opaque(col, ra) && near_rgb(above, {sole[0], sole[1], sole[2]}, 40),
                  "2 пикселя над краем — подошва героя: на экране " + rgb_text(above) + ", в картинке " + rgb_text({sole[0], sole[1], sole[2]}) +
                      " (строка " + std::to_string(ra) + ")");
            check(rb > bottom && near_rgb(below, {ground[0], ground[1], ground[2]}, 40),
                  "2 пикселя под краем — тайл, без героя: на экране " + rgb_text(below) + ", в тайле " +
                      rgb_text({ground[0], ground[1], ground[2]}) + " (строка картинки героя " + std::to_string(rb) + ")");
        };
        // Gets the hero to stand: no keys until it is on the ground and still for a few frames.
        auto settle = [&g, st](u32 f) {
            g.script(Controls{});
            // Not watched meanwhile: the frames after it start anew.
            st->seen_on.clear();
            if (f == 0) st->still = 0;
            if (!g.on_ground() || std::fabs(g.hero_x() - st->x0) > 1e-6) {
                st->x0 = g.hero_x();
                st->still = f;
                return false;
            }
            return f >= st->still + 8;
        };
        auto pictures = [&s, &g, st, picture, this] {
            check(picture(s.game_dir() / "pictures" / utf8_path("герой.png"), st->hero) && st->hero.width == 128 && st->hero.height == 64,
                  "картинка героя 128 × 64");
            check(picture(s.game_dir() / "pictures" / utf8_path("жук.png"), st->beetle) && st->beetle.width == 64 && st->beetle.height == 32,
                  "картинка «Жука» 64 × 32");
            check(picture(forge::level::level_folder(s.game_dir(), g.level_id()) / "tiles.png", st->tiles) && st->tiles.width == 512 &&
                      st->tiles.height == 32,
                  "тайлы уровня 512 × 32");
        };
        auto notes_text = [st] {
            std::string out;
            for (const auto& [k, v] : st->notes) out += k + " " + std::to_string(v) + "\n";
            return out;
        };

        if (!continued) {
            steps_.push_back({"шаблон «Платформер»: копия его папки игры", 10, [&s, &g, this](u32 f) {
                if (f < 5) return false;
                std::error_code ec;
                check(fs::equivalent(s.game_dir(), tpl::root() / "data", ec), "игра читает данные из " + path_to_utf8(s.game_dir()));
                check(s.data_errors().empty(), "данные шаблона читаются без ошибок");
                check(s.title() == "Платформер", "название из game.json: «" + s.title() + "»");
                g.set_level({});
                check(s.new_game(), "новая игра");
                return true;
            }});
            // The templates' pictures as the game cuts them (slice::Pictures), on a copy of the template's «Герой»
            // and «Жук» with two more of kind «Герой»: a strip of 4 is 4 frames of the sheet, pixel for pixel; of two
            // heroes the first by id is drawn, one of 2 frames is not (a warning); a strip its frames do not divide is
            // one frame (a warning).
            steps_.push_back({"картинки шаблонов: полоса — кадры листа, герой — первый по id", 2, [&s, this, picture](u32) {
                const fs::path dir = tpl::root() / utf8_path("картинки");
                std::error_code ec;
                fs::remove_all(dir, ec);
                fs::create_directories(dir / "objects", ec);
                fs::copy(s.game_dir() / "pictures", dir / "pictures", fs::copy_options::recursive, ec);
                fs::copy_file(s.game_dir() / "kinds.json", dir / "kinds.json", ec);
                for (const char* f : {"hero.object.json", "beetle.object.json"})
                    if (!ec) fs::copy_file(s.game_dir() / "objects" / f, dir / "objects" / f, ec);
                auto put = [&dir](const char* id, const char* picture_name, u32 frames) {
                    const std::string text = std::string("{\"id\": \"") + id + "\", \"name\": \"" + id + "\", \"kind\": \"hero\", \"picture\": \"" +
                                             picture_name + "\", \"frames\": " + std::to_string(frames) + "}\n";
                    return write_file_atomic(dir / "objects" / (std::string(id) + ".object.json"),
                                             {reinterpret_cast<const u8*>(text.data()), text.size()});
                };
                objects::Library lib;
                std::string why;
                if (!check_ok(!ec && lib.load(dir / "kinds.json", dir / "objects", &why), "шаблоны читаются: " + why)) return true;
                const objects::Template* hero = lib.find("hero_look");
                const objects::Template* beetle = lib.find("beetle");
                if (!check_ok(hero && beetle, "«Герой» и «Жук» есть")) return true;
                const u64 hero_key = hero->key, beetle_key = beetle->key;
                assets::CookedTexture strip;
                if (!check_ok(picture(dir / "pictures" / utf8_path("герой.png"), strip) && strip.width == 128 && strip.height == 64,
                              "картинка героя 128 × 64"))
                    return true;
                Pictures pics;
                demo::SheetImage base, sheet;
                pics.update(lib, base, sheet);
                const Pictures::Picture* h = pics.hero();
                check(h && h == pics.of(hero_key) && h->frames == 4 && std::fabs(h->aspect - 0.5f) < 1e-6f,
                      "герой — «Герой» шаблона, 4 кадра 32 × 64");
                u32 same = 0;
                for (u32 k = 0; h && k < 4 && h->frame + k < sheet.frames.size(); ++k) {
                    const render::SpriteRect r = sheet.frames[h->frame + k];
                    bool equal = r.w == 32 && r.h == 64;
                    for (u32 y = 0; equal && y < 64; ++y)
                        equal = std::equal(&strip.rgba8[(static_cast<usize>(y) * 128 + k * 32) * 4],
                                           &strip.rgba8[(static_cast<usize>(y) * 128 + k * 32 + 32) * 4],
                                           &sheet.rgba[((static_cast<usize>(r.y) + y) * sheet.width + r.x) * 4]);
                    same += equal;
                }
                check(same == 4, "в листе 4 кадра героя, каждый — свои 32 столбца полосы: совпало " + std::to_string(same));
                const Pictures::Picture* b = pics.of(beetle_key);
                check(b && b->frames == 2 && std::fabs(b->aspect - 1.0f) < 1e-6f, "«Жук» — 2 кадра 32 × 32");
                check(put("a_hero", "сердце.png", 1) && put("aa_hero", "герой.png", 2), "ещё два шаблона вида «Герой»");
                lib.reload_templates();
                const objects::Template* second = lib.find("a_hero");
                if (!check_ok(second != nullptr, "второй «Герой» прочитан")) return true;
                pics.update(lib, base, sheet);
                check(pics.hero() && pics.hero() == pics.of(second->key) && pics.hero()->frames == 1,
                      "из двух героев рисуется первый по id («a_hero», 1 кадр); «aa_hero» с 2 кадрами — нет");
                check(pics.of(lib.find("aa_hero")->key) && pics.of(lib.find("aa_hero")->key)->frames == 2, "его полоса — 2 кадра");
                std::string text;
                {
                    std::vector<u8> bytes;
                    read_file(dir / "objects" / "beetle.object.json", bytes);
                    text.assign(bytes.begin(), bytes.end());
                }
                const std::string two = "\"frames\": 2";
                const usize at = text.find(two);
                if (check_ok(at != std::string::npos, "у «Жука» 2 кадра в файле")) {
                    text.replace(at, two.size(), "\"frames\": 3");
                    check(write_file_atomic(dir / "objects" / "beetle.object.json", {reinterpret_cast<const u8*>(text.data()), text.size()}),
                          "«Жуку» 3 кадра");
                    lib.reload_templates();
                    pics.update(lib, base, sheet);
                    check(pics.of(beetle_key) && pics.of(beetle_key)->frames == 1 && std::fabs(pics.of(beetle_key)->aspect - 2.0f) < 1e-6f,
                          "ширину 64 не делят 3 кадра: картинка целиком, один кадр");
                }
                // A hero of 3 frames (128 does not divide into them) and one of 4 frames 130 wide are no hero's, though
                // first by id: the game draws by «a_hero» still, and hero_picture_ok (the editor's note) says the same.
                auto strip_png = [&dir](const char* name, u32 w, u32 ht, u32 frames, auto colour) {
                    assets::CookedTexture t;
                    t.width = w;
                    t.height = ht;
                    t.rgba8.resize(static_cast<usize>(w) * ht * 4);
                    for (u32 y = 0; y < ht; ++y)
                        for (u32 x = 0; x < w; ++x) {
                            const std::array<u8, 4> c = colour(x / (w / frames));
                            std::copy(c.begin(), c.end(), &t.rgba8[(static_cast<usize>(y) * w + x) * 4]);
                        }
                    std::vector<u8> png;
                    return assets::encode_image(t, ".png", png) && write_file_atomic(dir / "pictures" / utf8_path(name), png);
                };
                // Frame k of a test strip: a colour of its own, opaque.
                auto colour = [](u32 k) { return std::array<u8, 4>{static_cast<u8>(20 + k * 29), static_cast<u8>(230 - k * 23), static_cast<u8>(k % 2 ? 60 : 190), 255}; };
                check(put("a0_hero", "герой.png", 3) && strip_png("герой130.png", 130, 64, 1, colour) && put("a1_hero", "герой130.png", 4),
                      "«Герой» на 3 кадра и «Герой» на 4 кадра шириной 130");
                lib.reload_templates();
                pics.update(lib, base, sheet);
                check(pics.hero() && pics.hero() == pics.of(lib.find("a_hero")->key),
                      "3 кадра и неделимая ширина — не герой, хоть и первые по id: рисует «a_hero»");
                check(pics.of(lib.find("a0_hero")->key) && pics.of(lib.find("a0_hero")->key)->frames == 1, "у «a0_hero» картинка целиком");
                std::string why0, why1;
                check(!slice::hero_picture_ok(lib, *lib.find("a0_hero"), &why0) && !slice::hero_picture_ok(lib, *lib.find("a1_hero"), &why1) &&
                          slice::hero_picture_ok(lib, *lib.find("a_hero")) && slice::hero_picture_ok(lib, *lib.find("hero_look")) &&
                          !slice::hero_picture_ok(lib, *lib.find("aa_hero")),
                      "правило героя для редактора то же: «" + why0 + "», «" + why1 + "»");
                // Strips at the sheet's limits, each frame its own colour: 8 frames of 32 and 4 of 256 (a frame as wide as
                // a picture may be), and 8 frames of 256 × 512 scaled down to 128 × 256; on a sheet as narrow as it gets
                // (no drawn frames: kMaxSide + 2) and on one 512 wide. Every frame inside the sheet, apart from the
                // others, all of its pixels its frame's colour.
                struct Strip {
                    const char* id;
                    const char* file;
                    u32 w, h, frames, fw, fh;
                };
                const Strip strips[] = {{"strip8", "полоса8.png", 256, 32, 8, 32, 32},
                                        {"strip4", "полоса4.png", 1024, 64, 4, 256, 64},
                                        {"strip8_big", "полоса8_большая.png", 2048, 512, 8, 128, 256}};
                bool written = true;
                for (const Strip& p : strips) {
                    written &= strip_png(p.file, p.w, p.h, p.frames, colour);
                    const std::string json = std::string("{\"id\": \"") + p.id + "\", \"name\": \"" + p.id + "\", \"kind\": \"picture\", \"picture\": \"" +
                                             p.file + "\", \"frames\": " + std::to_string(p.frames) + "}\n";
                    written &= write_file_atomic(dir / "objects" / (std::string(p.id) + ".object.json"),
                                                 {reinterpret_cast<const u8*>(json.data()), json.size()});
                }
                if (!check_ok(written, "полосы записаны")) return true;
                lib.reload_templates();
                demo::SheetImage wide;
                wide.width = 512;
                wide.height = 32;
                wide.rgba.assign(512 * 32 * 4, 255);
                wide.frames = {{0, 0, 512, 32}};
                for (const demo::SheetImage* under : {&base, &wide}) {
                    Pictures fresh;
                    demo::SheetImage out;
                    fresh.update(lib, *under, out);
                    const std::string size = " (лист " + std::to_string(out.width) + " × " + std::to_string(out.height) + ")";
                    check(out.width == std::max(under->width, Pictures::kMaxSide + 2), "ширина листа" + size);
                    bool inside = true, apart = true;
                    for (usize i = under->frames.size(); i < out.frames.size(); ++i) {
                        const render::SpriteRect& r = out.frames[i];
                        inside &= r.x >= 1 && r.y >= under->height + 1 && r.x + r.w + 1 <= out.width && r.y + r.h + 1 <= out.height;
                        for (usize j = under->frames.size(); j < i; ++j) {
                            const render::SpriteRect& o = out.frames[j];
                            apart &= r.x >= o.x + o.w + 1 || o.x >= r.x + r.w + 1 || r.y >= o.y + o.h + 1 || o.y >= r.y + r.h + 1;
                        }
                    }
                    check(inside && apart, "все кадры в листе, с пикселем вокруг, друг на друга не заходят" + size);
                    for (const Strip& p : strips) {
                        const Pictures::Picture* pic = fresh.of(lib.find(p.id)->key);
                        if (!check_ok(pic && pic->frames == p.frames && std::fabs(pic->aspect - static_cast<f32>(p.fw) / static_cast<f32>(p.fh)) < 1e-6f,
                                      std::string(p.id) + ": " + std::to_string(p.frames) + " кадров " + std::to_string(p.fw) + " × " +
                                          std::to_string(p.fh) + size))
                            continue;
                        u32 right = 0;
                        for (u32 k = 0; k < p.frames && pic->frame + k < out.frames.size(); ++k) {
                            const render::SpriteRect& r = out.frames[pic->frame + k];
                            bool filled = r.w == p.fw && r.h == p.fh;
                            const std::array<u8, 4> c = colour(k);
                            for (u32 y = 0; filled && y < r.h; ++y)
                                for (u32 x = 0; filled && x < r.w; ++x)
                                    filled = std::equal(c.begin(), c.end(), &out.rgba[((static_cast<usize>(r.y) + y) * out.width + r.x + x) * 4]);
                            right += filled;
                        }
                        check(right == p.frames, std::string(p.id) + ": каждый кадр в листе — свой цвет целиком: " + std::to_string(right) + " из " +
                                                     std::to_string(p.frames) + size);
                    }
                }
                fs::remove_all(dir, ec);
                return true;
            }});
            // The hero's frame by the ticks (HeroLook, which the game draws by): a walk starts with step 1 and steps
            // every 6 ticks; one or two slow ticks in a walk (a step walked up stops the hero for one) show no frame of
            // standing, a third does; off the ground the walk's frame for 3 ticks, then the air's; a jump's at once.
            steps_.push_back({"кадр героя по тикам: без мигнувшего «стоит»", 2, [this](u32) {
                HeroLook look;
                const int level = 0;
                u64 tick = 100;
                // The frames of ticks one after another: (on the ground, speed) each, a jump's push on the last.
                auto frames = [&](std::initializer_list<std::pair<bool, f32>> ticks, bool jump = false) {
                    std::string out;
                    usize i = 0;
                    for (const auto& [ground, vx] : ticks) {
                        const u64 now = tick++;
                        const bool last = ++i == ticks.size();
                        out += std::to_string(look.see(&level, now, ground, vx, jump && last ? now : HeroLook::kNever));
                    }
                    return out;
                };
                std::string seq = frames({{true, 0}});
                check(seq == "0", "стоит: " + seq);
                seq = frames({{true, 8.5f}, {true, 8.5f}, {true, 8.5f}, {true, 8.5f}, {true, 8.5f}, {true, 8.5f}, {true, 8.5f},
                              {true, 8.5f}, {true, 8.5f}, {true, 8.5f}, {true, 8.5f}, {true, 8.5f}, {true, 8.5f}});
                check(seq == "1111112222221", "идёт: шаг 1 и шаг 2 по 6 тиков: " + seq);
                seq = frames({{true, 0}, {true, 1.2f}});
                check(seq == "11", "упёрся в ступеньку на тик и пошёл: " + seq);
                seq = frames({{true, 0}, {true, 0}, {true, 0}});
                check(seq == "110", "остановился: «стоит» на третьем медленном тике: " + seq);
                seq = frames({{true, 8.5f}, {false, 8.5f}, {false, 8.5f}, {false, 8.5f}, {false, 8.5f}});
                check(seq == "11113", "сошёл с края: 3 тика шаг, потом «в воздухе»: " + seq);
                seq = frames({{true, 8.5f}, {false, 8.5f}}, true);
                check(seq == "13", "прыжок: «в воздухе» сразу: " + seq);
                return true;
            }});
            steps_.push_back({"новая игра: «Луг», HUD без сырых {…}, герой своей картинкой стоит на траве", 120,
                              [&s, &g, this, st, settle, hud_says, hud_text, feet_on_edge, pictures, num, where, var](u32 f) {
                if (!settle(f)) return f >= 110 && check_ok(false, "герой не встал" + where());
                pictures();
                check(g.level_id() == "level" && std::fabs(g.hero_x() - 3.5) < 0.01 && std::fabs(g.hero_y() + kHeroHalfH) < 0.05,
                      "герой у точки появления «Луга»: " + where());
                check(g.camera().zoom == 32, "камера шаблона: 32 пикселя на клетку, а не " + num(g.camera().zoom));
                check(g.hearts() == 3 && g.score() == 0 && var("inv.coins") == 0 && !g.won() && !g.lost(), "три сердца, ноль очков и монет");
                check(s.screens().shown("hud") && hud_says() && hud_text().find('{') == std::string::npos, "HUD: " + hud_text());
                check(g.hero_pictured() && g.hero_frame() == 0, "герой нарисован картинкой шаблона «Герой», кадр «стоит»");
                check(g.copies_of("sign").size() == 2 && g.copies_of("coin").size() == 8 && g.copies_of("beetle").size() == 2,
                      "на «Луге» два «Указателя», восемь «Монеток» и два «Жука»");
                feet_on_edge(tpl::kGrass, false);
                return true;
            }});
            steps_.push_back({"влево: герой отражён, шагает и встаёт кадрами своей картинки", 150, [&g, this, st, settle, watch, feet_on_edge](u32 f) {
                if (f < 12) {
                    Controls c;
                    c.left = true;
                    g.script(c);
                    watch(f, 3);
                    return false;
                }
                if (!settle(f - 12)) {
                    watch(f, 0);
                    return f >= 140 && check_ok(false, "герой не встал");
                }
                check(st->seen[1] > 0 && st->looks > 0 && st->flipped > 0, "шаг влево снят, отражённым");
                feet_on_edge(tpl::kGrass, true);
                return true;
            }});
            steps_.push_back({"«Жук» шагает двумя кадрами своей картинки", 200, [&s, &g, this, st](u32 f) {
                if (f == 0) {
                    check(g.spawn_copy("beetle", 9.5, 0, tpl::kTestBeetle) != 0, "проверочный «Жук» у героя");
                    return false;
                }
                const flecs::entity_t e = g.copy_with_id(tpl::kTestBeetle);
                f64 x = 0, y = 0;
                if (!e || !g.position_of(e, x, y)) return check_ok(false, "проверочного «Жука» нет");
                if (f < 10) {
                    st->x0 = x;
                    return false;
                }
                const bool walking = std::fabs(x - st->x0) > 0.01;
                st->x0 = x;
                if (walking) {
                    std::vector<u8> px;
                    u32 w = 0, h = 0;
                    if (frame_pixels(s, px, w, h)) {
                        const Match m = drawn_frame(px, w, h, g.camera(), x, y, 1, 1, st->beetle, 2, 4);
                        ++st->beetle_looks;
                        if (m.score > 0.85) ++st->beetle_frames[m.frame];
                    }
                }
                if (f < 120 && (st->beetle_frames[0] < 3 || st->beetle_frames[1] < 3)) return false;
                check(st->beetle_frames[0] >= 3 && st->beetle_frames[1] >= 3, "идёт: на экране оба кадра «Жука»: " +
                                                                                 std::to_string(st->beetle_frames[0]) + " и " +
                                                                                 std::to_string(st->beetle_frames[1]) + " из " +
                                                                                 std::to_string(st->beetle_looks));
                return true;
            }});
            steps_.push_back({"Esc: пауза, мир стоит; Esc ещё раз — игра дальше", 100, [&s, &g, this, st](u32 f) {
                const flecs::entity_t e = g.copy_with_id(tpl::kTestBeetle);
                f64 x = 0, y = 0;
                if (!e || !g.position_of(e, x, y)) return check_ok(false, "проверочного «Жука» нет");
                if (f == 0 || f == 40) {
                    key(s, SDLK_ESCAPE, true);
                    key(s, SDLK_ESCAPE, false);
                    return false;
                }
                if (f == 2) {
                    check(s.screen() == Screen::Paused, "Esc открыл паузу игры");
                    st->x0 = x;
                }
                if (f == 38) check(s.screen() == Screen::Paused && x == st->x0, "на паузе «Жук» стоит: " + std::to_string(x - st->x0));
                if (f == 42) {
                    check(s.screen() == Screen::Playing, "Esc ещё раз — обратно в игру");
                    st->x0 = x;
                }
                if (f < 90) return false;
                check(std::fabs(x - st->x0) > 0.1, "после паузы «Жук» идёт дальше: " + std::to_string(x - st->x0));
                return true;
            }});
            steps_.push_back({"бот проходит «Луг»: ямы, «Жуки», монеты; «Выход на Холмы»", 3600, [&g, this, st, drive, watch, frames_ok,
                                                                                                    in, there, var, where, num](u32 f) {
                if (f == 0) {
                    st->score0 = g.score();
                    st->coins0 = var("inv.coins");
                }
                if (g.level_id() == "level") {
                    if (g.lost() || g.won()) return check_ok(false, "игра окончилась на «Луге»" + where());
                    g.script(drive());
                    watch(f, 20);
                    return false;
                }
                g.script(Controls{});
                check(g.level_id() == "hills" && g.travels() == 1 && in(tpl::area(1, 0)), "на «Холмах», во «Входе»" + where());
                frames_ok("«Луг»");
                check(st->seen[0] > 0 && st->seen[1] > 0 && st->seen[2] > 0 && st->seen[3] > 0, "все четыре кадра героя показаны");
                for (u32 i : {1u, 2u, 3u}) check(!there(tpl::id(0, i)), "«Монетка» " + std::to_string(i) + " «Луга» на пути собрана");
                check(var("inv.coins") >= 3, "монеты по пути собраны: " + num(var("inv.coins")));
                check(g.score() == 10 * var("inv.coins") + 100 * g.stomps(),
                      "очки: 10 за монету, 100 за «Жука»: " + num(g.score()) + " при монетах " + num(var("inv.coins")) + " и победах " +
                          std::to_string(g.stomps()));
                check(g.hearts() >= 1, "сердца есть: " + num(g.hearts()));
                FORGE_INFO("«Луг» пройден: очки %.0f, монеты %.0f, сердца %.0f, победы %u, ранен %u + %u, падений %u", g.score(),
                           var("inv.coins"), g.hearts(), g.stomps(), g.enemy_hits(), g.hazard_hits(), g.falls());
                return true;
            }});
            steps_.push_back({"«Холмы» до «Шипов», сохранение", 1200, [&s, &g, this, st, drive, watch, settle, frames_ok, there, var, where,
                                                                         hud_says, hud_text, notes_text](u32 f) {
                if (f == 0) st->at = 0;
                if (!st->at) {
                    if (g.hero_x() < 30.5 || !g.on_ground()) {
                        g.script(drive());
                        watch(f, 0);
                        return f >= 900 && check_ok(false, "бот не дошёл до «Шипов»" + where());
                    }
                    st->at = f;
                }
                if (!settle(f - st->at)) return false;
                frames_ok("«Холмы», до «Шипов»");
                for (u32 i : {1u, 2u, 3u}) check(!there(tpl::id(1, i)), "«Монетка» " + std::to_string(i) + " «Холмов» собрана");
                check(there(tpl::id(1, 6)) && there(tpl::id(1, 10)), "монетки дальше по «Холмам» лежат");
                check(hud_says(), "HUD: " + hud_text());
                check(s.save("холмы", "Холмы"), "сохранено на «Холмах»");
                st->notes = {{"x", g.hero_x()}, {"y", g.hero_y()}, {"score", g.score()}, {"coins", var("inv.coins")}, {"hearts", g.hearts()}};
                const std::string text = notes_text();
                check(write_file_atomic(tpl::notes(), {reinterpret_cast<const u8*>(text.data()), text.size()}), "заметки для второго процесса");
                return true;
            }});
            steps_.push_back({"три падения в «Пропасть»: поражение, окно «при поражении»", 600, [&s, &g, this, st, said, hud_says, hud_text,
                                                                                                    where, num](u32 f) {
                if (f == 0) st->at = 0;
                if (!g.lost()) {
                    g.script(Controls{});
                    if (g.safe_time() <= 0 && g.on_ground() && f >= st->at + 10) {
                        g.teleport(26.5, 6); // into the pit between the hills
                        st->at = f;
                    }
                    return f >= 550 && check_ok(false, "поражения нет: сердец " + num(g.hearts()) + where());
                }
                if (f < st->at + 40) return false; // the window comes up
                check(g.falls() == 3 && g.hearts() == 0 && g.endings() == 1, "три падения, сердец нет: падений " + std::to_string(g.falls()));
                check(s.screens().shown("lose") && said("lose", "Заголовок") == "Сердца кончились",
                      "окно «при поражении»: «" + said("lose", "Заголовок") + "»");
                check(said("lose", "Итог") == "Очки: " + std::to_string(static_cast<i64>(g.score())) + ". Попробуйте ещё раз.",
                      "в окне очки: «" + said("lose", "Итог") + "»");
                check(hud_says(), "HUD: " + hud_text());
                std::string why;
                check(!g.can_save(&why), "оконченную игру не сохранить: " + why);
                return true;
            }});
            steps_.push_back({"«Ещё раз»: новая игра на «Луге»", 80, [&s, &g, this, st, press, there, var, where](u32 f) {
                if (f == 0) {
                    check(press("lose", "Кнопка «Ещё раз»"), "«Ещё раз» нажата мышью");
                    return false;
                }
                if (f < 40) return false;
                check(g.running() && !g.lost() && g.endings() == 0 && !s.screens().shown("lose"), "новая игра, окна нет");
                check(g.level_id() == "level" && std::fabs(g.hero_x() - 3.5) < 0.01, "на «Луге» у точки появления" + where());
                check(g.hearts() == 3 && g.score() == 0 && var("inv.coins") == 0, "три сердца, ноль очков и монет");
                check(there(tpl::id(0, 1)) && there(tpl::id(0, 2)) && there(tpl::id(0, 3)) && g.copies_of("coin").size() == 8,
                      "монетки «Луга» снова лежат");
                return true;
            }});
            steps_.push_back({"сохранение «холмы» цело", 40, [&s, &g, this, st, there, var, where](u32 f) {
                if (f == 0) {
                    check(s.load("холмы"), "«холмы» загружается");
                    return false;
                }
                if (f < 15) return false;
                std::map<std::string, f64>& n = st->notes;
                check(g.level_id() == "hills" && std::fabs(g.hero_x() - n["x"]) < 0.01, "«Холмы», где сохранились" + where());
                check(g.score() == n["score"] && var("inv.coins") == n["coins"] && g.hearts() == n["hearts"] && !there(tpl::id(1, 1)),
                      "очки, монеты и сердца как при сохранении");
                return true;
            }});
            return;
        }

        steps_.push_back({"«Продолжить» другим процессом: «Холмы», где сохранились", 40, [&s, &g, this, st, there, var, where, pictures,
                                                                                         hud_says, hud_text](u32 f) {
            std::error_code ec;
            if (f < 5) return false;
            std::map<std::string, f64>& n = st->notes;
            if (f == 5) {
                std::vector<u8> bytes;
                check(read_file(tpl::notes(), bytes), "заметки первого процесса: " + path_to_utf8(tpl::notes()));
                const std::string text(bytes.begin(), bytes.end());
                for (usize start = 0; start < text.size();) {
                    usize end = text.find('\n', start);
                    if (end == std::string::npos) end = text.size();
                    const std::string line = text.substr(start, end - start);
                    if (const usize sp = line.find(' '); sp != std::string::npos) n[line.substr(0, sp)] = std::strtod(line.c_str() + sp + 1, nullptr);
                    start = end + 1;
                }
                FORGE_INFO("рабочая папка: %s", path_to_utf8(fs::current_path(ec)).c_str());
                check(fs::equivalent(s.user_folder(), tpl::root() / "user", ec), "файлы игрока — в " + path_to_utf8(s.user_folder()));
                check(s.screen() == Screen::Main, "игра начинается с главного меню");
                check(s.slots().latest() && s.slots().latest()->id == "холмы", "последнее сохранение — «холмы»");
                check(s.continue_game(), "«Продолжить»");
                return false;
            }
            if (f < 15) return false;
            pictures();
            check(g.level_id() == "hills" && std::fabs(g.hero_x() - n["x"]) < 0.01 && std::fabs(g.hero_y() - n["y"]) < 0.05,
                  "герой на «Холмах», где сохранился" + where());
            check(g.score() == n["score"] && var("inv.coins") == n["coins"] && g.hearts() == n["hearts"], "очки, монеты и сердца как при сохранении");
            for (u32 i : {1u, 2u, 3u}) check(!there(tpl::id(1, i)), "собранной «Монетки» " + std::to_string(i) + " нет");
            check(there(tpl::id(1, 6)) && there(tpl::id(1, 10)), "несобранные лежат");
            check(hud_says(), "HUD: " + hud_text());
            st->score0 = g.score();
            st->coins0 = var("inv.coins");
            return true;
        }});
        steps_.push_back({"наград второй раз нет", 70, [&g, this, st, var](u32 f) {
            if (f < 60) return false;
            check(g.score() == st->score0 && var("inv.coins") == st->coins0, "через секунду очки и монеты те же");
            return true;
        }});
        steps_.push_back({"бот на мостках «Холмов»: ступни на краю доски", 1500, [&g, this, st, drive, watch, settle, feet_on_edge, where](u32 f) {
            if (f == 0) st->at = 0;
            if (!st->at) {
                if (g.hero_x() < 54.2 || !g.on_ground()) {
                    g.script(drive());
                    watch(f, 25);
                    return f >= 1200 && check_ok(false, "бот не дошёл до мостков" + where());
                }
                st->at = f;
            }
            if (!settle(f - st->at)) return false;
            feet_on_edge(tpl::kBridge, false);
            return true;
        }});
        steps_.push_back({"бот проходит «Холмы» и «Вершину» до «Флага»: победа, окно «при победе»", 4800,
                          [&s, &g, this, st, drive, watch, frames_ok, said, var, where, num](u32 f) {
            if (f == 0) st->at = 0;
            if (!g.won()) {
                if (g.lost()) return check_ok(false, "игра проиграна" + where());
                g.script(drive());
                watch(f, 20);
                return f >= 4700 && check_ok(false, "до «Флага» не дошёл" + where());
            }
            g.script(Controls{});
            if (!st->at) st->at = f;
            if (f < st->at + 40) return false; // the window comes up
            check(g.level_id() == "summit" && g.travels() == 1 && g.endings() == 1, "победа на «Вершине»" + where());
            frames_ok("«Холмы» и «Вершина»");
            check(st->seen[0] > 0 && st->seen[1] > 0 && st->seen[2] > 0 && st->seen[3] > 0, "все четыре кадра героя показаны");
            const f64 coins = var("inv.coins");
            check(g.score() == st->score0 + 10 * (coins - st->coins0) + 100 * g.stomps(),
                  "очки: сохранённые, 10 за новую монету, 100 за «Жука»: " + num(g.score()));
            check(s.screens().shown("win") && said("win", "Заголовок") == "Победа!", "окно «при победе»: «" + said("win", "Заголовок") + "»");
            check(said("win", "Итог") == "Флаг взят. Очки: " + std::to_string(static_cast<i64>(g.score())) + ", монет: " +
                                             std::to_string(static_cast<i64>(coins)) + ".",
                  "в окне итог: «" + said("win", "Итог") + "»");
            FORGE_INFO("победа: очки %.0f, монеты %.0f, сердца %.0f, победы %u, ранен %u + %u, падений %u", g.score(), coins, g.hearts(),
                       g.stomps(), g.enemy_hits(), g.hazard_hits(), g.falls());
            return true;
        }});
        steps_.push_back({"«Ещё раз» после победы, сохранение «холмы» цело", 120, [&s, &g, this, st, press, there, var, where](u32 f) {
            if (f == 0) {
                check(press("win", "Кнопка «Ещё раз»"), "«Ещё раз» нажата мышью");
                return false;
            }
            if (f == 40) {
                check(g.running() && !g.won() && g.endings() == 0 && !s.screens().shown("win"), "новая игра, окна нет");
                check(g.level_id() == "level" && g.hearts() == 3 && g.score() == 0 && var("inv.coins") == 0 && there(tpl::id(0, 1)),
                      "«Луг», три сердца, ноль очков, монетки лежат" + where());
                check(s.load("холмы"), "«холмы» загружается");
                return false;
            }
            if (f < 60) return false;
            std::map<std::string, f64>& n = st->notes;
            check(g.level_id() == "hills" && g.score() == n["score"] && var("inv.coins") == n["coins"] && !there(tpl::id(1, 1)),
                  "«Холмы» как при сохранении" + where());
            return true;
        }});
    }

    // A level with tiles of its own (tiles.json and tiles.png, 32 px) and nothing around it (world.json), made here
    // as an import makes one: the hero stands on them, a wall of them stops him and nothing digs them; the sky shines
    // through the background ones; nobody comes to live around and no tiles are there; the editor draws them pixel
    // for pixel, the game's tiles enlarged without smoothing and still tinted on the walls; a «Картинка» stays where
    // it was put and the hero passes it; all of it through a save and a load, and the hero wakes at the spawn point.
    // The «Картинка» template (Табличка) is in this run's copy of the game's data (main).
    void build_own_tiles(Shell& s) {
        SliceGame& g = g_;
        struct State {
            std::filesystem::path dir;
            forge::level::LevelTiles own;
            render::Camera2D cam;
            u32 w = 0, h = 0;
            f64 sign_x = 0, sign_y = 0, saved_x = 0;
            u32 deep = 0; // cells of the game's world under the village with something in them
            Shot editor, game;
        };
        auto st = std::make_shared<State>();
        const i32 v = g.generator().village_y();
        constexpr u32 kPx = 32;
        using Rgba = std::array<u8, 4>;
        // The pictures: rock with seams, a sky that gets lighter downwards, a bush with air around it, brick.
        auto picture = [](u32 i, u32 x, u32 y) -> Rgba {
            switch (i) {
            case 0: return x % 8 == 0 || y % 8 == 0 ? Rgba{90, 50, 30, 255} : Rgba{static_cast<u8>(150 + (x * 3) % 20), 90, 50, 255};
            case 1: return Rgba{static_cast<u8>(80 + y), static_cast<u8>(140 + y), 230, 255};
            case 2: {
                const f64 dx = x + 0.5 - 16, dy = y + 0.5 - 20;
                return dx * dx + dy * dy < 121 ? Rgba{40, static_cast<u8>(140 + (x + y) % 30), 50, 255} : Rgba{0, 0, 0, 0};
            }
            default: return y % 8 == 7 || (x + (y / 8) % 2 * 8) % 16 == 15 ? Rgba{200, 195, 180, 255} : Rgba{170, 60, 55, 255};
            }
        };
        {
            forge::level::LevelTiles& t = st->own;
            t.px = kPx;
            const char* names[] = {"Скала", "Небо", "Куст", "Кирпич"};
            const u32 layers[] = {kBlocks, kWalls, kWalls, kBlocks};
            for (u32 i = 0; i < 4; ++i) {
                t.tiles.push_back({static_cast<world::TileId>(forge::level::kFirstOwnTile + i), names[i], layers[i], layers[i] == kBlocks,
                                   "сцена own_tiles"});
                for (u32 y = 0; y < kPx; ++y)
                    for (u32 x = 0; x < kPx; ++x) {
                        const Rgba c = picture(i, x, y);
                        t.rgba.insert(t.rgba.end(), c.begin(), c.end());
                    }
            }
        }
        // A pixel of an image at a tile point, as the renderers place it around the snapped camera.
        auto pixel = [st](const std::vector<u8>& rgba, f64 tx, f64 ty) {
            const render::Camera2D& c = st->cam;
            const i32 px = static_cast<i32>(std::floor((tx - c.snapped_x()) * c.zoom + st->w * 0.5));
            const i32 py = static_cast<i32>(std::floor((ty - c.snapped_y()) * c.zoom + st->h * 0.5));
            std::array<int, 3> out{-1, -1, -1};
            if (px < 0 || py < 0 || px >= static_cast<i32>(st->w) || py >= static_cast<i32>(st->h) || rgba.size() < st->w * st->h * 4)
                return out;
            const usize i = (static_cast<usize>(py) * st->w + static_cast<usize>(px)) * 4;
            return std::array<int, 3>{rgba[i], rgba[i + 1], rgba[i + 2]};
        };
        // Pixel (x, y) of the cell at (tx, ty): one screen pixel per picture pixel at the zoom of 32.
        auto at = [pixel](const std::vector<u8>& rgba, i32 tx, i32 ty, u32 x, u32 y) {
            return pixel(rgba, tx + (x + 0.5) / kPx, ty + (y + 0.5) / kPx);
        };
        auto rgb = [](const std::array<int, 3>& p) {
            return std::to_string(p[0]) + "," + std::to_string(p[1]) + "," + std::to_string(p[2]);
        };
        auto near = [](const std::array<int, 3>& a, const std::array<int, 3>& b, int most) {
            for (int c = 0; c < 3; ++c)
                if (std::abs(a[c] - b[c]) > most) return false;
            return true;
        };
        // Under the level's floor, where the game's world has its ground: the cells loaded, and of them those with
        // anything on any layer.
        auto below = [&g, v](u32& filled) {
            u32 cells = 0;
            filled = 0;
            for (i32 y = v + 6; y < v + 40; ++y)
                for (i32 x = -24; x < 24; ++x) {
                    if (!g.world() || !g.world()->find_chunk(world::chunk_of(x, y))) continue;
                    ++cells;
                    filled += g.tile(kWalls, x, y) != 0 || g.tile(kBlocks, x, y) != 0 || g.tile(kLiquids, x, y) != 0;
                }
            return cells;
        };
        auto empty_below = [below]() {
            u32 filled = 0;
            return below(filled) > 0 && filled == 0;
        };
        // Far from the level's cells, in chunks it never saved: what the world around makes there. The hero goes
        // there for a moment (it loads the place) and comes back.
        auto far_away = [&g, v](u32& filled, u32& entities) {
            const f64 x = g.hero_x(), y = g.hero_y();
            g.teleport(200.5, v - static_cast<f64>(kHeroHalfH));
            entities = g.entities();
            u32 cells = 0;
            filled = 0;
            for (i32 ty = v + 6; ty < v + 40; ++ty)
                for (i32 tx = 160; tx < 240; ++tx) {
                    if (!g.world() || !g.world()->find_chunk(world::chunk_of(tx, ty))) continue;
                    ++cells;
                    filled += g.tile(kWalls, tx, ty) != 0 || g.tile(kBlocks, tx, ty) != 0 || g.tile(kLiquids, tx, ty) != 0;
                }
            g.teleport(x, y);
            return cells;
        };

        steps_.push_back({"меню", 30, [&s, &g, this](u32 f) {
            if (f < 5) return false;
            g.set_level({});
            check(s.new_game(), "новая игра со своего уровня игры");
            return true;
        }});
        steps_.push_back({"мир игры: под деревней земля, в деревне жители", 20, [&g, below, far_away, this, st](u32 f) {
            if (f < 10) return false;
            u32 filled = 0;
            const u32 cells = below(filled);
            st->deep = filled;
            check(cells > 0 && filled > cells / 2, "под деревней в мире игры земля: клеток " + std::to_string(filled) + " из " +
                                                       std::to_string(cells));
            const u32 here = g.entities();
            u32 there = 0;
            const u32 far = far_away(filled, there);
            check(far > 0 && filled > 0, "и вдали под землёй земля: клеток " + std::to_string(filled) + " из " + std::to_string(far));
            check(there > here, "и там игра заселяет участки: сущностей " + std::to_string(here) + ", там " + std::to_string(there));
            check(g.count_npcs() > 0, "жители игры на месте");
            check(!g.location().empty(), "у мест игры есть имена: " + g.location());
            return true;
        }});
        steps_.push_back({"уровень со своими тайлами, вокруг пусто", 20, [&s, &g, v, this, st](u32 f) {
            if (f > 0) return f >= 2;
            st->dir = std::filesystem::temp_directory_path() / "forge_slice_own_tiles" / "level";
            std::error_code ec;
            std::filesystem::remove_all(st->dir.parent_path(), ec);
            std::filesystem::create_directories(st->dir, ec);
            std::string why;
            check(forge::level::save_world(st->dir, forge::level::LevelWorld{true}, &why), "world.json: вокруг пусто " + why);
            // As the editor (an import) makes it: the level module, its tiles, cells, the spawn point and a «Картинка».
            SliceLevel module;
            check(module.load_objects(s.game_dir(), &why), "шаблоны игры и «Табличка» из копии данных " + why);
            forge::level::Level level(module);
            check(level.open(st->dir, &why) && level.around().empty_around, "уровень открывается, вокруг пусто " + why);
            check(level.set_own_tiles(st->own), "четыре своих тайла ложатся в уровень");
            level.ensure_loaded({-30, v - 30, 30, v + 12});
            u32 painted = 0;
            auto put = [&](u32 layer, i32 x, i32 y, world::TileId t) { painted += level.set_tile(layer, x, y, t); };
            for (i32 x = -24; x < 24; ++x) {
                for (i32 y = v - 16; y < v; ++y) put(kWalls, x, y, 257);
                for (i32 y = v; y < v + 6; ++y) put(kBlocks, x, y, 256);
            }
            for (i32 y = v - 4; y < v; ++y) put(kBlocks, 10, y, 259), put(kBlocks, 11, y, 259);
            for (i32 x = -6; x <= -4; ++x) put(kWalls, x, v - 1, 258);
            put(kBlocks, -10, v - 1, world::TileStone);
            put(kWalls, -12, v - 1, world::TileStoneWall);
            check(painted == 48 * 22 + 8 + 3 + 2, "клетки нарисованы: " + std::to_string(painted));
            forge::level::LevelAreas a;
            a.spawn = true;
            a.spawn_x = 0.5;
            a.spawn_y = v;
            level.set_areas(a);
            const auto& defs = module.objects();
            usize sign = defs.size();
            for (usize i = 0; i < defs.size(); ++i)
                if (defs[i].id == "sign") sign = i;
            check(sign < defs.size(), "«Табличка» вида «Картинка» в палитре объектов");
            flecs::entity e = sign < defs.size() ? module.place_object(level, sign, 4.5, v - 0.5) : flecs::entity();
            check(e.is_valid() && e.has<sim::Body>() && e.get<sim::Body>().gravity == 0 && !e.get<sim::Body>().collide &&
                      e.get<sim::Body>().half_h == 1.0f,
                  "«Табличка» поставлена: не падает, ни во что не упирается, высотой 2 клетки");
            if (e.is_valid()) {
                st->sign_x = e.get<scene::Position>().tile_x();
                st->sign_y = e.get<scene::Position>().tile_y();
                level.touch_objects();
            }
            const forge::level::Level::SaveReport r = level.save();
            check(r.ok && r.own_tiles && r.areas && r.tile_chunks > 0, "уровень записан: " + r.error);
            for (const char* file : {"tiles.json", "tiles.png", "world.json", "areas.json"})
                check(std::filesystem::is_regular_file(st->dir / file, ec), std::string("в папке уровня ") + file);
            g.set_level(st->dir);
            check(s.new_game(), "новая игра с этого уровня");
            return false;
        }});
        steps_.push_back({"герой на своём полу у точки появления, вокруг никого и ничего", 40, [&g, v, empty_below, far_away, this, st](u32 f) {
            if (f < 30) return false;
            check(g.hero_x() == 0.5 && std::fabs(g.hero_y() - (v - static_cast<f64>(kHeroHalfH))) < 0.01 && g.on_ground(),
                  "герой стоит на «Скале» у точки появления: " + std::to_string(g.hero_x()) + ", " + std::to_string(g.hero_y()));
            check(g.tile(kBlocks, 0, v) == 256 && g.tile(kWalls, 0, v - 5) == 257 && g.tile(kBlocks, 10, v - 2) == 259 &&
                      g.tile(kWalls, -5, v - 1) == 258 && g.tile(kBlocks, -10, v - 1) == world::TileStone,
                  "в игре клетки уровня");
            check(empty_below() && st->deep != 0, "где в мире игры земля, здесь пусто: под полом всё загруженное пусто");
            check(g.count_npcs() == 0, "жителей нет: " + std::to_string(g.count_npcs()));
            u32 items = 0;
            for (u8 k = 0; k <= static_cast<u8>(ItemKind::Key); ++k) items += g.count_items(static_cast<ItemKind>(k));
            check(items == 0, "предметов игры нет: " + std::to_string(items));
            check(g.copies_of("sign").size() == 1, "«Табличка» одна");
            check(g.entities() == 2, "в игре только герой и «Табличка»: сущностей " + std::to_string(g.entities()));
            check(g.location().empty(), "имени места игры нет: «" + g.location() + "»");
            u32 filled = 0, there = 0;
            const u32 far = far_away(filled, there);
            check(far > 0 && filled == 0, "и вдали, где уровень ничего не записал, пусто: клеток " + std::to_string(filled) + " из " +
                                              std::to_string(far));
            check(there == 2, "и там никого не заселили: сущностей " + std::to_string(there));
            check(g.hero_x() == 0.5, "герой вернулся к точке появления");
            return true;
        }});
        steps_.push_back({"стена из своих тайлов держит, «Табличку» герой проходит", 150, [&g, v, this, st](u32 f) {
            if (f < 120) {
                Controls right;
                right.right = true;
                g.script(right);
                return false;
            }
            g.script(Controls{});
            if (f < 130) return false;
            check(g.hero_x() > 9 && g.hero_x() < 10 - kHeroHalfW + 0.01 && g.on_ground(),
                  "герой у «Кирпича», дальше не прошёл: x " + std::to_string(g.hero_x()));
            check(g.hero_x() > st->sign_x + 1, "«Табличку» прошёл насквозь");
            return true;
        }});
        steps_.push_back({"свои тайлы не копаются", 100, [&g, v, this](u32 f) {
            if (f < 90) {
                Controls c;
                c.use = true;
                c.aim_x = f < 45 ? 10.5 : 9.5;
                c.aim_y = f < 45 ? v - 1.5 : v + 0.5;
                g.script(c);
                return false;
            }
            g.script(Controls{});
            check(g.tile(kBlocks, 10, v - 2) == 259 && g.tile(kBlocks, 9, v) == 256, "«Кирпич» и «Скала» на месте");
            return true;
        }});
        steps_.push_back({"«Табличка» стоит, где поставили", 2, [&g, this, st](u32 f) {
            if (f > 0) return true;
            const std::vector<flecs::entity_t> signs = g.copies_of("sign");
            f64 x = 0, y = 0;
            check(signs.size() == 1 && g.position_of(signs[0], x, y) && x == st->sign_x && y == st->sign_y,
                  "«Табличка» на месте: " + std::to_string(x) + ", " + std::to_string(y));
            return true;
        }});
        steps_.push_back({"редактор рисует свои тайлы пиксель в пиксель", 5, [&s, &g, v, picture, pixel, at, rgb, near, this, st](u32 f) {
            if (f > 0) return true;
            st->w = g.frame_width();
            st->h = g.frame_height();
            st->cam = g.camera();
            st->cam.x = -2;
            st->cam.y = v - 4;
            st->cam.zoom = static_cast<f32>(kPx);
            SliceLevel module;
            std::string why;
            check(module.load_objects(s.game_dir(), &why), "шаблоны для редактора " + why);
            check(module.init_view(g.device(), SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM), "вид редактора");
            forge::level::Level level(module);
            check(level.open(st->dir, &why), "редактор открывает уровень " + why);
            const render::Camera2D cam = st->cam;
            level.ensure_loaded(cam.visible_tiles(st->w, st->h).expanded(render::kLightMargin));
            const u32 w = st->w, h = st->h;
            check(shoot(g.device(), w, h,
                        [&](SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* t) {
                            module.prepare_view(cmd, level, cam, w, h, forge::level::ViewOptions{}, 0);
                            SDL_GPUColorTargetInfo info{};
                            info.texture = t;
                            const Color bg = module.background();
                            info.clear_color = SDL_FColor{bg.r, bg.g, bg.b, 1};
                            info.load_op = SDL_GPU_LOADOP_CLEAR;
                            info.store_op = SDL_GPU_STOREOP_STORE;
                            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &info, 1, nullptr);
                            module.draw_view(cmd, pass);
                            SDL_EndGPURenderPass(pass);
                        },
                        module.lights(), st->editor),
                  "кадр вида редактора");
            const std::vector<u8>& img = st->editor.full;
            // Own tiles: every pixel as in its picture, on the walls too (not tinted).
            struct Cell {
                u32 tile;
                i32 x, y;
            };
            u32 checked = 0, same = 0;
            std::string first;
            for (const Cell c : {Cell{0, -20, v + 1}, Cell{1, -20, v - 10}, Cell{3, 10, v - 2}, Cell{2, -5, v - 1}})
                for (u32 y = 0; y < kPx; ++y)
                    for (u32 x = 0; x < kPx; ++x) {
                        const Rgba p = picture(c.tile, x, y);
                        if (p[3] == 0) continue;
                        ++checked;
                        const std::array<int, 3> want{p[0], p[1], p[2]}, got = at(img, c.x, c.y, x, y);
                        if (got == want) ++same;
                        else if (first.empty())
                            first = "тайл " + std::to_string(256 + c.tile) + " пиксель " + std::to_string(x) + "," + std::to_string(y) +
                                    ": " + rgb(got) + " вместо " + rgb(want);
            }
            check(checked > 3 * kPx * kPx && same == checked,
                  "свои тайлы как их картинки: " + std::to_string(same) + " из " + std::to_string(checked) + " пикселей " + first);
            // Where the bush has none, the sky behind the level shows.
            const Color bg = module.background();
            check(near(at(img, -5, v - 1, 0, 0), {static_cast<int>(bg.r * 255 + 0.5f), static_cast<int>(bg.g * 255 + 0.5f),
                                                  static_cast<int>(bg.b * 255 + 0.5f)}, 1),
                  "сквозь прозрачное у «Куста» видно небо: " + rgb(at(img, -5, v - 1, 0, 0)));
            // The game's tiles: twice as big, each pixel of theirs two by two, the walls still tinted.
            const std::vector<u8> atlas = make_atlas();
            const u32 row = demo::kTileCellPx * demo::kTileCells;
            auto game_px = [&](world::TileId t, u32 x, u32 y) {
                const usize i = (static_cast<usize>((t / demo::kTileCells) * demo::kTileCellPx + y / 2) * row + (t % demo::kTileCells) * demo::kTileCellPx + x / 2) * 4;
                return std::array<int, 3>{atlas[i], atlas[i + 1], atlas[i + 2]};
            };
            u32 stone = 0, wall = 0;
            for (u32 y = 0; y < kPx; ++y)
                for (u32 x = 0; x < kPx; ++x) {
                    stone += at(img, -10, v - 1, x, y) == game_px(world::TileStone, x, y);
                    const std::array<int, 3> a = game_px(world::TileStoneWall, x, y);
                    const std::array<int, 3> tinted{static_cast<int>(a[0] * 0.58 + 0.5), static_cast<int>(a[1] * 0.58 + 0.5),
                                                    static_cast<int>(a[2] * 0.64 + 0.5)};
                    wall += near(at(img, -12, v - 1, x, y), tinted, 1);
                }
            check(stone == kPx * kPx, "«Камень» игры увеличен без сглаживания: " + std::to_string(stone) + " из 1024");
            check(wall == kPx * kPx, "«Стена камня» игры по-прежнему темнее: " + std::to_string(wall) + " из 1024");
            // The «Картинка»: its board, two cells tall over its feet.
            check(near(pixel(img, st->sign_x, st->sign_y - 0.75), {150, 105, 60}, 1),
                  "«Табличка» нарисована своей картинкой: " + rgb(pixel(img, st->sign_x, st->sign_y - 0.75)));
            return true;
        }});
        steps_.push_back({"в игре небо светит сквозь свой фон, в толще «Скалы» темно", 5, [&g, v, picture, pixel, at, rgb, near, this, st](u32 f) {
            if (f > 0) return true;
            g.set_hero_light(false);
            g.camera() = st->cam;
            const u32 w = st->w, h = st->h;
            check(shoot(g.device(), w, h, [&g, w, h](SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* t) { g.render(cmd, t, w, h); }, g.lights(),
                        st->game),
                  "кадр игры");
            g.set_hero_light(true);
            const std::array<int, 3> back = pixel(st->game.light, -20.5, v - 10.5), open = pixel(st->game.light, -20.5, v - 17.5),
                                     deep = pixel(st->game.light, -20.5, v + 3.5);
            FORGE_INFO("свет: на своём фоне %s, в пустоте над картой %s, в толще «Скалы» %s", rgb(back).c_str(), rgb(open).c_str(),
                       rgb(deep).c_str());
            check(back[0] > 200 && near(back, open, 2), "свой фон освещён небом, как пустая клетка: " + rgb(back) + " и " + rgb(open));
            check(deep[0] < back[0] - 60, "в толще «Скалы» темнее: " + rgb(deep));
            // The frame: the picture times the light.
            const Rgba p = picture(1, 9, 9);
            const std::array<int, 3> got = at(st->game.full, -20, v - 10, 9, 9);
            const std::array<int, 3> want{p[0] * back[0] / 255, p[1] * back[1] / 255, p[2] * back[2] / 255};
            check(near(got, want, 2), "в кадре игры свой фон — его картинка под светом неба: " + rgb(got) + " и " + rgb(want));
            return true;
        }});
        steps_.push_back({"сохранение и загрузка", 40, [&s, &g, v, empty_below, this, st](u32 f) {
            if (f == 0) {
                st->saved_x = g.hero_x();
                check(s.save("own_tiles", "Свои тайлы"), "игра сохраняется");
                std::error_code ec;
                for (const char* file : {"tiles.json", "tiles.png", "world.json"})
                    check(std::filesystem::is_regular_file(s.slots().folder("own_tiles") / "world" / file, ec),
                          std::string("в сохранении ") + file);
                check(s.load("own_tiles"), "сохранение загружается");
                return false;
            }
            if (f < 30) return false;
            check(std::fabs(g.hero_x() - st->saved_x) < 0.01, "герой где был: " + std::to_string(g.hero_x()));
            check(g.tile(kBlocks, 0, v) == 256 && g.tile(kWalls, -5, v - 1) == 258 && g.tile(kBlocks, -10, v - 1) == world::TileStone,
                  "свои тайлы на месте");
            check(empty_below(), "вокруг по-прежнему пусто");
            check(g.count_npcs() == 0 && g.location().empty(), "жителей и имён мест нет");
            const std::vector<flecs::entity_t> signs = g.copies_of("sign");
            f64 x = 0, y = 0;
            check(signs.size() == 1 && g.position_of(signs[0], x, y) && x == st->sign_x && y == st->sign_y, "«Табличка» на месте");
            return true;
        }});
        steps_.push_back({"после загрузки свои тайлы видны как прежде", 5, [&g, v, at, this, st](u32 f) {
            if (f > 0) return true;
            g.set_hero_light(false);
            g.camera() = st->cam;
            const u32 w = st->w, h = st->h;
            Shot again;
            check(shoot(g.device(), w, h, [&g, w, h](SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* t) { g.render(cmd, t, w, h); }, g.lights(),
                        again),
                  "кадр игры после загрузки");
            g.set_hero_light(true);
            u32 same = 0;
            for (u32 y = 0; y < kPx; ++y)
                for (u32 x = 0; x < kPx; ++x) same += at(again.full, -20, v + 1, x, y) == at(st->game.full, -20, v + 1, x, y);
            check(same == kPx * kPx, "«Скала» в кадре та же, что до сохранения: " + std::to_string(same) + " из 1024");
            return true;
        }});
        steps_.push_back({"без сердец герой очнулся у точки появления", 20, [&g, v, this](u32 f) {
            if (f == 0) {
                g.teleport(-20.5, v - static_cast<f64>(kHeroHalfH));
                g.hurt(100);
                return false;
            }
            if (f < 10) return false;
            check(g.hero_x() == 0.5 && std::fabs(g.hero_y() - (v - static_cast<f64>(kHeroHalfH))) < 0.01, "герой у точки появления: " +
                                                                                         std::to_string(g.hero_x()) + ", " + std::to_string(g.hero_y()));
            check(g.hearts() > 0, "сердца вернулись");
            return true;
        }});
        steps_.push_back({"обратно в меню", 5, [&s, &g, this, st](u32 f) {
            if (f == 0) {
                s.to_main_menu();
                g.set_level({});
                std::error_code ec;
                std::filesystem::remove_all(st->dir.parent_path(), ec);
            }
            return f >= 2;
        }});
    }

    // games/examples/tiled (step 10): the level the editor's «Импорт из Tiled…» made of a map saved by Tiled 1.8.2,
    // with the «Картинка» templates, their pictures and the zone's music it wrote into the game, played with nothing
    // of Tiled. main() puts a copy of the game's data with what the import wrote, and of the example's level, in
    // tiled_root; tiled_update first imports the map again there, two pictures it depends on changed (the chest's
    // and the decorations' tileset), as an author does after changing them in Tiled. The editor's view and the game
    // are compared with the frame tmxrasterizer drew of the same map (tools/tiled_example/rasterize.py).
    void build_tiled(Shell& s, bool updated) {
        SliceGame& g = g_;
        struct State {
            std::filesystem::path level;
            forge::level::LevelAreas areas;
            u64 cave = 0;
            u32 link = 0, starts = 0, walked = 0;
            std::vector<world::TileId> cells; // the map's rectangle in the game, all layers
            assets::CookedTexture ref;        // the frame tmxrasterizer drew
            render::Camera2D cam;
            Shot editor, game;
            f64 x = 0, y = 0; // the hero when saved
        };
        auto st = std::make_shared<State>();
        // tmxrasterizer's frame: the map's chunks, cells -16…48 × -16…32, a pixel of the frame a pixel of a tile.
        constexpr u32 kW = 1024, kH = 768, kPx = 16;
        constexpr i32 kX0 = -16, kY0 = -16, kX1 = 48, kY1 = 32;
        // The map's pictures of tiles: the templates the import made of them, where their middles are.
        struct Thing {
            const char* id;
            f64 x, y;
        };
        static const Thing kThings[] = {{"tiled_уровень_предметы_0", 34.5, 13.5},  // «Сундук»
                                        {"tiled_уровень_предметы_0", 11, 13},      // «Большой сундук», stretched to 32 × 32
                                        {"tiled_уровень_предметы_3", 18.5, 13.5},  // «Табличка»
                                        {"tiled_уровень_предметы_3h", 23.5, 13.5}, // «Табличка назад», mirrored
                                        {"tiled_уровень_предметы_7", 21.5, 13},    // a «Фонарь» of the template .tx
                                        {"tiled_уровень_предметы_7", -10.5, 13}};  // «Фонарь у обрыва», at negative x
        static const char* const kTemplates[] = {"tiled_уровень_предметы_0", "tiled_уровень_предметы_3", "tiled_уровень_предметы_3h",
                                                 "tiled_уровень_предметы_7"};
        auto things = [&g](std::string& wrong) {
            u32 found = 0;
            for (const Thing& t : kThings) {
                bool here = false;
                for (flecs::entity_t e : g.copies_of(t.id)) {
                    f64 x = 0, y = 0;
                    here = here || (g.position_of(e, x, y) && std::fabs(x - t.x) < 1e-6 && std::fabs(y - t.y) < 1e-6);
                }
                found += here;
                if (!here && wrong.empty()) wrong = std::string(t.id) + " нет в " + std::to_string(t.x) + ", " + std::to_string(t.y);
            }
            usize all = 0;
            for (const char* id : kTemplates) all += g.copies_of(id).size();
            if (all != std::size(kThings) && wrong.empty()) wrong = "копий шаблонов " + std::to_string(all);
            return found == std::size(kThings) && all == std::size(kThings);
        };
        // The map's rectangle in the game: every layer, row by row.
        auto game_cells = [&g] {
            std::vector<world::TileId> out;
            for (u32 layer : {kWalls, kBlocks, kLiquids})
                for (i32 y = kY0; y < kY1; ++y)
                    for (i32 x = kX0; x < kX1; ++x) out.push_back(g.tile(layer, x, y));
            return out;
        };
        auto in = [&g](u64 id) {
            const std::vector<u64> now = g.areas_inside();
            return std::find(now.begin(), now.end(), id) != now.end();
        };
        auto put = [&g](f64 feet_x, f64 feet_y) { g.teleport(feet_x, feet_y - kHeroHalfH); };
        auto var = [&s](const char* name) { return s.vars().get(name).number(); };
        auto hud = [&s] {
            Rml::Element* t = s.find_element("tracker");
            return t && t->IsVisible(true) ? std::string(t->GetInnerRML()) : std::string();
        };
        auto heard = [&s, &g] {
            const f32 p = music_peak(g.sounds().mixer());
            s.apply_settings(s.settings());
            return p;
        };
        auto rgb = [](const std::vector<u8>& img, usize i) {
            return std::to_string(img[i]) + "," + std::to_string(img[i + 1]) + "," + std::to_string(img[i + 2]);
        };
        // A frame kept next to the run's copies, to be looked at beside tmxrasterizer's.
        auto keep = [this](const char* name, const std::vector<u8>& rgba) {
            assets::CookedTexture img;
            img.width = kW;
            img.height = kH;
            img.rgba8 = rgba;
            for (usize i = 3; i < img.rgba8.size(); i += 4) img.rgba8[i] = 255;
            std::vector<u8> png;
            const std::filesystem::path file = tiled_root / name;
            if (assets::encode_image(img, ".png", png) && write_file_atomic(file, png)) FORGE_INFO("кадр записан: %s", path_to_utf8(file).c_str());
        };
        // The editor's view of the level folder at tmxrasterizer's frame (no light: daylight, as the view shows it).
        auto editor_shot = [&s, &g, this, st](Shot& out) {
            SliceLevel module;
            std::string why;
            check(module.load_objects(s.game_dir(), &why), "шаблоны игры для вида редактора " + why);
            check(module.init_view(g.device(), SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM), "вид редактора");
            forge::level::Level level(module);
            check(level.open(st->level, &why), "редактор открывает уровень " + why);
            const render::Camera2D cam = st->cam;
            level.ensure_loaded(cam.visible_tiles(kW, kH).expanded(render::kLightMargin));
            return shoot(g.device(), kW, kH,
                         [&](SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* t) {
                             module.prepare_view(cmd, level, cam, kW, kH, forge::level::ViewOptions{}, 0);
                             SDL_GPUColorTargetInfo info{};
                             info.texture = t;
                             const Color bg = module.background();
                             info.clear_color = SDL_FColor{bg.r, bg.g, bg.b, 1};
                             info.load_op = SDL_GPU_LOADOP_CLEAR;
                             info.store_op = SDL_GPU_STOREOP_STORE;
                             SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &info, 1, nullptr);
                             module.draw_view(cmd, pass);
                             SDL_EndGPURenderPass(pass);
                         },
                         module.lights(), out);
        };

        steps_.push_back({"пример «Tiled»: новая игра с уровня, у игры ни одного файла Tiled", 40,
                          [&s, &g, updated, var, this, st](u32 f) {
            if (f < 5) return false;
            if (f == 5) {
                check(tiled_error.empty(), "данные игры и уровень примера готовы: " + tiled_error);
                st->level = tiled_root / "level";
                std::error_code ec;
                const bool packaged = std::filesystem::is_directory(exe_dir() / "data" / "game" / "objects", ec);
                FORGE_INFO("пример «Tiled» %s (%s); игра читает данные и уровень из %s", path_to_utf8(tiled_example).c_str(),
                           packaged ? "пакет" : "сборка", path_to_utf8(tiled_root).c_str());
                check(std::filesystem::is_regular_file(tiled_example / "level" / "tiled.json", ec), "пример на месте");
                const bool map = std::filesystem::exists(tiled_example / utf8_path("Карта Tiled"), ec);
                check(packaged ? !map : map, packaged ? "в пакете папки «Карта Tiled» нет" : "в исходниках «Карта Tiled» лежит рядом с примером");
                u32 tiled_files = 0, files = 0;
                for (const auto& e : std::filesystem::recursive_directory_iterator(tiled_root, ec)) {
                    if (!e.is_regular_file()) continue;
                    ++files;
                    const std::filesystem::path ext = e.path().extension();
                    tiled_files += ext == ".tmx" || ext == ".tsx" || ext == ".tx";
                }
                check(files > 0 && tiled_files == 0, "у игры ни одного файла Tiled: из " + std::to_string(files) + " файлов " +
                                                         std::to_string(tiled_files));
                if (updated) {
                    // The import again changed the chest's picture and the own tiles' picture, nothing else of the level.
                    objects::Library shipped, now;
                    std::string why;
                    check(shipped.load(s.game_dir() / "kinds.json", tiled_example / "objects", &why), "шаблоны примера " + why);
                    check(load_objects(now, s.game_dir(), &why), "шаблоны игры " + why);
                    const objects::Template* chest = now.find("tiled_уровень_предметы_0");
                    const objects::Template* was = shipped.find("tiled_уровень_предметы_0");
                    const std::filesystem::path pictures = s.game_dir() / "pictures";
                    check(chest && was && chest->picture != was->picture && std::filesystem::is_regular_file(pictures / utf8_path(chest->picture), ec) &&
                              !std::filesystem::exists(pictures / utf8_path(was->picture), ec),
                          "у «Сундука» новая картинка, прежней у игры нет: " + (chest ? chest->picture : std::string("шаблона нет")));
                    for (const char* id : {"tiled_уровень_предметы_3", "tiled_уровень_предметы_3h", "tiled_уровень_предметы_7"}) {
                        const objects::Template* a = now.find(id);
                        const objects::Template* b = shipped.find(id);
                        check(a && b && a->picture == b->picture, std::string("у ") + id + " картинка та же");
                    }
                    u32 same = 0, changed = 0, gone = 0;
                    std::string what;
                    for (const auto& e : std::filesystem::directory_iterator(tiled_example / "level", ec)) {
                        std::vector<u8> before, after;
                        read_file(e.path(), before);
                        const bool read = read_file(st->level / e.path().filename(), after);
                        // The .json as text, line ends aside (a Windows checkout has CRLF, the import writes LF); the
                        // rest byte for byte.
                        if (e.path().extension() == ".json") {
                            std::erase(before, '\r');
                            std::erase(after, '\r');
                        }
                        if (!read) ++gone;
                        else if (before == after) ++same;
                        else {
                            ++changed;
                            what += " " + path_to_utf8(e.path().filename());
                        }
                    }
                    check(gone == 0 && changed == 1 && what == " tiles.png",
                          "в папке уровня изменилась одна картинка своих тайлов:" + what + "; тех же файлов " + std::to_string(same));
                }
                std::string why;
                check(forge::level::load_areas(st->level, st->areas, nullptr, &why) && st->areas.areas.size() == 1, "в уровне одна зона " + why);
                const forge::level::Area* cave = st->areas.areas.empty() ? nullptr : &st->areas.areas[0];
                check(cave && cave->name == "Пещера" && cave->x0 == 25 && cave->y0 == 9 && cave->x1 == 37 && cave->y1 == 14 &&
                          cave->music == "пещера.wav",
                      "зона «Пещера» из прямоугольника карты: x 25…37, y 9…14, музыка пещера.wav");
                check(st->areas.spawn && st->areas.spawn_x == 3 && st->areas.spawn_y == 14, "точка появления из точки «spawn» карты: 3, 14");
                check(std::filesystem::is_regular_file(s.game_dir() / "sounds" / utf8_path("пещера.wav"), ec), "музыка зоны в звуках игры");
                if (cave) st->cave = cave->id;
                // The example's links (the game's, and «Герой входит в Пещеру» once: quest.copper = 1) as the game's.
                logic::Logic links;
                check(links.load(tiled_example / "logic.json", &why), "связи примера читаются " + why);
                for (const logic::Link& l : links.links)
                    if (l.a == "hero" && l.verb == "enter" && l.b == area_thing_id(st->cave) && l.once) st->link = l.id;
                check(st->link != 0, "в «Логике» примера связь «Герой входит в Пещеру», один раз");
                const std::filesystem::path file = std::filesystem::temp_directory_path() / "forge_slice_test_logic.json";
                std::filesystem::copy_file(tiled_example / "logic.json", file, std::filesystem::copy_options::overwrite_existing, ec);
                check(!ec && g.reload_links(&why), "связи примера — связи игры " + why);
                g.set_level(st->level);
                check(s.new_game(), "новая игра с уровня примера");
                return false;
            }
            if (f < 20) return false;
            // The game stands the hero in the middle of the cell the point is in (Tiled's point is on the cells' corner).
            check(g.hero_x() == std::floor(st->areas.spawn_x) + 0.5 &&
                      g.hero_y() == scene::Position::at_tile(0, st->areas.spawn_y - kHeroHalfH).tile_y() && g.on_ground(),
                  "герой стоит на земле у точки появления: " + std::to_string(g.hero_x()) + ", " + std::to_string(g.hero_y()));
            check(g.areas() && *g.areas() == st->areas, "в игре зона и точка появления из файла");
            check(g.areas_inside().empty() && var("quest.copper") == 0, "герой ни в одной зоне, задание не взято");
            check(s.screens().place_music().empty() && g.sounds().screens().music_name().empty(), "музыки места нет");
            return true;
        }});
        steps_.push_back({"клетки как в папке уровня, шесть картинок на своих местах, вокруг пусто", 5,
                          [&s, &g, things, game_cells, this, st](u32 f) {
            if (f > 0) return true;
            SliceLevel module;
            std::string why;
            check(module.load_objects(s.game_dir(), &why), "шаблоны игры " + why);
            forge::level::Level level(module);
            check(level.open(st->level, &why), "уровень открывается " + why);
            const world::Rect map{kX0, kY0, kX1, kY1};
            const world::Rect around = map.expanded(16);
            level.ensure_loaded(around);
            u32 chunks = 0, loaded = 0;
            for (i32 cy = around.y0 >> world::kChunkShift; cy <= (around.y1 - 1) >> world::kChunkShift; ++cy)
                for (i32 cx = around.x0 >> world::kChunkShift; cx <= (around.x1 - 1) >> world::kChunkShift; ++cx) {
                    ++chunks;
                    loaded += g.world() && g.world()->find_chunk({cx, cy}) != nullptr;
                }
            check(loaded == chunks, "в игре загружены карта и участок вокруг: " + std::to_string(loaded) + " из " + std::to_string(chunks));
            st->cells = game_cells();
            u32 same = 0, filled = 0, i = 0, outside = 0, empty = 0;
            std::string first;
            for (u32 layer : {kWalls, kBlocks, kLiquids})
                for (i32 y = kY0; y < kY1; ++y)
                    for (i32 x = kX0; x < kX1; ++x, ++i) {
                        const world::TileId want = level.tile(layer, x, y), got = st->cells[i];
                        same += got == want;
                        filled += got != 0;
                        if (got != want && first.empty())
                            first = " слой " + std::to_string(layer) + " клетка " + std::to_string(x) + "," + std::to_string(y) + ": " +
                                    std::to_string(got) + " вместо " + std::to_string(want);
                    }
            check(same == st->cells.size() && filled > 0, "в игре клетки уровня: совпало " + std::to_string(same) + " из " +
                                                               std::to_string(st->cells.size()) + ", непустых " + std::to_string(filled) + first);
            for (u32 layer : {kWalls, kBlocks, kLiquids})
                for (i32 y = around.y0; y < around.y1; ++y)
                    for (i32 x = around.x0; x < around.x1; ++x)
                        if (!map.contains(x, y)) {
                            ++outside;
                            empty += g.tile(layer, x, y) == 0;
                        }
            check(empty == outside, "вокруг карты пусто: пустых " + std::to_string(empty) + " из " + std::to_string(outside));
            std::string wrong;
            check(things(wrong), "шесть «Картинок» на местах из карты " + wrong);
            u32 items = 0;
            for (u8 k = 0; k <= static_cast<u8>(ItemKind::Key); ++k) items += g.count_items(static_cast<ItemKind>(k));
            check(g.count_npcs() == 0 && items == 0 && g.entities() == 1 + std::size(kThings),
                  "кроме героя и шести картинок никого и ничего: сущностей " + std::to_string(g.entities()));
            return true;
        }});
        steps_.push_back({updated ? "вид редактора — кадр tmxrasterizer с изменёнными картинками" : "вид редактора — кадр tmxrasterizer пиксель в пиксель", 5,
                          [&g, updated, editor_shot, keep, rgb, this, st](u32 f) {
            if (f > 0) return true;
            const std::filesystem::path file = tiled_example / (updated ? "tmxrasterizer_changed.png" : "tmxrasterizer.png");
            std::vector<u8> bytes;
            std::string why;
            if (!read_file(file, bytes) || !assets::decode_image(bytes, st->ref, &why) || st->ref.width != kW || st->ref.height != kH) {
                check(false, "кадр tmxrasterizer 1024 × 768 читается: " + path_to_utf8(file) + " " + why);
                st->ref = {};
                return true;
            }
            st->cam = g.camera();
            st->cam.x = (kX0 + kX1) * 0.5;
            st->cam.y = (kY0 + kY1) * 0.5;
            st->cam.zoom = static_cast<f32>(kPx);
            check(editor_shot(st->editor), "кадр вида редактора");
            keep("editor.png", st->editor.full);
            const std::vector<u8>& img = st->editor.full;
            const std::vector<u8>& ref = st->ref.rgba8;
            if (img.size() != ref.size()) return true;
            const Color bg = SliceLevel{}.background();
            const int sky[3] = {static_cast<int>(bg.r * 255 + 0.5f), static_cast<int>(bg.g * 255 + 0.5f), static_cast<int>(bg.b * 255 + 0.5f)};
            // Where Tiled stacks a see-through tile on another (moss over the cave's wall), the level's own tile is the
            // two blended once, exactly; Qt blends premultiplied and rounds twice: one off at most there.
            u32 drawn = 0, same = 0, near = 0, clear = 0, open = 0;
            std::string first, extra;
            for (usize i = 0; i < ref.size(); i += 4) {
                if (ref[i + 3] == 255) {
                    ++drawn;
                    const bool eq = img[i] == ref[i] && img[i + 1] == ref[i + 1] && img[i + 2] == ref[i + 2];
                    bool one = true;
                    for (int c = 0; c < 3; ++c) one = one && std::abs(img[i + c] - ref[i + c]) <= 1;
                    same += eq;
                    near += one;
                    if (!one && first.empty())
                        first = "; пиксель " + std::to_string(i / 4 % kW) + "," + std::to_string(i / 4 / kW) + ": " + rgb(img, i) + " вместо " + rgb(ref, i);
                } else {
                    ++open;
                    bool eq = true;
                    for (int c = 0; c < 3; ++c) eq = eq && std::abs(img[i + c] - sky[c]) <= 1;
                    clear += eq;
                    if (!eq && extra.empty())
                        extra = "; пиксель " + std::to_string(i / 4 % kW) + "," + std::to_string(i / 4 / kW) + ": " + rgb(img, i);
                }
            }
            FORGE_INFO("вид редактора и кадр tmxrasterizer: нарисовано %u, совпало точно %u, с разницей не больше 1 — %u; пусто %u, из них небо %u",
                       drawn, same, near, open, clear);
            check(drawn > 80000 && near == drawn && same * 100 >= drawn * 99,
                  "что нарисовал Tiled, вид редактора рисует так же: точно " + std::to_string(same) + ", с разницей до 1 — " + std::to_string(near) +
                      " из " + std::to_string(drawn) + " пикселей" + first);
            check(clear == open, "где Tiled не рисовал ничего, в редакторе небо: " + std::to_string(clear) + " из " + std::to_string(open) + extra);
            return true;
        }});
        steps_.push_back({"в игре тот же кадр под светом неба", 5, [&g, keep, rgb, this, st](u32 f) {
            if (f > 0) return true;
            const std::vector<u8>& ref = st->ref.rgba8;
            if (ref.empty() || st->editor.full.size() != ref.size()) return true;
            g.set_hero_light(false);
            const render::Camera2D kept = g.camera();
            g.camera() = st->cam;
            check(shoot(g.device(), kW, kH, [&g](SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* t) { g.render(cmd, t, kW, kH); }, g.lights(),
                        st->game),
                  "кадр игры");
            g.camera() = kept;
            g.set_hero_light(true);
            keep("game.png", st->game.full);
            const std::vector<u8>& img = st->game.full;
            const std::vector<u8>& light = st->game.light;
            const std::vector<u8>& view = st->editor.full;
            if (img.size() != ref.size() || light.size() != ref.size()) return true;
            // The hero stands in the frame: its box is left out.
            const f64 hx = (g.hero_x() - st->cam.x) * kPx + kW * 0.5, hy = (g.hero_y() - st->cam.y) * kPx + kH * 0.5;
            // Under the sky (light 240 and more) the frame is nearly the view itself: a picture times a light of one.
            u32 drawn = 0, same = 0, lit = 0;
            std::string first;
            for (usize i = 0; i < ref.size(); i += 4) {
                const f64 x = static_cast<f64>(i / 4 % kW) + 0.5, y = static_cast<f64>(i / 4 / kW) + 0.5;
                if (ref[i + 3] != 255 || (std::fabs(x - hx) < 24 && y > hy - 40 && y < hy + 24)) continue;
                ++drawn;
                bool eq = true, bright = true;
                for (int c = 0; c < 3; ++c) {
                    eq = eq && std::abs(img[i + c] - view[i + c] * light[i + c] / 255) <= 2;
                    bright = bright && light[i + c] >= 240;
                }
                same += eq;
                lit += bright;
                if (!eq && first.empty())
                    first = "; пиксель " + std::to_string(i / 4 % kW) + "," + std::to_string(i / 4 / kW) + ": " + rgb(img, i) + " при виде " +
                            rgb(view, i) + " и свете " + rgb(light, i);
            }
            FORGE_INFO("кадр игры: нарисовано Tiled %u, как вид редактора под светом %u, из них под небом (свет от 240) %u", drawn, same, lit);
            check(drawn > 80000 && same == drawn, "в игре нарисованное Tiled — вид редактора под светом: " + std::to_string(same) + " из " +
                                                      std::to_string(drawn) + first);
            check(lit * 5 > drawn, "под небом светло: пикселей со светом от 240 — " + std::to_string(lit) + " из " + std::to_string(drawn));
            return true;
        }});
        steps_.push_back({"пешком в «Пещеру»: название места, задание в HUD, музыка зоны", 300,
                          [&s, &g, in, put, var, hud, heard, this, st](u32 f) {
            audio::ScreenSounds& snd = g.sounds().screens();
            if (f == 0) {
                put(22.5, 14);
                st->walked = 0;
                return false;
            }
            if (f < 10) return false;
            if (f == 10) {
                check(!in(st->cave) && g.area_enters(st->cave) == 0, "у таблички перед пещерой герой ещё не в ней");
                st->starts = snd.music_starts();
            }
            if (!st->walked) {
                if (g.hero_x() < 26.5 && f < 250) {
                    Controls c;
                    c.right = true;
                    g.script(c);
                    return false;
                }
                g.stop_script();
                st->walked = f;
            }
            if (f < st->walked + 10) return false;
            check(in(st->cave) && g.area_enters(st->cave) == 1, "герой вошёл в «Пещеру» один раз: x " + std::to_string(g.hero_x()));
            check(g.last_hint() == "Пещера", "игра показала название места: " + g.last_hint());
            check(var("quest.copper") == 1, "связь «Логики» задала quest.copper = 1");
            check(hud().find("Медь для кузнеца") != std::string::npos, "в HUD задание «Медь для кузнеца»: " + hud());
            check(s.screens().place_music() == "пещера.wav" && snd.music_name() == "пещера.wav" && snd.music_playing() &&
                      snd.music_starts() == st->starts + 1,
                  "играет музыка зоны пещера.wav, начата один раз: " + snd.music_name());
            check(heard() > 0.05f, "музыку места слышно");
            return true;
        }});
        steps_.push_back({"сохранение и загрузка: клетки, картинки, зона и задание те же", 60,
                          [&s, &g, in, var, things, game_cells, this, st](u32 f) {
            if (f == 0) {
                st->x = g.hero_x();
                st->y = g.hero_y();
                check(s.save("tiled", "Tiled"), "игра сохраняется в «Пещере»");
                std::error_code ec;
                for (const char* file : {"areas.json", "tiles.json", "tiles.png", "world.json"})
                    check(std::filesystem::is_regular_file(s.slots().folder("tiled") / "world" / file, ec), std::string("в сохранении ") + file);
                check(s.load("tiled"), "сохранение загружается");
                return false;
            }
            if (f < 20) return false;
            check(std::fabs(g.hero_x() - st->x) < 0.01 && std::fabs(g.hero_y() - st->y) < 0.01, "герой где был: " + std::to_string(g.hero_x()));
            check(in(st->cave) && g.area_enters(st->cave) == 0, "в «Пещере», загрузка — не вход");
            check(var("quest.copper") == 1, "задание взято");
            check(g.sounds().screens().music_name() == "пещера.wav" && g.sounds().screens().music_playing(), "музыка зоны играет");
            check(game_cells() == st->cells, "клетки карты те же");
            std::string wrong;
            check(things(wrong), "шесть «Картинок» на местах " + wrong);
            return true;
        }});
        steps_.push_back({"обратно в меню", 5, [&s, &g](u32 f) {
            if (f == 0) {
                s.to_main_menu();
                g.set_level({});
            }
            return f >= 2;
        }});
    }

    SliceGame& g_;
    std::string scene_;
    std::vector<Step> steps_;
    std::deque<std::string> names_; // of the steps named as they are made
    usize index_ = 0;
    u32 frames_ = 0;
    int* failures_ = nullptr;
};

} // namespace

// --scene tiled and tiled_update (SelfTest::build_tiled): this run's copy of the game's data with what the import of
// games/examples/tiled wrote into the game (its objects, pictures and sounds) and of the example's level, in root;
// nothing of Tiled.
static bool tiled_copy(const std::filesystem::path& game, const std::filesystem::path& example, const std::filesystem::path& root,
                       std::string& why) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    if (!ec) fs::copy(game, root / "data", fs::copy_options::recursive, ec);
    for (const char* dir : {"objects", "pictures", "sounds"})
        if (!ec) fs::copy(example / dir, root / "data" / dir, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (!ec) fs::copy(example / "level", root / "level", fs::copy_options::recursive, ec);
    if (ec) why = "копия данных игры и уровня примера в " + path_to_utf8(root) + ": " + ec.message();
    return !ec;
}

// --scene tiled_update: the author changes two pictures the map depends on (the chest's, the picture of a tile
// object, and the decorations' tileset, the picture of an external .tsx; their colours turned over, as
// tools/tiled_example/rasterize.py turns them for tmxrasterizer_changed.png) and imports the map again as the
// editor's «Импорт из Tiled…» does (plan, the game's files, one step of the history, Ctrl+S), into root's copies
// before the game reads them. The map's copy goes after: the game needs nothing of Tiled.
static bool tiled_changed(const std::filesystem::path& example, const std::filesystem::path& root, std::string& why) {
    namespace fs = std::filesystem;
    namespace tl = forge::level::tiled;
    const fs::path source = example / utf8_path("Карта Tiled"), map = root / "map";
    std::error_code ec;
    if (!fs::is_directory(source, ec)) {
        why = "рядом с примером нет папки «Карта Tiled» (в пакете её нет: tiled_update идёт из сборки)";
        return false;
    }
    fs::copy(source, map, fs::copy_options::recursive, ec);
    if (ec) {
        why = "копия карты: " + ec.message();
        return false;
    }
    for (const char* name : {"картинки/сундук.png", "наборы/декор.png"}) {
        const fs::path file = map / utf8_path(name);
        std::vector<u8> bytes, png;
        assets::CookedTexture img;
        if (!read_file(file, bytes) || !assets::decode_image(bytes, img, &why)) {
            why = std::string(name) + " не читается " + why;
            return false;
        }
        for (usize i = 0; i + 3 < img.rgba8.size(); i += 4)
            for (usize c = 0; c < 3; ++c) img.rgba8[i + c] = static_cast<u8>(255 - img.rgba8[i + c]);
        if (!assets::encode_image(img, ".png", png) || !write_file_atomic(file, png)) {
            why = std::string(name) + " не записан";
            return false;
        }
    }
    SliceLevel module;
    if (!module.load_objects(root / "data", &why)) return false;
    objects::Library& lib = *module.library();
    forge::level::Level level(module);
    tl::Map m;
    tl::Plan p;
    tl::Options o;
    o.layer_count = static_cast<u32>(module.layer_names().size());
    o.liquids = module.liquids_layer();
    o.template_taken = [&lib](const tl::Picture& pic) { return tl::template_taken(lib, pic); };
    tl::Resources made;
    editor::Document doc;
    editor::UndoStack history(doc);
    if (!level.open(root / "level", &why) || !tl::read_map(map / utf8_path("уровень.tmx"), m, &why) ||
        !tl::plan(m, o, level.own_tiles(), level.areas(), level.tiled_record(), p, &why))
        return false;
    level.ensure_loaded({p.x0, p.y0, p.x1, p.y1});
    if (!tl::write_resources(p, lib, lib.sounds_folder(), made, &why) || !tl::apply(level, history, lib, p, tl::ApplyOptions{}, &why))
        return false;
    const forge::level::Level::SaveReport r = level.save();
    if (!r.ok) {
        why = "уровень не записан: " + r.error;
        return false;
    }
    FORGE_INFO("карта Tiled импортирована снова: своих тайлов обновлено %u, новых %u; шаблонов «Картинка» записано %zu; шаг истории %s",
               p.tiles_updated, p.tiles_new, made.templates.size(), history.undo_label().c_str());
    fs::remove_all(map, ec);
    return true;
}

// --scene levels: the game of two levels in trip::root(), made anew from the game's data (from): levels.json with
// «Деревня» (game/level, the start), «Пещера» (game/levels/cave) and «Файл», whose folder is a file; two templates of
// pictures, «Часы» and «Родник»; the links between the levels and around them; two windows, the areas' music and the
// objects' sounds. The areas are the scene's (on the village's floor, where the game's world has it).
static bool make_travel_game(const std::filesystem::path& from, std::string& why) {
    namespace fs = std::filesystem;
    const fs::path root = trip::root(), game = root / "data";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "user", ec);
    fs::copy(from, game, fs::copy_options::recursive, ec);
    if (ec) {
        why = "не скопированы данные игры из " + path_to_utf8(from) + ": " + ec.message();
        return false;
    }
    auto text = [&](const fs::path& file, std::string_view t) {
        fs::create_directories(file.parent_path(), ec);
        if (write_file_atomic(file, {reinterpret_cast<const u8*>(t.data()), t.size()})) return true;
        why = "не записан " + path_to_utf8(file);
        return false;
    };
    forge::level::LevelList list;
    list.levels = {{"level", "Деревня"}, {"cave", "Пещера"}, {"file", "Файл"}};
    list.start = "level";
    if (!forge::level::write_levels(game, list, &why) || !text(game / "levels" / "file", "не папка уровня"))
        return false;
    if (!text(game / "objects" / utf8_path("Часы.object.json"),
              R"({"id": "ticker", "name": "Часы", "kind": "picture", "about": "каждую секунду прибавляет test.ticks"})") ||
        !text(game / "objects" / utf8_path("Родник.object.json"),
              R"({"id": "spring", "name": "Родник", "kind": "picture", "about": "лечит героя, один раз"})"))
        return false;
    // The links: the game's, and these.
    logic::Logic links;
    if (!links.load(game / "logic.json", &why)) return false;
    auto link = [&](u32 id, const char* a, const char* verb, const std::string& b) -> logic::Link& {
        logic::Link l;
        l.id = id;
        l.a = a;
        l.verb = verb;
        l.b = b;
        links.links.push_back(l);
        return links.links.back();
    };
    link(trip::kYardLink, "hero", "enter", area_thing_id(trip::kYard)).code = "forge.ui.show(\"окно_уровня\")";
    link(trip::kMarkLink, "hero", "enter", area_thing_id(trip::kMark)).once = true;
    {
        logic::Link& l = link(trip::kGoLink, "hero", "go", area_thing_id(trip::kExit));
        l.level = "cave";
        l.arrive = area_thing_id(trip::kEntry);
    }
    {
        // Once: a going not needed (another link of the step went) leaves no mark.
        logic::Link& l = link(trip::kGoTooLink, "hero", "go", area_thing_id(trip::kExit));
        l.level = "cave";
        l.once = true;
    }
    {
        // Once: a going refused leaves no mark either; one done does.
        logic::Link& l = link(trip::kGateLink, "hero", "go", area_thing_id(trip::kGate));
        l.level = "cave";
        l.once = true;
    }
    {
        // Once, as a scheme that waits between its «Только один раз» and «Перейти на уровень»: a going refused after
        // the wait leaves no mark either.
        logic::Link& l = link(trip::kPathLink, "hero", "go", area_thing_id(trip::kPath));
        l.level = "cave";
        l.once = true;
        script::Graph g;
        g.name = "link " + std::to_string(trip::kPathLink);
        const u32 when = g.add("logic.when", 0, 0).uid;
        const u32 once = g.add("logic.once", 280, 0).uid;
        script::GraphNode& wait = g.add("api.wait", 560, 0);
        wait.set_value("seconds", "0.3");
        const u32 pause = wait.uid;
        script::GraphNode& go = g.add("logic.go", 840, 0);
        go.set_value("level", "cave");
        const u32 going = go.uid;
        g.link(when, script::kFlowNext, once);
        g.link(once, "first", pause);
        g.link(pause, script::kFlowNext, going);
        g.link(when, "a", going, "who");
        l.graph = g.to_json();
    }
    link(trip::kLateLink, "hero", "go", area_thing_id(trip::kExit)).code =
        "forge.wait(0.3)\nforge.game.add_var(\"test.after\", 1)\nlogic.go(target, \"level\", \"\", " + std::to_string(trip::kLateLink) + ")";
    link(trip::kBackLink, "hero", "go", area_thing_id(trip::kEntry)).level = "level";
    link(trip::kGrottoLink, "hero", "enter", area_thing_id(trip::kGrotto));
    link(trip::kGrottoWait, "hero", "enter", area_thing_id(trip::kGrotto)).code = "forge.wait(0.3)\nforge.game.add_var(\"test.grot\", 1)";
    link(trip::kTickerLink, "ticker", "follow", "hero").code = "while true do\n  forge.wait(1)\n  forge.game.add_var(\"test.ticks\", 1)\nend";
    link(trip::kSpringLink, "spring", "heal", "hero").once = true;
    script::Graph frames;
    const u32 tick = frames.add("std.event.tick").uid;
    script::GraphNode& add = frames.add("api.game.add_var", 240, 0);
    add.set_value("name", "test.frames");
    add.set_value("amount", "1");
    frames.link(tick, script::kFlowNext, add.uid);
    links.schemes.push_back({trip::kTickerScheme, "ticker", frames.to_json()});
    if (!links.save(game / "logic.json", &why)) return false;
    // Two windows that do not stop the world: one a link of «Двор» opens, one the player opens.
    auto page = [](const char* id, const char* says) {
        return std::string("<html><head><style>body, #") + id + " { pointer-events: none; } #" + id +
               " { position: relative; width: 100%; height: 100%; }</style></head><body><div id=\"" + id +
               "\" forge-screen=\"command\" forge-size=\"1920 1080\"><div>" + says + "</div></div></body></html>";
    };
    if (!text(game / "ui" / utf8_path("окно_уровня.html"), page("lv-level", "Окно уровня")) ||
        !text(game / "ui" / utf8_path("окно_игрока.html"), page("lv-player", "Окно игрока")))
        return false;
    // Sounds: the areas' music, a stream near «Часы», the steps of «Зверёк».
    auto tune = [](std::initializer_list<f32> notes, f32 step) {
        std::vector<audio::Tone> tones;
        f32 at = 0;
        for (f32 hz : notes) {
            tones.push_back({audio::Wave::Triangle, hz, hz, step * 0.95f, 0.01f, 3, 0.35f, at});
            at += step;
        }
        return audio::synth(tones);
    };
    const audio::Tone step[] = {{audio::Wave::Square, 300, 200, 0.05f, 0.001f, 40, 0.3f}};
    const std::pair<const char*, audio::ClipPtr> sounds[] = {{"двор.wav", tune({262, 330, 392}, 0.3f)},
                                                             {"пещера.wav", tune({196, 233, 175}, 0.3f)},
                                                             {"ручей.wav", tune({880, 988}, 0.2f)},
                                                             {"шаг.wav", audio::synth(step)}};
    fs::create_directories(game / "sounds", ec);
    for (const auto& [name, clip] : sounds) {
        std::vector<u8> wav;
        if (!clip || !audio::encode_wav(*clip, wav) || !write_file_atomic(game / "sounds" / utf8_path(name), wav)) {
            why = std::string("не записан звук ") + name;
            return false;
        }
    }
    return true;
}

// --scene platformer: the platformer's game in plat::root(), made anew from the game's data (from): levels.json with
// «Луг» (game/level, the start) and «Холм» (game/levels/hill), written by the scene's first step; the templates of
// the platformer (kinds «Враг», «Подбирается», «Ловушка»); the links «уходит через», «падает в», «доходит до»; the
// author's HUD and windows «при победе» and «при поражении»; the sounds of «Жук с голосом» and «Бродяга».
static bool make_platformer_game(const std::filesystem::path& from, std::string& why) {
    namespace fs = std::filesystem;
    const fs::path root = plat::root(), game = root / "data";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "user", ec);
    fs::copy(from, game, fs::copy_options::recursive, ec);
    if (ec) {
        why = "не скопированы данные игры из " + path_to_utf8(from) + ": " + ec.message();
        return false;
    }
    auto text = [&](const fs::path& file, std::string_view t) {
        fs::create_directories(file.parent_path(), ec);
        if (write_file_atomic(file, {reinterpret_cast<const u8*>(t.data()), t.size()})) return true;
        why = "не записан " + path_to_utf8(file);
        return false;
    };
    forge::level::LevelList list;
    list.levels = {{"level", "Луг"}, {"hill", "Холм"}};
    list.start = "level";
    if (!forge::level::write_levels(game, list, &why)) return false;
    const std::pair<const char*, const char*> templates[] = {
        {"Жук.object.json",
         R"({"id": "beetle", "name": "Жук", "kind": "enemy", "about": "стоит; прыжок сверху побеждает его", "values": {"scheme": "stand"}})"},
        {"Ёж.object.json",
         R"({"id": "hedgehog", "name": "Ёж", "kind": "enemy", "about": "колючий: сверху его не победить", "values": {"scheme": "stand", "stomp": false}})"},
        {"Жук с голосом.object.json",
         R"({"id": "loud_beetle", "name": "Жук с голосом", "kind": "enemy", "about": "его «Удар» — свой звук",
             "blocks": ["body", "control", "enemy", "sound"], "values": {"scheme": "stand", "sound_hit": "удар.wav"}})"},
        {"Бродяга.object.json",
         R"({"id": "wanderer", "name": "Бродяга", "kind": "enemy", "about": "бродит и стучит шагами",
             "blocks": ["body", "control", "enemy", "sound"], "values": {"scheme": "wander", "speed": 1.5, "sound_step": "шаг.wav"}})"},
        {"Монетка.object.json",
         R"({"id": "coin", "name": "Монетка", "kind": "pickup", "about": "одна монетка и 10 очков", "values": {"what": "coins", "count": 1, "score": 10}})"},
        {"Ряд шипов.object.json",
         R"({"id": "spikes_row", "name": "Ряд шипов", "kind": "trap", "about": "шипы в три клетки", "values": {"half_height": 0.5, "half_width": 1.5}})"},
        {"Самоцвет.object.json",
         R"({"id": "gem", "name": "Самоцвет", "kind": "pickup", "about": "его собирает связь: две монеты и 30 очков", "values": {"what": "coins", "count": 2, "score": 30}})"},
        {"Гудящий зверёк.object.json",
         R"({"id": "hum", "name": "Гудящий зверёк", "kind": "critter", "about": "стоит и гудит: звук «Рядом»",
             "blocks": ["body", "control", "sound"], "values": {"scheme": "stand", "sound_near": "шаг.wav"}})"}};
    for (const auto& [file, json] : templates)
        if (!text(game / "objects" / utf8_path(file), json)) return false;
    logic::Logic links;
    if (!links.load(game / "logic.json", &why)) return false;
    auto link = [&](u32 id, const char* verb, u64 area) -> logic::Link& {
        logic::Link l;
        l.id = id;
        l.a = "hero";
        l.verb = verb;
        l.b = area_thing_id(area);
        links.links.push_back(l);
        return links.links.back();
    };
    {
        logic::Link& l = link(plat::kTrailLink, "go", plat::kTrail);
        l.level = "hill";
        l.arrive = area_thing_id(plat::kEntry);
    }
    {
        logic::Link& l = link(plat::kEntryLink, "go", plat::kEntry);
        l.level = "level";
        l.arrive = area_thing_id(plat::kReturn);
    }
    link(plat::kPitLink, "fall", plat::kPit);
    link(plat::kFinishLink, "win", plat::kFinish);
    {
        logic::Link l; // «Герой собирает Самоцвет», with the verb's sound
        l.id = plat::kGemLink;
        l.a = "hero";
        l.verb = "collect";
        l.b = "gem";
        l.sound = true;
        links.links.push_back(l);
    }
    if (!links.save(game / "logic.json", &why)) return false;
    // The author's screens: the HUD over the game; the windows the game shows itself at its end, with «Ещё раз» (a new
    // game) and «В меню». The loss's window stops the world as a window may; the win's does not, the end does.
    const std::string hud =
        "<html><head><style>body, #pl-hud { pointer-events: none; }</style></head><body>"
        "<div id=\"pl-hud\" forge-screen=\"playing\" forge-size=\"1920 1080\">"
        "<div id=\"pl-score\" forge-text=\"Очки: {hero.score}\">?</div>"
        "<div id=\"pl-coins\" forge-text=\"Монеты: {inv.coins}\">?</div>"
        "<div id=\"pl-hearts\" forge-text=\"Сердца: {hero.hearts}\">?</div>"
        "</div></body></html>";
    auto ending = [](const char* id, const char* kind, bool pauses, const char* title) {
        const std::string button = "style=\"position: absolute; top: 500px; width: 200px; height: 60px; background: #333;";
        return std::string("<html><head><style>body, #") + id + " { pointer-events: none; } #" + id +
               " > div { pointer-events: auto; }</style></head><body><div id=\"" + id + "\" forge-screen=\"command\"" +
               (pauses ? " forge-pauses=\"1\"" : "") + " forge-ending=\"" + kind + "\" forge-size=\"1920 1080\" forge-fit=\"expand\"><div>" + title +
               "</div><div id=\"" + id + "-text\" forge-text=\"Итог: {game.result}\">?</div><div id=\"" + id + "-again\" " + button +
               " left: 700px;\" forge-click=\"[[&quot;new&quot;,&quot;&quot;]]\">Ещё раз</div><div id=\"" + id + "-menu\" " + button +
               " left: 1000px;\" forge-click=\"[[&quot;menu&quot;,&quot;&quot;]]\">В меню</div></div></body></html>";
    };
    if (!text(game / "ui" / utf8_path("платформер_hud.html"), hud) ||
        !text(game / "ui" / utf8_path("поражение.html"), ending("pl-lose", "lose", true, "Поражение")) ||
        !text(game / "ui" / utf8_path("победа.html"), ending("pl-win", "win", false, "Победа")))
        return false;
    const audio::Tone hit[] = {{audio::Wave::Square, 220, 110, 0.12f, 0.001f, 20, 0.4f}};
    const audio::Tone step[] = {{audio::Wave::Square, 300, 200, 0.05f, 0.001f, 40, 0.3f}};
    fs::create_directories(game / "sounds", ec);
    for (const auto& [name, clip] : {std::pair{"удар.wav", audio::synth(hit)}, std::pair{"шаг.wav", audio::synth(step)}}) {
        std::vector<u8> wav;
        if (!clip || !audio::encode_wav(*clip, wav) || !write_file_atomic(game / "sounds" / utf8_path(name), wav)) {
            why = std::string("не записан звук ") + name;
            return false;
        }
    }
    return true;
}

// The game, as the launcher starts it for a game of the module "slice" (slice_module.cpp).
namespace slice {
int run(int argc, char** argv);
}
int slice::run(int argc, char** argv) {
    Options options;
    std::string scene = "village";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--stress") == 0) options.stress = true;
        else if (std::strcmp(argv[i], "--play") == 0) options.edit_links = true;
        else if (std::strcmp(argv[i], "--test") == 0) {
            options.silent = true;
            // «Связи» over the game change a copy of the links, not the game's.
            options.edit_links = true;
            // The copy is of the links of the data the game plays: a package's own data/game (games/slice may not be
            // there), else games/slice. Never the copy an earlier run left: when this one fails the game has no links.
            options.links_file = std::filesystem::temp_directory_path() / "forge_slice_test_logic.json";
            const std::filesystem::path links = forge::game::game_data_dir(1, argv, utf8_path(SLICE_DATA_DIR)) / "logic.json";
            std::error_code ec0;
            std::filesystem::remove(options.links_file, ec0);
            if (!std::filesystem::copy_file(links, options.links_file, std::filesystem::copy_options::overwrite_existing, ec0))
                FORGE_ERROR("--test: связи «%s» не скопированы: %s", path_to_utf8(links).c_str(), ec0.message().c_str());
            // The links that happen go where the test reads them.
            options.fired_file = std::filesystem::temp_directory_path() / "forge_slice_test_fired.txt";
            std::error_code ec;
            std::filesystem::remove(options.fired_file, ec);
        }
        else if (std::strcmp(argv[i], "--scene") == 0 && i + 1 < argc) scene = argv[++i];
        else if (std::strcmp(argv[i], "--level") == 0 && i + 1 < argc) options.level_dir = utf8_path(argv[++i]);
        else if (std::strcmp(argv[i], "--fired") == 0 && i + 1 < argc) options.fired_file = utf8_path(argv[++i]);
        else if (std::strcmp(argv[i], "--at") == 0 && i + 1 < argc) {
            options.at = std::sscanf(argv[++i], "%lf,%lf", &options.at_x, &options.at_y) == 2;
        }
    }
    // --scene volumes: the game starts over the settings.json a player left, in this run's own folder: music and
    // sounds at 0, all at 100, as the game writes it.
    std::vector<char*> args(argv, argv + argc);
    std::string user, data;
    // --scene own_tiles: the game's data with a «Картинка» template (Табличка, its picture 16 × 24) in this run's
    // own folder; the game's own data stays as it is.
    if (scene == "own_tiles" && options.silent) {
        std::error_code ec;
        const std::filesystem::path packaged = exe_dir() / "data" / "game";
        const std::filesystem::path from =
            std::filesystem::is_directory(packaged / "objects", ec) ? packaged : utf8_path(SLICE_DATA_DIR);
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "forge_slice_own_tiles_data";
        std::filesystem::remove_all(dir, ec);
        std::filesystem::copy(from, dir, std::filesystem::copy_options::recursive, ec);
        std::filesystem::create_directories(dir / "pictures", ec);
        const std::string_view sign = R"({
  "id": "sign",
  "name": "Табличка",
  "kind": "picture",
  "values": {"half_height": 1},
  "picture": "табличка.png"
}
)";
        assets::CookedTexture img;
        img.width = 16;
        img.height = 24;
        for (u32 y = 0; y < img.height; ++y)
            for (u32 x = 0; x < img.width; ++x) {
                // A board on a post, air around the post.
                const bool board = y < 12, post = x >= 6 && x < 10;
                const u8 px[4] = {static_cast<u8>(board ? 150 : 90), static_cast<u8>(board ? 105 : 60), static_cast<u8>(board ? 60 : 30),
                                  static_cast<u8>(board || post ? 255 : 0)};
                img.rgba8.insert(img.rgba8.end(), px, px + 4);
            }
        std::vector<u8> png;
        if (ec || !assets::encode_image(img, ".png", png) || !write_file_atomic(dir / "pictures" / utf8_path("табличка.png"), png) ||
            !write_file_atomic(dir / "objects" / utf8_path("Табличка.object.json"), {reinterpret_cast<const u8*>(sign.data()), sign.size()}))
            FORGE_ERROR("не сделана копия данных игры с «Табличкой» в %s", path_to_utf8(dir).c_str());
        data = path_to_utf8(dir);
        args.push_back(const_cast<char*>("--data"));
        args.push_back(data.data());
    }
    // --scene tiled, tiled_update: the game's data and the level of games/examples/tiled as the import left them, in
    // this run's own folder (the game's own data and the example stay as they are).
    std::filesystem::path tiled_example, tiled_root;
    std::string tiled_error;
    if ((scene == "tiled" || scene == "tiled_update") && options.silent) {
        std::error_code ec;
        const std::filesystem::path packaged = exe_dir() / "data" / "game";
        const std::filesystem::path from =
            std::filesystem::is_directory(packaged / "objects", ec) ? packaged : utf8_path(SLICE_DATA_DIR);
        tiled_example = from.parent_path() / "examples" / "tiled";
        tiled_root = std::filesystem::temp_directory_path() / "forge_slice_tiled";
        if (tiled_copy(from, tiled_example, tiled_root, tiled_error) && scene == "tiled_update" &&
            !tiled_changed(tiled_example, tiled_root, tiled_error))
            tiled_error = "карта не импортирована снова: " + tiled_error;
        if (!tiled_error.empty()) FORGE_ERROR("%s", tiled_error.c_str());
        data = path_to_utf8(tiled_root / "data");
        args.push_back(const_cast<char*>("--data"));
        args.push_back(data.data());
    }
    // --scene project: a game the editor made (its data given with --data, as «Играть» gives it): the links are
    // that game's, not the engine's sources'.
    std::filesystem::path project_data, project_user;
    if (scene == "project" && options.silent) {
        for (int i = 1; i + 1 < argc; ++i) {
            if (std::strcmp(argv[i], "--data") == 0) project_data = utf8_path(argv[i + 1]);
            if (std::strcmp(argv[i], "--user") == 0) project_user = utf8_path(argv[i + 1]);
        }
        std::error_code ec;
        if (project_data.empty()) FORGE_ERROR("--scene project: нужна папка данных игры (--data)");
        else std::filesystem::copy_file(project_data / "logic.json", options.links_file,
                                        std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) FORGE_ERROR("--scene project: не скопирован %s", path_to_utf8(project_data / "logic.json").c_str());
    }
    ProjectEdits edits;
    std::string edits_error;
    bool has_edits = false;
    if (scene == "project" && options.silent)
        for (int i = 1; i + 1 < argc; ++i)
            if (std::strcmp(argv[i], "--edits") == 0) {
                has_edits = true;
                read_edits(utf8_path(argv[i + 1]), edits, edits_error);
            }
    // --scene levels, levels_continue (step 14.2b): the game of two levels in this run's own folder; levels makes it
    // anew, levels_continue plays on in it as levels left it (another process, the player's «Продолжить»).
    if ((scene == "levels" || scene == "levels_continue") && options.silent) {
        std::error_code ec;
        if (scene == "levels") {
            const std::filesystem::path packaged = exe_dir() / "data" / "game";
            const std::filesystem::path from =
                std::filesystem::is_directory(packaged / "objects", ec) ? packaged : utf8_path(SLICE_DATA_DIR);
            std::string why;
            if (!make_travel_game(from, why)) FORGE_ERROR("--scene levels: %s", why.c_str());
        }
        options.links_file.clear(); // the game's own logic.json, as a player's game has it
        data = path_to_utf8(trip::root() / "data");
        user = path_to_utf8(trip::root() / "user");
        args.push_back(const_cast<char*>("--data"));
        args.push_back(data.data());
        args.push_back(const_cast<char*>("--user"));
        args.push_back(user.data());
    }
    // --scene platformer, platformer_continue, platformer_edges (step 14.2c): the platformer's game in this run's own
    // folder, the same way.
    if ((scene == "platformer" || scene == "platformer_continue" || scene == "platformer_edges") && options.silent) {
        std::error_code ec;
        if (scene == "platformer") {
            const std::filesystem::path packaged = exe_dir() / "data" / "game";
            const std::filesystem::path from =
                std::filesystem::is_directory(packaged / "objects", ec) ? packaged : utf8_path(SLICE_DATA_DIR);
            std::string why;
            if (!make_platformer_game(from, why)) FORGE_ERROR("--scene platformer: %s", why.c_str());
        }
        options.links_file.clear();
        data = path_to_utf8(plat::root() / "data");
        user = path_to_utf8(plat::root() / "user");
        args.push_back(const_cast<char*>("--data"));
        args.push_back(data.data());
        args.push_back(const_cast<char*>("--user"));
        args.push_back(user.data());
    }
    // --scene platformer_template, platformer_template_continue (step 14.2d): a copy of the template «Платформер» in
    // this run's own folder, made anew by the first (from the package's data/examples/platformer, or games/platformer
    // beside the game's sources), played on in it by the second.
    if ((scene == "platformer_template" || scene == "platformer_template_continue") && options.silent) {
        std::error_code ec;
        if (scene == "platformer_template") {
            const std::filesystem::path packaged = exe_dir() / "data" / "examples" / "platformer";
            const std::filesystem::path from =
                std::filesystem::is_directory(packaged, ec) ? packaged : utf8_path(SLICE_DATA_DIR).parent_path() / "platformer";
            std::filesystem::remove_all(tpl::root(), ec);
            std::filesystem::create_directories(tpl::root(), ec);
            std::filesystem::copy(from, tpl::root() / "data", std::filesystem::copy_options::recursive, ec);
            if (ec) FORGE_ERROR("--scene platformer_template: не скопирован %s: %s", path_to_utf8(from).c_str(), ec.message().c_str());
        }
        options.links_file.clear();
        data = path_to_utf8(tpl::root() / "data");
        user = path_to_utf8(tpl::root() / "user");
        args.push_back(const_cast<char*>("--data"));
        args.push_back(data.data());
        args.push_back(const_cast<char*>("--user"));
        args.push_back(user.data());
    }
    if (scene == "volumes" && options.silent) {
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "forge_slice_volumes";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        const std::string_view text = R"({
  "$type": "forge::game::Settings",
  "fullscreen": false,
  "vsync": true,
  "ui_scale": 1.0,
  "master_volume": 1.0,
  "music_volume": 0.0,
  "sound_volume": 0.0,
  "theme": "",
  "show_fps": false
})";
        if (!write_file_atomic(dir / "settings.json", {reinterpret_cast<const u8*>(text.data()), text.size()}))
            FORGE_ERROR("не записался %s", path_to_utf8(dir / "settings.json").c_str());
        user = path_to_utf8(dir);
        args.push_back(const_cast<char*>("--user"));
        args.push_back(user.data());
    }
    args.push_back(nullptr);
    SliceGame game(options);
    SelfTest test(game, scene);
    test.tiled_example = tiled_example;
    test.tiled_root = tiled_root;
    test.tiled_error = tiled_error;
    test.project_data = project_data;
    test.project_user = project_user;
    test.project_at = options.at;
    test.project_at_x = options.at_x;
    test.project_at_y = options.at_y;
    test.project_has_edits = has_edits;
    test.project_edits = edits;
    test.project_edits_error = edits_error;
    GameMain m;
    m.dev_ui_dir = FORGE_UI_DIR;
    m.dev_game_dir = SLICE_DATA_DIR;
    m.module = kModuleId;
    m.test = [&](Shell& shell, u32, int& failures) { return test.frame(shell, failures); };
    return run_game(game, m, static_cast<int>(args.size()) - 1, args.data());
}
