#pragma once

// Putting a planned import of a Tiled map (tiled_import.h) into the game and
// the level:
//
//   tiled::Preview p = tiled::preview(level, library, plan, options); // the window shows it
//   tiled::Resources made;
//   if (!tiled::write_resources(plan, library, sounds, made, &why)) ...;  // nothing written then
//   tiled::apply(level, history, library, plan, options, &why);           // one entry of the history
//
// The «Картинка» templates and their pictures and the zones' music are the
// game's (the window says so before): written first, all or nothing, and not
// taken back by Ctrl+Z of the level. Everything the level holds (cells, its
// own tiles, what is around, objects, zones, the spawn point, tiled.json) is
// one entry of its history, written to its folder by Ctrl+S as any edit.

#include "forge/editor/undo.h"
#include "forge/level/level.h"
#include "forge/level/object_edit.h"
#include "forge/level/tiled_import.h"

#include <filesystem>
#include <string>
#include <vector>

namespace forge::level::tiled {

// The kind of the templates an import makes (kinds.json of the game).
inline constexpr const char* kPictureKind = "picture";

struct ApplyOptions {
    bool empty_around = true; // nothing around the map (world.json), else the game's world
    u32 walls = 0, blocks = 1; // the game's layers the plan's cells go to; every other layer is emptied in the map's rectangle
};

// The file a template's picture is kept in: named after the template and
// what it shows ("<id>_<8 hex>.png"), so a picture that changes is a new file
// (icons and the game see it at once) and the same picture the same file.
std::string picture_file(const Picture& p);

// Whether a template id is taken by something an import did not make (for
// Options::template_taken): a template of that id that is not a «Картинка»
// with a picture named after it.
bool template_taken(const objects::Library& library, const std::string& id);

// What applying would change, for the window. Nothing changes; the map's
// rectangle and the objects are loaded to be found (as the view loads them).
struct Preview {
    std::string refusal;  // why it cannot be applied at all ("" when it can)
    u64 cells = 0;        // cells of the map's rectangle that become different (all layers)
    u64 author_cells = 0; // of them, changed before (by the author or an earlier import): not as the game made them
    bool around = false;  // what is around changes
    u32 game_objects = 0; // the game's own objects (villagers, critters) that go: in the map, and around it when nothing will be
    u32 objects_new = 0;
    u32 objects_same = 0; // an earlier import's objects already as in the map
    std::vector<std::string> objects_back; // an earlier import's objects put back as in the map: "Сундук (3): сдвинут"
    u32 objects_removed = 0;
    u32 templates_new = 0, templates_updated = 0;
    bool nothing() const {
        return cells == 0 && !around && objects_new == 0 && objects_back.empty() && objects_removed == 0;
    }
};
Preview preview(Level& level, const objects::Library& library, const Plan& plan, const ApplyOptions& o);

// The game's files an import writes.
struct Resources {
    std::vector<std::string> pictures;  // written into the library's pictures folder
    std::vector<std::string> templates; // ids of templates made or changed
    std::vector<std::string> sounds;    // copied into the sounds folder
};
// Writes the pictures of the plan's templates into a folder aside, moves them
// in all together, then the zones' music into sounds and the templates
// (library.put). Anything fails: false, the file in words, and what it wrote
// is taken back (the game's files as before). A music file whose name another
// sound has gets a name of its own, and the plan's zones take it. Pictures
// that templates of an import no longer show are removed.
bool write_resources(Plan& plan, objects::Library& library, const std::filesystem::path& sounds, Resources& made,
                     std::string* error = nullptr);

// One entry of the history ("Импорт карты Tiled «…»"). The plan's templates
// must be in the library (write_resources). False (and nothing changes) when
// the level cannot take it; true and no entry when nothing would change.
bool apply(Level& level, editor::UndoStack& history, const objects::Library& library, const Plan& plan, const ApplyOptions& o,
           std::string* error = nullptr);

// --- the commands it is made of ---

// Sets whole chunks (all layers); undo puts them back. Kept compressed.
class SetChunks final : public editor::Command {
public:
    struct Change {
        world::ChunkCoord chunk;
        std::vector<u8> before, after; // world::encode_chunk
        bool edited = false;           // before: changed (else as the generator made it)
    };
    SetChunks(Level& level, std::vector<Change> changes, std::string label);
    void apply(editor::Document&) override { set(true); }
    void revert(editor::Document&) override { set(false); }
    std::string label() const override { return label_; }

private:
    void set(bool after);
    Level& level_;
    std::vector<Change> changes_;
    std::string label_;
};

// Changes what is around the level (Level::set_around).
class SetAround final : public editor::Command {
public:
    SetAround(Level& level, LevelWorld before, LevelWorld after, std::string label);
    void apply(editor::Document&) override { level_.set_around(after_); }
    void revert(editor::Document&) override { level_.set_around(before_); }
    std::string label() const override { return label_; }

private:
    Level& level_;
    LevelWorld before_, after_;
    std::string label_;
};

// Takes the game's own objects (those with no LevelId: the author never
// touched them, villagers and critters) out of a rectangle of tiles, and,
// with outside, out of every chunk nobody changed (nothing will be around
// them); undo brings them back. Which ones is found when it is done (the
// rectangle is loaded first, so it is peopled as the game would).
class TakeGameObjects final : public editor::Command {
public:
    TakeGameObjects(Level& level, world::Rect rect, bool outside, std::string label);
    void apply(editor::Document&) override;
    void revert(editor::Document&) override;
    std::string label() const override { return label_; }

private:
    Level& level_;
    world::Rect rect_;
    bool outside_;
    std::string label_;
    std::vector<ObjectSnapshot> gone_; // packed as they were, no ids
};

// Removes some objects and makes others, as one (an object made anew with
// the id of one removed replaces it); undo the other way.
class PutObjects final : public editor::Command {
public:
    PutObjects(Level& level, std::vector<ObjectSnapshot> gone, std::vector<ObjectSnapshot> made, std::string label);
    void apply(editor::Document&) override { set(gone_, made_); }
    void revert(editor::Document&) override { set(made_, gone_); }
    std::string label() const override { return label_; }

private:
    void set(const std::vector<ObjectSnapshot>& out, const std::vector<ObjectSnapshot>& in);
    Level& level_;
    std::vector<ObjectSnapshot> gone_, made_;
    std::string label_;
};

class SetRecord final : public editor::Command {
public:
    SetRecord(Level& level, Record before, Record after, std::string label);
    void apply(editor::Document&) override { level_.set_tiled_record(after_); }
    void revert(editor::Document&) override { level_.set_tiled_record(before_); }
    std::string label() const override { return label_; }

private:
    Level& level_;
    Record before_, after_;
    std::string label_;
};

// The game's own objects TakeGameObjects takes, of the loaded ones.
void game_objects(Level& level, const world::Rect& rect, bool outside, std::vector<flecs::entity>& out);

} // namespace forge::level::tiled
