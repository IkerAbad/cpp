// Prueba de regresión de la simulación: semilla fija, número de ticks fijo, hash
// del estado final fijado en el código. Si el hash cambia sin que se haya tocado
// el diseño, es un fallo de determinismo. Si el cambio de diseño es intencionado,
// se actualiza kExpectedHash en el mismo commit y se explica en el mensaje.
//
// La CI ejecuta esta prueba con MSVC, clang-cl, Clang y GCC: el mismo hash en los
// cuatro demuestra que la simulación es idéntica bit a bit entre compiladores.

#include <cstdint>
#include <format>

#include <doctest/doctest.h>

#include "sim/world.hpp"
#include "test_helpers.hpp"

using rts::sim::Fixed;
using rts::sim::World;
using rts::test::test_world_params;

namespace {

// 10 minutos de juego a 20 Hz.
constexpr int kTicks = 12'000;

}  // namespace

TEST_CASE("Regresión: hash de estado tras 12 000 ticks") {
    World world(test_world_params());
    for (int i = 0; i < kTicks; ++i) {
        world.step();
    }
    constexpr std::uint64_t kExpectedHash = 0xa6dc4a641e1269f9ULL;
    INFO(std::format("hash obtenido: 0x{:016x}", world.state_hash()));
    CHECK(world.state_hash() == kExpectedHash);
}

TEST_CASE("Determinismo: dos mundos con la misma semilla coinciden tick a tick") {
    World a(test_world_params());
    World b(test_world_params());
    for (int i = 0; i < 200; ++i) {
        a.step();
        b.step();
        REQUIRE(a.state_hash() == b.state_hash());
    }
}

TEST_CASE("Determinismo: otra semilla de marcadores u otra semilla de mapa dan otro estado") {
    auto other_demo = test_world_params();
    other_demo.demo.seed += 1;
    auto other_map = test_world_params();
    other_map.map.seed += 1;
    const World base(test_world_params());
    CHECK(base.state_hash() != World(other_demo).state_hash());
    CHECK(base.state_hash() != World(other_map).state_hash());
}

TEST_CASE("Demo: ningún marcador sale de su zona central") {
    const auto params = test_world_params();
    World world(params);
    rts::sim::Snapshot snap;
    for (int i = 0; i < 2'000; ++i) {
        world.step();
    }
    world.write_snapshot(snap);
    REQUIRE(snap.entities.size() == 1000);
    const Fixed lo = Fixed::from_int((params.map.width - params.demo.area_tiles) / 2);
    const Fixed hi = lo + Fixed::from_int(params.demo.area_tiles);
    for (const auto& e : snap.entities) {
        CHECK(e.pos.x >= lo);
        CHECK(e.pos.x <= hi);
        CHECK(e.pos.y >= lo);
        CHECK(e.pos.y <= hi);
    }
}

TEST_CASE("Snapshot: comparte el mapa sin copiarlo") {
    World world(test_world_params());
    rts::sim::Snapshot a;
    rts::sim::Snapshot b;
    world.write_snapshot(a);
    world.step();
    world.write_snapshot(b);
    REQUIRE(a.map.get() != nullptr);  // get(): doctest no sabe imprimir shared_ptr con la STL de MSVC
    CHECK(a.map.get() == b.map.get());
    CHECK(a.map.get() == &world.map());
}
