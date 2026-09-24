#include <array>
#include <vector>

#include <doctest/doctest.h>

#include "render/atlas.hpp"
#include "render/scene.hpp"
#include "sim/map_gen.hpp"
#include "test_helpers.hpp"

using rts::render::Atlas;
using rts::render::AtlasRegion;
using rts::render::Camera;
using rts::render::Marker;
using rts::render::Scene;
using rts::render::SceneBuilder;
using rts::render::SpriteBatch;
using rts::render::Vec2;
using rts::render::ViewParams;

namespace {

ViewParams test_view() {
    ViewParams v;
    v.tile_width_px = 64;
    v.tile_height_px = 32;
    v.marker_radius_px = 5;
    v.elevation_shade_min_percent = 70;
    v.hillshade_step_percent = 0;
    v.debug_overlay_color = {0, 0, 255, 100};
    v.marker_selected_color = {0, 255, 0, 255};
    v.selection_rect_color = {0, 255, 0, 64};
    v.hover_tile_color = {255, 255, 255, 64};
    return v;
}

bool overlaps(const AtlasRegion& a, const AtlasRegion& b) {
    return a.x_px < b.x_px + b.width_px && b.x_px < a.x_px + a.width_px && a.y_px < b.y_px + b.height_px &&
           b.y_px < a.y_px + a.height_px;
}

const std::vector<std::array<std::uint8_t, 3>> kColors{{0, 0, 200}, {0, 200, 0}, {200, 200, 200}};

}  // namespace

TEST_CASE("Atlas: regiones dentro del atlas, sin solaparse, con la forma esperada") {
    const Atlas atlas = rts::render::build_atlas(test_view());
    CHECK(atlas.width == 128);  // potencia de dos mínima para 64 + 2x12 + 4 + relleno
    const std::array<AtlasRegion, 4> regions{atlas.tile_diamond, atlas.marker_disc, atlas.marker_ring, atlas.solid};
    for (std::size_t i = 0; i < regions.size(); ++i) {
        CHECK(regions[i].x_px + regions[i].width_px <= atlas.width);
        CHECK(regions[i].y_px + regions[i].height_px <= atlas.height);
        for (std::size_t k = i + 1; k < regions.size(); ++k) {
            CHECK_FALSE(overlaps(regions[i], regions[k]));
        }
    }
    const AtlasRegion& d = atlas.tile_diamond;
    // Centro del rombo opaco y blanco; esquinas del rectángulo transparentes.
    CHECK(atlas.texel(d.x_px + 32, d.y_px + 16)[3] == 255);
    CHECK(atlas.texel(d.x_px + 32, d.y_px + 16)[0] == 255);
    CHECK(atlas.texel(d.x_px, d.y_px)[3] == 0);
    CHECK(atlas.texel(d.x_px + 63, d.y_px + 31)[3] == 0);
    // El bloque sólido es opaco en toda su región utilizable.
    const AtlasRegion& s = atlas.solid;
    CHECK(atlas.texel(s.x_px, s.y_px)[3] == 255);
    CHECK(atlas.texel(s.x_px + s.width_px - 1, s.y_px + s.height_px - 1)[3] == 255);
}

TEST_CASE("Escena: casillas visibles + marcadores + anillos + rectángulo = instancias") {
    const auto map_params = rts::test::test_map_params();
    const rts::sim::TileMap map = rts::sim::generate_map(map_params);
    const Atlas atlas = rts::render::build_atlas(test_view());
    SceneBuilder builder(test_view(), atlas, kColors);

    const std::vector<Marker> markers{
        {{100.0f, 100.0f}, false},
        {{200.0f, 150.0f}, true},
        {{-500.0f, 100.0f}, false},  // fuera de pantalla: no se dibuja
    };
    Scene scene;
    scene.map = &map;
    scene.screen = {1280.0f, 720.0f};
    const rts::render::IsoProjection proj(test_view());
    scene.camera.center_on(proj.tile_to_world({128.0f, 128.0f}), scene.screen);
    scene.markers = markers;
    scene.drag_rect = rts::render::Rect{{10.0f, 10.0f}, {50.0f, 40.0f}};
    scene.hovered_tile = rts::sim::TileCoord{128, 128};

    SpriteBatch batch;
    const auto stats = builder.build(scene, batch);
    CHECK(stats.markers_drawn == 2);
    CHECK(stats.tiles_drawn > 800);
    // casillas + casilla bajo el cursor + 2 discos + 1 anillo + relleno y 4 bordes.
    CHECK(batch.size() == static_cast<std::size_t>(stats.tiles_drawn) + 1 + 2 + 1 + 5);
}

TEST_CASE("Escena: a igual terreno, la casilla más alta es más clara") {
    rts::sim::TileMap map(2, 1);
    map.set_terrain({0, 0}, 1);
    map.set_terrain({1, 0}, 1);
    map.set_elevation({0, 0}, 0);
    map.set_elevation({1, 0}, 7);
    const Atlas atlas = rts::render::build_atlas(test_view());
    SceneBuilder builder(test_view(), atlas, kColors);
    const auto low = builder.tile_color(map, {0, 0});
    const auto high = builder.tile_color(map, {1, 0});
    CHECK(high[1] == 200);             // nivel máximo: 100 % del color base
    CHECK(low[1] == 200 * 70 / 100);   // nivel 0: elevation_shade_min_percent
    // El caché se invalida al cambiar el mapa.
    map.set_elevation({0, 0}, 7);
    CHECK(builder.tile_color(map, {0, 0})[1] == 200);
}
