#include "render/scene.hpp"

#include <algorithm>
#include <cassert>
#include <utility>

namespace rts::render {

namespace {

constexpr std::int32_t kPercent = 100;
// El anillo de selección se dibuja algo mayor que el disco para rodearlo.
constexpr float kRingScale = 1.6f;
constexpr float kRectBorderPx = 1.0f;
constexpr std::uint8_t kOpaque = 255;

std::uint8_t scale_channel(std::uint8_t c, std::int32_t percent) noexcept {
    return static_cast<std::uint8_t>(std::clamp<std::int32_t>(c * percent / kPercent, 0, kOpaque));
}

}  // namespace

SceneBuilder::SceneBuilder(const ViewParams& view, const Atlas& atlas,
                           std::vector<std::array<std::uint8_t, 3>> terrain_colors)
    : view_(view),
      proj_(view),
      diamond_(atlas.tile_diamond),
      disc_(atlas.marker_disc),
      ring_(atlas.marker_ring),
      solid_(atlas.solid),
      terrain_colors_(std::move(terrain_colors)) {}

void SceneBuilder::refresh_tile_colors(const sim::TileMap& map) {
    if (cached_map_ == &map && cached_revision_ == map.revision() && !tile_colors_.empty()) {
        return;
    }
    cached_map_ = &map;
    cached_revision_ = map.revision();

    std::int32_t max_level = 1;
    for (const std::uint8_t level : map.elevation_layer()) {
        max_level = std::max<std::int32_t>(max_level, level);
    }

    tile_colors_.resize(static_cast<std::size_t>(map.tile_count()));
    const std::int32_t span = kPercent - view_.elevation_shade_min_percent;
    for (std::int32_t y = 0; y < map.height(); ++y) {
        for (std::int32_t x = 0; x < map.width(); ++x) {
            const sim::TileCoord c{x, y};
            const std::int32_t level = map.elevation(c);
            // La casilla (x-1, y-1) queda justo detrás en pantalla: si esta es más alta,
            // su cara mira a la luz y se aclara; si es más baja, queda en sombra.
            const sim::TileCoord behind{x - 1, y - 1};
            const std::int32_t step = map.contains(behind) ? level - map.elevation(behind) : 0;
            const std::int32_t percent =
                view_.elevation_shade_min_percent + span * level / max_level + view_.hillshade_step_percent * step;

            const sim::TerrainId id = map.terrain(c);
            assert(id < terrain_colors_.size());
            const auto& rgb = terrain_colors_[id];
            tile_colors_[static_cast<std::size_t>(y) * static_cast<std::size_t>(map.width()) +
                         static_cast<std::size_t>(x)] = {scale_channel(rgb[0], percent), scale_channel(rgb[1], percent),
                                                         scale_channel(rgb[2], percent), kOpaque};
        }
    }
}

Rgba SceneBuilder::tile_color(const sim::TileMap& map, sim::TileCoord c) {
    refresh_tile_colors(map);
    return tile_colors_[static_cast<std::size_t>(c.y) * static_cast<std::size_t>(map.width()) +
                        static_cast<std::size_t>(c.x)];
}

SceneStats SceneBuilder::build(const Scene& scene, SpriteBatch& out) {
    SceneStats stats;
    const Vec2 tile_size{static_cast<float>(view_.tile_width_px), static_cast<float>(view_.tile_height_px)};
    const Vec2 top_to_corner{-proj_.half_width(), 0.0f};

    if (scene.map != nullptr) {
        const sim::TileMap& map = *scene.map;
        refresh_tile_colors(map);
        const std::size_t width = static_cast<std::size_t>(map.width());
        for_each_visible_tile(proj_, scene.camera, scene.screen, map.width(), map.height(),
                              [&](std::int32_t i, std::int32_t j) {
                                  const Vec2 pos = scene.camera.world_to_screen(proj_.tile_top(i, j)) + top_to_corner;
                                  const std::size_t idx = static_cast<std::size_t>(j) * width + static_cast<std::size_t>(i);
                                  out.add(pos, tile_size, diamond_, tile_colors_[idx]);
                                  ++stats.tiles_drawn;
                              });
        for (const TileTint& tint : scene.tile_tints) {
            if (map.contains(tint.tile)) {
                const Vec2 pos = scene.camera.world_to_screen(proj_.tile_top(tint.tile.x, tint.tile.y)) + top_to_corner;
                out.add(pos, tile_size, diamond_, tint.color);
            }
        }
        if (scene.hovered_tile && map.contains(*scene.hovered_tile)) {
            const Vec2 pos = scene.camera.world_to_screen(proj_.tile_top(scene.hovered_tile->x, scene.hovered_tile->y)) +
                             top_to_corner;
            out.add(pos, tile_size, diamond_, view_.hover_tile_color);
        }
    }

    const Vec2 disc_size{static_cast<float>(disc_.width_px), static_cast<float>(disc_.height_px)};
    const Vec2 ring_size = disc_size * kRingScale;
    for (const Marker& m : scene.markers) {
        // Recorte: fuera de pantalla (con el margen del anillo) no se dibuja.
        if (m.screen_pos.x < -ring_size.x || m.screen_pos.y < -ring_size.y || m.screen_pos.x > scene.screen.x + ring_size.x ||
            m.screen_pos.y > scene.screen.y + ring_size.y) {
            continue;
        }
        out.add(m.screen_pos - disc_size * 0.5f, disc_size, disc_, m.selected ? view_.marker_selected_color : m.color);
        if (m.selected) {
            out.add(m.screen_pos - ring_size * 0.5f, ring_size, ring_, view_.marker_selected_color);
        }
        ++stats.markers_drawn;
    }

    // Puntos de ruta: discos a la mitad de tamaño.
    const Vec2 dot_size = disc_size * 0.5f;
    for (const Vec2& p : scene.path_points) {
        out.add(p - dot_size * 0.5f, dot_size, disc_, view_.marker_selected_color);
    }

    if (scene.drag_rect) {
        const Rect& r = *scene.drag_rect;
        const Vec2 size = r.max - r.min;
        out.add(r.min, size, solid_, view_.selection_rect_color);
        Rgba border = view_.selection_rect_color;
        border[3] = kOpaque;
        out.add(r.min, {size.x, kRectBorderPx}, solid_, border);
        out.add({r.min.x, r.max.y - kRectBorderPx}, {size.x, kRectBorderPx}, solid_, border);
        out.add(r.min, {kRectBorderPx, size.y}, solid_, border);
        out.add({r.max.x - kRectBorderPx, r.min.y}, {kRectBorderPx, size.y}, solid_, border);
    }
    return stats;
}

}  // namespace rts::render
