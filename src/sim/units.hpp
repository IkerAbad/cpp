#pragma once

// Componentes de unidad y órdenes. Todo en Fixed/enteros.

#include <array>
#include <cstdint>
#include <vector>

#include <entt/entity/entity.hpp>

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

// Clases de armadura (data/units.toml, "classes"): el blanco de un golpe pertenece a
// una y el atacante puede tener daño extra contra ella.
using ArmorClassId = std::uint8_t;
inline constexpr std::size_t kMaxArmorClasses = 16;

// Combate (M4). Fórmula por golpe, estilo del género:
//   max(1, max(0, cuerpo - armadura_cuerpo) + max(0, proyectil - armadura_proyectil)
//          + bonus[clase del blanco])
struct CombatStats {
    std::int32_t hp = 1;
    std::int32_t attack_melee = 0;
    std::int32_t attack_pierce = 0;
    std::int32_t armor_melee = 0;
    std::int32_t armor_pierce = 0;
    ArmorClassId armor_class = 0;
    std::array<std::int32_t, kMaxArmorClasses> bonus{};  // daño extra por clase del blanco
    Fixed range;               // casillas entre los bordes de atacante y blanco
    std::int32_t reload_ticks = 1;
    std::int32_t sight_tiles = 0;  // radio de adquisición automática de blancos
    Fixed projectile_speed;    // casillas por tick; 0 = cuerpo a cuerpo
    bool auto_attack = true;   // busca blancos solo; los aldeanos, no
};

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
    CombatStats combat;
};

// Posición en casillas: (1.5, 2.5) es el centro de la casilla (1, 2).
struct Position {
    Fixed x;
    Fixed y;
};

// Vida de unidades y edificios. max_hp crece con el nivel (unidades) y la vida de un
// edificio en obra crece con el avance.
struct Health {
    std::int32_t hp = 0;
    std::int32_t max_hp = 0;
};

enum class Stance : std::uint8_t { Aggressive, HoldGround };

// Estado de combate de una unidad (M4).
struct Combatant {
    entt::entity target = entt::null;
    bool explicit_target = false;  // atacar ordenado por el jugador (no se cambia de blanco)
    bool attack_move = false;      // ataque-movimiento hacia move_dest
    Stance stance = Stance::Aggressive;
    TileCoord move_dest;
    std::int32_t cooldown = 0;     // ticks hasta el siguiente golpe
    std::uint32_t chase_order = 0; // orden de movimiento interna de persecución (0 = ninguna)
    TileCoord chase_tile;          // casilla del blanco cuando se pidió la persecución
    std::int32_t chase_failures = 0;  // persecuciones terminadas sin alcanzar un blanco quieto
    std::int32_t xp = 0;
    std::int32_t level = 0;
    std::int32_t hero_name = -1;   // índice en la lista de nombres; -1 = no es héroe
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
    Attack,       // units -> object (unidad o edificio enemigo)
    AttackMove,   // units -> target, atacando a los enemigos que encuentren por el camino
    SetStance,    // units adoptan la postura kind (sim::Stance)
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
    std::uint8_t kind = 0;              // tipo de edificio (Place), de unidad (Train) o postura
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
