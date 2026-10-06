#include "sim/combat.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <limits>
#include <utility>

#include "sim/climb.hpp"
#include "sim/formation.hpp"
#include "sim/garrison.hpp"
#include "sim/state_hash.hpp"

namespace rts::sim {

namespace {

// Tope de daño de un golpe: con él, ni la vida ni la experiencia acumulada de un tick
// desbordan 32 bits.
constexpr std::int64_t kMaxHitDamage = 1'000'000;

std::int32_t chebyshev(TileCoord a, TileCoord b) noexcept {
    return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y));
}

FVec2 footprint_center(const Footprint& f) noexcept {
    return {Fixed::from_int(f.origin.x) + Fixed::from_ratio(f.size, 2),
            Fixed::from_int(f.origin.y) + Fixed::from_ratio(f.size, 2)};
}

}  // namespace

CombatSystem::CombatSystem(const TileMap& map, const CombatParams& params, std::vector<UnitType> units,
                           std::vector<BuildingType> buildings)
    : map_(&map),
      width_(map.width()),
      height_(map.height()),
      params_(params),
      units_(std::move(units)),
      buildings_(std::move(buildings)) {
    assert(params_.acquire_interval_ticks > 0);
}

std::int32_t CombatSystem::attack_percent(const Combatant& c, bool aura) const noexcept {
    return kPercent + c.level * params_.attack_percent_per_level + (aura ? params_.hero_aura_attack_percent : 0);
}

std::int32_t CombatSystem::attack_percent(const entt::registry& registry, entt::entity e, const Combatant& c,
                                          bool aura) const noexcept {
    std::int32_t percent = attack_percent(c, aura);
    if (const Fatigue* f = registry.try_get<Fatigue>(e)) {
        percent = static_cast<std::int32_t>(std::int64_t{percent} * f->attack_percent / kPercent);  // cansancio
    }
    if (const Formation* fm = registry.try_get<Formation>(e); fm != nullptr && fm->active) {
        percent = static_cast<std::int32_t>(std::int64_t{percent} * fm->attack_percent / kPercent);  // formación
    }
    const Supply* s = registry.try_get<Supply>(e);
    if (s == nullptr || units_[registry.get<Unit>(e).type].supply.rations <= 0 || !s->hungry()) {
        return percent;
    }
    return static_cast<std::int32_t>(std::int64_t{percent} * params_.hungry_attack_percent / kPercent);
}

bool CombatSystem::out_of_ammo(const entt::registry& registry, entt::entity e) const noexcept {
    const Supply* s = registry.try_get<Supply>(e);
    return s != nullptr && units_[registry.get<Unit>(e).type].supply.ammo > 0 && s->ammo <= 0;
}

std::int32_t hit_damage(const CombatStats& a, std::int32_t percent, std::int32_t armor_melee,
                        std::int32_t armor_pierce, ArmorClassId armor_class) noexcept {
    // En 64 bits y con tope: con los límites de los datos, ataque por porcentaje de
    // nivel no cabe en 32 bits en el peor caso, y un desbordamiento rompería el
    // determinismo.
    const std::int64_t melee = std::int64_t{a.attack_melee} * percent / kPercent;
    const std::int64_t pierce = std::int64_t{a.attack_pierce} * percent / kPercent;
    const std::int64_t sum = std::max<std::int64_t>(0, melee - armor_melee) +
                             std::max<std::int64_t>(0, pierce - armor_pierce) + a.bonus[armor_class];
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(sum, 1, kMaxHitDamage));
}

std::int32_t CombatSystem::damage(UnitTypeId attacker_type, PlayerId attacker_owner, std::int32_t percent,
                                  const entt::registry& registry, entt::entity target) const {
    CombatStats a = units_[attacker_type].combat;
    if (economy_ != nullptr) {
        // Mejoras del atacante: el ataque, solo si ya lo tenía.
        const UpgradeBonus up = economy_->upgrade_bonus(attacker_owner, a.armor_class);
        a.attack_melee += a.attack_melee > 0 ? up.attack_melee : 0;
        a.attack_pierce += a.attack_pierce > 0 ? up.attack_pierce : 0;
    }
    std::int32_t armor_melee = 0;
    std::int32_t armor_pierce = 0;
    ArmorClassId armor_class = 0;
    if (const Unit* u = registry.try_get<Unit>(target)) {
        const CombatStats& t = units_[u->type].combat;
        const Combatant* tc = registry.try_get<Combatant>(target);
        const std::int32_t extra =
            tc != nullptr && params_.armor_every_levels > 0 ? tc->level / params_.armor_every_levels : 0;
        armor_melee = t.armor_melee + extra;
        armor_pierce = t.armor_pierce + extra;
        armor_class = t.armor_class;
        if (economy_ != nullptr && registry.all_of<Owner>(target)) {
            const UpgradeBonus up = economy_->upgrade_bonus(registry.get<Owner>(target).player, t.armor_class);
            armor_melee += up.armor_melee;
            armor_pierce += up.armor_pierce;
        }
    } else if (const Building* b = registry.try_get<Building>(target)) {
        const BuildingType& bt = buildings_[b->type];
        armor_melee = bt.armor_melee;
        armor_pierce = bt.armor_pierce;
        armor_class = bt.armor_class;
    }
    return hit_damage(a, percent, armor_melee, armor_pierce, armor_class);
}

void CombatSystem::apply(entt::registry& registry, MovementSystem& movement, const Command& command,
                         std::span<const entt::entity> units, std::uint32_t& next_order_id, Tick tick) {
    switch (command.type) {
        case CommandType::Attack: {
            const auto target = static_cast<entt::entity>(command.object);
            if (command.object == kNoObject || !is_enemy_target(registry, target, command.player)) {
                return;
            }
            const bool is_unit = registry.all_of<Unit>(target);
            for (const entt::entity e : units) {
                if (is_unit && units_[registry.get<Unit>(e).type].combat.buildings_only) {
                    continue;  // un ariete no ataca a unidades
                }
                if (Combatant* c = registry.try_get<Combatant>(e)) {
                    c->target = target;
                    c->explicit_target = true;
                    c->attack_move = false;
                    c->chase_order = 0;
                }
            }
            return;
        }
        case CommandType::AttackMove: {
            // Un solo movimiento de grupo (campo de flujo si son muchos); cada unidad
            // se desvía a pelear y, sin blanco, vuelve a su destino por su cuenta.
            const std::uint32_t order = next_order_id++;
            movement.order_move(registry, units, command.target, order, tick);
            for (const entt::entity e : units) {
                if (Combatant* c = registry.try_get<Combatant>(e)) {
                    c->target = entt::null;
                    c->explicit_target = false;
                    c->attack_move = true;
                    c->move_dest = command.target;
                    c->chase_order = order;
                }
            }
            return;
        }
        case CommandType::SetStance: {
            if (command.kind > static_cast<std::uint8_t>(Stance::HoldGround)) {
                return;
            }
            for (const entt::entity e : units) {
                if (Combatant* c = registry.try_get<Combatant>(e)) {
                    c->stance = static_cast<Stance>(command.kind);
                }
            }
            return;
        }
        case CommandType::Count:
        case CommandType::SetRally:
        case CommandType::Research:
            return;
        case CommandType::Garrison:
        case CommandType::Climb:
        case CommandType::Extinguish:
        case CommandType::Demolish:
        case CommandType::Convoy:
        case CommandType::Treat:
        case CommandType::Tend:
        case CommandType::Move:
        case CommandType::Stop:
        case CommandType::Gather:
        case CommandType::Build:
        case CommandType::Place:
        case CommandType::Train:
        case CommandType::CancelTrain:
            // Cualquier otra orden a una unidad cancela su combate.
            for (const entt::entity e : units) {
                if (Combatant* c = registry.try_get<Combatant>(e)) {
                    c->target = entt::null;
                    c->explicit_target = false;
                    c->attack_move = false;
                    c->chase_order = 0;
                }
            }
            return;
    }
}

bool CombatSystem::stops_charge(const entt::registry& registry, entt::entity target) const {
    const Formation* f = registry.try_get<Formation>(target);
    return f != nullptr && f->active && f->stops_charge;  // el erizo de picas
}

bool CombatSystem::charge_ground(TerrainId t) const noexcept {
    const auto& ok = params_.terrain.charge_by_terrain;
    return t < ok.size() && ok[t] != 0;
}

TerrainId CombatSystem::terrain_at(FVec2 p) const noexcept {
    const TileCoord t = tile_of(p);
    return map_->terrain({std::clamp(t.x, 0, width_ - 1), std::clamp(t.y, 0, height_ - 1)});
}

std::int32_t CombatSystem::slope_levels(const entt::registry& registry, FVec2 a, entt::entity target) const {
    const TerrainCombatParams& tc = params_.terrain;
    if (!tc.enabled || tc.max_levels <= 0) {
        return 0;
    }
    const auto level_at = [&](FVec2 p) {
        const TileCoord t = tile_of(p);
        return std::int32_t{map_->elevation({std::clamp(t.x, 0, width_ - 1), std::clamp(t.y, 0, height_ - 1)})};
    };
    const std::int32_t diff = level_at(a) - level_at(aim_point(registry, target));
    return std::clamp(diff, -tc.max_levels, tc.max_levels);
}

Fixed CombatSystem::effective_range(const CombatStats& st, std::int32_t levels) const noexcept {
    if (levels == 0 || st.projectile_speed.raw() == 0) {
        return st.range;  // cuerpo a cuerpo: la altura no alarga el brazo
    }
    return slope_range(st.range, params_.terrain.range_per_level, levels);
}

Fixed slope_range(Fixed range, Fixed per_level, std::int32_t levels) noexcept {
    // Un proyectil lento llega más lejos cuesta abajo y menos cuesta arriba.
    const Fixed r = range + per_level * levels;
    const Fixed floor = Fixed::from_raw(range.raw() / 2);
    return r < floor ? floor : r;
}

std::int32_t slope_percent(std::int32_t percent, std::int32_t per_level, std::int32_t levels) noexcept {
    return static_cast<std::int32_t>(std::int64_t{percent} * std::max(kPercent + per_level * levels, 0) / kPercent);
}

void CombatSystem::gather(const entt::registry& registry) {
    s_.entity.clear();
    s_.pos.clear();
    s_.radius.clear();
    s_.owner.clear();
    s_.klass.clear();
    s_.aiming.clear();
    s_.shielded.clear();
    const auto view = registry.view<const Position, const Unit, const Owner, const Health>();
    for (const entt::entity e : view) {
        const Position& p = view.get<const Position>(e);
        const UnitType& ut = units_[view.get<const Unit>(e).type];
        s_.entity.push_back(e);
        s_.pos.push_back({p.x, p.y});
        s_.radius.push_back(view.get<const Unit>(e).radius);
        s_.owner.push_back(view.get<const Owner>(e).player);
        TargetClass k = TargetClass::Other;
        if (ut.convoy_capacity > 0) {
            k = TargetClass::Carrier;
        } else if (ut.worker) {
            k = TargetClass::Worker;
        } else if (ut.combat.buildings_only) {
            k = TargetClass::Siege;
        } else if (ut.combat.auto_attack) {
            k = TargetClass::Armed;
        }
        s_.klass.push_back(k);
        const Combatant* c = registry.try_get<Combatant>(e);
        s_.aiming.push_back(c != nullptr ? c->target : entt::entity{entt::null});
        const Garrisoned* g = registry.try_get<Garrisoned>(e);
        s_.shielded.push_back(g != nullptr && g->inside ? 1 : 0);
    }
    const std::size_t n = s_.entity.size();
    s_.aura.assign(n, 0);
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

std::size_t CombatSystem::cell_of(FVec2 p) const noexcept {
    const TileCoord t = tile_of(p);
    const std::int32_t x = std::clamp(t.x, 0, width_ - 1);
    const std::int32_t y = std::clamp(t.y, 0, height_ - 1);
    return static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x);
}

// Candidatas en el cuadrado de casillas que cubre el círculo; fn filtra por distancia.
// Orden fijo: filas, columnas y, dentro de la casilla, el de la ordenación por conteo.
template <typename Fn>
void CombatSystem::for_each_near(FVec2 center, Fixed radius, Fn&& fn) const {
    const TileCoord t = tile_of(center);
    const std::int32_t reach = radius.floor_to_int() + 1;
    for (std::int32_t cy = std::max(t.y - reach, 0); cy <= std::min(t.y + reach, height_ - 1); ++cy) {
        for (std::int32_t cx = std::max(t.x - reach, 0); cx <= std::min(t.x + reach, width_ - 1); ++cx) {
            const std::size_t c = static_cast<std::size_t>(cy) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(cx);
            for (std::uint32_t u = s_.cell_start[c]; u < s_.cell_start[c + 1]; ++u) {
                fn(static_cast<std::size_t>(s_.cell_units[u]));
            }
        }
    }
}

void CombatSystem::mark_auras(const entt::registry& registry) {
    const Fixed radius = params_.hero_aura_radius;
    if (radius.raw() <= 0) {
        return;
    }
    const std::int64_t r_sq = mul_wide(radius, radius);
    for (std::size_t i = 0; i < s_.entity.size(); ++i) {
        const Combatant* c = registry.try_get<Combatant>(s_.entity[i]);
        if (c == nullptr || c->hero_name < 0) {
            continue;
        }
        // Las auras no se acumulan: basta con marcar.
        for_each_near(s_.pos[i], radius, [&](std::size_t j) {
            if (s_.owner[j] == s_.owner[i] && length_sq_wide(s_.pos[j] - s_.pos[i]) <= r_sq) {
                s_.aura[j] = 1;
            }
        });
    }
}

bool CombatSystem::is_enemy_target(const entt::registry& registry, entt::entity target, PlayerId me) const {
    if (registry.valid(target)) {
        if (const Garrisoned* g = registry.try_get<Garrisoned>(target); g != nullptr && g->inside) {
            return false;  // a cubierto en su torre: hay que tomar la torre
        }
    }
    return registry.valid(target) && registry.all_of<Health, Owner>(target) && registry.get<Owner>(target).player != me &&
           (registry.all_of<Unit, Position>(target) || registry.all_of<Building, Footprint>(target));
}

std::size_t CombatSystem::priority_of(TargetClass k) const noexcept {
    const auto& order = params_.target_priority;
    const auto it = std::ranges::find(order, k);
    return order.empty() ? 0 : static_cast<std::size_t>(it - order.begin());  // no listada: al final
}

entt::entity CombatSystem::acquire(const entt::registry& registry, std::size_t i, Fixed sight) const {
    const std::int64_t sight_sq = mul_wide(sight, sight);
    std::size_t best = s_.entity.size();
    std::size_t best_tier = std::numeric_limits<std::size_t>::max();
    std::int64_t best_d = std::numeric_limits<std::int64_t>::max();
    for_each_near(s_.pos[i], sight, [&](std::size_t j) {
        if (s_.owner[j] == s_.owner[i] || s_.shielded[j] != 0) {
            return;
        }
        const std::int64_t d = length_sq_wide(s_.pos[j] - s_.pos[i]);
        if (d > sight_sq || (vision_ != nullptr && !vision_->sees_unit(registry, s_.owner[i], s_.entity[j]))) {
            return;  // fuera de su vista, o escondido (entre árboles, de noche...)
        }
        // Autopreservación: el armado que me tiene por blanco, antes que nada.
        const bool at_me = s_.aiming[j] == s_.entity[i] && s_.klass[j] == TargetClass::Armed;
        const std::size_t tier = priority_of(at_me ? TargetClass::AttackingMe : s_.klass[j]);
        if (tier < best_tier || (tier == best_tier && (d < best_d || (d == best_d && j < best)))) {
            best_tier = tier;
            best_d = d;
            best = j;
        }
    });
    return best < s_.entity.size() ? s_.entity[best] : entt::entity{entt::null};
}

entt::entity CombatSystem::acquire_building(const entt::registry& registry, const EconomySystem& economy,
                                            std::size_t i, std::int32_t sight_tiles) const {
    const TileCoord t = tile_of(s_.pos[i]);
    const Fixed sight = Fixed::from_int(sight_tiles);
    const std::int64_t sight_sq = mul_wide(sight, sight);
    entt::entity best = entt::null;
    std::int64_t best_d = std::numeric_limits<std::int64_t>::max();
    // Recorrido fijo por filas: a igual distancia gana el primero encontrado.
    for (std::int32_t y = t.y - sight_tiles; y <= t.y + sight_tiles; ++y) {
        for (std::int32_t x = t.x - sight_tiles; x <= t.x + sight_tiles; ++x) {
            const entt::entity o = economy.occupant({x, y});
            if (o == entt::null || o == best || !registry.all_of<Building, Owner, Health>(o) ||
                registry.get<Owner>(o).player == s_.owner[i] ||
                !can_harm_building(registry, registry.get<Unit>(s_.entity[i]).type, o) ||
                (vision_ != nullptr && !vision_->sees_footprint(s_.owner[i], registry.get<Footprint>(o)))) {
                continue;
            }
            const std::int64_t d = distance_sq_to(registry.get<Footprint>(o), s_.pos[i]);
            if (d <= sight_sq && d < best_d) {
                best_d = d;
                best = o;
            }
        }
    }
    return best;
}

bool CombatSystem::in_range(const entt::registry& registry, FVec2 pos, Fixed radius, Fixed range,
                            entt::entity target) const {
    if (const Unit* u = registry.try_get<Unit>(target)) {
        const Position& p = registry.get<Position>(target);
        const Fixed reach = radius + u->radius + range;
        return length_sq_wide(FVec2{p.x, p.y} - pos) <= mul_wide(reach, reach);
    }
    const Fixed reach = radius + range + params_.building_reach;
    return distance_sq_to(registry.get<Footprint>(target), pos) <= mul_wide(reach, reach);
}

FVec2 CombatSystem::aim_point(const entt::registry& registry, entt::entity target) const {
    if (const Position* p = registry.try_get<Position>(target)) {
        return {p->x, p->y};
    }
    return footprint_center(registry.get<Footprint>(target));
}

void CombatSystem::clear_target(entt::registry& registry, MovementSystem& movement, entt::entity e, Combatant& c,
                                std::uint32_t& next_order_id, Tick tick) const {
    c.target = entt::null;
    c.explicit_target = false;
    if (c.attack_move) {
        // Sin blanco, el ataque-movimiento sigue hacia su destino.
        c.chase_order = next_order_id++;
        const std::array<entt::entity, 1> one{e};
        movement.order_move(registry, one, c.move_dest, c.chase_order, tick);
        return;
    }
    if (c.chase_order != 0) {
        const MoveGoal* g = registry.try_get<MoveGoal>(e);
        if (g != nullptr && g->order_id == c.chase_order) {
            registry.remove<MoveGoal, PathFollow>(e);
        }
        c.chase_order = 0;
    }
}

void CombatSystem::strike(const entt::registry& registry, FireSystem& fire, entt::entity target,
                          entt::entity attacker, UnitTypeId attacker_type, PlayerId attacker_owner,
                          std::int32_t percent) {
    const CombatStats& st = units_[attacker_type].combat;
    if (const Building* b = registry.try_get<Building>(target)) {
        const bool stone = buildings_[b->type].material == Material::Stone;
        if (st.undermine && stone) {
            // Mina bajo los cimientos: la armadura de los muros no cuenta.
            const std::int64_t dig = std::int64_t{st.attack_melee} * percent / kPercent;
            hits_.push_back(
                {target, attacker, static_cast<std::int32_t>(std::clamp<std::int64_t>(dig, 1, kMaxHitDamage)), attacker_type});
            return;
        }
        if (!st.siege) {
            fire.add_heat(target, st.ignite);
            return;
        }
    }
    hits_.push_back({target, attacker, damage(attacker_type, attacker_owner, percent, registry, target), attacker_type});
}

bool CombatSystem::can_harm_building(const entt::registry& registry, UnitTypeId attacker_type,
                                     entt::entity building) const {
    const CombatStats& st = units_[attacker_type].combat;
    if (st.siege) {
        return true;
    }
    const Building& b = registry.get<Building>(building);
    if (st.undermine && buildings_[b.type].material == Material::Stone) {
        return true;
    }
    // Piedra ya quemada: no le queda qué arder; solo el asedio la derriba.
    return st.ignite > 0 && !(buildings_[b.type].material == Material::Stone && b.burned);
}

void CombatSystem::update(entt::registry& registry, MovementSystem& movement, EconomySystem& economy,
                          FireSystem& fire, std::uint32_t& next_order_id, Tick tick, const VisionSystem* vision) {
    stats_ = CombatTickStats{};
    vision_ = vision;
    economy_ = &economy;
    hits_.clear();
    new_projectiles_.clear();
    morale_hits_.clear();
    morale_deaths_.clear();
    gather(registry);
    mark_auras(registry);
    const auto interval = static_cast<std::uint32_t>(params_.acquire_interval_ticks);

    for (std::size_t i = 0; i < s_.entity.size(); ++i) {
        const entt::entity e = s_.entity[i];
        Combatant* cp = registry.try_get<Combatant>(e);
        if (cp == nullptr) {
            continue;
        }
        Combatant& c = *cp;
        if (c.cooldown > 0 && registry.any_of<Patient, Reorganizing, Routing, Climbing>(e)) {
            --c.cooldown;
        }
        if (registry.any_of<Patient, Reorganizing, Routing, Climbing>(e)) {
            // Herido camino del puesto o ingresado, reorganizándose tras el alta, en
            // desbandada o escalando un muro: no pelea (se le puede atacar).
            c.target = entt::null;
            c.explicit_target = false;
            c.attack_move = false;
            continue;
        }
        const Unit& unit = registry.get<Unit>(e);
        const CombatStats& st = units_[unit.type].combat;
        const FVec2 pos = s_.pos[i];
        const Fixed sight = Fixed::from_int(st.sight_tiles);
        if (c.cooldown > 0) {
            --c.cooldown;
        }
        const bool no_ammo = out_of_ammo(registry, e);
        // Sin munición un tirador no puede hacer nada: deja el blanco y espera a reponer.
        if (no_ammo && c.target != entt::null) {
            clear_target(registry, movement, e, c, next_order_id, tick);
        }

        // 1. Blanco que ya no vale (muerto, cambió de dueño) o, si no fue ordenado,
        //    que se alejó más allá de la vista: se abandona.
        if (c.target != entt::null && !is_enemy_target(registry, c.target, s_.owner[i])) {
            clear_target(registry, movement, e, c, next_order_id, tick);
        }
        // Un edificio al que ya no puede hacer nada (piedra quemada, sin asedio) se deja.
        if (c.target != entt::null && registry.all_of<Building>(c.target) &&
            !can_harm_building(registry, unit.type, c.target)) {
            clear_target(registry, movement, e, c, next_order_id, tick);
        }
        if (c.target != entt::null && !c.explicit_target && !in_range(registry, pos, unit.radius, sight, c.target)) {
            clear_target(registry, movement, e, c, next_order_id, tick);
        }
        const MoveGoal* goal = registry.try_get<MoveGoal>(e);
        if (c.attack_move && c.target == entt::null && goal != nullptr && goal->order_id == c.chase_order &&
            goal->arrived) {
            c.attack_move = false;  // llegó al destino del ataque-movimiento
        }
        if (c.attack_move && c.target == entt::null && goal == nullptr) {
            // Sin camino a su destino (otra isla, fuera del radio de búsqueda): el
            // ataque-movimiento termina aquí en vez de quedar pendiente para siempre.
            c.attack_move = false;
        }

        // 2. Adquisición automática, repartida entre ticks por id.
        if (c.target == entt::null && !no_ammo && st.auto_attack && st.sight_tiles > 0 &&
            (entt::to_integral(e) + tick) % interval == 0) {
            bool idle = goal == nullptr || goal->arrived || c.attack_move;
            if (const Worker* w = registry.try_get<Worker>(e); w != nullptr && w->task != WorkerTask::Idle) {
                idle = false;
            }
            if (registry.all_of<Extinguisher>(e)) {
                idle = false;  // apagando un fuego: no sale a buscar pelea
            }
            if (idle) {
                entt::entity t = st.buildings_only ? entt::entity{entt::null} : acquire(registry, i, sight);
                if (t == entt::null) {
                    t = acquire_building(registry, economy, i, st.sight_tiles);
                }
                if (t != entt::null) {
                    c.target = t;
                    c.explicit_target = false;
                    c.chase_order = 0;
                    ++stats_.acquisitions;
                }
            }
        }
        if (c.target == entt::null) {
            continue;
        }

        // 3. Al alcance: golpe o disparo cuando la recarga lo permite. Si no, perseguir.
        const TerrainCombatParams& tc = params_.terrain;
        std::int32_t levels = slope_levels(registry, pos, c.target);
        const Garrisoned* garrison = registry.try_get<Garrisoned>(e);
        const bool in_tower = garrison != nullptr && garrison->inside;
        if (in_tower && tc.enabled && registry.valid(garrison->tower) && registry.all_of<Building>(garrison->tower)) {
            // Desde lo alto de la torre (B4): su altura se suma a la del terreno.
            levels += buildings_[registry.get<Building>(garrison->tower).type].garrison_levels;
        }
        if (tc.enabled) {
            // Carrera para la carga: ticks seguidos en marcha.
            if (goal != nullptr && !goal->arrived) {
                c.run_ticks = std::min(c.run_ticks + 1, tc.charge_run_ticks);
            } else if (c.cooldown == 0) {
                c.run_ticks = 0;
            }
        }
        if (in_range(registry, pos, unit.radius, effective_range(st, levels), c.target)) {
            c.chase_failures = 0;
            // Al alcance se detiene, venga de donde venga su movimiento: persecución,
            // ataque-movimiento u orden previa. Si era ataque-movimiento, al quedarse sin
            // blanco vuelve a su destino (clear_target).
            if (goal != nullptr) {
                registry.remove<MoveGoal, PathFollow>(e);
            }
            c.chase_order = 0;
            if (c.cooldown == 0) {
                c.cooldown = st.reload_ticks;
                std::int32_t percent = attack_percent(registry, e, c, s_.aura[i] != 0);
                if (tc.enabled) {
                    const bool ranged = st.projectile_speed.raw() != 0;
                    const std::int32_t per_level = ranged ? tc.ranged_percent_per_level : tc.melee_percent_per_level;
                    percent = slope_percent(percent, per_level, levels);
                    const std::int32_t charge = units_[unit.type].charge_percent;
                    if (!ranged && charge > kPercent && c.run_ticks >= tc.charge_run_ticks &&
                        registry.all_of<Unit>(c.target) && !stops_charge(registry, c.target) &&
                        charge_ground(terrain_at(pos)) &&
                        charge_ground(terrain_at(aim_point(registry, c.target)))) {
                        percent = static_cast<std::int32_t>(std::int64_t{percent} * charge / kPercent);
                    }
                    c.run_ticks = 0;
                }
                if (Supply* sp = registry.try_get<Supply>(e); sp != nullptr && units_[unit.type].supply.ammo > 0) {
                    --sp->ammo;  // no_ammo ya descartó el caso sin munición
                }
                if (st.projectile_speed.raw() == 0) {
                    strike(registry, fire, c.target, e, unit.type, s_.owner[i], percent);
                    ++stats_.melee_hits;
                } else {
                    // Apunta a donde está el blanco ahora: si se mueve, puede esquivarlo.
                    new_projectiles_.push_back({pos, aim_point(registry, c.target), st.projectile_speed, e, c.target,
                                                s_.owner[i], unit.type, percent});
                    ++stats_.projectiles_fired;
                }
            }
            continue;
        }
        if (in_tower) {
            c.target = entt::null;  // en la torre: no sale a perseguir
            c.explicit_target = false;
            continue;
        }
        if (c.stance == Stance::HoldGround && !c.explicit_target) {
            c.target = entt::null;  // mantener posición: no persigue
            continue;
        }
        if (unit.speed.raw() <= 0) {
            clear_target(registry, movement, e, c, next_order_id, tick);  // inmóvil: no puede perseguir
            continue;
        }
        const TileCoord target_tile = tile_of(aim_point(registry, c.target));
        const bool chasing = c.chase_order != 0 && goal != nullptr && goal->order_id == c.chase_order && !goal->arrived;
        const bool target_moved = chebyshev(target_tile, c.chase_tile) >= params_.repath_tiles;
        if (chasing && !target_moved) {
            continue;
        }
        if (!chasing && !target_moved && c.chase_order != 0 && goal != nullptr && goal->order_id == c.chase_order &&
            ++c.chase_failures > params_.chase_attempts) {
            // Varias llegadas sin alcanzar un blanco que no se mueve: inalcanzable.
            c.chase_failures = 0;
            clear_target(registry, movement, e, c, next_order_id, tick);
            continue;
        }
        c.chase_order = next_order_id++;
        c.chase_tile = target_tile;
        const std::array<entt::entity, 1> one{e};
        movement.order_move(registry, one, target_tile, c.chase_order, tick);
    }

    update_projectiles(registry, economy, fire);
    for (const Projectile& p : new_projectiles_) {
        registry.emplace<Projectile>(registry.create(), p);
    }
    apply_hits(registry, movement, economy, tick);
}

void CombatSystem::update_projectiles(entt::registry& registry, const EconomySystem& economy, FireSystem& fire) {
    scratch_.clear();
    for (const entt::entity e : registry.view<Projectile>()) {
        scratch_.push_back(e);
    }
    const Fixed hit_radius = params_.projectile_hit_radius;
    for (const entt::entity e : scratch_) {
        Projectile& p = registry.get<Projectile>(e);
        const FVec2 d = p.dest - p.pos;
        if (length_sq_wide(d) > mul_wide(p.speed, p.speed)) {
            p.pos = p.pos + with_length(d, p.speed);
            continue;
        }
        // Cae en el punto de destino: el blanco previsto si sigue ahí; si no, la unidad
        // enemiga más cercana que esté en ese punto; si no, un edificio enemigo; si no, falla.
        p.pos = p.dest;
        entt::entity target = entt::null;
        bool intended_here = false;
        std::size_t best = s_.entity.size();
        std::int64_t best_d = std::numeric_limits<std::int64_t>::max();
        for_each_near(p.dest, Fixed::from_int(1) + hit_radius, [&](std::size_t j) {
            if (s_.owner[j] == p.owner || s_.shielded[j] != 0 || !registry.valid(s_.entity[j])) {
                return;
            }
            const Fixed reach = s_.radius[j] + hit_radius;
            const std::int64_t dist = length_sq_wide(s_.pos[j] - p.dest);
            if (dist > mul_wide(reach, reach)) {
                return;
            }
            if (s_.entity[j] == p.intended) {
                intended_here = true;
            }
            if (dist < best_d || (dist == best_d && j < best)) {
                best_d = dist;
                best = j;
            }
        });
        if (intended_here) {
            target = p.intended;
        } else if (best < s_.entity.size()) {
            target = s_.entity[best];
        } else {
            const entt::entity o = economy.occupant(tile_of(p.dest));
            if (o != entt::null && registry.all_of<Building, Owner, Health>(o) &&
                registry.get<Owner>(o).player != p.owner) {
                target = o;
            }
        }
        if (target != entt::null) {
            std::int32_t percent = p.attack_percent;
            if (params_.terrain.enabled && registry.all_of<Unit>(target)) {
                // Entre árboles las flechas se quedan en las ramas.
                const auto& cover = params_.terrain.arrow_cover_percent_by_terrain;
                const TerrainId t = terrain_at(aim_point(registry, target));
                if (t < cover.size()) {
                    percent = static_cast<std::int32_t>(std::int64_t{percent} * cover[t] / kPercent);
                }
            }
            strike(registry, fire, target, p.attacker, p.attacker_type, p.owner, percent);
            ++stats_.projectiles_hit;
        } else {
            ++stats_.projectiles_missed;
        }
        registry.destroy(e);
    }
}

void CombatSystem::level_up(entt::registry& registry, entt::entity e, Combatant& c) {
    const auto& thresholds = params_.level_thresholds;
    std::int32_t level = 0;
    while (std::cmp_less(level, thresholds.size()) && c.xp >= thresholds[static_cast<std::size_t>(level)]) {
        ++level;
    }
    Health& health = registry.get<Health>(e);
    const std::int32_t base_hp = units_[registry.get<Unit>(e).type].combat.hp;
    while (c.level < level) {
        ++c.level;
        const std::int32_t gain = base_hp * params_.hp_percent_per_level / kPercent;
        health.max_hp += gain;
        health.hp += gain;
    }
    if (!thresholds.empty() && std::cmp_equal(c.level, thresholds.size()) && c.hero_name < 0) {
        c.hero_name = params_.hero_name_count > 0 ? heroes_made_ % params_.hero_name_count : 0;
        ++heroes_made_;
    }
}

void CombatSystem::apply_hits(entt::registry& registry, MovementSystem& movement, EconomySystem& economy,
                              Tick tick) {
    if (hits_.empty()) {
        return;
    }
    // Orden total (blanco, atacante, daño): la suma no depende del orden, pero el
    // reparto de la bonificación por baja se recorre agrupado por blanco.
    std::ranges::sort(hits_, [](const Hit& a, const Hit& b) {
        const auto ta = entt::to_integral(a.target);
        const auto tb = entt::to_integral(b.target);
        if (ta != tb) {
            return ta < tb;
        }
        const auto aa = entt::to_integral(a.attacker);
        const auto ab = entt::to_integral(b.attacker);
        return aa != ab ? aa < ab : a.amount < b.amount;
    });
    const auto credit = [&](entt::entity attacker, std::int32_t xp) {
        if (registry.valid(attacker)) {
            if (Combatant* c = registry.try_get<Combatant>(attacker)) {
                c->xp = static_cast<std::int32_t>(
                    std::min<std::int64_t>(std::int64_t{c->xp} + xp, std::numeric_limits<std::int32_t>::max()));
            }
        }
    };
    for (Hit& h : hits_) {
        if (const Climbing* cl = registry.try_get<Climbing>(h.target); cl != nullptr && cl->timer >= 0) {
            // En lo alto de la escala no hay cómo cubrirse.
            h.amount = static_cast<std::int32_t>(std::int64_t{h.amount} * params_.climb_exposed_percent / kPercent);
        }
        if (const Formation* f = registry.try_get<Formation>(h.target); f != nullptr && f->active) {
            // Formación (B5): el cuadro aguanta a la caballería pero es blanco fácil.
            const CombatStats& by = units_[h.attacker_type].combat;
            std::int64_t amount = h.amount;
            if (by.projectile_speed.raw() != 0) {
                amount = amount * f->ranged_taken_percent / kPercent;
            } else if (by.armor_class == params_.cavalry_class) {
                amount = amount * f->cavalry_taken_percent / kPercent;
            }
            h.amount = static_cast<std::int32_t>(std::max<std::int64_t>(amount, 1));
        }
    }
    for (const Hit& h : hits_) {
        if (Health* health = registry.try_get<Health>(h.target)) {
            health->hp = std::max(health->hp - h.amount, 0);  // sin desbordar con muchos golpes
            credit(h.attacker, h.amount);
            if (registry.all_of<Unit>(h.target)) {
                registry.emplace_or_replace<Hurt>(h.target, tick);  // las heridas leves sanan en calma
                morale_hits_.push_back({h.target, h.attacker});
            }
        }
    }
    // Bajas: cada atacante que golpeó este tick a un blanco que muere recibe la bonificación.
    std::vector<entt::entity> dead;
    std::vector<std::optional<PlayerId>> killers;  // paralelo a dead (estadísticas)
    for (std::size_t i = 0; i < hits_.size(); ++i) {
        const Hit& h = hits_[i];
        const Health* health = registry.try_get<Health>(h.target);
        if (health == nullptr || health->hp > 0) {
            continue;
        }
        const bool first_of_target = i == 0 || hits_[i - 1].target != h.target;
        if (first_of_target) {
            dead.push_back(h.target);
            const Owner* ko = registry.valid(h.attacker) ? registry.try_get<Owner>(h.attacker) : nullptr;
            killers.push_back(ko != nullptr ? std::optional<PlayerId>(ko->player) : std::nullopt);
        }
        if (first_of_target || hits_[i - 1].attacker != h.attacker) {
            credit(h.attacker, params_.xp_kill_bonus);
        }
    }
    for (const Hit& h : hits_) {
        if (registry.valid(h.attacker)) {
            if (Combatant* c = registry.try_get<Combatant>(h.attacker)) {
                level_up(registry, h.attacker, *c);
            }
        }
    }
    for (std::size_t i = 0; i < dead.size(); ++i) {
        const entt::entity e = dead[i];
        if (registry.all_of<Building>(e)) {
            economy.remove_building(registry, movement, e, true);  // derribado: escombros
        } else {
            const Position& p = registry.get<Position>(e);
            const Combatant* c = registry.try_get<Combatant>(e);
            morale_deaths_.push_back({{p.x, p.y}, registry.get<Owner>(e).player, c != nullptr && c->hero_name >= 0});
            economy.record_loss(registry.get<Owner>(e).player, killers[i]);
            registry.destroy(e);
        }
        ++stats_.kills;
    }
}

void CombatSystem::hash_into(StateHasher& h, const entt::registry& registry) const {
    h.add_i32(heroes_made_);
    for (const auto [e, hp] : registry.view<const Health>().each()) {
        h.add_u32(entt::to_integral(e));
        h.add_i32(hp.hp);
        h.add_i32(hp.max_hp);
    }
    for (const auto [e, c] : registry.view<const Combatant>().each()) {
        h.add_u32(entt::to_integral(e));
        h.add_u32(entt::to_integral(c.target));
        h.add_u32(c.explicit_target ? 1U : 0U);
        h.add_u32(c.attack_move ? 1U : 0U);
        h.add_u32(static_cast<std::uint32_t>(c.stance));
        h.add_i32(c.move_dest.x);
        h.add_i32(c.move_dest.y);
        h.add_i32(c.cooldown);
        h.add_u32(c.chase_order);
        h.add_i32(c.chase_tile.x);
        h.add_i32(c.chase_tile.y);
        h.add_i32(c.chase_failures);
        h.add_i32(c.xp);
        h.add_i32(c.level);
        h.add_i32(c.hero_name);
        if (params_.terrain.enabled) {
            h.add_i32(c.run_ticks);
        }
    }
    for (const auto [e, p] : registry.view<const Projectile>().each()) {
        h.add_u32(entt::to_integral(e));
        h.add_fixed(p.pos.x);
        h.add_fixed(p.pos.y);
        h.add_fixed(p.dest.x);
        h.add_fixed(p.dest.y);
        h.add_u32(entt::to_integral(p.attacker));
        h.add_u32(entt::to_integral(p.intended));
        h.add_u32(p.owner);
        h.add_i32(p.attack_percent);
    }
}

}  // namespace rts::sim
