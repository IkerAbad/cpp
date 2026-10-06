#include "sim/supply.hpp"

#include <algorithm>
#include <array>
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

std::int32_t total(const Stock& s) noexcept {
    std::int32_t sum = 0;
    for (const std::int32_t v : s) {
        sum += v;
    }
    return sum;
}

// Primera fuente que puede pagar cost; la paga. false si ninguna puede.
bool pay_from(std::span<Stock* const> payers, const Stock& cost) noexcept {
    for (Stock* p : payers) {
        if (affordable(*p, cost)) {
            pay(*p, cost);
            return true;
        }
    }
    return false;
}

}  // namespace

SupplySystem::SupplySystem(const SupplyParams& params, std::vector<UnitType> units,
                           std::vector<BuildingType> buildings)
    : params_(params), units_(std::move(units)), buildings_(std::move(buildings)) {
    assert(params_.resupply_interval_ticks > 0);
    assert(params_.starve_hp_interval_ticks > 0);
}

bool SupplySystem::is_home(const entt::registry& registry, entt::entity b, PlayerId player) const {
    if (b == entt::null || !registry.valid(b) || !registry.all_of<Building, Owner, Footprint>(b)) {
        return false;
    }
    const Building& bd = registry.get<Building>(b);
    const BuildingType& bt = buildings_[bd.type];
    return registry.get<Owner>(b).player == player && bd.working() && bt.supplies && bt.store_capacity == 0;
}

bool SupplySystem::is_camp(const entt::registry& registry, entt::entity b, PlayerId player) const {
    if (b == entt::null || !registry.valid(b) || !registry.all_of<Building, Owner, Footprint, SupplyStore>(b)) {
        return false;
    }
    const Building& bd = registry.get<Building>(b);
    return registry.get<Owner>(b).player == player && bd.working() && buildings_[bd.type].store_capacity > 0;
}

entt::entity SupplySystem::nearest_home(const entt::registry& registry, PlayerId player, FVec2 pos) const {
    entt::entity best = entt::null;
    std::int64_t best_d = std::numeric_limits<std::int64_t>::max();
    for (const entt::entity b : registry.view<const Building, const Owner, const Footprint>()) {
        if (!is_home(registry, b, player)) {
            continue;
        }
        const std::int64_t d = distance_sq_to(registry.get<Footprint>(b), pos);
        if (d < best_d) {
            best_d = d;
            best = b;
        }
    }
    return best;
}

void SupplySystem::apply(entt::registry& registry, MovementSystem& /*movement*/, const Command& command,
                         std::span<const entt::entity> units, std::uint32_t& /*next_order_id*/, Tick /*tick*/) {
    if (command.type == CommandType::SetStance) {
        return;  // la postura no interrumpe nada
    }
    const auto object = static_cast<entt::entity>(command.object);
    const bool camp = command.type == CommandType::Convoy && is_camp(registry, object, command.player);
    const bool home = command.type == CommandType::Convoy && is_home(registry, object, command.player);
    for (const entt::entity e : units) {
        Carrier* c = registry.try_get<Carrier>(e);
        if (c == nullptr) {
            continue;
        }
        c->timer = 0;
        c->move_order = 0;
        if (camp) {
            c->camp = object;
            c->home = entt::null;
            c->task = total(c->load) > 0 ? ConvoyTask::Unload : ConvoyTask::Load;
        } else if (home) {
            c->home = object;
            c->camp = entt::null;
            c->task = ConvoyTask::Load;
        } else {
            c->task = ConvoyTask::Idle;  // otra orden: deja la ruta y conserva la carga
        }
    }
}

bool SupplySystem::approach(entt::registry& registry, MovementSystem& movement, entt::entity e, Carrier& c,
                            const Footprint& f, std::uint32_t& next_order_id, Tick tick) const {
    const Position& p = registry.get<Position>(e);
    const Fixed reach = registry.get<Unit>(e).radius + params_.convoy_reach;
    const MoveGoal* g = registry.try_get<MoveGoal>(e);
    if (distance_sq_to(f, {p.x, p.y}) <= mul_wide(reach, reach)) {
        if (g != nullptr && g->order_id == c.move_order) {
            registry.remove<MoveGoal, PathFollow>(e);
        }
        c.move_order = 0;
        return true;
    }
    if (g == nullptr || g->order_id != c.move_order || g->arrived) {
        c.move_order = next_order_id++;
        const std::array<entt::entity, 1> one{e};
        movement.order_move(registry, one, clamp_to(f, tile_of(p)), c.move_order, tick);
    }
    return false;
}

void SupplySystem::update_carriers(entt::registry& registry, MovementSystem& movement, EconomySystem& economy,
                                   std::uint32_t& next_order_id, Tick tick) {
    scratch_.clear();
    for (const entt::entity e : registry.view<Carrier>()) {
        scratch_.push_back(e);
    }
    for (const entt::entity e : scratch_) {
        Carrier& c = registry.get<Carrier>(e);
        const PlayerId player = registry.get<Owner>(e).player;
        const Position& p = registry.get<Position>(e);
        if (c.task == ConvoyTask::Load) {
            if (!is_home(registry, c.home, player)) {
                c.home = nearest_home(registry, player, {p.x, p.y});
                c.move_order = 0;
                if (c.home == entt::null) {
                    c.task = ConvoyTask::Idle;  // nada en casa que abastezca
                    continue;
                }
            }
            if (!approach(registry, movement, e, c, registry.get<Footprint>(c.home), next_order_id, tick) ||
                ++c.timer < params_.load_ticks) {
                continue;
            }
            c.timer = 0;
            // Hacia un campamento: lo que le falta para su objetivo de almacén, hasta la
            // capacidad del bagaje. Si no: según el reparto. Siempre con lo que haya en casa.
            const std::int32_t capacity = units_[registry.get<Unit>(e).type].convoy_capacity;
            Stock& stock = economy.player_state(player).stock;
            Stock want{};
            if (is_camp(registry, c.camp, player)) {
                // Sin contar dos veces lo que ya llevan hacia allí otros convoyes.
                const Stock& target = buildings_[registry.get<Building>(c.camp).type].store_target;
                Stock coming = registry.get<SupplyStore>(c.camp).stock;
                for (const auto [o, other] : registry.view<const Carrier>().each()) {
                    if (o != e && other.camp == c.camp && other.task == ConvoyTask::Unload) {
                        for (std::size_t r = 0; r < kResourceCount; ++r) {
                            coming[r] += other.load[r];
                        }
                    }
                }
                for (std::size_t r = 0; r < kResourceCount; ++r) {
                    want[r] = std::max(target[r] - coming[r], 0);
                }
            } else {
                for (std::size_t r = 0; r < kResourceCount; ++r) {
                    want[r] = capacity * params_.convoy_mix[r] / kPercent;
                }
            }
            std::int32_t room = capacity - total(c.load);
            for (std::size_t r = 0; r < kResourceCount; ++r) {
                const std::int32_t take = std::clamp(std::min(want[r] - c.load[r], room), 0, std::max(stock[r], 0));
                stock[r] -= take;
                c.load[r] += take;
                room -= take;
            }
            ++stats_.loaded;
            c.task = is_camp(registry, c.camp, player) ? ConvoyTask::Unload : ConvoyTask::Idle;
        } else if (c.task == ConvoyTask::Unload) {
            if (!is_camp(registry, c.camp, player)) {
                c.camp = entt::null;
                c.task = ConvoyTask::Idle;  // campamento perdido: se queda con la carga
                continue;
            }
            if (!approach(registry, movement, e, c, registry.get<Footprint>(c.camp), next_order_id, tick) ||
                ++c.timer < params_.load_ticks) {
                continue;
            }
            c.timer = 0;
            Stock& store = registry.get<SupplyStore>(c.camp).stock;
            std::int32_t room =
                buildings_[registry.get<Building>(c.camp).type].store_capacity - total(store);
            for (std::size_t r = 0; r < kResourceCount; ++r) {
                const std::int32_t moved = std::clamp(c.load[r], 0, std::max(room, 0));
                c.load[r] -= moved;
                store[r] += moved;
                room -= moved;
            }
            ++stats_.unloaded;
            c.task = ConvoyTask::Load;  // vuelta a casa a por más
        }
    }
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

void SupplySystem::resupply(entt::registry& registry, EconomySystem& economy, entt::entity e, PlayerId player,
                            FVec2 pos, Supply& s, const SupplyStats& st) {
    // Fuentes al alcance, en orden: el edificio (almacén del jugador o del campamento)
    // y después el bagaje cargado cercano. Cada cosa la paga la primera que puede.
    constexpr std::size_t kMaxPayers = 8;
    std::array<Stock*, kMaxPayers> payers{};
    std::size_t n = 0;
    const entt::entity b = source_near(registry, economy, player, pos, units_[registry.get<Unit>(e).type].worker);
    if (b != entt::null) {
        SupplyStore* store = registry.try_get<SupplyStore>(b);
        payers[n++] = store != nullptr ? &store->stock : &economy.player_state(player).stock;
    }
    const Fixed reach = Fixed::from_int(params_.resupply_radius_tiles);
    const std::int64_t reach_sq = mul_wide(reach, reach);
    for (const CarrierSeen& c : carriers_) {
        if (n == kMaxPayers) {
            break;
        }
        if (c.owner == player && length_sq_wide(c.pos - pos) <= reach_sq) {
            payers[n++] = &registry.get<Carrier>(c.entity).load;
        }
    }
    if (n == 0) {
        return;
    }
    const std::span<Stock* const> sources(payers.data(), n);
    if (s.rations < st.rations && pay_from(sources, params_.ration_cost)) {
        ++s.rations;
        ++stats_.rations_issued;
    }
    if (s.ammo < st.ammo && pay_from(sources, st.ammo_cost)) {
        s.ammo = std::min(s.ammo + st.ammo_bundle, st.ammo);
        ++stats_.ammo_issued;
    }
}

bool SupplySystem::forage(entt::registry& registry, MovementSystem& movement, EconomySystem& economy,
                          entt::entity e, PlayerId player, FVec2 pos, Tick tick) {
    const std::int32_t food = std::max(params_.ration_cost[resource_index(Resource::Food)], 1);
    const std::int32_t r = params_.forage_reach_tiles;
    const Fixed reach = Fixed::from_int(r);
    const std::int64_t reach_sq = mul_wide(reach, reach);
    const TileCoord t = tile_of(pos);
    entt::entity farm = entt::null;   // granja enemiga con grano
    entt::entity store = entt::null;  // almacén de comida enemigo (la aldea)
    bool forest = false;
    // Recorrido fijo por filas: a igualdad, el primero encontrado.
    for (std::int32_t y = t.y - r; y <= t.y + r; ++y) {
        for (std::int32_t x = t.x - r; x <= t.x + r; ++x) {
            const entt::entity o = economy.occupant({x, y});
            if (o == entt::null || !registry.all_of<Footprint>(o) ||
                distance_sq_to(registry.get<Footprint>(o), pos) > reach_sq) {
                continue;
            }
            const ResourceNode* n = registry.try_get<ResourceNode>(o);
            const Owner* own = registry.try_get<Owner>(o);
            const bool enemy = own != nullptr && own->player != player;
            if (farm == entt::null && enemy && n != nullptr && n->kind == Resource::Food && n->amount > 0) {
                farm = o;
            } else if (store == entt::null && enemy && registry.all_of<Building>(o) &&
                       registry.get<Building>(o).working() &&
                       (buildings_[registry.get<Building>(o).type].accepts & resource_bit(Resource::Food)) != 0) {
                store = o;
            } else if (n != nullptr && own == nullptr && n->kind == Resource::Wood) {
                forest = true;  // caza y forraje entre los árboles
            }
        }
    }
    const auto taken_from = [&](PlayerId victim) {
        if (victim >= pillaged_from_.size()) {
            pillaged_from_.resize(victim + std::size_t{1}, 0);
        }
        ++pillaged_from_[victim];
        ++stats_.pillaged;
    };
    if (farm != entt::null) {
        ResourceNode& n = registry.get<ResourceNode>(farm);
        n.amount -= std::min(n.amount, food);
        taken_from(registry.get<Owner>(farm).player);
        if (n.amount <= 0) {
            economy.deplete(registry, movement, farm);  // granja vaciada
        }
        return true;
    }
    if (store != entt::null) {
        const PlayerId victim = registry.get<Owner>(store).player;
        std::int32_t& stock = economy.player_state(victim).stock[resource_index(Resource::Food)];
        if (stock >= food) {
            stock -= food;
            taken_from(victim);
            return true;
        }
    }
    if (forest && (entt::to_integral(e) + tick) % static_cast<Tick>(params_.forest_forage_ticks) == 0) {
        ++stats_.foraged;
        return true;
    }
    return false;
}

void SupplySystem::update(entt::registry& registry, MovementSystem& movement, EconomySystem& economy,
                          std::uint32_t& next_order_id, Tick tick) {
    stats_ = SupplyTickStats{};
    dead_.clear();

    // Campamentos terminados: su almacén propio, vacío.
    scratch_.clear();
    for (const auto [e, b] : registry.view<const Building>(entt::exclude<SupplyStore>).each()) {
        if (b.complete && buildings_[b.type].store_capacity > 0) {
            scratch_.push_back(e);
        }
    }
    for (const entt::entity e : scratch_) {
        registry.emplace<SupplyStore>(e);
    }

    update_carriers(registry, movement, economy, next_order_id, tick);
    carriers_.clear();
    for (const auto [e, c, o, p] : registry.view<const Carrier, const Owner, const Position>().each()) {
        if (total(c.load) > 0) {
            carriers_.push_back({e, o.player, {p.x, p.y}});
        }
    }

    const auto interval = static_cast<std::uint32_t>(params_.resupply_interval_ticks);
    const auto view = registry.view<Supply, const Unit, const Owner, const Position, Health>();
    for (const entt::entity e : view) {
        Supply& s = view.get<Supply>(e);
        const SupplyStats& st = units_[view.get<const Unit>(e).type].supply;
        if (const Patient* pt = registry.try_get<Patient>(e); pt != nullptr && pt->admitted) {
            continue;  // ingresado: come lo que le da el puesto médico (sanidad)
        }

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

        // 2. Reabastecimiento junto a una fuente propia.
        if ((s.rations >= st.rations && s.ammo >= st.ammo) || (entt::to_integral(e) + tick) % interval != 0) {
            continue;
        }
        const Position& p = view.get<const Position>(e);
        const PlayerId player = view.get<const Owner>(e).player;
        const std::int32_t before = s.rations;
        resupply(registry, economy, e, player, {p.x, p.y}, s, st);
        // 3. Sin fuente propia, vive del terreno: parada y sin pelear.
        if (params_.forage && s.rations == before && s.rations < st.rations && !units_[view.get<const Unit>(e).type].worker &&
            units_[view.get<const Unit>(e).type].convoy_capacity == 0) {
            const MoveGoal* goal = registry.try_get<MoveGoal>(e);
            const Combatant* c = registry.try_get<Combatant>(e);
            const bool still = goal == nullptr || goal->arrived;
            if (still && (c == nullptr || c->target == entt::null) && forage(registry, movement, economy, e, player, {p.x, p.y}, tick)) {
                ++s.rations;
            }
        }
    }
    for (const entt::entity e : dead_) {
        economy.record_loss(registry.get<Owner>(e).player);
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
    for (const auto [e, c] : registry.view<const Carrier>().each()) {
        h.add_u32(entt::to_integral(e));
        for (const std::int32_t v : c.load) {
            h.add_i32(v);
        }
        h.add_u32(static_cast<std::uint32_t>(c.task));
        h.add_u32(entt::to_integral(c.home));
        h.add_u32(entt::to_integral(c.camp));
        h.add_i32(c.timer);
        h.add_u32(c.move_order);
    }
    for (const auto [e, st] : registry.view<const SupplyStore>().each()) {
        h.add_u32(entt::to_integral(e));
        for (const std::int32_t v : st.stock) {
            h.add_i32(v);
        }
    }
}

}  // namespace rts::sim
