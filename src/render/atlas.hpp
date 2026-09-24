#pragma once

// Atlas de texturas RGBA8 generado por código. Mientras no haya arte propio, los
// sprites son formas planas en blanco que el shader tiñe con el color de cada
// instancia: un único rombo sirve para todos los terrenos.

#include <array>
#include <cstdint>
#include <vector>

#include "render/view_params.hpp"

namespace rts::render {

// Rectángulo de un sprite en el atlas, en coordenadas UV normalizadas a 16 bits
// (0 = borde izquierdo/superior, 65535 = derecho/inferior), como las lee el shader.
struct AtlasRegion {
    std::uint16_t u0 = 0;
    std::uint16_t v0 = 0;
    std::uint16_t u1 = 0;
    std::uint16_t v1 = 0;
    std::int32_t x_px = 0;  // origen en el atlas, en píxeles
    std::int32_t y_px = 0;
    std::int32_t width_px = 0;
    std::int32_t height_px = 0;
};

struct Atlas {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::vector<std::uint8_t> rgba;  // width * height * 4, fila a fila

    AtlasRegion tile_diamond;   // rombo de una casilla, con borde ligeramente oscuro
    AtlasRegion marker_disc;    // disco relleno
    AtlasRegion marker_ring;    // anillo de selección
    AtlasRegion solid;          // bloque blanco para rectángulos de color

    [[nodiscard]] std::array<std::uint8_t, 4> texel(std::int32_t x, std::int32_t y) const noexcept;
};

// Tamaño cuadrado del atlas: potencia de dos mínima para los sprites de la vista.
[[nodiscard]] Atlas build_atlas(const ViewParams& view);

}  // namespace rts::render
