#pragma once

// History of editor actions: every change is a command that can be undone and
// redone.
//
//   stack.execute(std::make_unique<SetComponent>(...)); // applies and records
//   stack.undo(); stack.redo();
//
// Merging: dragging a slider sends a hundred edits of one field; they become
// one entry, because a command may absorb the next one with the same merge
// key. seal() ends the run (the editor calls it when the mouse is released),
// so the next drag is a new entry.
//
// Groups: begin_group()/end_group() collect several commands into one entry
// ("Удалить 12 объектов" is undone by one Ctrl+Z).
//
// While the game runs (Play), recording is off: commands still apply, but the
// history does not change; the scene snapshot taken at Play is restored on Stop.

#include "forge/core/types.h"

#include <memory>
#include <string>
#include <vector>

namespace forge::editor {

class Document;

class Command {
public:
    virtual ~Command() = default;
    virtual void apply(Document& doc) = 0;  // do and redo
    virtual void revert(Document& doc) = 0; // undo
    virtual std::string label() const = 0;  // shown in Edit menu and history

    // Commands with the same non-empty key may merge (see try_merge).
    virtual std::string merge_key() const { return {}; }
    // Called on the newest entry with the next command of the same key; true
    // when this command now includes next (next is already applied).
    virtual bool try_merge(const Command& /*next*/) { return false; }
    // False for commands that do not change saved data (selection).
    virtual bool changes_document() const { return true; }
};

class UndoStack {
public:
    explicit UndoStack(Document& doc);
    ~UndoStack();
    UndoStack(const UndoStack&) = delete;
    UndoStack& operator=(const UndoStack&) = delete;

    // Applies the command and records it (unless recording is off).
    void execute(std::unique_ptr<Command> command);
    // The next command never merges into the newest entry.
    void seal() { sealed_ = true; }

    bool undo();
    bool redo();
    bool can_undo() const { return cursor_ > 0; }
    bool can_redo() const { return cursor_ < entries_.size(); }
    std::string undo_label() const;
    std::string redo_label() const;

    void begin_group(std::string label);
    void end_group();
    bool in_group() const { return !groups_.empty(); }

    void set_recording(bool on) { recording_ = on; }
    bool recording() const { return recording_; }

    // Oldest entries are dropped beyond this many.
    void set_limit(usize entries) { limit_ = entries; }
    void clear();

    // Saved state: dirty() is true when the document differs from the last
    // mark_saved() (undoing back to that point makes it clean again).
    void mark_saved();
    bool dirty() const;

    // For a history panel: labels oldest first, and how many are applied.
    usize size() const { return entries_.size(); }
    usize cursor() const { return cursor_; }
    std::string label_at(usize index) const;
    // Increases on every change of the history.
    u64 version() const { return version_; }

private:
    struct Group;
    void record(std::unique_ptr<Command> command);
    u64 document_state_at(usize cursor) const;

    Document& doc_;
    std::vector<std::unique_ptr<Command>> entries_;
    std::vector<u64> states_; // per entry: id of the document state after it
    usize cursor_ = 0;        // entries [0, cursor_) are applied
    std::vector<std::unique_ptr<Group>> groups_;
    usize limit_ = 1000;
    bool sealed_ = true;
    bool recording_ = true;
    u64 next_state_ = 1;
    u64 base_state_ = 0;  // state before entries_[0]
    u64 saved_state_ = 0;
    u64 version_ = 1;
};

} // namespace forge::editor
