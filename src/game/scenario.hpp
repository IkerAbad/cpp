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
//
// Para escribir escenarios a mano (F4) también vale, en lugar de terrain y elevation:
//
//   base = "pradera"                  # terreno de todo el mapa
//   base_level = 1                    # y su altura
//   [[paint]] at = [x, y], radius = 3, to = [[x, y], ...]   # trazo de rombos
//             terrain = "roca", raise = 1 | level = 2        # qué hace en cada rombo
//   [[forest]] at = [x, y], radius = 9, density = 400, type = "arbol"   # (por mil)
//   [[unit]] ... count = 12, cols = 4  # un bloque de unidades desde at, fila a fila
//
// y, en cualquier escenario:
//
//   briefing = "..."                  # lo que se cuenta antes de jugar
//   [[player]] stock = { comida = 300, madera = 200 }   # almacén inicial, por orden
//   [[building]] ... store = { comida = 240 }           # campamento ya abastecido
//   [[objective]] player = 0, kind = "destruir", text = "...", target = 1,
//                 building = "torre_piedra" | unit = "caballero" | unit = "*",
//                 zone = [x0, y0, x1, y1], count = 10, resource = "comida", seconds = 900
//   kind: destruir, conservar, sobrevivir, llegar, reunir, derrotar (sim/objectives.hpp)

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "game/config.hpp"
#include "sim/world.hpp"

namespace rts::game {

inline constexpr std::string_view kScenarioFile = "config/escenario.toml";

struct ScenarioDoc {
    std::string name;
    std::string briefing;
    sim::ScenarioParams params;
    std::vector<std::string> objective_texts;  // uno por objetivo de params.objectives
};

// Nombre de un tipo de objetivo en los ficheros ("destruir"...).
[[nodiscard]] std::string_view objective_kind_key(sim::ObjectiveKind k) noexcept;

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
