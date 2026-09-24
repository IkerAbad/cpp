#pragma once

// Mundo de la simulación: mapa, unidades y órdenes. Todo el estado que influye en el
// futuro entra en state_hash(); la presentación solo ve snapshots.

#include <cstdint>
#include <memory>
#include <vector>

#include <entt/entity/registry.hpp>

#include "sim/fixed.hpp"
#include "sim/map_gen.hpp"
#include "sim/movement.hpp"
#include "sim/rng.hpp"
#include "sim/tick.hpp"
#include "sim/tile_map.hpp"
#include "sim/units.hpp"

namespace rts::sim {

// Unidades iniciales de prueba hasta que haya economía (M3): count unidades de un tipo
// repartidas en un cuadrado de area_tiles casillas centrado en el mapa.
struct DemoParams {
    std::uint64_t seed = 0;
    UnitTypeId unit_type = 0;
    std::int32_t count = 0;
    std::int32_t area_tiles = 0;
};

struct WorldParams {
    MapGenParams map;
    std::vector<std::uint8_t> passable_by_terrain;  // por TerrainId
    std::vector<UnitType> unit_types;                // por UnitTypeId
    MovementParams movement;
    DemoParams demo;
};

struct SnapshotEntity {
    std::uint32_t id;
    Position pos;
    UnitTypeId type;
};

// Copia de solo lectura del estado que se presenta. El render nunca toca el registro.
// El mapa se comparte sin copiar (O(1) por tick). Regla: la simulación no modifica
// nunca un TileMap que un snapshot pueda estar leyendo. Cuando algo lo modifique
// (M3: recursos que se agotan), clonará el mapa si shared_ptr::use_count() > 1:
// copia al escribir, 128 KiB en 256x256, solo en los ticks que lo cambian.
struct Snapshot {
    Tick tick = 0;
    std::shared_ptr<const TileMap> map;
    std::vector<SnapshotEntity> entities;
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
    [[nodiscard]] const entt::registry& registry() const noexcept { return registry_; }
    [[nodiscard]] std::uint64_t state_hash() const;
    void write_snapshot(Snapshot& out) const;

private:
    void spawn_demo_units(const WorldParams& params);

    std::shared_ptr<TileMap> map_;
    std::vector<UnitType> unit_types_;
    MovementSystem movement_;
    entt::registry registry_;
    Xoshiro256pp rng_;
    std::vector<Command> pending_;
    std::uint32_t next_order_id_ = 1;
    Tick tick_ = 0;
};

}  // namespace rts::sim
