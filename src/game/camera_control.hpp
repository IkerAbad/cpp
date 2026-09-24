#pragma once

// Desplazamiento de cámara por teclado y por borde de pantalla. Sin SDL: la entrada
// llega ya traducida, así que se prueba con estados sintéticos.

#include "game/config.hpp"
#include "render/projection.hpp"

namespace rts::game {

struct CameraInput {
    bool left = false;
    bool right = false;
    bool up = false;
    bool down = false;
    // Posición del ratón en píxeles de ventana; solo cuenta si mouse_in_window.
    render::Vec2 mouse;
    bool mouse_in_window = false;
};

// Desplazamiento de la cámara en píxeles de mundo para un fotograma de dt_s segundos.
// Teclado y borde se suman por eje, con tope en la mayor de las dos velocidades.
[[nodiscard]] render::Vec2 camera_scroll(const CameraConfig& cfg, const CameraInput& in, render::Vec2 screen,
                                         float dt_s) noexcept;

}  // namespace rts::game
