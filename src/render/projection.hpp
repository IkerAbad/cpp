#pragma once

// Proyección isométrica 2:1 y cámara. Presentación pura: usa float (permitido fuera de
// src/sim/) y no conoce SDL, así que se prueba sin GPU.
//
// Tres espacios:
//   casilla: coordenadas del mapa, continuas. La casilla (i, j) ocupa [i, i+1) x [j, j+1).
//   mundo:   píxeles de la proyección, sin cámara. Esquina (i, j) -> ((i-j)*hw, (i+j)*hh),
//            con hw = ancho/2 y hh = alto/2 del rombo.
//   pantalla: píxeles de ventana = mundo - origen de la cámara.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "render/view_params.hpp"

namespace rts::render {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    friend constexpr Vec2 operator+(Vec2 a, Vec2 b) noexcept { return {a.x + b.x, a.y + b.y}; }
    friend constexpr Vec2 operator-(Vec2 a, Vec2 b) noexcept { return {a.x - b.x, a.y - b.y}; }
    friend constexpr Vec2 operator*(Vec2 a, float k) noexcept { return {a.x * k, a.y * k}; }
};

class IsoProjection {
public:
    explicit IsoProjection(const ViewParams& view) noexcept
        : half_w_(static_cast<float>(view.tile_width_px) * 0.5f),
          half_h_(static_cast<float>(view.tile_height_px) * 0.5f) {}

    [[nodiscard]] float half_width() const noexcept { return half_w_; }
    [[nodiscard]] float half_height() const noexcept { return half_h_; }

    [[nodiscard]] Vec2 tile_to_world(Vec2 t) const noexcept { return {(t.x - t.y) * half_w_, (t.x + t.y) * half_h_}; }

    [[nodiscard]] Vec2 world_to_tile(Vec2 w) const noexcept {
        const float a = w.x / half_w_;
        const float b = w.y / half_h_;
        return {(b + a) * 0.5f, (b - a) * 0.5f};
    }

    // Esquina superior del rombo de la casilla (i, j) en espacio mundo.
    [[nodiscard]] Vec2 tile_top(std::int32_t i, std::int32_t j) const noexcept {
        return {static_cast<float>(i - j) * half_w_, static_cast<float>(i + j) * half_h_};
    }

    // Caja que contiene todo el mapa en espacio mundo.
    [[nodiscard]] Vec2 world_min(std::int32_t /*map_w*/, std::int32_t map_h) const noexcept {
        return {-static_cast<float>(map_h) * half_w_, 0.0f};
    }
    [[nodiscard]] Vec2 world_max(std::int32_t map_w, std::int32_t map_h) const noexcept {
        return {static_cast<float>(map_w) * half_w_, static_cast<float>(map_w + map_h) * half_h_};
    }

private:
    float half_w_;
    float half_h_;
};

// La cámara es el punto del mundo que cae en la esquina superior izquierda de la pantalla.
struct Camera {
    Vec2 origin;

    [[nodiscard]] Vec2 world_to_screen(Vec2 w) const noexcept { return w - origin; }
    [[nodiscard]] Vec2 screen_to_world(Vec2 s) const noexcept { return s + origin; }

    // Mantiene el centro de la pantalla dentro de la caja del mapa.
    void clamp_to_map(const IsoProjection& proj, std::int32_t map_w, std::int32_t map_h, Vec2 screen) noexcept {
        const Vec2 lo = proj.world_min(map_w, map_h);
        const Vec2 hi = proj.world_max(map_w, map_h);
        const Vec2 half = screen * 0.5f;
        const Vec2 center = origin + half;
        origin = Vec2{std::clamp(center.x, lo.x, hi.x), std::clamp(center.y, lo.y, hi.y)} - half;
    }

    // Centra la vista en un punto del mundo.
    void center_on(Vec2 world, Vec2 screen) noexcept { origin = world - screen * 0.5f; }
};

// Llama a fn(i, j) para cada casilla del mapa cuyo rectángulo envolvente corta la
// pantalla, en orden de pintado (filas r = i + j crecientes: de atrás hacia delante).
// Coste O(casillas visibles): recorre filas y columnas de pantalla, no el mapa entero.
template <typename Fn>
void for_each_visible_tile(const IsoProjection& proj, const Camera& cam, Vec2 screen, std::int32_t map_w,
                           std::int32_t map_h, Fn&& fn) {
    const float hw = proj.half_width();
    const float hh = proj.half_height();
    const Vec2 o = cam.origin;
    // El rombo (i, j), con r = i + j y c = i - j, ocupa x en [(c-1)hw, (c+1)hw] e
    // y en [r hh, (r+2) hh]. Se pide solape estricto con [o, o + screen].
    const auto r_min = static_cast<std::int32_t>(std::floor(o.y / hh)) - 2;
    const auto r_max = static_cast<std::int32_t>(std::ceil((o.y + screen.y) / hh));
    const auto c_min = static_cast<std::int32_t>(std::floor(o.x / hw)) - 1;
    const auto c_max = static_cast<std::int32_t>(std::ceil((o.x + screen.x) / hw)) + 1;

    const std::int32_t r_lo = std::max(r_min, 0);
    const std::int32_t r_hi = std::min(r_max, map_w + map_h - 2);
    for (std::int32_t r = r_lo; r <= r_hi; ++r) {
        // i = (r + c) / 2 exige r + c par; i en [0, map_w) y j = r - i en [0, map_h).
        std::int32_t c_lo = std::max(c_min, std::max(r - 2 * (map_h - 1), -r));
        const std::int32_t c_hi = std::min(c_max, std::min(r, 2 * (map_w - 1) - r));
        if (((r + c_lo) & 1) != 0) {
            ++c_lo;
        }
        for (std::int32_t c = c_lo; c <= c_hi; c += 2) {
            const std::int32_t i = (r + c) / 2;
            const std::int32_t j = (r - c) / 2;
            const float left = static_cast<float>(c - 1) * hw;
            const float top = static_cast<float>(r) * hh;
            if (left + 2.0f * hw > o.x && left < o.x + screen.x && top + 2.0f * hh > o.y && top < o.y + screen.y) {
                fn(i, j);
            }
        }
    }
}

}  // namespace rts::render
