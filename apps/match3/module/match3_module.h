#pragma once

// The module «Три в ряд» (step 14.3b, 14.3-три-в-ряд-модель.md): a field of gems, swapped with the mouse; rows of
// three and more vanish and score, gems fall, new ones come from above. What links it sees this header only: the rest
// of apps/match3 is the module's own.

#include "forge/modules/modules.h"

namespace match3 {

inline constexpr const char* kModuleId = "match3";

forge::modules::ModuleDef module_def();

} // namespace match3
