// Pruebas de los módulos de la IA que piensan mejor (perfil "normal"), cada uno por
// separado sobre un mapa llano: se llama a AiSystem::think y se miran sus órdenes.

#include <algorithm>
#include <cstdint>
#include <vector>

#include <doctest/doctest.h>

#include "sim/ai.hpp"
#include "sim/world.hpp"
#include "test_helpers.hpp"

using rts::sim::AiBehavior;
using rts::sim::AiParams;
using rts::sim::AiSystem;
using rts::sim::Command;
using rts::sim::CommandType;
using rts::sim::World;
using rts::sim::WorldParams;
using namespace rts::test;

namespace {

constexpr rts::sim::PlayerId kAi = 1;
constexpr rts::sim::PlayerId kRival = 0;
// El jugador 1 decide en los ticks con tick % 20 == 1.
constexpr rts::sim::Tick kThinkTick = 1;

WorldParams flat_two_players() {
    WorldParams p = test_world_params(0, 16);
    p.map.bands = {{rts::sim::kElevationRange, 1}};
    p.demo.player = 1;  // dos jugadores
    return p;
}

// Órdenes de un solo módulo del perfil "normal" (índice 1 de test_ai_params).
std::vector<Command> think_with(const World& world, std::vector<AiBehavior> behaviors) {
    AiParams params = test_ai_params();
    params.profiles[1].behaviors = std::move(behaviors);
    AiSystem ai(params, {{kAi, 1}});
    std::vector<Command> out;
    ai.think(world.registry(), world.economy(), world.movement().grid(), kThinkTick, out);
    return out;
}

// Base de la IA: centro urbano (da plazas) y cuartel terminados, y dinero de sobra.
void ai_base(World& world) {
    world.spawn_building(kAi, kCenter, {150, 150}, true);
    world.spawn_building(kAi, kBarracks, {160, 150}, true);
    world.set_stock(kAi, stock(1000, 1000, 1000, 1000));
}

}  // namespace

TEST_CASE("IA normal: el ejército se elige contra lo que tiene el rival") {
    WorldParams p = flat_two_players();
    // Arqueros muy eficaces contra infantería: contra soldados conviene el arquero.
    p.unit_types[kArcher].combat.bonus[kClassInfantry] = 20;

    SUBCASE("contra un ejército de soldados, arqueros") {
        World world(p);
        ai_base(world);
        for (std::int32_t i = 0; i < 8; ++i) {
            world.spawn_unit(kRival, kSoldier, {60 + i, 60});
        }
        world.step();
        const auto out = think_with(world, {AiBehavior::ArmyCounter});
        REQUIRE(out.size() == 1);
        CHECK(out[0].type == CommandType::Train);
        CHECK(out[0].kind == kArcher);
    }
    SUBCASE("sin unidades armadas enemigas, lo que antes derriba edificios") {
        World world(p);
        ai_base(world);
        world.spawn_building(kRival, kHouse, {60, 60}, true);
        world.spawn_unit(kRival, kVillager, {70, 60});  // desarmado: no cuenta como ejército
        world.step();
        // Soldado contra casa: 5 - 3 = 2 por golpe; arquero: 4 - 8 < 0, el mínimo, 1.
        const auto out = think_with(world, {AiBehavior::ArmyCounter});
        REQUIRE(out.size() == 1);
        CHECK(out[0].kind == kSoldier);
    }
}

TEST_CASE("IA normal: fuego concentrado en el enemigo armado que antes cae") {
    World world(flat_two_players());
    ai_base(world);
    const auto a = world.spawn_unit(kAi, kSoldier, {100, 100});
    const auto b = world.spawn_unit(kAi, kSoldier, {100, 101});
    // El soldado enemigo está más cerca, pero el arquero cae en 6 golpes (30 / 5) y el
    // soldado en 10 (40 / (5 - 1)).
    const auto soldier = world.spawn_unit(kRival, kSoldier, {102, 100});
    const auto archer = world.spawn_unit(kRival, kArcher, {104, 101});
    const auto villager = world.spawn_unit(kRival, kVillager, {101, 102});  // desarmado: no es prioridad
    Command c;
    c.player = kAi;
    c.type = CommandType::AttackMove;
    c.units = {a, b};
    c.target = {110, 100};
    world.issue(c);
    world.step();

    const auto out = think_with(world, {AiBehavior::FocusFire});
    REQUIRE_FALSE(out.empty());
    for (const Command& o : out) {
        CHECK(o.type == CommandType::Attack);
        CHECK(o.object == archer);
        CHECK(o.object != soldier);
        CHECK(o.object != villager);
    }
    std::vector<std::uint32_t> attackers;
    for (const Command& o : out) {
        attackers.insert(attackers.end(), o.units.begin(), o.units.end());
    }
    std::ranges::sort(attackers);
    CHECK(attackers == std::vector<std::uint32_t>{std::min(a, b), std::max(a, b)});
}

TEST_CASE("IA normal: ataca cuando su fuerza supera a la enemiga, y no antes") {
    SUBCASE("ocho soldados contra dos: ataca el edificio enemigo") {
        World world(flat_two_players());
        ai_base(world);
        for (std::int32_t i = 0; i < 8; ++i) {
            world.spawn_unit(kAi, kSoldier, {145, 140 + i});
        }
        world.spawn_unit(kRival, kSoldier, {60, 60});
        world.spawn_unit(kRival, kSoldier, {61, 60});
        world.spawn_building(kRival, kHouse, {50, 50}, true);
        world.step();
        const auto out = think_with(world, {AiBehavior::AttackStrength});
        REQUIRE(out.size() == 1);
        CHECK(out[0].type == CommandType::AttackMove);
        CHECK(out[0].units.size() == 8);
        CHECK(out[0].target == rts::sim::TileCoord{51, 51});  // centro de la casa
    }
    SUBCASE("ocho soldados contra veinte: espera") {
        World world(flat_two_players());
        ai_base(world);
        for (std::int32_t i = 0; i < 8; ++i) {
            world.spawn_unit(kAi, kSoldier, {145, 140 + i});
        }
        for (std::int32_t i = 0; i < 20; ++i) {
            world.spawn_unit(kRival, kSoldier, {60 + i % 10, 60 + i / 10});
        }
        world.step();
        CHECK(think_with(world, {AiBehavior::AttackStrength}).empty());
    }
}

TEST_CASE("IA normal: se retira de una batalla perdida") {
    World world(flat_two_players());
    ai_base(world);
    const auto a = world.spawn_unit(kAi, kSoldier, {60, 64});
    const auto b = world.spawn_unit(kAi, kSoldier, {61, 64});
    for (std::int32_t i = 0; i < 10; ++i) {
        world.spawn_unit(kRival, kSoldier, {56 + i, 58});
    }
    Command c;
    c.player = kAi;
    c.type = CommandType::AttackMove;
    c.units = {a, b};
    c.target = {60, 50};
    world.issue(c);
    world.step();

    const auto out = think_with(world, {AiBehavior::AttackStrength});
    REQUIRE(out.size() == 1);
    CHECK(out[0].type == CommandType::Move);
    CHECK(out[0].target == rts::sim::TileCoord{151, 151});  // centro del centro urbano
    std::vector<std::uint32_t> units = out[0].units;
    std::ranges::sort(units);
    CHECK(units == std::vector<std::uint32_t>{std::min(a, b), std::max(a, b)});
}
