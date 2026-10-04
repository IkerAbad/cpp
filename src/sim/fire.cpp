#include "sim/fire.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <utility>

#include "sim/state_hash.hpp"

namespace rts::sim {

namespace {

constexpr std::int32_t kMilli = 1000;

}  // namespace

FireSystem::FireSystem(const FireParams& params, std::vector<UnitType> units, std::vector<BuildingType> buildings)
    : params_(params), units_(std::move(units)), buildings_(std::move(buildings)) {
    assert(params_.max_intensity > 0);
}

void FireSystem::apply(entt::registry& registry, MovementSystem& movement, const Command& command,
                       std::span<const entt::entity> units, std::uint32_t& next_order_id, Tick tick) {
    if (command.type == CommandType::SetStance) {
        return;  // la postura no interrumpe nada
    }
    if (command.type != CommandType::Extinguish) {
        for (const entt::entity e : units) {
            registry.remove<Extinguisher>(e);
        }
        return;
    }
    const auto building = static_cast<entt::entity>(command.object);
    if (command.object == kNoObject || !registry.valid(building) ||
        !registry.all_of<Building, Owner, Footprint>(building) ||
        registry.get<Owner>(building).player != command.player) {
        return;
    }
    const std::uint32_t order = next_order_id++;
    movement.order_move(registry, units, center_of(registry.get<Footprint>(building)), order, tick);
    for (const entt::entity e : units) {
        registry.emplace_or_replace<Extinguisher>(e, building, order);
    }
}

void FireSystem::add_heat(entt::entity building, std::int32_t amount) {
    if (amount != 0) {
        heat_.push_back({building, amount});
    }
}

void FireSystem::update_extinguishers(entt::registry& registry, MovementSystem& movement,
                                      std::uint32_t& next_order_id, Tick tick) {
    scratch_.clear();
    for (const entt::entity e : registry.view<Extinguisher>()) {
        scratch_.push_back(e);
    }
    for (const entt::entity e : scratch_) {
        Extinguisher& x = registry.get<Extinguisher>(e);
        if (!registry.valid(x.building) || !registry.all_of<Fire, Footprint>(x.building)) {
            registry.remove<Extinguisher>(e);  // apagado o caído: tarea terminada
            continue;
        }
        const Footprint& f = registry.get<Footprint>(x.building);
        const Position& p = registry.get<Position>(e);
        const Fixed reach = registry.get<Unit>(e).radius + params_.extinguish_reach;
        if (distance_sq_to(f, {p.x, p.y}) <= mul_wide(reach, reach)) {
            // Al alcance: se detiene si iba en su propio desplazamiento y apaga.
            const MoveGoal* g = registry.try_get<MoveGoal>(e);
            if (g != nullptr && g->order_id == x.move_order) {
                registry.remove<MoveGoal, PathFollow>(e);
            }
            x.move_order = 0;
            add_heat(x.building, -units_[registry.get<Unit>(e).type].combat.extinguish);
            continue;
        }
        const MoveGoal* g = registry.try_get<MoveGoal>(e);
        if (g == nullptr || g->order_id != x.move_order || g->arrived) {
            // Sin desplazamiento propio en curso (llegó a un sitio sin alcance o se lo
            // quitaron): vuelve a acercarse a la casilla de la huella más próxima.
            const TileCoord near = clamp_to(f, tile_of(p));
            x.move_order = next_order_id++;
            const std::array<entt::entity, 1> one{e};
            movement.order_move(registry, one, near, x.move_order, tick);
        }
    }
}

void FireSystem::spread_from(const entt::registry& registry, const EconomySystem& economy, entt::entity building) {
    const Footprint& f = registry.get<Footprint>(building);
    const std::int32_t gap = params_.spread_gap_tiles;
    // Recorrido fijo del anillo alrededor de la huella; cada vecino una sola vez.
    std::vector<entt::entity>& seen = spread_seen_;
    seen.clear();
    // Vecino: con gap casillas libres o menos entre su huella y la de este edificio.
    for (std::int32_t y = f.origin.y - gap - 1; y <= f.origin.y + f.size + gap; ++y) {
        for (std::int32_t x = f.origin.x - gap - 1; x <= f.origin.x + f.size + gap; ++x) {
            const entt::entity o = economy.occupant({x, y});
            if (o == entt::null || o == building || std::ranges::find(seen, o) != seen.end()) {
                continue;
            }
            seen.push_back(o);
            const Building* b = registry.try_get<Building>(o);
            if (b != nullptr && buildings_[b->type].material == Material::Wood) {
                add_heat(o, params_.spread_per_tick);
            }
        }
    }
}

void FireSystem::update(entt::registry& registry, MovementSystem& movement, EconomySystem& economy,
                        std::uint32_t& next_order_id, Tick tick) {
    stats_ = FireTickStats{};
    update_extinguishers(registry, movement, next_order_id, tick);

    // 1. Propagación desde los fuegos muy vivos (sobre el estado del principio del tick).
    for (const auto [e, fire] : registry.view<const Fire>().each()) {
        if (fire.intensity >= params_.spread_intensity) {
            spread_from(registry, economy, e);
        }
    }

    // 2. Aportes del tick: golpes incendiarios, apagado y propagación. Un aporte
    //    negativo sin fuego no hace nada; uno positivo enciende el edificio.
    for (const Heat& h : heat_) {
        if (!registry.valid(h.building) || !registry.all_of<Building, Health>(h.building)) {
            continue;
        }
        Fire* fire = registry.try_get<Fire>(h.building);
        if (fire == nullptr) {
            if (h.amount <= 0) {
                continue;
            }
            fire = &registry.emplace<Fire>(h.building);
            ++stats_.ignited;
        }
        fire->intensity = std::clamp(fire->intensity + h.amount, 0, params_.max_intensity);
    }
    heat_.clear();

    // 3. Evolución y quema.
    scratch_.clear();
    std::vector<entt::entity> fallen;
    for (const entt::entity e : registry.view<Fire>()) {
        scratch_.push_back(e);
    }
    for (const entt::entity e : scratch_) {
        Fire& fire = registry.get<Fire>(e);
        Building& b = registry.get<Building>(e);
        Health& health = registry.get<Health>(e);
        const BuildingType& bt = buildings_[b.type];
        const bool stone = bt.material == Material::Stone;
        const bool fuel = !(stone && b.burned);  // piedra ya quemada: no queda qué arder
        if (fuel && fire.intensity >= params_.sustain_intensity) {
            fire.intensity += stone ? params_.growth_stone_per_tick : params_.growth_wood_per_tick;
        } else {
            fire.intensity -= params_.decay_per_tick;
        }
        fire.intensity = std::clamp(fire.intensity, 0, params_.max_intensity);
        if (fire.intensity == 0) {
            registry.remove<Fire>(e);
            continue;
        }
        ++stats_.burning;
        if (!fuel) {
            continue;
        }
        const std::int32_t rate = stone ? params_.burn_stone_milli_per_tick : params_.burn_wood_milli_per_tick;
        fire.burn_acc += static_cast<std::int32_t>(std::int64_t{fire.intensity} * rate / params_.max_intensity);
        const std::int32_t loss = fire.burn_acc / kMilli;
        fire.burn_acc %= kMilli;
        if (stone) {
            const std::int32_t floor = health.max_hp * params_.stone_floor_percent / kPercent;
            if (health.hp - loss <= floor) {
                health.hp = std::min(health.hp, floor);
                b.burned = true;  // tejado e interior consumidos; los muros siguen en pie
                ++stats_.gutted;
            } else {
                health.hp -= loss;
            }
        } else {
            health.hp -= loss;
            if (health.hp <= 0) {
                fallen.push_back(e);
            }
        }
    }
    for (const entt::entity e : fallen) {
        economy.remove_building(registry, movement, e);
        ++stats_.burned_down;
    }
}

void FireSystem::hash_into(StateHasher& h, const entt::registry& registry) const {
    for (const auto [e, fire] : registry.view<const Fire>().each()) {
        h.add_u32(entt::to_integral(e));
        h.add_i32(fire.intensity);
        h.add_i32(fire.burn_acc);
    }
    for (const auto [e, x] : registry.view<const Extinguisher>().each()) {
        h.add_u32(entt::to_integral(e));
        h.add_u32(entt::to_integral(x.building));
        h.add_u32(x.move_order);
    }
    h.add_u64(heat_.size());
}

}  // namespace rts::sim
