#include "sim/morale.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <limits>

#include "sim/state_hash.hpp"

namespace rts::sim {

namespace {

bool within(FVec2 a, FVec2 b, Fixed radius) noexcept {
    return length_sq_wide(a - b) <= mul_wide(radius, radius);
}

}  // namespace

MoraleSystem::MoraleSystem(std::int32_t width, std::int32_t height, const MoraleParams& params,
                           std::vector<UnitType> units, Fixed hero_aura_radius)
    : width_(width),
      height_(height),
      params_(params),
      units_(std::move(units)),
      hero_aura_radius_(hero_aura_radius) {
    assert(params_.interval_ticks > 0);
    assert(params_.flee_repath_ticks > 0);
}

bool MoraleSystem::has_morale(const entt::registry& registry, entt::entity e) const {
    const Unit* u = registry.try_get<Unit>(e);
    return u != nullptr && units_[u->type].morale_resolve > 0 && registry.all_of<Combatant>(e);
}

void MoraleSystem::gather(const entt::registry& registry) {
    s_.entity.clear();
    s_.pos.clear();
    s_.owner.clear();
    s_.armed.clear();
    s_.routing.clear();
    s_.hero.clear();
    std::fill(index_of_.begin(), index_of_.end(), -1);
    const auto view = registry.view<const Position, const Unit, const Owner, const Health>();
    for (const entt::entity e : view) {
        const Position& p = view.get<const Position>(e);
        const UnitType& ut = units_[view.get<const Unit>(e).type];
        const auto id = static_cast<std::size_t>(entt::to_entity(e));
        if (id >= index_of_.size()) {
            index_of_.resize(id + 1, -1);
        }
        index_of_[id] = static_cast<std::int32_t>(s_.entity.size());
        s_.entity.push_back(e);
        s_.pos.push_back({p.x, p.y});
        s_.owner.push_back(view.get<const Owner>(e).player);
        // Tropa: puede herir a otra tropa. Un paciente o un ingresado no cuenta.
        const bool armed = ut.morale_resolve > 0 && ut.combat.auto_attack && !ut.combat.buildings_only &&
                           !registry.all_of<Patient>(e);
        s_.armed.push_back(armed ? 1 : 0);
        s_.routing.push_back(registry.all_of<Routing>(e) ? 1 : 0);
        const Combatant* c = registry.try_get<Combatant>(e);
        s_.hero.push_back(c != nullptr && c->hero_name >= 0 ? 1 : 0);
    }
    const std::size_t n = s_.entity.size();
    const auto cells = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
    s_.cell_start.assign(cells + 1, 0);
    s_.cell_units.resize(n);
    std::vector<std::uint32_t> cell(n);
    for (std::size_t i = 0; i < n; ++i) {
        cell[i] = static_cast<std::uint32_t>(cell_of(s_.pos[i]));
        ++s_.cell_start[cell[i] + 1];
    }
    for (std::size_t c = 0; c < cells; ++c) {
        s_.cell_start[c + 1] += s_.cell_start[c];
    }
    std::vector<std::uint32_t> fill(s_.cell_start.begin(), s_.cell_start.end() - 1);
    for (std::size_t i = 0; i < n; ++i) {
        s_.cell_units[fill[cell[i]]++] = static_cast<std::uint32_t>(i);
    }
}

std::size_t MoraleSystem::cell_of(FVec2 p) const noexcept {
    const TileCoord t = tile_of(p);
    const std::int32_t x = std::clamp(t.x, 0, width_ - 1);
    const std::int32_t y = std::clamp(t.y, 0, height_ - 1);
    return static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x);
}

template <typename Fn>
void MoraleSystem::for_each_near(FVec2 center, Fixed radius, Fn&& fn) const {
    const TileCoord t = tile_of(center);
    const std::int32_t reach = radius.floor_to_int() + 1;
    for (std::int32_t cy = std::max(t.y - reach, 0); cy <= std::min(t.y + reach, height_ - 1); ++cy) {
        for (std::int32_t cx = std::max(t.x - reach, 0); cx <= std::min(t.x + reach, width_ - 1); ++cx) {
            const std::size_t c =
                static_cast<std::size_t>(cy) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(cx);
            for (std::uint32_t u = s_.cell_start[c]; u < s_.cell_start[c + 1]; ++u) {
                const auto j = static_cast<std::size_t>(s_.cell_units[u]);
                if (within(s_.pos[j], center, radius)) {
                    fn(j);
                }
            }
        }
    }
}

std::int32_t MoraleSystem::scaled_loss(const entt::registry& registry, entt::entity e, std::int32_t loss,
                                       bool hero_near) const {
    const std::int64_t resolve = units_[registry.get<Unit>(e).type].morale_resolve;
    std::int64_t v = std::int64_t{loss} * kPercent / std::max<std::int64_t>(resolve, 1);
    if (daylight_ < kPercent) {
        v = v * params_.night_loss_percent / kPercent;
    }
    if (hero_near) {
        v = v * params_.hero_loss_percent / kPercent;
    }
    return static_cast<std::int32_t>(std::min<std::int64_t>(v, kFullMorale));
}

void MoraleSystem::lose(entt::registry& registry, entt::entity e, std::int32_t loss, bool hero_near) {
    if (Morale* m = registry.try_get<Morale>(e)) {
        m->value = std::max(m->value - scaled_loss(registry, e, loss, hero_near), 0);
    }
}

void MoraleSystem::apply_events(entt::registry& registry, std::span<const MoraleHit> hits,
                                std::span<const MoraleDeath> deaths) {
    // Golpes: de frente (quien le golpea es su blanco o está delante, hacia su blanco)
    // o de flanco y retaguardia (le pillan sin blanco o por detrás).
    for (const MoraleHit& h : hits) {
        if (!registry.valid(h.target) || !registry.all_of<Morale>(h.target)) {
            continue;
        }
        bool flank = true;
        const Combatant& c = registry.get<Combatant>(h.target);
        if (c.target != entt::null && c.target == h.attacker) {
            flank = false;
        } else if (c.target != entt::null && registry.valid(c.target) && registry.valid(h.attacker) &&
                   registry.all_of<Position>(c.target) && registry.all_of<Position>(h.attacker)) {
            const Position& me = registry.get<Position>(h.target);
            const Position& foe = registry.get<Position>(h.attacker);
            const Position& front = registry.get<Position>(c.target);
            const FVec2 here{me.x, me.y};
            // Más de 90° entre hacia dónde mira y de dónde le golpean: flanco o espalda.
            flank = dot_wide(FVec2{foe.x, foe.y} - here, FVec2{front.x, front.y} - here) < 0;
        }
        lose(registry, h.target, flank ? params_.flank_hit_loss : params_.hit_loss, false);
    }
    // Bajas: pesan a los compañeros que las ven caer y animan a sus enemigos.
    for (const MoraleDeath& d : deaths) {
        for_each_near(d.pos, params_.awareness_radius, [&](std::size_t j) {
            const entt::entity e = s_.entity[j];
            if (!registry.valid(e) || !registry.all_of<Morale>(e)) {
                return;
            }
            Morale& m = registry.get<Morale>(e);
            if (s_.owner[j] == d.owner) {
                lose(registry, e, params_.casualty_loss, false);
            } else if (s_.routing[j] == 0) {
                m.value = std::min(m.value + params_.enemy_casualty_gain, kFullMorale);
            }
        });
        if (d.hero) {
            for_each_near(d.pos, params_.hero_death_radius, [&](std::size_t j) {
                if (s_.owner[j] == d.owner && registry.valid(s_.entity[j])) {
                    lose(registry, s_.entity[j], params_.hero_death_loss, false);
                }
            });
        }
    }
}

void MoraleSystem::rout(entt::registry& registry, MovementSystem& movement, entt::entity e,
                        std::uint32_t& next_order_id, Tick tick) {
    Combatant& c = registry.get<Combatant>(e);
    c.target = entt::null;
    c.explicit_target = false;
    c.attack_move = false;
    c.chase_order = 0;
    registry.remove<Waypoints>(e);
    registry.emplace<Routing>(e);
    const auto owner = registry.get<Owner>(e).player;
    if (owner >= routs_.size()) {
        routs_.resize(owner + std::size_t{1}, 0);
    }
    ++routs_[owner];
    ++stats_.routed;
    const auto id = static_cast<std::size_t>(entt::to_entity(e));
    if (id < index_of_.size() && index_of_[id] >= 0) {
        const auto i = static_cast<std::size_t>(index_of_[id]);
        s_.routing[i] = 1;
        flee(registry, movement, e, i, next_order_id, tick);
    }
}

void MoraleSystem::flee(entt::registry& registry, MovementSystem& movement, entt::entity e, std::size_t i,
                        std::uint32_t& next_order_id, Tick tick) {
    Routing& r = registry.get<Routing>(e);
    r.next_flee = tick + static_cast<Tick>(params_.flee_repath_ticks);
    // Lejos del centro de los enemigos armados cercanos.
    std::int64_t sx = 0;
    std::int64_t sy = 0;
    std::int64_t n = 0;
    for_each_near(s_.pos[i], params_.rally_safe_radius, [&](std::size_t j) {
        if (s_.owner[j] != s_.owner[i] && s_.armed[j] != 0 && s_.routing[j] == 0) {
            sx += s_.pos[j].x.raw();
            sy += s_.pos[j].y.raw();
            ++n;
        }
    });
    if (n == 0) {
        return;  // nadie le persigue: no hace falta seguir corriendo
    }
    const FVec2 threat{Fixed::from_raw(static_cast<std::int32_t>(sx / n)),
                       Fixed::from_raw(static_cast<std::int32_t>(sy / n))};
    FVec2 away = s_.pos[i] - threat;
    if (away.x.raw() == 0 && away.y.raw() == 0) {
        away = {Fixed::from_int(1), Fixed{}};  // encima del enemigo: hacia cualquier lado
    }
    const FVec2 dest = s_.pos[i] + with_length(away, Fixed::from_int(params_.flee_tiles));
    const TileCoord t = tile_of(dest);
    const TileCoord target{std::clamp(t.x, 0, width_ - 1), std::clamp(t.y, 0, height_ - 1)};
    r.flee_order = next_order_id++;
    const std::array<entt::entity, 1> one{e};
    movement.order_move(registry, one, target, r.flee_order, tick);
}

void MoraleSystem::review(entt::registry& registry, MovementSystem& movement, std::uint32_t& next_order_id,
                          Tick tick) {
    for (std::size_t i = 0; i < s_.entity.size(); ++i) {
        const entt::entity e = s_.entity[i];
        if (!registry.valid(e)) {
            continue;
        }
        Morale* m = registry.try_get<Morale>(e);
        if (m == nullptr || registry.all_of<Patient>(e)) {
            continue;
        }
        std::int32_t friends = 0;
        std::int32_t enemies = 0;
        std::int32_t routing_friends = 0;
        bool hero_near = false;
        bool enemy_close = false;
        for_each_near(s_.pos[i], params_.awareness_radius, [&](std::size_t j) {
            if (j == i) {
                return;
            }
            if (s_.owner[j] == s_.owner[i]) {
                friends += s_.armed[j] != 0 && s_.routing[j] == 0 ? 1 : 0;
                routing_friends += s_.routing[j];
            } else if (s_.armed[j] != 0 && s_.routing[j] == 0) {
                ++enemies;
            }
        });
        if (hero_aura_radius_.raw() > 0) {
            for_each_near(s_.pos[i], hero_aura_radius_, [&](std::size_t j) {
                hero_near = hero_near || (s_.hero[j] != 0 && s_.owner[j] == s_.owner[i]);
            });
        }
        for_each_near(s_.pos[i], params_.rally_safe_radius, [&](std::size_t j) {
            enemy_close = enemy_close || (s_.owner[j] != s_.owner[i] && s_.armed[j] != 0 && s_.routing[j] == 0);
        });

        std::int32_t loss = routing_friends * params_.contagion_loss;
        if (enemies > friends + 1) {  // ella misma cuenta como uno
            loss += params_.outnumbered_loss;
        }
        if (const Supply* sp = registry.try_get<Supply>(e);
            sp != nullptr && units_[registry.get<Unit>(e).type].supply.rations > 0 && sp->hungry()) {
            loss += params_.hungry_loss;
        }
        m->value = std::max(m->value - scaled_loss(registry, e, loss, hero_near), 0);

        const Hurt* hurt = registry.try_get<Hurt>(e);
        const bool calm = hurt == nullptr || tick - hurt->tick >= static_cast<Tick>(params_.calm_ticks);
        Routing* r = registry.try_get<Routing>(e);
        if (r != nullptr) {
            if (!enemy_close) {
                m->value = std::min(m->value + params_.routing_gain, kFullMorale);
            }
            if (m->value >= params_.rally_above && !enemy_close) {
                // Se rehace: se detiene y se reorganiza antes de volver a pelear.
                registry.remove<Routing>(e);
                s_.routing[i] = 0;
                MovementSystem::stop(registry, std::array<entt::entity, 1>{e});
                registry.emplace_or_replace<Reorganizing>(e, params_.rally_reorganize_ticks);
                ++stats_.rallied;
            } else if (enemy_close && tick >= r->next_flee) {
                flee(registry, movement, e, i, next_order_id, tick);
            }
            continue;
        }
        if (calm && loss == 0) {
            std::int32_t gain = params_.calm_gain + std::min(friends, params_.comrade_cap) * params_.comrade_gain;
            if (hero_near) {
                gain += params_.hero_gain;
            }
            m->value = std::min(m->value + gain, kFullMorale);
        }
        if (m->value < params_.rout_below) {
            rout(registry, movement, e, next_order_id, tick);
        }
    }
}

void MoraleSystem::update(entt::registry& registry, MovementSystem& movement, std::span<const MoraleHit> hits,
                          std::span<const MoraleDeath> deaths, std::int32_t daylight, std::uint32_t& next_order_id,
                          Tick tick) {
    stats_ = MoraleTickStats{};
    if (!params_.enabled) {
        return;
    }
    daylight_ = daylight;
    const bool review_now = tick % static_cast<Tick>(params_.interval_ticks) == 0;
    if (!review_now && hits.empty() && deaths.empty()) {
        return;  // nada que mirar este tick: ni sucesos ni revisión
    }
    // Las unidades nuevas llegan con la moral entera.
    std::vector<entt::entity> fresh;
    for (const auto [e, u] : registry.view<const Unit>(entt::exclude<Morale>).each()) {
        if (has_morale(registry, e)) {
            fresh.push_back(e);
        }
    }
    for (const entt::entity e : fresh) {
        registry.emplace<Morale>(e, kFullMorale);
    }
    gather(registry);
    apply_events(registry, hits, deaths);
    // Quien cae por debajo del umbral por un suceso no espera a su revisión.
    std::vector<entt::entity> broken;
    for (const auto [e, m] : registry.view<const Morale>(entt::exclude<Routing, Patient>).each()) {
        if (m.value < params_.rout_below) {
            broken.push_back(e);
        }
    }
    std::ranges::sort(broken, {}, [](entt::entity e) { return entt::to_integral(e); });
    for (const entt::entity e : broken) {
        rout(registry, movement, e, next_order_id, tick);
    }
    if (review_now) {
        review(registry, movement, next_order_id, tick);
    }
    stats_.routing = static_cast<std::int32_t>(registry.view<const Routing>().size());
}

void MoraleSystem::hash_into(StateHasher& h, const entt::registry& registry) const {
    if (!params_.enabled) {
        return;
    }
    for (const auto [e, m] : registry.view<const Morale>().each()) {
        h.add_u32(entt::to_integral(e));
        h.add_i32(m.value);
    }
    for (const auto [e, r] : registry.view<const Routing>().each()) {
        h.add_u32(entt::to_integral(e));
        h.add_u32(r.flee_order);
        h.add_u32(static_cast<std::uint32_t>(r.next_flee));
    }
}

}  // namespace rts::sim
