#pragma once

// Escenarios hechos a mano (F3): formato, captura de un mapa generado y ediciones.
//
// Un escenario es un fichero TOML (el editor los guarda en escenarios/) que, al jugarlo,
// viaja con los datos de la partida como config/escenario.toml: la repetición y la
// partida en red lo llevan dentro.
//
//   name = "..."
//   players = 2
//   width = 128
//   height = 128
//   terrain = ["abcc...", ...]    # una letra por casilla: a = terreno 0 de terrain.toml
//   elevation = ["0012...", ...]  # un dígito por casilla: nivel de altura
//   [[building]] player = 0, type = "centro_urbano", at = [x, y]   (origen)
//   [[node]] type = "arbol", at = [x, y]                            (origen)
//   [[unit]] player = 0, type = "aldeano", at = [x, y]              (casilla)

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "game/config.hpp"
#include "sim/world.hpp"

namespace rts::game {

inline constexpr std::string_view kScenarioFile = "config/escenario.toml";

struct ScenarioDoc {
    std::string name;
    sim::ScenarioParams params;
};

[[nodiscard]] std::expected<ScenarioDoc, std::string> parse_scenario_doc(std::string_view text, const GameData& data,
                                                                       std::string_view source = "<memoria>");
[[nodiscard]] std::string scenario_doc_toml(const ScenarioDoc& doc, const GameData& data);

// Lo que hay en un mundo (mapa, edificios, recursos, unidades) como escenario: el punto
// de partida del editor.
[[nodiscard]] ScenarioDoc scenario_from_world(const sim::World& world, std::string name, std::int32_t players);

// Datos de una partida con este escenario: ajustes con un puesto por jugador del
// escenario (el 0, humano; los demás, la IA con el rival de settings).
[[nodiscard]] std::expected<GameData, std::string> with_scenario(const GameData& base, const ScenarioDoc& doc,
                                                                MatchSettings settings);

// --- Ediciones -------------------------------------------------------------------

// Terreno en un rombo de radio radius alrededor de center.
void paint_terrain(sim::ScenarioParams& s, sim::TileCoord center, std::int32_t radius, sim::TerrainId terrain);
// Sube (delta > 0) o baja la altura en el rombo, sin salir de [0, max_level].
void raise(sim::ScenarioParams& s, sim::TileCoord center, std::int32_t radius, std::int32_t delta,
           std::int32_t max_level);
// Quita lo último colocado que ocupa la casilla (las unidades primero). Falso si no hay nada.
bool erase_at(sim::ScenarioParams& s, sim::TileCoord tile, const GameData& data);

}  // namespace rts::game
