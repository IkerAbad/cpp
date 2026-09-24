#include "sim/tile_map.hpp"

#include "sim/state_hash.hpp"

namespace rts::sim {

TileMap::TileMap(std::int32_t width, std::int32_t height)
    : width_(width),
      height_(height),
      terrain_(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), TerrainId{0}),
      elevation_(terrain_.size(), std::uint8_t{0}) {
    assert(width > 0 && height > 0);
}

void TileMap::set_terrain(TileCoord c, TerrainId id) noexcept {
    terrain_[index(c)] = id;
    ++revision_;
}

void TileMap::set_elevation(TileCoord c, std::uint8_t level) noexcept {
    elevation_[index(c)] = level;
    ++revision_;
}

void TileMap::hash_into(StateHasher& h) const {
    h.add_i32(width_);
    h.add_i32(height_);
    h.add_bytes(terrain_);
    h.add_bytes(elevation_);
}

}  // namespace rts::sim
