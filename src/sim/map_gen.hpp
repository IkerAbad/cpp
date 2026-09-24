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
};

[[nodiscard]] TileMap generate_map(const MapGenParams& params);

}  // namespace rts::sim
