// Objetivos de escenario y campaña (F4): cada objetivo, quién gana y quién pierde, el
// formato y los capítulos de data/campaigns (se cargan, todo cabe, se juegan igual).

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>

#include <doctest/doctest.h>

#include "game/campaign.hpp"
#include "game/config.hpp"
#include "game/scenario.hpp"
#include "sim/world.hpp"

using rts::game::ScenarioDoc;
using rts::sim::Command;
using rts::sim::CommandType;
using rts::sim::ObjectiveStatus;

namespace {

const rts::game::GameData& game_data() {
    static const rts::game::GameData data = [] {
        auto d = rts::game::load_game_data(RTS_DATA_DIR);
        REQUIRE_MESSAGE(d.has_value(), (d ? std::string() : d.error()));
        return std::move(*d);
    }();
    return data;
}

ScenarioDoc parse(const std::string& text) {
    auto doc = rts::game::parse_scenario_doc(text, game_data());
    REQUIRE_MESSAGE(doc.has_value(), (doc ? std::string() : doc.error()));
    return std::move(*doc);
}

// Prado de 32 x 32 con dos jugadores: un edificio de cada uno lejos del otro y lo que
// añada `extra` (objetos y objetivos). Sin IA: solo cambia lo que ordena la prueba.
rts::sim::WorldParams small_world(const std::string& extra) {
    const std::string text = std::format(R"(
name = "Prueba"
players = 2
width = 32
height = 32
base = "pradera"

[[building]]
player = 0
type = "casa"
at = [3, 3]

[[building]]
player = 1
type = "casa"
at = [26, 26]
{})",
                                         extra);
    rts::sim::WorldParams p = game_data().engine.world;
    p.scenario = parse(text).params;
    p.ai_players.clear();
    return p;
}

std::vector<std::uint32_t> units_of(const rts::sim::World& world, rts::sim::PlayerId player) {
    rts::sim::Snapshot s;
    world.write_snapshot(s);
    std::vector<std::uint32_t> out;
    for (const auto& e : s.entities) {
        if (e.owner == player) {
            out.push_back(e.id);
        }
    }
    return out;
}

std::uint32_t building_of(const rts::sim::World& world, rts::sim::PlayerId player) {
    rts::sim::Snapshot s;
    world.write_snapshot(s);
    for (const auto& o : s.objects) {
        if (o.kind == rts::sim::ObjectKind::Building && o.owner == player) {
            return o.id;
        }
    }
    FAIL("sin edificio");
    return 0;
}

Command order(CommandType type, rts::sim::PlayerId player, std::vector<std::uint32_t> units,
              std::uint32_t object = rts::sim::kNoObject, rts::sim::TileCoord target = {}) {
    Command c;
    c.type = type;
    c.player = player;
    c.units = std::move(units);
    c.object = object;
    c.target = target;
    return c;
}

// Avanza hasta que haya ganador o pasen max_ticks.
void run_until_decided(rts::sim::World& world, int max_ticks) {
    for (int t = 0; t < max_ticks && !world.objectives().winner(); ++t) {
        world.step();
    }
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

constexpr std::string_view kKnights = R"(
[[unit]]
player = {}
type = "caballero"
at = [{}, {}]
count = 6
cols = 3
)";

}  // namespace

TEST_CASE("Objetivos: sin objetivos no hay ganador ni cambia nada") {
    rts::sim::World world(small_world(""));
    CHECK_FALSE(world.objectives().active());
    for (int t = 0; t < 40; ++t) {
        world.step();
    }
    CHECK_FALSE(world.objectives().winner());
    CHECK_FALSE(world.objectives().lost(0));
}

TEST_CASE("Objetivos: sobrevivir gana al cumplirse el plazo y el otro pierde") {
    rts::sim::World world(small_world(R"(
[[objective]]
player = 1
kind = "sobrevivir"
seconds = 2

[[objective]]
player = 0
kind = "destruir"
target = 1
)"));
    run_until_decided(world, 30);
    CHECK_FALSE(world.objectives().winner());
    run_until_decided(world, 200);
    REQUIRE(world.objectives().winner());
    CHECK(*world.objectives().winner() == 1);
    CHECK(world.objectives().lost(0));
    CHECK_FALSE(world.objectives().lost(1));
    CHECK(world.tick() <= 2 * rts::sim::kTicksPerSecond + 20);  // se comprueba cada 10 ticks
}

TEST_CASE("Objetivos: destruir el edificio del rival da la victoria") {
    rts::sim::World world(small_world(std::format(kKnights, 0, 20, 20) + R"(
[[objective]]
player = 0
kind = "destruir"
text = "Derriba la casa"
target = 1
building = "casa"
)"));
    world.issue(order(CommandType::Attack, 0, units_of(world, 0), building_of(world, 1)));
    run_until_decided(world, 6000);
    REQUIRE(world.objectives().winner());
    CHECK(*world.objectives().winner() == 0);
    CHECK(world.objectives().status()[0] == ObjectiveStatus::Done);
    rts::sim::Snapshot s;
    world.write_snapshot(s);
    CHECK(s.winner == 0);
    REQUIRE(s.objectives.size() == 1);
    CHECK(s.objectives[0] == ObjectiveStatus::Done);
}

TEST_CASE("Objetivos: perder lo que hay que conservar es la derrota") {
    rts::sim::World world(small_world(std::format(kKnights, 1, 8, 8) + R"(
[[objective]]
player = 0
kind = "conservar"
building = "casa"

[[objective]]
player = 0
kind = "sobrevivir"
seconds = 3600
)"));
    world.issue(order(CommandType::Attack, 1, units_of(world, 1), building_of(world, 0)));
    for (int t = 0; t < 6000 && !world.objectives().lost(0); ++t) {
        world.step();
    }
    CHECK(world.objectives().lost(0));
    CHECK(world.objectives().status()[0] == ObjectiveStatus::Failed);
    CHECK_FALSE(world.objectives().winner());  // el otro no tiene objetivos que cumplir
}

TEST_CASE("Objetivos: llegar a la zona con bastantes tropas") {
    rts::sim::World world(small_world(std::format(kKnights, 0, 4, 10) + R"(
[[unit]]
player = 0
type = "aldeano"
at = [4, 14]
count = 6

[[objective]]
player = 0
kind = "llegar"
count = 6
zone = [16, 1, 30, 14]
)"));
    // Los aldeanos no son tropa: llegar con ellos no cuenta.
    std::vector<std::uint32_t> villagers;
    std::vector<std::uint32_t> knights;
    rts::sim::Snapshot s;
    world.write_snapshot(s);
    for (const auto& e : s.entities) {
        (game_data().units.types[e.type].name == "aldeano" ? villagers : knights).push_back(e.id);
    }
    world.issue(order(CommandType::Move, 0, villagers, rts::sim::kNoObject, {23, 7}));
    run_until_decided(world, 1200);
    CHECK_FALSE(world.objectives().winner());
    world.issue(order(CommandType::Move, 0, knights, rts::sim::kNoObject, {23, 7}));
    run_until_decided(world, 1200);
    REQUIRE(world.objectives().winner());
    CHECK(*world.objectives().winner() == 0);
}

TEST_CASE("Objetivos: reunir recursos y derrotar al rival") {
    SUBCASE("reunir") {
        rts::sim::World world(small_world(R"(
[[player]]
stock = { comida = 500 }

[[objective]]
player = 0
kind = "reunir"
resource = "comida"
count = 400
)"));
        run_until_decided(world, 20);
        REQUIRE(world.objectives().winner());
        CHECK(*world.objectives().winner() == 0);
    }
    SUBCASE("derrotar") {
        rts::sim::World world(small_world(std::format(kKnights, 0, 20, 20) + R"(
[[objective]]
player = 0
kind = "derrotar"
target = 1
)"));
        world.issue(order(CommandType::Attack, 0, units_of(world, 0), building_of(world, 1)));
        run_until_decided(world, 6000);
        REQUIRE(world.objectives().winner());
        CHECK(world.player_state(1).defeated);
    }
}

TEST_CASE("Objetivos: entran en el hash solo si los hay, y la partida es determinista") {
    const auto with = small_world(std::format(kKnights, 0, 20, 20) + R"(
[[objective]]
player = 0
kind = "destruir"
target = 1
)");
    auto without = with;
    without.scenario.objectives.clear();
    rts::sim::World a(with);
    rts::sim::World b(with);
    rts::sim::World c(without);
    for (rts::sim::World* w : {&a, &b, &c}) {
        w->issue(order(CommandType::Attack, 0, units_of(*w, 0), building_of(*w, 1)));
        for (int t = 0; t < 300; ++t) {
            w->step();
        }
    }
    CHECK(a.state_hash() == b.state_hash());
    CHECK(a.state_hash() != c.state_hash());
}

TEST_CASE("Escenario F4: pinceladas, bosques, bloques, almacenes y objetivos de ida y vuelta") {
    const ScenarioDoc doc = parse(R"(
name = "Con \"comillas\""
briefing = "Dos líneas:\nla segunda."
players = 2
width = 40
height = 30
base = "pradera"
base_level = 2

[[paint]]
at = [5, 5]
to = [[30, 5]]
radius = 1
terrain = "agua"
level = 1

[[paint]]
at = [20, 20]
radius = 3
raise = 2

[[forest]]
at = [30, 22]
radius = 5
density = 500

[[unit]]
player = 1
type = "leva"
at = [2, 20]
count = 7
cols = 3

[[player]]
stock = { comida = 10 }

[[player]]
stock = { oro = 20, hierro = 5 }

[[objective]]
player = 0
kind = "llegar"
text = "Ve allí"
unit = "leva"
count = 3
zone = [10, 10, 12, 12]

[[objective]]
player = 1
kind = "sobrevivir"
seconds = 90
)");
    const auto& d = game_data();
    const auto agua = d.terrain.find("agua");
    REQUIRE(agua);
    const auto at = [&](std::int32_t x, std::int32_t y) { return static_cast<std::size_t>(y * 40 + x); };
    CHECK(doc.params.terrain[at(5, 5)] == *agua);
    CHECK(doc.params.terrain[at(30, 6)] == *agua);
    CHECK(doc.params.terrain[at(5, 8)] != *agua);
    CHECK(doc.params.elevation[at(20, 20)] == 4);
    CHECK(doc.params.elevation[at(20, 24)] == 2);
    const auto units = std::ranges::count(doc.params.placements, rts::sim::ScenarioPlacement::Kind::Unit,
                                          &rts::sim::ScenarioPlacement::kind);
    const auto trees = std::ranges::count(doc.params.placements, rts::sim::ScenarioPlacement::Kind::Node,
                                          &rts::sim::ScenarioPlacement::kind);
    CHECK(units == 7);
    CHECK(trees > 10);
    CHECK(trees < 70);  // densidad 500 por mil en un círculo de radio 5 (~80 casillas)
    REQUIRE(doc.params.stocks.size() == 2);
    CHECK(doc.params.stocks[1][rts::sim::resource_index(rts::sim::Resource::Gold)] == 20);
    REQUIRE(doc.params.objectives.size() == 2);
    CHECK(doc.params.objectives[0].units);
    CHECK(doc.params.objectives[1].ticks == 90 * rts::sim::kTicksPerSecond);
    // Mismo fichero, mismos árboles; y vuelta por el formato del editor.
    CHECK(parse(rts::game::scenario_doc_toml(doc, d)).params.placements.size() == doc.params.placements.size());
    const ScenarioDoc back = parse(rts::game::scenario_doc_toml(doc, d));
    CHECK(back.name == doc.name);
    CHECK(back.briefing == doc.briefing);
    CHECK(back.objective_texts == doc.objective_texts);
    CHECK(back.params.stocks == doc.params.stocks);
    CHECK(back.params.terrain == doc.params.terrain);
    REQUIRE(back.params.objectives.size() == 2);
    CHECK(back.params.objectives[0].type == doc.params.objectives[0].type);
    CHECK(back.params.objectives[0].zone_max.x == 12);
    CHECK(back.params.objectives[1].ticks == doc.params.objectives[1].ticks);
}

TEST_CASE("Brecha: un muro derribado deja escombros que se cruzan; una casa derribada, no") {
    rts::sim::World world(small_world(R"(
[[building]]
player = 1
type = "muralla"
at = [12, 12]

[[unit]]
player = 0
type = "ariete"
at = [10, 12]
count = 2
cols = 1

[[unit]]
player = 0
type = "ariete"
at = [22, 26]
count = 2
cols = 1
)"));
    rts::sim::Snapshot s;
    world.write_snapshot(s);
    std::uint32_t wall = 0;
    std::uint32_t house = 0;
    for (const auto& o : s.objects) {
        const std::string& name = game_data().buildings.types[o.type].name;
        wall = name == "muralla" ? o.id : wall;
        house = name == "casa" && o.owner == 1 ? o.id : house;
    }
    std::vector<std::uint32_t> near_wall;
    std::vector<std::uint32_t> near_house;
    for (const auto& e : s.entities) {
        (rts::sim::tile_of(e.pos).x < 16 ? near_wall : near_house).push_back(e.id);
    }
    world.issue(order(CommandType::Attack, 0, near_wall, wall));
    world.issue(order(CommandType::Attack, 0, near_house, house));
    const auto& grid = world.movement().grid();
    for (int t = 0; t < 12000 && (!grid.passable({12, 12}) || world.object_at({26, 26}).value_or(0) == house); ++t) {
        world.step();
    }
    world.write_snapshot(s);
    const auto node_at = [&](rts::sim::TileCoord c) -> std::string {
        for (const auto& o : s.objects) {
            if (o.kind == rts::sim::ObjectKind::Resource && o.origin.x == c.x && o.origin.y == c.y) {
                return game_data().nodes.types[o.type].name;
            }
        }
        return {};
    };
    CHECK(node_at({12, 12}) == "brecha_piedra");
    CHECK(grid.passable({12, 12}));
    CHECK(node_at({26, 26}) == "escombros_madera");
    CHECK_FALSE(grid.passable({26, 26}));
}

TEST_CASE("Escenario F4: un campamento puede empezar abastecido, hasta lo que cabe") {
    rts::sim::World world(small_world(R"(
[[building]]
player = 0
type = "campamento"
at = [10, 10]
store = { comida = 250, hierro = 200 }
)"));
    rts::sim::Snapshot s;
    world.write_snapshot(s);
    const auto camp = std::ranges::find_if(s.objects, [](const rts::sim::SnapshotObject& o) {
        return o.kind == rts::sim::ObjectKind::Building && game_data().buildings.types[o.type].name == "campamento";
    });
    REQUIRE(camp != s.objects.end());
    const auto capacity = game_data().buildings.types[camp->type].type.store_capacity;
    CHECK(camp->store[rts::sim::resource_index(rts::sim::Resource::Food)] == 250);
    CHECK(camp->store[rts::sim::resource_index(rts::sim::Resource::Iron)] == capacity - 250);
}

TEST_CASE("Escenario F4: errores claros") {
    const auto& d = game_data();
    const std::string head = "players = 2\nwidth = 10\nheight = 10\nbase = \"pradera\"\n";
    const auto error = [&](const std::string& rest) {
        const auto r = rts::game::parse_scenario_doc(head + rest, d);
        REQUIRE_FALSE(r.has_value());
        return r.error();
    };
    CHECK(error("[[objective]]\nplayer = 0\nkind = \"ganar\"\n").find("'kind'") != std::string::npos);
    CHECK(error("[[objective]]\nplayer = 0\nkind = \"llegar\"\n").find("zone") != std::string::npos);
    CHECK(error("[[objective]]\nplayer = 5\nkind = \"sobrevivir\"\nseconds = 3\n").find("'player'") != std::string::npos);
    CHECK(error("[[paint]]\nat = [1, 1]\nterrain = \"lava\"\n").find("lava") != std::string::npos);
    CHECK(error("[[unit]]\nplayer = 0\ntype = \"leva\"\nat = [8, 8]\ncount = 9\n").find("fuera") != std::string::npos);
    CHECK(error("[[player]]\nstock = { trigo = 3 }\n").find("trigo") != std::string::npos);
}

TEST_CASE("Campaña: progreso y capítulos desbloqueados") {
    const auto c = rts::game::parse_campaign(R"(
name = "C"
[[chapter]]
id = "a"
title = "A"
scenario = "a.toml"
[[chapter]]
id = "b"
title = "B"
scenario = "b.toml"
)");
    REQUIRE(c.has_value());
    rts::game::Campaign camp = *c;
    camp.id = "c";
    rts::game::CampaignProgress p;
    CHECK(p.unlocked(camp, 0));
    CHECK_FALSE(p.unlocked(camp, 1));
    p.mark_won("c", "a");
    p.mark_won("c", "a");
    CHECK(p.won.size() == 1);
    CHECK(p.unlocked(camp, 1));
    const auto back = rts::game::parse_progress(rts::game::progress_toml(p));
    CHECK(back.won == p.won);
    CHECK(rts::game::parse_progress("esto no es toml [").won.empty());
    CHECK_FALSE(rts::game::parse_campaign("name = \"x\"\n").has_value());
}

// Los capítulos de verdad: se cargan, todo lo colocado cabe, la tropa propia puede llegar
// a las zonas de sus objetivos y, con la IA en los dos bandos, la partida es determinista.
TEST_CASE("Campaña: los capítulos de data/campaigns se cargan y se juegan") {
    const auto& d = game_data();
    std::vector<std::string> errors;
    const auto campaigns = rts::game::load_campaigns(RTS_DATA_DIR, errors);
    for (const std::string& e : errors) {
        FAIL_CHECK(e);
    }
    REQUIRE_FALSE(campaigns.empty());
    for (const rts::game::Campaign& c : campaigns) {
        for (const rts::game::CampaignChapter& ch : c.chapters) {
            CAPTURE(ch.id);
            CHECK_FALSE(ch.sources.empty());
            CHECK_FALSE(ch.history.empty());
            const std::string text = read_file(c.dir / ch.scenario);
            REQUIRE_FALSE(text.empty());
            const auto doc = rts::game::parse_scenario_doc(text, d, ch.scenario);
            REQUIRE_MESSAGE(doc.has_value(), (doc ? std::string() : doc.error()));
            CHECK_FALSE(doc->briefing.empty());
            REQUIRE_FALSE(doc->params.objectives.empty());
            CHECK(doc->objective_texts.size() == doc->params.objectives.size());
            CHECK(std::ranges::find(d.engine.ai_profile_names, ch.rival) != d.engine.ai_profile_names.end());

            rts::game::MatchSettings settings;
            settings.rival = ch.rival;
            settings.fog = ch.fog;
            const auto data = rts::game::with_scenario(d, *doc, settings);
            REQUIRE_MESSAGE(data.has_value(), (data ? std::string() : data.error()));
            rts::sim::World world(data->engine.world);
            rts::sim::Snapshot s;
            world.write_snapshot(s);
            const auto placed = [&](rts::sim::ScenarioPlacement::Kind k) {
                return static_cast<std::size_t>(
                    std::ranges::count(doc->params.placements, k, &rts::sim::ScenarioPlacement::kind));
            };
            // Nada se queda fuera por caer en agua, roca o encima de otra cosa.
            CHECK(s.entities.size() == placed(rts::sim::ScenarioPlacement::Kind::Unit));
            CHECK(s.objects.size() == placed(rts::sim::ScenarioPlacement::Kind::Building) +
                                          placed(rts::sim::ScenarioPlacement::Kind::Node));
            // Zonas de llegar: alcanzables por tierra desde la tropa del jugador.
            const auto& grid = world.movement().grid();
            const auto mine = std::ranges::find(s.entities, rts::sim::PlayerId{0}, &rts::sim::SnapshotEntity::owner);
            REQUIRE(mine != s.entities.end());
            const std::uint32_t home = grid.component(rts::sim::tile_of(mine->pos));
            for (const rts::sim::Objective& o : doc->params.objectives) {
                if (o.kind != rts::sim::ObjectiveKind::Reach || o.player != 0) {
                    continue;
                }
                std::int32_t reachable = 0;
                for (std::int32_t y = o.zone_min.y; y <= o.zone_max.y; ++y) {
                    for (std::int32_t x = o.zone_min.x; x <= o.zone_max.x; ++x) {
                        reachable += grid.component({x, y}) == home ? 1 : 0;
                    }
                }
                CHECK(reachable >= o.count);
            }
            // IA en los dos bandos, dos veces: mismo hash; los objetivos siguen sin decidir
            // al principio (nadie gana en el primer minuto).
            auto params = data->engine.world;
            params.ai_players = {{0, params.ai_players.front().profile}, {1, params.ai_players.front().profile}};
            rts::sim::World a(params);
            rts::sim::World b(params);
            for (int t = 0; t < 600; ++t) {
                a.step();
                b.step();
            }
            CHECK(a.state_hash() == b.state_hash());
            CHECK_FALSE(a.objectives().winner());
        }
    }
}

namespace {

// Jugador con guion para los capítulos: la tropa del jugador 0 recorre `route` en
// ataque-movimiento (pasa al punto siguiente cuando la mitad ha llegado cerca) y la IA
// juega el otro bando. Devuelve el ganador (o -1), el tick y la tropa que le queda.
struct Outcome {
    int winner = -1;
    rts::sim::Tick tick = 0;
    std::size_t army = 0;
    std::size_t army_start = 0;
};

Outcome scripted(const std::string& chapter, const std::vector<rts::sim::TileCoord>& route,
                                       int max_ticks) {
    const auto& d = game_data();
    std::vector<std::string> errors;
    const auto campaigns = rts::game::load_campaigns(RTS_DATA_DIR, errors);
    const rts::game::Campaign& c = campaigns.front();
    const auto ch = std::ranges::find(c.chapters, chapter, &rts::game::CampaignChapter::id);
    const auto doc = rts::game::parse_scenario_doc(read_file(c.dir / ch->scenario), d);
    rts::game::MatchSettings settings;
    settings.rival = ch->rival;
    settings.fog = ch->fog;
    const auto data = rts::game::with_scenario(d, *doc, settings);
    rts::sim::World world(data->engine.world);
    std::size_t leg = 0;
    rts::sim::Snapshot s;
    Outcome out;
    for (int t = 0; t < max_ticks && !world.objectives().winner() && !world.objectives().lost(0); ++t) {
        if (t % 100 == 0) {
            world.write_snapshot(s);
            std::vector<std::uint32_t> army;
            int near = 0;
            for (const auto& e : s.entities) {
                const auto& ut = d.units.types[e.type];
                // Todo lo que pelea: tropa, arietes y zapadores; no aldeanos, bagaje ni cirujanos.
                if (e.owner != 0 || ut.type.worker || ut.type.convoy_capacity > 0 || ut.type.care_skill > 0) {
                    continue;
                }
                army.push_back(e.id);
                const auto tile = rts::sim::tile_of(e.pos);
                near += std::abs(tile.x - route[leg].x) + std::abs(tile.y - route[leg].y) <= 8 ? 1 : 0;
            }
            out.army = army.size();
            out.army_start = std::max(out.army_start, army.size());
            if (leg + 1 < route.size() && near * 2 >= static_cast<int>(army.size())) {
                ++leg;
            }
            if (t % 400 == 0 || near * 2 >= static_cast<int>(army.size())) {
                world.issue(order(CommandType::AttackMove, 0, army, rts::sim::kNoObject, route[leg]));
            }
        }
        world.step();
    }
    const auto w = world.objectives().winner();
    out.winner = w ? static_cast<int>(*w) : -1;
    out.tick = world.tick();
    return out;
}

}  // namespace

// Equilibrio del capítulo 3: el guion más simple (todo el ejército en ataque-movimiento,
// sin formaciones) gana por la senda del oeste y se deja el ejército forzando el puerto
// guardado. Es lo que cuenta la historia: el paso lo decide todo.
// Lenta sin optimizar: corre en Release (trabajo de banco del CI).
#ifdef NDEBUG
constexpr bool kSlowSkipped = false;
#else
constexpr bool kSlowSkipped = true;
#endif

TEST_CASE("Campaña: Las Navas se gana por la senda del oeste y no por el puerto guardado" *
          doctest::skip(kSlowSkipped)) {
    const Outcome west = scripted("navas", {{15, 30}, {18, 41}, {11, 46}, {19, 51}, {15, 62}, {64, 80}, {64, 93}},
                                  2700 * rts::sim::kTicksPerSecond);
    MESSAGE("senda: ganador ", west.winner, " en el tick ", west.tick, ", tropa ", west.army, " de ", west.army_start);
    CHECK(west.winner == 0);
    const Outcome pass = scripted("navas", {{64, 30}, {64, 62}, {64, 80}, {64, 93}}, 10'000);
    MESSAGE("puerto: tropa ", pass.army, " de ", pass.army_start, " en el tick ", pass.tick);
    CHECK(pass.winner == -1);
    CHECK(pass.army * 2 < pass.army_start);
}

// Capítulo 2: con un plan sensato (la tropa en guardia junto al campamento, que las
// carretas abastecen desde Malagón; los arietes a la puerta y, abierta la brecha, todos
// a por el alcázar) se toma Calatrava antes del plazo.
TEST_CASE("Campaña: Calatrava se toma con arietes, campamento abastecido y asalto por la brecha" *
          doctest::skip(kSlowSkipped)) {
    const auto& d = game_data();
    std::vector<std::string> errors;
    const auto campaigns = rts::game::load_campaigns(RTS_DATA_DIR, errors);
    const rts::game::Campaign& c = campaigns.front();
    const auto ch = std::ranges::find(c.chapters, "calatrava", &rts::game::CampaignChapter::id);
    const auto doc = rts::game::parse_scenario_doc(read_file(c.dir / ch->scenario), d);
    rts::game::MatchSettings settings;
    settings.rival = ch->rival;
    const auto data = rts::game::with_scenario(d, *doc, settings);
    rts::sim::World world(data->engine.world);
    rts::sim::Snapshot s;
    bool breached = false;
    for (int t = 0; t < 36000 && !world.objectives().winner() && !world.objectives().lost(0); ++t) {
        if (t % 100 == 0) {
            world.write_snapshot(s);
            std::uint32_t gate = rts::sim::kNoObject;
            for (const auto& o : s.objects) {
                if (o.kind == rts::sim::ObjectKind::Building && o.owner == 1 &&
                    d.buildings.types[o.type].name == "puerta_muralla") {
                    gate = o.id;
                }
            }
            std::vector<std::uint32_t> rams;
            std::vector<std::uint32_t> army;
            std::vector<std::uint32_t> carts;
            std::uint32_t camp = rts::sim::kNoObject;
            for (const auto& o : s.objects) {
                if (o.kind == rts::sim::ObjectKind::Building && o.owner == 0 &&
                    d.buildings.types[o.type].name == "campamento") {
                    camp = o.id;
                }
            }
            for (const auto& e : s.entities) {
                const auto& ut = d.units.types[e.type];
                if (e.owner == 0 && ut.type.convoy_capacity > 0) {
                    carts.push_back(e.id);
                }
                if (e.owner != 0 || ut.type.worker || ut.type.convoy_capacity > 0) {
                    continue;
                }
                (ut.name == "ariete" ? rams : army).push_back(e.id);
            }
            if (t == 0) {
                world.issue(order(CommandType::Convoy, 0, carts, camp));
            }
            if (gate == rts::sim::kNoObject && !breached) {
                breached = true;
            }
            // Los que tienen hambre, al campamento; los demás, en guardia junto a él o al asalto.
            std::vector<std::uint32_t> fed;
            std::vector<std::uint32_t> hungry;
            for (const std::uint32_t id : army) {
                const auto it = std::ranges::find(s.entities, id, &rts::sim::SnapshotEntity::id);
                (it->hungry || it->rations <= 1 ? hungry : fed).push_back(id);
            }
            if (!hungry.empty()) {
                world.issue(order(CommandType::Move, 0, hungry, rts::sim::kNoObject, {57, 74}));
            }
            if (t % 400 == 0) {
                if (!breached) {
                    world.issue(order(CommandType::Attack, 0, rams, gate));
                    world.issue(order(CommandType::AttackMove, 0, fed, rts::sim::kNoObject, {62, 74}));
                } else {
                    // Los arietes, al alcázar; la tropa limpia el recinto y luego también va a por él.
                    std::uint32_t keep = rts::sim::kNoObject;
                    for (const auto& o : s.objects) {
                        if (o.kind == rts::sim::ObjectKind::Building && o.owner == 1 &&
                            d.buildings.types[o.type].name == "centro_urbano") {
                            keep = o.id;
                        }
                    }
                    world.issue(order(CommandType::Attack, 0, rams, keep));
                    world.issue(t % 1200 == 0 ? order(CommandType::Attack, 0, fed, keep)
                                              : order(CommandType::AttackMove, 0, fed, rts::sim::kNoObject, {88, 74}));
                }
            }
        }
        world.step();
    }
    const auto w = world.objectives().winner();
    MESSAGE("calatrava: ganador ", w ? static_cast<int>(*w) : -1, " en el tick ", world.tick());
    CHECK(breached);
    REQUIRE(w);
    CHECK(*w == 0);
}

// Capítulo 1: con la economía en manos de la IA y la tropa enviada al campo de la reunión
// en cuanto hay 30, ese objetivo se cumple bastante antes del plazo (los puentes llevan
// al otro lado del Tajo y la población da para el ejército).
TEST_CASE("Campaña: en Toledo se puede reunir la hueste al otro lado del Tajo" * doctest::skip(kSlowSkipped)) {
    const auto& d = game_data();
    std::vector<std::string> errors;
    const auto campaigns = rts::game::load_campaigns(RTS_DATA_DIR, errors);
    const rts::game::Campaign& c = campaigns.front();
    const auto doc = rts::game::parse_scenario_doc(read_file(c.dir / "1_toledo.toml"), d);
    rts::game::MatchSettings settings;
    settings.rival = "normal";
    const auto data = rts::game::with_scenario(d, *doc, settings);
    auto params = data->engine.world;
    params.ai_players = {{0, params.ai_players.front().profile}};
    rts::sim::World world(params);
    rts::sim::Snapshot s;
    while (world.objectives().status()[1] != ObjectiveStatus::Done && world.tick() < 2400 * 20) {
        if (world.tick() % 200 == 0) {
            world.write_snapshot(s);
            std::vector<std::uint32_t> army;
            for (const auto& e : s.entities) {
                if (e.owner == 0 && d.units.types[e.type].type.morale_resolve > 0) {
                    army.push_back(e.id);
                }
            }
            if (army.size() >= 30) {
                world.issue(order(CommandType::Move, 0, army, rts::sim::kNoObject, {65, 81}));
            }
        }
        world.step();
    }
    MESSAGE("toledo: tropa reunida en el minuto ", world.tick() / 1200);
    CHECK(world.objectives().status()[1] == ObjectiveStatus::Done);
    CHECK(world.tick() < 30 * 1200);
    CHECK_FALSE(world.objectives().winner());
}
