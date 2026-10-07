// Pruebas aleatorias de órdenes (G2): partidas reales con órdenes al azar, válidas y
// mal formadas, sin romperse, sin salirse de las invariantes y sin dejar de ser
// deterministas (dos mundos a la par y la repetición grabada). Aquí, partidas cortas;
// rts_fuzz las hace largas en la CI.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <doctest/doctest.h>

#include "game/campaign.hpp"
#include "game/config.hpp"
#include "game/fuzz.hpp"
#include "game/scenario.hpp"

namespace {

const rts::game::GameData& game_data() {
    static const rts::game::GameData data = [] {
        auto d = rts::game::load_game_data(RTS_DATA_DIR);
        REQUIRE_MESSAGE(d.has_value(), (d ? std::string() : d.error()));
        return std::move(*d);
    }();
    return data;
}

void run(const rts::game::GameData& data, std::uint64_t seed, std::int32_t ticks) {
    rts::game::FuzzParams params;
    params.orders_per_tick = 2;
    const auto r = rts::game::run_fuzz(data, seed, ticks, params, 50);
    CAPTURE(seed);
    CHECK_MESSAGE(!r.failure, r.failure.value_or(""));
    CHECK(r.orders == 2 * ticks);
}

}  // namespace

TEST_CASE("Órdenes al azar: partida normal con niebla y cuatro jugadores") {
    rts::game::MatchSettings s;
    s.seed = 7;
    s.fog = true;
    s.rival = game_data().engine.ai_profile_names.back();
    s.seats = {"humano", "ia", "humano", "ia"};
    const auto data = rts::game::with_match_settings(game_data(), s);
    REQUIRE(data.has_value());
    for (const std::uint64_t seed : {1U}) {
        run(*data, seed, 300);
    }
}

TEST_CASE("Órdenes al azar: un capítulo con murallas, torres y campamentos") {
    std::vector<std::string> errors;
    const auto campaigns = rts::game::load_campaigns(RTS_DATA_DIR, errors);
    REQUIRE_FALSE(campaigns.empty());
    const auto& c = campaigns.front();
    const auto ch = std::ranges::find(c.chapters, "calatrava", &rts::game::CampaignChapter::id);
    REQUIRE(ch != c.chapters.end());
    std::ifstream in(c.dir / ch->scenario, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const auto doc = rts::game::parse_scenario_doc(text, game_data());
    REQUIRE(doc.has_value());
    rts::game::MatchSettings s;
    s.rival = ch->rival;
    const auto data = rts::game::with_scenario(game_data(), *doc, s);
    REQUIRE(data.has_value());
    run(*data, 3, 300);
}

TEST_CASE("Órdenes al azar: las invariantes detectan lo que está mal") {
    const auto& d = game_data();
    rts::sim::World world(d.engine.world);
    CHECK_FALSE(rts::game::check_invariants(world));
    rts::sim::Stock negative{};
    negative[0] = -1;
    world.set_stock(0, negative);
    const auto bad = rts::game::check_invariants(world);
    REQUIRE(bad);
    CHECK(bad->find("negativo") != std::string::npos);
}

// Encontrado por rts_fuzz: una orden con una unidad repetida (puede llegar por la red)
// guarnecía dos veces la misma unidad y la simulación abortaba.
TEST_CASE("Órdenes al azar: una unidad repetida en una orden cuenta una sola vez") {
    const auto doc = rts::game::parse_scenario_doc(R"(
players = 2
width = 24
height = 24
base = "pradera"

[[building]]
player = 0
type = "torre_madera"
at = [10, 10]

[[unit]]
player = 0
type = "leva"
at = [6, 10]
count = 2
)",
                                                   game_data());
    REQUIRE(doc.has_value());
    rts::sim::WorldParams p = game_data().engine.world;
    p.scenario = doc->params;
    p.ai_players.clear();
    rts::sim::World world(p);
    rts::sim::Snapshot s;
    world.write_snapshot(s);
    REQUIRE(s.entities.size() == 2);
    const auto tower = std::ranges::find(s.objects, rts::sim::ObjectKind::Building, &rts::sim::SnapshotObject::kind);
    REQUIRE(tower != s.objects.end());
    rts::sim::Command c;
    c.type = rts::sim::CommandType::Garrison;
    c.player = 0;
    c.units = {s.entities[0].id, s.entities[0].id, s.entities[1].id, s.entities[0].id};
    c.object = tower->id;
    world.issue(c);
    for (int t = 0; t < 400; ++t) {
        world.step();
    }
    world.write_snapshot(s);
    const auto t = std::ranges::find(s.objects, tower->id, &rts::sim::SnapshotObject::id);
    REQUIRE(t != s.objects.end());
    CHECK(t->garrison == 2);
    CHECK_FALSE(rts::game::check_invariants(world));
}
