#include "slice_module.h"

#include "platformer_template.h"
#include "slice_level.h"
#include "slice_links.h"

#include "forge/core/path.h"
#include "forge/objects/library.h"

#ifndef SLICE_MODULE_DIR
#define SLICE_MODULE_DIR "games/modules/slice"
#endif

namespace slice {

// main.cpp: the game «Старая шахта», as forge_slice's main() was before step 14.3a.
int run(int argc, char** argv);

namespace {

// What screens can show of the game: the values it keeps in its variables, in the author's words. The fixed ones, and
// a thing a pickup gives (its «what») as inv.<what> with the template's name: the game counts each from 0 at a new
// game (SliceGame::begin), so every one of them is there to show.
std::vector<forge::modules::Value> values(const forge::objects::Library* lib) {
    std::vector<forge::modules::Value> out = {{"hero.hearts", "Сердца героя"}, {"hero.hearts_max", "Сердец всего"},
                                              {"hero.score", "Очки"},          {"inv.coins", "Монеты"},
                                              {"inv.copper", "Медь"}};
    if (!lib) return out;
    for (const forge::objects::Template& t : lib->templates()) {
        if (!lib->has_block(t, "pickup")) continue;
        const forge::objects::PropDef* what = lib->prop_of(t, "what");
        if (!what) continue;
        std::string item = lib->value(t, *what);
        if (item.size() >= 2 && item.front() == '"') item = item.substr(1, item.size() - 2);
        if (!item.empty()) out.push_back({"inv." + item, t.name});
    }
    return out;
}

} // namespace

forge::modules::ModuleDef module_def() {
    forge::modules::ModuleDef m;
    m.id = kModuleId;
    m.name = "Старая шахта";
    m.files = forge::utf8_path(SLICE_MODULE_DIR);
    m.events = verb_events();
    m.values = values;
    m.run = run;
    m.make_level_module = [] { return std::make_unique<SliceLevel>(); };
    m.templates.push_back({"platformer", platformer_template::make});
    return m;
}

} // namespace slice
