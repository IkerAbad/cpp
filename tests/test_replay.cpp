// Pruebas de las repeticiones (M5): el formato va y vuelve sin perder nada, rechaza
// ficheros corruptos o de otra versión, y una partida grabada se reproduce con los
// mismos hashes; si se altera una orden, la divergencia aparece en el primer
// checkpoint posterior.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "game/config.hpp"
#include "game/replay.hpp"
#include "sim/world.hpp"
#include "test_helpers.hpp"

using rts::game::DataFile;
using rts::game::Replay;
using rts::game::ReplayCommand;
using rts::game::ReplayRecorder;
using rts::sim::Command;
using rts::sim::CommandType;
using rts::sim::World;
using rts::sim::WorldParams;
using namespace rts::test;

namespace {

constexpr std::int32_t kInterval = 200;

// Humano (jugador 0) con órdenes grabadas contra la IA (jugador 1).
WorldParams replay_game() {
    WorldParams p = test_world_params(0, 16);
    p.setup.seed = 33;
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
    p.ai_players = {{1, 0}};
    return p;
}

std::vector<std::uint32_t> units_of(const World& world, rts::sim::PlayerId player) {
    rts::sim::Snapshot s;
    world.write_snapshot(s);
    std::vector<std::uint32_t> ids;
    for (const auto& e : s.entities) {
        if (e.owner == player) {
            ids.push_back(e.id);
        }
    }
    return ids;
}

std::uint32_t own_building(const World& world, rts::sim::PlayerId player) {
    rts::sim::Snapshot s;
    world.write_snapshot(s);
    for (const auto& o : s.objects) {
        if (o.kind == rts::sim::ObjectKind::Building && o.owner == player) {
            return o.id;
        }
    }
    return rts::sim::kNoObject;
}

// Graba ticks ticks: órdenes de movimiento, de producción y una programada a futuro.
Replay record_game(const WorldParams& params, std::int32_t ticks, std::uint64_t* final_hash = nullptr) {
    World world(params);
    ReplayRecorder rec({{"datos.toml", "x = 1\n"}}, kInterval);
    const auto center = own_building(world, 0);
    for (std::int32_t t = 0; t < ticks; ++t) {
        const auto units = units_of(world, 0);
        Command c;
        c.tick = world.tick();
        c.player = 0;
        if (t == 0) {
            c.type = CommandType::Train;
            c.object = center;
            c.kind = kVillager;
            rec.issue(world, c);
            c.tick = world.tick() + 50;  // programada: pasa por la cola de pendientes
            rec.issue(world, c);
        } else if (t % 350 == 100) {
            c.type = CommandType::Move;
            c.units = units;
            c.target = {60 + t % 7, 70 + t % 5};
            rec.issue(world, c);
        }
        world.step();
        rec.after_step(world);
    }
    if (final_hash != nullptr) {
        *final_hash = world.state_hash();
    }
    return rec.finish(world);
}

void check_same(const Command& a, const Command& b) {
    CHECK(a.tick == b.tick);
    CHECK(a.player == b.player);
    CHECK(a.type == b.type);
    CHECK(a.units == b.units);
    CHECK(a.target == b.target);
    CHECK(a.object == b.object);
    CHECK(a.kind == b.kind);
}

}  // namespace

TEST_CASE("Repetición: codificar y decodificar no pierde nada") {
    Replay r;
    r.data = {{"units.toml", "[[unit]]\nname = \"ñandú\"\n"}, {"config/engine.toml", std::string(3, '\0') + "x"}};
    Command c;
    c.tick = 77;
    c.player = 3;
    c.type = CommandType::AttackMove;
    c.units = {1, 2, 0xFFFF'FFF0U};
    c.target = {-5, 1024};
    c.object = 42;
    c.kind = 9;
    r.commands = {{70, c}, {71, Command{}}};
    r.checkpoints = {{200, 0x0123'4567'89ab'cdefULL}, {400, 1}};
    r.end_tick = 401;
    r.end_hash = 0xdead'beefULL;

    const auto bytes = rts::game::encode_replay(r);
    const auto back = rts::game::decode_replay(bytes);
    REQUIRE_MESSAGE(back.has_value(), (back ? std::string() : back.error()));
    CHECK(back->data == r.data);
    REQUIRE(back->commands.size() == 2);
    CHECK(back->commands[0].issued_at == 70);
    check_same(back->commands[0].command, c);
    check_same(back->commands[1].command, Command{});
    REQUIRE(back->checkpoints.size() == 2);
    CHECK(back->checkpoints[0].tick == 200);
    CHECK(back->checkpoints[0].hash == 0x0123'4567'89ab'cdefULL);
    CHECK(back->end_tick == 401);
    CHECK(back->end_hash == 0xdead'beefULL);
}

TEST_CASE("Repetición: se rechazan ficheros ajenos, truncados, corruptos o de otra versión") {
    Replay r;
    r.data = {{"a.toml", "a = 1"}};
    r.commands = {{0, Command{}}};
    r.end_tick = 10;
    const auto good = rts::game::encode_replay(r);
    REQUIRE(rts::game::decode_replay(good).has_value());

    const auto error_of = [](std::vector<std::uint8_t> bytes) {
        const auto d = rts::game::decode_replay(bytes);
        return d ? std::string() : d.error();
    };
    CHECK(error_of({}).find("no es una repetición") != std::string::npos);
    CHECK(error_of(std::vector<std::uint8_t>{0x68, 0x6f, 0x6c, 0x61}).find("no es una repetición") != std::string::npos);

    auto truncated = good;
    truncated.resize(truncated.size() - 3);
    CHECK_FALSE(error_of(truncated).empty());

    // Un byte cambiado en cualquier sitio tras la versión: la suma no cuadra.
    for (std::size_t i = 12; i < good.size(); i += 7) {
        auto flipped = good;
        flipped[i] = static_cast<std::uint8_t>(flipped[i] ^ 0x10U);
        INFO("byte " << i);
        CHECK_FALSE(error_of(flipped).empty());
    }
    auto corrupt = good;
    corrupt[good.size() / 2] = static_cast<std::uint8_t>(corrupt[good.size() / 2] ^ 0x01U);
    CHECK(error_of(corrupt).find("corrupta") != std::string::npos);

    auto other_version = good;
    other_version[8] = static_cast<std::uint8_t>(rts::game::kReplayFormatVersion + 1);
    CHECK(error_of(other_version).find("versión") != std::string::npos);
}

TEST_CASE("Repetición: una partida grabada se reproduce con los mismos hashes") {
    const WorldParams params = replay_game();
    std::uint64_t final_hash = 0;
    const Replay recorded = record_game(params, 2000, &final_hash);
    CHECK(recorded.checkpoints.size() == 2000 / kInterval);
    CHECK(recorded.end_hash == final_hash);
    // Pasa por el formato: lo que se verifica es lo que se leería de disco.
    const auto decoded = rts::game::decode_replay(rts::game::encode_replay(recorded));
    REQUIRE(decoded.has_value());
    const auto result = rts::game::verify_replay(*decoded, params);
    CHECK(result.ok);
    CHECK(result.ticks == 2000);
    CHECK(result.checkpoints == recorded.checkpoints.size());
    CHECK(result.actual_hash == final_hash);
}

TEST_CASE("Repetición: una orden alterada se detecta en el primer checkpoint posterior") {
    const WorldParams params = replay_game();
    const Replay recorded = record_game(params, 2000);
    // Órdenes de movimiento en los ticks 100, 450, 800, 1150...: se altera la del 800.
    Replay tampered = recorded;
    auto it = std::ranges::find(tampered.commands, rts::sim::Tick{800}, &ReplayCommand::issued_at);
    REQUIRE(it != tampered.commands.end());
    it->command.target.x += 3;
    const auto result = rts::game::verify_replay(tampered, params);
    CHECK_FALSE(result.ok);
    REQUIRE(result.diverged_at.has_value());
    // Se aplica en el step del tick 800; el primer hash grabado después es el de 1000.
    CHECK(*result.diverged_at == 1000);

    // Quitar una orden también se nota.
    Replay dropped = recorded;
    dropped.commands.erase(dropped.commands.begin() + 1);
    CHECK_FALSE(rts::game::verify_replay(dropped, params).ok);
}

TEST_CASE("Repetición: con los datos del repositorio, grabada y reproducida desde sus datos copiados") {
    auto data = rts::game::load_game_data(RTS_DATA_DIR);
    REQUIRE_MESSAGE(data.has_value(), (data ? std::string() : data.error()));
    World world(data->engine.world);
    ReplayRecorder rec(data->files, data->engine.replay.checkpoint_interval_ticks);
    Command c;
    c.player = 0;
    c.type = CommandType::Move;
    c.units = units_of(world, 0);
    c.target = {100, 100};
    rec.issue(world, c);
    for (int t = 0; t < 600; ++t) {
        world.step();
        rec.after_step(world);
    }
    const auto decoded = rts::game::decode_replay(rts::game::encode_replay(rec.finish(world)));
    REQUIRE(decoded.has_value());
    const auto replayed_data = rts::game::parse_game_data(decoded->data);
    REQUIRE(replayed_data.has_value());
    const auto result = rts::game::verify_replay(*decoded, replayed_data->engine.world);
    CHECK(result.ok);
    CHECK(result.actual_hash == world.state_hash());
}

TEST_CASE("Partida guardada: cargar y seguir da el mismo estado que no haber parado") {
    auto data = rts::game::load_game_data(RTS_DATA_DIR);
    REQUIRE_MESSAGE(data.has_value(), (data ? std::string() : data.error()));
    constexpr std::int32_t kSaveAt = 600;
    constexpr std::int32_t kEnd = 1200;
    World world(data->engine.world);
    ReplayRecorder rec(data->files, data->engine.replay.checkpoint_interval_ticks);
    std::optional<Replay> save;
    for (std::int32_t t = 0; t < kEnd; ++t) {
        if (t == 0 || t == 700) {
            Command c;
            c.player = 0;
            c.type = CommandType::Move;
            c.units = units_of(world, 0);
            c.target = {90 + t / 100, 90};
            rec.issue(world, c);
        }
        world.step();
        rec.after_step(world);
        if (world.tick() == kSaveAt) {
            // Guardar = la repetición hasta aquí, y por el formato binario de ida y vuelta.
            save = rts::game::decode_replay(rts::game::encode_replay(rec.finish(world))).value();
        }
    }
    const std::uint64_t uninterrupted = world.state_hash();
    REQUIRE(save.has_value());

    auto loaded_data = rts::game::parse_game_data(save->data);
    REQUIRE(loaded_data.has_value());
    World resumed(loaded_data->engine.world);
    ReplayRecorder rec2(loaded_data->files, loaded_data->engine.replay.checkpoint_interval_ticks);
    REQUIRE(rts::game::resume_saved_game(*save, resumed, rec2).has_value());
    CHECK(resumed.tick() == static_cast<rts::sim::Tick>(kSaveAt));
    // Se sigue jugando: la orden del tick 700, como la primera vez.
    for (std::int32_t t = kSaveAt; t < kEnd; ++t) {
        if (t == 700) {
            Command c;
            c.player = 0;
            c.type = CommandType::Move;
            c.units = units_of(resumed, 0);
            c.target = {90 + t / 100, 90};
            rec2.issue(resumed, c);
        }
        resumed.step();
        rec2.after_step(resumed);
    }
    CHECK(resumed.state_hash() == uninterrupted);
    // Y el grabador del cargado tiene la partida entera: se reproduce entera.
    const auto full = rec2.finish(resumed);
    const auto verify = rts::game::verify_replay(full, loaded_data->engine.world);
    CHECK(verify.ok);

    // Una partida guardada manipulada se rechaza al cargar.
    Replay bad = *save;
    bad.end_hash ^= 1U;
    World other(loaded_data->engine.world);
    ReplayRecorder rec3(loaded_data->files, loaded_data->engine.replay.checkpoint_interval_ticks);
    CHECK_FALSE(rts::game::resume_saved_game(bad, other, rec3).has_value());
}
