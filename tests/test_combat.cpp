// Pruebas del combate (M4): fórmula de daño, muerte simultánea, proyectiles que se
// esquivan, niveles y héroes con aura, posturas, ataque-movimiento, destrucción de
// edificios con actualización del pathfinding y determinismo.

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <vector>

#include <doctest/doctest.h>

#include "sim/combat.hpp"
#include "sim/path/grid.hpp"
#include "sim/path/hpa.hpp"
#include "sim/world.hpp"
#include "test_helpers.hpp"

using rts::sim::Combatant;
using rts::sim::Command;
using rts::sim::CommandType;
using rts::sim::GridSearch;
using rts::sim::Health;
using rts::sim::HpaGraph;
using rts::sim::Position;
using rts::sim::TileCoord;
using rts::sim::World;
using rts::sim::WorldParams;
using rts::test::kArcher;
using rts::test::kCenter;
using rts::test::kSoldier;
using rts::test::kVillager;

namespace {

WorldParams open_field() {
    WorldParams p = rts::test::test_world_params(0, 16);
    p.map.bands = {{rts::sim::kElevationRange, 1}};
    p.demo.player = 1;  // dos jugadores (sin unidades de demostración: count = 0)
    return p;
}

entt::entity ent(std::uint32_t id) {
    return static_cast<entt::entity>(id);
}

Command order(CommandType type, rts::sim::PlayerId player, std::vector<std::uint32_t> units,
              std::uint32_t object = rts::sim::kNoObject) {
    Command c;
    c.type = type;
    c.player = player;
    c.units = std::move(units);
    c.object = object;
    return c;
}

bool alive(const World& w, std::uint32_t id) {
    return w.registry().valid(ent(id));
}

std::int32_t hp(const World& w, std::uint32_t id) {
    return w.registry().get<Health>(ent(id)).hp;
}

}  // namespace

TEST_CASE("Combate: la fórmula de daño resta armadura por tipo, suma bonus y nunca baja de 1") {
    World world(open_field());
    const auto soldier = world.spawn_unit(0, kSoldier, {100, 100});
    const auto archer = world.spawn_unit(0, kArcher, {100, 104});
    const auto villager = world.spawn_unit(1, kVillager, {110, 100});
    const auto enemy_soldier = world.spawn_unit(1, kSoldier, {110, 104});
    const auto house = world.spawn_building(1, rts::test::kHouse, {120, 120}, true);
    REQUIRE(house);
    (void)soldier;
    (void)archer;
    const auto& reg = world.registry();
    const auto& cb = world.combat();
    // Soldado 5 cuerpo contra soldado con armadura 1: 4.
    CHECK(cb.damage(kSoldier, 100, reg, ent(enemy_soldier)) == 4);
    // Contra un aldeano sin armadura: 5.
    CHECK(cb.damage(kSoldier, 100, reg, ent(villager)) == 5);
    // Arquero 4 proyectil + 1 de bonus contra infantería.
    CHECK(cb.damage(kArcher, 100, reg, ent(enemy_soldier)) == 5);
    // Contra la casa (armadura 3/8): 5-3 = 2 cuerpo; 4-8 < 0 -> mínimo 1.
    CHECK(cb.damage(kSoldier, 100, reg, ent(*house)) == 2);
    CHECK(cb.damage(kArcher, 100, reg, ent(*house)) == 1);
    CHECK(cb.damage(kVillager, 100, reg, ent(*house)) == 1);
    // Porcentaje de ataque (nivel/aura): 5 * 150 % = 7 -> 7 - 1 = 6.
    CHECK(cb.damage(kSoldier, 150, reg, ent(enemy_soldier)) == 6);
}

TEST_CASE("Combate: dos soldados iguales que se atacan mueren en el mismo tick") {
    World world(open_field());
    // A 0,55 casillas entre centros: bordes a 0,05 < alcance 0,15.
    const auto a = world.spawn_unit(0, kSoldier, {100, 100});
    const auto b = world.spawn_unit(1, kSoldier, {100, 100});
    // Mismo centro de casilla: se separan en x; basta con que estén al alcance.
    world.issue(order(CommandType::Attack, 0, {a}, b));
    world.issue(order(CommandType::Attack, 1, {b}, a));
    // Golpean en el mismo tick y suben de nivel a la vez: la vida es idéntica en cada
    // tick y los dos mueren en el mismo, sin que importe cuál se procesa antes.
    std::int32_t ticks = 0;
    while (alive(world, a) && alive(world, b) && ticks < 2000) {
        REQUIRE(hp(world, a) == hp(world, b));
        world.step();
        ++ticks;
    }
    MESSAGE("duelo resuelto en " << ticks << " ticks");
    CHECK_FALSE(alive(world, a));
    CHECK_FALSE(alive(world, b));
    CHECK(world.combat().last_stats().kills == 2);
}

TEST_CASE("Combate: un proyectil acierta a un blanco quieto y se esquiva moviéndose") {
    auto shoot = [](bool target_moves) {
        World world(open_field());
        const auto archer = world.spawn_unit(0, kArcher, {100, 100});
        const auto target = world.spawn_unit(1, kSoldier, {100, 104});
        world.issue(order(CommandType::Attack, 0, {archer}, target));
        if (target_moves) {
            // Cruza en perpendicular a la línea de tiro a 1,2 casillas/s.
            Command run = order(CommandType::Move, 1, {target});
            run.target = {130, 104};
            world.issue(run);
        }
        std::int32_t hit = 0;
        std::int32_t missed = 0;
        std::int32_t fired = 0;
        // Vuelo de ~4 casillas a 0,35 por tick: cae en el tick 12; la recarga es de 20.
        for (int t = 0; t < 15; ++t) {
            world.step();
            fired += world.combat().last_stats().projectiles_fired;
            hit += world.combat().last_stats().projectiles_hit;
            missed += world.combat().last_stats().projectiles_missed;
        }
        return std::array<std::int32_t, 3>{fired, hit, missed};
    };
    const auto still = shoot(false);
    CHECK(still[0] == 1);
    CHECK(still[1] == 1);
    CHECK(still[2] == 0);
    const auto moving = shoot(true);
    CHECK(moving[0] == 1);
    CHECK(moving[1] == 0);
    CHECK(moving[2] == 1);
}

TEST_CASE("Combate: la experiencia sube de nivel, da vida y en el último nivel hace un héroe con aura") {
    World world(open_field());
    const auto soldier = world.spawn_unit(0, kSoldier, {100, 100});
    const auto victim = world.spawn_unit(1, kVillager, {100, 100});
    world.issue(order(CommandType::Attack, 0, {soldier}, victim));
    // Aldeano: 25 de vida. Golpes de 5 (nivel 0 y 1: 5 * 110 % = 5) y, desde el nivel 2
    // (20 de experiencia), de 6: 5 + 5 + 5 + 5 + 6 = 26 de daño en los ticks 0..80, + 20 por la baja.
    for (int t = 0; t < 81 && alive(world, victim); ++t) {
        world.step();
    }
    REQUIRE_FALSE(alive(world, victim));
    const Combatant& c = world.registry().get<Combatant>(ent(soldier));
    CHECK(c.xp == 46);
    CHECK(c.level == 3);
    CHECK(c.hero_name == 0);
    // +10 % de 40 por nivel.
    CHECK(world.registry().get<Health>(ent(soldier)).max_hp == 52);
    CHECK(world.combat().attack_percent(c, false) == 130);
    CHECK(world.combat().attack_percent(c, true) == 150);

    // Aura: un aliado de nivel 0 junto al héroe pega un 20 % más (5 -> 6); lejos, no.
    // Cada aliado comparte casilla con su blanco: al alcance desde el primer tick.
    const auto near_ally = world.spawn_unit(0, kSoldier, {101, 100});
    const auto far_ally = world.spawn_unit(0, kSoldier, {140, 100});
    const auto dummy_near = world.spawn_unit(1, kVillager, {101, 100});
    const auto dummy_far = world.spawn_unit(1, kVillager, {140, 100});
    // El héroe se queda quieto y sin perseguir, para no tocar a los blancos de prueba.
    Command hold = order(CommandType::SetStance, 0, {soldier});
    hold.kind = static_cast<std::uint8_t>(rts::sim::Stance::HoldGround);
    world.issue(hold);
    world.issue(order(CommandType::Attack, 0, {near_ally}, dummy_near));
    world.issue(order(CommandType::Attack, 0, {far_ally}, dummy_far));
    world.step();
    CHECK(hp(world, dummy_near) == 25 - 6);
    CHECK(hp(world, dummy_far) == 25 - 5);
}

TEST_CASE("Combate: con mantener posición no persigue; agresiva sí") {
    auto chases = [](rts::sim::Stance stance) {
        World world(open_field());
        const auto guard = world.spawn_unit(0, kSoldier, {100, 100});
        world.spawn_unit(1, kVillager, {104, 100});  // a 4 casillas: dentro de la vista (6)
        Command st = order(CommandType::SetStance, 0, {guard});
        st.kind = static_cast<std::uint8_t>(stance);
        world.issue(st);
        for (int t = 0; t < 60; ++t) {
            world.step();
        }
        const Position& p = world.registry().get<Position>(ent(guard));
        return p.x.floor_to_int() != 100;
    };
    CHECK(chases(rts::sim::Stance::Aggressive));
    CHECK_FALSE(chases(rts::sim::Stance::HoldGround));
}

TEST_CASE("Combate: ataque-movimiento pelea por el camino y sigue hasta el destino") {
    World world(open_field());
    std::vector<std::uint32_t> army;
    for (int i = 0; i < 6; ++i) {
        army.push_back(world.spawn_unit(0, kSoldier, {60 + i % 3, 100 + i / 3}));
    }
    std::vector<std::uint32_t> enemies;
    for (int i = 0; i < 3; ++i) {
        enemies.push_back(world.spawn_unit(1, kVillager, {80, 99 + i}));
    }
    Command am = order(CommandType::AttackMove, 0, army);
    am.target = {100, 100};
    world.issue(am);
    for (int t = 0; t < 2400; ++t) {
        world.step();
    }
    for (const auto e : enemies) {
        CHECK_FALSE(alive(world, e));
    }
    std::int32_t near_dest = 0;
    for (const auto e : army) {
        REQUIRE(alive(world, e));
        const Position& p = world.registry().get<Position>(ent(e));
        near_dest += (p.x.floor_to_int() >= 95) ? 1 : 0;
    }
    MESSAGE("unidades cerca del destino: " << near_dest << " de 6");
    CHECK(near_dest == 6);
}

TEST_CASE("Combate: un edificio destruido libera sus casillas y el grafo coincide con uno nuevo") {
    WorldParams p = open_field();
    World world(p);
    const auto house = world.spawn_building(1, rts::test::kHouse, {110, 110}, true);
    REQUIRE(house);
    std::vector<std::uint32_t> army;
    for (int i = 0; i < 8; ++i) {
        army.push_back(world.spawn_unit(0, kSoldier, {105 + i % 4, 106 + i / 4}));
    }
    world.issue(order(CommandType::Attack, 0, army, *house));
    world.step();
    REQUIRE_FALSE(world.movement().grid().passable({110, 110}));
    // 500 de vida, 2 por golpe cada 20 ticks y 8 soldados (los que llegan): < 1000 ticks.
    for (int t = 0; t < 1500 && alive(world, *house); ++t) {
        world.step();
    }
    CHECK_FALSE(alive(world, *house));
    world.step();
    CHECK(world.movement().grid().passable({110, 110}));
    CHECK(world.object_at({111, 111}) == std::nullopt);
    GridSearch search(256, 256);
    CHECK(world.movement().hpa().same_graph(HpaGraph(world.movement().grid(), p.movement.hpa, search)));
}

TEST_CASE("Combate: dos batallas iguales coinciden tick a tick") {
    auto make = []() {
        auto w = std::make_unique<World>(open_field());
        std::vector<std::uint32_t> a;
        std::vector<std::uint32_t> b;
        for (int i = 0; i < 30; ++i) {
            a.push_back(w->spawn_unit(0, i % 3 == 0 ? kArcher : kSoldier, {90 + i % 6, 100 + i / 6}));
            b.push_back(w->spawn_unit(1, i % 3 == 0 ? kArcher : kSoldier, {110 + i % 6, 100 + i / 6}));
        }
        Command ca = order(CommandType::AttackMove, 0, a);
        ca.target = {115, 102};
        Command cb = order(CommandType::AttackMove, 1, b);
        cb.target = {90, 102};
        w->issue(ca);
        w->issue(cb);
        return w;
    };
    auto w1 = make();
    auto w2 = make();
    std::int32_t kills = 0;
    for (int t = 0; t < 600; ++t) {
        w1->step();
        w2->step();
        kills += w1->combat().last_stats().kills;
        REQUIRE(w1->state_hash() == w2->state_hash());
    }
    MESSAGE("bajas en 600 ticks: " << kills);
    CHECK(kills > 10);
}

TEST_CASE("Regresión: hash de una batalla de 30 contra 30 tras 600 ticks") {
    World world(open_field());
    std::vector<std::uint32_t> a;
    std::vector<std::uint32_t> b;
    for (int i = 0; i < 30; ++i) {
        a.push_back(world.spawn_unit(0, i % 3 == 0 ? kArcher : kSoldier, {90 + i % 6, 100 + i / 6}));
        b.push_back(world.spawn_unit(1, i % 3 == 0 ? kArcher : kSoldier, {110 + i % 6, 100 + i / 6}));
    }
    Command ca = order(CommandType::AttackMove, 0, a);
    ca.target = {115, 102};
    Command cb = order(CommandType::AttackMove, 1, b);
    cb.target = {90, 102};
    world.issue(ca);
    world.issue(cb);
    for (int t = 0; t < 600; ++t) {
        world.step();
    }
    constexpr std::uint64_t kExpectedHash = 0x6f44b192af50f861ULL;
    INFO(std::format("hash obtenido: 0x{:016x}", world.state_hash()));
    CHECK(world.state_hash() == kExpectedHash);
}

TEST_CASE("Combate: al alcance del blanco la unidad se detiene, aunque viniera en ataque-movimiento") {
    World world(open_field());
    const auto house = world.spawn_building(1, rts::test::kHouse, {102, 99}, true);
    REQUIRE(house.has_value());
    const auto soldier = world.spawn_unit(0, kSoldier, {100, 100});
    Command c = order(CommandType::AttackMove, 0, {soldier});
    c.target = {140, 100};
    world.issue(c);
    std::int32_t max_x = 0;
    for (int t = 0; t < 400; ++t) {
        world.step();
        max_x = std::max(max_x, world.registry().get<Position>(ent(soldier)).x.floor_to_int());
    }
    // Se queda junto a la casa golpeándola en vez de seguir andando hacia su destino.
    CHECK(max_x <= 104);
    // 5 - 3 de armadura = 2 por golpe, uno cada 20 ticks: unos 20 golpes en 400 ticks.
    CHECK(world.registry().get<Health>(ent(*house)).hp <= 500 - 2 * 17);
}

TEST_CASE("Combate: un ataque-movimiento sin camino a su destino termina en vez de quedar pendiente") {
    World world(open_field());
    const auto soldier = world.spawn_unit(0, kSoldier, {100, 100});
    // Un bolsillo de 2x2 casillas cerrado por casas: nada fuera es alcanzable.
    for (const TileCoord o : {TileCoord{98, 98}, TileCoord{100, 98}, TileCoord{102, 98}, TileCoord{98, 100},
                              TileCoord{102, 100}, TileCoord{98, 102}, TileCoord{100, 102}, TileCoord{102, 102}}) {
        REQUIRE(world.spawn_building(1, rts::test::kHouse, o, true).has_value());
    }
    world.step();
    Command c = order(CommandType::AttackMove, 0, {soldier});
    c.target = {200, 200};
    world.issue(c);
    world.step();
    world.step();
    CHECK_FALSE(world.registry().get<Combatant>(ent(soldier)).attack_move);
}

TEST_CASE("Combate: el daño de un golpe no desborda con ataques y niveles extremos") {
    rts::sim::CombatStats a;
    a.attack_melee = 1'000'000;
    a.attack_pierce = 1'000'000;
    // 64 niveles a +1000 % y aura: más de 650 veces el ataque base.
    CHECK(rts::sim::hit_damage(a, 65'100, 0, 0, 0) == 1'000'000);
    CHECK(rts::sim::hit_damage(a, 100, 0, 0, 0) == 1'000'000);
    rts::sim::CombatStats weak;
    weak.attack_melee = 1;
    CHECK(rts::sim::hit_damage(weak, 100, 50, 0, 0) == 1);  // el mínimo se mantiene
}
