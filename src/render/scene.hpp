#pragma once

// Traducción de lo que hay que dibujar (mapa, marcadores, selección) a un lote de
// sprites. Sin SDL ni GPU: se prueba contando y comprobando instancias.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "render/atlas.hpp"
#include "render/projection.hpp"
#include "render/sprite_batch.hpp"
#include "render/view_params.hpp"
#include "sim/tile_map.hpp"

namespace rts::render {

struct Marker {
    Vec2 screen_pos;  // centro, píxeles de pantalla
    bool selected = false;
};

struct Rect {
    Vec2 min;
    Vec2 max;
};

struct Scene {
    const sim::TileMap* map = nullptr;
    Camera camera;
    Vec2 screen;
    std::span<const Marker> markers;  // en orden de pintado
    std::optional<Rect> drag_rect;
    std::optional<sim::TileCoord> hovered_tile;
};

struct SceneStats {
    std::int32_t tiles_drawn = 0;
    std::int32_t markers_drawn = 0;
};

class SceneBuilder {
public:
    // terrain_colors[id] es el color RGB provisional del terreno id.
    SceneBuilder(const ViewParams& view, const Atlas& atlas, std::vector<std::array<std::uint8_t, 3>> terrain_colors);

    SceneStats build(const Scene& scene, SpriteBatch& out);

    // Color final de una casilla (terreno + sombreado). Público para las pruebas.
    [[nodiscard]] Rgba tile_color(const sim::TileMap& map, sim::TileCoord c);

private:
    void refresh_tile_colors(const sim::TileMap& map);

    ViewParams view_;
    IsoProjection proj_;
    AtlasRegion diamond_;
    AtlasRegion disc_;
    AtlasRegion ring_;
    AtlasRegion solid_;
    std::vector<std::array<std::uint8_t, 3>> terrain_colors_;

    // Colores por casilla, recalculados solo cuando cambia el mapa (puntero o revisión).
    std::vector<Rgba> tile_colors_;
    const sim::TileMap* cached_map_ = nullptr;
    std::uint32_t cached_revision_ = 0;
};

}  // namespace rts::render
