#include "forge/editor/document.h"

#include "forge/core/log.h"
#include "forge/core/profile.h"
#include "forge/data/json.h"

#include <yyjson.h>

#include <algorithm>
#include <cstdlib>
#include <new>

namespace forge::editor {

// ---------------------------------------------------------------------------
// Component

Component::Component(const reflect::TypeInfo* type) : type_(type) {
    data_ = ::operator new(type->size, std::align_val_t(type->align));
    type->construct(data_);
}

Component::~Component() {
    type_->destruct(data_);
    ::operator delete(data_, std::align_val_t(type_->align));
}

Component* Object::find(const reflect::TypeInfo* type) {
    for (auto& c : components)
        if (c->type() == type) return c.get();
    return nullptr;
}

const Component* Object::find(const reflect::TypeInfo* type) const {
    for (const auto& c : components)
        if (c->type() == type) return c.get();
    return nullptr;
}

// ---------------------------------------------------------------------------
// Objects

Document::Document() = default;
Document::~Document() = default;

namespace {

void insert_at(std::vector<ObjectId>& list, ObjectId id, i64 index) {
    if (index < 0 || index >= static_cast<i64>(list.size())) list.push_back(id);
    else list.insert(list.begin() + index, id);
}

const std::vector<ObjectId> kEmpty;

} // namespace

std::vector<ObjectId>& Document::children_ref(ObjectId parent) {
    if (parent == kNoObject) return roots_;
    return objects_.at(parent)->children;
}

const std::vector<ObjectId>& Document::children_of(ObjectId parent) const {
    if (parent == kNoObject) return roots_;
    const Object* p = find(parent);
    return p ? p->children : kEmpty;
}

ObjectId Document::create(std::string_view name, ObjectId parent, i64 index, ObjectId id) {
    if (parent != kNoObject && !find(parent)) return kNoObject;
    if (id == kNoObject) id = next_id_;
    if (objects_.count(id)) return kNoObject;
    next_id_ = std::max(next_id_, id + 1);
    auto object = std::make_unique<Object>();
    object->id = id;
    object->name = name;
    object->parent = parent;
    objects_.emplace(id, std::move(object));
    insert_at(children_ref(parent), id, index);
    ++structure_version_;
    touch();
    return id;
}

void Document::destroy_recursive(ObjectId id) {
    auto it = objects_.find(id);
    if (it == objects_.end()) return;
    for (ObjectId child : it->second->children) destroy_recursive(child);
    objects_.erase(it);
}

bool Document::destroy(ObjectId id) {
    Object* object = find(id);
    if (!object) return false;
    auto& siblings = children_ref(object->parent);
    siblings.erase(std::remove(siblings.begin(), siblings.end(), id), siblings.end());
    destroy_recursive(id);
    const usize before = selection_.size();
    selection_.erase(std::remove_if(selection_.begin(), selection_.end(), [&](ObjectId s) { return !find(s); }),
                     selection_.end());
    if (selection_.size() != before) ++selection_version_;
    ++structure_version_;
    touch();
    return true;
}

void Document::set_selection(std::vector<ObjectId> ids) {
    if (ids == selection_) return;
    selection_ = std::move(ids);
    ++selection_version_;
}

bool Document::selected(ObjectId id) const {
    return std::find(selection_.begin(), selection_.end(), id) != selection_.end();
}

bool Document::rename(ObjectId id, std::string_view name) {
    Object* object = find(id);
    if (!object) return false;
    object->name = name;
    touch();
    return true;
}

bool Document::reparent(ObjectId id, ObjectId parent, i64 index) {
    Object* object = find(id);
    if (!object || (parent != kNoObject && !find(parent))) return false;
    for (ObjectId p = parent; p != kNoObject; p = find(p)->parent)
        if (p == id) return false; // into itself
    auto& old_list = children_ref(object->parent);
    old_list.erase(std::remove(old_list.begin(), old_list.end(), id), old_list.end());
    object->parent = parent;
    insert_at(children_ref(parent), id, index);
    ++structure_version_;
    touch();
    return true;
}

Object* Document::find(ObjectId id) {
    auto it = objects_.find(id);
    return it != objects_.end() ? it->second.get() : nullptr;
}

const Object* Document::find(ObjectId id) const {
    auto it = objects_.find(id);
    return it != objects_.end() ? it->second.get() : nullptr;
}

i64 Document::index_in_parent(ObjectId id) const {
    const Object* object = find(id);
    if (!object) return -1;
    const auto& list = children_of(object->parent);
    auto it = std::find(list.begin(), list.end(), id);
    return it != list.end() ? it - list.begin() : -1;
}

void Document::clear() {
    objects_.clear();
    roots_.clear();
    selection_.clear();
    ++selection_version_;
    ++structure_version_;
    next_id_ = 1;
    touch();
}

// ---------------------------------------------------------------------------
// Components

void* Document::add_component(ObjectId id, const reflect::TypeInfo* type, i64 index) {
    Object* object = find(id);
    if (!object || !type || type->kind != reflect::Kind::Struct) return nullptr;
    if (Component* existing = object->find(type)) return existing->data();
    auto component = std::make_unique<Component>(type);
    void* data = component->data();
    if (index < 0 || index >= static_cast<i64>(object->components.size()))
        object->components.push_back(std::move(component));
    else
        object->components.insert(object->components.begin() + index, std::move(component));
    ++structure_version_;
    touch();
    return data;
}

bool Document::remove_component(ObjectId id, const reflect::TypeInfo* type) {
    Object* object = find(id);
    if (!object) return false;
    auto& list = object->components;
    auto it = std::find_if(list.begin(), list.end(), [&](const auto& c) { return c->type() == type; });
    if (it == list.end()) return false;
    list.erase(it);
    ++structure_version_;
    touch();
    return true;
}

void* Document::component(ObjectId id, const reflect::TypeInfo* type) {
    Object* object = find(id);
    Component* c = object ? object->find(type) : nullptr;
    return c ? c->data() : nullptr;
}

i64 Document::component_index(ObjectId id, const reflect::TypeInfo* type) const {
    const Object* object = find(id);
    if (!object) return -1;
    for (usize i = 0; i < object->components.size(); ++i)
        if (object->components[i]->type() == type) return static_cast<i64>(i);
    return -1;
}

std::string Document::component_json(ObjectId id, const reflect::TypeInfo* type) const {
    const Object* object = find(id);
    const Component* c = object ? object->find(type) : nullptr;
    return c ? data::to_json(type, c->data(), false) : std::string();
}

bool Document::set_component_json(ObjectId id, const reflect::TypeInfo* type, std::string_view json,
                                  std::string* error) {
    void* data = component(id, type);
    if (!data) data = add_component(id, type);
    if (!data) {
        if (error) *error = "no object " + std::to_string(id);
        return false;
    }
    // Check the JSON on a scratch value first, so a bad edit leaves the
    // component untouched. Then reset the live value and read it, so fields
    // missing from the JSON get their defaults, exactly as when loading.
    Component scratch(type);
    data::LoadReport report;
    if (!data::from_json(type, scratch.data(), json, report)) {
        if (error) *error = report.error;
        return false;
    }
    type->destruct(data);
    type->construct(data);
    data::from_json(type, data, json, report);
    touch();
    return true;
}

// ---------------------------------------------------------------------------
// JSON

struct DocumentJson {
    static yyjson_mut_val* write_object(const Document& doc, yyjson_mut_doc* out, const Object& object,
                                        bool keep_ids = true) {
        yyjson_mut_val* o = yyjson_mut_obj(out);
        if (keep_ids) yyjson_mut_obj_add_strcpy(out, o, "id", std::to_string(object.id).c_str());
        yyjson_mut_obj_add_strncpy(out, o, "name", object.name.data(), object.name.size());
        yyjson_mut_val* components = yyjson_mut_obj_add_arr(out, o, "components");
        for (const auto& c : object.components) {
            const std::string text = data::to_json(c->type(), c->data(), false);
            yyjson_doc* parsed = yyjson_read(text.data(), text.size(), 0);
            if (!parsed) continue;
            yyjson_mut_arr_append(components, yyjson_val_mut_copy(out, yyjson_doc_get_root(parsed)));
            yyjson_doc_free(parsed);
        }
        if (!object.children.empty()) {
            yyjson_mut_val* children = yyjson_mut_obj_add_arr(out, o, "children");
            for (ObjectId child : object.children)
                if (const Object* c = doc.find(child))
                    yyjson_mut_arr_append(children, write_object(doc, out, *c, keep_ids));
        }
        return o;
    }

    static ObjectId read_object(Document& doc, yyjson_val* o, ObjectId parent, i64 index, std::string* error) {
        if (!yyjson_is_obj(o)) {
            if (error) *error = "object expected";
            return kNoObject;
        }
        ObjectId id = kNoObject;
        if (yyjson_val* v = yyjson_obj_get(o, "id"); yyjson_is_str(v)) id = std::strtoull(yyjson_get_str(v), nullptr, 10);
        const char* name = yyjson_get_str(yyjson_obj_get(o, "name"));
        const ObjectId created = doc.create(name ? name : "", parent, index, id);
        if (created == kNoObject) {
            if (error) *error = "cannot create object " + std::to_string(id);
            return kNoObject;
        }
        size_t i, n;
        yyjson_val* c;
        yyjson_arr_foreach(yyjson_obj_get(o, "components"), i, n, c) {
            const char* type_name = yyjson_get_str(yyjson_obj_get(c, "$type"));
            const reflect::TypeInfo* type = type_name ? reflect::find_type(type_name) : nullptr;
            if (!type) {
                FORGE_WARN("editor: unknown component type '%s' skipped", type_name ? type_name : "?");
                continue;
            }
            usize len = 0;
            char* text = yyjson_val_write(c, 0, &len);
            if (!text) continue;
            void* data = doc.add_component(created, type);
            data::LoadReport report;
            data::from_json(type, data, std::string_view(text, len), report);
            for (const auto& w : report.warnings) FORGE_WARN("editor: %s: %s", type_name, w.c_str());
            std::free(text);
        }
        yyjson_val* child;
        yyjson_arr_foreach(yyjson_obj_get(o, "children"), i, n, child) {
            if (read_object(doc, child, created, -1, error) == kNoObject) return kNoObject;
        }
        return created;
    }

    static std::string write(yyjson_mut_doc* out, yyjson_mut_val* root) {
        yyjson_mut_doc_set_root(out, root);
        usize len = 0;
        char* text = yyjson_mut_write(out, YYJSON_WRITE_ALLOW_INVALID_UNICODE, &len);
        std::string result = text ? std::string(text, len) : std::string();
        std::free(text);
        yyjson_mut_doc_free(out);
        return result;
    }
};

std::string Document::object_json(ObjectId id, bool keep_ids) const {
    const Object* object = find(id);
    if (!object) return {};
    yyjson_mut_doc* out = yyjson_mut_doc_new(nullptr);
    return DocumentJson::write(out, DocumentJson::write_object(*this, out, *object, keep_ids));
}

ObjectId Document::create_from_json(std::string_view json, ObjectId parent, i64 index, std::string* error) {
    yyjson_doc* doc = yyjson_read(json.data(), json.size(), 0);
    if (!doc) {
        if (error) *error = "JSON syntax error";
        return kNoObject;
    }
    const ObjectId id = DocumentJson::read_object(*this, yyjson_doc_get_root(doc), parent, index, error);
    yyjson_doc_free(doc);
    return id;
}

std::string Document::save_json() const {
    FORGE_ZONE();
    yyjson_mut_doc* out = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(out);
    yyjson_mut_obj_add_strcpy(out, root, "next_id", std::to_string(next_id_).c_str());
    yyjson_mut_val* list = yyjson_mut_obj_add_arr(out, root, "objects");
    for (ObjectId id : roots_)
        if (const Object* o = find(id)) yyjson_mut_arr_append(list, DocumentJson::write_object(*this, out, *o));
    return DocumentJson::write(out, root);
}

bool Document::load_json(std::string_view json, std::string* error) {
    FORGE_ZONE();
    yyjson_doc* doc = yyjson_read(json.data(), json.size(), 0);
    if (!doc) {
        if (error) *error = "JSON syntax error";
        return false;
    }
    clear();
    yyjson_val* root = yyjson_doc_get_root(doc);
    bool ok = true;
    size_t i, n;
    yyjson_val* o;
    yyjson_arr_foreach(yyjson_obj_get(root, "objects"), i, n, o) {
        if (DocumentJson::read_object(*this, o, kNoObject, -1, error) == kNoObject) {
            ok = false;
            break;
        }
    }
    if (yyjson_val* v = yyjson_obj_get(root, "next_id"); yyjson_is_str(v))
        next_id_ = std::max(next_id_, static_cast<ObjectId>(std::strtoull(yyjson_get_str(v), nullptr, 10)));
    yyjson_doc_free(doc);
    touch();
    return ok;
}

} // namespace forge::editor
