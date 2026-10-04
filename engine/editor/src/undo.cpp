#include "forge/editor/undo.h"

#include "forge/editor/document.h"

namespace forge::editor {

// A group is recorded as one command that applies its parts in order and
// reverts them in reverse.
struct UndoStack::Group final : Command {
    std::string text;
    std::vector<std::unique_ptr<Command>> parts;
    bool sealed = true;

    void apply(Document& doc) override {
        for (auto& c : parts) c->apply(doc);
    }
    void revert(Document& doc) override {
        for (auto it = parts.rbegin(); it != parts.rend(); ++it) (*it)->revert(doc);
    }
    std::string label() const override { return text; }
    bool changes_document() const override {
        for (const auto& c : parts)
            if (c->changes_document()) return true;
        return false;
    }
};

UndoStack::UndoStack(Document& doc) : doc_(doc) {}
UndoStack::~UndoStack() = default;

namespace {

// Merges next into last when both allow it; true when next was absorbed.
bool merge_into(Command* last, const Command& next) {
    if (!last) return false;
    const std::string key = next.merge_key();
    return !key.empty() && last->merge_key() == key && last->try_merge(next);
}

} // namespace

void UndoStack::execute(std::unique_ptr<Command> command) {
    if (!command) return;
    command->apply(doc_);
    if (!recording_) return;
    if (!groups_.empty()) {
        Group& group = *groups_.back();
        if (!group.sealed && !group.parts.empty() && merge_into(group.parts.back().get(), *command)) return;
        group.parts.push_back(std::move(command));
        group.sealed = false;
        return;
    }
    record(std::move(command));
}

void UndoStack::record(std::unique_ptr<Command> command) {
    const bool changes = command->changes_document();
    // Merge into the newest entry if it is the one just applied.
    if (!sealed_ && cursor_ == entries_.size() && cursor_ > 0 && merge_into(entries_.back().get(), *command)) {
        if (changes) states_.back() = next_state_++;
        ++version_;
        return;
    }
    // A new action drops whatever could be redone.
    entries_.resize(cursor_);
    states_.resize(cursor_);
    const u64 before = document_state_at(cursor_);
    entries_.push_back(std::move(command));
    states_.push_back(changes ? next_state_++ : before);
    cursor_ = entries_.size();
    if (entries_.size() > limit_) {
        const usize drop = entries_.size() - limit_;
        base_state_ = states_[drop - 1];
        entries_.erase(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(drop));
        states_.erase(states_.begin(), states_.begin() + static_cast<std::ptrdiff_t>(drop));
        cursor_ = entries_.size();
    }
    sealed_ = false;
    ++version_;
}

bool UndoStack::undo() {
    if (!groups_.empty() || cursor_ == 0) return false;
    entries_[--cursor_]->revert(doc_);
    sealed_ = true;
    ++version_;
    return true;
}

bool UndoStack::redo() {
    if (!groups_.empty() || cursor_ >= entries_.size()) return false;
    entries_[cursor_++]->apply(doc_);
    sealed_ = true;
    ++version_;
    return true;
}

std::string UndoStack::undo_label() const { return can_undo() ? entries_[cursor_ - 1]->label() : std::string(); }
std::string UndoStack::redo_label() const { return can_redo() ? entries_[cursor_]->label() : std::string(); }
std::string UndoStack::label_at(usize index) const { return index < entries_.size() ? entries_[index]->label() : std::string(); }

void UndoStack::begin_group(std::string label) {
    auto group = std::make_unique<Group>();
    group->text = std::move(label);
    groups_.push_back(std::move(group));
}

void UndoStack::end_group() {
    if (groups_.empty()) return;
    std::unique_ptr<Group> group = std::move(groups_.back());
    groups_.pop_back();
    if (group->parts.empty()) return;
    if (!groups_.empty()) {
        groups_.back()->parts.push_back(std::move(group));
        return;
    }
    // A group of one is just that command, so its label and merging stay.
    if (group->parts.size() == 1) {
        record(std::move(group->parts.front()));
        return;
    }
    sealed_ = true;
    record(std::move(group));
    sealed_ = true;
}

void UndoStack::clear() {
    entries_.clear();
    states_.clear();
    groups_.clear();
    cursor_ = 0;
    base_state_ = next_state_++;
    saved_state_ = base_state_;
    sealed_ = true;
    ++version_;
}

u64 UndoStack::document_state_at(usize cursor) const { return cursor == 0 ? base_state_ : states_[cursor - 1]; }

void UndoStack::mark_saved() {
    saved_state_ = document_state_at(cursor_);
    ++version_;
}

bool UndoStack::dirty() const { return document_state_at(cursor_) != saved_state_; }

} // namespace forge::editor
