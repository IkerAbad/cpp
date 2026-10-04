// Pruebas del suministro (M6): las raciones se gastan con el tiempo; sin ellas el
// ataque baja, los aldeanos trabajan más despacio y las tropas acaban muriendo de
// hambre; junto a un edificio propio que abastece se reponen víveres y munición
// pagando del almacén; un tirador sin munición no dispara.

#include <cstdint>
#include <vector>

#include <doctest/doctest.h>

#include "sim/supply.hpp"
#include "sim/world.hpp"
#include "test_helpers.hpp"

using rts::sim::Combatant;
using rts::sim::Command;
using rts::sim::CommandType;
using rts::sim::Health;
using rts::sim::Supply;
using rts::sim::World;
using rts::sim::WorldParams;
using namespace rts::test;

namespace {

constexpr std::int32_t kRations = 3;
constexpr std::int32_t kRationTicks = 10;
constexpr std::int32_t kStarveAfter = 40;
constexpr std::int32_t kStarveEvery = 5;
constexpr std::int32_t kAmmo = 4;
constexpr std::int32_t kBundle = 2;

WorldParams supplied_world() {
    WorldParams p = test_world_params(0, 16);
    p.map.bands = {{rts::sim::kElevationRange, 1}};
    p.demo.player = 1;  // dos jugadores
    for (auto& u : p.unit_types) {
        u.supply.rations = kRations;
        u.supply.ration_ticks = kRationTicks;
        u.supply.starves = !u.worker;
    }
    auto& archer = p.unit_types[kArcher].supply;
    archer.ammo = kAmmo;
    archer.ammo_bundle = kBundle;
    archer.ammo_cost = stock(0, 1, 0, 0);
    p.building_types[kCenter].supplies = true;
    p.supply.resupply_radius_tiles = 3;
    p.supply.resupply_interval_ticks = 1;
    p.supply.ration_cost = stock(1, 0, 0, 0);
    p.supply.starve_after_ticks = kStarveAfter;
    p.supply.starve_hp_interval_ticks = kStarveEvery;
    p.combat.hungry_attack_percent = 60;
    p.economy.hungry_work_percent = 50;
    return p;
}

entt::entity ent(std::uint32_t id) {
    return static_cast<entt::entity>(id);
}

Command order(CommandType type, rts::sim::PlayerId player, std::vector<std::uint32_t> units, std::uint32_t object) {
    Command c;
    c.type = type;
    c.player = player;
    c.units = std::move(units);
    c.object = object;
    return c;
}

const Supply& supply_of(const World& w, std::uint32_t id) {
    return w.registry().get<Supply>(ent(id));
}

void run(World& w, std::int32_t ticks) {
    for (std::int32_t t = 0; t < ticks; ++t) {
        w.step();
    }
}

}  // namespace

TEST_CASE("Suministro: works_this_tick reparte el trabajo de forma uniforme") {
    for (const std::int32_t percent : {0, 25, 50, 73, 100}) {
        std::int32_t worked = 0;
        for (rts::sim::Tick t = 0; t < 1000; ++t) {
            worked += rts::sim::works_this_tick(t, percent) ? 1 : 0;
        }
        CHECK(worked == percent * 10);
    }
    // Con 50 %, uno sí y otro no.
    CHECK(rts::sim::works_this_tick(0, 50) != rts::sim::works_this_tick(1, 50));
}

TEST_CASE("Suministro: las raciones se gastan y con hambre el ataque baja") {
    World world(supplied_world());
    const auto soldier = world.spawn_unit(0, kSoldier, {100, 100});
    world.step();
    CHECK(supply_of(world, soldier).rations == kRations);
    const auto& c = world.registry().get<Combatant>(ent(soldier));
    const std::int32_t fed = world.combat().attack_percent(world.registry(), ent(soldier), c, false);
    run(world, kRations * kRationTicks);
    CHECK(supply_of(world, soldier).hungry());
    const std::int32_t hungry = world.combat().attack_percent(world.registry(), ent(soldier), c, false);
    CHECK(hungry == fed * 60 / 100);
}

TEST_CASE("Suministro: el hambre prolongada mata a las tropas; a los aldeanos, no") {
    World world(supplied_world());
    const auto soldier = world.spawn_unit(0, kSoldier, {100, 100});
    const auto villager = world.spawn_unit(0, kVillager, {104, 100});
    const std::int32_t full = world.registry().get<Health>(ent(soldier)).hp;
    run(world, kRations * kRationTicks + kStarveAfter);
    CHECK(world.registry().get<Health>(ent(soldier)).hp == full);  // aún aguanta
    run(world, kStarveEvery * 3);
    CHECK(world.registry().get<Health>(ent(soldier)).hp == full - 3);
    run(world, kStarveEvery * full);
    CHECK_FALSE(world.registry().valid(ent(soldier)));
    CHECK(world.supply().last_stats().starved == 0);  // murió antes; este tick, nadie
    REQUIRE(world.registry().valid(ent(villager)));
    CHECK(world.registry().get<Health>(ent(villager)).hp == world.registry().get<Health>(ent(villager)).max_hp);
}

TEST_CASE("Suministro: junto a un edificio propio que abastece se reponen víveres pagando comida") {
    World world(supplied_world());
    world.set_stock(0, stock(100, 0, 0, 0));
    REQUIRE(world.spawn_building(0, kCenter, {104, 99}, true));
    const auto near = world.spawn_unit(0, kSoldier, {102, 100});
    const auto far = world.spawn_unit(0, kSoldier, {90, 100});
    run(world, kRations * kRationTicks * 4);
    CHECK_FALSE(supply_of(world, near).hungry());
    CHECK(supply_of(world, far).hungry());
    // Comida gastada = raciones repuestas.
    CHECK(world.player_state(0).stock[0] < 100);
    CHECK(world.player_state(0).stock[0] >= 100 - 4 * kRations);
}

TEST_CASE("Suministro: sin comida en el almacén no hay raciones; el edificio enemigo no abastece") {
    World world(supplied_world());
    world.set_stock(0, stock(0, 0, 0, 0));
    world.set_stock(1, stock(100, 0, 0, 0));
    REQUIRE(world.spawn_building(0, kCenter, {104, 99}, true));
    REQUIRE(world.spawn_building(1, kCenter, {104, 109}, true));
    const auto broke = world.spawn_unit(0, kSoldier, {102, 100});
    const auto foreign = world.spawn_unit(0, kSoldier, {102, 110});
    world.issue(order(CommandType::SetStance, 0, {broke, foreign}, rts::sim::kNoObject));
    run(world, kRations * kRationTicks + 1);
    CHECK(supply_of(world, broke).hungry());
    CHECK(supply_of(world, foreign).hungry());
    CHECK(world.player_state(1).stock[0] == 100);
}

TEST_CASE("Suministro: el tirador gasta munición, sin ella no dispara y la repone pagando madera") {
    World world(supplied_world());
    world.set_stock(0, stock(1000, 0, 0, 0));
    REQUIRE(world.spawn_building(0, kCenter, {94, 99}, true));
    const auto archer = world.spawn_unit(0, kArcher, {98, 100});
    const auto dummy = world.spawn_unit(1, kVillager, {101, 100});
    world.issue(order(CommandType::Attack, 0, {archer}, dummy));
    run(world, 400);
    CHECK(supply_of(world, archer).ammo == 0);
    const std::int32_t after_volley = world.registry().get<Health>(ent(dummy)).hp;
    CHECK(after_volley < world.registry().get<Health>(ent(dummy)).max_hp);
    run(world, 200);
    CHECK(world.registry().get<Health>(ent(dummy)).hp == after_volley);  // ya no dispara
    // Con madera, junto al centro, repone de kBundle en kBundle.
    world.set_stock(0, stock(1000, 1, 0, 0));
    run(world, 2);
    CHECK(supply_of(world, archer).ammo == kBundle);
    CHECK(world.player_state(0).stock[1] == 0);
}

TEST_CASE("Suministro: un aldeano hambriento recoge a la mitad de ritmo") {
    const auto gathered = [](bool hungry) {
        WorldParams p = supplied_world();
        auto& v = p.unit_types[kVillager];
        v.carry_capacity = 1000;  // no va a descargar: se mide solo el ritmo
        if (hungry) {
            v.supply.rations = 1;
            v.supply.ration_ticks = 1;
        } else {
            v.supply.rations = 1000;
            v.supply.ration_ticks = 1000;
        }
        World world(p);
        const auto node = *world.spawn_node(kGoldMine, {102, 100});
        const auto villager = world.spawn_unit(0, kVillager, {100, 100});
        world.issue(order(CommandType::Gather, 0, {villager}, node));
        run(world, 1000);
        rts::sim::Snapshot snap;
        world.write_snapshot(snap);
        return snap.entities.front().carried;
    };
    const std::int32_t fed = gathered(false);
    const std::int32_t hungry = gathered(true);
    CHECK(fed > 20);
    CHECK(hungry * 2 >= fed - 2);
    CHECK(hungry * 2 <= fed + 2);
}
