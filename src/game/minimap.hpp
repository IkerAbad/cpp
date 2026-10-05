#pragma once

// Minimapa: qué se dibuja en él, sin ImGui ni GPU (se prueba). Respeta la niebla del
// jugador que mira: sus unidades y edificios siempre; del enemigo, solo lo que ve
// ahora y los edificios que recuerda; los recursos, en lo explorado.

#include <cstdint>
#include <optional>
#include <vector>

#include "render/view_params.hpp"
#include "sim/world.hpp"

namespace rts::game {

struct MinimapDot {
    sim::TileCoord origin;
    std::int32_t size = 1;  // casillas por lado (edificios y recursos)
    render::Rgba color{};
};

// player_colors[p]: color del jugador p; resource_colors[r]: del recurso r.
// viewer vacío: se ve todo (repeticiones).
[[nodiscard]] std::vector<MinimapDot> minimap_dots(const sim::Snapshot& snap, std::optional<sim::PlayerId> viewer,
                                                   const std::vector<render::Rgba>& player_colors,
                                                   const std::vector<render::Rgba>& node_colors);

}  // namespace rts::game
