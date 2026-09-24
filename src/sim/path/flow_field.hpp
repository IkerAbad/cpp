#pragma once

// Campo de flujo: coste acumulado desde cada casilla hasta el destino, calculado con un
// único Dijkstra que parte del destino. Sirve a todo un grupo con una sola búsqueda:
// cada unidad baja por el gradiente desde su casilla.
//
// El Dijkstra se limita a un pasillo de sectores (los del camino abstracto y los que
// ocupan las unidades), no al mapa entero: O(casillas del pasillo).

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "sim/path/grid.hpp"

namespace rts::sim {

class FlowField {
public:
    FlowField(const PassGrid& grid, TileCoord goal, const SectorMask& corridor, GridSearch& search);

    [[nodiscard]] TileCoord goal() const noexcept { return goal_; }

    // Coste hasta el destino; kUnreached si la casilla quedó fuera del pasillo.
    [[nodiscard]] std::int32_t cost(const PassGrid& grid, TileCoord c) const noexcept;

    // Vecina con menor coste (orden fijo de kDirections en los empates). nullopt en el
    // destino o fuera del campo.
    [[nodiscard]] std::optional<TileCoord> next_step(const PassGrid& grid, TileCoord from) const noexcept;

    [[nodiscard]] std::size_t tiles_reached() const noexcept { return reached_; }

private:
    TileCoord goal_;
    std::vector<std::int32_t> cost_;
    std::size_t reached_ = 0;
};

}  // namespace rts::sim
