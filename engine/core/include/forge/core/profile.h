#pragma once

// Thin wrapper over Tracy so the rest of the engine never includes it directly.
// With FORGE_PROFILE=OFF every macro expands to nothing.
#include <tracy/Tracy.hpp>

#define FORGE_ZONE() ZoneScoped
#define FORGE_ZONE_N(name) ZoneScopedN(name)
#define FORGE_FRAME_MARK() FrameMark
#define FORGE_THREAD_NAME(name) tracy::SetThreadName(name)
#define FORGE_PLOT(name, value) TracyPlot(name, value)
