#pragma once

// Economía (M3): jugadores, nodos de recurso, edificios con obra y colas de producción,
// y el ciclo del aldeano: ir al nodo, recoger, llevar al almacén más cercano, volver.
// Todo en enteros; los tiempos en ticks. Los nodos y los edificios son entidades con
// una huella que bloquea casillas en la rejilla de transitabilidad.

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <entt/entity/registry.hpp>

#include "sim/fixed.hpp"
#include "sim/fmath.hpp"
#include "sim/movement.hpp"
#include "sim/tick.hpp"
#include "sim/tile_map.hpp"
#include "sim/units.hpp"

namespace rts::sim {

class StateHasher;

using BuildingTypeId = std::uint8_t;
using NodeTypeId = std::uint8_t;

// Tipo de nodo de recurso (data/resources.toml).
struct ResourceNodeType {
    Resource kind = Resource::Food;
    std::int32_t amount = 0;
    std::int32_t size = 1;  // casillas por lado
};

// Material de un edificio. La madera arde hasta caer; en la piedra el fuego solo
// consume tejado e interior: el edificio queda quemado (inutilizado) y en pie.
enum class Material : std::uint8_t { Wood, Stone };

// Tipo de edificio (data/buildings.toml).
struct BuildingType {
    Material material = Material::Wood;
    std::int32_t size = 1;         // casillas por lado
    Stock cost{};
    std::int32_t build_ticks = 0;  // ticks de trabajo de un aldeano; n aldeanos, n veces más rápido
    std::int32_t hp = 0;
    std::int32_t armor_melee = 0;
    std::int32_t armor_pierce = 0;
    ArmorClassId armor_class = 0;
    std::uint8_t accepts = 0;      // almacén: bit resource_index(r) si admite el recurso r
    std::int32_t population = 0;   // plazas de población que aporta terminado
    std::vector<UnitTypeId> trains;
    // Para colocarlo hay que tener terminado un edificio de cada uno de estos tipos.
    std::vector<BuildingTypeId> required;
    // Granja: al terminarse se convierte en un nodo de comida de su dueño con esta
    // cantidad (0 = no es granja). Se agota y desaparece como cualquier nodo.
    std::int32_t farm_food = 0;
    // Vital (centro urbano): un jugador que tuvo alguno terminado pierde cuando ya no
    // le queda ninguno terminado; lo que esté en obra no cuenta.
    bool vital = false;
    // Abastece: las tropas propias cercanas reponen aquí víveres y munición.
    bool supplies = false;
    // Campamento de campaña: no tira del almacén del jugador sino de uno propio, con
    // esta capacidad, que llenan los convoyes (0 = no es campamento).
    std::int32_t store_capacity = 0;
};

[[nodiscard]] constexpr std::uint8_t resource_bit(Resource r) noexcept {
    return static_cast<std::uint8_t>(1U << resource_index(r));
}

// data/config/engine.toml, sección [economy].
struct EconomyParams {
    std::array<std::int32_t, kResourceCount> gather_ticks{};  // ticks por unidad recogida
    Fixed interact_range;                    // holgura entre el borde de la unidad y el del objeto
    std::int32_t retarget_radius_tiles = 0;  // al agotarse un nodo, otro del mismo recurso a esta distancia
    std::int32_t approach_attempts = 0;      // casillas de acceso que prueba antes de cambiar de objetivo
    // Recolectores por casilla de huella: un árbol de 1x1 admite este número; una mina
    // de 2x2, cuatro veces más. Los que sobran van al nodo accesible más cercano.
    std::int32_t gatherers_per_tile = 0;
    std::int32_t queue_capacity = 0;
    std::int32_t max_population = 0;
    std::int32_t spawn_search_radius = 0;    // anillos alrededor del edificio donde aparece lo producido
    // Reparar la vida entera de un edificio cuesta este % de su coste en madera.
    std::int32_t repair_cost_percent = 0;
    // Escombros: un edificio derribado por asedio o desmontado deja, en su solar, un
    // nodo con este % del coste de su material (piedra o madera), que hay que recoger
    // y transportar. Lo que arde no deja nada aprovechable.
    std::int32_t salvage_percent = 0;
    NodeTypeId rubble_stone = 0;
    NodeTypeId rubble_wood = 0;
    // Un aldeano hambriento (sin víveres) trabaja a este % de su ritmo.
    std::int32_t hungry_work_percent = 100;
};

// --- Componentes -------------------------------------------------------------------

struct Owner {
    PlayerId player = 0;
};

// Objeto estático: ocupa size x size casillas desde origin y las bloquea.
struct Footprint {
    TileCoord origin;
    std::int32_t size = 1;

    [[nodiscard]] bool contains(TileCoord c) const noexcept {
        return c.x >= origin.x && c.y >= origin.y && c.x < origin.x + size && c.y < origin.y + size;
    }
};

// Casilla central de una huella (la de arriba a la izquierda del centro si es par).
[[nodiscard]] inline TileCoord center_of(const Footprint& f) noexcept {
    return {f.origin.x + f.size / 2, f.origin.y + f.size / 2};
}

// Casilla de la huella más cercana a c.
[[nodiscard]] inline TileCoord clamp_to(const Footprint& f, TileCoord c) noexcept {
    return {std::clamp(c.x, f.origin.x, f.origin.x + f.size - 1), std::clamp(c.y, f.origin.y, f.origin.y + f.size - 1)};
}

// Distancia al cuadrado (32.32) de un punto al rectángulo de una huella; 0 dentro.
[[nodiscard]] std::int64_t distance_sq_to(const Footprint& f, FVec2 p) noexcept;

struct ResourceNode {
    NodeTypeId type = 0;
    Resource kind = Resource::Food;
    std::int32_t amount = 0;
};

struct Building {
    BuildingTypeId type = 0;
    std::int32_t progress = 0;  // ticks de trabajo acumulados (la vida va en Health)
    bool complete = false;
    bool burned = false;        // piedra quemada: no produce, no almacena ni da plazas
    std::int64_t repair_acc = 0;  // fracción de madera de la reparación aún sin cobrar
    std::uint32_t spawned = 0;  // unidades producidas: reparte las casillas de salida

    // Funciona: terminado y no quemado.
    [[nodiscard]] bool working() const noexcept { return complete && !burned; }
};

struct ProductionQueue {
    std::vector<UnitTypeId> items;
    std::int32_t progress = 0;  // ticks del primero de la cola
};

enum class WorkerTask : std::uint8_t { Idle, Gather, Deliver, Build, Demolish };

struct Worker {
    WorkerTask task = WorkerTask::Idle;
    entt::entity node = entt::null;      // nodo que explota o al que volverá tras descargar
    entt::entity building = entt::null;  // almacén o edificio en obra
    Resource gather_kind = Resource::Food;
    TileCoord gather_area{-1, -1};       // último nodo: centro de la búsqueda de otro (-1: ninguno)
    Resource carry_kind = Resource::Food;
    std::int32_t carried = 0;
    std::int32_t timer = 0;              // ticks recogiendo la unidad en curso
    std::int32_t attempts = 0;           // casillas de acceso probadas sin llegar al objetivo
    std::int32_t retargets = 0;          // nodos descartados seguidos por inalcanzables
    std::uint32_t move_order = 0;        // orden de movimiento interna en curso (0 = ninguna)
};

struct PlayerState {
    Stock stock{};
    std::int32_t population = 0;
    std::int32_t population_cap = 0;
    bool started = false;   // ha tenido alguna unidad o edificio
    bool had_vital = false; // ha tenido algún edificio vital terminado
    // Derrota: si tuvo un edificio vital, al quedarse sin ninguno terminado; si nunca
    // lo tuvo (pruebas, demo), al quedarse sin unidades ni edificios. Es definitiva, y
    // lo que le quede se retira del mapa.
    bool defeated = false;
};

struct EconomyTickStats {
    std::int32_t gathered = 0;
    std::int32_t delivered = 0;
    std::int32_t nodes_depleted = 0;
    std::int32_t units_trained = 0;
    std::int32_t approach_moves = 0;
};

// Catálogos de tipos que la economía necesita (copias: son pequeños).
struct EconomyCatalog {
    std::vector<UnitType> units;
    std::vector<BuildingType> buildings;
    std::vector<ResourceNodeType> nodes;
};

class EconomySystem {
public:
    EconomySystem(std::int32_t width, std::int32_t height, const EconomyParams& params, EconomyCatalog catalog,
                  std::int32_t players);

    // Las unidades ya vienen filtradas por dueño. Move y Stop solo cancelan la tarea
    // (el movimiento lo aplica MovementSystem); el resto son órdenes de economía.
    void apply(entt::registry& registry, MovementSystem& movement, const Command& command,
               std::span<const entt::entity> units, std::uint32_t& next_order_id, Tick tick);
    void update(entt::registry& registry, MovementSystem& movement, std::uint32_t& next_order_id, Tick tick);

    // Preparación de la partida y pruebas: no son órdenes y no viajan por la red.
    entt::entity spawn_unit(entt::registry& registry, PlayerId player, UnitTypeId type, FVec2 pos) const;
    std::optional<entt::entity> place_building(entt::registry& registry, MovementSystem& movement, PlayerId player,
                                               BuildingTypeId type, TileCoord origin, bool complete);
    // Destrucción de un edificio: libera su huella en la rejilla y, con rubble, deja
    // escombros recuperables en su solar (asedio, demolición; no el fuego).
    void remove_building(entt::registry& registry, MovementSystem& movement, entt::entity building,
                         bool rubble = false);
    std::optional<entt::entity> place_node(entt::registry& registry, MovementSystem& movement, NodeTypeId type,
                                           TileCoord origin);
    // Casilla libre alrededor de una huella, repartida por index entre las del primer
    // anillo que tenga alguna (así las unidades producidas no nacen apiladas).
    [[nodiscard]] std::optional<TileCoord> spawn_tile(const PassGrid& grid, const Footprint& f,
                                                      std::uint32_t index) const;

    // Terreno transitable, sin objetos y sin unidades encima.
    [[nodiscard]] bool can_place(const entt::registry& registry, const PassGrid& grid, std::int32_t size,
                                 TileCoord origin) const;
    [[nodiscard]] entt::entity occupant(TileCoord c) const noexcept;
    // El jugador tiene terminado un edificio de cada tipo que exige el tipo type.
    [[nodiscard]] bool meets_requirements(const entt::registry& registry, PlayerId player, BuildingTypeId type) const;
    // Terminado, dañado o quemado y sin fuego: los aldeanos pueden repararlo.
    [[nodiscard]] static bool needs_repair(const entt::registry& registry, entt::entity building);
    // Nodo explotable por el jugador: sin dueño (natural) o suyo (granja).
    [[nodiscard]] static bool can_gather(const entt::registry& registry, entt::entity node, PlayerId player);

    // Población, tope y derrota de cada jugador; update() lo recalcula al empezar cada
    // tick y después retira del mapa lo que les quede a los derrotados.
    void recount_population(const entt::registry& registry);

    [[nodiscard]] std::span<const PlayerState> players() const noexcept { return players_; }
    [[nodiscard]] PlayerState& player_state(PlayerId p) { return players_[p]; }
    [[nodiscard]] const EconomyParams& params() const noexcept { return params_; }
    [[nodiscard]] const EconomyCatalog& catalog() const noexcept { return catalog_; }
    [[nodiscard]] const EconomyTickStats& last_stats() const noexcept { return stats_; }

    void hash_into(StateHasher& h, const entt::registry& registry) const;

private:
    enum class Approach : std::uint8_t { InReach, Moving, Failed };

    [[nodiscard]] std::size_t tile_index(TileCoord c) const noexcept {
        return static_cast<std::size_t>(c.y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(c.x);
    }
    void occupy(MovementSystem& movement, const Footprint& f, entt::entity e);
    void release(MovementSystem& movement, const Footprint& f);

    void update_worker(entt::registry& registry, MovementSystem& movement, entt::entity e,
                       std::uint32_t& next_order_id, Tick tick);
    // Aldeano hambriento que en este tick no trabaja (hungry_work_percent).
    [[nodiscard]] bool slowed_by_hunger(const entt::registry& registry, entt::entity e, Tick tick) const;
    void step_gather(entt::registry& registry, MovementSystem& movement, entt::entity e, Worker& w,
                     std::uint32_t& next_order_id, Tick tick);
    void step_deliver(entt::registry& registry, MovementSystem& movement, entt::entity e, Worker& w,
                      std::uint32_t& next_order_id, Tick tick);
    void step_build(entt::registry& registry, MovementSystem& movement, entt::entity e, Worker& w,
                    std::uint32_t& next_order_id, Tick tick);
    void step_demolish(entt::registry& registry, MovementSystem& movement, entt::entity e, Worker& w,
                       std::uint32_t& next_order_id, Tick tick);
    Approach approach(entt::registry& registry, MovementSystem& movement, entt::entity e, Worker& w,
                      const Footprint& target, std::uint32_t& next_order_id, Tick tick);
    void start_gather(Worker& w, entt::entity node, const Footprint& f, Resource kind) const;
    [[nodiscard]] bool has_room(const entt::registry& registry, entt::entity node) const;
    [[nodiscard]] std::optional<entt::entity> find_node_near(const entt::registry& registry, const PassGrid& grid,
                                                             PlayerId player, Resource kind, TileCoord center,
                                                             std::uint32_t component, entt::entity exclude) const;
    [[nodiscard]] std::optional<entt::entity> nearest_dropoff(const entt::registry& registry, PlayerId player,
                                                              Resource kind, TileCoord from) const;
    void deplete(entt::registry& registry, MovementSystem& movement, entt::entity node);
    void update_production(entt::registry& registry, const MovementSystem& movement);
    void retire_defeated(entt::registry& registry, MovementSystem& movement);
    void repair(entt::registry& registry, entt::entity building, Building& b, const BuildingType& bt);
    // Granja terminada: se convierte también en un nodo de comida de su dueño.
    static void start_farm(entt::registry& registry, entt::entity building, const BuildingType& bt);

    std::int32_t width_;
    std::int32_t height_;
    EconomyParams params_;
    EconomyCatalog catalog_;
    std::vector<PlayerState> players_;
    std::vector<entt::entity> occupant_;  // casilla -> objeto estático que la ocupa
    std::vector<entt::entity> scratch_;
    EconomyTickStats stats_;
};

}  // namespace rts::sim
