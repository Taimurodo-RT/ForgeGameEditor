// The vertical slice: «Старая шахта», a small side-view game made the way a
// big one is (see docs/vertical-slice.md).
//
//   forge_slice                     play
//   forge_slice --stress            play with 200 000 critters and a million particles
//   forge_slice --play --level DIR --at X,Y [--fired FILE]
//                                   a new game from a level folder, the hero at X,Y
//                                   (the level editor's «Играть отсюда»; FILE gets the links
//                                   that happen, for its «Логика» tab; F2 shows the links
//                                   over the game and draws new ones into logic.json)
//   forge_slice --test --screenshot out.png [--scene village|mine|door|links|menu]
//                                   offscreen: plays the game through and checks it
//   forge_slice --test --window --no-vsync --scene inventory
//                                   10 000 things in a list scrolled to the end and back
//                                   in a real window; the frame times while scrolling go
//                                   to the log and to inventory-scroll.txt
//
// Controls: A/D walk, Space/W jump (and swim), left mouse digs or breaks a
// crate, right mouse builds with the selected slot (1-6), E talks, the
// wheel zooms. Esc pauses, J opens the journal, F5 saves, F9 loads.

#include "slice_game.h"

#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/core/time.h"
#include "forge/game/runner.h"
#include "forge/ui/ui.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Transform.h>
#include <RmlUi/Core/TransformPrimitive.h>

#include <SDL3/SDL_main.h> // the window-only entry point on Windows
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "simple_pages.h" // the screens of games/examples/simple-mode

using namespace forge;
using namespace forge::game;
using namespace slice;

namespace {

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
        const Rml::Vector2f p = e->GetAbsoluteOffset(Rml::BoxArea::Border) + e->GetBox().GetSize(Rml::BoxArea::Border) * 0.5f;
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

    void build(Shell& s) {
        if (scene_ == "stress") {
            build_stress(s);
            return;
        }
        if (scene_ == "inventory") {
            build_inventory(s);
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

    SliceGame& g_;
    std::string scene_;
    std::vector<Step> steps_;
    usize index_ = 0;
    u32 frames_ = 0;
    int* failures_ = nullptr;
};

} // namespace

int main(int argc, char** argv) {
    Options options;
    std::string scene = "village";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--stress") == 0) options.stress = true;
        else if (std::strcmp(argv[i], "--play") == 0) options.edit_links = true;
        else if (std::strcmp(argv[i], "--test") == 0) {
            options.silent = true;
            // «Связи» over the game change a copy of the links, not the game's.
            options.edit_links = true;
            options.links_file = std::filesystem::temp_directory_path() / "forge_slice_test_logic.json";
            std::error_code ec0;
            std::filesystem::copy_file(utf8_path(SLICE_DATA_DIR) / "logic.json", options.links_file,
                                       std::filesystem::copy_options::overwrite_existing, ec0);
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
    SliceGame game(options);
    SelfTest test(game, scene);
    GameMain m;
    m.dev_ui_dir = FORGE_UI_DIR;
    m.dev_game_dir = SLICE_DATA_DIR;
    m.test = [&](Shell& shell, u32, int& failures) { return test.frame(shell, failures); };
    return run_game(game, m, argc, argv);
}
