#pragma once

// IA básica (adelanto de M7). Vive dentro de la simulación: es determinista, corre
// igual en todas las máquinas y no hay que enviar sus órdenes por la red. Solo LEE el
// estado y emite las mismas órdenes que un jugador humano; no recibe recursos ni
// ventajas. La dificultad futura saldrá de jugar mejor, nunca de hacer trampas.

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include <entt/entity/registry.hpp>

#include "sim/economy.hpp"
#include "sim/path/grid.hpp"
#include "sim/supply.hpp"
#include "sim/vision.hpp"
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
    Workshop,        // taller de asedio cuando ya tiene cuartel
    Extinguish,      // aldeanos a apagar los fuegos de sus edificios
    Raid,            // incursiones de jinetes a quemar edificios de madera enemigos sin defensa
    Resupply,        // tropas cortas de víveres o munición vuelven a abastecerse
    Logistics,       // campamento avanzado camino del objetivo, abastecido por convoyes
    Explore,         // con niebla de guerra: un explorador recorre lo no explorado
    Assault,         // ante un recinto cerrado: brecha en el muro más cercano (ingenios, escalas, fuego)
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
    std::int32_t extinguishers_per_fire = 0;  // apagar: aldeanos por edificio en llamas
    // ejercito_contra: con menos aldeanos que esto solo entrena si su ejército es más
    // débil que el enemigo conocido (economía primero, sin quedar indefenso).
    std::int32_t army_min_villagers = 0;
    // ... salvo con niebla: lo que no se ve no es que no exista; mantiene al menos
    // fog_guard_army tropas aunque no haya visto ningún ejército enemigo.
    std::int32_t fog_guard_army = 0;
    // incendiar: grupos de raid_group unidades de tipo raid_unit contra edificios de
    // madera sin enemigos armados a raid_safe_radius_tiles.
    std::optional<UnitTypeId> raid_unit;
    std::int32_t raid_group = 0;
    std::int32_t raid_safe_radius_tiles = 0;
    // Saqueo (C2): una granja enemiga con enemigos armados a esta distancia no se saquea.
    std::int32_t pillage_guard_tiles = 0;
    // abastecer: una unidad que no pelea vuelve a abastecerse cuando sus víveres o su
    // munición bajan de este % de lo que puede llevar.
    std::int32_t resupply_percent = 0;
    // Reserva para las raciones: no gasta en otra cosa este % del coste de una ración
    // por cada unidad propia que come (si se lo gasta todo, nadie podrá comer).
    std::int32_t upkeep_reserve_percent = 0;
    // logistica: si el objetivo queda a más de camp_distance_tiles de toda fuente de
    // suministro propia, campamento a camp_offset_tiles de él (de camino desde la
    // base), abastecido por convoy_carriers unidades de bagaje.
    // camp_distance_tiles = 0: sin campamentos. El bagaje (convoy_carriers unidades)
    // sigue al ejército en campaña baggage_offset_tiles por detrás, hacia la base.
    std::int32_t camp_distance_tiles = 0;
    std::int32_t camp_offset_tiles = 0;
    std::int32_t convoy_carriers = 0;
    std::int32_t baggage_offset_tiles = 0;
    // Asedio: con el campamento en uso, monta allí hasta tantos ingenios de asedio.
    std::int32_t siege_engines = 0;
    // El campamento se levanta solo con su ejército en campaña a esta distancia del
    // objetivo o menos (domina el terreno).
    std::int32_t siege_front_tiles = 0;
    // explorar: mientras no conozca ningún edificio vital enemigo, scouts unidades
    // ociosas (las de raid_unit primero) recorren lo no explorado, por los puntos de una
    // rejilla de explore_step_tiles casillas, el más cercano primero.
    std::int32_t scouts = 0;
    std::int32_t explore_step_tiles = 1;
    std::vector<UnitTypeId> army;            // ciclo de entrenamiento en el cuartel
};

// data/config/engine.toml, sección [ai]. Tipos ya resueltos a ids.
struct AiParams {
    std::int32_t think_interval_ticks = 1;  // cada cuánto decide (repartido entre jugadores)
    // Con niebla, el ejército enemigo visto se recuerda y el recuerdo se desvanece esta
    // fracción (por mil) en cada decisión: lo que ya no se ve se va olvidando.
    std::int32_t enemy_memory_decay_permille = 0;
    // Tipos que usa cualquier perfil (dependerán de la civilización, no de la dificultad).
    UnitTypeId worker_type = 0;
    BuildingTypeId house = 0;
    BuildingTypeId barracks = 0;
    BuildingTypeId farm = 0;
    std::optional<BuildingTypeId> workshop;  // taller de asedio (módulo taller)
    std::optional<BuildingTypeId> camp;      // campamento de campaña (módulo logistica)
    std::optional<UnitTypeId> carrier;       // bagaje de los convoyes (módulo logistica)
    std::optional<UnitTypeId> siege_engine;  // se monta en el campamento (trabuquete)
    std::array<BuildingTypeId, kResourceCount> dropoff{};  // almacén para cada recurso
    Stock ladder_cost{};  // escala de asedio (de [climb]; módulo asalto)
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
    std::vector<entt::entity> scouts;       // exploradores en curso
    std::vector<TileCoord> scout_targets;   // punto al que va cada uno (paralelo a scouts)
    std::vector<TileCoord> explore_done;    // puntos visitados o inalcanzables: no se repiten
    std::vector<std::int64_t> enemy_seen_milli;  // por tipo: unidades enemigas recordadas (milésimas)
};

class AiSystem {
public:
    // supply: las mismas reglas de abastecimiento que aplica la simulación.
    AiSystem(const AiParams& params, const SupplyParams& supply, const std::vector<AiSeat>& seats);

    // Decide para los jugadores a los que les toca este tick y añade sus órdenes a out
    // (se aplican en este mismo tick, después de las del jugador humano).
    // vision: niebla de guerra (la IA solo sabe lo que ve y recuerda); null = lo ve todo.
    void think(const entt::registry& registry, const EconomySystem& economy, const PassGrid& grid, Tick tick,
               std::vector<Command>& out, const VisionSystem* vision = nullptr);

    [[nodiscard]] const std::vector<AiPlayerState>& players() const noexcept { return players_; }
    void hash_into(StateHasher& h) const;

private:
    AiParams params_;
    SupplyParams supply_;
    std::vector<AiPlayerState> players_;
    std::vector<std::uint8_t> profiles_;  // perfil de cada jugador de players_
};

}  // namespace rts::sim
