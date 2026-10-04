// Pruebas del fuego: sin asedio, atacar un edificio es prenderle fuego; un fuego débil
// se apaga solo y uno sostenido crece, quema y se propaga a la madera vecina; los
// aldeanos lo apagan; la piedra no cae por el fuego: queda quemada e inutilizada
// hasta que se repara con madera.

#include <cstdint>
#include <vector>

#include <doctest/doctest.h>

#include "sim/fire.hpp"
#include "sim/world.hpp"
#include "test_helpers.hpp"

using rts::sim::Command;
using rts::sim::CommandType;
using rts::sim::Fire;
using rts::sim::Health;
using rts::sim::World;
using rts::sim::WorldParams;
using namespace rts::test;

namespace {

WorldParams flat_two_players() {
    WorldParams p = test_world_params(0, 16);
    p.map.bands = {{rts::sim::kElevationRange, 1}};
    p.demo.player = 1;  // dos jugadores
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

std::int32_t hp(const World& w, std::uint32_t id) {
    return w.registry().get<Health>(ent(id)).hp;
}

}  // namespace

TEST_CASE("Fuego: un golpe sin asedio no quita vida al edificio; prende fuego") {
    World world(flat_two_players());
    const auto house = *world.spawn_building(1, kHouse, {102, 99}, true);
    const auto soldier = world.spawn_unit(0, kSoldier, {101, 100});
    world.step();
    world.issue(order(CommandType::Attack, 0, {soldier}, house));
    // Hasta el primer golpe y algo después: la vida no baja por el golpe en sí.
    for (int t = 0; t < 5; ++t) {
        world.step();
    }
    CHECK(hp(world, house) == 500);
    CHECK(world.registry().all_of<Fire>(ent(house)));
}

TEST_CASE("Fuego: uno débil se apaga solo sin dañar") {
    WorldParams p = flat_two_players();
    p.unit_types[kSoldier].combat.reload_ticks = 100'000;  // un solo golpe
    World world(p);
    const auto house = *world.spawn_building(1, kHouse, {102, 99}, true);
    const auto soldier = world.spawn_unit(0, kSoldier, {101, 100});
    world.step();
    world.issue(order(CommandType::Attack, 0, {soldier}, house));
    for (int t = 0; t < 10; ++t) {
        world.step();
    }
    REQUIRE(world.registry().all_of<Fire>(ent(house)));
    CHECK(world.registry().get<Fire>(ent(house)).intensity < 200);
    // 60 de intensidad bajo el sostén (200) mengua 2 por tick: apagado en 30 ticks.
    for (int t = 0; t < 40; ++t) {
        world.step();
    }
    CHECK_FALSE(world.registry().all_of<Fire>(ent(house)));
    CHECK(hp(world, house) == 500);
}

TEST_CASE("Fuego: sostenido, una casa de madera arde hasta caer y prende a su vecina") {
    World world(flat_two_players());
    const auto house = *world.spawn_building(1, kHouse, {102, 99}, true);
    const auto neighbor = *world.spawn_building(1, kHouse, {105, 99}, true);  // a 1 casilla
    const auto far = *world.spawn_building(1, kHouse, {120, 99}, true);
    std::vector<std::uint32_t> raiders;
    for (std::int32_t i = 0; i < 4; ++i) {
        raiders.push_back(world.spawn_unit(0, kSoldier, {101, 98 + i}));
    }
    world.step();
    world.issue(order(CommandType::Attack, 0, raiders, house));
    // Prendido y abandonado: los atacantes se van.
    int t = 0;
    for (; t < 400 && (!world.registry().all_of<Fire>(ent(house)) ||
                       world.registry().get<Fire>(ent(house)).intensity < 300);
         ++t) {
        world.step();
    }
    REQUIRE(t < 400);
    Command away = order(CommandType::Move, 0, raiders, rts::sim::kNoObject);
    away.target = {60, 60};
    world.issue(away);
    int burned_at = -1;
    for (int k = 0; k < 2400 && burned_at < 0; ++k) {
        world.step();
        if (!world.registry().valid(ent(house))) {
            burned_at = k;
        }
    }
    REQUIRE(burned_at >= 0);
    MESSAGE("la casa cae a los " << burned_at / 20 << " s de quedar ardiendo sola");
    CHECK(burned_at < 1200);  // menos de un minuto
    // La vecina prendió; la lejana, no.
    const bool neighbor_hit = !world.registry().valid(ent(neighbor)) ||
                              world.registry().all_of<Fire>(ent(neighbor)) || hp(world, neighbor) < 500;
    CHECK(neighbor_hit);
    CHECK(world.registry().valid(ent(far)));
    CHECK_FALSE(world.registry().all_of<Fire>(ent(far)));
}

TEST_CASE("Fuego: los aldeanos lo apagan a tiempo") {
    World world(flat_two_players());
    const auto house = *world.spawn_building(1, kHouse, {102, 99}, true);
    std::vector<std::uint32_t> raiders;
    for (std::int32_t i = 0; i < 4; ++i) {
        raiders.push_back(world.spawn_unit(0, kSoldier, {101, 98 + i}));
    }
    std::vector<std::uint32_t> villagers;
    for (std::int32_t i = 0; i < 3; ++i) {
        villagers.push_back(world.spawn_unit(1, kVillager, {110, 98 + i}));
    }
    world.step();
    world.issue(order(CommandType::Attack, 0, raiders, house));
    for (int t = 0; t < 400 && (!world.registry().all_of<Fire>(ent(house)) ||
                                world.registry().get<Fire>(ent(house)).intensity < 300);
         ++t) {
        world.step();
    }
    REQUIRE(world.registry().all_of<Fire>(ent(house)));
    Command away = order(CommandType::Move, 0, raiders, rts::sim::kNoObject);
    away.target = {60, 60};
    world.issue(away);
    world.issue(order(CommandType::Extinguish, 1, villagers, house));
    for (int t = 0; t < 600 && world.registry().all_of<Fire>(ent(house)); ++t) {
        world.step();
    }
    CHECK(world.registry().valid(ent(house)));
    CHECK_FALSE(world.registry().all_of<Fire>(ent(house)));
    CHECK(hp(world, house) > 0);
    // Terminado, nadie sigue con la tarea de apagar.
    world.step();
    for (const auto v : villagers) {
        CHECK_FALSE(world.registry().all_of<rts::sim::Extinguisher>(ent(v)));
    }
}

TEST_CASE("Fuego: la piedra queda quemada e inutilizada y se repara con madera") {
    WorldParams p = flat_two_players();
    p.building_types[kCenter].material = rts::sim::Material::Stone;
    World world(p);
    const auto center = *world.spawn_building(1, kCenter, {100, 100}, true);
    std::vector<std::uint32_t> raiders;
    for (std::int32_t i = 0; i < 6; ++i) {
        raiders.push_back(world.spawn_unit(0, kSoldier, {97, 98 + i}));
    }
    world.set_stock(1, stock(0, 1000, 0, 0));
    world.step();
    REQUIRE(world.player_state(1).population_cap == 5);
    world.issue(order(CommandType::Attack, 0, raiders, center));
    const auto& reg = world.registry();
    for (int t = 0; t < 6000 && !reg.get<rts::sim::Building>(ent(center)).burned; ++t) {
        world.step();
    }
    REQUIRE(reg.get<rts::sim::Building>(ent(center)).burned);
    // En pie, al 60 % como mucho, y sin función: no da plazas ni entrena.
    CHECK(reg.valid(ent(center)));
    CHECK(hp(world, center) <= 2000 * 60 / 100);
    CHECK(hp(world, center) > 0);
    world.step();
    CHECK(world.player_state(1).population_cap == 0);
    // Los atacantes la dejan: ya no le pueden hacer nada.
    for (int t = 0; t < 60; ++t) {
        world.step();
    }
    for (const auto r : raiders) {
        CHECK(reg.get<rts::sim::Combatant>(ent(r)).target == entt::entity{entt::null});
    }
    // Sin fuego, los aldeanos la reparan pagando madera; al terminar vuelve a funcionar.
    for (int t = 0; t < 600 && reg.all_of<Fire>(ent(center)); ++t) {
        world.step();
    }
    REQUIRE_FALSE(reg.all_of<Fire>(ent(center)));
    std::vector<std::uint32_t> builders;
    for (std::int32_t i = 0; i < 3; ++i) {
        builders.push_back(world.spawn_unit(1, kVillager, {104, 100 + i}));
    }
    world.step();
    world.issue(order(CommandType::Build, 1, builders, center));
    for (int t = 0; t < 3000 && reg.get<rts::sim::Building>(ent(center)).burned; ++t) {
        world.step();
    }
    CHECK_FALSE(reg.get<rts::sim::Building>(ent(center)).burned);
    CHECK(hp(world, center) == 2000);
    const auto wood = world.player_state(1).stock[rts::sim::resource_index(rts::sim::Resource::Wood)];
    // El 40 % de la vida devuelta, al 50 % del coste en madera (200): unos 40.
    CHECK(wood < 1000);
    CHECK(wood >= 1000 - 45);
    world.step();
    CHECK(world.player_state(1).population_cap == 5);
}

TEST_CASE("Asedio: el ariete daña edificios (también la piedra) e ignora a las unidades") {
    WorldParams p = flat_two_players();
    p.building_types[kCenter].material = rts::sim::Material::Stone;
    World world(p);
    const auto center = *world.spawn_building(1, kCenter, {100, 100}, true);
    const auto ram = world.spawn_unit(0, kRam, {98, 101});
    const auto enemy = world.spawn_unit(1, kSoldier, {97, 104});  // a la vista, pero es una unidad
    world.step();
    // Una orden de atacar a una unidad no le afecta.
    world.issue(order(CommandType::Attack, 0, {ram}, enemy));
    world.step();
    CHECK(world.registry().get<rts::sim::Combatant>(ent(ram)).target != ent(enemy));
    world.issue(order(CommandType::Attack, 0, {ram}, center));
    for (int t = 0; t < 400; ++t) {
        world.step();
    }
    // 50 - 3 de armadura = 47 por golpe cada 80 ticks: unos 5 golpes en 400 ticks.
    CHECK(hp(world, center) <= 2000 - 47 * 4);
    CHECK_FALSE(world.registry().all_of<Fire>(ent(center)));  // daño, no fuego
}

TEST_CASE("Requisitos: el taller de asedio solo se coloca con un cuartel terminado, y sin él no se cobra") {
    World world(flat_two_players());
    const auto villager = world.spawn_unit(0, kVillager, {100, 100});
    world.set_stock(0, stock(0, 1000, 0, 0));
    world.step();
    CHECK_FALSE(world.meets_requirements(0, kWorkshop));
    Command place = order(CommandType::Place, 0, {villager}, rts::sim::kNoObject);
    place.kind = kWorkshop;
    place.target = {110, 110};
    world.issue(place);
    world.step();
    CHECK(world.player_state(0).stock[rts::sim::resource_index(rts::sim::Resource::Wood)] == 1000);
    CHECK_FALSE(world.object_at({111, 111}).has_value());
    // Un cuartel en obra no basta; terminado, sí.
    REQUIRE(world.spawn_building(0, kBarracks, {120, 100}, false).has_value());
    world.step();
    CHECK_FALSE(world.meets_requirements(0, kWorkshop));
    REQUIRE(world.spawn_building(0, kBarracks, {120, 110}, true).has_value());
    world.step();
    CHECK(world.meets_requirements(0, kWorkshop));
    world.issue(place);
    world.step();
    CHECK(world.player_state(0).stock[rts::sim::resource_index(rts::sim::Resource::Wood)] == 800);
    CHECK(world.object_at({111, 111}).has_value());
}

TEST_CASE("Escombros: un edificio derribado por asedio deja materiales recuperables; quemado, no") {
    WorldParams p = flat_two_players();
    p.building_types[kCenter].material = rts::sim::Material::Stone;
    p.building_types[kCenter].cost = stock(0, 200, 100, 0);
    p.building_types[kCenter].hp = 300;
    World world(p);
    const auto center = *world.spawn_building(1, kCenter, {100, 100}, true);
    std::vector<std::uint32_t> rams;
    for (std::int32_t i = 0; i < 2; ++i) {
        rams.push_back(world.spawn_unit(0, kRam, {98, 100 + i}));
    }
    world.step();
    world.issue(order(CommandType::Attack, 0, rams, center));
    for (int t = 0; t < 3000 && world.registry().valid(ent(center)); ++t) {
        world.step();
    }
    REQUIRE_FALSE(world.registry().valid(ent(center)));
    // En el solar, escombros de piedra: el 50 % de 100.
    const auto rubble = world.object_at({101, 101});
    REQUIRE(rubble.has_value());
    const auto& node = world.registry().get<rts::sim::ResourceNode>(ent(*rubble));
    CHECK(node.kind == rts::sim::Resource::Stone);
    CHECK(node.amount == 50);
    CHECK(world.registry().get<rts::sim::Footprint>(ent(*rubble)).size == 3);

    // Una casa de madera que arde hasta caer no deja nada.
    const auto house = *world.spawn_building(1, kHouse, {120, 100}, true);
    std::vector<std::uint32_t> raiders;
    for (std::int32_t i = 0; i < 4; ++i) {
        raiders.push_back(world.spawn_unit(0, kSoldier, {119, 99 + i}));
    }
    world.step();
    world.issue(order(CommandType::Attack, 0, raiders, house));
    for (int t = 0; t < 3000 && world.registry().valid(ent(house)); ++t) {
        world.step();
    }
    REQUIRE_FALSE(world.registry().valid(ent(house)));
    CHECK_FALSE(world.object_at({120, 100}).has_value());
}

TEST_CASE("Demolición: los aldeanos desmontan un edificio propio y quedan sus materiales para recoger") {
    World world(flat_two_players());
    const auto house = *world.spawn_building(0, kHouse, {100, 100}, true);  // madera, cuesta 30 de madera
    std::vector<std::uint32_t> workers;
    for (std::int32_t i = 0; i < 2; ++i) {
        workers.push_back(world.spawn_unit(0, kVillager, {98, 100 + i}));
    }
    world.step();
    world.issue(order(CommandType::Demolish, 0, workers, house));
    for (int t = 0; t < 2000 && world.registry().valid(ent(house)); ++t) {
        world.step();
    }
    REQUIRE_FALSE(world.registry().valid(ent(house)));
    const auto rubble = world.object_at({100, 100});
    REQUIRE(rubble.has_value());
    const auto& node = world.registry().get<rts::sim::ResourceNode>(ent(*rubble));
    CHECK(node.kind == rts::sim::Resource::Wood);
    CHECK(node.amount == 15);
    // Se pueden recoger como cualquier nodo.
    CHECK(rts::sim::EconomySystem::can_gather(world.registry(), ent(*rubble), 0));
}

TEST_CASE("Minado: el zapador ignora la armadura de la piedra; contra la madera, prende fuego") {
    WorldParams p = flat_two_players();
    p.building_types[kCenter].material = rts::sim::Material::Stone;
    p.building_types[kCenter].armor_melee = 40;  // muros que ningún golpe atraviesa
    rts::sim::UnitType sapper = p.unit_types[kSoldier];
    sapper.combat.attack_melee = 9;
    sapper.combat.reload_ticks = 80;
    sapper.combat.undermine = true;
    sapper.combat.buildings_only = true;
    sapper.combat.ignite = 30;
    p.unit_types.push_back(sapper);
    const auto kSapper = static_cast<rts::sim::UnitTypeId>(p.unit_types.size() - 1);
    World world(p);
    const auto center = *world.spawn_building(1, kCenter, {100, 100}, true);
    const auto miner = world.spawn_unit(0, kSapper, {98, 101});
    const auto house = *world.spawn_building(1, kHouse, {110, 100}, true);
    const auto torch = world.spawn_unit(0, kSapper, {109, 100});
    world.step();
    world.issue(order(CommandType::Attack, 0, {miner}, center));
    world.issue(order(CommandType::Attack, 0, {torch}, house));
    for (int t = 0; t < 400; ++t) {
        world.step();
    }
    // 9 por golpe sin armadura, uno cada 80 ticks: unos 5 golpes.
    CHECK(hp(world, center) <= 2000 - 9 * 4);
    CHECK_FALSE(world.registry().all_of<Fire>(ent(center)));
    // Contra la madera no mina: sus golpes solo avivan un fuego, y una antorcha sola
    // no lo sostiene (mengua más entre golpe y golpe de lo que aporta). Ni un rasguño.
    CHECK(hp(world, house) == 500);
}
