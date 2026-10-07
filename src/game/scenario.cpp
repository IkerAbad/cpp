#include "game/scenario.hpp"

#include <algorithm>
#include <format>

#include <toml++/toml.hpp>

namespace rts::game {

namespace {

constexpr char kFirstTerrainLetter = 'a';
constexpr char kFirstLevelDigit = '0';
constexpr std::int32_t kMaxLetters = 26;
constexpr std::int32_t kMaxDigits = 10;
constexpr std::int32_t kMaxSide = 1024;

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

}  // namespace

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
    const toml::array* terrain = root["terrain"].as_array();
    const toml::array* elevation = root["elevation"].as_array();
    if (terrain == nullptr || elevation == nullptr || std::cmp_not_equal(terrain->size(), s.height) ||
        std::cmp_not_equal(elevation->size(), s.height)) {
        return fail(std::format("'terrain' y 'elevation' deben tener {} filas", s.height));
    }
    const auto terrain_types = static_cast<std::int32_t>(data.terrain.types.size());
    const std::int32_t levels = data.engine.world.map.elevation_levels;
    // Una letra por terreno y un dígito por nivel: el formato no admite más.
    if (terrain_types > kMaxLetters || levels > kMaxDigits) {
        return fail(std::format("el formato admite hasta {} terrenos y {} niveles de altura", kMaxLetters, kMaxDigits));
    }
    s.terrain.resize(static_cast<std::size_t>(s.width) * static_cast<std::size_t>(s.height));
    s.elevation.resize(s.terrain.size());
    for (std::int32_t y = 0; y < s.height; ++y) {
        const auto t_row = (*terrain)[static_cast<std::size_t>(y)].value<std::string>();
        const auto e_row = (*elevation)[static_cast<std::size_t>(y)].value<std::string>();
        if (!t_row || !e_row || std::cmp_not_equal(t_row->size(), s.width) || std::cmp_not_equal(e_row->size(), s.width)) {
            return fail(std::format("la fila {} de 'terrain' o de 'elevation' no tiene {} casillas", y, s.width));
        }
        for (std::int32_t x = 0; x < s.width; ++x) {
            const std::int32_t t = (*t_row)[static_cast<std::size_t>(x)] - kFirstTerrainLetter;
            const std::int32_t e = (*e_row)[static_cast<std::size_t>(x)] - kFirstLevelDigit;
            if (t < 0 || t >= terrain_types) {
                return fail(std::format("terreno '{}' desconocido en ({}, {})", (*t_row)[static_cast<std::size_t>(x)], x, y));
            }
            if (e < 0 || e >= levels) {
                return fail(std::format("altura '{}' fuera de 0..{} en ({}, {})", (*e_row)[static_cast<std::size_t>(x)],
                                        levels - 1, x, y));
            }
            s.terrain[cell(s, {x, y})] = static_cast<sim::TerrainId>(t);
            s.elevation[cell(s, {x, y})] = static_cast<std::uint8_t>(e);
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
            const toml::array* at = (*t)["at"].as_array();
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
            if (at == nullptr || at->size() != 2) {
                return std::format("'{}[{}].at' debe ser [x, y]", key, i);
            }
            p.type = *id;
            p.at = {static_cast<std::int32_t>((*at)[0].value_or(std::int64_t{-1})),
                    static_cast<std::int32_t>((*at)[1].value_or(std::int64_t{-1}))};
            if (!inside(s, p.at)) {
                return std::format("'{}[{}].at' queda fuera del mapa", key, i);
            }
            if (kind != sim::ScenarioPlacement::Kind::Node) {
                const auto owner = (*t)["player"].value_or(std::int64_t{-1});
                if (owner < 0 || owner >= s.players) {
                    return std::format("'{}[{}].player' debe estar entre 0 y {}", key, i, s.players - 1);
                }
                p.owner = static_cast<sim::PlayerId>(owner);
            }
            s.placements.push_back(p);
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
    return doc;
}

std::string scenario_doc_toml(const ScenarioDoc& doc, const GameData& data) {
    const sim::ScenarioParams& s = doc.params;
    std::string out = std::format(
        "# Escenario (F3), hecho con el editor. terrain: una letra por casilla (a = el primer\n"
        "# terreno de terrain.toml); elevation: un dígito por casilla (nivel de altura).\n"
        "name = \"{}\"\nplayers = {}\nwidth = {}\nheight = {}\n",
        doc.name, s.players, s.width, s.height);
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
        std::int32_t size = 1;
        if (p.kind == sim::ScenarioPlacement::Kind::Building) {
            size = data.buildings.types[p.type].type.size;
        } else if (p.kind == sim::ScenarioPlacement::Kind::Node) {
            size = data.nodes.types[p.type].type.size;
        }
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
