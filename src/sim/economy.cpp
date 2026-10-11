#include "sim/economy.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <utility>

#include "sim/fire.hpp"
#include "sim/state_hash.hpp"
#include "sim/supply.hpp"

namespace rts::sim {

namespace {

FVec2 tile_center(TileCoord c) noexcept {
    return {Fixed::from_int(c.x) + Fixed::from_ratio(1, 2), Fixed::from_int(c.y) + Fixed::from_ratio(1, 2)};
}

// Casillas a distancia de Chebyshev k de la huella, en orden fijo: fila de arriba de
// izquierda a derecha, columna derecha hacia abajo, fila de abajo de derecha a
// izquierda y columna izquierda hacia arriba.
template <typename Fn>
void for_each_ring_tile(const Footprint& f, std::int32_t k, Fn&& fn) {
    const std::int32_t x0 = f.origin.x - k;
    const std::int32_t y0 = f.origin.y - k;
    const std::int32_t x1 = f.origin.x + f.size - 1 + k;
    const std::int32_t y1 = f.origin.y + f.size - 1 + k;
    for (std::int32_t x = x0; x <= x1; ++x) {
        fn(TileCoord{x, y0});
    }
    for (std::int32_t y = y0 + 1; y <= y1; ++y) {
        fn(TileCoord{x1, y});
    }
    for (std::int32_t x = x1 - 1; x >= x0; --x) {
        fn(TileCoord{x, y1});
    }
    for (std::int32_t y = y1 - 1; y > y0; --y) {
        fn(TileCoord{x0, y});
    }
}

// Casilla desde la que acercarse a una huella: del primer anillo, en la componente de
// la unidad, ordenadas por distancia octil a ella (empate: índice de casilla). skip
// elige la siguiente tras un intento fallido.
std::optional<TileCoord> approach_tile(const PassGrid& grid, const Footprint& f, TileCoord from, std::int32_t skip) {
    const std::uint32_t comp = grid.component(from);
    if (comp == 0) {
        return std::nullopt;
    }
    struct Candidate {
        std::int32_t d;
        std::size_t index;
        TileCoord tile;
    };
    std::vector<Candidate> candidates;
    for_each_ring_tile(f, 1, [&](TileCoord c) {
        if (grid.component(c) == comp) {
            candidates.push_back({octile_distance(from, c), grid.index(c), c});
        }
    });
    if (candidates.empty()) {
        return std::nullopt;
    }
    std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b) {
        return a.d != b.d ? a.d < b.d : a.index < b.index;
    });
    return candidates[static_cast<std::size_t>(skip) % candidates.size()].tile;
}

bool affordable(const Stock& stock, const Stock& cost) noexcept {
    for (std::size_t r = 0; r < kResourceCount; ++r) {
        if (stock[r] < cost[r]) {
            return false;
        }
    }
    return true;
}

void add_stock(Stock& stock, const Stock& amount, std::int32_t sign) noexcept {
    for (std::size_t r = 0; r < kResourceCount; ++r) {
        stock[r] += sign * amount[r];
    }
}

void reset_movement(Worker& w) noexcept {
    w.attempts = 0;
    w.move_order = 0;
}

}  // namespace

std::int64_t distance_sq_to(const Footprint& f, FVec2 p) noexcept {
    const Fixed x0 = Fixed::from_int(f.origin.x);
    const Fixed y0 = Fixed::from_int(f.origin.y);
    const Fixed x1 = Fixed::from_int(f.origin.x + f.size);
    const Fixed y1 = Fixed::from_int(f.origin.y + f.size);
    const Fixed dx = p.x < x0 ? x0 - p.x : (p.x > x1 ? p.x - x1 : Fixed{});
    const Fixed dy = p.y < y0 ? y0 - p.y : (p.y > y1 ? p.y - y1 : Fixed{});
    return length_sq_wide({dx, dy});
}

EconomySystem::EconomySystem(std::int32_t width, std::int32_t height, const EconomyParams& params,
                             EconomyCatalog catalog, std::int32_t players)
    : width_(width),
      height_(height),
      params_(params),
      catalog_(std::move(catalog)),
      players_(static_cast<std::size_t>(std::max(players, 1))),
      occupant_(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), entt::entity{entt::null}) {}

entt::entity EconomySystem::occupant(TileCoord c) const noexcept {
    if (c.x < 0 || c.y < 0 || c.x >= width_ || c.y >= height_) {
        return entt::null;
    }
    return occupant_[tile_index(c)];
}

void EconomySystem::occupy(MovementSystem& movement, const Footprint& f, entt::entity e, bool block) {
    for (std::int32_t y = f.origin.y; y < f.origin.y + f.size; ++y) {
        for (std::int32_t x = f.origin.x; x < f.origin.x + f.size; ++x) {
            occupant_[tile_index({x, y})] = e;
            movement.set_blocked({x, y}, block);
        }
    }
}

void EconomySystem::release(MovementSystem& movement, const Footprint& f) {
    for (std::int32_t y = f.origin.y; y < f.origin.y + f.size; ++y) {
        for (std::int32_t x = f.origin.x; x < f.origin.x + f.size; ++x) {
            occupant_[tile_index({x, y})] = entt::null;
            movement.set_blocked({x, y}, false);
            movement.set_gate({x, y}, MovementSystem::kNoGate);
            movement.set_road({x, y}, 0);
        }
    }
}

bool EconomySystem::can_place(const entt::registry& registry, const PassGrid& grid, std::int32_t size,
                              TileCoord origin) const {
    if (size <= 0) {
        return false;
    }
    const Footprint f{origin, size};
    for (std::int32_t y = origin.y; y < origin.y + size; ++y) {
        for (std::int32_t x = origin.x; x < origin.x + size; ++x) {
            if (!grid.terrain_passable({x, y}) || occupant_[tile_index({x, y})] != entt::null ||
                (!unbuildable_.empty() && unbuildable_[tile_index({x, y})] != 0)) {
                return false;
            }
        }
    }
    const auto units = registry.view<const Position, const Unit>();
    for (const entt::entity e : units) {
        const Position& p = units.get<const Position>(e);
        if (f.contains(tile_of(FVec2{p.x, p.y}))) {
            return false;
        }
    }
    return true;
}

entt::entity EconomySystem::spawn_unit(entt::registry& registry, PlayerId player, UnitTypeId type, FVec2 pos) const {
    assert(type < catalog_.units.size());
    assert(player < players_.size());
    const UnitType& ut = catalog_.units[type];
    const auto e = registry.create();
    registry.emplace<Position>(e, pos.x, pos.y);
    registry.emplace<Velocity>(e);
    registry.emplace<Unit>(e, type, ut.radius, ut.speed, ut.rough_speed_percent);
    registry.emplace<Owner>(e, player);
    registry.emplace<Health>(e, ut.combat.hp, ut.combat.hp);
    registry.emplace<Combatant>(e);
    if (ut.worker) {
        registry.emplace<Worker>(e);
    }
    if (ut.supply.rations > 0 || ut.supply.ammo > 0) {
        registry.emplace<Supply>(e, ut.supply.rations, 0, 0, ut.supply.ammo);  // sale con todo
    }
    if (ut.convoy_capacity > 0) {
        registry.emplace<Carrier>(e);  // sale vacío: hay que cargarlo
    }
    return e;
}

std::optional<entt::entity> EconomySystem::place_building(entt::registry& registry, MovementSystem& movement,
                                                          PlayerId player, BuildingTypeId type, TileCoord origin,
                                                          bool complete) {
    if (type >= catalog_.buildings.size() || player >= players_.size()) {
        return std::nullopt;
    }
    const BuildingType& bt = catalog_.buildings[type];
    if (!can_place(registry, movement.grid(), bt.size, origin)) {
        return std::nullopt;
    }
    const auto e = registry.create();
    const Footprint f{origin, bt.size};
    registry.emplace<Footprint>(e, f);
    registry.emplace<Owner>(e, player);
    Building b;
    b.type = type;
    b.complete = complete;
    b.progress = complete ? bt.build_ticks : 0;
    registry.emplace<Building>(e, b);
    registry.emplace<Health>(e, complete ? bt.hp : 1, bt.hp);  // los cimientos empiezan con 1
    if (complete) {
        start_farm(registry, e, bt);  // una granja colocada ya terminada también da comida
    }
    if (!bt.trains.empty()) {
        registry.emplace<ProductionQueue>(e);
    }
    occupy(movement, f, e);
    if (bt.road_speed_percent > 0) {
        for (std::int32_t y = f.origin.y; y < f.origin.y + f.size; ++y) {
            for (std::int32_t x = f.origin.x; x < f.origin.x + f.size; ++x) {
                movement.set_blocked({x, y}, false);  // se anda por encima
                movement.set_road({x, y}, complete ? bt.road_speed_percent : 0);
            }
        }
    }
    if (bt.gate) {
        const std::int32_t gate_owner = registry.get<Owner>(e).player;
        for (std::int32_t y = f.origin.y; y < f.origin.y + f.size; ++y) {
            for (std::int32_t x = f.origin.x; x < f.origin.x + f.size; ++x) {
                movement.set_blocked({x, y}, false);
                movement.set_gate({x, y}, gate_owner);
            }
        }
    }
    return e;
}

std::optional<entt::entity> EconomySystem::place_node(entt::registry& registry, MovementSystem& movement,
                                                      NodeTypeId type, TileCoord origin) {
    if (type >= catalog_.nodes.size()) {
        return std::nullopt;
    }
    const ResourceNodeType& nt = catalog_.nodes[type];
    if (!can_place(registry, movement.grid(), nt.size, origin)) {
        return std::nullopt;
    }
    const auto e = registry.create();
    const Footprint f{origin, nt.size};
    registry.emplace<Footprint>(e, f);
    registry.emplace<ResourceNode>(e, type, nt.kind, nt.amount);
    occupy(movement, f, e);
    return e;
}

std::optional<TileCoord> EconomySystem::spawn_tile(const PassGrid& grid, const Footprint& f,
                                                   std::uint32_t index) const {
    std::vector<TileCoord> free;
    for (std::int32_t k = 1; k <= params_.spawn_search_radius; ++k) {
        free.clear();
        for_each_ring_tile(f, k, [&](TileCoord c) {
            if (grid.passable(c)) {
                free.push_back(c);
            }
        });
        if (!free.empty()) {
            return free[index % free.size()];
        }
    }
    return std::nullopt;
}

bool EconomySystem::has_room(const entt::registry& registry, entt::entity node) const {
    const Footprint& f = registry.get<Footprint>(node);
    std::int32_t assigned = 0;
    const auto workers = registry.view<const Worker>();
    for (const entt::entity e : workers) {
        const Worker& w = workers.get<const Worker>(e);
        if (w.node == node && (w.task == WorkerTask::Gather || w.task == WorkerTask::Deliver)) {
            ++assigned;
        }
    }
    return assigned < params_.gatherers_per_tile * f.size * f.size;
}

void EconomySystem::start_gather(Worker& w, entt::entity node, const Footprint& f, Resource kind) const {
    w.task = WorkerTask::Gather;
    w.node = node;
    w.gather_kind = kind;
    w.gather_area = f.origin;
    w.timer = 0;
    w.retargets = 0;
    reset_movement(w);
}

void EconomySystem::apply(entt::registry& registry, MovementSystem& movement, const Command& command,
                          std::span<const entt::entity> units, std::uint32_t& next_order_id, Tick tick) {
    const PlayerId player = command.player;
    if (player >= players_.size()) {
        return;
    }
    const auto object = static_cast<entt::entity>(command.object);
    const bool has_object = command.object != kNoObject && registry.valid(object);
    // Quien no puede cumplir la orden (un soldado ante un árbol) se limita a acercarse.
    std::vector<entt::entity> walkers;
    auto walk_to = [&](TileCoord target) {
        if (!walkers.empty()) {
            movement.order_move(registry, walkers, target, next_order_id++, tick);
        }
    };

    switch (command.type) {
        case CommandType::SetStance:
        case CommandType::Garrison:  // los aldeanos no guarnecen ni escalan: siguen con lo suyo
        case CommandType::Climb:
        case CommandType::Trade:
        case CommandType::TradeRoute:
        case CommandType::AiTakeover:
        case CommandType::Count:
            return;
        case CommandType::Move:
        case CommandType::Stop:
        case CommandType::Attack:
        case CommandType::AttackMove:
        case CommandType::Extinguish:
        case CommandType::Convoy:
        case CommandType::Treat:
            for (const entt::entity e : units) {
                if (Worker* w = registry.try_get<Worker>(e)) {
                    w->task = WorkerTask::Idle;
                    reset_movement(*w);
                }
            }
            return;

        case CommandType::Gather: {
            if (!has_object || !can_gather(registry, object, player)) {
                return;
            }
            const Resource kind = registry.get<ResourceNode>(object).kind;
            const Footprint f = registry.get<Footprint>(object);
            for (const entt::entity e : units) {
                Worker* w = registry.try_get<Worker>(e);
                if (w == nullptr) {
                    walkers.push_back(e);
                    continue;
                }
                // Con el nodo lleno, el accesible más cercano con sitio; si no hay, este.
                w->task = WorkerTask::Idle;
                entt::entity target = object;
                if (!has_room(registry, object)) {
                    const Position& p = registry.get<Position>(e);
                    const std::uint32_t comp = movement.grid().component(tile_of(FVec2{p.x, p.y}));
                    target = find_node_near(registry, movement.grid(), player, kind, f.origin, comp, object)
                                 .value_or(object);
                }
                start_gather(*w, target, registry.get<Footprint>(target), kind);
            }
            walk_to(f.origin);
            return;
        }

        case CommandType::Build: {
            // Edificio propio: en obra, se construye; terminado pero dañado o quemado (y sin
            // fuego), se repara; almacén del recurso que lleva el aldeano, se descarga allí.
            if (!has_object || !registry.all_of<Building, Owner, Footprint>(object) ||
                registry.get<Owner>(object).player != player) {
                return;
            }
            const Building& b = registry.get<Building>(object);
            const std::uint8_t accepts = catalog_.buildings[b.type].accepts;
            for (const entt::entity e : units) {
                Worker* w = registry.try_get<Worker>(e);
                if (w != nullptr && (!b.complete || needs_repair(registry, object))) {
                    w->task = WorkerTask::Build;
                    w->building = object;
                    reset_movement(*w);
                } else if (w != nullptr && w->carried > 0 && (accepts & resource_bit(w->carry_kind)) != 0) {
                    w->task = WorkerTask::Deliver;
                    w->building = object;
                    reset_movement(*w);
                } else {
                    walkers.push_back(e);
                }
            }
            walk_to(registry.get<Footprint>(object).origin);
            return;
        }

        case CommandType::SetRally: {
            if (!has_object || !registry.all_of<Building, Owner>(object) ||
                registry.get<Owner>(object).player != player ||
                catalog_.buildings[registry.get<Building>(object).type].trains.empty()) {
                return;
            }
            Building& b = registry.get<Building>(object);
            const bool clear = command.kind == kClearRally || !movement.grid().contains(command.target);
            b.rally = clear ? TileCoord{-1, -1} : command.target;
            return;
        }

        case CommandType::Tend: {
            if (!has_object || !registry.all_of<Building, Owner, Footprint>(object) ||
                registry.get<Owner>(object).player != player ||
                catalog_.buildings[registry.get<Building>(object).type].beds <= 0) {
                return;
            }
            for (const entt::entity e : units) {
                if (Worker* w = registry.try_get<Worker>(e)) {
                    w->task = WorkerTask::Nurse;
                    w->building = object;
                    reset_movement(*w);
                } else {
                    walkers.push_back(e);
                }
            }
            walk_to(registry.get<Footprint>(object).origin);
            return;
        }

        case CommandType::Demolish: {
            if (!has_object || !registry.all_of<Building, Owner, Footprint>(object) ||
                registry.get<Owner>(object).player != player) {
                return;
            }
            for (const entt::entity e : units) {
                if (Worker* w = registry.try_get<Worker>(e)) {
                    w->task = WorkerTask::Demolish;
                    w->building = object;
                    reset_movement(*w);
                } else {
                    walkers.push_back(e);
                }
            }
            walk_to(registry.get<Footprint>(object).origin);
            return;
        }

        case CommandType::Place: {
            if (command.kind >= catalog_.buildings.size()) {
                return;
            }
            const BuildingType& bt = catalog_.buildings[command.kind];
            PlayerState& ps = players_[player];
            if (!affordable(ps.stock, bt.cost) || !meets_requirements(registry, player, command.kind)) {
                return;  // sin cobrar
            }
            const auto b = place_building(registry, movement, player, command.kind, command.target, false);
            if (!b) {
                return;
            }
            add_stock(ps.stock, bt.cost, -1);
            for (const entt::entity e : units) {
                if (Worker* w = registry.try_get<Worker>(e)) {
                    w->task = WorkerTask::Build;
                    w->building = *b;
                    reset_movement(*w);
                }
            }
            return;
        }

        case CommandType::Train: {
            if (!has_object || !registry.all_of<Building, Owner, ProductionQueue>(object) ||
                registry.get<Owner>(object).player != player || command.kind >= catalog_.units.size()) {
                return;
            }
            const Building& b = registry.get<Building>(object);
            const auto& trains = catalog_.buildings[b.type].trains;
            ProductionQueue& q = registry.get<ProductionQueue>(object);
            const Stock& cost = catalog_.units[command.kind].cost;
            // Un campamento de campaña paga con su propio almacén (lo traído en convoy).
            SupplyStore* store = registry.try_get<SupplyStore>(object);
            Stock& pays = store != nullptr ? store->stock : players_[player].stock;
            if (!b.working() || std::ranges::find(trains, command.kind) == trains.end() ||
                std::cmp_greater_equal(q.items.size(), params_.queue_capacity) || !affordable(pays, cost)) {
                return;
            }
            add_stock(pays, cost, -1);  // se cobra al encolar
            q.items.push_back(command.kind);
            return;
        }

        case CommandType::Research: {
            if (!has_object || !can_research(registry, player, object, command.kind)) {
                return;
            }
            add_stock(players_[player].stock, catalog_.upgrades[command.kind].cost, -1);
            registry.emplace<Research>(object, Research{command.kind, 0});
            return;
        }

        case CommandType::CancelTrain: {
            if (has_object && registry.all_of<Owner, Research>(object) && registry.get<Owner>(object).player == player) {
                // Anular la investigación en curso: reembolso íntegro.
                add_stock(players_[player].stock, catalog_.upgrades[registry.get<Research>(object).upgrade].cost, 1);
                registry.remove<Research>(object);
                return;
            }
            if (!has_object || !registry.all_of<Owner, ProductionQueue>(object) ||
                registry.get<Owner>(object).player != player) {
                return;
            }
            ProductionQueue& q = registry.get<ProductionQueue>(object);
            if (q.items.empty()) {
                return;
            }
            SupplyStore* store = registry.try_get<SupplyStore>(object);
            add_stock(store != nullptr ? store->stock : players_[player].stock, catalog_.units[q.items.back()].cost,
                      1);  // reembolso íntegro, a quien pagó
            q.items.pop_back();
            if (q.items.empty()) {
                q.progress = 0;
            }
            return;
        }
    }
}

bool EconomySystem::researched(PlayerId p, UpgradeId u) const noexcept {
    return p < players_.size() && u < players_[p].researched.size() && players_[p].researched[u] != 0;
}

UpgradeBonus EconomySystem::upgrade_bonus(PlayerId p, ArmorClassId c) const noexcept {
    UpgradeBonus b;
    if (p >= players_.size()) {
        return b;
    }
    const auto& done = players_[p].researched;
    for (std::size_t u = 0; u < done.size(); ++u) {
        const UpgradeType& up = catalog_.upgrades[u];
        if (done[u] == 0 || (up.classes & (1U << c)) == 0) {
            continue;
        }
        b.attack_melee += up.attack_melee;
        b.attack_pierce += up.attack_pierce;
        b.armor_melee += up.armor_melee;
        b.armor_pierce += up.armor_pierce;
    }
    return b;
}

bool EconomySystem::can_research(const entt::registry& registry, PlayerId p, entt::entity b, UpgradeId u) const {
    if (u >= catalog_.upgrades.size() || p >= players_.size() || !registry.valid(b) ||
        !registry.all_of<Building, Owner>(b) || registry.get<Owner>(b).player != p || registry.all_of<Research>(b)) {
        return false;
    }
    const UpgradeType& up = catalog_.upgrades[u];
    const Building& bl = registry.get<Building>(b);
    if (bl.type != up.at || !bl.complete || bl.burned || researched(p, u) ||
        (up.requires_upgrade && !researched(p, *up.requires_upgrade))) {
        return false;
    }
    for (const auto [e, r, o] : registry.view<const Research, const Owner>().each()) {
        if (o.player == p && r.upgrade == u) {
            return false;  // ya se investiga en otro edificio
        }
    }
    return affordable(players_[p].stock, up.cost);
}

void EconomySystem::update_research(entt::registry& registry) {
    std::vector<entt::entity> done;
    for (const auto [e, r, b, o] : registry.view<Research, const Building, const Owner>().each()) {
        if (!b.complete || b.burned) {
            continue;  // quemada, la herrería no trabaja
        }
        if (++r.progress >= catalog_.upgrades[r.upgrade].research_ticks) {
            done.push_back(e);
        }
    }
    std::ranges::sort(done, {}, [](entt::entity e) { return entt::to_integral(e); });
    for (const entt::entity e : done) {
        const PlayerId p = registry.get<Owner>(e).player;
        auto& list = players_[p].researched;
        list.resize(catalog_.upgrades.size(), 0);
        list[registry.get<Research>(e).upgrade] = 1;
        registry.remove<Research>(e);
    }
}

void EconomySystem::record_loss(PlayerId victim, std::optional<PlayerId> killer) {
    if (victim < players_.size()) {
        ++players_[victim].stats.units_lost;
    }
    if (killer && *killer < players_.size() && *killer != victim) {
        ++players_[*killer].stats.enemies_killed;
    }
}

void EconomySystem::recount_population(const entt::registry& registry) {
    for (PlayerState& p : players_) {
        p.population = 0;
        p.population_cap = 0;
    }
    const auto units = registry.view<const Unit, const Owner>();
    for (const entt::entity e : units) {
        const PlayerId owner = units.get<const Owner>(e).player;
        assert(owner < players_.size());
        players_[owner].population += catalog_.units[units.get<const Unit>(e).type].population;
    }
    const auto buildings = registry.view<const Building, const Owner>();
    for (const entt::entity e : buildings) {
        const Building& b = buildings.get<const Building>(e);
        if (b.working()) {
            players_[buildings.get<const Owner>(e).player].population_cap += catalog_.buildings[b.type].population;
        }
    }
    for (PlayerState& p : players_) {
        p.stats.peak_population = std::max(p.stats.peak_population, p.population);
    }
    // Derrota: ver PlayerState.
    std::vector<std::uint8_t> present(players_.size(), 0);
    std::vector<std::uint8_t> vital(players_.size(), 0);
    for (const entt::entity e : units) {
        present[units.get<const Owner>(e).player] = 1;
    }
    for (const entt::entity e : buildings) {
        const PlayerId owner = buildings.get<const Owner>(e).player;
        present[owner] = 1;
        const Building& b = buildings.get<const Building>(e);
        if (b.complete && catalog_.buildings[b.type].vital) {
            vital[owner] = 1;
        }
    }
    for (std::size_t i = 0; i < players_.size(); ++i) {
        PlayerState& p = players_[i];
        p.population_cap = std::min(p.population_cap, params_.max_population);
        p.started = p.started || present[i] != 0;
        p.had_vital = p.had_vital || vital[i] != 0;
        const bool lost = p.had_vital ? vital[i] == 0 : (p.started && present[i] == 0);
        p.defeated = p.defeated || lost;
    }
}

void EconomySystem::update(entt::registry& registry, MovementSystem& movement, std::uint32_t& next_order_id,
                           Tick tick) {
    stats_ = EconomyTickStats{};
    recount_population(registry);
    retire_defeated(registry, movement);
    // Lista previa: las tareas crean y destruyen entidades (nodos agotados).
    scratch_.clear();
    for (const entt::entity e : registry.view<Worker>()) {
        scratch_.push_back(e);
    }
    for (const entt::entity e : scratch_) {
        update_worker(registry, movement, e, next_order_id, tick);
    }
    update_production(registry, movement);
    update_research(registry);
}

void EconomySystem::update_worker(entt::registry& registry, MovementSystem& movement, entt::entity e,
                                  std::uint32_t& next_order_id, Tick tick) {
    // La referencia es estable: ninguna tarea añade ni quita componentes Worker.
    Worker& w = registry.get<Worker>(e);
    switch (w.task) {
        case WorkerTask::Idle:
            break;
        case WorkerTask::Gather:
            step_gather(registry, movement, e, w, next_order_id, tick);
            break;
        case WorkerTask::Deliver:
            step_deliver(registry, movement, e, w, next_order_id, tick);
            break;
        case WorkerTask::Build:
            step_build(registry, movement, e, w, next_order_id, tick);
            break;
        case WorkerTask::Demolish:
            step_demolish(registry, movement, e, w, next_order_id, tick);
            break;
        case WorkerTask::Nurse:
            step_nurse(registry, movement, e, w, next_order_id, tick);
            break;
    }
}

void EconomySystem::step_nurse(entt::registry& registry, MovementSystem& movement, entt::entity e, Worker& w,
                               std::uint32_t& next_order_id, Tick tick) {
    const PlayerId player = registry.get<Owner>(e).player;
    if (!registry.valid(w.building) || !registry.all_of<Building, Owner, Footprint>(w.building) ||
        registry.get<Owner>(w.building).player != player || !registry.get<Building>(w.building).working() ||
        catalog_.buildings[registry.get<Building>(w.building).type].beds <= 0) {
        w.task = WorkerTask::Idle;  // puesto perdido o inutilizado
        return;
    }
    const Footprint f = registry.get<Footprint>(w.building);
    if (approach(registry, movement, e, w, f, next_order_id, tick) == Approach::Failed) {
        w.task = WorkerTask::Idle;
    }
    // Al alcance se queda: los cuidados los aplica el sistema de sanidad.
}

EconomySystem::Approach EconomySystem::approach(entt::registry& registry, MovementSystem& movement, entt::entity e,
                                                Worker& w, const Footprint& target, std::uint32_t& next_order_id,
                                                Tick tick) {
    const Position& p = registry.get<Position>(e);
    const FVec2 pos{p.x, p.y};
    const Fixed reach = registry.get<Unit>(e).radius + params_.interact_range;
    if (distance_sq_to(target, pos) <= mul_wide(reach, reach)) {
        // Al alcance: se detiene si iba en un desplazamiento propio.
        if (w.move_order != 0) {
            const MoveGoal* g = registry.try_get<MoveGoal>(e);
            if (g != nullptr && g->order_id == w.move_order) {
                registry.remove<MoveGoal, PathFollow>(e);
            }
        }
        reset_movement(w);
        return Approach::InReach;
    }
    if (w.move_order != 0) {
        const MoveGoal* g = registry.try_get<MoveGoal>(e);
        if (g != nullptr && g->order_id == w.move_order && !g->arrived) {
            return Approach::Moving;
        }
        ++w.attempts;  // el desplazamiento anterior terminó sin llegar al alcance
    }
    const auto tile = w.attempts < params_.approach_attempts
                          ? approach_tile(movement.grid(), target, tile_of(pos), w.attempts)
                          : std::nullopt;
    if (!tile) {
        reset_movement(w);
        return Approach::Failed;
    }
    w.move_order = next_order_id++;
    const std::array<entt::entity, 1> one{e};
    movement.order_move(registry, one, *tile, w.move_order, tick);
    ++stats_.approach_moves;
    return Approach::Moving;
}

void EconomySystem::step_gather(entt::registry& registry, MovementSystem& movement, entt::entity e, Worker& w,
                                std::uint32_t& next_order_id, Tick tick) {
    const UnitType& ut = catalog_.units[registry.get<Unit>(e).type];
    auto go_deliver = [&w] {
        w.task = WorkerTask::Deliver;
        w.building = entt::null;
        reset_movement(w);
    };
    if (w.carried >= ut.carry_capacity) {
        go_deliver();
        return;
    }
    const Position& pos = registry.get<Position>(e);
    const std::uint32_t component = movement.grid().component(tile_of(FVec2{pos.x, pos.y}));
    const PlayerId me = registry.get<Owner>(e).player;
    if (!can_gather(registry, w.node, me)) {
        // Agotado (por él o por otro): otro nodo del mismo recurso cerca del anterior.
        const auto next =
            find_node_near(registry, movement.grid(), me, w.gather_kind, w.gather_area, component, entt::null);
        if (!next) {
            if (w.carried > 0) {
                go_deliver();
            } else {
                w.task = WorkerTask::Idle;
            }
            return;
        }
        w.node = *next;
        w.gather_area = registry.get<Footprint>(*next).origin;
        reset_movement(w);
    }
    const Footprint f = registry.get<Footprint>(w.node);
    switch (approach(registry, movement, e, w, f, next_order_id, tick)) {
        case Approach::Moving:
            return;
        case Approach::Failed: {
            // Inalcanzable (rodeado de otros aldeanos o aislado): otro nodo cercano.
            const auto other = ++w.retargets <= params_.approach_attempts
                                   ? find_node_near(registry, movement.grid(), me, w.gather_kind, f.origin,
                                                    component, w.node)
                                   : std::nullopt;
            if (!other) {
                if (w.carried > 0) {
                    go_deliver();
                } else {
                    w.task = WorkerTask::Idle;
                }
                return;
            }
            w.node = *other;
            w.gather_area = registry.get<Footprint>(*other).origin;
            return;
        }
        case Approach::InReach:
            break;
    }
    ResourceNode& rn = registry.get<ResourceNode>(w.node);
    if (w.carried > 0 && w.carry_kind != rn.kind) {
        w.carried = 0;  // lo que llevaba de otro recurso se pierde, como en el género
    }
    if (slowed_by_hunger(registry, e, tick)) {
        return;
    }
    if (++w.timer < params_.gather_ticks[resource_index(rn.kind)]) {
        return;
    }
    w.timer = 0;
    --rn.amount;
    ++w.carried;
    w.carry_kind = rn.kind;
    w.retargets = 0;
    ++stats_.gathered;
    if (rn.amount <= 0) {
        deplete(registry, movement, w.node);
    }
    if (w.carried >= ut.carry_capacity) {
        go_deliver();
    }
}

bool EconomySystem::slowed_by_hunger(const entt::registry& registry, entt::entity e, Tick tick) const {
    const Supply* s = registry.try_get<Supply>(e);
    return s != nullptr && catalog_.units[registry.get<Unit>(e).type].supply.rations > 0 && s->hungry() &&
           !works_this_tick(tick, params_.hungry_work_percent);
}

void EconomySystem::step_deliver(entt::registry& registry, MovementSystem& movement, entt::entity e, Worker& w,
                                 std::uint32_t& next_order_id, Tick tick) {
    auto resume = [&w] {
        // Vuelve a su nodo (o a buscar otro cerca); sin haber recogido nunca, queda ocioso.
        w.task = w.gather_area.x >= 0 ? WorkerTask::Gather : WorkerTask::Idle;
        w.timer = 0;
        reset_movement(w);
    };
    if (w.carried <= 0) {
        resume();
        return;
    }
    const PlayerId player = registry.get<Owner>(e).player;
    const auto valid_dropoff = [&](entt::entity b) {
        if (!registry.valid(b) || !registry.all_of<Building, Owner, Footprint>(b)) {
            return false;
        }
        const Building& bd = registry.get<Building>(b);
        return registry.get<Owner>(b).player == player && bd.working() &&
               (catalog_.buildings[bd.type].accepts & resource_bit(w.carry_kind)) != 0;
    };
    if (!valid_dropoff(w.building)) {
        const Position& p = registry.get<Position>(e);
        const auto d = nearest_dropoff(registry, player, w.carry_kind, tile_of(FVec2{p.x, p.y}));
        if (!d) {
            w.task = WorkerTask::Idle;  // sin almacén: espera con la carga
            return;
        }
        w.building = *d;
        reset_movement(w);
    }
    const Footprint f = registry.get<Footprint>(w.building);
    switch (approach(registry, movement, e, w, f, next_order_id, tick)) {
        case Approach::Moving:
            return;
        case Approach::Failed:
            w.task = WorkerTask::Idle;
            return;
        case Approach::InReach:
            break;
    }
    players_[player].stock[resource_index(w.carry_kind)] += w.carried;
    players_[player].stats.gathered[resource_index(w.carry_kind)] += w.carried;
    stats_.delivered += w.carried;
    w.carried = 0;
    resume();
}

void EconomySystem::step_build(entt::registry& registry, MovementSystem& movement, entt::entity e, Worker& w,
                               std::uint32_t& next_order_id, Tick tick) {
    if (!registry.valid(w.building) || !registry.all_of<Building, Footprint>(w.building) ||
        (registry.get<Building>(w.building).complete && !needs_repair(registry, w.building))) {
        w.task = WorkerTask::Idle;
        return;
    }
    const Footprint f = registry.get<Footprint>(w.building);
    switch (approach(registry, movement, e, w, f, next_order_id, tick)) {
        case Approach::Moving:
            return;
        case Approach::Failed:
            w.task = WorkerTask::Idle;
            return;
        case Approach::InReach:
            break;
    }
    if (slowed_by_hunger(registry, e, tick)) {
        return;
    }
    // Cada constructor al alcance aporta un tick de trabajo: n constructores, n veces
    // más rápido (lineal; el género suele dar rendimientos decrecientes).
    Building& b = registry.get<Building>(w.building);
    const BuildingType& bt = catalog_.buildings[b.type];
    if (b.complete) {
        repair(registry, w.building, b, bt);
        return;
    }
    // La obra suma vida en proporción al trabajo (el daño recibido se conserva).
    Health& health = registry.get<Health>(w.building);
    const auto before = std::int64_t{bt.hp} * b.progress / std::max(bt.build_ticks, 1);
    ++b.progress;
    const auto after = std::int64_t{bt.hp} * b.progress / std::max(bt.build_ticks, 1);
    health.hp = std::min(health.max_hp, health.hp + static_cast<std::int32_t>(after - before));
    if (b.progress >= bt.build_ticks) {
        b.complete = true;
        start_farm(registry, w.building, bt);
        if (bt.road_speed_percent > 0) {
            const Footprint& road = registry.get<Footprint>(w.building);
            for (std::int32_t y = road.origin.y; y < road.origin.y + road.size; ++y) {
                for (std::int32_t x = road.origin.x; x < road.origin.x + road.size; ++x) {
                    movement.set_road({x, y}, bt.road_speed_percent);  // terminado: ya se corre
                }
            }
        }
    }
}

bool EconomySystem::meets_requirements(const entt::registry& registry, PlayerId player, BuildingTypeId type) const {
    for (const BuildingTypeId needed : catalog_.buildings[type].required) {
        bool found = false;
        for (const auto [e, b, owner] : registry.view<const Building, const Owner>().each()) {
            if (owner.player == player && b.type == needed && b.complete) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

bool EconomySystem::needs_repair(const entt::registry& registry, entt::entity building) {
    const Building& b = registry.get<Building>(building);
    const Health& h = registry.get<Health>(building);
    return b.complete && (b.burned || h.hp < h.max_hp) && !registry.all_of<Fire>(building);
}

void EconomySystem::repair(entt::registry& registry, entt::entity building, Building& b, const BuildingType& bt) {
    // Al ritmo de la construcción; cuesta madera en proporción a la vida devuelta
    // (repair_cost_percent del coste en madera del edificio entero). Sin madera, espera.
    Health& health = registry.get<Health>(building);
    const std::int32_t gain = std::min(std::max(bt.hp / std::max(bt.build_ticks, 1), 1), health.max_hp - health.hp);
    PlayerState& ps = players_[registry.get<Owner>(building).player];
    const auto wood = resource_index(Resource::Wood);
    const std::int64_t scale = std::int64_t{kPercent} * std::max(health.max_hp, 1);
    const std::int64_t acc = b.repair_acc + std::int64_t{gain} * bt.cost[wood] * params_.repair_cost_percent;
    const auto pay = static_cast<std::int32_t>(acc / scale);
    if (ps.stock[wood] < pay) {
        return;
    }
    ps.stock[wood] -= pay;
    b.repair_acc = acc % scale;
    health.hp += gain;
    if (health.hp >= health.max_hp) {
        b.burned = false;  // tejado e interior rehechos
        b.repair_acc = 0;
    }
}

void EconomySystem::start_farm(entt::registry& registry, entt::entity building, const BuildingType& bt) {
    if (bt.farm_food > 0) {
        registry.emplace<ResourceNode>(building, NodeTypeId{0}, Resource::Food, bt.farm_food);
    }
}

bool EconomySystem::can_gather(const entt::registry& registry, entt::entity node, PlayerId player) {
    if (!registry.valid(node) || !registry.all_of<ResourceNode, Footprint>(node)) {
        return false;
    }
    const Owner* owner = registry.try_get<Owner>(node);
    return owner == nullptr || owner->player == player;
}

std::optional<entt::entity> EconomySystem::find_node_near(const entt::registry& registry, const PassGrid& grid,
                                                          PlayerId player, Resource kind, TileCoord center,
                                                          std::uint32_t component, entt::entity exclude) const {
    if (center.x < 0 || component == 0) {
        return std::nullopt;
    }
    // Solo cuenta un nodo con sitio (gatherers_per_tile) y con alguna casilla vecina en
    // la componente del aldeano: en un bosque denso, los árboles del interior no se
    // pueden talar hasta abrir paso.
    const auto accessible = [&](entt::entity o) {
        bool found = false;
        for_each_ring_tile(registry.get<Footprint>(o), 1, [&](TileCoord c) {
            found = found || grid.component(c) == component;
        });
        return found;
    };
    // Anillos de Chebyshev crecientes; en cada uno, el de menor distancia octil al
    // centro y, a igualdad, el primero en orden de recorrido.
    for (std::int32_t r = 0; r <= params_.retarget_radius_tiles; ++r) {
        entt::entity best = entt::null;
        std::int32_t best_d = std::numeric_limits<std::int32_t>::max();
        auto consider = [&](TileCoord c) {
            const entt::entity o = occupant(c);
            if (o == entt::null || o == exclude) {
                return;
            }
            const ResourceNode* rn = registry.try_get<ResourceNode>(o);
            if (rn == nullptr || rn->kind != kind || !can_gather(registry, o, player)) {
                return;
            }
            const std::int32_t d = octile_distance(center, c);
            if (d < best_d && accessible(o) && has_room(registry, o)) {
                best_d = d;
                best = o;
            }
        };
        if (r == 0) {
            consider(center);
        } else {
            for_each_ring_tile(Footprint{center, 1}, r, consider);
        }
        if (best != entt::null) {
            return best;
        }
    }
    return std::nullopt;
}

std::optional<entt::entity> EconomySystem::nearest_dropoff(const entt::registry& registry, PlayerId player,
                                                           Resource kind, TileCoord from) const {
    std::optional<entt::entity> best;
    std::int32_t best_d = std::numeric_limits<std::int32_t>::max();
    for (const auto [e, b, owner, f] : registry.view<const Building, const Owner, const Footprint>().each()) {
        if (owner.player != player || !b.working() || (catalog_.buildings[b.type].accepts & resource_bit(kind)) == 0) {
            continue;
        }
        const std::int32_t d = octile_distance(from, clamp_to(f, from));
        if (d < best_d || (d == best_d && best && entt::to_integral(e) < entt::to_integral(*best))) {
            best_d = d;
            best = e;
        }
    }
    return best;
}

void EconomySystem::remove_building(entt::registry& registry, MovementSystem& movement, entt::entity building,
                                    bool rubble) {
    const Footprint f = registry.get<Footprint>(building);
    const BuildingType& bt = catalog_.buildings[registry.get<Building>(building).type];
    if (const Owner* o = registry.try_get<Owner>(building); o != nullptr && o->player < players_.size()) {
        ++players_[o->player].stats.buildings_lost;
    }
    release(movement, f);
    registry.destroy(building);
    if (!rubble) {
        return;
    }
    const bool stone = bt.material == Material::Stone;
    const Resource kind = stone ? Resource::Stone : Resource::Wood;
    const std::int32_t amount = bt.cost[resource_index(kind)] * params_.salvage_percent / kPercent;
    if (amount <= 0) {
        return;
    }
    // Escombros del tamaño del solar, con la cantidad que dio el edificio. Los de un muro
    // o una puerta son una brecha: no cierran el paso.
    const bool breach = bt.climbable || bt.gate;
    const NodeTypeId type = breach ? (stone ? params_.breach_stone : params_.breach_wood)
                                   : (stone ? params_.rubble_stone : params_.rubble_wood);
    const auto e = registry.create();
    registry.emplace<Footprint>(e, f);
    registry.emplace<ResourceNode>(e, type, kind, amount);
    occupy(movement, f, e, !breach);
}

void EconomySystem::step_demolish(entt::registry& registry, MovementSystem& movement, entt::entity e, Worker& w,
                                  std::uint32_t& next_order_id, Tick tick) {
    if (!registry.valid(w.building) || !registry.all_of<Building, Footprint, Health>(w.building)) {
        w.task = WorkerTask::Idle;
        return;
    }
    const Footprint f = registry.get<Footprint>(w.building);
    switch (approach(registry, movement, e, w, f, next_order_id, tick)) {
        case Approach::Moving:
            return;
        case Approach::Failed:
            w.task = WorkerTask::Idle;
            return;
        case Approach::InReach:
            break;
    }
    if (slowed_by_hunger(registry, e, tick)) {
        return;
    }
    // Desmontar va al ritmo de construir: cada aldeano quita la vida que pondría.
    const BuildingType& bt = catalog_.buildings[registry.get<Building>(w.building).type];
    Health& health = registry.get<Health>(w.building);
    health.hp -= std::max(bt.hp / std::max(bt.build_ticks, 1), 1);
    if (health.hp <= 0) {
        remove_building(registry, movement, w.building, true);
        w.task = WorkerTask::Idle;
        w.building = entt::null;
    }
}

void EconomySystem::deplete(entt::registry& registry, MovementSystem& movement, entt::entity node) {
    release(movement, registry.get<Footprint>(node));
    registry.destroy(node);
    ++stats_.nodes_depleted;
}

void EconomySystem::retire_defeated(entt::registry& registry, MovementSystem& movement) {
    if (std::ranges::none_of(players_, &PlayerState::defeated)) {
        return;
    }
    // Lista previa en el orden de la vista: destruir mientras se recorre la invalida.
    scratch_.clear();
    for (const auto [e, owner] : registry.view<const Owner>().each()) {
        if (players_[owner.player].defeated) {
            scratch_.push_back(e);
        }
    }
    for (const entt::entity e : scratch_) {
        if (registry.all_of<Footprint>(e)) {
            remove_building(registry, movement, e);
        } else {
            registry.destroy(e);
        }
    }
}

void EconomySystem::update_production(entt::registry& registry, const MovementSystem& movement) {
    scratch_.clear();
    for (const entt::entity e : registry.view<Building, ProductionQueue>()) {
        scratch_.push_back(e);
    }
    for (const entt::entity e : scratch_) {
        // Referencias estables: crear una unidad no toca Building ni ProductionQueue.
        Building& b = registry.get<Building>(e);
        ProductionQueue& q = registry.get<ProductionQueue>(e);
        if (!b.working() || q.items.empty()) {
            continue;
        }
        const PlayerId player = registry.get<Owner>(e).player;
        const UnitTypeId type = q.items.front();
        const UnitType& ut = catalog_.units[type];
        PlayerState& ps = players_[player];
        if (ps.population + ut.population > ps.population_cap) {
            continue;  // sin plazas: la cola se detiene, no se pierde nada
        }
        if (q.progress < ut.train_ticks) {
            ++q.progress;
        }
        if (q.progress < ut.train_ticks) {
            continue;
        }
        const auto tile = spawn_tile(movement.grid(), registry.get<Footprint>(e), b.spawned);
        if (!tile) {
            continue;  // rodeado: la unidad espera lista hasta que haya sitio
        }
        const entt::entity unit = spawn_unit(registry, player, type, tile_center(*tile));
        if (b.rally.x >= 0) {
            registry.emplace<Waypoints>(unit, std::vector<TileCoord>{b.rally});  // al punto de reunión
        }
        ++b.spawned;
        ps.population += ut.population;
        ++ps.stats.units_trained;
        q.items.erase(q.items.begin());
        q.progress = 0;
        ++stats_.units_trained;
    }
}

void EconomySystem::hash_into(StateHasher& h, const entt::registry& registry) const {
    h.add_u64(players_.size());
    for (const PlayerState& p : players_) {
        for (const std::int32_t s : p.stock) {
            h.add_i32(s);
        }
        h.add_i32(p.population);
        h.add_i32(p.population_cap);
        h.add_u32(p.started ? 1U : 0U);
        h.add_u32(p.had_vital ? 1U : 0U);
        h.add_u32(p.defeated ? 1U : 0U);
        if (!p.researched.empty()) {
            h.add_bytes(p.researched);
        }
    }
    for (const auto [e, r] : registry.view<const Research>().each()) {
        h.add_u32(entt::to_integral(e));
        h.add_u32(r.upgrade);
        h.add_i32(r.progress);
    }
    // La ocupación de la rejilla se deriva de las huellas: basta con hashear estas.
    for (const auto [e, o] : registry.view<const Owner>().each()) {
        h.add_u32(entt::to_integral(e));
        h.add_u32(o.player);
    }
    for (const auto [e, f] : registry.view<const Footprint>().each()) {
        h.add_u32(entt::to_integral(e));
        h.add_i32(f.origin.x);
        h.add_i32(f.origin.y);
        h.add_i32(f.size);
    }
    for (const auto [e, n] : registry.view<const ResourceNode>().each()) {
        h.add_u32(entt::to_integral(e));
        h.add_u32(n.type);
        h.add_u32(static_cast<std::uint32_t>(n.kind));
        h.add_i32(n.amount);
    }
    for (const auto [e, b] : registry.view<const Building>().each()) {
        h.add_u32(entt::to_integral(e));
        h.add_u32(b.type);
        h.add_i32(b.progress);
        h.add_u32(b.complete ? 1U : 0U);
        h.add_u32(b.burned ? 1U : 0U);
        h.add_u64(static_cast<std::uint64_t>(b.repair_acc));
        h.add_u32(b.spawned);
        if (b.rally.x >= 0) {  // solo si lo hay: sin él, el hash es el de siempre
            h.add_i32(b.rally.x);
            h.add_i32(b.rally.y);
        }
    }
    for (const auto [e, q] : registry.view<const ProductionQueue>().each()) {
        h.add_u32(entt::to_integral(e));
        h.add_u64(q.items.size());
        for (const UnitTypeId t : q.items) {
            h.add_u32(t);
        }
        h.add_i32(q.progress);
    }
    for (const auto [e, w] : registry.view<const Worker>().each()) {
        h.add_u32(entt::to_integral(e));
        h.add_u32(static_cast<std::uint32_t>(w.task));
        h.add_u32(entt::to_integral(w.node));
        h.add_u32(entt::to_integral(w.building));
        h.add_u32(static_cast<std::uint32_t>(w.gather_kind));
        h.add_i32(w.gather_area.x);
        h.add_i32(w.gather_area.y);
        h.add_u32(static_cast<std::uint32_t>(w.carry_kind));
        h.add_i32(w.carried);
        h.add_i32(w.timer);
        h.add_i32(w.attempts);
        h.add_i32(w.retargets);
        h.add_u32(w.move_order);
    }
}

}  // namespace rts::sim
