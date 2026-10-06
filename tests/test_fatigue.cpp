// Pruebas de la fatiga (B3): marchar cansa y parar repone; cansada, la unidad anda y
// pega peor; el paso forzado va más deprisa y cansa más; el terreno difícil cansa
// más; el aguante del tipo escala el cansancio; desactivada, nada cambia.

#include <cstdint>
#include <vector>

#include <doctest/doctest.h>

#include "sim/world.hpp"
#include "test_helpers.hpp"

using rts::sim::Command;
using rts::sim::CommandType;
using rts::sim::Fatigue;
using rts::sim::Fixed;
using rts::sim::TileCoord;
using rts::sim::World;
using rts::sim::WorldParams;
using namespace rts::test;

namespace {

WorldParams fatigue_params() {
    WorldParams p = test_world_params(0, 16);
    p.map.bands = {{rts::sim::kElevationRange, 1}};
    p.demo.player = 1;
    p.unit_types[kSoldier].stamina = 100;
    p.unit_types[kArcher].stamina = 100;
    auto& f = p.fatigue;
    f.enabled = true;
    f.march_per_tick = 100;
    f.strike = 400;
    f.rest_per_tick = 200;
    f.forced_speed_percent = 120;
    f.forced_fatigue_percent = 300;
    f.exhausted_speed_percent = 70;
    f.exhausted_attack_percent = 70;
    return p;
}

entt::entity ent(std::uint32_t id) {
    return static_cast<entt::entity>(id);
}

const Fatigue& fatigue(const World& w, std::uint32_t id) {
    return w.registry().get<Fatigue>(ent(id));
}

void pace(World& w, std::uint32_t id, std::uint8_t kind) {
    Command c;
    c.type = CommandType::SetStance;
    c.player = 0;
    c.units = {id};
    c.kind = kind;
    w.issue(c);
}

// Marcha n ticks hacia el este y devuelve cuánto ha avanzado (en casillas).
Fixed march(World& w, std::uint32_t id, int ticks) {
    const auto x0 = w.registry().get<rts::sim::Position>(ent(id)).x;
    w.issue(move_order(w.tick(), {id}, {120, 30}));
    for (int t = 0; t < ticks; ++t) {
        w.step();
    }
    return w.registry().get<rts::sim::Position>(ent(id)).x - x0;
}

}  // namespace

TEST_CASE("Fatiga: marchar cansa y parar repone") {
    World w(fatigue_params());
    const auto s = w.spawn_unit(0, kSoldier, {20, 30});
    w.step();
    CHECK(fatigue(w, s).value == 0);
    march(w, s, 200);
    const std::int32_t tired = fatigue(w, s).value;
    CHECK(tired > 15'000);
    rts::sim::Command stop;
    stop.type = CommandType::Stop;
    stop.player = 0;
    stop.units = {s};
    w.issue(stop);
    for (int t = 0; t < 50; ++t) {
        w.step();
    }
    CHECK(fatigue(w, s).value < tired);
}

TEST_CASE("Fatiga: cansada, la unidad anda y pega peor; agotada, al 70 %") {
    WorldParams p = fatigue_params();
    p.fatigue.march_per_tick = 2000;  // se agota enseguida
    World w(p);
    const auto s = w.spawn_unit(0, kSoldier, {20, 30});
    march(w, s, 100);
    CHECK(fatigue(w, s).value == rts::sim::kFullFatigue);
    CHECK(fatigue(w, s).speed_percent == 70);
    CHECK(fatigue(w, s).attack_percent == 70);

    World fresh(fatigue_params());
    const auto f = fresh.spawn_unit(0, kSoldier, {20, 30});
    World exhausted(p);
    const auto x = exhausted.spawn_unit(0, kSoldier, {20, 30});
    march(exhausted, x, 60);  // ya agotada
    const Fixed d_fresh = march(fresh, f, 200);
    const Fixed d_tired = march(exhausted, x, 200);
    CHECK(d_tired < d_fresh * Fixed::from_ratio(8, 10));
}

TEST_CASE("Fatiga: el paso forzado va un 20 % más deprisa y cansa el triple") {
    World normal(fatigue_params());
    World forced(fatigue_params());
    const auto a = normal.spawn_unit(0, kSoldier, {20, 30});
    const auto b = forced.spawn_unit(0, kSoldier, {20, 30});
    pace(forced, b, rts::sim::kPaceForced);
    forced.step();
    normal.step();
    CHECK(fatigue(forced, b).forced);
    const Fixed da = march(normal, a, 40);
    const Fixed db = march(forced, b, 40);
    CHECK(db > da * Fixed::from_ratio(11, 10));
    CHECK(fatigue(forced, b).value > fatigue(normal, a).value * 25 / 10);
    pace(forced, b, rts::sim::kPaceNormal);
    forced.step();
    CHECK_FALSE(fatigue(forced, b).forced);
}

TEST_CASE("Fatiga: el terreno difícil cansa más") {
    WorldParams rough = fatigue_params();
    rough.movement.speed_percent_by_terrain = {100, 50, 100};
    World flat(fatigue_params());
    World hard(rough);
    const auto a = flat.spawn_unit(0, kSoldier, {20, 30});
    const auto b = hard.spawn_unit(0, kSoldier, {20, 30});
    march(flat, a, 100);
    march(hard, b, 100);
    CHECK(fatigue(hard, b).value > fatigue(flat, a).value * 18 / 10);
}

TEST_CASE("Fatiga: el aguante del tipo escala el cansancio") {
    WorldParams p = fatigue_params();
    p.unit_types[kArcher].stamina = 200;
    World w(p);
    const auto s = w.spawn_unit(0, kSoldier, {20, 30});
    const auto a = w.spawn_unit(0, kArcher, {20, 34});
    w.issue(move_order(w.tick(), {s}, {120, 30}));
    w.issue(move_order(w.tick(), {a}, {120, 34}));
    for (int t = 0; t < 100; ++t) {
        w.step();
    }
    CHECK(fatigue(w, a).value * 2 <= fatigue(w, s).value + 200);
    CHECK(fatigue(w, a).value > 0);
}

TEST_CASE("Fatiga: desactivada, ninguna unidad se cansa") {
    WorldParams p = fatigue_params();
    p.fatigue.enabled = false;
    World w(p);
    const auto s = w.spawn_unit(0, kSoldier, {20, 30});
    march(w, s, 100);
    CHECK(w.registry().view<const Fatigue>().empty());
}
