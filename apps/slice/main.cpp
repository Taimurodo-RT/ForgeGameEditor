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
//   forge_slice --test --screenshot out.png [--scene village|mine|door|links|menu|windows|templates|volumes|physics|light]
//                                   offscreen: plays the game through and checks it
//                                   (volumes: over a settings.json of music and sounds at 0;
//                                   physics, light: the level of games/examples/physics or light as «Играть отсюда»
//                                   starts it; light also draws the editor's view of it to compare)
//   forge_slice --test --window --no-vsync --scene inventory
//                                   10 000 things in a list scrolled to the end and back
//                                   in a real window; the frame times while scrolling go
//                                   to the log and to inventory-scroll.txt
//
// Controls: A/D walk, Space/W jump (and swim), left mouse digs or breaks a
// crate, right mouse builds with the selected slot (1-6), E talks, the
// wheel zooms. Esc pauses, J opens the journal, F5 saves, F9 loads.

#include "slice_art.h"
#include "slice_game.h"
#include "slice_level.h"

#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/core/time.h"
#include "forge/audio/screen_sounds.h"
#include "forge/game/runner.h"
#include "forge/level/level.h"
#include "forge/level/light.h"
#include "forge/render/offscreen.h"
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
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
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

    void build(Shell& s) {
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

    // The level editor's «Свет» as the game plays it: games/examples/light/level, made in the editor with the mouse
    // and the panel (its self-test makes it again and compares), read from the files the way «Играть отсюда» starts
    // it (in a package from the package's own data/examples). An old level keeps the light it always had; the
    // example's hour (21:30) and its two sources are the author's. The game's frame and the editor's view «как в
    // игре» are made with the same camera, the hero's own light put out: their light alone is compared pixel by
    // pixel, and the sources' light where it reaches (nothing past the radius). A copy at noon has a brighter sky and
    // the same lamps; a save keeps light.json; the example's files stay as they were.
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
            Shot game, editor, hero, at_noon;
        };
        auto st = std::make_shared<State>();
        const i32 v = g.generator().village_y();
        const u32 w = 1600, h = 900;
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
        auto pixel = [w, h](const std::vector<u8>& rgba, const render::Camera2D& c, f64 tx, f64 ty) {
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
        steps_.push_back({"игра и редактор светят одинаково", 5, [&s, &g, v, w, h, ax, ay, bx, by, hx, pixel, rgb, differ, lit, this,
                                                                 st](u32 f) {
            if (f > 0) return true;
            st->cam = g.camera();
            check(std::fabs(st->cam.x - hx) < 1 && st->cam.zoom == 22, "камера у героя: " + std::to_string(st->cam.x) + ", " +
                                                                           std::to_string(st->cam.y));
            // The game's frame with the hero's own light put out, as the editor has no hero.
            g.set_hero_light(false);
            check(shoot(g.device(), w, h, [&g, w, h](SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* t) { g.render(cmd, t, w, h); }, g.lights(),
                        st->game),
                  "кадр игры без света героя");
            // The editor's view «как в игре» of the same folder, the same camera and size.
            SliceLevel module;
            std::string why;
            check(module.load_objects(s.game_dir(), &why), "шаблоны игры для редактора " + why);
            check(module.init_view(g.device(), SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM), "вид редактора");
            {
                forge::level::Level level(module);
                check(level.open(st->dir), "редактор открывает пример");
                const world::Rect view = st->cam.visible_tiles(w, h);
                level.ensure_loaded({view.x0 - 48, view.y0 - 48, view.x1 + 48, view.y1 + 48});
                check(level.light().time == 21.5f, "в редакторе тоже 21:30");
                forge::level::ViewOptions options;
                options.game_light = true;
                const render::Camera2D cam = st->cam;
                check(shoot(g.device(), w, h,
                            [&](SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* t) {
                                module.prepare_view(cmd, level, cam, w, h, options, 0);
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
                      "вид редактора «как в игре»");
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
                module.level_closing(level);
            }
            module.shutdown_view();
            int most = 0;
            const usize n = differ(st->game.light, st->editor.light, most);
            FORGE_INFO("свет кадра: игра и редактор различаются в %zu пикселях из %u, не больше чем на %d", n, w * h, most);
            check(n == 0, "свет игры и вида редактора совпадает попиксельно: разных " + std::to_string(n) + ", до " + std::to_string(most));
            const auto pa = pixel(st->game.light, st->cam, ax, ay), pb = pixel(st->game.light, st->cam, bx, by);
            const auto mid = pixel(st->game.light, st->cam, hx, by), sky = pixel(st->game.light, st->cam, hx, v - 3);
            FORGE_INFO("свет в пикселях: тёплый %s, синий %s, между ними %s, небо %s", rgb(pa).c_str(), rgb(pb).c_str(), rgb(mid).c_str(),
                       rgb(sky).c_str());
            check(pa[0] == 255 && pa[0] >= pa[1] && pa[1] > pa[2] + 60, "тёплый в кадре: " + rgb(pa));
            check(pb[2] == 255 && pb[2] > pb[1] && pb[1] > pb[0], "синий в кадре: " + rgb(pb));
            check(mid[0] < 40 && mid[1] < 40 && mid[2] < 40, "между ними, вне обоих кругов, темно: " + rgb(mid));
            check(sky[2] > sky[0] + 20 && sky[2] < 100, "ночное небо тёмно-синее: " + rgb(sky));
            // The hero's own light again: brighter around him, the same elsewhere.
            g.set_hero_light(true);
            check(shoot(g.device(), w, h, [&g, w, h](SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* t) { g.render(cmd, t, w, h); }, g.lights(),
                        st->hero),
                  "кадр игры со светом героя");
            const auto lit_hero = pixel(st->hero.light, st->cam, hx, by), at_a = pixel(st->hero.light, st->cam, ax, ay);
            check(lit_hero[0] > mid[0] + 40, "у героя светлее, чем без его света: " + rgb(mid) + " → " + rgb(lit_hero));
            check(at_a == pa, "у тёплого источника так же: " + rgb(at_a));
            for (const auto& [name, shot] : {std::pair<const char*, const Shot*>{"game", &st->game}, {"editor", &st->editor}, {"hero", &st->hero}}) {
                render::write_png(path_to_utf8(st->out / (std::string(name) + ".png")).c_str(), w, h, shot->full);
                render::write_png(path_to_utf8(st->out / (std::string(name) + "-light.png")).c_str(), w, h, shot->light);
            }
            FORGE_INFO("картинки: %s (game, editor, hero и их -light)", path_to_utf8(st->out).c_str());
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
        steps_.push_back({"копия в полдень: небо светлее, источники те же", 20, [&s, &g, v, w, h, zoom, ax, ay, hx, pixel, rgb, this,
                                                                               st](u32 f) {
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
        steps_.push_back({"обратно в меню", 5, [&s, &g, files_of, this, st](u32 f) {
            if (f == 0) {
                s.to_main_menu();
                g.set_level({});
                check(files_of(st->dir) == st->files, "файлы примера как были");
                std::error_code ec;
                std::filesystem::remove_all(st->noon, ec);
            }
            return f >= 2;
        }});
    }

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
    // --scene volumes: the game starts over the settings.json a player left, in this run's own folder: music and
    // sounds at 0, all at 100, as the game writes it.
    std::vector<char*> args(argv, argv + argc);
    std::string user;
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
    GameMain m;
    m.dev_ui_dir = FORGE_UI_DIR;
    m.dev_game_dir = SLICE_DATA_DIR;
    m.test = [&](Shell& shell, u32, int& failures) { return test.frame(shell, failures); };
    return run_game(game, m, static_cast<int>(args.size()) - 1, args.data());
}
