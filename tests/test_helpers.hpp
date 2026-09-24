#pragma once

// Parámetros de mundo fijos para las pruebas. No leen data/: si alguien retoca el
// TOML del juego, las pruebas de regresión no cambian.

#include "sim/world.hpp"

namespace rts::test {

inline sim::MapGenParams test_map_params() {
    sim::MapGenParams p;
    p.seed = 0x5EED'2026'0924ULL;
    p.width = 256;
    p.height = 256;
    p.noise_cell_tiles = 32;
    p.elevation_levels = 8;
    // Tres terrenos: 0 = agua, 1 = llanura, 2 = monte.
    p.bands = {{20'000, 0}, {45'000, 1}, {sim::kElevationRange, 2}};
    return p;
}

inline sim::WorldParams test_world_params() {
    sim::WorldParams p;
    p.map = test_map_params();
    p.demo.seed = 0x5EED'2026'0924ULL;
    p.demo.point_count = 1000;
    p.demo.area_tiles = 64;
    p.demo.max_speed = sim::Fixed::from_ratio(3, 10);
    return p;
}

}  // namespace rts::test
