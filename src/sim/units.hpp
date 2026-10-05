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

inline constexpr std::int32_t kPercent = 100;

using UnitTypeId = std::uint8_t;
using PlayerId = std::uint8_t;

// Recursos: el índice en Stock es el valor del enum.
enum class Resource : std::uint8_t { Food, Wood, Stone, Gold, Iron };
inline constexpr std::size_t kResourceCount = 5;
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
    // Edificios. Sin armas de asedio no se les hace daño: el golpe de cualquier otra
    // unidad (antorcha, flecha incendiaria) aviva un fuego en ellos con esta cantidad.
    std::int32_t ignite = 0;
    std::int32_t extinguish = 0;  // fuego que quita por tick apagando (cubos, mantas, tierra)
    bool siege = false;           // arma de asedio: su golpe daña edificios
    bool buildings_only = false;  // solo ataca edificios (ariete)
    // Zapador: contra la piedra, mina bajo los cimientos (su ataque ignora la armadura);
    // contra la madera, como cualquiera, le prende fuego.
    bool undermine = false;
};

// Sustento de un tipo de unidad (data/units.toml). Un ejército no vive del aire: cada
// unidad lleva víveres (y forraje, la caballería) que gasta con el tiempo, y los
// tiradores, munición que gastan al disparar. Se reponen junto a un edificio propio
// que abastece (SupplySystem), pagando del almacén del jugador.
struct SupplyStats {
    std::int32_t rations = 0;       // raciones que lleva encima (0 = no necesita víveres)
    std::int32_t ration_ticks = 1;  // ticks que dura una ración
    bool starves = false;           // sin víveres acaba perdiendo vida (los aldeanos, no)
    std::int32_t ammo = 0;          // disparos que lleva encima (0 = no gasta munición)
    std::int32_t ammo_bundle = 1;   // disparos que se reponen de una vez
    Stock ammo_cost{};              // coste de cada reposición
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
    SupplyStats supply;
    // Bagaje (acémila, carreta): lleva suministros para las tropas; 0 = no es bagaje.
    std::int32_t convoy_capacity = 0;
    // Se le puede curar en un puesto médico y sus heridas leves sanan solas (personas;
    // no animales ni ingenios).
    bool treatable = false;
    // Pericia atendiendo heridos (% del cuidado de un enfermero; 0 = no sabe): el
    // aldeano, 100; el barbero cirujano, más.
    std::int32_t care_skill = 0;
    // Firmeza (B1): % que resiste la moral; las pérdidas se escalan por 100 / firmeza.
    // 0 = sin moral (aldeanos, bagaje, ingenios): ni se desbanda ni cuenta como tropa.
    std::int32_t morale_resolve = 0;
    // Terreno (B2): % de su velocidad fuera de llano (en terreno con speed_percent < 100),
    // sobre lo que ya resta el terreno: la carreta se atasca (50); el resto, 100.
    std::int32_t rough_speed_percent = 100;
    // Carga: % de ataque del primer golpe tras una carrera, solo en terreno que la
    // permite (llano firme). 100 = no carga.
    std::int32_t charge_percent = 100;
};

// Personal sanitario que no es aldeano (cirujano) atendiendo un puesto médico (los
// aldeanos usan su tarea de enfermero).
struct Carer {
    entt::entity post = entt::null;
    std::uint32_t move_order = 0;
    std::int32_t approaches = 0;  // llegadas sin alcanzarlo (a la segunda, desiste)
};

// Herido en un puesto médico. Hasta que ingresa (hay cama y está al alcance) va hacia
// él; ingresado queda inoperativo y sin bienes, se le puede atacar y muere si cae el
// puesto. Le curan el tiempo y los enfermeros.
struct Patient {
    entt::entity post = entt::null;
    bool admitted = false;
    std::int32_t heal_acc = 0;      // milésimas de vida curadas aún sin sumar
    std::int32_t ration_timer = 0;  // ticks desde su última ración (come como un aldeano)
    bool fed = true;                // ¿pudo comer su última ración?
    std::uint32_t move_order = 0;   // desplazamiento propio hacia el puesto (0 = ninguno)
    std::int32_t approaches = 0;    // llegadas sin alcanzarlo (a la segunda, desiste)
};

// Recién dado de alta (o sacado del puesto): no ataca hasta reorganizarse y armarse.
struct Reorganizing {
    std::int32_t ticks_left = 0;
};

// Moral de una unidad combatiente (B1), de 0 a kFullMorale.
struct Morale {
    std::int32_t value = 0;
};

// En desbandada: huye, no pelea y no obedece hasta rehacerse.
struct Routing {
    std::uint32_t flee_order = 0;  // su movimiento de huida en curso (0 = ninguno)
    Tick next_flee = 0;            // cuándo vuelve a elegir hacia dónde huir
};

// Último tick en que recibió daño (las heridas leves solo sanan en calma).
struct Hurt {
    Tick tick = 0;
};

// Víveres y munición que lleva una unidad. Hambrienta: sin raciones.
struct Supply {
    std::int32_t rations = 0;
    std::int32_t ration_timer = 0;  // ticks consumidos de la ración en curso
    std::int32_t hungry_ticks = 0;  // ticks seguidos sin raciones
    std::int32_t ammo = 0;

    [[nodiscard]] bool hungry() const noexcept { return rations <= 0; }
};

// Trabajo parcial repartido de forma uniforme: con percent = 50, un tick sí y otro no;
// con 25, uno de cada cuatro. Determinista y sin estado.
[[nodiscard]] constexpr bool works_this_tick(Tick tick, std::int32_t percent) noexcept {
    const auto p = static_cast<std::int64_t>(percent);
    const auto t = static_cast<std::int64_t>(tick);
    return (t + 1) * p / kPercent != t * p / kPercent;
}

// Posición en casillas: (1.5, 2.5) es el centro de la casilla (1, 2).
struct Position {
    Fixed x;
    Fixed y;
};

// Casilla que contiene un punto.
[[nodiscard]] inline TileCoord tile_of(FVec2 p) noexcept {
    return {p.x.floor_to_int(), p.y.floor_to_int()};
}
[[nodiscard]] inline TileCoord tile_of(const Position& p) noexcept {
    return {p.x.floor_to_int(), p.y.floor_to_int()};
}

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
    std::int32_t run_ticks = 0;    // ticks seguidos en marcha (carrera para la carga)
};

struct Velocity {
    FVec2 v;  // casillas por tick
};

struct Unit {
    UnitTypeId type = 0;
    Fixed radius;
    Fixed speed;
    std::int32_t rough_speed_percent = 100;  // del tipo (UnitType)
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
    Build,        // units -> object (edificio propio: construir, reparar o descargar)
    Place,        // colocar el edificio de tipo kind con origen target; units lo construyen
    Train,        // encolar una unidad de tipo kind en el edificio object
    CancelTrain,  // quitar la última de la cola de object, con reembolso íntegro
    Attack,       // units -> object (unidad o edificio enemigo)
    AttackMove,   // units -> target, atacando a los enemigos que encuentren por el camino
    SetStance,    // units adoptan la postura kind (sim::Stance)
    Extinguish,   // units -> object (edificio propio en llamas): apagarlo
    Demolish,     // units -> object (edificio propio): desmontarlo; deja escombros recuperables
    // units (acémilas, carretas) -> object: a un campamento, ruta de convoy (cargar en
    // casa, descargar allí, repetir); a otro edificio que abastece, cargar y quedarse.
    Convoy,
    // units (heridos) -> object (puesto médico propio): ingresar para que los curen.
    Treat,
    // units (aldeanos, cirujanos) -> object (puesto médico propio): atenderlo.
    Tend,
    // object (edificio propio que produce) -> target: punto de reunión de lo que
    // produzca (kind = kClearRally: quitarlo).
    SetRally,
    Count,        // número de tipos (no es una orden)
};

inline constexpr std::uint32_t kNoObject = 0xFFFF'FFFFU;
// Move con este kind se encola tras el movimiento en curso (punto de paso, Mayús).
inline constexpr std::uint8_t kQueueMove = 1;
// SetRally con este kind quita el punto de reunión.
inline constexpr std::uint8_t kClearRally = 1;

// Puntos de paso pendientes: al llegar al destino en curso, va al siguiente. También
// lleva al punto de reunión a lo recién producido.
struct Waypoints {
    std::vector<TileCoord> pending;
};

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

    // Terreno (B2): % de velocidad por tipo de terreno (vacío = todo al 100 %).
    std::vector<std::int32_t> speed_percent_by_terrain;
};

}  // namespace rts::sim
