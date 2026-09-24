#pragma once

// Selección de entidades con el ratón. Es estado local del jugador, no de la
// simulación: en lockstep (M8) lo que viaja por la red son órdenes con los ids
// seleccionados, nunca la selección en sí.

#include <cstdint>
#include <span>
#include <vector>

#include "game/config.hpp"
#include "render/projection.hpp"

namespace rts::game {

// Entidad ya proyectada a pantalla en el fotograma actual.
struct ScreenEntity {
    std::uint32_t id = 0;
    render::Vec2 pos;
};

struct ScreenRect {
    render::Vec2 min;
    render::Vec2 max;

    [[nodiscard]] static ScreenRect from_corners(render::Vec2 a, render::Vec2 b) noexcept;
    [[nodiscard]] bool contains(render::Vec2 p) const noexcept {
        return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y;
    }
};

class Selection {
public:
    explicit Selection(const SelectionConfig& cfg) noexcept : cfg_(cfg) {}

    void begin_drag(render::Vec2 at) noexcept;
    void update_drag(render::Vec2 at) noexcept;

    // Cierra el arrastre. Si apenas se movió cuenta como clic: selecciona la entidad
    // más cercana dentro de click_radius_px. Con additive (Mayús) se suma a la
    // selección actual; si no, la reemplaza.
    void end_drag(render::Vec2 at, bool additive, std::span<const ScreenEntity> entities);

    void clear() noexcept { selected_.clear(); }

    [[nodiscard]] bool dragging() const noexcept { return dragging_; }
    // Rectángulo del arrastre en curso, solo si ya superó el umbral de clic.
    [[nodiscard]] bool has_visible_rect() const noexcept;
    [[nodiscard]] ScreenRect drag_rect() const noexcept { return ScreenRect::from_corners(start_, current_); }

    // Ids seleccionados, ordenados y sin repetir.
    [[nodiscard]] const std::vector<std::uint32_t>& selected() const noexcept { return selected_; }
    [[nodiscard]] bool is_selected(std::uint32_t id) const noexcept;

private:
    [[nodiscard]] bool is_click() const noexcept;

    SelectionConfig cfg_;
    bool dragging_ = false;
    render::Vec2 start_;
    render::Vec2 current_;
    std::vector<std::uint32_t> selected_;
};

}  // namespace rts::game
