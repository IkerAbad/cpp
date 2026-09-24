#include "game/config.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <utility>

#include <toml++/toml.hpp>

namespace rts::game {

namespace {

// Convención de data/: sin números en coma flotante. Las magnitudes fraccionarias
// se escriben como enteros en una unidad menor, indicada en el nombre de la clave
// (_milli = milésimas). Así la carga no depende de cómo cada plataforma redondea
// un decimal a binario.
constexpr std::int32_t kMilli = 1000;
constexpr std::int64_t kByteMax = std::numeric_limits<std::uint8_t>::max();

// Lector con acumulación del primer error. Las rutas de error incluyen el prefijo
// de la tabla (p. ej. "map.bands[2].terrain") para que el mensaje sea accionable.
class Reader {
public:
    Reader(const toml::table& table, std::string_view source, std::string prefix, std::optional<std::string>& error)
        : table_(table), source_(source), prefix_(std::move(prefix)), error_(error) {}

    std::int32_t get_i32(std::string_view path, std::int64_t min, std::int64_t max) {
        const auto value = table_.at_path(path).value<std::int64_t>();
        if (!value) {
            fail(std::format("falta la clave entera '{}'", full(path)));
            return 0;
        }
        if (*value < min || *value > max) {
            fail(std::format("'{}' = {} fuera del rango [{}, {}]", full(path), *value, min, max));
            return 0;
        }
        return static_cast<std::int32_t>(*value);
    }

    std::uint64_t get_u64(std::string_view path) {
        const auto value = table_.at_path(path).value<std::int64_t>();
        if (!value || *value < 0) {
            fail(std::format("falta la clave entera no negativa '{}'", full(path)));
            return 0;
        }
        return static_cast<std::uint64_t>(*value);
    }

    bool get_bool(std::string_view path) {
        const auto value = table_.at_path(path).value<bool>();
        if (!value) {
            fail(std::format("falta la clave booleana '{}'", full(path)));
            return false;
        }
        return *value;
    }

    std::string get_string(std::string_view path) {
        const auto value = table_.at_path(path).value<std::string>();
        if (!value) {
            fail(std::format("falta la clave de texto '{}'", full(path)));
            return {};
        }
        return *value;
    }

    // Lista de N enteros en [0, 255]: [r, g, b] o [r, g, b, a].
    template <std::size_t N>
    std::array<std::uint8_t, N> get_color(std::string_view path) {
        const toml::array* arr = table_.at_path(path).as_array();
        std::array<std::uint8_t, N> color{};
        if (arr == nullptr || arr->size() != N) {
            fail(std::format("'{}' debe ser una lista de {} enteros ({})", full(path), N,
                             N == 3 ? "[r, g, b]" : "[r, g, b, a]"));
            return color;
        }
        for (std::size_t i = 0; i < N; ++i) {
            const auto v = (*arr)[i].value<std::int64_t>();
            if (!v || *v < 0 || *v > kByteMax) {
                fail(std::format("'{}[{}]' debe ser un entero en [0, 255]", full(path), i));
                return color;
            }
            color[i] = static_cast<std::uint8_t>(*v);
        }
        return color;
    }

    // Casilla [x, y] con coordenadas en [0, 1023] (el mapa más grande admitido).
    sim::TileCoord get_tile(std::string_view path) {
        constexpr std::int64_t kMaxCoord = 1023;
        const toml::array* arr = table_.at_path(path).as_array();
        if (arr == nullptr || arr->size() != 2) {
            fail(std::format("'{}' debe ser una lista de 2 enteros [x, y]", full(path)));
            return {};
        }
        const auto x = (*arr)[0].value<std::int64_t>();
        const auto y = (*arr)[1].value<std::int64_t>();
        if (!x || !y || *x < 0 || *y < 0 || *x > kMaxCoord || *y > kMaxCoord) {
            fail(std::format("'{}' debe tener coordenadas enteras en [0, {}]", full(path), kMaxCoord));
            return {};
        }
        return {static_cast<std::int32_t>(*x), static_cast<std::int32_t>(*y)};
    }

    // Lista de tablas ([[nombre]] en TOML). Vacía y con error si no existe.
    const toml::array* get_table_array(std::string_view path) {
        const toml::array* arr = table_.at_path(path).as_array();
        if (arr == nullptr || arr->empty() || !arr->is_array_of_tables()) {
            fail(std::format("falta la lista de tablas '[[{}]]'", full(path)));
            return nullptr;
        }
        return arr;
    }

    void fail(std::string message) {
        if (!error_) {
            error_ = std::format("{}: {}", source_, message);
        }
    }

    [[nodiscard]] std::string full(std::string_view path) const {
        return prefix_.empty() ? std::string(path) : std::format("{}.{}", prefix_, path);
    }

private:
    const toml::table& table_;
    std::string_view source_;
    std::string prefix_;
    std::optional<std::string>& error_;
};

std::expected<toml::table, std::string> parse_toml(std::string_view text, std::string_view source) {
    try {
        return toml::parse(text, source);
    } catch (const toml::parse_error& e) {
        return std::unexpected(std::format("{}: {}", source, e.description()));
    }
}

std::expected<std::string, std::string> read_text_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected(std::format("{}: no se puede abrir", path.string()));
    }
    return std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void read_map_params(const toml::table& root, std::string_view source, const TerrainCatalog& terrain,
                     std::optional<std::string>& error, sim::MapGenParams& map) {
    Reader r(root, source, "", error);
    map.width = r.get_i32("map.width_tiles", 16, 1024);
    map.height = r.get_i32("map.height_tiles", 16, 1024);
    map.seed = r.get_u64("map.seed");
    map.noise_cell_tiles = r.get_i32("map.noise_cell_tiles", 2, 1024);
    map.elevation_levels = r.get_i32("map.elevation_levels", 1, kByteMax + 1);

    const toml::array* bands = r.get_table_array("map.bands");
    if (bands == nullptr) {
        return;
    }
    std::int32_t previous = 0;
    for (std::size_t i = 0; i < bands->size(); ++i) {
        const toml::table& band_table = *(*bands)[i].as_table();
        Reader br(band_table, source, std::format("map.bands[{}]", i), error);
        const std::string name = br.get_string("terrain");
        const std::int32_t max_elevation = br.get_i32("max_elevation", 1, sim::kElevationRange);
        const auto id = terrain.find(name);
        if (!id) {
            br.fail(std::format("'{}' = \"{}\" no está en terrain.toml", br.full("terrain"), name));
            return;
        }
        if (max_elevation <= previous) {
            br.fail(std::format("'{}' debe ser mayor que el de la banda anterior ({})", br.full("max_elevation"),
                                previous));
            return;
        }
        previous = max_elevation;
        map.bands.push_back({max_elevation, *id});
    }
    if (previous != sim::kElevationRange) {
        r.fail(std::format("la última banda de 'map.bands' debe llegar a max_elevation = {}", sim::kElevationRange));
    }
}

}  // namespace

std::optional<sim::TerrainId> TerrainCatalog::find(std::string_view name) const {
    for (std::size_t i = 0; i < types.size(); ++i) {
        if (types[i].name == name) {
            return static_cast<sim::TerrainId>(i);
        }
    }
    return std::nullopt;
}

std::optional<sim::UnitTypeId> UnitCatalog::find(std::string_view name) const {
    for (std::size_t i = 0; i < types.size(); ++i) {
        if (types[i].name == name) {
            return static_cast<sim::UnitTypeId>(i);
        }
    }
    return std::nullopt;
}

std::expected<UnitCatalog, std::string> parse_unit_catalog(std::string_view toml_text, std::string_view source_name) {
    auto root = parse_toml(toml_text, source_name);
    if (!root) {
        return std::unexpected(root.error());
    }
    std::optional<std::string> error;
    Reader r(*root, source_name, "", error);
    UnitCatalog catalog;
    const toml::array* types = r.get_table_array("unit");
    if (types != nullptr && types->size() > static_cast<std::size_t>(kByteMax) + 1) {
        r.fail(std::format("hay {} tipos de unidad y el máximo es {}", types->size(), kByteMax + 1));
    }
    if (types != nullptr && !error) {
        for (std::size_t i = 0; i < types->size(); ++i) {
            Reader ur(*(*types)[i].as_table(), source_name, std::format("unit[{}]", i), error);
            UnitInfo info;
            info.name = ur.get_string("name");
            // Radio hasta media casilla: la separación y la rejilla espacial suponen
            // que dos unidades que se tocan están en casillas vecinas.
            info.type.radius = sim::Fixed::from_ratio(ur.get_i32("radius_milli_tiles", 50, kMilli / 2), kMilli);
            info.type.speed = sim::Fixed::from_ratio(ur.get_i32("speed_milli_tiles_per_tick", 1, kMilli / 2), kMilli);
            info.color = ur.get_color<3>("color");
            if (!error && catalog.find(info.name)) {
                ur.fail(std::format("nombre de unidad repetido: \"{}\"", info.name));
            }
            catalog.types.push_back(std::move(info));
        }
    }
    if (error) {
        return std::unexpected(*error);
    }
    return catalog;
}

std::expected<std::vector<ScenarioOrder>, std::string> parse_scenario(std::string_view toml_text,
                                                                      std::string_view source_name) {
    auto root = parse_toml(toml_text, source_name);
    if (!root) {
        return std::unexpected(root.error());
    }
    std::optional<std::string> error;
    Reader r(*root, source_name, "", error);
    std::vector<ScenarioOrder> orders;
    const toml::array* list = r.get_table_array("order");
    if (list != nullptr) {
        for (std::size_t i = 0; i < list->size(); ++i) {
            Reader orr(*(*list)[i].as_table(), source_name, std::format("order[{}]", i), error);
            ScenarioOrder o;
            o.tick = static_cast<sim::Tick>(orr.get_i32("tick", 0, std::numeric_limits<std::int32_t>::max()));
            o.first = orr.get_i32("first", 0, 1'000'000);
            o.count = orr.get_i32("count", 1, 1'000'000);
            o.target = orr.get_tile("target");
            if (!orders.empty() && o.tick < orders.back().tick) {
                orr.fail(std::format("'{}' debe ser >= que el de la orden anterior", orr.full("tick")));
            }
            orders.push_back(o);
        }
    }
    if (error) {
        return std::unexpected(*error);
    }
    return orders;
}

std::expected<TerrainCatalog, std::string> parse_terrain_catalog(std::string_view toml_text,
                                                                 std::string_view source_name) {
    auto root = parse_toml(toml_text, source_name);
    if (!root) {
        return std::unexpected(root.error());
    }
    std::optional<std::string> error;
    Reader r(*root, source_name, "", error);
    TerrainCatalog catalog;
    const toml::array* types = r.get_table_array("terrain");
    if (types != nullptr && types->size() > static_cast<std::size_t>(kByteMax) + 1) {
        r.fail(std::format("hay {} tipos de terreno y el máximo es {}", types->size(), kByteMax + 1));
    }
    if (types != nullptr && !error) {
        for (std::size_t i = 0; i < types->size(); ++i) {
            Reader tr(*(*types)[i].as_table(), source_name, std::format("terrain[{}]", i), error);
            TerrainInfo info;
            info.name = tr.get_string("name");
            info.passable = tr.get_bool("passable");
            info.color = tr.get_color<3>("color");
            if (!error && catalog.find(info.name)) {
                tr.fail(std::format("nombre de terreno repetido: \"{}\"", info.name));
            }
            catalog.types.push_back(std::move(info));
        }
    }
    if (error) {
        return std::unexpected(*error);
    }
    return catalog;
}

std::expected<EngineConfig, std::string> parse_engine_config(std::string_view toml_text,
                                                             const TerrainCatalog& terrain, const UnitCatalog& units,
                                                             std::string_view source_name) {
    auto root = parse_toml(toml_text, source_name);
    if (!root) {
        return std::unexpected(root.error());
    }
    std::optional<std::string> error;
    Reader r(*root, source_name, "", error);
    EngineConfig cfg;

    cfg.window.title = r.get_string("window.title");
    cfg.window.width = r.get_i32("window.width", 320, 16384);
    cfg.window.height = r.get_i32("window.height", 240, 16384);
    cfg.window.vsync = r.get_bool("window.vsync");
    cfg.loop.max_ticks_per_frame = r.get_i32("loop.max_ticks_per_frame", 1, 100);

    read_map_params(*root, source_name, terrain, error, cfg.world.map);

    for (const TerrainInfo& t : terrain.types) {
        cfg.world.passable_by_terrain.push_back(t.passable ? 1 : 0);
    }
    for (const UnitInfo& u : units.types) {
        cfg.world.unit_types.push_back(u.type);
    }

    sim::DemoParams& demo = cfg.world.demo;
    demo.seed = r.get_u64("demo.seed");
    const std::string unit_name = r.get_string("demo.unit");
    demo.count = r.get_i32("demo.count", 0, 100'000);
    demo.area_tiles = r.get_i32("demo.area_tiles", 2, 1024);
    if (const auto id = units.find(unit_name)) {
        demo.unit_type = *id;
    } else if (!error) {
        r.fail(std::format("'demo.unit' = \"{}\" no está en units.toml", unit_name));
    }

    sim::MovementParams& mv = cfg.world.movement;
    mv.hpa.sector_size = r.get_i32("movement.sector_size_tiles", 4, 256);
    mv.hpa.max_portal_width = r.get_i32("movement.max_portal_width_tiles", 1, 256);
    mv.path_node_budget_per_tick = r.get_i32("movement.path_node_budget_per_tick", 256, 10'000'000);
    mv.flow_field_min_group = r.get_i32("movement.flow_field_min_group", 1, 100'000);
    mv.flow_field_cache_size = r.get_i32("movement.flow_field_cache_size", 1, 64);
    mv.retarget_radius_tiles = r.get_i32("movement.retarget_radius_tiles", 0, 256);
    mv.neighbor_radius = sim::Fixed::from_ratio(r.get_i32("movement.neighbor_radius_milli_tiles", 100, 8 * kMilli), kMilli);
    mv.max_neighbors = r.get_i32("movement.max_neighbors", 1, 32);
    mv.time_horizon_ticks = r.get_i32("movement.time_horizon_ticks", 1, 400);
    mv.preference_weight = r.get_i32("movement.preference_weight", 0, 10'000);
    mv.collision_weight = r.get_i32("movement.collision_weight", 0, 10'000);
    mv.stuck_arrive_ticks = r.get_i32("movement.stuck_arrive_ticks", 1, 10'000);
    mv.arrive_radius = sim::Fixed::from_ratio(r.get_i32("movement.arrive_radius_milli_tiles", 10, 4 * kMilli), kMilli);
    mv.waypoint_radius =
        sim::Fixed::from_ratio(r.get_i32("movement.waypoint_radius_milli_tiles", 10, 4 * kMilli), kMilli);

    cfg.view.tile_width_px = r.get_i32("view.tile_width_px", 4, 1024);
    cfg.view.tile_height_px = r.get_i32("view.tile_height_px", 2, 512);
    cfg.view.marker_radius_px = r.get_i32("view.marker_radius_px", 1, 64);
    cfg.view.elevation_shade_min_percent = r.get_i32("view.elevation_shade_min_percent", 0, 100);
    cfg.view.hillshade_step_percent = r.get_i32("view.hillshade_step_percent", 0, 100);
    cfg.view.clear_color = r.get_color<4>("view.clear_color");
    cfg.view.debug_overlay_color = r.get_color<4>("view.debug_overlay_color");
    cfg.view.marker_selected_color = r.get_color<4>("view.marker_selected_color");
    cfg.view.selection_rect_color = r.get_color<4>("view.selection_rect_color");
    cfg.view.hover_tile_color = r.get_color<4>("view.hover_tile_color");

    cfg.camera.scroll_keys_px_per_s = r.get_i32("camera.scroll_keys_px_per_s", 0, 100'000);
    cfg.camera.scroll_edge_px_per_s = r.get_i32("camera.scroll_edge_px_per_s", 0, 100'000);
    cfg.camera.edge_margin_px = r.get_i32("camera.edge_margin_px", 0, 256);

    cfg.selection.drag_threshold_px = r.get_i32("selection.drag_threshold_px", 0, 256);
    cfg.selection.click_radius_px = r.get_i32("selection.click_radius_px", 1, 256);

    if (error) {
        return std::unexpected(*error);
    }
    if (cfg.view.tile_width_px != 2 * cfg.view.tile_height_px) {
        return std::unexpected(std::format("{}: la proyección es 2:1, view.tile_width_px ({}) debe ser el doble de "
                                           "view.tile_height_px ({})",
                                           source_name, cfg.view.tile_width_px, cfg.view.tile_height_px));
    }
    return cfg;
}

std::expected<GameData, std::string> load_game_data(const std::filesystem::path& data_dir) {
    GameData data;
    // Cada fichero: leer, analizar y mover al resultado; el primer error corta.
    auto load = [&](const std::filesystem::path& rel, auto&& parse, auto& dest) -> std::optional<std::string> {
        const std::filesystem::path path = data_dir / rel;
        const auto text = read_text_file(path);
        if (!text) {
            return text.error();
        }
        auto parsed = parse(*text, path.string());
        if (!parsed) {
            return parsed.error();
        }
        dest = std::move(*parsed);
        return std::nullopt;
    };
    if (auto e = load("terrain.toml", [](std::string_view t, std::string_view n) { return parse_terrain_catalog(t, n); },
                      data.terrain)) {
        return std::unexpected(*e);
    }
    if (auto e = load("units.toml", [](std::string_view t, std::string_view n) { return parse_unit_catalog(t, n); },
                      data.units)) {
        return std::unexpected(*e);
    }
    if (auto e = load(std::filesystem::path("config") / "engine.toml",
                      [&](std::string_view t, std::string_view n) {
                          return parse_engine_config(t, data.terrain, data.units, n);
                      },
                      data.engine)) {
        return std::unexpected(*e);
    }
    if (auto e = load(std::filesystem::path("scenarios") / "headless.toml",
                      [](std::string_view t, std::string_view n) { return parse_scenario(t, n); },
                      data.headless_scenario)) {
        return std::unexpected(*e);
    }
    return data;
}

}  // namespace rts::game
