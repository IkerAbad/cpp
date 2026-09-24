#include "sim/map_gen.hpp"

#include <cassert>

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
    return map;
}

}  // namespace rts::sim
