#pragma once

// Mundo de la simulación: el mapa y las entidades. Hasta M2 las únicas entidades son
// marcadores de prueba que rebotan dentro de una zona del mapa; sirven para comprobar
// la interpolación, la selección y el hash de regresión.

#include <cstdint>
#include <memory>
#include <vector>

#include <entt/entity/registry.hpp>

#include "sim/fixed.hpp"
#include "sim/map_gen.hpp"
#include "sim/rng.hpp"
#include "sim/tick.hpp"
#include "sim/tile_map.hpp"

namespace rts::sim {

// Posición en casillas: (1.5, 2.5) es el centro de la casilla (1, 2).
struct Position {
    Fixed x;
    Fixed y;
};

// Desplazamiento en casillas por tick.
struct Velocity {
    Fixed dx;
    Fixed dy;
};

struct DemoParams {
    std::uint64_t seed = 0;
    std::int32_t point_count = 0;
    // Lado del cuadrado centrado en el mapa donde rebotan los marcadores.
    std::int32_t area_tiles = 0;
    Fixed max_speed;  // casillas por tick
};

struct WorldParams {
    MapGenParams map;
    DemoParams demo;
};

struct SnapshotEntity {
    std::uint32_t id;
    Position pos;
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

    void step();

    [[nodiscard]] Tick tick() const noexcept { return tick_; }
    [[nodiscard]] const TileMap& map() const noexcept { return *map_; }
    [[nodiscard]] std::uint64_t state_hash() const;
    void write_snapshot(Snapshot& out) const;

private:
    std::shared_ptr<TileMap> map_;
    entt::registry registry_;
    Xoshiro256pp rng_;
    Fixed area_min_;
    Fixed area_max_;
    Tick tick_ = 0;
};

}  // namespace rts::sim
