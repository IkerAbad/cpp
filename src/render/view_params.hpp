#pragma once

#include <array>
#include <cstdint>

namespace rts::render {

using Rgba = std::array<std::uint8_t, 4>;

// Parámetros de la proyección y del aspecto provisional. Enteros: vienen de
// data/config/engine.toml, que solo admite enteros.
struct ViewParams {
    std::int32_t tile_width_px = 0;   // ancho del rombo de una casilla
    std::int32_t tile_height_px = 0;  // alto del rombo; 2:1 => la mitad del ancho
    std::int32_t marker_radius_px = 0;

    // Sombreado por altura: brillo del nivel 0 en % (el nivel máximo es 100 %) y
    // ajuste por cada nivel de diferencia con la casilla de detrás (luz del norte).
    std::int32_t elevation_shade_min_percent = 0;
    std::int32_t hillshade_step_percent = 0;

    Rgba clear_color{};
    Rgba marker_color{};
    Rgba marker_selected_color{};
    Rgba selection_rect_color{};  // alfa < 255: relleno translúcido
    Rgba hover_tile_color{};
};

}  // namespace rts::render
