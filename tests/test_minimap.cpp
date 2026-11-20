// Minimapa: respeta la niebla del jugador que mira.

#include <vector>

#include <doctest/doctest.h>

#include "game/minimap.hpp"

using reinos::game::minimap_dots;
using reinos::sim::Snapshot;
using reinos::sim::SnapshotEntity;
using reinos::sim::SnapshotObject;

namespace {

const std::vector<reinos::render::Rgba> kPlayers{{0, 0, 255, 255}, {255, 0, 0, 255}};
const std::vector<reinos::render::Rgba> kNodes{{0, 255, 0, 255}};

SnapshotEntity unit(reinos::sim::PlayerId owner, std::uint8_t seen_by) {
    SnapshotEntity e;
    e.owner = owner;
    e.seen_by = seen_by;
    return e;
}

}  // namespace

TEST_CASE("Minimapa: lo propio siempre; del enemigo, solo lo visto o recordado") {
    Snapshot s;
    s.entities = {unit(0, 0b01), unit(1, 0b10), unit(1, 0b11)};  // la 2.ª, el jugador 0 no la ve
    SnapshotObject hidden;
    hidden.id = 7;
    hidden.kind = reinos::sim::ObjectKind::Building;
    hidden.owner = 1;
    hidden.seen_by = 0b10;
    hidden.origin = {40, 40};
    s.objects = {hidden};
    CHECK(minimap_dots(s, reinos::sim::PlayerId{0}, kPlayers, kNodes).size() == 2);   // la suya y la enemiga vista
    CHECK(minimap_dots(s, std::nullopt, kPlayers, kNodes).size() == 4);  // repetición: todo
    // Recordado: aparece aunque no se vea.
    reinos::sim::RememberedBuilding m;
    m.entity = static_cast<entt::entity>(7);
    m.owner = 1;
    m.footprint = {{40, 40}, 2};
    s.memory = {{m}, {}};
    const auto dots = minimap_dots(s, reinos::sim::PlayerId{0}, kPlayers, kNodes);
    CHECK(dots.size() == 3);
    CHECK(std::ranges::any_of(dots,
                              [](const auto& d) { return d.origin == reinos::sim::TileCoord{40, 40} && d.size == 2; }));
}
