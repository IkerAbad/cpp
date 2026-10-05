// Pruebas del terreno en combate (B2): la altura alarga el tiro y lo hace más dañino
// cuesta abajo (y al revés cuesta arriba); el bosque cubre de las flechas; el terreno
// frena la marcha y a la carreta más; la carga solo vale tras una carrera y en llano
// firme; desactivado, nada cambia.

#include <cstdint>
#include <vector>

#include <doctest/doctest.h>

#include "sim/combat.hpp"
#include "sim/world.hpp"
#include "test_helpers.hpp"

using rts::sim::Command;
using rts::sim::CommandType;
using rts::sim::Fixed;
using rts::sim::Stance;
using rts::sim::TileCoord;
using rts::sim::World;
using rts::sim::WorldParams;
using namespace rts::test;

namespace {

// Todo el mapa es llanura (terreno 1); la altura sigue variando, pero no cuenta
// (max_levels = 0) salvo que la prueba lo pida.
WorldParams terrain_params() {
    WorldParams p = test_world_params(0, 16);
    p.map.bands = {{rts::sim::kElevationRange, 1}};
    p.demo.player = 1;
    auto& t = p.combat.terrain;
    t.enabled = true;
    t.range_per_level = Fixed::from_ratio(1, 2);
    t.max_levels = 0;
    t.ranged_percent_per_level = 10;
    t.melee_percent_per_level = 0;
    t.charge_run_ticks = 20;
    t.arrow_cover_percent_by_terrain = {100, 100, 100};
    t.charge_by_terrain = {0, 1, 1};
    return p;
}

entt::entity ent(std::uint32_t id) {
    return static_cast<entt::entity>(id);
}

Command attack(rts::sim::PlayerId player, std::uint32_t unit, std::uint32_t target) {
    Command c;
    c.type = CommandType::Attack;
    c.player = player;
    c.units = {unit};
    c.object = target;
    return c;
}

void hold_ground(World& w, rts::sim::PlayerId player, std::vector<std::uint32_t> units) {
    Command c;
    c.type = CommandType::SetStance;
    c.player = player;
    c.units = std::move(units);
    c.kind = static_cast<std::uint8_t>(Stance::HoldGround);
    w.issue(c);
}

std::int32_t hp(const World& w, std::uint32_t id) {
    return w.registry().get<rts::sim::Health>(ent(id)).hp;
}

// Daño del primer golpe de un soldado (que carga al 300 %) a un aldeano que no se
// defiende, empezando a la distancia dada.
std::int32_t first_blow(WorldParams p, std::int32_t start_gap) {
    p.unit_types[kSoldier].charge_percent = 300;
    p.unit_types[kVillager].combat.hp = 400;
    World w(p);
    const auto soldier = w.spawn_unit(0, kSoldier, {30, 30});
    const auto villager = w.spawn_unit(1, kVillager, {30 + start_gap, 30});
    w.issue(attack(0, soldier, villager));
    const std::int32_t full = hp(w, villager);
    for (int t = 0; t < 600 && hp(w, villager) == full; ++t) {
        w.step();
    }
    return full - hp(w, villager);
}

}  // namespace

TEST_CASE("Terreno: la altura alarga el tiro cuesta abajo y lo acorta cuesta arriba, sin bajar de la mitad") {
    const Fixed range = Fixed::from_int(4);
    const Fixed per = Fixed::from_ratio(1, 2);
    CHECK(rts::sim::slope_range(range, per, 0) == range);
    CHECK(rts::sim::slope_range(range, per, 2) == Fixed::from_int(5));
    CHECK(rts::sim::slope_range(range, per, -2) == Fixed::from_int(3));
    CHECK(rts::sim::slope_range(range, per, -10) == Fixed::from_int(2));  // la mitad, como mucho
}

TEST_CASE("Terreno: la altura da daño cuesta abajo y lo quita cuesta arriba") {
    CHECK(rts::sim::slope_percent(100, 10, 0) == 100);
    CHECK(rts::sim::slope_percent(100, 10, 3) == 130);
    CHECK(rts::sim::slope_percent(100, 10, -3) == 70);
    CHECK(rts::sim::slope_percent(100, 50, -5) == 0);  // nunca negativo
}

TEST_CASE("Terreno: entre árboles las flechas hacen la mitad") {
    const auto damage_taken = [](std::int32_t cover) {
        WorldParams p = terrain_params();
        p.combat.terrain.arrow_cover_percent_by_terrain = {100, cover, 100};
        p.unit_types[kSoldier].combat.hp = 400;
        World w(p);
        const auto archer = w.spawn_unit(0, kArcher, {30, 30});
        const auto soldier = w.spawn_unit(1, kSoldier, {33, 30});
        hold_ground(w, 0, {archer});
        hold_ground(w, 1, {soldier});
        w.issue(attack(0, archer, soldier));
        for (int t = 0; t < 400; ++t) {
            w.step();
        }
        return w.registry().get<rts::sim::Health>(ent(soldier)).max_hp - hp(w, soldier);
    };
    const std::int32_t open = damage_taken(100);
    const std::int32_t forest = damage_taken(50);
    REQUIRE(open > 0);
    CHECK(forest * 10 <= open * 6);  // como mucho el 60 % (el bonus por clase no se cubre)
    CHECK(forest > 0);
}

TEST_CASE("Terreno: el terreno frena la marcha y a la carreta todavía más fuera de llano") {
    WorldParams p = terrain_params();
    p.unit_types[kSoldier].rough_speed_percent = 50;  // como una carreta
    const auto ticks_to_arrive = [&](std::vector<std::int32_t> speeds, rts::sim::UnitTypeId type) {
        WorldParams q = p;
        q.movement.speed_percent_by_terrain = std::move(speeds);
        World w(q);
        const auto u = w.spawn_unit(0, type, {20, 30});
        w.issue(move_order(w.tick(), {u}, {32, 30}));
        for (int t = 0; t < 5000; ++t) {
            w.step();
            const auto& pos = w.registry().get<rts::sim::Position>(ent(u));
            if (rts::sim::tile_of(pos) == TileCoord{32, 30}) {
                return t;
            }
        }
        return 5000;
    };
    const int flat = ticks_to_arrive({100, 100, 100}, kArcher);
    const int rough = ticks_to_arrive({100, 50, 100}, kArcher);
    const int cart_flat = ticks_to_arrive({100, 100, 100}, kSoldier);
    const int cart_rough = ticks_to_arrive({100, 50, 100}, kSoldier);
    CHECK(rough > flat * 17 / 10);
    CHECK(cart_flat < 5000);
    CHECK(cart_rough > cart_flat * 3);  // 50 % del terreno x 50 % de la carreta
}

TEST_CASE("Terreno: la carga solo vale tras una carrera y en llano firme") {
    const std::int32_t standing = first_blow(terrain_params(), 1);
    const std::int32_t charging = first_blow(terrain_params(), 6);
    REQUIRE(standing > 0);
    CHECK(charging >= standing * 25 / 10);
    WorldParams soft = terrain_params();
    soft.combat.terrain.charge_by_terrain = {0, 0, 0};  // terreno roto: no hay carga
    CHECK(first_blow(soft, 6) == standing);
}

TEST_CASE("Terreno: desactivado, la marcha y el golpe no cambian") {
    WorldParams p = terrain_params();
    p.combat.terrain.enabled = false;
    p.movement.speed_percent_by_terrain.clear();
    CHECK(first_blow(p, 6) == first_blow(p, 1));
}
