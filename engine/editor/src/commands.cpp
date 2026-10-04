#include "forge/editor/commands.h"

#include "forge/core/log.h"

namespace forge::editor {

namespace {

std::string type_label(const reflect::TypeInfo* type) { return type ? type->name : std::string("?"); }

// Short name of a type: "forge::sim::Body" -> "Body".
std::string short_name(const reflect::TypeInfo* type) {
    const std::string name = type_label(type);
    const usize colon = name.rfind("::");
    return colon == std::string::npos ? name : name.substr(colon + 2);
}

void set_json(Document& doc, ObjectId id, const reflect::TypeInfo* type, const std::string& json) {
    std::string error;
    if (!doc.set_component_json(id, type, json, &error))
        FORGE_WARN("editor: cannot set %s on object %llu: %s", type_label(type).c_str(),
                   static_cast<unsigned long long>(id), error.c_str());
}

} // namespace

// --- SetComponent ----------------------------------------------------------

SetComponent::SetComponent(ObjectId id, const reflect::TypeInfo* type, std::string before, std::string after,
                           std::string field, std::string label)
    : id_(id), type_(type), before_(std::move(before)), after_(std::move(after)), field_(std::move(field)),
      label_(std::move(label)) {}

void SetComponent::apply(Document& doc) { set_json(doc, id_, type_, after_); }

void SetComponent::revert(Document& doc) {
    if (before_.empty()) doc.remove_component(id_, type_);
    else set_json(doc, id_, type_, before_);
}

std::string SetComponent::label() const {
    if (!label_.empty()) return label_;
    return "Изменить " + short_name(type_) + (field_.empty() ? std::string() : "." + field_);
}

std::string SetComponent::merge_key() const {
    if (field_.empty()) return {};
    return "set:" + std::to_string(id_) + ":" + type_label(type_) + ":" + field_;
}

bool SetComponent::try_merge(const Command& next) {
    // Equal merge keys mean the same command type (no RTTI in the engine).
    const auto* other = static_cast<const SetComponent*>(&next);
    after_ = other->after_;
    return true;
}

// --- AddComponent / RemoveComponent ------------------------------------------

AddComponent::AddComponent(ObjectId id, const reflect::TypeInfo* type, std::string json)
    : id_(id), type_(type), json_(std::move(json)) {}

void AddComponent::apply(Document& doc) {
    if (json_.empty()) doc.add_component(id_, type_);
    else set_json(doc, id_, type_, json_);
}

void AddComponent::revert(Document& doc) { doc.remove_component(id_, type_); }

std::string AddComponent::label() const { return "Добавить " + short_name(type_); }

RemoveComponent::RemoveComponent(const Document& doc, ObjectId id, const reflect::TypeInfo* type)
    : id_(id), type_(type), json_(doc.component_json(id, type)), index_(doc.component_index(id, type)) {}

void RemoveComponent::apply(Document& doc) { doc.remove_component(id_, type_); }

void RemoveComponent::revert(Document& doc) {
    if (json_.empty()) return;
    doc.add_component(id_, type_, index_);
    set_json(doc, id_, type_, json_);
}

std::string RemoveComponent::label() const { return "Убрать " + short_name(type_); }

// --- objects -----------------------------------------------------------------

CreateObject::CreateObject(std::string json, ObjectId parent, i64 index, std::string label)
    : json_(std::move(json)), parent_(parent), index_(index), label_(std::move(label)) {}

std::unique_ptr<CreateObject> CreateObject::named(Document& doc, std::string_view name, ObjectId parent, i64 index) {
    auto command = std::make_unique<CreateObject>(std::string(), parent, index);
    command->name_ = name;
    command->id_ = doc.reserve_id();
    return command;
}

void CreateObject::apply(Document& doc) {
    if (json_.empty()) {
        doc.create(name_, parent_, index_, id_);
        return;
    }
    std::string error;
    id_ = doc.create_from_json(json_, parent_, index_, &error);
    if (id_ == kNoObject) FORGE_WARN("editor: cannot create object: %s", error.c_str());
    // A copy made without ids got new ones; remember them so redo recreates
    // the very same objects that later commands refer to.
    else json_ = doc.object_json(id_);
}

void CreateObject::revert(Document& doc) { doc.destroy(id_); }

DeleteObject::DeleteObject(const Document& doc, ObjectId id)
    : id_(id), json_(doc.object_json(id)), parent_(kNoObject), index_(doc.index_in_parent(id)) {
    if (const Object* o = doc.find(id)) {
        name_ = o->name;
        parent_ = o->parent;
    }
}

void DeleteObject::apply(Document& doc) { doc.destroy(id_); }

void DeleteObject::revert(Document& doc) {
    if (json_.empty()) return;
    std::string error;
    if (doc.create_from_json(json_, parent_, index_, &error) == kNoObject)
        FORGE_WARN("editor: cannot restore object: %s", error.c_str());
}

std::string DeleteObject::label() const { return "Удалить «" + name_ + "»"; }

RenameObject::RenameObject(const Document& doc, ObjectId id, std::string name) : id_(id), after_(std::move(name)) {
    if (const Object* o = doc.find(id)) before_ = o->name;
}

void RenameObject::apply(Document& doc) { doc.rename(id_, after_); }
void RenameObject::revert(Document& doc) { doc.rename(id_, before_); }
std::string RenameObject::merge_key() const { return "rename:" + std::to_string(id_); }

bool RenameObject::try_merge(const Command& next) {
    // Equal merge keys mean the same command type (no RTTI in the engine).
    const auto* other = static_cast<const RenameObject*>(&next);
    after_ = other->after_;
    return true;
}

MoveObject::MoveObject(const Document& doc, ObjectId id, ObjectId parent, i64 index)
    : id_(id), parent_(parent), old_parent_(kNoObject), index_(index), old_index_(doc.index_in_parent(id)) {
    if (const Object* o = doc.find(id)) old_parent_ = o->parent;
}

void MoveObject::apply(Document& doc) { doc.reparent(id_, parent_, index_); }
void MoveObject::revert(Document& doc) { doc.reparent(id_, old_parent_, old_index_); }

Select::Select(const Document& doc, std::vector<ObjectId> ids) : before_(doc.selection()), after_(std::move(ids)) {}

// --- Play --------------------------------------------------------------------

bool PlaySession::start(Document& doc, UndoStack& history) {
    if (playing_) return false;
    history.seal();
    snapshot_ = doc.save_json();
    selection_ = doc.selection();
    history.set_recording(false);
    playing_ = true;
    return true;
}

bool PlaySession::stop(Document& doc, UndoStack& history) {
    if (!playing_) return false;
    std::string error;
    if (!doc.load_json(snapshot_, &error)) FORGE_ERROR("editor: cannot restore the scene after Play: %s", error.c_str());
    doc.set_selection(selection_);
    snapshot_.clear();
    snapshot_.shrink_to_fit();
    history.set_recording(true);
    playing_ = false;
    return true;
}

} // namespace forge::editor
