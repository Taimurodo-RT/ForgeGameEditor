# The composition root (step 14.3a): the list of the modules built into Forge, for the launcher and the editor alike.
# It sees each module's one public header (module/<id>_module.h), never the rest of a module.
add_library(forge_builtin_modules STATIC builtin.cpp)
target_include_directories(forge_builtin_modules PUBLIC .)
target_link_libraries(forge_builtin_modules PUBLIC forge::modules PRIVATE forge_slice_module forge_probe_module)
forge_target_defaults(forge_builtin_modules)
