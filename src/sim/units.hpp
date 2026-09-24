#pragma once

// Componentes de unidad y órdenes. Todo en Fixed/enteros.

#include <array>
#include <cstdint>
#include <vector>

#include "sim/fixed.hpp"
#include "sim/fmath.hpp"
#include "sim/path/hpa.hpp"
#include "sim/tick.hpp"
#include "sim/tile_map.hpp"

namespace rts::sim {

using UnitTypeId = std::uint8_t;
using PlayerId = std::uint8_t;

// Recursos: el índice en Stock es el valor del enum.
enum class Resource : std::uint8_t { Food, Wood, Stone, Gold };
inline constexpr std::size_t kResourceCount = 4;
using Stock = std::array<std::int32_t, kResourceCount>;

[[nodiscard]] constexpr std::size_t resource_index(Resource r) noexcept { return static_cast<std::size_t>(r); }

// Parámetros de un tipo de unidad (data/units.toml).
struct UnitType {
    Fixed radius;  // casillas
    Fixed speed;   // casillas por tick
    Stock cost{};
    std::int32_t train_ticks = 0;
    std::int32_t population = 1;  // plazas de población que ocupa
    // Aldeano: recoge recursos y construye. carry_capacity = unidades que lleva encima.
    bool worker = false;
    std::int32_t carry_capacity = 0;
};

// Posición en casillas: (1.5, 2.5) es el centro de la casilla (1, 2).
struct Position {
    Fixed x;
    Fixed y;
};

struct Velocity {
    FVec2 v;  // casillas por tick
};

struct Unit {
    UnitTypeId type = 0;
    Fixed radius;
    Fixed speed;
};

// Orden de movimiento en curso.
struct MoveGoal {
    TileCoord tile;
    FVec2 point;               // centro de la casilla destino
    std::uint32_t order_id = 0;  // unidades de la misma orden llegan "en cadena"
    std::int32_t flow_field = -1;  // índice en la caché de campos, o -1 si va por HPA*
    std::int32_t stuck_ticks = 0;  // ticks seguidos avanzando a menos de 1/4 de su velocidad
    std::int32_t group_size = 1;   // unidades que recibieron la misma orden y destino
    bool arrived = false;
};

// Camino individual (HPA*): puntos de paso y el tramo refinado en curso.
struct PathFollow {
    std::vector<TileCoord> waypoints;
    std::uint32_t next_waypoint = 0;
    std::vector<TileCoord> segment;
    std::uint32_t next_tile = 0;
    bool waiting = true;  // pendiente de que el planificador le asigne camino
};

enum class CommandType : std::uint8_t {
    Move,         // units -> target
    Stop,         // units
    Gather,       // units -> object (nodo de recurso)
    Build,        // units -> object (edificio propio en obra)
    Place,        // colocar el edificio de tipo kind con origen target; units lo construyen
    Train,        // encolar una unidad de tipo kind en el edificio object
    CancelTrain,  // quitar la última de la cola de object, con reembolso íntegro
};

inline constexpr std::uint32_t kNoObject = 0xFFFF'FFFFU;

// Orden de un jugador, aplicada al inicio del tick indicado. Es la unidad de las
// repeticiones (M5) y de la sincronización lockstep (M8). Una orden sobre entidades
// que no son del jugador se ignora.
struct Command {
    Tick tick = 0;
    PlayerId player = 0;
    CommandType type = CommandType::Move;
    std::vector<std::uint32_t> units;  // ids de entidad
    TileCoord target;
    std::uint32_t object = kNoObject;  // nodo o edificio
    std::uint8_t kind = 0;              // tipo de edificio (Place) o de unidad (Train)
};

// Parámetros del movimiento (data/config/engine.toml, sección [movement]).
struct MovementParams {
    HpaParams hpa;
    // Presupuesto de nodos de rejilla expandidos por tick para el planificador. Se mide
    // en nodos, no en tiempo: el reparto entre ticks es idéntico en todas las máquinas.
    std::int32_t path_node_budget_per_tick = 0;
    // Una orden a este número de unidades o más usa un campo de flujo compartido.
    std::int32_t flow_field_min_group = 0;
    std::int32_t flow_field_cache_size = 0;
    // Radio al que se buscan destinos alternativos si el pedido no es alcanzable.
    std::int32_t retarget_radius_tiles = 0;

    // Evitación local (RVO muestreado).
    Fixed neighbor_radius;  // casillas
    std::int32_t max_neighbors = 0;
    std::int32_t time_horizon_ticks = 0;
    std::int32_t preference_weight = 0;  // penaliza desviarse de la velocidad deseada
    std::int32_t collision_weight = 0;   // penaliza la colisión inminente

    // Atasco: una unidad que lleva estos ticks casi parada llega si tiene cerca una
    // compañera de orden ya llegada; con el cuádruple, llega de todos modos (desiste).
    std::int32_t stuck_arrive_ticks = 0;

    Fixed arrive_radius;     // casillas: a esta distancia del destino se da por llegado
    Fixed waypoint_radius;   // casillas: a esta distancia de una casilla del tramo se pasa a la siguiente
};

}  // namespace rts::sim
