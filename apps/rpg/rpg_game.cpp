#include "rpg_game.h"

#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"

#include <yyjson.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rpg {

namespace fs = std::filesystem;
using namespace forge;

const std::vector<modules::Event>& events() {
    static const std::vector<modules::Event> list = {{"pick", "герой подбирает предмет"}, {"reach", "герой доходит до места"}};
    return list;
}

std::vector<std::string> event_ids() {
    std::vector<std::string> out;
    for (const modules::Event& e : events()) out.push_back(e.id);
    return out;
}

std::vector<modules::Value> values(const objects::Library* library) {
    std::vector<modules::Value> out = {{"rpg.map", "Карта"}, {"rpg.near", "Рядом житель (1 — можно поговорить)"}};
    if (!library) return out;
    for (const objects::Template& t : library->templates()) {
        const std::string what = library->item_of(t);
        if (what.empty()) continue;
        const std::string var = "inv." + what;
        if (std::none_of(out.begin(), out.end(), [&](const modules::Value& v) { return v.var == var; }))
            out.push_back({var, "В инвентаре: " + t.name});
    }
    return out;
}

const std::vector<input::ActionDef>& actions() {
    static const std::vector<input::ActionDef> defs = {
        {"up", "Вверх", {"key:W", "key:Up", "pad:dpup", "pad:lefty-"}},
        {"down", "Вниз", {"key:S", "key:Down", "pad:dpdown", "pad:lefty+"}},
        {"left", "Влево", {"key:A", "key:Left", "pad:dpleft", "pad:leftx-"}},
        {"right", "Вправо", {"key:D", "key:Right", "pad:dpright", "pad:leftx+"}},
        {"talk", "Поговорить", {"key:E", "pad:west"}},
    };
    return defs;
}

namespace {

const char* const kSaveFile = "rpg.json";
constexpr u64 kSaveVersion = 1;

bool mirrored(Dir d) { return d == Dir::W || d == Dir::NW || d == Dir::SW; }

} // namespace

RpgGame::RpgGame(const Options& options) : options_(options) {}
RpgGame::~RpgGame() = default;

bool RpgGame::init(game::Shell& shell, SDL_GPUDevice* device, SDL_GPUTextureFormat format) {
    shell_ = &shell;
    device_ = device;
    const fs::path game = shell.game_dir();
    std::string error;
    if (!module_.load_objects(game, &error)) problems_.push_back("объекты: " + error);
    if (!verbs_.load(game / "verbs.json", &error, event_ids())) problems_.push_back("действия (verbs.json): " + error);
    if (!links_.load(game / "logic.json", &error)) problems_.push_back("связи (logic.json): " + error);
    levels_ = level::read_levels(game);
    if (levels_.broken) problems_.push_back("уровни (levels.json): " + levels_.problem);
    for (const std::string& p : problems_) FORGE_ERROR("RPG: %s", p.c_str());
    if (!painter_.init(device, format)) FORGE_ERROR("RPG: карта не рисуется (спрайты не начались)");
    painter_.update(module_.library_ref());
    const f64 zoom = game::read_game_info(game).zoom;
    zoom_ = zoom > 0 ? static_cast<f32>(std::clamp(zoom, 8.0, 256.0)) : kZoom;
    // Sound: the screens' music and buttons.
    if (!options_.silent && !mixer_.open()) FORGE_WARN("Звука нет: не открылось устройство вывода");
    screen_sounds_.attach(&mixer_, module_.library_ref().sounds_folder());
    shell.screens().on_music = [this](const std::string& name) { screen_sounds_.music(name); };
    shell.screens().on_sound = [this](const std::string& name) { screen_sounds_.click(name); };

    // The inventory lives in variables (inv.key...): dialogues check and change it, saves carry it.
    using game::Value;
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
    // The end a dialogue asks for comes when the dialogue is over: the player reads its last line first.
    shell.define("win", [this](std::string_view, const std::vector<Value>&) {
        if (outcome_ == Outcome::Playing && !pending_end_) pending_end_ = true;
        return Value(true);
    });
    shell.define("lose", [this](std::string_view, const std::vector<Value>&) {
        if (outcome_ == Outcome::Playing && !pending_end_) pending_end_ = false;
        return Value(true);
    });

    // What the screens' lists call the things the hero carries: each «Предмет» template's, in their order.
    std::vector<game::ScreenItem> items;
    for (const objects::Template& t : module_.library_ref().templates()) {
        const std::string what = module_.library_ref().item_of(t);
        if (what.empty() || std::any_of(items.begin(), items.end(), [&](const game::ScreenItem& i) { return i.id == what; })) continue;
        items.push_back({what, t.name, module_.library_ref().picture_in(t, game), t.about});
    }
    shell.screens().set_items(std::move(items));
    set_values();
    return true;
}

void RpgGame::shutdown() {
    map_.reset();
    painter_.shutdown();
    screen_sounds_.detach();
    mixer_.close();
    running_ = false;
}

// --- the inventory ----------------------------------------------------------------------------------------------------

f64 RpgGame::inv(const std::string& item) const { return shell_->vars().get("inv." + item).number(); }

void RpgGame::give(const std::string& item, f64 n, bool tell) {
    if (item.empty() || !(n > 0)) return;
    shell_->vars().set("inv." + item, inv(item) + n);
    if (!tell) return;
    std::string name = item;
    for (const objects::Template& t : module_.library_ref().templates())
        if (module_.library_ref().item_of(t) == item) {
            name = t.name;
            break;
        }
    say(n == 1 ? "Получено: " + name : "Получено: " + name + " ×" + std::to_string(static_cast<i64>(n)));
}

bool RpgGame::take(const std::string& item, f64 n) {
    if (item.empty() || !(n > 0) || inv(item) < n) return false;
    shell_->vars().set("inv." + item, inv(item) - n);
    return true;
}

// --- maps -------------------------------------------------------------------------------------------------------------

bool RpgGame::read_level(const std::string& id, const fs::path& folder, MapData& out, std::string& name, std::string& why) {
    const level::LevelEntry* entry = levels_.find(id);
    name = entry ? entry->name : id.empty() ? path_to_utf8(folder.filename()) : id;
    level::Level lv(module_);
    if (!lv.open(folder, &why)) {
        why = "карта «" + name + "» не открылась: " + why;
        return false;
    }
    return read_map(lv, module_.library_ref(), name, out, why);
}

bool RpgGame::start_level(const std::string& id, const fs::path& folder, std::string& why) {
    MapData m;
    std::string name;
    if (!read_level(id, folder, m, name, why)) return false;
    map_ = std::move(m);
    ++map_gen_;
    level_ = id;
    level_name_ = name;
    folder_ = folder;
    hero_x_ = map_->hero.x;
    hero_y_ = map_->hero.y;
    facing_ = Dir::S;
    walking_ = false;
    walk_ticks_ = stand_ticks_ = 0;
    gone_.clear();
    inside_.clear();
    // Places the hero starts in are entered already: no «доходит до» for standing still.
    const f64 fx = hero_x_, fy = hero_y_ - kHeroFeet.depth / 2;
    for (const Thing& p : map_->places) {
        const Box b = place_box(p);
        if (fx >= b.x0 && fx < b.x1 && fy >= b.y0 && fy < b.y1) inside_.push_back(p.id);
    }
    outcome_ = Outcome::Playing;
    pending_end_.reset();
    shell_->vars().set("game.result", std::string());
    FORGE_INFO("RPG: карта «%s» (%s), %d × %d клеток, жителей %zu, предметов %zu, мест %zu", name.c_str(), id.c_str(),
               map_->map.w(), map_->map.h(), map_->villagers.size(), map_->pickups.size(), map_->places.size());
    set_values();
    return true;
}

usize RpgGame::level_index() const {
    for (usize i = 0; i < levels_.levels.size(); ++i)
        if (levels_.levels[i].id == level_) return i;
    return levels_.levels.size();
}

bool RpgGame::begin(const fs::path& session, bool new_game, std::string* error) {
    std::string why;
    refusal_.clear();
    ends_ = 0;
    map_.reset();
    said_.clear();
    fired_.clear();
    toasts_.clear();
    talked_.clear();
    running_ = false;
    auto refuse = [&](const std::string& w) {
        refusal_ = w;
        if (error) *error = w;
        FORGE_WARN("RPG: %s", w.c_str());
        map_.reset();
        return false;
    };
    if (new_game) {
        std::string id = levels_.start;
        fs::path folder = level::level_folder(shell_->game_dir(), id);
        if (!options_.level_dir.empty()) {
            const level::LevelEntry* e = level::level_of_folder(shell_->game_dir(), levels_, options_.level_dir);
            id = e ? e->id : std::string();
            folder = options_.level_dir;
        }
        if (!start_level(id, folder, why)) {
            problems_.push_back(why);
            return refuse(why);
        }
        // Every thing the hero may carry starts at none, so the screens show 0, not an empty place.
        for (const objects::Template& t : module_.library_ref().templates()) {
            const std::string what = module_.library_ref().item_of(t);
            if (!what.empty() && !shell_->vars().has("inv." + what)) shell_->vars().set("inv." + what, 0.0);
        }
        running_ = true;
        set_values();
        return true;
    }
    // A save: the map, where the hero stands, what is gone from it.
    std::vector<u8> text;
    if (!read_file(session / kSaveFile, text)) return refuse("в сохранении нет карты (rpg.json)");
    yyjson_doc* doc = yyjson_read(reinterpret_cast<const char*>(text.data()), text.size(), 0);
    yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
    auto str = [&](const char* key) {
        yyjson_val* v = yyjson_obj_get(root, key);
        return v && yyjson_is_str(v) ? std::string(yyjson_get_str(v), yyjson_get_len(v)) : std::string();
    };
    auto strings = [&](const char* key, std::vector<std::string>& out) {
        yyjson_val* a = yyjson_obj_get(root, key);
        if (!yyjson_is_arr(a)) return false;
        for (usize i = 0; i < yyjson_arr_size(a); ++i) {
            yyjson_val* v = yyjson_arr_get(a, i);
            if (!yyjson_is_str(v)) return false;
            out.emplace_back(yyjson_get_str(v), yyjson_get_len(v));
        }
        return true;
    };
    yyjson_val* xv = yyjson_obj_get(root, "x");
    yyjson_val* yv = yyjson_obj_get(root, "y");
    const f64 x = yyjson_is_num(xv) ? yyjson_get_num(xv) : NAN, y = yyjson_is_num(yv) ? yyjson_get_num(yv) : NAN;
    const std::string id = str("level"), face = str("facing"), outcome = str("outcome");
    std::vector<std::string> gone, inside;
    Dir dir = Dir::S;
    if (!root || !yyjson_is_obj(root)) why = "сохранение не читается (rpg.json)";
    else if (yyjson_get_uint(yyjson_obj_get(root, "version")) != kSaveVersion)
        why = "сохранение другой версии (rpg.json, версия " + std::to_string(yyjson_get_uint(yyjson_obj_get(root, "version"))) + ")";
    else if (!levels_.find(id)) why = "карты «" + id + "» из сохранения в игре нет";
    else if (!std::isfinite(x) || !std::isfinite(y)) why = "место героя в сохранении не читается";
    else if (!dir_of(face, dir)) why = "куда смотрит герой, в сохранении не читается";
    else if (!strings("gone", gone) || !strings("inside", inside)) why = "что подобрано, в сохранении не читается";
    else if (outcome != "playing" && outcome != "level_passed") why = "состояние игры в сохранении не читается";
    yyjson_doc_free(doc);
    if (!why.empty()) return refuse(why);
    if (!start_level(id, level::level_folder(shell_->game_dir(), id), why)) return refuse(why);
    // What the save says, of what the map still has; what it has no more is left out (the author changed the map).
    gone_.clear();
    for (const std::string& g : gone) {
        if (std::any_of(map_->pickups.begin(), map_->pickups.end(), [&](const Thing& p) { return p.id == g; })) gone_.push_back(g);
        else FORGE_WARN("RPG: подобранного «%s» на карте «%s» больше нет", g.c_str(), level_name_.c_str());
    }
    inside_.clear();
    for (const std::string& p : inside)
        if (std::any_of(map_->places.begin(), map_->places.end(), [&](const Thing& t) { return t.id == p; })) inside_.push_back(p);
    facing_ = dir;
    if (fits(map_->map, bodies(), x, y, kHeroFeet)) {
        hero_x_ = x;
        hero_y_ = y;
    } else
        FORGE_WARN("RPG: место героя из сохранения (%.2f, %.2f) на карте «%s» теперь занято, он на её начале", x, y, level_name_.c_str());
    running_ = true;
    // A map passed and saved with its window up: the window again, its links not run again.
    if (outcome == "level_passed") {
        outcome_ = Outcome::LevelPassed;
        if (shell_->screens().show_ending("level") == 0) say("Карта пройдена!");
    }
    set_values();
    FORGE_INFO("RPG: продолжается карта «%s», герой в (%.2f, %.2f), подобрано %zu", level_name_.c_str(), hero_x_, hero_y_, gone_.size());
    return true;
}

bool RpgGame::save(const fs::path& session, std::string& location, std::string* error) {
    if (!map_) {
        if (error) *error = "карты нет";
        return false;
    }
    location = level_name_;
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_uint(doc, root, "version", kSaveVersion);
    yyjson_mut_obj_add_strcpy(doc, root, "level", level_.c_str());
    yyjson_mut_obj_add_real(doc, root, "x", hero_x_);
    yyjson_mut_obj_add_real(doc, root, "y", hero_y_);
    yyjson_mut_obj_add_str(doc, root, "facing", dir_id(facing_));
    yyjson_mut_val* gone = yyjson_mut_arr(doc);
    if (break_ != Break::GoneNotSaved)
        for (const std::string& g : gone_) yyjson_mut_arr_add_strcpy(doc, gone, g.c_str());
    yyjson_mut_obj_add_val(doc, root, "gone", gone);
    yyjson_mut_val* inside = yyjson_mut_arr(doc);
    for (const std::string& p : inside_) yyjson_mut_arr_add_strcpy(doc, inside, p.c_str());
    yyjson_mut_obj_add_val(doc, root, "inside", inside);
    yyjson_mut_obj_add_str(doc, root, "outcome", outcome_ == Outcome::LevelPassed ? "level_passed" : "playing");
    usize len = 0;
    char* json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, &len);
    yyjson_mut_doc_free(doc);
    const bool ok = json && write_file_atomic(session / kSaveFile, {reinterpret_cast<const u8*>(json), len});
    std::free(json);
    if (!ok && error) *error = "rpg.json не записан";
    return ok;
}

void RpgGame::end() {
    running_ = false;
    map_.reset();
    outcome_ = Outcome::Playing;
    pending_end_.reset();
}

bool RpgGame::can_save(std::string* why) const {
    if (!map_) return false;
    if (outcome_ == Outcome::Won || outcome_ == Outcome::Lost || pending_end_) {
        if (why) *why = outcome_ == Outcome::Lost || (pending_end_ && !*pending_end_) ? "игра проиграна" : "игра пройдена";
        return false;
    }
    return true;
}

bool RpgGame::next_level() {
    if (!running_ || !map_) return false;
    const usize at = level_index();
    if (at + 1 >= levels_.levels.size()) return false;
    std::string why;
    const std::string& id = levels_.levels[at + 1].id;
    if (!start_level(id, level::level_folder(shell_->game_dir(), id), why)) {
        problems_.push_back(why);
        say(why);
        // The next map does not open: the player stays on this one and plays on (and may save), its window is gone.
        if (outcome_ == Outcome::LevelPassed) {
            outcome_ = Outcome::Playing;
            shell_->vars().set("game.result", std::string());
            FORGE_WARN("RPG: следующая карта «%s» не открылась, игра идёт дальше на карте «%s»", id.c_str(), level_name_.c_str());
        }
        return false;
    }
    return true;
}

// --- the hero ---------------------------------------------------------------------------------------------------------

bool RpgGame::gone(std::string_view id) const { return std::find(gone_.begin(), gone_.end(), id) != gone_.end(); }
bool RpgGame::inside(std::string_view id) const { return std::find(inside_.begin(), inside_.end(), id) != inside_.end(); }

std::vector<Box> RpgGame::bodies() const {
    std::vector<Box> out;
    if (!map_) return out;
    for (const Thing& v : map_->villagers) out.push_back(feet_box(v.x, v.y, kHeroFeet));
    return out;
}

i32 RpgGame::near_villager() const {
    if (!map_) return -1;
    std::vector<Spot> spots;
    for (const Thing& v : map_->villagers) spots.push_back({v.x, v.y});
    return nearest(spots, hero_x_, hero_y_, kTalkReach);
}

void RpgGame::walk(f64 dt, bool input) {
    Heading h;
    if (input) {
        const input::Input& in = shell_->input();
        h = heading(in.down("up"), in.down("down"), in.down("left"), in.down("right"));
    }
    if (break_ == Break::DiagonalFast && !h.none()) h = {h.x > 0 ? 1.0 : h.x < 0 ? -1.0 : 0.0, h.y > 0 ? 1.0 : h.y < 0 ? -1.0 : 0.0};
    const f64 step = map_->hero.speed * dt;
    Moved m;
    if (break_ == Break::NoWalls) m = {hero_x_ + h.x * step, hero_y_ + h.y * step};
    else m = move(map_->map, bodies(), hero_x_, hero_y_, h.x * step, h.y * step, kHeroFeet);
    const bool moved = std::hypot(m.x - hero_x_, m.y - hero_y_) > 1e-9;
    last_move_ = m;
    hero_x_ = m.x;
    hero_y_ = m.y;
    facing_ = rpg::facing(h, facing_);
    if (moved != walking_) (moved ? walk_ticks_ : stand_ticks_) = 0;
    walking_ = moved;
    tick_left_ += dt * 60;
    const u64 whole = static_cast<u64>(std::floor(tick_left_ + 1e-9));
    tick_left_ -= static_cast<f64>(whole);
    (walking_ ? walk_ticks_ : stand_ticks_) += whole;
    clock_ticks_ += whole;
}

void RpgGame::touch() {
    const Box feet = feet_box(hero_x_, hero_y_, kHeroFeet);
    std::vector<std::pair<std::string, Thing>> happened;
    for (const Thing& p : map_->pickups) {
        if (gone(p.id) || !overlap(feet, pickup_box(p))) continue;
        gone_.push_back(p.id);
        give(p.what, p.count, false);
        if (break_ == Break::PickTwice) give(p.what, p.count, false);
        play_sound(p.sound);
        say(p.count == 1 ? "Найдено: " + p.tmpl->name : "Найдено: " + p.tmpl->name + " ×" + std::to_string(p.count));
        FORGE_INFO("RPG: подобрано «%s» (%s), inv.%s = %g", p.tmpl->name.c_str(), p.id.c_str(), p.what.c_str(), inv(p.what));
        happened.emplace_back("pick", p);
    }
    // In a place: the middle of the hero's feet in its cell.
    const f64 fx = hero_x_, fy = hero_y_ - kHeroFeet.depth / 2;
    for (const Thing& p : map_->places) {
        const Box b = place_box(p);
        const bool in = fx >= b.x0 && fx < b.x1 && fy >= b.y0 && fy < b.y1;
        if (in == inside(p.id)) continue;
        if (in) {
            inside_.push_back(p.id);
            happened.emplace_back("reach", p);
        } else
            std::erase(inside_, p.id);
    }
    // The links after: a map passed starts the next one, and nothing of the old one happens on it.
    const u64 gen = map_gen_;
    const u32 ends = ends_;
    for (const auto& [kind, thing] : happened) {
        if (outcome_ != Outcome::Playing || map_gen_ != gen || ends_ != ends) break;
        on_event(kind, thing);
    }
}

void RpgGame::play_sound(const std::string& name) {
    if (name.empty()) return;
    auto it = named_.find(name);
    if (it == named_.end()) {
        std::string error;
        audio::ClipPtr clip = audio::load(module_.library_ref().sounds_folder() / utf8_path(name), &error);
        if (!clip) FORGE_WARN("Звук «%s» не играет: %s", name.c_str(), error.c_str());
        it = named_.emplace(name, clip ? audio::sound_of(std::move(clip), 1.0f, 0.04f) : nullptr).first;
    }
    if (!it->second) return;
    sounds_played_.push_back(name);
    mixer_.play(*it->second, {});
}

void RpgGame::talk() {
    if (!running_ || !map_ || outcome_ != Outcome::Playing || shell_->in_dialogue()) return;
    const i32 v = near_villager();
    if (v < 0) return;
    const Thing& t = map_->villagers[static_cast<usize>(v)];
    talked_ = t.id;
    // He turns to the villager.
    facing_ = toward(t.x - hero_x_, t.y - hero_y_, facing_);
    if (t.talk.empty() || !shell_->talk(t.talk)) {
        say(t.tmpl->name + " молчит");
        FORGE_WARN("RPG: житель «%s» молчит: разговора «%s» нет в игре, или ему сейчас нечего сказать", t.tmpl->name.c_str(),
                   t.talk.c_str());
    }
}

void RpgGame::on_event(const std::string& kind, const Thing& b) {
    said_.push_back(kind + ":" + b.tmpl->id);
    // An event belongs to the map it happened on. Once one of its links ends that map (the next one starts at once
    // with no window, or fails to open and this one goes on), its other links are not run: not on the new map, where
    // only its own events count, and not again on this one.
    const u64 gen = map_gen_;
    const u32 ends = ends_;
    for (const logic::Link& link : links_.links) {
        if (outcome_ != Outcome::Playing || map_gen_ != gen || ends_ != ends) break;
        const logic::VerbDef* verb = verbs_.find(link.verb);
        if (!verb || verb->when != kind || link.b != b.tmpl->id) continue;
        // Side A is the hero: «Логика» names it logic::kHero, as every module's hero.
        if (!link.a.empty() && link.a != logic::kHero) continue;
        fired_.push_back(link.id);
        if (verb->action == "level_won") level_won();
        else if (verb->action == "lose") finish(false);
        else FORGE_WARN("RPG: связь %u: действия «%s» у игры нет", link.id, verb->action.c_str());
    }
}

void RpgGame::level_won() {
    if (outcome_ != Outcome::Playing) return;
    if (level_index() + 1 >= levels_.levels.size()) {
        finish(true);
        return;
    }
    outcome_ = Outcome::LevelPassed;
    walking_ = false;
    ++ends_;
    shell_->vars().set("game.result", std::string("карта пройдена"));
    const u32 shown = shell_->screens().show_ending("level");
    FORGE_INFO("RPG: карта «%s» пройдена; окон «Уровень пройден» %u", level_name_.c_str(), shown);
    // No window to go on from: the next map at once.
    if (shown == 0) {
        say("Карта пройдена!");
        next_level();
    }
}

void RpgGame::finish(bool won) {
    if (outcome_ == Outcome::Won || outcome_ == Outcome::Lost) return;
    outcome_ = won ? Outcome::Won : Outcome::Lost;
    walking_ = false;
    ++ends_;
    shell_->vars().set("game.result", std::string(won ? "победа" : "поражение"));
    const u32 shown = shell_->screens().show_ending(won ? "win" : "lose");
    if (shown == 0) say(won ? "Победа!" : "Поражение");
    FORGE_INFO("RPG: %s на карте «%s»; окон %u", won ? "победа" : "поражение", level_name_.c_str(), shown);
}

void RpgGame::say(std::string text) {
    toasts_.push_back(text);
    shell_->toast(std::move(text));
}

void RpgGame::set_values() {
    game::Vars& v = shell_->vars();
    const f64 near = running_ && map_ && outcome_ == Outcome::Playing && !shell_->in_dialogue() && near_villager() >= 0 ? 1 : 0;
    if (!v.has("rpg.near") || v.get("rpg.near").number() != near) v.set("rpg.near", near);
    if (!v.has("rpg.map") || v.get("rpg.map").text() != level_name_) v.set("rpg.map", level_name_);
}

void RpgGame::update(f64 dt, bool playing, bool input) {
    mixer_.set_master(shell_->settings().master_volume);
    mixer_.set_volume(audio::Bus::Sound, shell_->settings().sound_volume);
    mixer_.set_volume(audio::Bus::Music, shell_->settings().music_volume);
    mixer_.set_volume(audio::Bus::Ui, shell_->settings().sound_volume);
    if (!mixer_.has_device()) mixer_.advance(dt);
    if (!running_ || !playing || !map_) return;
    if (outcome_ != Outcome::Playing) {
        walking_ = false;
        set_values();
        return;
    }
    if (pending_end_ && !shell_->in_dialogue()) {
        const bool won = *pending_end_;
        pending_end_.reset();
        finish(won);
        set_values();
        return;
    }
    walk(dt, input);
    touch();
    set_values();
}

void RpgGame::handle_event(const SDL_Event& e) {
    if (shell_->input().presses(e, "talk")) talk();
}

// --- drawing -------------------------------------------------------------------------------------------------------------

void RpgGame::fit_camera(u32 width, u32 height) {
    camera_.zoom = zoom_;
    const Map& m = map_->map;
    auto axis = [](f64 at, f64 from, f64 cells, f64 half) {
        if (cells <= 2 * half) return from + cells * 0.5; // the whole map in sight: in the middle
        return std::clamp(at, from + half, from + cells - half);
    };
    camera_.x = axis(hero_x_, m.x0(), m.w(), width * 0.5 / zoom_);
    camera_.y = axis(hero_y_ - 0.5, m.y0(), m.h(), height * 0.5 / zoom_);
}

void RpgGame::screen_of(f64 x, f64 y, f32& sx, f32& sy) const {
    sx = static_cast<f32>((x - camera_.snapped_x()) * camera_.zoom + width_ * 0.5);
    sy = static_cast<f32>((y - camera_.snapped_y()) * camera_.zoom + height_ * 0.5);
}

void RpgGame::render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, u32 width, u32 height) {
    width_ = width;
    height_ = height;
    const Color bg = module_.background();
    const bool draw = map_ && running_ && painter_.ready();
    if (draw) {
        fit_camera(width, height);
        const objects::Library& lib = module_.library_ref();
        painter_.update(lib);
        painter_.begin(camera_);
        const world::Rect view = camera_.visible_tiles(width, height);
        const MapData& m = *map_;
        painter_.put_tiles(view, [&](u32 layer, i32 x, i32 y) { return layer == kGround ? m.ground_at(x, y) : m.obstacle_at(x, y); });
        const f32 view_h = static_cast<f32>(view.y1 - view.y0);
        auto order = [&](f64 feet) { return render::draw_order(1, static_cast<f32>(feet - view.y0), view_h); };
        auto stand = [&](const objects::Template& t) { return objects::clip_frame(clip_of(lib, t, "stand"), clock_ticks_); };
        for (const Thing& p : m.places)
            if (p.shown) painter_.put_thing(*p.tmpl, lib, stand(*p.tmpl), p.x, p.y, false, order(p.y - 0.9));
        for (const Thing& p : m.pickups)
            if (!gone(p.id)) painter_.put_thing(*p.tmpl, lib, stand(*p.tmpl), p.x, p.y, false, order(p.y));
        for (const Thing& v : m.villagers) painter_.put_thing(*v.tmpl, lib, stand(*v.tmpl), v.x, v.y, v.x > hero_x_, order(v.y));
        const objects::Template& hero = *m.hero.tmpl;
        hero_frame_ = walking_ ? objects::clip_frame(clip_of(lib, hero, "walk"), walk_ticks_)
                               : objects::clip_frame(clip_of(lib, hero, "stand"), stand_ticks_);
        hero_mirrored_ = mirrored(facing_);
        painter_.put_thing(hero, lib, hero_frame_, hero_x_, hero_y_, hero_mirrored_, order(hero_y_));
        painter_.prepare(cmd, camera_, width, height);
    }
    SDL_GPUColorTargetInfo info{};
    info.texture = target;
    info.clear_color = SDL_FColor{bg.r, bg.g, bg.b, 1.0f};
    info.load_op = SDL_GPU_LOADOP_CLEAR;
    info.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &info, 1, nullptr);
    if (draw) painter_.draw(cmd, pass);
    SDL_EndGPURenderPass(pass);
}

} // namespace rpg
