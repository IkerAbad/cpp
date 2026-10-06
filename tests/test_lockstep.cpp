// Partidas en red por lockstep (E1): códec de órdenes, dos sesiones en el mismo proceso
// por la interfaz de bucle local, desincronización con su tick, rechazos y plazos.

#include <algorithm>
#include <chrono>
#include <memory>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "game/config.hpp"
#include "game/lockstep.hpp"
#include "game/replay.hpp"
#include "game/wire.hpp"
#include "net/socket.hpp"
#include "sim/world.hpp"

using rts::game::GameData;
using rts::game::LockstepSession;
using rts::game::LockstepState;
using rts::sim::Command;
using rts::sim::CommandType;
using rts::sim::World;

namespace {

const GameData& game_data() {
    static const GameData data = [] {
        auto d = rts::game::load_game_data(RTS_DATA_DIR);
        REQUIRE_MESSAGE(d.has_value(), (d ? std::string() : d.error()));
        return std::move(*d);
    }();
    return data;
}

constexpr std::uint16_t kAnyPort = 0;

// Un jugador: su sesión, su mundo y las órdenes de otros que le llegaron.
struct Side {
    LockstepSession session;
    World world;
    rts::sim::Xoshiro256pp rng;
    std::size_t remote_orders = 0;

    Side(LockstepSession s, const GameData& data)
        : session(std::move(s)), world(data.engine.world), rng(data.engine.world.setup.seed + 1) {}
    Side(const Side&) = delete;
    Side& operator=(const Side&) = delete;

    // Un paso del bucle de juego: órdenes de prueba, red y, si se puede, un tick.
    void advance(const GameData& data, std::optional<Command>& extra) {
        const auto& probe = data.engine.net.probe;
        if (world.tick() % static_cast<rts::sim::Tick>(probe.every_ticks) == 0 && last_probe != world.tick()) {
            last_probe = world.tick();
            rts::sim::Snapshot snap;
            world.write_snapshot(snap);
            for (Command& c : rts::game::probe_orders(snap, session.local_player(), probe,
                                                      {world.map().width(), world.map().height()}, rng)) {
                session.submit(std::move(c));
            }
        }
        session.poll(0);
        if (world.tick() >= session.end_tick() || session.state() != LockstepState::Running) {
            return;
        }
        const bool go = session.begin_tick(world, [this](Command c) {
            if (c.player != session.local_player()) {
                ++remote_orders;
            }
            world.issue(std::move(c));
        });
        if (go) {
            // Desincronización provocada: una orden que solo ve este mundo.
            if (extra && extra->tick == world.tick()) {
                world.issue(*extra);
                extra.reset();
            }
            world.step();
        }
    }

    std::optional<rts::sim::Tick> last_probe;
};

std::vector<std::uint32_t> units_of(const World& w, rts::sim::PlayerId p) {
    rts::sim::Snapshot snap;
    w.write_snapshot(snap);
    std::vector<std::uint32_t> ids;
    for (const auto& e : snap.entities) {
        if (e.owner == p) {
            ids.push_back(e.id);
        }
    }
    return ids;
}

// Juega hasta que todas las sesiones acaben (o fallen). extra solo lo ve el último.
void play(const GameData& data, const std::vector<Side*>& sides, std::optional<Command> extra = std::nullopt) {
    std::optional<Command> none;
    for (int guard = 0; guard < 1'000'000; ++guard) {
        bool all_over = true;
        for (Side* s : sides) {
            s->advance(data, s == sides.back() ? extra : none);
            if (s->session.state() == LockstepState::Running && s->world.tick() >= s->session.end_tick()) {
                s->session.finish(s->world);
            }
            all_over = all_over && (s->session.state() == LockstepState::Finished ||
                                    s->session.state() == LockstepState::Failed);
        }
        if (all_over) {
            return;
        }
    }
    FAIL("la partida no terminó");
}

// Anfitrión y un invitado, ya en partida.
std::pair<LockstepSession, LockstepSession> connect_pair(const GameData& data, rts::sim::Tick end_tick,
                                                         const rts::game::LockstepConfig& cfg) {
    const std::uint64_t hash = rts::game::data_hash(data.files);
    auto host = LockstepSession::host(kAnyPort, 2, cfg, hash, end_tick);
    REQUIRE_MESSAGE(host.has_value(), (host ? std::string() : host.error()));
    auto guest = LockstepSession::join("127.0.0.1", host->port(), cfg, hash);
    REQUIRE_MESSAGE(guest.has_value(), (guest ? std::string() : guest.error()));
    for (int i = 0; i < 10'000 && (host->state() == LockstepState::Lobby || guest->state() == LockstepState::Lobby);
         ++i) {
        host->poll(1);
        guest->poll(1);
    }
    REQUIRE(host->state() == LockstepState::Running);
    REQUIRE(guest->state() == LockstepState::Running);
    return {std::move(*host), std::move(*guest)};
}

}  // namespace

TEST_CASE("Red: una orden sale y vuelve igual del códec") {
    Command c;
    c.tick = 1234;
    c.player = 3;
    c.type = CommandType::Attack;
    c.units = {7, 8, 9};
    c.target = {-5, 40};
    c.object = 77;
    c.kind = 2;
    rts::game::ByteWriter w;
    rts::game::write_command(w, c);
    rts::game::ByteReader r(w.out());
    Command d;
    REQUIRE(rts::game::read_command(r, d));
    CHECK(d.tick == c.tick);
    CHECK(d.player == c.player);
    CHECK(d.type == c.type);
    CHECK(d.units == c.units);
    CHECK(d.target == c.target);
    CHECK(d.object == c.object);
    CHECK(d.kind == c.kind);
    // Truncada: falla sin leer fuera del búfer.
    std::vector<std::uint8_t> cut(w.out().begin(), w.out().end() - 1);
    rts::game::ByteReader rc(cut);
    CHECK_FALSE(rts::game::read_command(rc, d));
}

TEST_CASE("Red: órdenes de prueba solo para unidades propias y repetibles con la misma semilla") {
    const GameData& data = game_data();
    const World w(data.engine.world);
    rts::sim::Snapshot snap;
    w.write_snapshot(snap);
    rts::sim::Xoshiro256pp r1(1);
    rts::sim::Xoshiro256pp r2(1);
    const rts::sim::TileCoord size{w.map().width(), w.map().height()};
    const auto a = rts::game::probe_orders(snap, 1, data.engine.net.probe, size, r1);
    const auto b = rts::game::probe_orders(snap, 1, data.engine.net.probe, size, r2);
    REQUIRE(!a.empty());
    REQUIRE(a.size() == b.size());
    const auto own = units_of(w, 1);
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].units == b[i].units);
        CHECK(a[i].target == b[i].target);
        CHECK(a[i].player == 1);
        CHECK(std::ranges::find(own, a[i].units.front()) != own.end());
        CHECK(a[i].target.x >= 0);
        CHECK(a[i].target.y >= 0);
        CHECK(a[i].target.x < size.x);
        CHECK(a[i].target.y < size.y);
    }
}

TEST_CASE("Red: dos jugadores por bucle local acaban con el mismo hash, que es el de la repetición") {
    const GameData& data = game_data();
    const auto& cfg = data.engine.net.lockstep;
    constexpr rts::sim::Tick kEnd = 300;
    auto [h, g] = connect_pair(data, kEnd, cfg);
    CHECK(h.local_player() == 0);
    CHECK(g.local_player() == 1);
    CHECK(g.end_tick() == kEnd);  // la duración la fija el anfitrión
    Side host(std::move(h), data);
    Side guest(std::move(g), data);
    play(data, {&host, &guest});
    REQUIRE_MESSAGE(host.session.state() == LockstepState::Finished, host.session.error());
    REQUIRE_MESSAGE(guest.session.state() == LockstepState::Finished, guest.session.error());
    CHECK(host.world.tick() == kEnd);
    CHECK(host.world.state_hash() == guest.world.state_hash());
    // Cada uno ejecutó órdenes que solo conocía el otro: el intercambio funciona.
    CHECK(host.remote_orders > 0);
    CHECK(guest.remote_orders > 0);
    // Hash cada hash_every_turns turnos, comparado con el único otro jugador.
    const auto every = static_cast<rts::sim::Tick>(cfg.turn_ticks * cfg.hash_every_turns);
    CHECK(host.session.hashes_compared() >= kEnd / every - 1);
    CHECK(guest.session.hashes_compared() >= kEnd / every - 1);
}

TEST_CASE("Red: con tres jugadores el anfitrión reenvía y los tres acaban igual") {
    const GameData& data = game_data();
    const auto& cfg = data.engine.net.lockstep;
    const std::uint64_t hash = rts::game::data_hash(data.files);
    auto h = LockstepSession::host(kAnyPort, 3, cfg, hash, 200);
    REQUIRE(h.has_value());
    auto g1 = LockstepSession::join("127.0.0.1", h->port(), cfg, hash);
    REQUIRE(g1.has_value());
    // El segundo invitado llega cuando el primero ya ha entrado: jugador 1 y jugador 2.
    for (int i = 0; i < 1000; ++i) {
        h->poll(1);
        g1->poll(1);
    }
    auto g2 = LockstepSession::join("127.0.0.1", h->port(), cfg, hash);
    REQUIRE(g2.has_value());
    for (int i = 0; i < 10'000 && g2->state() == LockstepState::Lobby; ++i) {
        h->poll(1);
        g1->poll(1);
        g2->poll(1);
    }
    REQUIRE(g2->state() == LockstepState::Running);
    CHECK(g1->local_player() == 1);
    CHECK(g2->local_player() == 2);
    Side host(std::move(*h), data);
    Side a(std::move(*g1), data);
    Side b(std::move(*g2), data);
    play(data, {&host, &a, &b});
    for (const Side* s : {&host, &a, &b}) {
        REQUIRE_MESSAGE(s->session.state() == LockstepState::Finished, s->session.error());
        CHECK(s->world.state_hash() == host.world.state_hash());
    }
    // Los invitados solo se oyen a través del anfitrión.
    CHECK(a.remote_orders > 0);
    CHECK(b.remote_orders > 0);
}

TEST_CASE("Red: una orden que solo ve un mundo se detecta como desincronización en el siguiente hash") {
    const GameData& data = game_data();
    const auto& cfg = data.engine.net.lockstep;
    auto [h, g] = connect_pair(data, 400, cfg);
    Side host(std::move(h), data);
    Side guest(std::move(g), data);
    Command extra;
    extra.tick = 10;
    extra.player = 1;
    extra.type = CommandType::Move;
    extra.units = units_of(guest.world, 1);
    extra.target = {0, 0};
    play(data, {&host, &guest}, extra);
    CHECK(host.session.state() == LockstepState::Failed);
    CHECK(guest.session.state() == LockstepState::Failed);
    // Al menos uno lo ve como desincronización (el otro puede ver antes el corte).
    const auto every = static_cast<rts::sim::Tick>(cfg.turn_ticks * cfg.hash_every_turns);
    const rts::sim::Tick first_hash_after = (extra.tick / every + 1) * every;
    bool seen = false;
    for (const Side* s : {&host, &guest}) {
        if (const auto& d = s->session.desync()) {
            seen = true;
            CHECK(d->tick == first_hash_after);
            CHECK(d->local != d->remote);
            CHECK(s->session.error().find("desincronización") != std::string::npos);
        }
    }
    CHECK(seen);
}

TEST_CASE("Red: el anfitrión rechaza datos distintos y la partida completa") {
    const GameData& data = game_data();
    const auto& cfg = data.engine.net.lockstep;
    const std::uint64_t hash = rts::game::data_hash(data.files);
    auto host = LockstepSession::host(kAnyPort, 2, cfg, hash, 100);
    REQUIRE(host.has_value());
    auto wrong = LockstepSession::join("127.0.0.1", host->port(), cfg, hash + 1);
    REQUIRE(wrong.has_value());
    for (int i = 0; i < 10'000 && wrong->state() == LockstepState::Lobby; ++i) {
        host->poll(1);
        wrong->poll(1);
    }
    CHECK(wrong->state() == LockstepState::Failed);
    CHECK(wrong->error().find("datos") != std::string::npos);
    CHECK(host->state() == LockstepState::Lobby);  // sigue esperando a uno bueno

    auto good = LockstepSession::join("127.0.0.1", host->port(), cfg, hash);
    REQUIRE(good.has_value());
    for (int i = 0; i < 10'000 && good->state() == LockstepState::Lobby; ++i) {
        host->poll(1);
        good->poll(1);
    }
    CHECK(good->state() == LockstepState::Running);
    CHECK(host->state() == LockstepState::Running);
}

TEST_CASE("Red: sin noticias del otro jugador, la partida falla al cumplirse el plazo") {
    const GameData& data = game_data();
    auto cfg = data.engine.net.lockstep;
    cfg.stall_timeout_ms = 50;
    auto [h, g] = connect_pair(data, 1000, cfg);
    Side host(std::move(h), data);  // el invitado g se queda callado
    std::optional<Command> none;
    const auto start = std::chrono::steady_clock::now();
    while (host.session.state() == LockstepState::Running &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
        host.advance(data, none);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(host.session.state() == LockstepState::Failed);
    CHECK(host.session.error().find("jugadores 1") != std::string::npos);
    // Se paró justo donde faltaban las órdenes: los primeros turnos van vacíos.
    CHECK(host.world.tick() == static_cast<rts::sim::Tick>(cfg.turn_ticks * cfg.input_delay_turns));
    CHECK(g.state() == LockstepState::Running);
}

TEST_CASE("Red: en la sala, el anfitrión empieza cuando quiere y los ajustes llegan a todos") {
    const GameData& data = game_data();
    const auto& cfg = data.engine.net.lockstep;
    const std::uint64_t hash = rts::game::data_hash(data.files);
    auto host = LockstepSession::host(kAnyPort, 0, cfg, hash, 0);
    REQUIRE(host.has_value());
    auto guest = LockstepSession::join("127.0.0.1", host->port(), cfg, hash);
    REQUIRE(guest.has_value());
    for (int i = 0; i < 10'000 && guest->connected() < 2; ++i) {
        host->poll(1);
        guest->poll(1);
    }
    CHECK(host->connected() == 2);
    CHECK(guest->connected() == 2);
    // Sin start() no empieza, por mucho que se espere.
    for (int i = 0; i < 50; ++i) {
        host->poll(1);
        guest->poll(1);
    }
    CHECK(host->state() == LockstepState::Lobby);
    CHECK(guest->state() == LockstepState::Lobby);
    // Charla en la sala, en los dos sentidos.
    guest->send_chat("hola");
    host->send_chat("buenas");
    host->start("seed = 7\n", 2);
    for (int i = 0; i < 10'000 && guest->state() == LockstepState::Lobby; ++i) {
        host->poll(1);
        guest->poll(1);
    }
    REQUIRE(guest->state() == LockstepState::Running);
    for (int i = 0; i < 100; ++i) {
        host->poll(1);
        guest->poll(1);
    }
    CHECK(host->players() == 2);
    CHECK(guest->local_player() == 1);
    CHECK(guest->settings() == "seed = 7\n");
    CHECK(guest->takeover_profile() == 2);
    for (const LockstepSession* s : {&*host, &*guest}) {
        REQUIRE(s->chat().size() == 2);
        CHECK(s->chat()[0].text != s->chat()[1].text);
    }
    CHECK(host->chat()[0].text == "buenas");  // el propio, al momento
    CHECK(host->chat()[1].player == 1);
    CHECK(host->chat()[1].text == "hola");
}

TEST_CASE("Red: la charla se corta al máximo sin partir un carácter") {
    const GameData& data = game_data();
    auto cfg = data.engine.net.lockstep;
    cfg.chat_max_chars = 5;
    const std::uint64_t hash = rts::game::data_hash(data.files);
    auto host = LockstepSession::host(kAnyPort, 0, cfg, hash, 0);
    REQUIRE(host.has_value());
    host->send_chat("abcdñe");  // la ñ ocupa los bytes 4 y 5
    REQUIRE(host->chat().size() == 1);
    CHECK(host->chat()[0].text == "abcd");
}

TEST_CASE("Red: si un invitado se va, la IA toma su bando en el mismo tick en todas partes") {
    rts::game::MatchSettings ms;
    ms.rival = game_data().engine.ai_profile_names.front();
    ms.seats = {"humano", "humano", "humano"};
    const auto three = rts::game::with_match_settings(game_data(), ms);
    REQUIRE(three.has_value());
    const GameData& data = *three;
    const auto& cfg = data.engine.net.lockstep;
    const std::uint64_t hash = rts::game::data_hash(data.files);
    constexpr rts::sim::Tick kEnd = 300;
    auto h = LockstepSession::host(kAnyPort, 3, cfg, hash, kEnd);
    REQUIRE(h.has_value());
    auto g1 = LockstepSession::join("127.0.0.1", h->port(), cfg, hash);
    REQUIRE(g1.has_value());
    for (int i = 0; i < 1000; ++i) {
        h->poll(1);
        g1->poll(1);
    }
    auto g2 = LockstepSession::join("127.0.0.1", h->port(), cfg, hash);
    REQUIRE(g2.has_value());
    for (int i = 0; i < 10'000 && g2->state() == LockstepState::Lobby; ++i) {
        h->poll(1);
        g1->poll(1);
        g2->poll(1);
    }
    REQUIRE(g2->state() == LockstepState::Running);
    Side host(std::move(*h), data);
    Side a(std::move(*g1), data);
    auto b = std::make_unique<Side>(std::move(*g2), data);
    const rts::sim::PlayerId gone = b->session.local_player();
    std::optional<Command> none;
    // Juegan un rato los tres y el jugador 2 se va (se destruye su sesión: cierra).
    for (int guard = 0; guard < 100'000 && b->world.tick() < 100; ++guard) {
        host.advance(data, none);
        a.advance(data, none);
        b->advance(data, none);
    }
    const rts::sim::Tick left_at = b->world.tick();
    b.reset();
    play(data, {&host, &a});
    REQUIRE_MESSAGE(host.session.state() == LockstepState::Finished, host.session.error());
    REQUIRE_MESSAGE(a.session.state() == LockstepState::Finished, a.session.error());
    CHECK(host.world.state_hash() == a.world.state_hash());
    CHECK(host.world.tick() == kEnd);
    for (const Side* s : {&host, &a}) {
        CHECK(s->session.dropped_players() == std::vector<rts::sim::PlayerId>{gone});
        const auto& seats = s->world.ai().players();
        CHECK(std::ranges::any_of(seats, [&](const auto& p) { return p.player == gone; }));
        REQUIRE_FALSE(s->session.chat().empty());
        CHECK(s->session.chat().back().player == gone);
    }
    CHECK(left_at >= 100);
}

TEST_CASE("Red: los ajustes con puestos crean una partida de cuatro, con IA donde se pide") {
    const GameData& base = game_data();
    CHECK(base.engine.world.setup.starts.size() == 2);  // sin ajustes, la de siempre
    CHECK(base.engine.seat_starts.size() == 4);
    rts::game::MatchSettings s;
    s.seed = 5;
    s.rival = base.engine.ai_profile_names.front();
    s.seats = {"humano", "humano", "ia", "ia"};
    const auto data = rts::game::with_match_settings(base, s);
    REQUIRE_MESSAGE(data.has_value(), (data ? std::string() : data.error()));
    CHECK(data->engine.world.setup.starts.size() == 4);
    CHECK(data->engine.player_colors.size() == 4);
    REQUIRE(data->engine.world.ai_players.size() == 2);
    CHECK(data->engine.world.ai_players[0].player == 2);
    CHECK(data->engine.world.ai_players[1].player == 3);
    const World w(data->engine.world);
    rts::sim::Snapshot snap;
    w.write_snapshot(snap);
    CHECK(snap.players.size() == 4);
    for (rts::sim::PlayerId p = 0; p < 4; ++p) {
        CHECK_FALSE(units_of(w, p).empty());
    }
    s.seats = {"humano"};
    CHECK_FALSE(rts::game::with_match_settings(base, s).has_value());
    s.seats = {"humano", "nadie"};
    CHECK_FALSE(rts::game::with_match_settings(base, s).has_value());
}

TEST_CASE("Red: la orden de relevo pasa un jugador a la IA una sola vez") {
    const GameData& data = game_data();
    World w(data.engine.world);
    const auto before = w.ai().players().size();
    Command c;
    c.player = 0;
    c.type = CommandType::AiTakeover;
    c.kind = 0;
    w.issue(c);
    w.issue(c);
    Command bad = c;
    bad.player = 9;  // no existe
    w.issue(bad);
    w.step();
    CHECK(w.ai().players().size() == before + 1);
}

TEST_CASE("Red: unirse no espera; si el anfitrión aún no escucha se reintenta, y si no aparece se avisa") {
    const GameData& data = game_data();
    auto cfg = data.engine.net.lockstep;
    const std::uint64_t hash = rts::game::data_hash(data.files);
    // Un puerto libre: se abre y se cierra.
    std::uint16_t port = 0;
    {
        auto probe = rts::net::Listener::listen(0);
        REQUIRE(probe.has_value());
        port = probe->port();
    }
    const auto t0 = std::chrono::steady_clock::now();
    auto guest = LockstepSession::join("127.0.0.1", port, cfg, hash);
    REQUIRE(guest.has_value());
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1));  // no bloquea
    CHECK(guest->joining());
    // Varios intentos fallidos (nadie escucha) sin dar la partida por perdida.
    for (int i = 0; i < 20; ++i) {
        guest->poll(cfg.connect_retry_ms / 4 + 1);
    }
    CHECK(guest->state() == LockstepState::Lobby);
    auto host = LockstepSession::host(port, 2, cfg, hash, 100);
    REQUIRE(host.has_value());
    for (int i = 0; i < 10'000 && guest->state() == LockstepState::Lobby; ++i) {
        host->poll(1);
        guest->poll(1);
    }
    CHECK(guest->state() == LockstepState::Running);
    CHECK_FALSE(guest->joining());

    // Otro puerto en el que nadie escucha nunca: se agota el plazo.
    std::uint16_t empty_port = 0;
    {
        auto probe = rts::net::Listener::listen(0);
        REQUIRE(probe.has_value());
        empty_port = probe->port();
    }
    cfg.connect_timeout_ms = 200;
    auto lost = LockstepSession::join("127.0.0.1", empty_port, cfg, hash);
    REQUIRE(lost.has_value());
    for (int i = 0; i < 1000 && lost->state() == LockstepState::Lobby; ++i) {
        lost->poll(5);
    }
    CHECK(lost->state() == LockstepState::Failed);
}
