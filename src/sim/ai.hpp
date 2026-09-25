#pragma once

// IA básica (adelanto de M7). Vive dentro de la simulación: es determinista, corre
// igual en todas las máquinas y no hay que enviar sus órdenes por la red. Solo LEE el
// estado y emite las mismas órdenes que un jugador humano; no recibe recursos ni
// ventajas. La dificultad futura saldrá de jugar mejor, nunca de hacer trampas.

#include <array>
#include <cstdint>
#include <vector>

#include <entt/entity/registry.hpp>

#include "sim/economy.hpp"
#include "sim/path/grid.hpp"
#include "sim/tick.hpp"
#include "sim/units.hpp"

namespace rts::sim {

class StateHasher;

// data/config/engine.toml, sección [ai]. Tipos ya resueltos a ids.
struct AiParams {
    std::int32_t think_interval_ticks = 1;  // cada cuánto decide (repartido entre jugadores)
    std::int32_t villager_target = 0;
    std::array<std::int32_t, kResourceCount> gather_percent{};  // reparto de aldeanos por recurso
    std::int32_t house_margin = 0;           // construye casa cuando le quedan estas plazas o menos
    std::int32_t barracks_at_villagers = 0;
    std::int32_t dropoff_distance_tiles = 0; // almacén nuevo si el recurso queda más lejos que esto
    std::int32_t dropoff_min_gatherers = 0;  // ... y lo explotan al menos tantos aldeanos
    std::int32_t builders = 1;               // aldeanos que manda a cada obra
    std::int32_t gatherers_per_farm = 1;     // granjas deseadas = recolectores de comida deseados / esto
    std::int32_t build_gap_tiles = 0;        // casillas libres alrededor de sus edificios
    std::int32_t build_search_radius_tiles = 0;
    std::int32_t first_wave = 0;             // tamaño del primer ataque
    std::int32_t wave_growth = 0;            // cada oleada, tantas unidades más
    std::int32_t defend_radius_tiles = 0;    // enemigos a esta distancia de su base: defensa
    UnitTypeId worker_type = 0;
    std::vector<UnitTypeId> army;            // ciclo de entrenamiento en el cuartel
    BuildingTypeId house = 0;
    BuildingTypeId barracks = 0;
    BuildingTypeId farm = 0;
    std::array<BuildingTypeId, kResourceCount> dropoff{};  // almacén para cada recurso
};

// Estado propio de cada jugador controlado por la IA (entra en el hash).
struct AiPlayerState {
    PlayerId player = 0;
    std::int32_t wave_size = 0;
    std::int32_t army_cycle = 0;
    std::int32_t waves_sent = 0;
};

class AiSystem {
public:
    AiSystem(const AiParams& params, std::vector<PlayerId> players);

    // Decide para los jugadores a los que les toca este tick y añade sus órdenes a out
    // (se aplican en este mismo tick, después de las del jugador humano).
    void think(const entt::registry& registry, const EconomySystem& economy, const PassGrid& grid, Tick tick,
               std::vector<Command>& out);

    [[nodiscard]] const std::vector<AiPlayerState>& players() const noexcept { return players_; }
    void hash_into(StateHasher& h) const;

private:
    struct View;  // resumen del estado de un jugador, rehecho en cada decisión

    void think_player(const entt::registry& registry, const EconomySystem& economy, const PassGrid& grid,
                      AiPlayerState& ai, Tick tick, std::vector<Command>& out);

    AiParams params_;
    std::vector<AiPlayerState> players_;
};

}  // namespace rts::sim
