#include "sim/world.hpp"

#include <algorithm>
#include <cassert>
#include <memory>

#include "sim/state_hash.hpp"

namespace rts::sim {

namespace {

// Refleja una coordenada que ha salido de [lo, hi] e invierte su velocidad.
void reflect(Fixed& coord, Fixed& speed, Fixed lo, Fixed hi) noexcept {
    if (coord < lo) {
        coord = lo * 2 - coord;
        speed = -speed;
    } else if (coord > hi) {
        coord = hi * 2 - coord;
        speed = -speed;
    }
}

}  // namespace

World::World(const WorldParams& params)
    : map_(std::make_shared<TileMap>(generate_map(params.map))), rng_(params.demo.seed) {
    const DemoParams& demo = params.demo;
    assert(demo.point_count >= 0);
    assert(demo.area_tiles > 0);

    const std::int32_t side = std::min({demo.area_tiles, map_->width(), map_->height()});
    const std::int32_t min_x = (map_->width() - side) / 2;
    area_min_ = Fixed::from_int(min_x);
    area_max_ = Fixed::from_int(min_x + side);
    assert(demo.max_speed * 2 < area_max_ - area_min_);

    const std::int32_t lo = area_min_.raw();
    const std::int32_t hi = area_max_.raw();
    const std::int32_t speed = demo.max_speed.raw();
    for (std::int32_t i = 0; i < demo.point_count; ++i) {
        // Cada extracción del RNG va en su propia sentencia. Como argumentos de una
        // misma llamada, el orden de evaluación no está especificado en C++ (GCC y
        // Clang lo hacen al revés) y la simulación divergiría entre compiladores.
        const std::int32_t x = rng_.next_in_range(lo, hi);
        const std::int32_t y = rng_.next_in_range(lo, hi);
        const std::int32_t dx = rng_.next_in_range(-speed, speed);
        const std::int32_t dy = rng_.next_in_range(-speed, speed);
        const auto entity = registry_.create();
        registry_.emplace<Position>(entity, Fixed::from_raw(x), Fixed::from_raw(y));
        registry_.emplace<Velocity>(entity, Fixed::from_raw(dx), Fixed::from_raw(dy));
    }
}

void World::step() {
    registry_.view<Position, Velocity>().each([this](Position& pos, Velocity& vel) {
        pos.x += vel.dx;
        pos.y += vel.dy;
        reflect(pos.x, vel.dx, area_min_, area_max_);
        reflect(pos.y, vel.dy, area_min_, area_max_);
    });
    ++tick_;
}

std::uint64_t World::state_hash() const {
    StateHasher h;
    h.add_u32(tick_);
    for (const auto word : rng_.state()) {
        h.add_u64(word);
    }
    map_->hash_into(h);
    // El orden de iteración de una vista de EnTT es el del array denso del pool,
    // que depende solo de la secuencia de create/emplace/destroy: determinista.
    registry_.view<const Position, const Velocity>().each(
        [&h](const entt::entity e, const Position& pos, const Velocity& vel) {
            h.add_u32(entt::to_integral(e));
            h.add_fixed(pos.x);
            h.add_fixed(pos.y);
            h.add_fixed(vel.dx);
            h.add_fixed(vel.dy);
        });
    return h.value();
}

void World::write_snapshot(Snapshot& out) const {
    out.tick = tick_;
    out.map = map_;
    out.entities.clear();
    const auto view = registry_.view<const Position>();
    out.entities.reserve(view.size());
    view.each([&out](const entt::entity e, const Position& pos) {
        out.entities.push_back({entt::to_integral(e), pos});
    });
}

}  // namespace rts::sim
