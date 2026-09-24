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
    m.path_node_budget_per_tick = 60'000;
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

// Tipos de la economía de prueba.
inline constexpr sim::UnitTypeId kSoldier = 0;
inline constexpr sim::UnitTypeId kVillager = 1;
inline constexpr sim::BuildingTypeId kCenter = 0;
inline constexpr sim::BuildingTypeId kHouse = 1;
inline constexpr sim::BuildingTypeId kLumberCamp = 2;
inline constexpr sim::NodeTypeId kTree = 0;
inline constexpr sim::NodeTypeId kGoldMine = 1;
inline constexpr sim::NodeTypeId kBerries = 2;

inline sim::Stock stock(std::int32_t food, std::int32_t wood, std::int32_t stone, std::int32_t gold) {
    return {food, wood, stone, gold};
}

inline std::vector<sim::UnitType> test_unit_types() {
    sim::UnitType soldier;
    soldier.radius = sim::Fixed::from_ratio(3, 10);
    soldier.speed = sim::Fixed::from_ratio(6, 100);
    soldier.cost = stock(60, 0, 0, 20);
    soldier.train_ticks = 100;
    sim::UnitType villager;
    villager.radius = sim::Fixed::from_ratio(1, 4);
    villager.speed = sim::Fixed::from_ratio(5, 100);
    villager.cost = stock(50, 0, 0, 0);
    villager.train_ticks = 200;
    villager.worker = true;
    villager.carry_capacity = 10;
    return {soldier, villager};
}

inline std::vector<sim::BuildingType> test_building_types() {
    constexpr std::uint8_t kAll = 0x0F;
    sim::BuildingType center;
    center.size = 3;
    center.cost = stock(0, 200, 0, 0);
    center.build_ticks = 1000;
    center.hp = 2000;
    center.accepts = kAll;
    center.population = 5;
    center.trains = {kVillager, kSoldier};
    sim::BuildingType house;
    house.size = 2;
    house.cost = stock(0, 30, 0, 0);
    house.build_ticks = 300;
    house.hp = 500;
    house.population = 5;
    sim::BuildingType camp;
    camp.size = 2;
    camp.cost = stock(0, 100, 0, 0);
    camp.build_ticks = 400;
    camp.hp = 600;
    camp.accepts = sim::resource_bit(sim::Resource::Wood);
    return {center, house, camp};
}

inline std::vector<sim::ResourceNodeType> test_node_types() {
    return {{sim::Resource::Wood, 100, 1}, {sim::Resource::Gold, 800, 2}, {sim::Resource::Food, 20, 1}};
}

inline sim::EconomyParams test_economy_params() {
    sim::EconomyParams e;
    e.gather_ticks = {10, 12, 14, 16};
    e.interact_range = sim::Fixed::from_ratio(4, 5);
    e.retarget_radius_tiles = 8;
    e.approach_attempts = 3;
    e.gatherers_per_tile = 2;
    e.queue_capacity = 5;
    e.max_population = 200;
    e.spawn_search_radius = 4;
    return e;
}

inline sim::WorldParams test_world_params(std::int32_t units = 1000, std::int32_t area = 64) {
    sim::WorldParams p;
    p.map = test_map_params();
    p.passable_by_terrain = {0, 1, 1};
    p.unit_types = test_unit_types();
    p.building_types = test_building_types();
    p.node_types = test_node_types();
    p.economy = test_economy_params();
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
