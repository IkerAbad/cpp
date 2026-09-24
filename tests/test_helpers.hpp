#pragma once

// Parámetros de mundo fijos para las pruebas. No leen data/: si alguien retoca el
// TOML del juego, las pruebas de regresión no cambian.

#include <cstdint>
#include <vector>

#include "sim/world.hpp"

namespace rts::test {

inline sim::MapGenParams test_map_params() {
    sim::MapGenParams p;
    p.seed = 0x5EED'2026'0924ULL;
    p.width = 256;
    p.height = 256;
    p.noise_cell_tiles = 32;
    p.elevation_levels = 8;
    // Tres terrenos: 0 = agua (bloquea), 1 = llanura, 2 = monte.
    p.bands = {{20'000, 0}, {45'000, 1}, {sim::kElevationRange, 2}};
    return p;
}

inline sim::MovementParams test_movement_params() {
    sim::MovementParams m;
    m.hpa = {16, 8};
    m.path_node_budget_per_tick = 20'000;
    m.flow_field_min_group = 8;
    m.flow_field_cache_size = 4;
    m.retarget_radius_tiles = 16;
    m.neighbor_radius = sim::Fixed::from_int(2);
    m.max_neighbors = 8;
    m.time_horizon_ticks = 20;
    m.preference_weight = 100;
    m.collision_weight = 60;
    m.stuck_arrive_ticks = 40;
    m.arrive_radius = sim::Fixed::from_ratio(1, 4);
    m.waypoint_radius = sim::Fixed::from_ratio(45, 100);
    return m;
}

inline sim::WorldParams test_world_params(std::int32_t units = 1000, std::int32_t area = 64) {
    sim::WorldParams p;
    p.map = test_map_params();
    p.passable_by_terrain = {0, 1, 1};
    p.unit_types = {{sim::Fixed::from_ratio(3, 10), sim::Fixed::from_ratio(6, 100)}};
    p.movement = test_movement_params();
    p.demo.seed = 0x5EED'2026'0924ULL;
    p.demo.unit_type = 0;
    p.demo.count = units;
    p.demo.area_tiles = area;
    return p;
}

inline std::vector<std::uint32_t> all_unit_ids(const sim::World& world) {
    sim::Snapshot snap;
    world.write_snapshot(snap);
    std::vector<std::uint32_t> ids;
    for (const auto& e : snap.entities) {
        ids.push_back(e.id);
    }
    return ids;
}

inline sim::Command move_order(sim::Tick tick, std::vector<std::uint32_t> units, sim::TileCoord target) {
    sim::Command c;
    c.tick = tick;
    c.type = sim::CommandType::Move;
    c.units = std::move(units);
    c.target = target;
    return c;
}

}  // namespace rts::test
