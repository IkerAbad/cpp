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

// Módulos de comportamiento. Un perfil de IA es la lista ordenada de los que usa: la
// dificultad sale de qué módulos juegan y de lo bien que piensan, nunca de ventajas
// económicas (ningún parámetro de un perfil toca la economía ni las reglas).
enum class AiBehavior : std::uint8_t {
    Defend,     // el ejército ocioso sale a por los enemigos cerca de la base; los aldeanos huyen
    Villagers,  // aldeanos hasta el objetivo
    Houses,     // casa antes de quedarse sin plazas
    Barracks,   // cuartel al llegar a cierto número de aldeanos
    Farms,      // granjas cuando no queda comida natural cerca
    Dropoffs,   // almacenes junto a recursos lejanos que ya se explotan
    Builders,   // obras sin constructores suficientes
    Gather,     // aldeanos ociosos al recurso con más déficit
    Army,       // el cuartel entrena en ciclo
    Attack,     // oleadas crecientes en ataque-movimiento
    // Módulos que piensan mejor (perfil "normal").
    ArmyCounter,     // entrena el tipo que mejor rinde contra el ejército enemigo conocido
    AttackStrength,  // ataca cuando su fuerza supera a la enemiga; se retira si pierde la batalla
    FocusFire,       // en combate, cada unidad remata al enemigo armado que antes puede matar
    Count,
};

// Perfil de IA: estrategia y umbrales (data/config/engine.toml, [[ai.profile]]).
struct AiProfile {
    std::vector<AiBehavior> behaviors;       // en orden de ejecución
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
    std::int32_t flee_enemy_tiles = 0;       // un aldeano con un enemigo a esto huye a la base...
    std::int32_t safe_base_tiles = 0;        // ... salvo que ya esté a esto de ella
    std::int32_t barracks_queue = 1;         // unidades en cola que mantiene en el cuartel
    std::int32_t villager_queue = 1;         // aldeanos en cola que mantiene en el centro urbano
    // ataque_fuerza: ataca si su fuerza total es al menos attack_ratio_percent % de la
    // enemiga conocida y tiene min_attack_army unidades ociosas; se retira si, en un
    // radio de engage_radius_tiles alrededor de su ejército en campaña, su fuerza baja
    // del retreat_ratio_percent % de la enemiga (histéresis: no oscila).
    std::int32_t attack_ratio_percent = 0;
    std::int32_t retreat_ratio_percent = 0;
    std::int32_t min_attack_army = 0;
    std::int32_t engage_radius_tiles = 0;
    std::vector<UnitTypeId> army;            // ciclo de entrenamiento en el cuartel
};

// data/config/engine.toml, sección [ai]. Tipos ya resueltos a ids.
struct AiParams {
    std::int32_t think_interval_ticks = 1;  // cada cuánto decide (repartido entre jugadores)
    // Tipos que usa cualquier perfil (dependerán de la civilización, no de la dificultad).
    UnitTypeId worker_type = 0;
    BuildingTypeId house = 0;
    BuildingTypeId barracks = 0;
    BuildingTypeId farm = 0;
    std::array<BuildingTypeId, kResourceCount> dropoff{};  // almacén para cada recurso
    std::vector<AiProfile> profiles;
};

// Jugador controlado por la IA y su perfil (índice en AiParams::profiles).
struct AiSeat {
    PlayerId player = 0;
    std::uint8_t profile = 0;
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
    AiSystem(const AiParams& params, const std::vector<AiSeat>& seats);

    // Decide para los jugadores a los que les toca este tick y añade sus órdenes a out
    // (se aplican en este mismo tick, después de las del jugador humano).
    void think(const entt::registry& registry, const EconomySystem& economy, const PassGrid& grid, Tick tick,
               std::vector<Command>& out);

    [[nodiscard]] const std::vector<AiPlayerState>& players() const noexcept { return players_; }
    void hash_into(StateHasher& h) const;

private:
    AiParams params_;
    std::vector<AiPlayerState> players_;
    std::vector<std::uint8_t> profiles_;  // perfil de cada jugador de players_
};

}  // namespace rts::sim
