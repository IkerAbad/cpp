#include <cmath>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "render/projection.hpp"
#include "sim/rng.hpp"

using rts::render::Camera;
using rts::render::IsoProjection;
using rts::render::Vec2;
using rts::render::ViewParams;

namespace {

ViewParams view_64x32() {
    ViewParams v;
    v.tile_width_px = 64;
    v.tile_height_px = 32;
    return v;
}

constexpr std::int32_t kMap = 256;

// Misma condición que for_each_visible_tile, evaluada casilla a casilla.
std::set<std::pair<int, int>> brute_force_visible(const IsoProjection& p, const Camera& cam, Vec2 screen) {
    std::set<std::pair<int, int>> out;
    for (int j = 0; j < kMap; ++j) {
        for (int i = 0; i < kMap; ++i) {
            const Vec2 top = p.tile_top(i, j);
            const float left = top.x - p.half_width();
            const float right = top.x + p.half_width();
            const float bottom = top.y + 2.0f * p.half_height();
            if (right > cam.origin.x && left < cam.origin.x + screen.x && bottom > cam.origin.y &&
                top.y < cam.origin.y + screen.y) {
                out.insert({i, j});
            }
        }
    }
    return out;
}

}  // namespace

TEST_CASE("Proyección: esquinas de casilla en los píxeles esperados") {
    const IsoProjection p(view_64x32());
    CHECK(p.tile_top(0, 0).x == 0.0f);
    CHECK(p.tile_top(0, 0).y == 0.0f);
    CHECK(p.tile_top(1, 0).x == 32.0f);  // un paso en x: derecha-abajo
    CHECK(p.tile_top(1, 0).y == 16.0f);
    CHECK(p.tile_top(0, 1).x == -32.0f);  // un paso en y: izquierda-abajo
    CHECK(p.tile_top(0, 1).y == 16.0f);
}

TEST_CASE("Proyección: ida y vuelta en las 65 536 casillas") {
    const IsoProjection p(view_64x32());
    for (int j = 0; j < kMap; ++j) {
        for (int i = 0; i < kMap; ++i) {
            // El centro de la casilla vuelve a caer en la misma casilla.
            const Vec2 center{static_cast<float>(i) + 0.5f, static_cast<float>(j) + 0.5f};
            const Vec2 back = p.world_to_tile(p.tile_to_world(center));
            REQUIRE(static_cast<int>(std::floor(back.x)) == i);
            REQUIRE(static_cast<int>(std::floor(back.y)) == j);
        }
    }
}

TEST_CASE("Recorte: coincide con la fuerza bruta en 200 cámaras aleatorias") {
    const IsoProjection p(view_64x32());
    rts::sim::Xoshiro256pp rng(123);
    const Vec2 world_lo = p.world_min(kMap, kMap);
    const Vec2 world_hi = p.world_max(kMap, kMap);
    for (int trial = 0; trial < 200; ++trial) {
        // Ventanas de 320x240 a 2560x1440, en cualquier punto del mapa y algo fuera.
        const Vec2 screen{static_cast<float>(rng.next_in_range(320, 2560)),
                          static_cast<float>(rng.next_in_range(240, 1440))};
        Camera cam;
        cam.origin = {static_cast<float>(rng.next_in_range(static_cast<int>(world_lo.x) - 3000,
                                                           static_cast<int>(world_hi.x) + 1000)) +
                          0.25f,
                      static_cast<float>(rng.next_in_range(-1000, static_cast<int>(world_hi.y) + 1000)) + 0.75f};

        std::set<std::pair<int, int>> iterated;
        int last_row = -1;
        bool ordered = true;
        rts::render::for_each_visible_tile(p, cam, screen, kMap, kMap, [&](int i, int j) {
            iterated.insert({i, j});
            ordered = ordered && (i + j) >= last_row;
            last_row = i + j;
        });
        REQUIRE(iterated == brute_force_visible(p, cam, screen));
        REQUIRE(ordered);
    }
}

TEST_CASE("Recorte: a 1920x1080 se visitan unas 2 200 casillas, no las 65 536") {
    const IsoProjection p(view_64x32());
    const Vec2 screen{1920.0f, 1080.0f};
    Camera cam;
    cam.center_on(p.tile_to_world({128.0f, 128.0f}), screen);
    int count = 0;
    rts::render::for_each_visible_tile(p, cam, screen, kMap, kMap, [&](int, int) { ++count; });
    MESSAGE("casillas visibles a 1920x1080: " << count);
    CHECK(count > 1'900);
    CHECK(count < 2'400);
}

TEST_CASE("Cámara: el centro de la pantalla nunca sale de la caja del mapa") {
    const IsoProjection p(view_64x32());
    const Vec2 screen{1600.0f, 900.0f};
    Camera cam;
    cam.origin = {-1.0e6f, 1.0e6f};
    cam.clamp_to_map(p, kMap, kMap, screen);
    const Vec2 center = cam.origin + screen * 0.5f;
    CHECK(static_cast<double>(center.x) == doctest::Approx(static_cast<double>(p.world_min(kMap, kMap).x)));
    CHECK(static_cast<double>(center.y) == doctest::Approx(static_cast<double>(p.world_max(kMap, kMap).y)));
}
