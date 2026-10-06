#include "sim/movement.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <limits>

#include "sim/economy.hpp"
#include "sim/formation.hpp"
#include "sim/state_hash.hpp"

namespace rts::sim {

namespace {

// Vectores unitarios en 16.16 para las 8 direcciones candidatas. 46341 = round(65536/sqrt(2)).
constexpr std::int32_t kOne = Fixed::kOneRaw;
constexpr std::int32_t kDiag = 46341;
constexpr std::array<std::array<std::int32_t, 2>, 8> kUnitDirs{{
    {kOne, 0},
    {kDiag, kDiag},
    {0, kOne},
    {-kDiag, kDiag},
    {-kOne, 0},
    {-kDiag, -kDiag},
    {0, -kOne},
    {kDiag, -kDiag},
}};
// Candidatas: la deseada, parado, 8 direcciones a velocidad máxima y 8 a media.
constexpr std::size_t kCandidates = 2 + 2 * kUnitDirs.size();
constexpr std::int32_t kWideShift = 16;  // 32.32 -> 16.16

FVec2 dir_scaled(std::size_t k, Fixed len) noexcept {
    return {Fixed::from_raw(kUnitDirs[k][0]) * len, Fixed::from_raw(kUnitDirs[k][1]) * len};
}

// Radio de un racimo compacto de n discos de radio r: el empaquetado hexagonal da
// ~0,53 * 2r * sqrt(n); se deja 0,6 * 2r * sqrt(n) = 2r * isqrt(36 n) / 10.
Fixed cluster_radius(Fixed r, std::int32_t n) noexcept {
    const auto sqrt_36n = static_cast<std::int64_t>(isqrt(std::uint64_t{36} * static_cast<std::uint64_t>(n)));
    return Fixed::from_raw(static_cast<std::int32_t>(std::int64_t{(r * 2).raw()} * sqrt_36n / 10));
}

}  // namespace

MovementSystem::MovementSystem(const TileMap& map, std::span<const std::uint8_t> passable_by_terrain,
                               const MovementParams& params)
    : params_(params),
      grid_(map, passable_by_terrain),
      search_(map.width(), map.height()),
      hpa_(grid_, params.hpa, search_) {
    assert(params.max_neighbors > 0);
    width_ = map.width();
    if (!params.speed_percent_by_terrain.empty()) {
        tile_speed_.resize(static_cast<std::size_t>(map.width()) * static_cast<std::size_t>(map.height()), kPercent);
        for (std::int32_t y = 0; y < map.height(); ++y) {
            for (std::int32_t x = 0; x < map.width(); ++x) {
                const TerrainId t = map.terrain({x, y});
                if (t < params.speed_percent_by_terrain.size()) {
                    tile_speed_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
                                static_cast<std::size_t>(x)] = params.speed_percent_by_terrain[t];
                }
            }
        }
    }
    assert(params.time_horizon_ticks > 0);
    fields_.resize(static_cast<std::size_t>(std::max(params.flow_field_cache_size, 1)));
    dirty_sectors_.assign(hpa_.sector_count(), 0);
}

std::int32_t MovementSystem::terrain_speed_percent(FVec2 pos) const noexcept {
    if (tile_speed_.empty()) {
        return kPercent;
    }
    const TileCoord t = tile_of(pos);
    const std::int32_t height = static_cast<std::int32_t>(tile_speed_.size() / static_cast<std::size_t>(width_));
    const std::int32_t x = std::clamp(t.x, 0, width_ - 1);
    const std::int32_t y = std::clamp(t.y, 0, height - 1);
    return tile_speed_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x)];
}

Fixed MovementSystem::effective_speed(const entt::registry& registry, entt::entity e, const Unit& unit,
                                      FVec2 pos) const noexcept {
    std::int64_t percent = terrain_speed_percent(pos);
    if (percent < kPercent) {
        percent = percent * unit.rough_speed_percent / kPercent;  // fuera de llano
    }
    if (const Fatigue* f = registry.try_get<Fatigue>(e)) {
        percent = percent * f->speed_percent / kPercent;  // cansancio y paso
    }
    if (const Formation* fm = registry.try_get<Formation>(e); fm != nullptr && fm->active) {
        percent = percent * fm->speed_percent / kPercent;  // formación
    }
    if (percent == kPercent) {
        return unit.speed;
    }
    return Fixed::from_raw(static_cast<std::int32_t>(std::int64_t{unit.speed.raw()} * percent / kPercent));
}

void MovementSystem::set_gate(TileCoord c, std::int32_t owner) {
    if (!grid_.contains(c)) {
        return;
    }
    if (gate_owner_.empty()) {
        if (owner == kNoGate) {
            return;
        }
        gate_owner_.assign(static_cast<std::size_t>(grid_.width()) * static_cast<std::size_t>(grid_.height()), kNoGate);
    }
    gate_owner_[grid_.index(c)] = owner;
}

bool MovementSystem::can_enter(TileCoord c, std::int32_t owner) const noexcept {
    if (!grid_.passable(c)) {
        return false;
    }
    if (gate_owner_.empty()) {
        return true;
    }
    const std::int32_t gate = gate_owner_[grid_.index(c)];
    return gate == kNoGate || gate == owner;
}

void MovementSystem::set_blocked(TileCoord c, bool blocked) {
    if (grid_.blocked(c) == blocked) {
        return;
    }
    grid_.set_blocked(c, blocked);
    dirty_sectors_[static_cast<std::size_t>(hpa_.sector_of(c))] = 1;
    grid_dirty_ = true;
}

void MovementSystem::commit_grid_changes(entt::registry& registry) {
    if (!grid_dirty_) {
        return;
    }
    grid_.relabel_components();
    stats_.sectors_rebuilt += hpa_.rebuild(grid_, dirty_sectors_, search_);

    // Campos de flujo cuyo pasillo toca un sector cambiado: el mismo Dijkstra, otra vez.
    for (CachedField& cf : fields_) {
        if (!cf.field) {
            continue;
        }
        bool touched = false;
        for (std::size_t sct = 0; sct < cf.corridor.size() && !touched; ++sct) {
            touched = cf.corridor[sct] != 0 && dirty_sectors_[sct] != 0;
        }
        if (touched) {
            const TileCoord goal = cf.field->goal();
            cf.field = std::make_unique<FlowField>(grid_, goal, SectorMask{cf.corridor, hpa_.sector_size(), hpa_.sectors_w()},
                                                   search_);
            ++stats_.flow_fields_built;
        }
    }

    // Caminos en curso que ahora pisan una casilla bloqueada: a replanificar.
    for (const auto [e, follow] : registry.view<PathFollow>().each()) {
        if (follow.waiting) {
            continue;
        }
        bool blocked = false;
        for (std::size_t i = follow.next_tile; i < follow.segment.size() && !blocked; ++i) {
            blocked = !grid_.passable(follow.segment[i]);
        }
        for (std::size_t i = follow.next_waypoint; i < follow.waypoints.size() && !blocked; ++i) {
            blocked = !grid_.passable(follow.waypoints[i]);
        }
        if (blocked) {
            enqueue_path(e, follow);
            ++stats_.paths_invalidated;
        }
    }
    std::ranges::fill(dirty_sectors_, std::uint8_t{0});
    grid_dirty_ = false;
}

const FlowField* MovementSystem::flow_field_of(const MoveGoal& goal) const noexcept {
    if (goal.flow_field < 0) {
        return nullptr;
    }
    const CachedField& cf = fields_[static_cast<std::size_t>(goal.flow_field)];
    return cf.order_id == goal.order_id ? cf.field.get() : nullptr;
}

void MovementSystem::enqueue_path(entt::entity e, PathFollow& follow) {
    follow = PathFollow{};
    follow.waiting = true;
    path_queue_.push_back(e);
}

void MovementSystem::stop(entt::registry& registry, std::span<const entt::entity> units) {
    for (const entt::entity e : units) {
        registry.remove<MoveGoal, PathFollow>(e);
    }
}

void MovementSystem::order_move(entt::registry& registry, std::span<const entt::entity> units, TileCoord target_in,
                                std::uint32_t order_id, Tick tick) {
    const TileCoord target{std::clamp(target_in.x, 0, grid_.width() - 1),
                           std::clamp(target_in.y, 0, grid_.height() - 1)};
    struct Resolved {
        entt::entity e;
        TileCoord goal;
    };
    std::vector<Resolved> resolved;
    for (const entt::entity e : units) {
        if (registry.get<Unit>(e).speed.raw() <= 0) {
            continue;  // inmóvil (un trabuquete montado): no recibe desplazamientos
        }
        const Position& p = registry.get<Position>(e);
        const std::uint32_t comp = grid_.component(tile_of(FVec2{p.x, p.y}));
        // Si el destino no es alcanzable desde aquí (agua, otra isla), la casilla
        // alcanzable más cercana dentro del radio configurado.
        const auto goal = grid_.nearest_in_component(target, comp, params_.retarget_radius_tiles);
        if (goal) {
            resolved.push_back({e, *goal});
        } else {
            // Sin destino alcanzable: la orden nueva sustituye a la anterior igualmente,
            // así que se detiene en vez de seguir hacia su destino viejo.
            registry.remove<MoveGoal, PathFollow>(e);
        }
    }
    if (resolved.empty()) {
        return;
    }

    // Grupo grande con el mismo destino: un campo de flujo compartido.
    std::int32_t slot = -1;
    const TileCoord flow_goal = resolved.front().goal;
    std::vector<entt::entity> group;
    for (const Resolved& r : resolved) {
        if (r.goal == flow_goal) {
            group.push_back(r.e);
        }
    }
    if (static_cast<std::int32_t>(group.size()) >= params_.flow_field_min_group) {
        slot = build_flow_field(registry, group, flow_goal, order_id, tick);
    }

    // Huecos de llegada: cada unidad va al destino más su desplazamiento respecto al
    // centroide del grupo, comprimido para caber en el radio de un racimo compacto.
    // Conserva la disposición relativa y evita que todas peleen por el mismo punto.
    struct GroupShape {
        TileCoord goal;
        FVec2 centroid;
        Fixed scale_num;  // escala = scale_num / scale_den (<= 1)
        Fixed scale_den;
        std::int32_t size = 0;
    };
    std::vector<GroupShape> shapes;
    for (const Resolved& r : resolved) {
        if (std::ranges::any_of(shapes, [&](const GroupShape& g) { return g.goal == r.goal; })) {
            continue;
        }
        GroupShape shape{r.goal, {}, Fixed::from_int(1), Fixed::from_int(1), 0};
        std::int64_t sx = 0;
        std::int64_t sy = 0;
        Fixed max_radius;
        for (const Resolved& o : resolved) {
            if (o.goal == r.goal) {
                const Position& p = registry.get<Position>(o.e);
                sx += p.x.raw();
                sy += p.y.raw();
                ++shape.size;
                const Fixed rad = registry.get<Unit>(o.e).radius;
                max_radius = rad > max_radius ? rad : max_radius;
            }
        }
        shape.centroid = {Fixed::from_raw(static_cast<std::int32_t>(sx / shape.size)),
                          Fixed::from_raw(static_cast<std::int32_t>(sy / shape.size))};
        Fixed spread;
        for (const Resolved& o : resolved) {
            if (o.goal == r.goal) {
                const Position& p = registry.get<Position>(o.e);
                const Fixed d = length(FVec2{p.x, p.y} - shape.centroid);
                spread = d > spread ? d : spread;
            }
        }
        const Fixed target_spread = cluster_radius(max_radius, shape.size);
        if (spread > target_spread) {
            shape.scale_num = target_spread;
            shape.scale_den = spread;
        }
        shapes.push_back(shape);
    }

    for (const Resolved& r : resolved) {
        const GroupShape& shape = *std::ranges::find_if(shapes, [&](const GroupShape& g) { return g.goal == r.goal; });
        const Position& p = registry.get<Position>(r.e);
        const FVec2 offset = FVec2{p.x, p.y} - shape.centroid;
        const FVec2 scaled{offset.x * shape.scale_num / shape.scale_den, offset.y * shape.scale_num / shape.scale_den};
        FVec2 arrival = tile_center(r.goal) + scaled;
        // Un hueco que cae en agua o fuera de la componente vuelve al centro del destino.
        if (grid_.component(tile_of(arrival)) != grid_.component(r.goal)) {
            arrival = tile_center(r.goal);
        }
        MoveGoal goal;
        goal.tile = r.goal;
        goal.point = arrival;
        goal.order_id = order_id;
        goal.group_size = shape.size;
        if (slot >= 0 && r.goal == flow_goal) {
            goal.flow_field = slot;
            registry.remove<PathFollow>(r.e);
        } else {
            enqueue_path(r.e, registry.emplace_or_replace<PathFollow>(r.e));
        }
        registry.emplace_or_replace<MoveGoal>(r.e, goal);
    }
}

std::int32_t MovementSystem::build_flow_field(entt::registry& registry, std::span<const entt::entity> units,
                                              TileCoord goal, std::uint32_t order_id, Tick tick) {
    // Pasillo: sectores de las unidades, del destino y del camino abstracto desde la
    // primera unidad, más un anillo alrededor para que el grupo pueda abrirse.
    const std::int32_t sw = hpa_.sectors_w();
    const std::int32_t sh = hpa_.sectors_h();
    std::vector<std::uint8_t> core(static_cast<std::size_t>(sw) * static_cast<std::size_t>(sh), 0);
    auto mark = [&](TileCoord c) { core[static_cast<std::size_t>(hpa_.sector_of(c))] = 1; };
    for (const entt::entity e : units) {
        const Position& p = registry.get<Position>(e);
        mark(tile_of(FVec2{p.x, p.y}));
    }
    mark(goal);
    const Position& lead = registry.get<Position>(units.front());
    if (const auto waypoints = hpa_.find_waypoints(grid_, tile_of(FVec2{lead.x, lead.y}), goal, search_)) {
        for (const TileCoord& w : *waypoints) {
            mark(w);
        }
    }
    std::vector<std::uint8_t> corridor(core.size(), 0);
    for (std::int32_t sy = 0; sy < sh; ++sy) {
        for (std::int32_t sx = 0; sx < sw; ++sx) {
            if (core[static_cast<std::size_t>(sy * sw + sx)] == 0) {
                continue;
            }
            for (std::int32_t dy = -1; dy <= 1; ++dy) {
                for (std::int32_t dx = -1; dx <= 1; ++dx) {
                    const std::int32_t nx = sx + dx;
                    const std::int32_t ny = sy + dy;
                    if (nx >= 0 && ny >= 0 && nx < sw && ny < sh) {
                        corridor[static_cast<std::size_t>(ny * sw + nx)] = 1;
                    }
                }
            }
        }
    }

    // Hueco libre o, si no, el menos usado recientemente (empate: índice menor).
    std::size_t slot = 0;
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        if (!fields_[i].field) {
            slot = i;
            break;
        }
        if (fields_[i].last_used < fields_[slot].last_used) {
            slot = i;
        }
    }
    fields_[slot].field =
        std::make_unique<FlowField>(grid_, goal, SectorMask{corridor, hpa_.sector_size(), sw}, search_);
    fields_[slot].corridor = std::move(corridor);
    fields_[slot].order_id = order_id;
    fields_[slot].last_used = tick;
    ++stats_.flow_fields_built;
    return static_cast<std::int32_t>(slot);
}

void MovementSystem::run_planner(entt::registry& registry) {
    const std::int64_t start = search_.expanded_total();
    while (queue_head_ < path_queue_.size() && search_.expanded_total() - start < params_.path_node_budget_per_tick) {
        const entt::entity e = path_queue_[queue_head_++];
        if (!registry.valid(e) || !registry.all_of<PathFollow, MoveGoal, Position>(e)) {
            continue;
        }
        PathFollow& follow = registry.get<PathFollow>(e);
        if (!follow.waiting) {
            continue;  // entrada duplicada ya atendida
        }
        MoveGoal& goal = registry.get<MoveGoal>(e);
        const Position& p = registry.get<Position>(e);
        auto waypoints = hpa_.find_waypoints(grid_, tile_of(FVec2{p.x, p.y}), goal.tile, search_);
        if (!waypoints) {
            goal.arrived = true;  // inalcanzable: la orden termina aquí
            registry.remove<PathFollow>(e);
            continue;
        }
        follow.waypoints = std::move(*waypoints);
        follow.next_waypoint = 0;
        follow.segment.clear();
        follow.next_tile = 0;
        follow.waiting = false;
        ++stats_.paths_solved;
    }
    if (queue_head_ > 0 && queue_head_ * 2 >= path_queue_.size()) {
        path_queue_.erase(path_queue_.begin(), path_queue_.begin() + static_cast<std::ptrdiff_t>(queue_head_));
        queue_head_ = 0;
    }
    stats_.paths_pending = static_cast<std::int32_t>(path_queue_.size() - queue_head_);
}

FVec2 MovementSystem::preferred_velocity(entt::registry& registry, entt::entity e, FVec2 pos, const Unit& unit,
                                         MoveGoal& goal, Tick tick) {
    const FVec2 to_goal = goal.point - pos;
    if (length_sq_wide(to_goal) <= mul_wide(params_.arrive_radius, params_.arrive_radius)) {
        goal.arrived = true;
        return {};
    }
    FVec2 target = goal.point;
    bool final_leg = true;

    if (goal.flow_field >= 0) {
        CachedField& cf = fields_[static_cast<std::size_t>(goal.flow_field)];
        const TileCoord here = tile_of(pos);
        const bool valid = cf.field && cf.order_id == goal.order_id &&
                           cf.field->cost(grid_, here) != kUnreached;
        if (!valid) {
            // Fuera del pasillo o campo desalojado de la caché: camino individual.
            goal.flow_field = -1;
            enqueue_path(e, registry.emplace_or_replace<PathFollow>(e));
            return {};
        }
        cf.last_used = tick;
        // Cerca del destino (dentro del radio del racimo), cada una va directa a su hueco.
        const Fixed near = cluster_radius(unit.radius, goal.group_size) + unit.radius * 4;
        const bool close = length_sq_wide(tile_center(goal.tile) - pos) <= mul_wide(near, near);
        if (const auto next = close ? std::nullopt : cf.field->next_step(grid_, here)) {
            target = tile_center(*next);
            final_leg = false;
        }
    } else {
        PathFollow* follow = registry.try_get<PathFollow>(e);
        if (follow == nullptr) {
            enqueue_path(e, registry.emplace<PathFollow>(e));
            return {};
        }
        if (follow->waiting) {
            return {};
        }
        const std::int64_t reach_sq = mul_wide(params_.waypoint_radius, params_.waypoint_radius);
        while (true) {
            if (follow->next_tile < follow->segment.size()) {
                const FVec2 c = tile_center(follow->segment[follow->next_tile]);
                if (length_sq_wide(c - pos) <= reach_sq) {
                    ++follow->next_tile;
                    continue;
                }
                target = c;
                final_leg = false;
                break;
            }
            if (follow->next_waypoint < follow->waypoints.size()) {
                const TileCoord from = tile_of(pos);
                const TileCoord to = follow->waypoints[follow->next_waypoint++];
                follow->next_tile = 0;
                if (from == to) {
                    follow->segment.clear();
                    continue;
                }
                if (!hpa_.refine(grid_, from, to, search_, follow->segment)) {
                    enqueue_path(e, *follow);  // la unidad se desvió demasiado: replanificar
                    return {};
                }
                continue;
            }
            break;  // último tramo: directo al centro de la casilla destino
        }
    }

    const FVec2 d = target - pos;
    Fixed speed = effective_speed(registry, e, unit, pos);
    if (final_leg) {
        const Fixed dist = length(d);
        speed = dist < speed ? dist : speed;
    }
    return with_length(d, speed);
}

void MovementSystem::build_spatial_grid(std::size_t n) {
    const auto cells = static_cast<std::size_t>(grid_.width()) * static_cast<std::size_t>(grid_.height());
    s_.cell_start.assign(cells + 1, 0);
    s_.cell_units.resize(n);
    std::vector<std::uint32_t> cell_of(n);
    for (std::size_t i = 0; i < n; ++i) {
        const TileCoord t = tile_of(s_.pos[i]);
        const TileCoord c{std::clamp(t.x, 0, grid_.width() - 1), std::clamp(t.y, 0, grid_.height() - 1)};
        cell_of[i] = static_cast<std::uint32_t>(grid_.index(c));
        ++s_.cell_start[cell_of[i] + 1];
    }
    for (std::size_t c = 0; c < cells; ++c) {
        s_.cell_start[c + 1] += s_.cell_start[c];
    }
    std::vector<std::uint32_t> fill(s_.cell_start.begin(), s_.cell_start.end() - 1);
    for (std::size_t i = 0; i < n; ++i) {
        s_.cell_units[fill[cell_of[i]]++] = static_cast<std::uint32_t>(i);
    }
}

void MovementSystem::gather_neighbors(std::size_t n) {
    const auto k = static_cast<std::size_t>(params_.max_neighbors);
    s_.neighbors.assign(n * k, 0);
    s_.neighbor_count.assign(n, 0);
    const std::int64_t r_sq = mul_wide(params_.neighbor_radius, params_.neighbor_radius);
    const std::int32_t reach = params_.neighbor_radius.floor_to_int() + 1;
    std::vector<std::int64_t> dist(k);
    for (std::size_t i = 0; i < n; ++i) {
        const TileCoord t = tile_of(s_.pos[i]);
        std::uint32_t count = 0;
        std::uint32_t* out = &s_.neighbors[i * k];
        for (std::int32_t cy = std::max(t.y - reach, 0); cy <= std::min(t.y + reach, grid_.height() - 1); ++cy) {
            for (std::int32_t cx = std::max(t.x - reach, 0); cx <= std::min(t.x + reach, grid_.width() - 1); ++cx) {
                const std::size_t cell = grid_.index({cx, cy});
                for (std::uint32_t u = s_.cell_start[cell]; u < s_.cell_start[cell + 1]; ++u) {
                    const std::uint32_t j = s_.cell_units[u];
                    if (j == i) {
                        continue;
                    }
                    const std::int64_t d2 = length_sq_wide(s_.pos[j] - s_.pos[i]);
                    if (d2 >= r_sq) {
                        continue;
                    }
                    // Inserción ordenada por (distancia, índice) en una lista de k: determinista.
                    std::size_t at = count;
                    while (at > 0 && (dist[at - 1] > d2 || (dist[at - 1] == d2 && out[at - 1] > j))) {
                        if (at < k) {
                            dist[at] = dist[at - 1];
                            out[at] = out[at - 1];
                        }
                        --at;
                    }
                    if (at < k) {
                        dist[at] = d2;
                        out[at] = j;
                        count = std::min<std::uint32_t>(count + 1, static_cast<std::uint32_t>(k));
                    }
                }
            }
        }
        s_.neighbor_count[i] = count;
    }
}

FVec2 MovementSystem::choose_velocity(std::size_t i) const {
    // Una unidad que ya llegó se queda quieta: para las demás es un obstáculo estático
    // (velocidad 0) que hay que rodear. Solo la separación física la desplaza.
    if (s_.order[i] != 0 && s_.arrived[i] != 0) {
        return {};
    }
    const std::uint32_t count = s_.neighbor_count[i];
    if (count == 0) {
        return s_.pref[i];
    }
    const auto k = static_cast<std::size_t>(params_.max_neighbors);
    const Fixed horizon = Fixed::from_int(params_.time_horizon_ticks);
    const Fixed half_speed = s_.speed[i] * Fixed::from_ratio(1, 2);

    FVec2 best = s_.pref[i];
    std::int64_t best_penalty = std::numeric_limits<std::int64_t>::max();
    for (std::size_t c = 0; c < kCandidates; ++c) {
        FVec2 cand;
        if (c == 0) {
            cand = s_.pref[i];
        } else if (c == 1) {
            cand = {};
        } else if (c < 2 + kUnitDirs.size()) {
            cand = dir_scaled(c - 2, s_.speed[i]);
        } else {
            cand = dir_scaled(c - 2 - kUnitDirs.size(), half_speed);
        }

        // Tiempo hasta la primera colisión con alguna vecina, suponiendo que ella
        // también se aparta a medias (RVO: velocidad relativa 2v' - v_i - v_j).
        Fixed ttc = horizon;
        for (std::uint32_t nn = 0; nn < count; ++nn) {
            const std::uint32_t j = s_.neighbors[i * k + nn];
            const FVec2 p = s_.pos[j] - s_.pos[i];
            const FVec2 w = cand * 2 - s_.vel[i] - s_.vel[j];
            const Fixed r = s_.radius[i] + s_.radius[j];
            // |p - w t| = r  =>  a t^2 - 2 b t + c = 0, con a = w.w, b = p.w, c = p.p - r^2.
            // Se baja a 16.16 antes de elevar al cuadrado para que b^2 quepa en int64.
            const std::int64_t b = dot_wide(p, w) >> kWideShift;
            const std::int64_t cc = (length_sq_wide(p) - mul_wide(r, r)) >> kWideShift;
            if (b <= 0) {
                continue;  // se alejan (o ya se separan si están solapadas)
            }
            if (cc < 0) {
                ttc = Fixed{};  // solapadas y acercándose
                break;
            }
            const std::int64_t a = length_sq_wide(w) >> kWideShift;
            if (a == 0) {
                continue;
            }
            const std::int64_t disc = b * b - a * cc;
            if (disc < 0) {
                continue;  // la trayectoria relativa no toca el círculo
            }
            const auto root = static_cast<std::int64_t>(isqrt(static_cast<std::uint64_t>(disc)));
            const std::int64_t t_raw = ((b - root) << kWideShift) / a;
            if (t_raw < ttc.raw()) {
                ttc = Fixed::from_raw(static_cast<std::int32_t>(std::max<std::int64_t>(t_raw, 0)));
            }
        }

        const std::int64_t deviation = length(cand - s_.pref[i]).raw();
        const std::int64_t danger = (horizon - ttc).raw();
        const std::int64_t penalty = params_.preference_weight * deviation +
                                     params_.collision_weight * danger / params_.time_horizon_ticks;
        if (penalty < best_penalty) {
            best_penalty = penalty;
            best = cand;
        }
    }
    return best;
}

void MovementSystem::integrate_and_separate(std::size_t n) {
    // 1. Integración con choque contra casillas bloqueadas: se desliza por el eje libre.
    for (std::size_t i = 0; i < n; ++i) {
        const FVec2 v = s_.chosen[i];
        const FVec2 p = s_.pos[i];
        FVec2 next = p + v;
        const std::int32_t owner = s_.owner[i];
        // Quien ya está en una puerta ajena (se construyó encima, o la cruzaba cuando cambió
        // de dueño) puede moverse dentro de ella para salir.
        const auto enter = [&](FVec2 q) {
            return can_enter(tile_of(q), owner) || (tile_of(q) == tile_of(p) && grid_.passable(tile_of(q)));
        };
        if (!enter(next)) {
            const FVec2 only_x{p.x + v.x, p.y};
            const FVec2 only_y{p.x, p.y + v.y};
            if (enter(only_x)) {
                next = only_x;
                s_.chosen[i] = {v.x, Fixed{}};
            } else if (enter(only_y)) {
                next = only_y;
                s_.chosen[i] = {Fixed{}, v.y};
            } else {
                next = p;
                s_.chosen[i] = {};
            }
        }
        s_.pos[i] = next;
    }

    // 2. Separación de solapamientos (Jacobi: las correcciones se acumulan y se aplican
    //    a la vez, así el resultado no depende del orden de los pares).
    s_.correction.assign(n, FVec2{});
    for (std::size_t i = 0; i < n; ++i) {
        const TileCoord t = tile_of(s_.pos[i]);
        for (std::int32_t cy = std::max(t.y - 1, 0); cy <= std::min(t.y + 1, grid_.height() - 1); ++cy) {
            for (std::int32_t cx = std::max(t.x - 1, 0); cx <= std::min(t.x + 1, grid_.width() - 1); ++cx) {
                const std::size_t cell = grid_.index({cx, cy});
                for (std::uint32_t u = s_.cell_start[cell]; u < s_.cell_start[cell + 1]; ++u) {
                    const std::uint32_t j = s_.cell_units[u];
                    if (j <= i) {
                        continue;
                    }
                    const FVec2 d = s_.pos[j] - s_.pos[i];
                    const Fixed r = s_.radius[i] + s_.radius[j];
                    if (length_sq_wide(d) >= mul_wide(r, r)) {
                        continue;
                    }
                    const Fixed dist = length(d);
                    const Fixed half_overlap = (r - dist) * Fixed::from_ratio(1, 2);
                    // Coincidencia exacta: se separan en x, el de índice menor hacia la izquierda.
                    const FVec2 push = dist.raw() == 0 ? FVec2{half_overlap, Fixed{}} : with_length(d, half_overlap);
                    s_.correction[i] = s_.correction[i] - push;
                    s_.correction[j] = s_.correction[j] + push;
                }
            }
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        const FVec2 next = s_.pos[i] + s_.correction[i];
        if (can_enter(tile_of(next), s_.owner[i]) ||
            (tile_of(next) == tile_of(s_.pos[i]) && grid_.passable(tile_of(next)))) {
            s_.pos[i] = next;
        }
    }
}

void MovementSystem::update(entt::registry& registry, Tick tick) {
    const std::int64_t expanded_before = search_.expanded_total();
    run_planner(registry);

    // Recoger las unidades en estructura de arrays (orden denso del pool: determinista).
    auto view = registry.view<Position, Velocity, Unit>();
    s_.entity.clear();
    s_.pos.clear();
    s_.vel.clear();
    s_.pref.clear();
    s_.radius.clear();
    s_.speed.clear();
    s_.order.clear();
    s_.arrived.clear();
    s_.stuck.clear();
    s_.waiting.clear();
    s_.blob_radius.clear();
    s_.goal_point.clear();
    s_.owner.clear();
    for (const entt::entity e : view) {
        const auto& [p, v, unit] = view.get<Position, Velocity, Unit>(e);
        const FVec2 pos{p.x, p.y};
        FVec2 pref{};
        std::uint32_t order = 0;
        bool arrived = true;
        std::int32_t stuck = 0;
        std::int32_t group = 1;
        FVec2 goal_point = pos;
        // Las ya llegadas conservan orden y destino: son el núcleo al que se encadenan
        // las demás de su grupo.
        if (MoveGoal* goal = registry.try_get<MoveGoal>(e); goal != nullptr) {
            if (!goal->arrived) {
                pref = preferred_velocity(registry, e, pos, unit, *goal, tick);
            }
            order = goal->order_id;
            arrived = goal->arrived;
            stuck = goal->stuck_ticks;
            group = goal->group_size;
            goal_point = goal->point;
        }
        s_.entity.push_back(e);
        s_.pos.push_back(pos);
        s_.vel.push_back(v.v);
        s_.pref.push_back(pref);
        s_.radius.push_back(unit.radius);
        s_.speed.push_back(effective_speed(registry, e, unit, pos));
        s_.order.push_back(order);
        s_.arrived.push_back(arrived ? 1 : 0);
        s_.stuck.push_back(stuck);
        // Esperando camino del planificador: está parada por falta de ruta, no atascada.
        const PathFollow* follow = registry.try_get<PathFollow>(e);
        s_.waiting.push_back(follow != nullptr && follow->waiting ? 1 : 0);
        s_.blob_radius.push_back(cluster_radius(unit.radius, group) + params_.arrive_radius);
        s_.goal_point.push_back(goal_point);
        const Owner* own = registry.try_get<Owner>(e);
        s_.owner.push_back(own != nullptr ? std::int32_t{own->player} : kNoGate);
    }
    const std::size_t n = s_.entity.size();

    build_spatial_grid(n);
    gather_neighbors(n);
    s_.chosen.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        s_.chosen[i] = choose_velocity(i);
    }
    integrate_and_separate(n);

    // Llegada en cadena: una unidad que toca a otra de su misma orden ya llegada, y no
    // está más lejos del destino que ella, también llega. Así un grupo se para en
    // racimo alrededor del destino en lugar de empujarse para ocupar una sola casilla.
    // Además, una unidad atascada (casi parada varios ticks) con una compañera llegada
    // cerca también llega, y una atascada mucho tiempo desiste: sin esto, las últimas
    // de un grupo grande empujan para siempre contra el racimo.
    const auto k = static_cast<std::size_t>(params_.max_neighbors);
    for (std::size_t i = 0; i < n; ++i) {
        if (s_.arrived[i] != 0 || s_.order[i] == 0) {
            continue;
        }
        const bool crawling =
            s_.waiting[i] == 0 && length_sq_wide(s_.chosen[i]) * 16 < mul_wide(s_.speed[i], s_.speed[i]);
        s_.stuck[i] = crawling ? s_.stuck[i] + 1 : 0;
        const bool stuck = s_.stuck[i] >= params_.stuck_arrive_ticks;
        if (s_.stuck[i] >= 4 * params_.stuck_arrive_ticks) {
            s_.arrived[i] = 1;
            continue;
        }
        const Fixed my_dist = length(s_.goal_point[i] - s_.pos[i]);
        // Fuera del radio esperado del racimo no hay llegada en cadena: evita que una
        // columna que llega en fila se pare entera propagando la llegada hacia atrás.
        // Una atascada se admite hasta 1,5 veces ese radio: el racimo real nunca es
        // un círculo perfecto.
        const Fixed limit = stuck ? s_.blob_radius[i] + s_.blob_radius[i] * Fixed::from_ratio(1, 2) : s_.blob_radius[i];
        if (my_dist > limit) {
            continue;
        }
        for (std::uint32_t nn = 0; nn < s_.neighbor_count[i]; ++nn) {
            const std::uint32_t j = s_.neighbors[i * k + nn];
            if (s_.arrived[j] == 0 || s_.order[j] != s_.order[i]) {
                continue;
            }
            // "Tocarse" con holgura de medio radio; atascada, se admite otro diámetro de
            // hueco (la evitación frena antes del contacto).
            const Fixed contact = s_.radius[i] + s_.radius[j];
            const Fixed touch = contact + s_.radius[i] * Fixed::from_ratio(1, 2);
            const Fixed d = length(s_.pos[j] - s_.pos[i]);
            const bool near = stuck ? d <= touch + contact : d <= touch;
            if (near && my_dist <= length(s_.goal_point[j] - s_.pos[j]) + touch) {
                s_.arrived[i] = 1;
                break;
            }
        }
    }

    for (std::size_t i = 0; i < n; ++i) {
        const entt::entity e = s_.entity[i];
        auto& p = registry.get<Position>(e);
        p.x = s_.pos[i].x;
        p.y = s_.pos[i].y;
        registry.get<Velocity>(e).v = s_.chosen[i];
        if (MoveGoal* goal = registry.try_get<MoveGoal>(e); goal != nullptr) {
            goal->stuck_ticks = s_.stuck[i];
            if (s_.arrived[i] != 0) {
                // Llegada por cualquier vía (también la de preferred_velocity): el camino
                // ya no se sigue; si quedara, un cambio en la rejilla lo replanificaría.
                goal->arrived = true;
                registry.remove<PathFollow>(e);
            }
            if (!goal->arrived) {
                ++stats_.moving_units;
            }
        }
    }
    stats_.nodes_expanded = search_.expanded_total() - expanded_before;
}

void MovementSystem::hash_into(StateHasher& h) const {
    h.add_u64(path_queue_.size() - queue_head_);
    for (std::size_t i = queue_head_; i < path_queue_.size(); ++i) {
        h.add_u32(entt::to_integral(path_queue_[i]));
    }
    for (const CachedField& cf : fields_) {
        h.add_u32(cf.field ? 1U : 0U);
        if (cf.field) {
            h.add_i32(cf.field->goal().x);
            h.add_i32(cf.field->goal().y);
            h.add_u32(cf.order_id);
            h.add_u32(cf.last_used);
        }
    }
}

}  // namespace rts::sim
