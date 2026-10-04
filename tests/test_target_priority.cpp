// Prioridad de blancos: una unidad que elige blanco sola atiende primero a quien la
// ataca, luego a lo armado, el asedio, el bagaje y los aldeanos; dentro de cada clase,
// al más cercano. La misma regla para el jugador y para la IA.

#include <cstdint>
#include <vector>

#include <doctest/doctest.h>

#include "sim/world.hpp"
#include "test_helpers.hpp"

using rts::sim::Combatant;
using rts::sim::TargetClass;
using rts::sim::World;
using rts::sim::WorldParams;
using namespace rts::test;

namespace {

WorldParams priority_world() {
    WorldParams p = test_world_params(0, 16);
    p.map.bands = {{rts::sim::kElevationRange, 1}};
    p.demo.player = 1;
    p.combat.target_priority = {TargetClass::AttackingMe, TargetClass::Armed, TargetClass::Siege,
                                TargetClass::Carrier, TargetClass::Worker, TargetClass::Other};
    return p;
}

entt::entity target_of(const World& w, std::uint32_t id) {
    return w.registry().get<Combatant>(static_cast<entt::entity>(id)).target;
}

void run(World& w, std::int32_t ticks) {
    for (std::int32_t t = 0; t < ticks; ++t) {
        w.step();
    }
}

}  // namespace

TEST_CASE("Prioridad: lo armado antes que el aldeano más cercano") {
    World world(priority_world());
    const auto me = world.spawn_unit(0, kSoldier, {100, 100});
    world.spawn_unit(1, kVillager, {101, 100});              // al lado
    const auto soldier = world.spawn_unit(1, kSoldier, {104, 100});  // más lejos, armado
    // Lo armado va antes; si además el soldado enemigo me elige, con más razón.
    run(world, 12);
    CHECK(target_of(world, me) == static_cast<entt::entity>(soldier));
}

TEST_CASE("Prioridad: sin lista, el más cercano (comportamiento anterior)") {
    WorldParams p = priority_world();
    p.combat.target_priority.clear();
    World world(p);
    const auto me = world.spawn_unit(0, kSoldier, {100, 100});
    const auto villager = world.spawn_unit(1, kVillager, {101, 100});
    world.spawn_unit(1, kSoldier, {104, 100});
    run(world, 12);
    CHECK(target_of(world, me) == static_cast<entt::entity>(villager));
}

TEST_CASE("Prioridad: quien me ataca antes que otro armado más cercano") {
    WorldParams p = priority_world();
    // Arquero enemigo de alcance largo que me tiene por blanco; un soldado enemigo más
    // cerca que no me ataca (mantiene posición y su vista no me alcanza).
    World world(p);
    const auto me = world.spawn_unit(0, kSoldier, {100, 100});
    const auto near = world.spawn_unit(1, kSoldier, {103, 100});
    const auto archer = world.spawn_unit(1, kArcher, {105, 101});
    rts::sim::Command hold;
    hold.type = rts::sim::CommandType::SetStance;
    hold.player = 1;
    hold.kind = static_cast<std::uint8_t>(rts::sim::Stance::HoldGround);
    hold.units = {near, archer};
    world.issue(hold);
    rts::sim::Command shoot;
    shoot.type = rts::sim::CommandType::Attack;
    shoot.player = 1;
    shoot.units = {archer};
    shoot.object = me;
    world.issue(shoot);
    // Mi soldado, quieto, sin buscar aún (su primera búsqueda llega en unos ticks).
    run(world, 12);
    REQUIRE(target_of(world, archer) == static_cast<entt::entity>(me));
    CHECK(target_of(world, me) == static_cast<entt::entity>(archer));
}
