#pragma once

// Atlas de texturas RGBA8 generado por código. Lleva las formas básicas (rombo, disco,
// anillo, bloque sólido, en blanco para teñirlas) y, con un ArtSpec, todo el arte propio
// (F1): figuras en cada pose con su capa del jugador, edificios, recursos, texturas de
// terreno, llamas y humo.

#include <array>
#include <cstdint>
#include <vector>

#include "render/art.hpp"
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

// Sprite con su ancla: el píxel (desde la esquina superior izquierda de la región) que
// se pone sobre el punto del mapa.
struct Sprite {
    AtlasRegion region;
    Vec2 anchor;

    [[nodiscard]] bool valid() const noexcept { return region.width_px > 0; }
};

// Cuerpo y capa del jugador (team no válido si no tiene).
struct SpritePair {
    Sprite body;
    Sprite team;
};

struct Atlas {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::vector<std::uint8_t> rgba;  // width * height * 4, fila a fila

    AtlasRegion tile_diamond;   // rombo de una casilla, con borde ligeramente oscuro
    AtlasRegion marker_disc;    // disco relleno
    AtlasRegion marker_ring;    // anillo de selección
    AtlasRegion solid;          // bloque blanco para rectángulos de color

    // Arte propio (vacío sin ArtSpec).
    std::vector<std::vector<AtlasRegion>> terrain;           // por TerrainId: variantes en grises
    std::vector<std::array<SpritePair, kPoseCount>> units;  // por UnitTypeId y pose
    std::vector<SpritePair> buildings;                       // por BuildingTypeId
    std::vector<std::vector<Sprite>> nodes;                  // por NodeTypeId: variantes
    std::vector<Sprite> flames;                              // fotogramas
    Sprite smoke;
    Sprite ground_ring;
    Rgba smoke_color{};
    std::vector<std::uint8_t> flat_buildings;  // por BuildingTypeId: campos y caminos (a ras de suelo)

    [[nodiscard]] std::array<std::uint8_t, 4> texel(std::int32_t x, std::int32_t y) const noexcept;
    [[nodiscard]] bool has_art() const noexcept { return !units.empty() || !buildings.empty(); }
};

// Tamaño cuadrado del atlas: la potencia de dos más pequeña en la que cabe todo.
[[nodiscard]] Atlas build_atlas(const ViewParams& view, const ArtSpec& art = {});

}  // namespace rts::render
