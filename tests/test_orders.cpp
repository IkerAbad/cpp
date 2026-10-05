// Pruebas de los controles de mando en la simulación: puntos de paso encolados
// (Mayús) y puntos de reunión de los edificios que producen.

#include <cstdint>
#include <vector>

#include <doctest/doctest.h>

#include "sim/world.hpp"
#include "test_helpers.hpp"

using rts::sim::Command;
using rts::sim::CommandType;
using rts::sim::TileCoord;
using rts::sim::World;
using rts::sim::WorldParams;
using namespace rts::test;

namespace {

WorldParams flat() {
    WorldParams p = test_world_params(0, 16);
    p.map.bands = {{rts::sim::kElevationRange, 1}};
    p.demo.player = 1;
    return p;
}

Command move(std::vector<std::uint32_t> units, TileCoord to, bool queue) {
    Command c;
    c.type = CommandType::Move;
    c.units = std::move(units);
    c.target = to;
    c.kind = queue ? rts::sim::kQueueMove : 0;
    return c;
}

TileCoord tile(const World& w, std::uint32_t id) {
    return rts::sim::tile_of(w.registry().get<rts::sim::Position>(static_cast<entt::entity>(id)));
}

std::int32_t dist(TileCoord a, TileCoord b) {
    return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y));
}

}  // namespace

TEST_CASE("Órdenes: los puntos de paso encolados se recorren en orden") {
    World world(flat());
    const auto u = world.spawn_unit(0, kSoldier, {100, 100});
    world.issue(move({u}, {110, 100}, false));
    world.issue(move({u}, {110, 110}, true));
    world.issue(move({u}, {100, 110}, true));
    bool passed_first = false;
    bool passed_second = false;
    for (std::int32_t t = 0; t < 2000; ++t) {
        world.step();
        passed_first = passed_first || dist(tile(world, u), {110, 100}) <= 1;
        passed_second = passed_second || (passed_first && dist(tile(world, u), {110, 110}) <= 1);
    }
    CHECK(passed_first);
    CHECK(passed_second);
    CHECK(dist(tile(world, u), {100, 110}) <= 1);
}

TEST_CASE("Órdenes: una orden normal olvida los puntos de paso") {
    World world(flat());
    const auto u = world.spawn_unit(0, kSoldier, {100, 100});
    world.issue(move({u}, {120, 100}, false));
    world.issue(move({u}, {120, 120}, true));
    world.step();
    world.issue(move({u}, {90, 100}, false));
    for (std::int32_t t = 0; t < 1500; ++t) {
        world.step();
    }
    CHECK(dist(tile(world, u), {90, 100}) <= 1);
    CHECK_FALSE(world.registry().all_of<rts::sim::Waypoints>(static_cast<entt::entity>(u)));
}

TEST_CASE("Órdenes: lo producido va al punto de reunión; un aldeano, a recoger si es un recurso") {
    World world(flat());
    world.set_stock(0, stock(1000, 1000, 1000, 1000));
    const auto center = *world.spawn_building(0, kCenter, {100, 100}, true);
    const auto berries = *world.spawn_node(kBerries, {110, 104});
    Command rally;
    rally.type = CommandType::SetRally;
    rally.object = center;
    rally.target = {110, 104};
    world.issue(rally);
    Command train;
    train.type = CommandType::Train;
    train.object = center;
    train.kind = kVillager;
    world.issue(train);
    for (std::int32_t t = 0; t < 900; ++t) {
        world.step();
    }
    rts::sim::Snapshot snap;
    world.write_snapshot(snap);
    REQUIRE(snap.entities.size() == 1);
    // Recogiendo o ya llevando lo recogido al almacén.
    CHECK((snap.entities[0].task == rts::sim::WorkerTask::Gather ||
           snap.entities[0].task == rts::sim::WorkerTask::Deliver));
    const auto& w = world.registry().get<rts::sim::Worker>(static_cast<entt::entity>(snap.entities[0].id));
    CHECK(w.node == static_cast<entt::entity>(berries));
    // Quitar el punto de reunión.
    rally.kind = rts::sim::kClearRally;
    world.issue(rally);
    world.step();
    world.write_snapshot(snap);
    const auto it = std::ranges::find(snap.objects, center, &rts::sim::SnapshotObject::id);
    CHECK(it->rally.x == -1);
}
