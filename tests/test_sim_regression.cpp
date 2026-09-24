// Prueba de regresión de la simulación: semilla fija, número de ticks fijo, hash
// del estado final fijado en el código. Si el hash cambia sin que se haya tocado
// el diseño, es un fallo de determinismo. Si el cambio de diseño es intencionado,
// se actualiza kExpectedHash en el mismo commit y se explica en el mensaje.
//
// La CI ejecuta esta prueba con MSVC, clang-cl, Clang y GCC: el mismo hash en los
// cuatro demuestra que la simulación es idéntica bit a bit entre compiladores.

#include <algorithm>
#include <cstdint>
#include <format>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "sim/world.hpp"
#include "test_helpers.hpp"

using rts::sim::World;
using rts::test::test_world_params;

namespace {

// 1 minuto de juego a 20 Hz con órdenes que ejercitan campos de flujo, HPA* y evitación.
constexpr int kTicks = 1'200;

void run_script(World& world) {
    const auto ids = rts::test::all_unit_ids(world);
    const std::vector<std::uint32_t> first_half(ids.begin(), ids.begin() + static_cast<std::ptrdiff_t>(ids.size() / 2));
    const std::vector<std::uint32_t> second_half(ids.begin() + static_cast<std::ptrdiff_t>(ids.size() / 2), ids.end());
    const std::vector<std::uint32_t> four(ids.begin(), ids.begin() + 4);
    world.issue(rts::test::move_order(10, ids, {170, 90}));
    world.issue(rts::test::move_order(500, first_half, {100, 160}));
    world.issue(rts::test::move_order(500, second_half, {160, 120}));
    world.issue(rts::test::move_order(900, four, {40, 210}));
    for (int i = 0; i < kTicks; ++i) {
        world.step();
    }
}

}  // namespace

TEST_CASE("Regresión: hash de estado tras 1 200 ticks con 500 unidades en movimiento") {
    // 500 y no 1000 para que la prueba quepa en ~20 s en Debug; el banco rts_bench
    // cubre 1000 y 2000 unidades en Release.
    World world(test_world_params(500, 64));
    run_script(world);
    constexpr std::uint64_t kExpectedHash = 0x8b647c9748cec1b9ULL;
    INFO(std::format("hash obtenido: 0x{:016x}", world.state_hash()));
    CHECK(world.state_hash() == kExpectedHash);
}

namespace {

// Partida de dos jugadores con preparación completa (bosques, recursos, aldeanos) y un
// guion económico: recogida de cuatro recursos, entrenamiento con pausa por población,
// construcción de una casa y de un almacén, talas que cambian el pathfinding.
rts::sim::WorldParams economy_params() {
    using namespace rts::test;
    rts::sim::WorldParams p = test_world_params(0, 16);
    p.setup.seed = 21;
    p.setup.starts = {{60, 60}, {196, 196}};
    p.setup.start_search_radius = 60;
    p.setup.min_start_region_tiles = 2000;
    p.setup.start_building = kCenter;
    p.setup.start_unit = kVillager;
    p.setup.start_units = 5;
    p.setup.start_stock = stock(300, 200, 100, 100);
    p.setup.near_start = {{kGoldMine, 1, 6, 10}, {kBerries, 6, 4, 7}, {kTree, 12, 8, 12}};
    p.setup.forest_terrain = 2;
    p.setup.tree_type = kTree;
    p.setup.tree_density_permille = 300;
    p.setup.clear_radius = 7;
    return p;
}

// Objeto del tipo pedido más cercano (en índice de snapshot) al primer edificio del
// jugador; en enteros.
std::uint32_t nearest_object(const rts::sim::Snapshot& snap, rts::sim::PlayerId player, rts::sim::ObjectKind kind,
                             std::uint8_t type) {
    rts::sim::TileCoord home{};
    for (const auto& o : snap.objects) {
        if (o.kind == rts::sim::ObjectKind::Building && o.owner == player) {
            home = o.origin;
            break;
        }
    }
    std::uint32_t best = rts::sim::kNoObject;
    std::int32_t best_d = 1 << 30;
    for (const auto& o : snap.objects) {
        if (o.kind == kind && o.type == type) {
            const std::int32_t d = rts::sim::octile_distance(home, o.origin);
            if (d < best_d) {
                best_d = d;
                best = o.id;
            }
        }
    }
    return best;
}

void run_economy_script(World& world, std::int32_t ticks) {
    using namespace rts::test;
    rts::sim::Snapshot snap;
    world.write_snapshot(snap);
    for (rts::sim::PlayerId player = 0; player < 2; ++player) {
        std::vector<std::uint32_t> units;
        std::uint32_t center = rts::sim::kNoObject;
        rts::sim::TileCoord center_origin{};
        for (const auto& e : snap.entities) {
            if (e.owner == player) {
                units.push_back(e.id);
            }
        }
        for (const auto& o : snap.objects) {
            if (o.kind == rts::sim::ObjectKind::Building && o.owner == player) {
                center = o.id;
                center_origin = o.origin;
            }
        }
        REQUIRE(units.size() == 5);
        auto order = [&](rts::sim::Tick tick, rts::sim::CommandType type, std::vector<std::uint32_t> who,
                         std::uint32_t object) {
            rts::sim::Command c;
            c.tick = tick;
            c.player = player;
            c.type = type;
            c.units = std::move(who);
            c.object = object;
            return c;
        };
        const auto tree = nearest_object(snap, player, rts::sim::ObjectKind::Resource, kTree);
        const auto gold = nearest_object(snap, player, rts::sim::ObjectKind::Resource, kGoldMine);
        const auto berries = nearest_object(snap, player, rts::sim::ObjectKind::Resource, kBerries);
        world.issue(order(1, rts::sim::CommandType::Gather, {units[0], units[1], units[2]}, tree));
        world.issue(order(1, rts::sim::CommandType::Gather, {units[3]}, gold));
        world.issue(order(1, rts::sim::CommandType::Gather, {units[4]}, berries));
        for (std::int32_t i = 0; i < 3; ++i) {  // la tercera espera a la casa
            auto train = order(1, rts::sim::CommandType::Train, {}, center);
            train.kind = kVillager;
            world.issue(train);
        }
        // Casa de 2x2 dejando una fila libre por encima del centro urbano (dentro del claro).
        auto house = order(300, rts::sim::CommandType::Place, {units[4]}, rts::sim::kNoObject);
        house.kind = kHouse;
        house.target = {center_origin.x, center_origin.y - 3};
        world.issue(house);
    }
    for (std::int32_t i = 0; i < ticks; ++i) {
        world.step();
    }
}

}  // namespace

TEST_CASE("Regresión: hash de estado de una partida económica de 1 500 ticks") {
    World world(economy_params());
    run_economy_script(world, 1'500);
    rts::sim::Snapshot snap;
    world.write_snapshot(snap);
    for (std::size_t p = 0; p < snap.players.size(); ++p) {
        const auto& ps = snap.players[p];
        MESSAGE("jugador " << p << ": comida " << ps.stock[0] << ", madera " << ps.stock[1] << ", piedra "
                           << ps.stock[2] << ", oro " << ps.stock[3] << ", población " << ps.population << "/"
                           << ps.population_cap);
    }
    // Las dos casas se construyen (tope 10) y cada jugador entrena sus 3 aldeanos.
    CHECK(snap.players[0].population_cap == 10);
    CHECK(snap.players[1].population == 8);
    constexpr std::uint64_t kExpectedHash = 0x1c2c5da3f0f96e61ULL;
    INFO(std::format("hash obtenido: 0x{:016x}", world.state_hash()));
    CHECK(world.state_hash() == kExpectedHash);
}

TEST_CASE("Determinismo: dos partidas económicas coinciden tick a tick") {
    World a(economy_params());
    World b(economy_params());
    run_economy_script(a, 0);
    run_economy_script(b, 0);
    for (int i = 0; i < 400; ++i) {
        a.step();
        b.step();
        REQUIRE(a.state_hash() == b.state_hash());
    }
}

TEST_CASE("Determinismo: dos mundos con las mismas órdenes coinciden tick a tick") {
    World a(test_world_params(300, 32));
    World b(test_world_params(300, 32));
    const auto ids = rts::test::all_unit_ids(a);
    a.issue(rts::test::move_order(0, ids, {150, 100}));
    b.issue(rts::test::move_order(0, ids, {150, 100}));
    for (int i = 0; i < 300; ++i) {
        a.step();
        b.step();
        REQUIRE(a.state_hash() == b.state_hash());
    }
}

TEST_CASE("Determinismo: otra semilla de unidades u otra semilla de mapa dan otro estado") {
    auto other_demo = test_world_params(100, 32);
    other_demo.demo.seed += 1;
    auto other_map = test_world_params(100, 32);
    other_map.map.seed += 1;
    const World base(test_world_params(100, 32));
    CHECK(base.state_hash() != World(other_demo).state_hash());
    CHECK(base.state_hash() != World(other_map).state_hash());
}

TEST_CASE("Aparición: las unidades nacen en casillas transitables y distintas") {
    const World world(test_world_params());
    rts::sim::Snapshot snap;
    world.write_snapshot(snap);
    REQUIRE(snap.entities.size() == 1000);
    std::vector<std::pair<std::int32_t, std::int32_t>> tiles;
    for (const auto& e : snap.entities) {
        const rts::sim::TileCoord t{e.pos.x.floor_to_int(), e.pos.y.floor_to_int()};
        CHECK(world.movement().grid().passable(t));
        tiles.emplace_back(t.x, t.y);
    }
    std::ranges::sort(tiles);
    CHECK(std::ranges::adjacent_find(tiles) == tiles.end());
}

TEST_CASE("Snapshot: comparte el mapa sin copiarlo") {
    World world(test_world_params(10, 16));
    rts::sim::Snapshot a;
    rts::sim::Snapshot b;
    world.write_snapshot(a);
    world.step();
    world.write_snapshot(b);
    REQUIRE(a.map.get() != nullptr);  // get(): doctest no sabe imprimir shared_ptr con la STL de MSVC
    CHECK(a.map.get() == b.map.get());
    CHECK(a.map.get() == &world.map());
}
