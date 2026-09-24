#include <string>

#include <doctest/doctest.h>

#include "game/config.hpp"

using rts::game::load_game_data;
using rts::game::parse_engine_config;
using rts::game::parse_terrain_catalog;
using rts::game::TerrainCatalog;
using rts::sim::Fixed;

namespace {

constexpr const char* kTerrain = R"(
[[terrain]]
name = "agua"
passable = false
color = [0, 0, 200]

[[terrain]]
name = "llanura"
passable = true
color = [0, 200, 0]
)";

constexpr const char* kEngine = R"(
[window]
title = "prueba"
width = 800
height = 600
vsync = false

[loop]
max_ticks_per_frame = 5

[map]
width_tiles = 64
height_tiles = 32
seed = 7
noise_cell_tiles = 16
elevation_levels = 4

[[map.bands]]
terrain = "agua"
max_elevation = 30000

[[map.bands]]
terrain = "llanura"
max_elevation = 65536

[demo]
seed = 1
point_count = 10
area_tiles = 16
max_speed_milli_tiles_per_tick = 250

[view]
tile_width_px = 64
tile_height_px = 32
marker_radius_px = 5
elevation_shade_min_percent = 70
hillshade_step_percent = 8
clear_color = [1, 2, 3, 255]
marker_color = [4, 5, 6, 255]
marker_selected_color = [7, 8, 9, 255]
selection_rect_color = [10, 11, 12, 48]
hover_tile_color = [13, 14, 15, 56]

[camera]
scroll_keys_px_per_s = 1000
scroll_edge_px_per_s = 900
edge_margin_px = 6

[selection]
drag_threshold_px = 4
click_radius_px = 10
)";

TerrainCatalog catalog() {
    auto c = parse_terrain_catalog(kTerrain);
    REQUIRE(c.has_value());
    return *c;
}

std::string replaced(std::string text, const std::string& from, const std::string& to) {
    const auto pos = text.find(from);
    REQUIRE(pos != std::string::npos);
    text.replace(pos, from.size(), to);
    return text;
}

}  // namespace

TEST_CASE("Terrenos: el catálogo asigna ids por orden y resuelve nombres") {
    const TerrainCatalog c = catalog();
    REQUIRE(c.types.size() == 2);
    CHECK(c.types[1].name == "llanura");
    CHECK(c.types[1].passable);
    CHECK(c.types[0].color[2] == 200);
    CHECK(c.find("llanura") == 1);
    CHECK_FALSE(c.find("lava").has_value());
}

TEST_CASE("Terrenos: nombres repetidos y colores fuera de rango se rechazan") {
    CHECK_FALSE(parse_terrain_catalog(replaced(kTerrain, "\"llanura\"", "\"agua\"")).has_value());
    const auto bad = parse_terrain_catalog(replaced(kTerrain, "[0, 200, 0]", "[0, 256, 0]"));
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().find("terrain[1].color[1]") != std::string::npos);
}

TEST_CASE("Configuración: un fichero válido se lee entero") {
    const auto cfg = parse_engine_config(kEngine, catalog());
    REQUIRE(cfg.has_value());
    CHECK(cfg->window.title == "prueba");
    CHECK_FALSE(cfg->window.vsync);
    CHECK(cfg->world.map.width == 64);
    REQUIRE(cfg->world.map.bands.size() == 2);
    CHECK(cfg->world.map.bands[0].terrain == 0);
    CHECK(cfg->world.map.bands[1].max_elevation == 65'536);
    CHECK(cfg->world.demo.max_speed == Fixed::from_ratio(1, 4));
    CHECK(cfg->view.selection_rect_color[3] == 48);
    CHECK(cfg->camera.scroll_edge_px_per_s == 900);
    CHECK(cfg->selection.click_radius_px == 10);
}

TEST_CASE("Configuración: una clave ausente da un error que la nombra") {
    const auto cfg = parse_engine_config(replaced(kEngine, "point_count", "otra_clave"), catalog());
    REQUIRE_FALSE(cfg.has_value());
    CHECK(cfg.error().find("demo.point_count") != std::string::npos);
}

TEST_CASE("Configuración: las bandas del mapa se validan") {
    SUBCASE("terreno desconocido") {
        const auto cfg = parse_engine_config(replaced(kEngine, "\"llanura\"", "\"lava\""), catalog());
        REQUIRE_FALSE(cfg.has_value());
        CHECK(cfg.error().find("map.bands[1].terrain") != std::string::npos);
    }
    SUBCASE("no crecientes") {
        const auto cfg = parse_engine_config(replaced(kEngine, "max_elevation = 30000", "max_elevation = 65536"),
                                             catalog());
        CHECK_FALSE(cfg.has_value());
    }
    SUBCASE("la última no cubre todo el rango") {
        const auto cfg = parse_engine_config(replaced(kEngine, "max_elevation = 65536", "max_elevation = 60000"),
                                             catalog());
        REQUIRE_FALSE(cfg.has_value());
        CHECK(cfg.error().find("65536") != std::string::npos);
    }
}

TEST_CASE("Configuración: la proyección exige ancho = 2 x alto") {
    const auto cfg = parse_engine_config(replaced(kEngine, "tile_height_px = 32", "tile_height_px = 30"), catalog());
    REQUIRE_FALSE(cfg.has_value());
    CHECK(cfg.error().find("2:1") != std::string::npos);
}

TEST_CASE("Configuración: un decimal donde se espera un entero se rechaza") {
    CHECK_FALSE(parse_engine_config(replaced(kEngine, "= 250", "= 0.25"), catalog()).has_value());
}

TEST_CASE("Configuración: TOML mal formado da error, no excepción") {
    CHECK_FALSE(parse_engine_config("[window\ntitle = ", catalog()).has_value());
    CHECK_FALSE(parse_terrain_catalog("[[terrain]\n").has_value());
}

TEST_CASE("Configuración: los datos del repositorio son válidos") {
    const auto data = load_game_data(RTS_DATA_DIR);
    CHECK_MESSAGE(data.has_value(), (data ? std::string() : data.error()));
}
