#pragma once

// Mundo de la simulación. En M0 solo contiene la demostración del bucle a paso fijo:
// puntos que rebotan dentro de una arena cuadrada. Su propósito es comprobar la
// interpolación del render y fijar la primera prueba de regresión por hash.

#include <cstdint>
#include <vector>

#include <entt/entity/registry.hpp>

#include "sim/fixed.hpp"
#include "sim/rng.hpp"
#include "sim/tick.hpp"

namespace rts::sim {

// Posición en casillas.
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
    std::int32_t arena_tiles = 0;
    Fixed max_speed;  // casillas por tick
};

struct SnapshotEntity {
    std::uint32_t id;
    Position pos;
};

// Copia de solo lectura del estado relevante para presentar. El render nunca toca
// el registro de la simulación: lee dos snapshots consecutivos e interpola.
struct Snapshot {
    Tick tick = 0;
    Fixed arena_size;
    std::vector<SnapshotEntity> entities;
};

class World {
public:
    explicit World(const DemoParams& params);

    void step();

    [[nodiscard]] Tick tick() const noexcept { return tick_; }
    [[nodiscard]] std::uint64_t state_hash() const;
    void write_snapshot(Snapshot& out) const;

private:
    entt::registry registry_;
    Xoshiro256pp rng_;
    Fixed arena_size_;
    Tick tick_ = 0;
};

}  // namespace rts::sim
