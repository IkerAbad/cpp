// Pruebas de las formaciones (B5): solo valen con bastantes en ellas; la columna marcha
// más rápido; la línea da tiro a todos; el cuadro anula la carga y aguanta a la
// caballería, pero las flechas le hacen más; los puestos se reparten según la forma.

#include <cstdint>
#include <cstdlib>
#include <set>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "sim/world.hpp"
#include "test_helpers.hpp"

using rts::sim::Command;
using rts::sim::CommandType;
using rts::sim::FVec2;
using rts::sim::Fixed;
using rts::sim::Formation;
using rts::sim::FormationKind;
using rts::sim::TileCoord;
using rts::sim::World;
using rts::sim::WorldParams;
using namespace rts::test;

namespace {

WorldParams formation_params() {
    WorldParams p = test_world_params(0, 16);
    p.map.bands = {{rts::sim::kElevationRange, 1}};
    p.demo.player = 1;
    p.unit_types[kSoldier].combat.hp = 400;
    p.combat.cavalry_class = kClassInfantry;  // en la prueba, el soldado hace de jinete
    auto& f = p.formation;
    f.enabled = true;
    f.min_members = 4;
    f.cohesion_radius = Fixed::from_int(3);
    f.spacing = Fixed::from_int(1);
    f.cavalry_class = kClassInfantry;
    f.line = {90, 100, 120, 100, 100, false};
    f.column = {115, 80, 80, 120, 100, false};
    f.square = {50, 100, 90, 50, 125, true};
    return p;
}

entt::entity ent(std::uint32_t id) {
    return static_cast<entt::entity>(id);
}

void form(World& w, rts::sim::PlayerId player, std::vector<std::uint32_t> ids, FormationKind kind) {
    Command c;
    c.type = CommandType::SetStance;
    c.player = player;
    c.units = std::move(ids);
    c.kind = static_cast<std::uint8_t>(rts::sim::kFormationBase + static_cast<std::uint8_t>(kind));
    w.issue(c);
}

void hold(World& w, rts::sim::PlayerId player, std::vector<std::uint32_t> ids) {
    Command c;
    c.type = CommandType::SetStance;
    c.player = player;
    c.units = std::move(ids);
    c.kind = static_cast<std::uint8_t>(rts::sim::Stance::HoldGround);
    w.issue(c);
}

// count unidades del jugador en un bloque junto a (x, y).
std::vector<std::uint32_t> block(World& w, rts::sim::PlayerId player, rts::sim::UnitTypeId type, std::int32_t count,
                                 std::int32_t x, std::int32_t y) {
    std::vector<std::uint32_t> ids;
    for (std::int32_t i = 0; i < count; ++i) {
        ids.push_back(w.spawn_unit(player, type, {x + i % 2, y + i / 2}));
    }
    return ids;
}

const Formation& formation(const World& w, std::uint32_t id) {
    return w.registry().get<Formation>(ent(id));
}

void run(World& w, int ticks) {
    for (int t = 0; t < ticks; ++t) {
        w.step();
    }
}

}  // namespace

TEST_CASE("Formaciones: solo valen con bastantes en ellas") {
    World w(formation_params());
    const auto three = block(w, 0, kSoldier, 3, 30, 30);
    const auto four = block(w, 0, kSoldier, 4, 60, 30);
    form(w, 0, three, FormationKind::Square);
    form(w, 0, four, FormationKind::Square);
    w.step();
    CHECK_FALSE(formation(w, three[0]).active);
    CHECK(formation(w, four[0]).active);
    CHECK(formation(w, four[0]).stops_charge);
    CHECK(formation(w, four[0]).speed_percent == 50);
}

TEST_CASE("Formaciones: la columna marcha más rápido y la línea da tiro a los tiradores") {
    World w(formation_params());
    const auto col = block(w, 0, kSoldier, 4, 30, 30);
    const auto archers = block(w, 0, kArcher, 4, 60, 30);
    form(w, 0, col, FormationKind::Column);
    form(w, 0, archers, FormationKind::Line);
    w.step();
    CHECK(formation(w, col[0]).speed_percent == 115);
    CHECK(formation(w, col[0]).attack_percent == 80);
    CHECK(formation(w, archers[0]).attack_percent == 120);

    const auto advance = [](bool in_column) {
        World world(formation_params());
        const auto ids = block(world, 0, kSoldier, 4, 30, 30);
        if (in_column) {
            form(world, 0, ids, FormationKind::Column);
        }
        world.step();
        world.issue(move_order(world.tick(), ids, {80, 30}));
        run(world, 150);
        return world.registry().get<rts::sim::Position>(ent(ids[0])).x;
    };
    CHECK(advance(true) > advance(false));
}

TEST_CASE("Formaciones: el cuadro aguanta a la caballería y anula su carga, pero las flechas le hacen más") {
    // Daño que recibe un soldado del cuadro (o suelto) de un atacante del tipo dado.
    const auto taken = [](bool square, rts::sim::UnitTypeId attacker, std::int32_t gap, std::int32_t charge) {
        WorldParams p = formation_params();
        p.unit_types[kSoldier].charge_percent = charge;
        if (charge > 100) {
            p.combat.terrain.enabled = true;
            p.combat.terrain.charge_run_ticks = 20;
            p.combat.terrain.arrow_cover_percent_by_terrain = {100, 100, 100};
            p.combat.terrain.charge_by_terrain = {0, 1, 1};
            p.formation.square.cavalry_taken_percent = 100;  // solo la carga
        }
        World w(p);
        const auto ids = block(w, 1, kSoldier, 4, 40, 40);
        hold(w, 1, ids);
        if (square) {
            form(w, 1, ids, FormationKind::Square);
        }
        const auto a = w.spawn_unit(0, attacker, {40 - gap, 40});
        hold(w, 0, {a});
        Command atk;
        atk.type = CommandType::Attack;
        atk.player = 0;
        atk.units = {a};
        atk.object = ids[0];
        w.issue(atk);
        const auto& h = w.registry().get<rts::sim::Health>(ent(ids[0]));
        for (int t = 0; t < 600 && h.hp == h.max_hp; ++t) {
            w.step();
        }
        return h.max_hp - h.hp;
    };
    // Caballería (cuerpo a cuerpo): la mitad contra el cuadro.
    const auto loose = taken(false, kSoldier, 1, 100);
    const auto braced = taken(true, kSoldier, 1, 100);
    REQUIRE(loose > 1);
    CHECK(braced * 2 <= loose + 1);
    // Flechas: más contra el cuadro.
    CHECK(taken(true, kArcher, 3, 100) > taken(false, kArcher, 3, 100));
    // Carga: suelto, la recibe; en cuadro, no.
    const auto charged = taken(false, kSoldier, 6, 300);
    const auto stopped = taken(true, kSoldier, 6, 300);
    CHECK(charged >= loose * 25 / 10);
    CHECK(stopped == loose);
}

TEST_CASE("Formaciones: los puestos se reparten según la forma") {
    World w(formation_params());
    const FVec2 east{Fixed::from_int(1), Fixed{}};
    const auto line = w.formation().slots(FormationKind::Line, 4, {50, 50}, east);
    const auto column = w.formation().slots(FormationKind::Column, 4, {50, 50}, east);
    // Línea mirando al este: de lado a lado (misma x, distintas y).
    std::set<std::int32_t> ys;
    for (const TileCoord t : line) {
        CHECK(t.x == 50);
        ys.insert(t.y);
    }
    CHECK(ys.size() == 4);
    // Columna: de dos en fondo, hacia atrás (al oeste).
    std::set<std::int32_t> xs;
    for (const TileCoord t : column) {
        xs.insert(t.x);
    }
    CHECK(xs.size() == 2);

    // Al moverse en formación, cada una a su puesto.
    const auto ids = block(w, 0, kSoldier, 4, 30, 50);
    form(w, 0, ids, FormationKind::Line);
    w.step();
    w.issue(move_order(w.tick(), ids, {50, 50}));
    run(w, 800);
    std::set<std::pair<std::int32_t, std::int32_t>> ends;
    for (const auto id : ids) {
        const TileCoord t = rts::sim::tile_of(w.registry().get<rts::sim::Position>(ent(id)));
        ends.insert({t.x, t.y});
        CHECK(std::abs(t.x - 50) <= 1);
    }
    CHECK(ends.size() == 4);
}
