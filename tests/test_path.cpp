#include <algorithm>
#include <cstdint>
#include <vector>

#include <doctest/doctest.h>

#include "sim/fmath.hpp"
#include "sim/map_gen.hpp"
#include "sim/path/flow_field.hpp"
#include "sim/path/grid.hpp"
#include "sim/path/hpa.hpp"
#include "sim/rng.hpp"
#include "test_helpers.hpp"

using rts::sim::FlowField;
using rts::sim::GridSearch;
using rts::sim::HpaGraph;
using rts::sim::HpaParams;
using rts::sim::kUnreached;
using rts::sim::PassGrid;
using rts::sim::SearchRect;
using rts::sim::SectorMask;
using rts::sim::TileCoord;
using rts::sim::TileMap;
using rts::sim::Xoshiro256pp;

namespace {

// Mapa aleatorio con obstáculos sueltos: terreno 0 transitable, 1 bloqueado.
TileMap random_obstacles(std::int32_t size, std::uint32_t percent_blocked, std::uint64_t seed) {
    TileMap map(size, size);
    Xoshiro256pp rng(seed);
    for (std::int32_t y = 0; y < size; ++y) {
        for (std::int32_t x = 0; x < size; ++x) {
            map.set_terrain({x, y}, rng.next_below(100) < percent_blocked ? 1 : 0);
        }
    }
    return map;
}

const std::vector<std::uint8_t> kPassable01{1, 0};
// En los mapas generados de las pruebas: 0 = agua (bloquea), 1 y 2 transitables.
const std::vector<std::uint8_t> kPassableGen{0, 1, 1};

TileCoord random_passable(const PassGrid& g, Xoshiro256pp& rng) {
    while (true) {
        const TileCoord c{static_cast<std::int32_t>(rng.next_below(static_cast<std::uint32_t>(g.width()))),
                          static_cast<std::int32_t>(rng.next_below(static_cast<std::uint32_t>(g.height())))};
        if (g.passable(c)) {
            return c;
        }
    }
}

std::int32_t path_cost(TileCoord start, const std::vector<TileCoord>& path) {
    std::int32_t cost = 0;
    TileCoord prev = start;
    for (const TileCoord& c : path) {
        const bool diagonal = c.x != prev.x && c.y != prev.y;
        cost += diagonal ? rts::sim::kDiagonalCost : rts::sim::kStraightCost;
        prev = c;
    }
    return cost;
}

}  // namespace

TEST_CASE("isqrt: exacta para todo n < 10^6 y en los extremos de 64 bits") {
    for (std::uint64_t n = 0; n < 1'000'000; ++n) {
        const std::uint64_t r = rts::sim::isqrt(n);
        REQUIRE(r * r <= n);
        REQUIRE((r + 1) * (r + 1) > n);
    }
    CHECK(rts::sim::isqrt(UINT64_MAX) == 0xFFFFFFFFULL);
    CHECK(rts::sim::isqrt(std::uint64_t{1} << 62) == std::uint64_t{1} << 31);
    CHECK(rts::sim::isqrt((std::uint64_t{1} << 62) - 1) == (std::uint64_t{1} << 31) - 1);
}

TEST_CASE("FVec2: longitud y reescalado") {
    using rts::sim::Fixed;
    const rts::sim::FVec2 v{Fixed::from_int(3), Fixed::from_int(4)};
    CHECK(rts::sim::length(v) == Fixed::from_int(5));
    const auto u = rts::sim::with_length(v, Fixed::from_int(10));
    CHECK(u.x == Fixed::from_int(6));
    CHECK(u.y == Fixed::from_int(8));
    CHECK(rts::sim::clamp_length(v, Fixed::from_int(10)) == v);
}

TEST_CASE("Rejilla: sin atajar esquinas y componentes conexas") {
    // . # .
    // # . .
    TileMap map(3, 2);
    map.set_terrain({1, 0}, 1);
    map.set_terrain({0, 1}, 1);
    const PassGrid g(map, kPassable01);
    // De (0,0) a (1,1) en diagonal rozaría dos muros: prohibido.
    CHECK_FALSE(g.can_step({0, 0}, {1, 1, rts::sim::kDiagonalCost}));
    CHECK(g.component({0, 0}) != g.component({1, 1}));
    CHECK(g.component({1, 1}) == g.component({2, 0}));
    CHECK(g.component({1, 0}) == 0);
    const auto near = g.nearest_in_component({0, 1}, g.component({2, 1}), 4);
    REQUIRE(near.has_value());
    CHECK(*near == TileCoord{1, 1});
}

TEST_CASE("A*: coste óptimo (igual que Dijkstra) en 300 consultas sobre mapas con obstáculos") {
    for (std::uint64_t seed = 1; seed <= 3; ++seed) {
        const TileMap map = random_obstacles(64, 28, seed);
        const PassGrid g(map, kPassable01);
        GridSearch search(64, 64);
        GridSearch reference(64, 64);
        Xoshiro256pp rng(seed * 77);
        const SearchRect whole{0, 0, 64, 64};
        for (int q = 0; q < 100; ++q) {
            const TileCoord a = random_passable(g, rng);
            const TileCoord b = random_passable(g, rng);
            std::vector<TileCoord> path;
            const auto cost = search.astar(g, a, b, whole, &path);
            reference.dijkstra(g, a, whole);
            const std::int32_t expected = reference.cost_at(g, b);
            if (expected == kUnreached) {
                REQUIRE_FALSE(cost.has_value());
                REQUIRE(g.component(a) != g.component(b));
                continue;
            }
            REQUIRE(cost.has_value());
            REQUIRE(*cost == expected);
            REQUIRE(path_cost(a, path) == expected);
            REQUIRE(path.back() == b);
        }
    }
}

TEST_CASE("HPA*: camino válido y como mucho un 10 % más largo que el óptimo") {
    auto params = rts::test::test_map_params();
    params.width = 128;
    params.height = 128;
    const TileMap map = rts::sim::generate_map(params);
    const PassGrid g(map, kPassableGen);
    GridSearch search(128, 128);
    GridSearch reference(128, 128);
    HpaGraph hpa(g, HpaParams{16, 8}, search);
    MESSAGE("HPA* en 128x128: " << hpa.node_count() << " nodos, " << hpa.edge_count() << " aristas");
    CHECK(hpa.node_count() > 0);

    Xoshiro256pp rng(99);
    const SearchRect whole{0, 0, 128, 128};
    double worst_ratio = 1.0;
    double sum_ratio = 0.0;
    int solved = 0;
    for (int q = 0; q < 200; ++q) {
        const TileCoord a = random_passable(g, rng);
        const TileCoord b = random_passable(g, rng);
        const auto waypoints = hpa.find_waypoints(g, a, b, search);
        const auto optimal = reference.astar(g, a, b, whole, nullptr);
        REQUIRE(waypoints.has_value() == optimal.has_value());
        if (!optimal) {
            continue;
        }
        // Refinado tramo a tramo: debe existir siempre y encadenar hasta b.
        std::int32_t total = 0;
        TileCoord from = a;
        std::vector<TileCoord> segment;
        for (const TileCoord& w : *waypoints) {
            const auto c = hpa.refine(g, from, w, search, segment);
            REQUIRE(c.has_value());
            total += *c;
            from = w;
        }
        REQUIRE(from == b);
        const double ratio = *optimal == 0 ? 1.0 : static_cast<double>(total) / static_cast<double>(*optimal);
        worst_ratio = std::max(worst_ratio, ratio);
        sum_ratio += ratio;
        ++solved;
    }
    MESSAGE("HPA*: " << solved << " rutas, exceso medio " << (sum_ratio / solved - 1.0) * 100.0 << " %, peor "
                     << (worst_ratio - 1.0) * 100.0 << " %");
    CHECK(solved > 100);
    CHECK(sum_ratio / solved <= 1.10);
}

TEST_CASE("Campo de flujo: desde cualquier casilla alcanzada se baja hasta el destino") {
    const TileMap map = random_obstacles(64, 20, 5);
    const PassGrid g(map, kPassable01);
    GridSearch search(64, 64);
    const std::vector<std::uint8_t> all_sectors(16, 1);  // 4x4 sectores de 16
    const SectorMask mask{all_sectors, 16, 4};
    Xoshiro256pp rng(3);
    const TileCoord goal = random_passable(g, rng);
    const FlowField field(g, goal, mask, search);
    CHECK(field.cost(g, goal) == 0);
    std::size_t checked = 0;
    for (std::int32_t y = 0; y < 64; ++y) {
        for (std::int32_t x = 0; x < 64; ++x) {
            TileCoord c{x, y};
            const std::int32_t start_cost = field.cost(g, c);
            if (start_cost == kUnreached) {
                continue;
            }
            ++checked;
            int steps = 0;
            while (!(c == goal)) {
                const auto next = field.next_step(g, c);
                REQUIRE(next.has_value());
                REQUIRE(field.cost(g, *next) < field.cost(g, c));
                c = *next;
                REQUIRE(++steps <= start_cost / rts::sim::kStraightCost + 1);
            }
        }
    }
    CHECK(checked == field.tiles_reached());
    CHECK(checked > 2000);
}

TEST_CASE("Campo de flujo: la máscara de sectores limita la búsqueda") {
    TileMap map(64, 64);
    const PassGrid g(map, kPassable01);
    GridSearch search(64, 64);
    std::vector<std::uint8_t> sectors(16, 0);
    sectors[0] = 1;  // solo el sector (0,0)
    sectors[1] = 1;  // y su vecino (1,0)
    const FlowField field(g, {2, 2}, SectorMask{sectors, 16, 4}, search);
    CHECK(field.tiles_reached() == 2u * 16u * 16u);
    CHECK(field.cost(g, {40, 40}) == kUnreached);
    CHECK(field.cost(g, {20, 5}) != kUnreached);
}

namespace {

// Terreno con pesos: 0 llano, 1 bloquea, 2 bosque (60 %), 3 colinas (75 %), 4 vado (45 %).
const std::vector<std::uint8_t> kPassableWeighted{1, 0, 1, 1, 1};
const std::vector<std::int32_t> kSpeedWeighted{100, 100, 60, 75, 45};
constexpr std::int32_t kRoadPercent = 125;

TileMap random_weighted(std::int32_t size, std::uint64_t seed) {
    TileMap map(size, size);
    Xoshiro256pp rng(seed);
    for (std::int32_t y = 0; y < size; ++y) {
        for (std::int32_t x = 0; x < size; ++x) {
            const std::uint32_t r = rng.next_below(100);
            map.set_terrain({x, y}, r < 15 ? 1 : (r < 35 ? 2 : (r < 50 ? 3 : (r < 55 ? 4 : 0))));
        }
    }
    return map;
}

// Unos tramos de camino al azar (horizontales y verticales) sobre casillas transitables.
void random_roads(PassGrid& g, Xoshiro256pp& rng, int count) {
    for (int k = 0; k < count; ++k) {
        TileCoord c = random_passable(g, rng);
        const bool horizontal = rng.next_below(2) == 0;
        for (int i = 0; i < 12 && g.contains(c); ++i) {
            g.set_speed_override(c, kRoadPercent);
            c = horizontal ? TileCoord{c.x + 1, c.y} : TileCoord{c.x, c.y + 1};
        }
    }
    g.refresh_heuristic();
}

std::int32_t weighted_cost(const PassGrid& g, TileCoord start, const std::vector<TileCoord>& path) {
    std::int32_t cost = 0;
    TileCoord prev = start;
    for (const TileCoord& c : path) {
        const rts::sim::Direction d{c.x - prev.x, c.y - prev.y,
                                    c.x != prev.x && c.y != prev.y ? rts::sim::kDiagonalCost : rts::sim::kStraightCost};
        REQUIRE(g.can_step(prev, d));
        cost += g.step_cost(prev, d);
        prev = c;
    }
    return cost;
}

}  // namespace

TEST_CASE("Pesos: llano 10/14 como antes; camino, bosque y vado en proporción a su velocidad") {
    using rts::sim::kDiagonalCost;
    using rts::sim::kStraightCost;
    using rts::sim::tile_weight;
    using rts::sim::weighted_step;
    const std::int32_t plain = tile_weight(100);
    CHECK(plain == rts::sim::kWeightUnit);
    CHECK(weighted_step(kStraightCost, plain, plain) == kStraightCost);
    CHECK(weighted_step(kDiagonalCost, plain, plain) == kDiagonalCost);
    CHECK(weighted_step(kStraightCost, tile_weight(kRoadPercent), tile_weight(kRoadPercent)) == 8);
    CHECK(weighted_step(kStraightCost, tile_weight(60), tile_weight(60)) == 17);
    CHECK(weighted_step(kStraightCost, tile_weight(45), tile_weight(45)) == 22);
    CHECK(weighted_step(kStraightCost, plain, tile_weight(60)) == 13);  // la media de las dos
}

TEST_CASE("A* con pesos: coste óptimo (igual que Dijkstra) y heurística consistente") {
    for (std::uint64_t seed = 1; seed <= 3; ++seed) {
        const TileMap map = random_weighted(64, seed);
        PassGrid g(map, kPassableWeighted, kSpeedWeighted);
        Xoshiro256pp rng(seed * 31);
        if (seed != 1) {
            random_roads(g, rng, 10);  // con caminos la heurística baja al peso del camino
        }
        GridSearch search(64, 64);
        GridSearch reference(64, 64);
        const SearchRect whole{0, 0, 64, 64};
        for (int q = 0; q < 100; ++q) {
            const TileCoord a = random_passable(g, rng);
            const TileCoord b = random_passable(g, rng);
            // Consistencia: ningún paso baja la heurística más de lo que cuesta.
            for (const auto& d : rts::sim::kDirections) {
                if (g.can_step(a, d)) {
                    REQUIRE(g.heuristic(a, b) <= g.step_cost(a, d) + g.heuristic({a.x + d.dx, a.y + d.dy}, b));
                }
            }
            std::vector<TileCoord> path;
            const auto cost = search.astar(g, a, b, whole, &path);
            reference.dijkstra(g, a, whole);
            const std::int32_t expected = reference.cost_at(g, b);
            if (expected == kUnreached) {
                REQUIRE_FALSE(cost.has_value());
                continue;
            }
            REQUIRE(cost.has_value());
            REQUIRE(*cost == expected);
            REQUIRE(weighted_cost(g, a, path) == expected);
            // Simétrico: de vuelta cuesta lo mismo (los campos de flujo cuentan con ello).
            REQUIRE(search.astar(g, b, a, whole, nullptr) == expected);
        }
    }
}

TEST_CASE("A* con pesos: rodea el bosque si llega antes y aprovecha el camino") {
    // 20x9 de llano con un bosque en las columnas 5..14 y filas 1..7: por dentro son 10
    // casillas al 60 %; por la fila 0 se anda más pero se llega antes.
    TileMap map(20, 9);
    for (std::int32_t y = 1; y <= 7; ++y) {
        for (std::int32_t x = 5; x <= 14; ++x) {
            map.set_terrain({x, y}, 2);
        }
    }
    GridSearch search(20, 9);
    const SearchRect whole{0, 0, 20, 9};
    std::vector<TileCoord> path;
    const auto crosses_forest = [&] {
        return std::ranges::any_of(path, [&](TileCoord c) { return map.terrain(c) == 2; });
    };
    {
        const PassGrid flat(map, kPassableWeighted);  // sin pesos: todo es llano
        REQUIRE(search.astar(flat, {0, 4}, {19, 4}, whole, &path));
        CHECK(crosses_forest());
    }
    PassGrid g(map, kPassableWeighted, kSpeedWeighted);
    const auto around = search.astar(g, {0, 4}, {19, 4}, whole, &path);
    REQUIRE(around);
    CHECK_FALSE(crosses_forest());
    // Un camino que cruza el bosque por la fila 4 vuelve a hacer rentable atravesarlo.
    for (std::int32_t x = 4; x <= 15; ++x) {
        g.set_speed_override({x, 4}, kRoadPercent);
    }
    g.refresh_heuristic();
    const auto by_road = search.astar(g, {0, 4}, {19, 4}, whole, &path);
    REQUIRE(by_road);
    CHECK(*by_road < *around);
    CHECK(std::ranges::count_if(path, [](TileCoord c) { return c.y == 4 && c.x >= 4 && c.x <= 15; }) == 12);
    // Quitar el camino devuelve el peso del terreno.
    for (std::int32_t x = 4; x <= 15; ++x) {
        g.set_speed_override({x, 4}, 0);
    }
    g.refresh_heuristic();
    CHECK(search.astar(g, {0, 4}, {19, 4}, whole, nullptr) == around);
}

TEST_CASE("HPA* con pesos: cerca del óptimo, e incremental igual que desde cero al poner caminos") {
    const TileMap map = random_weighted(128, 11);
    PassGrid g(map, kPassableWeighted, kSpeedWeighted);
    GridSearch search(128, 128);
    GridSearch reference(128, 128);
    HpaGraph hpa(g, HpaParams{16, 8}, search);

    // Caminos en algunos sectores: rebuild solo de los sucios = grafo construido de cero.
    Xoshiro256pp rng(5);
    std::vector<std::uint8_t> dirty(hpa.sector_count(), 0);
    for (int k = 0; k < 6; ++k) {
        TileCoord c = random_passable(g, rng);
        for (int i = 0; i < 20 && g.contains(c); ++i, ++c.x) {
            g.set_speed_override(c, kRoadPercent);
            dirty[static_cast<std::size_t>(hpa.sector_of(c))] = 1;
        }
    }
    g.refresh_heuristic();
    const std::int32_t recomputed = hpa.rebuild(g, dirty, search);
    CHECK(recomputed < static_cast<std::int32_t>(hpa.sector_count()));
    GridSearch fresh_search(128, 128);
    const HpaGraph fresh(g, HpaParams{16, 8}, fresh_search);
    CHECK(hpa.same_graph(fresh));

    const SearchRect whole{0, 0, 128, 128};
    double sum_ratio = 0.0;
    int solved = 0;
    for (int q = 0; q < 200; ++q) {
        const TileCoord a = random_passable(g, rng);
        const TileCoord b = random_passable(g, rng);
        const auto waypoints = hpa.find_waypoints(g, a, b, search);
        const auto optimal = reference.astar(g, a, b, whole, nullptr);
        REQUIRE(waypoints.has_value() == optimal.has_value());
        if (!optimal || *optimal == 0) {
            continue;
        }
        std::int32_t total = 0;
        TileCoord from = a;
        std::vector<TileCoord> segment;
        for (const TileCoord& w : *waypoints) {
            const auto c = hpa.refine(g, from, w, search, segment);
            REQUIRE(c.has_value());
            REQUIRE(weighted_cost(g, from, segment) == *c);
            total += *c;
            from = w;
        }
        REQUIRE(from == b);
        sum_ratio += static_cast<double>(total) / static_cast<double>(*optimal);
        ++solved;
    }
    MESSAGE("HPA* con pesos: " << solved << " rutas, exceso medio " << (sum_ratio / solved - 1.0) * 100.0 << " %");
    CHECK(solved > 100);
    CHECK(sum_ratio / solved <= 1.10);
}

TEST_CASE("Campo de flujo con pesos: bajando por él se paga exactamente su coste") {
    const TileMap map = random_weighted(64, 9);
    PassGrid g(map, kPassableWeighted, kSpeedWeighted);
    Xoshiro256pp rng(4);
    random_roads(g, rng, 8);
    GridSearch search(64, 64);
    const std::vector<std::uint8_t> all_sectors(16, 1);
    const FlowField field(g, random_passable(g, rng), SectorMask{all_sectors, 16, 4}, search);
    std::size_t checked = 0;
    for (std::int32_t y = 0; y < 64; ++y) {
        for (std::int32_t x = 0; x < 64; ++x) {
            TileCoord c{x, y};
            const std::int32_t start_cost = field.cost(g, c);
            if (start_cost == kUnreached) {
                continue;
            }
            ++checked;
            std::int32_t paid = 0;
            while (!(c == field.goal())) {
                const auto next = field.next_step(g, c);
                REQUIRE(next.has_value());
                const rts::sim::Direction d{next->x - c.x, next->y - c.y,
                                            next->x != c.x && next->y != c.y ? rts::sim::kDiagonalCost
                                                                             : rts::sim::kStraightCost};
                paid += g.step_cost(c, d);
                c = *next;
            }
            REQUIRE(paid == start_cost);
        }
    }
    CHECK(checked == field.tiles_reached());
    CHECK(checked > 1500);
}
