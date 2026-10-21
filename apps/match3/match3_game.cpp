#include "match3_game.h"

#include "scene_tunes.h"

#include "forge/core/ease.h"
#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/core/time.h"

#include <yyjson.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace match3 {

namespace fs = std::filesystem;
using namespace forge;

const std::vector<modules::Event>& events() {
    // Steps of «Логика» in the game's words (step 14.3c); a level ends only by a link on «цель» or «ходы».
    static const std::vector<modules::Event> list = {
        {"match", "поле собирает фишку", "Когда на поле исчезает ряд {b:gen}", ""},
        {"target", "цель набрана", "Когда очки дошли до цели",
         "Уровень не кончится победой: в «Логике» нет связи «… набирает цель». Добавьте её: щелчок по правилам уровня на доске, потом ещё один по ним же."},
        {"moves_out", "ходы кончились", "Когда ходы кончились, а цель не набрана",
         "Уровень не кончится поражением: в «Логике» нет связи «… остаётся без ходов». Добавьте её: щелчок по правилам уровня на доске, потом ещё один по ним же."}};
    return list;
}

std::vector<std::string> event_ids() {
    std::vector<std::string> out;
    for (const modules::Event& e : events()) out.push_back(e.id);
    return out;
}

std::vector<modules::Value> values(const objects::Library*) {
    return {{"board.score", "Очки"},
            {"board.target", "Цель"},
            {"board.moves", "Ходов осталось"},
            {"board.level", "Уровень"},
            {"board.level_number", "Номер уровня"},
            {"board.reason", "Причина поражения"}};
}

namespace {

const char* const kSaveFile = "board.json";
const char* const kBusy = "Сохранить можно между ходами: фишки ещё падают";

const char* end_word(End e) { return e == End::Won ? "won" : e == End::Lost ? "lost" : "none"; }
End end_of(std::string_view w) { return w == "won" ? End::Won : w == "lost" ? End::Lost : End::None; }

} // namespace

// A field being played: the level it is, its rules and what is on screen.
struct Match3Game::Live {
    u64 generation = 0;
    std::string level; // its id in levels.json
    std::string name;  // «Первый»
    u32 number = 0;    // 1, 2, … in levels.json
    Field field;
    Play play;
    // On screen: the board as it was before what is being shown, and what is shown.
    Board shown;
    Phase phase = Phase::Calm;
    f64 t = 0, duration = 0;
    i32 a = -1, b = -1; // the swap
    Step step;
    // The falls of the step being shown: by cell of play's board, where its gem comes from (rows, may be above the
    // board) and how long it falls.
    std::vector<f64> from_y, fall_time;
    i32 picked = -1;
    u64 seed = 0; // the seed the field was started with (the rules', or the time's for 0)
    std::filesystem::path folder; // the level's
};

Match3Game::Match3Game(const Options& options) : options_(options) {}
Match3Game::~Match3Game() = default;

bool Match3Game::init(game::Shell& shell, SDL_GPUDevice* device, SDL_GPUTextureFormat format) {
    shell_ = &shell;
    device_ = device;
    const fs::path game = shell.game_dir();
    std::string error;
    if (!module_.load_objects(game, &error)) problems_.push_back("объекты: " + error);
    set_ = gem_set(module_.library_ref());
    if (!verbs_.load(game / "verbs.json", &error, event_ids())) problems_.push_back("действия (verbs.json): " + error);
    if (!links_.load(game / "logic.json", &error)) problems_.push_back("связи (logic.json): " + error);
    levels_ = level::read_levels(game);
    if (levels_.broken) problems_.push_back("уровни (levels.json): " + levels_.problem);
    for (const std::string& p : problems_) FORGE_ERROR("Три в ряд: %s", p.c_str());
    if (!painter_.init(device, format)) FORGE_ERROR("Три в ряд: поле не рисуется (спрайты не начались)");
    painter_.update(module_.library_ref(), set_);
    // Sound: the gems' own, the screens' music and buttons.
    if (!options_.silent && !mixer_.open()) FORGE_WARN("Звука нет: не открылось устройство вывода");
    screen_sounds_.attach(&mixer_, module_.library_ref().sounds_folder());
    shell.screens().on_music = [this](const std::string& name) { screen_sounds_.music(name); };
    // Part 6г5б: a screen's scene behind plays its music as the screen's.
    shell.screens().on_scene_music = [this](const std::vector<game::SceneTune>& tunes) { screen_sounds_.screen_tunes(screen_tunes_of(tunes)); };
    shell.screens().on_sound = [this](const std::string& name) { screen_sounds_.click(name); };
    set_values();
    return true;
}

void Match3Game::shutdown() {
    field_.reset();
    painter_.shutdown();
    screen_sounds_.detach();
    mixer_.close();
    running_ = false;
}

// --- levels ------------------------------------------------------------------------------------------------------------

bool Match3Game::read_level(const std::string& id, const fs::path& folder, Field& field, std::string& name, std::string& why) {
    const level::LevelEntry* entry = levels_.find(id);
    name = entry ? entry->name : id.empty() ? path_to_utf8(folder.filename()) : id;
    level::Level lv(module_);
    if (!lv.open(folder, &why)) {
        why = "уровень «" + name + "» не открылся: " + why;
        return false;
    }
    return read_field(lv, module_.library_ref(), set_, name, field, why);
}

bool Match3Game::start_level(const std::string& id, const fs::path& folder, std::string& why) {
    auto live = std::make_unique<Live>();
    if (!read_level(id, folder, live->field, live->name, why)) return false;
    live->folder = folder;
    Rules rules = live->field.rules;
    // Zero: a new field each time (the time), the seed of what is played saved with it.
    if (rules.seed == 0) rules.seed = time_now_ns() | 1;
    if (!live->play.start(live->field.shape, set_.size(), set_.scoring, rules, live->name, why)) return false;
    live->level = id;
    live->seed = rules.seed;
    live->number = 1;
    for (usize i = 0; i < levels_.levels.size(); ++i)
        if (levels_.levels[i].id == id) live->number = static_cast<u32>(i + 1);
    live->generation = ++generations_;
    const u64 gen = live->generation;
    live->play.on_event = [this, gen](const Event& e) { on_board(gen, e); };
    live->shown = live->play.board();
    field_ = std::move(live);
    outcome_ = Outcome::Playing;
    pressed_ = -1;
    held_ = false;
    shell_->vars().set("game.result", std::string());
    shell_->vars().set("board.reason", std::string());
    FORGE_INFO("Три в ряд: уровень «%s» (%s), поле %u, %d × %d, ходов %u, цель %llu, зерно %llu", field_->name.c_str(),
               id.c_str(), static_cast<unsigned>(gen), field_->field.shape.w, field_->field.shape.h, rules.moves,
               static_cast<unsigned long long>(rules.target), static_cast<unsigned long long>(rules.seed));
    set_values();
    return true;
}

const std::string& Match3Game::level_id() const {
    static const std::string none;
    return field_ ? field_->level : none;
}

const Play* Match3Game::play() const { return field_ ? &field_->play : nullptr; }
const Field* Match3Game::field() const { return field_ ? &field_->field : nullptr; }
Match3Game::Phase Match3Game::phase() const { return field_ ? field_->phase : Phase::Calm; }
u64 Match3Game::generation() const { return field_ ? field_->generation : 0; }
u32 Match3Game::level_number() const { return field_ ? field_->number : 0; }
i32 Match3Game::picked() const { return field_ ? field_->picked : -1; }

bool Match3Game::begin(const fs::path& session, bool new_game, std::string* error) {
    std::string why;
    refusal_.clear();
    ends_ = 0;
    field_.reset();
    later_.clear();
    said_.clear();
    fired_.clear();
    clock_ = 0;
    set_ = gem_set(module_.library_ref());
    if (new_game) {
        // The start level, or the one asked for (a level of the game, or a folder of no game: no level after it).
        std::string id = levels_.start;
        fs::path folder = level::level_folder(shell_->game_dir(), id);
        if (!options_.level_dir.empty()) {
            const level::LevelEntry* e = level::level_of_folder(shell_->game_dir(), levels_, options_.level_dir);
            id = e ? e->id : std::string();
            folder = options_.level_dir;
        }
        if (!start_level(id, folder, why)) {
            problems_.push_back(why);
            FORGE_ERROR("Три в ряд: %s", why.c_str());
            refusal_ = why;
            if (error) *error = why;
            return false;
        }
        running_ = true;
        return true;
    }
    // A save: the level, its board and how it stood.
    std::vector<u8> text;
    if (!read_file(session / kSaveFile, text)) {
        refusal_ = "в сохранении нет поля (board.json)";
        if (error) *error = refusal_;
        return false;
    }
    yyjson_doc* doc = yyjson_read(reinterpret_cast<const char*>(text.data()), text.size(), 0);
    yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
    auto str = [&](const char* key) {
        yyjson_val* v = yyjson_obj_get(root, key);
        return v && yyjson_is_str(v) ? std::string(yyjson_get_str(v), yyjson_get_len(v)) : std::string();
    };
    auto num = [&](const char* key) { return yyjson_get_uint(yyjson_obj_get(root, key)); };
    bool ok = root != nullptr;
    auto live = std::make_unique<Live>();
    const std::string id = str("level");
    Play::Saved saved;
    if (ok && !levels_.find(id)) {
        why = "уровня «" + id + "» из сохранения в игре нет";
        ok = false;
    }
    if (ok && !read_level(id, level::level_folder(shell_->game_dir(), id), live->field, live->name, why)) ok = false;
    live->folder = level::level_folder(shell_->game_dir(), id);
    if (ok) {
        yyjson_val* cells = yyjson_obj_get(root, "cells");
        yyjson_val* names = yyjson_obj_get(root, "names");
        const i32 w = static_cast<i32>(num("w")), h = static_cast<i32>(num("h"));
        saved.board = Board(w, h, kHole);
        if (!yyjson_is_arr(cells) || yyjson_arr_size(cells) != static_cast<usize>(w) * static_cast<usize>(h) || w < 1 || h < 1) {
            why = "поле в сохранении не читается";
            ok = false;
        }
        for (usize i = 0; ok && i < yyjson_arr_size(cells); ++i) {
            yyjson_val* c = yyjson_arr_get(cells, i);
            if (yyjson_is_null(c)) continue; // a hole
            const std::string tid = yyjson_is_str(c) ? std::string(yyjson_get_str(c), yyjson_get_len(c)) : std::string();
            const i32 gem = set_.index(tid);
            if (gem < 0) {
                yyjson_val* n = yyjson_obj_get(names, tid.c_str());
                const std::string name = n && yyjson_is_str(n) ? yyjson_get_str(n) : tid;
                why = "в сохранении фишка «" + name + "», её больше нет в игре";
                ok = false;
                break;
            }
            saved.board.cells[i] = static_cast<u8>(gem);
        }
        if (ok) {
            // The level's cells and the save's: the same field.
            Board holes = live->field.shape;
            for (u8& c : holes.cells) c = c == kHole ? kHole : kNone;
            Board saved_holes = saved.board;
            for (u8& c : saved_holes.cells) c = c == kHole ? kHole : kNone;
            if (!(holes == saved_holes)) {
                why = "поле уровня «" + live->name + "» с тех пор изменилось: сохранение к нему не подходит";
                ok = false;
            }
        }
        if (ok) {
            saved.score = num("score");
            saved.moves = static_cast<u32>(num("moves"));
            saved.end = end_of(str("end"));
            saved.rng = num("rng");
            saved.shuffles = static_cast<u32>(num("shuffles"));
            Rules rules = live->field.rules;
            rules.seed = live->seed = num("seed");
            if (!live->play.resume(saved, set_.size(), set_.scoring, rules, why)) ok = false;
        }
    }
    const u64 generation = num("generation");
    const std::string outcome = str("outcome");
    yyjson_doc_free(doc);
    if (!ok) {
        if (why.empty()) why = "поле в сохранении не читается";
        refusal_ = why;
        if (error) *error = why;
        FORGE_WARN("Три в ряд: сохранение не загружено: %s", why.c_str());
        return false;
    }
    live->level = id;
    live->number = 1;
    for (usize i = 0; i < levels_.levels.size(); ++i)
        if (levels_.levels[i].id == id) live->number = static_cast<u32>(i + 1);
    generations_ = std::max(generations_, generation);
    live->generation = generation ? generation : ++generations_;
    const u64 gen = live->generation;
    live->play.on_event = [this, gen](const Event& e) { on_board(gen, e); };
    live->shown = live->play.board();
    field_ = std::move(live);
    outcome_ = Outcome::Playing;
    running_ = true;
    // A level passed and saved with its window up: the window again, its links not run again.
    if (outcome == "level_passed") {
        outcome_ = Outcome::LevelPassed;
        if (shell_->screens().show_ending("level") == 0) shell_->toast("Уровень пройден!");
    }
    set_values();
    FORGE_INFO("Три в ряд: продолжается уровень «%s», поле %llu, очков %llu, ходов %u", field_->name.c_str(),
               static_cast<unsigned long long>(gen), static_cast<unsigned long long>(field_->play.score()), field_->play.moves());
    return true;
}

bool Match3Game::save(const fs::path& session, std::string& location, std::string* error) {
    if (!field_) {
        if (error) *error = "поля нет";
        return false;
    }
    const Play::Saved s = field_->play.saved();
    location = field_->name;
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "level", field_->level.c_str());
    yyjson_mut_obj_add_uint(doc, root, "generation", field_->generation);
    yyjson_mut_obj_add_uint(doc, root, "w", static_cast<u64>(s.board.w));
    yyjson_mut_obj_add_uint(doc, root, "h", static_cast<u64>(s.board.h));
    // By template id, not by number in the set: a gem added or turned off does not move the others.
    yyjson_mut_val* cells = yyjson_mut_arr(doc);
    yyjson_mut_val* names = yyjson_mut_obj(doc);
    for (u8 c : s.board.cells) {
        if (c >= set_.size()) {
            yyjson_mut_arr_add_null(doc, cells);
            continue;
        }
        const objects::Template& t = *set_.gems[c];
        yyjson_mut_arr_add_strcpy(doc, cells, t.id.c_str());
        if (!yyjson_mut_obj_get(names, t.id.c_str())) yyjson_mut_obj_add_strcpy(doc, names, t.id.c_str(), t.name.c_str());
    }
    yyjson_mut_obj_add_val(doc, root, "cells", cells);
    yyjson_mut_obj_add_val(doc, root, "names", names);
    yyjson_mut_obj_add_uint(doc, root, "score", s.score);
    yyjson_mut_obj_add_uint(doc, root, "moves", s.moves);
    yyjson_mut_obj_add_str(doc, root, "end", end_word(s.end));
    yyjson_mut_obj_add_str(doc, root, "outcome", outcome_ == Outcome::LevelPassed ? "level_passed" : "playing");
    yyjson_mut_obj_add_uint(doc, root, "rng", break_ == Break::NoSeedSaved ? 0 : s.rng);
    yyjson_mut_obj_add_uint(doc, root, "seed", field_->seed);
    yyjson_mut_obj_add_uint(doc, root, "shuffles", s.shuffles);
    usize len = 0;
    char* json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, &len);
    yyjson_mut_doc_free(doc);
    const bool ok = json && write_file_atomic(session / kSaveFile, {reinterpret_cast<const u8*>(json), len});
    std::free(json);
    if (!ok && error) *error = "board.json не записан";
    return ok;
}

void Match3Game::end() {
    running_ = false;
    field_.reset();
    later_.clear();
    outcome_ = Outcome::Playing;
}

bool Match3Game::can_save(std::string* why) const {
    if (!field_) return false;
    if (field_->phase != Phase::Calm || field_->play.busy()) {
        if (why) *why = kBusy;
        return false;
    }
    if (outcome_ == Outcome::Won || outcome_ == Outcome::Lost) {
        if (why) *why = outcome_ == Outcome::Won ? "игра пройдена" : "уровень проигран: «Ещё раз» начнёт его заново";
        return false;
    }
    // The board settled, but its end has not been shown yet (the window comes after kEndDelay): wait for it.
    if (field_->play.end() != End::None && outcome_ == Outcome::Playing) {
        if (why) *why = kBusy;
        return false;
    }
    return true;
}

bool Match3Game::next_level() {
    if (!running_ || !field_) return false;
    usize at = levels_.levels.size();
    for (usize i = 0; i < levels_.levels.size(); ++i)
        if (levels_.levels[i].id == field_->level) at = i;
    if (at + 1 >= levels_.levels.size()) return false;
    std::string why;
    const std::string& id = levels_.levels[at + 1].id;
    if (!start_level(id, level::level_folder(shell_->game_dir(), id), why)) {
        problems_.push_back(why);
        shell_->toast(why);
        return false;
    }
    return true;
}

bool Match3Game::retry_level() {
    if (!running_ || !field_) return false;
    std::string why;
    const std::string id = field_->level;
    const fs::path folder = field_->folder;
    if (!start_level(id, folder, why)) {
        problems_.push_back(why);
        shell_->toast(why);
        return false;
    }
    return true;
}

// --- the board's events and the links -------------------------------------------------------------------------------

void Match3Game::on_board(u64 generation, const Event& e) {
    if (!field_ || generation != field_->generation) {
        ++stale_events_;
        return;
    }
    said_.push_back({generation, e});
    const std::string kind = e.kind == Event::Kind::Match      ? "match"
                             : e.kind == Event::Kind::Target   ? "target"
                             : e.kind == Event::Kind::MovesOut ? "moves_out"
                                                               : "";
    if (e.kind == Event::Kind::Match && e.gem < set_.size()) play_sound(set_.sounds[e.gem]);
    if (e.kind == Event::Kind::Shuffled) {
        shell_->toast("Ходов нет — фишки перемешаны");
        field_->shown = field_->play.board();
        return;
    }
    if (e.kind == Event::Kind::NoMoves) {
        later(kEndDelay, [this] { finish(false, "Ходов нет"); });
        return;
    }
    const std::string gem = e.kind == Event::Kind::Match && e.gem < set_.size() ? set_.gems[e.gem]->id : std::string();
    bool any = false;
    for (const logic::Link& link : links_.links) {
        const logic::VerbDef* verb = verbs_.find(link.verb);
        if (!verb || verb->when != kind) continue;
        if (e.kind == Event::Kind::Match && link.b != gem) continue;
        any = true;
        fired_.push_back(link.id);
        act(link, *verb);
        if (break_ == Break::RewardTwice) act(link, *verb);
    }
    if (!any && e.kind != Event::Kind::Match && !said_once_[kind]) {
        said_once_[kind] = true;
        FORGE_WARN("Три в ряд: у события «%s» нет связей: уровень так и стоит", kind.c_str());
    }
}

void Match3Game::act(const logic::Link& link, const logic::VerbDef& verb) {
    const std::string& a = verb.action;
    if (a == "add_moves") field_->play.add_moves(static_cast<i32>(verb.amount));
    else if (a == "add_score") field_->play.add_score(static_cast<i64>(verb.amount));
    else if (a == "level_won") later(kEndDelay, [this] { level_won(); });
    else if (a == "lose") later(kEndDelay, [this] { finish(false, "Ходы кончились"); });
    else if (!said_once_["do:" + a]) {
        said_once_["do:" + a] = true;
        FORGE_WARN("Три в ряд: связь %u: действия «%s» у игры нет", link.id, a.c_str());
    }
    set_values();
}

void Match3Game::later(f64 delay, std::function<void()> fn) {
    later_.push_back({field_ ? field_->generation : 0, clock_ + delay, std::move(fn)});
}

void Match3Game::level_won() {
    if (outcome_ != Outcome::Playing) return; // two links «набирает цель»: the level is passed once
    usize at = levels_.levels.size();
    for (usize i = 0; i < levels_.levels.size(); ++i)
        if (levels_.levels[i].id == field_->level) at = i;
    if (at + 1 >= levels_.levels.size()) {
        finish(true, {});
        return;
    }
    outcome_ = Outcome::LevelPassed;
    ++ends_;
    shell_->vars().set("game.result", std::string("уровень пройден"));
    const u32 shown = shell_->screens().show_ending("level");
    FORGE_INFO("Три в ряд: уровень «%s» пройден, очков %llu; окон «Уровень пройден» %u", field_->name.c_str(),
               static_cast<unsigned long long>(field_->play.score()), shown);
    // No window to go on from: the next level at once.
    if (shown == 0) {
        shell_->toast("Уровень пройден!");
        next_level();
    }
}

void Match3Game::finish(bool won, const std::string& why) {
    if (outcome_ == Outcome::Won || outcome_ == Outcome::Lost) return;
    outcome_ = won ? Outcome::Won : Outcome::Lost;
    ++ends_;
    shell_->vars().set("game.result", std::string(won ? "победа" : "поражение"));
    shell_->vars().set("board.reason", why);
    const u32 shown = shell_->screens().show_ending(won ? "win" : "lose");
    if (shown == 0) shell_->toast(won ? "Победа!" : "Поражение: " + why);
    FORGE_INFO("Три в ряд: %s на уровне «%s», очков %llu%s%s; окон %u", won ? "победа" : "поражение", field_->name.c_str(),
               static_cast<unsigned long long>(field_->play.score()), why.empty() ? "" : ": ", why.c_str(), shown);
}

void Match3Game::play_sound(const std::string& name) {
    if (name.empty()) return;
    auto it = named_.find(name);
    if (it == named_.end()) {
        std::string error;
        audio::ClipPtr clip = audio::load(module_.library_ref().sounds_folder() / utf8_path(name), &error);
        if (!clip) FORGE_WARN("Звук «%s» не играет: %s", name.c_str(), error.c_str());
        it = named_.emplace(name, clip ? audio::sound_of(std::move(clip), 1.0f, 0.04f) : nullptr).first;
    }
    if (!it->second) return;
    ++sounds_played_;
    ++sounds_by_name_[name];
    mixer_.play(*it->second, {});
}

void Match3Game::set_values() {
    game::Vars& v = shell_->vars();
    auto put = [&](const char* name, f64 x) {
        if (!v.has(name) || v.get(name).number() != x) v.set(name, x);
    };
    put("board.score", field_ ? static_cast<f64>(field_->play.score()) : 0);
    put("board.target", field_ ? static_cast<f64>(field_->play.target()) : 0);
    put("board.moves", field_ ? static_cast<f64>(field_->play.moves()) : 0);
    put("board.level_number", field_ ? static_cast<f64>(field_->number) : 0);
    const std::string name = field_ ? field_->name : std::string();
    if (!v.has("board.level") || v.get("board.level").text() != name) v.set("board.level", name);
    if (!v.has("board.reason")) v.set("board.reason", std::string()); // what a level was lost of: finish() says it
}

// --- moves and their animation ----------------------------------------------------------------------------------------

void Match3Game::try_swap(i32 a, i32 b) {
    Live& f = *field_;
    f.picked = -1;
    if (f.phase != Phase::Calm || outcome_ != Outcome::Playing) return;
    const Swap r = f.play.swap(a, b);
    if (r == Swap::Refused) return;
    f.a = a;
    f.b = b;
    f.t = 0;
    f.phase = r == Swap::Done ? Phase::Swap : Phase::SwapBack;
    f.duration = r == Swap::Done ? kSwapTime : 2 * kSwapTime;
    set_values(); // the move is spent at the swap
}

void Match3Game::cascade() {
    Live& f = *field_;
    f.shown = f.play.board();
    Step s;
    if (!f.play.step(s)) { // calm: its end, or a shuffle, was said
        f.phase = Phase::Calm;
        f.shown = f.play.board();
        set_values();
        return;
    }
    f.step = std::move(s);
    f.phase = Phase::Vanish;
    f.t = 0;
    f.duration = kVanishTime;
    // Where each gem of the board after the step comes from, and how long it falls.
    const Board& after = f.play.board();
    f.from_y.assign(after.cells.size(), 0);
    f.fall_time.assign(after.cells.size(), 0);
    for (usize i = 0; i < after.cells.size(); ++i) f.from_y[i] = after.y_of(static_cast<i32>(i));
    for (const Fall& fl : f.step.falls) f.from_y[static_cast<usize>(fl.to)] = after.y_of(fl.from);
    for (const Drop& d : f.step.drops) {
        const i32 x = after.x_of(d.cell);
        i32 top = 0;
        while (top < after.h && !after.cell(x, top)) ++top;
        f.from_y[static_cast<usize>(d.cell)] = top - d.above;
    }
    for (usize i = 0; i < after.cells.size(); ++i)
        f.fall_time[i] = ease::fall_time(after.y_of(static_cast<i32>(i)) - f.from_y[i], kFallPull);
    set_values();
}

void Match3Game::advance(f64 dt) {
    Live* f = field_.get();
    if (!f || f->phase == Phase::Calm) return;
    f->t += dt;
    while (f && f->phase != Phase::Calm && f->t >= f->duration) {
        const f64 over = f->t - f->duration;
        switch (f->phase) {
        case Phase::Swap:
            f->shown = f->play.board();
            cascade();
            break;
        case Phase::SwapBack: f->phase = Phase::Calm; break;
        case Phase::Vanish:
            f->phase = Phase::Fall;
            f->duration = std::max(1.0 / 60.0, *std::max_element(f->fall_time.begin(), f->fall_time.end()));
            break;
        case Phase::Fall: cascade(); break;
        case Phase::Calm: break;
        }
        f = field_.get();
        if (f && f->phase != Phase::Calm) f->t = std::min(over, f->duration);
    }
}

f64 Match3Game::phase_progress() const { return field_ ? ease::progress(field_->t, field_->duration) : 1; }

void Match3Game::update(f64 dt, bool playing, bool) {
    mixer_.set_master(shell_->settings().master_volume);
    mixer_.set_volume(audio::Bus::Sound, shell_->settings().sound_volume);
    mixer_.set_volume(audio::Bus::Music, shell_->settings().music_volume);
    mixer_.set_volume(audio::Bus::Ui, shell_->settings().sound_volume);
    if (!mixer_.has_device()) mixer_.advance(dt);
    if (!running_ || !playing || !field_) return;
    clock_ += dt;
    advance(dt);
    // What an old field left: dropped; this field's whose time came: done.
    for (usize i = 0; i < later_.size();) {
        if (later_[i].at > clock_) {
            ++i;
            continue;
        }
        Later l = std::move(later_[i]);
        later_.erase(later_.begin() + static_cast<std::ptrdiff_t>(i));
        if (field_ && (l.generation == field_->generation || break_ == Break::StaleCall)) l.fn();
        else ++stale_;
    }
    set_values();
}

// --- the mouse --------------------------------------------------------------------------------------------------------

i32 Match3Game::cell_at(f32 sx, f32 sy) const {
    if (!field_ || !width_) return -1;
    f64 tx = 0, ty = 0;
    camera_.screen_to_tile(sx, sy, width_, height_, tx, ty);
    const i32 x = static_cast<i32>(std::floor(tx)) - field_->field.x0, y = static_cast<i32>(std::floor(ty)) - field_->field.y0;
    const Board& b = field_->play.board();
    return b.gem(x, y) ? b.index(x, y) : -1;
}

void Match3Game::handle_event(const SDL_Event& e) {
    if (!running_ || !field_ || outcome_ != Outcome::Playing) return;
    auto pixels = [](const SDL_Event& ev, f32 x, f32 y, f32& px, f32& py) {
        SDL_Window* w = SDL_GetWindowFromID(ev.button.windowID);
        const f32 density = w ? SDL_GetWindowPixelDensity(w) : 1.0f;
        px = x * density;
        py = y * density;
    };
    Live& f = *field_;
    const bool calm = f.phase == Phase::Calm;
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        if (!calm) return; // no input while the board moves
        f32 px, py;
        pixels(e, e.button.x, e.button.y, px, py);
        const i32 cell = cell_at(px, py);
        if (cell < 0) {
            f.picked = -1;
            return;
        }
        // A click on a gem next to the one picked: the swap.
        if (f.picked >= 0 && f.picked != cell && neighbours(f.play.board(), f.picked, cell)) {
            try_swap(f.picked, cell);
            held_ = false;
            return;
        }
        f.picked = f.picked == cell ? -1 : cell;
        pressed_ = cell;
        held_ = f.picked >= 0;
    } else if (e.type == SDL_EVENT_MOUSE_MOTION && held_) {
        if (!calm) return;
        f32 px, py;
        pixels(e, e.motion.x, e.motion.y, px, py);
        const i32 cell = cell_at(px, py);
        // Dragged onto a neighbour: the swap.
        if (cell >= 0 && cell != pressed_ && neighbours(f.play.board(), pressed_, cell)) {
            held_ = false;
            try_swap(pressed_, cell);
        }
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
        held_ = false;
    }
}

bool Match3Game::cell_on_screen(i32 cell, f32& sx, f32& sy) const {
    if (!field_ || !width_) return false;
    const Board& b = field_->play.board();
    if (cell < 0 || cell >= static_cast<i32>(b.cells.size())) return false;
    const f64 tx = field_->field.x0 + b.x_of(cell) + 0.5, ty = field_->field.y0 + b.y_of(cell) + 0.5;
    sx = static_cast<f32>((tx - camera_.snapped_x()) * camera_.zoom + width_ * 0.5);
    sy = static_cast<f32>((ty - camera_.snapped_y()) * camera_.zoom + height_ * 0.5);
    return true;
}

// --- drawing -------------------------------------------------------------------------------------------------------------

void Match3Game::fit_camera(u32 width, u32 height) {
    if (!field_) return;
    const Board& b = field_->field.shape;
    // The field and a margin: a cell and a half above for the screens' line of numbers, half a cell elsewhere.
    const f64 zw = width / (b.w + 1.0), zh = height / (b.h + 2.0);
    camera_.zoom = static_cast<f32>(std::floor(std::min(zw, zh)));
    camera_.x = field_->field.x0 + b.w * 0.5;
    camera_.y = field_->field.y0 + b.h * 0.5 - 0.5;
}

void Match3Game::push_board() {
    Live& f = *field_;
    const Field& fd = f.field;
    painter_.put_cells(fd);
    const Color bg = module_.background();
    painter_.put_covers(fd, render::pack_color(static_cast<u8>(std::lround(bg.r * 255)), static_cast<u8>(std::lround(bg.g * 255)),
                                               static_cast<u8>(std::lround(bg.b * 255)), 255));
    const Board& now = f.play.board();
    drawn_.assign(now.cells.size(), Drawn{});
    const f64 p = ease::progress(f.t, f.duration);
    auto gem_at = [&](i32 i, u8 gem, f64 x, f64 y, f32 alpha, f32 scale, u32 frame_no) {
        Drawn& d = drawn_[static_cast<usize>(i)];
        d.gem = gem;
        d.x = x;
        d.y = y;
        d.alpha = alpha;
        d.scale = scale;
        d.frame = frame_no;
        const u8 a = static_cast<u8>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
        painter_.put(painter_.gem_frame(gem, frame_no), x, y, 0.86f * scale, 0.86f * scale, render::pack_color(255, 255, 255, a), 3);
    };
    const Board& shown = f.phase == Phase::Fall ? now : f.shown;
    for (i32 i = 0; i < static_cast<i32>(shown.cells.size()); ++i) {
        const u8 g = shown.cells[static_cast<usize>(i)];
        if (g == kHole) {
            drawn_[static_cast<usize>(i)].gem = kHole;
            continue;
        }
        if (g == kNone) continue;
        f64 x = fd.x0 + shown.x_of(i) + 0.5, y = fd.y0 + shown.y_of(i) + 0.5;
        f32 alpha = 1, scale = 1;
        u32 frame = 0;
        switch (f.phase) {
        case Phase::Swap:
        case Phase::SwapBack:
            if (i == f.a || i == f.b) {
                // There and, for no row, back: the way is the first half's, then the second's backwards.
                const i32 other = i == f.a ? f.b : f.a;
                f64 k = f.phase == Phase::Swap ? ease::in_out(p) : ease::in_out(p < 0.5 ? p * 2 : (1 - p) * 2);
                x = ease::lerp(x, fd.x0 + shown.x_of(other) + 0.5, k);
                y = ease::lerp(y, fd.y0 + shown.y_of(other) + 0.5, k);
            }
            break;
        case Phase::Vanish:
            for (const Group& grp : f.step.groups)
                if (std::binary_search(grp.cells.begin(), grp.cells.end(), i)) {
                    const u32 frames = painter_.gem_frames(g);
                    if (frames > 1) frame = 1 + std::min(frames - 2, static_cast<u32>(p * (frames - 1)));
                    else alpha = static_cast<f32>(1 - p), scale = static_cast<f32>(1 - 0.3 * p);
                }
            break;
        case Phase::Fall: {
            const usize k = static_cast<usize>(i);
            const f64 to = shown.y_of(i), from = f.from_y[k];
            const f64 q = f.fall_time[k] > 0 ? ease::fall(ease::progress(f.t, f.fall_time[k])) : 1;
            y = fd.y0 + ease::lerp(from, to, q) + 0.5;
            break;
        }
        case Phase::Calm: break;
        }
        gem_at(i, g, x, y, alpha, scale, frame);
    }
    if (f.picked >= 0 && f.phase == Phase::Calm)
        painter_.put(painter_.white(), fd.x0 + now.x_of(f.picked) + 0.5, fd.y0 + now.y_of(f.picked) + 0.5, 0.96f, 0.96f,
                     render::pack_color(255, 240, 160, 110), 2);
}

void Match3Game::render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, u32 width, u32 height) {
    width_ = width;
    height_ = height;
    const Color bg = module_.background();
    if (field_ && painter_.ready()) {
        fit_camera(width, height);
        painter_.update(module_.library_ref(), set_, &field_->field.own);
        painter_.begin(camera_);
        push_board();
        painter_.prepare(cmd, camera_, width, height);
    }
    SDL_GPUColorTargetInfo info{};
    info.texture = target;
    info.clear_color = SDL_FColor{bg.r, bg.g, bg.b, 1.0f};
    info.load_op = SDL_GPU_LOADOP_CLEAR;
    info.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &info, 1, nullptr);
    if (field_ && painter_.ready()) painter_.draw(cmd, pass);
    SDL_EndGPURenderPass(pass);
}

} // namespace match3
