#include "forge/modules/modules.h"

#include "forge/core/types.h"

namespace forge::modules {

std::vector<std::string> ModuleDef::event_ids() const {
    std::vector<std::string> out;
    for (const Event& e : events) out.push_back(e.id);
    return out;
}

bool Registry::add(ModuleDef def, std::string* error) {
    auto fail = [&](std::string why) {
        if (error) *error = std::move(why);
        return false;
    };
    if (def.id.empty()) return fail("у модуля нет id");
    if (find(def.id)) return fail("модуль «" + def.id + "» уже есть: два модуля с одним id");
    if (!def.run) return fail("у модуля «" + def.id + "» нет игры");
    if (!def.make_level_module) return fail("у модуля «" + def.id + "» нет вида уровня для редактора");
    for (usize i = 0; i < def.events.size(); ++i) {
        if (def.events[i].id.empty()) return fail("у модуля «" + def.id + "» событие без id");
        for (usize j = 0; j < i; ++j)
            if (def.events[j].id == def.events[i].id) return fail("у модуля «" + def.id + "» событие «" + def.events[i].id + "» дважды");
    }
    modules_.push_back(std::make_unique<ModuleDef>(std::move(def)));
    return true;
}

const ModuleDef* Registry::find(std::string_view id) const {
    for (const auto& m : modules_)
        if (m->id == id) return m.get();
    return nullptr;
}

std::vector<const ModuleDef*> Registry::all() const {
    std::vector<const ModuleDef*> out;
    for (const auto& m : modules_) out.push_back(m.get());
    return out;
}

std::string missing_module(std::string_view id) {
    return "игре нужен модуль «" + std::string(id) + "», его нет в этой сборке Forge";
}

} // namespace forge::modules
