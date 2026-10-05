#include "game/alerts.hpp"

#include <algorithm>
#include <unordered_map>

namespace rts::game {

bool AlertTracker::allow(AlertKind kind, sim::TileCoord where, sim::Tick tick) {
    const std::int32_t z = std::max(params_.zone_tiles, 1);
    const sim::TileCoord zone{where.x / z, where.y / z};
    for (Last& l : last_) {
        if (l.kind == kind && l.zone == zone) {
            if (tick - l.tick < static_cast<sim::Tick>(params_.cooldown_ticks)) {
                return false;
            }
            l.tick = tick;
            return true;
        }
    }
    last_.push_back({kind, zone, tick});
    return true;
}

std::vector<Alert> AlertTracker::update(const sim::Snapshot& prev, const sim::Snapshot& curr, sim::PlayerId me) {
    std::vector<Alert> out;
    if (prev.entities.empty() && prev.objects.empty()) {
        return out;  // primer estado: no hay con qué comparar
    }
    const auto raise = [&](AlertKind kind, sim::TileCoord where) {
        if (allow(kind, where, curr.tick)) {
            out.push_back({kind, where, curr.tick});
        }
    };
    std::unordered_map<std::uint32_t, const sim::SnapshotEntity*> before;
    for (const sim::SnapshotEntity& e : prev.entities) {
        before.emplace(e.id, &e);
    }
    std::int32_t hungry_before = 0;
    for (const sim::SnapshotEntity& e : prev.entities) {
        hungry_before += e.owner == me && e.hungry ? 1 : 0;
    }
    std::int32_t hungry_now = 0;
    std::optional<sim::TileCoord> hungry_at;
    for (const sim::SnapshotEntity& e : curr.entities) {
        if (e.owner != me) {
            continue;
        }
        const sim::TileCoord t = sim::tile_of(e.pos);
        if (e.hungry) {
            ++hungry_now;
            hungry_at = hungry_at.value_or(t);
        }
        const auto it = before.find(e.id);
        if (it == before.end()) {
            raise(AlertKind::UnitReady, t);  // nueva: recién producida
        } else if (e.routing && !it->second->routing) {
            raise(AlertKind::Rout, t);
        } else if (e.hp < it->second->hp && !e.admitted) {
            raise(AlertKind::UnderAttack, t);
        }
    }
    if (hungry_now > hungry_before && hungry_at) {
        raise(AlertKind::Hunger, *hungry_at);
    }
    for (const sim::SnapshotObject& o : curr.objects) {
        if (o.kind != sim::ObjectKind::Building || o.owner != me) {
            continue;
        }
        const auto it = std::ranges::find(prev.objects, o.id, &sim::SnapshotObject::id);
        if (it == prev.objects.end()) {
            continue;
        }
        if (o.fire > 0 && it->fire == 0) {
            raise(AlertKind::Fire, o.origin);
        } else if (o.hp < it->hp && o.complete) {
            raise(AlertKind::UnderAttack, o.origin);
        }
    }
    if (me < curr.players.size() && me < prev.players.size() && curr.players[me].stock[0] == 0 &&
        prev.players[me].stock[0] > 0) {
        raise(AlertKind::NoFood, hungry_at.value_or(sim::TileCoord{-1, -1}));
    }
    for (const Alert& a : out) {
        recent_.push_back(a);
    }
    std::erase_if(recent_, [&](const Alert& a) { return curr.tick - a.tick > static_cast<sim::Tick>(params_.show_ticks); });
    return out;
}

std::vector<Alert> AlertTracker::shown(sim::Tick now) const {
    std::vector<Alert> out;
    for (auto it = recent_.rbegin(); it != recent_.rend() && std::cmp_less(out.size(), params_.max_shown); ++it) {
        if (now - it->tick <= static_cast<sim::Tick>(params_.show_ticks)) {
            out.push_back(*it);
        }
    }
    return out;
}

}  // namespace rts::game
