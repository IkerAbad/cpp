#pragma once

// Macros de perfilado. Sin REINOS_TRACY se reducen a nada: coste cero en el binario.

#if defined(REINOS_TRACY)
#include <tracy/Tracy.hpp>
#define REINOS_PROFILE_ZONE() ZoneScoped
#define REINOS_PROFILE_ZONE_NAMED(name) ZoneScopedN(name)
#define REINOS_PROFILE_FRAME() FrameMark
#else
#define REINOS_PROFILE_ZONE()
#define REINOS_PROFILE_ZONE_NAMED(name)
#define REINOS_PROFILE_FRAME()
#endif
