// Pruebas de la niebla de guerra sobre VisionSystem con un mapa a medida: alcance de
// la vista, árboles que tapan y esconden, altura (más vista desde arriba, lomas que
// tapan), noche, lo explorado que se queda y la memoria de edificios enemigos.

#include <cstdint>
#include <vector>

#include <doctest/doctest.h>

#include "sim/vision.hpp"
#include "test_helpers.hpp"

using rts::sim::Footprint;
using rts::sim::Owner;
using rts::sim::Position;
using rts::sim::TileCoord;
using rts::sim::TileMap;
using rts::sim::VisionParams;
using rts::sim::VisionSystem;
using namespace rts::test;

namespace {

constexpr std::int32_t kSize = 64;
constexpr std::int32_t kSight = 6;  // vista del soldado de prueba
constexpr rts::sim::NodeTypeId kTreeType = 0;

VisionParams params() {
    VisionParams p;
    p.enabled = true;
    p.interval_ticks = 1;
    p.elevation_sight_per_level = 1;
    p.cover_depth_tiles = 2;
    p.spot_in_cover_tiles = 2;
    p.day_ticks = 1000;
    p.night_ticks = 1000;
    p.twilight_ticks = 0;
    p.night_sight_percent = 50;
    p.start_tick = 0;  // de día en el tick 0
    return p;
}

struct Fixture {
    TileMap map{kSize, kSize};
    entt::registry registry;
    std::vector<rts::sim::UnitType> units = test_unit_types();
    std::vector<rts::sim::BuildingType> buildings = test_building_types();
    std::vector<rts::sim::ResourceNodeType> nodes{{rts::sim::Resource::Wood, 100, 1, true}};

    Fixture() { units[kSoldier].combat.sight_tiles = kSight; }

    VisionSystem vision(const VisionParams& p = params()) { return {p, map, units, buildings, nodes, 2}; }

    entt::entity unit(rts::sim::PlayerId owner, TileCoord t) {
        const auto e = registry.create();
        registry.emplace<rts::sim::Unit>(e, kSoldier, units[kSoldier].radius, units[kSoldier].speed);
        registry.emplace<Position>(e, rts::sim::Fixed::from_int(t.x) + rts::sim::Fixed::from_ratio(1, 2),
                                   rts::sim::Fixed::from_int(t.y) + rts::sim::Fixed::from_ratio(1, 2));
        registry.emplace<Owner>(e, owner);
        return e;
    }

    void tree(TileCoord t) {
        const auto e = registry.create();
        registry.emplace<Footprint>(e, t, 1);
        registry.emplace<rts::sim::ResourceNode>(e, kTreeType, rts::sim::Resource::Wood, 100);
    }

    entt::entity building(rts::sim::PlayerId owner, TileCoord origin) {
        const auto e = registry.create();
        registry.emplace<Footprint>(e, origin, 2);
        rts::sim::Building b;
        b.type = kHouse;
        b.complete = true;
        registry.emplace<rts::sim::Building>(e, b);
        registry.emplace<Owner>(e, owner);
        registry.emplace<rts::sim::Health>(e, 500, 500);
        return e;
    }
};

}  // namespace

TEST_CASE("Visión: sin niebla todo se ve; con ella, solo dentro del alcance") {
    Fixture f;
    f.unit(0, {20, 20});
    VisionSystem off = f.vision(VisionParams{});
    off.update(f.registry, 0, true);
    CHECK(off.visible(0, {60, 60}));
    VisionSystem v = f.vision();
    v.update(f.registry, 0, true);
    CHECK(v.visible(0, {20, 20}));
    CHECK(v.visible(0, {20 + kSight, 20}));
    CHECK_FALSE(v.visible(0, {20 + kSight + 1, 20}));
    CHECK_FALSE(v.visible(1, {20, 20}));  // el otro jugador no ve nada
    CHECK_FALSE(v.explored(0, {40, 40}));
}

TEST_CASE("Visión: lo explorado se queda al irse; lo visible, no") {
    Fixture f;
    const auto e = f.unit(0, {20, 20});
    VisionSystem v = f.vision();
    v.update(f.registry, 0, true);
    f.registry.replace<Position>(e, rts::sim::Fixed::from_int(50), rts::sim::Fixed::from_int(50));
    v.update(f.registry, 1, true);
    CHECK_FALSE(v.visible(0, {20, 20}));
    CHECK(v.explored(0, {20, 20}));
    CHECK(v.visible(0, {50, 50}));
}

TEST_CASE("Visión: los árboles dejan ver un par de casillas de bosque y esconden a quien está dentro") {
    Fixture f;
    f.unit(0, {10, 20});
    for (std::int32_t x = 12; x <= 16; ++x) {
        f.tree({x, 20});
    }
    VisionSystem v = f.vision();
    v.update(f.registry, 0, true);
    CHECK(v.visible(0, {12, 20}));
    CHECK(v.visible(0, {14, 20}));        // 2 casillas de bosque por delante
    CHECK_FALSE(v.visible(0, {15, 20}));  // 3: ya no
    // Una unidad enemiga entre árboles a 4 casillas no se ve; a 2, sí.
    const auto far = f.unit(1, {14, 20});
    const auto close = f.unit(1, {12, 20});
    CHECK_FALSE(v.sees_unit(f.registry, 0, far));
    CHECK(v.sees_unit(f.registry, 0, close));
    // Talar el bosque abre la vista.
    std::vector<entt::entity> trees;
    for (const auto e : f.registry.view<rts::sim::ResourceNode>()) {
        trees.push_back(e);
    }
    f.registry.destroy(trees.begin(), trees.end());
    v.update(f.registry, 1, true);
    CHECK(v.visible(0, {15, 20}));
}

TEST_CASE("Visión: desde arriba se ve más lejos y una loma tapa lo que hay detrás") {
    Fixture f;
    // Observador en una loma de nivel 3: ve 3 casillas más cuesta abajo.
    f.map.set_elevation({20, 20}, 3);
    f.unit(0, {20, 20});
    VisionSystem v = f.vision();
    v.update(f.registry, 0, true);
    CHECK(v.visible(0, {20 + kSight + 3, 20}));
    CHECK_FALSE(v.visible(0, {20 + kSight + 4, 20}));
    // Una cresta más alta que ambos extremos tapa lo de detrás.
    Fixture g;
    g.unit(0, {20, 20});
    g.map.set_elevation({22, 20}, 2);
    VisionSystem w = g.vision();
    w.update(g.registry, 0, true);
    CHECK(w.visible(0, {22, 20}));         // la cresta misma se ve
    CHECK_FALSE(w.visible(0, {24, 20}));   // detrás, no
}

TEST_CASE("Visión: de noche la vista baja a la mitad, con amanecer gradual") {
    Fixture f;
    f.unit(0, {20, 20});
    VisionParams p = params();
    p.twilight_ticks = 100;
    VisionSystem v = f.vision(p);
    CHECK(v.daylight_percent(500) == 100);
    CHECK(v.daylight_percent(1500) == 50);
    CHECK(v.daylight_percent(50) == 75);   // amanecer a medias
    CHECK(v.daylight_percent(950) == 75);  // anochecer a medias
    v.update(f.registry, 1500, true);
    CHECK(v.visible(0, {20 + kSight / 2, 20}));
    CHECK_FALSE(v.visible(0, {20 + kSight / 2 + 1, 20}));
}

TEST_CASE("Visión: un edificio enemigo se recuerda como se vio, aunque caiga sin verlo") {
    Fixture f;
    const auto scout = f.unit(0, {20, 20});
    const auto house = f.building(1, {23, 20});
    VisionSystem v = f.vision();
    v.update(f.registry, 0, true);
    REQUIRE(v.memory(0).size() == 1);
    CHECK(v.memory(0)[0].entity == house);
    // El explorador se va y la casa cae: se sigue recordando.
    f.registry.replace<Position>(scout, rts::sim::Fixed::from_int(55), rts::sim::Fixed::from_int(55));
    f.registry.destroy(house);
    v.update(f.registry, 1, true);
    CHECK(v.memory(0).size() == 1);
    // Al volver a ver el sitio vacío, se olvida.
    f.registry.replace<Position>(scout, rts::sim::Fixed::from_int(20), rts::sim::Fixed::from_int(20));
    v.update(f.registry, 2, true);
    CHECK(v.memory(0).empty());
}
