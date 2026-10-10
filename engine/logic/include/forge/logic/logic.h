#pragma once

// Links: the simplest way to say what happens in a game.
//
// An author puts the game's things on a board and draws arrows between them,
// each with a verb: «Ключ открывает Дверь», «Шипы ранят героя», «Герой
// собирает Монеты», «Герой входит в Шахту». A thing is an object template
// (every copy of it on every level), the hero, or an area of the level (a
// named rectangle of the «Зоны» mode, which the hero comes into). A link may be refined: only at night, only once, with
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
#include "forge/script/graph.h"
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
// An area of the level is the thing "area:" and its id (16 hex digits): a
// renamed area keeps its links.
inline constexpr std::string_view kAreaPrefix = "area:";
bool is_area(std::string_view thing);
// A level of the game is the thing "level:" and its id in the game's list
// (levels.json): where a link sends the hero («уходит на уровень»). Levels are
// never a side of a link, only where it leads.
inline constexpr std::string_view kLevelPrefix = "level:";
bool is_level(std::string_view thing);
// The game's action that sends the hero to another level: a link with it says
// where in Link::level and Link::arrive, and runs forge.logic.go.
inline constexpr std::string_view kGoAction = "go";

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
    // touched side; the hero's own side means the other one); an area is
    // touched when the hero comes into it. "always": from the start, as long
    // as the first thing is there.
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
    // Which things fit each side: "hero", "thing" (a template: not the hero,
    // not an area), "area" or "" any.
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

// --- ideas -----------------------------------------------------------------

// A ready piece of a game with a few fields: «Дверь с ключом» (the door, the
// key), «Ловушка» (what hurts). An idea is a way to see and make a link: its
// verb, which sides the author picks, and the refinements it starts with.
// Ideas are data (the game's ideas.json).
struct IdeaField {
    Side side = Side::B;
    std::string label; // «Дверь», «Что ранит»
};

struct Idea {
    std::string id, name, icon, group, about;
    std::string verb;
    std::vector<IdeaField> fields;
    bool night = false, once = false, sound = false, hint = false; // what a new one starts with
};

class Ideas {
public:
    bool load(const std::filesystem::path& file, std::string* error = nullptr);
    bool parse(std::string_view json, std::string* error = nullptr);
    const Idea* find(std::string_view id) const;
    // The idea a link is seen as: the first one with its verb.
    const Idea* of_verb(std::string_view verb) const;
    const std::vector<Idea>& all() const { return ideas_; }

private:
    std::vector<Idea> ideas_;
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
    // Its own code («Код» block): Luau run instead of the verb's action when
    // the link happens (self, hero, target are there). Empty: the verb's.
    std::string code;
    // Its scheme («Схема»): a node graph (script::Graph as JSON) that starts
    // at «Когда» (logic.when). When it is the author's own (own_scheme) it
    // does what the link does; when it is just what the refinements make, it
    // only keeps where its nodes are. Empty: the verb's, laid out anew.
    std::string graph;
    // Where it sends the hero, for a verb whose action is kGoAction: the
    // level's id in the game's list, and where the hero comes out there: ""
    // its spawn point, or one of its areas ("area:" and the id).
    std::string level;
    std::string arrive;
};

// A thing's own scheme: what every copy of it does by itself, with no link
// and no hero touching it: «При старте», «Каждый шаг», «При ударе»… (a
// platform going back and forth, a lamp that blinks). Only things (object
// templates) have one, the hero does not.
struct ThingScheme {
    u32 id = 0;        // from the same ids as links (errors and lines refer to it)
    std::string thing; // a template id
    std::string graph; // script::Graph as JSON; its events are std.event.*
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
    std::vector<ThingScheme> schemes; // at most one per thing
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
    ThingScheme* find_scheme(u32 id);
    const ThingScheme* find_scheme(u32 id) const;
    const ThingScheme* scheme_for(std::string_view thing) const;
    // Gives the scheme a new id; returns it (0: the thing has one already).
    u32 add_scheme(ThingScheme scheme);
    bool remove_scheme(u32 id);
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
    bool area = false;    // an area of the level («Шахта»), not a template
    // An area's level (its id in the game's list; "" when it is no level of
    // the list); a level's own id for a level ("level:" things).
    std::string level;
    Forms forms;
    std::vector<std::string> blocks; // the template's blocks (none for the hero or an area)
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
// An area of the level as a thing: id is "area:" and its id, name its name;
// level: the level it is on.
Thing area_thing(std::string_view id, std::string_view name, std::string_view level = {});
// A level of the game as a thing: id is "level:" and level, name its name.
Thing level_thing(std::string_view level, std::string_view name);

using FindThing = std::function<const Thing*(std::string_view id)>;

// Where a link sends the hero, in words: «на уровень «Пещера»», then «, в зону
// «Вход»» when it comes out in an area; «— уровень не выбран» when it has no
// level. Empty for a link that sends nowhere (its verb's action is not
// kGoAction). Names from things ("level:" and the area), else the ids.
std::string destination(const Link& link, const VerbDef& verb, const FindThing& things);
// «Ключ открывает Дверь» (empty when a thing or the verb is unknown); where:
// destination(), put after it.
std::string phrase(const Link& link, const VerbDef& verb, const Thing& a, const Thing& b, std::string_view where = {});
// What happens, in plain words, with the refinements (and where).
std::string meaning(const Link& link, const VerbDef& verb, const Thing& a, const Thing& b, std::string_view where = {});
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
std::vector<Step> steps(const Link& link, const VerbDef& verb, const Thing& a, const Thing& b, std::string_view where = {});
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
    // line -> index of the link in Logic::links; a thing's scheme is
    // links.size() + its index in Logic::schemes
    script::SourceMap map;
    bool touch = false;  // copies need a touch trigger
};

struct Problem {
    u32 link = 0; // the link's id, or a thing scheme's
    std::string text; // in Russian, for the author
    u32 node = 0;         // a node of the link's scheme it is about (0: the link)
    bool warning = false; // the link still works
};

struct Compiled {
    std::vector<Module> modules;
    std::vector<Problem> problems; // links left out
    const Module* find(std::string_view thing) const;
};

std::string module_name(std::string_view thing);
// nodes: what links' schemes are made of (node_library); without it a link
// with a scheme is left out.
Compiled compile(const Logic& logic, const Verbs& verbs, const FindThing& things,
                 const script::NodeLibrary* nodes = nullptr);
// The whole game's code as one text, for the editor's «Код» mode; map gives
// the link (index) of each line.
std::string listing(const Logic& logic, const Verbs& verbs, const FindThing& things, script::SourceMap* map = nullptr,
                    const script::NodeLibrary* nodes = nullptr);
// What a link does as code, to start its own from: the verb's action with
// the link's refinements («Код» block).
std::string default_code(const Link& link, const Verbs& verbs, const FindThing& things);
// The lines of a link's own code (none for "").
usize code_lines(std::string_view code);

// --- schemes («Схема») ---------------------------------------------------

// The nodes a scheme can use: the standard ones, the engine's functions
// (api: the host's, or one made with script::register_core_api) and the
// links' own (category «Связи»).
script::NodeLibrary node_library(const script::ScriptApi& api);
// What a link does, as a scheme: «Когда» → «Если» (the checks) → «Сделать»…
// laid out left to right. Its own scheme when it has one.
script::Graph scheme_of(const Link& link, const Verbs& verbs, const FindThing& things);
// The «Когда» node of a scheme (0: none).
u32 when_node(const script::Graph& graph);
// Gives the link this scheme. When the scheme is what some refinements make
// (a check taken away, a sound added, nodes moved), the link gets those
// refinements and keeps the scheme only for where its nodes are: simple
// modes stay simple. Where its «Перейти на уровень» leads is the link's
// level and arrive.
void set_scheme(Link& link, const script::Graph& graph, const Verbs& verbs, const FindThing& things);
// Sends the link elsewhere: its level and arrive, and its scheme's «Перейти на
// уровень» with them (so a scheme kept for its places stays the link's).
void set_destination(Link& link, std::string_view level, std::string_view arrive);
// The link has a scheme of its own: one no refinements make. Only then does
// the scheme decide what happens (and the simple modes show «Уточнено в
// Схеме»).
bool own_scheme(const Link& link, const VerbDef& verb, const Thing& a, const Thing& b);
bool own_scheme(const Link& link, const Verbs& verbs, const FindThing& things);
// The nodes of a link's own scheme (0 for none).
usize scheme_nodes(const Link& link);
// A thing's new scheme: «При старте» and «Каждый шаг», nothing after them.
script::Graph new_thing_scheme(std::string_view thing);
// What a thing's scheme can start from: the events of std (not «Когда»).
bool thing_event(std::string_view def);

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
    // A link sends the hero to a level (its id in the game's list) and an
    // area there ("" its spawn point). Only asked for: the game goes after
    // the tick. False: this game has no levels to go to (said once in the log).
    virtual bool go(flecs::entity_t hero, std::string_view level, std::string_view arrive, u32 link) {
        (void)hero, (void)level, (void)arrive, (void)link;
        return false;
    }
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
    // The level's areas: things links may name besides the templates (load()
    // compiles their links). Each one a link listens to gets an entity of its
    // own running its module, with no Position: it is no chunk's, so it is
    // never unloaded, doubled or saved with one.
    void set_areas(std::vector<Thing> areas);
    // The areas of the game's other levels: links to them compile (they are no broken links) but never run here.
    void set_other_areas(std::vector<Thing> areas);
    // The game's levels (level_thing): where links may send the hero.
    void set_levels(std::vector<Thing> levels);
    // The hero came into an area or left it: the area's links run with this
    // tick's triggers (ScriptHost::enter). The game checks where the hero is.
    void area_event(std::string_view area, flecs::entity_t hero, bool entered);
    // The entity standing for an area; 0 when no link listens to it.
    flecs::entity_t area_entity(std::string_view area) const;
    bool is_area_entity(flecs::entity_t e) const;
    // The link of a line in a module (for errors): index into the links
    // loaded, or -1.
    i32 link_at(std::string_view module, i32 line) const;
    const Compiled& compiled() const { return compiled_; }
    // Each link's id by its index, as loaded (then the things' schemes).
    const std::vector<u32>& link_ids() const { return ids_; }

    struct Impl;
    friend struct Api;

private:
    void attach_one(flecs::entity e);
    void sync_areas();

    script::ScriptHost& host_;
    const objects::Library& library_;
    Game& game_;
    Compiled compiled_;
    std::vector<u32> ids_;
    std::unique_ptr<script::NodeLibrary> nodes_; // what links' schemes are made of
    std::unordered_map<u64, std::string> listening_; // template key -> module
    std::unordered_map<u64, bool> touch_;            // template key -> needs a trigger
    scene::Scene* scene_ = nullptr;
    flecs::observer observer_;
    std::vector<Thing> areas_;
    std::vector<Thing> other_areas_;
    std::vector<Thing> levels_;
    std::unordered_map<std::string, flecs::entity_t> area_entities_; // area -> its entity
};

} // namespace forge::logic
