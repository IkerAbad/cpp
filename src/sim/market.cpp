#include "sim/market.hpp"

#include <algorithm>
#include <array>
#include <limits>

#include "sim/state_hash.hpp"

namespace rts::sim {

namespace {

constexpr std::int64_t kMilli = 1000;
constexpr std::size_t kGold = resource_index(Resource::Gold);

}  // namespace

MarketSystem::MarketSystem(const MarketParams& params, std::vector<UnitType> units, std::vector<BuildingType> buildings)
    : params_(params), units_(std::move(units)), buildings_(std::move(buildings)), price_(params.base_price) {}

std::int32_t MarketSystem::sell_price(std::size_t r) const noexcept {
    return static_cast<std::int32_t>(std::int64_t{price_[r]} * (kPercent - params_.sell_fee_percent) / kPercent);
}

bool MarketSystem::is_market(const entt::registry& registry, entt::entity b, PlayerId player) const {
    if (!registry.valid(b) || !registry.all_of<Building, Owner, Footprint>(b)) {
        return false;
    }
    const Building& bl = registry.get<Building>(b);
    return registry.get<Owner>(b).player == player && bl.working() && buildings_[bl.type].market;
}

entt::entity MarketSystem::nearest_market(const entt::registry& registry, PlayerId player, FVec2 pos,
                                          entt::entity exclude) const {
    entt::entity best = entt::null;
    std::int64_t best_d = std::numeric_limits<std::int64_t>::max();
    for (const auto [e, f] : registry.view<const Footprint>().each()) {
        if (e == exclude || !is_market(registry, e, player)) {
            continue;
        }
        const std::int64_t d = distance_sq_to(f, pos);
        if (d < best_d || (d == best_d && entt::to_integral(e) < entt::to_integral(best))) {
            best_d = d;
            best = e;
        }
    }
    return best;
}

void MarketSystem::head_to(entt::registry& registry, MovementSystem& movement, entt::entity e, Caravan& c,
                           std::uint32_t& next_order_id, Tick tick) const {
    const entt::entity target = c.outbound ? c.to : c.from;
    c.move_order = next_order_id++;
    const std::array<entt::entity, 1> one{e};
    movement.order_move(registry, one, registry.get<Footprint>(target).origin, c.move_order, tick);
}

void MarketSystem::apply(entt::registry& registry, MovementSystem& movement, EconomySystem& economy,
                         const Command& command, std::span<const entt::entity> units, std::uint32_t& next_order_id,
                         Tick tick) {
    if (!params_.enabled || command.type == CommandType::SetStance) {
        return;
    }
    const auto object = static_cast<entt::entity>(command.object);
    if (command.type == CommandType::Trade) {
        const std::size_t r = command.kind & static_cast<std::uint8_t>(~kSellBit);
        if (r >= kResourceCount || r == kGold || params_.base_price[r] <= 0 ||
            !is_market(registry, object, command.player)) {
            return;
        }
        Stock& stock = economy.player_state(command.player).stock;
        used_ = true;
        if ((command.kind & kSellBit) != 0) {
            if (stock[r] < params_.lot) {
                return;
            }
            stock[r] -= params_.lot;
            stock[kGold] += sell_price(r);
            price_[r] = std::max(price_[r] - params_.price_step, params_.min_price);
        } else {
            if (stock[kGold] < price_[r]) {
                return;
            }
            stock[kGold] -= price_[r];
            stock[r] += params_.lot;
            price_[r] = std::min(price_[r] + params_.price_step, params_.max_price);
        }
        return;
    }
    // Cualquier otra orden al bagaje deja la ruta de caravana.
    for (const entt::entity e : units) {
        registry.remove<Caravan>(e);
    }
    if (command.type != CommandType::TradeRoute || !is_market(registry, object, command.player)) {
        return;
    }
    for (const entt::entity e : units) {
        const Unit* u = registry.try_get<Unit>(e);
        if (u == nullptr || units_[u->type].convoy_capacity <= 0) {
            continue;
        }
        const Position& p = registry.get<Position>(e);
        const entt::entity from = nearest_market(registry, command.player, {p.x, p.y}, object);
        if (from == entt::null) {
            continue;  // hace falta otro mercado propio del que salir
        }
        used_ = true;
        Caravan& c = registry.emplace_or_replace<Caravan>(e, Caravan{from, object, true, 0, 0});
        head_to(registry, movement, e, c, next_order_id, tick);
    }
}

void MarketSystem::update(entt::registry& registry, MovementSystem& movement, EconomySystem& economy,
                          std::uint32_t& next_order_id, Tick tick) {
    if (!params_.enabled) {
        return;
    }
    // Los precios vuelven poco a poco a su base.
    if (tick % static_cast<Tick>(params_.recover_interval_ticks) == 0) {
        for (std::size_t r = 0; r < kResourceCount; ++r) {
            price_[r] += price_[r] < params_.base_price[r] ? 1 : (price_[r] > params_.base_price[r] ? -1 : 0);
        }
    }
    std::vector<entt::entity> list;
    for (const entt::entity e : registry.view<Caravan>()) {
        list.push_back(e);
    }
    std::ranges::sort(list, {}, [](entt::entity e) { return entt::to_integral(e); });
    for (const entt::entity e : list) {
        Caravan& c = registry.get<Caravan>(e);
        const PlayerId owner = registry.get<Owner>(e).player;
        if (!is_market(registry, c.from, owner) || !is_market(registry, c.to, owner)) {
            registry.remove<Caravan>(e);  // un mercado cayó: se acabó la ruta
            continue;
        }
        const Position& p = registry.get<Position>(e);
        const entt::entity target = c.outbound ? c.to : c.from;
        const Fixed reach = registry.get<Unit>(e).radius + params_.caravan_reach;
        if (distance_sq_to(registry.get<Footprint>(target), FVec2{p.x, p.y}) <= mul_wide(reach, reach)) {
            // Llegó: cobra según la distancia entre los mercados y da la vuelta.
            const TileCoord a = center_of(registry.get<Footprint>(c.from));
            const TileCoord b = center_of(registry.get<Footprint>(c.to));
            const std::int64_t tiles = std::max(std::abs(a.x - b.x), std::abs(a.y - b.y));
            if (owner >= gold_milli_.size()) {
                gold_milli_.resize(owner + std::size_t{1}, 0);
                caravan_gold_.resize(owner + std::size_t{1}, 0);
            }
            gold_milli_[owner] += static_cast<std::int32_t>(tiles * params_.caravan_gold_milli_per_tile);
            const std::int32_t gold = static_cast<std::int32_t>(gold_milli_[owner] / kMilli);
            gold_milli_[owner] -= static_cast<std::int32_t>(gold * kMilli);
            economy.player_state(owner).stock[kGold] += gold;
            caravan_gold_[owner] += gold;
            c.outbound = !c.outbound;
            c.approaches = 0;
            head_to(registry, movement, e, c, next_order_id, tick);
            continue;
        }
        const MoveGoal* goal = registry.try_get<MoveGoal>(e);
        if (goal == nullptr || goal->order_id != c.move_order || goal->arrived) {
            if (++c.approaches >= 2) {
                registry.remove<Caravan>(e);  // no llega al mercado: desiste
                continue;
            }
            head_to(registry, movement, e, c, next_order_id, tick);
        }
    }
}

void MarketSystem::hash_into(StateHasher& h, const entt::registry& registry) const {
    if (!params_.enabled || !used_) {
        return;
    }
    for (const std::int32_t p : price_) {
        h.add_i32(p);
    }
    for (const std::int32_t g : gold_milli_) {
        h.add_i32(g);
    }
    for (const auto [e, c] : registry.view<const Caravan>().each()) {
        h.add_u32(entt::to_integral(e));
        h.add_u32(entt::to_integral(c.from));
        h.add_u32(entt::to_integral(c.to));
        h.add_u32(c.outbound ? 1U : 0U);
        h.add_u32(c.move_order);
        h.add_i32(c.approaches);
    }
}

}  // namespace rts::sim
