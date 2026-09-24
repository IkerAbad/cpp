#pragma once

// HPA* (Botea, Müller y Schaeffer, 2004): el mapa se divide en sectores cuadrados;
// en cada borde compartido por dos sectores, cada tramo transitable contiguo se
// convierte en uno o más portales (dos nodos, uno a cada lado, unidos con coste 10).
// Dentro de cada sector se precalculan las distancias entre sus nodos. Una consulta
// larga es un A* sobre ese grafo pequeño; el camino fino se refina por tramos, cada
// uno acotado a uno o dos sectores.
//
// Cuando la ocupación cambia (M3: edificios, árboles talados), rebuild() rehace el
// grafo reutilizando las aristas internas de los sectores intactos. El resultado es
// idéntico, nodo a nodo y arista a arista, al de construirlo desde cero.

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "sim/path/grid.hpp"

namespace rts::sim {

struct HpaParams {
    std::int32_t sector_size = 0;        // casillas por lado del sector
    std::int32_t max_portal_width = 0;   // un tramo más ancho se parte en varios portales
};

class HpaGraph {
public:
    HpaGraph(const PassGrid& grid, const HpaParams& params, GridSearch& search);

    // Rehace el grafo tras cambios de transitabilidad. dirty_sectors (un byte por
    // sector) marca los sectores con alguna casilla cambiada. Los portales se vuelven a
    // detectar en todos los bordes (barato: solo recorre bordes); los Dijkstra internos
    // se repiten solo en los sectores sucios y en aquellos cuyo conjunto de nodos
    // cambió. Devuelve cuántos sectores recalcularon sus aristas internas.
    std::int32_t rebuild(const PassGrid& grid, std::span<const std::uint8_t> dirty_sectors, GridSearch& search);

    // Puntos de paso desde start (excluido) hasta goal (incluido): casillas de portal
    // intermedias y el destino. nullopt si goal no es alcanzable desde start.
    [[nodiscard]] std::optional<std::vector<TileCoord>> find_waypoints(const PassGrid& grid, TileCoord start,
                                                                       TileCoord goal, GridSearch& search);

    // Camino fino entre dos puntos de paso consecutivos (mismo sector o sectores
    // vecinos): A* acotado a la caja de ambos sectores.
    [[nodiscard]] std::optional<std::int32_t> refine(const PassGrid& grid, TileCoord from, TileCoord to,
                                                     GridSearch& search, std::vector<TileCoord>& out) const;

    [[nodiscard]] std::int32_t sector_of(TileCoord c) const noexcept {
        return (c.y / params_.sector_size) * sectors_w_ + c.x / params_.sector_size;
    }
    [[nodiscard]] SearchRect sector_rect(std::int32_t sector) const noexcept;
    [[nodiscard]] std::int32_t sectors_w() const noexcept { return sectors_w_; }
    [[nodiscard]] std::int32_t sectors_h() const noexcept { return sectors_h_; }
    [[nodiscard]] std::int32_t sector_size() const noexcept { return params_.sector_size; }
    [[nodiscard]] std::size_t node_count() const noexcept { return nodes_.size(); }
    [[nodiscard]] std::size_t edge_count() const noexcept { return edge_to_.size(); }
    [[nodiscard]] TileCoord node_tile(std::size_t n) const noexcept { return nodes_[n].tile; }
    [[nodiscard]] std::size_t sector_count() const noexcept { return sector_nodes_.size(); }

    // Comparación estructural exacta (pruebas: incremental frente a desde cero).
    [[nodiscard]] bool same_graph(const HpaGraph& other) const noexcept;

private:
    struct Node {
        TileCoord tile;
        std::int32_t sector;
    };
    struct TempEdge {
        std::uint32_t node;
        std::int32_t cost;
    };
    // Arista interna de un sector en índices locales (posición en sector_nodes_[s]):
    // no depende de la numeración global, así que sobrevive a un rebuild.
    struct IntraEdge {
        std::uint32_t from;
        std::uint32_t to;
        std::int32_t cost;
    };

    std::uint32_t add_node(const PassGrid& grid, TileCoord tile);
    void add_portals_on_border(const PassGrid& grid, std::vector<std::vector<TempEdge>>& adj, bool vertical_border,
                               std::int32_t fixed, std::int32_t from, std::int32_t to);
    void connect_temp(const PassGrid& grid, TileCoord tile, GridSearch& search, std::vector<TempEdge>& out) const;

    HpaParams params_;
    std::int32_t sectors_w_ = 0;
    std::int32_t sectors_h_ = 0;
    std::vector<Node> nodes_;
    std::vector<std::int32_t> tile_to_node_;               // -1 si la casilla no es nodo
    std::vector<std::vector<std::uint32_t>> sector_nodes_;
    std::vector<std::vector<IntraEdge>> sector_intra_;
    // Aristas en formato CSR: las de n están en [edge_begin_[n], edge_begin_[n + 1]).
    std::vector<std::uint32_t> edge_begin_;
    std::vector<std::uint32_t> edge_to_;
    std::vector<std::int32_t> edge_cost_;

    // Búferes de la búsqueda abstracta (sellados por generación, como GridSearch).
    std::vector<std::uint32_t> stamp_;
    std::vector<std::int32_t> g_;
    std::vector<std::uint32_t> parent_;
    std::vector<std::uint8_t> closed_;
    std::uint32_t generation_ = 0;
};

}  // namespace rts::sim
