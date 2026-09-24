#pragma once

// Rejilla de transitabilidad y búsquedas sobre ella. Movimiento en 8 direcciones con
// coste octil entero (10 recto, 14 diagonal) y sin atajar esquinas: una diagonal solo
// es válida si las dos casillas ortogonales que roza también son transitables.

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "sim/tile_map.hpp"

namespace rts::sim {

inline constexpr std::int32_t kStraightCost = 10;
inline constexpr std::int32_t kDiagonalCost = 14;
inline constexpr std::int32_t kUnreached = std::numeric_limits<std::int32_t>::max();

struct Direction {
    std::int32_t dx;
    std::int32_t dy;
    std::int32_t cost;
};

// Orden fijo de vecinos: forma parte del determinismo de todas las búsquedas.
inline constexpr std::array<Direction, 8> kDirections{{
    {1, 0, kStraightCost},
    {0, 1, kStraightCost},
    {-1, 0, kStraightCost},
    {0, -1, kStraightCost},
    {1, 1, kDiagonalCost},
    {-1, 1, kDiagonalCost},
    {-1, -1, kDiagonalCost},
    {1, -1, kDiagonalCost},
}};

// Heurística octil admisible y consistente para los costes 10/14.
[[nodiscard]] constexpr std::int32_t octile_distance(TileCoord a, TileCoord b) noexcept {
    const std::int32_t dx = a.x > b.x ? a.x - b.x : b.x - a.x;
    const std::int32_t dy = a.y > b.y ? a.y - b.y : b.y - a.y;
    const std::int32_t lo = dx < dy ? dx : dy;
    const std::int32_t hi = dx < dy ? dy : dx;
    return kStraightCost * hi + (kDiagonalCost - kStraightCost) * lo;
}

// Transitabilidad = terreno transitable y sin objeto estático encima (edificio, árbol,
// mina). La capa de bloqueo vive aquí y no en el TileMap: el mapa del terreno no cambia
// durante la partida y los snapshots lo comparten sin copiarlo.
class PassGrid {
public:
    // passable_by_terrain[id] indica si el terreno id es transitable.
    PassGrid(const TileMap& map, std::span<const std::uint8_t> passable_by_terrain);

    [[nodiscard]] std::int32_t width() const noexcept { return width_; }
    [[nodiscard]] std::int32_t height() const noexcept { return height_; }
    [[nodiscard]] bool contains(TileCoord c) const noexcept {
        return c.x >= 0 && c.y >= 0 && c.x < width_ && c.y < height_;
    }
    [[nodiscard]] std::size_t index(TileCoord c) const noexcept {
        return static_cast<std::size_t>(c.y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(c.x);
    }
    [[nodiscard]] TileCoord coord(std::size_t i) const noexcept {
        return {static_cast<std::int32_t>(i % static_cast<std::size_t>(width_)),
                static_cast<std::int32_t>(i / static_cast<std::size_t>(width_))};
    }
    [[nodiscard]] bool passable(TileCoord c) const noexcept { return contains(c) && pass_[index(c)] != 0; }
    // Solo el terreno, sin objetos: dónde se puede colocar algo.
    [[nodiscard]] bool terrain_passable(TileCoord c) const noexcept {
        return contains(c) && terrain_pass_[index(c)] != 0;
    }
    [[nodiscard]] bool blocked(TileCoord c) const noexcept { return contains(c) && blocked_[index(c)] != 0; }

    // Marca o libera una casilla ocupada por un objeto estático. Las componentes quedan
    // desfasadas hasta relabel_components(): los cambios de un tick se agrupan.
    void set_blocked(TileCoord c, bool blocked) noexcept;
    void relabel_components() { label_components(); }

    // ¿Se puede ir de c a c + d en un paso? (incluye la regla de no atajar esquinas)
    [[nodiscard]] bool can_step(TileCoord c, const Direction& d) const noexcept;

    // Componente conexa de la casilla (0 = no transitable). Dos casillas son mutuamente
    // alcanzables si y solo si comparten componente.
    [[nodiscard]] std::uint32_t component(TileCoord c) const noexcept { return passable(c) ? comp_[index(c)] : 0; }
    // Casillas de una componente (0 si no existe).
    [[nodiscard]] std::uint32_t component_size(std::uint32_t component) const noexcept {
        return component < comp_size_.size() ? comp_size_[component] : 0;
    }

    // Casilla transitable más cercana a target (BFS en anillos) dentro de la componente
    // pedida. nullopt si no hay ninguna en un radio de max_radius casillas.
    [[nodiscard]] std::optional<TileCoord> nearest_in_component(TileCoord target, std::uint32_t component,
                                                                std::int32_t max_radius) const;

private:
    void label_components();

    std::int32_t width_;
    std::int32_t height_;
    std::vector<std::uint8_t> terrain_pass_;
    std::vector<std::uint8_t> blocked_;
    std::vector<std::uint8_t> pass_;  // terrain_pass_ && !blocked_
    std::vector<std::uint32_t> comp_;
    std::vector<std::uint32_t> comp_size_;  // por componente; [0] sin uso
};

// Rectángulo de casillas [x0, x1) x [y0, y1) al que se restringe una búsqueda.
struct SearchRect {
    std::int32_t x0 = 0;
    std::int32_t y0 = 0;
    std::int32_t x1 = 0;
    std::int32_t y1 = 0;

    [[nodiscard]] constexpr bool contains(TileCoord c) const noexcept {
        return c.x >= x0 && c.y >= y0 && c.x < x1 && c.y < y1;
    }
};

// Máscara opcional de sectores permitidos dentro del rectángulo (para campos de flujo).
struct SectorMask {
    std::span<const std::uint8_t> allowed;  // un byte por sector, fila a fila
    std::int32_t sector_size = 0;
    std::int32_t sectors_w = 0;

    [[nodiscard]] bool contains(TileCoord c) const noexcept {
        if (allowed.empty()) {
            return true;
        }
        const auto s = static_cast<std::size_t>((c.y / sector_size) * sectors_w + c.x / sector_size);
        return allowed[s] != 0;
    }
};

// Búsquedas reutilizables sobre la rejilla. Los búferes se reservan una vez (W*H) y se
// invalidan con un sello de generación, así que cada consulta cuesta O(nodos visitados).
class GridSearch {
public:
    explicit GridSearch(std::int32_t width, std::int32_t height);

    // A* de start a goal dentro de rect. Devuelve el coste o nullopt. Si path no es
    // nulo, recibe las casillas desde la siguiente a start hasta goal incluida.
    std::optional<std::int32_t> astar(const PassGrid& grid, TileCoord start, TileCoord goal, const SearchRect& rect,
                                      std::vector<TileCoord>* path);

    // Dijkstra desde source dentro de rect (y de la máscara). Los costes quedan
    // disponibles en cost_at() hasta la siguiente búsqueda.
    void dijkstra(const PassGrid& grid, TileCoord source, const SearchRect& rect, const SectorMask& mask = {});

    [[nodiscard]] std::int32_t cost_at(const PassGrid& grid, TileCoord c) const noexcept;

    // Nodos expandidos en total desde la creación: mide el trabajo para el presupuesto.
    [[nodiscard]] std::int64_t expanded_total() const noexcept { return expanded_total_; }

private:
    struct HeapNode {
        std::int32_t f;
        std::int32_t h;
        std::uint32_t index;
    };

    void begin(std::size_t tile_count);
    void push(HeapNode n);
    HeapNode pop();
    [[nodiscard]] bool visited(std::size_t i) const noexcept { return stamp_[i] == generation_; }

    std::vector<std::uint32_t> stamp_;
    std::vector<std::int32_t> g_;
    std::vector<std::uint32_t> parent_;
    std::vector<std::uint8_t> closed_;
    std::vector<HeapNode> heap_;
    std::uint32_t generation_ = 0;
    std::int64_t expanded_total_ = 0;
};

}  // namespace rts::sim
