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
    constexpr std::uint64_t kExpectedHash = 0xc17592b0db725cbfULL;
    INFO(std::format("hash obtenido: 0x{:016x}", world.state_hash()));
    CHECK(world.state_hash() == kExpectedHash);
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
