#include "sim/garrison.hpp"

#include <algorithm>
#include <array>

#include "sim/state_hash.hpp"

namespace rts::sim {

GarrisonSystem::GarrisonSystem(const GarrisonParams& params, std::vector<UnitType> units,
                               std::vector<BuildingType> buildings)
    : params_(params), units_(std::move(units)), buildings_(std::move(buildings)) {}

bool GarrisonSystem::can_garrison(const entt::registry& registry, entt::entity e) const {
    const Unit* u = registry.try_get<Unit>(e);
    if (u == nullptr || !registry.all_of<Combatant>(e) || registry.any_of<Patient, Routing>(e)) {
        return false;
    }
    const UnitType& t = units_[u->type];
    return !t.worker && t.convoy_capacity == 0 && !t.combat.buildings_only && t.combat.auto_attack &&
           u->speed.raw() > 0;
}

bool GarrisonSystem::is_tower(const entt::registry& registry, entt::entity b, PlayerId player) const {
    if (!registry.valid(b) || !registry.all_of<Building, Owner, Footprint>(b)) {
        return false;
    }
    const Building& bl = registry.get<Building>(b);
    return registry.get<Owner>(b).player == player && bl.complete && buildings_[bl.type].garrison > 0;
}

std::int32_t GarrisonSystem::tower_levels(const entt::registry& registry, entt::entity e) const {
    const Garrisoned* g = registry.try_get<Garrisoned>(e);
    if (g == nullptr || !g->inside || !registry.valid(g->tower) || !registry.all_of<Building>(g->tower)) {
        return 0;
    }
    return buildings_[registry.get<Building>(g->tower).type].garrison_levels;
}

std::int32_t GarrisonSystem::inside_count(const entt::registry& registry, entt::entity tower) const {
    std::int32_t n = 0;
    for (const auto [e, g] : registry.view<const Garrisoned>().each()) {
        n += g.inside && g.tower == tower ? 1 : 0;
    }
    return n;
}

void GarrisonSystem::enter(entt::registry& registry, entt::entity e, Garrisoned& g) const {
    g.inside = true;
    g.move_order = 0;
    registry.remove<MoveGoal, PathFollow, Velocity, Waypoints>(e);
    const Footprint& f = g.tower_footprint;
    const FVec2 c{Fixed::from_int(f.origin.x) + Fixed::from_ratio(f.size, 2),
                  Fixed::from_int(f.origin.y) + Fixed::from_ratio(f.size, 2)};
    Position& p = registry.get<Position>(e);
    p.x = c.x;
    p.y = c.y;
}

void GarrisonSystem::leave(entt::registry& registry, const MovementSystem& movement, const EconomySystem& economy,
                           entt::entity e) const {
    const Garrisoned g = registry.get<Garrisoned>(e);
    registry.remove<Garrisoned>(e);
    if (!g.inside) {
        return;
    }
    // Sale a una casilla libre junto a la torre (o se queda donde está si no la hay).
    if (const auto t = economy.spawn_tile(movement.grid(), g.tower_footprint, entt::to_integral(e))) {
        Position& p = registry.get<Position>(e);
        p.x = Fixed::from_int(t->x) + Fixed::from_ratio(1, 2);
        p.y = Fixed::from_int(t->y) + Fixed::from_ratio(1, 2);
    }
    registry.emplace_or_replace<Velocity>(e);
}

void GarrisonSystem::apply(entt::registry& registry, MovementSystem& movement, const EconomySystem& economy,
                           const Command& command, std::span<const entt::entity> units,
                           std::uint32_t& next_order_id, Tick tick) {
    const auto object = static_cast<entt::entity>(command.object);
    if (command.type == CommandType::Garrison && command.kind == kUngarrison) {
        if (!is_tower(registry, object, command.player)) {
            return;
        }
        std::vector<entt::entity> out;
        for (const auto [e, g] : registry.view<const Garrisoned>().each()) {
            if (g.tower == object && g.inside) {
                out.push_back(e);
            }
        }
        std::ranges::sort(out, {}, [](entt::entity e) { return entt::to_integral(e); });
        for (const entt::entity e : out) {
            leave(registry, movement, economy, e);
        }
        return;
    }
    if (command.type == CommandType::SetStance) {
        return;  // cambiar de postura o de paso no saca a nadie
    }
    // Cualquier orden saca de la torre (o anula el camino hacia ella)...
    for (const entt::entity e : units) {
        if (registry.all_of<Garrisoned>(e)) {
            leave(registry, movement, economy, e);
        }
    }
    if (command.type != CommandType::Garrison || !is_tower(registry, object, command.player)) {
        return;
    }
    // ...y la de guarnecer lleva a la torre.
    const Footprint& f = registry.get<Footprint>(object);
    std::vector<entt::entity> walkers;
    for (const entt::entity e : units) {
        if (can_garrison(registry, e)) {
            registry.emplace<Garrisoned>(e, Garrisoned{object, f, false, 0, 0});
            walkers.push_back(e);
        }
    }
    if (walkers.empty()) {
        return;
    }
    const std::uint32_t order = next_order_id++;
    movement.order_move(registry, walkers, f.origin, order, tick);
    for (const entt::entity e : walkers) {
        registry.get<Garrisoned>(e).move_order = order;
    }
}

void GarrisonSystem::update(entt::registry& registry, MovementSystem& movement, const EconomySystem& economy,
                            std::uint32_t& next_order_id, Tick tick) {
    // Torres que ya no lo son (derribadas, quemadas del todo): fuera todos.
    std::vector<entt::entity> out;
    for (const auto [e, g] : registry.view<const Garrisoned>().each()) {
        const PlayerId owner = registry.get<Owner>(e).player;
        if (!is_tower(registry, g.tower, owner)) {
            out.push_back(e);
        }
    }
    std::ranges::sort(out, {}, [](entt::entity e) { return entt::to_integral(e); });
    for (const entt::entity e : out) {
        leave(registry, movement, economy, e);
    }
    // Llegadas: entran por orden de entidad mientras haya plaza.
    std::vector<entt::entity> coming;
    for (const auto [e, g] : registry.view<const Garrisoned>().each()) {
        if (!g.inside) {
            coming.push_back(e);
        }
    }
    std::ranges::sort(coming, {}, [](entt::entity e) { return entt::to_integral(e); });
    for (const entt::entity e : coming) {
        Garrisoned& g = registry.get<Garrisoned>(e);
        const Position& p = registry.get<Position>(e);
        const Fixed reach = registry.get<Unit>(e).radius + params_.enter_reach;
        const bool near = distance_sq_to(g.tower_footprint, FVec2{p.x, p.y}) <= mul_wide(reach, reach);
        const std::int32_t capacity = buildings_[registry.get<Building>(g.tower).type].garrison;
        if (near && inside_count(registry, g.tower) < capacity) {
            enter(registry, e, g);
            continue;
        }
        const MoveGoal* goal = registry.try_get<MoveGoal>(e);
        const bool stopped = goal == nullptr || goal->order_id != g.move_order || goal->arrived;
        if (near && !stopped) {
            continue;  // llegando, o esperando plaza a la puerta
        }
        if (stopped && !near) {
            if (++g.approaches >= 2) {
                registry.remove<Garrisoned>(e);  // no llega: desiste
                continue;
            }
            g.move_order = next_order_id++;
            const std::array<entt::entity, 1> one{e};
            movement.order_move(registry, one, g.tower_footprint.origin, g.move_order, tick);
        }
    }
}

void GarrisonSystem::hash_into(StateHasher& h, const entt::registry& registry) const {
    for (const auto [e, g] : registry.view<const Garrisoned>().each()) {
        h.add_u32(entt::to_integral(e));
        h.add_u32(entt::to_integral(g.tower));
        h.add_u32(g.inside ? 1U : 0U);
        h.add_u32(g.move_order);
        h.add_i32(g.approaches);
    }
}

}  // namespace rts::sim
