#include <vector>

#include <doctest/doctest.h>

#include "game/camera_control.hpp"
#include "game/selection.hpp"

using rts::game::CameraConfig;
using rts::game::CameraInput;
using rts::game::ScreenEntity;
using rts::game::Selection;
using rts::game::SelectionConfig;
using rts::render::Vec2;

namespace {

constexpr SelectionConfig kSel{4, 10};

const std::vector<ScreenEntity> kEntities{
    {1, {100.0f, 100.0f}},
    {2, {150.0f, 120.0f}},
    {3, {400.0f, 400.0f}},
    {4, {104.0f, 100.0f}},
};

}  // namespace

TEST_CASE("Selección: el rectángulo selecciona lo que contiene, en cualquier dirección de arrastre") {
    Selection sel(kSel);
    sel.begin_drag({200.0f, 200.0f});  // de abajo-derecha a arriba-izquierda
    sel.update_drag({90.0f, 90.0f});
    CHECK(sel.has_visible_rect());
    sel.end_drag({90.0f, 90.0f}, false, kEntities);
    CHECK(sel.selected() == std::vector<std::uint32_t>{1, 2, 4});
    CHECK_FALSE(sel.dragging());
}

TEST_CASE("Selección: un arrastre menor que el umbral es un clic sobre la más cercana") {
    Selection sel(kSel);
    sel.begin_drag({103.0f, 101.0f});
    sel.update_drag({105.0f, 102.0f});
    CHECK_FALSE(sel.has_visible_rect());
    sel.end_drag({105.0f, 102.0f}, false, kEntities);
    CHECK(sel.selected() == std::vector<std::uint32_t>{4});
}

TEST_CASE("Selección: clic en vacío sin Mayús vacía la selección") {
    Selection sel(kSel);
    sel.begin_drag({0.0f, 0.0f});
    sel.end_drag({500.0f, 500.0f}, false, kEntities);
    REQUIRE(sel.selected().size() == 4);
    sel.begin_drag({700.0f, 700.0f});
    sel.end_drag({700.0f, 700.0f}, false, kEntities);
    CHECK(sel.selected().empty());
}

TEST_CASE("Selección: con Mayús se suma sin duplicar") {
    Selection sel(kSel);
    sel.begin_drag({400.0f, 400.0f});
    sel.end_drag({400.0f, 400.0f}, false, kEntities);
    sel.begin_drag({90.0f, 90.0f});
    sel.end_drag({160.0f, 130.0f}, true, kEntities);
    sel.begin_drag({100.0f, 100.0f});
    sel.end_drag({100.0f, 100.0f}, true, kEntities);
    CHECK(sel.selected() == std::vector<std::uint32_t>{1, 2, 3, 4});
    CHECK(sel.is_selected(3));
    sel.clear();
    CHECK_FALSE(sel.is_selected(3));
}

TEST_CASE("Cámara: teclado, borde y tope de velocidad") {
    constexpr CameraConfig cfg{1000, 500, 8};
    const Vec2 screen{800.0f, 600.0f};

    CameraInput keys;
    keys.right = true;
    keys.up = true;
    Vec2 d = rts::game::camera_scroll(cfg, keys, screen, 0.5f);
    CHECK(static_cast<double>(d.x) == doctest::Approx(500.0));
    CHECK(static_cast<double>(d.y) == doctest::Approx(-500.0));

    CameraInput edge;
    edge.mouse = {2.0f, 300.0f};  // pegado al borde izquierdo
    edge.mouse_in_window = true;
    d = rts::game::camera_scroll(cfg, edge, screen, 1.0f);
    CHECK(static_cast<double>(d.x) == doctest::Approx(-500.0));
    CHECK(static_cast<double>(d.y) == doctest::Approx(0.0));

    edge.mouse_in_window = false;  // ratón fuera de la ventana: el borde no cuenta
    d = rts::game::camera_scroll(cfg, edge, screen, 1.0f);
    CHECK(static_cast<double>(d.x) == doctest::Approx(0.0));

    CameraInput both = edge;  // teclado y borde en el mismo sentido: tope en la mayor
    both.mouse_in_window = true;
    both.left = true;
    d = rts::game::camera_scroll(cfg, both, screen, 1.0f);
    CHECK(static_cast<double>(d.x) == doctest::Approx(-1000.0));
}
