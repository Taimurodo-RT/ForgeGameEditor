#include "forge/level/object_edit.h"

#include "forge/data/json.h"

#include <cmath>
#include <cstddef>

namespace forge::level {

namespace {

world::Rect around(f64 x, f64 y) {
    const i32 tx = static_cast<i32>(std::floor(x)), ty = static_cast<i32>(std::floor(y));
    return {tx - 2, ty - 2, tx + 3, ty + 3};
}

// Loads the place around a point unless it is loaded already.
void need(Level& level, f64 x, f64 y) {
    const world::Rect r = around(x, y);
    for (i32 cy = r.y0; cy < r.y1; cy += 2)
        for (i32 cx = r.x0; cx < r.x1; cx += 2)
            if (!level.loaded(cx, cy)) {
                level.ensure_loaded(r);
                return;
            }
}

flecs::entity_t component_id(Level& level, const reflect::TypeInfo* type) {
    for (const auto& c : level.scene().saved_components())
        if (c.type == type) return c.id;
    return 0;
}

} // namespace

ObjectSnapshot snapshot(Level& level, flecs::entity e) {
    ObjectSnapshot s;
    s.id = level.id_of(e, true);
    if (const scene::Position* p = e.try_get<scene::Position>()) {
        s.x = p->tile_x();
        s.y = p->tile_y();
    }
    s.bytes = level.scene().pack(e);
    return s;
}

ObjectsCommand::ObjectsCommand(Level& level, std::vector<ObjectSnapshot> objects, bool create, std::string label)
    : level_(level), objects_(std::move(objects)), create_(create), label_(std::move(label)) {}

void ObjectsCommand::set(bool present) {
    for (const ObjectSnapshot& o : objects_) {
        need(level_, o.x, o.y);
        flecs::entity e = level_.find(o.id);
        if (present && !e.is_valid()) level_.scene().unpack(o.bytes);
        if (!present && e.is_valid()) e.destruct();
    }
    level_.touch_objects();
}

MoveObjects::MoveObjects(Level& level, std::vector<Move> moves, std::string label)
    : level_(level), moves_(std::move(moves)), label_(std::move(label)) {
    key_ = "move";
    for (const Move& m : moves_) key_ += ":" + std::to_string(m.id);
}

void MoveObjects::set(bool to) {
    for (const Move& m : moves_) {
        const f64 x = to ? m.to_x : m.from_x, y = to ? m.to_y : m.from_y;
        // Both places: the object may be in either.
        need(level_, m.from_x, m.from_y);
        need(level_, m.to_x, m.to_y);
        flecs::entity e = level_.find(m.id);
        if (!e.is_valid()) continue;
        e.set<scene::Position>(scene::Position::at_tile(x, y));
        level_.module().object_moved(e);
    }
    level_.touch_objects();
}

bool MoveObjects::try_merge(const editor::Command& next) {
    const auto* n = static_cast<const MoveObjects*>(&next);
    if (!n || n->moves_.size() != moves_.size()) return false;
    for (usize i = 0; i < moves_.size(); ++i) {
        if (n->moves_[i].id != moves_[i].id) return false;
        moves_[i].to_x = n->moves_[i].to_x;
        moves_[i].to_y = n->moves_[i].to_y;
    }
    return true;
}

std::string component_json(Level& level, flecs::entity e, const reflect::TypeInfo* type) {
    const flecs::entity_t id = component_id(level, type);
    if (!id || !e.is_alive()) return {};
    const void* data = ecs_get_id(level.scene().ecs().c_ptr(), e, id);
    return data ? data::to_json(type, data, false) : std::string();
}

bool set_component_json(Level& level, flecs::entity e, const reflect::TypeInfo* type, const std::string& json) {
    const flecs::entity_t id = component_id(level, type);
    if (!id || !e.is_alive()) return false;
    std::vector<std::max_align_t> scratch((type->size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t) + 1);
    void* object = scratch.data();
    type->construct(object);
    data::LoadReport report;
    const bool ok = data::from_json(type, object, json, report);
    if (ok) ecs_set_id(level.scene().ecs().c_ptr(), e, id, type->size, object);
    type->destruct(object);
    return ok;
}

SetObjectComponent::SetObjectComponent(Level& level, u64 id, f64 x, f64 y, const reflect::TypeInfo* type,
                                       std::string before, std::string after, std::string field, std::string label)
    : level_(level), id_(id), x_(x), y_(y), type_(type), before_(std::move(before)), after_(std::move(after)),
      field_(std::move(field)), label_(std::move(label)) {}

void SetObjectComponent::set(const std::string& json) {
    need(level_, x_, y_);
    flecs::entity e = level_.find(id_);
    if (!e.is_valid()) return;
    set_component_json(level_, e, type_, json);
    if (type_ == reflect::type_of<scene::Position>()) level_.module().object_moved(e);
    if (const scene::Position* p = e.try_get<scene::Position>()) {
        x_ = p->tile_x();
        y_ = p->tile_y();
    }
    level_.touch_objects();
}

std::string SetObjectComponent::merge_key() const {
    if (field_.empty()) return {};
    return "set:" + std::to_string(id_) + ":" + type_->name + ":" + field_;
}

bool SetObjectComponent::try_merge(const editor::Command& next) {
    const auto* n = static_cast<const SetObjectComponent*>(&next);
    if (!n || n->id_ != id_ || n->type_ != type_ || n->field_ != field_) return false;
    after_ = n->after_;
    return true;
}

} // namespace forge::level
