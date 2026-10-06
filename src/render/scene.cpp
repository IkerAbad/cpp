#include "render/scene.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <utility>

#include "sim/fog.hpp"

namespace rts::render {

namespace {

constexpr std::int32_t kPercent = 100;
// El anillo de selección se dibuja algo mayor que el disco para rodearlo; el del dueño,
// justo por fuera del disco. El punto de estado es un disco a media escala.
constexpr float kRingScale = 1.6f;
constexpr float kOwnerRingScale = 1.25f;
constexpr float kBadgeScale = 0.5f;
constexpr float kRectBorderPx = 1.0f;
constexpr std::uint8_t kOpaque = 255;
constexpr float kOpaqueF = 255.0f;
constexpr std::int32_t kPermille = 1000;
constexpr float kMsPerSecond = 1000.0f;

// Producto de dos colores (cada canal en 0-255).
Rgba modulate(Rgba a, Rgba b) noexcept {
    Rgba out{};
    for (std::size_t i = 0; i < 4; ++i) {
        out[i] = static_cast<std::uint8_t>(a[i] * b[i] / kOpaque);
    }
    return out;
}

// Mezcla entera de las coordenadas de una casilla: variante estable del terreno y de
// los árboles (siempre la misma para la misma casilla).
std::uint32_t tile_hash(std::int32_t x, std::int32_t y) noexcept {
    auto h = static_cast<std::uint32_t>(x) * 0x9E3779B1U ^ static_cast<std::uint32_t>(y) * 0x85EBCA77U;
    h ^= h >> 15U;
    h *= 0x2C1B3C6DU;
    h ^= h >> 12U;
    return h;
}

float fract(float v) noexcept { return v - std::floor(v); }

std::uint8_t scale_channel(std::uint8_t c, std::int32_t percent) noexcept {
    return static_cast<std::uint8_t>(std::clamp<std::int32_t>(c * percent / kPercent, 0, kOpaque));
}

}  // namespace

SceneBuilder::SceneBuilder(const ViewParams& view, const Atlas& atlas,
                           std::vector<std::array<std::uint8_t, 3>> terrain_colors)
    : view_(view),
      atlas_(&atlas),
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

void SceneBuilder::add_health_bar(Vec2 center_top, std::int32_t permille, SpriteBatch& out) const {
    const Vec2 size{static_cast<float>(view_.health_bar_width_px), static_cast<float>(view_.health_bar_height_px)};
    const Vec2 top_left = center_top - Vec2{size.x * 0.5f, size.y};
    out.add(top_left, size, solid_, view_.health_back_color);
    const float fraction = static_cast<float>(std::clamp(permille, 0, kPermille)) / static_cast<float>(kPermille);
    out.add(top_left, {size.x * fraction, size.y}, solid_,
            permille < view_.health_low_permille ? view_.health_low_color : view_.health_color);
}

void SceneBuilder::add_sprite(const Sprite& sprite, Vec2 at, Rgba color, bool mirror, SpriteBatch& out,
                              float visible_fraction) const {
    if (!sprite.valid()) {
        return;
    }
    const AtlasRegion& r = sprite.region;
    const auto w = static_cast<float>(r.width_px);
    const auto h = static_cast<float>(r.height_px);
    AtlasRegion shown = r;
    float top = at.y - sprite.anchor.y;
    float height = h;
    if (visible_fraction < 1.0f) {
        // Solo la parte de abajo (obra): se recortan filas por arriba.
        const float hidden = h * (1.0f - visible_fraction);
        shown.v0 = static_cast<std::uint16_t>(r.v0 + static_cast<float>(r.v1 - r.v0) * (1.0f - visible_fraction));
        top += hidden;
        height -= hidden;
    }
    if (mirror) {
        // Espejo: el quad de ancho negativo invierte la imagen alrededor del ancla.
        out.add({at.x + sprite.anchor.x, top}, {-w, height}, shown, color);
    } else {
        out.add({at.x - sprite.anchor.x, top}, {w, height}, shown, color);
    }
}

bool SceneBuilder::is_flat(const SceneObject& o) const {
    if (o.art == SceneObject::Art::None || o.art_type < 0) {
        return true;  // rombos planos
    }
    return o.art == SceneObject::Art::Building && static_cast<std::size_t>(o.art_type) < atlas_->flat_buildings.size() &&
           atlas_->flat_buildings[static_cast<std::size_t>(o.art_type)] != 0;
}

void SceneBuilder::draw_flat_object(const Scene& scene, const SceneObject& o, SpriteBatch& out) {
    const Vec2 tile_size{static_cast<float>(view_.tile_width_px), static_cast<float>(view_.tile_height_px)};
    const auto n = static_cast<float>(o.size);
    const Vec2 size = tile_size * n;
    const Vec2 top_left =
        scene.camera.world_to_screen(proj_.tile_top(o.origin.x, o.origin.y)) + Vec2{-proj_.half_width() * n, 0.0f};
    if (o.art != SceneObject::Art::None && o.art_type >= 0) {
        // Campo o camino: el sprite a ras de suelo, debajo de todo lo que tiene altura.
        const SpritePair& sp = atlas_->buildings[static_cast<std::size_t>(o.art_type)];
        const Vec2 center = top_left + size * 0.5f;
        if (o.highlighted) {
            out.add(top_left, size, diamond_, view_.hover_tile_color);
        }
        add_sprite(sp.body, center, o.tint, false, out);
        if (o.health_permille >= 0) {
            bars_.push_back({center, o.health_permille});
        }
        return;
    }
    if (o.base[3] != 0) {
        out.add(top_left, size, diamond_, o.base);
    }
    const float scale = static_cast<float>(o.body_percent) / static_cast<float>(kPercent);
    const Vec2 body = size * scale;
    out.add(top_left + (size - body) * 0.5f, body, diamond_, o.body);
    if (o.highlighted) {
        out.add(top_left, size, diamond_, view_.hover_tile_color);
    }
    if (o.health_permille >= 0) {
        bars_.push_back({top_left + Vec2{size.x * 0.5f, size.y * 0.5f}, o.health_permille});
    }
}

void SceneBuilder::draw_fire(const Scene& scene, const SceneObject& o, Vec2 center, SpriteBatch& out) const {
    if (o.fire_permille <= 0 || atlas_->flames.empty()) {
        return;
    }
    const auto n = static_cast<float>(o.size);
    const std::int32_t count =
        std::max(1, view_.flames_per_tile * o.size * std::min(o.fire_permille, kPermille) / kPermille);
    const auto frames = static_cast<std::int32_t>(atlas_->flames.size());
    const float hw = proj_.half_width() * n * 0.6f;
    const float hh = proj_.half_height() * n * 0.6f;
    for (std::int32_t i = 0; i < count; ++i) {
        const std::uint32_t h = tile_hash(o.origin.x * 31 + i, o.origin.y * 17 - i);
        // Punto al azar (fijo) dentro del rombo de la huella, algo elevado.
        const float u = static_cast<float>(h & 0xFFU) / kOpaqueF * 2.0f - 1.0f;
        const float v = static_cast<float>((h >> 8U) & 0xFFU) / kOpaqueF * 2.0f - 1.0f;
        const Vec2 at = center + Vec2{(u - v) * hw * 0.5f, (u + v) * hh * 0.5f - static_cast<float>(view_.tile_height_px) * 0.4f};
        const auto frame = static_cast<std::int32_t>(scene.time_s * static_cast<float>(view_.flame_fps) +
                                                     static_cast<float>(h >> 16U)) %
                           frames;
        add_sprite(atlas_->flames[static_cast<std::size_t>(frame)], at, {kOpaque, kOpaque, kOpaque, kOpaque},
                   ((h >> 4U) & 1U) != 0, out);
        // Humo: bocanadas que suben, crecen y se deshacen, desfasadas entre sí.
        constexpr int kPuffs = 3;
        for (int k = 0; k < kPuffs; ++k) {
            const float phase = fract(scene.time_s * kMsPerSecond / static_cast<float>(view_.smoke_period_ms) +
                                      static_cast<float>(k) / kPuffs + static_cast<float>(h % 97U) / 97.0f);
            Rgba c = atlas_->smoke_color;
            c[3] = static_cast<std::uint8_t>(static_cast<float>(view_.smoke_alpha_percent) / kPercent * (1.0f - phase) *
                                             kOpaqueF);
            const Sprite& smoke = atlas_->smoke;
            const float grow = 0.6f + phase;
            const Vec2 size{static_cast<float>(smoke.region.width_px) * grow, static_cast<float>(smoke.region.height_px) * grow};
            const Vec2 pos = at + Vec2{phase * size.x * 0.3f, -static_cast<float>(view_.smoke_rise_px) * phase -
                                                                  static_cast<float>(view_.tile_height_px) * 0.5f};
            out.add(pos - size * 0.5f, size, smoke.region, c);
        }
    }
}

void SceneBuilder::draw_art_object(const Scene& scene, const SceneObject& o, SpriteBatch& out) {
    const auto n = static_cast<float>(o.size);
    // Centro de la huella en pantalla.
    const Vec2 center = scene.camera.world_to_screen(proj_.tile_to_world(
        {static_cast<float>(o.origin.x) + n * 0.5f, static_cast<float>(o.origin.y) + n * 0.5f}));
    if (o.art == SceneObject::Art::Node) {
        const auto& variants = atlas_->nodes[static_cast<std::size_t>(o.art_type)];
        if (!variants.empty()) {
            add_sprite(variants[static_cast<std::size_t>(o.variant) % variants.size()], center, o.tint, false, out);
        }
        return;
    }
    const SpritePair& sp = atlas_->buildings[static_cast<std::size_t>(o.art_type)];
    if (o.highlighted) {
        const Vec2 size = Vec2{static_cast<float>(view_.tile_width_px), static_cast<float>(view_.tile_height_px)} * n;
        out.add(center - size * 0.5f, size, diamond_, view_.hover_tile_color);
    }
    float visible = 1.0f;
    if (o.build_permille < kPermille) {
        const float min_part = static_cast<float>(view_.construction_min_percent) / kPercent;
        visible = min_part + (1.0f - min_part) * static_cast<float>(std::max(o.build_permille, 0)) / kPermille;
    }
    add_sprite(sp.body, center, o.tint, false, out, visible);
    if (o.team[3] != 0 && visible >= 1.0f) {
        add_sprite(sp.team, center, modulate(o.team, o.tint), false, out);
    }
    draw_fire(scene, o, center, out);
    if (o.health_permille >= 0) {
        const float top = sp.body.valid() ? sp.body.anchor.y * 0.8f : 0.0f;
        bars_.push_back({center - Vec2{0.0f, top}, o.health_permille});
    }
}

void SceneBuilder::draw_marker(const Marker& m, SpriteBatch& out) {
    const Vec2 disc_size{static_cast<float>(disc_.width_px), static_cast<float>(disc_.height_px)};
    const Vec2 ring_size = disc_size * kRingScale;
    const Vec2 badge_size = disc_size * kBadgeScale;
    if (m.unit_type >= 0 && static_cast<std::size_t>(m.unit_type) < atlas_->units.size()) {
        const SpritePair& sp = atlas_->units[static_cast<std::size_t>(m.unit_type)][static_cast<std::size_t>(m.pose)];
        const Sprite& ring = atlas_->ground_ring;
        if (m.hero && ring.valid()) {
            const Vec2 size{static_cast<float>(ring.region.width_px) * kRingScale,
                            static_cast<float>(ring.region.height_px) * kRingScale};
            out.add(m.screen_pos - size * 0.5f, size, ring.region, view_.hero_color);
        }
        if (m.selected) {
            add_sprite(ring, m.screen_pos, view_.marker_selected_color, false, out);
        }
        add_sprite(sp.body, m.screen_pos, m.tint, m.mirror, out);
        if (m.owner[3] != 0) {
            add_sprite(sp.team, m.screen_pos, modulate(m.owner, m.tint), m.mirror, out);
        }
        const float head = sp.body.valid() ? sp.body.anchor.y : disc_size.y;
        if (m.badge[3] != 0) {
            const Vec2 at = m.screen_pos + Vec2{disc_size.x * 0.6f, -head};
            out.add(at - badge_size * 0.5f, badge_size, disc_, m.badge);
        }
        if (m.health_permille >= 0) {
            bars_.push_back({m.screen_pos - Vec2{0.0f, head + 2.0f}, m.health_permille});
        }
        return;
    }
    const Vec2 owner_size = disc_size * kOwnerRingScale;
    out.add(m.screen_pos - disc_size * 0.5f, disc_size, disc_, m.selected ? view_.marker_selected_color : m.color);
    if (m.owner[3] != 0) {
        out.add(m.screen_pos - owner_size * 0.5f, owner_size, ring_, m.owner);
    }
    if (m.hero) {
        out.add(m.screen_pos - ring_size * 0.5f, ring_size, ring_, view_.hero_color);
    }
    if (m.selected) {
        out.add(m.screen_pos - ring_size * 0.5f, ring_size, ring_, view_.marker_selected_color);
    }
    if (m.badge[3] != 0) {
        // Encima del disco, algo a la derecha.
        const Vec2 at = m.screen_pos + Vec2{disc_size.x * 0.5f, -disc_size.y * 0.5f};
        out.add(at - badge_size * 0.5f, badge_size, disc_, m.badge);
    }
    if (m.health_permille >= 0) {
        bars_.push_back({m.screen_pos - Vec2{0.0f, ring_size.y * 0.5f}, m.health_permille});
    }
}

SceneStats SceneBuilder::build(const Scene& scene, SpriteBatch& out) {
    SceneStats stats;
    bars_.clear();
    const Vec2 tile_size{static_cast<float>(view_.tile_width_px), static_cast<float>(view_.tile_height_px)};
    const Vec2 top_to_corner{-proj_.half_width(), 0.0f};

    if (scene.map != nullptr) {
        const sim::TileMap& map = *scene.map;
        refresh_tile_colors(map);
        const std::size_t width = static_cast<std::size_t>(map.width());
        const bool textured = !atlas_->terrain.empty();
        for_each_visible_tile(proj_, scene.camera, scene.screen, map.width(), map.height(),
                              [&](std::int32_t i, std::int32_t j) {
                                  const Vec2 pos = scene.camera.world_to_screen(proj_.tile_top(i, j)) + top_to_corner;
                                  const std::size_t idx = static_cast<std::size_t>(j) * width + static_cast<std::size_t>(i);
                                  Rgba color = tile_colors_[idx];
                                  if (idx < scene.fog.size()) {
                                      const auto fog = static_cast<sim::Fog>(scene.fog[idx]);
                                      if (fog == sim::Fog::Unexplored) {
                                          color = view_.fog_unexplored_color;
                                      } else if (fog == sim::Fog::Explored) {
                                          for (std::size_t ch = 0; ch < 3; ++ch) {
                                              color[ch] = scale_channel(color[ch], view_.fog_explored_shade_percent);
                                          }
                                      }
                                  }
                                  const AtlasRegion* region = &diamond_;
                                  if (textured) {
                                      const auto& variants = atlas_->terrain[map.terrain({i, j})];
                                      if (!variants.empty()) {
                                          region = &variants[tile_hash(i, j) % variants.size()];
                                      }
                                  }
                                  out.add(pos, tile_size, *region, color);
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

    // Lo que está a ras de suelo (rombos planos, campos, caminos) va primero; lo que tiene
    // altura (edificios, árboles, figuras) se ordena de atrás hacia delante por la y en
    // pantalla de su punto más adelantado, para que lo de delante tape a lo de detrás.
    drawables_.clear();
    for (std::size_t i = 0; i < scene.objects.size(); ++i) {
        const SceneObject& o = scene.objects[i];
        const auto n = static_cast<float>(o.size);
        const Vec2 top_left =
            scene.camera.world_to_screen(proj_.tile_top(o.origin.x, o.origin.y)) + Vec2{-proj_.half_width() * n, 0.0f};
        const Vec2 size = tile_size * n;
        // Recorte generoso: los sprites altos asoman por encima de su huella.
        const float tall = size.y * 2.0f + static_cast<float>(view_.tile_height_px) * 4.0f;
        if (top_left.x > scene.screen.x || top_left.y - tall > scene.screen.y || top_left.x + size.x < 0.0f ||
            top_left.y + size.y < 0.0f) {
            continue;
        }
        ++stats.objects_drawn;
        if (is_flat(o)) {
            draw_flat_object(scene, o, out);
        } else {
            drawables_.push_back({top_left.y + size.y, false, i});
        }
    }
    const Vec2 disc_size{static_cast<float>(disc_.width_px), static_cast<float>(disc_.height_px)};
    const Vec2 ring_size = disc_size * kRingScale;
    for (std::size_t i = 0; i < scene.markers.size(); ++i) {
        const Marker& m = scene.markers[i];
        // Recorte: fuera de pantalla (con el margen de la figura) no se dibuja.
        const float above = m.unit_type >= 0 ? static_cast<float>(view_.tile_height_px) * 2.0f : ring_size.y;
        if (m.screen_pos.x < -ring_size.x * 2.0f || m.screen_pos.y < -ring_size.y ||
            m.screen_pos.x > scene.screen.x + ring_size.x * 2.0f || m.screen_pos.y > scene.screen.y + above) {
            continue;
        }
        drawables_.push_back({m.screen_pos.y, true, i});
        ++stats.markers_drawn;
    }
    std::ranges::stable_sort(drawables_, {}, &Drawable::depth);
    for (const Drawable& d : drawables_) {
        if (d.marker) {
            draw_marker(scene.markers[d.index], out);
        } else {
            draw_art_object(scene, scene.objects[d.index], out);
        }
    }
    // Barras de vida por encima de todo.
    for (const Bar& b : bars_) {
        add_health_bar(b.center_top, b.permille, out);
    }

    // Proyectiles: puntos a un cuarto del disco.
    const Vec2 shot_size = disc_size * 0.25f;
    for (const Vec2& p : scene.projectiles) {
        out.add(p - shot_size * 0.5f, shot_size, disc_, view_.projectile_color);
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
