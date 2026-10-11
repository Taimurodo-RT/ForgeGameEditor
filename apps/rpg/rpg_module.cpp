#include "rpg_module.h"

#include "rpg_game.h"
#include "rpg_testgame.h"

#include "forge/assets/image.h"
#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/game/runner.h"
#include "forge/game/screens.h"
#include "forge/render/offscreen.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <SDL3/SDL.h>
#include <yyjson.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <map>

#ifndef FORGE_UI_DIR
#define FORGE_UI_DIR "ui"
#endif
#ifndef RPG_GAME_DIR
#define RPG_GAME_DIR "tests/data/rpg/game"
#endif
#ifndef RPG_MODULE_DIR
#define RPG_MODULE_DIR "games/modules/rpg"
#endif
#ifndef RPG_GAMES_DIR
#define RPG_GAMES_DIR "games"
#endif

namespace rpg {

namespace fs = std::filesystem;
using namespace forge;
using game::Screen;
using game::Shell;

namespace {

// --- the self-test --------------------------------------------------------------------------------------------------
//
// --scene rpg           the bot plays the test game with the keys, as a player: eight ways from the start (each a cell
//                       its way, a diagonal as far as straight, the hero looks and is drawn that way), into the house's
//                       wall and along it, E far from everyone, the coins, the dog who says nothing, the elder's quest
//                       taken (and the hero standing while they talk), the key, the key given back, the gate and the
//                       window «Уровень пройден», «Дальше» to the glade, the chest and the window of victory
// --scene rpg_save      the quest taken and the key picked up, saved as rpg-slot half way back, what is saved written
// --scene rpg_continue  (another process) rpg-slot goes on: the hero where he was, the key gone from the map and in the
//                       inventory, the quest's stage; saves broken three ways refused, one with the hero in a wall
//                       loaded with him at the start; then the guard and «Сдаюсь»: the window of defeat after the talk
// --scene rpg_make      the test game made anew, byte for byte the one in tests/data/rpg

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
Rgb rgb_of(u32 c) { return {static_cast<int>(c & 0xff), static_cast<int>((c >> 8) & 0xff), static_cast<int>((c >> 16) & 0xff)}; }

std::string num(f64 v) {
    char b[64];
    std::snprintf(b, sizeof b, "%.6f", v);
    return b;
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
        // A checkout on Windows may end the lines of text files with CR LF; the maker writes LF.
        if (name.ends_with(".json") || name.ends_with(".html")) std::erase(bytes, u8{'\r'});
        out[name] = std::move(bytes);
    }
    return out;
}

// The eight ways, with the keys that make them.
struct Way {
    Dir dir;
    const char* words;
    bool up, down, left, right;
};
constexpr Way kWays[] = {
    {Dir::S, "вниз", false, true, false, false},       {Dir::SW, "вниз-влево", false, true, true, false},
    {Dir::W, "влево", false, false, true, false},      {Dir::NW, "вверх-влево", true, false, true, false},
    {Dir::N, "вверх", true, false, false, false},      {Dir::NE, "вверх-вправо", true, false, false, true},
    {Dir::E, "вправо", false, false, false, true},     {Dir::SE, "вниз-вправо", false, true, false, true},
};

class SelfTest {
public:
    SelfTest(RpgGame& game, std::string scene) : g_(game), scene_(std::move(scene)) {}

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
        return " [карта «" + g_.level_id() + "», герой (" + num(g_.hero_x()) + ", " + num(g_.hero_y()) + "), смотрит " +
               dir_id(g_.facing()) + ", итог " + std::to_string(static_cast<int>(g_.outcome())) + "]";
    }
    f64 var(const std::string& name) const { return s_->vars().get(name).number(); }
    std::string text(const std::string& name) const { return s_->vars().get(name).text(); }
    bool said_toast(const std::string& words) const {
        return std::find(g_.toasts().begin(), g_.toasts().end(), words) != g_.toasts().end();
    }
    bool window(const char* name) const {
        const std::vector<std::string> up = s_->screens().windows();
        return std::find(up.begin(), up.end(), name) != up.end();
    }
    const game::JournalEntry* journal(const char* quest) const {
        static std::vector<game::JournalEntry> entries;
        entries = s_->quests().journal(s_->vars());
        for (const game::JournalEntry& e : entries)
            if (e.quest && e.quest->id == quest) return &e;
        return nullptr;
    }

    // --- the keys and the mouse, as a player's ---
    void send(const SDL_Event& e) {
        if (!s_->handle_event(e) && s_->screen() == Screen::Playing && !s_->in_dialogue()) g_.handle_event(e);
    }
    void key(SDL_Keycode k, bool down) {
        SDL_Event e{};
        e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        e.key.key = k;
        e.key.scancode = SDL_GetScancodeFromKey(k, nullptr);
        e.key.down = down;
        send(e);
    }
    void tap(SDL_Keycode k) {
        key(k, true);
        key(k, false);
    }
    // Holds the keys of a way (and lets go of the others).
    void hold(bool up, bool down, bool left, bool right) {
        const bool want[4] = {up, down, left, right};
        static constexpr SDL_Keycode keys[4] = {SDLK_W, SDLK_S, SDLK_A, SDLK_D};
        for (int i = 0; i < 4; ++i)
            if (want[i] != held_[i]) {
                key(keys[i], want[i]);
                held_[i] = want[i];
            }
    }
    void let_go() { hold(false, false, false, false); }
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
    // A frame of the screen kept for the report, in the player's folder.
    void shot(const char* name) {
        if (!grab()) return fail(std::string("снимок «") + name + "» не снят");
        const fs::path file = s_->user_folder() / utf8_path(std::string(name) + ".png");
        check(render::write_png(path_to_utf8(file).c_str(), pw_, ph_, px_), "снимок " + path_to_utf8(file));
    }
    Rgb pixel(f64 tx, f64 ty) const {
        f32 sx, sy;
        g_.screen_of(tx, ty, sx, sy);
        const i32 px = static_cast<i32>(std::floor(sx)), py = static_cast<i32>(std::floor(sy));
        if (px < 0 || py < 0 || px >= static_cast<i32>(pw_) || py >= static_cast<i32>(ph_)) return {-1, -1, -1};
        const usize i = (static_cast<usize>(py) * pw_ + static_cast<usize>(px)) * 4;
        return {px_[i], px_[i + 1], px_[i + 2]};
    }
    // The middle of a thing's picture standing with its feet at (x, y).
    Rgb thing_pixel(const char* tmpl, f64 x, f64 y) const {
        const objects::Template* t = g_.library().find(std::string_view(tmpl));
        return pixel(x, y - (t ? g_.painter().aspect(*t) : 1.0f) * 0.5);
    }
    // The colour in the middle of a template's picture's frame.
    Rgb picture_colour(const char* tmpl, u32 frame) const {
        const objects::Template* t = g_.library().find(std::string_view(tmpl));
        if (!t) return {-1, -1, -1};
        std::vector<u8> bytes;
        assets::CookedTexture img;
        if (t->picture.empty() || !read_file(s_->game_dir() / "pictures" / utf8_path(t->picture), bytes) || !assets::decode_image(bytes, img))
            return {-1, -1, -1};
        const u32 frames = std::max(1u, t->frames), fw = img.width / frames, f = std::min(frame, frames - 1);
        const u8* p = &img.rgba8[(static_cast<usize>(img.height / 2) * img.width + f * fw + fw / 2) * 4];
        return {p[0], p[1], p[2]};
    }
    void check_pixel(const std::string& what, const Rgb& got, const Rgb& want, int most = 10) {
        check(near(got, want, most), what + ": " + rgb_text(got) + (near(got, want, most) ? "" : " вместо " + rgb_text(want)));
    }

    // --- the bot's way ---
    // Where the bot walks the hero's feet to in a cell: its middle across, a third up from its bottom.
    static f64 feet_x(i32 cx) { return cx + 0.5; }
    static f64 feet_y(i32 cy) { return cy + 0.65; }
    // The cells to walk through to (tx, ty), by the eight ways (a diagonal only past two free cells), round villagers,
    // things not to pick up and places not to come to. Empty: no way.
    std::vector<std::pair<i32, i32>> way_to(i32 tx, i32 ty) const {
        const MapData& m = *g_.map();
        auto blocked = [&](i32 x, i32 y) {
            if (!m.map.free(x, y)) return true;
            if (x == tx && y == ty) return false;
            for (const Thing& v : m.villagers)
                if (v.cx == x && v.cy == y) return true;
            for (const Thing& p : m.pickups)
                if (!g_.gone(p.id) && p.cx == x && p.cy == y) return true;
            for (const Thing& p : m.places)
                if (p.cx == x && p.cy == y) return true;
            return false;
        };
        const i32 sx = cell_x(g_.hero_x()), sy = static_cast<i32>(std::floor(g_.hero_y() - 0.15));
        std::map<std::pair<i32, i32>, std::pair<i32, i32>> from;
        std::deque<std::pair<i32, i32>> open = {{sx, sy}};
        from[{sx, sy}] = {sx, sy};
        while (!open.empty()) {
            const auto [x, y] = open.front();
            open.pop_front();
            if (x == tx && y == ty) break;
            for (i32 dy = -1; dy <= 1; ++dy)
                for (i32 dx = -1; dx <= 1; ++dx) {
                    if ((!dx && !dy) || blocked(x + dx, y + dy) || from.count({x + dx, y + dy})) continue;
                    if (dx && dy && (blocked(x + dx, y) || blocked(x, y + dy))) continue;
                    from[{x + dx, y + dy}] = {x, y};
                    open.push_back({x + dx, y + dy});
                }
        }
        std::vector<std::pair<i32, i32>> path;
        if (!from.count({tx, ty})) return path;
        for (std::pair<i32, i32> c{tx, ty}; c != std::pair{sx, sy}; c = from[c]) path.push_back(c);
        std::reverse(path.begin(), path.end());
        return path;
    }
    // Walks to the cell, a frame at a time; true when there (the keys let go).
    bool walk_to(i32 tx, i32 ty, u32 f) {
        // A map passed or the game over on the way (a gate, a chest): the walk is over.
        if (g_.outcome() != RpgGame::Outcome::Playing) {
            let_go();
            return true;
        }
        if (f == 0) {
            path_ = way_to(tx, ty);
            at_cell_ = 0;
            if (path_.empty() && !(cell_x(g_.hero_x()) == tx && static_cast<i32>(std::floor(g_.hero_y() - 0.15)) == ty)) {
                fail("пути к клетке " + std::to_string(tx) + ":" + std::to_string(ty) + " нет" + state());
                return true;
            }
        }
        while (at_cell_ < path_.size() || at_cell_ == path_.size()) {
            const bool last = at_cell_ >= path_.size();
            const i32 cx = last ? tx : path_[at_cell_].first, cy = last ? ty : path_[at_cell_].second;
            const f64 dx = feet_x(cx) - g_.hero_x(), dy = feet_y(cy) - g_.hero_y();
            constexpr f64 close = 0.07;
            if (std::fabs(dx) <= close && std::fabs(dy) <= close) {
                if (last) {
                    let_go();
                    return true;
                }
                ++at_cell_;
                continue;
            }
            hold(dy < -close, dy > close, dx < -close, dx > close);
            return false;
        }
        return false;
    }

    // --- the scenes ---
    void build(Shell& s) {
        s_ = &s;
        const std::string sc = scene_;
        if (sc == "rpg_make") return make_scene();
        steps_.push_back({"данные тестовой игры", 5, [this](u32) {
            check(g_.problems().empty(), "данные игры читаются: " + (g_.problems().empty() ? std::string("без ошибок") : g_.problems().front()));
            std::vector<std::string> list;
            for (const objects::Template& t : g_.library().templates()) list.push_back(t.id);
            std::sort(list.begin(), list.end());
            std::string ids;
            for (const std::string& id : list) ids += (ids.empty() ? "" : ", ") + id;
            check(ids == "chest, coins, dog, elder, gate, guard, hero, key", "шаблоны тестовой игры: " + ids);
            list.clear();
            for (const modules::Value& v : values(&g_.library())) list.push_back(v.var);
            std::sort(list.begin() + 2, list.end());
            std::string vals;
            for (const std::string& v : list) vals += (vals.empty() ? "" : ", ") + v;
            check(vals == "rpg.map, rpg.near, inv.coins, inv.key, inv.treasure", "значения для экранов: " + vals);
            std::string acts;
            for (const input::ActionDef& a : s_->action_defs()) acts += (acts.empty() ? "" : ", ") + a.id;
            check(acts.starts_with("up, down, left, right, talk, "), "действия игры, потом оболочки: " + acts);
            return true;
        }});
        if (sc == "rpg") main_scene();
        else if (sc == "rpg_save") save_scene();
        else if (sc == "rpg_continue") continue_scene();
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
    // Walks to a cell, as a step.
    void go(const char* name, i32 x, i32 y) {
        steps_.push_back({name, 3000, [this, x, y](u32 f) { return walk_to(x, y, f); }});
    }
    // E as the player presses it, and a frame for it to show.
    void talk_step(const char* name, std::function<void()> then) {
        steps_.push_back({name, 4, [this, then](u32 f) {
            if (f == 0) tap(SDLK_E);
            if (f < 2) return false;
            then();
            return true;
        }});
    }

    void main_scene() {
        new_game();
        steps_.push_back({"«Деревня»", 3, [this](u32) {
            const MapData* m = g_.map();
            check(m && g_.level_id() == "village" && text("rpg.map") == "Деревня", "карта «Деревня» (village) в значении rpg.map" + state());
            check(m && m->villagers.size() == 3 && m->pickups.size() == 2 && m->places.size() == 1 && m->map.w() == 24 && m->map.h() == 14,
                  "на ней 3 жителя, 2 предмета, 1 место, 24 × 14 клеток");
            check(g_.hero_x() == 4.5 && g_.hero_y() == 11.0 && g_.facing() == Dir::S, "герой на своей копии (4.5, 11), смотрит вниз" + state());
            check(var("inv.key") == 0 && s_->vars().has("inv.key") && var("inv.coins") == 0 && var("inv.treasure") == 0,
                  "в инвентаре всего по нулю, и переменные есть");
            check(var("rpg.near") == 0, "рядом жителя нет: rpg.near = 0");
            if (!grab()) return fail("кадр не снят"), true;
            check_pixel("герой стоит, кадр 0", thing_pixel("hero", g_.hero_x(), g_.hero_y()), picture_colour("hero", 0));
            check_pixel("староста", thing_pixel("elder", 7.5, 7.0), picture_colour("elder", 0));
            check_pixel("стражник без картинки — фигура", thing_pixel("guard", 21.5, 8.0), rgb_of(Painter::figure_colour(Thing::Kind::Villager)));
            check_pixel("монеты без картинки — ромб", pixel(9.5, 11.5), rgb_of(Painter::figure_colour(Thing::Kind::Pickup)));
            check_pixel("ключ", thing_pixel("key", 15.5, 3.0), picture_colour("key", 0));
            check_pixel("трава", pixel(2.5, 11.5), rgb_of(Painter::tile_colour(kGround, kGrass)));
            check_pixel("дорога", pixel(12.5, 8.5), rgb_of(Painter::tile_colour(kGround, kPath)));
            check_pixel("вода", pixel(12.5, 1.5), rgb_of(Painter::tile_colour(kGround, kWater)));
            check_pixel("песок", pixel(10.5, 3.5), rgb_of(Painter::tile_colour(kGround, kSand)));
            check_pixel("стена дома", pixel(3.5, 5.5), rgb_of(Painter::tile_colour(kObstacles, kWall)));
            check_pixel("дерево", pixel(23.5, 0.5), rgb_of(Painter::tile_colour(kObstacles, kTree)));
            shot("rpg-деревня");
            return true;
        }});
        eight_ways();
        steps_.push_back({"в стену дома", 120, [this](u32 f) {
            if (f == 0) hold(true, false, false, false);
            if (f < 80) return false;
            let_go();
            check(g_.last_move().hit_y && std::fabs(g_.hero_y() - (7.0 + kHeroFeet.depth)) < 1e-6 && g_.hero_x() == 4.5,
                  "вверх до стены: ступни у её нижнего края (y = 7.3), x тот же" + state());
            return true;
        }});
        steps_.push_back({"вдоль стены", 60, [this](u32 f) {
            if (f == 0) {
                start_x_ = g_.hero_x(), start_y_ = g_.hero_y();
                hold(true, false, false, true);
            }
            if (f < 30) return false;
            let_go();
            const f64 along = 30.0 / 60.0 * 4 * std::sqrt(0.5);
            check(std::fabs(g_.hero_y() - start_y_) < 1e-9 && std::fabs(g_.hero_x() - (start_x_ + along)) < 1e-6 && g_.last_move().hit_y &&
                      !g_.last_move().hit_x,
                  "вверх-вправо у стены: y стоит, x прошёл " + num(along) + " (скорость по диагонали)" + state());
            return true;
        }});
        talk_step("E вдали от жителей", [this] {
            check(!s_->in_dialogue() && g_.talked_to().empty() && g_.toasts().empty() && var("rpg.near") == 0,
                  "E вдали от жителей: разговора нет, никто не молчит" + state());
        });
        // Water is ground the hero does not walk on, as a wall is: under the pond, in the gap of the fences.
        go("к пруду", 13, 4);
        steps_.push_back({"в пруд", 60, [this](u32 f) {
            if (f == 0) {
                start_x_ = g_.hero_x();
                hold(true, false, false, false);
            }
            if (f < 40) return false;
            let_go();
            check(g_.last_move().hit_y && std::fabs(g_.hero_y() - (4.0 + kHeroFeet.depth)) < 1e-6 && g_.hero_x() == start_x_,
                  "вверх к воде: ступни у её нижнего края (y = 4.3), в воду не зашёл" + state());
            return true;
        }});
        go("к монетам", 9, 11);
        steps_.push_back({"монеты подобраны", 3, [this](u32 f) {
            if (f < 1) return false;
            check(var("inv.coins") == 5 && g_.gone("coins@9,11") && said_toast("Найдено: Монеты ×5"),
                  "монеты: inv.coins = 5, на карте их нет, сообщение «Найдено: Монеты ×5»");
            check(g_.said() == std::vector<std::string>{"pick:coins"} && g_.fired().empty(), "событие pick:coins, связей на него нет");
            return true;
        }});
        go("к псу", 17, 11);
        talk_step("пёс молчит", [this] {
            check(!s_->in_dialogue() && g_.talked_to() == "dog@18,11" && said_toast("Пёс молчит"), "у пса нет разговора: «Пёс молчит»" + state());
        });
        go("к старосте", 7, 7);
        talk_step("разговор со старостой", [this] {
            check(var("rpg.near") == 0, "в разговоре подсказки «поговорить» нет");
            const game::DialogueRunner& r = s_->dialogue_runner();
            check(s_->in_dialogue() && r.dialogue() && r.dialogue()->id() == "elder" && r.node_id() == "hello" && r.line().choices.size() == 2,
                  "E у старосты: его разговор, «hello», два ответа");
            check(g_.talked_to() == "elder@7,6" && g_.facing() == Dir::N, "герой говорит со старостой и смотрит на него" + state());
            shot("rpg-разговор");
            start_x_ = g_.hero_x(), start_y_ = g_.hero_y();
        });
        steps_.push_back({"в разговоре герой стоит", 15, [this](u32 f) {
            if (f == 0) hold(false, true, false, true);
            if (f < 10) return false;
            let_go();
            check(g_.hero_x() == start_x_ && g_.hero_y() == start_y_ && !g_.walking(), "клавиши ходьбы в разговоре героя не двигают" + state());
            return true;
        }});
        steps_.push_back({"задание взято", 5, [this](u32 f) {
            if (f == 0) tap(SDLK_1);
            if (f == 1) {
                check(var("quest.key") == 1 && s_->dialogue_runner().node_id() == "go", "«Найду.»: quest.key = 1, дальше «go»");
                const game::JournalEntry* e = journal("key");
                check(e && e->state == game::QuestState::Active && e->text.starts_with("Найти ключ"),
                      "в журнале задание «Ключ от ворот»: " + (e ? e->text : std::string("нет")));
                tap(SDLK_E); // E goes on, as the talk action
            }
            if (f < 2) return false;
            check(!s_->in_dialogue(), "E закрыл последнюю строку");
            return true;
        }});
        go("к ключу", 15, 2);
        steps_.push_back({"ключ подобран", 3, [this](u32 f) {
            if (f < 1) return false;
            check(var("inv.key") == 1 && g_.gone("key@15,2") && said_toast("Найдено: Ключ"), "ключ: inv.key = 1, на карте его нет, «Найдено: Ключ»");
            if (!grab()) return fail("кадр не снят"), true;
            check(!near(thing_pixel("key", 15.5, 3.0), picture_colour("key", 0), 10), "ключа на карте не видно");
            return true;
        }});
        go("назад к старосте", 7, 7);
        talk_step("ключ отдан", [this] {
            const game::DialogueRunner& r = s_->dialogue_runner();
            check(s_->in_dialogue() && r.node_id() == "give_back", "староста видит ключ: «give_back»");
            check(var("inv.key") == 0 && var("inv.coins") == 35 && var("quest.key") == 2 && said_toast("Получено: Монеты ×30"),
                  "take(\"key\"), give(\"coins\", 30): ключа нет, монет 35, quest.key = 2");
            const game::JournalEntry* e = journal("key");
            check(e && e->state == game::QuestState::Done, "задание выполнено");
            tap(SDLK_SPACE);
        });
        steps_.push_back({"разговор кончился", 3, [this](u32) {
            check(!s_->in_dialogue(), "пробел закрыл последнюю строку");
            return true;
        }});
        go("к воротам", 23, 8);
        steps_.push_back({"окно «Уровень пройден»", 30, [this](u32) {
            if (!window("level")) return false;
            check(g_.outcome() == RpgGame::Outcome::LevelPassed && g_.said().back() == "reach:gate" && g_.fired() == std::vector<u32>{1},
                  "ворота: событие reach:gate, связь 1 «доходит до», карта пройдена" + state());
            std::string why;
            check(g_.can_save(&why), "с окном «Уровень пройден» сохранить можно");
            shot("rpg-ворота");
            return true;
        }});
        steps_.push_back({"«Дальше»", 30, [this](u32 f) {
            if (f == 0) check(press("level", "Дальше"), "кнопка «Дальше»");
            if (g_.level_id() != "glade") return false;
            check(text("rpg.map") == "Поляна" && g_.hero_x() == 2.5 && g_.hero_y() == 7.0 && g_.outcome() == RpgGame::Outcome::Playing,
                  "«Поляна»: герой на своей копии" + state());
            check(!window("level") && var("inv.coins") == 35, "окна нет, монеты с собой");
            return true;
        }});
        steps_.push_back({"кадр «Поляны»", 3, [this](u32 f) {
            if (f < 2) return false;
            shot("rpg-поляна");
            return true;
        }});
        go("к сундуку", 9, 2);
        steps_.push_back({"победа", 30, [this](u32) {
            if (!window("win")) return false;
            check(g_.outcome() == RpgGame::Outcome::Won && var("inv.treasure") == 1 && text("game.result") == "победа",
                  "сундук на последней карте: победа, inv.treasure = 1" + state());
            check(g_.fired() == std::vector<u32>{1, 2} && g_.ends() == 2, "связи 1 и 2, концов карт два");
            std::string why;
            const bool can = g_.can_save(&why);
            check(!can && why == "игра пройдена", "после победы не сохранить: «" + why + "»");
            start_x_ = g_.hero_x();
            shot("rpg-победа");
            return true;
        }});
    }

    // Each of the eight ways a cell from the start and back: as far on a diagonal as straight, in the same time; the
    // hero looks and is drawn that way, his steps' frames while he walks, frame 0 when he stands.
    void eight_ways() {
        for (const Way& w : kWays) {
            steps_.push_back({w.words, 40, [this, w](u32 f) {
                if (f == 0) {
                    start_x_ = g_.hero_x(), start_y_ = g_.hero_y();
                    hold(w.up, w.down, w.left, w.right);
                }
                if (f < 15) return false;
                if (f == 15) {
                    const f64 dx = g_.hero_x() - start_x_, dy = g_.hero_y() - start_y_;
                    const Heading h = heading(w.up, w.down, w.left, w.right);
                    check(std::fabs(std::hypot(dx, dy) - 1.0) < 1e-6 && std::fabs(dx - h.x) < 1e-6 && std::fabs(dy - h.y) < 1e-6,
                          std::string(w.words) + ": за 15 кадров клетка в свою сторону (" + num(dx) + ", " + num(dy) + ")");
                    const bool left = w.dir == Dir::W || w.dir == Dir::NW || w.dir == Dir::SW;
                    check(g_.facing() == w.dir && g_.walking() && (g_.hero_frame() == 1 || g_.hero_frame() == 2) && g_.hero_mirrored() == left,
                          std::string(w.words) + ": смотрит " + dir_id(w.dir) + ", кадр шага " + std::to_string(g_.hero_frame()) +
                              (left ? ", отражён" : ", не отражён"));
                    // Back the other way.
                    hold(w.down, w.up, w.right, w.left);
                    return false;
                }
                if (f < 30) return false;
                let_go();
                if (f < 32) return false;
                check(std::fabs(g_.hero_x() - start_x_) < 1e-9 && std::fabs(g_.hero_y() - start_y_) < 1e-9 && !g_.walking() && g_.hero_frame() == 0,
                      std::string(w.words) + ": назад на место, стоит, кадр 0" + state());
                return true;
            }});
        }
    }

    // The quest taken and the key picked up, saved half way back.
    void save_scene() {
        new_game();
        go("к старосте", 7, 7);
        talk_step("разговор", [this] { check(s_->in_dialogue(), "разговор со старостой"); });
        steps_.push_back({"задание взято", 4, [this](u32 f) {
            if (f == 0) tap(SDLK_1);
            if (f == 1) tap(SDLK_E);
            if (f < 2) return false;
            check(var("quest.key") == 1 && !s_->in_dialogue(), "quest.key = 1");
            return true;
        }});
        go("к ключу", 15, 2);
        go("на полпути назад", 12, 6);
        steps_.push_back({"сохранено", 5, [this](u32 f) {
            if (f == 0) hold(false, false, true, false); // a step left: the hero looks left, his feet between cells
            if (f < 4) return false;
            let_go();
            check(var("inv.key") == 1 && s_->save("rpg-slot", "С ключом"), "сохранение с ключом в rpg-slot");
            char json[512];
            std::snprintf(json, sizeof json,
                          "{\"x\": %.17g, \"y\": %.17g, \"facing\": \"%s\", \"key\": %g, \"coins\": %g, \"quest\": %g}\n", g_.hero_x(),
                          g_.hero_y(), dir_id(g_.facing()), var("inv.key"), var("inv.coins"), var("quest.key"));
            check(write_file_atomic(s_->user_folder() / "rpg-expect.json", {reinterpret_cast<const u8*>(json), std::strlen(json)}),
                  std::string("ожидания для другого процесса: ") + json);
            return true;
        }});
    }

    // Copies rpg-slot to another slot with its rpg.json changed by edit; whether it loads, and why not.
    bool load_changed(const char* slot, const std::function<void(std::string&)>& edit) {
        const fs::path from = s_->slots().folder("rpg-slot"), to = s_->slots().folder(slot);
        std::error_code ec;
        fs::remove_all(to, ec);
        fs::copy(from, to, fs::copy_options::recursive, ec);
        std::vector<u8> bytes;
        read_file(to / "rpg.json", bytes);
        std::string json(bytes.begin(), bytes.end());
        edit(json);
        if (ec || !write_file_atomic(to / "rpg.json", {reinterpret_cast<const u8*>(json.data()), json.size()})) fail(std::string("слот ") + slot + " не сделан");
        return s_->load(slot);
    }
    static void replace(std::string& s, const std::string& key, const std::string& value) {
        // "key": <anything up to , or newline> → "key": value
        const usize at = s.find("\"" + key + "\"");
        if (at == std::string::npos) return;
        const usize colon = s.find(':', at), end = s.find_first_of(",\n}", colon);
        s.replace(colon + 1, end - colon - 1, " " + value);
    }

    void continue_scene() {
        steps_.push_back({"rpg-slot из другого процесса", 30, [this](u32) {
            std::vector<u8> text;
            check(read_file(s_->user_folder() / "rpg-expect.json", text), "ожидания rpg_save есть");
            yyjson_doc* doc = yyjson_read(reinterpret_cast<const char*>(text.data()), text.size(), 0);
            yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
            const f64 x = yyjson_get_num(yyjson_obj_get(root, "x")), y = yyjson_get_num(yyjson_obj_get(root, "y"));
            const char* face = yyjson_get_str(yyjson_obj_get(root, "facing"));
            const f64 coins = yyjson_get_num(yyjson_obj_get(root, "coins"));
            check(s_->load("rpg-slot"), "rpg-slot загружается");
            check(g_.hero_x() == x && g_.hero_y() == y && face && std::string(dir_id(g_.facing())) == face,
                  "герой там же (" + num(x) + ", " + num(y) + "), смотрит " + (face ? face : "?") + state());
            check(g_.gone("key@15,2") && var("inv.key") == 1 && var("quest.key") == 1 && var("inv.coins") == coins,
                  "ключа на карте нет, он в инвентаре, задание на той же стадии");
            const game::JournalEntry* e = journal("key");
            check(e && e->state == game::QuestState::Active, "в журнале задание идёт");
            yyjson_doc_free(doc);
            return true;
        }});
        go("туда, где лежал ключ", 15, 2);
        steps_.push_back({"второй раз ключ не подобрать", 3, [this](u32 f) {
            if (f < 1) return false;
            check(var("inv.key") == 1 && g_.said().empty(), "на месте ключа ничего не подобрано, inv.key = 1");
            return true;
        }});
        steps_.push_back({"испорченные сохранения", 10, [this](u32) {
            const auto refused = [&](const char* slot, const std::function<void(std::string&)>& edit, const std::string& words) {
                const bool loaded = load_changed(slot, edit);
                check(!loaded && g_.refusal() == words && !g_.running() && s_->screen() == Screen::Main,
                      std::string(slot) + " не загружается: «" + g_.refusal() + "», игрок в главном меню");
            };
            refused("rpg-x", [](std::string& j) { replace(j, "x", "\"abc\""); }, "место героя в сохранении не читается");
            refused("rpg-version", [](std::string& j) { replace(j, "version", "2"); }, "сохранение другой версии (rpg.json, версия 2)");
            refused("rpg-level", [](std::string& j) { replace(j, "level", "\"nowhere\""); }, "карты «nowhere» из сохранения в игре нет");
            // The hero in the house's wall (the map changed): he starts on the map's start, the rest as saved.
            const bool walled = load_changed("rpg-wall", [](std::string& j) {
                replace(j, "x", "3.5");
                replace(j, "y", "6.0");
            });
            check(walled && g_.hero_x() == 4.5 && g_.hero_y() == 11.0 && var("inv.key") == 1, "герой в стене: на начале карты, ключ с ним" + state());
            check(s_->load("rpg-slot") && g_.gone("key@15,2"), "своё сохранение после отказов загружается");
            return true;
        }});
        go("к стражнику", 20, 7);
        talk_step("разговор со стражником", [this] {
            check(s_->in_dialogue() && s_->dialogue_runner().dialogue() && s_->dialogue_runner().dialogue()->id() == "guard",
                  "E у стражника: его разговор, а не старосты");
            tap(SDLK_2);
        });
        steps_.push_back({"«Сдаюсь»: конец после разговора", 5, [this](u32 f) {
            if (f == 0) {
                std::string why;
                check(s_->in_dialogue() && s_->dialogue_runner().node_id() == "give_up" && !window("lose") && g_.outcome() == RpgGame::Outcome::Playing,
                      "lose() в разговоре: окно ждёт конца разговора");
                const bool can = g_.can_save(&why);
                check(!can && why == "игра проиграна", "пока он идёт, сохранить нельзя: «" + why + "»");
                tap(SDLK_SPACE);
                return false;
            }
            if (!window("lose")) return false;
            check(g_.outcome() == RpgGame::Outcome::Lost && text("game.result") == "поражение" && g_.ends() == 1, "окно «Поражение»" + state());
            return true;
        }});
    }

    void make_scene() {
        steps_.push_back({"тестовая игра делается заново байт в байт", 5, [this](u32) {
            const fs::path data = s_->game_dir().parent_path();
            const fs::path out = fs::temp_directory_path() / utf8_path("forge_rpg_тестовая");
            std::string why;
            check(make_test_game(utf8_path(RPG_GAMES_DIR), out, why), "игра сделана: " + why);
            const auto made = files_of(out), kept = files_of(data);
            std::string diff;
            for (const auto& [name, bytes] : made)
                if (!kept.count(name) || kept.at(name) != bytes) diff += " " + name;
            for (const auto& [name, bytes] : kept)
                if (!made.count(name)) diff += " -" + name;
            check(diff.empty() && !made.empty(), "файлов " + std::to_string(made.size()) + ", все как в tests/data/rpg" + diff);
            std::error_code ec;
            fs::remove_all(out, ec);
            return true;
        }});
    }

    RpgGame& g_;
    std::string scene_;
    Shell* s_ = nullptr;
    int* failures_ = nullptr;
    std::vector<Step> steps_;
    usize at_ = 0;
    u32 in_step_ = 0;
    std::vector<u8> px_;
    u32 pw_ = 0, ph_ = 0;
    bool held_[4] = {false, false, false, false};
    std::vector<std::pair<i32, i32>> path_;
    usize at_cell_ = 0;
    f64 start_x_ = 0, start_y_ = 0;
};

int run(int argc, char** argv) {
    Options options;
    std::string scene = "rpg";
    RpgGame::Break broken = RpgGame::Break::None;
    for (int i = 1; i < argc; ++i) {
        const bool value = i + 1 < argc;
        if (std::strcmp(argv[i], "--scene") == 0 && value) scene = argv[++i];
        else if (std::strcmp(argv[i], "--level") == 0 && value) options.level_dir = utf8_path(argv[++i]);
        else if (std::strcmp(argv[i], "--test") == 0) options.silent = true;
        else if (std::strcmp(argv[i], "--make-test-game") == 0 && value) {
            std::string why;
            if (!make_test_game(utf8_path(RPG_GAMES_DIR), utf8_path(argv[++i]), why)) {
                FORGE_ERROR("Тестовая игра «RPG сверху» не сделана: %s", why.c_str());
                return 1;
            }
            FORGE_INFO("Тестовая игра «RPG сверху» сделана в %s", argv[i]);
            return 0;
        } else if (std::strcmp(argv[i], "--break") == 0 && value) {
            const std::string b = argv[++i];
            broken = b == "diagonal-fast"    ? RpgGame::Break::DiagonalFast
                     : b == "no-walls"       ? RpgGame::Break::NoWalls
                     : b == "gone-not-saved" ? RpgGame::Break::GoneNotSaved
                     : b == "pick-twice"     ? RpgGame::Break::PickTwice
                                             : RpgGame::Break::None;
        }
    }
    RpgGame game(options);
    game.set_break(broken);
    SelfTest test(game, scene);
    game::GameMain m;
    m.dev_ui_dir = FORGE_UI_DIR;
    m.dev_game_dir = RPG_GAME_DIR;
    m.module = kModuleId;
    m.test = [&](Shell& shell, u32 frame, int& failures) { return test.frame(shell, frame, failures); };
    return game::run_game(game, m, argc, argv);
}

} // namespace

modules::ModuleDef module_def() {
    modules::ModuleDef m;
    m.id = kModuleId;
    m.name = "RPG сверху";
    m.files = utf8_path(RPG_MODULE_DIR);
    m.events = events();
    m.actions = actions();
    m.values = values;
    m.run = run;
    m.make_level_module = [] { return std::make_unique<RpgLevel>(); };
    return m;
}

} // namespace rpg
