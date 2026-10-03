add_executable(forge_sandbox main.cpp)
target_link_libraries(forge_sandbox PRIVATE forge::platform)
forge_target_defaults(forge_sandbox)
