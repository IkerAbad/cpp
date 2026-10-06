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
// Límites de cordura de los datos (no de diseño): atrapan erratas como un cero de más.
constexpr std::int64_t kMaxAmount = 1'000'000;
constexpr std::int64_t kMaxTicks = 1'000'000;
constexpr std::int64_t kMaxFootprint = 8;
constexpr std::int64_t kMaxPlayers = 8;
constexpr std::size_t kMaxReplaySpeeds = 8;
constexpr std::size_t kMaxAiProfiles = 32;
constexpr std::size_t kMaxLabelBytes = 4;  // iniciales cortas: caben sobre el marcador
// Nombres de los módulos de IA en los datos, en el orden de sim::AiBehavior.
constexpr std::array<std::string_view, static_cast<std::size_t>(sim::AiBehavior::Count)> kAiBehaviorNames{
    "defensa",  "aldeanos", "casas",       "cuartel",         "granjas",  "almacenes", "obras",
    "recoleccion", "ejercito", "ataque", "ejercito_contra", "ataque_fuerza", "concentrar",
    "taller",      "apagar",   "incendiar",     "abastecer",     "logistica",     "explorar",
    "asalto",
};

constexpr std::array<std::string_view, sim::kResourceCount> kResourceKeys{"comida", "madera", "piedra", "oro", "hierro"};

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

    // Lista de tablas ([[nombre]] en TOML). Si es obligatoria, vacía o ausente es error;
    // si no, ausente devuelve nullptr sin error.
    const toml::array* get_table_array(std::string_view path, bool required = true) {
        const toml::node_view node = table_.at_path(path);
        if (!node && !required) {
            return nullptr;
        }
        const toml::array* arr = node.as_array();
        if (arr == nullptr || arr->empty() || !arr->is_array_of_tables()) {
            fail(std::format("falta la lista de tablas '[[{}]]'", full(path)));
            return nullptr;
        }
        return arr;
    }

    // Cantidades por recurso: { comida = 50, madera = 20 }. Las ausentes valen 0; una
    // clave que no es un recurso es un error (atrapa erratas).
    sim::Stock get_stock(std::string_view path) {
        sim::Stock stock{};
        const toml::table* t = table_.at_path(path).as_table();
        if (t == nullptr) {
            fail(std::format("falta la tabla '{}' (p. ej. {{ comida = 50, madera = 20 }})", full(path)));
            return stock;
        }
        for (const auto& [key, node] : *t) {
            const auto r = find_resource(key.str());
            const auto v = node.value<std::int64_t>();
            if (!r) {
                fail(std::format("'{}.{}' no es un recurso (comida, madera, piedra, oro, hierro)", full(path), key.str()));
                return stock;
            }
            if (!v || *v < 0 || *v > kMaxAmount) {
                fail(std::format("'{}.{}' debe ser un entero en [0, {}]", full(path), key.str(), kMaxAmount));
                return stock;
            }
            stock[sim::resource_index(*r)] = static_cast<std::int32_t>(*v);
        }
        return stock;
    }

    std::vector<std::string> get_string_list(std::string_view path) {
        const toml::array* arr = table_.at_path(path).as_array();
        if (arr == nullptr) {
            fail(std::format("falta la lista de textos '{}'", full(path)));
            return {};
        }
        std::vector<std::string> out;
        for (std::size_t i = 0; i < arr->size(); ++i) {
            const auto v = (*arr)[i].value<std::string>();
            if (!v) {
                fail(std::format("'{}[{}]' debe ser un texto", full(path), i));
                return {};
            }
            out.push_back(*v);
        }
        return out;
    }

    sim::Resource get_resource(std::string_view path) {
        const std::string key = get_string(path);
        const auto r = find_resource(key);
        if (!r && !error_) {
            fail(std::format("'{}' = \"{}\" no es un recurso (comida, madera, piedra, oro, hierro)", full(path), key));
        }
        return r.value_or(sim::Resource::Food);
    }

    // Nombre que se resuelve contra un catálogo (unidad, edificio, nodo, terreno).
    template <typename Catalog>
    auto get_named(std::string_view path, const Catalog& catalog, std::string_view file) {
        const std::string name = get_string(path);
        const auto id = catalog.find(name);
        if (!id && !error_) {
            fail(std::format("'{}' = \"{}\" no está en {}", full(path), name, file));
        }
        return id.value_or(0);
    }

    // { cuerpo = n, proyectil = m }: las ausentes valen 0.
    std::array<std::int32_t, 2> get_melee_pierce(std::string_view path) {
        std::array<std::int32_t, 2> out{};
        const toml::table* t = table_.at_path(path).as_table();
        if (t == nullptr) {
            fail(std::format("falta la tabla '{}' (p. ej. {{ cuerpo = 5, proyectil = 0 }})", full(path)));
            return out;
        }
        for (const auto& [key, node] : *t) {
            const auto v = node.value<std::int64_t>();
            const std::size_t i = key.str() == "cuerpo" ? 0 : key.str() == "proyectil" ? 1 : 2;
            if (i == 2) {
                fail(std::format("'{}.{}': las claves son cuerpo y proyectil", full(path), key.str()));
                return out;
            }
            if (!v || *v < 0 || *v > kMaxAmount) {
                fail(std::format("'{}.{}' debe ser un entero en [0, {}]", full(path), key.str(), kMaxAmount));
                return out;
            }
            out[i] = static_cast<std::int32_t>(*v);
        }
        return out;
    }

    // Lista de enteros estrictamente crecientes y positivos.
    std::vector<std::int32_t> get_increasing_list(std::string_view path, std::size_t max_size) {
        const toml::array* arr = table_.at_path(path).as_array();
        std::vector<std::int32_t> out;
        if (arr == nullptr || arr->size() > max_size) {
            fail(std::format("'{}' debe ser una lista de hasta {} enteros", full(path), max_size));
            return out;
        }
        for (std::size_t i = 0; i < arr->size(); ++i) {
            const auto v = (*arr)[i].value<std::int64_t>();
            if (!v || *v <= (out.empty() ? 0 : out.back()) || *v > kMaxAmount) {
                fail(std::format("'{}[{}]' debe ser un entero mayor que el anterior (y que 0)", full(path), i));
                return {};
            }
            out.push_back(static_cast<std::int32_t>(*v));
        }
        return out;
    }

    [[nodiscard]] bool failed() const noexcept { return error_.has_value(); }

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

template <typename Types>
std::optional<std::uint8_t> find_by_name(const Types& types, std::string_view name) {
    for (std::size_t i = 0; i < types.size(); ++i) {
        if (types[i].name == name) {
            return static_cast<std::uint8_t>(i);
        }
    }
    return std::nullopt;
}

// Recorre [[array_name]] y llama a read(reader, índice) con un lector con prefijo.
// Adaptador para resolver nombres de clase de armadura con Reader::get_named.
struct ClassNames {
    const UnitCatalog& units;
    [[nodiscard]] std::optional<sim::ArmorClassId> find(std::string_view name) const {
        return units.find_class(name);
    }
};

template <typename Read>
void for_each_table(const toml::array* list, std::string_view source, std::string_view array_name,
                    std::optional<std::string>& error, Read&& read) {
    if (list == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < list->size() && !error; ++i) {
        Reader item(*(*list)[i].as_table(), source, std::format("{}[{}]", array_name, i), error);
        read(item, i);
    }
}

}  // namespace

std::string_view resource_key(sim::Resource r) noexcept {
    return kResourceKeys[sim::resource_index(r)];
}

std::optional<sim::Resource> find_resource(std::string_view key) noexcept {
    for (std::size_t i = 0; i < kResourceKeys.size(); ++i) {
        if (kResourceKeys[i] == key) {
            return static_cast<sim::Resource>(i);
        }
    }
    return std::nullopt;
}

std::optional<sim::NodeTypeId> NodeCatalog::find(std::string_view name) const {
    return find_by_name(types, name);
}

std::optional<sim::BuildingTypeId> BuildingCatalog::find(std::string_view name) const {
    return find_by_name(types, name);
}

std::optional<sim::TerrainId> TerrainCatalog::find(std::string_view name) const {
    return find_by_name(types, name);
}

std::optional<sim::UnitTypeId> UnitCatalog::find(std::string_view name) const {
    return find_by_name(types, name);
}

std::optional<sim::ArmorClassId> UnitCatalog::find_class(std::string_view name) const {
    for (std::size_t i = 0; i < classes.size(); ++i) {
        if (classes[i] == name) {
            return static_cast<sim::ArmorClassId>(i);
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
    catalog.classes = r.get_string_list("classes");
    if (!error && (catalog.classes.empty() || catalog.classes.size() > sim::kMaxArmorClasses)) {
        r.fail(std::format("'classes' debe tener entre 1 y {} clases de armadura", sim::kMaxArmorClasses));
    }
    const toml::array* types = r.get_table_array("unit");
    if (types != nullptr && types->size() > static_cast<std::size_t>(kByteMax) + 1) {
        r.fail(std::format("hay {} tipos de unidad y el máximo es {}", types->size(), kByteMax + 1));
    }
    if (types != nullptr && !error) {
        for (std::size_t i = 0; i < types->size(); ++i) {
            Reader ur(*(*types)[i].as_table(), source_name, std::format("unit[{}]", i), error);
            UnitInfo info;
            info.name = ur.get_string("name");
            info.label = ur.get_string("label");
            if (!error && (info.label.empty() || info.label.size() > kMaxLabelBytes)) {
                ur.fail(std::format("'label' debe tener entre 1 y {} bytes", kMaxLabelBytes));
            }
            // Radio hasta media casilla: la separación y la rejilla espacial suponen
            // que dos unidades que se tocan están en casillas vecinas.
            info.type.radius = sim::Fixed::from_ratio(ur.get_i32("radius_milli_tiles", 50, kMilli / 2), kMilli);
            info.type.speed = sim::Fixed::from_ratio(ur.get_i32("speed_milli_tiles_per_tick", 0, kMilli / 2), kMilli);
            info.color = ur.get_color<3>("color");
            info.type.cost = ur.get_stock("cost");
            info.type.train_ticks = ur.get_i32("train_ticks", 1, kMaxTicks);
            info.type.population = ur.get_i32("population", 0, 100);
            info.type.worker = ur.get_bool("worker");
            // Un aldeano debe poder llevar algo; los demás no llevan nada.
            info.type.carry_capacity =
                ur.get_i32("carry_capacity", info.type.worker ? 1 : 0, info.type.worker ? kMaxAmount : 0);
            sim::CombatStats& cs = info.type.combat;
            cs.hp = ur.get_i32("hp", 1, kMaxAmount);
            const auto attack = ur.get_melee_pierce("attack");
            const auto armor = ur.get_melee_pierce("armor");
            cs.attack_melee = attack[0];
            cs.attack_pierce = attack[1];
            cs.armor_melee = armor[0];
            cs.armor_pierce = armor[1];
            cs.armor_class = ur.get_named("class", ClassNames{catalog}, "units.toml (classes)");
            if (const toml::table* bonus = (*types)[i].as_table()->at_path("bonus").as_table()) {
                for (const auto& [key, node] : *bonus) {
                    const auto cls = catalog.find_class(key.str());
                    const auto v = node.value<std::int64_t>();
                    if (!cls || !v || *v < 0 || *v > kMaxAmount) {
                        ur.fail(std::format("'{}.{}' debe nombrar una clase de 'classes' con un entero >= 0",
                                            ur.full("bonus"), key.str()));
                        break;
                    }
                    cs.bonus[*cls] = static_cast<std::int32_t>(*v);
                }
            } else {
                ur.fail(std::format("falta la tabla '{}' (puede ser {{}})", ur.full("bonus")));
            }
            cs.range = sim::Fixed::from_ratio(ur.get_i32("range_milli_tiles", 0, 32 * kMilli), kMilli);
            cs.reload_ticks = ur.get_i32("reload_ticks", 1, kMaxTicks);
            cs.sight_tiles = ur.get_i32("sight_tiles", 0, 32);
            cs.projectile_speed =
                sim::Fixed::from_ratio(ur.get_i32("projectile_speed_milli_tiles_per_tick", 0, 4 * kMilli), kMilli);
            cs.auto_attack = ur.get_bool("auto_attack");
            cs.ignite = ur.get_i32("ignite", 0, kMaxAmount);
            cs.extinguish = ur.get_i32("extinguish", 0, kMaxAmount);
            cs.siege = ur.get_bool("siege");
            cs.buildings_only = ur.get_bool("buildings_only");
            cs.undermine = ur.get_bool("undermine");
            sim::SupplyStats& sup = info.type.supply;
            sup.rations = ur.get_i32("rations", 0, kMaxAmount);
            sup.ration_ticks = ur.get_i32("ration_ticks", 1, kMaxTicks);
            sup.starves = ur.get_bool("starves");
            sup.ammo = ur.get_i32("ammo", 0, kMaxAmount);
            sup.ammo_bundle = ur.get_i32("ammo_bundle", 1, kMaxAmount);
            sup.ammo_cost = ur.get_stock("ammo_cost");
            info.type.convoy_capacity = ur.get_i32("convoy_capacity", 0, kMaxAmount);
            info.type.treatable = ur.get_bool("treatable");
            info.type.care_skill = ur.get_i32("care_skill", 0, 1000);
            info.type.morale_resolve = ur.get_i32("morale_resolve", 0, 1000);
            info.type.rough_speed_percent = ur.get_i32("rough_speed_percent", 1, 100);
            info.type.charge_percent = ur.get_i32("charge_percent", 100, 1000);
            info.type.stamina = ur.get_i32("stamina", 0, 1000);
            info.type.climbs = ur.get_bool("climbs");
            if (!error && info.type.convoy_capacity > 0 && info.type.worker) {
                ur.fail("un aldeano no puede ser bagaje ('convoy_capacity' debe ser 0)");
            }
            if (!error && sup.ammo > 0 && sup.ammo_bundle > sup.ammo) {
                ur.fail("'ammo_bundle' no puede superar 'ammo'");
            }
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

std::expected<NodeCatalog, std::string> parse_node_catalog(std::string_view toml_text, std::string_view source_name) {
    auto root = parse_toml(toml_text, source_name);
    if (!root) {
        return std::unexpected(root.error());
    }
    std::optional<std::string> error;
    Reader r(*root, source_name, "", error);
    NodeCatalog catalog;
    const toml::array* list = r.get_table_array("node");
    if (list != nullptr && list->size() > static_cast<std::size_t>(kByteMax) + 1) {
        r.fail(std::format("hay {} tipos de nodo y el máximo es {}", list->size(), kByteMax + 1));
    }
    for_each_table(list, source_name, "node", error, [&](Reader& nr, std::size_t) {
        NodeInfo info;
        info.name = nr.get_string("name");
        info.type.kind = nr.get_resource("resource");
        info.type.amount = nr.get_i32("amount", 1, kMaxAmount);
        info.type.size = nr.get_i32("size_tiles", 1, kMaxFootprint);
        info.type.blocks_sight = nr.get_bool("blocks_sight");
        info.color = nr.get_color<3>("color");
        if (!nr.failed() && catalog.find(info.name)) {
            nr.fail(std::format("nombre de nodo repetido: \"{}\"", info.name));
        }
        catalog.types.push_back(std::move(info));
    });
    if (error) {
        return std::unexpected(*error);
    }
    return catalog;
}

std::expected<BuildingCatalog, std::string> parse_building_catalog(std::string_view toml_text,
                                                                   const UnitCatalog& units,
                                                                   std::string_view source_name) {
    auto root = parse_toml(toml_text, source_name);
    if (!root) {
        return std::unexpected(root.error());
    }
    std::optional<std::string> error;
    Reader r(*root, source_name, "", error);
    BuildingCatalog catalog;
    const toml::array* list = r.get_table_array("building");
    if (list != nullptr && list->size() > static_cast<std::size_t>(kByteMax) + 1) {
        r.fail(std::format("hay {} tipos de edificio y el máximo es {}", list->size(), kByteMax + 1));
    }
    std::vector<std::vector<std::string>> requires_names;  // se resuelven al final: pueden nombrar edificios posteriores
    for_each_table(list, source_name, "building", error, [&](Reader& br, std::size_t) {
        BuildingInfo info;
        requires_names.push_back(br.get_string_list("requires"));
        info.name = br.get_string("name");
        info.type.size = br.get_i32("size_tiles", 1, kMaxFootprint);
        info.type.cost = br.get_stock("cost");
        info.type.build_ticks = br.get_i32("build_ticks", 1, kMaxTicks);
        info.type.hp = br.get_i32("hp", 1, kMaxAmount);
        info.type.population = br.get_i32("population", 0, 1000);
        const auto armor = br.get_melee_pierce("armor");
        info.type.armor_melee = armor[0];
        info.type.armor_pierce = armor[1];
        info.type.armor_class = br.get_named("class", ClassNames{units}, "units.toml (classes)");
        info.type.farm_food = br.get_i32("farm_food", 0, kMaxAmount);
        info.type.vital = br.get_bool("vital");
        info.type.supplies = br.get_bool("supplies");
        info.type.store_capacity = br.get_i32("store_capacity", 0, kMaxAmount);
        info.type.store_target = br.get_stock("store_target");
        info.type.beds = br.get_i32("beds", 0, kMaxAmount);
        info.type.nurses = br.get_i32("nurses", 0, kMaxAmount);
        info.type.care_percent = br.get_i32("care_percent", 0, 1000);
        info.type.heal_to_percent = br.get_i32("heal_to_percent", 0, 100);
        info.type.sight_tiles = br.get_i32("sight_tiles", 0, 32);
        info.type.gate = br.get_bool("gate");
        info.type.garrison = br.get_i32("garrison", 0, 64);
        info.type.garrison_levels = br.get_i32("garrison_levels", 0, 8);
        info.type.climbable = br.get_bool("climbable");
        if (!error && info.type.beds > 0 && (info.type.care_percent <= 0 || info.type.heal_to_percent <= 0)) {
            br.fail("un puesto médico ('beds' > 0) necesita 'care_percent' y 'heal_to_percent' mayores que 0");
        }
        std::int64_t target_sum = 0;
        for (const std::int32_t v : info.type.store_target) {
            target_sum += v;
        }
        if (!error && target_sum > info.type.store_capacity) {
            br.fail("'store_target' no puede sumar más que 'store_capacity'");
        }
        if (!error && info.type.store_capacity > 0 && !info.type.supplies) {
            br.fail("un campamento ('store_capacity' > 0) debe abastecer ('supplies = true')");
        }
        const std::string material = br.get_string("material");
        if (material == "madera") {
            info.type.material = sim::Material::Wood;
        } else if (material == "piedra") {
            info.type.material = sim::Material::Stone;
        } else if (!br.failed()) {
            br.fail(std::format("'{}' = \"{}\": debe ser \"madera\" o \"piedra\"", br.full("material"), material));
        }
        for (const std::string& key : br.get_string_list("accepts")) {
            const auto res = find_resource(key);
            if (!res) {
                br.fail(std::format("'{}' contiene \"{}\", que no es un recurso", br.full("accepts"), key));
                return;
            }
            info.type.accepts = static_cast<std::uint8_t>(info.type.accepts | sim::resource_bit(*res));
        }
        for (const std::string& name : br.get_string_list("trains")) {
            const auto id = units.find(name);
            if (!id) {
                br.fail(std::format("'{}' contiene \"{}\", que no está en units.toml", br.full("trains"), name));
                return;
            }
            info.type.trains.push_back(*id);
        }
        info.color = br.get_color<3>("color");
        if (!br.failed() && catalog.find(info.name)) {
            br.fail(std::format("nombre de edificio repetido: \"{}\"", info.name));
        }
        catalog.types.push_back(std::move(info));
    });
    for (std::size_t i = 0; i < requires_names.size() && !error; ++i) {
        for (const std::string& name : requires_names[i]) {
            const auto id = catalog.find(name);
            if (!id || *id == i) {
                r.fail(std::format("building[{}].requires contiene \"{}\", que no es otro edificio de la lista", i, name));
                break;
            }
            catalog.types[i].type.required.push_back(*id);
        }
    }
    // Sin ciclos: un edificio que (directa o indirectamente) se exige a sí mismo nunca
    // se podría colocar. Búsqueda en profundidad con colores.
    std::vector<std::uint8_t> state(catalog.types.size(), 0);  // 0 sin ver, 1 en curso, 2 hecho
    const auto cyclic = [&](auto&& self, std::size_t b) -> bool {
        if (state[b] == 1) {
            return true;
        }
        if (state[b] == 2) {
            return false;
        }
        state[b] = 1;
        for (const sim::BuildingTypeId n : catalog.types[b].type.required) {
            if (self(self, n)) {
                return true;
            }
        }
        state[b] = 2;
        return false;
    };
    for (std::size_t b = 0; b < catalog.types.size() && !error; ++b) {
        if (cyclic(cyclic, b)) {
            r.fail(std::format("los requisitos (requires) de \"{}\" forman un ciclo", catalog.types[b].name));
        }
    }
    // Mejoras (C1): [[upgrade]], opcional. requires nombra otra mejora anterior.
    const toml::array* ups = r.get_table_array("upgrade", false);
    if (ups != nullptr && ups->size() > static_cast<std::size_t>(kByteMax) + 1) {
        r.fail(std::format("hay {} mejoras y el máximo es {}", ups->size(), kByteMax + 1));
    }
    for_each_table(ups, source_name, "upgrade", error, [&](Reader& ur, std::size_t) {
        UpgradeInfo info;
        info.name = ur.get_string("name");
        info.label = ur.get_string("label");
        const std::string at = ur.get_string("at");
        const auto at_id = catalog.find(at);
        if (!at_id) {
            ur.fail(std::format("'{}' = \"{}\" no es un edificio", ur.full("at"), at));
            return;
        }
        info.type.at = *at_id;
        info.type.cost = ur.get_stock("cost");
        info.type.research_ticks = ur.get_i32("research_ticks", 1, kMaxTicks);
        const std::string req = ur.get_string("requires");
        if (!req.empty()) {
            const auto it = std::ranges::find(catalog.upgrades, req, &UpgradeInfo::name);
            if (it == catalog.upgrades.end()) {
                ur.fail(std::format("'{}' = \"{}\" no es una mejora anterior", ur.full("requires"), req));
                return;
            }
            info.type.requires_upgrade = static_cast<sim::UpgradeId>(it - catalog.upgrades.begin());
        }
        for (const std::string& cls : ur.get_string_list("classes")) {
            const auto c = std::ranges::find(units.classes, cls);
            if (c == units.classes.end()) {
                ur.fail(std::format("'{}' contiene \"{}\", que no es una clase de units.toml", ur.full("classes"), cls));
                return;
            }
            info.type.classes |= 1U << static_cast<std::uint32_t>(c - units.classes.begin());
        }
        info.type.attack_melee = ur.get_i32("attack_melee", 0, kMaxAmount);
        info.type.attack_pierce = ur.get_i32("attack_pierce", 0, kMaxAmount);
        info.type.armor_melee = ur.get_i32("armor_melee", 0, kMaxAmount);
        info.type.armor_pierce = ur.get_i32("armor_pierce", 0, kMaxAmount);
        if (!ur.failed() && std::ranges::find(catalog.upgrades, info.name, &UpgradeInfo::name) != catalog.upgrades.end()) {
            ur.fail(std::format("nombre de mejora repetido: \"{}\"", info.name));
        }
        catalog.upgrades.push_back(std::move(info));
    });
    if (error) {
        return std::unexpected(*error);
    }
    return catalog;
}

std::expected<Scenario, std::string> parse_scenario(std::string_view toml_text, const Catalogs& catalogs,
                                                    std::string_view source_name) {
    auto root = parse_toml(toml_text, source_name);
    if (!root) {
        return std::unexpected(root.error());
    }
    std::optional<std::string> error;
    Reader r(*root, source_name, "", error);
    Scenario scenario;
    scenario.build_gap_tiles = r.get_i32("build_site.gap_tiles", 0, 64);
    scenario.build_search_radius_tiles = r.get_i32("build_site.search_radius_tiles", 1, 256);
    std::vector<ScenarioOrder>& orders = scenario.orders;
    for_each_table(r.get_table_array("order"), source_name, "order", error, [&](Reader& orr, std::size_t) {
        ScenarioOrder o;
        o.tick = static_cast<sim::Tick>(orr.get_i32("tick", 0, std::numeric_limits<std::int32_t>::max()));
        o.player = static_cast<sim::PlayerId>(orr.get_i32("player", 0, kMaxPlayers - 1));
        const std::string action = orr.get_string("action");
        if (action == "mover") {
            o.action = ScenarioAction::Move;
            o.target = orr.get_tile("target");
        } else if (action == "recoger") {
            o.action = ScenarioAction::Gather;
            o.resource = orr.get_resource("resource");
        } else if (action == "construir") {
            o.action = ScenarioAction::Build;
            o.building = orr.get_named("building", catalogs.buildings, "buildings.toml");
        } else if (action == "entrenar") {
            o.action = ScenarioAction::Train;
            o.building = orr.get_named("building", catalogs.buildings, "buildings.toml");
            o.unit = orr.get_named("unit", catalogs.units, "units.toml");
        } else if (!orr.failed()) {
            orr.fail(std::format("'{}' = \"{}\" no es una acción (mover, recoger, construir, entrenar)",
                                 orr.full("action"), action));
        }
        // entrenar no elige unidades: count es cuántas se encolan.
        o.first = o.action == ScenarioAction::Train ? 0 : orr.get_i32("first", 0, 1'000'000);
        o.count = orr.get_i32("count", 1, 1'000'000);
        if (!orders.empty() && o.tick < orders.back().tick) {
            orr.fail(std::format("'{}' debe ser >= que el de la orden anterior", orr.full("tick")));
        }
        orders.push_back(o);
    });
    if (error) {
        return std::unexpected(*error);
    }
    return scenario;
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
            info.speed_percent = tr.get_i32("speed_percent", 1, 100);
            info.arrow_cover_percent = tr.get_i32("arrow_cover_percent", 0, 100);
            info.charge = tr.get_bool("charge");
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

std::expected<EngineConfig, std::string> parse_engine_config(std::string_view toml_text, const Catalogs& catalogs,
                                                             std::string_view source_name) {
    const TerrainCatalog& terrain = catalogs.terrain;
    const UnitCatalog& units = catalogs.units;
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
    // Terreno en combate (B2): desactivado, ni la marcha ni el combate lo miran.
    sim::TerrainCombatParams& tc = cfg.world.combat.terrain;
    tc.enabled = r.get_bool("terrain.enabled");
    tc.range_per_level =
        sim::Fixed::from_ratio(r.get_i32("terrain.range_per_level_milli_tiles", 0, 4 * kMilli), kMilli);
    tc.max_levels = r.get_i32("terrain.max_levels", 0, 16);
    tc.ranged_percent_per_level = r.get_i32("terrain.ranged_percent_per_level", 0, 50);
    tc.melee_percent_per_level = r.get_i32("terrain.melee_percent_per_level", 0, 50);
    tc.charge_run_ticks = r.get_i32("terrain.charge_run_ticks", 1, kMaxTicks);
    if (tc.enabled) {
        for (const TerrainInfo& t : terrain.types) {
            cfg.world.movement.speed_percent_by_terrain.push_back(t.speed_percent);
            tc.arrow_cover_percent_by_terrain.push_back(t.arrow_cover_percent);
            tc.charge_by_terrain.push_back(t.charge ? 1 : 0);
        }
    }
    for (const UnitInfo& u : units.types) {
        cfg.world.unit_types.push_back(u.type);
    }
    for (const BuildingInfo& b : catalogs.buildings.types) {
        cfg.world.building_types.push_back(b.type);
    }
    for (const UpgradeInfo& u : catalogs.buildings.upgrades) {
        cfg.world.upgrades.push_back(u.type);
    }
    for (const NodeInfo& n : catalogs.nodes.types) {
        cfg.world.node_types.push_back(n.type);
    }

    // Jugadores: inicio (simulación) y color (presentación).
    std::vector<std::pair<std::string, std::string>> ai_profile_names;  // nombre, clave (para errores)
    for_each_table(r.get_table_array("player"), source_name, "player", error, [&](Reader& pr, std::size_t i) {
        if (i >= static_cast<std::size_t>(kMaxPlayers)) {
            pr.fail(std::format("hay más de {} jugadores", kMaxPlayers));
            return;
        }
        const sim::TileCoord start = pr.get_tile("start");
        if (!pr.failed() && (start.x >= cfg.world.map.width || start.y >= cfg.world.map.height)) {
            pr.fail(std::format("'{}' queda fuera del mapa", pr.full("start")));
        }
        cfg.world.setup.starts.push_back(start);
        cfg.player_colors.push_back(pr.get_color<3>("color"));
        const std::string controller = pr.get_string("controller");
        if (controller == "ia") {
            // El perfil se resuelve cuando se hayan leído los de [ai].
            ai_profile_names.emplace_back(pr.get_string("ai_profile"), pr.full("ai_profile"));
            cfg.world.ai_players.push_back({static_cast<sim::PlayerId>(i), 0});
        } else if (controller != "humano" && !pr.failed()) {
            pr.fail(std::format("'{}' = \"{}\": debe ser \"humano\" o \"ia\"", pr.full("controller"), controller));
        }
    });

    sim::DemoParams& demo = cfg.world.demo;
    demo.seed = r.get_u64("demo.seed");
    demo.unit_type = r.get_named("demo.unit", units, "units.toml");
    demo.player = static_cast<sim::PlayerId>(r.get_i32("demo.player", 0, kMaxPlayers - 1));
    demo.count = r.get_i32("demo.count", 0, 100'000);
    demo.area_tiles = r.get_i32("demo.area_tiles", 2, 1024);

    sim::EconomyParams& eco = cfg.world.economy;
    const sim::Stock gather = r.get_stock("economy.gather_ticks");
    for (std::size_t i = 0; i < sim::kResourceCount; ++i) {
        eco.gather_ticks[i] = gather[i];
        if (!error && gather[i] < 1) {
            r.fail(std::format("'economy.gather_ticks.{}' debe ser al menos 1", kResourceKeys[i]));
        }
    }
    eco.interact_range = sim::Fixed::from_ratio(r.get_i32("economy.interact_range_milli_tiles", 0, 4 * kMilli), kMilli);
    eco.retarget_radius_tiles = r.get_i32("economy.retarget_radius_tiles", 0, 64);
    eco.approach_attempts = r.get_i32("economy.approach_attempts", 1, 32);
    eco.gatherers_per_tile = r.get_i32("economy.gatherers_per_tile", 1, 8);
    eco.queue_capacity = r.get_i32("economy.queue_capacity", 1, 64);
    eco.max_population = r.get_i32("economy.max_population", 1, 100'000);
    eco.spawn_search_radius = r.get_i32("economy.spawn_search_radius_tiles", 1, 32);
    eco.repair_cost_percent = r.get_i32("economy.repair_cost_percent", 0, 1000);
    eco.salvage_percent = r.get_i32("economy.salvage_percent", 0, 100);
    eco.rubble_stone = r.get_named("economy.rubble_stone", catalogs.nodes, "resources.toml");
    eco.rubble_wood = r.get_named("economy.rubble_wood", catalogs.nodes, "resources.toml");
    if (!error && (catalogs.nodes.types[eco.rubble_stone].type.kind != sim::Resource::Stone ||
                   catalogs.nodes.types[eco.rubble_wood].type.kind != sim::Resource::Wood)) {
        r.fail("'economy.rubble_stone' debe ser un nodo de piedra y 'economy.rubble_wood', uno de madera");
    }

    sim::FireParams& fp = cfg.world.fire;
    fp.max_intensity = r.get_i32("fire.max_intensity", 1, kMaxAmount);
    fp.sustain_intensity = r.get_i32("fire.sustain_intensity", 1, kMaxAmount);
    fp.decay_per_tick = r.get_i32("fire.decay_per_tick", 1, kMaxAmount);
    fp.growth_wood_per_tick = r.get_i32("fire.growth_wood_per_tick", 0, kMaxAmount);
    fp.growth_stone_per_tick = r.get_i32("fire.growth_stone_per_tick", 0, kMaxAmount);
    fp.burn_wood_milli_per_tick = r.get_i32("fire.burn_wood_milli_hp_per_tick", 0, kMaxAmount);
    fp.burn_stone_milli_per_tick = r.get_i32("fire.burn_stone_milli_hp_per_tick", 0, kMaxAmount);
    fp.stone_floor_percent = r.get_i32("fire.stone_floor_percent", 0, 100);
    fp.spread_intensity = r.get_i32("fire.spread_intensity", 0, kMaxAmount);
    fp.spread_per_tick = r.get_i32("fire.spread_per_tick", 0, kMaxAmount);
    fp.spread_gap_tiles = r.get_i32("fire.spread_gap_tiles", 0, 8);
    fp.extinguish_reach = sim::Fixed::from_ratio(r.get_i32("fire.extinguish_reach_milli_tiles", 0, 4 * kMilli), kMilli);
    if (!error && (fp.sustain_intensity > fp.max_intensity || fp.spread_intensity > fp.max_intensity)) {
        r.fail("'fire.sustain_intensity' y 'fire.spread_intensity' no pueden superar 'fire.max_intensity'");
    }

    sim::SupplyParams& sp = cfg.world.supply;
    sp.resupply_radius_tiles = r.get_i32("supply.resupply_radius_tiles", 0, 32);
    sp.resupply_interval_ticks = r.get_i32("supply.resupply_interval_ticks", 1, kMaxTicks);
    sp.ration_cost = r.get_stock("supply.ration_cost");
    sp.starve_after_ticks = r.get_i32("supply.starve_after_ticks", 0, kMaxTicks);
    sp.starve_hp_interval_ticks = r.get_i32("supply.starve_hp_interval_ticks", 1, kMaxTicks);
    eco.hungry_work_percent = r.get_i32("supply.hungry_work_percent", 0, 100);
    sp.convoy_mix = r.get_stock("supply.convoy_mix");
    std::int32_t mix_sum = 0;
    for (const std::int32_t v : sp.convoy_mix) {
        mix_sum += v;
    }
    if (!error && mix_sum != sim::kPercent) {
        r.fail("'supply.convoy_mix' debe sumar 100");
    }
    sp.load_ticks = r.get_i32("supply.load_ticks", 0, kMaxTicks);
    sp.convoy_reach = sim::Fixed::from_ratio(r.get_i32("supply.convoy_reach_milli_tiles", 0, 4 * kMilli), kMilli);

    cfg.alerts.cooldown_ticks = r.get_i32("alerts.cooldown_ticks", 0, kMaxTicks);
    cfg.alerts.zone_tiles = r.get_i32("alerts.zone_tiles", 1, 256);
    cfg.alerts.show_ticks = r.get_i32("alerts.show_ticks", 1, kMaxTicks);
    cfg.alerts.max_shown = r.get_i32("alerts.max_shown", 1, 16);

    sim::VisionParams& vp = cfg.world.vision;
    vp.enabled = r.get_bool("vision.enabled");
    vp.interval_ticks = r.get_i32("vision.interval_ticks", 1, kMaxTicks);
    vp.elevation_sight_per_level = r.get_i32("vision.elevation_sight_per_level", 0, 8);
    vp.cover_depth_tiles = r.get_i32("vision.cover_depth_tiles", 0, 32);
    vp.spot_in_cover_tiles = r.get_i32("vision.spot_in_cover_tiles", 0, 32);
    vp.day_ticks = r.get_i32("vision.day_ticks", 1, kMaxTicks);
    vp.night_ticks = r.get_i32("vision.night_ticks", 0, kMaxTicks);
    vp.twilight_ticks = r.get_i32("vision.twilight_ticks", 0, kMaxTicks);
    vp.night_sight_percent = r.get_i32("vision.night_sight_percent", 1, 100);
    vp.start_tick = r.get_i32("vision.start_tick", 0, kMaxTicks);
    if (!error && 2 * vp.twilight_ticks > vp.day_ticks) {
        r.fail("'vision.twilight_ticks' no puede pasar de la mitad de 'vision.day_ticks'");
    }

    sim::MedicineParams& md = cfg.world.medicine;
    md.bed_heal_milli_per_tick = r.get_i32("medicine.bed_heal_milli_hp_per_tick", 0, kMaxAmount);
    md.nurse_heal_milli_per_tick = r.get_i32("medicine.nurse_heal_milli_hp_per_tick", 0, kMaxAmount);
    md.patients_per_nurse = r.get_i32("medicine.patients_per_nurse", 1, 1000);
    md.patient_ration_ticks = r.get_i32("medicine.patient_ration_ticks", 1, kMaxTicks);
    md.reorganize_ticks = r.get_i32("medicine.reorganize_ticks", 0, kMaxTicks);
    md.care_reach = sim::Fixed::from_ratio(r.get_i32("medicine.care_reach_milli_tiles", 0, 4 * kMilli), kMilli);
    md.light_wound_percent = r.get_i32("medicine.light_wound_percent", 0, 100);
    md.natural_heal_interval_ticks = r.get_i32("medicine.natural_heal_interval_ticks", 0, kMaxTicks);
    md.calm_ticks = r.get_i32("medicine.calm_ticks", 0, kMaxTicks);

    cfg.world.garrison.enter_reach =
        sim::Fixed::from_ratio(r.get_i32("garrison.enter_reach_milli_tiles", 0, 4 * kMilli), kMilli);

    sim::ClimbParams& cl = cfg.world.climb;
    cl.ladder_cost = r.get_stock("climb.ladder_cost");
    cl.climb_ticks = r.get_i32("climb.climb_ticks", 1, kMaxTicks);
    cl.reach = sim::Fixed::from_ratio(r.get_i32("climb.reach_milli_tiles", 0, 4 * kMilli), kMilli);
    cl.exposed_percent = r.get_i32("climb.exposed_percent", 100, 1000);
    cfg.world.combat.climb_exposed_percent = cl.exposed_percent;
    cfg.world.ai.ladder_cost = cl.ladder_cost;

    sim::FormationParams& fo = cfg.world.formation;
    fo.enabled = r.get_bool("formation.enabled");
    fo.min_members = r.get_i32("formation.min_members", 1, 1000);
    fo.cohesion_radius = sim::Fixed::from_ratio(r.get_i32("formation.cohesion_milli_tiles", 0, 32 * kMilli), kMilli);
    fo.spacing = sim::Fixed::from_ratio(r.get_i32("formation.spacing_milli_tiles", 250, 4 * kMilli), kMilli);
    const auto effect = [&](const std::string& name) {
        sim::FormationEffect fx;
        const std::string k = "formation." + name + ".";
        fx.speed_percent = r.get_i32(k + "speed_percent", 1, 300);
        fx.melee_attack_percent = r.get_i32(k + "melee_attack_percent", 0, 300);
        fx.ranged_attack_percent = r.get_i32(k + "ranged_attack_percent", 0, 300);
        fx.cavalry_taken_percent = r.get_i32(k + "cavalry_taken_percent", 0, 300);
        fx.ranged_taken_percent = r.get_i32(k + "ranged_taken_percent", 0, 300);
        fx.stops_charge = r.get_bool(k + "stops_charge");
        return fx;
    };
    fo.cavalry_class = r.get_named("formation.cavalry_class", ClassNames{units}, "units.toml (classes)");
    cfg.world.combat.cavalry_class = fo.cavalry_class;
    fo.line = effect("line");
    fo.column = effect("column");
    fo.square = effect("square");

    sim::FatigueParams& fa = cfg.world.fatigue;
    fa.enabled = r.get_bool("fatigue.enabled");
    fa.march_per_tick = r.get_i32("fatigue.march_per_tick", 0, sim::kFullFatigue);
    fa.strike = r.get_i32("fatigue.strike", 0, sim::kFullFatigue);
    fa.rest_per_tick = r.get_i32("fatigue.rest_per_tick", 0, sim::kFullFatigue);
    fa.forced_speed_percent = r.get_i32("fatigue.forced_speed_percent", 100, 200);
    fa.forced_fatigue_percent = r.get_i32("fatigue.forced_fatigue_percent", 100, 1000);
    fa.exhausted_speed_percent = r.get_i32("fatigue.exhausted_speed_percent", 1, 100);
    fa.exhausted_attack_percent = r.get_i32("fatigue.exhausted_attack_percent", 1, 100);

    sim::MoraleParams& mo = cfg.world.morale;
    const auto tiles = [&](const char* key) {
        return sim::Fixed::from_ratio(r.get_i32(key, 0, 64 * kMilli), kMilli);
    };
    mo.enabled = r.get_bool("morale.enabled");
    mo.interval_ticks = r.get_i32("morale.interval_ticks", 1, kMaxTicks);
    mo.rout_below = r.get_i32("morale.rout_below", 0, sim::kFullMorale);
    mo.rally_above = r.get_i32("morale.rally_above", 0, sim::kFullMorale);
    mo.awareness_radius = tiles("morale.awareness_milli_tiles");
    mo.rally_safe_radius = tiles("morale.rally_safe_milli_tiles");
    mo.casualty_loss = r.get_i32("morale.casualty_loss", 0, sim::kFullMorale);
    mo.enemy_casualty_gain = r.get_i32("morale.enemy_casualty_gain", 0, sim::kFullMorale);
    mo.hit_loss = r.get_i32("morale.hit_loss", 0, sim::kFullMorale);
    mo.flank_hit_loss = r.get_i32("morale.flank_hit_loss", 0, sim::kFullMorale);
    mo.hero_death_loss = r.get_i32("morale.hero_death_loss", 0, sim::kFullMorale);
    mo.hero_death_radius = tiles("morale.hero_death_milli_tiles");
    mo.contagion_loss = r.get_i32("morale.contagion_loss", 0, sim::kFullMorale);
    mo.outnumbered_loss = r.get_i32("morale.outnumbered_loss", 0, sim::kFullMorale);
    mo.hungry_loss = r.get_i32("morale.hungry_loss", 0, sim::kFullMorale);
    mo.calm_ticks = r.get_i32("morale.calm_ticks", 0, kMaxTicks);
    mo.calm_gain = r.get_i32("morale.calm_gain", 0, sim::kFullMorale);
    mo.comrade_gain = r.get_i32("morale.comrade_gain", 0, sim::kFullMorale);
    mo.comrade_cap = r.get_i32("morale.comrade_cap", 0, 1000);
    mo.hero_gain = r.get_i32("morale.hero_gain", 0, sim::kFullMorale);
    mo.hero_loss_percent = r.get_i32("morale.hero_loss_percent", 0, 100);
    mo.night_loss_percent = r.get_i32("morale.night_loss_percent", 100, 1000);
    mo.routing_gain = r.get_i32("morale.routing_gain", 0, sim::kFullMorale);
    mo.flee_tiles = r.get_i32("morale.flee_tiles", 1, 64);
    mo.flee_repath_ticks = r.get_i32("morale.flee_repath_ticks", 1, kMaxTicks);
    mo.rally_reorganize_ticks = r.get_i32("morale.rally_reorganize_ticks", 0, kMaxTicks);
    if (!error && mo.rally_above <= mo.rout_below) {
        r.fail("'morale.rally_above' debe ser mayor que 'morale.rout_below'");
    }

    sim::CombatParams& cb = cfg.world.combat;
    cb.acquire_interval_ticks = r.get_i32("combat.acquire_interval_ticks", 1, 1000);
    cb.repath_tiles = r.get_i32("combat.repath_tiles", 1, 64);
    cb.chase_attempts = r.get_i32("combat.chase_attempts", 1, 64);
    cb.building_reach = sim::Fixed::from_ratio(r.get_i32("combat.building_reach_milli_tiles", 0, 4 * kMilli), kMilli);
    cb.projectile_hit_radius =
        sim::Fixed::from_ratio(r.get_i32("combat.projectile_hit_radius_milli_tiles", 0, kMilli), kMilli);
    cb.xp_kill_bonus = r.get_i32("combat.xp_kill_bonus", 0, kMaxAmount);
    cb.level_thresholds = r.get_increasing_list("combat.level_thresholds", 64);
    cb.hp_percent_per_level = r.get_i32("combat.hp_percent_per_level", 0, 1000);
    cb.attack_percent_per_level = r.get_i32("combat.attack_percent_per_level", 0, 1000);
    cb.armor_every_levels = r.get_i32("combat.armor_every_levels", 0, 64);
    cb.hero_aura_radius =
        sim::Fixed::from_ratio(r.get_i32("combat.hero_aura_radius_milli_tiles", 0, 32 * kMilli), kMilli);
    cb.hero_aura_attack_percent = r.get_i32("combat.hero_aura_attack_percent", 0, 1000);
    cb.hungry_attack_percent = r.get_i32("supply.hungry_attack_percent", 0, 100);
    cfg.hero_names = r.get_string_list("combat.hero_names");
    {
        constexpr std::array<std::pair<std::string_view, sim::TargetClass>, 6> kTargetClasses{{
            {"me_ataca", sim::TargetClass::AttackingMe},
            {"armada", sim::TargetClass::Armed},
            {"asedio", sim::TargetClass::Siege},
            {"bagaje", sim::TargetClass::Carrier},
            {"aldeano", sim::TargetClass::Worker},
            {"otra", sim::TargetClass::Other},
        }};
        for (const std::string& name : r.get_string_list("combat.target_priority")) {
            const auto it = std::ranges::find(kTargetClasses, name, &std::pair<std::string_view, sim::TargetClass>::first);
            if (it == kTargetClasses.end()) {
                r.fail(std::format("'combat.target_priority' contiene \"{}\"; valen: me_ataca, armada, asedio, "
                                   "bagaje, aldeano, otra",
                                   name));
                break;
            }
            if (std::ranges::find(cb.target_priority, it->second) != cb.target_priority.end()) {
                r.fail(std::format("'combat.target_priority' repite \"{}\"", name));
                break;
            }
            cb.target_priority.push_back(it->second);
        }
    }
    cb.hero_name_count = static_cast<std::int32_t>(cfg.hero_names.size());

    sim::AiParams& ai = cfg.world.ai;
    ai.think_interval_ticks = r.get_i32("ai.think_interval_ticks", 1, 1000);
    ai.enemy_memory_decay_permille = r.get_i32("ai.enemy_memory_decay_permille", 0, 1000);
    ai.worker_type = r.get_named("ai.worker", units, "units.toml");
    ai.house = r.get_named("ai.house", catalogs.buildings, "buildings.toml");
    ai.barracks = r.get_named("ai.barracks", catalogs.buildings, "buildings.toml");
    ai.farm = r.get_named("ai.farm", catalogs.buildings, "buildings.toml");
    ai.workshop = r.get_named("ai.workshop", catalogs.buildings, "buildings.toml");
    // Logística (módulo logistica): "" en los dos = sin campamentos ni convoyes.
    const std::string camp = r.get_string("ai.camp");
    const std::string carrier = r.get_string("ai.carrier");
    if (!error && camp.empty() != carrier.empty()) {
        r.fail("'ai.camp' y 'ai.carrier' van juntos: los dos con nombre o los dos vacíos");
    }
    if (const std::string engine = r.get_string("ai.siege_engine"); !error && !engine.empty()) {
        ai.siege_engine = units.find(engine);
        if (!ai.siege_engine) {
            r.fail(std::format("'ai.siege_engine' = \"{}\": no está en units.toml", engine));
        }
    }
    if (!error && !camp.empty()) {
        ai.camp = catalogs.buildings.find(camp);
        ai.carrier = units.find(carrier);
        if (!ai.camp || catalogs.buildings.types[*ai.camp].type.store_capacity <= 0 || !ai.carrier ||
            units.types[*ai.carrier].type.convoy_capacity <= 0) {
            r.fail("'ai.camp' debe nombrar un campamento (store_capacity > 0) y 'ai.carrier', un bagaje "
                   "(convoy_capacity > 0)");
        }
    }
    for (std::size_t i = 0; i < sim::kResourceCount; ++i) {
        ai.dropoff[i] = r.get_named(std::format("ai.dropoff.{}", kResourceKeys[i]), catalogs.buildings, "buildings.toml");
    }
    std::vector<std::string> profile_names;
    for_each_table(r.get_table_array("ai.profile"), source_name, "ai.profile", error, [&](Reader& pr, std::size_t) {
        if (profile_names.size() >= kMaxAiProfiles) {
            pr.fail(std::format("hay más de {} perfiles de IA", kMaxAiProfiles));
            return;
        }
        sim::AiProfile p;
        profile_names.push_back(pr.get_string("name"));
        for (const std::string& name : pr.get_string_list("behaviors")) {
            const auto it = std::ranges::find(kAiBehaviorNames, name);
            if (it == kAiBehaviorNames.end()) {
                pr.fail(std::format("'{}' contiene \"{}\", que no es un módulo de IA", pr.full("behaviors"), name));
                return;
            }
            p.behaviors.push_back(static_cast<sim::AiBehavior>(it - kAiBehaviorNames.begin()));
        }
        p.villager_target = pr.get_i32("villager_target", 0, 1000);
        const sim::Stock gather_pct = pr.get_stock("gather_percent");
        std::int32_t pct_sum = 0;
        for (std::size_t i = 0; i < sim::kResourceCount; ++i) {
            p.gather_percent[i] = gather_pct[i];
            pct_sum += gather_pct[i];
        }
        if (!pr.failed() && pct_sum != 100) {
            pr.fail(std::format("'{}' debe sumar 100 (suma {})", pr.full("gather_percent"), pct_sum));
        }
        p.house_margin = pr.get_i32("house_margin", 0, 100);
        p.barracks_at_villagers = pr.get_i32("barracks_at_villagers", 0, 1000);
        p.dropoff_distance_tiles = pr.get_i32("dropoff_distance_tiles", 1, 256);
        p.dropoff_min_gatherers = pr.get_i32("dropoff_min_gatherers", 1, 1000);
        p.builders = pr.get_i32("builders", 1, 100);
        p.gatherers_per_farm = pr.get_i32("gatherers_per_farm", 1, 64);
        p.build_gap_tiles = pr.get_i32("build_gap_tiles", 0, 8);
        p.build_search_radius_tiles = pr.get_i32("build_search_radius_tiles", 1, 128);
        p.first_wave = pr.get_i32("first_wave", 1, 10'000);
        p.wave_growth = pr.get_i32("wave_growth", 0, 10'000);
        p.defend_radius_tiles = pr.get_i32("defend_radius_tiles", 1, 256);
        p.flee_enemy_tiles = pr.get_i32("flee_enemy_tiles", 0, 256);
        p.safe_base_tiles = pr.get_i32("safe_base_tiles", 0, 256);
        p.barracks_queue = pr.get_i32("barracks_queue", 1, 64);
        p.villager_queue = pr.get_i32("villager_queue", 1, 64);
        p.attack_ratio_percent = pr.get_i32("attack_ratio_percent", 0, 10'000);
        p.retreat_ratio_percent = pr.get_i32("retreat_ratio_percent", 0, 10'000);
        p.min_attack_army = pr.get_i32("min_attack_army", 1, 10'000);
        p.engage_radius_tiles = pr.get_i32("engage_radius_tiles", 1, 256);
        p.extinguishers_per_fire = pr.get_i32("extinguishers_per_fire", 0, 64);
        p.army_min_villagers = pr.get_i32("army_min_villagers", 0, 1000);
        if (const std::string raider = pr.get_string("raid_unit"); !raider.empty()) {
            p.raid_unit = units.find(raider);
            if (!p.raid_unit && !pr.failed()) {
                pr.fail(std::format("'{}' = \"{}\": no está en units.toml", pr.full("raid_unit"), raider));
            }
        }
        p.raid_group = pr.get_i32("raid_group", 1, 1000);
        p.raid_safe_radius_tiles = pr.get_i32("raid_safe_radius_tiles", 0, 256);
        p.resupply_percent = pr.get_i32("resupply_percent", 0, 100);
        p.upkeep_reserve_percent = pr.get_i32("upkeep_reserve_percent", 0, 1000);
        p.camp_distance_tiles = pr.get_i32("camp_distance_tiles", 0, 1024);
        p.camp_offset_tiles = pr.get_i32("camp_offset_tiles", 0, 1024);
        p.convoy_carriers = pr.get_i32("convoy_carriers", 0, 64);
        p.baggage_offset_tiles = pr.get_i32("baggage_offset_tiles", 0, 64);
        p.siege_engines = pr.get_i32("siege_engines", 0, 64);
        p.siege_front_tiles = pr.get_i32("siege_front_tiles", 0, 1024);
        p.scouts = pr.get_i32("scouts", 0, 64);
        p.fog_guard_army = pr.get_i32("fog_guard_army", 0, 1000);
        p.explore_step_tiles = pr.get_i32("explore_step_tiles", 1, 256);
        for (const std::string& name : pr.get_string_list("army")) {
            if (const auto id = units.find(name)) {
                p.army.push_back(*id);
            } else if (!pr.failed()) {
                pr.fail(std::format("'{}' contiene \"{}\", que no está en units.toml", pr.full("army"), name));
            }
        }
        ai.profiles.push_back(std::move(p));
    });
    cfg.ai_profile_names = profile_names;
    for (std::size_t k = 0; k < ai_profile_names.size() && !error; ++k) {
        const auto& [name, key] = ai_profile_names[k];
        const auto it = std::ranges::find(profile_names, name);
        if (it == profile_names.end()) {
            r.fail(std::format("'{}' = \"{}\": no hay ningún [[ai.profile]] con ese nombre", key, name));
        } else {
            cfg.world.ai_players[k].profile = static_cast<std::uint8_t>(it - profile_names.begin());
        }
    }

    sim::SetupParams& setup = cfg.world.setup;
    setup.seed = r.get_u64("setup.seed");
    setup.start_search_radius = r.get_i32("setup.start_search_radius_tiles", 0, 1024);
    setup.min_start_region_tiles = r.get_i32("setup.min_start_region_tiles", 0, 1 << 20);
    setup.start_building = r.get_named("setup.start_building", catalogs.buildings, "buildings.toml");
    setup.start_unit = r.get_named("setup.start_unit", units, "units.toml");
    setup.start_units = r.get_i32("setup.start_units", 0, 1000);
    setup.start_stock = r.get_stock("setup.start_stock");
    setup.forest_terrain = r.get_named("setup.forest_terrain", terrain, "terrain.toml");
    setup.tree_type = r.get_named("setup.tree", catalogs.nodes, "resources.toml");
    setup.tree_density_permille = r.get_i32("setup.tree_density_permille", 0, kMilli);
    setup.clear_radius = r.get_i32("setup.clear_radius_tiles", 0, 1024);
    for_each_table(r.get_table_array("setup.near_start", false), source_name, "setup.near_start", error,
                   [&](Reader& nr, std::size_t) {
                       sim::StartNodes group;
                       group.type = nr.get_named("node", catalogs.nodes, "resources.toml");
                       group.count = nr.get_i32("count", 0, 1000);
                       group.min_distance = nr.get_i32("min_distance_tiles", 0, 1024);
                       group.max_distance = nr.get_i32("max_distance_tiles", group.min_distance, 1024);
                       setup.near_start.push_back(group);
                   });

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
    cfg.view.building_body_percent = r.get_i32("view.building_body_percent", 1, 100);
    cfg.view.node_body_percent = r.get_i32("view.node_body_percent", 1, 100);
    cfg.view.construction_shade_percent = r.get_i32("view.construction_shade_percent", 0, 100);
    cfg.view.ghost_valid_color = r.get_color<4>("view.ghost_valid_color");
    cfg.view.ghost_invalid_color = r.get_color<4>("view.ghost_invalid_color");
    for (std::size_t i = 0; i < sim::kResourceCount; ++i) {
        cfg.view.resource_colors[i] = r.get_color<4>(std::format("view.resource_colors.{}", kResourceKeys[i]));
    }
    cfg.view.health_bar_width_px = r.get_i32("view.health_bar_width_px", 1, 256);
    cfg.view.health_bar_height_px = r.get_i32("view.health_bar_height_px", 1, 64);
    cfg.view.health_low_permille = r.get_i32("view.health_low_permille", 0, kMilli);
    cfg.view.health_back_color = r.get_color<4>("view.health_back_color");
    cfg.view.health_color = r.get_color<4>("view.health_color");
    cfg.view.health_low_color = r.get_color<4>("view.health_low_color");
    cfg.view.projectile_color = r.get_color<4>("view.projectile_color");
    cfg.view.hero_color = r.get_color<4>("view.hero_color");
    cfg.view.fire_color = r.get_color<4>("view.fire_color");
    cfg.view.burned_shade_percent = r.get_i32("view.burned_shade_percent", 0, 100);
    cfg.view.fog_unexplored_color = r.get_color<4>("view.fog_unexplored_color");
    cfg.view.fog_explored_shade_percent = r.get_i32("view.fog_explored_shade_percent", 0, 100);
    cfg.view.night_color = r.get_color<4>("view.night_color");
    cfg.view.minimap_width_px = r.get_i32("view.minimap_width_px", 32, 1024);
    cfg.view.minimap_cells = r.get_i32("view.minimap_cells", 8, 256);

    cfg.camera.scroll_keys_px_per_s = r.get_i32("camera.scroll_keys_px_per_s", 0, 100'000);
    cfg.camera.scroll_edge_px_per_s = r.get_i32("camera.scroll_edge_px_per_s", 0, 100'000);
    cfg.camera.edge_margin_px = r.get_i32("camera.edge_margin_px", 0, 256);

    cfg.selection.drag_threshold_px = r.get_i32("selection.drag_threshold_px", 0, 256);
    cfg.selection.click_radius_px = r.get_i32("selection.click_radius_px", 1, 256);

    cfg.replay.checkpoint_interval_ticks = r.get_i32("replay.checkpoint_interval_ticks", 1, kMaxTicks);
    cfg.replay.directory = r.get_string("replay.directory");
    cfg.replay.speeds = r.get_increasing_list("replay.speeds", kMaxReplaySpeeds);
    if (!error && cfg.replay.speeds.empty()) {
        r.fail("'replay.speeds' debe tener al menos una velocidad");
    }

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

namespace {

constexpr std::array<std::string_view, 6> kDataFilePaths{
    "terrain.toml",       "units.toml",         "resources.toml",
    "buildings.toml",     "config/engine.toml", "scenarios/headless.toml",
};

}  // namespace

std::span<const std::string_view> data_file_paths() noexcept {
    return kDataFilePaths;
}

std::expected<std::vector<DataFile>, std::string> read_data_files(const std::filesystem::path& data_dir) {
    std::vector<DataFile> files;
    for (const std::string_view rel : kDataFilePaths) {
        auto text = read_text_file(data_dir / std::filesystem::path(rel));
        if (!text) {
            return std::unexpected(text.error());
        }
        files.push_back({std::string(rel), std::move(*text)});
    }
    return files;
}

std::expected<GameData, std::string> parse_game_data(std::vector<DataFile> files) {
    GameData data;
    data.files = std::move(files);
    // Cada fichero: buscar, analizar y mover al resultado; el primer error corta.
    auto load = [&](std::string_view rel, auto&& parse, auto& dest) -> std::optional<std::string> {
        const auto it = std::ranges::find(data.files, rel, &DataFile::path);
        if (it == data.files.end()) {
            return std::format("{}: falta el fichero", rel);
        }
        auto parsed = parse(it->text, it->path);
        if (!parsed) {
            return parsed.error();
        }
        dest = std::move(*parsed);
        return std::nullopt;
    };
    if (auto e = load(kDataFilePaths[0],
                      [](std::string_view t, std::string_view n) { return parse_terrain_catalog(t, n); },
                      data.terrain)) {
        return std::unexpected(*e);
    }
    if (auto e = load(kDataFilePaths[1], [](std::string_view t, std::string_view n) { return parse_unit_catalog(t, n); },
                      data.units)) {
        return std::unexpected(*e);
    }
    if (auto e = load(kDataFilePaths[2], [](std::string_view t, std::string_view n) { return parse_node_catalog(t, n); },
                      data.nodes)) {
        return std::unexpected(*e);
    }
    if (auto e = load(kDataFilePaths[3],
                      [&](std::string_view t, std::string_view n) { return parse_building_catalog(t, data.units, n); },
                      data.buildings)) {
        return std::unexpected(*e);
    }
    const Catalogs catalogs{data.terrain, data.units, data.buildings, data.nodes};
    if (auto e = load(kDataFilePaths[4],
                      [&](std::string_view t, std::string_view n) { return parse_engine_config(t, catalogs, n); },
                      data.engine)) {
        return std::unexpected(*e);
    }
    if (auto e = load(kDataFilePaths[5],
                      [&](std::string_view t, std::string_view n) { return parse_scenario(t, catalogs, n); },
                      data.headless_scenario)) {
        return std::unexpected(*e);
    }
    // Ajustes de la partida elegidos en el menú (opcional).
    const auto match = std::ranges::find(data.files, kMatchSettingsFile, &DataFile::path);
    if (match != data.files.end()) {
        auto root = parse_toml(match->text, match->path);
        if (!root) {
            return std::unexpected(root.error());
        }
        std::optional<std::string> error;
        Reader r(*root, match->path, "", error);
        const auto seed = static_cast<std::uint64_t>(r.get_i32("seed", 0, std::numeric_limits<std::int32_t>::max()));
        const std::string rival = r.get_string("rival");
        const bool fog = r.get_bool("fog");
        const auto& names = data.engine.ai_profile_names;
        const auto it = std::ranges::find(names, rival);
        if (!error && it == names.end()) {
            r.fail(std::format("'rival' = \"{}\": no es un perfil de [[ai.profile]]", rival));
        }
        if (error) {
            return std::unexpected(*error);
        }
        data.engine.world.map.seed = seed;
        data.engine.world.setup.seed = seed;
        data.engine.world.vision.enabled = fog;
        for (sim::AiSeat& seat : data.engine.world.ai_players) {
            seat.profile = static_cast<std::uint8_t>(it - names.begin());
        }
    }
    return data;
}

std::string match_settings_toml(const MatchSettings& s) {
    return std::format("# Ajustes de esta partida (los genera el menú).\nseed = {}\nrival = \"{}\"\nfog = {}\n", s.seed,
                       s.rival, s.fog ? "true" : "false");
}

std::expected<GameData, std::string> with_match_settings(const GameData& base, const MatchSettings& settings) {
    std::vector<DataFile> files = base.files;
    std::erase_if(files, [](const DataFile& f) { return f.path == kMatchSettingsFile; });
    files.push_back({std::string(kMatchSettingsFile), match_settings_toml(settings)});
    return parse_game_data(std::move(files));
}

std::expected<GameData, std::string> load_game_data(const std::filesystem::path& data_dir) {
    auto files = read_data_files(data_dir);
    if (!files) {
        return std::unexpected(files.error());
    }
    return parse_game_data(std::move(*files));
}

}  // namespace rts::game
