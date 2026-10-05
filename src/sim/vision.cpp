#include "sim/vision.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <utility>

#include "sim/fire.hpp"
#include "sim/state_hash.hpp"

namespace rts::sim {

VisionSystem::VisionSystem(const VisionParams& params, const TileMap& map, std::vector<UnitType> units,
                           std::vector<BuildingType> buildings, std::vector<ResourceNodeType> nodes, std::int32_t players)
    : params_(params),
      map_(map),
      width_(map.width()),
      height_(map.height()),
      units_(std::move(units)),
      buildings_(std::move(buildings)),
      nodes_(std::move(nodes)) {
    assert(params_.interval_ticks > 0);
    assert(params_.day_ticks > 0);
    const auto tiles = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
    cover_.assign(tiles, 0);
    flags_.assign(static_cast<std::size_t>(players), std::vector<std::uint8_t>(tiles, 0));
    memory_.resize(static_cast<std::size_t>(players));
    fog_.resize(static_cast<std::size_t>(players));
}

std::int32_t VisionSystem::daylight_percent(Tick tick) const noexcept {
    const std::int64_t cycle = std::int64_t{params_.day_ticks} + params_.night_ticks;
    const std::int64_t t = (std::int64_t{tick} + params_.start_tick) % cycle;
    const std::int64_t night = params_.night_sight_percent;
    const std::int64_t tw = params_.twilight_ticks;
    if (t >= params_.day_ticks) {
        return static_cast<std::int32_t>(night);  // noche
    }
    if (tw > 0 && t < tw) {
        return static_cast<std::int32_t>(night + (kPercent - night) * t / tw);  // amanecer
    }
    const std::int64_t to_dusk = params_.day_ticks - t;
    if (tw > 0 && to_dusk < tw) {
        return static_cast<std::int32_t>(night + (kPercent - night) * to_dusk / tw);  // anochecer
    }
    return kPercent;
}

bool VisionSystem::visible(PlayerId p, TileCoord c) const noexcept {
    if (!params_.enabled) {
        return true;
    }
    return map_.contains(c) && (flags_[p][index(c)] & kVisible) != 0;
}

bool VisionSystem::explored(PlayerId p, TileCoord c) const noexcept {
    if (!params_.enabled) {
        return true;
    }
    return map_.contains(c) && (flags_[p][index(c)] & kExplored) != 0;
}

bool VisionSystem::sees_unit(const entt::registry& registry, PlayerId p, entt::entity e) const {
    if (!params_.enabled || registry.get<Owner>(e).player == p) {
        return true;
    }
    const TileCoord t = tile_of(registry.get<Position>(e));
    if (!map_.contains(t)) {
        return false;
    }
    const std::uint8_t f = flags_[p][index(t)];
    // Entre árboles, solo de cerca.
    return (f & kVisible) != 0 && (cover_[index(t)] == 0 || (f & kNear) != 0);
}

bool VisionSystem::sees_footprint(PlayerId p, const Footprint& fp) const noexcept {
    if (!params_.enabled) {
        return true;
    }
    for (std::int32_t y = fp.origin.y; y < fp.origin.y + fp.size; ++y) {
        for (std::int32_t x = fp.origin.x; x < fp.origin.x + fp.size; ++x) {
            if (map_.contains({x, y}) && (flags_[p][index({x, y})] & kVisible) != 0) {
                return true;
            }
        }
    }
    return false;
}

std::span<const RememberedBuilding> VisionSystem::memory(PlayerId p) const noexcept {
    return memory_[p];
}

std::shared_ptr<const std::vector<std::uint8_t>> VisionSystem::fog_layer(PlayerId p) const {
    return params_.enabled ? fog_[p] : nullptr;
}

// Recorrido de Bresenham entre las dos casillas, sin contar los extremos: lo tapa una
// casilla más alta que ambos extremos o más de cover_depth_tiles casillas de árboles.
bool VisionSystem::line_clear(TileCoord from, TileCoord to, std::int32_t h_from, std::int32_t h_to) const noexcept {
    if (from == to) {
        return true;
    }
    const std::int32_t ridge = std::max(h_from, h_to);
    const std::int32_t dx = std::abs(to.x - from.x);
    const std::int32_t dy = -std::abs(to.y - from.y);
    const std::int32_t sx = from.x < to.x ? 1 : -1;
    const std::int32_t sy = from.y < to.y ? 1 : -1;
    std::int32_t err = dx + dy;
    std::int32_t x = from.x;
    std::int32_t y = from.y;
    std::int32_t depth = 0;
    while (true) {
        const std::int32_t e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y += sy;
        }
        if (x == to.x && y == to.y) {
            return true;
        }
        if (map_.elevation({x, y}) > ridge) {
            return false;
        }
        if (cover_[index({x, y})] != 0 && ++depth > params_.cover_depth_tiles) {
            return false;
        }
    }
}

void VisionSystem::look(std::vector<std::uint8_t>& flags, const Observer& o, std::int32_t daylight) const {
    const std::int32_t base = std::max(1, o.sight * daylight / kPercent);
    const std::int32_t h0 = map_.elevation(o.tile);
    const std::int32_t reach = base + params_.elevation_sight_per_level * h0;
    const std::int32_t spot = params_.spot_in_cover_tiles;
    for (std::int32_t y = std::max(o.tile.y - reach, 0); y <= std::min(o.tile.y + reach, height_ - 1); ++y) {
        for (std::int32_t x = std::max(o.tile.x - reach, 0); x <= std::min(o.tile.x + reach, width_ - 1); ++x) {
            const TileCoord t{x, y};
            std::uint8_t& f = flags[index(t)];
            const std::int32_t dx = x - o.tile.x;
            const std::int32_t dy = y - o.tile.y;
            if (std::max(std::abs(dx), std::abs(dy)) <= spot) {
                f = static_cast<std::uint8_t>(f | kNear);
            }
            if ((f & kVisible) != 0) {
                continue;  // ya la ve otro
            }
            const std::int32_t ht = map_.elevation(t);
            const std::int32_t r = base + params_.elevation_sight_per_level * std::max(0, h0 - ht);
            if (dx * dx + dy * dy > r * r || !line_clear(o.tile, t, h0, ht)) {
                continue;
            }
            f = static_cast<std::uint8_t>(f | kVisible | kExplored);
        }
    }
}

void VisionSystem::remember(const entt::registry& registry, PlayerId p) {
    std::vector<RememberedBuilding>& mem = memory_[p];
    // Lo visto que ya no está (caído, o el sitio vacío): se olvida.
    std::erase_if(mem, [&](const RememberedBuilding& m) {
        if (!sees_footprint(p, m.footprint)) {
            return false;
        }
        return !registry.valid(m.entity) || !registry.all_of<Building>(m.entity);
    });
    // Lo que se ve ahora: su estado actual.
    for (const auto [e, b, f, o, hp] : registry.view<const Building, const Footprint, const Owner, const Health>().each()) {
        if (o.player == p || !sees_footprint(p, f)) {
            continue;
        }
        const RememberedBuilding r{e, b.type, o.player, f, hp.hp, b.complete, b.burned, registry.all_of<Fire>(e)};
        auto it = std::ranges::find(mem, e, &RememberedBuilding::entity);
        if (it != mem.end()) {
            *it = r;
        } else {
            mem.push_back(r);
        }
    }
    std::ranges::sort(mem, {}, [](const RememberedBuilding& m) { return entt::to_integral(m.entity); });
}

void VisionSystem::update(const entt::registry& registry, Tick tick, bool force) {
    if (!params_.enabled || (!force && tick % static_cast<Tick>(params_.interval_ticks) != 0)) {
        return;
    }
    // Árboles: tapan la vista donde estén.
    std::ranges::fill(cover_, std::uint8_t{0});
    for (const auto [e, n, f] : registry.view<const ResourceNode, const Footprint>().each()) {
        if (registry.all_of<Building>(e) || !nodes_[n.type].blocks_sight) {
            continue;
        }
        for (std::int32_t y = f.origin.y; y < f.origin.y + f.size; ++y) {
            for (std::int32_t x = f.origin.x; x < f.origin.x + f.size; ++x) {
                if (map_.contains({x, y})) {
                    cover_[index({x, y})] = 1;
                }
            }
        }
    }
    const std::int32_t daylight = daylight_percent(tick);
    StateHasher h;
    h.add_bytes(cover_);
    for (std::size_t p = 0; p < flags_.size(); ++p) {
        std::vector<std::uint8_t>& flags = flags_[p];
        for (std::uint8_t& f : flags) {
            f &= kExplored;  // lo explorado se queda; la vista se rehace
        }
        // Observadores del jugador, sin repetir (en una batalla muchos comparten casilla).
        observers_.clear();
        for (const auto [e, u, pos, o] : registry.view<const Unit, const Position, const Owner>().each()) {
            if (o.player == p && units_[u.type].combat.sight_tiles > 0) {
                observers_.push_back({tile_of(pos), units_[u.type].combat.sight_tiles});
            }
        }
        for (const auto [e, b, f, o] : registry.view<const Building, const Footprint, const Owner>().each()) {
            if (o.player == p && buildings_[b.type].sight_tiles > 0) {
                observers_.push_back({center_of(f), buildings_[b.type].sight_tiles});
            }
        }
        std::ranges::sort(observers_, [](const Observer& a, const Observer& b) {
            if (a.tile.y != b.tile.y) {
                return a.tile.y < b.tile.y;
            }
            return a.tile.x != b.tile.x ? a.tile.x < b.tile.x : a.sight > b.sight;
        });
        // A igual casilla, el de más vista cubre a los demás.
        const auto dup = std::ranges::unique(observers_, [](const Observer& a, const Observer& b) { return a.tile == b.tile; });
        observers_.erase(dup.begin(), dup.end());
        for (const Observer& o : observers_) {
            if (map_.contains(o.tile)) {
                look(flags, o, daylight);
            }
        }
        remember(registry, static_cast<PlayerId>(p));
        auto fog = std::make_shared<std::vector<std::uint8_t>>(flags.size());
        for (std::size_t i = 0; i < flags.size(); ++i) {
            (*fog)[i] = static_cast<std::uint8_t>((flags[i] & kVisible) != 0    ? Fog::Visible
                                                  : (flags[i] & kExplored) != 0 ? Fog::Explored
                                                                                : Fog::Unexplored);
        }
        fog_[p] = std::move(fog);
        h.add_bytes(flags);
        for (const RememberedBuilding& m : memory_[p]) {
            h.add_u32(entt::to_integral(m.entity));
            h.add_u32(m.type);
            h.add_i32(m.hp);
            h.add_u32((m.complete ? 1U : 0U) | (m.burned ? 2U : 0U) | (m.burning ? 4U : 0U));
        }
    }
    digest_ = h.value();
}

void VisionSystem::hash_into(StateHasher& h) const {
    if (params_.enabled) {
        h.add_u64(digest_);  // desactivada no hay estado: el hash es el de siempre
    }
}

}  // namespace rts::sim
