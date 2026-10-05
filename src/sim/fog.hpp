#pragma once

// Estado de una casilla en la niebla de guerra, tal como lo ve un jugador. Va aparte
// para que la presentación lo use sin depender del resto de la simulación.

#include <cstdint>

namespace rts::sim {

enum class Fog : std::uint8_t { Unexplored = 0, Explored = 1, Visible = 2 };

}  // namespace rts::sim
