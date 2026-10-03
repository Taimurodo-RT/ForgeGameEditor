#pragma once

#include "forge/core/log.h"

#include <cstdlib>

#if defined(_MSC_VER)
#define FORGE_DEBUG_BREAK() __debugbreak()
#elif defined(__GNUC__)
#define FORGE_DEBUG_BREAK() __builtin_trap()
#else
#define FORGE_DEBUG_BREAK() std::abort()
#endif

// FORGE_ASSERT is compiled out of release builds; FORGE_VERIFY always runs.
#define FORGE_VERIFY(cond, ...)                                        \
    do {                                                               \
        if (!(cond)) {                                                 \
            FORGE_ERROR("assertion failed: %s", #cond);                \
            FORGE_DEBUG_BREAK();                                       \
        }                                                              \
    } while (0)

#if defined(NDEBUG)
#define FORGE_ASSERT(cond) ((void)0)
#else
#define FORGE_ASSERT(cond) FORGE_VERIFY(cond)
#endif
