#pragma once

// The module «RPG сверху» (step 14.4a, 14.4-rpg-модель.md): the hero walks a map seen from above in eight directions,
// talks to villagers, picks things up, comes to places; quests are the game's dialogues and quests.json. What links it
// sees this header only: the rest of apps/rpg is the module's own.

#include "forge/modules/modules.h"

namespace rpg {

inline constexpr const char* kModuleId = "rpg";

forge::modules::ModuleDef module_def();

} // namespace rpg
