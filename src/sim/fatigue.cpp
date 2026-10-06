#include "sim/fatigue.hpp"

#include <algorithm>

#include "sim/state_hash.hpp"

namespace rts::sim {

FatigueSystem::FatigueSystem(const FatigueParams& params, std::vector<UnitType> units)
    : params_(params), units_(std::move(units)) {}

std::int32_t FatigueSystem::scaled(UnitTypeId type, std::int64_t amount) const noexcept {
    const std::int64_t stamina = std::max(units_[type].stamina, 1);
    return static_cast<std::int32_t>(std::min<std::int64_t>(amount * kPercent / stamina, kFullFatigue));
}

void FatigueSystem::refresh(Fatigue& f) const noexcept {
    // Lineal: fresca, 100 %; agotada, exhausted_*_percent.
    const auto at = [&](std::int32_t exhausted) {
        return static_cast<std::int32_t>(kPercent - std::int64_t{kPercent - exhausted} * f.value / kFullFatigue);
    };
    f.speed_percent = at(params_.exhausted_speed_percent);
    f.attack_percent = at(params_.exhausted_attack_percent);
    if (f.forced) {
        f.speed_percent = static_cast<std::int32_t>(std::int64_t{f.speed_percent} * params_.forced_speed_percent / kPercent);
    }
}

void FatigueSystem::apply(entt::registry& registry, const Command& command,
                          std::span<const entt::entity> units) const {
    if (!params_.enabled || command.type != CommandType::SetStance ||
        (command.kind != kPaceNormal && command.kind != kPaceForced)) {
        return;
    }
    for (const entt::entity e : units) {
        const Unit* u = registry.try_get<Unit>(e);
        if (u == nullptr || units_[u->type].stamina <= 0) {
            continue;
        }
        Fatigue& f = registry.get_or_emplace<Fatigue>(e);
        f.forced = command.kind == kPaceForced;
        refresh(f);
    }
}

void FatigueSystem::update(entt::registry& registry, const MovementSystem& movement,
                           std::span<const MoraleHit> hits) const {
    if (!params_.enabled) {
        return;
    }
    // Las unidades nuevas que se cansan llegan frescas.
    std::vector<entt::entity> fresh;
    for (const auto [e, u] : registry.view<const Unit>(entt::exclude<Fatigue>).each()) {
        if (units_[u.type].stamina > 0) {
            fresh.push_back(e);
        }
    }
    for (const entt::entity e : fresh) {
        registry.emplace<Fatigue>(e);
    }
    for (const MoraleHit& h : hits) {
        if (registry.valid(h.attacker)) {
            if (Fatigue* f = registry.try_get<Fatigue>(h.attacker)) {
                const UnitTypeId type = registry.get<Unit>(h.attacker).type;
                f->value = std::min(f->value + scaled(type, params_.strike), kFullFatigue);
            }
        }
    }
    for (const auto [e, f, u, p, v] : registry.view<Fatigue, const Unit, const Position, const Velocity>().each()) {
        if (v.v.x.raw() != 0 || v.v.y.raw() != 0) {
            // Marchando: el terreno difícil cansa en proporción a lo que frena.
            std::int64_t gain = params_.march_per_tick * std::int64_t{kPercent} /
                                std::max(movement.terrain_speed_percent(FVec2{p.x, p.y}), 1);
            if (f.forced) {
                gain = gain * params_.forced_fatigue_percent / kPercent;
            }
            f.value = std::min(f.value + scaled(u.type, gain), kFullFatigue);
        } else {
            f.value = std::max(f.value - params_.rest_per_tick, 0);
        }
        refresh(f);
    }
}

void FatigueSystem::hash_into(StateHasher& h, const entt::registry& registry) const {
    if (!params_.enabled) {
        return;
    }
    for (const auto [e, f] : registry.view<const Fatigue>().each()) {
        h.add_u32(entt::to_integral(e));
        h.add_i32(f.value);
        h.add_u32(f.forced ? 1U : 0U);
    }
}

}  // namespace rts::sim
