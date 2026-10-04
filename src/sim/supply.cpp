#include "sim/supply.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <utility>

#include "sim/state_hash.hpp"

namespace rts::sim {

namespace {

bool affordable(const Stock& stock, const Stock& cost) noexcept {
    for (std::size_t i = 0; i < kResourceCount; ++i) {
        if (stock[i] < cost[i]) {
            return false;
        }
    }
    return true;
}

void pay(Stock& stock, const Stock& cost) noexcept {
    for (std::size_t i = 0; i < kResourceCount; ++i) {
        stock[i] -= cost[i];
    }
}

}  // namespace

SupplySystem::SupplySystem(const SupplyParams& params, std::vector<UnitType> units,
                           std::vector<BuildingType> buildings)
    : params_(params), units_(std::move(units)), buildings_(std::move(buildings)) {
    assert(params_.resupply_interval_ticks > 0);
    assert(params_.starve_hp_interval_ticks > 0);
}

entt::entity SupplySystem::source_near(const entt::registry& registry, const EconomySystem& economy,
                                       PlayerId player, FVec2 pos, bool worker) const {
    const std::int32_t r = params_.resupply_radius_tiles;
    const Fixed reach = Fixed::from_int(r);
    const std::int64_t reach_sq = mul_wide(reach, reach);
    const TileCoord t = tile_of(pos);
    // Recorrido fijo por filas: a igualdad, el primero encontrado.
    for (std::int32_t y = t.y - r; y <= t.y + r; ++y) {
        for (std::int32_t x = t.x - r; x <= t.x + r; ++x) {
            const entt::entity o = economy.occupant({x, y});
            if (o == entt::null || !registry.all_of<Building, Owner, Footprint>(o)) {
                continue;
            }
            const Building& b = registry.get<Building>(o);
            const BuildingType& bt = buildings_[b.type];
            if (registry.get<Owner>(o).player != player || !b.working() ||
                !(bt.supplies || (worker && bt.accepts != 0))) {
                continue;
            }
            if (distance_sq_to(registry.get<Footprint>(o), pos) <= reach_sq) {
                return o;
            }
        }
    }
    return entt::null;
}

void SupplySystem::update(entt::registry& registry, EconomySystem& economy, Tick tick) {
    stats_ = SupplyTickStats{};
    dead_.clear();
    const auto interval = static_cast<std::uint32_t>(params_.resupply_interval_ticks);
    const auto view = registry.view<Supply, const Unit, const Owner, const Position, Health>();
    for (const entt::entity e : view) {
        Supply& s = view.get<Supply>(e);
        const SupplyStats& st = units_[view.get<const Unit>(e).type].supply;

        // 1. Consumo de víveres y hambre.
        if (st.rations > 0) {
            if (s.rations > 0 && ++s.ration_timer >= st.ration_ticks) {
                s.ration_timer = 0;
                --s.rations;
            }
            if (s.hungry()) {
                ++stats_.hungry;
                s.hungry_ticks = std::min(s.hungry_ticks + 1, std::numeric_limits<std::int32_t>::max() - 1);
                const std::int32_t over = s.hungry_ticks - params_.starve_after_ticks;
                if (st.starves && over > 0 && over % params_.starve_hp_interval_ticks == 0) {
                    Health& hp = view.get<Health>(e);
                    hp.hp = std::max(hp.hp - 1, 0);
                    if (hp.hp == 0) {
                        dead_.push_back(e);
                        continue;
                    }
                }
            } else {
                s.hungry_ticks = 0;
            }
        }

        // 2. Reabastecimiento junto a un edificio propio que abastece.
        const bool need_rations = s.rations < st.rations;
        const bool need_ammo = s.ammo < st.ammo;
        if ((!need_rations && !need_ammo) || (entt::to_integral(e) + tick) % interval != 0) {
            continue;
        }
        const PlayerId player = view.get<const Owner>(e).player;
        const Position& p = view.get<const Position>(e);
        const bool worker = units_[view.get<const Unit>(e).type].worker;
        if (source_near(registry, economy, player, {p.x, p.y}, worker) == entt::null) {
            continue;
        }
        Stock& stock = economy.player_state(player).stock;
        if (need_rations && affordable(stock, params_.ration_cost)) {
            pay(stock, params_.ration_cost);
            ++s.rations;
            ++stats_.rations_issued;
        }
        if (need_ammo && affordable(stock, st.ammo_cost)) {
            pay(stock, st.ammo_cost);
            s.ammo = std::min(s.ammo + st.ammo_bundle, st.ammo);
            ++stats_.ammo_issued;
        }
    }
    for (const entt::entity e : dead_) {
        registry.destroy(e);
        ++stats_.starved;
    }
}

void SupplySystem::hash_into(StateHasher& h, const entt::registry& registry) const {
    for (const auto [e, s] : registry.view<const Supply>().each()) {
        h.add_u32(entt::to_integral(e));
        h.add_i32(s.rations);
        h.add_i32(s.ration_timer);
        h.add_i32(s.hungry_ticks);
        h.add_i32(s.ammo);
    }
}

}  // namespace rts::sim
