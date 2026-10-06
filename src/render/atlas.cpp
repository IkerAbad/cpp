#include "render/atlas.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <format>
#include <stdexcept>

namespace rts::render {

namespace {

// Submuestreo por píxel para el antialiasing de los bordes: 4x4 = 16 muestras.
constexpr int kSupersample = 4;
constexpr float kSupersampleF = static_cast<float>(kSupersample);
constexpr float kSamplesPerPixel = kSupersampleF * kSupersampleF;
constexpr float kByteMaxF = 255.0f;
// Separación entre sprites para que el filtrado nunca mezcle vecinos.
constexpr std::int32_t kPadding = 2;
// Brillo del borde del rombo (de 255): marca la retícula sin dibujar líneas.
constexpr float kDiamondEdgeShade = 0.82f;
constexpr float kDiamondEdgeWidth = 0.06f;  // fracción del semieje
// Grosor del anillo de selección en píxeles.
constexpr float kRingThicknessPx = 1.5f;
constexpr std::int32_t kSolidSize = 4;
// Lados del atlas que se prueban (potencias de dos).
constexpr std::int32_t kMinAtlasSize = 64;
constexpr std::int32_t kMaxAtlasSize = 8192;
// Ancho del anillo de selección a los pies, en radios de marcador.
constexpr std::int32_t kRingWidthPerRadius = 5;

class ShelfPacker {
public:
    explicit ShelfPacker(std::int32_t size) : size_(size) {}

    // Devuelve la esquina superior izquierda, o false si no cabe.
    bool place(std::int32_t w, std::int32_t h, std::int32_t& out_x, std::int32_t& out_y) {
        if (x_ + w + kPadding > size_) {
            x_ = 0;
            y_ += shelf_h_ + kPadding;
            shelf_h_ = 0;
        }
        if (y_ + h + kPadding > size_ || w + kPadding > size_) {
            return false;
        }
        out_x = x_ + kPadding;
        out_y = y_ + kPadding;
        x_ += w + kPadding;
        shelf_h_ = std::max(shelf_h_, h);
        return true;
    }

private:
    std::int32_t size_;
    std::int32_t x_ = 0;
    std::int32_t y_ = 0;
    std::int32_t shelf_h_ = 0;
};

std::uint16_t to_unorm16(std::int32_t px, std::int32_t size) {
    return static_cast<std::uint16_t>((static_cast<std::int64_t>(px) * 65535) / size);
}

AtlasRegion region_at(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h, std::int32_t size) {
    return {to_unorm16(x, size), to_unorm16(y, size), to_unorm16(x + w, size), to_unorm16(y + h, size), x, y, w, h};
}

// Forma básica en blanco con alfa = cobertura; shape(u, v) recibe coordenadas locales en
// píxeles (continuas) y devuelve el brillo [0, 1] o un valor negativo si está fuera.
template <typename Shape>
Image shape_image(std::int32_t w, std::int32_t h, Shape&& shape) {
    Image img;
    img.width = w;
    img.height = h;
    img.rgba.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 0);
    for (std::int32_t y = 0; y < h; ++y) {
        for (std::int32_t x = 0; x < w; ++x) {
            float coverage = 0.0f;
            float brightness = 0.0f;
            for (int sy = 0; sy < kSupersample; ++sy) {
                for (int sx = 0; sx < kSupersample; ++sx) {
                    const float u = static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) / kSupersampleF;
                    const float v = static_cast<float>(y) + (static_cast<float>(sy) + 0.5f) / kSupersampleF;
                    const float b = shape(u, v);
                    if (b >= 0.0f) {
                        coverage += 1.0f;
                        brightness += b;
                    }
                }
            }
            const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4;
            const float lum = coverage > 0.0f ? brightness / coverage : 0.0f;
            const auto c = static_cast<std::uint8_t>(std::lround(lum * kByteMaxF));
            img.rgba[i + 0] = c;
            img.rgba[i + 1] = c;
            img.rgba[i + 2] = c;
            img.rgba[i + 3] = static_cast<std::uint8_t>(std::lround(coverage / kSamplesPerPixel * kByteMaxF));
        }
    }
    return img;
}

// Una imagen pendiente de colocar y dónde se apunta su región.
struct Item {
    Image image;
    AtlasRegion* region = nullptr;
    Sprite* sprite = nullptr;
};

// Coloca todas las imágenes (de la más alta a la más baja) en un cuadrado de lado size;
// falso si no caben.
bool pack(std::vector<Item>& items, const std::vector<std::size_t>& order, std::int32_t size,
          std::vector<std::pair<std::int32_t, std::int32_t>>& pos) {
    ShelfPacker packer(size);
    pos.assign(items.size(), {0, 0});
    for (const std::size_t i : order) {
        const Image& img = items[i].image;
        if (img.empty()) {
            continue;
        }
        if (!packer.place(img.width, img.height, pos[i].first, pos[i].second)) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::array<std::uint8_t, 4> Atlas::texel(std::int32_t x, std::int32_t y) const noexcept {
    const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4;
    return {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
}

Atlas build_atlas(const ViewParams& view, const ArtSpec& art) {
    Atlas atlas;
    const std::int32_t tile_w = view.tile_width_px;
    const std::int32_t tile_h = view.tile_height_px;
    const std::int32_t marker = 2 * view.marker_radius_px + 2;

    // Las regiones del arte se dimensionan antes de reunir las imágenes: los punteros a
    // ellas no cambian después.
    atlas.terrain.resize(art.terrain.size());
    for (auto& variants : atlas.terrain) {
        variants.resize(static_cast<std::size_t>(art.terrain_variants));
    }
    atlas.units.resize(art.units.size());
    atlas.buildings.resize(art.buildings.size());
    atlas.nodes.resize(art.nodes.size());
    for (std::size_t i = 0; i < art.nodes.size(); ++i) {
        atlas.nodes[i].resize(static_cast<std::size_t>(art.nodes[i].variants));
    }
    const bool with_art = !art.units.empty() || !art.buildings.empty();
    atlas.flames.resize(with_art ? static_cast<std::size_t>(art.flame_frames) : 0);

    std::vector<Item> items;
    const auto add_region = [&](Image img, AtlasRegion* r) { items.push_back({std::move(img), r, nullptr}); };
    const auto add_sprite = [&](Image img, Sprite* sp) { items.push_back({std::move(img), nullptr, sp}); };

    const float hw = static_cast<float>(tile_w) * 0.5f;
    const float hh = static_cast<float>(tile_h) * 0.5f;
    add_region(shape_image(tile_w, tile_h, [&](float u, float v) {
                   // Distancia "manhattan" normalizada al centro: 1 en el borde del rombo.
                   const float d = std::abs(u - hw) / hw + std::abs(v - hh) / hh;
                   if (d > 1.0f) {
                       return -1.0f;
                   }
                   return d > 1.0f - kDiamondEdgeWidth ? kDiamondEdgeShade : 1.0f;
               }),
               &atlas.tile_diamond);
    const float r = static_cast<float>(view.marker_radius_px);
    const float c = static_cast<float>(marker) * 0.5f;
    add_region(shape_image(marker, marker,
                           [&](float u, float v) { return std::hypot(u - c, v - c) <= r ? 1.0f : -1.0f; }),
               &atlas.marker_disc);
    add_region(shape_image(marker, marker,
                           [&](float u, float v) {
                               const float d = std::hypot(u - c, v - c);
                               return (d <= r && d >= r - kRingThicknessPx) ? 1.0f : -1.0f;
                           }),
               &atlas.marker_ring);
    add_region(shape_image(kSolidSize, kSolidSize, [](float, float) { return 1.0f; }), &atlas.solid);

    for (std::size_t t = 0; t < art.terrain.size(); ++t) {
        for (std::size_t v = 0; v < atlas.terrain[t].size(); ++v) {
            add_region(paint_terrain(art.terrain[t], view, static_cast<std::int32_t>(v)), &atlas.terrain[t][v]);
        }
    }
    for (std::size_t u = 0; u < art.units.size(); ++u) {
        for (std::size_t p = 0; p < kPoseCount; ++p) {
            SpriteImages s = paint_unit(art.units[u], art.style, static_cast<Pose>(p));
            add_sprite(std::move(s.body), &atlas.units[u][p].body);
            add_sprite(std::move(s.team), &atlas.units[u][p].team);
        }
    }
    for (std::size_t b = 0; b < art.buildings.size(); ++b) {
        SpriteImages s = paint_building(art.buildings[b], art.style, view);
        add_sprite(std::move(s.body), &atlas.buildings[b].body);
        add_sprite(std::move(s.team), &atlas.buildings[b].team);
    }
    for (std::size_t n = 0; n < art.nodes.size(); ++n) {
        for (std::size_t v = 0; v < atlas.nodes[n].size(); ++v) {
            add_sprite(paint_node(art.nodes[n], art.style, view, static_cast<std::int32_t>(v)), &atlas.nodes[n][v]);
        }
    }
    for (std::size_t f = 0; f < atlas.flames.size(); ++f) {
        add_sprite(paint_flame(art.effects, static_cast<std::int32_t>(f), art.flame_frames), &atlas.flames[f]);
    }
    for (const BuildingArt& b : art.buildings) {
        atlas.flat_buildings.push_back(b.shape == BuildingShape::Field || b.shape == BuildingShape::Road ? 1 : 0);
    }
    atlas.smoke_color = {art.effects.smoke[0], art.effects.smoke[1], art.effects.smoke[2], 255};
    if (with_art) {
        add_sprite(paint_smoke(art.effects), &atlas.smoke);
        add_sprite(paint_ground_ring(view.marker_radius_px * kRingWidthPerRadius), &atlas.ground_ring);
    }

    // De la más alta a la más baja; el lado, la menor potencia de dos en que caben.
    std::vector<std::size_t> order(items.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) {
        return items[a].image.height > items[b].image.height;
    });
    std::vector<std::pair<std::int32_t, std::int32_t>> pos;
    std::int32_t size = kMinAtlasSize;
    while (!pack(items, order, size, pos)) {
        if (size >= kMaxAtlasSize) {
            throw std::length_error(std::format("el arte no cabe en un atlas de {0}x{0}", kMaxAtlasSize));
        }
        size *= 2;
    }
    atlas.width = size;
    atlas.height = size;
    atlas.rgba.assign(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4, 0);
    for (std::size_t i = 0; i < items.size(); ++i) {
        const Image& img = items[i].image;
        if (img.empty()) {
            continue;
        }
        const auto [ox, oy] = pos[i];
        for (std::int32_t y = 0; y < img.height; ++y) {
            const auto src = img.rgba.begin() + static_cast<std::ptrdiff_t>(y) * img.width * 4;
            const auto dst = atlas.rgba.begin() +
                             (static_cast<std::ptrdiff_t>(oy + y) * size + ox) * 4;
            std::copy(src, src + static_cast<std::ptrdiff_t>(img.width) * 4, dst);
        }
        const AtlasRegion reg = region_at(ox, oy, img.width, img.height, size);
        if (items[i].region != nullptr) {
            *items[i].region = reg;
        }
        if (items[i].sprite != nullptr) {
            *items[i].sprite = {reg, img.anchor};
        }
    }
    // El bloque sólido se muestrea en su interior (un texel de margen) para que el
    // filtrado no toque el borde transparente.
    const AtlasRegion block = atlas.solid;
    const std::int32_t inner = kSolidSize - 2;
    atlas.solid = region_at(block.x_px + 1, block.y_px + 1, inner, inner, size);
    return atlas;
}

}  // namespace rts::render
