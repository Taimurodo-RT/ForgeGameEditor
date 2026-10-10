#pragma once

// Objects as the author sees them: a coin to pick up, a villager, a crate.
//
// A kind ("Подбираемое", "Житель") says what an object is in plain words: the
// components an object of that kind is made of, and the properties the author
// sets ("Что это", "Сколько"), each bound to one field of a component. Kinds
// are common to every genre; their presets are ready answers for one genre
// ("Монетка" for a platformer). A game lists its kinds in one JSON file.
//
// A template is one object the author made from a kind: "Монеты" is a
// Подбираемое with Что это = монеты, Сколько = 10. Each lives in its own
// file in the game's objects folder (*.object.json), so it is easy to keep in
// git, copy into another game or edit by hand.
//
// Objects on a level are copies of a template: they keep an ObjectRef with
// the template's key. Changing the template changes every copy, the loaded
// ones at once (refresh) and the others when their chunk loads, in the editor
// and in the game alike. A copy may set a property its own way (twelve coins
// in this one pile): that property is then the copy's override and the
// template no longer changes it.
//
//   Library lib;
//   lib.load(game / "kinds.json", game / "objects");
//   lib.attach(scene);                              // copies follow their templates
//   lib.spawn(scene, *lib.find("coins"), x, feet_y); // a new copy

#include "forge/core/types.h"
#include "forge/data/reflect.h"

#include <flecs.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <vector>

namespace forge::scene {
class Scene;
}

namespace forge::objects {

// One answer of a choice property: shown as name, written as id, stored in
// the field as value.
struct Choice {
    std::string id;   // "coins"
    std::string name; // "Монеты"
    f64 value = 0;
};

// A property in plain words, bound to one field of a component.
struct PropDef {
    std::string id;        // "count"
    std::string name;      // "Сколько"
    std::string hint;      // "сколько монет в кучке"
    std::string component; // "Item": a saved component, by its short or full name
    std::string field;     // "count"
    std::vector<Choice> choices; // not empty: pick one of these
    bool has_range = false;
    f64 min = 0, max = 0;
    bool advanced = false; // shown under «Подробно»
    // "sound": the value is a sound's name in the library's sounds folder
    // (a file today; the sound editor's sounds later).
    std::string asset;
    std::string empty; // shown for an empty value: "обычный", "нет"

    const reflect::TypeInfo* type = nullptr;  // the component's, found when the kinds load
    const reflect::FieldInfo* info = nullptr; // the field's
};

// A ready template of a kind for one use ("Монетка" for a platformer).
struct Preset {
    std::string name;   // "Монетка"
    std::string genre;  // "Платформер"; empty: any game
    std::string about;
    std::vector<std::pair<std::string, std::string>> values; // prop id, JSON value
};

// A component an object is made of, with its starting values (a JSON
// object; fields not given keep their defaults).
struct Part {
    std::string name;
    std::string json; // starting values
    const reflect::TypeInfo* type = nullptr;
};

// A building block of objects: «Тело», «Подбирается», «Житель». It adds
// components to the object and the properties the author sets on them.
// Objects are put together from blocks; a kind is a ready set of them.
struct BlockDef {
    std::string id;    // "pickup"
    std::string name;  // "Подбирается"
    std::string icon;  // a Material Symbols name
    std::string about; // what the block gives the object
    std::vector<Part> components;
    std::vector<PropDef> props;
    std::vector<std::string> needs;    // blocks it does not work without (added with it)
    std::vector<std::string> excludes; // blocks it cannot be together with (taken away)
    std::vector<std::string> was;      // its former ids: templates naming them get this one

    const PropDef* prop(std::string_view prop_id) const;
};

struct KindDef {
    std::string id;    // "pickup"
    std::string name;  // "Подбираемое"
    std::string group; // "Предметы"
    std::string icon;  // a Material Symbols name, "paid"
    std::string about; // what objects of this kind do, in one or two sentences
    f64 foot = 0.5;    // from the object's centre down to its feet, in tiles
    // Its blocks (ids), when the kind is put together from blocks; the
    // components and props below are then theirs, with the kind's own
    // starting values on top.
    std::vector<std::string> blocks;
    // What an object is made of: component name and its starting values.
    using Part = objects::Part;
    std::vector<Part> components;
    std::vector<PropDef> props;
    std::vector<Preset> presets;

    const PropDef* prop(std::string_view prop_id) const;
};

struct Template {
    std::string id;   // "coins": stable, written in the file; the key comes from it
    u64 key = 0;      // fnv1a(id): what copies keep
    std::string name; // "Монеты"
    std::string kind; // "pickup"
    std::string genre; // "Платформер"; empty: for any game (only for finding it)
    // The blocks it is made of (ids), when they differ from its kind's.
    std::vector<std::string> blocks;
    std::string about;
    // Property values that differ from the kind's own (prop id, JSON value:
    // 10, "coins", true). Missing ones take the kind's starting value.
    std::vector<std::pair<std::string, std::string>> values;
    // Its own picture: a file name in the library's pictures folder
    // ("coin.png"); empty: the game draws its usual one.
    std::string picture;
    std::filesystem::path file;
    u32 rev = 0; // hash of the values and blocks: copies with another rev are behind
    // Its picture file got new content under the same name (Library::picture_changed); not saved.
    u32 picture_stamp = 0;

    // Changes whenever what the template looks like may change (its values
    // or its picture): names cached icons.
    u32 look() const;

    const std::string* value(std::string_view prop) const;
};

// Kept by every copy of a template, saved with it.
struct ObjectRef {
    u64 key = 0;           // the template's
    u32 rev = 0;           // which values of the template the copy has
    std::string overrides; // the copy's own properties: "count,facing"

    bool overrides_prop(std::string_view prop) const;
    void set_override(std::string_view prop, bool on);
};

class Library {
public:
    Library();
    ~Library();
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    // Reads the kinds and every template in the folder (which may be
    // missing: no templates yet). False when the kinds cannot be read.
    bool load(const std::filesystem::path& kinds_file, const std::filesystem::path& folder,
              std::string* error = nullptr);
    // Reads the folder again (files changed outside the editor).
    void reload_templates();
    const std::filesystem::path& folder() const { return folder_; }
    const std::filesystem::path& kinds_file() const { return kinds_file_; }
    // Where templates' pictures are: "pictures" next to the kinds file,
    // unless set.
    const std::filesystem::path& pictures_folder() const { return pictures_; }
    void set_pictures_folder(std::filesystem::path folder) { pictures_ = std::move(folder); }
    // The template's picture file (empty without one).
    std::filesystem::path picture_file(const Template& t) const;
    // The same, as a path inside folder ("pictures/coin.png", with /) for
    // pages of a game in that folder; empty without a picture or when it is
    // not in that folder.
    std::string picture_in(const Template& t, const std::filesystem::path& folder) const;
    // Where the sounds of templates are: "sounds" next to the kinds file,
    // unless set; a sound's file there (empty name: none).
    const std::filesystem::path& sounds_folder() const { return sounds_; }
    void set_sounds_folder(std::filesystem::path folder) { sounds_ = std::move(folder); }
    std::filesystem::path sound_file(std::string_view name) const;

    const std::vector<KindDef>& kinds() const { return kinds_; }
    const std::vector<BlockDef>& blocks() const { return blocks_; }
    const BlockDef* block(std::string_view id) const;
    // Genres to sort templates by: the kinds file's list, then any other a
    // template or preset names.
    std::vector<std::string> genres() const;
    const std::vector<Template>& templates() const { return templates_; }
    const KindDef* kind(std::string_view id) const;
    const KindDef* kind_of(const Template& t) const { return kind(t.kind); }
    const Template* find(u64 key) const;
    const Template* find(std::string_view id) const;
    // Bumped by every change of a template (palettes and lists rebuild).
    u64 version() const { return version_; }
    // A picture of the pictures folder has new content under its name (an asset of «Ресурсы» changed): the
    // templates that show it look different (their look() changes, their icons are drawn again).
    void picture_changed(std::string_view name);

    // --- changing templates; each writes the template's file ---
    // A new template of a kind, from a preset or the kind's own values,
    // under a name not taken yet ("Монетка 2" when "Монетка" is).
    std::optional<Template> make(const KindDef& kind, const Preset* preset, std::string_view name) const;
    // Adds a template or replaces the one with its key. Undo of any template
    // edit puts the whole old template back through this.
    bool put(Template t, std::string* error = nullptr);
    bool remove(u64 key);
    // --- what a template is made of ---
    // Its blocks: its own list, else its kind's.
    std::vector<const BlockDef*> blocks_of(const Template& t) const;
    bool has_block(const Template& t, std::string_view block) const;
    // Its components (with starting values) and its properties, from its
    // blocks (a kind without blocks: the kind's own).
    std::vector<Part> parts_of(const Template& t) const;
    std::vector<const PropDef*> props_of(const Template& t) const;
    const PropDef* prop_of(const Template& t, std::string_view prop_id) const;
    // The thing a pickup gives ("key", its «what»); empty for anything else.
    std::string item_of(const Template& t) const;
    // A template with a block added (with the blocks it needs, without the
    // ones it excludes) or taken away (with the ones that need it). Not
    // stored: give it to put().
    Template with_block(const Template& t, std::string_view block, bool on) const;

    // A property value of a template (JSON): its own, else the kind's.
    std::string value(const Template& t, const PropDef& prop) const;
    // A template with one value changed (not stored: give it to put()).
    Template with_value(const Template& t, std::string_view prop, std::string json) const;
    // --- between libraries (a game's and the shared one) ---
    // A template of another library as it would be here: the same id (so
    // the same object), its file here (the one it already has, else a new
    // one), its picture and sounds copied into this library's folders. Not
    // stored: give it to put(). Empty when this library has no such kind
    // or the picture cannot be copied.
    std::optional<Template> copy_from(const Library& from, const Template& t, std::string* error = nullptr) const;
    // This library's template with t's id, when it is the same as t (name,
    // note, genre, kind, blocks, values, picture and sounds).
    bool same_as(const Library& from, const Template& t) const;

    // A name nobody uses yet, starting from wanted.
    std::string free_name(std::string_view wanted) const;

    // --- copies in a scene ---
    // Registers ObjectRef with the scene and keeps its copies up to date as
    // their chunks load.
    void attach(scene::Scene& scene);
    // A new copy with its feet at (x, feet_y); empty when that chunk is not
    // loaded.
    flecs::entity spawn(scene::Scene& scene, const Template& t, f64 x, f64 feet_y) const;
    // Writes the template's values into a copy (not its overrides).
    void apply(scene::Scene& scene, flecs::entity e, const Template& t) const;
    // Brings every loaded copy that is behind its template up to date.
    u32 refresh(scene::Scene& scene) const;
    // The template of a copy (null: not a copy, or its template is gone).
    const Template* template_of(flecs::entity e) const;
    // A property of a copy as it is now (JSON), and a new value for it (which
    // becomes the copy's override unless it equals the template's).
    std::string value(scene::Scene& scene, flecs::entity e, const PropDef& prop) const;
    bool set_value(scene::Scene& scene, flecs::entity e, const PropDef& prop, const std::string& json) const;
    // Objects made before templates: the game says which template one is,
    // and it becomes a copy whose differing values are its overrides.
    using AdoptFn = std::function<const Template*(flecs::entity)>;
    void set_adopt(AdoptFn fn) { adopt_ = std::move(fn); }

    // JSON value <-> what a person reads and types ("Монеты", "10", "да").
    static std::string display(const PropDef& prop, const std::string& json);
    static std::optional<std::string> parse(const PropDef& prop, std::string_view text);

private:
    void sort_templates();
    bool write(const Template& t, std::string* error) const;
    void on_unpacked(scene::Scene& scene, flecs::entity e) const;

    std::vector<KindDef> kinds_;
    std::vector<BlockDef> blocks_;
    std::vector<std::string> genres_; // from the kinds file
    std::vector<Template> templates_;
    std::filesystem::path folder_;
    std::filesystem::path kinds_file_;
    std::filesystem::path pictures_;
    std::filesystem::path sounds_;
    u64 version_ = 1;
    std::unordered_map<std::string, u32> picture_stamps_; // by picture name: how many times it changed
    u32 stamps_ = 0;
    AdoptFn adopt_;
};

// Parses and writes template files (exposed for tests and tools).
std::optional<Template> read_template(const std::filesystem::path& file, std::string* error = nullptr);
std::string template_json(const Template& t);
u32 values_rev(const std::vector<std::pair<std::string, std::string>>& values);
// What copies compare: the values and the blocks.
u32 template_rev(const Template& t);

} // namespace forge::objects

FORGE_REFLECT_DECLARE(forge::objects::ObjectRef)
