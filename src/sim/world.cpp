#include "sim/world.hpp"

#include <algorithm>
#include <cassert>
#include <memory>
#include <utility>

#include "sim/state_hash.hpp"

namespace rts::sim {

namespace {

// Intentos por unidad para caer en una casilla transitable libre del área.
constexpr std::int32_t kSpawnAttempts = 64;

std::shared_ptr<TileMap> make_map(const WorldParams& params) {
    return std::make_shared<TileMap>(generate_map(params.map));
}

}  // namespace

World::World(const WorldParams& params)
    : map_(make_map(params)),
      unit_types_(params.unit_types),
      movement_(*map_, params.passable_by_terrain, params.movement),
      rng_(params.demo.seed) {
    spawn_demo_units(params);
}

void World::spawn_demo_units(const WorldParams& params) {
    const DemoParams& demo = params.demo;
    assert(demo.count >= 0);
    assert(demo.area_tiles > 0);
    assert(demo.unit_type < unit_types_.size());
    const UnitType& type = unit_types_[demo.unit_type];

    const std::int32_t side = std::min({demo.area_tiles, map_->width(), map_->height()});
    const std::int32_t lo = (map_->width() - side) / 2;
    // Posiciones al azar dentro del área, solo en casillas transitables y sin repetir
    // casilla: así ninguna unidad nace solapada ni encima de agua.
    std::vector<std::uint8_t> taken(static_cast<std::size_t>(map_->tile_count()), 0);
    const PassGrid& grid = movement_.grid();
    for (std::int32_t i = 0; i < demo.count; ++i) {
        for (std::int32_t attempt = 0; attempt < kSpawnAttempts; ++attempt) {
            // Cada extracción del RNG en su propia sentencia: el orden de evaluación de
            // los argumentos de una llamada no está especificado en C++.
            const std::int32_t tx = lo + static_cast<std::int32_t>(rng_.next_below(static_cast<std::uint32_t>(side)));
            const std::int32_t ty = lo + static_cast<std::int32_t>(rng_.next_below(static_cast<std::uint32_t>(side)));
            const TileCoord t{tx, ty};
            if (!grid.passable(t) || taken[grid.index(t)] != 0) {
                continue;
            }
            taken[grid.index(t)] = 1;
            const auto e = registry_.create();
            registry_.emplace<Position>(e, Fixed::from_int(tx) + Fixed::from_ratio(1, 2),
                                        Fixed::from_int(ty) + Fixed::from_ratio(1, 2));
            registry_.emplace<Velocity>(e);
            registry_.emplace<Unit>(e, demo.unit_type, type.radius, type.speed);
            break;
        }
    }
}

void World::issue(Command command) {
    pending_.push_back(std::move(command));
}

void World::step() {
    movement_.begin_tick();
    // Órdenes de este tick (o atrasadas), en orden de llegada. stable_partition conserva
    // el orden relativo de las que se quedan para ticks futuros.
    const auto rest = std::ranges::stable_partition(pending_, [this](const Command& c) { return c.tick <= tick_; });
    for (auto it = pending_.begin(); it != rest.begin(); ++it) {
        movement_.apply(registry_, *it, next_order_id_++, tick_);
    }
    pending_.erase(pending_.begin(), rest.begin());

    movement_.update(registry_, tick_);
    ++tick_;
}

std::uint64_t World::state_hash() const {
    StateHasher h;
    h.add_u32(tick_);
    for (const auto word : rng_.state()) {
        h.add_u64(word);
    }
    map_->hash_into(h);
    h.add_u32(next_order_id_);
    h.add_u64(pending_.size());
    movement_.hash_into(h);
    // El orden de iteración de una vista de EnTT es el del array denso del pool,
    // que depende solo de la secuencia de create/emplace/destroy: determinista.
    const auto units = registry_.view<const Position, const Velocity, const Unit>();
    for (const entt::entity e : units) {
        const auto& [p, v, u] = units.get<const Position, const Velocity, const Unit>(e);
        h.add_u32(entt::to_integral(e));
        h.add_fixed(p.x);
        h.add_fixed(p.y);
        h.add_fixed(v.v.x);
        h.add_fixed(v.v.y);
        h.add_u32(u.type);
        if (const MoveGoal* g = registry_.try_get<MoveGoal>(e)) {
            h.add_i32(g->tile.x);
            h.add_i32(g->tile.y);
            h.add_u32(g->order_id);
            h.add_i32(g->flow_field);
            h.add_i32(g->stuck_ticks);
            h.add_i32(g->group_size);
            h.add_u32(g->arrived ? 1U : 0U);
        }
        if (const PathFollow* f = registry_.try_get<PathFollow>(e)) {
            h.add_u64(f->waypoints.size());
            h.add_u32(f->next_waypoint);
            h.add_u64(f->segment.size());
            h.add_u32(f->next_tile);
            h.add_u32(f->waiting ? 1U : 0U);
        }
    }
    return h.value();
}

void World::write_snapshot(Snapshot& out) const {
    out.tick = tick_;
    out.map = map_;
    out.entities.clear();
    const auto view = registry_.view<const Position, const Unit>();
    view.each([&out](const entt::entity e, const Position& pos, const Unit& unit) {
        out.entities.push_back({entt::to_integral(e), pos, unit.type});
    });
}

}  // namespace rts::sim
