#pragma once

// Sistema de movimiento: órdenes, planificación (HPA* y campos de flujo) y evitación
// local (RVO muestreado). Determinista: enteros y Fixed, orden de iteración fijo.

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <entt/entity/registry.hpp>

#include "sim/path/flow_field.hpp"
#include "sim/path/grid.hpp"
#include "sim/path/hpa.hpp"
#include "sim/units.hpp"

namespace rts::sim {

class StateHasher;

struct MovementTickStats {
    std::int64_t nodes_expanded = 0;
    std::int32_t paths_solved = 0;
    std::int32_t paths_pending = 0;
    std::int32_t flow_fields_built = 0;
    std::int32_t moving_units = 0;
    // Cambios de ocupación aplicados en este tick (M3).
    std::int32_t sectors_rebuilt = 0;
    std::int32_t paths_invalidated = 0;
};

class MovementSystem {
public:
    MovementSystem(const TileMap& map, std::span<const std::uint8_t> passable_by_terrain,
                   const MovementParams& params);

    // Pone a cero las estadísticas del tick; se llama antes de aplicar las órdenes.
    void begin_tick() noexcept { stats_ = MovementTickStats{}; }
    // Mover unidades a una casilla: órdenes del jugador y desplazamientos internos
    // (un aldeano que va al árbol). Las unidades ya validadas por quien llama.
    void order_move(entt::registry& registry, std::span<const entt::entity> units, TileCoord target,
                    std::uint32_t order_id, Tick tick);
    static void stop(entt::registry& registry, std::span<const entt::entity> units);
    void update(entt::registry& registry, Tick tick);

    // Ocupación por objetos estáticos. Los cambios se acumulan y commit_grid_changes()
    // los aplica todos juntos: componentes conexas, sectores HPA* afectados, campos de
    // flujo cuyo pasillo tocan y caminos en curso que ahora atraviesan una casilla
    // bloqueada (se replanifican).
    void set_blocked(TileCoord c, bool blocked);
    // Puerta (B4): la casilla es de paso para las unidades de owner y cerrada para las
    // demás. kNoGate la vuelve una casilla normal.
    static constexpr std::int32_t kNoGate = -1;
    void set_gate(TileCoord c, std::int32_t owner);
    // Camino (C4): % de velocidad en la casilla c (0 lo quita: vuelve la del terreno).
    void set_road(TileCoord c, std::int32_t percent);
    // ¿Puede la unidad de owner entrar en la casilla c? (rejilla y puertas)
    [[nodiscard]] bool can_enter(TileCoord c, std::int32_t owner) const noexcept;
    [[nodiscard]] bool grid_dirty() const noexcept { return grid_dirty_; }
    void commit_grid_changes(entt::registry& registry);

    [[nodiscard]] const PassGrid& grid() const noexcept { return grid_; }
    [[nodiscard]] const HpaGraph& hpa() const noexcept { return hpa_; }
    [[nodiscard]] const MovementTickStats& last_stats() const noexcept { return stats_; }
    // Campo de flujo de una unidad, si lo sigue (para la superposición de depuración).
    [[nodiscard]] const FlowField* flow_field_of(const MoveGoal& goal) const noexcept;

    void hash_into(StateHasher& h) const;

    // % de velocidad del terreno en pos (B2; 100 sin terreno en combate).
    [[nodiscard]] std::int32_t terrain_speed_percent(FVec2 pos) const noexcept;
    // Velocidad de la unidad e en pos según el terreno (B2) y su cansancio y paso (B3).
    [[nodiscard]] Fixed effective_speed(const entt::registry& registry, entt::entity e, const Unit& unit,
                                        FVec2 pos) const noexcept;

private:
    struct CachedField {
        std::unique_ptr<FlowField> field;
        std::vector<std::uint8_t> corridor;  // sectores permitidos: para rehacerlo si cambia el mapa
        std::uint32_t order_id = 0;
        Tick last_used = 0;
    };

    // Datos por unidad de un tick, en estructura de arrays e índice denso.
    struct Scratch {
        std::vector<entt::entity> entity;
        std::vector<FVec2> pos;
        std::vector<FVec2> vel;
        std::vector<FVec2> pref;
        std::vector<Fixed> radius;
        std::vector<Fixed> speed;
        std::vector<std::uint32_t> order;   // 0 = sin orden
        std::vector<std::uint8_t> arrived;
        std::vector<std::int32_t> stuck;
        std::vector<std::uint8_t> waiting;  // esperando camino del planificador
        std::vector<Fixed> blob_radius;  // radio esperado del racimo de su orden
        std::vector<FVec2> goal_point;
        std::vector<std::int32_t> owner;  // jugador (-1 sin dueño): para las puertas
        std::vector<FVec2> chosen;
        std::vector<FVec2> correction;
        // Rejilla espacial por ordenación por conteo: cell_start[c]..cell_start[c+1].
        std::vector<std::uint32_t> cell_start;
        std::vector<std::uint32_t> cell_units;
        std::vector<std::uint32_t> neighbors;  // max_neighbors por unidad
        std::vector<std::uint32_t> neighbor_count;
    };

    void run_planner(entt::registry& registry);
    void enqueue_path(entt::entity e, PathFollow& follow);
    FVec2 preferred_velocity(entt::registry& registry, entt::entity e, FVec2 pos, const Unit& unit, MoveGoal& goal,
                             Tick tick);
    std::int32_t build_flow_field(entt::registry& registry, std::span<const entt::entity> units, TileCoord goal,
                                  std::uint32_t order_id, Tick tick);
    void build_spatial_grid(std::size_t n);
    void gather_neighbors(std::size_t n);
    FVec2 choose_velocity(std::size_t i) const;
    void integrate_and_separate(std::size_t n);

    [[nodiscard]] TileCoord tile_of(FVec2 p) const noexcept { return {p.x.floor_to_int(), p.y.floor_to_int()}; }
    [[nodiscard]] static FVec2 tile_center(TileCoord c) noexcept {
        return {Fixed::from_int(c.x) + Fixed::from_ratio(1, 2), Fixed::from_int(c.y) + Fixed::from_ratio(1, 2)};
    }

    MovementParams params_;
    std::int32_t width_ = 0;
    std::vector<std::int32_t> tile_speed_;
    std::vector<std::int32_t> gate_owner_;
    std::vector<std::int32_t> road_;  // por casilla: % de velocidad del camino (vacío = ninguno)  // por casilla: dueño de la puerta (vacío = ninguna)  // % de velocidad por casilla (vacío = 100 en todas)
    PassGrid grid_;
    GridSearch search_;
    HpaGraph hpa_;
    std::vector<CachedField> fields_;
    std::vector<std::uint8_t> dirty_sectors_;
    bool grid_dirty_ = false;
    std::vector<entt::entity> path_queue_;
    std::size_t queue_head_ = 0;
    Scratch s_;
    MovementTickStats stats_;
};

}  // namespace rts::sim
