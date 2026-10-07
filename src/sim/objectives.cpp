#include "sim/objectives.hpp"

#include <algorithm>

#include <entt/entity/registry.hpp>

#include "sim/state_hash.hpp"

namespace rts::sim {

ObjectiveSystem::ObjectiveSystem(const ObjectiveParams& params, std::vector<Objective> objectives,
                                 std::vector<UnitType> unit_types, std::size_t player_count)
    : params_(params),
      objectives_(std::move(objectives)),
      status_(objectives_.size(), ObjectiveStatus::Pending),
      unit_types_(std::move(unit_types)),
      player_count_(player_count) {
    // Objetivos de jugadores que no existen: no cuentan.
    std::erase_if(objectives_, [&](const Objective& o) { return o.player >= player_count_; });
    status_.resize(objectives_.size());
}

std::int32_t ObjectiveSystem::count_matching(const entt::registry& registry, const Objective& o, PlayerId owner) const {
    std::int32_t n = 0;
    if (o.units) {
        const auto view = registry.view<const Position, const Unit, const Owner>();
        for (const entt::entity e : view) {
            const auto& [pos, unit, own] = view.get<const Position, const Unit, const Owner>(e);
            if (own.player != owner || !o.in_zone(tile_of(pos))) {
                continue;
            }
            // Cualquier tipo: en Reach, solo la tropa (lo que tiene moral), no aldeanos ni bagaje.
            const bool match = o.type == kAnyObjectType
                                   ? (o.kind != ObjectiveKind::Reach || unit_types_[unit.type].morale_resolve > 0)
                                   : unit.type == o.type;
            n += match ? 1 : 0;
        }
        return n;
    }
    const auto view = registry.view<const Building, const Footprint, const Owner>();
    for (const entt::entity e : view) {
        const auto& [b, f, own] = view.get<const Building, const Footprint, const Owner>(e);
        // Un edificio de piedra quemado ya no sirve: cuenta como destruido.
        if (own.player != owner || b.burned || (o.type != kAnyObjectType && b.type != o.type)) {
            continue;
        }
        bool inside = !o.zoned;
        for (std::int32_t y = 0; y < f.size && !inside; ++y) {
            for (std::int32_t x = 0; x < f.size && !inside; ++x) {
                inside = o.in_zone({f.origin.x + x, f.origin.y + y});
            }
        }
        n += inside ? 1 : 0;
    }
    return n;
}

void ObjectiveSystem::update(const entt::registry& registry, const EconomySystem& economy, Tick tick) {
    if (objectives_.empty() || winner_ || params_.check_every_ticks <= 0 ||
        tick % static_cast<Tick>(params_.check_every_ticks) != 0) {
        return;
    }
    for (std::size_t i = 0; i < objectives_.size(); ++i) {
        const Objective& o = objectives_[i];
        ObjectiveStatus& s = status_[i];
        if (s != ObjectiveStatus::Pending) {
            continue;  // cumplido o fallado: es definitivo
        }
        switch (o.kind) {
            case ObjectiveKind::Destroy:
                s = count_matching(registry, o, o.target) == 0 ? ObjectiveStatus::Done : s;
                break;
            case ObjectiveKind::Keep:
                s = count_matching(registry, o, o.target) == 0 ? ObjectiveStatus::Failed : s;
                break;
            case ObjectiveKind::Survive:
                s = tick >= o.ticks ? ObjectiveStatus::Done : s;
                break;
            case ObjectiveKind::Reach:
                s = count_matching(registry, o, o.player) >= o.count ? ObjectiveStatus::Done : s;
                break;
            case ObjectiveKind::Gather:
                s = economy.players()[o.player].stock[resource_index(o.resource)] >= o.count ? ObjectiveStatus::Done
                                                                                              : s;
                break;
            case ObjectiveKind::Defeat:
                s = o.target < economy.players().size() && economy.players()[o.target].defeated ? ObjectiveStatus::Done
                                                                                                 : s;
                break;
        }
    }
    // El primero (por número de jugador) que lo tiene todo cumplido gana.
    for (std::size_t p = 0; p < player_count_ && !winner_; ++p) {
        bool any = false;
        bool all = true;
        for (std::size_t i = 0; i < objectives_.size(); ++i) {
            if (objectives_[i].player != p) {
                continue;
            }
            if (objectives_[i].kind == ObjectiveKind::Keep) {
                all = all && status_[i] != ObjectiveStatus::Failed;
            } else {
                any = true;
                all = all && status_[i] == ObjectiveStatus::Done;
            }
        }
        if (any && all && !lost(static_cast<PlayerId>(p)) && !economy.players()[p].defeated) {
            winner_ = static_cast<PlayerId>(p);
        }
    }
}

bool ObjectiveSystem::lost(PlayerId p) const noexcept {
    if (winner_ && *winner_ != p) {
        return true;
    }
    for (std::size_t i = 0; i < objectives_.size(); ++i) {
        if (objectives_[i].player == p && status_[i] == ObjectiveStatus::Failed) {
            return true;
        }
    }
    return false;
}

void ObjectiveSystem::hash_into(StateHasher& h) const {
    if (objectives_.empty()) {
        return;  // partidas sin objetivos: el hash de siempre
    }
    h.add_u64(objectives_.size());
    for (const ObjectiveStatus s : status_) {
        h.add_u32(static_cast<std::uint32_t>(s));
    }
    h.add_i32(winner_ ? static_cast<std::int32_t>(*winner_) : -1);
}

}  // namespace rts::sim
