// Pruebas de la sanidad: el herido ingresa en un puesto médico, queda inoperativo y
// sin bienes y se cura con el tiempo, antes si hay enfermeros; las camas son
// limitadas; si el puesto cae, mueren los ingresados; al alta (o si se le saca) tarda
// en reorganizarse; los pacientes comen como aldeanos; fuera de los puestos solo lo
// leve sana solo, en calma.

#include <cstdint>
#include <vector>

#include <doctest/doctest.h>

#include "sim/world.hpp"
#include "test_helpers.hpp"

using rts::sim::Command;
using rts::sim::CommandType;
using rts::sim::Health;
using rts::sim::Patient;
using rts::sim::World;
using rts::sim::WorldParams;
using namespace rts::test;

namespace {

constexpr std::int32_t kReorganize = 50;

struct MedWorld {
    WorldParams params = test_world_params(0, 16);
    rts::sim::BuildingTypeId hospital = 0;
    rts::sim::BuildingTypeId aid = 0;  // puesto de socorro: estabiliza hasta el 70 %

    MedWorld() {
        params.map.bands = {{rts::sim::kElevationRange, 1}};
        params.demo.player = 1;
        for (auto& u : params.unit_types) {
            u.treatable = u.combat.buildings_only == false;
        }
        auto& archer = params.unit_types[kArcher].supply;
        archer.rations = 10;
        archer.ration_ticks = 1000;
        archer.ammo = 10;
        archer.ammo_bundle = 5;
        rts::sim::BuildingType h = params.building_types[kHouse];
        h.population = 0;
        h.beds = 2;
        h.nurses = 1;
        h.care_percent = 100;
        h.heal_to_percent = 100;
        hospital = static_cast<rts::sim::BuildingTypeId>(params.building_types.size());
        params.building_types.push_back(h);
        h.heal_to_percent = 70;
        aid = static_cast<rts::sim::BuildingTypeId>(params.building_types.size());
        params.building_types.push_back(h);
        auto& m = params.medicine;
        m.bed_heal_milli_per_tick = 50;
        m.nurse_heal_milli_per_tick = 450;
        m.patients_per_nurse = 1;
        m.patient_ration_ticks = 100;
        m.reorganize_ticks = kReorganize;
        m.care_reach = rts::sim::Fixed::from_ratio(4, 5);
        m.light_wound_percent = 70;
        m.natural_heal_interval_ticks = 10;
        m.calm_ticks = 20;
        params.supply.ration_cost = stock(1, 0, 0, 0);
    }
};

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

void run(World& w, std::int32_t ticks) {
    for (std::int32_t t = 0; t < ticks; ++t) {
        w.step();
    }
}

std::int32_t hp(const World& w, std::uint32_t id) {
    return w.registry().get<Health>(ent(id)).hp;
}

bool admitted(const World& w, std::uint32_t id) {
    const Patient* p = w.registry().try_get<Patient>(ent(id));
    return p != nullptr && p->admitted;
}

// Ticks hasta el alta de un soldado con 5 de vida (de 40), con o sin enfermero.
std::int32_t ticks_to_heal(bool nurse) {
    MedWorld mw;
    World world(mw.params);
    world.set_stock(0, stock(1000, 0, 0, 0));
    const auto post = *world.spawn_building(0, mw.hospital, {104, 99}, true);
    const auto soldier = world.spawn_unit(0, kSoldier, {102, 100});
    if (nurse) {
        const auto v = world.spawn_unit(0, kVillager, {106, 98});
        world.issue(order(CommandType::Tend, 0, {v}, post));
    }
    world.set_hp(soldier, 5);
    world.issue(order(CommandType::Treat, 0, {soldier}, post));
    for (std::int32_t t = 0; t < 20'000; ++t) {
        world.step();
        if (world.registry().all_of<rts::sim::Reorganizing>(ent(soldier))) {
            CHECK(hp(world, soldier) == world.registry().get<Health>(ent(soldier)).max_hp);
            return t;
        }
    }
    return -1;
}

}  // namespace

TEST_CASE("Sanidad: el herido ingresa sin bienes, no combate y se cura; con enfermero, antes") {
    MedWorld mw;
    World world(mw.params);
    world.set_stock(0, stock(1000, 0, 0, 0));
    const auto post = *world.spawn_building(0, mw.hospital, {104, 99}, true);
    const auto archer = world.spawn_unit(0, kArcher, {100, 100});
    const auto enemy = world.spawn_unit(1, kVillager, {101, 103});  // a su vista
    world.set_hp(archer, 5);
    world.issue(order(CommandType::Treat, 0, {archer}, post));
    run(world, 120);
    REQUIRE(admitted(world, archer));
    const auto& s = world.registry().get<rts::sim::Supply>(ent(archer));
    CHECK(s.ammo == 0);
    CHECK(s.rations == 0);
    CHECK(world.registry().get<rts::sim::Combatant>(ent(archer)).target == entt::entity{entt::null});
    CHECK(hp(world, enemy) == world.registry().get<Health>(ent(enemy)).max_hp);

    const std::int32_t alone = ticks_to_heal(false);
    const std::int32_t attended = ticks_to_heal(true);
    REQUIRE(alone > 0);
    REQUIRE(attended > 0);
    CHECK(attended * 3 < alone);
}

TEST_CASE("Sanidad: tras el alta se reorganiza sin atacar y después vuelve a combatir") {
    MedWorld mw;
    World world(mw.params);
    world.set_stock(0, stock(1000, 0, 0, 0));
    const auto post = *world.spawn_building(0, mw.hospital, {104, 99}, true);
    const auto soldier = world.spawn_unit(0, kSoldier, {102, 100});
    world.set_hp(soldier, 39);
    world.issue(order(CommandType::Treat, 0, {soldier}, post));
    for (std::int32_t t = 0; t < 2000 && !world.registry().all_of<rts::sim::Reorganizing>(ent(soldier)); ++t) {
        world.step();
    }
    REQUIRE(world.registry().all_of<rts::sim::Reorganizing>(ent(soldier)));
    const auto enemy = world.spawn_unit(1, kVillager, {100, 101});
    run(world, kReorganize - 5);
    CHECK(hp(world, enemy) == world.registry().get<Health>(ent(enemy)).max_hp);  // aún no ataca
    run(world, 200);
    CHECK_FALSE(world.registry().all_of<rts::sim::Reorganizing>(ent(soldier)));
    CHECK((!world.registry().valid(ent(enemy)) || hp(world, enemy) < world.registry().get<Health>(ent(enemy)).max_hp));
}

TEST_CASE("Sanidad: camas limitadas; el que no cabe espera y entra al quedar una libre") {
    MedWorld mw;
    mw.params.building_types[mw.hospital].beds = 1;
    World world(mw.params);
    world.set_stock(0, stock(1000, 0, 0, 0));
    const auto post = *world.spawn_building(0, mw.hospital, {104, 99}, true);
    const auto a = world.spawn_unit(0, kSoldier, {102, 100});
    const auto b = world.spawn_unit(0, kSoldier, {102, 102});
    world.set_hp(a, 30);
    world.set_hp(b, 30);
    world.issue(order(CommandType::Treat, 0, {a, b}, post));
    run(world, 100);
    CHECK(admitted(world, a) != admitted(world, b));
    // Hasta que los dos han pasado por la cama y han salido.
    for (std::int32_t t = 0;
         t < 5000 && (world.registry().all_of<Patient>(ent(a)) || world.registry().all_of<Patient>(ent(b))); ++t) {
        world.step();
    }
    CHECK(hp(world, a) == world.registry().get<Health>(ent(a)).max_hp);
    CHECK(hp(world, b) == world.registry().get<Health>(ent(b)).max_hp);
}

TEST_CASE("Sanidad: si cae el puesto, mueren los ingresados") {
    MedWorld mw;
    World world(mw.params);
    world.set_stock(0, stock(1000, 0, 0, 0));
    const auto post = *world.spawn_building(0, mw.hospital, {104, 99}, true);
    const auto soldier = world.spawn_unit(0, kSoldier, {102, 100});
    world.set_hp(soldier, 5);
    world.issue(order(CommandType::Treat, 0, {soldier}, post));
    run(world, 60);
    REQUIRE(admitted(world, soldier));
    // Lo derriban (aquí, desmontado por aldeanos propios con el puesto casi en ruinas).
    world.set_hp(post, 1);
    const auto v = world.spawn_unit(0, kVillager, {106, 98});
    world.issue(order(CommandType::Demolish, 0, {v}, post));
    for (std::int32_t t = 0; t < 500 && world.registry().valid(ent(post)); ++t) {
        world.step();
    }
    REQUIRE_FALSE(world.registry().valid(ent(post)));
    run(world, 2);
    CHECK_FALSE(world.registry().valid(ent(soldier)));
}

TEST_CASE("Sanidad: otra orden lo saca del puesto, sin curar, y tiene que reorganizarse") {
    MedWorld mw;
    World world(mw.params);
    world.set_stock(0, stock(1000, 0, 0, 0));
    const auto post = *world.spawn_building(0, mw.hospital, {104, 99}, true);
    const auto soldier = world.spawn_unit(0, kSoldier, {102, 100});
    world.set_hp(soldier, 5);
    world.issue(order(CommandType::Treat, 0, {soldier}, post));
    run(world, 60);
    REQUIRE(admitted(world, soldier));
    Command go = order(CommandType::Move, 0, {soldier}, rts::sim::kNoObject);
    go.target = {90, 100};
    world.issue(go);
    run(world, 2);
    CHECK_FALSE(world.registry().all_of<Patient>(ent(soldier)));
    CHECK(world.registry().all_of<rts::sim::Reorganizing>(ent(soldier)));
    CHECK(hp(world, soldier) < 20);
}

TEST_CASE("Sanidad: los pacientes comen del almacén; sin comida no mejoran") {
    MedWorld mw;
    World world(mw.params);
    world.set_stock(0, stock(0, 0, 0, 0));
    const auto post = *world.spawn_building(0, mw.hospital, {104, 99}, true);
    const auto soldier = world.spawn_unit(0, kSoldier, {102, 100});
    world.set_hp(soldier, 5);
    world.issue(order(CommandType::Treat, 0, {soldier}, post));
    run(world, 60);
    REQUIRE(admitted(world, soldier));
    run(world, 400);
    const std::int32_t starving = hp(world, soldier);
    CHECK(starving == 5);  // llegó sin bienes y no hay comida: no mejora nada
    world.set_stock(0, stock(10, 0, 0, 0));
    run(world, 400);
    CHECK(hp(world, soldier) > starving);
    CHECK(world.player_state(0).stock[0] < 10);
}

TEST_CASE("Sanidad: lo leve sana solo en calma; lo grave no; el socorro estabiliza y lo demás sana solo") {
    MedWorld mw;
    World world(mw.params);
    world.set_stock(0, stock(1000, 0, 0, 0));
    const auto light = world.spawn_unit(0, kSoldier, {100, 100});
    const auto grave = world.spawn_unit(0, kSoldier, {100, 104});
    world.set_hp(light, 30);  // 75 %: leve
    world.set_hp(grave, 20);  // 50 %: grave
    run(world, 400);
    CHECK(hp(world, light) == 40);
    CHECK(hp(world, grave) == 20);
    // Socorro: lo sube hasta el 70 % (28) y, ya leve, sana solo.
    const auto aid = *world.spawn_building(0, mw.aid, {104, 103}, true);
    world.issue(order(CommandType::Treat, 0, {grave}, aid));
    for (std::int32_t t = 0; t < 3000 && !world.registry().all_of<rts::sim::Reorganizing>(ent(grave)); ++t) {
        world.step();
    }
    CHECK(hp(world, grave) == 28);
    run(world, 400);
    CHECK(hp(world, grave) == 40);
}
