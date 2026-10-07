#include "sim/map_gen.hpp"

#include <algorithm>
#include <cassert>
#include <limits>

#include "sim/rng.hpp"

namespace rts::sim {

namespace {

constexpr std::int32_t kWeightBits = 16;

// smoothstep 3t² - 2t³ con t = num/den, en punto fijo de 16 bits. Resultado en [0, 65536].
std::int64_t smooth_weight(std::int32_t num, std::int32_t den) noexcept {
    const std::int64_t t = (std::int64_t{num} << kWeightBits) / den;
    const std::int64_t t2 = (t * t) >> kWeightBits;
    const std::int64_t t3 = (t2 * t) >> kWeightBits;
    return 3 * t2 - 2 * t3;
}

std::int64_t lerp(std::int64_t a, std::int64_t b, std::int64_t w) noexcept {
    return a + (((b - a) * w) >> kWeightBits);
}

// Retícula de valores aleatorios en [0, kElevationRange), interpolada entre nodos.
class ValueNoise {
public:
    ValueNoise(Xoshiro256pp& rng, std::int32_t width, std::int32_t height, std::int32_t cell)
        : cell_(cell), lattice_w_(width / cell + 2), lattice_h_(height / cell + 2) {
        assert(cell > 0);
        values_.resize(static_cast<std::size_t>(lattice_w_) * static_cast<std::size_t>(lattice_h_));
        // Relleno en orden de filas: el orden de extracción del RNG forma parte del formato.
        for (auto& v : values_) {
            v = static_cast<std::int32_t>(rng.next_below(static_cast<std::uint32_t>(kElevationRange)));
        }
    }

    [[nodiscard]] std::int64_t sample(std::int32_t x, std::int32_t y) const noexcept {
        const std::int32_t lx = x / cell_;
        const std::int32_t ly = y / cell_;
        const std::int64_t wx = smooth_weight(x % cell_, cell_);
        const std::int64_t wy = smooth_weight(y % cell_, cell_);
        const std::int64_t top = lerp(at(lx, ly), at(lx + 1, ly), wx);
        const std::int64_t bottom = lerp(at(lx, ly + 1), at(lx + 1, ly + 1), wx);
        return lerp(top, bottom, wy);
    }

private:
    [[nodiscard]] std::int64_t at(std::int32_t lx, std::int32_t ly) const noexcept {
        return values_[static_cast<std::size_t>(ly) * static_cast<std::size_t>(lattice_w_) +
                       static_cast<std::size_t>(lx)];
    }

    std::int32_t cell_;
    std::int32_t lattice_w_;
    std::int32_t lattice_h_;
    std::vector<std::int32_t> values_;
};

// Ríos: cada uno, de un borde al opuesto (los pares de arriba abajo, los impares de
// izquierda a derecha). Se prueban varios trazados y se queda el primero que pasa lejos
// de todos los inicios (o el que más se aleja).
void carve_rivers(const MapGenParams& params, Xoshiro256pp& rng, TileMap& map) {
    const RiverParams& rp = params.rivers;
    struct Course {
        bool vertical = false;
        std::vector<std::int32_t> path;  // posición del eje en cada paso
    };
    std::vector<Course> courses;
    constexpr std::int32_t kTries = 16;
    constexpr std::int32_t kPercent = 100;
    constexpr std::int32_t kSpreadDivisor = 6;  // el eje cae en el tercio central del mapa
    constexpr std::int32_t kDriftDivisor = 8;   // sin alejarse del eje más de 1/8 del lado
    for (std::int32_t r = 0; r < rp.count; ++r) {
        const bool vertical = r % 2 == 0;
        const std::int32_t along = vertical ? params.height : params.width;
        const std::int32_t across = vertical ? params.width : params.height;
        const auto at = [&](std::int32_t a, std::int32_t c) { return vertical ? TileCoord{c, a} : TileCoord{a, c}; };

        std::vector<std::int32_t> best;
        std::int64_t best_clearance = -1;
        for (std::int32_t t = 0; t < kTries; ++t) {
            const std::int32_t axis =
                across / 2 + rng.next_in_range(-across / kSpreadDivisor, across / kSpreadDivisor);
            std::vector<std::int32_t> path;
            std::int32_t c = axis;
            for (std::int32_t a = 0; a < along; ++a) {
                if (static_cast<std::int32_t>(rng.next_below(kPercent)) < rp.meander_percent) {
                    c += rng.next_in_range(-1, 1);
                }
                const std::int32_t drift = across / kDriftDivisor;
                c = std::clamp(c, axis - drift, axis + drift);
                path.push_back(c);
            }
            // Distancia (al cuadrado) del cauce al inicio más cercano.
            std::int64_t clearance = std::numeric_limits<std::int64_t>::max();
            for (const TileCoord& p : params.keep_dry) {
                for (std::int32_t a = 0; a < along; ++a) {
                    const TileCoord q = at(a, path[static_cast<std::size_t>(a)]);
                    const std::int64_t dx = q.x - p.x;
                    const std::int64_t dy = q.y - p.y;
                    clearance = std::min(clearance, dx * dx + dy * dy);
                }
            }
            if (clearance > best_clearance) {
                best_clearance = clearance;
                best = std::move(path);
            }
            if (best_clearance >= std::int64_t{rp.clearance_tiles} * rp.clearance_tiles) {
                break;
            }
        }
        for (std::int32_t a = 0; a < along; ++a) {
            const std::int32_t c0 = best[static_cast<std::size_t>(a)] - rp.width_tiles / 2;
            for (std::int32_t c = c0; c < c0 + rp.width_tiles; ++c) {
                const TileCoord q = at(a, c);
                if (map.contains(q)) {
                    map.set_terrain(q, rp.water);
                    map.set_elevation(q, static_cast<std::uint8_t>(std::min<std::int32_t>(map.elevation(q), rp.bed_level)));
                }
            }
        }
        courses.push_back({vertical, std::move(best)});
    }

    // Vados, con todos los cauces ya abiertos. Un río solo separa tierra donde hay tierra
    // en las dos orillas: en cada uno de esos tramos va al menos un vado (más si es largo,
    // según la densidad pedida), así el río nunca aísla lo que el terreno dejaba unido.
    const auto land = [&](TileCoord q) {
        return map.contains(q) && map.terrain(q) < params.passable.size() && params.passable[map.terrain(q)] != 0;
    };
    for (const Course& course : courses) {
        const std::int32_t along = course.vertical ? params.height : params.width;
        const auto at = [&](std::int32_t a, std::int32_t c) {
            return course.vertical ? TileCoord{c, a} : TileCoord{a, c};
        };
        std::vector<std::uint8_t> banks(static_cast<std::size_t>(along), 0);
        for (std::int32_t a = 0; a < along; ++a) {
            const std::int32_t c0 = course.path[static_cast<std::size_t>(a)] - rp.width_tiles / 2;
            banks[static_cast<std::size_t>(a)] = land(at(a, c0 - 1)) && land(at(a, c0 + rp.width_tiles)) ? 1 : 0;
        }
        for (std::int32_t a = 0; a < along;) {
            if (banks[static_cast<std::size_t>(a)] == 0) {
                ++a;
                continue;
            }
            std::int32_t end = a;
            while (end < along && banks[static_cast<std::size_t>(end)] != 0) {
                ++end;
            }
            const std::int32_t length = end - a;
            const std::int32_t count = std::max(1, (length * rp.fords + along / 2) / along);
            for (std::int32_t f = 0; f < count; ++f) {
                const std::int32_t center = a + length * (2 * f + 1) / (2 * count);
                for (std::int32_t k = center - rp.ford_length_tiles / 2;
                     k < center - rp.ford_length_tiles / 2 + rp.ford_length_tiles; ++k) {
                    if (k < a || k >= end) {
                        continue;
                    }
                    const std::int32_t c0 = course.path[static_cast<std::size_t>(k)] - rp.width_tiles / 2;
                    for (std::int32_t c = c0; c < c0 + rp.width_tiles; ++c) {
                        if (map.contains(at(k, c))) {
                            map.set_terrain(at(k, c), rp.ford);
                        }
                    }
                    // Embarcaderos: orilla despejada a los dos lados.
                    for (std::int32_t d = 1; d <= rp.landing_tiles; ++d) {
                        for (const TileCoord q : {at(k, c0 - d), at(k, c0 + rp.width_tiles - 1 + d)}) {
                            if (land(q) && map.terrain(q) != rp.ford) {
                                map.set_terrain(q, rp.landing);
                            }
                        }
                    }
                }
            }
            a = end;
        }
    }
}

}  // namespace

TileMap generate_map(const MapGenParams& params) {
    assert(params.width > 0 && params.height > 0);
    assert(params.noise_cell_tiles >= 2);
    assert(params.elevation_levels > 0 && params.elevation_levels <= 256);
    assert(!params.bands.empty() && params.bands.back().max_elevation >= kElevationRange);

    Xoshiro256pp rng(params.seed);
    const ValueNoise coarse(rng, params.width, params.height, params.noise_cell_tiles);
    const ValueNoise fine(rng, params.width, params.height, params.noise_cell_tiles / 2);

    TileMap map(params.width, params.height);
    for (std::int32_t y = 0; y < params.height; ++y) {
        for (std::int32_t x = 0; x < params.width; ++x) {
            // Octavas con peso 2:1; el resultado sigue en [0, kElevationRange).
            const std::int64_t elevation = (2 * coarse.sample(x, y) + fine.sample(x, y)) / 3;

            TerrainId terrain = params.bands.back().terrain;
            for (const TerrainBand& band : params.bands) {
                if (elevation < band.max_elevation) {
                    terrain = band.terrain;
                    break;
                }
            }
            const auto level = static_cast<std::uint8_t>((elevation * params.elevation_levels) / kElevationRange);
            map.set_terrain({x, y}, terrain);
            map.set_elevation({x, y}, level);
        }
    }
    if (params.rivers.count > 0) {
        carve_rivers(params, rng, map);
    }
    return map;
}

}  // namespace rts::sim
