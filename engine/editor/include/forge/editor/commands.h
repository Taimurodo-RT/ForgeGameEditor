#pragma once

// The editor's actions as undoable commands. They work through reflection and
// JSON, so any component described with FORGE_REFLECT gets undo for free.

#include "forge/editor/document.h"
#include "forge/editor/undo.h"

#include <string>
#include <vector>

namespace forge::editor {

// Replaces a component's value (adds the component if missing). before/after
// are the component's JSON. Edits of the same field merge while the stack is
// not sealed: pass the field path (e.g. "speed") as field. label replaces
// the default "Изменить Type.field" in the history.
class SetComponent final : public Command {
public:
    SetComponent(ObjectId id, const reflect::TypeInfo* type, std::string before, std::string after,
                 std::string field = {}, std::string label = {});
    void apply(Document& doc) override;
    void revert(Document& doc) override;
    std::string label() const override;
    std::string merge_key() const override;
    bool try_merge(const Command& next) override;

private:
    ObjectId id_;
    const reflect::TypeInfo* type_;
    std::string before_, after_, field_, label_;
};

class AddComponent final : public Command {
public:
    // json: the starting value, or empty for the type's defaults.
    AddComponent(ObjectId id, const reflect::TypeInfo* type, std::string json = {});
    void apply(Document& doc) override;
    void revert(Document& doc) override;
    std::string label() const override;

private:
    ObjectId id_;
    const reflect::TypeInfo* type_;
    std::string json_;
};

class RemoveComponent final : public Command {
public:
    RemoveComponent(const Document& doc, ObjectId id, const reflect::TypeInfo* type);
    void apply(Document& doc) override;
    void revert(Document& doc) override;
    std::string label() const override;

private:
    ObjectId id_;
    const reflect::TypeInfo* type_;
    std::string json_;
    i64 index_;
};

// Creates an object from JSON (see Document::object_json), keeping its ids.
class CreateObject final : public Command {
public:
    CreateObject(std::string json, ObjectId parent, i64 index = -1, std::string label = "Создать объект");
    // A new empty object; reserves an id now so redo recreates the same one.
    static std::unique_ptr<CreateObject> named(Document& doc, std::string_view name, ObjectId parent,
                                               i64 index = -1);
    void apply(Document& doc) override;
    void revert(Document& doc) override;
    std::string label() const override { return label_; }
    ObjectId id() const { return id_; }

private:
    std::string json_;
    std::string name_; // when json_ is empty
    ObjectId parent_;
    i64 index_;
    std::string label_;
    ObjectId id_ = kNoObject;
};

// Deletes an object with everything under it; undo brings back the same ids.
class DeleteObject final : public Command {
public:
    DeleteObject(const Document& doc, ObjectId id);
    void apply(Document& doc) override;
    void revert(Document& doc) override;
    std::string label() const override;

private:
    ObjectId id_;
    std::string json_;
    std::string name_;
    ObjectId parent_;
    i64 index_;
};

class RenameObject final : public Command {
public:
    RenameObject(const Document& doc, ObjectId id, std::string name);
    void apply(Document& doc) override;
    void revert(Document& doc) override;
    std::string label() const override { return "Переименовать"; }
    std::string merge_key() const override;
    bool try_merge(const Command& next) override;

private:
    ObjectId id_;
    std::string before_, after_;
};

class MoveObject final : public Command {
public:
    MoveObject(const Document& doc, ObjectId id, ObjectId parent, i64 index = -1);
    void apply(Document& doc) override;
    void revert(Document& doc) override;
    std::string label() const override { return "Переместить"; }

private:
    ObjectId id_;
    ObjectId parent_, old_parent_;
    i64 index_, old_index_;
};

class Select final : public Command {
public:
    Select(const Document& doc, std::vector<ObjectId> ids);
    void apply(Document& doc) override { doc.set_selection(after_); }
    void revert(Document& doc) override { doc.set_selection(before_); }
    std::string label() const override { return "Выделение"; }
    bool changes_document() const override { return false; }

private:
    std::vector<ObjectId> before_, after_;
};

// Play / Stop: the document is remembered when the game starts and put back
// when it stops; changes made while playing are not recorded in the history.
class PlaySession {
public:
    bool start(Document& doc, UndoStack& history);
    bool stop(Document& doc, UndoStack& history);
    bool playing() const { return playing_; }
    usize snapshot_bytes() const { return snapshot_.size(); }

private:
    std::string snapshot_;
    std::vector<ObjectId> selection_;
    bool playing_ = false;
};

} // namespace forge::editor
