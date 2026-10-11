#pragma once

// The modules built into this Forge (step 14.3a): the one place that names them. The launcher (apps/game) and the
// editor take their list from here, so both know the same modules, with the same events, values and files.

#include "forge/modules/modules.h"

#include <string>

namespace forge::builtin {

// Adds the built-in modules to registry: «Старая шахта» (slice); with tests also the test module «Проба» (probe),
// which a player's run never has. False (error says why) when one is refused (two of one id).
bool add_modules(modules::Registry& registry, bool tests, std::string* error = nullptr);

} // namespace forge::builtin
