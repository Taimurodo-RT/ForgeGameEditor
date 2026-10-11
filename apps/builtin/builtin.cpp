#include "builtin.h"

#include "probe_module.h"
#include "slice_module.h"

namespace forge::builtin {

bool add_modules(modules::Registry& registry, bool tests, std::string* error) {
    if (!registry.add(slice::module_def(), error)) return false;
    if (tests && !registry.add(probe::module_def(), error)) return false;
    return true;
}

} // namespace forge::builtin
