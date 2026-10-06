#include "sim/climb.hpp"

#include <algorithm>
#include <array>

#include "sim/state_hash.hpp"

namespace rts::sim {

namespace {

std::int32_t sign(std::int32_t v) noexcept {
    return (v > 0) - (v < 0);
}

}  // namespace

ClimbSystem::ClimbSystem(const ClimbParams& params, std::vector<UnitType> units, std::vector<BuildingType> buildings)
    : params_(params), units_(std::move(units)), buildings_(std::move(buildings)) {}

bool ClimbSystem::is_climbable(const entt::registry& registry, entt::entity b, PlayerId attacker) const {
    if (!registry.valid(b) || !registry.all_of<Building, Owner, Footprint>(b)) {
        return false;
    }
    return registry.get<Owner>(b).player != attacker && buildings_[registry.get<Building>(b).type].climbable;
}

void ClimbSystem::apply(entt::registry& registry, MovementSystem& movement, EconomySystem& economy,
                        const Command& command, std::span<const entt::entity> units, std::uint32_t& next_order_id,
                        Tick tick) {
    if (command.type == CommandType::SetStance) {
        return;
    }
    for (const entt::entity e : units) {
        registry.remove<Climbing>(e);  // otra orden: deja la escalada
    }
    const auto wall = static_cast<entt::entity>(command.object);
    if (command.type != CommandType::Climb || !is_climbable(registry, wall, command.player)) {
        return;
    }
    // Cada escalador lleva su escala, pagada al dar la orden; sin madera, no va.
    Stock& stock = economy.player_state(command.player).stock;
    const TileCoord wall_tile = registry.get<Footprint>(wall).origin;
    std::vector<entt::entity> climbers;
    for (const entt::entity e : units) {
        const Unit* u = registry.try_get<Unit>(e);
        if (u == nullptr || !units_[u->type].climbs || registry.any_of<Patient, Routing>(e)) {
            continue;
        }
        bool afford = true;
        for (std::size_t r = 0; r < kResourceCount; ++r) {
            afford = afford && stock[r] >= params_.ladder_cost[r];
        }
        if (!afford) {
            break;
        }
        for (std::size_t r = 0; r < kResourceCount; ++r) {
            stock[r] -= params_.ladder_cost[r];
        }
        registry.emplace<Climbing>(e, Climbing{wall, wall_tile, {}, -1, 0, 0});
        climbers.push_back(e);
    }
    if (climbers.empty()) {
        return;
    }
    const std::uint32_t order = next_order_id++;
    movement.order_move(registry, climbers, wall_tile, order, tick);
    for (const entt::entity e : climbers) {
        registry.get<Climbing>(e).move_order = order;
    }
}

void ClimbSystem::update(entt::registry& registry, MovementSystem& movement, const EconomySystem& economy,
                         std::uint32_t& next_order_id, Tick tick) {
    stats_ = ClimbTickStats{};
    std::vector<entt::entity> list;
    for (const entt::entity e : registry.view<Climbing>()) {
        list.push_back(e);
    }
    std::ranges::sort(list, {}, [](entt::entity e) { return entt::to_integral(e); });
    for (const entt::entity e : list) {
        Climbing& c = registry.get<Climbing>(e);
        const PlayerId owner = registry.get<Owner>(e).player;
        if (!is_climbable(registry, c.wall, owner)) {
            registry.remove<Climbing>(e);  // el muro cayó o cambió: ya no hace falta
            continue;
        }
        Position& p = registry.get<Position>(e);
        if (c.timer >= 0) {
            if (++c.timer < params_.climb_ticks) {
                continue;
            }
            // Arriba: baja al lado opuesto, si está libre y se puede pisar.
            const TileCoord land{c.wall_tile.x + sign(c.wall_tile.x - c.from.x),
                                 c.wall_tile.y + sign(c.wall_tile.y - c.from.y)};
            registry.remove<Climbing>(e);
            if (movement.can_enter(land, owner) && economy.occupant(land) == entt::null) {
                p.x = Fixed::from_int(land.x) + Fixed::from_ratio(1, 2);
                p.y = Fixed::from_int(land.y) + Fixed::from_ratio(1, 2);
                registry.emplace_or_replace<Velocity>(e);
                ++stats_.climbed;
            } else {
                ++stats_.failed;
            }
            continue;
        }
        const Fixed reach = registry.get<Unit>(e).radius + params_.reach;
        const Footprint f{c.wall_tile, 1};
        const bool near = distance_sq_to(f, FVec2{p.x, p.y}) <= mul_wide(reach, reach);
        const TileCoord at = tile_of(p);
        if (near && at != c.wall_tile) {
            // Al pie del muro: planta la escala y sube.
            c.from = at;
            c.timer = 0;
            registry.remove<MoveGoal, PathFollow, Waypoints>(e);
            if (Velocity* v = registry.try_get<Velocity>(e)) {
                v->v = {};
            }
            continue;
        }
        const MoveGoal* goal = registry.try_get<MoveGoal>(e);
        const bool stopped = goal == nullptr || goal->order_id != c.move_order || goal->arrived;
        if (stopped) {
            if (++c.approaches >= 2) {
                registry.remove<Climbing>(e);  // no llega al pie del muro: desiste
                continue;
            }
            c.move_order = next_order_id++;
            const std::array<entt::entity, 1> one{e};
            movement.order_move(registry, one, c.wall_tile, c.move_order, tick);
        }
    }
}

void ClimbSystem::hash_into(StateHasher& h, const entt::registry& registry) const {
    for (const auto [e, c] : registry.view<const Climbing>().each()) {
        h.add_u32(entt::to_integral(e));
        h.add_u32(entt::to_integral(c.wall));
        h.add_i32(c.from.x);
        h.add_i32(c.from.y);
        h.add_i32(c.timer);
        h.add_u32(c.move_order);
        h.add_i32(c.approaches);
    }
}

}  // namespace rts::sim
