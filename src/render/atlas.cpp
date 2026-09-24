#include "render/atlas.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

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

// Pinta en blanco con alfa = cobertura; coverage(u, v) recibe coordenadas locales en
// píxeles (continuas) y devuelve el brillo [0, 1] o un valor negativo si está fuera.
template <typename Shape>
AtlasRegion paint(Atlas& atlas, ShelfPacker& packer, std::int32_t w, std::int32_t h, Shape&& shape) {
    std::int32_t ox = 0;
    std::int32_t oy = 0;
    const bool placed = packer.place(w, h, ox, oy);
    assert(placed);
    (void)placed;
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
            const std::size_t i = (static_cast<std::size_t>(oy + y) * static_cast<std::size_t>(atlas.width) +
                                   static_cast<std::size_t>(ox + x)) *
                                  4;
            const float lum = coverage > 0.0f ? brightness / coverage : 0.0f;
            const auto c = static_cast<std::uint8_t>(std::lround(lum * kByteMaxF));
            atlas.rgba[i + 0] = c;
            atlas.rgba[i + 1] = c;
            atlas.rgba[i + 2] = c;
            atlas.rgba[i + 3] = static_cast<std::uint8_t>(std::lround(coverage / kSamplesPerPixel * kByteMaxF));
        }
    }
    return {to_unorm16(ox, atlas.width), to_unorm16(oy, atlas.height), to_unorm16(ox + w, atlas.width),
            to_unorm16(oy + h, atlas.height), ox, oy, w, h};
}

}  // namespace

std::array<std::uint8_t, 4> Atlas::texel(std::int32_t x, std::int32_t y) const noexcept {
    const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4;
    return {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
}

Atlas build_atlas(const ViewParams& view) {
    const std::int32_t tile_w = view.tile_width_px;
    const std::int32_t tile_h = view.tile_height_px;
    const std::int32_t marker = 2 * view.marker_radius_px + 2;

    // Potencia de dos que acoge una fila con los cuatro sprites.
    const std::int32_t needed = tile_w + 2 * marker + kSolidSize + 8 * kPadding;
    std::int32_t size = 64;
    while (size < needed) {
        size *= 2;
    }

    Atlas atlas;
    atlas.width = size;
    atlas.height = size;
    atlas.rgba.assign(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4, 0);
    ShelfPacker packer(size);

    const float hw = static_cast<float>(tile_w) * 0.5f;
    const float hh = static_cast<float>(tile_h) * 0.5f;
    atlas.tile_diamond = paint(atlas, packer, tile_w, tile_h, [&](float u, float v) {
        // Distancia "manhattan" normalizada al centro: 1 en el borde del rombo.
        const float d = std::abs(u - hw) / hw + std::abs(v - hh) / hh;
        if (d > 1.0f) {
            return -1.0f;
        }
        return d > 1.0f - kDiamondEdgeWidth ? kDiamondEdgeShade : 1.0f;
    });

    const float r = static_cast<float>(view.marker_radius_px);
    const float c = static_cast<float>(marker) * 0.5f;
    atlas.marker_disc = paint(atlas, packer, marker, marker, [&](float u, float v) {
        const float d = std::hypot(u - c, v - c);
        return d <= r ? 1.0f : -1.0f;
    });
    atlas.marker_ring = paint(atlas, packer, marker, marker, [&](float u, float v) {
        const float d = std::hypot(u - c, v - c);
        return (d <= r && d >= r - kRingThicknessPx) ? 1.0f : -1.0f;
    });

    // El bloque sólido se muestrea en su interior (un texel de margen) para que el
    // filtrado no toque el borde transparente.
    const AtlasRegion block = paint(atlas, packer, kSolidSize, kSolidSize, [](float, float) { return 1.0f; });
    const std::int32_t bx = block.x_px + 1;
    const std::int32_t by = block.y_px + 1;
    const std::int32_t inner = kSolidSize - 2;
    atlas.solid = {to_unorm16(bx, size), to_unorm16(by, size), to_unorm16(bx + inner, size),
                   to_unorm16(by + inner, size), bx, by, inner, inner};
    return atlas;
}

}  // namespace rts::render
