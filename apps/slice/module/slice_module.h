#pragma once

// «Старая шахта» as a module of Forge (step 14.3a): the one header of apps/slice that the composition root
// (apps/builtin) includes. Everything else of apps/slice is the module's own: the core (engine/, the launcher
// apps/game, the editor) sees the module only through forge::modules::ModuleDef.

#include "forge/modules/modules.h"

namespace slice {

inline constexpr const char* kModuleId = "slice";

// id "slice", «Старая шахта»: its game (the old forge_slice's main), its level for the editor (SliceLevel), its files
// (games/modules/slice), the events of its verbs, the values of its game, its template «Платформер».
forge::modules::ModuleDef module_def();

} // namespace slice
