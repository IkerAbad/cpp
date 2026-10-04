#pragma once

// Mundo de la simulación: mapa, unidades, economía y órdenes. Todo el estado que influye
// en el futuro entra en state_hash(); la presentación solo ve snapshots.

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <entt/entity/registry.hpp>

#include "sim/ai.hpp"
#include "sim/combat.hpp"
#include "sim/economy.hpp"
#include "sim/fire.hpp"
#include "sim/fixed.hpp"
#include "sim/map_gen.hpp"
#include "sim/movement.hpp"
#include "sim/rng.hpp"
#include "sim/supply.hpp"
#include "sim/tick.hpp"
#include "sim/tile_map.hpp"
#include "sim/units.hpp"

namespace rts::sim {

// Unidades de prueba sueltas (movimiento, banco): count unidades de un tipo del jugador
// indicado, repartidas en un cuadrado de area_tiles casillas centrado en el mapa.
struct DemoParams {
    std::uint64_t seed = 0;
    UnitTypeId unit_type = 0;
    PlayerId player = 0;
    std::int32_t count = 0;
    std::int32_t area_tiles = 0;
};

// Nodos de recurso alrededor de cada inicio: el primero al azar a una distancia de
// Chebyshev en [min_distance, max_distance] del centro del edificio inicial; los
// siguientes, en el sitio libre más cercano al primero (forman un grupo).
struct StartNodes {
    NodeTypeId type = 0;
    std::int32_t count = 0;
    std::int32_t min_distance = 0;
    std::int32_t max_distance = 0;
};

// Preparación de una partida (data/config/engine.toml, sección [setup]). Sin inicios
// no hay jugadores con economía: es el mundo de las pruebas de movimiento y del banco.
struct SetupParams {
    std::uint64_t seed = 0;
    std::vector<TileCoord> starts;  // uno por jugador: centro deseado del edificio inicial
    std::int32_t start_search_radius = 0;
    std::int32_t min_start_region_tiles = 0;  // la región del inicio debe ser al menos así de grande
    BuildingTypeId start_building = 0;
    UnitTypeId start_unit = 0;
    std::int32_t start_units = 0;
    Stock start_stock{};
    std::vector<StartNodes> near_start;
    // Bosques: árbol en cada casilla de forest_terrain con probabilidad density / 1000,
    // salvo a menos de clear_radius casillas de un inicio.
    TerrainId forest_terrain = 0;
    NodeTypeId tree_type = 0;
    std::int32_t tree_density_permille = 0;
    std::int32_t clear_radius = 0;
};

struct WorldParams {
    MapGenParams map;
    std::vector<std::uint8_t> passable_by_terrain;  // por TerrainId
    std::vector<UnitType> unit_types;                // por UnitTypeId
    std::vector<BuildingType> building_types;        // por BuildingTypeId
    std::vector<ResourceNodeType> node_types;        // por NodeTypeId
    MovementParams movement;
    EconomyParams economy;
    CombatParams combat;
    FireParams fire;
    SupplyParams supply;
    AiParams ai;
    std::vector<AiSeat> ai_players;  // jugadores que controla la IA y su perfil
    SetupParams setup;
    DemoParams demo;
};

struct SnapshotEntity {
    std::uint32_t id = 0;
    Position pos;
    UnitTypeId type = 0;
    PlayerId owner = 0;
    WorkerTask task = WorkerTask::Idle;
    Resource carry_kind = Resource::Food;
    std::int32_t carried = 0;
    std::int32_t hp = 0;
    std::int32_t max_hp = 0;
    std::int32_t level = 0;
    std::int32_t xp = 0;
    std::int32_t hero_name = -1;  // índice en la lista de nombres de héroe; -1 = no es héroe
    Stance stance = Stance::Aggressive;
    std::int32_t rations = 0;      // víveres que lleva (máximo: SupplyStats del tipo)
    std::int32_t ammo = 0;         // munición que lleva
    bool hungry = false;           // necesita víveres y no le quedan
};

enum class ObjectKind : std::uint8_t { Building, Resource };

// Objeto estático (edificio o nodo de recurso).
struct SnapshotObject {
    std::uint32_t id = 0;
    ObjectKind kind = ObjectKind::Resource;
    std::uint8_t type = 0;  // BuildingTypeId o NodeTypeId
    PlayerId owner = 0;     // solo edificios
    TileCoord origin;
    std::int32_t size = 1;
    std::int32_t amount = 0;         // nodos: cantidad restante
    std::int32_t hp = 0;             // edificios
    std::int32_t progress = 0;       // edificios: ticks de obra acumulados
    bool complete = true;
    std::vector<UnitTypeId> queue;   // edificios que producen
    std::int32_t queue_progress = 0; // ticks del primero de la cola
    std::int32_t fire = 0;           // edificios: intensidad del fuego (0 = sin fuego)
    bool burned = false;             // edificios de piedra quemados (inutilizados)
};

// Copia de solo lectura del estado que se presenta. El render nunca toca el registro.
// El mapa del terreno se comparte sin copiar (O(1) por tick): la simulación no lo
// modifica nunca; la ocupación por edificios y recursos vive en PassGrid y llega a la
// presentación como objetos.
struct Snapshot {
    Tick tick = 0;
    std::shared_ptr<const TileMap> map;
    std::vector<SnapshotEntity> entities;
    std::vector<SnapshotObject> objects;
    std::vector<PlayerState> players;
    std::vector<Position> projectiles;
};

class World {
public:
    explicit World(const WorldParams& params);

    // Encola una orden. Se aplica al inicio del tick command.tick (o del siguiente
    // step() si ese tick ya pasó), en el orden en que se encoló.
    void issue(Command command);

    void step();

    [[nodiscard]] Tick tick() const noexcept { return tick_; }
    [[nodiscard]] const TileMap& map() const noexcept { return *map_; }
    [[nodiscard]] const MovementSystem& movement() const noexcept { return movement_; }
    [[nodiscard]] const EconomySystem& economy() const noexcept { return economy_; }
    [[nodiscard]] const CombatSystem& combat() const noexcept { return combat_; }
    [[nodiscard]] const FireSystem& fire() const noexcept { return fire_; }
    [[nodiscard]] const SupplySystem& supply() const noexcept { return supply_; }
    [[nodiscard]] const AiSystem& ai() const noexcept { return ai_; }
    [[nodiscard]] const entt::registry& registry() const noexcept { return registry_; }
    [[nodiscard]] std::uint64_t state_hash() const;
    void write_snapshot(Snapshot& out) const;

    // Consultas de solo lectura para la interfaz (fantasma de colocación, clic derecho).
    [[nodiscard]] bool can_place(BuildingTypeId type, TileCoord origin) const;
    [[nodiscard]] bool meets_requirements(PlayerId player, BuildingTypeId type) const {
        return economy_.meets_requirements(registry_, player, type);
    }
    [[nodiscard]] std::optional<std::uint32_t> object_at(TileCoord c) const;
    [[nodiscard]] const PlayerState& player_state(PlayerId p) const { return economy_.players()[p]; }

    // Preparación de escenarios y pruebas. No son órdenes: no se graban ni viajan por la
    // red. Los cambios de ocupación se aplican al inicio del siguiente step().
    std::uint32_t spawn_unit(PlayerId player, UnitTypeId type, TileCoord tile);
    std::optional<std::uint32_t> spawn_building(PlayerId player, BuildingTypeId type, TileCoord origin,
                                                bool complete);
    std::optional<std::uint32_t> spawn_node(NodeTypeId type, TileCoord origin);
    void set_stock(PlayerId player, const Stock& stock);

private:
    void setup_game(const SetupParams& setup);
    void spawn_demo_units(const DemoParams& demo);
    void apply_command(const Command& command);

    std::shared_ptr<TileMap> map_;
    MovementSystem movement_;
    EconomySystem economy_;
    CombatSystem combat_;
    FireSystem fire_;
    SupplySystem supply_;
    AiSystem ai_;
    entt::registry registry_;
    Xoshiro256pp rng_;
    std::vector<Command> pending_;
    std::vector<Command> ai_orders_;
    std::uint32_t next_order_id_ = 1;
    Tick tick_ = 0;
};

}  // namespace rts::sim
