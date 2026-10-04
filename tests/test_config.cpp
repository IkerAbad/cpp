#include <cstdint>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "game/config.hpp"

using rts::game::BuildingCatalog;
using rts::game::Catalogs;
using rts::game::load_game_data;
using rts::game::NodeCatalog;
using rts::game::parse_building_catalog;
using rts::game::parse_engine_config;
using rts::game::parse_node_catalog;
using rts::game::parse_scenario;
using rts::game::parse_terrain_catalog;
using rts::game::parse_unit_catalog;
using rts::game::ScenarioAction;
using rts::game::TerrainCatalog;
using rts::game::UnitCatalog;
using rts::sim::Fixed;
using rts::sim::Resource;

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
classes = ["infanteria", "edificio"]

[[unit]]
name = "lancero"
radius_milli_tiles = 300
speed_milli_tiles_per_tick = 60
color = [10, 20, 30]
cost = { comida = 60, oro = 20 }
train_ticks = 400
population = 1
worker = false
carry_capacity = 0
hp = 40
attack = { cuerpo = 5 }
armor = { proyectil = 1 }
class = "infanteria"
bonus = { edificio = 2 }
range_milli_tiles = 150
reload_ticks = 40
sight_tiles = 6
projectile_speed_milli_tiles_per_tick = 0
auto_attack = true
ignite = 50
extinguish = 4
siege = false
buildings_only = false
undermine = false

[[unit]]
name = "peon"
radius_milli_tiles = 250
speed_milli_tiles_per_tick = 50
color = [1, 2, 3]
cost = { comida = 50 }
train_ticks = 500
population = 1
worker = true
carry_capacity = 10
hp = 40
attack = { cuerpo = 5 }
armor = { proyectil = 1 }
class = "infanteria"
bonus = { edificio = 2 }
range_milli_tiles = 150
reload_ticks = 40
sight_tiles = 6
projectile_speed_milli_tiles_per_tick = 0
auto_attack = false
ignite = 50
extinguish = 4
siege = false
buildings_only = false
undermine = false
)";

constexpr const char* kNodes = R"(
[[node]]
name = "pino"
resource = "madera"
amount = 100
size_tiles = 1
color = [0, 90, 0]

[[node]]
name = "veta"
resource = "oro"
amount = 800
size_tiles = 2
color = [200, 200, 0]

[[node]]
name = "cascote"
resource = "piedra"
amount = 1
size_tiles = 1
color = [150, 140, 130]
)";

constexpr const char* kBuildings = R"(
[[building]]
name = "fuerte"
size_tiles = 3
cost = { madera = 275, piedra = 100 }
build_ticks = 3000
hp = 2400
accepts = ["comida", "madera", "piedra", "oro", "hierro"]
population = 5
armor = { cuerpo = 3, proyectil = 8 }
class = "edificio"
trains = ["peon"]
requires = []
farm_food = 0
vital = false
material = "madera"
color = [9, 9, 9]

[[building]]
name = "choza"
size_tiles = 2
cost = { madera = 30 }
build_ticks = 500
hp = 550
accepts = []
population = 5
armor = { cuerpo = 1 }
class = "edificio"
trains = []
requires = []
farm_food = 0
vital = false
material = "madera"
color = [8, 8, 8]
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

[[player]]
start = [10, 10]
color = [0, 0, 255]
controller = "humano"

[[player]]
start = [50, 20]
color = [255, 0, 0]
controller = "ia"
ai_profile = "basica"

[demo]
seed = 1
unit = "lancero"
player = 0
count = 10
area_tiles = 16

[economy]
gather_ticks = { comida = 50, madera = 55, piedra = 70, oro = 65, hierro = 70 }
interact_range_milli_tiles = 500
retarget_radius_tiles = 10
approach_attempts = 3
gatherers_per_tile = 2
queue_capacity = 5
max_population = 200
spawn_search_radius_tiles = 4
repair_cost_percent = 50
salvage_percent = 50
rubble_stone = "cascote"
rubble_wood = "pino"

[fire]
max_intensity = 1000
sustain_intensity = 200
decay_per_tick = 2
growth_wood_per_tick = 4
growth_stone_per_tick = 2
burn_wood_milli_hp_per_tick = 750
burn_stone_milli_hp_per_tick = 500
stone_floor_percent = 60
spread_intensity = 800
spread_per_tick = 3
spread_gap_tiles = 1
extinguish_reach_milli_tiles = 800

[combat]
acquire_interval_ticks = 10
repath_tiles = 2
chase_attempts = 3
building_reach_milli_tiles = 600
projectile_hit_radius_milli_tiles = 150
xp_kill_bonus = 20
level_thresholds = [30, 70, 120]
hp_percent_per_level = 8
attack_percent_per_level = 6
armor_every_levels = 3
hero_aura_radius_milli_tiles = 4000
hero_aura_attack_percent = 15
hero_names = ["Brunilda", "Tello"]

[ai]
think_interval_ticks = 20
worker = "peon"
house = "choza"
barracks = "fuerte"
farm = "choza"
workshop = "fuerte"
dropoff = { comida = "fuerte", madera = "fuerte", piedra = "fuerte", oro = "fuerte", hierro = "fuerte" }

[[ai.profile]]
name = "basica"
behaviors = ["defensa", "aldeanos", "ataque"]
villager_target = 25
gather_percent = { comida = 40, madera = 35, oro = 15, piedra = 10 }
house_margin = 3
barracks_at_villagers = 10
dropoff_distance_tiles = 12
dropoff_min_gatherers = 3
builders = 2
gatherers_per_farm = 2
build_gap_tiles = 1
build_search_radius_tiles = 24
first_wave = 6
wave_growth = 4
defend_radius_tiles = 14
flee_enemy_tiles = 3
safe_base_tiles = 2
barracks_queue = 2
villager_queue = 1
attack_ratio_percent = 130
retreat_ratio_percent = 60
min_attack_army = 6
engage_radius_tiles = 8
extinguishers_per_fire = 0
army_min_villagers = 0
raid_unit = ""
raid_group = 1
raid_safe_radius_tiles = 0
army = ["lancero"]

[setup]
seed = 3
start_search_radius_tiles = 20
min_start_region_tiles = 100
start_building = "fuerte"
start_unit = "peon"
start_units = 4
start_stock = { comida = 200, madera = 150 }
forest_terrain = "llanura"
tree = "pino"
tree_density_permille = 100
clear_radius_tiles = 6

[[setup.near_start]]
node = "veta"
count = 1
min_distance_tiles = 6
max_distance_tiles = 9

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
building_body_percent = 70
node_body_percent = 80
construction_shade_percent = 45
ghost_valid_color = [0, 255, 0, 100]
ghost_invalid_color = [255, 0, 0, 100]
resource_colors = { comida = [1, 0, 0, 255], madera = [2, 0, 0, 255], piedra = [3, 0, 0, 255], oro = [4, 0, 0, 255], hierro = [5, 0, 0, 255] }
# Barras de vida (unidades heridas o seleccionadas, edificios dañados): 20x3 píxeles,
# en rojo por debajo del 35 %.
health_bar_width_px = 20
health_bar_height_px = 3
health_low_permille = 350
health_back_color = [20, 20, 20, 200]
health_color = [90, 220, 90, 255]
health_low_color = [230, 70, 50, 255]
projectile_color = [245, 240, 220, 255]
hero_color = [255, 200, 40, 255]
fire_color = [255, 110, 20, 255]
burned_shade_percent = 30

[camera]
scroll_keys_px_per_s = 1000
scroll_edge_px_per_s = 900
edge_margin_px = 6

[selection]
drag_threshold_px = 4
click_radius_px = 10

[replay]
checkpoint_interval_ticks = 200
directory = "replays"
speeds = [1, 2, 4, 8]
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

NodeCatalog nodes() {
    auto c = parse_node_catalog(kNodes);
    REQUIRE(c.has_value());
    return *c;
}

BuildingCatalog buildings(const UnitCatalog& u) {
    auto c = parse_building_catalog(kBuildings, u);
    REQUIRE(c.has_value());
    return *c;
}

// Catálogos de prueba con vida propia (Catalogs guarda referencias).
struct TestCatalogs {
    TerrainCatalog terrain = catalog();
    UnitCatalog unit_catalog = units();
    NodeCatalog node_catalog = nodes();
    BuildingCatalog building_catalog = buildings(unit_catalog);

    [[nodiscard]] Catalogs get() const { return {terrain, unit_catalog, building_catalog, node_catalog}; }
};

auto parse_engine(const std::string& text) {
    const TestCatalogs c;
    return parse_engine_config(text, c.get());
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
    const auto cfg = parse_engine(kEngine);
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
    REQUIRE(cfg->world.unit_types.size() == 2);
    CHECK(cfg->world.unit_types[0].speed == Fixed::from_ratio(6, 100));
    CHECK(cfg->world.movement.hpa.sector_size == 16);
    CHECK(cfg->world.movement.neighbor_radius == Fixed::from_int(2));
    CHECK(cfg->view.selection_rect_color[3] == 48);
    CHECK(cfg->camera.scroll_edge_px_per_s == 900);
    CHECK(cfg->selection.click_radius_px == 10);
}

TEST_CASE("Configuración: una clave ausente da un error que la nombra") {
    const auto cfg = parse_engine(replaced(kEngine, "count = 10", "otra_clave = 10"));
    REQUIRE_FALSE(cfg.has_value());
    CHECK(cfg.error().find("demo.count") != std::string::npos);
}

TEST_CASE("Configuración: las bandas del mapa se validan") {
    SUBCASE("terreno desconocido") {
        const auto cfg = parse_engine(replaced(kEngine, "\"llanura\"", "\"lava\""));
        REQUIRE_FALSE(cfg.has_value());
        CHECK(cfg.error().find("map.bands[1].terrain") != std::string::npos);
    }
    SUBCASE("no crecientes") {
        const auto cfg = parse_engine(replaced(kEngine, "max_elevation = 30000", "max_elevation = 65536"));
        CHECK_FALSE(cfg.has_value());
    }
    SUBCASE("la última no cubre todo el rango") {
        const auto cfg = parse_engine(replaced(kEngine, "max_elevation = 65536", "max_elevation = 60000"));
        REQUIRE_FALSE(cfg.has_value());
        CHECK(cfg.error().find("65536") != std::string::npos);
    }
}

TEST_CASE("Configuración: la proyección exige ancho = 2 x alto") {
    const auto cfg = parse_engine(replaced(kEngine, "tile_height_px = 32", "tile_height_px = 30"));
    REQUIRE_FALSE(cfg.has_value());
    CHECK(cfg.error().find("2:1") != std::string::npos);
}

TEST_CASE("Configuración: un decimal donde se espera un entero se rechaza") {
    CHECK_FALSE(parse_engine(replaced(kEngine, "= 250", "= 0.25")).has_value());
}

TEST_CASE("Configuración: el reproductor necesita al menos una velocidad") {
    const auto cfg = parse_engine(replaced(kEngine, "speeds = [1, 2, 4, 8]", "speeds = []"));
    REQUIRE_FALSE(cfg.has_value());
    CHECK(cfg.error().find("replay.speeds") != std::string::npos);
}

TEST_CASE("Configuración: TOML mal formado da error, no excepción") {
    CHECK_FALSE(parse_engine("[window\ntitle = ").has_value());
    CHECK_FALSE(parse_terrain_catalog("[[terrain]\n").has_value());
}

TEST_CASE("Configuración: los datos del repositorio son válidos") {
    const auto data = load_game_data(RTS_DATA_DIR);
    CHECK_MESSAGE(data.has_value(), (data ? std::string() : data.error()));
}

TEST_CASE("Unidades: el catálogo convierte milésimas a Fixed y rechaza radios mayores de media casilla") {
    const UnitCatalog c = units();
    REQUIRE(c.types.size() == 2);
    CHECK(c.types[0].type.radius == Fixed::from_ratio(3, 10));
    CHECK(c.find("lancero") == 0);
    CHECK_FALSE(parse_unit_catalog(replaced(kUnits, "radius_milli_tiles = 300", "radius_milli_tiles = 600"))
                    .has_value());
}

TEST_CASE("Configuración: la demo debe nombrar un tipo de unidad existente") {
    const auto cfg = parse_engine(replaced(kEngine, "\"lancero\"", "\"dragón\""));
    REQUIRE_FALSE(cfg.has_value());
    CHECK(cfg.error().find("demo.unit") != std::string::npos);
}

TEST_CASE("Guion: acciones con sus claves, en orden de tick") {
    const char* ok = R"(
[build_site]
gap_tiles = 2
search_radius_tiles = 20

[[order]]
tick = 5
action = "mover"
player = 0
first = 0
count = 3
target = [10, 20]

[[order]]
tick = 9
action = "recoger"
player = 1
first = 3
count = 1
resource = "oro"

[[order]]
tick = 9
action = "entrenar"
player = 1
building = "fuerte"
unit = "peon"
count = 2
)";
    const TestCatalogs c;
    const auto sc = parse_scenario(ok, c.get());
    REQUIRE(sc.has_value());
    REQUIRE(sc->orders.size() == 3);
    CHECK(sc->build_gap_tiles == 2);
    CHECK(sc->orders[0].target.x == 10);
    CHECK(sc->orders[1].action == ScenarioAction::Gather);
    CHECK(sc->orders[1].resource == Resource::Gold);
    CHECK(sc->orders[2].unit == 1);
    CHECK(sc->orders[2].count == 2);
    CHECK_FALSE(parse_scenario(replaced(ok, "tick = 9", "tick = 2"), c.get()).has_value());
    CHECK_FALSE(parse_scenario(replaced(ok, "[10, 20]", "[10]"), c.get()).has_value());
    CHECK_FALSE(parse_scenario(replaced(ok, "\"recoger\"", "\"volar\""), c.get()).has_value());
    CHECK_FALSE(parse_scenario(replaced(ok, "\"oro\"", "\"plata\""), c.get()).has_value());
    CHECK_FALSE(parse_scenario(replaced(ok, "unit = \"peon\"", "unit = \"grifo\""), c.get()).has_value());
}

TEST_CASE("Economía: costes, almacenes y producción se leen de los catálogos") {
    const TestCatalogs c;
    const auto& peon = c.unit_catalog.types[1].type;
    CHECK(peon.worker);
    CHECK(peon.carry_capacity == 10);
    CHECK(peon.cost[rts::sim::resource_index(Resource::Food)] == 50);
    CHECK(c.unit_catalog.types[0].type.cost[rts::sim::resource_index(Resource::Gold)] == 20);
    const auto& fuerte = c.building_catalog.types[0].type;
    CHECK(fuerte.accepts == 0x1F);
    CHECK(fuerte.trains == std::vector<rts::sim::UnitTypeId>{1});
    CHECK(c.building_catalog.types[1].type.accepts == 0);
    CHECK(c.node_catalog.types[1].type.kind == Resource::Gold);
    CHECK(c.node_catalog.types[1].type.size == 2);

    // Erratas: recurso desconocido en un coste, unidad producida inexistente, un
    // aldeano que no puede llevar nada.
    CHECK_FALSE(parse_unit_catalog(replaced(kUnits, "comida = 60", "comdia = 60")).has_value());
    CHECK_FALSE(parse_building_catalog(replaced(kBuildings, "[\"peon\"]", "[\"grifo\"]"), c.unit_catalog).has_value());
    CHECK_FALSE(parse_unit_catalog(replaced(kUnits, "carry_capacity = 10", "carry_capacity = 0")).has_value());
    CHECK_FALSE(parse_node_catalog(replaced(kNodes, "\"madera\"", "\"plata\"")).has_value());
    // Combate: clase desconocida, bonus contra una clase inexistente, clave de ataque rara.
    CHECK_FALSE(parse_unit_catalog(replaced(kUnits, "class = \"infanteria\"", "class = \"dragon\"")).has_value());
    CHECK_FALSE(parse_unit_catalog(replaced(kUnits, "bonus = { edificio = 2 }", "bonus = { barco = 2 }")).has_value());
    CHECK_FALSE(parse_unit_catalog(replaced(kUnits, "attack = { cuerpo = 5 }", "attack = { fuego = 5 }")).has_value());
    const auto& lancero = c.unit_catalog.types[0].type.combat;
    CHECK(lancero.hp == 40);
    CHECK(lancero.attack_melee == 5);
    CHECK(lancero.armor_pierce == 1);
    CHECK(lancero.bonus[1] == 2);
    CHECK(lancero.range == Fixed::from_ratio(15, 100));
    CHECK(c.building_catalog.types[0].type.armor_pierce == 8);
    CHECK(c.building_catalog.types[0].type.armor_class == 1);
}

TEST_CASE("Configuración: jugadores, economía y preparación") {
    const auto cfg = parse_engine(kEngine);
    REQUIRE(cfg.has_value());
    const auto& w = cfg->world;
    REQUIRE(w.setup.starts.size() == 2);
    CHECK(w.setup.starts[1].x == 50);
    CHECK(cfg->player_colors[1][0] == 255);
    CHECK(w.economy.gather_ticks[rts::sim::resource_index(Resource::Stone)] == 70);
    CHECK(w.economy.interact_range == Fixed::from_ratio(1, 2));
    CHECK(w.setup.start_building == 0);
    CHECK(w.setup.start_unit == 1);
    CHECK(w.setup.start_stock[rts::sim::resource_index(Resource::Wood)] == 150);
    CHECK(w.setup.start_stock[rts::sim::resource_index(Resource::Gold)] == 0);
    REQUIRE(w.setup.near_start.size() == 1);
    CHECK(w.setup.near_start[0].type == 1);
    CHECK(w.building_types.size() == 2);
    CHECK(cfg->view.resource_colors[rts::sim::resource_index(Resource::Stone)][0] == 3);
    CHECK(w.combat.level_thresholds == std::vector<std::int32_t>{30, 70, 120});
    CHECK(w.combat.hero_name_count == 2);
    CHECK(w.combat.building_reach == Fixed::from_ratio(3, 5));
    CHECK(cfg->hero_names[1] == "Tello");
    REQUIRE(w.ai_players.size() == 1);
    CHECK(w.ai_players[0].player == 1);
    CHECK(w.ai_players[0].profile == 0);
    CHECK(w.ai.worker_type == 1);
    CHECK(w.ai.house == 1);
    REQUIRE(w.ai.profiles.size() == 1);
    CHECK(w.ai.profiles[0].gather_percent[rts::sim::resource_index(Resource::Food)] == 40);
    CHECK(w.ai.profiles[0].behaviors ==
          std::vector<rts::sim::AiBehavior>{rts::sim::AiBehavior::Defend, rts::sim::AiBehavior::Villagers,
                                            rts::sim::AiBehavior::Attack});
    SUBCASE("módulo de IA desconocido") {
        const auto bad = parse_engine(replaced(kEngine, "\"aldeanos\", \"ataque\"", "\"aldeanos\", \"trampas\""));
        REQUIRE_FALSE(bad.has_value());
        CHECK(bad.error().find("trampas") != std::string::npos);
    }
    SUBCASE("perfil de jugador inexistente") {
        const auto bad = parse_engine(replaced(kEngine, "ai_profile = \"basica\"", "ai_profile = \"dificil\""));
        REQUIRE_FALSE(bad.has_value());
        CHECK(bad.error().find("dificil") != std::string::npos);
    }
    SUBCASE("reparto que no suma 100") {
        CHECK_FALSE(parse_engine(replaced(kEngine, "comida = 40, madera = 35, oro = 15", "comida = 40, madera = 35, oro = 16")).has_value());
    }
    SUBCASE("controlador desconocido") {
        CHECK_FALSE(parse_engine(replaced(kEngine, "controller = \"ia\"", "controller = \"robot\"")).has_value());
    }
    SUBCASE("umbrales no crecientes") {
        CHECK_FALSE(parse_engine(replaced(kEngine, "[30, 70, 120]", "[30, 30, 120]")).has_value());
    }
    CHECK(w.node_types.size() == 3);

    SUBCASE("inicio fuera del mapa") {
        CHECK_FALSE(parse_engine(replaced(kEngine, "[50, 20]", "[50, 40]")).has_value());
    }
    SUBCASE("tiempo de recogida nulo") {
        CHECK_FALSE(parse_engine(replaced(kEngine, "piedra = 70", "piedra = 0")).has_value());
    }
    SUBCASE("edificio inicial desconocido") {
        const auto bad = parse_engine(replaced(kEngine, "start_building = \"fuerte\"", "start_building = \"castillo\""));
        REQUIRE_FALSE(bad.has_value());
        CHECK(bad.error().find("setup.start_building") != std::string::npos);
    }
}

TEST_CASE("Edificios: los requisitos nombran otros edificios y no forman ciclos") {
    const UnitCatalog u = units();
    // fuerte exige choza y choza exige fuerte: ninguno se podría colocar nunca.
    std::string text = kBuildings;
    const auto first = text.find("requires = []");
    REQUIRE(first != std::string::npos);
    text.replace(first, std::string("requires = []").size(), "requires = [\"choza\"]");
    const auto ok = parse_building_catalog(text, u);
    REQUIRE_MESSAGE(ok.has_value(), (ok ? std::string() : ok.error()));
    CHECK(ok->types[0].type.required == std::vector<rts::sim::BuildingTypeId>{1});
    const auto second = text.find("requires = []");
    REQUIRE(second != std::string::npos);
    text.replace(second, std::string("requires = []").size(), "requires = [\"fuerte\"]");
    const auto cycle = parse_building_catalog(text, u);
    REQUIRE_FALSE(cycle.has_value());
    CHECK(cycle.error().find("ciclo") != std::string::npos);
    CHECK_FALSE(parse_building_catalog(replaced(kBuildings, "requires = []", "requires = [\"torre\"]"), u).has_value());
}

TEST_CASE("Configuración: un fuego necesita una intensidad de sostén de al menos 1") {
    CHECK_FALSE(parse_engine(replaced(kEngine, "sustain_intensity = 200", "sustain_intensity = 0")).has_value());
}
