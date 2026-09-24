#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "sim/fmath.hpp"
#include "sim/world.hpp"
#include "test_helpers.hpp"

using rts::sim::Command;
using rts::sim::CommandType;
using rts::sim::Fixed;
using rts::sim::FVec2;
using rts::sim::MoveGoal;
using rts::sim::Position;
using rts::sim::TileCoord;
using rts::sim::Unit;
using rts::sim::World;
using rts::sim::WorldParams;
using rts::test::all_unit_ids;
using rts::test::move_order;
using rts::test::test_world_params;

namespace {

// Mapa todo transitable (una sola banda de llanura): geometría predecible.
WorldParams open_field(std::int32_t units, std::int32_t area) {
    WorldParams p = test_world_params(units, area);
    p.map.bands = {{rts::sim::kElevationRange, 1}};
    return p;
}

struct Stats {
    std::int32_t with_goal = 0;
    std::int32_t arrived = 0;
    double max_center_dist = 0.0;   // casillas hasta el centro de la casilla destino
    double mean_center_dist = 0.0;
};

Stats goal_stats(const World& world) {
    Stats s;
    const auto& reg = world.registry();
    for (const auto e : reg.view<const Position, const MoveGoal>()) {
        const auto& p = reg.get<Position>(e);
        const auto& g = reg.get<MoveGoal>(e);
        ++s.with_goal;
        s.arrived += g.arrived ? 1 : 0;
        const FVec2 center{Fixed::from_int(g.tile.x) + Fixed::from_ratio(1, 2),
                           Fixed::from_int(g.tile.y) + Fixed::from_ratio(1, 2)};
        const double d = static_cast<double>(rts::sim::length(center - FVec2{p.x, p.y}).raw()) / 65536.0;
        s.max_center_dist = std::max(s.max_center_dist, d);
        s.mean_center_dist += d;
    }
    if (s.with_goal > 0) {
        s.mean_center_dist /= s.with_goal;
    }
    return s;
}

// Menor distancia entre centros relativa a la suma de radios (1.0 = se tocan).
double min_separation_ratio(const World& world) {
    const auto& reg = world.registry();
    std::vector<std::pair<FVec2, Fixed>> units;
    for (const auto e : reg.view<const Position, const Unit>()) {
        const auto& p = reg.get<Position>(e);
        units.push_back({{p.x, p.y}, reg.get<Unit>(e).radius});
    }
    double worst = 1e9;
    for (std::size_t i = 0; i < units.size(); ++i) {
        for (std::size_t j = i + 1; j < units.size(); ++j) {
            const FVec2 d = units[j].first - units[i].first;
            if (d.x > Fixed::from_int(2) || d.x < Fixed::from_int(-2) || d.y > Fixed::from_int(2) ||
                d.y < Fixed::from_int(-2)) {
                continue;
            }
            const double dist = static_cast<double>(rts::sim::length(d).raw());
            const double sum = static_cast<double>((units[i].second + units[j].second).raw());
            worst = std::min(worst, dist / sum);
        }
    }
    return worst;
}

bool all_on_passable_tiles(const World& world) {
    const auto& reg = world.registry();
    for (const auto e : reg.view<const Position>()) {
        const auto& p = reg.get<Position>(e);
        if (!world.movement().grid().passable({p.x.floor_to_int(), p.y.floor_to_int()})) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("Movimiento: un grupo de 200 llega en racimo con un campo de flujo") {
    World world(open_field(200, 20));
    const auto ids = all_unit_ids(world);
    REQUIRE(ids.size() == 200);
    world.issue(move_order(0, ids, {180, 60}));
    world.step();
    CHECK(world.movement().last_stats().flow_fields_built == 1);
    CHECK(world.movement().last_stats().paths_pending == 0);  // nadie pidió A*

    // ~100 casillas en diagonal a 1,2 casillas/s: ~85 s; margen hasta 150 s.
    for (int t = 0; t < 3000 && goal_stats(world).arrived < 200; ++t) {
        world.step();
    }
    const Stats s = goal_stats(world);
    MESSAGE("llegaron " << s.arrived << " de 200 en " << world.tick() << " ticks; distancia al centro: media "
                        << s.mean_center_dist << ", máxima " << s.max_center_dist << " casillas");
    CHECK(s.arrived == 200);
    // Ideal: 200 discos de radio 0,3 en empaquetado hexagonal perfecto ocupan un círculo
    // de radio ~4,5 casillas, con distancia media al centro ~3. Criterio: media menor
    // que el doble del ideal y máxima menor que el triple del radio ideal (los que
    // desisten por atasco se quedan en el borde).
    CHECK(s.mean_center_dist < 6.0);
    CHECK(s.max_center_dist < 13.5);
}

TEST_CASE("Movimiento: nunca se solapan de forma apreciable ni pisan casillas bloqueadas") {
    // El centro del mapa de prueba es agua: el área de aparición de 64 casillas llega a tierra.
    World world(test_world_params(300, 64));
    const auto ids = all_unit_ids(world);
    REQUIRE(ids.size() == 300);
    world.issue(move_order(0, ids, {40, 200}));
    double worst = 1e9;
    for (int t = 0; t < 600; ++t) {
        world.step();
        REQUIRE(all_on_passable_tiles(world));
        if (t % 20 == 0) {
            worst = std::min(worst, min_separation_ratio(world));
        }
    }
    MESSAGE("separación mínima observada: " << worst << " x (suma de radios)");
    // La separación Jacobi corrige cada tick; un solapamiento residual pequeño es normal.
    CHECK(worst > 0.6);
}

TEST_CASE("Movimiento: dos grupos que se cruzan de frente no se bloquean") {
    // 100 unidades en un cuadrado de 12 casillas: se separan en dos mitades, cada una
    // a un extremo, y después se intercambian los destinos para que se crucen de frente.
    World w2(open_field(100, 12));
    const auto ids = all_unit_ids(w2);
    std::vector<std::uint32_t> a(ids.begin(), ids.begin() + 50);
    std::vector<std::uint32_t> b(ids.begin() + 50, ids.end());
    w2.issue(move_order(0, a, {128, 100}));
    w2.issue(move_order(0, b, {128, 156}));
    w2.step();
    w2.issue(move_order(1, a, {128, 156}));
    w2.issue(move_order(1, b, {128, 100}));
    for (int t = 0; t < 2400 && goal_stats(w2).arrived < 100; ++t) {
        w2.step();
    }
    const Stats s = goal_stats(w2);
    MESSAGE("cruce: llegaron " << s.arrived << " de 100 en " << w2.tick() << " ticks");
    CHECK(s.arrived == 100);
}

TEST_CASE("Movimiento: unidades sueltas usan HPA* y rodean el agua") {
    World world(test_world_params(4, 64));
    const auto ids = all_unit_ids(world);
    REQUIRE(ids.size() == 4);
    world.issue(move_order(0, ids, {30, 220}));
    world.step();
    CHECK(world.movement().last_stats().flow_fields_built == 0);
    for (int t = 0; t < 6000 && goal_stats(world).arrived < 4; ++t) {
        world.step();
        REQUIRE(all_on_passable_tiles(world));
    }
    CHECK(goal_stats(world).arrived == 4);
}

TEST_CASE("Movimiento: destino inalcanzable se sustituye por la casilla alcanzable más cercana") {
    World world(test_world_params(1, 64));
    const auto ids = all_unit_ids(world);
    REQUIRE(ids.size() == 1);
    // Buscar una casilla de agua (no transitable) en el mapa de prueba.
    TileCoord water{-1, -1};
    for (std::int32_t y = 0; y < 256 && water.x < 0; ++y) {
        for (std::int32_t x = 0; x < 256; ++x) {
            if (!world.movement().grid().passable({x, y})) {
                water = {x, y};
                break;
            }
        }
    }
    REQUIRE(water.x >= 0);
    world.issue(move_order(0, ids, water));
    world.step();
    const auto* goal = world.registry().try_get<MoveGoal>(static_cast<entt::entity>(ids[0]));
    if (goal != nullptr) {  // puede no haber casilla alcanzable en el radio si es un lago lejano
        CHECK(world.movement().grid().passable(goal->tile));
        CHECK_FALSE(goal->tile == water);
    }
}

TEST_CASE("Movimiento: el presupuesto reparte las búsquedas entre ticks y Stop detiene") {
    WorldParams p = test_world_params(40, 64);
    p.movement.path_node_budget_per_tick = 300;  // ~1 búsqueda por tick
    p.movement.flow_field_min_group = 1000;      // obliga a A* individual
    World world(p);
    const auto ids = all_unit_ids(world);
    world.issue(move_order(0, ids, {200, 60}));
    world.step();
    CHECK(world.movement().last_stats().paths_pending > 10);
    for (int t = 0; t < 200; ++t) {
        world.step();
    }
    CHECK(world.movement().last_stats().paths_pending == 0);

    Command stop;
    stop.tick = world.tick();
    stop.type = CommandType::Stop;
    stop.units = ids;
    world.issue(stop);
    world.step();
    world.step();
    CHECK(goal_stats(world).with_goal == 0);
}
