// The registry of the genre modules (step 14.3a): a module is found by the id a game names, and the registry refuses
// what would make two modules one or a module without its game or its level.

#include "forge/modules/modules.h"

#include "forge/core/types.h"
#include "forge/level/level.h"

#include <doctest/doctest.h>

#include <memory>
#include <string>

using namespace forge;

namespace {

modules::ModuleDef module_named(std::string id) {
    modules::ModuleDef m;
    m.id = std::move(id);
    m.name = "Модуль " + m.id;
    m.events = {{"touch", "касается"}, {"always", "всегда"}};
    m.run = [](int, char**) { return 7; };
    m.make_level_module = [] { return std::unique_ptr<level::LevelModule>(); };
    return m;
}

} // namespace

TEST_CASE("modules: the registry finds a module by its id and refuses one it cannot tell apart or run") {
    modules::Registry reg;
    std::string error;
    REQUIRE(reg.add(module_named("slice"), &error));
    REQUIRE(reg.add(module_named("probe"), &error));
    REQUIRE(reg.find("slice"));
    CHECK(reg.find("slice")->name == "Модуль slice");
    CHECK(reg.find("probe")->run(0, nullptr) == 7);
    CHECK(reg.find("probe")->event_ids() == std::vector<std::string>{"touch", "always"});
    CHECK_FALSE(reg.find("match3"));
    CHECK_FALSE(reg.find(""));
    CHECK(modules::missing_module("match3") == "игре нужен модуль «match3», его нет в этой сборке Forge");
    // In the order added.
    REQUIRE(reg.all().size() == 2);
    CHECK(reg.all()[0]->id == "slice");
    CHECK(reg.all()[1]->id == "probe");
    // A found module stays where it is when others come.
    const modules::ModuleDef* slice = reg.find("slice");
    for (int i = 0; i < 20; ++i) REQUIRE(reg.add(module_named("m" + std::to_string(i)), &error));
    CHECK(reg.find("slice") == slice);

    // Refused, and nothing added: the same id twice, no id, no game, no level, an event twice or without an id.
    const usize had = reg.all().size();
    CHECK_FALSE(reg.add(module_named("slice"), &error));
    CHECK(error == "модуль «slice» уже есть: два модуля с одним id");
    CHECK(reg.find("slice") == slice);
    CHECK_FALSE(reg.add(module_named(""), &error));
    CHECK(error == "у модуля нет id");
    modules::ModuleDef no_run = module_named("no-run");
    no_run.run = nullptr;
    CHECK_FALSE(reg.add(std::move(no_run), &error));
    CHECK(error == "у модуля «no-run» нет игры");
    modules::ModuleDef no_level = module_named("no-level");
    no_level.make_level_module = nullptr;
    CHECK_FALSE(reg.add(std::move(no_level), &error));
    CHECK(error == "у модуля «no-level» нет вида уровня для редактора");
    modules::ModuleDef twice = module_named("twice");
    twice.events.push_back({"touch", "ещё раз"});
    CHECK_FALSE(reg.add(std::move(twice), &error));
    CHECK(error == "у модуля «twice» событие «touch» дважды");
    modules::ModuleDef unnamed = module_named("unnamed");
    unnamed.events.push_back({"", "без имени"});
    CHECK_FALSE(reg.add(std::move(unnamed), &error));
    CHECK(error == "у модуля «unnamed» событие без id");
    CHECK(reg.all().size() == had);
    CHECK_FALSE(reg.find("no-run"));
    CHECK_FALSE(reg.find("twice"));
}
