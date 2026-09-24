#pragma once

#include <cstdint>

namespace rts::sim {

// Contador de ticks. A 20 Hz, 2^32 ticks son 6,8 años de partida.
using Tick = std::uint32_t;

// Frecuencia de la simulación. Es una constante de arquitectura (regla 3), no un
// parámetro de diseño: cambiarla invalida todas las magnitudes en ticks de data/
// y todas las repeticiones grabadas. Por eso vive aquí y no en TOML.
inline constexpr std::int32_t kTicksPerSecond = 20;

}  // namespace rts::sim
