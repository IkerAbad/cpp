#include <array>
#include <cstdint>

#include <doctest/doctest.h>

#include "sim/rng.hpp"

using rts::sim::SplitMix64;
using rts::sim::Xoshiro256pp;

// Vectores de referencia generados con las implementaciones en C de los autores
// (prng.di.unimi.it), tal como los recogen las pruebas del crate rand_xoshiro:
// https://github.com/rust-random/rngs/tree/master/rand_xoshiro/src

TEST_CASE("xoshiro256++: coincide con la implementación de referencia") {
    auto rng = Xoshiro256pp::from_state({1, 2, 3, 4});
    constexpr std::array<std::uint64_t, 10> expected{
        41943041ULL,
        58720359ULL,
        3588806011781223ULL,
        3591011842654386ULL,
        9228616714210784205ULL,
        9973669472204895162ULL,
        14011001112246962877ULL,
        12406186145184390807ULL,
        15849039046786891736ULL,
        10450023813501588000ULL,
    };
    for (const auto value : expected) {
        CHECK(rng.next() == value);
    }
}

TEST_CASE("splitmix64: coincide con la implementación de referencia") {
    SplitMix64 rng(1477776061723855037ULL);
    constexpr std::array<std::uint64_t, 5> expected{
        1985237415132408290ULL,
        2979275885539914483ULL,
        13511426838097143398ULL,
        8488337342461049707ULL,
        15141737807933549159ULL,
    };
    for (const auto value : expected) {
        CHECK(rng.next() == value);
    }
}

TEST_CASE("xoshiro256++: la misma semilla da la misma secuencia") {
    Xoshiro256pp a(42);
    Xoshiro256pp b(42);
    Xoshiro256pp c(43);
    bool differs = false;
    for (int i = 0; i < 1000; ++i) {
        const auto va = a.next();
        CHECK(va == b.next());
        differs = differs || va != c.next();
    }
    CHECK(differs);
}

TEST_CASE("next_below: siempre dentro del rango y cubre todos los valores") {
    Xoshiro256pp rng(7);
    constexpr std::uint32_t kBound = 6;
    std::array<int, kBound> counts{};
    for (int i = 0; i < 60'000; ++i) {
        const auto v = rng.next_below(kBound);
        REQUIRE(v < kBound);
        ++counts[v];
    }
    // Cada cara ~10 000; la tolerancia de ±600 son ~6,5 desviaciones típicas.
    for (const int n : counts) {
        CHECK(n > 9'400);
        CHECK(n < 10'600);
    }
}

TEST_CASE("next_in_range: extremos incluidos y rango completo de int32") {
    Xoshiro256pp rng(99);
    bool saw_lo = false;
    bool saw_hi = false;
    for (int i = 0; i < 10'000; ++i) {
        const auto v = rng.next_in_range(-2, 2);
        REQUIRE(v >= -2);
        REQUIRE(v <= 2);
        saw_lo = saw_lo || v == -2;
        saw_hi = saw_hi || v == 2;
    }
    CHECK(saw_lo);
    CHECK(saw_hi);
    CHECK(rng.next_in_range(5, 5) == 5);
}
