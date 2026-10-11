// Escenarios hechos a mano (F3): formato, ida y vuelta, partida jugable y ediciones.

#include <algorithm>
#include <array>
#include <string>

#include <doctest/doctest.h>

#include "game/config.hpp"
#include "game/replay.hpp"
#include "game/scenario.hpp"
#include "sim/world.hpp"

using rts::game::ScenarioDoc;
using rts::sim::ScenarioPlacement;

namespace {

const rts::game::GameData& game_data() {
    static const rts::game::GameData data = [] {
        auto d = rts::game::load_game_data(RTS_DATA_DIR);
        REQUIRE_MESSAGE(d.has_value(), (d ? std::string() : d.error()));
        return std::move(*d);
    }();
    return data;
}

// El mundo generado de siempre, como escenario de dos jugadores.
ScenarioDoc generated_doc() {
    const rts::sim::World world(game_data().engine.world);
    return rts::game::scenario_from_world(world, "Prueba", 2);
}

std::size_t count(const rts::sim::ScenarioParams& s, ScenarioPlacement::Kind k) {
    return static_cast<std::size_t>(std::ranges::count(s.placements, k, &ScenarioPlacement::kind));
}

}  // namespace

TEST_CASE("Escenario: un mundo capturado sale y vuelve igual del fichero") {
    const auto& d = game_data();
    const ScenarioDoc doc = generated_doc();
    CHECK(doc.params.width == d.engine.world.map.width);
    CHECK(count(doc.params, ScenarioPlacement::Kind::Building) == 2);
    CHECK(count(doc.params, ScenarioPlacement::Kind::Unit) > 0);
    CHECK(count(doc.params, ScenarioPlacement::Kind::Node) > 100);
    const std::string text = rts::game::scenario_doc_toml(doc, d);
    const auto back = rts::game::parse_scenario_doc(text, d);
    REQUIRE_MESSAGE(back.has_value(), (back ? std::string() : back.error()));
    CHECK(back->name == "Prueba");
    CHECK(back->params.players == 2);
    CHECK(back->params.terrain == doc.params.terrain);
    CHECK(back->params.elevation == doc.params.elevation);
    REQUIRE(back->params.placements.size() == doc.params.placements.size());
}

TEST_CASE("Escenario: se juega, con el mismo mapa y los mismos objetos, y su repetición se verifica") {
    const auto& d = game_data();
    const ScenarioDoc doc = generated_doc();
    rts::game::MatchSettings settings;
    settings.rival = d.engine.ai_profile_names.front();
    settings.seed = 99;  // con escenario, la semilla no cambia el mapa
    const auto data = rts::game::with_scenario(d, doc, settings);
    REQUIRE_MESSAGE(data.has_value(), (data ? std::string() : data.error()));
    CHECK(data->scenario_name == "Prueba");
    rts::sim::World world(data->engine.world);
    const rts::sim::World original(d.engine.world);
    for (std::int32_t y = 0; y < world.map().height(); y += 7) {
        for (std::int32_t x = 0; x < world.map().width(); x += 7) {
            REQUIRE(world.map().terrain({x, y}) == original.map().terrain({x, y}));
        }
    }
    rts::sim::Snapshot a;
    rts::sim::Snapshot b;
    world.write_snapshot(a);
    original.write_snapshot(b);
    CHECK(a.objects.size() == b.objects.size());
    CHECK(a.entities.size() == b.entities.size());
    // Se juega (la IA del jugador 1 decide) y la repetición, con el escenario dentro, se verifica.
    rts::game::ReplayRecorder rec(data->files, data->engine.replay.checkpoint_interval_ticks);
    for (int t = 0; t < 400; ++t) {
        world.step();
        rec.after_step(world);
    }
    const auto replay = rec.finish(world);
    const auto replayed = rts::game::parse_game_data(replay.data);
    REQUIRE(replayed.has_value());
    CHECK(rts::game::verify_replay(replay, replayed->engine.world).ok);
}

TEST_CASE("Escenario: pintar, subir, bajar y borrar") {
    const auto& d = game_data();
    ScenarioDoc doc = generated_doc();
    rts::sim::ScenarioParams& s = doc.params;
    const rts::sim::TileCoord c{100, 100};
    rts::game::paint_terrain(s, c, 2, 4);
    // Un rombo de radio 2: 13 casillas.
    std::int32_t painted = 0;
    for (std::int32_t y = 96; y <= 104; ++y) {
        for (std::int32_t x = 96; x <= 104; ++x) {
            const bool in = std::abs(x - c.x) + std::abs(y - c.y) <= 2;
            const auto t = s.terrain[static_cast<std::size_t>(y * s.width + x)];
            painted += in && t == 4 ? 1 : 0;
        }
    }
    CHECK(painted == 13);
    const std::int32_t max_level = d.engine.world.map.elevation_levels - 1;
    rts::game::raise(s, c, 0, 100, max_level);
    CHECK(s.elevation[static_cast<std::size_t>(c.y * s.width + c.x)] == max_level);
    rts::game::raise(s, c, 0, -100, max_level);
    CHECK(s.elevation[static_cast<std::size_t>(c.y * s.width + c.x)] == 0);
    // Borrar: primero la unidad que hay en la casilla, luego el edificio.
    const auto building = std::ranges::find(s.placements, ScenarioPlacement::Kind::Building, &ScenarioPlacement::kind);
    REQUIRE(building != s.placements.end());
    const rts::sim::TileCoord on = building->at;
    s.placements.push_back({ScenarioPlacement::Kind::Unit, 0, 0, on});
    const auto before = s.placements.size();
    CHECK(rts::game::erase_at(s, on, d));
    CHECK(s.placements.size() == before - 1);
    CHECK(count(s, ScenarioPlacement::Kind::Building) == 2);  // la unidad se fue antes
    CHECK(rts::game::erase_at(s, on, d));
    CHECK(count(s, ScenarioPlacement::Kind::Building) == 1);
    // En una casilla sin nada (agua del borde, sin objetos), no se borra nada.
    rts::sim::ScenarioParams empty = s;
    empty.placements.clear();
    CHECK_FALSE(rts::game::erase_at(empty, {0, 0}, d));
}

TEST_CASE("Escenario: errores claros en el fichero") {
    const auto& d = game_data();
    const std::string good = rts::game::scenario_doc_toml(generated_doc(), d);
    const auto broken = [&](std::string_view from, std::string_view to) {
        std::string t = good;
        const auto at = t.find(from);
        REQUIRE(at != std::string::npos);
        t.replace(at, from.size(), to);
        return rts::game::parse_scenario_doc(t, d);
    };
    CHECK(broken("players = 2", "players = 9").error().find("players") != std::string::npos);
    CHECK(broken("type = \"centro_urbano\"", "type = \"castillo\"").error().find("castillo") != std::string::npos);
    CHECK(broken("width = 256", "width = 255").error().find("casillas") != std::string::npos);
    // Una letra que no es un terreno.
    std::string bad_letter = good;
    bad_letter[bad_letter.find("terrain = [\n  \"") + 15] = 'z';  // la primera casilla
    const auto letter = rts::game::parse_scenario_doc(bad_letter, d);
    REQUIRE_FALSE(letter.has_value());
    CHECK(letter.error().find("terreno") != std::string::npos);
}

TEST_CASE("Escenario: el mapa reflejado es igual visto desde los dos jugadores") {
    const auto& d = game_data();
    const ScenarioDoc doc = rts::game::mirrored_scenario(generated_doc(), d);
    const auto& s = doc.params;
    REQUIRE(s.width == s.height);
    const std::int32_t n = s.width;
    for (std::int32_t y = 0; y < n; y += 3) {
        for (std::int32_t x = 0; x < n; x += 3) {
            const auto a = static_cast<std::size_t>(y * n + x);
            const auto b = static_cast<std::size_t>((n - 1 - y) * n + (n - 1 - x));
            REQUIRE(s.terrain[a] == s.terrain[b]);
            REQUIRE(s.elevation[a] == s.elevation[b]);
        }
    }
    rts::sim::WorldParams p = d.engine.world;
    p.scenario = s;
    const rts::sim::World world(p);
    rts::sim::Snapshot snap;
    world.write_snapshot(snap);
    std::array<std::size_t, 2> units{};
    std::array<std::size_t, 2> buildings{};
    std::size_t nodes = 0;
    for (const auto& e : snap.entities) {
        ++units.at(e.owner);
    }
    for (const auto& o : snap.objects) {
        if (o.kind == rts::sim::ObjectKind::Building) {
            ++buildings.at(o.owner);
        } else {
            ++nodes;
        }
    }
    CHECK(units[0] > 0);
    CHECK(units[0] == units[1]);
    CHECK(buildings[0] == 1);
    CHECK(buildings[1] == 1);
    CHECK(nodes % 2 == 0);  // cada recurso y su reflejo
    CHECK(world.player_state(0).stock == world.player_state(1).stock);
}

TEST_CASE("Mapa Espejo: los dos jugadores empiezan igual y la repetición se reproduce") {
    rts::game::MatchSettings settings;
    settings.seed = 5;
    settings.rival = game_data().engine.ai_profile_names.front();
    settings.map = "Espejo";
    const auto data = rts::game::with_match_settings(game_data(), settings);
    REQUIRE_MESSAGE(data.has_value(), (data ? std::string() : data.error()));
    REQUIRE(data->engine.world.scenario.active());
    rts::sim::World world(data->engine.world);
    const rts::sim::TileMap& map = world.map();
    const std::int32_t n = map.width();
    for (std::int32_t y = 0; y < n; y += 5) {
        for (std::int32_t x = 0; x < n; x += 5) {
            REQUIRE(map.terrain({x, y}) == map.terrain({n - 1 - x, n - 1 - y}));
        }
    }
    rts::game::ReplayRecorder rec(data->files, data->engine.replay.checkpoint_interval_ticks);
    for (int t = 0; t < 300; ++t) {
        world.step();
        rec.after_step(world);
    }
    const auto replay = rec.finish(world);
    const auto replayed = rts::game::parse_game_data(replay.data);
    REQUIRE(replayed.has_value());
    CHECK(rts::game::verify_replay(replay, replayed->engine.world).ok);
}

TEST_CASE("Vados: se pasa por encima pero no se construye") {
    const auto& d = game_data();
    const auto doc = rts::game::parse_scenario_doc(R"(
players = 2
width = 20
height = 20
base = "pradera"

[[paint]]
at = [10, 0]
to = [[10, 19]]
radius = 1
terrain = "vado"
)",
                                                   d);
    REQUIRE(doc.has_value());
    rts::sim::WorldParams p = d.engine.world;
    p.scenario = doc->params;
    p.ai_players.clear();
    const rts::sim::World world(p);
    const auto casa = std::ranges::find(d.buildings.types, std::string("casa"), &rts::game::BuildingInfo::name);
    REQUIRE(casa != d.buildings.types.end());
    const auto type = static_cast<rts::sim::BuildingTypeId>(casa - d.buildings.types.begin());
    CHECK(world.can_place(type, {3, 5}));
    CHECK_FALSE(world.can_place(type, {9, 5}));   // pisa el vado
    CHECK_FALSE(world.can_place(type, {11, 5}));  // también
    CHECK(world.movement().grid().passable({10, 5}));
}
