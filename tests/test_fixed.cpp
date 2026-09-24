#include <doctest/doctest.h>

#include "sim/fixed.hpp"

using rts::sim::Fixed;

// Casi todo es constexpr: si una aserción de esta lista falla, no compila.
static_assert(Fixed::from_int(1).raw() == 65536);
static_assert(Fixed::from_int(-3).raw() == -3 * 65536);
static_assert(Fixed::from_ratio(3, 2).raw() == 98304);
static_assert(Fixed::from_ratio(1, 3).raw() == 21845);   // truncado hacia cero
static_assert(Fixed::from_ratio(-1, 3).raw() == -21845);  // también hacia cero
static_assert(Fixed::from_int(2) * Fixed::from_int(3) == Fixed::from_int(6));
static_assert(Fixed::from_int(7) / Fixed::from_int(2) == Fixed::from_ratio(7, 2));
static_assert(Fixed::from_int(5) - Fixed::from_int(8) == Fixed::from_int(-3));
static_assert(-Fixed::from_int(4) == Fixed::from_int(-4));
static_assert(Fixed::from_int(1) < Fixed::from_ratio(3, 2));

TEST_CASE("Fixed: la multiplicación redondea hacia -infinito") {
    const Fixed tiny = Fixed::from_raw(1);  // 1/65536
    const Fixed half = Fixed::from_ratio(1, 2);
    CHECK((tiny * half).raw() == 0);
    CHECK((-tiny * half).raw() == -1);
}

TEST_CASE("Fixed: la división redondea hacia cero") {
    CHECK((Fixed::from_int(1) / Fixed::from_int(3)).raw() == 21845);
    CHECK((Fixed::from_int(-1) / Fixed::from_int(3)).raw() == -21845);
}

TEST_CASE("Fixed: floor_to_int redondea hacia -infinito") {
    CHECK(Fixed::from_ratio(5, 2).floor_to_int() == 2);
    CHECK(Fixed::from_ratio(-1, 2).floor_to_int() == -1);
    CHECK(Fixed::from_ratio(-5, 2).floor_to_int() == -3);
}

TEST_CASE("Fixed: los extremos del rango") {
    CHECK(Fixed::max().raw() == 0x7fffffff);
    CHECK(Fixed::lowest().raw() == static_cast<std::int32_t>(0x80000000u));
    CHECK(Fixed::from_int(32767).floor_to_int() == 32767);
    CHECK(Fixed::from_int(-32768) == Fixed::lowest());
}

TEST_CASE("mul_wide: distancia al cuadrado de la diagonal de un mapa de 256 casillas") {
    // 256² + 256² = 131072 no cabe en 16.16 (máximo 32767); en 32.32 sí.
    const Fixed side = Fixed::from_int(256);
    const std::int64_t dist_sq = mul_wide(side, side) + mul_wide(side, side);
    CHECK(dist_sq == std::int64_t{131072} << 32);
}
