#pragma once

// Undoable edits of a level's objects: placing, deleting, moving, and
// changing a component's fields. Objects are found by their LevelId, since
// the entity itself is made anew whenever its chunk loads; every command
// loads the place it acts on first, so undo works far from the view too.

#include "forge/editor/undo.h"
#include "forge/level/level.h"

#include <string>
#include <vector>

namespace forge::level {

// An object as bytes (Scene::pack), with its id and where it stands.
struct ObjectSnapshot {
    u64 id = 0;
    std::vector<u8> bytes;
    f64 x = 0, y = 0;
};
// The object's snapshot; gives it a LevelId when it has none.
ObjectSnapshot snapshot(Level& level, flecs::entity e);

// Makes the objects (create = true) or removes them (false); undo does the
// opposite. Placing, copying and deleting are all this.
class ObjectsCommand final : public editor::Command {
public:
    ObjectsCommand(Level& level, std::vector<ObjectSnapshot> objects, bool create, std::string label);
    void apply(editor::Document&) override { set(create_); }
    void revert(editor::Document&) override { set(!create_); }
    std::string label() const override { return label_; }

private:
    void set(bool present);
    Level& level_;
    std::vector<ObjectSnapshot> objects_;
    bool create_;
    std::string label_;
};

// Moves objects by their centres. Moves of the same objects merge while the
// history is not sealed (one entry for a whole drag).
class MoveObjects final : public editor::Command {
public:
    struct Move {
        u64 id;
        f64 from_x, from_y, to_x, to_y;
    };
    MoveObjects(Level& level, std::vector<Move> moves, std::string label);
    void apply(editor::Document&) override { set(true); }
    void revert(editor::Document&) override { set(false); }
    std::string label() const override { return label_; }
    std::string merge_key() const override { return key_; }
    bool try_merge(const editor::Command& next) override;

private:
    void set(bool to);
    Level& level_;
    std::vector<Move> moves_;
    std::string label_, key_;
};

// Replaces one saved component of an object (JSON of the whole component).
// Edits of the same field merge while the history is not sealed.
class SetObjectComponent final : public editor::Command {
public:
    SetObjectComponent(Level& level, u64 id, f64 x, f64 y, const reflect::TypeInfo* type, std::string before,
                       std::string after, std::string field, std::string label);
    void apply(editor::Document&) override { set(after_); }
    void revert(editor::Document&) override { set(before_); }
    std::string label() const override { return label_; }
    std::string merge_key() const override;
    bool try_merge(const editor::Command& next) override;

private:
    void set(const std::string& json);
    Level& level_;
    u64 id_;
    f64 x_, y_;
    const reflect::TypeInfo* type_;
    std::string before_, after_, field_, label_;
};

// Sets a property of a template's copy (objects::Library::set_value): the
// value becomes the copy's own, unless it is the template's again. Edits of
// the same property merge while the history is not sealed.
class SetObjectProp final : public editor::Command {
public:
    SetObjectProp(Level& level, const objects::Library& library, u64 id, const objects::PropDef& prop, std::string value,
                  std::string label);
    void apply(editor::Document&) override;
    void revert(editor::Document&) override { set(before_part_, before_ref_); }
    std::string label() const override { return label_; }
    std::string merge_key() const override { return "prop:" + std::to_string(id_) + ":" + prop_.id; }
    bool try_merge(const editor::Command& next) override;

private:
    flecs::entity find();
    void set(const std::string& part, const std::string& ref);
    Level& level_;
    const objects::Library& library_;
    u64 id_;
    f64 x_ = 0, y_ = 0;
    const objects::PropDef& prop_;
    std::string value_, label_;
    bool done_ = false;
    std::string before_part_, before_ref_, after_part_, after_ref_;
};

// A component of an entity as JSON ("" when it has none), and back.
std::string component_json(Level& level, flecs::entity e, const reflect::TypeInfo* type);
bool set_component_json(Level& level, flecs::entity e, const reflect::TypeInfo* type, const std::string& json);

} // namespace forge::level
