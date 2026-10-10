#include "game/scenario.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <optional>

#include <toml++/toml.hpp>

namespace rts::game {

namespace {

constexpr char kFirstTerrainLetter = 'a';
constexpr char kFirstLevelDigit = '0';
constexpr std::int32_t kMaxLetters = 26;
constexpr std::int32_t kMaxDigits = 10;
constexpr std::int32_t kMaxSide = 1024;
constexpr std::int64_t kMaxBrushRadius = 64;
constexpr std::int64_t kMaxGroup = 400;          // unidades de un bloque
constexpr std::int64_t kPermille = 1000;
constexpr std::int64_t kMaxStock = 1'000'000;
constexpr std::int64_t kMaxSeconds = 24 * 3600;  // un día de partida
constexpr std::string_view kDefaultTree = "arbol";

// En el orden de sim::ObjectiveKind.
constexpr std::array<std::string_view, 6> kObjectiveKeys{"destruir", "conservar", "sobrevivir",
                                                         "llegar",   "reunir",    "derrotar"};

// Ruido de una casilla para los bosques (splitmix64): mismo fichero, mismos árboles.
std::uint64_t tile_noise(sim::TileCoord c, std::size_t salt) {
    std::uint64_t z = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.x)) << 32U) ^
                      static_cast<std::uint32_t>(c.y) ^ (static_cast<std::uint64_t>(salt) << 48U);
    z += 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31U);
}

std::optional<sim::Resource> resource_of(std::string_view key) {
    for (std::size_t r = 0; r < sim::kResourceCount; ++r) {
        if (resource_key(static_cast<sim::Resource>(r)) == key) {
            return static_cast<sim::Resource>(r);
        }
    }
    return std::nullopt;
}

// { comida = 300, ... }
std::expected<sim::Stock, std::string> read_stock(const toml::table& t) {
    sim::Stock stock{};
    for (const auto& [k, v] : t) {
        const auto r = resource_of(k.str());
        const auto amount = v.value<std::int64_t>();
        if (!r || !amount || *amount < 0 || *amount > kMaxStock) {
            return std::unexpected(std::format("'{}': recurso desconocido o cantidad fuera de 0..{}", k.str(), kMaxStock));
        }
        stock[sim::resource_index(*r)] = static_cast<std::int32_t>(*amount);
    }
    return stock;
}

std::string stock_toml(const sim::Stock& stock) {
    std::string out = "{";
    for (std::size_t r = 0; r < sim::kResourceCount; ++r) {
        out += std::format("{} {} = {}", r == 0 ? "" : ",", resource_key(static_cast<sim::Resource>(r)), stock[r]);
    }
    return out + " }";
}

// Texto como cadena básica de TOML.
std::string toml_string(std::string_view text) {
    std::string out = "\"";
    for (const char ch : text) {
        switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            default:
                out += ch;
        }
    }
    return out + "\"";
}

template <typename Types>
std::optional<std::uint8_t> index_of(const Types& types, std::string_view name) {
    for (std::size_t i = 0; i < types.size(); ++i) {
        if (types[i].name == name) {
            return static_cast<std::uint8_t>(i);
        }
    }
    return std::nullopt;
}

std::size_t cell(const sim::ScenarioParams& s, sim::TileCoord c) {
    return static_cast<std::size_t>(c.y) * static_cast<std::size_t>(s.width) + static_cast<std::size_t>(c.x);
}

bool inside(const sim::ScenarioParams& s, sim::TileCoord c) {
    return c.x >= 0 && c.y >= 0 && c.x < s.width && c.y < s.height;
}

// Recorre el rombo de radio r (distancia de Manhattan) alrededor de center.
template <typename Fn>
void for_diamond(const sim::ScenarioParams& s, sim::TileCoord center, std::int32_t r, Fn&& fn) {
    for (std::int32_t y = center.y - r; y <= center.y + r; ++y) {
        for (std::int32_t x = center.x - r; x <= center.x + r; ++x) {
            const sim::TileCoord c{x, y};
            if (inside(s, c) && std::abs(x - center.x) + std::abs(y - center.y) <= r) {
                fn(c);
            }
        }
    }
}

std::int32_t placement_size(const sim::ScenarioPlacement& p, const GameData& data) {
    switch (p.kind) {
        case sim::ScenarioPlacement::Kind::Building:
            return data.buildings.types[p.type].type.size;
        case sim::ScenarioPlacement::Kind::Node:
            return data.nodes.types[p.type].type.size;
        case sim::ScenarioPlacement::Kind::Unit:
            break;
    }
    return 1;
}

}  // namespace

std::string_view objective_kind_key(sim::ObjectiveKind k) noexcept {
    return kObjectiveKeys[static_cast<std::size_t>(k)];
}

std::expected<ScenarioDoc, std::string> parse_scenario_doc(std::string_view text, const GameData& data,
                                                         std::string_view source) {
    toml::table root;
    try {
        root = toml::parse(text, source);
    } catch (const toml::parse_error& e) {
        return std::unexpected(std::format("{}: {}", source, e.description()));
    }
    const auto fail = [&](std::string msg) { return std::unexpected(std::format("{}: {}", source, msg)); };
    ScenarioDoc doc;
    doc.name = root["name"].value_or(std::string("Sin nombre"));
    doc.briefing = root["briefing"].value_or(std::string());
    sim::ScenarioParams& s = doc.params;
    s.players = static_cast<std::int32_t>(root["players"].value_or(std::int64_t{0}));
    s.width = static_cast<std::int32_t>(root["width"].value_or(std::int64_t{0}));
    s.height = static_cast<std::int32_t>(root["height"].value_or(std::int64_t{0}));
    const auto max_players = static_cast<std::int32_t>(data.engine.seat_starts.size());
    if (s.players < 1 || s.players > max_players) {
        return fail(std::format("'players' debe estar entre 1 y {}", max_players));
    }
    if (s.width < 1 || s.height < 1 || s.width > kMaxSide || s.height > kMaxSide) {
        return fail(std::format("'width' y 'height' deben estar entre 1 y {}", kMaxSide));
    }
    const auto terrain_types = static_cast<std::int32_t>(data.terrain.types.size());
    const std::int32_t levels = data.engine.world.map.elevation_levels;
    s.terrain.resize(static_cast<std::size_t>(s.width) * static_cast<std::size_t>(s.height));
    s.elevation.resize(s.terrain.size());
    const toml::array* terrain = root["terrain"].as_array();
    const toml::array* elevation = root["elevation"].as_array();
    if (terrain == nullptr && elevation == nullptr && root.contains("base")) {
        // Escrito a mano: un terreno y una altura de base, y luego las pinceladas.
        const std::string base = root["base"].value_or(std::string());
        const auto t = data.terrain.find(base);
        const auto level = root["base_level"].value_or(std::int64_t{0});
        if (!t) {
            return fail(std::format("'base' = \"{}\": no es un terreno", base));
        }
        if (level < 0 || level >= levels) {
            return fail(std::format("'base_level' debe estar entre 0 y {}", levels - 1));
        }
        std::ranges::fill(s.terrain, *t);
        std::ranges::fill(s.elevation, static_cast<std::uint8_t>(level));
    } else {
        if (terrain == nullptr || elevation == nullptr || std::cmp_not_equal(terrain->size(), s.height) ||
            std::cmp_not_equal(elevation->size(), s.height)) {
            return fail(std::format("'terrain' y 'elevation' deben tener {} filas (o usar 'base')", s.height));
        }
        // Una letra por terreno y un dígito por nivel: el formato no admite más.
        if (terrain_types > kMaxLetters || levels > kMaxDigits) {
            return fail(
                std::format("el formato admite hasta {} terrenos y {} niveles de altura", kMaxLetters, kMaxDigits));
        }
        for (std::int32_t y = 0; y < s.height; ++y) {
            const auto t_row = (*terrain)[static_cast<std::size_t>(y)].value<std::string>();
            const auto e_row = (*elevation)[static_cast<std::size_t>(y)].value<std::string>();
            if (!t_row || !e_row || std::cmp_not_equal(t_row->size(), s.width) ||
                std::cmp_not_equal(e_row->size(), s.width)) {
                return fail(std::format("la fila {} de 'terrain' o de 'elevation' no tiene {} casillas", y, s.width));
            }
            for (std::int32_t x = 0; x < s.width; ++x) {
                const std::int32_t t = (*t_row)[static_cast<std::size_t>(x)] - kFirstTerrainLetter;
                const std::int32_t e = (*e_row)[static_cast<std::size_t>(x)] - kFirstLevelDigit;
                if (t < 0 || t >= terrain_types) {
                    return fail(std::format("terreno '{}' desconocido en ({}, {})", (*t_row)[static_cast<std::size_t>(x)],
                                            x, y));
                }
                if (e < 0 || e >= levels) {
                    return fail(std::format("altura '{}' fuera de 0..{} en ({}, {})",
                                            (*e_row)[static_cast<std::size_t>(x)], levels - 1, x, y));
                }
                s.terrain[cell(s, {x, y})] = static_cast<sim::TerrainId>(t);
                s.elevation[cell(s, {x, y})] = static_cast<std::uint8_t>(e);
            }
        }
    }
    // Casilla [x, y] de una tabla; nullopt si falta o no es así.
    const auto read_tile = [](const toml::node* n) -> std::optional<sim::TileCoord> {
        const toml::array* a = n != nullptr ? n->as_array() : nullptr;
        if (a == nullptr || a->size() != 2 || !(*a)[0].is_integer() || !(*a)[1].is_integer()) {
            return std::nullopt;
        }
        return sim::TileCoord{static_cast<std::int32_t>((*a)[0].value_or(std::int64_t{0})),
                              static_cast<std::int32_t>((*a)[1].value_or(std::int64_t{0}))};
    };
    // Pinceladas, en orden: cada una pinta o mueve la altura en rombos a lo largo de su trazo.
    if (const toml::array* paints = root["paint"].as_array()) {
        for (std::size_t i = 0; i < paints->size(); ++i) {
            const toml::table* t = (*paints)[i].as_table();
            if (t == nullptr) {
                return fail(std::format("'paint[{}]' debe ser una tabla", i));
            }
            const auto at = read_tile(t->get("at"));
            const auto radius = (*t)["radius"].value_or(std::int64_t{0});
            if (!at || radius < 0 || radius > kMaxBrushRadius) {
                return fail(std::format("'paint[{}]' necesita 'at' = [x, y] y 'radius' entre 0 y {}", i, kMaxBrushRadius));
            }
            std::optional<sim::TerrainId> terr;
            if (const auto name = (*t)["terrain"].value<std::string>()) {
                terr = data.terrain.find(*name);
                if (!terr) {
                    return fail(std::format("'paint[{}].terrain' = \"{}\": no es un terreno", i, *name));
                }
            }
            const auto delta = (*t)["raise"].value<std::int64_t>();
            const auto level = (*t)["level"].value<std::int64_t>();
            if (level && (*level < 0 || *level >= levels)) {
                return fail(std::format("'paint[{}].level' debe estar entre 0 y {}", i, levels - 1));
            }
            if (delta && (*delta < -levels || *delta > levels)) {
                return fail(std::format("'paint[{}].raise' debe estar entre -{} y {}", i, levels, levels));
            }
            std::vector<sim::TileCoord> path{*at};
            if (const toml::array* to = (*t)["to"].as_array()) {
                for (std::size_t k = 0; k < to->size(); ++k) {
                    const auto p = read_tile(&(*to)[k]);
                    if (!p) {
                        return fail(std::format("'paint[{}].to[{}]' debe ser [x, y]", i, k));
                    }
                    path.push_back(*p);
                }
            }
            // Las casillas del trazo, sin repetir (subir dos veces la misma casilla sería un pico).
            std::vector<std::uint8_t> touched(s.terrain.size(), 0);
            const auto r = static_cast<std::int32_t>(radius);
            const auto stamp = [&](sim::TileCoord c) {
                for_diamond(s, c, r, [&](sim::TileCoord q) { touched[cell(s, q)] = 1; });
            };
            stamp(path.front());
            for (std::size_t k = 1; k < path.size(); ++k) {
                const sim::TileCoord a = path[k - 1];
                const sim::TileCoord b = path[k];
                const std::int32_t steps = std::max(std::abs(b.x - a.x), std::abs(b.y - a.y));
                for (std::int32_t j = 1; j <= steps; ++j) {
                    stamp({a.x + (b.x - a.x) * j / steps, a.y + (b.y - a.y) * j / steps});
                }
            }
            for (std::size_t c = 0; c < touched.size(); ++c) {
                if (touched[c] == 0) {
                    continue;
                }
                if (terr) {
                    s.terrain[c] = *terr;
                }
                if (level) {
                    s.elevation[c] = static_cast<std::uint8_t>(*level);
                }
                if (delta) {
                    s.elevation[c] = static_cast<std::uint8_t>(
                        std::clamp<std::int64_t>(std::int64_t{s.elevation[c]} + *delta, 0, levels - 1));
                }
            }
        }
    }
    // Objetos, en el orden del fichero.
    const auto read_list = [&](std::string_view key, sim::ScenarioPlacement::Kind kind) -> std::optional<std::string> {
        const toml::array* list = root[key].as_array();
        if (list == nullptr) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < list->size(); ++i) {
            const toml::table* t = (*list)[i].as_table();
            if (t == nullptr) {
                return std::format("'{}[{}]' debe ser una tabla", key, i);
            }
            const std::string type = (*t)["type"].value_or(std::string());
            sim::ScenarioPlacement p;
            p.kind = kind;
            std::optional<std::uint8_t> id;
            switch (kind) {
                case sim::ScenarioPlacement::Kind::Unit:
                    id = index_of(data.units.types, type);
                    break;
                case sim::ScenarioPlacement::Kind::Building:
                    id = index_of(data.buildings.types, type);
                    break;
                case sim::ScenarioPlacement::Kind::Node:
                    id = index_of(data.nodes.types, type);
                    break;
            }
            if (!id) {
                return std::format("'{}[{}].type' = \"{}\": no existe", key, i, type);
            }
            const auto at = read_tile(t->get("at"));
            if (!at) {
                return std::format("'{}[{}].at' debe ser [x, y]", key, i);
            }
            p.type = *id;
            if (kind != sim::ScenarioPlacement::Kind::Node) {
                const auto owner = (*t)["player"].value_or(std::int64_t{-1});
                if (owner < 0 || owner >= s.players) {
                    return std::format("'{}[{}].player' debe estar entre 0 y {}", key, i, s.players - 1);
                }
                p.owner = static_cast<sim::PlayerId>(owner);
            }
            if (const toml::table* st = (*t)["store"].as_table(); st != nullptr) {
                auto stock = read_stock(*st);
                if (!stock) {
                    return std::format("'{}[{}].store': {}", key, i, stock.error());
                }
                p.store = *stock;
            }
            // Unidades: un bloque de count, cols por fila (por omisión, casi cuadrado).
            const auto count = kind == sim::ScenarioPlacement::Kind::Unit ? (*t)["count"].value_or(std::int64_t{1}) : 1;
            if (count < 1 || count > kMaxGroup) {
                return std::format("'{}[{}].count' debe estar entre 1 y {}", key, i, kMaxGroup);
            }
            std::int64_t side = 1;
            while (side * side < count) {
                ++side;
            }
            const auto cols = (*t)["cols"].value_or(side);
            if (cols < 1) {
                return std::format("'{}[{}].cols' debe ser positivo", key, i);
            }
            for (std::int64_t k = 0; k < count; ++k) {
                p.at = {at->x + static_cast<std::int32_t>(k % cols), at->y + static_cast<std::int32_t>(k / cols)};
                if (!inside(s, p.at)) {
                    return std::format("'{}[{}].at' queda fuera del mapa", key, i);
                }
                s.placements.push_back(p);
            }
        }
        return std::nullopt;
    };
    for (const auto& [key, kind] : {std::pair{"building", sim::ScenarioPlacement::Kind::Building},
                                    std::pair{"node", sim::ScenarioPlacement::Kind::Node},
                                    std::pair{"unit", sim::ScenarioPlacement::Kind::Unit}}) {
        if (auto e = read_list(key, kind)) {
            return fail(*e);
        }
    }
    // Bosques: árboles al azar (determinista: sale de la casilla) en un círculo, sin
    // tocar agua ni lo ya colocado y su contorno.
    if (const toml::array* forests = root["forest"].as_array()) {
        std::vector<std::uint8_t> taken(s.terrain.size(), 0);
        const auto mark = [&](sim::TileCoord o, std::int32_t size) {
            for (std::int32_t y = o.y - 1; y <= o.y + size; ++y) {
                for (std::int32_t x = o.x - 1; x <= o.x + size; ++x) {
                    if (inside(s, {x, y})) {
                        taken[cell(s, {x, y})] = 1;
                    }
                }
            }
        };
        for (const sim::ScenarioPlacement& p : s.placements) {
            mark(p.at, placement_size(p, data));
        }
        for (std::size_t i = 0; i < forests->size(); ++i) {
            const toml::table* t = (*forests)[i].as_table();
            if (t == nullptr) {
                return fail(std::format("'forest[{}]' debe ser una tabla", i));
            }
            const std::string type = (*t)["type"].value_or(std::string(kDefaultTree));
            const auto id = index_of(data.nodes.types, type);
            const auto at = read_tile(t->get("at"));
            const auto radius = (*t)["radius"].value_or(std::int64_t{0});
            const auto density = (*t)["density"].value_or(std::int64_t{-1});
            if (!id || !at || radius < 0 || radius > kMaxSide || density < 0 || density > kPermille) {
                return fail(std::format("'forest[{}]' necesita 'type' de recurso, 'at' = [x, y], 'radius' y 'density' "
                                        "(0 a {} por mil)",
                                        i, kPermille));
            }
            const auto r = static_cast<std::int32_t>(radius);
            for (std::int32_t y = at->y - r; y <= at->y + r; ++y) {
                for (std::int32_t x = at->x - r; x <= at->x + r; ++x) {
                    const sim::TileCoord c{x, y};
                    if (!inside(s, c) || (x - at->x) * (x - at->x) + (y - at->y) * (y - at->y) > r * r ||
                        taken[cell(s, c)] != 0 || !data.terrain.types[s.terrain[cell(s, c)]].passable ||
                        tile_noise(c, i) % kPermille >= static_cast<std::uint64_t>(density)) {
                        continue;
                    }
                    s.placements.push_back({sim::ScenarioPlacement::Kind::Node, *id, 0, c});
                    taken[cell(s, c)] = 1;
                }
            }
        }
    }
    // Almacén inicial de cada jugador, por orden.
    if (const toml::array* players = root["player"].as_array()) {
        if (std::cmp_greater(players->size(), s.players)) {
            return fail(std::format("hay más [[player]] que jugadores ({})", s.players));
        }
        for (std::size_t i = 0; i < players->size(); ++i) {
            const toml::table* t = (*players)[i].as_table();
            const toml::table* st = t != nullptr ? (*t)["stock"].as_table() : nullptr;
            if (st == nullptr) {
                return fail(std::format("'player[{}].stock' debe ser una tabla de recursos", i));
            }
            auto stock = read_stock(*st);
            if (!stock) {
                return fail(std::format("'player[{}].stock': {}", i, stock.error()));
            }
            s.stocks.push_back(*stock);
        }
    }
    // Objetivos (F4).
    if (const toml::array* objectives = root["objective"].as_array()) {
        for (std::size_t i = 0; i < objectives->size(); ++i) {
            const toml::table* t = (*objectives)[i].as_table();
            if (t == nullptr) {
                return fail(std::format("'objective[{}]' debe ser una tabla", i));
            }
            const auto bad = [&](std::string_view what) {
                return fail(std::format("'objective[{}]': {}", i, what));
            };
            sim::Objective o;
            const std::string kind = (*t)["kind"].value_or(std::string());
            const auto k = std::ranges::find(kObjectiveKeys, kind);
            if (k == kObjectiveKeys.end()) {
                return bad(std::format("'kind' = \"{}\": debe ser destruir, conservar, sobrevivir, llegar, reunir o "
                                       "derrotar",
                                       kind));
            }
            o.kind = static_cast<sim::ObjectiveKind>(k - kObjectiveKeys.begin());
            const auto player = (*t)["player"].value_or(std::int64_t{-1});
            if (player < 0 || player >= s.players) {
                return bad(std::format("'player' debe estar entre 0 y {}", s.players - 1));
            }
            o.player = static_cast<sim::PlayerId>(player);
            const auto target = (*t)["target"].value_or(player);
            if (target < 0 || target >= s.players) {
                return bad(std::format("'target' debe estar entre 0 y {}", s.players - 1));
            }
            o.target = static_cast<sim::PlayerId>(target);
            if (const auto b = (*t)["building"].value<std::string>()) {
                const auto id = index_of(data.buildings.types, *b);
                if (!id) {
                    return bad(std::format("'building' = \"{}\": no existe", *b));
                }
                o.type = *id;
            } else if (const auto u = (*t)["unit"].value<std::string>()) {
                o.units = true;
                if (*u != "*") {
                    const auto id = index_of(data.units.types, *u);
                    if (!id) {
                        return bad(std::format("'unit' = \"{}\": no existe", *u));
                    }
                    o.type = *id;
                }
            }
            if (o.kind == sim::ObjectiveKind::Reach) {
                o.units = true;  // llegar es cosa de unidades (sin tipo: cualquier tropa)
            }
            const auto count = (*t)["count"].value_or(std::int64_t{1});
            if (count < 1 || count > kMaxStock) {
                return bad(std::format("'count' debe estar entre 1 y {}", kMaxStock));
            }
            o.count = static_cast<std::int32_t>(count);
            if (o.kind == sim::ObjectiveKind::Gather) {
                const auto r = resource_of((*t)["resource"].value_or(std::string()));
                if (!r) {
                    return bad("'resource' debe ser un recurso (comida, madera...)");
                }
                o.resource = *r;
            }
            if (o.kind == sim::ObjectiveKind::Survive) {
                const auto secs = (*t)["seconds"].value_or(std::int64_t{0});
                if (secs < 1 || secs > kMaxSeconds) {
                    return bad(std::format("'seconds' debe estar entre 1 y {}", kMaxSeconds));
                }
                o.ticks = static_cast<sim::Tick>(secs * sim::kTicksPerSecond);
            }
            if (const toml::array* z = (*t)["zone"].as_array()) {
                if (z->size() != 4) {
                    return bad("'zone' debe ser [x0, y0, x1, y1]");
                }
                std::array<std::int32_t, 4> v{};
                for (std::size_t j = 0; j < 4; ++j) {
                    v[j] = static_cast<std::int32_t>((*z)[j].value_or(std::int64_t{-1}));
                }
                o.zoned = true;
                o.zone_min = {std::min(v[0], v[2]), std::min(v[1], v[3])};
                o.zone_max = {std::max(v[0], v[2]), std::max(v[1], v[3])};
                if (!inside(s, o.zone_min) || !inside(s, o.zone_max)) {
                    return bad("'zone' queda fuera del mapa");
                }
            } else if (o.kind == sim::ObjectiveKind::Reach) {
                return bad("llegar necesita 'zone'");
            }
            s.objectives.push_back(o);
            doc.objective_texts.push_back((*t)["text"].value_or(std::string(kind)));
        }
    }
    return doc;
}

std::string scenario_doc_toml(const ScenarioDoc& doc, const GameData& data) {
    const sim::ScenarioParams& s = doc.params;
    std::string out = std::format(
        "# Escenario (F3), hecho con el editor. terrain: una letra por casilla (a = el primer\n"
        "# terreno de terrain.toml); elevation: un dígito por casilla (nivel de altura).\n"
        "name = {}\nplayers = {}\nwidth = {}\nheight = {}\n",
        toml_string(doc.name), s.players, s.width, s.height);
    if (!doc.briefing.empty()) {
        out += std::format("briefing = {}\n", toml_string(doc.briefing));
    }
    const auto rows = [&](const auto& layer, char base) {
        std::string r = "[\n";
        for (std::int32_t y = 0; y < s.height; ++y) {
            r += "  \"";
            for (std::int32_t x = 0; x < s.width; ++x) {
                r += static_cast<char>(base + layer[cell(s, {x, y})]);
            }
            r += "\",\n";
        }
        return r + "]\n";
    };
    out += "terrain = " + rows(s.terrain, kFirstTerrainLetter);
    out += "elevation = " + rows(s.elevation, kFirstLevelDigit);
    for (const sim::Stock& stock : s.stocks) {
        out += "\n[[player]]\nstock = " + stock_toml(stock) + "\n";
    }
    for (std::size_t i = 0; i < s.objectives.size(); ++i) {
        const sim::Objective& o = s.objectives[i];
        out += std::format("\n[[objective]]\nplayer = {}\nkind = \"{}\"\ntext = {}\ntarget = {}\ncount = {}\n", o.player,
                           objective_kind_key(o.kind), toml_string(i < doc.objective_texts.size() ? doc.objective_texts[i] : ""),
                           o.target, o.count);
        if (o.units) {
            out += std::format("unit = \"{}\"\n", o.type == sim::kAnyObjectType ? "*" : data.units.types[o.type].name);
        } else if (o.type != sim::kAnyObjectType) {
            out += std::format("building = \"{}\"\n", data.buildings.types[o.type].name);
        }
        if (o.kind == sim::ObjectiveKind::Gather) {
            out += std::format("resource = \"{}\"\n", resource_key(o.resource));
        }
        if (o.kind == sim::ObjectiveKind::Survive) {
            out += std::format("seconds = {}\n", o.ticks / sim::kTicksPerSecond);
        }
        if (o.zoned) {
            out += std::format("zone = [{}, {}, {}, {}]\n", o.zone_min.x, o.zone_min.y, o.zone_max.x, o.zone_max.y);
        }
    }
    for (const auto kind : {sim::ScenarioPlacement::Kind::Building, sim::ScenarioPlacement::Kind::Node,
                            sim::ScenarioPlacement::Kind::Unit}) {
        for (const sim::ScenarioPlacement& p : s.placements) {
            if (p.kind != kind) {
                continue;
            }
            switch (kind) {
                case sim::ScenarioPlacement::Kind::Building:
                    out += std::format("\n[[building]]\nplayer = {}\ntype = \"{}\"\nat = [{}, {}]\n", p.owner,
                                       data.buildings.types[p.type].name, p.at.x, p.at.y);
                    if (std::ranges::any_of(p.store, [](std::int32_t v) { return v > 0; })) {
                        out += "store = " + stock_toml(p.store) + "\n";
                    }
                    break;
                case sim::ScenarioPlacement::Kind::Node:
                    out += std::format("\n[[node]]\ntype = \"{}\"\nat = [{}, {}]\n", data.nodes.types[p.type].name,
                                       p.at.x, p.at.y);
                    break;
                case sim::ScenarioPlacement::Kind::Unit:
                    out += std::format("\n[[unit]]\nplayer = {}\ntype = \"{}\"\nat = [{}, {}]\n", p.owner,
                                       data.units.types[p.type].name, p.at.x, p.at.y);
                    break;
            }
        }
    }
    return out;
}

ScenarioDoc scenario_from_world(const sim::World& world, std::string name, std::int32_t players) {
    ScenarioDoc doc;
    doc.name = std::move(name);
    sim::ScenarioParams& s = doc.params;
    const sim::TileMap& map = world.map();
    s.width = map.width();
    s.height = map.height();
    s.players = players;
    s.terrain.resize(static_cast<std::size_t>(s.width) * static_cast<std::size_t>(s.height));
    s.elevation.resize(s.terrain.size());
    for (std::int32_t y = 0; y < s.height; ++y) {
        for (std::int32_t x = 0; x < s.width; ++x) {
            s.terrain[cell(s, {x, y})] = map.terrain({x, y});
            s.elevation[cell(s, {x, y})] = map.elevation({x, y});
        }
    }
    sim::Snapshot snap;
    world.write_snapshot(snap);
    for (const sim::SnapshotObject& o : snap.objects) {
        sim::ScenarioPlacement p;
        p.kind = o.kind == sim::ObjectKind::Building ? sim::ScenarioPlacement::Kind::Building
                                                     : sim::ScenarioPlacement::Kind::Node;
        p.type = o.type;
        p.owner = o.owner;
        p.at = o.origin;
        s.placements.push_back(p);
    }
    for (const sim::SnapshotEntity& e : snap.entities) {
        s.placements.push_back({sim::ScenarioPlacement::Kind::Unit, e.type, e.owner, sim::tile_of(e.pos)});
    }
    return doc;
}

std::expected<GameData, std::string> with_scenario(const GameData& base, const ScenarioDoc& doc,
                                                   MatchSettings settings) {
    settings.seats.assign(static_cast<std::size_t>(std::max(doc.params.players, 2)), "ia");
    settings.seats[0] = "humano";
    std::vector<DataFile> files = base.files;
    std::erase_if(files, [](const DataFile& f) { return f.path == kMatchSettingsFile || f.path == kScenarioFile; });
    files.push_back({std::string(kMatchSettingsFile), match_settings_toml(settings)});
    files.push_back({std::string(kScenarioFile), scenario_doc_toml(doc, base)});
    return parse_game_data(std::move(files));
}

ScenarioDoc mirrored_scenario(const ScenarioDoc& doc, const GameData& data) {
    const sim::ScenarioParams& in = doc.params;
    if (in.width != in.height || in.players != 2) {
        return doc;
    }
    const std::int32_t n = in.width;
    // La casilla de la mitad del jugador 0: por debajo de la diagonal, o en ella y antes
    // del centro.
    const auto primary = [n](sim::TileCoord c) { return c.x + c.y < n - 1 || (c.x + c.y == n - 1 && c.x < n - 1 - c.x); };
    const auto mirror = [n](sim::TileCoord c) { return sim::TileCoord{n - 1 - c.x, n - 1 - c.y}; };
    ScenarioDoc out = doc;
    sim::ScenarioParams& s = out.params;
    for (std::int32_t y = 0; y < n; ++y) {
        for (std::int32_t x = 0; x < n; ++x) {
            if (!primary({x, y})) {
                const sim::TileCoord m = mirror({x, y});
                s.terrain[cell(s, {x, y})] = in.terrain[cell(in, m)];
                s.elevation[cell(s, {x, y})] = in.elevation[cell(in, m)];
            }
        }
    }
    s.placements.clear();
    for (const sim::ScenarioPlacement& p : in.placements) {
        const std::int32_t size = placement_size(p, data);
        bool inside_half = true;
        for (std::int32_t dy = 0; dy < size && inside_half; ++dy) {
            for (std::int32_t dx = 0; dx < size && inside_half; ++dx) {
                inside_half = primary({p.at.x + dx, p.at.y + dy});
            }
        }
        // Del jugador 1 no se guarda nada: su mitad es la copia de la del 0.
        if (!inside_half || (p.kind != sim::ScenarioPlacement::Kind::Node && p.owner != 0)) {
            continue;
        }
        s.placements.push_back(p);
        sim::ScenarioPlacement q = p;
        // El origen es la esquina de arriba a la izquierda: la reflejada es la de abajo a
        // la derecha del original.
        q.at = mirror({p.at.x + size - 1, p.at.y + size - 1});
        if (q.kind != sim::ScenarioPlacement::Kind::Node) {
            q.owner = 1;
        }
        s.placements.push_back(q);
    }
    return out;
}

void paint_terrain(sim::ScenarioParams& s, sim::TileCoord center, std::int32_t radius, sim::TerrainId terrain) {
    for_diamond(s, center, radius, [&](sim::TileCoord c) { s.terrain[cell(s, c)] = terrain; });
}

void raise(sim::ScenarioParams& s, sim::TileCoord center, std::int32_t radius, std::int32_t delta,
           std::int32_t max_level) {
    for_diamond(s, center, radius, [&](sim::TileCoord c) {
        auto& e = s.elevation[cell(s, c)];
        e = static_cast<std::uint8_t>(std::clamp<std::int32_t>(e + delta, 0, max_level));
    });
}

bool erase_at(sim::ScenarioParams& s, sim::TileCoord tile, const GameData& data) {
    const auto covers = [&](const sim::ScenarioPlacement& p) {
        const std::int32_t size = placement_size(p, data);
        return tile.x >= p.at.x && tile.y >= p.at.y && tile.x < p.at.x + size && tile.y < p.at.y + size;
    };
    // Las unidades primero (están encima), y de lo último colocado a lo primero.
    for (const bool units : {true, false}) {
        for (auto it = s.placements.rbegin(); it != s.placements.rend(); ++it) {
            if ((it->kind == sim::ScenarioPlacement::Kind::Unit) == units && covers(*it)) {
                s.placements.erase(std::next(it).base());
                return true;
            }
        }
    }
    return false;
}

}  // namespace rts::game
