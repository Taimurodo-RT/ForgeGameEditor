#include "match3_module.h"

#include "match3_game.h"
#include "match3_testgame.h"

#include "forge/assets/image.h"
#include "forge/core/ease.h"
#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/game/runner.h"
#include "forge/game/screens.h"
#include "forge/render/offscreen.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Context.h>
#include <SDL3/SDL.h>
#include <yyjson.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <optional>

#ifndef FORGE_UI_DIR
#define FORGE_UI_DIR "ui"
#endif
#ifndef MATCH3_GAME_DIR
#define MATCH3_GAME_DIR "tests/data/match3/game"
#endif
#ifndef MATCH3_MODULE_DIR
#define MATCH3_MODULE_DIR "games/modules/match3"
#endif
#ifndef MATCH3_GAMES_DIR
#define MATCH3_GAMES_DIR "games"
#endif

namespace match3 {

namespace fs = std::filesystem;
using namespace forge;
using game::Screen;
using game::Shell;

namespace {

// --- the self-test --------------------------------------------------------------------------------------------------
//
// --scene match3           the bot plays the three fields of the test game with the mouse (clicks and drags) to the
//                          window of victory: the values, the gems put before and the holes, the pixels of every
//                          board at rest and the frames between (a swap, a row vanishing by its frames or fading,
//                          gems falling), each level's reward once, «Дальше» from the window «Уровень пройден»
// --scene match3_lose      the field «Короткий» (two moves, an aim out of reach): lost once, the window «Поражение»,
//                          «Ещё раз» the same field anew; «Ещё раз» while gems fall and while the end is still to
//                          come: nothing of the old field happens after it
// --scene match3_next      «Дальше» while the last move still plays: no reward, no window of the old field; Esc
//                          during an animation stops it; «Сохранить» then says why not, an autosave keeps quiet
// --scene match3_save      three moves, saved between moves as m3-slot, what two moves more make written down
// --scene match3_continue  (another process) m3-slot goes on with the same board, and the same two moves make the
//                          same board; a save with a gem the game no longer has is refused, the game as it was
// --scene match3_make      the test game made anew, byte for byte the one in tests/data/match3

struct Step {
    const char* name;
    u32 max_frames;
    std::function<bool(u32 frame)> run;
};

using Rgb = std::array<int, 3>;

std::string rgb_text(const Rgb& p) { return std::to_string(p[0]) + "," + std::to_string(p[1]) + "," + std::to_string(p[2]); }
bool near(const Rgb& a, const Rgb& b, int most) {
    for (int i = 0; i < 3; ++i)
        if (std::abs(a[i] - b[i]) > most) return false;
    return true;
}

// The files of a folder by their paths in it.
std::map<std::string, std::vector<u8>> files_of(const fs::path& dir) {
    std::map<std::string, std::vector<u8>> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::vector<u8> bytes;
        read_file(it->path(), bytes);
        std::string name = path_to_utf8(it->path().lexically_relative(dir));
        std::replace(name.begin(), name.end(), '\\', '/');
        out[name] = std::move(bytes);
    }
    return out;
}

class SelfTest {
public:
    SelfTest(Match3Game& game, std::string scene) : g_(game), scene_(std::move(scene)) {}

    bool frame(Shell& s, u32 f, int& failures) {
        failures_ = &failures;
        if (f == 0) build(s);
        if (at_ >= steps_.size()) return false;
        Step& st = steps_[at_];
        if (st.run(in_step_)) {
            FORGE_INFO("self-test: шаг «%s» пройден (%u кадров)", st.name, in_step_);
            ++at_;
            in_step_ = 0;
            return at_ < steps_.size();
        }
        if (++in_step_ > st.max_frames) {
            fail(std::string("шаг «") + st.name + "» не закончился за " + std::to_string(st.max_frames) + " кадров" + state());
            return false;
        }
        return true;
    }

private:
    // --- checks ---
    void check(bool ok, const std::string& what) {
        if (ok) FORGE_INFO("self-test: ok   %s", what.c_str());
        else fail(what);
    }
    void fail(const std::string& what) {
        ++*failures_;
        FORGE_ERROR("self-test: FAIL %s", what.c_str());
    }
    std::string state() const {
        const Play* p = g_.play();
        std::string out = " [уровень «" + g_.level_id() + "», поле " + std::to_string(g_.generation());
        if (p) out += ", очков " + std::to_string(p->score()) + " из " + std::to_string(p->target()) + ", ходов " + std::to_string(p->moves());
        out += ", фаза " + std::to_string(static_cast<int>(g_.phase())) + ", итог " + std::to_string(static_cast<int>(g_.outcome())) + "]";
        return out;
    }
    f64 var(const char* name) const { return s_->vars().get(name).number(); }
    std::string text(const char* name) const { return s_->vars().get(name).text(); }

    // --- the mouse and the keys, as a player's ---
    void send(const SDL_Event& e) {
        if (!s_->handle_event(e) && s_->screen() == Screen::Playing && !s_->in_dialogue()) g_.handle_event(e);
    }
    void mouse(SDL_EventType type, f32 x, f32 y) {
        SDL_Event e{};
        e.type = type;
        if (type == SDL_EVENT_MOUSE_MOTION) {
            e.motion.x = x;
            e.motion.y = y;
        } else {
            e.button.button = SDL_BUTTON_LEFT;
            e.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            e.button.x = x;
            e.button.y = y;
        }
        send(e);
    }
    void key(SDL_Keycode k) {
        for (bool down : {true, false}) {
            SDL_Event e{};
            e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            e.key.key = k;
            e.key.down = down;
            send(e);
        }
    }
    // A move with the mouse: a click on each gem, or a drag from one onto the other.
    bool move(i32 a, i32 b, bool drag) {
        f32 ax, ay, bx, by;
        if (!g_.cell_on_screen(a, ax, ay) || !g_.cell_on_screen(b, bx, by)) return false;
        mouse(SDL_EVENT_MOUSE_MOTION, ax, ay);
        mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, ax, ay);
        if (drag) {
            mouse(SDL_EVENT_MOUSE_MOTION, (ax + bx) * 0.5f, (ay + by) * 0.5f);
            mouse(SDL_EVENT_MOUSE_MOTION, bx, by);
            mouse(SDL_EVENT_MOUSE_BUTTON_UP, bx, by);
        } else {
            mouse(SDL_EVENT_MOUSE_BUTTON_UP, ax, ay);
            if (g_.picked() != a) return false;
            mouse(SDL_EVENT_MOUSE_MOTION, bx, by);
            mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, bx, by);
            mouse(SDL_EVENT_MOUSE_BUTTON_UP, bx, by);
        }
        return true;
    }
    // A button of a window («Кнопка «Дальше»»), pressed with the mouse where the player sees it.
    bool press(const std::string& page, const std::string& words) {
        Rml::ElementDocument* doc = s_->screens().document(page);
        const std::string title = "Кнопка «" + words + "»";
        std::function<Rml::Element*(Rml::Element*)> find = [&](Rml::Element* e) -> Rml::Element* {
            if (!e) return nullptr;
            if (e->HasAttribute("forge-click") && e->GetAttribute<Rml::String>("title", "") == title) return e;
            for (int i = 0; i < e->GetNumChildren(); ++i)
                if (Rml::Element* found = find(e->GetChild(i))) return found;
            return nullptr;
        };
        Rml::Element* e = find(doc);
        Rml::Element* root = e;
        while (root && !root->HasAttribute("forge-screen")) root = root->GetParentNode();
        if (!e || !root) return false;
        f32 w = 1920, h = 1080;
        std::sscanf(root->GetAttribute<Rml::String>("forge-size", "").c_str(), "%f %f", &w, &h);
        const Rml::Vector2f c = e->GetAbsoluteOffset(Rml::BoxArea::Border) - root->GetAbsoluteOffset(Rml::BoxArea::Border) +
                                e->GetBox().GetSize(Rml::BoxArea::Border) * 0.5f;
        const Rml::Vector2i size = s_->context()->GetDimensions();
        const game::ScreenFit fit = game::fit_screen(root->GetAttribute<Rml::String>("forge-fit", ""), w, h,
                                                     static_cast<f32>(size.x), static_cast<f32>(size.y));
        const f32 x = game::fit_to_view_x(fit, c.x), y = game::fit_to_view_y(fit, c.y);
        mouse(SDL_EVENT_MOUSE_MOTION, x, y);
        mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y);
        mouse(SDL_EVENT_MOUSE_BUTTON_UP, x, y);
        return true;
    }

    // --- the pixels ---
    bool grab() {
        const u32 w = g_.frame_width(), h = g_.frame_height();
        SDL_GPUDevice* device = g_.device();
        SDL_GPUTexture* target = w && h ? render::create_render_target(device, w, h) : nullptr;
        if (!target) return false;
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
        s_->render(cmd, target, w, h);
        SDL_SubmitGPUCommandBuffer(cmd);
        const bool ok = render::read_pixels(device, target, w, h, px_);
        SDL_ReleaseGPUTexture(device, target);
        pw_ = w;
        ph_ = h;
        return ok;
    }
    Rgb pixel(f32 x, f32 y) const {
        const i32 px = static_cast<i32>(std::floor(x)), py = static_cast<i32>(std::floor(y));
        if (px < 0 || py < 0 || px >= static_cast<i32>(pw_) || py >= static_cast<i32>(ph_)) return {-1, -1, -1};
        const usize i = (static_cast<usize>(py) * pw_ + static_cast<usize>(px)) * 4;
        return {px_[i], px_[i + 1], px_[i + 2]};
    }
    // Where a point of the level (tiles) is on the screen.
    void screen_of(f64 tx, f64 ty, f32& sx, f32& sy) const {
        f32 x0, y0;
        g_.cell_on_screen(0, x0, y0); // cell 0's middle
        const Field& f = *g_.field();
        const f64 cx = f.x0 + 0.5, cy = f.y0 + 0.5;
        sx = static_cast<f32>(x0 + (tx - cx) * g_.cell_pixels());
        sy = static_cast<f32>(y0 + (ty - cy) * g_.cell_pixels());
    }
    // A gem's colour in the middle of one of its frames: its picture's, or the disc of a gem without one.
    Rgb colour(u8 gem, u32 frame) {
        const std::pair<u8, u32> key{gem, frame};
        if (auto it = colours_.find(key); it != colours_.end()) return it->second;
        Rgb out{-1, -1, -1};
        const GemSet& set = g_.gems();
        if (gem < set.size()) {
            const objects::Template& t = *set.gems[gem];
            std::vector<u8> bytes;
            assets::CookedTexture img;
            const fs::path file = t.picture.empty() ? fs::path() : s_->game_dir() / "pictures" / utf8_path(t.picture);
            if (!file.empty() && read_file(file, bytes) && assets::decode_image(bytes, img)) {
                const u32 frames = std::max(1u, t.frames), fw = img.width / frames, f = std::min(frame, frames - 1);
                const u8* p = &img.rgba8[(static_cast<usize>(img.height / 2) * img.width + f * fw + fw / 2) * 4];
                out = {p[0], p[1], p[2]};
            } else {
                const u32 c = Painter::placeholder(gem);
                out = {static_cast<int>(c & 0xff), static_cast<int>((c >> 8) & 0xff), static_cast<int>((c >> 16) & 0xff)};
            }
        }
        colours_[key] = out;
        return out;
    }
    Rgb cell_colour(i32 cell) const {
        const Field& f = *g_.field();
        const bool dark = static_cast<usize>(cell) < f.dark.size() && f.dark[static_cast<usize>(cell)];
        return dark ? Rgb{70, 78, 100} : Rgb{92, 100, 122};
    }

    // Every cell at rest: its gem's colour in its middle; a hole: the background.
    void check_rest(const std::string& what) {
        if (!grab()) {
            fail(what + ": кадр не снят");
            return;
        }
        const Board& b = g_.play()->board();
        u32 good = 0, holes = 0;
        std::string bad;
        for (i32 i = 0; i < static_cast<i32>(b.cells.size()); ++i) {
            f32 x, y;
            g_.cell_on_screen(i, x, y);
            const Rgb p = pixel(x, y);
            if (b.cells[static_cast<usize>(i)] == kHole) {
                if (near(p, {28, 26, 41}, 6)) ++holes;
                else bad += " дыра " + std::to_string(i) + ": " + rgb_text(p);
                continue;
            }
            const Rgb want = colour(b.cells[static_cast<usize>(i)], 0);
            if (near(p, want, 12)) ++good;
            else if (bad.size() < 400) bad += " клетка " + std::to_string(i) + ": " + rgb_text(p) + " вместо " + rgb_text(want);
        }
        check(bad.empty(), what + ": фишки на своих клетках (" + std::to_string(good) + "), дыры пусты (" + std::to_string(holes) + ")" + bad);
    }

    // The frames between, while a move plays (once each per move): the swap half-way, a row vanishing, gems falling.
    void check_between() {
        const Play* p = g_.play();
        if (!p) return;
        const f64 t = g_.phase_progress();
        const auto& drawn = g_.drawn();
        if (g_.phase() == Match3Game::Phase::Swap && !seen_swap_ && t >= 0.75 && t <= 0.95) {
            seen_swap_ = true;
            if (!grab()) return fail("кадр обмена не снят");
            // Most of the way each gem is where the easing puts it between the two cells (not yet in the other's
            // middle, and far enough from the other gem): there its colour shows.
            const Field& f = *g_.field();
            const Board& b = p->board();
            const f64 k = ease::in_out(t);
            std::string bad;
            for (const auto& [from, to] : {std::pair{move_a_, move_b_}, std::pair{move_b_, move_a_}}) {
                const Match3Game::Drawn& d = drawn[static_cast<usize>(from)];
                const f64 wx = ease::lerp(f.x0 + b.x_of(from) + 0.5, f.x0 + b.x_of(to) + 0.5, k);
                const f64 wy = ease::lerp(f.y0 + b.y_of(from) + 0.5, f.y0 + b.y_of(to) + 0.5, k);
                if (std::fabs(d.x - wx) > 1e-6 || std::fabs(d.y - wy) > 1e-6)
                    bad += " фишка из " + std::to_string(from) + " в (" + std::to_string(d.x) + ", " + std::to_string(d.y) + "), а не в (" +
                           std::to_string(wx) + ", " + std::to_string(wy) + ")";
                f32 x, y;
                screen_of(d.x, d.y, x, y);
                const Rgb got = pixel(x, y), want = colour(d.gem, 0);
                if (!near(got, want, 12)) bad += " фишка из " + std::to_string(from) + ": " + rgb_text(got) + " вместо " + rgb_text(want);
            }
            between_[0] += bad.empty();
            if (!bad.empty()) fail("обмен (" + std::to_string(t) + "):" + bad + state());
        }
        if (g_.phase() == Match3Game::Phase::Vanish && !seen_vanish_ && t >= 0.4 && t <= 0.75) {
            seen_vanish_ = true;
            if (!grab()) return fail("кадр исчезновения не снят");
            std::string bad;
            for (usize i = 0; i < drawn.size(); ++i) {
                const Match3Game::Drawn& d = drawn[i];
                if (d.gem >= kNone || (d.alpha >= 1 && d.frame == 0)) continue;
                f32 x, y;
                screen_of(d.x, d.y, x, y);
                const Rgb got = pixel(x, y);
                Rgb want = colour(d.gem, d.frame);
                if (d.alpha < 1) { // fading: its colour over the cell's
                    const Rgb under = cell_colour(static_cast<i32>(i));
                    for (int c = 0; c < 3; ++c) want[c] = static_cast<int>(std::lround(want[c] * static_cast<f64>(d.alpha) + under[c] * (1 - static_cast<f64>(d.alpha))));
                    ++fades_;
                } else ++frames_;
                if (!near(got, want, 14)) bad += " " + std::to_string(i) + ": " + rgb_text(got) + " вместо " + rgb_text(want);
            }
            between_[1] += bad.empty();
            if (!bad.empty()) fail("ряд исчезает (" + std::to_string(t) + "):" + bad + state());
        }
        // Any frame of a fall with a gem passing a hole (a few per scene): the hole hides it.
        if (g_.phase() == Match3Game::Phase::Fall && in_hole_ < 6) {
            const Field& f = *g_.field();
            std::vector<usize> passing;
            for (usize i = 0; i < drawn.size(); ++i) {
                const Match3Game::Drawn& d = drawn[i];
                const f64 ry = d.y - f.y0, rx = d.x - f.x0;
                if (d.gem >= kNone || ry < 0 || std::fabs(ry - std::round(ry)) < 0.2) continue;
                if (!f.shape.cell(static_cast<i32>(std::floor(rx)), static_cast<i32>(std::floor(ry)))) passing.push_back(i);
            }
            if (!passing.empty() && grab()) {
                std::string bad;
                for (usize i : passing) {
                    f32 x, y;
                    screen_of(drawn[i].x, drawn[i].y, x, y);
                    const Rgb got = pixel(x, y);
                    if (!near(got, {28, 26, 41}, 6)) bad += " " + std::to_string(i) + ": " + rgb_text(got);
                    ++in_hole_;
                }
                if (!bad.empty()) fail("фишка видна в дыре поля:" + bad + state());
            }
        }
        if (g_.phase() == Match3Game::Phase::Fall && !seen_fall_ && t >= 0.3 && t <= 0.7) {
            seen_fall_ = true;
            if (!grab()) return fail("кадр падения не снят");
            const Board& b = p->board();
            const Field& f = *g_.field();
            u32 moving = 0;
            std::string bad;
            for (usize i = 0; i < drawn.size(); ++i) {
                const Match3Game::Drawn& d = drawn[i];
                if (d.gem >= kNone) continue;
                const f64 rest = f.y0 + b.y_of(static_cast<i32>(i)) + 0.5;
                if (std::fabs(d.y - rest) < 0.05) continue;
                ++moving;
                if (d.y > rest) bad += " " + std::to_string(i) + " ниже своей клетки";
                // Its colour where it is drawn, when no other gem is drawn near it.
                bool alone = true;
                for (usize j = 0; j < drawn.size(); ++j)
                    if (j != i && drawn[j].gem < kNone && std::fabs(drawn[j].x - d.x) < 0.95 && std::fabs(drawn[j].y - d.y) < 0.95) alone = false;
                f32 x, y;
                screen_of(d.x, d.y, x, y);
                // Above the field and over its holes a gem is hidden: there the background shows (above the field
                // the HUD may be over it, so only a hole is looked at).
                const f64 ry = d.y - f.y0, rx = d.x - f.x0;
                const i32 row = static_cast<i32>(std::floor(ry)), col = static_cast<i32>(std::floor(rx));
                const bool edge = std::fabs(ry - std::round(ry)) < 0.08; // on the line between two cells
                if (row < 0) {
                    ++hidden_;
                    continue;
                }
                if (edge) continue;
                if (!f.shape.cell(col, row)) {
                    ++hidden_;
                    const Rgb got = pixel(x, y);
                    if (!near(got, {28, 26, 41}, 6)) bad += " " + std::to_string(i) + " видна в дыре: " + rgb_text(got);
                    continue;
                }
                if (!alone) continue;
                const Rgb got = pixel(x, y);
                if (!near(got, colour(d.gem, 0), 12)) bad += " " + std::to_string(i) + ": " + rgb_text(got) + " вместо " + rgb_text(colour(d.gem, 0));
            }
            between_[2] += bad.empty() && moving > 0;
            if (!bad.empty() || moving == 0) fail("фишки падают (" + std::to_string(t) + "), в пути " + std::to_string(moving) + ":" + bad + state());
        }
    }

    // Plays the field with the bot till it is over (the board's end): one move whenever the board is calm, clicks and
    // drags by turns. between: the frames between checked.
    bool bot(u32 f, bool between, u32 most_moves = 1000) {
        const Play* p = g_.play();
        if (!p) return false;
        if (between) check_between();
        if (g_.phase() != Match3Game::Phase::Calm || p->busy()) return false;
        if (moved_) { // a move just ended: the board at rest
            moved_ = false;
            check_rest("ход " + std::to_string(moves_) + " сыгран");
            if (between && !(seen_swap_ && seen_vanish_ && seen_fall_)) fail("ход " + std::to_string(moves_) + ": не все кадры между сняты" + state());
        }
        if (p->end() != End::None || moves_on_field_ >= most_moves) return true;
        if (f % 2) return false; // a frame between moves
        const auto m = find_move(p->board());
        if (!m) return false; // a shuffle comes, or the end
        move_a_ = m->first;
        move_b_ = m->second;
        seen_swap_ = seen_vanish_ = seen_fall_ = false;
        const u32 left = p->moves();
        const bool drag = moves_ % 2 == 1;
        check(move(move_a_, move_b_, drag), std::string("ход ") + std::to_string(moves_ + 1) + (drag ? " протяжкой" : " щелчками") +
                                                ": " + std::to_string(move_a_) + " ↔ " + std::to_string(move_b_));
        check(g_.phase() == Match3Game::Phase::Swap && p->moves() == left - 1, "обмен начался, ход потрачен сразу" + state());
        ++moves_;
        ++moves_on_field_;
        moved_ = true;
        return false;
    }

    // A new field: what its values say.
    void check_level(const char* id, const char* name, u32 number, u32 moves, u64 target) {
        moves_on_field_ = 0;
        check(g_.level_id() == id && text("board.level") == name && var("board.level_number") == number,
              std::string("уровень «") + name + "» (" + id + "), номер " + std::to_string(number) + state());
        check(var("board.moves") == moves && var("board.target") == static_cast<f64>(target) && var("board.score") == 0,
              "его ходы " + std::to_string(moves) + " и цель " + std::to_string(target) + " в значениях экрана, очков 0");
        check(g_.play()->board().cells == expected_start(), "поле то, что даёт его зерно (те же фишки, что у правил без окна)");
    }
    // The board the level's rules make from its seed, played without the game.
    std::vector<u8> expected_start() {
        const Field& f = *g_.field();
        Play p;
        std::string why;
        p.start(f.shape, g_.gems().size(), g_.gems().scoring, f.rules, "проверка", why);
        return p.board().cells;
    }
    u32 count(Event::Kind kind, u64 generation) const {
        u32 n = 0;
        for (const Match3Game::Said& s : g_.said())
            if (s.event.kind == kind && s.generation == generation) ++n;
        return n;
    }
    bool window(const char* name) const {
        const std::vector<std::string> up = s_->screens().windows();
        return std::find(up.begin(), up.end(), name) != up.end();
    }

    // --- the scenes ---
    void build(Shell& s) {
        s_ = &s;
        const std::string sc = scene_;
        if (sc == "match3_make") return make_scene();
        steps_.push_back({"данные тестовой игры", 5, [this](u32) {
            check(g_.problems().empty(), "данные игры читаются: " + (g_.problems().empty() ? std::string("без ошибок") : g_.problems().front()));
            const GemSet& set = g_.gems();
            std::string ids;
            for (const objects::Template* t : set.gems) ids += (ids.empty() ? "" : ", ") + t->id;
            check(ids == "emerald, ruby, sapphire, topaz", "набор поля — фишки с «На поле», по id: " + ids + " (аметиста нет)");
            check(set.scoring.points == std::vector<u32>{10, 10, 10, 20}, "их очки из шаблонов");
            for (const modules::Value& v : values(nullptr)) check(s_->vars().has(v.var), "значение «" + v.words + "» (" + v.var + ") есть");
            return true;
        }});
        if (sc == "match3") main_scene();
        else if (sc == "match3_lose") lose_scene();
        else if (sc == "match3_next") next_scene();
        else if (sc == "match3_save") save_scene();
        else if (sc == "match3_continue") continue_scene();
        else
            steps_.push_back({"сцена есть", 1, [this](u32) {
                fail("сцены «" + scene_ + "» нет");
                return true;
            }});
    }

    void new_game() {
        steps_.push_back({"новая игра", 10, [this](u32) {
            check(s_->new_game(), "новая игра начинается" + (g_.problems().empty() ? std::string() : ": " + g_.problems().back()));
            return true;
        }});
        steps_.push_back({"первый кадр", 3, [this](u32 f) { return f >= 2; }});
    }

    // Plays the field to its end, then waits for its window.
    void play_level(const char* name, bool between) {
        steps_.push_back({name, 20000, [this, between](u32 f) {
            if (!bot(f, between)) return false;
            const u64 gen = g_.generation();
            check(g_.play()->end() == End::Won && count(Event::Kind::Target, gen) == 1 && count(Event::Kind::MovesOut, gen) == 0,
                  "цель набрана за " + std::to_string(moves_on_field_) + " ходов, событие «цель» одно" + state());
            if (between) check(between_[0] > 0 && between_[1] > 0 && between_[2] > 0 && hidden_ > 0,
                               "кадры между: обменов " + std::to_string(between_[0]) + ", исчезновений " + std::to_string(between_[1]) +
                                   ", падений " + std::to_string(between_[2]) + "; фишек, скрытых над полем и в дырах, " +
                                   std::to_string(hidden_));
            return true;
        }});
    }

    void main_scene() {
        new_game();
        steps_.push_back({"«Первый»", 5, [this](u32) {
            check_level("level", "Первый", 1, 10, 150);
            check_rest("«Первый» в начале");
            return true;
        }});
        play_level("бот играет «Первый»", true);
        steps_.push_back({"окно «Уровень пройден»", 60, [this](u32) {
            if (g_.outcome() != Match3Game::Outcome::LevelPassed) return false;
            check(window("level") && s_->screens().pauses(), "окно «Уровень пройден» показано, игра стоит");
            check(text("game.result") == "уровень пройден", "game.result: «" + text("game.result") + "»");
            const u64 gen = g_.generation();
            u32 rewards = 0;
            for (u32 id : g_.fired())
                if (const logic::Link* l = links().find(id); l && l->verb == "reaches_target") ++rewards;
            check(rewards == 1 && count(Event::Kind::Target, gen) == 1, "связь «Поле набирает цель» сработала один раз");
            std::string why;
            check(g_.can_save(&why), "с окном «Уровень пройден» сохранить можно (окно вернётся)");
            check(fades_ + frames_ > 0, "ряды исчезали: тая " + std::to_string(fades_) + ", кадрами " + std::to_string(frames_));
            check(press("level", "Дальше"), "«Дальше» нажата мышью");
            return true;
        }});
        steps_.push_back({"«Второй»", 30, [this](u32) {
            if (g_.level_id() != "second") return false;
            check(!window("level") && s_->screen() == Screen::Playing, "окно закрыто, игра идёт");
            check_level("second", "Второй", 2, 10, 200);
            // Its holes, and the gems the author put before, where they were put.
            const Board& b = g_.play()->board();
            const GemSet& set = g_.gems();
            check(b.at(0, 0) == kHole && b.at(6, 0) == kHole && b.at(3, 3) == kHole && b.at(6, 6) == kHole, "дыры поля — дыры");
            check(b.at(1, 1) == set.index("ruby") && b.at(2, 1) == set.index("ruby") && b.at(5, 5) == set.index("sapphire"),
                  "фишки, поставленные заранее, на своих клетках");
            check_rest("«Второй» в начале");
            return true;
        }});
        play_level("бот играет «Второй»", true);
        steps_.push_back({"«Дальше» на «Третий»", 60, [this](u32) {
            if (g_.outcome() != Match3Game::Outcome::LevelPassed) return false;
            check(press("level", "Дальше"), "«Дальше» нажата мышью");
            return true;
        }});
        steps_.push_back({"«Третий»", 30, [this](u32) {
            if (g_.level_id() != "third") return false;
            check_level("third", "Третий", 3, 10, 200);
            const Board& b = g_.play()->board();
            check(b.at(3, 0) == kHole && b.at(4, 1) == kHole && b.gem(3, 2), "вырез сверху: дыры, под ними клетки");
            check_rest("«Третий» в начале");
            return true;
        }});
        play_level("бот играет «Третий»", true);
        steps_.push_back({"победа", 60, [this](u32) {
            if (g_.outcome() != Match3Game::Outcome::Won) return false;
            check(window("win") && !window("level"), "последний уровень: окно победы, не «Уровень пройден»");
            check(text("game.result") == "победа", "game.result: «победа»");
            std::string why;
            const bool refused = !g_.can_save(&why) && !s_->save("m3-after", "после победы");
            check(refused && why == "игра пройдена", "пройденная игра не сохраняется: «" + why + "»");
            check(g_.stale() == 0 && g_.stale_events() == 0, "ничего от прежних полей не случилось");
            check(in_hole_ > 0, "фишки, что падают сквозь дыры и вырез, там скрыты: " + std::to_string(in_hole_));
            return true;
        }});
    }

    void lose_scene() {
        steps_.push_back({"поле «Короткий»", 3, [this](u32) {
            g_.set_level(s_->game_dir().parent_path() / "short");
            return true;
        }});
        new_game();
        steps_.push_back({"два хода", 3000, [this](u32 f) {
            if (f == 0) {
                check(g_.level_id().empty() && text("board.level") == "short" && var("board.moves") == 2 && var("board.target") == 5000,
                      "поле не из уровней игры: 2 хода, цель 5000" + state());
                start_ = g_.play()->board().cells;
            }
            return bot(f, false);
        }});
        steps_.push_back({"поражение", 60, [this](u32) {
            if (g_.outcome() != Match3Game::Outcome::Lost) return false;
            const u64 gen = g_.generation();
            check(count(Event::Kind::MovesOut, gen) == 1 && count(Event::Kind::Target, gen) == 0, "событие «ходы кончились» одно, цели нет");
            check(window("lose") && text("board.reason") == "Ходы кончились", "окно «Поражение»: «" + text("board.reason") + "»");
            std::string why;
            const bool refused = !g_.can_save(&why);
            check(refused && !why.empty(), "проигранный уровень не сохраняется: «" + why + "»");
            gen_ = gen;
            check(press("lose", "Ещё раз"), "«Ещё раз» нажата мышью");
            return true;
        }});
        steps_.push_back({"«Ещё раз»", 30, [this](u32) {
            if (g_.generation() == gen_) return false;
            check(g_.generation() == gen_ + 1 && g_.outcome() == Match3Game::Outcome::Playing && !window("lose"),
                  "то же поле заново: следующее поколение, окна нет" + state());
            check(var("board.moves") == 2 && var("board.score") == 0 && g_.play()->board().cells == start_,
                  "ходов снова 2, очков 0, фишки те же (зерно уровня)");
            moves_on_field_ = 0;
            return true;
        }});
        // «Ещё раз» while the gems fall: the old field says nothing after it.
        steps_.push_back({"«Ещё раз» посреди падения", 600, [this](u32 f) {
            if (g_.phase() == Match3Game::Phase::Fall && g_.phase_progress() > 0.3) {
                const usize said = g_.said().size();
                gen_ = g_.generation();
                check(g_.retry_level(), "«Ещё раз» посреди падения");
                check(g_.generation() == gen_ + 1 && g_.phase() == Match3Game::Phase::Calm && g_.said().size() == said,
                      "новое поле спокойно, событий не прибавилось" + state());
                said_ = g_.said().size();
                return true;
            }
            bot(f, false);
            return false;
        }});
        steps_.push_back({"после него старое поле молчит", 90, [this](u32 f) {
            if (f < 80) return false;
            check(g_.said().size() == said_ && g_.stale_events() == 0, "за 80 кадров ни одного события старого поля");
            moves_on_field_ = 0;
            return true;
        }});
        // «Ещё раз» after the last move settled, before the window came: the window never comes.
        steps_.push_back({"«Ещё раз», пока окно поражения не пришло", 3000, [this](u32 f) {
            const Play* p = g_.play();
            if (p->end() == End::Lost && g_.outcome() == Match3Game::Outcome::Playing) {
                stale_ = g_.stale();
                gen_ = g_.generation();
                check(g_.retry_level(), "«Ещё раз», пока окно ещё не показано");
                return true;
            }
            bot(f, false);
            return false;
        }});
        steps_.push_back({"окна старого поля нет", 60, [this](u32 f) {
            if (f < 50) return false;
            check(g_.stale() == stale_ + 1, "отложенное поражение старого поля отброшено (" + std::to_string(g_.stale()) + ")");
            check(!window("lose") && g_.outcome() == Match3Game::Outcome::Playing && g_.generation() == gen_ + 1,
                  "окна поражения нет, новое поле играет" + state());
            check(g_.stale_events() == 0, "событий старых полей нет");
            return true;
        }});
    }

    void next_scene() {
        new_game();
        // Esc while a move plays: the board stops; «Сохранить» says why not; an autosave keeps quiet.
        steps_.push_back({"пауза посреди хода", 600, [this](u32 f) {
            if (g_.phase() == Match3Game::Phase::Vanish && g_.phase_progress() > 0.2) {
                key(SDLK_ESCAPE);
                check(s_->screen() == Screen::Paused, "Esc посреди хода — пауза");
                progress_ = g_.phase_progress();
                return true;
            }
            bot(f, false);
            return false;
        }});
        steps_.push_back({"на паузе поле стоит", 40, [this](u32 f) {
            if (f < 30) return false;
            check(g_.phase() == Match3Game::Phase::Vanish && g_.phase_progress() == progress_, "за 30 кадров паузы анимация не сдвинулась");
            std::string why;
            const bool refused = !g_.can_save(&why);
            check(refused && why == "Сохранить можно между ходами: фишки ещё падают", "сохранить нельзя: «" + why + "»");
            check(!s_->save("m3-busy", "посреди хода") && !s_->slots().exists("m3-busy"), "«Сохранить» слот не пишет");
            check(!s_->save("autosave", "Автосохранение", true) && !s_->slots().exists("autosave"), "автосохранение ждёт спокойного поля");
            key(SDLK_ESCAPE);
            check(s_->screen() == Screen::Playing, "Esc — игра дальше");
            return true;
        }});
        steps_.push_back({"анимация идёт дальше", 30, [this](u32) {
            if (g_.phase() == Match3Game::Phase::Vanish && g_.phase_progress() == progress_) return false;
            check(true, "после паузы ход доигрывается");
            return true;
        }});
        // «Дальше» while the move that reaches the target still plays: the old level gives nothing.
        steps_.push_back({"«Дальше» посреди последнего хода", 20000, [this](u32 f) {
            const Play* p = g_.play();
            if (g_.phase() != Match3Game::Phase::Calm && p->score() >= p->target()) {
                gen_ = g_.generation();
                said_ = g_.said().size();
                check(g_.next_level(), "«Дальше», пока последний ход ещё играет");
                check(g_.level_id() == "second" && g_.generation() == gen_ + 1 && g_.phase() == Match3Game::Phase::Calm,
                      "начался «Второй», спокойное поле" + state());
                return true;
            }
            bot(f, false);
            return false;
        }});
        steps_.push_back({"от «Первого» ничего", 90, [this](u32 f) {
            if (f < 80) return false;
            check(count(Event::Kind::Target, gen_) == 0 && g_.said().size() == said_ && g_.stale_events() == 0,
                  "награды и событий брошенного поля нет");
            check(!window("level") && g_.outcome() == Match3Game::Outcome::Playing, "окна «Уровень пройден» нет");
            check(var("board.score") == 0 && var("board.level_number") == 2, "очки и номер — нового поля");
            return true;
        }});
    }

    // A save after the first move of «Первый», and what the two moves after it make (the second of them reaches the
    // target): written for match3_continue, which plays them again in another process from the save.
    void save_scene() {
        new_game();
        steps_.push_back({"один ход", 6000, [this](u32 f) { return bot(f, false, 1); }});
        steps_.push_back({"сохранено между ходами", 5, [this](u32) {
            std::string why;
            const bool can = g_.can_save(&why);
            check(can && s_->save("m3-slot", "Один ход"), "сохранение между ходами в m3-slot" + (why.empty() ? std::string() : ": " + why));
            saved_ = g_.play()->saved();
            gen_ = g_.generation();
            moves_on_field_ = 0;
            return true;
        }});
        steps_.push_back({"ещё два хода", 6000, [this](u32 f) { return bot(f, false, 2); }});
        steps_.push_back({"записано, что будет", 5, [this](u32) {
            std::string json = "{\"board\": [";
            for (usize i = 0; i < g_.play()->board().cells.size(); ++i) json += (i ? "," : "") + std::to_string(g_.play()->board().cells[i]);
            json += "], \"score\": " + std::to_string(g_.play()->score()) + ", \"end\": " + std::to_string(static_cast<int>(g_.play()->end())) +
                    ", \"saved_score\": " + std::to_string(saved_.score) +
                    ", \"saved_moves\": " + std::to_string(saved_.moves) + ", \"saved_rng\": \"" + std::to_string(saved_.rng) +
                    "\", \"generation\": " + std::to_string(gen_) + "}\n";
            check(write_file_atomic(s_->user_folder() / "m3-expect.json", {reinterpret_cast<const u8*>(json.data()), json.size()}),
                  "ожидания для другого процесса записаны");
            return true;
        }});
    }

    void continue_scene() {
        steps_.push_back({"m3-slot из другого процесса", 30, [this](u32) {
            std::vector<u8> text;
            check(read_file(s_->user_folder() / "m3-expect.json", text), "ожидания match3_save есть");
            yyjson_doc* doc = yyjson_read(reinterpret_cast<const char*>(text.data()), text.size(), 0);
            yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
            yyjson_val* board = yyjson_obj_get(root, "board");
            for (usize i = 0; i < yyjson_arr_size(board); ++i) after_.push_back(static_cast<u8>(yyjson_get_uint(yyjson_arr_get(board, i))));
            after_score_ = yyjson_get_uint(yyjson_obj_get(root, "score"));
            after_end_ = static_cast<int>(yyjson_get_int(yyjson_obj_get(root, "end")));
            const u64 score = yyjson_get_uint(yyjson_obj_get(root, "saved_score")), moves = yyjson_get_uint(yyjson_obj_get(root, "saved_moves"));
            const char* rng = yyjson_get_str(yyjson_obj_get(root, "saved_rng"));
            const u64 gen = yyjson_get_uint(yyjson_obj_get(root, "generation"));
            saved_score_ = score;
            check(s_->load("m3-slot"), "m3-slot загружается");
            const Play* p = g_.play();
            check(p && p->score() == score && p->moves() == moves && rng && std::to_string(p->rng().state) == rng && g_.generation() == gen,
                  "то же поле: очки " + std::to_string(score) + ", ходов " + std::to_string(moves) + ", генератор и поколение те же" + state());
            check(var("board.score") == static_cast<f64>(score) && text_var("board.level") == "Первый", "значения экрана — из сохранения");
            yyjson_doc_free(doc);
            moves_on_field_ = 0;
            return true;
        }});
        steps_.push_back({"те же два хода", 6000, [this](u32 f) {
            if (!bot(f, false, 2)) return false;
            check(!after_.empty() && g_.play()->board().cells == after_ && g_.play()->score() == after_score_ &&
                      static_cast<int>(g_.play()->end()) == after_end_,
                  "после двух ходов то же поле, те же очки и тот же конец, что без сохранения (" + std::to_string(after_score_) + ")");
            return true;
        }});
        steps_.push_back({"сохранение с фишкой, которой нет", 10, [this](u32) {
            // m3-slot as another slot, its rubies a gem the game has not: «Звезда».
            const fs::path from = s_->slots().folder("m3-slot"), to = s_->slots().folder("m3-gone");
            std::error_code ec;
            fs::remove_all(to, ec);
            fs::copy(from, to, fs::copy_options::recursive, ec);
            std::vector<u8> bytes;
            read_file(to / "board.json", bytes);
            std::string json(bytes.begin(), bytes.end());
            for (usize at = 0; (at = json.find("\"ruby\"", at)) != std::string::npos;) json.replace(at, 6, "\"star\"");
            for (usize at = 0; (at = json.find("\"Рубин\"", at)) != std::string::npos;) json.replace(at, std::strlen("\"Рубин\""), "\"Звезда\"");
            check(!ec && write_file_atomic(to / "board.json", {reinterpret_cast<const u8*>(json.data()), json.size()}), "слот m3-gone со «Звездой»");
            const bool loaded = s_->load("m3-gone");
            check(!loaded && g_.refusal() == "в сохранении фишка «Звезда», её больше нет в игре",
                  "сохранение с фишкой, которой нет, не загружается: «" + g_.refusal() + "»");
            check(!g_.running() && s_->screen() == Screen::Main, "игрок в главном меню, игра не идёт");
            check(g_.problems().empty(), "ошибок данных игры от этого нет");
            const bool again = s_->load("m3-slot");
            check(again && g_.play() && g_.play()->score() == saved_score_, "своё сохранение после отказа загружается" + state());
            return true;
        }});
    }

    void make_scene() {
        steps_.push_back({"тестовая игра делается заново байт в байт", 5, [this](u32) {
            const fs::path data = s_->game_dir().parent_path();
            const fs::path out = fs::temp_directory_path() / utf8_path("forge_match3_тестовая");
            std::string why;
            check(make_test_game(utf8_path(MATCH3_GAMES_DIR), out, why), "игра сделана: " + why);
            const auto made = files_of(out), kept = files_of(data);
            std::string diff;
            for (const auto& [name, bytes] : made)
                if (!kept.count(name) || kept.at(name) != bytes) diff += " " + name;
            for (const auto& [name, bytes] : kept)
                if (!made.count(name)) diff += " -" + name;
            check(diff.empty() && !made.empty(), "файлов " + std::to_string(made.size()) + ", все как в tests/data/match3" + diff);
            std::error_code ec;
            fs::remove_all(out, ec);
            return true;
        }});
    }

    std::string text_var(const char* name) const { return text(name); }
    const logic::Logic& links() {
        if (!links_loaded_) {
            links_.load(s_->game_dir() / "logic.json");
            links_loaded_ = true;
        }
        return links_;
    }

    Match3Game& g_;
    std::string scene_;
    Shell* s_ = nullptr;
    int* failures_ = nullptr;
    std::vector<Step> steps_;
    usize at_ = 0;
    u32 in_step_ = 0;
    std::vector<u8> px_;
    u32 pw_ = 0, ph_ = 0;
    std::map<std::pair<u8, u32>, Rgb> colours_;
    // The bot.
    u32 moves_ = 0, moves_on_field_ = 0;
    bool moved_ = false;
    i32 move_a_ = -1, move_b_ = -1;
    bool seen_swap_ = false, seen_vanish_ = false, seen_fall_ = false;
    u32 between_[3] = {0, 0, 0};
    u32 fades_ = 0, frames_ = 0, hidden_ = 0, in_hole_ = 0;
    // What steps hand on.
    u64 gen_ = 0;
    usize said_ = 0;
    u32 stale_ = 0;
    f64 progress_ = 0;
    std::vector<u8> start_, after_;
    u64 after_score_ = 0, saved_score_ = 0;
    int after_end_ = -1;
    Play::Saved saved_;
    logic::Logic links_;
    bool links_loaded_ = false;
};

int run(int argc, char** argv) {
    Options options;
    std::string scene = "match3";
    Match3Game::Break broken = Match3Game::Break::None;
    for (int i = 1; i < argc; ++i) {
        const bool value = i + 1 < argc;
        if (std::strcmp(argv[i], "--scene") == 0 && value) scene = argv[++i];
        else if (std::strcmp(argv[i], "--level") == 0 && value) options.level_dir = utf8_path(argv[++i]);
        else if (std::strcmp(argv[i], "--test") == 0) options.silent = true;
        else if (std::strcmp(argv[i], "--make-test-game") == 0 && value) {
            std::string why;
            if (!make_test_game(utf8_path(MATCH3_GAMES_DIR), utf8_path(argv[++i]), why)) {
                FORGE_ERROR("Тестовая игра «Трёх в ряд» не сделана: %s", why.c_str());
                return 1;
            }
            FORGE_INFO("Тестовая игра «Трёх в ряд» сделана в %s", argv[i]);
            return 0;
        } else if (std::strcmp(argv[i], "--break") == 0 && value) {
            const std::string b = argv[++i];
            broken = b == "stale-call" ? Match3Game::Break::StaleCall : b == "reward-twice" ? Match3Game::Break::RewardTwice
                     : b == "no-seed-saved"                           ? Match3Game::Break::NoSeedSaved
                                                                       : Match3Game::Break::None;
        }
    }
    Match3Game game(options);
    game.set_break(broken);
    SelfTest test(game, scene);
    game::GameMain m;
    m.dev_ui_dir = FORGE_UI_DIR;
    m.dev_game_dir = MATCH3_GAME_DIR;
    m.module = kModuleId;
    m.test = [&](Shell& shell, u32 frame, int& failures) { return test.frame(shell, frame, failures); };
    return game::run_game(game, m, argc, argv);
}

} // namespace

modules::ModuleDef module_def() {
    modules::ModuleDef m;
    m.id = kModuleId;
    m.name = "Три в ряд";
    m.files = utf8_path(MATCH3_MODULE_DIR);
    m.events = events();
    m.values = values;
    m.run = run;
    m.make_level_module = [] { return std::make_unique<Match3Level>(); };
    return m;
}

} // namespace match3
