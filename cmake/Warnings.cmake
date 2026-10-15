# forge_target_defaults(target [WITH_EXCEPTIONS])
# WITH_EXCEPTIONS: the target includes the C++ headers of a library built with exceptions and RTTI (Luau's syntax
# tree), so it is built the same way. Its inline copies of that library's types must match the library's own: MSVC
# lays std::exception out differently under _HAS_EXCEPTIONS=0, and the linker keeps one copy for the whole program.
function(forge_target_defaults target)
  cmake_parse_arguments(arg "WITH_EXCEPTIONS" "" "" ${ARGN})
  if(MSVC)
    # C4324: "structure was padded due to alignment specifier" is the intended
    # effect of alignas(kCacheLine), which keeps per-thread data on separate cache lines.
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:preprocessor /wd4324)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
  endif()
  # The runtime does not use exceptions or RTTI; errors are returned as values.
  if(arg_WITH_EXCEPTIONS)
    return()
  endif()
  if(MSVC)
    target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0)
  else()
    target_compile_options(${target} PRIVATE -fno-exceptions -fno-rtti)
  endif()
endfunction()
