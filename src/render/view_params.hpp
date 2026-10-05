#pragma once

#include <array>
#include <cstdint>

#include "sim/units.hpp"

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
    Rgba debug_overlay_color{};  // superposiciones de depuración (portales, campo de flujo)
    Rgba marker_selected_color{};
    Rgba selection_rect_color{};  // alfa < 255: relleno translúcido
    Rgba hover_tile_color{};

    // Objetos estáticos (M3): lado del rombo interior respecto a la huella, brillo de un
    // edificio en obra y colores del fantasma de colocación y de cada recurso.
    std::int32_t building_body_percent = 0;
    std::int32_t node_body_percent = 0;
    std::int32_t construction_shade_percent = 0;
    Rgba ghost_valid_color{};
    Rgba ghost_invalid_color{};
    std::array<Rgba, sim::kResourceCount> resource_colors{};  // por sim::Resource

    // Combate (M4): barras de vida, proyectiles y anillo de héroe.
    std::int32_t health_bar_width_px = 0;
    std::int32_t health_bar_height_px = 0;
    std::int32_t health_low_permille = 0;  // por debajo, la barra usa health_low_color
    Rgba health_back_color{};
    Rgba health_color{};
    Rgba health_low_color{};
    Rgba projectile_color{};
    Rgba hero_color{};
    Rgba fire_color{};                    // un edificio en llamas tiende a este color
    std::int32_t burned_shade_percent = 0;  // brillo de un edificio quemado

    // Niebla de guerra: color de lo no explorado, brillo de lo explorado que no se ve
    // ahora (y de lo recordado) y velo de la noche (color y opacidad en plena noche).
    Rgba fog_unexplored_color{};
    std::int32_t fog_explored_shade_percent = 0;
    Rgba night_color{};

    // Minimapa: ancho en píxeles (alto, la mitad: es un rombo como la vista) y celdas
    // por lado en que se resume el terreno.
    std::int32_t minimap_width_px = 0;
    std::int32_t minimap_cells = 1;
};

}  // namespace rts::render
