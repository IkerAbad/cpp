// La estación de campaña (H2): qué estación toca en cada tick y lo que cambia cada una
// (marcha, cosecha de las granjas, forraje del bosque y consumo de víveres), con el
// verano al 100 % en todo; y la estación de data/, del escenario y del menú.

#include <cstdint>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "game/config.hpp"
#include "game/scenario.hpp"
#include "sim/season.hpp"
#include "sim/world.hpp"
#include "test_helpers.hpp"

using reinos::sim::Command;
using reinos::sim::CommandType;
using reinos::sim::SeasonParams;
using reinos::sim::World;
using reinos::sim::WorldParams;
using namespace reinos::test;

namespace {

constexpr std::uint8_t kSpring = 0;
constexpr std::uint8_t kSummer = 1;
constexpr std::uint8_t kAutumn = 2;
constexpr std::uint8_t kWinter = 3;

// Las cifras de data/config/engine.toml.
SeasonParams seasons(std::uint8_t start) {
    SeasonParams s;
    s.seasons[kSpring] = {50, 75, 90, 100};
    s.seasons[kSummer] = {100, 100, 100, 100};
    s.seasons[kAutumn] = {125, 125, 90, 100};
    s.seasons[kWinter] = {0, 25, 75, 125};
    s.start = start;
    return s;
}

WorldParams world_in(std::uint8_t season) {
    WorldParams p = test_world_params(0, 16);
    p.map.bands = {{reinos::sim::kElevationRange, 1}};
    p.season = seasons(season);
    return p;
}

Command order(CommandType type, std::vector<std::uint32_t> units, std::uint32_t object = reinos::sim::kNoObject,
              reinos::sim::TileCoord target = {}) {
    Command c;
    c.type = type;
    c.player = 0;
    c.units = std::move(units);
    c.object = object;
    c.target = target;
    return c;
}

void run(World& w, std::int32_t ticks) {
    for (std::int32_t t = 0; t < ticks; ++t) {
        w.step();
    }
}

const reinos::game::GameData& game_data() {
    static const reinos::game::GameData data = [] {
        auto d = reinos::game::load_game_data(REINOS_DATA_DIR);
        REQUIRE_MESSAGE(d.has_value(), (d ? std::string() : d.error()));
        return std::move(*d);
    }();
    return data;
}

}  // namespace

TEST_CASE("Estación: fija por omisión; con season_ticks, primavera, verano, otoño, invierno y vuelta") {
    SeasonParams s = seasons(kSummer);
    CHECK(reinos::sim::season_at(s, 0) == kSummer);
    CHECK(reinos::sim::season_at(s, 1'000'000) == kSummer);  // season_ticks = 0: no cambia
    s.start = kAutumn;
    s.season_ticks = 100;
    CHECK(reinos::sim::season_at(s, 0) == kAutumn);
    CHECK(reinos::sim::season_at(s, 99) == kAutumn);
    CHECK(reinos::sim::season_at(s, 100) == kWinter);
    CHECK(reinos::sim::season_at(s, 200) == kSpring);
    CHECK(reinos::sim::season_at(s, 300) == kSummer);
    CHECK(reinos::sim::season_at(s, 400) == kAutumn);
    CHECK(reinos::sim::season_effects(s, 150).march_percent == 75);
}

TEST_CASE("Estación: en invierno se marcha al 75 % de lo que se marcha en verano") {
    const auto marched = [](std::uint8_t season) {
        World world(world_in(season));
        const auto soldier = world.spawn_unit(0, kSoldier, {100, 100});
        world.issue(order(CommandType::Move, {soldier}, reinos::sim::kNoObject, {140, 100}));
        run(world, 100);
        CHECK(world.season() == season);
        reinos::sim::Snapshot s;
        world.write_snapshot(s);
        CHECK(s.season == season);
        return (s.entities.front().pos.x - reinos::sim::Fixed::from_int(100)).raw();
    };
    const std::int64_t summer = marched(kSummer);
    const std::int64_t winter = marched(kWinter);
    REQUIRE(summer > 0);
    // 75 %, con un 2 % de margen por el arranque y la llegada.
    CHECK(winter * 100 >= summer * 73);
    CHECK(winter * 100 <= summer * 77);
}

TEST_CASE("Estación: la granja no da nada en invierno, la mitad en primavera y más en otoño") {
    const auto harvested = [](std::uint8_t season) {
        WorldParams p = world_in(season);
        p.unit_types[kVillager].carry_capacity = 1000;  // no va a descargar: se mide el ritmo
        World world(p);
        const auto farm = world.spawn_building(0, kFarm, {100, 100}, true);
        REQUIRE(farm.has_value());
        const auto villager = world.spawn_unit(0, kVillager, {98, 98});
        world.issue(order(CommandType::Gather, {villager}, *farm));
        run(world, 1000);
        reinos::sim::Snapshot s;
        world.write_snapshot(s);
        return s.entities.front().carried;
    };
    const std::int32_t summer = harvested(kSummer);
    REQUIRE(summer > 20);
    CHECK(harvested(kWinter) == 0);
    const std::int32_t spring = harvested(kSpring);
    CHECK(spring * 2 >= summer - 2);
    CHECK(spring * 2 <= summer + 2);
    CHECK(harvested(kAutumn) > summer);
}

TEST_CASE("Estación: los recursos naturales se recogen igual en cualquier estación") {
    const auto gathered = [](std::uint8_t season) {
        WorldParams p = world_in(season);
        p.unit_types[kVillager].carry_capacity = 1000;
        World world(p);
        const auto mine = *world.spawn_node(kGoldMine, {102, 100});
        const auto villager = world.spawn_unit(0, kVillager, {100, 100});
        world.issue(order(CommandType::Gather, {villager}, mine));
        run(world, 1000);
        reinos::sim::Snapshot s;
        world.write_snapshot(s);
        return s.entities.front().carried;
    };
    CHECK(gathered(kWinter) == gathered(kSummer));
}

TEST_CASE("Estación: con frío se come más y el bosque da menos forraje") {
    // Un soldado sin fuente propia, junto al bosque: gasta raciones y forrajea.
    const auto camp = [](std::uint8_t season, std::int32_t& foraged) {
        WorldParams p = world_in(season);
        p.demo.player = 1;
        for (auto& u : p.unit_types) {
            u.supply.rations = 100;
            u.supply.ration_ticks = 10;
        }
        p.supply.forage = true;
        p.supply.forage_reach_tiles = 2;
        p.supply.forest_forage_ticks = 20;
        p.supply.resupply_interval_ticks = 1;
        p.supply.ration_cost = stock(1, 0, 0, 0);
        World world(p);
        REQUIRE(world.spawn_node(kTree, {101, 100}).has_value());
        const auto soldier = world.spawn_unit(0, kSoldier, {100, 100});
        foraged = 0;
        for (int t = 0; t < 800; ++t) {
            world.step();
            foraged += world.supply().last_stats().foraged;
        }
        return world.registry().get<reinos::sim::Supply>(static_cast<entt::entity>(soldier)).rations;
    };
    std::int32_t summer_forage = 0;
    std::int32_t winter_forage = 0;
    const std::int32_t summer = camp(kSummer, summer_forage);
    const std::int32_t winter = camp(kWinter, winter_forage);
    MESSAGE("verano: ", summer, " raciones, ", summer_forage, " del bosque; invierno: ", winter, " raciones, ",
            winter_forage, " del bosque");
    // Una ración cada 20 ticks de bosque en verano y cada 80 en invierno (25 %).
    CHECK(summer_forage >= 36);
    CHECK(winter_forage * 4 >= summer_forage - 4);
    CHECK(winter_forage * 4 <= summer_forage + 4);
    // 800 ticks: en verano se comen 80 raciones (una cada 10 ticks); en invierno, 100
    // (una cada 8). Lo que queda es lo que había, menos lo comido, más lo forrajeado.
    CHECK(summer == 100 - 80 + summer_forage);
    CHECK(winter == 100 - 100 + winter_forage);
}

TEST_CASE("Estación: data/ trae las cuatro, empieza en verano y la eligen el menú o el escenario") {
    const auto& d = game_data();
    REQUIRE(d.engine.season_names == std::vector<std::string>{"primavera", "verano", "otono", "invierno"});
    const SeasonParams& s = d.engine.world.season;
    CHECK(s.start == kSummer);
    CHECK(s.season_ticks == 0);
    const auto& summer = s.seasons[kSummer];
    CHECK((summer.farm_percent == 100 && summer.forage_percent == 100 && summer.march_percent == 100 &&
           summer.ration_percent == 100));
    CHECK(s.seasons[kWinter].farm_percent == 0);

    reinos::game::MatchSettings settings;
    settings.rival = "normal";
    settings.season = "otono";
    const auto autumn = reinos::game::with_match_settings(d, settings);
    REQUIRE_MESSAGE(autumn.has_value(), (autumn ? std::string() : autumn.error()));
    CHECK(autumn->engine.world.season.start == kAutumn);

    const std::string scenario = R"(
name = "Invierno"
players = 2
width = 32
height = 32
base = "pradera"
season = "invierno"

[[building]]
player = 0
type = "casa"
at = [3, 3]

[[building]]
player = 1
type = "casa"
at = [26, 26]
)";
    const auto doc = reinos::game::parse_scenario_doc(scenario, d);
    REQUIRE_MESSAGE(doc.has_value(), (doc ? std::string() : doc.error()));
    CHECK(doc->season == "invierno");
    // El del escenario manda sobre el del menú, y va y vuelve por el formato.
    const auto winter = reinos::game::with_scenario(d, *doc, settings);
    REQUIRE_MESSAGE(winter.has_value(), (winter ? std::string() : winter.error()));
    CHECK(winter->engine.world.season.start == kWinter);
    CHECK(reinos::game::scenario_doc_toml(*doc, d).find("season = \"invierno\"") != std::string::npos);

    // Una estación que no existe: error claro, en el escenario y en el menú.
    std::string bad = scenario;
    bad.replace(bad.find("invierno"), 8, "monzon");
    const auto wrong = reinos::game::parse_scenario_doc(bad, d);
    REQUIRE_FALSE(wrong.has_value());
    CHECK(wrong.error().find("monzon") != std::string::npos);
    settings.season = "monzon";
    const auto wrong_menu = reinos::game::with_match_settings(d, settings);
    REQUIRE_FALSE(wrong_menu.has_value());
    CHECK(wrong_menu.error().find("monzon") != std::string::npos);
}
