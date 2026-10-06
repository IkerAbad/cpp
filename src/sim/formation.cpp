#include "sim/formation.hpp"

#include <algorithm>
#include <array>

#include "sim/economy.hpp"
#include "sim/state_hash.hpp"

namespace rts::sim {

namespace {

bool is_formation_kind(std::uint8_t kind) noexcept {
    return kind >= kFormationBase && kind <= kFormationBase + static_cast<std::uint8_t>(FormationKind::Square);
}

std::int32_t ceil_div(std::int32_t a, std::int32_t b) noexcept {
    return (a + b - 1) / b;
}

}  // namespace

FormationSystem::FormationSystem(std::int32_t width, std::int32_t height, const FormationParams& params,
                                 std::vector<UnitType> units)
    : width_(width), height_(height), params_(params), units_(std::move(units)) {}

const FormationEffect& FormationSystem::effect(FormationKind k) const noexcept {
    switch (k) {
        case FormationKind::Line:
            return params_.line;
        case FormationKind::Column:
            return params_.column;
        case FormationKind::Square:
        case FormationKind::None:
            break;
    }
    return params_.square;
}

std::vector<TileCoord> FormationSystem::slots(FormationKind kind, std::size_t n, TileCoord target, FVec2 dir) const {
    const Fixed s = params_.spacing;
    FVec2 f = with_length(dir, Fixed::from_int(1));
    if (f.x.raw() == 0 && f.y.raw() == 0) {
        f = {Fixed{}, Fixed::from_int(-1)};  // sin dirección: mirando al norte
    }
    const FVec2 r{-f.y, f.x};
    const auto count = static_cast<std::int32_t>(n);
    std::int32_t width = 1;
    switch (kind) {
        case FormationKind::Line:
            width = count > 8 ? ceil_div(count, 2) : count;  // dos filas si son muchos
            break;
        case FormationKind::Column:
            width = std::min(count, 2);
            break;
        case FormationKind::Square:
        case FormationKind::None: {
            width = 1;
            while (width * width < count) {
                ++width;
            }
            break;
        }
    }
    const FVec2 center{Fixed::from_int(target.x) + Fixed::from_ratio(1, 2),
                       Fixed::from_int(target.y) + Fixed::from_ratio(1, 2)};
    std::vector<TileCoord> out;
    out.reserve(n);
    for (std::int32_t i = 0; i < count; ++i) {
        const std::int32_t col = i % width;
        const std::int32_t row = i / width;
        // Centrado a lo ancho; las filas, hacia atrás desde el destino.
        const Fixed across = s * Fixed::from_ratio(2 * col - (width - 1), 2);
        const Fixed back = s * row;
        const FVec2 p = center + r * across - f * back;
        const TileCoord t = tile_of(p);
        out.push_back({std::clamp(t.x, 0, width_ - 1), std::clamp(t.y, 0, height_ - 1)});
    }
    return out;
}

std::vector<entt::entity> FormationSystem::apply(entt::registry& registry, MovementSystem& movement,
                                                 const Command& command, std::span<const entt::entity> units,
                                                 std::uint32_t& next_order_id, Tick tick) {
    std::vector<entt::entity> handled;
    if (!params_.enabled) {
        return handled;
    }
    if (command.type == CommandType::SetStance && is_formation_kind(command.kind)) {
        const auto kind = static_cast<FormationKind>(command.kind - kFormationBase);
        for (const entt::entity e : units) {
            const Unit* u = registry.try_get<Unit>(e);
            if (u == nullptr || !registry.all_of<Combatant>(e) || units_[u->type].worker ||
                units_[u->type].convoy_capacity > 0) {
                continue;
            }
            registry.get_or_emplace<Formation>(e).kind = kind;
        }
        return handled;
    }
    if (command.type != CommandType::Move || command.kind == kQueueMove) {
        return handled;
    }
    // Las que van en la misma formación (la de la primera), cada una a su puesto.
    FormationKind kind = FormationKind::None;
    for (const entt::entity e : units) {
        if (const Formation* f = registry.try_get<Formation>(e); f != nullptr && f->kind != FormationKind::None) {
            kind = f->kind;
            break;
        }
    }
    if (kind == FormationKind::None) {
        return handled;
    }
    for (const entt::entity e : units) {
        const Formation* f = registry.try_get<Formation>(e);
        if (f != nullptr && f->kind == kind) {
            handled.push_back(e);
        }
    }
    if (handled.size() < 2) {
        handled.clear();
        return handled;
    }
    std::ranges::sort(handled, {}, [](entt::entity e) { return entt::to_integral(e); });
    std::int64_t sx = 0;
    std::int64_t sy = 0;
    for (const entt::entity e : handled) {
        const Position& p = registry.get<Position>(e);
        sx += p.x.raw();
        sy += p.y.raw();
    }
    const auto n = static_cast<std::int64_t>(handled.size());
    const FVec2 from{Fixed::from_raw(static_cast<std::int32_t>(sx / n)), Fixed::from_raw(static_cast<std::int32_t>(sy / n))};
    const FVec2 to{Fixed::from_int(command.target.x) + Fixed::from_ratio(1, 2),
                   Fixed::from_int(command.target.y) + Fixed::from_ratio(1, 2)};
    const std::vector<TileCoord> places = slots(kind, handled.size(), command.target, to - from);
    for (std::size_t i = 0; i < handled.size(); ++i) {
        const std::array<entt::entity, 1> one{handled[i]};
        movement.order_move(registry, one, places[i], next_order_id++, tick);
    }
    return handled;
}

void FormationSystem::update(entt::registry& registry) {
    if (!params_.enabled) {
        return;
    }
    struct Member {
        entt::entity e;
        FVec2 pos;
        PlayerId owner;
        FormationKind kind;
    };
    std::vector<Member> members;
    for (const auto [e, f, p, o] : registry.view<const Formation, const Position, const Owner>().each()) {
        if (f.kind != FormationKind::None) {
            members.push_back({e, {p.x, p.y}, o.player, f.kind});
        }
    }
    const std::int64_t r_sq = mul_wide(params_.cohesion_radius, params_.cohesion_radius);
    for (auto [e, f] : registry.view<Formation>().each()) {
        f.active = false;
        f.speed_percent = kPercent;
        f.attack_percent = kPercent;
        f.cavalry_taken_percent = kPercent;
        f.ranged_taken_percent = kPercent;
        f.stops_charge = false;
        if (f.kind == FormationKind::None) {
            continue;
        }
        const Position& p = registry.get<Position>(e);
        const PlayerId owner = registry.get<Owner>(e).player;
        std::int32_t near = 0;
        for (const Member& m : members) {
            near += m.owner == owner && m.kind == f.kind && length_sq_wide(m.pos - FVec2{p.x, p.y}) <= r_sq ? 1 : 0;
        }
        if (near < params_.min_members) {
            continue;  // pocos para formar: como si nada
        }
        const FormationEffect& fx = effect(f.kind);
        const bool ranged = units_[registry.get<Unit>(e).type].combat.projectile_speed.raw() != 0;
        f.active = true;
        f.speed_percent = fx.speed_percent;
        f.attack_percent = ranged ? fx.ranged_attack_percent : fx.melee_attack_percent;
        f.cavalry_taken_percent = fx.cavalry_taken_percent;
        f.ranged_taken_percent = fx.ranged_taken_percent;
        f.stops_charge = fx.stops_charge;
    }
}

void FormationSystem::hash_into(StateHasher& h, const entt::registry& registry) const {
    if (!params_.enabled) {
        return;
    }
    for (const auto [e, f] : registry.view<const Formation>().each()) {
        h.add_u32(entt::to_integral(e));
        h.add_u32(static_cast<std::uint32_t>(f.kind));
    }
}

}  // namespace rts::sim
