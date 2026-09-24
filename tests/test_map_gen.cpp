#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>

#include <doctest/doctest.h>

#include "sim/map_gen.hpp"
#include "sim/state_hash.hpp"
#include "test_helpers.hpp"

using rts::sim::generate_map;
using rts::sim::StateHasher;
using rts::sim::TileCoord;
using rts::sim::TileMap;
using rts::test::test_map_params;

namespace {

std::uint64_t map_hash(const TileMap& map) {
    StateHasher h;
    map.hash_into(h);
    return h.value();
}

}  // namespace

TEST_CASE("Generación de mapa: misma semilla, mismo mapa; otra semilla, otro") {
    const TileMap a = generate_map(test_map_params());
    const TileMap b = generate_map(test_map_params());
    auto params = test_map_params();
    params.seed += 1;
    const TileMap c = generate_map(params);
    CHECK(map_hash(a) == map_hash(b));
    CHECK(map_hash(a) != map_hash(c));
}

TEST_CASE("Generación de mapa: dimensiones y rangos de las capas") {
    const auto params = test_map_params();
    const TileMap map = generate_map(params);
    CHECK(map.width() == params.width);
    CHECK(map.height() == params.height);
    CHECK(map.terrain_layer().size() == 65'536);
    for (const auto level : map.elevation_layer()) {
        REQUIRE(level < params.elevation_levels);
    }
    for (const auto id : map.terrain_layer()) {
        REQUIRE(id <= 2);
    }
}

TEST_CASE("Generación de mapa: aparecen todas las bandas y el terreno sigue a la altura") {
    const TileMap map = generate_map(test_map_params());
    std::array<int, 3> counts{};
    for (const auto id : map.terrain_layer()) {
        ++counts[id];
    }
    // Con 65 536 casillas y bandas anchas, las tres aparecen en cantidad apreciable.
    for (const int n : counts) {
        CHECK(n > 1'000);
    }
    // Ambos derivan de la misma elevación: a más nivel, banda igual o superior.
    std::array<int, 8> max_band_at_level{};
    std::array<int, 8> min_band_at_level{3, 3, 3, 3, 3, 3, 3, 3};
    for (std::int32_t y = 0; y < map.height(); ++y) {
        for (std::int32_t x = 0; x < map.width(); ++x) {
            const auto level = map.elevation({x, y});
            const auto band = static_cast<int>(map.terrain({x, y}));
            max_band_at_level[level] = std::max(max_band_at_level[level], band);
            min_band_at_level[level] = std::min(min_band_at_level[level], band);
        }
    }
    for (std::size_t l = 1; l < max_band_at_level.size(); ++l) {
        if (min_band_at_level[l] <= 2 && max_band_at_level[l - 1] >= 0 && min_band_at_level[l - 1] <= 2) {
            CHECK(min_band_at_level[l] >= min_band_at_level[l - 1]);
        }
    }
}

TEST_CASE("Generación de mapa: el ruido es continuo (sin saltos bruscos entre vecinas)") {
    const TileMap map = generate_map(test_map_params());
    // Con retícula de 32 casillas y 8 niveles, dos vecinas nunca difieren en más de 1 nivel.
    int max_step = 0;
    for (std::int32_t y = 0; y < map.height(); ++y) {
        for (std::int32_t x = 1; x < map.width(); ++x) {
            const int step = std::abs(map.elevation({x, y}) - map.elevation({x - 1, y}));
            max_step = std::max(max_step, step);
        }
    }
    CHECK(max_step <= 1);
}

TEST_CASE("TileMap: la revisión crece con cada escritura") {
    TileMap map(4, 4);
    const auto r0 = map.revision();
    map.set_terrain(TileCoord{1, 2}, 3);
    CHECK(map.revision() > r0);
    CHECK(map.terrain({1, 2}) == 3);
    CHECK(map.contains({3, 3}));
    CHECK_FALSE(map.contains({4, 0}));
    CHECK_FALSE(map.contains({0, -1}));
}
