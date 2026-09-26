// Pruebas de la IA básica: crece su economía, construye cuartel y granjas, ataca, no
// hace trampas (nunca gasta lo que no tiene) y es determinista. También la derrota.

#include <cstdint>
#include <format>
#include <memory>
#include <vector>

#include <doctest/doctest.h>

#include "sim/world.hpp"
#include "test_helpers.hpp"

using rts::sim::Snapshot;
using rts::sim::World;
using rts::sim::WorldParams;
using namespace rts::test;

namespace {

// Partida de dos jugadores con preparación completa. El jugador 1 es de la IA; el 0,
// también si both_ai, o un humano que no hace nada.
WorldParams ai_game(bool both_ai, std::uint8_t profile = 0) {
    WorldParams p = test_world_params(0, 16);
    p.setup.seed = 21;
    p.setup.starts = {{60, 60}, {196, 196}};
    p.setup.start_search_radius = 60;
    p.setup.min_start_region_tiles = 2000;
    p.setup.start_building = kCenter;
    p.setup.start_unit = kVillager;
    p.setup.start_units = 5;
    p.setup.start_stock = stock(300, 300, 100, 150);
    p.setup.near_start = {{kGoldMine, 1, 6, 10}, {kBerries, 6, 4, 7}, {kTree, 16, 8, 12}};
    p.setup.forest_terrain = 2;
    p.setup.tree_type = kTree;
    p.setup.tree_density_permille = 300;
    p.setup.clear_radius = 7;
    p.ai_players = both_ai ? std::vector<rts::sim::AiSeat>{{0, profile}, {1, profile}}
                           : std::vector<rts::sim::AiSeat>{{1, profile}};
    return p;
}

struct Census {
    std::int32_t workers = 0;
    std::int32_t army = 0;
    std::int32_t barracks = 0;
    std::int32_t farms = 0;
    std::int32_t houses = 0;
};

Census census(const World& world, rts::sim::PlayerId player) {
    Snapshot s;
    world.write_snapshot(s);
    Census c;
    for (const auto& e : s.entities) {
        if (e.owner == player) {
            (e.type == kVillager ? c.workers : c.army) += 1;
        }
    }
    for (const auto& o : s.objects) {
        if (o.kind == rts::sim::ObjectKind::Building && o.owner == player && o.complete) {
            c.barracks += o.type == kBarracks ? 1 : 0;
            c.farms += o.type == kFarm ? 1 : 0;
            c.houses += o.type == kHouse ? 1 : 0;
        }
    }
    return c;
}

}  // namespace

namespace {

// Perfil 0: básica; 1: normal. Los dos, con las mismas reglas.
void beats_idle_rival(std::uint8_t profile) {
    INFO("perfil " << static_cast<int>(profile));
    World world(ai_game(false, profile));
    std::int32_t waves_seen = 0;
    // 11 minutos: las bases están a ~190 casillas; la primera oleada tarda en llegar.
    for (int t = 0; t < 13200; ++t) {
        world.step();
        const auto& stock = world.player_state(1).stock;
        for (const std::int32_t r : stock) {
            REQUIRE(r >= 0);  // sin trampas: nunca en negativo
        }
        waves_seen = world.ai().players()[0].waves_sent;
    }
    const Census c = census(world, 1);
    MESSAGE("IA a los 11 min: " << c.workers << " aldeanos, " << c.army << " soldados, " << c.houses << " casas, "
                                 << c.barracks << " cuarteles, " << c.farms << " granjas, " << waves_seen << " oleadas");
    CHECK(c.workers >= 12);
    CHECK(c.houses >= 1);
    CHECK(c.barracks >= 1);
    CHECK(waves_seen >= 1);
    // El rival quieto no se defiende: a los 11 minutos la IA lo ha derrotado.
    CHECK(world.player_state(0).defeated);
    CHECK_FALSE(world.player_state(1).defeated);
}

}  // namespace

TEST_CASE("IA básica: contra un rival quieto crece, construye cuartel, entrena y ataca sin gastar lo que no tiene") {
    beats_idle_rival(0);
}

TEST_CASE("IA normal: contra un rival quieto crece, construye cuartel, entrena y ataca sin gastar lo que no tiene") {
    beats_idle_rival(1);
}

TEST_CASE("IA: dos IA juegan la misma partida tick a tick") {
    auto a = std::make_unique<World>(ai_game(true));
    auto b = std::make_unique<World>(ai_game(true));
    for (int t = 0; t < 1500; ++t) {
        a->step();
        b->step();
        REQUIRE(a->state_hash() == b->state_hash());
    }
}

TEST_CASE("Derrota: sin unidades ni edificios después de haberlos tenido") {
    WorldParams p = test_world_params(0, 16);
    p.map.bands = {{rts::sim::kElevationRange, 1}};
    p.demo.player = 1;  // dos jugadores
    World world(p);
    const auto mine = world.spawn_unit(0, kSoldier, {100, 100});
    const auto lone = world.spawn_unit(1, kVillager, {100, 100});
    world.step();
    CHECK(world.player_state(1).started);
    CHECK_FALSE(world.player_state(1).defeated);
    rts::sim::Command attack;
    attack.type = rts::sim::CommandType::Attack;
    attack.player = 0;
    attack.units = {mine};
    attack.object = lone;
    world.issue(attack);
    for (int t = 0; t < 400 && !world.player_state(1).defeated; ++t) {
        world.step();
    }
    CHECK(world.player_state(1).defeated);
    CHECK_FALSE(world.player_state(0).defeated);
}

TEST_CASE("Regresión: hash de una partida IA contra IA de 3000 ticks") {
    World world(ai_game(true));
    for (int t = 0; t < 3000; ++t) {
        world.step();
    }
    constexpr std::uint64_t kExpectedHash = 0x00a964d76d8b7b68ULL;
    INFO(std::format("hash obtenido: 0x{:016x}", world.state_hash()));
    CHECK(world.state_hash() == kExpectedHash);
}

TEST_CASE("Regresión: hash de una partida IA normal contra IA normal de 3000 ticks") {
    World world(ai_game(true, 1));
    for (int t = 0; t < 3000; ++t) {
        world.step();
    }
    constexpr std::uint64_t kExpectedHash = 0xae0f0c145dcf21fdULL;
    INFO(std::format("hash obtenido: 0x{:016x}", world.state_hash()));
    CHECK(world.state_hash() == kExpectedHash);
}
