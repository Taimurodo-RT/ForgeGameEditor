# Build-time shader compiler. Runs on the build machine, never ships.
add_executable(forge_shaderc main.cpp)
target_link_libraries(forge_shaderc PRIVATE glslang glslang-default-resource-limits spirv-cross-hlsl)
if(WIN32)
  target_link_libraries(forge_shaderc PRIVATE d3dcompiler)
endif()
