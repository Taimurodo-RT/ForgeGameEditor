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
#include <map>
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
    // "talk": the value is the id of one of the game's talks (its dialogues of
    // «Сюжет»); the choices are those, «empty» first (Library::set_talks).
    std::string asset;
    std::string empty; // shown for an empty value: "обычный", "нет"
    // The value is the template's alone: the game reads it from the template, so a copy on a level has none of its
    // own (set_value on a copy refuses; the level's panel shows the template's, not to be changed there).
    bool template_only = false;

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
    // Its templates are put on levels and linked in «Логика» ("placed": false: they only say something to the
    // game, as the hero's picture does, and the editor's palette and «Логика» leave them out).
    bool placed = true;
    // Its one copy on a level is where the game's hero starts ("hero": true, step 14.4b): «Логика» calls it the hero
    // (logic::kHero), as it does a game's hero that is not on the level, and has no thing of its own for it.
    bool hero = false;
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

// The game's frame limit: the most frames a template's picture may be cut into, an animation may show in a row, and
// «Векторная» may bake of all clips together. One number of the game, "frame_limit" in its game.json, this one
// without it (docs/models/движение-модель.md, «Предел кадров»).
inline constexpr u32 kDefaultFrameLimit = 64;
// The widest strip, in pixels: the side of a texture SDL GPU promises on any video card (MAX_2D_DIMENSION in
// SDL_gpu.c). 64 frames of 256, the largest frame «Векторная» bakes, are exactly that wide.
inline constexpr u32 kMaxStripWidth = 16384;
// The highest limit a game may set: that many frames of one pixel make the widest strip.
inline constexpr u32 kMaxFrameLimit = kMaxStripWidth;
// The game's limit from game_dir/game.json: kDefaultFrameLimit without the field (or without the file). False, with
// why in words, when game.json does not read or the field is not a whole number of 1..kMaxFrameLimit.
bool read_frame_limit(const std::filesystem::path& game_dir, u32& limit, std::string* why = nullptr);
// game.json's text with "frame_limit" set to limit (a new field goes last), the other fields as they were and in their
// order, laid out as the editor writes game.json, its lines ending as they did (CR LF when the text has them, as a
// checkout on Windows gives). False when the text is not a JSON object.
bool with_frame_limit(std::string_view json, u32 limit, std::string& out);

// A mark of an animation (part 5а of docs/models/движение-модель.md): emit("удар"[, s]) or sound("шаг") of a clip of
// «Векторная», baked onto the frame that first shows its moment. `frame` is its place in the animation's frames (from
// 0), not a frame of the picture; `frames` the frames its window holds, 1 for a plain mark and a sound. The game fires
// it in the world step its frame begins to show in; a window goes while any of its frames shows.
struct ClipEvent {
    std::string name;
    bool sound = false; // sound("шаг"): the game's sound of that name; else emit("удар")
    u32 frame = 0, frames = 1;

    bool operator==(const ClipEvent&) const = default;
};

// An animation of a template's picture in one state the game's module names ("walk"): which frames of its strip show
// one after another, how many a second, and whether it starts over (else it stays on its last frame). A template
// without one for a state is drawn by the module's own rule for it (the tab «Анимация»).
struct Clip {
    std::vector<u32> frames;
    f32 fps = 10;
    bool loop = true;
    std::vector<ClipEvent> events; // by frame, those of one frame as the clip made them

    bool operator==(const Clip&) const = default;
};
// The frames a second an animation may have: each divides the game's 60 ticks a second, so a frame shows a whole
// number of ticks and an animation keeps time exactly.
inline constexpr f32 kClipFps[] = {2, 3, 4, 5, 6, 7.5f, 10, 12, 15, 20, 30};
// The ticks (60 a second) one frame shows; 0 when fps is none of kClipFps.
u32 clip_ticks(f32 fps);
// Why the animation cannot play on a picture of `frames` frames in a game of that frame limit: no frames, more than the
// limit, a frame past the strip, fps not of the list. Empty: it can.
std::string clip_problem(const Clip& clip, u32 frames, u32 limit);
// The frame of the strip it shows `ticks` ticks after its state began (of one that can play).
u32 clip_frame(const Clip& clip, u64 ticks);
// Its place in its frames then: counted on round with «Повторять», else held on the last.
u32 clip_place(const Clip& clip, u64 ticks);
// The events (indices in clip.events) whose frame begins to show at a tick after `was` and not after `now`, in the
// order they begin; was < 0: the animation began at tick 0, so the events of its first frame count. Each frame of a
// round once more; past the last frame of one without «Повторять», none. More frames than the animation has between
// the two (never at 60 ticks a second): the last round of them, each event once.
std::vector<u32> clip_events(const Clip& clip, i64 was, u64 now);
// Whether event e goes `ticks` ticks after the state began: the frame shown is one of its window.
bool clip_event_going(const Clip& clip, const ClipEvent& e, u64 ticks);
// Why an event cannot be one of the clip's: no name, its place or its window past the frames, a sound with a window.
// Empty: it can.
std::string event_problem(const Clip& clip, const ClipEvent& e);
// What each place of a clip shows under its frame in «Покадровая»: the mark that begins there, an emit's flag over a
// sound's note, and whether a window of more than one frame goes on there.
struct FrameMark {
    enum class Begins : u8 { None, Sound, Emit } begins = Begins::None;
    bool window = false;
};
std::vector<FrameMark> frame_marks(const Clip& clip);
// The clip's events cut to its frames, after frames were taken away: those that began past them go, a window that
// went on past them ends at the last; the names of those gone, in order.
std::vector<std::string> trim_events(Clip& clip);
// The file of a mark's sound("шаг") among the game's sounds, as «Векторная» finds it: шаг, else шаг.wav, else шаг.ogg
// in `folder`; empty when none is there or the name is no file of the folder (a / or \ in it).
std::filesystem::path mark_sound_file(const std::filesystem::path& folder, std::string_view name);

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
    // How many frames the picture is: a strip of that many equal frames side by side (1..the game's frame limit; 1: the
    // picture as it is).
    // Which one shows is the game's: the hero's four (stands, step, step, in the air), a walker's two.
    u32 frames = 1;
    // Its own animations of the picture, by the state of the module's they play in ("stand", "walk"). States the
    // module does not know stay as they are read.
    std::map<std::string, Clip> animations;
    // What its file says past the game's frame limit, set aside by the library whenever it reads or puts the template
    // (Library::reload_templates, Library::put): the game and the tabs see one picture (frames 1) and no such
    // animation, but a save writes them back as they were (file_frames, file_animations), and a higher limit reads them
    // again. frames_over: the file's frames when the picture reads as one, 0 when none were set aside; animations_over:
    // the file's animations longer than the limit, by state.
    u32 frames_over = 0;
    std::map<std::string, Clip> animations_over;
    std::filesystem::path file;
    u32 rev = 0; // hash of the values and blocks: copies with another rev are behind
    // Its picture file got new content under the same name (Library::picture_changed); not saved.
    u32 picture_stamp = 0;

    // Changes whenever what the template looks like may change (its values
    // or its picture): names cached icons.
    u32 look() const;
    // As its file has them: frames and animations set aside past the game's limit included, unless changed since (a
    // picture of frames other than 1, an animation of its own for that state). What template_json writes.
    u32 file_frames() const;
    std::map<std::string, Clip> file_animations() const;

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

    // The game's frame limit (read_frame_limit): set before load. A template whose picture has more frames than it reads
    // as one picture, and an animation longer than it does not read, each said in the log (and the first in
    // read_notes). The same when a template is put (an undo of a tab's step after the limit changed in another): what
    // the library holds is what reading its files again gives. kDefaultFrameLimit unless set.
    u32 frame_limit() const { return frame_limit_; }
    void set_frame_limit(u32 limit) { frame_limit_ = limit; }
    // The templates whose picture reads as one past the limit, as they are now, in the list's order: «шаблон «Ёжик»
    // (ezhik.object.json): кадров 80 больше предела игры 64, читается одной картинкой».
    const std::vector<std::string>& read_notes() const { return read_notes_; }

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

    // The game's talks (ids of its dialogues, step 14.4b): the choices of every property "asset": "talk", the empty
    // value first. A value no talk has any more shows as such (display). Not a change of any template.
    void set_talks(const std::vector<std::string>& ids);

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
    // edit puts the whole old template back through this. Its frames and
    // animations past the game's frame limit are set aside as reading its
    // file would (reload_templates); its file gets them all.
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
    // stored: give it to put(). Empty when this library has no such kind,
    // its picture has more frames than this game's limit, or the picture
    // cannot be copied.
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
    // Its frames and animations by the game's limit, from what its file has: past it set aside, within it read; what is
    // newly set aside said in the log.
    void fit(Template& t) const;
    // read_notes of the templates as they are now.
    void note_limits();
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
    u32 frame_limit_ = kDefaultFrameLimit;
    std::vector<std::string> read_notes_;
    AdoptFn adopt_;
};

// Parses and writes template files (exposed for tests and tools). Reading knows no game: frames of 1..kMaxFrameLimit
// read as written (what is not reads as one), the game's limit is the library's.
std::optional<Template> read_template(const std::filesystem::path& file, std::string* error = nullptr);
std::string template_json(const Template& t);
u32 values_rev(const std::vector<std::pair<std::string, std::string>>& values);
// What copies compare: the values and the blocks.
u32 template_rev(const Template& t);
// A step of a tab's history on a template (from → to), put onto the template as it is now, so a step of another tab's
// history on it stays: its frames and its animations each where this step changed them, as the file has them (set aside
// past the game's limit included). Library::put sets aside again by the limit as it is then.
void take_look(Template& now, const Template& to, const Template& from);

} // namespace forge::objects

FORGE_REFLECT_DECLARE(forge::objects::ObjectRef)
