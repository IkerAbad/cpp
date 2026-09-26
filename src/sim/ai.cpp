#include "sim/ai.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>

#include "sim/state_hash.hpp"

namespace rts::sim {

namespace {

constexpr std::int32_t kPercent = 100;

TileCoord tile_of(const Position& p) noexcept {
    return {p.x.floor_to_int(), p.y.floor_to_int()};
}

TileCoord center_of(const Footprint& f) noexcept {
    return {f.origin.x + f.size / 2, f.origin.y + f.size / 2};
}

std::int32_t chebyshev(TileCoord a, TileCoord b) noexcept {
    return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y));
}

bool affordable(const Stock& stock, const Stock& cost) noexcept {
    for (std::size_t r = 0; r < kResourceCount; ++r) {
        if (stock[r] < cost[r]) {
            return false;
        }
    }
    return true;
}

void spend(Stock& stock, const Stock& cost) noexcept {
    for (std::size_t r = 0; r < kResourceCount; ++r) {
        stock[r] -= cost[r];
    }
}

std::vector<std::uint32_t> ids(const std::vector<entt::entity>& es) {
    std::vector<std::uint32_t> out;
    out.reserve(es.size());
    for (const entt::entity e : es) {
        out.push_back(entt::to_integral(e));
    }
    return out;
}

std::optional<TileCoord> nearest(const std::vector<TileCoord>& targets, TileCoord from) {
    std::optional<TileCoord> best;
    std::int32_t best_d = std::numeric_limits<std::int32_t>::max();
    for (const TileCoord t : targets) {
        const std::int32_t d = octile_distance(from, t);
        if (d < best_d) {
            best_d = d;
            best = t;
        }
    }
    return best;
}

// --- Percepción ---------------------------------------------------------------------
// Lo que el jugador de la IA sabe del mundo, rehecho en cada decisión. Es el único
// sitio que decide qué ve la IA: cuando haya niebla de guerra, se filtrará aquí con la
// misma visibilidad que tiene un jugador humano (ni ventaja ni desventaja).
struct AiView {
    bool has_anything = false;
    TileCoord base;
    std::optional<entt::entity> town_center;  // el que produce aldeanos
    bool town_center_busy = false;
    std::vector<entt::entity> workers;
    std::vector<entt::entity> idle_workers;
    std::array<std::int32_t, kResourceCount> gatherers{};
    std::vector<entt::entity> army;
    std::vector<entt::entity> idle_army;
    std::optional<entt::entity> barracks;      // cuartel propio (terminado o no)
    bool barracks_complete = false;
    std::int32_t barracks_queue = 0;
    bool house_in_progress = false;
    bool farm_in_progress = false;
    std::vector<std::pair<entt::entity, std::int32_t>> unfinished;  // edificio en obra, constructores
    std::vector<std::pair<Footprint, std::uint8_t>> dropoffs;       // huella, recursos que acepta
    std::uint8_t dropoff_building = 0;         // bit por recurso: hay un almacén de ese tipo en obra
    std::vector<TileCoord> threats;            // enemigos cerca de la base
    std::vector<TileCoord> enemy_buildings;
    std::vector<TileCoord> enemy_units;
    bool threatened = false;
};

// Una decisión de un jugador: lo que sabe, lo que puede gastar y las órdenes que da.
// Los módulos comparten el presupuesto: lo que uno gasta, el siguiente ya no lo tiene.
struct Decision {
    const entt::registry& registry;
    const EconomySystem& economy;
    const PassGrid& grid;
    const AiParams& params;
    const AiProfile& profile;
    AiPlayerState& ai;
    Tick tick;
    std::vector<Command>& out;
    const PlayerState& ps;
    AiView v;
    Stock budget{};
    std::int32_t population = 0;

    [[nodiscard]] PlayerId me() const noexcept { return ai.player; }
    [[nodiscard]] const EconomyCatalog& catalog() const noexcept { return economy.catalog(); }

    [[nodiscard]] Command order(CommandType type) const {
        Command c;
        c.tick = tick;
        c.player = me();
        c.type = type;
        return c;
    }

    // Sitio para un edificio cerca de un punto: anillos crecientes de orígenes; la
    // huella más gap casillas alrededor debe estar libre (deja pasillos).
    [[nodiscard]] std::optional<TileCoord> site_near(TileCoord center, BuildingTypeId type) const {
        const std::int32_t size = catalog().buildings[type].size;
        const std::int32_t gap = profile.build_gap_tiles;
        const TileCoord want{center.x - size / 2, center.y - size / 2};
        for (std::int32_t r = 0; r <= profile.build_search_radius_tiles; ++r) {
            for (std::int32_t y = want.y - r; y <= want.y + r; ++y) {
                for (std::int32_t x = want.x - r; x <= want.x + r; ++x) {
                    if (chebyshev({x, y}, want) != r) {
                        continue;
                    }
                    if (economy.can_place(registry, grid, size + 2 * gap, {x - gap, y - gap})) {
                        return TileCoord{x, y};
                    }
                }
            }
        }
        return std::nullopt;
    }

    // Constructores: primero ociosos; si faltan, recolectores (los últimos de la lista).
    std::vector<entt::entity> take_builders(std::int32_t count) {
        std::vector<entt::entity> chosen;
        while (std::cmp_less(chosen.size(), count) && !v.idle_workers.empty()) {
            chosen.push_back(v.idle_workers.back());
            v.idle_workers.pop_back();
        }
        for (auto it = v.workers.rbegin(); it != v.workers.rend() && std::cmp_less(chosen.size(), count); ++it) {
            if (std::ranges::find(chosen, *it) == chosen.end() &&
                registry.get<Worker>(*it).task != WorkerTask::Build) {
                chosen.push_back(*it);
            }
        }
        return chosen;
    }

    bool place(BuildingTypeId type, TileCoord near) {
        const BuildingType& bt = catalog().buildings[type];
        if (!affordable(budget, bt.cost)) {
            return false;
        }
        const auto site = site_near(near, type);
        const auto builders = take_builders(profile.builders);
        if (!site || builders.empty()) {
            return false;
        }
        Command c = order(CommandType::Place);
        c.kind = type;
        c.target = *site;
        c.units = ids(builders);
        out.push_back(std::move(c));
        spend(budget, bt.cost);
        return true;
    }
};

// Rellena d.v. Devuelve false si el jugador ya no tiene nada (derrotado).
bool perceive(Decision& d) {
    AiView& v = d.v;
    const entt::registry& registry = d.registry;
    const AiParams& params = d.params;
    const PlayerId me = d.me();

    // Edificios.
    const auto buildings = registry.view<const Building, const Footprint, const Owner>();
    std::optional<TileCoord> first_building;
    for (const entt::entity e : buildings) {
        const Building& b = buildings.get<const Building>(e);
        const Footprint& f = buildings.get<const Footprint>(e);
        if (buildings.get<const Owner>(e).player != me) {
            v.enemy_buildings.push_back(center_of(f));
            continue;
        }
        v.has_anything = true;
        if (!first_building) {
            first_building = center_of(f);
        }
        const BuildingType& bt = d.catalog().buildings[b.type];
        const bool trains_workers = std::ranges::find(bt.trains, params.worker_type) != bt.trains.end();
        const ProductionQueue* q = registry.try_get<ProductionQueue>(e);
        if (trains_workers && !v.town_center && b.complete) {
            v.town_center = e;
            v.town_center_busy = q != nullptr && !q->items.empty();
        }
        if (b.type == params.barracks && !v.barracks) {
            v.barracks = e;
            v.barracks_complete = b.complete;
            v.barracks_queue = q != nullptr ? static_cast<std::int32_t>(q->items.size()) : 0;
        }
        if (!b.complete) {
            v.unfinished.emplace_back(e, 0);
            v.house_in_progress = v.house_in_progress || b.type == params.house;
            v.farm_in_progress = v.farm_in_progress || b.type == params.farm;
            for (std::size_t r = 0; r < kResourceCount; ++r) {
                if (b.type == params.dropoff[r]) {
                    v.dropoff_building = static_cast<std::uint8_t>(v.dropoff_building | (1U << r));
                }
            }
        }
        if (bt.accepts != 0) {
            v.dropoffs.emplace_back(f, bt.accepts);
        }
    }

    // Unidades.
    const auto units = registry.view<const Unit, const Position, const Owner>();
    std::optional<TileCoord> first_unit;
    for (const entt::entity e : units) {
        const TileCoord t = tile_of(units.get<const Position>(e));
        if (units.get<const Owner>(e).player != me) {
            v.enemy_units.push_back(t);
            continue;
        }
        v.has_anything = true;
        if (!first_unit) {
            first_unit = t;
        }
        const MoveGoal* goal = registry.try_get<MoveGoal>(e);
        const bool still = goal == nullptr || goal->arrived;
        if (const Worker* w = registry.try_get<Worker>(e)) {
            v.workers.push_back(e);
            if (w->task == WorkerTask::Idle && still) {
                v.idle_workers.push_back(e);
            } else if (w->task == WorkerTask::Gather || w->task == WorkerTask::Deliver) {
                ++v.gatherers[resource_index(w->gather_kind)];
            } else if (w->task == WorkerTask::Build) {
                for (auto& [b, n] : v.unfinished) {
                    n += b == w->building ? 1 : 0;
                }
            }
            continue;
        }
        v.army.push_back(e);
        const Combatant* c = registry.try_get<Combatant>(e);
        if (still && (c == nullptr || (c->target == entt::null && !c->attack_move))) {
            v.idle_army.push_back(e);
        }
    }
    if (!v.has_anything) {
        return false;
    }
    v.base = v.town_center ? center_of(registry.get<Footprint>(*v.town_center))
                           : (first_building ? *first_building : *first_unit);
    for (const TileCoord t : v.enemy_units) {
        if (chebyshev(t, v.base) <= d.profile.defend_radius_tiles) {
            v.threats.push_back(t);
        }
    }
    v.threatened = !v.threats.empty();
    return true;
}

// --- Módulos --------------------------------------------------------------------

// El ejército ocioso sale a por los enemigos cercanos; los aldeanos junto a ellos se
// refugian en la base (los demás módulos no los reasignan mientras dure la amenaza).
void defend(Decision& d) {
    AiView& v = d.v;
    if (!v.threatened) {
        return;
    }
    if (!v.idle_army.empty()) {
        Command c = d.order(CommandType::AttackMove);
        c.target = *nearest(v.threats, v.base);
        c.units = ids(v.idle_army);
        d.out.push_back(std::move(c));
        v.idle_army.clear();
    }
    std::vector<entt::entity> flee;
    for (const entt::entity w : v.workers) {
        const TileCoord t = tile_of(d.registry.get<Position>(w));
        const auto enemy = nearest(v.threats, t);
        if (enemy && chebyshev(*enemy, t) <= d.profile.flee_enemy_tiles && chebyshev(t, v.base) > d.profile.safe_base_tiles) {
            flee.push_back(w);
        }
    }
    if (!flee.empty()) {
        Command c = d.order(CommandType::Move);
        c.target = v.base;
        c.units = ids(flee);
        d.out.push_back(std::move(c));
    }
}

// Aldeanos hasta el objetivo, de uno en uno.
void villagers(Decision& d) {
    const AiView& v = d.v;
    if (!v.town_center || v.town_center_busy || !std::cmp_less(v.workers.size(), d.profile.villager_target) ||
        d.population >= d.ps.population_cap) {
        return;
    }
    const Stock& cost = d.catalog().units[d.params.worker_type].cost;
    if (affordable(d.budget, cost)) {
        Command c = d.order(CommandType::Train);
        c.object = entt::to_integral(*v.town_center);
        c.kind = d.params.worker_type;
        d.out.push_back(std::move(c));
        spend(d.budget, cost);
        ++d.population;
    }
}

// Casa antes de quedarse sin plazas.
void houses(Decision& d) {
    if (!d.v.house_in_progress && d.ps.population_cap < d.economy.params().max_population &&
        d.ps.population_cap - d.population <= d.profile.house_margin) {
        d.place(d.params.house, d.v.base);
    }
}

// Cuartel al llegar a cierto número de aldeanos.
void barracks(Decision& d) {
    if (!d.v.barracks && std::cmp_greater_equal(d.v.workers.size(), d.profile.barracks_at_villagers)) {
        d.place(d.params.barracks, d.v.base);
    }
}

// La comida natural se agota. Tantas granjas como pidan los recolectores de comida
// deseados, contando los nodos de comida explotables cerca de la base.
void farms(Decision& d) {
    const AiView& v = d.v;
    const auto food = resource_index(Resource::Food);
    if (v.threatened || v.farm_in_progress || d.profile.gather_percent[food] <= 0) {
        return;
    }
    std::int32_t food_near = 0;
    const auto nodes = d.registry.view<const ResourceNode, const Footprint>();
    for (const entt::entity e : nodes) {
        if (nodes.get<const ResourceNode>(e).kind == Resource::Food &&
            EconomySystem::can_gather(d.registry, e, d.me()) &&
            chebyshev(nodes.get<const Footprint>(e).origin, v.base) <= d.profile.dropoff_distance_tiles) {
            ++food_near;
        }
    }
    const auto wanted_gatherers =
        d.profile.gather_percent[food] * static_cast<std::int32_t>(v.workers.size()) / kPercent;
    const std::int32_t per_farm = std::max(d.profile.gatherers_per_farm, 1);
    const std::int32_t wanted_farms = (wanted_gatherers + d.profile.gatherers_per_farm - 1) / per_farm;
    if (food_near < wanted_farms) {
        d.place(d.params.farm, v.base);
    }
}

// Almacenes junto a recursos lejanos que ya se explotan: uno nuevo por decisión.
void dropoffs(Decision& d) {
    const AiView& v = d.v;
    if (v.threatened) {
        return;
    }
    for (std::size_t r = 0; r < kResourceCount; ++r) {
        if (v.gatherers[r] < d.profile.dropoff_min_gatherers || (v.dropoff_building & (1U << r)) != 0) {
            continue;
        }
        // El nodo de ese recurso más cercano a la base.
        std::optional<TileCoord> node;
        std::int32_t best = std::numeric_limits<std::int32_t>::max();
        const auto nodes = d.registry.view<const ResourceNode, const Footprint>();
        for (const entt::entity e : nodes) {
            const Footprint& f = nodes.get<const Footprint>(e);
            if (resource_index(nodes.get<const ResourceNode>(e).kind) == r) {
                const std::int32_t dist = octile_distance(v.base, f.origin);
                if (dist < best) {
                    best = dist;
                    node = f.origin;
                }
            }
        }
        if (!node) {
            continue;
        }
        const bool served = std::ranges::any_of(v.dropoffs, [&](const auto& dp) {
            return (dp.second & (1U << r)) != 0 &&
                   chebyshev(center_of(dp.first), *node) <= d.profile.dropoff_distance_tiles;
        });
        if (!served) {
            d.place(d.params.dropoff[r], *node);
            return;
        }
    }
}

// Obras sin constructores suficientes.
void builders(Decision& d) {
    AiView& v = d.v;
    for (const auto& [b, n] : v.unfinished) {
        const std::int32_t missing = d.profile.builders - n;
        if (missing <= 0 || v.idle_workers.empty()) {
            continue;
        }
        std::vector<entt::entity> chosen;
        while (std::cmp_less(chosen.size(), missing) && !v.idle_workers.empty()) {
            chosen.push_back(v.idle_workers.back());
            v.idle_workers.pop_back();
        }
        Command c = d.order(CommandType::Build);
        c.object = entt::to_integral(b);
        c.units = ids(chosen);
        d.out.push_back(std::move(c));
    }
}

// Aldeanos ociosos al recurso con más déficit respecto al reparto deseado, al nodo más
// cercano de ese recurso (la economía reparte si está lleno).
void gather(Decision& d) {
    AiView& v = d.v;
    if (v.threatened) {
        return;
    }
    // Solo recursos que quedan en el mapa (las bayas se agotan).
    std::array<bool, kResourceCount> available{};
    const auto all_nodes = d.registry.view<const ResourceNode>();
    for (const entt::entity e : all_nodes) {
        if (EconomySystem::can_gather(d.registry, e, d.me())) {
            available[resource_index(all_nodes.get<const ResourceNode>(e).kind)] = true;
        }
    }
    const auto total = static_cast<std::int32_t>(v.workers.size());
    for (const entt::entity w : v.idle_workers) {
        std::size_t pick = 0;
        std::int32_t worst = std::numeric_limits<std::int32_t>::max();
        for (std::size_t r = 0; r < kResourceCount; ++r) {
            // Déficit con signo: cuántos le faltan (en centésimas) para su porcentaje.
            const std::int32_t have = v.gatherers[r] * kPercent;
            const std::int32_t want = d.profile.gather_percent[r] * total;
            const std::int32_t surplus = have - want;
            if (d.profile.gather_percent[r] > 0 && available[r] && surplus < worst) {
                worst = surplus;
                pick = r;
            }
        }
        const TileCoord from = tile_of(d.registry.get<Position>(w));
        std::optional<entt::entity> node;
        std::int32_t best = std::numeric_limits<std::int32_t>::max();
        const auto nodes = d.registry.view<const ResourceNode, const Footprint>();
        for (const entt::entity e : nodes) {
            if (resource_index(nodes.get<const ResourceNode>(e).kind) == pick &&
                EconomySystem::can_gather(d.registry, e, d.me())) {
                const std::int32_t dist = octile_distance(from, nodes.get<const Footprint>(e).origin);
                if (dist < best) {
                    best = dist;
                    node = e;
                }
            }
        }
        if (!node) {
            continue;
        }
        Command c = d.order(CommandType::Gather);
        c.object = entt::to_integral(*node);
        c.units = {entt::to_integral(w)};
        d.out.push_back(std::move(c));
        ++v.gatherers[pick];
    }
}

// El cuartel entrena en ciclo mientras haya plazas: el siguiente del ciclo que se
// pueda pagar.
void army(Decision& d) {
    const AiView& v = d.v;
    const auto& cycle = d.profile.army;
    if (!v.barracks || !v.barracks_complete || v.barracks_queue >= d.profile.barracks_queue || cycle.empty() ||
        d.population >= d.ps.population_cap) {
        return;
    }
    const auto n = static_cast<std::int32_t>(cycle.size());
    for (std::int32_t k = 0; k < n; ++k) {
        const UnitTypeId type = cycle[static_cast<std::size_t>((d.ai.army_cycle + k) % n)];
        const Stock& cost = d.catalog().units[type].cost;
        if (affordable(d.budget, cost)) {
            Command c = d.order(CommandType::Train);
            c.object = entt::to_integral(*v.barracks);
            c.kind = type;
            d.out.push_back(std::move(c));
            spend(d.budget, cost);
            d.ai.army_cycle = (d.ai.army_cycle + k + 1) % n;
            return;
        }
    }
}

// Con bastantes tropas ociosas en casa, ataque-movimiento contra el edificio enemigo
// más cercano (o la unidad, si no le quedan edificios). Cada oleada, mayor.
void attack(Decision& d) {
    const AiView& v = d.v;
    if (v.threatened || !std::cmp_greater_equal(v.idle_army.size(), d.ai.wave_size)) {
        return;
    }
    auto target = nearest(v.enemy_buildings, v.base);
    if (!target) {
        target = nearest(v.enemy_units, v.base);
    }
    if (target) {
        Command c = d.order(CommandType::AttackMove);
        c.target = *target;
        c.units = ids(v.idle_army);
        d.out.push_back(std::move(c));
        d.ai.wave_size += d.profile.wave_growth;
        ++d.ai.waves_sent;
    }
}

using BehaviorFn = void (*)(Decision&);
constexpr std::array<BehaviorFn, static_cast<std::size_t>(AiBehavior::Count)> kBehaviors{
    defend, villagers, houses, barracks, farms, dropoffs, builders, gather, army, attack,
};

}  // namespace

AiSystem::AiSystem(const AiParams& params, const std::vector<AiSeat>& seats) : params_(params) {
    for (const AiSeat& seat : seats) {
        AiPlayerState s;
        s.player = seat.player;
        s.wave_size = params_.profiles[seat.profile].first_wave;
        players_.push_back(s);
        profiles_.push_back(seat.profile);
    }
}

void AiSystem::think(const entt::registry& registry, const EconomySystem& economy, const PassGrid& grid, Tick tick,
                     std::vector<Command>& out) {
    const auto interval = static_cast<std::uint32_t>(std::max(params_.think_interval_ticks, 1));
    for (std::size_t i = 0; i < players_.size(); ++i) {
        AiPlayerState& ai = players_[i];
        // Repartidos: el jugador p decide en los ticks con tick % intervalo == p % intervalo.
        if ((tick + interval - static_cast<std::uint32_t>(ai.player) % interval) % interval != 0) {
            continue;
        }
        const PlayerState& ps = economy.players()[ai.player];
        Decision d{registry, economy, grid, params_, params_.profiles[profiles_[i]], ai, tick, out, ps, {}, ps.stock,
                   ps.population};
        if (!perceive(d)) {
            continue;  // derrotado
        }
        for (const AiBehavior b : d.profile.behaviors) {
            kBehaviors[static_cast<std::size_t>(b)](d);
        }
    }
}

void AiSystem::hash_into(StateHasher& h) const {
    h.add_u64(players_.size());
    for (const AiPlayerState& ai : players_) {
        h.add_u32(ai.player);
        h.add_i32(ai.wave_size);
        h.add_i32(ai.army_cycle);
        h.add_i32(ai.waves_sent);
    }
}

}  // namespace rts::sim
