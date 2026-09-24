#include <cstdint>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "game/config.hpp"

using rts::game::load_game_data;
using rts::game::parse_engine_config;
using rts::game::parse_terrain_catalog;
using rts::game::parse_scenario;
using rts::game::parse_unit_catalog;
using rts::game::TerrainCatalog;
using rts::game::UnitCatalog;
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

constexpr const char* kUnits = R"(
[[unit]]
name = "lancero"
radius_milli_tiles = 300
speed_milli_tiles_per_tick = 60
color = [10, 20, 30]
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
unit = "lancero"
count = 10
area_tiles = 16

[movement]
sector_size_tiles = 16
max_portal_width_tiles = 8
path_node_budget_per_tick = 20000
flow_field_min_group = 8
flow_field_cache_size = 4
retarget_radius_tiles = 16
neighbor_radius_milli_tiles = 2000
max_neighbors = 8
time_horizon_ticks = 20
preference_weight = 100
collision_weight = 60
stuck_arrive_ticks = 40
arrive_radius_milli_tiles = 250
waypoint_radius_milli_tiles = 450

[view]
tile_width_px = 64
tile_height_px = 32
marker_radius_px = 5
elevation_shade_min_percent = 70
hillshade_step_percent = 8
clear_color = [1, 2, 3, 255]
debug_overlay_color = [4, 5, 6, 255]
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

UnitCatalog units() {
    auto c = parse_unit_catalog(kUnits);
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
    const auto cfg = parse_engine_config(kEngine, catalog(), units());
    REQUIRE(cfg.has_value());
    CHECK(cfg->window.title == "prueba");
    CHECK_FALSE(cfg->window.vsync);
    CHECK(cfg->world.map.width == 64);
    REQUIRE(cfg->world.map.bands.size() == 2);
    CHECK(cfg->world.map.bands[0].terrain == 0);
    CHECK(cfg->world.map.bands[1].max_elevation == 65'536);
    CHECK(cfg->world.demo.unit_type == 0);
    CHECK(cfg->world.demo.count == 10);
    CHECK(cfg->world.passable_by_terrain == std::vector<std::uint8_t>{0, 1});
    REQUIRE(cfg->world.unit_types.size() == 1);
    CHECK(cfg->world.unit_types[0].speed == Fixed::from_ratio(6, 100));
    CHECK(cfg->world.movement.hpa.sector_size == 16);
    CHECK(cfg->world.movement.neighbor_radius == Fixed::from_int(2));
    CHECK(cfg->view.selection_rect_color[3] == 48);
    CHECK(cfg->camera.scroll_edge_px_per_s == 900);
    CHECK(cfg->selection.click_radius_px == 10);
}

TEST_CASE("Configuración: una clave ausente da un error que la nombra") {
    const auto cfg = parse_engine_config(replaced(kEngine, "count = 10", "otra_clave = 10"), catalog(), units());
    REQUIRE_FALSE(cfg.has_value());
    CHECK(cfg.error().find("demo.count") != std::string::npos);
}

TEST_CASE("Configuración: las bandas del mapa se validan") {
    SUBCASE("terreno desconocido") {
        const auto cfg = parse_engine_config(replaced(kEngine, "\"llanura\"", "\"lava\""), catalog(), units());
        REQUIRE_FALSE(cfg.has_value());
        CHECK(cfg.error().find("map.bands[1].terrain") != std::string::npos);
    }
    SUBCASE("no crecientes") {
        const auto cfg = parse_engine_config(replaced(kEngine, "max_elevation = 30000", "max_elevation = 65536"),
                                             catalog(), units());
        CHECK_FALSE(cfg.has_value());
    }
    SUBCASE("la última no cubre todo el rango") {
        const auto cfg = parse_engine_config(replaced(kEngine, "max_elevation = 65536", "max_elevation = 60000"),
                                             catalog(), units());
        REQUIRE_FALSE(cfg.has_value());
        CHECK(cfg.error().find("65536") != std::string::npos);
    }
}

TEST_CASE("Configuración: la proyección exige ancho = 2 x alto") {
    const auto cfg = parse_engine_config(replaced(kEngine, "tile_height_px = 32", "tile_height_px = 30"), catalog(), units());
    REQUIRE_FALSE(cfg.has_value());
    CHECK(cfg.error().find("2:1") != std::string::npos);
}

TEST_CASE("Configuración: un decimal donde se espera un entero se rechaza") {
    CHECK_FALSE(parse_engine_config(replaced(kEngine, "= 250", "= 0.25"), catalog(), units()).has_value());
}

TEST_CASE("Configuración: TOML mal formado da error, no excepción") {
    CHECK_FALSE(parse_engine_config("[window\ntitle = ", catalog(), units()).has_value());
    CHECK_FALSE(parse_terrain_catalog("[[terrain]\n").has_value());
}

TEST_CASE("Configuración: los datos del repositorio son válidos") {
    const auto data = load_game_data(RTS_DATA_DIR);
    CHECK_MESSAGE(data.has_value(), (data ? std::string() : data.error()));
}

TEST_CASE("Unidades: el catálogo convierte milésimas a Fixed y rechaza radios mayores de media casilla") {
    const UnitCatalog c = units();
    REQUIRE(c.types.size() == 1);
    CHECK(c.types[0].type.radius == Fixed::from_ratio(3, 10));
    CHECK(c.find("lancero") == 0);
    CHECK_FALSE(parse_unit_catalog(replaced(kUnits, "radius_milli_tiles = 300", "radius_milli_tiles = 600"))
                    .has_value());
}

TEST_CASE("Configuración: la demo debe nombrar un tipo de unidad existente") {
    const auto cfg = parse_engine_config(replaced(kEngine, "\"lancero\"", "\"dragón\""), catalog(), units());
    REQUIRE_FALSE(cfg.has_value());
    CHECK(cfg.error().find("demo.unit") != std::string::npos);
}

TEST_CASE("Guion: órdenes en orden de tick y con destino válido") {
    const char* ok = R"(
[[order]]
tick = 5
first = 0
count = 3
target = [10, 20]

[[order]]
tick = 9
first = 3
count = 1
target = [300, 1]
)";
    const auto orders = parse_scenario(ok);
    REQUIRE(orders.has_value());
    REQUIRE(orders->size() == 2);
    CHECK((*orders)[1].target.x == 300);
    CHECK((*orders)[0].count == 3);
    CHECK_FALSE(parse_scenario(replaced(ok, "tick = 9", "tick = 2")).has_value());
    CHECK_FALSE(parse_scenario(replaced(ok, "[300, 1]", "[300]")).has_value());
}
