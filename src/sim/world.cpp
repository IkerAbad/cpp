#include "sim/world.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <memory>
#include <utility>

#include "sim/state_hash.hpp"

namespace rts::sim {

namespace {

// Intentos por unidad para caer en una casilla transitable libre del área.
constexpr std::int32_t kSpawnAttempts = 64;
// Intentos para colocar el primer nodo de un grupo cerca de un inicio.
constexpr std::int32_t kStartNodeAttempts = 64;
constexpr std::uint32_t kPermille = 1000;

std::shared_ptr<TileMap> make_map(const WorldParams& params) {
    return std::make_shared<TileMap>(generate_map(params.map));
}

std::int32_t player_count(const WorldParams& params) {
    return std::max({1, static_cast<std::int32_t>(params.setup.starts.size()), params.demo.player + 1});
}

FVec2 tile_center(TileCoord c) noexcept {
    return {Fixed::from_int(c.x) + Fixed::from_ratio(1, 2), Fixed::from_int(c.y) + Fixed::from_ratio(1, 2)};
}

std::int32_t chebyshev(TileCoord a, TileCoord b) noexcept {
    return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y));
}

// Primer origen que cumple fits, en anillos de Chebyshev crecientes alrededor de want
// (dentro de cada anillo, orden de filas y columnas: determinista).
template <typename Fits>
std::optional<TileCoord> nearest_origin(TileCoord want, std::int32_t radius, Fits&& fits) {
    for (std::int32_t r = 0; r <= radius; ++r) {
        for (std::int32_t y = want.y - r; y <= want.y + r; ++y) {
            for (std::int32_t x = want.x - r; x <= want.x + r; ++x) {
                const TileCoord c{x, y};
                if (chebyshev(c, want) == r && fits(c)) {
                    return c;
                }
            }
        }
    }
    return std::nullopt;
}

}  // namespace

World::World(const WorldParams& params)
    : map_(make_map(params)),
      movement_(*map_, params.passable_by_terrain, params.movement),
      economy_(map_->width(), map_->height(), params.economy,
               {params.unit_types, params.building_types, params.node_types}, player_count(params)),
      combat_(map_->width(), map_->height(), params.combat, params.unit_types, params.building_types),
      fire_(params.fire, params.unit_types, params.building_types),
      supply_(params.supply, params.unit_types, params.building_types),
      medicine_(params.medicine, params.supply.ration_cost, params.unit_types, params.building_types),
      vision_(params.vision, *map_, params.unit_types, params.building_types, params.node_types, player_count(params)),
      ai_(params.ai, params.supply, params.ai_players),
      rng_(params.demo.seed) {
    setup_game(params.setup);
    spawn_demo_units(params.demo);
    movement_.commit_grid_changes(registry_);
    economy_.recount_population(registry_);
}

void World::setup_game(const SetupParams& setup) {
    if (setup.starts.empty()) {
        return;
    }
    Xoshiro256pp rng(setup.seed);
    const PassGrid& grid = movement_.grid();
    const EconomyCatalog& catalog = economy_.catalog();
    assert(setup.start_building < catalog.buildings.size());
    const std::int32_t start_size = catalog.buildings[setup.start_building].size;

    struct Start {
        std::optional<entt::entity> building;
        TileCoord center;
        std::uint32_t region = 0;  // componente del terreno donde está
    };
    std::vector<Start> starts(setup.starts.size());

    // 1. Edificio inicial de cada jugador en el sitio válido más cercano al pedido, en
    //    una región de tierra lo bastante grande (no en un islote).
    for (std::size_t p = 0; p < setup.starts.size(); ++p) {
        const auto player = static_cast<PlayerId>(p);
        economy_.player_state(player).stock = setup.start_stock;
        const TileCoord want{setup.starts[p].x - start_size / 2, setup.starts[p].y - start_size / 2};
        const auto origin = nearest_origin(want, setup.start_search_radius, [&](TileCoord o) {
            return economy_.can_place(registry_, grid, start_size, o) &&
                   std::cmp_greater_equal(grid.component_size(grid.component(o)), setup.min_start_region_tiles);
        });
        if (!origin) {
            continue;  // sin sitio: el jugador empieza sin edificio ni aldeanos
        }
        starts[p].region = grid.component(*origin);
        starts[p].center = {origin->x + start_size / 2, origin->y + start_size / 2};
        starts[p].building = economy_.place_building(registry_, movement_, player, setup.start_building, *origin, true);
    }

    // 2. Recursos alrededor de cada inicio, en su misma región.
    for (const Start& start : starts) {
        if (!start.building) {
            continue;
        }
        for (const StartNodes& group : setup.near_start) {
            assert(group.type < catalog.nodes.size());
            const std::int32_t size = catalog.nodes[group.type].size;
            const auto fits = [&](TileCoord o) {
                if (!economy_.can_place(registry_, grid, size, o)) {
                    return false;
                }
                for (std::int32_t y = o.y; y < o.y + size; ++y) {
                    for (std::int32_t x = o.x; x < o.x + size; ++x) {
                        if (grid.component({x, y}) != start.region) {
                            return false;
                        }
                    }
                }
                return true;
            };
            std::optional<TileCoord> first;
            for (std::int32_t i = 0; i < group.count; ++i) {
                std::optional<TileCoord> origin;
                if (!first) {
                    const auto span = static_cast<std::uint32_t>(2 * group.max_distance + 1);
                    for (std::int32_t attempt = 0; attempt < kStartNodeAttempts && !origin; ++attempt) {
                        // Cada extracción en su propia sentencia (orden de evaluación).
                        const std::int32_t dx = static_cast<std::int32_t>(rng.next_below(span)) - group.max_distance;
                        const std::int32_t dy = static_cast<std::int32_t>(rng.next_below(span)) - group.max_distance;
                        const TileCoord o{start.center.x + dx, start.center.y + dy};
                        if (chebyshev(o, start.center) >= group.min_distance && fits(o)) {
                            origin = o;
                        }
                    }
                } else {
                    origin = nearest_origin(*first, group.max_distance, fits);
                }
                if (!origin) {
                    break;
                }
                economy_.place_node(registry_, movement_, group.type, *origin);
                first = first ? first : origin;
            }
        }
    }

    // 3. Bosques. Una extracción por casilla de bosque, se use o no: el resultado de
    //    cada casilla no depende de las anteriores.
    if (setup.tree_density_permille > 0) {
        for (std::int32_t y = 0; y < map_->height(); ++y) {
            for (std::int32_t x = 0; x < map_->width(); ++x) {
                const TileCoord c{x, y};
                if (map_->terrain(c) != setup.forest_terrain) {
                    continue;
                }
                const bool tree = rng.next_below(kPermille) < static_cast<std::uint32_t>(setup.tree_density_permille);
                const bool clearing = std::ranges::any_of(starts, [&](const Start& s) {
                    return s.building && chebyshev(c, s.center) <= setup.clear_radius;
                });
                if (tree && !clearing && grid.terrain_passable(c) && economy_.occupant(c) == entt::null) {
                    economy_.place_node(registry_, movement_, setup.tree_type, c);
                }
            }
        }
    }

    // 4. Aldeanos iniciales alrededor del edificio inicial.
    for (std::size_t p = 0; p < starts.size(); ++p) {
        if (!starts[p].building) {
            continue;
        }
        Building& b = registry_.get<Building>(*starts[p].building);
        const Footprint f = registry_.get<Footprint>(*starts[p].building);
        for (std::int32_t i = 0; i < setup.start_units; ++i) {
            const auto tile = economy_.spawn_tile(grid, f, b.spawned);
            if (!tile) {
                break;
            }
            economy_.spawn_unit(registry_, static_cast<PlayerId>(p), setup.start_unit, tile_center(*tile));
            ++b.spawned;
        }
    }
}

void World::spawn_demo_units(const DemoParams& demo) {
    if (demo.count <= 0) {
        return;
    }
    assert(demo.area_tiles > 0);
    assert(demo.unit_type < economy_.catalog().units.size());

    const std::int32_t side = std::min({demo.area_tiles, map_->width(), map_->height()});
    const std::int32_t lo = (map_->width() - side) / 2;
    // Posiciones al azar dentro del área, solo en casillas transitables y sin repetir
    // casilla: así ninguna unidad nace solapada ni encima de agua.
    std::vector<std::uint8_t> taken(static_cast<std::size_t>(map_->tile_count()), 0);
    const PassGrid& grid = movement_.grid();
    for (std::int32_t i = 0; i < demo.count; ++i) {
        for (std::int32_t attempt = 0; attempt < kSpawnAttempts; ++attempt) {
            // Cada extracción del RNG en su propia sentencia: el orden de evaluación de
            // los argumentos de una llamada no está especificado en C++.
            const std::int32_t tx = lo + static_cast<std::int32_t>(rng_.next_below(static_cast<std::uint32_t>(side)));
            const std::int32_t ty = lo + static_cast<std::int32_t>(rng_.next_below(static_cast<std::uint32_t>(side)));
            const TileCoord t{tx, ty};
            if (!grid.passable(t) || taken[grid.index(t)] != 0) {
                continue;
            }
            taken[grid.index(t)] = 1;
            economy_.spawn_unit(registry_, demo.player, demo.unit_type, tile_center(t));
            break;
        }
    }
}

void World::issue(Command command) {
    pending_.push_back(std::move(command));
}

void World::apply_command(const Command& command) {
    // Solo las unidades del jugador que da la orden.
    std::vector<entt::entity> units;
    for (const std::uint32_t id : command.units) {
        const auto e = static_cast<entt::entity>(id);
        if (registry_.valid(e) && registry_.all_of<Unit, Position, Owner>(e) &&
            registry_.get<Owner>(e).player == command.player) {
            units.push_back(e);
        }
    }
    economy_.apply(registry_, movement_, command, units, next_order_id_, tick_);
    combat_.apply(registry_, movement_, command, units, next_order_id_, tick_);
    fire_.apply(registry_, movement_, command, units, next_order_id_, tick_);
    supply_.apply(registry_, movement_, command, units, next_order_id_, tick_);
    medicine_.apply(registry_, movement_, command, units, next_order_id_, tick_);
    if (command.type == CommandType::Move) {
        movement_.order_move(registry_, units, command.target, next_order_id_++, tick_);
    } else if (command.type == CommandType::Stop) {
        MovementSystem::stop(registry_, units);
    }
}

void World::step() {
    movement_.begin_tick();
    // Cambios de ocupación hechos fuera de un tick (preparación de escenarios, pruebas).
    movement_.commit_grid_changes(registry_);
    // La IA decide sobre el estado del principio del tick; sus órdenes van detrás de las
    // de los jugadores humanos y siguen el mismo camino (se validan igual).
    ai_orders_.clear();
    vision_.update(registry_, tick_);
    ai_.think(registry_, economy_, movement_.grid(), tick_, ai_orders_, &vision_);
    for (Command& c : ai_orders_) {
        pending_.push_back(std::move(c));
    }
    // Órdenes de este tick (o atrasadas), en orden de llegada. stable_partition conserva
    // el orden relativo de las que se quedan para ticks futuros.
    const auto rest = std::ranges::stable_partition(pending_, [this](const Command& c) { return c.tick <= tick_; });
    for (auto it = pending_.begin(); it != rest.begin(); ++it) {
        apply_command(*it);
    }
    pending_.erase(pending_.begin(), rest.begin());

    economy_.update(registry_, movement_, next_order_id_, tick_);
    supply_.update(registry_, movement_, economy_, next_order_id_, tick_);
    medicine_.update(registry_, movement_, economy_, next_order_id_, tick_);
    combat_.update(registry_, movement_, economy_, fire_, next_order_id_, tick_, &vision_);
    fire_.update(registry_, movement_, economy_, next_order_id_, tick_);
    // Edificios colocados o destruidos y nodos agotados en este tick: rejilla, HPA* y caminos.
    movement_.commit_grid_changes(registry_);
    movement_.update(registry_, tick_);
    ++tick_;
}

bool World::can_place(BuildingTypeId type, TileCoord origin) const {
    const auto& buildings = economy_.catalog().buildings;
    return type < buildings.size() && economy_.can_place(registry_, movement_.grid(), buildings[type].size, origin);
}

std::optional<std::uint32_t> World::object_at(TileCoord c) const {
    const entt::entity e = economy_.occupant(c);
    if (e == entt::null) {
        return std::nullopt;
    }
    return entt::to_integral(e);
}

std::uint32_t World::spawn_unit(PlayerId player, UnitTypeId type, TileCoord tile) {
    return entt::to_integral(economy_.spawn_unit(registry_, player, type, tile_center(tile)));
}

std::optional<std::uint32_t> World::spawn_building(PlayerId player, BuildingTypeId type, TileCoord origin,
                                                   bool complete) {
    const auto e = economy_.place_building(registry_, movement_, player, type, origin, complete);
    return e ? std::optional<std::uint32_t>(entt::to_integral(*e)) : std::nullopt;
}

std::optional<std::uint32_t> World::spawn_node(NodeTypeId type, TileCoord origin) {
    const auto e = economy_.place_node(registry_, movement_, type, origin);
    return e ? std::optional<std::uint32_t>(entt::to_integral(*e)) : std::nullopt;
}

void World::set_stock(PlayerId player, const Stock& stock) {
    economy_.player_state(player).stock = stock;
}

void World::set_hp(std::uint32_t entity, std::int32_t hp) {
    Health& h = registry_.get<Health>(static_cast<entt::entity>(entity));
    h.hp = std::clamp(hp, 1, h.max_hp);
}

std::uint64_t World::state_hash() const {
    StateHasher h;
    h.add_u32(tick_);
    for (const auto word : rng_.state()) {
        h.add_u64(word);
    }
    map_->hash_into(h);
    h.add_u32(next_order_id_);
    h.add_u64(pending_.size());
    movement_.hash_into(h);
    // El orden de iteración de una vista de EnTT es el del array denso del pool,
    // que depende solo de la secuencia de create/emplace/destroy: determinista.
    const auto units = registry_.view<const Position, const Velocity, const Unit>();
    for (const entt::entity e : units) {
        const auto& [p, v, u] = units.get<const Position, const Velocity, const Unit>(e);
        h.add_u32(entt::to_integral(e));
        h.add_fixed(p.x);
        h.add_fixed(p.y);
        h.add_fixed(v.v.x);
        h.add_fixed(v.v.y);
        h.add_u32(u.type);
        if (const MoveGoal* g = registry_.try_get<MoveGoal>(e)) {
            h.add_i32(g->tile.x);
            h.add_i32(g->tile.y);
            h.add_u32(g->order_id);
            h.add_i32(g->flow_field);
            h.add_i32(g->stuck_ticks);
            h.add_i32(g->group_size);
            h.add_u32(g->arrived ? 1U : 0U);
        }
        if (const PathFollow* f = registry_.try_get<PathFollow>(e)) {
            h.add_u64(f->waypoints.size());
            h.add_u32(f->next_waypoint);
            h.add_u64(f->segment.size());
            h.add_u32(f->next_tile);
            h.add_u32(f->waiting ? 1U : 0U);
        }
    }
    economy_.hash_into(h, registry_);
    combat_.hash_into(h, registry_);
    fire_.hash_into(h, registry_);
    supply_.hash_into(h, registry_);
    medicine_.hash_into(h, registry_);
    vision_.hash_into(h);
    ai_.hash_into(h);
    return h.value();
}

void World::write_snapshot(Snapshot& out) const {
    out.tick = tick_;
    out.map = map_;
    out.entities.clear();
    const auto view = registry_.view<const Position, const Unit>();
    view.each([&](const entt::entity e, const Position& pos, const Unit& unit) {
        SnapshotEntity s;
        s.id = entt::to_integral(e);
        s.pos = pos;
        s.type = unit.type;
        if (const Owner* o = registry_.try_get<Owner>(e)) {
            s.owner = o->player;
        }
        if (const Worker* w = registry_.try_get<Worker>(e)) {
            s.task = w->task;
            if (w->building != entt::null) {
                s.work_building = entt::to_integral(w->building);
            }
            s.carry_kind = w->carry_kind;
            s.carried = w->carried;
        }
        if (const Health* hp = registry_.try_get<Health>(e)) {
            s.hp = hp->hp;
            s.max_hp = hp->max_hp;
        }
        if (const Combatant* c = registry_.try_get<Combatant>(e)) {
            s.level = c->level;
            s.xp = c->xp;
            s.hero_name = c->hero_name;
            s.stance = c->stance;
        }
        if (const Supply* sp = registry_.try_get<Supply>(e)) {
            s.rations = sp->rations;
            s.ammo = sp->ammo;
            s.hungry = economy_.catalog().units[unit.type].supply.rations > 0 && sp->hungry();
        }
        if (const Carrier* c = registry_.try_get<Carrier>(e)) {
            s.load = c->load;
            s.convoy = c->task;
        }
        if (const Patient* p = registry_.try_get<Patient>(e)) {
            s.care_post = entt::to_integral(p->post);
            s.admitted = p->admitted;
        }
        s.reorganizing = registry_.all_of<Reorganizing>(e);
        if (const Carer* c = registry_.try_get<Carer>(e)) {
            s.tending = true;
            s.work_building = entt::to_integral(c->post);
        }
        s.tending = s.tending || s.task == WorkerTask::Nurse;
        if (vision_.enabled()) {
            s.seen_by = 0;
            for (std::size_t p = 0; p < economy_.players().size(); ++p) {
                if (vision_.sees_unit(registry_, static_cast<PlayerId>(p), e)) {
                    s.seen_by = static_cast<std::uint8_t>(s.seen_by | (1U << p));
                }
            }
        }
        out.entities.push_back(s);
    });
    out.objects.clear();
    for (const auto [e, f] : registry_.view<const Footprint>().each()) {
        SnapshotObject o;
        o.id = entt::to_integral(e);
        o.origin = f.origin;
        o.size = f.size;
        // Un edificio puede ser también un nodo (granja terminada): se presenta como
        // edificio y su cantidad restante va en amount.
        if (const Building* b = registry_.try_get<Building>(e)) {
            o.kind = ObjectKind::Building;
            o.type = b->type;
            o.owner = registry_.get<Owner>(e).player;
            o.hp = registry_.get<Health>(e).hp;
            o.progress = b->progress;
            o.complete = b->complete;
            o.burned = b->burned;
            if (const Fire* fire = registry_.try_get<Fire>(e)) {
                o.fire = fire->intensity;
            }
            if (const ProductionQueue* q = registry_.try_get<ProductionQueue>(e)) {
                o.queue = q->items;
                o.queue_progress = q->progress;
            }
            if (const ResourceNode* n = registry_.try_get<ResourceNode>(e)) {
                o.amount = n->amount;
            }
            if (const SupplyStore* st = registry_.try_get<SupplyStore>(e)) {
                o.store = st->stock;
            }
        } else if (const ResourceNode* n = registry_.try_get<ResourceNode>(e)) {
            o.kind = ObjectKind::Resource;
            o.type = n->type;
            o.amount = n->amount;
        }
        if (vision_.enabled() && o.kind == ObjectKind::Building) {
            o.seen_by = 0;
            for (std::size_t p = 0; p < economy_.players().size(); ++p) {
                if (p == o.owner || vision_.sees_footprint(static_cast<PlayerId>(p), f)) {
                    o.seen_by = static_cast<std::uint8_t>(o.seen_by | (1U << p));
                }
            }
        }
        out.objects.push_back(std::move(o));
    }
    out.players.assign(economy_.players().begin(), economy_.players().end());
    out.daylight_percent = vision_.enabled() ? vision_.daylight_percent(tick_) : kPercent;
    out.fog.clear();
    out.memory.clear();
    if (vision_.enabled()) {
        for (std::size_t p = 0; p < economy_.players().size(); ++p) {
            out.fog.push_back(vision_.fog_layer(static_cast<PlayerId>(p)));
            const auto mem = vision_.memory(static_cast<PlayerId>(p));
            out.memory.emplace_back(mem.begin(), mem.end());
        }
    }
    out.projectiles.clear();
    for (const auto [e, p] : registry_.view<const Projectile>().each()) {
        out.projectiles.push_back({p.pos.x, p.pos.y});
    }
}

}  // namespace rts::sim
