#include "sim/ai.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>

#include "sim/combat.hpp"
#include "sim/fire.hpp"
#include "sim/climb.hpp"
#include "sim/formation.hpp"
#include "sim/garrison.hpp"
#include "sim/state_hash.hpp"

namespace rts::sim {

namespace {

// Escala entera de las estimaciones de combate (fracciones de vida por tick).
constexpr std::int64_t kScale = 1'000'000;
constexpr std::int64_t kScoreScale = 1'000;

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

// Unidad vista: la IA solo usa lo que un jugador ve en pantalla (tipo, vida, posición,
// si está peleando).
struct UnitSeen {
    entt::entity entity = entt::null;
    TileCoord tile;
    UnitTypeId type = 0;
    std::int32_t hp = 0;
    bool armed = false;          // ataca solo (no es aldeano)
    bool carrier = false;        // bagaje (acémila, carreta): se ve qué es
    entt::entity target = entt::null;
    bool attack_move = false;
    // Solo de las propias (el jugador ve las barras de las suyas): lo que le queda de
    // víveres y munición, en % de lo que puede llevar (el menor de los dos).
    std::int32_t supply_percent = kPercent;
    bool needs_rations = false;
    bool needs_ammo = false;
};

std::int64_t cost_sum(const Stock& cost) noexcept {
    std::int64_t sum = 0;
    for (const std::int32_t c : cost) {
        sum += c;
    }
    return sum;
}

// Daño por tick de a contra b, como fracción de la vida de b (escala kScale).
std::int64_t kill_rate(const UnitType& a, const UnitType& b) noexcept {
    const CombatStats& t = b.combat;
    const std::int64_t dmg = hit_damage(a.combat, kPercent, t.armor_melee, t.armor_pierce, t.armor_class);
    return dmg * kScale / (static_cast<std::int64_t>(std::max(a.combat.reload_ticks, 1)) * std::max(t.hp, 1));
}

// Daño por tick de a contra un edificio, como fracción de su vida (escala kScale).
// Lo que una unidad hace por tick a un edificio, como fracción de su vida (escala
// kScale). Asedio: daño. El resto: fuego, que derriba la madera pero no la piedra.
// Los vitales pesan más: su caída decide la partida.
constexpr std::int64_t kVitalWeight = 4;
std::int64_t demolish_rate(const UnitType& a, const BuildingType& b) noexcept {
    std::int64_t per_hit = 0;
    if (a.combat.undermine && b.material == Material::Stone) {
        per_hit = a.combat.attack_melee;  // mina: sin armadura
    } else if (a.combat.siege) {
        per_hit = hit_damage(a.combat, kPercent, b.armor_melee, b.armor_pierce, b.armor_class);
    } else if (b.material == Material::Wood) {
        per_hit = a.combat.ignite;
    }
    const std::int64_t rate =
        per_hit * kScale / (static_cast<std::int64_t>(std::max(a.combat.reload_ticks, 1)) * std::max(b.hp, 1));
    return b.vital ? rate * kVitalWeight : rate;
}

// Fuerza estimada de una unidad: vida por daño por tick (escala kScoreScale).
std::int64_t strength(const UnitType& u, std::int32_t hp) noexcept {
    const std::int64_t attack = u.combat.attack_melee + u.combat.attack_pierce;
    return static_cast<std::int64_t>(hp) * attack * kScoreScale / std::max(u.combat.reload_ticks, 1);
}

// Bagaje propio: dónde está, qué lleva y si está parado sin ruta.
struct CarrierView {
    entt::entity entity = entt::null;
    TileCoord tile;
    Stock load{};
    bool idle = false;
    std::optional<TileCoord> goal;  // destino de su movimiento en curso
};

// --- Percepción ---------------------------------------------------------------------
// Lo que el jugador de la IA sabe del mundo, rehecho en cada decisión. Es el único
// sitio que decide qué ve la IA: cuando haya niebla de guerra, se filtrará aquí con la
// misma visibilidad que tiene un jugador humano (ni ventaja ni desventaja).
struct AiView {
    bool has_anything = false;
    TileCoord base;
    std::optional<entt::entity> town_center;  // el que produce aldeanos
    std::int32_t town_center_queue = 0;
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
    std::vector<UnitSeen> soldiers;  // ejército propio con su estado de combate
    std::vector<UnitSeen> enemies;   // todas las unidades enemigas
    std::vector<BuildingTypeId> enemy_building_types;
    std::vector<entt::entity> enemy_building_entities;
    std::vector<std::int32_t> own_type_count;  // edificios propios por tipo (también en obra)
    // Edificios propios que producen (terminados y en uso) con su cola.
    std::vector<std::pair<entt::entity, std::int32_t>> trainers;
    std::vector<entt::entity> own_fires;  // edificios propios en llamas
    std::vector<std::pair<Footprint, entt::entity>> supply_sources;  // edificios propios que abastecen (en uso)
    std::vector<TileCoord> camps;          // campamentos propios (también en obra)
    std::optional<entt::entity> camp;      // el primer campamento propio terminado
    std::vector<CarrierView> carriers;     // bagaje propio
    // Estimación del ejército enemigo con niebla: lo visto y lo recordado, por tipo, en
    // milésimas de unidad. Sin niebla no se usa (todo está a la vista).
    bool use_estimate = false;
    std::vector<std::int64_t> enemy_estimate_milli;
};

constexpr std::int64_t kMilliUnit = 1000;

// Una decisión de un jugador: lo que sabe, lo que puede gastar y las órdenes que da.
// Los módulos comparten el presupuesto: lo que uno gasta, el siguiente ya no lo tiene.
struct Decision {
    const entt::registry& registry;
    const EconomySystem& economy;
    const PassGrid& grid;
    const AiParams& params;
    const SupplyParams& supply;
    const AiProfile& profile;
    AiPlayerState& ai;
    Tick tick;
    std::vector<Command>& out;
    const PlayerState& ps;
    AiView v;
    Stock budget{};
    std::int32_t population = 0;
    const VisionSystem* vision = nullptr;  // niebla de guerra; null = lo ve todo

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
        if (!affordable(budget, bt.cost) || !economy.meets_requirements(registry, me(), type)) {
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
            // Con niebla, los enemigos salen de lo recordado (abajo), como para un humano.
            if (d.vision == nullptr || !d.vision->enabled()) {
                v.enemy_buildings.push_back(center_of(f));
                v.enemy_building_types.push_back(b.type);
                v.enemy_building_entities.push_back(e);
            }
            continue;
        }
        if (v.own_type_count.empty()) {
            v.own_type_count.assign(d.catalog().buildings.size(), 0);
        }
        ++v.own_type_count[b.type];
        if (registry.all_of<Fire>(e)) {
            v.own_fires.push_back(e);
        }
        v.has_anything = true;
        if (!first_building) {
            first_building = center_of(f);
        }
        const BuildingType& bt = d.catalog().buildings[b.type];
        const bool trains_workers = std::ranges::find(bt.trains, params.worker_type) != bt.trains.end();
        const ProductionQueue* q = registry.try_get<ProductionQueue>(e);
        if (q != nullptr && b.working() && !trains_workers) {
            v.trainers.emplace_back(e, static_cast<std::int32_t>(q->items.size()));
        }
        if (trains_workers && !v.town_center && b.complete) {
            v.town_center = e;
            v.town_center_queue = q != nullptr ? static_cast<std::int32_t>(q->items.size()) : 0;
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
        if (bt.supplies && b.working()) {
            v.supply_sources.emplace_back(f, e);
        }
        if (bt.store_capacity > 0) {
            v.camps.push_back(center_of(f));
            if (b.working() && !v.camp) {
                v.camp = e;
            }
        }
    }

    // Edificios enemigos recordados (lo último que se vio de cada uno).
    if (d.vision != nullptr && d.vision->enabled()) {
        for (const RememberedBuilding& m : d.vision->memory(me)) {
            v.enemy_buildings.push_back(center_of(m.footprint));
            v.enemy_building_types.push_back(m.type);
            v.enemy_building_entities.push_back(m.entity);
        }
    }

    // Unidades.
    const auto units = registry.view<const Unit, const Position, const Owner>();
    std::optional<TileCoord> first_unit;
    for (const entt::entity e : units) {
        const TileCoord t = tile_of(units.get<const Position>(e));
        UnitSeen seen;
        seen.entity = e;
        seen.tile = t;
        seen.type = units.get<const Unit>(e).type;
        const Health* health = registry.try_get<Health>(e);
        seen.hp = health != nullptr ? health->hp : 0;
        seen.armed = registry.try_get<Worker>(e) == nullptr && d.catalog().units[seen.type].combat.auto_attack;
        seen.carrier = d.catalog().units[seen.type].convoy_capacity > 0;
        if (units.get<const Owner>(e).player != me) {
            if (d.vision == nullptr || d.vision->sees_unit(registry, me, e)) {
                v.enemy_units.push_back(t);
                v.enemies.push_back(seen);
            }
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
            if (w->task == WorkerTask::Idle && still && !registry.all_of<Extinguisher>(e)) {
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
        if (const Carrier* cr = registry.try_get<Carrier>(e)) {
            // El bagaje no es ejército: no se manda a pelear.
            CarrierView cv{e, t, cr->load, cr->task == ConvoyTask::Idle, std::nullopt};
            if (goal != nullptr && !goal->arrived) {
                cv.goal = goal->tile;
            }
            v.carriers.push_back(cv);
            continue;
        }
        v.army.push_back(e);
        const Combatant* c = registry.try_get<Combatant>(e);
        if (c != nullptr) {
            seen.target = c->target;
            seen.attack_move = c->attack_move;
        }
        if (const Supply* sp = registry.try_get<Supply>(e)) {
            const SupplyStats& st = d.catalog().units[seen.type].supply;
            if (st.rations > 0) {
                seen.supply_percent = std::min(seen.supply_percent, sp->rations * kPercent / st.rations);
                seen.needs_rations = sp->rations < st.rations;
            }
            if (st.ammo > 0) {
                seen.supply_percent = std::min(seen.supply_percent, sp->ammo * kPercent / st.ammo);
                seen.needs_ammo = sp->ammo < st.ammo;
            }
        }
        v.soldiers.push_back(seen);
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

// ¿Es armado un tipo (ataca solo y no es aldeano)? Igual que UnitSeen::armed.
bool armed_type(const UnitType& u) noexcept {
    return !u.worker && u.combat.auto_attack;
}

// Fuerza armada enemiga estimada: con niebla, lo visto y recordado; si no, lo visible.
std::int64_t enemy_armed_strength(const Decision& d) {
    const auto& types = d.catalog().units;
    std::int64_t total = 0;
    if (d.v.use_estimate) {
        for (std::size_t t = 0; t < d.v.enemy_estimate_milli.size(); ++t) {
            if (armed_type(types[t])) {
                total += strength(types[t], types[t].combat.hp) * d.v.enemy_estimate_milli[t] / kMilliUnit;
            }
        }
        return total;
    }
    for (const UnitSeen& e : d.v.enemies) {
        total += e.armed ? strength(types[e.type], e.hp) : 0;
    }
    return total;
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
    if (!v.town_center || v.town_center_queue >= d.profile.villager_queue || !std::cmp_less(v.workers.size(), d.profile.villager_target) ||
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

// Entrena el tipo del ciclo que mejor rinde contra lo que tiene el enemigo, por unidad
// de coste. Contra un ejército: cuánto de él mata por tick frente a cuánto de él matan.
// Sin unidades armadas enemigas: lo que antes derriba sus edificios (rematar). Todo
// sale de la fórmula de daño y de los datos; sin nada enemigo a la vista, sigue el
// ciclo como el módulo ejercito.
void army_counter(Decision& d) {
    const AiView& v = d.v;
    const auto& cycle = d.profile.army;
    if (cycle.empty() || d.population >= d.ps.population_cap) {
        return;
    }
    // Edificio propio en uso que entrena el tipo u y tiene sitio en la cola.
    const auto trainer_for = [&](UnitTypeId u) -> std::optional<entt::entity> {
        for (const auto& [b, queue] : v.trainers) {
            const auto& trains = d.catalog().buildings[d.registry.get<Building>(b).type].trains;
            if (queue < d.profile.barracks_queue && std::ranges::find(trains, u) != trains.end()) {
                return b;
            }
        }
        return std::nullopt;
    };
    if (std::ranges::none_of(cycle, [&](UnitTypeId u) { return trainer_for(u).has_value(); })) {
        return;
    }
    bool enemy_armed = std::ranges::any_of(v.enemies, &UnitSeen::armed);
    if (v.use_estimate) {
        for (std::size_t t = 0; t < v.enemy_estimate_milli.size(); ++t) {
            enemy_armed = enemy_armed || (v.enemy_estimate_milli[t] > 0 && armed_type(d.catalog().units[t]));
        }
    }
    if (std::cmp_less(v.workers.size(), d.profile.army_min_villagers)) {
        std::int64_t own = 0;
        for (const UnitSeen& s : v.soldiers) {
            own += strength(d.catalog().units[s.type], s.hp);
        }
        const std::int64_t enemy = enemy_armed_strength(d);
        const bool guard_short = v.use_estimate && std::cmp_less(v.soldiers.size(), d.profile.fog_guard_army);
        if (own >= enemy && !guard_short) {
            return;  // economía primero
        }
    }
    if (!enemy_armed && v.enemy_building_types.empty()) {
        if (v.barracks && v.barracks_complete && v.barracks_queue < d.profile.barracks_queue) {
            army(d);
        }
        return;
    }
    const auto& types = d.catalog().units;
    const auto& buildings = d.catalog().buildings;
    // Contra un ejército se entrena lo mejor que se pueda pagar ya. Sin ejército
    // enemigo no hay prisa: se ahorra para lo mejor (el ariete contra la piedra) en vez
    // de gastar en tropas que no derriban nada, siempre que se esté recogiendo lo que
    // falta para pagarlo; si no, nunca llegaría y se entrena lo mejor asequible.
    std::optional<UnitTypeId> best;          // entre todos
    std::int64_t best_score = -1;
    std::optional<UnitTypeId> best_now;      // entre los asequibles ya
    std::int64_t best_now_score = -1;
    for (const UnitTypeId u : cycle) {
        if (!trainer_for(u)) {
            continue;
        }
        std::int64_t offense = 0;
        std::int64_t threat = 0;
        if (enemy_armed && v.use_estimate) {
            // Con niebla: contra lo visto y recordado.
            for (std::size_t t = 0; t < v.enemy_estimate_milli.size(); ++t) {
                const std::int64_t m = v.enemy_estimate_milli[t];
                offense += kill_rate(types[u], types[t]) * m / kMilliUnit;
                threat += armed_type(types[t]) ? kill_rate(types[t], types[u]) * m / kMilliUnit : 0;
            }
        } else if (enemy_armed) {
            for (const UnitSeen& e : v.enemies) {
                offense += kill_rate(types[u], types[e.type]);
                threat += e.armed ? kill_rate(types[e.type], types[u]) : 0;
            }
        } else {
            for (const BuildingTypeId b : v.enemy_building_types) {
                offense += demolish_rate(types[u], buildings[b]);
            }
        }
        const std::int64_t score =
            offense * kScoreScale / (threat + 1) * kScoreScale / std::max<std::int64_t>(cost_sum(types[u].cost), 1);
        if (score > best_score) {
            best_score = score;
            best = u;
        }
        if (affordable(d.budget, types[u].cost) && score > best_now_score) {
            best_now_score = score;
            best_now = u;
        }
    }
    std::optional<UnitTypeId> pick = best_now;
    if (!enemy_armed && best && best != best_now) {
        // ¿Llegará a poder pagar el mejor? Solo si se recoge cada recurso que falta.
        bool reachable = true;
        for (std::size_t r = 0; r < kResourceCount; ++r) {
            reachable = reachable && (d.budget[r] >= types[*best].cost[r] || v.gatherers[r] > 0);
        }
        if (reachable) {
            return;  // ahorra
        }
    }
    if (!pick) {
        return;
    }
    Command c = d.order(CommandType::Train);
    c.object = entt::to_integral(*trainer_for(*pick));
    c.kind = *pick;
    d.out.push_back(std::move(c));
    spend(d.budget, types[*pick].cost);
}

// Taller de asedio en cuanto se cumplen sus requisitos (place los comprueba).
void workshop(Decision& d) {
    const AiView& v = d.v;
    if (!d.params.workshop || v.threatened || v.own_type_count.empty() || v.own_type_count[*d.params.workshop] > 0) {
        return;
    }
    d.place(*d.params.workshop, v.base);
}

// Aldeanos a apagar los edificios propios en llamas: los más cercanos que no estén
// ya apagando, hasta extinguishers_per_fire por edificio.
void extinguish(Decision& d) {
    AiView& v = d.v;
    // Durante una amenaza no se manda a nadie junto a los enemigos (los que huyen
    // siguen huyendo): solo fuegos y aldeanos lejos de ellos.
    const auto near_threat = [&](TileCoord t) {
        return std::ranges::any_of(v.threats, [&](TileCoord e) { return chebyshev(e, t) <= d.profile.flee_enemy_tiles; });
    };
    std::vector<entt::entity> assigned;  // en esta decisión (la orden aún no se ha aplicado)
    for (const entt::entity fire : v.own_fires) {
        const TileCoord at = center_of(d.registry.get<Footprint>(fire));
        if (near_threat(at)) {
            continue;
        }
        std::int32_t already = 0;
        for (const entt::entity w : v.workers) {
            const Extinguisher* x = d.registry.try_get<Extinguisher>(w);
            already += x != nullptr && x->building == fire ? 1 : 0;
        }
        std::vector<std::pair<std::int32_t, entt::entity>> candidates;  // distancia, aldeano
        for (const entt::entity w : v.workers) {
            const TileCoord t = tile_of(d.registry.get<Position>(w));
            if (!d.registry.all_of<Extinguisher>(w) && d.registry.get<Worker>(w).task != WorkerTask::Build &&
                std::ranges::find(assigned, w) == assigned.end() && !near_threat(t)) {
                candidates.emplace_back(octile_distance(t, at), w);
            }
        }
        std::ranges::sort(candidates, [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first < b.first : entt::to_integral(a.second) < entt::to_integral(b.second);
        });
        Command c = d.order(CommandType::Extinguish);
        c.object = entt::to_integral(fire);
        for (std::size_t i = 0; i < candidates.size() && already < d.profile.extinguishers_per_fire; ++i) {
            c.units.push_back(entt::to_integral(candidates[i].second));
            assigned.push_back(candidates[i].second);
            ++already;
        }
        if (!c.units.empty()) {
            d.out.push_back(std::move(c));
        }
    }
    // Ya no están ociosos para los módulos siguientes.
    std::erase_if(v.idle_workers, [&](entt::entity w) { return std::ranges::find(assigned, w) != assigned.end(); });
}

// Incursión: con raid_group unidades del tipo de incursión ociosas en casa, van a por
// el bagaje enemigo sin escolta (cortar convoyes) o, si no lo hay, a
// quemar el edificio de madera enemigo más cercano sin enemigos armados cerca (lo
// indefenso; lo fortificado se deja para el asedio).
void raid(Decision& d) {
    AiView& v = d.v;
    if (!d.profile.raid_unit || v.threatened) {
        return;
    }
    std::vector<std::uint32_t> riders;
    for (const entt::entity e : v.idle_army) {
        if (d.registry.get<Unit>(e).type == *d.profile.raid_unit &&
            chebyshev(tile_of(d.registry.get<Position>(e)), v.base) <= d.profile.defend_radius_tiles) {
            riders.push_back(entt::to_integral(e));
        }
    }
    if (riders.empty() || std::cmp_less(riders.size(), d.profile.raid_group)) {
        return;
    }
    const auto guarded = [&](TileCoord at) {
        return std::ranges::any_of(v.enemies, [&](const UnitSeen& e) {
            return e.armed && chebyshev(e.tile, at) <= d.profile.raid_safe_radius_tiles;
        });
    };
    std::optional<entt::entity> target;
    std::int32_t best = std::numeric_limits<std::int32_t>::max();
    // Primero, cortar convoyes: el bagaje enemigo sin escolta (sin él, su ejército en
    // campaña se queda sin víveres ni munición).
    for (const UnitSeen& e : v.enemies) {
        const std::int32_t dist = octile_distance(v.base, e.tile);
        if (e.carrier && !guarded(e.tile) && dist < best) {
            best = dist;
            target = e.entity;
        }
    }
    for (std::size_t i = 0; i < v.enemy_buildings.size() && !target; ++i) {
        if (d.catalog().buildings[v.enemy_building_types[i]].material != Material::Wood) {
            continue;
        }
        const TileCoord at = v.enemy_buildings[i];
        const std::int32_t dist = octile_distance(v.base, at);
        if (!guarded(at) && dist < best) {
            best = dist;
            target = v.enemy_building_entities[i];
        }
    }
    if (!target) {
        return;
    }
    std::erase_if(v.idle_army, [&](entt::entity e) {
        return std::ranges::find(riders, entt::to_integral(e)) != riders.end();
    });
    Command c = d.order(CommandType::Attack);
    c.object = entt::to_integral(*target);
    c.units = std::move(riders);
    d.out.push_back(std::move(c));
}

// Abastecerse: cada unidad que no pelea y se ha quedado por debajo de resupply_percent
// de víveres o munición vuelve al edificio propio que abastece más cercano; la que ya
// está a su alcance espera allí a llenarse mientras el almacén pueda pagarlo. Unas y
// otras dejan de estar ociosas para los módulos siguientes (no se las manda a atacar
// con el morral vacío). Las que pelean siguen peleando.
void resupply(Decision& d) {
    AiView& v = d.v;
    if ((v.supply_sources.empty() && v.carriers.empty() && !d.supply.forage) || d.profile.resupply_percent <= 0) {
        return;
    }
    const std::int32_t reach = d.supply.resupply_radius_tiles;
    std::vector<entt::entity> busy;
    std::vector<std::pair<TileCoord, std::vector<std::uint32_t>>> going;  // destino, unidades
    for (const UnitSeen& s : v.soldiers) {
        if (s.supply_percent >= kPercent || s.target != entt::null) {
            continue;
        }
        // La fuente más cercana que puede darle algo: un edificio (paga el almacén del
        // campamento o, si no lo es, el del jugador; un campamento vacío no sirve) o
        // un bagaje con carga.
        const Stock& ammo_cost = d.catalog().units[s.type].supply.ammo_cost;
        const auto can_give = [&](const Stock& pays) {
            return (s.needs_rations && affordable(pays, d.supply.ration_cost)) ||
                   (s.needs_ammo && affordable(pays, ammo_cost));
        };
        std::int32_t best_d = std::numeric_limits<std::int32_t>::max();
        TileCoord best_at;
        for (const auto& [f, e] : v.supply_sources) {
            const SupplyStore* store = d.registry.try_get<SupplyStore>(e);
            const std::int32_t dist = chebyshev(clamp_to(f, s.tile), s.tile);
            if (dist < best_d && can_give(store != nullptr ? store->stock : d.ps.stock)) {
                best_d = dist;
                best_at = center_of(f);
            }
        }
        for (const CarrierView& c : v.carriers) {
            const std::int32_t dist = chebyshev(c.tile, s.tile);
            if (dist < best_d && can_give(c.load)) {
                best_d = dist;
                best_at = c.tile;
            }
        }
        // Saqueo (C2): una granja enemiga sin defensa cerca también da raciones.
        bool pillage = false;
        if (d.supply.forage && s.needs_rations) {
            for (std::size_t i = 0; i < v.enemy_buildings.size(); ++i) {
                if (d.catalog().buildings[v.enemy_building_types[i]].farm_food <= 0) {
                    continue;
                }
                const TileCoord at = v.enemy_buildings[i];
                const std::int32_t dist = chebyshev(at, s.tile);
                const auto guarded = [&](const UnitSeen& e) {
                    return e.armed && chebyshev(e.tile, at) <= d.profile.pillage_guard_tiles;
                };
                if (dist < best_d && std::ranges::none_of(v.enemies, guarded)) {
                    best_d = dist;
                    best_at = at;
                    pillage = true;
                }
            }
        }
        if (best_d == std::numeric_limits<std::int32_t>::max()) {
            continue;  // nadie puede darle nada ahora
        }
        if (best_d <= (pillage ? d.supply.forage_reach_tiles : reach)) {
            busy.push_back(s.entity);  // se está abasteciendo: que termine
            continue;
        }
        if (s.supply_percent >= d.profile.resupply_percent) {
            continue;
        }
        busy.push_back(s.entity);
        const MoveGoal* g = d.registry.try_get<MoveGoal>(s.entity);
        if (g != nullptr && !g->arrived && chebyshev(g->tile, best_at) <= reach) {
            continue;  // ya va hacia allí
        }
        auto it = std::ranges::find(going, best_at, &std::pair<TileCoord, std::vector<std::uint32_t>>::first);
        if (it == going.end()) {
            going.emplace_back(best_at, std::vector<std::uint32_t>{});
            it = going.end() - 1;
        }
        it->second.push_back(entt::to_integral(s.entity));
    }
    for (auto& [at, units] : going) {
        Command c = d.order(CommandType::Move);
        c.target = at;
        c.units = std::move(units);
        d.out.push_back(std::move(c));
    }
    std::erase_if(v.idle_army, [&](entt::entity e) { return std::ranges::find(busy, e) != busy.end(); });
}

// Objetivo de los ataques: el edificio vital enemigo más cercano (su caída decide la
// partida); si no se conoce ninguno, el edificio enemigo más cercano; si no, la unidad.
std::optional<TileCoord> attack_target(const Decision& d) {
    const AiView& v = d.v;
    std::vector<TileCoord> vital;
    for (std::size_t i = 0; i < v.enemy_buildings.size(); ++i) {
        if (d.catalog().buildings[v.enemy_building_types[i]].vital) {
            vital.push_back(v.enemy_buildings[i]);
        }
    }
    auto target = nearest(vital, v.base);
    if (!target) {
        std::vector<TileCoord> worth;  // un camino no es objetivo de nada
        for (std::size_t i = 0; i < v.enemy_buildings.size(); ++i) {
            if (d.catalog().buildings[v.enemy_building_types[i]].road_speed_percent <= 0) {
                worth.push_back(v.enemy_buildings[i]);
            }
        }
        target = nearest(worth, v.base);
    }
    if (!target) {
        target = nearest(v.enemy_units, v.base);
    }
    return target;
}

// Centro del ejército en campaña: las tropas que pelean o avanzan atacando lejos de
// la base.
std::optional<TileCoord> field_center(const Decision& d) {
    std::int64_t sx = 0;
    std::int64_t sy = 0;
    std::int64_t n = 0;
    for (const UnitSeen& s : d.v.soldiers) {
        if ((s.target != entt::null || s.attack_move) && chebyshev(s.tile, d.v.base) > d.profile.defend_radius_tiles) {
            sx += s.tile.x;
            sy += s.tile.y;
            ++n;
        }
    }
    if (n == 0) {
        return std::nullopt;
    }
    return TileCoord{static_cast<std::int32_t>(sx / n), static_cast<std::int32_t>(sy / n)};
}

// Punto a k casillas de from sobre la recta hacia to (from si están más cerca).
TileCoord toward(TileCoord from, TileCoord to, std::int32_t k) {
    const std::int32_t span = chebyshev(from, to);
    if (span <= k) {
        return to;
    }
    return {from.x + (to.x - from.x) * k / span, from.y + (to.y - from.y) * k / span};
}

std::int32_t stock_total(const Stock& s) {
    std::int32_t sum = 0;
    for (const std::int32_t v : s) {
        sum += v;
    }
    return sum;
}

// Logística. El bagaje sigue al ejército: mantiene convoy_carriers unidades cuando
// tiene ejército para atacar; la que lleva menos de media carga vuelve a cargar a
// casa; la cargada va baggage_offset_tiles por detrás del ejército en campaña (hacia
// la base) o, sin campaña, espera en casa. Con camp_distance_tiles > 0, además, si el
// objetivo queda más lejos que eso de toda fuente de suministro, campamento a
// camp_offset_tiles de él y el bagaje en ruta de convoy hacia allí.
void logistics(Decision& d) {
    AiView& v = d.v;
    if (!d.params.carrier) {
        return;
    }
    const UnitTypeId carrier = *d.params.carrier;
    const std::int32_t capacity = d.catalog().units[carrier].convoy_capacity;
    const bool campaigning = std::cmp_greater_equal(v.army.size(), d.profile.min_attack_army);
    const Stock& cost = d.catalog().units[carrier].cost;
    if (campaigning && std::cmp_less(v.carriers.size(), d.profile.convoy_carriers) && v.town_center &&
        v.town_center_queue < d.profile.villager_queue && d.population < d.ps.population_cap &&
        affordable(d.budget, cost)) {
        Command c = d.order(CommandType::Train);
        c.object = entt::to_integral(*v.town_center);
        c.kind = carrier;
        d.out.push_back(std::move(c));
        spend(d.budget, cost);
        ++v.town_center_queue;
    }

    // Campamento (opcional).
    const auto target = attack_target(d);
    if (d.params.camp && d.profile.camp_distance_tiles > 0 && target) {
        std::int32_t nearest_source = std::numeric_limits<std::int32_t>::max();
        for (const auto& [f, e] : v.supply_sources) {
            nearest_source = std::min(nearest_source, chebyshev(clamp_to(f, *target), *target));
        }
        for (const TileCoord c : v.camps) {
            nearest_source = std::min(nearest_source, chebyshev(c, *target));
        }
        // Solo cuando su ejército en campaña ya está junto al objetivo (domina el
        // terreno): un campamento sin tropas delante es leña para el enemigo (medido).
        const auto front = field_center(d);
        if (nearest_source > d.profile.camp_distance_tiles && !v.threatened && front &&
            chebyshev(*front, *target) <= d.profile.siege_front_tiles &&
            chebyshev(*target, v.base) > d.profile.camp_offset_tiles) {
            d.place(*d.params.camp, toward(*target, v.base, d.profile.camp_offset_tiles));
        }
    }
    // Asedio: monta ingenios en el campamento con lo que han traído los convoyes.
    if (v.camp && d.params.siege_engine) {
        const UnitTypeId engine = *d.params.siege_engine;
        std::int32_t engines = 0;
        for (const UnitSeen& s : v.soldiers) {
            engines += s.type == engine ? 1 : 0;
        }
        const ProductionQueue* q = d.registry.try_get<ProductionQueue>(*v.camp);
        const SupplyStore* store = d.registry.try_get<SupplyStore>(*v.camp);
        const auto& trains = d.catalog().buildings[d.registry.get<Building>(*v.camp).type].trains;
        if (q != nullptr && q->items.empty() && store != nullptr && engines < d.profile.siege_engines &&
            std::ranges::find(trains, engine) != trains.end() &&
            affordable(store->stock, d.catalog().units[engine].cost) && d.population < d.ps.population_cap) {
            Command c = d.order(CommandType::Train);
            c.object = entt::to_integral(*v.camp);
            c.kind = engine;
            d.out.push_back(std::move(c));
        }
    }

    // Edificio de casa que abastece más cercano a un punto (para cargar).
    const auto nearest_home = [&](TileCoord t) -> std::optional<entt::entity> {
        std::optional<entt::entity> best;
        std::int32_t best_d = std::numeric_limits<std::int32_t>::max();
        for (const auto& [f, e] : v.supply_sources) {
            const std::int32_t dist = chebyshev(clamp_to(f, t), t);
            if (!d.registry.all_of<SupplyStore>(e) && dist < best_d) {
                best_d = dist;
                best = e;
            }
        }
        return best;
    };
    const auto field = field_center(d);
    std::vector<std::pair<entt::entity, std::vector<std::uint32_t>>> loads;  // edificio, bagaje
    std::vector<std::uint32_t> to_camp;
    std::vector<std::uint32_t> follow;
    std::vector<std::uint32_t> home;
    const TileCoord rear = field ? toward(*field, v.base, d.profile.baggage_offset_tiles) : v.base;
    constexpr std::int32_t kSlackTiles = 3;  // no se reordena por menos de esto
    for (const CarrierView& c : v.carriers) {
        if (!c.idle) {
            continue;  // cargando o en ruta de convoy
        }
        if (v.camp) {
            to_camp.push_back(entt::to_integral(c.entity));
            continue;
        }
        if (stock_total(c.load) * 2 < capacity) {
            if (const auto b = nearest_home(c.tile)) {
                auto it = std::ranges::find(loads, *b, &std::pair<entt::entity, std::vector<std::uint32_t>>::first);
                if (it == loads.end()) {
                    loads.emplace_back(*b, std::vector<std::uint32_t>{});
                    it = loads.end() - 1;
                }
                it->second.push_back(entt::to_integral(c.entity));
            }
            continue;
        }
        const TileCoord want = field ? rear : v.base;
        const TileCoord going = c.goal ? *c.goal : c.tile;
        if (chebyshev(going, want) > (field ? kSlackTiles : d.profile.defend_radius_tiles / 2)) {
            (field ? follow : home).push_back(entt::to_integral(c.entity));
        }
    }
    for (auto& [b, units] : loads) {
        Command c = d.order(CommandType::Convoy);
        c.object = entt::to_integral(b);
        c.units = std::move(units);
        d.out.push_back(std::move(c));
    }
    if (!to_camp.empty()) {
        Command c = d.order(CommandType::Convoy);
        c.object = entt::to_integral(*v.camp);
        c.units = std::move(to_camp);
        d.out.push_back(std::move(c));
    }
    for (auto* group : {&follow, &home}) {
        if (!group->empty()) {
            Command c = d.order(CommandType::Move);
            c.target = group == &follow ? rear : v.base;
            c.units = std::move(*group);
            d.out.push_back(std::move(c));
        }
    }
}

// Exploración (solo con niebla de guerra). Mientras no conozca ningún edificio vital
// enemigo, mantiene scouts exploradores: unidades ociosas (las de raid_unit primero)
// que van, una tras otra, a puntos sin explorar de una rejilla de explore_step_tiles
// casillas: primero hacia el punto simétrico de su base respecto al centro del mapa
// (donde lo buscaría un jugador) y sin alejarse demasiado de donde están. El explorador que llega a su punto, o lo ve ya
// explorado, recibe el siguiente; al encontrar al enemigo, vuelve a ser tropa.
constexpr std::int32_t kScoutNearWeight = 1;
constexpr std::int32_t kScoutMirrorWeight = 2;

// Exploración continua (D2): con el enemigo localizado, cada explorador va al punto del
// anillo de patrol_radius_tiles alrededor de su base vital que ahora no se ve, el más
// cercano a él; así se sabe por dónde se mueve su ejército.
void patrol(Decision& d) {
    AiView& v = d.v;
    std::optional<TileCoord> center;
    for (std::size_t i = 0; i < v.enemy_buildings.size() && !center; ++i) {
        if (d.catalog().buildings[v.enemy_building_types[i]].vital) {
            center = v.enemy_buildings[i];
        }
    }
    if (!center) {
        return;
    }
    const std::int32_t r = d.profile.patrol_radius_tiles;
    const PassGrid& grid = d.grid;
    for (std::size_t i = 0; i < d.ai.scouts.size(); ++i) {
        const entt::entity s = d.ai.scouts[i];
        std::erase(v.idle_army, s);
        const TileCoord at = tile_of(d.registry.get<Position>(s));
        const MoveGoal* g = d.registry.try_get<MoveGoal>(s);
        TileCoord& target = d.ai.scout_targets[i];
        if (target.x >= 0 && g != nullptr && !g->arrived) {
            continue;  // de camino
        }
        std::optional<TileCoord> best;
        std::int32_t best_d = std::numeric_limits<std::int32_t>::max();
        // Ocho puntos del anillo, en orden fijo.
        constexpr std::array<std::array<std::int32_t, 2>, 8> kRing{
            {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}}};
        for (const auto& [dx, dy] : kRing) {
            const TileCoord t{std::clamp(center->x + dx * r, 0, grid.width() - 1),
                              std::clamp(center->y + dy * r, 0, grid.height() - 1)};
            if (!grid.passable(t) || grid.component(t) != grid.component(at) || t == target ||
                d.vision->sees_footprint(d.me(), Footprint{t, 1})) {
                continue;
            }
            const std::int32_t dist = octile_distance(at, t);
            if (dist < best_d) {
                best_d = dist;
                best = t;
            }
        }
        if (best) {
            target = *best;
            Command c = d.order(CommandType::Move);
            c.target = *best;
            c.units = {entt::to_integral(s)};
            d.out.push_back(std::move(c));
        }
    }
}

void explore(Decision& d) {
    AiView& v = d.v;
    std::vector<entt::entity>& scouts = d.ai.scouts;
    std::vector<TileCoord>& targets = d.ai.scout_targets;
    // Fuera los caídos o los que ya no son suyos.
    for (std::size_t i = scouts.size(); i-- > 0;) {
        const entt::entity e = scouts[i];
        if (!d.registry.valid(e) || !d.registry.all_of<Unit, Owner>(e) || d.registry.get<Owner>(e).player != d.me()) {
            scouts.erase(scouts.begin() + static_cast<std::ptrdiff_t>(i));
            targets.erase(targets.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    bool enemy_known = false;
    for (const BuildingTypeId t : v.enemy_building_types) {
        enemy_known = enemy_known || d.catalog().buildings[t].vital;
    }
    if (d.vision == nullptr || !d.vision->enabled() || (enemy_known && d.profile.patrol_radius_tiles <= 0)) {
        scouts.clear();  // ya sabe dónde atacar: vuelven a ser tropa
        targets.clear();
        return;
    }
    // Nuevos exploradores hasta scouts: primero los del tipo de incursión.
    for (const bool prefer_raider : {true, false}) {
        for (auto it = v.idle_army.begin(); it != v.idle_army.end() && std::cmp_less(scouts.size(), d.profile.scouts);) {
            const UnitTypeId type = d.registry.get<Unit>(*it).type;
            if (prefer_raider && (!d.profile.raid_unit || type != *d.profile.raid_unit)) {
                ++it;
                continue;
            }
            scouts.push_back(*it);
            targets.push_back({-1, -1});
            it = v.idle_army.erase(it);
        }
    }
    if (enemy_known) {
        patrol(d);  // exploración continua: vigilar lo que no se ve alrededor del enemigo
        return;
    }
    const std::int32_t step = std::max(d.profile.explore_step_tiles, 1);
    const PassGrid& grid = d.grid;
    const TileCoord mirror{grid.width() - 1 - v.base.x, grid.height() - 1 - v.base.y};
    for (std::size_t i = 0; i < scouts.size(); ++i) {
        const entt::entity s = scouts[i];
        std::erase(v.idle_army, s);
        const TileCoord at = tile_of(d.registry.get<Position>(s));
        const MoveGoal* g = d.registry.try_get<MoveGoal>(s);
        TileCoord& target = targets[i];
        const bool has_target = target.x >= 0;
        if (has_target && g != nullptr && !g->arrived && !d.vision->explored(d.me(), target)) {
            continue;  // va hacia un punto que aún no se ha visto
        }
        if (has_target && !d.vision->explored(d.me(), target)) {
            // Llegó (o se paró) sin verlo: tapado por árboles o lomas, o inalcanzable.
            d.ai.explore_done.push_back(target);
        }
        std::optional<TileCoord> best;
        std::int32_t best_d = std::numeric_limits<std::int32_t>::max();
        for (std::int32_t y = step / 2; y < grid.height(); y += step) {
            for (std::int32_t x = step / 2; x < grid.width(); x += step) {
                const TileCoord t{x, y};
                if (d.vision->explored(d.me(), t) || !grid.passable(t) ||
                    grid.component(t) != grid.component(at) ||
                    std::ranges::find(d.ai.explore_done, t) != d.ai.explore_done.end()) {
                    continue;
                }
                // Primero hacia donde lo buscaría un jugador: el punto simétrico de su base
                // respecto al centro del mapa; sin alejarse demasiado de donde está.
                const std::int32_t score =
                    kScoutNearWeight * octile_distance(at, t) + kScoutMirrorWeight * octile_distance(mirror, t);
                if (score < best_d) {
                    best_d = score;
                    best = t;
                }
            }
        }
        target = best.value_or(TileCoord{-1, -1});
        if (best) {
            Command c = d.order(CommandType::Move);
            c.target = *best;
            c.units = {entt::to_integral(s)};
            d.out.push_back(std::move(c));
        }
    }
}

// Ataque por fuerza, con retirada. Primero, si el ejército en campaña pierde su
// batalla (fuerza local por debajo de retreat_ratio_percent % de la enemiga), vuelve
// a casa. Si no, y la fuerza total supera attack_ratio_percent % de la enemiga
// conocida, las tropas ociosas atacan el edificio vital enemigo más cercano (su caída
// decide la partida) o, si no se conoce ninguno, el edificio más cercano.
void attack_strength(Decision& d) {
    const AiView& v = d.v;
    const auto& types = d.catalog().units;
    const std::int32_t radius = d.profile.engage_radius_tiles;

    std::vector<const UnitSeen*> field;
    std::int64_t sx = 0;
    std::int64_t sy = 0;
    for (const UnitSeen& s : v.soldiers) {
        if ((s.target != entt::null || s.attack_move) && chebyshev(s.tile, v.base) > d.profile.defend_radius_tiles) {
            field.push_back(&s);
            sx += s.tile.x;
            sy += s.tile.y;
        }
    }
    std::optional<TileCoord> field_center;
    if (!field.empty()) {
        const auto n = static_cast<std::int64_t>(field.size());
        const TileCoord center{static_cast<std::int32_t>(sx / n), static_cast<std::int32_t>(sy / n)};
        field_center = center;
        std::int64_t own = 0;
        for (const UnitSeen* s : field) {
            own += chebyshev(s->tile, center) <= radius ? strength(types[s->type], s->hp) : 0;
        }
        std::int64_t enemy = 0;
        for (const UnitSeen& e : v.enemies) {
            enemy += e.armed && chebyshev(e.tile, center) <= radius ? strength(types[e.type], e.hp) : 0;
        }
        // Moral (D3): si el ejército flaquea, se retira antes de desbandarse.
        std::int64_t morale = 0;
        std::int64_t counted = 0;
        for (const UnitSeen* s : field) {
            if (const Morale* m = d.registry.try_get<Morale>(s->entity)) {
                morale += m->value;
                ++counted;
            }
        }
        const bool shaken = d.profile.retreat_morale > 0 && counted > 0 && enemy > 0 &&
                            morale < std::int64_t{d.profile.retreat_morale} * counted;
        if ((enemy > 0 && own * kPercent < enemy * d.profile.retreat_ratio_percent) || shaken) {
            Command c = d.order(CommandType::Move);
            c.target = v.base;
            for (const UnitSeen* s : field) {
                c.units.push_back(entt::to_integral(s->entity));
            }
            d.out.push_back(std::move(c));
            return;
        }
    }

    const auto target = attack_target(d);
    if (!target) {
        return;
    }
    // Sueltas: ociosas lejos de casa y sin ningún enemigo a la vista (por ejemplo, al
    // caer el blanco que les asignó concentrar, que apaga su ataque-movimiento). Se
    // unen al ejército que pelea en campaña o, si no hay ninguno, siguen hacia el
    // objetivo. Las que tienen enemigos a la vista se dejan: eligen blanco solas, y
    // reordenarlas cada decisión les cortaría el ataque (medido en el torneo).
    std::vector<entt::entity> stranded;
    std::vector<entt::entity> home;
    for (const entt::entity e : v.idle_army) {
        const TileCoord t = tile_of(d.registry.get<Position>(e));
        if (chebyshev(t, v.base) <= d.profile.defend_radius_tiles) {
            home.push_back(e);
            continue;
        }
        const std::int32_t sight = types[d.registry.get<Unit>(e).type].combat.sight_tiles;
        const auto in_sight = [&](TileCoord o) { return chebyshev(o, t) <= sight; };
        if (std::ranges::none_of(v.enemy_units, in_sight) && std::ranges::none_of(v.enemy_buildings, in_sight)) {
            stranded.push_back(e);
        }
    }
    if (!stranded.empty()) {
        Command c = d.order(CommandType::AttackMove);
        c.target = field_center ? *field_center : *target;
        c.units = ids(stranded);
        d.out.push_back(std::move(c));
    }

    if (v.threatened || std::cmp_less(home.size(), d.profile.min_attack_army)) {
        return;
    }
    std::int64_t own_total = 0;
    for (const UnitSeen& s : v.soldiers) {
        own_total += strength(types[s.type], s.hp);
    }
    const std::int64_t enemy_total = enemy_armed_strength(d);
    // De noche el rival ve la mitad: basta menos ventaja para atacar (D2).
    std::int32_t ratio = d.profile.attack_ratio_percent;
    if (d.profile.night_attack_ratio_percent > 0 && d.vision != nullptr && d.vision->enabled() &&
        d.vision->daylight_percent(d.tick) < kPercent) {
        ratio = d.profile.night_attack_ratio_percent;
    }
    if (own_total * kPercent < enemy_total * ratio) {
        return;
    }
    Command c = d.order(CommandType::AttackMove);
    c.target = *target;
    c.units = ids(home);
    d.out.push_back(std::move(c));
    ++d.ai.waves_sent;
}

// Fuego concentrado: cada unidad que pelea elige, entre los enemigos armados a su
// vista, el que necesita menos golpes suyos para caer (a igualdad, el más cercano).
// Los aldeanos enemigos no entran: primero lo que amenaza al ejército.
void focus_fire(Decision& d) {
    const AiView& v = d.v;
    const auto& types = d.catalog().units;
    std::vector<std::pair<entt::entity, std::vector<std::uint32_t>>> orders;  // blanco, atacantes
    for (const UnitSeen& s : v.soldiers) {
        if (s.target == entt::null && !s.attack_move) {
            continue;
        }
        const UnitType& me = types[s.type];
        const std::int32_t sight = me.combat.sight_tiles;
        const UnitSeen* best = nullptr;
        std::int64_t best_hits = std::numeric_limits<std::int64_t>::max();
        std::int32_t best_dist = std::numeric_limits<std::int32_t>::max();
        for (const UnitSeen& e : v.enemies) {
            const std::int32_t dist = chebyshev(e.tile, s.tile);
            if (!e.armed || dist > sight) {
                continue;
            }
            const CombatStats& t = types[e.type].combat;
            const std::int64_t dmg = hit_damage(me.combat, kPercent, t.armor_melee, t.armor_pierce, t.armor_class);
            const std::int64_t hits = (e.hp + dmg - 1) / dmg;
            if (hits < best_hits || (hits == best_hits && dist < best_dist)) {
                best_hits = hits;
                best_dist = dist;
                best = &e;
            }
        }
        if (best == nullptr || best->entity == s.target) {
            continue;
        }
        auto it = std::ranges::find(orders, best->entity, &std::pair<entt::entity, std::vector<std::uint32_t>>::first);
        if (it == orders.end()) {
            orders.emplace_back(best->entity, std::vector<std::uint32_t>{});
            it = orders.end() - 1;
        }
        it->second.push_back(entt::to_integral(s.entity));
    }
    for (auto& [target, attackers] : orders) {
        Command c = d.order(CommandType::Attack);
        c.object = entt::to_integral(target);
        c.units = std::move(attackers);
        d.out.push_back(std::move(c));
    }
}

// Asalto: si el ejército en campaña no puede llegar a su objetivo (recinto cerrado sin
// puerta propia que cruzar), abre brecha en el tramo de muro enemigo más cercano: los
// ingenios lo golpean, la infantería que sabe lo escala si hay madera para escalas y
// el resto lo ataca (la madera arde; la piedra solo cede al asedio). Las puertas no
// hacen falta: quien llega a una se queda ante ella y la ataca solo.
void assault(Decision& d) {
    const AiView& v = d.v;
    const auto target = attack_target(d);
    if (!target) {
        return;
    }
    std::vector<const UnitSeen*> field;
    for (const UnitSeen& s : v.soldiers) {
        if (chebyshev(s.tile, v.base) > d.profile.defend_radius_tiles &&
            !d.registry.any_of<Climbing, Garrisoned>(s.entity)) {
            field.push_back(&s);
        }
    }
    if (field.empty()) {
        return;
    }
    // ¿Comparte componente con alguna casilla junto al objetivo? Entonces se llega.
    const std::uint32_t army_comp = d.grid.component(field.front()->tile);
    if (army_comp == 0) {
        return;
    }
    constexpr std::int32_t kReachRing = 3;  // el objetivo puede ocupar hasta 3x3 casillas
    for (std::int32_t dy = -kReachRing; dy <= kReachRing; ++dy) {
        for (std::int32_t dx = -kReachRing; dx <= kReachRing; ++dx) {
            if (d.grid.component({target->x + dx, target->y + dy}) == army_comp) {
                return;
            }
        }
    }
    // Encerrado: el tramo de muro (o puerta) enemigo más cercano al ejército.
    std::int64_t sx = 0;
    std::int64_t sy = 0;
    for (const UnitSeen* s : field) {
        sx += s->tile.x;
        sy += s->tile.y;
    }
    const auto n = static_cast<std::int64_t>(field.size());
    const TileCoord center{static_cast<std::int32_t>(sx / n), static_cast<std::int32_t>(sy / n)};
    const auto& buildings = d.catalog().buildings;
    std::optional<std::size_t> best;
    std::int32_t best_d = std::numeric_limits<std::int32_t>::max();
    for (std::size_t i = 0; i < v.enemy_building_entities.size(); ++i) {
        const BuildingType& bt = buildings[v.enemy_building_types[i]];
        if (!bt.climbable && !bt.gate) {
            continue;
        }
        const std::int32_t dist = chebyshev(v.enemy_buildings[i], center);
        if (dist < best_d) {
            best_d = dist;
            best = i;
        }
    }
    if (!best) {
        return;
    }
    const entt::entity wall = v.enemy_building_entities[*best];
    if (!d.registry.valid(wall)) {
        return;  // recordado, pero ya no está
    }
    const BuildingType& wt = buildings[v.enemy_building_types[*best]];
    const bool wood = wt.material == Material::Wood;
    const auto& types = d.catalog().units;
    Command hit = d.order(CommandType::Attack);
    hit.object = entt::to_integral(wall);
    Command climb = d.order(CommandType::Climb);
    climb.object = hit.object;
    for (const UnitSeen* s : field) {
        if (s->target == wall) {
            continue;  // ya está en ello
        }
        const UnitType& t = types[s->type];
        if (t.combat.siege || (wood && !t.combat.buildings_only)) {
            hit.units.push_back(entt::to_integral(s->entity));
        } else if (wt.climbable && t.climbs && affordable(d.budget, d.params.ladder_cost)) {
            for (std::size_t r = 0; r < kResourceCount; ++r) {
                d.budget[r] -= d.params.ladder_cost[r];
            }
            climb.units.push_back(entt::to_integral(s->entity));
        }
    }
    for (Command* c : {&hit, &climb}) {
        if (!c->units.empty()) {
            d.out.push_back(std::move(*c));
        }
    }
}

// Sanidad (D1): un puesto médico junto a la base cuando hay ejército; aldeanos de
// enfermeros hasta llenar sus plazas; cirujanos; y los heridos que no pelean, al puesto.
void medicine(Decision& d) {
    AiView& v = d.v;
    if (!d.params.medical_post || v.own_type_count.empty()) {
        return;
    }
    const BuildingTypeId post_type = *d.params.medical_post;
    if (v.own_type_count[post_type] == 0) {
        if (!v.threatened && std::cmp_greater_equal(v.army.size(), d.profile.medical_min_army) && v.barracks_complete) {
            d.place(post_type, v.base);
        }
        return;
    }
    // El puesto propio en uso más cercano a la base.
    entt::entity post = entt::null;
    std::int32_t best = std::numeric_limits<std::int32_t>::max();
    for (const auto [e, b, f, o] : d.registry.view<const Building, const Footprint, const Owner>().each()) {
        if (o.player == d.me() && b.type == post_type && b.working()) {
            const std::int32_t dist = chebyshev(center_of(f), v.base);
            if (dist < best) {
                best = dist;
                post = e;
            }
        }
    }
    if (post == entt::null) {
        return;
    }
    const BuildingType& pt = d.catalog().buildings[post_type];
    // Personal: cirujanos y aldeanos que lo atienden.
    std::int32_t staff = 0;
    std::int32_t surgeons = 0;
    for (const auto [e, u, o] : d.registry.view<const Unit, const Owner>().each()) {
        if (o.player != d.me()) {
            continue;
        }
        if (d.params.surgeon && u.type == *d.params.surgeon) {
            ++surgeons;
        }
        const Worker* w = d.registry.try_get<Worker>(e);
        const Carer* c = d.registry.try_get<Carer>(e);
        if ((w != nullptr && w->task == WorkerTask::Nurse && w->building == post) || (c != nullptr && c->post == post)) {
            ++staff;
        }
    }
    if (d.params.surgeon && surgeons < d.profile.surgeons) {
        const ProductionQueue* q = d.registry.try_get<ProductionQueue>(post);
        const Stock& cost = d.catalog().units[*d.params.surgeon].cost;
        if (q != nullptr && q->items.empty() && affordable(d.budget, cost)) {
            Command c = d.order(CommandType::Train);
            c.object = entt::to_integral(post);
            c.kind = *d.params.surgeon;
            d.out.push_back(std::move(c));
            spend(d.budget, cost);
        }
    }
    if (staff < pt.nurses) {
        const auto nurses = d.take_builders(1);
        if (!nurses.empty()) {
            Command c = d.order(CommandType::Tend);
            c.object = entt::to_integral(post);
            c.units = ids(nurses);
            d.out.push_back(std::move(c));
        }
    }
    // Heridos que no pelean: al puesto.
    Command treat = d.order(CommandType::Treat);
    treat.object = entt::to_integral(post);
    for (const UnitSeen& s : v.soldiers) {
        if (s.target != entt::null || !d.catalog().units[s.type].treatable ||
            d.registry.any_of<Patient, Reorganizing, Routing, Garrisoned, Climbing>(s.entity)) {
            continue;
        }
        const std::int32_t max_hp = d.registry.get<Health>(s.entity).max_hp;
        if (std::int64_t{s.hp} * kPercent < std::int64_t{max_hp} * d.profile.wounded_percent) {
            treat.units.push_back(entt::to_integral(s.entity));
        }
    }
    if (!treat.units.empty()) {
        d.out.push_back(std::move(treat));
    }
}

// Punto a `dist` casillas de from en dirección a to (from si coinciden).
TileCoord step_toward(TileCoord from, TileCoord to, std::int32_t dist) {
    const std::int32_t dx = to.x - from.x;
    const std::int32_t dy = to.y - from.y;
    const std::int32_t len = std::max(std::abs(dx), std::abs(dy));
    if (len == 0) {
        return from;
    }
    const std::int32_t k = std::min(dist, len);
    return {from.x + dx * k / len, from.y + dy * k / len};
}

// Emboscada (D2): con niebla, unos tiradores esperan quietos en un claro del bosque
// camino del enemigo; entre los árboles no se les ve hasta tenerlos encima.
void ambush(Decision& d) {
    AiView& v = d.v;
    auto& team = d.ai.ambushers;
    std::erase_if(team, [&](entt::entity e) {
        return !d.registry.valid(e) || !d.registry.all_of<Owner>(e) || d.registry.get<Owner>(e).player != d.me();
    });
    for (const entt::entity e : team) {
        std::erase(v.idle_army, e);  // los emboscados no son tropa para los demás módulos
    }
    const auto target = attack_target(d);
    if (d.vision == nullptr || !d.vision->enabled() || !target || d.profile.ambush_size <= 0 || v.threatened) {
        return;
    }
    if (d.ai.ambush_spot.x < 0) {
        // El claro con más árboles alrededor cerca del punto elegido.
        const TileCoord aim = step_toward(v.base, *target, d.profile.ambush_distance_tiles);
        // La base está bajo un edificio: su región es la de alguna casilla libre al lado.
        std::uint32_t home_comp = 0;
        for (std::int32_t r = 1; r <= d.profile.defend_radius_tiles && home_comp == 0; ++r) {
            for (std::int32_t dx = -r; dx <= r && home_comp == 0; ++dx) {
                home_comp = d.grid.component({v.base.x + dx, v.base.y + r});
            }
        }
        const std::int32_t search = d.profile.ambush_search_tiles;
        const std::int32_t wood = d.profile.ambush_wood_tiles;
        std::int32_t best_trees = d.profile.ambush_min_trees - 1;
        std::int32_t best_d = std::numeric_limits<std::int32_t>::max();
        for (std::int32_t y = aim.y - search; y <= aim.y + search; ++y) {
            for (std::int32_t x = aim.x - search; x <= aim.x + search; ++x) {
                const TileCoord t{x, y};
                if (!d.grid.passable(t) || d.grid.component(t) != home_comp) {
                    continue;
                }
                std::int32_t trees = 0;
                for (std::int32_t wy = y - wood; wy <= y + wood; ++wy) {
                    for (std::int32_t wx = x - wood; wx <= x + wood; ++wx) {
                        const entt::entity o = d.economy.occupant({wx, wy});
                        const ResourceNode* n = o != entt::null ? d.registry.try_get<ResourceNode>(o) : nullptr;
                        trees += n != nullptr && n->kind == Resource::Wood && !d.registry.all_of<Owner>(o) ? 1 : 0;
                    }
                }
                const std::int32_t dist = chebyshev(t, aim);
                if (trees > best_trees || (trees == best_trees && trees >= d.profile.ambush_min_trees && dist < best_d)) {
                    best_trees = trees;
                    best_d = dist;
                    d.ai.ambush_spot = t;
                }
            }
        }
        if (d.ai.ambush_spot.x < 0) {
            d.ai.ambush_spot = {-2, -2};  // no hay bosque que valga: no se vuelve a buscar
        }
    }
    if (d.ai.ambush_spot.x < 0) {
        return;
    }
    // Completar el grupo con tiradores ociosos en casa.
    std::vector<entt::entity> fresh;
    for (auto it = v.idle_army.begin(); it != v.idle_army.end() && std::cmp_less(team.size(), d.profile.ambush_size);) {
        const UnitType& t = d.catalog().units[d.registry.get<Unit>(*it).type];
        if (t.combat.projectile_speed.raw() != 0 && chebyshev(tile_of(d.registry.get<Position>(*it)), v.base) <=
                                                         d.profile.defend_radius_tiles) {
            team.push_back(*it);
            fresh.push_back(*it);
            it = v.idle_army.erase(it);
        } else {
            ++it;
        }
    }
    if (fresh.empty()) {
        return;
    }
    Command go = d.order(CommandType::Move);
    go.target = d.ai.ambush_spot;
    go.units = ids(fresh);
    d.out.push_back(std::move(go));
    Command hold = d.order(CommandType::SetStance);
    hold.kind = static_cast<std::uint8_t>(Stance::HoldGround);
    hold.units = ids(fresh);
    d.out.push_back(std::move(hold));
}

// Torres (D2): levanta torres a tower_offset_tiles de la base, hacia el enemigo; si
// atacan la base, los tiradores ociosos se guarnecen en ellas.
void towers(Decision& d) {
    AiView& v = d.v;
    if (!d.params.tower || d.profile.towers <= 0 || v.own_type_count.empty()) {
        return;
    }
    const BuildingTypeId tower = *d.params.tower;
    if (!v.threatened) {
        if (v.own_type_count[tower] < d.profile.towers && v.barracks_complete) {
            const auto target = attack_target(d);
            const TileCoord mirror{d.grid.width() - 1 - v.base.x, d.grid.height() - 1 - v.base.y};
            d.place(tower, step_toward(v.base, target.value_or(mirror), d.profile.tower_offset_tiles));
        }
        return;
    }
    const std::int32_t places = d.catalog().buildings[tower].garrison;
    for (const auto [e, b, o] : d.registry.view<const Building, const Owner>().each()) {
        if (o.player != d.me() || b.type != tower || !b.working()) {
            continue;
        }
        std::int32_t inside = 0;
        for (const auto [g, gar] : d.registry.view<const Garrisoned>().each()) {
            inside += gar.tower == e ? 1 : 0;
        }
        Command c = d.order(CommandType::Garrison);
        c.object = entt::to_integral(e);
        for (auto it = v.idle_army.begin(); it != v.idle_army.end() && inside < places;) {
            const UnitType& t = d.catalog().units[d.registry.get<Unit>(*it).type];
            if (t.combat.projectile_speed.raw() != 0 && !d.registry.all_of<Garrisoned>(*it)) {
                c.units.push_back(entt::to_integral(*it));
                ++inside;
                it = v.idle_army.erase(it);
            } else {
                ++it;
            }
        }
        if (!c.units.empty()) {
            d.out.push_back(std::move(c));
        }
    }
}

// Táctica (D3): formaciones según la situación y paso forzado para volver a defender.
//   - En campaña sin enemigos cerca: columna (marcha más rápido).
//   - Con enemigos armados a formation_engage_tiles: los tiradores, en línea; la
//     infantería, en cuadro si hay caballería enemiga cerca; si no, sin formación.
//   - Si atacan la base, quien está lejos vuelve a paso forzado; en casa, paso normal.
void tactics(Decision& d) {
    const AiView& v = d.v;
    const auto& units = d.catalog().units;
    std::array<std::vector<std::uint32_t>, 4> forms;  // por FormationKind
    std::vector<std::uint32_t> forced;
    std::vector<std::uint32_t> normal;
    for (const UnitSeen& s : v.soldiers) {
        const UnitType& t = units[s.type];
        const bool home = chebyshev(s.tile, v.base) <= d.profile.defend_radius_tiles;
        if (const Fatigue* f = d.registry.try_get<Fatigue>(s.entity)) {
            const bool want = d.profile.defend_forced_march && v.threatened && !home;
            if (want != f->forced) {
                (want ? forced : normal).push_back(entt::to_integral(s.entity));
            }
        }
        if (d.profile.formation_engage_tiles <= 0 || t.combat.buildings_only) {
            continue;
        }
        FormationKind want = FormationKind::None;
        if (!home) {
            bool enemy_near = false;
            bool cavalry_near = false;
            for (const UnitSeen& e : v.enemies) {
                if (e.armed && chebyshev(e.tile, s.tile) <= d.profile.formation_engage_tiles) {
                    enemy_near = true;
                    cavalry_near = cavalry_near || units[e.type].combat.armor_class == d.params.cavalry_class;
                }
            }
            const bool ranged = t.combat.projectile_speed.raw() != 0;
            if (!enemy_near) {
                want = FormationKind::Column;
            } else if (ranged) {
                want = FormationKind::Line;
            } else if (cavalry_near && t.combat.armor_class != d.params.cavalry_class) {
                want = FormationKind::Square;
            }
        }
        const Formation* f = d.registry.try_get<Formation>(s.entity);
        const FormationKind now = f != nullptr ? f->kind : FormationKind::None;
        if (now != want) {
            forms[static_cast<std::size_t>(want)].push_back(entt::to_integral(s.entity));
        }
    }
    for (std::size_t k = 0; k < forms.size(); ++k) {
        if (!forms[k].empty()) {
            Command c = d.order(CommandType::SetStance);
            c.kind = static_cast<std::uint8_t>(kFormationBase + k);
            c.units = std::move(forms[k]);
            d.out.push_back(std::move(c));
        }
    }
    const std::array<std::pair<std::vector<std::uint32_t>*, std::uint8_t>, 2> paces{
        {{&forced, kPaceForced}, {&normal, kPaceNormal}}};
    for (const auto& [list, kind] : paces) {
        if (!list->empty()) {
            Command c = d.order(CommandType::SetStance);
            c.kind = kind;
            c.units = std::move(*list);
            d.out.push_back(std::move(c));
        }
    }
}

// Herrería (D3): con forge_at_villagers aldeanos, la levanta; terminada, investiga la
// primera mejora pagable para las clases de su ejército (o de las que entrena).
void forge(Decision& d) {
    const AiView& v = d.v;
    if (!d.params.forge || v.own_type_count.empty() || v.threatened) {
        return;
    }
    const BuildingTypeId type = *d.params.forge;
    if (v.own_type_count[type] == 0) {
        if (std::cmp_greater_equal(v.workers.size(), d.profile.forge_at_villagers) && v.barracks_complete) {
            d.place(type, v.base);
        }
        return;
    }
    std::uint32_t classes = 0;
    for (const UnitTypeId u : d.profile.army) {
        classes |= 1U << d.catalog().units[u].combat.armor_class;
    }
    for (const auto [e, b, o] : d.registry.view<const Building, const Owner>().each()) {
        if (o.player != d.me() || b.type != type) {
            continue;
        }
        const auto& ups = d.catalog().upgrades;
        for (std::size_t u = 0; u < ups.size(); ++u) {
            const auto id = static_cast<UpgradeId>(u);
            if ((ups[u].classes & classes) == 0 || !d.economy.can_research(d.registry, d.me(), e, id) ||
                !affordable(d.budget, ups[u].cost)) {
                continue;
            }
            Command c = d.order(CommandType::Research);
            c.object = entt::to_integral(e);
            c.kind = id;
            d.out.push_back(std::move(c));
            spend(d.budget, ups[u].cost);
            return;
        }
    }
}

using BehaviorFn = void (*)(Decision&);
constexpr std::array<BehaviorFn, static_cast<std::size_t>(AiBehavior::Count)> kBehaviors{
    defend, villagers, houses, barracks, farms, dropoffs, builders, gather, army, attack,
    army_counter, attack_strength, focus_fire, workshop, extinguish, raid, resupply, logistics, explore,
    assault, medicine, ambush, towers, tactics, forge,
};

}  // namespace

AiSystem::AiSystem(const AiParams& params, const SupplyParams& supply, const std::vector<AiSeat>& seats)
    : params_(params), supply_(supply) {
    for (const AiSeat& seat : seats) {
        AiPlayerState s;
        s.player = seat.player;
        s.wave_size = params_.profiles[seat.profile].first_wave;
        players_.push_back(s);
        profiles_.push_back(seat.profile);
    }
}

void AiSystem::think(const entt::registry& registry, const EconomySystem& economy, const PassGrid& grid, Tick tick,
                     std::vector<Command>& out, const VisionSystem* vision) {
    const auto interval = static_cast<std::uint32_t>(std::max(params_.think_interval_ticks, 1));
    for (std::size_t i = 0; i < players_.size(); ++i) {
        AiPlayerState& ai = players_[i];
        // Repartidos: el jugador p decide en los ticks con tick % intervalo == p % intervalo.
        if ((tick + interval - static_cast<std::uint32_t>(ai.player) % interval) % interval != 0) {
            continue;
        }
        const PlayerState& ps = economy.players()[ai.player];
        Decision d{registry, economy,  grid, params_, supply_, params_.profiles[profiles_[i]], ai, tick, out, ps, {},
                   ps.stock, ps.population};
        d.vision = vision;
        if (!perceive(d)) {
            continue;  // derrotado
        }
        if (vision != nullptr && vision->enabled()) {
            // Recuerdo del ejército enemigo: lo que se ve ahora o, si es más, lo que se vio
            // y se va olvidando.
            const std::size_t n = economy.catalog().units.size();
            std::vector<std::int64_t> seen(n, 0);
            for (const UnitSeen& e : d.v.enemies) {
                seen[e.type] += kMilliUnit;
            }
            ai.enemy_seen_milli.resize(n, 0);
            for (std::size_t t = 0; t < n; ++t) {
                std::int64_t& m = ai.enemy_seen_milli[t];
                m = std::max(seen[t], m - m * params_.enemy_memory_decay_permille / kMilliUnit);
            }
            d.v.use_estimate = true;
            d.v.enemy_estimate_milli = ai.enemy_seen_milli;
        }
        // Reserva para las raciones de los que comen: fuera del presupuesto.
        std::int64_t eaters = 0;
        for (const auto [e, s, o] : registry.view<const Supply, const Owner>().each()) {
            eaters += o.player == ai.player && economy.catalog().units[registry.get<Unit>(e).type].supply.rations > 0;
        }
        for (std::size_t r = 0; r < kResourceCount; ++r) {
            const std::int64_t reserve = eaters * supply_.ration_cost[r] * d.profile.upkeep_reserve_percent / kPercent;
            d.budget[r] = static_cast<std::int32_t>(std::max<std::int64_t>(d.budget[r] - reserve, 0));
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
        // Lo que solo existe con niebla de guerra entra solo si existe: sin niebla, el
        // hash es el de siempre.
        if (!ai.ambushers.empty() || ai.ambush_spot.x != -1) {
            h.add_u64(ai.ambushers.size());
            for (const entt::entity e : ai.ambushers) {
                h.add_u32(entt::to_integral(e));
            }
            h.add_i32(ai.ambush_spot.x);
            h.add_i32(ai.ambush_spot.y);
        }
        if (!ai.scouts.empty() || !ai.explore_done.empty() || !ai.enemy_seen_milli.empty()) {
            h.add_u64(ai.scouts.size());
            for (std::size_t i = 0; i < ai.scouts.size(); ++i) {
                h.add_u32(entt::to_integral(ai.scouts[i]));
                h.add_i32(ai.scout_targets[i].x);
                h.add_i32(ai.scout_targets[i].y);
            }
            h.add_u64(ai.explore_done.size());
            for (const TileCoord t : ai.explore_done) {
                h.add_i32(t.x);
                h.add_i32(t.y);
            }
            h.add_u64(ai.enemy_seen_milli.size());
            for (const std::int64_t m : ai.enemy_seen_milli) {
                h.add_u64(static_cast<std::uint64_t>(m));
            }
        }
    }
}

}  // namespace rts::sim
