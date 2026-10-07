#pragma once

// Generación determinista de mapas: ruido de valor en dos octavas, todo en enteros.
// Misma semilla y mismos parámetros dan el mismo mapa en cualquier plataforma.

#include <cstdint>
#include <vector>

#include "sim/tile_map.hpp"

namespace rts::sim {

// Elevación normalizada del generador: [0, kElevationRange).
inline constexpr std::int32_t kElevationRange = 65536;

// Las casillas con elevación < max_elevation (y >= la banda anterior) reciben este terreno.
struct TerrainBand {
    std::int32_t max_elevation = 0;
    TerrainId terrain = 0;
};

// Ríos (F3): de un borde al opuesto, con meandros, lejos de los puntos keep_dry y con
// vados (agua somera que se puede cruzar) repartidos a lo largo. Con count = 0 no se
// toca nada (ni el generador aleatorio): el mapa es el de siempre.
struct RiverParams {
    std::int32_t count = 0;
    std::int32_t width_tiles = 0;
    std::int32_t fords = 0;             // vados por río
    std::int32_t ford_length_tiles = 0; // largo de cada vado a lo largo del cauce
    std::int32_t meander_percent = 0;   // probabilidad de desviarse una casilla en cada paso
    std::int32_t clearance_tiles = 0;   // distancia mínima a cada punto de keep_dry
    std::int32_t bed_level = 0;         // nivel de altura del cauce
    TerrainId water = 0;
    TerrainId ford = 0;
    // Orillas de cada vado: la tierra a menos de landing_tiles pasa a este terreno (sin
    // bosque), para que los árboles no tapen el paso.
    TerrainId landing = 0;
    std::int32_t landing_tiles = 0;
};

struct MapGenParams {
    std::uint64_t seed = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    // Distancia entre puntos de la retícula de la primera octava, en casillas.
    // La segunda octava usa la mitad.
    std::int32_t noise_cell_tiles = 0;
    // Número de niveles de altura: la elevación se cuantiza a [0, elevation_levels).
    std::int32_t elevation_levels = 0;
    // Ordenadas por max_elevation creciente; la última debe llegar a kElevationRange.
    std::vector<TerrainBand> bands;
    RiverParams rivers;
    std::vector<TileCoord> keep_dry;  // inicios de los jugadores: los ríos no pasan cerca
    std::vector<std::uint8_t> passable;  // por TerrainId: para poner los vados donde sirven
};

[[nodiscard]] TileMap generate_map(const MapGenParams& params);

}  // namespace rts::sim
