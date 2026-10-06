// Pruebas de la herrería (C1): investigar cuesta y tarda; exige la mejora anterior; no
// se repite ni se investiga dos veces a la vez; anularla devuelve el coste; lo
// investigado vale para las unidades que ya había y para las nuevas, en ataque y en
// armadura, y solo para las clases que nombra.

#include <cstdint>
#include <vector>

#include <doctest/doctest.h>

#include "sim/world.hpp"
#include "test_helpers.hpp"

using rts::sim::Command;
using rts::sim::CommandType;
using rts::sim::UpgradeType;
using rts::sim::World;
using rts::sim::WorldParams;
using namespace rts::test;

namespace {

constexpr rts::sim::UpgradeId kSharp = 0;   // +1 cuerpo a cuerpo a la infantería
constexpr rts::sim::UpgradeId kSteel = 1;   // +1 más, exige la anterior
constexpr rts::sim::UpgradeId kArmor = 2;   // +1/+1 de armadura a la infantería
constexpr std::int32_t kTicks = 50;

WorldParams forge_params() {
    WorldParams p = test_world_params(0, 16);
    p.map.bands = {{rts::sim::kElevationRange, 1}};
    p.demo.player = 1;
    UpgradeType u;
    u.at = kBarracks;  // en la prueba, el cuartel hace de herrería
    u.cost = stock(0, 0, 0, 50);
    u.research_ticks = kTicks;
    u.classes = 1U << kClassInfantry;
    u.attack_melee = 1;
    p.upgrades.push_back(u);
    u.requires_upgrade = kSharp;
    p.upgrades.push_back(u);
    u.requires_upgrade.reset();
    u.attack_melee = 0;
    u.armor_melee = 1;
    u.armor_pierce = 1;
    p.upgrades.push_back(u);
    return p;
}

Command research(std::uint32_t forge, rts::sim::UpgradeId u, rts::sim::PlayerId player = 0) {
    Command c;
    c.type = CommandType::Research;
    c.player = player;
    c.object = forge;
    c.kind = u;
    return c;
}

std::int32_t gold(const World& w, rts::sim::PlayerId p = 0) {
    return w.player_state(p).stock[rts::sim::resource_index(rts::sim::Resource::Gold)];
}

void run(World& w, int ticks) {
    for (int t = 0; t < ticks; ++t) {
        w.step();
    }
}

}  // namespace

TEST_CASE("Herrería: investigar cuesta, tarda y exige la mejora anterior") {
    World w(forge_params());
    const auto forge = *w.spawn_building(0, kBarracks, {40, 40}, true);
    w.set_stock(0, stock(0, 0, 0, 200));
    w.issue(research(forge, kSteel));  // sin la anterior: no
    w.step();
    CHECK(gold(w) == 200);
    w.issue(research(forge, kSharp));
    w.step();
    CHECK(gold(w) == 150);
    w.issue(research(forge, kArmor));  // ocupada: una a la vez
    w.step();
    CHECK(gold(w) == 150);
    run(w, kTicks);
    CHECK(w.economy().researched(0, kSharp));
    w.issue(research(forge, kSharp));  // ya hecha
    w.step();
    CHECK(gold(w) == 150);
    w.issue(research(forge, kSteel));  // ahora sí
    w.step();
    CHECK(gold(w) == 100);
}

TEST_CASE("Herrería: anular la investigación devuelve el coste") {
    World w(forge_params());
    const auto forge = *w.spawn_building(0, kBarracks, {40, 40}, true);
    w.set_stock(0, stock(0, 0, 0, 100));
    w.issue(research(forge, kArmor));
    run(w, 10);
    CHECK(gold(w) == 50);
    Command cancel;
    cancel.type = CommandType::CancelTrain;
    cancel.player = 0;
    cancel.object = forge;
    w.issue(cancel);
    run(w, kTicks + 5);
    CHECK(gold(w) == 100);
    CHECK_FALSE(w.economy().researched(0, kArmor));
}

TEST_CASE("Herrería: lo investigado vale para las unidades que ya había y para las nuevas") {
    World w(forge_params());
    const auto forge = *w.spawn_building(0, kBarracks, {40, 40}, true);
    w.set_stock(0, stock(0, 0, 0, 200));
    const auto old_soldier = w.spawn_unit(0, kSoldier, {30, 30});
    const auto enemy = w.spawn_unit(1, kSoldier, {60, 60});
    w.step();
    const auto& cb = w.combat();
    const auto e_enemy = static_cast<entt::entity>(enemy);
    const auto e_old = static_cast<entt::entity>(old_soldier);
    const std::int32_t before = cb.damage(kSoldier, 0, 100, w.registry(), e_enemy);
    const std::int32_t taken_before = cb.damage(kSoldier, 1, 100, w.registry(), e_old);
    const std::int32_t archer_before = cb.damage(kArcher, 0, 100, w.registry(), e_enemy);
    w.issue(research(forge, kSharp));
    run(w, kTicks + 2);
    w.issue(research(forge, kArmor));
    run(w, kTicks + 2);
    REQUIRE(w.economy().researched(0, kSharp));
    REQUIRE(w.economy().researched(0, kArmor));
    // Ataque: +1 al soldado (tipo y dueño: el que ya había y el que venga).
    CHECK(cb.damage(kSoldier, 0, 100, w.registry(), e_enemy) == before + 1);
    // El arquero (otra clase) no gana nada.
    CHECK(cb.damage(kArcher, 0, 100, w.registry(), e_enemy) == archer_before);
    // Armadura: el soldado que ya había recibe 1 menos; y uno nuevo, también.
    CHECK(cb.damage(kSoldier, 1, 100, w.registry(), e_old) == taken_before - 1);
    const auto fresh = w.spawn_unit(0, kSoldier, {32, 30});
    CHECK(cb.damage(kSoldier, 1, 100, w.registry(), static_cast<entt::entity>(fresh)) == taken_before - 1);
    // Las del rival no cambian.
    CHECK(cb.damage(kSoldier, 1, 100, w.registry(), e_enemy) == before);
}
