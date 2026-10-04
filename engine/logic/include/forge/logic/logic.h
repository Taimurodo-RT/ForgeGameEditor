#pragma once

// Links: the simplest way to say what happens in a game.
//
// An author puts the game's things on a board and draws arrows between them,
// each with a verb: «Ключ открывает Дверь», «Шипы ранят героя», «Герой
// собирает Монеты». A thing is an object template (every copy of it on every
// level) or the hero. A link may be refined: only at night, only once, with
// a sound, with a hint when it cannot happen.
//
// Verbs are data (the game's verbs.json), so a game adds its own: a verb says
// which of the two things the hero touches, what the hero must carry, and
// which action the game performs. Actions are the game's (logic::Game).
//
// Links compile to Luau: one module per template that listens, run by the
// script host like any other script. The same code is what the editor shows
// in its «Код» mode, and each line knows which link made it.
//
//   logic::Verbs verbs;  verbs.load(game / "verbs.json");
//   logic::Logic links;  links.load(game / "logic.json");
//   logic::Runtime run(host, library, my_game);
//   run.load(links, verbs);
//   run.attach(scene); // copies of listening templates get their scripts

#include "forge/core/types.h"
#include "forge/script/host.h"

#include <flecs.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace forge::objects {
class Library;
struct Template;
} // namespace forge::objects

namespace forge::scene {
class Scene;
}

namespace forge::logic {

// The hero: a thing every game has.
inline constexpr std::string_view kHero = "hero";

// --- verbs ---------------------------------------------------------------

enum class Side : u8 { A, B };

struct VerbDef {
    std::string id;     // "open": links keep it
    std::string name;   // "открывает"
    std::string plural; // "открывают" (things named in the plural: «Шипы»)
    std::string icon;   // Material Symbols
    // How the second thing is named after the verb: "acc" (открывает дверь),
    // "dat" (даёт монету герою), "ins" (говорит с героем), "gen" (убегает от
    // героя).
    std::string object_case = "acc";
    // "touch": it happens when the hero touches one of the two things (the
    // touched side; the hero's own side means the other one). "always": from
    // the start, as long as the first thing is there.
    bool always = false;
    Side touch = Side::B;
    // The hero must carry a copy of this side's thing ("" none).
    std::string needs;
    // The game's action ("open", "hurt"…) and the side it is done to.
    std::string action;
    Side target = Side::B;
    std::string sound; // the game's usual sound for it ("" none)
    // Said to the hero when the link cannot happen (with a «Подсказка»
    // refinement); {a} {b} are the things' names.
    std::string fail;
    // What happens, in plain words for the editor; {a} {b} names, {a:gen}…
    // their forms.
    std::string about;
    // Which things fit each side: "hero", "thing" (not the hero) or "" any.
    std::string a_is, b_is;
    // A thing on that side must have one of these blocks («Подбирается»:
    // "pickup», «Дверь»: "door"); empty: any. Only for offering verbs: a
    // link made anyway still runs.
    std::vector<std::string> a_has, b_has;
    // What the game does, as a step: «Открыть {b:acc}».
    std::string step;
};

class Verbs {
public:
    bool load(const std::filesystem::path& file, std::string* error = nullptr);
    bool parse(std::string_view json, std::string* error = nullptr);
    const VerbDef* find(std::string_view id) const;
    const std::vector<VerbDef>& all() const { return verbs_; }

private:
    std::vector<VerbDef> verbs_;
};

// --- the links of a game -------------------------------------------------

struct Link {
    u32 id = 0;        // stable within the game (the editor and errors refer to it)
    std::string a;     // a template id or kHero
    std::string verb;
    std::string b;
    // Refinements («Уточнить»).
    bool night = false; // only at night
    bool once = false;  // only once (for each copy)
    bool sound = false; // with the verb's sound
    bool hint = false;  // a hint when it cannot happen
};

// Where a thing lies on the editor's board.
struct Spot {
    std::string thing;
    f32 x = 0, y = 0;
};

class Logic {
public:
    std::vector<Link> links;
    std::vector<Spot> board;
    u32 next_id = 1;

    // A missing file is an empty game (true).
    bool load(const std::filesystem::path& file, std::string* error = nullptr);
    bool parse(std::string_view json, std::string* error = nullptr);
    bool save(const std::filesystem::path& file, std::string* error = nullptr) const;
    std::string json() const;

    Link* find(u32 id);
    const Link* find(u32 id) const;
    // Gives the link a new id; returns it.
    u32 add(Link link);
    bool remove(u32 id);
    const Spot* spot(std::string_view thing) const;
    void set_spot(std::string_view thing, f32 x, f32 y);
};

// --- names in Russian ----------------------------------------------------

struct Forms {
    std::string nom, acc, dat, ins, gen;
    const std::string& get(std::string_view form) const; // "acc"… ("" or unknown: nom)
};

// A thing as the words see it.
struct Thing {
    std::string id;
    std::string name;     // as the author wrote it: «Ключ от кузницы»
    bool animate = false; // a person or an animal: «вижу героя», not «вижу герой»
    bool plural = false;  // «Шипы», «Монеты»
    Forms forms;
    std::vector<std::string> blocks; // the template's blocks (none for the hero)
};

// The forms of a name, guessed from its endings: the first word (and the
// noun after a leading adjective) changes, the rest stays («Ключа от
// кузницы»). Good for most names; a template may give its own.
Forms decline(std::string_view name, bool animate);
bool looks_plural(std::string_view name);
// The hero.
Thing hero_thing();
// A template as a thing (animate when it is a villager, critter or player).
Thing thing_of(const objects::Library& library, const objects::Template& t);

using FindThing = std::function<const Thing*(std::string_view id)>;

// «Ключ открывает Дверь» (empty when a thing or the verb is unknown).
std::string phrase(const Link& link, const VerbDef& verb, const Thing& a, const Thing& b);
// What happens, in plain words, with the refinements.
std::string meaning(const Link& link, const VerbDef& verb, const Thing& a, const Thing& b);
// A verb's text with {a} {b} {a:gen}… filled in.
std::string fill(std::string_view text, const Thing& a, const Thing& b);
// The verb makes sense for these two (sides and blocks): what the editor
// offers.
bool suits(const VerbDef& verb, const Thing& a, const Thing& b);

// A link as steps («Шаги»): when it happens, the checks, what is done, and
// what happens otherwise. Steps made by a refinement name it, so the editor
// can take them away or add them.
struct Step {
    std::string part;   // "when", "if", "then", "else"
    std::string icon;   // Material Symbols
    std::string text;
    std::string refine; // "night", "once", "sound", "hint"; "" the verb's own
};
std::vector<Step> steps(const Link& link, const VerbDef& verb, const Thing& a, const Thing& b);
// The refinements a link can have, as steps not yet there (what «Добавить
// шаг» offers), and whether it has them.
struct Refine {
    std::string id, label, about;
    bool on = false;
};
std::vector<Refine> refinements(const Link& link, const VerbDef& verb, const Thing& a, const Thing& b);

// --- compiling -----------------------------------------------------------

// The module of one template: the links its copies listen for.
struct Module {
    std::string thing;   // template id
    std::string name;    // module name: "logic:" + thing
    std::string source;  // Luau
    script::SourceMap map; // line -> index of the link in Logic::links
    bool touch = false;  // copies need a touch trigger
};

struct Problem {
    u32 link = 0;
    std::string text; // in Russian, for the author
};

struct Compiled {
    std::vector<Module> modules;
    std::vector<Problem> problems; // links left out
    const Module* find(std::string_view thing) const;
};

std::string module_name(std::string_view thing);
Compiled compile(const Logic& logic, const Verbs& verbs, const FindThing& things);
// The whole game's code as one text, for the editor's «Код» mode; map gives
// the link (index) of each line.
std::string listing(const Logic& logic, const Verbs& verbs, const FindThing& things, script::SourceMap* map = nullptr);

// --- running -------------------------------------------------------------

// What links do in a particular game. Entities are flecs ids.
class Game {
public:
    virtual ~Game() = default;
    virtual bool is_hero(flecs::entity_t e) = 0;
    virtual flecs::entity_t hero() = 0; // 0 when there is none
    // The hero carries a copy of the thing (a template id).
    virtual bool has(flecs::entity_t hero, std::string_view thing) = 0;
    // Performs a verb's action on target (a copy, or the hero). thing is
    // the target's template id (kHero for the hero), other the other side's
    // entity. False: this game cannot do it (said once in the log).
    virtual bool act(std::string_view action, flecs::entity_t target, std::string_view thing, flecs::entity_t other,
                     flecs::entity_t hero) = 0;
    virtual void hint(flecs::entity_t hero, std::string_view text) = 0;
    virtual void sound(flecs::entity_t at, std::string_view cue) = 0;
    virtual bool night() = 0;
    // A link happened (the editor lights it up).
    virtual void fired(u32 link) { (void)link; }
};

// How close the hero must come to touch a thing, in tiles from its centre.
inline constexpr f32 kTouchRadius = 1.4f;

struct Api;

class Runtime {
public:
    // Registers the forge.logic functions with the host: make it before the
    // host loads or runs anything (ScriptHost::api_built).
    Runtime(script::ScriptHost& host, const objects::Library& library, Game& game);
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // Compiles and loads the modules (again: they are replaced while the
    // game runs). Problems are links left out; script errors are the host's.
    bool load(const Logic& logic, const Verbs& verbs, std::vector<Problem>* problems = nullptr);
    // Copies of listening templates get their script and a touch trigger,
    // now and whenever one appears (a chunk loads, a copy is made).
    void attach(scene::Scene& scene);
    // The link of a line in a module (for errors): index into the links
    // loaded, or -1.
    i32 link_at(std::string_view module, i32 line) const;
    const Compiled& compiled() const { return compiled_; }
    // Each link's id by its index, as loaded.
    const std::vector<u32>& link_ids() const { return ids_; }

    struct Impl;
    friend struct Api;

private:
    void attach_one(flecs::entity e);

    script::ScriptHost& host_;
    const objects::Library& library_;
    Game& game_;
    Compiled compiled_;
    std::vector<u32> ids_;
    std::unordered_map<u64, std::string> listening_; // template key -> module
    std::unordered_map<u64, bool> touch_;            // template key -> needs a trigger
    scene::Scene* scene_ = nullptr;
    flecs::observer observer_;
};

} // namespace forge::logic
