#pragma once

// What the editor edits: a tree of named objects, each holding components
// described with FORGE_REFLECT. Every change goes through commands
// (commands.h) so it can be undone; the document itself only knows how to
// store objects and turn them into JSON and back.
//
// Objects keep their id for life, also across undo/redo of a delete, so
// selection, links and history entries stay valid.

#include "forge/core/types.h"
#include "forge/data/reflect.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace forge::editor {

using ObjectId = u64;
constexpr ObjectId kNoObject = 0;

class Component {
public:
    explicit Component(const reflect::TypeInfo* type);
    ~Component();
    Component(const Component&) = delete;
    Component& operator=(const Component&) = delete;

    const reflect::TypeInfo* type() const { return type_; }
    void* data() { return data_; }
    const void* data() const { return data_; }

private:
    const reflect::TypeInfo* type_;
    void* data_;
};

struct Object {
    ObjectId id = kNoObject;
    std::string name;
    ObjectId parent = kNoObject;
    std::vector<ObjectId> children;
    std::vector<std::unique_ptr<Component>> components;

    Component* find(const reflect::TypeInfo* type);
    const Component* find(const reflect::TypeInfo* type) const;
};

class Document {
public:
    Document();
    ~Document();
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    // --- objects ---
    // index: position among the parent's children (-1 = last). id 0 picks a
    // new id. Returns kNoObject if the parent does not exist or id is taken.
    ObjectId create(std::string_view name, ObjectId parent = kNoObject, i64 index = -1, ObjectId id = kNoObject);
    // An id no object has or will get from create(); for commands that must
    // recreate the same object on redo.
    ObjectId reserve_id() { return next_id_++; }
    // Removes the object and everything under it.
    bool destroy(ObjectId id);
    bool rename(ObjectId id, std::string_view name);
    // Moves an object under another parent (or to the root). Refuses to put an
    // object inside itself.
    bool reparent(ObjectId id, ObjectId parent, i64 index = -1);

    Object* find(ObjectId id);
    const Object* find(ObjectId id) const;
    const std::vector<ObjectId>& roots() const { return roots_; }
    const std::vector<ObjectId>& children_of(ObjectId parent) const; // kNoObject = roots
    i64 index_in_parent(ObjectId id) const;
    usize object_count() const { return objects_.size(); }

    // --- components ---
    void* add_component(ObjectId id, const reflect::TypeInfo* type, i64 index = -1);
    bool remove_component(ObjectId id, const reflect::TypeInfo* type);
    void* component(ObjectId id, const reflect::TypeInfo* type);
    template <typename T>
    T* component(ObjectId id) {
        return static_cast<T*>(component(id, reflect::type_of<T>()));
    }
    template <typename T>
    T* add(ObjectId id) {
        return static_cast<T*>(add_component(id, reflect::type_of<T>()));
    }
    i64 component_index(ObjectId id, const reflect::TypeInfo* type) const;

    // Component as JSON (compact) and back. set_component_json adds the
    // component if the object lacks it.
    std::string component_json(ObjectId id, const reflect::TypeInfo* type) const;
    bool set_component_json(ObjectId id, const reflect::TypeInfo* type, std::string_view json,
                            std::string* error = nullptr);

    // --- JSON of whole objects ---
    // An object with its components and, recursively, its children:
    //   {"id": "17", "name": "Дерево", "components": [{"$type": ...}], "children": [...]}
    // keep_ids = false leaves the ids out, so creating from it makes a copy.
    std::string object_json(ObjectId id, bool keep_ids = true) const;
    // Recreates an object (keeping its saved ids) under parent at index.
    ObjectId create_from_json(std::string_view json, ObjectId parent, i64 index, std::string* error = nullptr);

    // The whole document. load_json replaces everything.
    std::string save_json() const;
    bool load_json(std::string_view json, std::string* error = nullptr);
    void clear();

    // --- selection ---
    // Not saved with the document, but changed through commands so it is
    // part of the history. Deleting an object drops it from the selection.
    const std::vector<ObjectId>& selection() const { return selection_; }
    void set_selection(std::vector<ObjectId> ids);
    bool selected(ObjectId id) const;
    u64 selection_version() const { return selection_version_; }

    // --- change tracking ---
    // Bumped by every change; views compare it to know when to refresh.
    u64 version() const { return version_; }
    void touch() { ++version_; }
    // Bumped only when objects appear, disappear or move in the tree, or gain
    // or lose components (component values keep their address otherwise).
    u64 structure_version() const { return structure_version_; }

private:
    friend struct DocumentJson;
    std::vector<ObjectId>& children_ref(ObjectId parent);
    void destroy_recursive(ObjectId id);

    std::unordered_map<ObjectId, std::unique_ptr<Object>> objects_;
    std::vector<ObjectId> roots_;
    ObjectId next_id_ = 1;
    u64 version_ = 1;
    u64 structure_version_ = 1;
    std::vector<ObjectId> selection_;
    u64 selection_version_ = 1;
};

} // namespace forge::editor
