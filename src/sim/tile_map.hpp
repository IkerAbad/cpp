#pragma once

// Mapa por casillas. Capas en estructura de arrays (SoA): cada capa es un bloque
// contiguo de width*height bytes, índice y*width + x. Un sistema que recorre solo
// el terreno no arrastra la altura a la caché.
//
// M1: terreno y altura (1 byte cada una: 64 KiB por capa en 256x256). Recursos,
// objetos estáticos y visibilidad por jugador se añaden en sus hitos.

#include <cassert>
#include <cstdint>
#include <span>
#include <vector>

namespace rts::sim {

class StateHasher;

using TerrainId = std::uint8_t;

struct TileCoord {
    std::int32_t x = 0;
    std::int32_t y = 0;
    friend constexpr bool operator==(TileCoord, TileCoord) = default;
};

class TileMap {
public:
    TileMap(std::int32_t width, std::int32_t height);

    [[nodiscard]] std::int32_t width() const noexcept { return width_; }
    [[nodiscard]] std::int32_t height() const noexcept { return height_; }
    [[nodiscard]] std::int32_t tile_count() const noexcept { return width_ * height_; }

    [[nodiscard]] bool contains(TileCoord c) const noexcept {
        return c.x >= 0 && c.y >= 0 && c.x < width_ && c.y < height_;
    }

    [[nodiscard]] TerrainId terrain(TileCoord c) const noexcept { return terrain_[index(c)]; }
    [[nodiscard]] std::uint8_t elevation(TileCoord c) const noexcept { return elevation_[index(c)]; }

    void set_terrain(TileCoord c, TerrainId id) noexcept;
    void set_elevation(TileCoord c, std::uint8_t level) noexcept;

    [[nodiscard]] std::span<const TerrainId> terrain_layer() const noexcept { return terrain_; }
    [[nodiscard]] std::span<const std::uint8_t> elevation_layer() const noexcept { return elevation_; }

    // Crece con cada modificación. La presentación lo usa para saber cuándo
    // reconstruir lo que haya derivado del mapa (colores, sombreado).
    [[nodiscard]] std::uint32_t revision() const noexcept { return revision_; }

    void hash_into(StateHasher& h) const;

private:
    [[nodiscard]] std::size_t index(TileCoord c) const noexcept {
        assert(contains(c));
        return static_cast<std::size_t>(c.y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(c.x);
    }

    std::int32_t width_;
    std::int32_t height_;
    std::vector<TerrainId> terrain_;
    std::vector<std::uint8_t> elevation_;
    std::uint32_t revision_ = 0;
};

}  // namespace rts::sim
