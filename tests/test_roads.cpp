// Pruebas de los caminos (C4): se anda por encima, también el enemigo; terminados,
// aceleran la marcha y quitan el castigo de ir fuera de llano, así que la carreta gana
// mucho más; en obra no; nadie los ataca por su cuenta.

#include <cstdint>
#include <vector>

#include <doctest/doctest.h>

#include "sim/world.hpp"
#include "test_helpers.hpp"

using rts::sim::TileCoord;
using rts::sim::World;
using rts::sim::WorldParams;
using namespace rts::test;

namespace {

struct RoadSetup {
    WorldParams params = test_world_params(0, 16);
    rts::sim::BuildingTypeId road = 0;

    RoadSetup() {
        params.map.bands = {{rts::sim::kElevationRange, 1}};
        params.demo.player = 1;
        rts::sim::BuildingType r = params.building_types[kHouse];
        r.size = 1;
        r.population = 0;
        r.road_speed_percent = 125;
        r.material = rts::sim::Material::Stone;
        road = static_cast<rts::sim::BuildingTypeId>(params.building_types.size());
        params.building_types.push_back(r);
    }
};

// Ticks que tarda una unidad del tipo dado de (17, 24) a (30, 24), con o sin camino.
// Todo dentro de un mismo sector de 16x16: el camino recto va por encima del camino
// (la búsqueda de caminos aún no prefiere los caminos: entre sectores podría rodearlo).
int travel(RoadSetup s, rts::sim::UnitTypeId type, bool with_road, bool complete = true,
           rts::sim::PlayerId road_owner = 0) {
    World w(s.params);
    if (with_road) {
        for (std::int32_t x = 16; x <= 31; ++x) {
            REQUIRE(w.spawn_building(road_owner, s.road, {x, 24}, complete).has_value());
        }
    }
    const auto u = w.spawn_unit(0, type, {17, 24});
    w.issue(move_order(w.tick(), {u}, {30, 24}));
    for (int t = 0; t < 6000; ++t) {
        w.step();
        if (rts::sim::tile_of(w.registry().get<rts::sim::Position>(static_cast<entt::entity>(u))) ==
            TileCoord{30, 24}) {
            return t;
        }
    }
    return 6000;
}

}  // namespace

TEST_CASE("Caminos: se anda por encima y van más deprisa; en obra, no") {
    RoadSetup s;
    const int grass = travel(s, kSoldier, false);
    const int road = travel(s, kSoldier, true);
    REQUIRE(grass < 6000);
    CHECK(road < grass * 9 / 10);
    CHECK(travel(s, kSoldier, true, false) >= grass - 2);  // cimientos: como la hierba
    // El enemigo también los usa.
    CHECK(travel(s, kSoldier, true, true, 1) == road);
}

TEST_CASE("Caminos: la carreta, que se atasca fuera de llano, es la que más gana") {
    RoadSetup s;
    s.params.movement.speed_percent_by_terrain = {100, 60, 100};  // todo el mapa es «bosque»
    s.params.unit_types[kSoldier].rough_speed_percent = 50;       // el soldado hace de carreta
    const int cart_off = travel(s, kSoldier, false);
    const int cart_on = travel(s, kSoldier, true);
    const int foot_off = travel(s, kArcher, false);
    const int foot_on = travel(s, kArcher, true);
    REQUIRE(cart_off < 6000);
    // La carreta pasa del 30 % al 125 %; el arquero, del 60 % al 125 %.
    CHECK(cart_off * 10 > cart_on * 35);
    CHECK(foot_off * 10 < foot_on * 25);
}

TEST_CASE("Caminos: las tropas no atacan por su cuenta un camino enemigo") {
    RoadSetup s;
    World w(s.params);
    w.spawn_building(1, s.road, {31, 30}, true);
    const auto soldier = w.spawn_unit(0, kSoldier, {30, 30});
    for (int t = 0; t < 100; ++t) {
        w.step();
    }
    CHECK(w.registry().get<rts::sim::Combatant>(static_cast<entt::entity>(soldier)).target ==
          entt::entity{entt::null});
}
