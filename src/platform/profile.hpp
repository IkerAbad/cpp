#pragma once

// Macros de perfilado. Sin RTS_TRACY se reducen a nada: coste cero en el binario.

#if defined(RTS_TRACY)
#include <tracy/Tracy.hpp>
#define RTS_PROFILE_ZONE() ZoneScoped
#define RTS_PROFILE_ZONE_NAMED(name) ZoneScopedN(name)
#define RTS_PROFILE_FRAME() FrameMark
#else
#define RTS_PROFILE_ZONE()
#define RTS_PROFILE_ZONE_NAMED(name)
#define RTS_PROFILE_FRAME()
#endif
