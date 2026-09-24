#include "game/camera_control.hpp"

#include <algorithm>

namespace rts::game {

namespace {

float axis(bool negative, bool positive) noexcept {
    return (positive ? 1.0f : 0.0f) - (negative ? 1.0f : 0.0f);
}

}  // namespace

render::Vec2 camera_scroll(const CameraConfig& cfg, const CameraInput& in, render::Vec2 screen, float dt_s) noexcept {
    const auto key_speed = static_cast<float>(cfg.scroll_keys_px_per_s);
    const auto edge_speed = static_cast<float>(cfg.scroll_edge_px_per_s);
    const auto margin = static_cast<float>(cfg.edge_margin_px);

    render::Vec2 velocity{axis(in.left, in.right) * key_speed, axis(in.up, in.down) * key_speed};
    if (in.mouse_in_window && margin > 0.0f) {
        velocity.x += axis(in.mouse.x < margin, in.mouse.x >= screen.x - margin) * edge_speed;
        velocity.y += axis(in.mouse.y < margin, in.mouse.y >= screen.y - margin) * edge_speed;
    }
    const float cap = std::max(key_speed, edge_speed);
    velocity.x = std::clamp(velocity.x, -cap, cap);
    velocity.y = std::clamp(velocity.y, -cap, cap);
    return velocity * dt_s;
}

}  // namespace rts::game
