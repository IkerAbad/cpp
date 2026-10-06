// Pruebas de las fortificaciones (B4): el muro cierra el paso; la puerta deja pasar al
// dueño y a nadie más; el enemigo que se queda ante la puerta acaba atacándola; las
// torres admiten guarnición hasta llenarse, la protegen y le dan alcance; vaciarlas o
// cualquier otra orden la saca.

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "sim/world.hpp"
#include "test_helpers.hpp"

using rts::sim::BuildingTypeId;
using rts::sim::TileCoord;
using rts::sim::World;
using rts::sim::WorldParams;
using namespace rts::test;

namespace {

constexpr std::int32_t kLo = 40;  // recinto: muro en el borde del cuadrado [kLo, kHi]
constexpr std::int32_t kHi = 46;
constexpr TileCoord kGateTile{43, kLo};

struct Walled {
    WorldParams params = test_world_params(0, 16);
    BuildingTypeId wall = 0;
    BuildingTypeId gate = 0;

    Walled() {
        params.map.bands = {{rts::sim::kElevationRange, 1}};
        params.demo.player = 1;
        rts::sim::BuildingType w = params.building_types[kHouse];
        w.size = 1;
        w.population = 0;
        w.material = rts::sim::Material::Stone;
        w.hp = 2000;
        w.climbable = true;
        wall = static_cast<BuildingTypeId>(params.building_types.size());
        params.building_types.push_back(w);
        w.climbable = false;
        w.gate = true;
        w.material = rts::sim::Material::Wood;
        gate = static_cast<BuildingTypeId>(params.building_types.size());
        params.building_types.push_back(w);
    }

    // Recinto del jugador 0 con una puerta en el lado norte.
    std::uint32_t build(World& world) const {
        std::uint32_t gate_id = 0;
        for (std::int32_t i = kLo; i <= kHi; ++i) {
            for (const TileCoord t : {TileCoord{i, kLo}, TileCoord{i, kHi}, TileCoord{kLo, i}, TileCoord{kHi, i}}) {
                if (t == kGateTile) {
                    gate_id = *world.spawn_building(0, gate, t, true);
                } else if (world.object_at(t) == std::nullopt) {
                    REQUIRE(world.spawn_building(0, wall, t, true).has_value());
                }
            }
        }
        world.step();
        return gate_id;
    }
};

TileCoord tile(const World& w, std::uint32_t id) {
    return rts::sim::tile_of(w.registry().get<rts::sim::Position>(static_cast<entt::entity>(id)));
}

bool inside(TileCoord t) {
    return t.x > kLo && t.x < kHi && t.y > kLo && t.y < kHi;
}

void run(World& w, int ticks) {
    for (int t = 0; t < ticks; ++t) {
        w.step();
    }
}

}  // namespace

TEST_CASE("Fortificaciones: el dueño sale y entra por su puerta") {
    Walled f;
    World w(f.params);
    f.build(w);
    const auto s = w.spawn_unit(0, kVillager, {43, 43});
    w.issue(move_order(w.tick(), {s}, {43, 30}));
    run(w, 800);
    CHECK(tile(w, s) == TileCoord{43, 30});
    w.issue(move_order(w.tick(), {s}, {44, 44}));
    run(w, 800);
    CHECK(tile(w, s) == TileCoord{44, 44});
}

TEST_CASE("Fortificaciones: el enemigo no cruza ni el muro ni la puerta") {
    Walled f;
    World w(f.params);
    f.build(w);
    const auto out = w.spawn_unit(1, kVillager, {43, 34});  // no ataca: solo prueba el paso
    w.issue([&] {
        auto c = move_order(w.tick(), {out}, {44, 44});
        c.player = 1;
        return c;
    }());
    run(w, 1500);
    CHECK_FALSE(inside(tile(w, out)));
    CHECK(tile(w, out).y < kLo + 1);
}

TEST_CASE("Fortificaciones: quien se queda ante la puerta enemiga acaba atacándola") {
    Walled f;
    World w(f.params);
    const auto gate = f.build(w);
    const auto soldier = w.spawn_unit(1, kSoldier, {43, 34});
    w.issue([&] {
        auto c = move_order(w.tick(), {soldier}, {44, 44});
        c.player = 1;
        return c;
    }());
    bool attacked = false;
    for (int t = 0; t < 3000 && !attacked; ++t) {
        w.step();
        const auto& c = w.registry().get<rts::sim::Combatant>(static_cast<entt::entity>(soldier));
        attacked = c.target == static_cast<entt::entity>(gate);
    }
    CHECK(attacked);
    CHECK_FALSE(inside(tile(w, soldier)));
}

namespace {

// Torre de 1 casilla en (60, 60) para el jugador 0, con plazas y 2 niveles de altura.
struct Towered {
    WorldParams params = test_world_params(0, 16);
    BuildingTypeId tower = 0;

    explicit Towered(std::int32_t places = 2) {
        params.map.bands = {{rts::sim::kElevationRange, 1}};
        params.demo.player = 1;
        params.garrison.enter_reach = rts::sim::Fixed::from_ratio(4, 5);
        auto& t = params.combat.terrain;
        t.enabled = true;
        t.range_per_level = rts::sim::Fixed::from_ratio(1, 2);
        t.max_levels = 0;  // el terreno no cuenta: solo la torre
        t.ranged_percent_per_level = 10;
        t.charge_run_ticks = 20;
        rts::sim::BuildingType b = params.building_types[kHouse];
        b.size = 1;
        b.population = 0;
        b.garrison = places;
        b.garrison_levels = 2;
        tower = static_cast<BuildingTypeId>(params.building_types.size());
        params.building_types.push_back(b);
        params.unit_types[kSoldier].combat.hp = 400;
    }
};

void hold_ground_both(World& w, std::uint32_t mine, std::uint32_t theirs) {
    for (const auto& [player, id] : {std::pair{rts::sim::PlayerId{0}, mine}, std::pair{rts::sim::PlayerId{1}, theirs}}) {
        rts::sim::Command c;
        c.type = rts::sim::CommandType::SetStance;
        c.player = player;
        c.units = {id};
        c.kind = static_cast<std::uint8_t>(rts::sim::Stance::HoldGround);
        w.issue(c);
    }
}

rts::sim::Command garrison_order(std::vector<std::uint32_t> units, std::uint32_t tower, std::uint8_t kind = 0) {
    rts::sim::Command c;
    c.type = rts::sim::CommandType::Garrison;
    c.player = 0;
    c.units = std::move(units);
    c.object = tower;
    c.kind = kind;
    return c;
}

const rts::sim::Garrisoned* garrisoned(const World& w, std::uint32_t id) {
    return w.registry().try_get<rts::sim::Garrisoned>(static_cast<entt::entity>(id));
}

bool inside_tower(const World& w, std::uint32_t id) {
    const auto* g = garrisoned(w, id);
    return g != nullptr && g->inside;
}

}  // namespace

TEST_CASE("Torres: los tiradores entran hasta llenar las plazas y dentro no se les puede atacar") {
    Towered f(1);
    World w(f.params);
    const auto tower = *w.spawn_building(0, f.tower, {60, 60}, true);
    const auto a = w.spawn_unit(0, kArcher, {56, 60});
    const auto b = w.spawn_unit(0, kArcher, {56, 62});
    w.issue(garrison_order({a, b}, tower));
    run(w, 300);
    CHECK(inside_tower(w, a) != inside_tower(w, b));  // una plaza: entra uno
    const auto in = inside_tower(w, a) ? a : b;
    CHECK_FALSE(w.registry().all_of<rts::sim::Velocity>(static_cast<entt::entity>(in)));
    rts::sim::Snapshot snap;
    w.write_snapshot(snap);
    const auto it = std::ranges::find(snap.objects, tower, &rts::sim::SnapshotObject::id);
    REQUIRE(it != snap.objects.end());
    CHECK(it->garrison == 1);

    // Un enemigo no puede tomarlo por blanco.
    const auto foe = w.spawn_unit(1, kSoldier, {62, 60});
    rts::sim::Command atk;
    atk.type = rts::sim::CommandType::Attack;
    atk.player = 1;
    atk.units = {foe};
    atk.object = in;
    w.issue(atk);
    w.step();
    CHECK(w.registry().get<rts::sim::Combatant>(static_cast<entt::entity>(foe)).target !=
          static_cast<entt::entity>(in));
}

TEST_CASE("Torres: desde lo alto se alcanza más lejos que a pie") {
    const auto damage = [](bool in_tower) {
        Towered f;
        World w(f.params);
        const auto tower = *w.spawn_building(0, f.tower, {60, 60}, true);
        const auto archer = w.spawn_unit(0, kArcher, in_tower ? TileCoord{58, 60} : TileCoord{60, 61});
        const auto target = w.spawn_unit(1, kSoldier, {65, 60});  // a 5 casillas del centro de la torre
        hold_ground_both(w, archer, target);
        if (in_tower) {
            w.issue(garrison_order({archer}, tower));
        }
        run(w, 400);
        const auto& h = w.registry().get<rts::sim::Health>(static_cast<entt::entity>(target));
        return h.max_hp - h.hp;
    };
    CHECK(damage(false) == 0);  // a pie, 4 de alcance: no llega
    CHECK(damage(true) > 0);    // en la torre, 2 niveles: 5
}

TEST_CASE("Torres: vaciar la torre o cualquier otra orden saca a la guarnición") {
    Towered f;
    World w(f.params);
    const auto tower = *w.spawn_building(0, f.tower, {60, 60}, true);
    const auto a = w.spawn_unit(0, kArcher, {57, 60});
    const auto b = w.spawn_unit(0, kArcher, {57, 61});
    w.issue(garrison_order({a, b}, tower));
    run(w, 300);
    REQUIRE(inside_tower(w, a));
    REQUIRE(inside_tower(w, b));
    w.issue(garrison_order({}, tower, rts::sim::kUngarrison));
    w.step();
    CHECK(garrisoned(w, a) == nullptr);
    CHECK(w.registry().all_of<rts::sim::Velocity>(static_cast<entt::entity>(a)));
    w.issue(garrison_order({a}, tower));
    run(w, 200);
    REQUIRE(inside_tower(w, a));
    auto mv = move_order(w.tick(), {a}, {50, 50});
    w.issue(mv);
    run(w, 600);
    CHECK(garrisoned(w, a) == nullptr);
    CHECK(tile(w, a) == TileCoord{50, 50});
}

namespace {

rts::sim::Command climb_order(std::vector<std::uint32_t> units, std::uint32_t wall) {
    rts::sim::Command c;
    c.type = rts::sim::CommandType::Climb;
    c.player = 1;
    c.units = std::move(units);
    c.object = wall;
    return c;
}

Walled climbing_params() {
    Walled f;
    f.params.unit_types[kSoldier].climbs = true;
    f.params.climb.ladder_cost = stock(0, 15, 0, 0);
    f.params.climb.climb_ticks = 60;
    f.params.climb.reach = rts::sim::Fixed::from_ratio(4, 5);
    f.params.climb.exposed_percent = 150;
    return f;
}

}  // namespace

TEST_CASE("Escalas: la infantería toma el muro y baja al otro lado, pagando la escala") {
    Walled f = climbing_params();
    World w(f.params);
    f.build(w);
    w.set_stock(1, stock(0, 20, 0, 0));
    const auto s = w.spawn_unit(1, kSoldier, {42, 36});
    const auto wall = *w.object_at({42, kLo});
    w.issue(climb_order({s}, wall));
    w.step();
    CHECK(w.player_state(1).stock[rts::sim::resource_index(rts::sim::Resource::Wood)] == 5);
    bool was_climbing = false;
    for (int t = 0; t < 600 && !inside(tile(w, s)); ++t) {
        w.step();
        was_climbing = was_climbing || w.registry().all_of<rts::sim::Climbing>(static_cast<entt::entity>(s));
    }
    CHECK(was_climbing);
    CHECK(inside(tile(w, s)));
    CHECK(tile(w, s) == TileCoord{42, kLo + 1});
}

TEST_CASE("Escalas: sin madera para la escala no hay escalada, y quien no sabe no escala") {
    Walled f = climbing_params();
    World w(f.params);
    f.build(w);
    w.set_stock(1, stock(0, 10, 0, 0));
    const auto s = w.spawn_unit(1, kSoldier, {42, 36});
    const auto archer = w.spawn_unit(1, kArcher, {41, 36});
    const auto wall = *w.object_at({42, kLo});
    w.issue(climb_order({s, archer}, wall));
    w.step();
    CHECK_FALSE(w.registry().all_of<rts::sim::Climbing>(static_cast<entt::entity>(s)));
    CHECK_FALSE(w.registry().all_of<rts::sim::Climbing>(static_cast<entt::entity>(archer)));
    CHECK(w.player_state(1).stock[rts::sim::resource_index(rts::sim::Resource::Wood)] == 10);
}

TEST_CASE("Escalas: si al otro lado no hay sitio, la escalada fracasa") {
    Walled f = climbing_params();
    World w(f.params);
    f.build(w);
    REQUIRE(w.spawn_building(0, f.wall, {42, kLo + 1}, true).has_value());  // segunda fila de muro
    w.set_stock(1, stock(0, 20, 0, 0));
    const auto s = w.spawn_unit(1, kSoldier, {42, 36});
    w.issue(climb_order({s}, *w.object_at({42, kLo})));
    run(w, 600);
    CHECK_FALSE(inside(tile(w, s)));
    CHECK(w.climb().last_stats().failed == 0);  // ya pasó el tick del fracaso
    CHECK_FALSE(w.registry().all_of<rts::sim::Climbing>(static_cast<entt::entity>(s)));
}
