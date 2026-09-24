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

using rts::sim::DemoParams;
using rts::sim::Fixed;
using rts::sim::World;

namespace {

DemoParams regression_params() {
    DemoParams p;
    p.seed = 0x5EED'2026'0924ULL;
    p.point_count = 1000;
    p.arena_tiles = 256;
    p.max_speed = Fixed::from_ratio(3, 10);
    return p;
}

// 10 minutos de juego a 20 Hz.
constexpr int kTicks = 12'000;

}  // namespace

TEST_CASE("Regresión: hash de estado tras 12 000 ticks") {
    World world(regression_params());
    for (int i = 0; i < kTicks; ++i) {
        world.step();
    }
    constexpr std::uint64_t kExpectedHash = 0xb462a265f9449b10ULL;
    INFO(std::format("hash obtenido: 0x{:016x}", world.state_hash()));
    CHECK(world.state_hash() == kExpectedHash);
}

TEST_CASE("Determinismo: dos mundos con la misma semilla coinciden tick a tick") {
    World a(regression_params());
    World b(regression_params());
    for (int i = 0; i < 200; ++i) {
        a.step();
        b.step();
        REQUIRE(a.state_hash() == b.state_hash());
    }
}

TEST_CASE("Determinismo: otra semilla da otro estado") {
    DemoParams other = regression_params();
    other.seed += 1;
    World a(regression_params());
    World b(other);
    CHECK(a.state_hash() != b.state_hash());
}

TEST_CASE("Demo: ningún punto sale de la arena") {
    World world(regression_params());
    rts::sim::Snapshot snap;
    for (int i = 0; i < 2'000; ++i) {
        world.step();
    }
    world.write_snapshot(snap);
    REQUIRE(snap.entities.size() == 1000);
    for (const auto& e : snap.entities) {
        CHECK(e.pos.x >= Fixed{});
        CHECK(e.pos.x <= snap.arena_size);
        CHECK(e.pos.y >= Fixed{});
        CHECK(e.pos.y <= snap.arena_size);
    }
}
