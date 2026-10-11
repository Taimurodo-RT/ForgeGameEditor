#pragma once

// «Проба», the test module (step 14.3a): a second module next to «Старая шахта», so the tests see the launcher and the
// editor take another module's game, level, events and values by the id a game names, and keep the two apart. It is
// built in only for tests (forge_game --test, forge_editor --self-test), never for a player.

#include "forge/modules/modules.h"

namespace probe {

inline constexpr const char* kModuleId = "probe";
// What its game clears the screen to, so a picture of it says which game ran (r, g, b).
inline constexpr unsigned char kColor[3] = {30, 140, 120};

forge::modules::ModuleDef module_def();

} // namespace probe
