function(forge_target_defaults target)
  if(MSVC)
    # C4324: "structure was padded due to alignment specifier" is the intended
    # effect of alignas(kCacheLine), which keeps per-thread data on separate cache lines.
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:preprocessor /wd4324)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
  endif()
  # The runtime does not use exceptions or RTTI; errors are returned as values.
  if(MSVC)
    target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0)
  else()
    target_compile_options(${target} PRIVATE -fno-exceptions -fno-rtti)
  endif()
endfunction()
