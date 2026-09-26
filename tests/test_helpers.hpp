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
inline constexpr sim::UnitTypeId kArcher = 2;
// Clases de armadura de prueba.
inline constexpr sim::ArmorClassId kClassVillager = 0;
inline constexpr sim::ArmorClassId kClassInfantry = 1;
inline constexpr sim::ArmorClassId kClassArcher = 2;
inline constexpr sim::ArmorClassId kClassBuilding = 3;
inline constexpr sim::BuildingTypeId kCenter = 0;
inline constexpr sim::BuildingTypeId kHouse = 1;
inline constexpr sim::BuildingTypeId kLumberCamp = 2;
inline constexpr sim::BuildingTypeId kBarracks = 3;
inline constexpr sim::BuildingTypeId kFarm = 4;
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
    soldier.combat.hp = 40;
    soldier.combat.attack_melee = 5;
    soldier.combat.armor_melee = 1;
    soldier.combat.armor_class = kClassInfantry;
    soldier.combat.range = sim::Fixed::from_ratio(15, 100);
    soldier.combat.reload_ticks = 20;
    soldier.combat.sight_tiles = 6;
    sim::UnitType villager;
    villager.radius = sim::Fixed::from_ratio(1, 4);
    villager.speed = sim::Fixed::from_ratio(5, 100);
    villager.cost = stock(50, 0, 0, 0);
    villager.train_ticks = 200;
    villager.worker = true;
    villager.carry_capacity = 10;
    villager.combat.hp = 25;
    villager.combat.attack_melee = 3;
    villager.combat.armor_class = kClassVillager;
    villager.combat.range = sim::Fixed::from_ratio(15, 100);
    villager.combat.reload_ticks = 20;
    villager.combat.sight_tiles = 4;
    villager.combat.auto_attack = false;
    sim::UnitType archer;
    archer.radius = sim::Fixed::from_ratio(1, 4);
    archer.speed = sim::Fixed::from_ratio(5, 100);
    archer.cost = stock(0, 25, 0, 45);
    archer.train_ticks = 150;
    archer.combat.hp = 30;
    archer.combat.attack_pierce = 4;
    archer.combat.armor_class = kClassArcher;
    archer.combat.bonus[kClassInfantry] = 1;
    archer.combat.range = sim::Fixed::from_int(4);
    archer.combat.reload_ticks = 20;
    archer.combat.sight_tiles = 7;
    archer.combat.projectile_speed = sim::Fixed::from_ratio(35, 100);
    return {soldier, villager, archer};
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
    center.armor_melee = 3;
    center.armor_pierce = 8;
    center.armor_class = kClassBuilding;
    sim::BuildingType house;
    house.size = 2;
    house.cost = stock(0, 30, 0, 0);
    house.build_ticks = 300;
    house.hp = 500;
    house.population = 5;
    house.armor_melee = 3;
    house.armor_pierce = 8;
    house.armor_class = kClassBuilding;
    sim::BuildingType camp;
    camp.size = 2;
    camp.cost = stock(0, 100, 0, 0);
    camp.build_ticks = 400;
    camp.hp = 600;
    camp.accepts = sim::resource_bit(sim::Resource::Wood);
    camp.armor_class = kClassBuilding;
    sim::BuildingType barracks;
    barracks.size = 3;
    barracks.cost = stock(0, 150, 0, 0);
    barracks.build_ticks = 400;
    barracks.hp = 800;
    barracks.armor_melee = 2;
    barracks.armor_pierce = 6;
    barracks.armor_class = kClassBuilding;
    barracks.trains = {kSoldier, kArcher};
    sim::BuildingType farm;
    farm.size = 2;
    farm.cost = stock(0, 60, 0, 0);
    farm.build_ticks = 150;
    farm.hp = 200;
    farm.armor_class = kClassBuilding;
    farm.farm_food = 200;
    return {center, house, camp, barracks, farm};
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

inline sim::CombatParams test_combat_params() {
    sim::CombatParams c;
    c.acquire_interval_ticks = 10;
    c.repath_tiles = 2;
    c.chase_attempts = 3;
    c.building_reach = sim::Fixed::from_ratio(3, 5);
    c.projectile_hit_radius = sim::Fixed::from_ratio(15, 100);
    c.xp_kill_bonus = 20;
    c.level_thresholds = {10, 20, 30};  // 3 niveles: el tercero es héroe
    c.hp_percent_per_level = 10;
    c.attack_percent_per_level = 10;
    c.armor_every_levels = 2;
    c.hero_aura_radius = sim::Fixed::from_int(4);
    c.hero_aura_attack_percent = 20;
    c.hero_name_count = 2;
    return c;
}

inline sim::AiProfile test_ai_profile() {
    using B = sim::AiBehavior;
    sim::AiProfile a;
    a.behaviors = {B::Defend, B::Villagers, B::Houses, B::Barracks, B::Farms,
                   B::Dropoffs, B::Builders, B::Gather, B::Army, B::Attack};
    a.villager_target = 15;
    a.gather_percent = {40, 35, 10, 15};
    a.house_margin = 3;
    a.barracks_at_villagers = 8;
    a.dropoff_distance_tiles = 12;
    a.dropoff_min_gatherers = 3;
    a.builders = 2;
    a.gatherers_per_farm = 2;
    a.build_gap_tiles = 1;
    a.build_search_radius_tiles = 24;
    a.first_wave = 4;
    a.wave_growth = 2;
    a.defend_radius_tiles = 14;
    a.flee_enemy_tiles = 3;
    a.safe_base_tiles = 2;
    a.barracks_queue = 2;
    a.villager_queue = 1;
    a.army = {kSoldier, kArcher};
    return a;
}

// Perfil "normal" de prueba: módulos que piensan mejor, mismos umbrales económicos.
inline sim::AiProfile test_ai_profile_normal() {
    using B = sim::AiBehavior;
    sim::AiProfile a = test_ai_profile();
    a.behaviors = {B::Defend, B::FocusFire, B::AttackStrength, B::Villagers, B::Houses, B::Barracks,
                   B::Farms,  B::Dropoffs,  B::Builders,       B::Gather,    B::ArmyCounter};
    a.villager_queue = 2;
    a.house_margin = 5;
    a.attack_ratio_percent = 130;
    a.retreat_ratio_percent = 60;
    a.min_attack_army = 4;
    a.engage_radius_tiles = 8;
    return a;
}

inline sim::AiParams test_ai_params() {
    sim::AiParams a;
    a.think_interval_ticks = 20;
    a.worker_type = kVillager;
    a.house = kHouse;
    a.barracks = kBarracks;
    a.farm = kFarm;
    a.dropoff = {kCenter, kLumberCamp, kCenter, kCenter};
    a.profiles = {test_ai_profile(), test_ai_profile_normal()};
    return a;
}

inline sim::WorldParams test_world_params(std::int32_t units = 1000, std::int32_t area = 64) {
    sim::WorldParams p;
    p.map = test_map_params();
    p.passable_by_terrain = {0, 1, 1};
    p.unit_types = test_unit_types();
    p.building_types = test_building_types();
    p.node_types = test_node_types();
    p.economy = test_economy_params();
    p.combat = test_combat_params();
    p.ai = test_ai_params();
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
