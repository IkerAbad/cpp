#include "sim/world.hpp"

#include <cassert>

#include "sim/state_hash.hpp"

namespace rts::sim {

namespace {

// Refleja una coordenada que ha salido de [0, limit] e invierte su velocidad.
void reflect(Fixed& coord, Fixed& speed, Fixed limit) noexcept {
    if (coord < Fixed{}) {
        coord = -coord;
        speed = -speed;
    } else if (coord > limit) {
        coord = limit * 2 - coord;
        speed = -speed;
    }
}

}  // namespace

World::World(const DemoParams& params)
    : rng_(params.seed), arena_size_(Fixed::from_int(params.arena_tiles)) {
    assert(params.point_count >= 0);
    assert(params.arena_tiles > 0);
    assert(params.max_speed * 2 < arena_size_);

    const std::int32_t arena_raw = arena_size_.raw();
    const std::int32_t speed_raw = params.max_speed.raw();
    for (std::int32_t i = 0; i < params.point_count; ++i) {
        // Cada extracción del RNG va en su propia sentencia. Como argumentos de una
        // misma llamada, el orden de evaluación no está especificado en C++ (GCC y
        // Clang lo hacen al revés) y la simulación divergiría entre compiladores.
        const std::int32_t x = rng_.next_in_range(0, arena_raw);
        const std::int32_t y = rng_.next_in_range(0, arena_raw);
        const std::int32_t dx = rng_.next_in_range(-speed_raw, speed_raw);
        const std::int32_t dy = rng_.next_in_range(-speed_raw, speed_raw);
        const auto entity = registry_.create();
        registry_.emplace<Position>(entity, Fixed::from_raw(x), Fixed::from_raw(y));
        registry_.emplace<Velocity>(entity, Fixed::from_raw(dx), Fixed::from_raw(dy));
    }
}

void World::step() {
    registry_.view<Position, Velocity>().each([this](Position& pos, Velocity& vel) {
        pos.x += vel.dx;
        pos.y += vel.dy;
        reflect(pos.x, vel.dx, arena_size_);
        reflect(pos.y, vel.dy, arena_size_);
    });
    ++tick_;
}

std::uint64_t World::state_hash() const {
    StateHasher h;
    h.add_u32(tick_);
    for (const auto word : rng_.state()) {
        h.add_u64(word);
    }
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
    out.arena_size = arena_size_;
    out.entities.clear();
    const auto view = registry_.view<const Position>();
    out.entities.reserve(view.size());
    view.each([&out](const entt::entity e, const Position& pos) {
        out.entities.push_back({entt::to_integral(e), pos});
    });
}

}  // namespace rts::sim
