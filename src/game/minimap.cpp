#include "game/minimap.hpp"

#include "sim/fog.hpp"

namespace rts::game {

namespace {

bool explored(const sim::Snapshot& snap, sim::PlayerId p, sim::TileCoord t) {
    if (p >= snap.fog.size() || !snap.fog[p] || !snap.map || !snap.map->contains(t)) {
        return true;
    }
    const auto idx = static_cast<std::size_t>(t.y) * static_cast<std::size_t>(snap.map->width()) +
                     static_cast<std::size_t>(t.x);
    return static_cast<sim::Fog>((*snap.fog[p])[idx]) != sim::Fog::Unexplored;
}

render::Rgba color_of(const std::vector<render::Rgba>& colors, std::size_t i) {
    return i < colors.size() ? colors[i] : render::Rgba{255, 255, 255, 255};
}

}  // namespace

std::vector<MinimapDot> minimap_dots(const sim::Snapshot& snap, std::optional<sim::PlayerId> viewer,
                                     const std::vector<render::Rgba>& player_colors,
                                     const std::vector<render::Rgba>& node_colors) {
    std::vector<MinimapDot> dots;
    const auto sees = [&](sim::PlayerId owner, std::uint8_t seen_by) {
        return !viewer || owner == *viewer || (seen_by & (1U << *viewer)) != 0;
    };
    for (const sim::SnapshotObject& o : snap.objects) {
        if (o.kind == sim::ObjectKind::Resource) {
            if (!viewer || explored(snap, *viewer, o.origin)) {
                dots.push_back({o.origin, o.size, color_of(node_colors, o.type)});
            }
        } else if (sees(o.owner, o.seen_by)) {
            dots.push_back({o.origin, o.size, color_of(player_colors, o.owner)});
        }
    }
    // Edificios enemigos recordados que ahora no se ven.
    if (viewer && *viewer < snap.memory.size()) {
        for (const sim::RememberedBuilding& m : snap.memory[*viewer]) {
            const bool live_seen = std::ranges::any_of(snap.objects, [&](const sim::SnapshotObject& o) {
                return o.id == entt::to_integral(m.entity) && sees(o.owner, o.seen_by);
            });
            if (!live_seen) {
                dots.push_back({m.footprint.origin, m.footprint.size, color_of(player_colors, m.owner)});
            }
        }
    }
    for (const sim::SnapshotEntity& e : snap.entities) {
        if (sees(e.owner, e.seen_by)) {
            dots.push_back({sim::tile_of(e.pos), 1, color_of(player_colors, e.owner)});
        }
    }
    return dots;
}

}  // namespace rts::game
