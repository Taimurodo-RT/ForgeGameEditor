#include "rpg_module.h"

#include "project_edits.h"
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
// --scene project --edits FILE (step 14.4b) a game made of the template «RPG сверху» and changed by its author in the
//                       editor's tabs (forge::edits::ProjectEdits, rp_*), played as a player: the cells the author laid,
//                       the elder and the author's villager drawn with the author's picture, the hero walking by the
//                       author's frames, the coins picked up with the author's sound, the HUD's hint as the author
//                       wrote it near the villager, his dialogue giving its reward once (saved then as rpg-author, what
//                       is saved written next to it), the author's place passing the map
// --scene project_continue --edits FILE (another process, the same --user) rpg-author goes on: the hero where he
//                       was, the coins taken and the reward kept (the author's quest variable), the villager without
//                       the answer that gives it, the author's hint; the author's place passes «Деревня», «Дальше»
//                       opens «Поляна», the game goes on there until its chest: the window of victory
// --scene rpg_chain, rpg_chain_window, rpg_next_broken, rpg_next_broken_window
//                       the test game copied and changed before it starts (prepare_copy): a second link «Герой доходит
//                       до Ворот» (two links of one event), without the window «Уровень пройден» (the next map at once)
//                       or with it, «Поляна» that does not open. A map passed ends the event: the rest of its links do not
//                       run on the next map (it goes on until its own chest), and a next map that does not open leaves
//                       the player on the old one, walking and saving

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
    SelfTest(RpgGame& game, std::string scene, fs::path edits) : g_(game), scene_(std::move(scene)), edits_file_(std::move(edits)) {}

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
        // A map passed or the game over on the way (a gate, a chest), with its window or straight to the next map: the
        // walk is over.
        if (f == 0) walk_ends_ = g_.ends();
        if (g_.outcome() != RpgGame::Outcome::Playing || g_.ends() != walk_ends_) {
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
        if (sc == "project") return project_scene();
        if (sc == "project_continue") return project_continue_scene();
        steps_.push_back({"данные тестовой игры", 5, [this](u32) {
            check(g_.problems().empty(), "данные игры читаются: " + (g_.problems().empty() ? std::string("без ошибок") : g_.problems().front()));
            std::vector<std::string> list;
            for (const objects::Template& t : g_.library().templates()) list.push_back(t.id);
            std::sort(list.begin(), list.end());
            std::string ids;
            for (const std::string& id : list) ids += (ids.empty() ? "" : ", ") + id;
            check(ids == "chest, coins, dog, elder, gate, guard, hero_look, key", "шаблоны тестовой игры: " + ids);
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
        else if (sc == "rpg_chain" || sc == "rpg_chain_window") chain_scene(sc == "rpg_chain_window");
        else if (sc == "rpg_next_broken" || sc == "rpg_next_broken_window") next_broken_scene(sc == "rpg_next_broken_window");
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
            check_pixel("герой стоит, кадр 0", thing_pixel("hero_look", g_.hero_x(), g_.hero_y()), picture_colour("hero_look", 0));
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

    // --- the author's game (step 14.4b) ---
    // A layer of a screen's page (#n<id>), and its words without the markup.
    Rml::Element* layer(const std::string& screen, u32 id) const {
        Rml::ElementDocument* doc = s_->screens().document(screen);
        return doc ? doc->GetElementById("n" + std::to_string(id)) : nullptr;
    }
    static std::string words_of(Rml::Element* e) {
        if (!e) return {};
        std::string plain;
        bool tag = false;
        for (char c : std::string(e->GetInnerRML())) {
            if (c == '<') tag = true;
            else if (c == '>') tag = false;
            else if (!tag) plain += c;
        }
        return plain;
    }
    // The map's thing of a template at a cell (null: none).
    const Thing* thing_at(const std::vector<Thing>& things, const std::string& tmpl, i32 cx, i32 cy) const {
        for (const Thing& t : things)
            if (t.tmpl && t.tmpl->id == tmpl && t.cx == cx && t.cy == cy) return &t;
        return nullptr;
    }
    const Thing* first_of(const std::vector<Thing>& things, const std::string& tmpl) const {
        for (const Thing& t : things)
            if (t.tmpl && t.tmpl->id == tmpl) return &t;
        return nullptr;
    }

    // What the author changed (--edits), read: a step of both scenes of the author's game.
    bool read_author() {
        std::string why;
        const bool read = !edits_file_.empty() && edits::read_edits(edits_file_, e_, why);
        check(read && !e_.rp_villager.empty() && e_.rp_villager_at.size() == 2 && e_.rp_place_at.size() == 2 && e_.rp_elder.size() == 3,
              "файл ожиданий " + path_to_utf8(edits_file_) + " читается " + why);
        check(g_.problems().empty(), "данные игры читаются: " + (g_.problems().empty() ? std::string("без ошибок") : g_.problems().front()));
        return read;
    }

    void project_scene() {
        steps_.push_back({"ожидания автора", 5, [this](u32) {
            read_author();
            const objects::Library& lib = g_.library();
            const objects::Template* v = lib.find(std::string_view(e_.rp_villager));
            const objects::PropDef* talk = v ? lib.prop_of(*v, "talk") : nullptr;
            check(v && v->name == e_.rp_villager_name && v->kind == "villager" && talk && lib.value(*v, *talk) == "\"" + e_.rp_talk + "\"",
                  "житель автора «" + e_.rp_villager_name + "» (" + e_.rp_villager + "): говорит «" + e_.rp_talk + "»");
            const objects::Template* place = lib.find(std::string_view(e_.rp_place));
            check(place && place->name == e_.rp_place_name && place->kind == "place", "место автора «" + e_.rp_place_name + "» (" + e_.rp_place + ")");
            const objects::Template* coins = lib.find(std::string_view(e_.rp_coins));
            const objects::PropDef* sound = coins ? lib.prop_of(*coins, "sound") : nullptr;
            check(sound && lib.value(*coins, *sound) == "\"" + e_.rp_sound + "\"" && fs::is_regular_file(lib.sound_file(e_.rp_sound)),
                  "у «" + (coins ? coins->name : e_.rp_coins) + "» звук автора " + e_.rp_sound);
            return true;
        }});
        new_game();
        steps_.push_back({"карта автора", 3, [this](u32) {
            const MapData* m = g_.map();
            check(m && g_.level_id() == e_.level, "игра начинается на карте «" + e_.level_name + "» (" + e_.level + ")" + state());
            if (!m) return true;
            std::string differ;
            for (usize i = 0; i + 3 < e_.cells.size(); i += 4) {
                const i32 x = e_.cells[i + 1], y = e_.cells[i + 2];
                const world::TileId got = e_.cells[i] == 0 ? m->ground_at(x, y) : m->obstacle_at(x, y);
                if (got != static_cast<world::TileId>(e_.cells[i + 3]))
                    differ += " " + std::to_string(x) + ":" + std::to_string(y) + "=" + std::to_string(got);
            }
            check(!e_.cells.empty() && differ.empty(), std::to_string(e_.cells.size() / 4) + " клеток, проложенных автором, на месте" + differ);
            const Thing* v = thing_at(m->villagers, e_.rp_villager, e_.rp_villager_at[0], e_.rp_villager_at[1]);
            const Thing* p = thing_at(m->places, e_.rp_place, e_.rp_place_at[0], e_.rp_place_at[1]);
            check(v && v->talk == e_.rp_talk, "«" + e_.rp_villager_name + "» на своей клетке, его разговор «" + (v ? v->talk : "?") + "»");
            check(p && p->shown, "«" + e_.rp_place_name + "» на своей клетке, видно в игре");
            if (!grab()) return fail("кадр не снят"), true;
            const Rgb want{e_.rp_elder[0], e_.rp_elder[1], e_.rp_elder[2]};
            const Thing* elder = first_of(m->villagers, "elder");
            // The template's own copies keep their talk after the editor saved the map (a copy's talk is its id, not a number).
            check(elder && elder->talk == "elder", "староста на карте, его разговор «" + (elder ? elder->talk : "?") + "»");
            if (elder) check_pixel("староста нарисован картинкой автора", thing_pixel("elder", elder->x, elder->y), want);
            if (v) check_pixel("«" + e_.rp_villager_name + "» — копия старосты, с той же картинкой", thing_pixel(e_.rp_villager.c_str(), v->x, v->y), want);
            shot("rpg-автор-карта");
            return true;
        }});
        // The hero walks by the author's frames: their runs while the right key is held.
        steps_.push_back({"герой идёт кадрами автора", 80, [this](u32 f) {
            if (f == 0) {
                runs_.clear();
                hold(false, false, false, true);
            }
            if (f >= 2 && g_.walking()) {
                const u32 fr = g_.hero_frame();
                if (runs_.empty() || runs_.back().first != fr) runs_.push_back({fr, 0});
                ++runs_.back().second;
            }
            if (f < 70) return false;
            let_go();
            const u32 tick = e_.rp_walk_fps > 0 ? static_cast<u32>(std::lround(60.0 / e_.rp_walk_fps)) : 0;
            std::string seen;
            for (const auto& [fr, n] : runs_) seen += " " + std::to_string(fr) + "×" + std::to_string(n);
            // The first run may be cut by the start; the next ones are whole: frames 1, 2, 1 at fps, the last 1 and the
            // first 1 of the next round one run of two.
            const bool frames = e_.rp_walk == std::vector<int>{1, 2, 1} && runs_.size() >= 4 && runs_[1].first == 2 && runs_[2].first == 1 &&
                                runs_[3].first == 2;
            check(frames && runs_[1].second == tick && runs_[2].second == 2 * tick,
                  "кадры шага 1, 2, 1 по " + std::to_string(tick) + " кадров игры (" + num(e_.rp_walk_fps) + " в секунду):" + seen);
            return true;
        }});
        steps_.push_back({"монеты", 3000, [this](u32 f) {
            const Thing* c = g_.map() ? first_of(g_.map()->pickups, e_.rp_coins) : nullptr;
            if (f == 0) {
                if (!c) return fail("монет на карте нет"), true;
                coins_ = var("inv.coins");
                sounds_ = g_.sounds_played().size();
                coins_at_ = {c->cx, c->cy};
                coins_id_ = c->id;
            }
            if (!walk_to(coins_at_.first, coins_at_.second, f)) return false;
            return true;
        }});
        steps_.push_back({"монеты со звуком автора", 3, [this](u32 f) {
            if (f < 1) return false;
            const std::vector<std::string>& played = g_.sounds_played();
            check(var("inv.coins") > coins_ && played.size() == sounds_ + 1 && played.back() == e_.rp_sound,
                  "монеты подобраны (inv.coins = " + num(var("inv.coins")) + "), прозвучал " + (played.size() > sounds_ ? played.back() : "никакой звук"));
            return true;
        }});
        steps_.push_back({"к жителю автора", 3000, [this](u32 f) {
            return walk_to(e_.rp_villager_at[0] - 1, e_.rp_villager_at[1], f);
        }});
        steps_.push_back({"подсказка автора", 5, [this](u32 f) {
            if (f < 2) return false;
            Rml::Element* hint = layer(e_.rp_hud, e_.rp_hint_node);
            check(var("rpg.near") > 0 && hint && hint->IsVisible(true) && words_of(hint) == e_.rp_hint,
                  "рядом с жителем HUD говорит «" + words_of(hint) + "»" + state());
            coins_ = var("inv.coins");
            return true;
        }});
        talk_step("разговор автора", [this] {
            const game::DialogueRunner& r = s_->dialogue_runner();
            check(s_->in_dialogue() && r.dialogue() && r.dialogue()->id() == e_.rp_talk && r.line().text == e_.rp_talk_line &&
                      r.line().choices.size() == 1 && r.line().choices[0] == e_.rp_choice,
                  "E у «" + e_.rp_villager_name + "»: разговор «" + e_.rp_talk + "», «" + std::string(r.line().text) + "», ответ «" +
                      (r.line().choices.empty() ? std::string() : r.line().choices[0]) + "»");
            check(g_.talked_to().rfind(e_.rp_villager + "@", 0) == 0, "герой говорит с ним: " + g_.talked_to());
            tap(SDLK_1);
        });
        steps_.push_back({"награда", 5, [this](u32 f) {
            if (f < 1) return false;
            check(var("inv.coins") == coins_ + e_.rp_reward && var(e_.rp_once) == 1,
                  "«" + e_.rp_choice + "»: монет +" + num(e_.rp_reward) + " (" + num(var("inv.coins")) + "), " + e_.rp_once + " = 1");
            if (s_->in_dialogue()) tap(SDLK_E);
            if (f < 3) return false;
            check(!s_->in_dialogue(), "разговор кончился");
            coins_ = var("inv.coins");
            return true;
        }});
        talk_step("второй раз", [this] {
            const game::DialogueRunner& r = s_->dialogue_runner();
            check(s_->in_dialogue() && r.dialogue() && r.dialogue()->id() == e_.rp_talk && r.line().choices.empty(),
                  "второй разговор: ответа с наградой больше нет");
            tap(SDLK_E);
        });
        steps_.push_back({"награды нет", 4, [this](u32 f) {
            if (f < 2) return false;
            check(!s_->in_dialogue() && var("inv.coins") == coins_, "монет столько же: " + num(var("inv.coins")));
            return true;
        }});
        // Saved with the reward taken: another process goes on from here (project_continue).
        steps_.push_back({"сохранено после награды", 3, [this](u32 f) {
            if (f < 1) return false;
            std::string why;
            const bool can = g_.can_save(&why);
            check(can && s_->save("rpg-author", "Награда мельника"), "сохранение с наградой в rpg-author " + why);
            char json[512];
            std::snprintf(json, sizeof json, "{\"x\": %.17g, \"y\": %.17g, \"facing\": \"%s\", \"coins\": %g, \"once\": %g, \"gone\": \"%s\"}\n",
                          g_.hero_x(), g_.hero_y(), dir_id(g_.facing()), var("inv.coins"), var(e_.rp_once), coins_id_.c_str());
            check(write_file_atomic(s_->user_folder() / "rpg-author-expect.json", {reinterpret_cast<const u8*>(json), std::strlen(json)}),
                  std::string("ожидания для другого процесса: ") + json);
            return true;
        }});
        steps_.push_back({"к месту автора", 3000, [this](u32 f) { return walk_to(e_.rp_place_at[0], e_.rp_place_at[1], f); }});
        steps_.push_back({"окно «Уровень пройден»", 30, [this](u32) {
            if (!window("level")) return false;
            check(g_.outcome() == RpgGame::Outcome::LevelPassed && !g_.said().empty() && g_.said().back() == "reach:" + e_.rp_place,
                  "«" + e_.rp_place_name + "»: событие reach:" + e_.rp_place + ", связь автора, карта пройдена" + state());
            shot("rpg-автор-место");
            return true;
        }});
    }

    // rpg-author, saved by the scene project with the author's reward taken, goes on in another process: through the
    // author's place to «Поляна» and its chest.
    void project_continue_scene() {
        steps_.push_back({"rpg-author из другого процесса", 30, [this](u32) {
            if (!read_author()) return true;
            std::vector<u8> bytes;
            check(read_file(s_->user_folder() / "rpg-author-expect.json", bytes), "ожидания сцены project есть");
            yyjson_doc* doc = yyjson_read(reinterpret_cast<const char*>(bytes.data()), bytes.size(), 0);
            yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
            const f64 x = yyjson_get_num(yyjson_obj_get(root, "x")), y = yyjson_get_num(yyjson_obj_get(root, "y"));
            const char* face = yyjson_get_str(yyjson_obj_get(root, "facing"));
            const char* gone = yyjson_get_str(yyjson_obj_get(root, "gone"));
            coins_ = yyjson_get_num(yyjson_obj_get(root, "coins"));
            const f64 once = yyjson_get_num(yyjson_obj_get(root, "once"));
            check(s_->load("rpg-author"), "rpg-author загружается");
            check(g_.level_id() == e_.level && g_.outcome() == RpgGame::Outcome::Playing && g_.hero_x() == x && g_.hero_y() == y && face &&
                      std::string(dir_id(g_.facing())) == face,
                  "«" + e_.level_name + "», герой там же (" + num(x) + ", " + num(y) + "), смотрит " + (face ? face : "?") + state());
            check(once == 1 && var(e_.rp_once) == 1 && coins_ > 0 && var("inv.coins") == coins_,
                  "награда автора сохранена: " + e_.rp_once + " = " + num(var(e_.rp_once)) + ", монет " + num(var("inv.coins")) + " из " + num(coins_));
            check(gone && *gone && g_.gone(gone), std::string("подобранные монеты (") + (gone ? gone : "?") + ") на карте не появились");
            yyjson_doc_free(doc);
            return true;
        }});
        steps_.push_back({"подсказка автора после загрузки", 5, [this](u32 f) {
            if (f < 2) return false;
            Rml::Element* hint = layer(e_.rp_hud, e_.rp_hint_node);
            check(var("rpg.near") > 0 && hint && hint->IsVisible(true) && words_of(hint) == e_.rp_hint,
                  "герой у жителя, HUD говорит «" + words_of(hint) + "»" + state());
            return true;
        }});
        talk_step("разговор после загрузки", [this] {
            const game::DialogueRunner& r = s_->dialogue_runner();
            check(s_->in_dialogue() && r.dialogue() && r.dialogue()->id() == e_.rp_talk && r.line().text == e_.rp_talk_line && r.line().choices.empty(),
                  "E у «" + e_.rp_villager_name + "»: «" + std::string(r.line().text) + "», ответа с наградой нет");
            tap(SDLK_E);
        });
        steps_.push_back({"второй награды нет", 4, [this](u32 f) {
            if (f < 2) return false;
            check(!s_->in_dialogue() && var("inv.coins") == coins_ && var(e_.rp_once) == 1, "монет столько же: " + num(var("inv.coins")));
            return true;
        }});
        steps_.push_back({"к месту автора", 3000, [this](u32 f) { return walk_to(e_.rp_place_at[0], e_.rp_place_at[1], f); }});
        steps_.push_back({"окно «Уровень пройден»", 30, [this](u32) {
            if (!window("level")) return false;
            check(g_.outcome() == RpgGame::Outcome::LevelPassed && !g_.said().empty() && g_.said().back() == "reach:" + e_.rp_place && g_.ends() == 1,
                  "«" + e_.rp_place_name + "» после загрузки: связь автора, карта пройдена" + state());
            return true;
        }});
        steps_.push_back({"«Дальше»", 30, [this](u32 f) {
            if (f == 0) check(press("level", "Дальше"), "кнопка «Дальше»");
            return g_.level_id() != e_.level;
        }});
        steps_.push_back({"«Поляна»", 25, [this](u32 f) {
            if (f < 20) return false;
            const MapData* m = g_.map();
            check(m && g_.level_id() == "glade" && text("rpg.map") == "Поляна" && g_.outcome() == RpgGame::Outcome::Playing && !window("level") &&
                      !window("win"),
                  "вторая карта «Поляна»: игра идёт, окон нет" + state());
            check(var("inv.coins") == coins_ && var(e_.rp_once) == 1 && var("inv.treasure") == 0 && text("game.result").empty(),
                  "монеты и " + e_.rp_once + " с героем, сундук не найден, итога нет");
            const Thing* chest = m ? first_of(m->pickups, "chest") : nullptr;
            check(chest != nullptr, "на «Поляне» сундук");
            if (chest) coins_at_ = {chest->cx, chest->cy};
            return true;
        }});
        steps_.push_back({"к сундуку", 3000, [this](u32 f) { return walk_to(coins_at_.first, coins_at_.second, f); }});
        steps_.push_back({"победа", 30, [this](u32) {
            if (!window("win")) return false;
            check(g_.outcome() == RpgGame::Outcome::Won && var("inv.treasure") == 1 && g_.ends() == 2, "сундук «Поляны»: окно победы" + state());
            shot("rpg-автор-победа");
            return true;
        }});
    }

    // Two links of «доходит до Ворот» (1 and 3): the first passes «Деревня», the second is not run on «Поляна».
    void chain_scene(bool window_shown) {
        new_game();
        steps_.push_back({"две связи одного события", 3, [this, window_shown](u32) {
            u32 gate = 0;
            for (const logic::Link& l : links_of_game())
                if (l.verb == "reaches" && l.b == "gate") ++gate;
            check(gate == 2 && s_->screens().endings("level").empty() != window_shown,
                  std::string("у ворот две связи «доходит до», окно «Уровень пройден» ") + (window_shown ? "есть" : "убрано"));
            return true;
        }});
        go("к воротам", 23, 8);
        if (window_shown) {
            steps_.push_back({"окно «Уровень пройден»", 30, [this](u32) {
                if (!window("level")) return false;
                check(g_.outcome() == RpgGame::Outcome::LevelPassed && g_.fired() == std::vector<u32>{1} && g_.ends() == 1,
                      "ворота: связь 1, карта пройдена, связь 3 того же события не сработала" + state());
                return true;
            }});
            steps_.push_back({"«Дальше»", 30, [this](u32 f) {
                if (f == 0) check(press("level", "Дальше"), "кнопка «Дальше»");
                return g_.level_id() == "glade";
            }});
        } else {
            steps_.push_back({"сразу «Поляна»", 30, [this](u32) { return g_.level_id() == "glade"; }});
        }
        steps_.push_back({"на «Поляне» игра идёт", 25, [this](u32 f) {
            if (f < 20) return false;
            check(g_.outcome() == RpgGame::Outcome::Playing && g_.hero_x() == 2.5 && g_.hero_y() == 7.0 && !window("win") && !window("level"),
                  "«Поляна»: игра идёт, герой на своей копии, окон нет" + state());
            check(g_.fired() == std::vector<u32>{1} && g_.ends() == 1 && var("inv.treasure") == 0 && text("game.result").empty(),
                  "сработала только связь 1, конец карты один, сундук не подобран, итога нет (связей сработало " +
                      std::to_string(g_.fired().size()) + ")");
            return true;
        }});
        go("к сундуку", 9, 2);
        steps_.push_back({"победа от сундука", 30, [this](u32) {
            if (!window("win")) return false;
            check(g_.outcome() == RpgGame::Outcome::Won && g_.fired() == std::vector<u32>{1, 2} && g_.ends() == 2 && var("inv.treasure") == 1,
                  "победа — от своего события «Поляны»: сундук, связь 2" + state());
            return true;
        }});
    }

    // «Поляна» does not open: the player stays on «Деревня», walks on and may save.
    void next_broken_scene(bool window_shown) {
        new_game();
        go("к воротам", 23, 8);
        if (window_shown) {
            steps_.push_back({"«Дальше» к карте, которой нет", 30, [this](u32 f) {
                if (f == 0) return false;
                if (!window("level")) return f > 20 ? (fail("окна «Уровень пройден» нет" + state()), true) : false;
                check(press("level", "Дальше"), "кнопка «Дальше»");
                return true;
            }});
        }
        steps_.push_back({"«Поляна» не открылась", 10, [this](u32 f) {
            if (f < 3) return false;
            const bool said = !g_.problems().empty() && g_.problems().back().find("Поляна") != std::string::npos;
            check(g_.level_id() == "village" && g_.outcome() == RpgGame::Outcome::Playing && !window("level") && said &&
                      said_toast(g_.problems().back()),
                  "следующая карта не открылась: игрок на «Деревне», игра идёт, сказано почему: «" +
                      (g_.problems().empty() ? std::string() : g_.problems().back()) + "»" + state());
            check(g_.fired() == std::vector<u32>{1} && g_.ends() == 1 && text("game.result").empty(), "связь 3 того же события не сработала, итога нет");
            start_x_ = g_.hero_x();
            return true;
        }});
        steps_.push_back({"игра продолжается", 20, [this](u32 f) {
            if (f == 0) hold(false, false, true, false);
            if (f < 15) return false;
            let_go();
            std::string why;
            const bool can = g_.can_save(&why);
            check(g_.hero_x() < start_x_ && g_.outcome() == RpgGame::Outcome::Playing, "герой идёт дальше по «Деревне»" + state());
            check(can && s_->save("rpg-broken", "Без поляны"), "сохранить можно " + why);
            start_x_ = g_.hero_x();
            start_y_ = g_.hero_y();
            return true;
        }});
        steps_.push_back({"сохранение загружается", 5, [this](u32 f) {
            if (f < 2) return false;
            check(s_->load("rpg-broken") && g_.level_id() == "village" && g_.outcome() == RpgGame::Outcome::Playing && !window("level") &&
                      g_.hero_x() == start_x_ && g_.hero_y() == start_y_ && text("game.result").empty(),
                  "«Без поляны» загружено: «Деревня», игра идёт, герой где был" + state());
            return true;
        }});
    }
    // The links the game plays (its logic.json).
    std::vector<logic::Link> links_of_game() const {
        logic::Logic links;
        links.load(s_->game_dir() / "logic.json");
        return links.links;
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
    u32 walk_ends_ = 0;
    f64 start_x_ = 0, start_y_ = 0;
    // The author's game (scene project).
    fs::path edits_file_;
    edits::ProjectEdits e_;
    std::vector<std::pair<u32, u32>> runs_; // the hero's frames while he walks: frame, frames of the game it showed
    f64 coins_ = 0;
    usize sounds_ = 0;
    std::pair<i32, i32> coins_at_;
    std::string coins_id_; // the author's coins picked up (project), for the other process
};

// The scenes that play a changed copy of the test game (--scene rpg_chain…): the copy, made from the game the command
// line names, in the temporary folder; empty when the scene plays the game as it is. False: why says.
bool prepare_copy(const std::string& scene, const fs::path& game, fs::path& out, std::string& why) {
    const bool chain = scene == "rpg_chain" || scene == "rpg_chain_window";
    const bool broken = scene == "rpg_next_broken" || scene == "rpg_next_broken_window";
    if (!chain && !broken) return true;
    out = fs::temp_directory_path() / utf8_path("forge_rpg_связи") / utf8_path(scene) / "game";
    std::error_code ec;
    fs::remove_all(out.parent_path(), ec);
    fs::create_directories(out.parent_path(), ec);
    fs::copy(game, out, fs::copy_options::recursive, ec);
    if (ec) {
        why = "игра не скопирована в " + path_to_utf8(out) + ": " + ec.message();
        return false;
    }
    if (!scene.ends_with("_window")) {
        fs::remove(out / "ui" / "level.json", ec);
        fs::remove(out / "ui" / "level.html", ec);
    }
    if (broken) fs::remove_all(out / "levels" / "glade", ec);
    // A second link of the same event: «Герой доходит до Ворот» again.
    logic::Logic links;
    if (!links.load(out / "logic.json", &why)) return false;
    const logic::Link* first = links.find(1);
    if (!first || first->verb != "reaches" || first->b != "gate") {
        why = "в logic.json нет связи 1 «доходит до Ворот»";
        return false;
    }
    logic::Link again = *first;
    again.id = 0;
    if (links.add(again) != 3) {
        why = "вторая связь не получила id 3";
        return false;
    }
    return links.save(out / "logic.json", &why);
}

int run(int argc, char** argv) {
    Options options;
    std::string scene = "rpg";
    RpgGame::Break broken = RpgGame::Break::None;
    fs::path edits;
    for (int i = 1; i < argc; ++i) {
        const bool value = i + 1 < argc;
        if (std::strcmp(argv[i], "--scene") == 0 && value) scene = argv[++i];
        else if (std::strcmp(argv[i], "--edits") == 0 && value) edits = utf8_path(argv[++i]);
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
    // A scene of a changed copy plays the copy: --data names it.
    std::vector<std::string> args(argv, argv + argc);
    {
        fs::path copy;
        std::string why;
        if (!prepare_copy(scene, game::game_data_dir(argc, argv, utf8_path(RPG_GAME_DIR)), copy, why)) {
            FORGE_ERROR("Копия тестовой игры для сцены %s не сделана: %s", scene.c_str(), why.c_str());
            return 1;
        }
        if (!copy.empty()) {
            const auto at = std::find(args.begin(), args.end(), std::string("--data"));
            if (at != args.end() && at + 1 != args.end()) *(at + 1) = path_to_utf8(copy);
            else args.insert(args.end(), {"--data", path_to_utf8(copy)});
            FORGE_INFO("Сцена %s играет копию %s", scene.c_str(), path_to_utf8(copy).c_str());
        }
    }
    std::vector<char*> pointers;
    for (std::string& a : args) pointers.push_back(a.data());
    pointers.push_back(nullptr);
    RpgGame game(options);
    game.set_break(broken);
    SelfTest test(game, scene, edits);
    game::GameMain m;
    m.dev_ui_dir = FORGE_UI_DIR;
    m.dev_game_dir = RPG_GAME_DIR;
    m.module = kModuleId;
    m.test = [&](Shell& shell, u32 frame, int& failures) { return test.frame(shell, frame, failures); };
    return game::run_game(game, m, static_cast<int>(args.size()), pointers.data());
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
    m.templates.push_back({"rpg", make_template});
    return m;
}

} // namespace rpg
