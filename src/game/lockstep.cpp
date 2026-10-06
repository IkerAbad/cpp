#include "game/lockstep.hpp"

#include <algorithm>
#include <format>
#include <utility>

#include "game/wire.hpp"

namespace rts::game {

namespace {

enum class Msg : std::uint8_t { Hello = 1, Welcome, Reject, Roster, Start, Turn, Done, Chat, Drop };

constexpr std::uint32_t kNoHashTick = ~std::uint32_t{0};

using Clock = std::chrono::steady_clock;

std::int64_t ms_since(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

std::vector<std::uint8_t> reject_frame(std::string_view reason) {
    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(Msg::Reject));
    w.str(reason);
    return std::move(w.out());
}

}  // namespace

std::vector<sim::Command> probe_orders(const sim::Snapshot& snap, sim::PlayerId player, const ProbeConfig& config,
                                      sim::TileCoord map_size, sim::Xoshiro256pp& rng) {
    std::vector<const sim::SnapshotEntity*> own;
    for (const sim::SnapshotEntity& e : snap.entities) {
        if (e.owner == player) {
            own.push_back(&e);
        }
    }
    std::vector<sim::Command> orders;
    for (std::int32_t i = 0; i < config.units && !own.empty(); ++i) {
        const std::size_t k = rng.next_below(static_cast<std::uint32_t>(own.size()));
        const sim::SnapshotEntity& e = *own[k];
        own.erase(own.begin() + static_cast<std::ptrdiff_t>(k));
        const sim::TileCoord at = sim::tile_of(e.pos);
        sim::Command c;
        c.player = player;
        c.type = sim::CommandType::Move;
        c.units = {e.id};
        c.target = {std::clamp(at.x + rng.next_in_range(-config.radius_tiles, config.radius_tiles), 0, map_size.x - 1),
                    std::clamp(at.y + rng.next_in_range(-config.radius_tiles, config.radius_tiles), 0, map_size.y - 1)};
        orders.push_back(std::move(c));
    }
    return orders;
}

LockstepSession::LockstepSession(const LockstepConfig& config, std::uint64_t data_hash)
    : config_(config), data_hash_(data_hash), created_(Clock::now()) {}

std::expected<LockstepSession, std::string> LockstepSession::host(std::uint16_t port,
                                                                  std::uint8_t auto_start_players,
                                                                  const LockstepConfig& config,
                                                                  std::uint64_t data_hash, sim::Tick end_tick) {
    if (auto_start_players != 0 && (auto_start_players < 2 || auto_start_players > config.max_players)) {
        return std::unexpected(std::format("una partida en red es de 2 a {} jugadores, no de {}", config.max_players,
                                           auto_start_players));
    }
    auto listener = net::Listener::listen(port);
    if (!listener) {
        return std::unexpected(listener.error());
    }
    LockstepSession s(config, data_hash);
    s.is_host_ = true;
    s.listener_ = std::move(*listener);
    s.local_ = 0;
    s.auto_start_ = auto_start_players;
    s.connected_ = 1;
    s.turn_ticks_ = config.turn_ticks;
    s.delay_turns_ = config.input_delay_turns;
    s.hash_every_ = config.hash_every_turns;
    s.end_tick_ = end_tick;
    s.next_send_turn_ = static_cast<std::uint32_t>(s.delay_turns_);
    return s;
}

std::expected<LockstepSession, std::string> LockstepSession::join(std::string_view address, std::uint16_t port,
                                                                  const LockstepConfig& config,
                                                                  std::uint64_t data_hash) {
    LockstepSession s(config, data_hash);
    s.join_address_ = address;
    s.join_port_ = port;
    if (auto error = s.connect_to_host()) {
        return std::unexpected(*error);
    }
    return s;
}

std::optional<std::string> LockstepSession::connect_to_host() {
    auto conn = net::Connection::start_connect(join_address_, join_port_,
                                               static_cast<std::uint32_t>(config_.max_frame_bytes));
    if (!conn) {
        return conn.error();
    }
    host_ = std::move(*conn);
    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(Msg::Hello));
    w.u32(kLockstepProtocolVersion);
    w.u64(data_hash_);
    host_->send(w.out());  // sale en cuanto se conecte
    retry_at_.reset();
    return std::nullopt;
}

void LockstepSession::fail(std::string reason) {
    if (state_ != LockstepState::Failed) {
        state_ = LockstepState::Failed;
        error_ = std::move(reason);
    }
}

bool LockstepSession::flushed() const {
    if (host_ && host_->sending()) {
        return false;
    }
    return std::ranges::none_of(peers_, [](const Peer& p) { return p.conn.sending(); });
}

std::vector<sim::PlayerId> LockstepSession::dropped_players() const {
    std::vector<sim::PlayerId> out;
    for (std::size_t p = 0; p < dropped_.size(); ++p) {
        if (dropped_[p]) {
            out.push_back(static_cast<sim::PlayerId>(p));
        }
    }
    return out;
}

std::size_t LockstepSession::active_remotes() const {
    std::size_t n = 0;
    for (std::size_t p = 0; p < players_; ++p) {
        if (p != local_ && !(p < dropped_.size() && dropped_[p])) {
            ++n;
        }
    }
    return n;
}

void LockstepSession::poll(std::int32_t wait_ms) {
    std::vector<net::Socket::Handle> reads;
    std::vector<net::Socket::Handle> writes;
    if (listener_ && state_ == LockstepState::Lobby) {
        reads.push_back(listener_->handle());
    }
    for (const Peer& p : peers_) {
        reads.push_back(p.conn.handle());
        if (p.conn.sending()) {
            writes.push_back(p.conn.handle());
        }
    }
    if (host_) {
        reads.push_back(host_->handle());
        if (host_->sending()) {
            writes.push_back(host_->handle());
        }
    }
    net::wait(reads, writes, wait_ms);

    if (is_host_) {
        if (state_ == LockstepState::Lobby) {
            accept_peers();
        }
        std::vector<sim::PlayerId> lost;
        for (Peer& p : peers_) {
            const bool was_open = p.conn.open();
            const bool alive = p.conn.pump();
            // Lo que llegó antes del cierre se procesa igual (p. ej. su mensaje de fin).
            while (auto frame = p.conn.receive()) {
                if (state_ == LockstepState::Failed) {
                    break;
                }
                handle_host_frame(p, std::move(*frame));
            }
            if (was_open && !alive && p.welcomed && state_ == LockstepState::Running && !finals_[p.player]) {
                lost.push_back(p.player);
            }
        }
        for (const sim::PlayerId player : lost) {
            // Primer turno del que no llegó nada suyo: nadie lo ha podido ejecutar.
            const auto it = std::ranges::find(peers_, player, &Peer::player);
            const std::uint32_t from = it->last_turn ? *it->last_turn + 1 : static_cast<std::uint32_t>(delay_turns_);
            ByteWriter w;
            w.u8(static_cast<std::uint8_t>(Msg::Drop));
            w.u8(player);
            w.u32(from);
            relay(nullptr, w.out());
            drop(player, from);
        }
        if (state_ == LockstepState::Lobby) {
            // En la sala, quien se va deja su sitio.
            const std::size_t before = peers_.size();
            std::erase_if(peers_, [](const Peer& p) { return !p.conn.open(); });
            if (peers_.size() != before) {
                send_roster();
            }
        }
    } else if (host_) {
        const bool alive = host_->pump();
        while (auto frame = host_->receive()) {
            if (state_ == LockstepState::Failed) {
                break;
            }
            handle_client_frame(std::move(*frame));
        }
        // Sin bienvenida aún, el anfitrión puede no estar escuchando: se reintenta
        // cada connect_retry_ms durante connect_timeout_ms (sin bloquear).
        const bool joining = state_ == LockstepState::Lobby && turn_ticks_ == 0;
        if (!alive && joining && ms_since(created_) < config_.connect_timeout_ms) {
            if (!retry_at_) {
                retry_at_ = Clock::now() + std::chrono::milliseconds(config_.connect_retry_ms);
            } else if (Clock::now() >= *retry_at_) {
                if (auto error = connect_to_host()) {
                    fail(*error);
                }
            }
        } else if (joining && ms_since(created_) >= config_.connect_timeout_ms) {
            fail(std::format("no se pudo conectar con {}:{} en {} ms: {}", join_address_, join_port_,
                             config_.connect_timeout_ms, host_->error().empty() ? "sin respuesta" : host_->error()));
        } else if (!alive && (state_ == LockstepState::Running || state_ == LockstepState::Lobby)) {
            fail("se perdió la conexión con el anfitrión: " + host_->error());
        }
    }
    if (state_ == LockstepState::Lobby && auto_start_ != 0 && ms_since(created_) > config_.lobby_timeout_ms) {
        fail(std::format("los jugadores no llegaron a reunirse en {} ms", config_.lobby_timeout_ms));
    }
}

void LockstepSession::accept_peers() {
    while (auto sock = listener_->accept()) {
        peers_.push_back(
            {net::Connection(std::move(*sock), static_cast<std::uint32_t>(config_.max_frame_bytes)), 0, false, {}});
    }
}

void LockstepSession::send_roster() {
    connected_ = static_cast<std::uint8_t>(1 + std::ranges::count_if(peers_, [](const Peer& p) { return p.welcomed; }));
    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(Msg::Roster));
    w.u8(connected_);
    relay(nullptr, w.out());
}

void LockstepSession::handle_host_frame(Peer& from, std::vector<std::uint8_t> frame) {
    if (frame.empty()) {
        from.conn.close("mensaje vacío");
        return;
    }
    const auto type = static_cast<Msg>(frame[0]);
    if (!from.welcomed) {
        ByteReader r(std::span<const std::uint8_t>(frame).subspan(1));
        const std::uint32_t version = r.u32();
        const std::uint64_t hash = r.u64();
        std::string reason;
        const auto welcomed = std::ranges::count_if(peers_, [](const Peer& p) { return p.welcomed; });
        const std::int64_t capacity = auto_start_ != 0 ? auto_start_ : config_.max_players;
        if (type != Msg::Hello || !r.ok()) {
            reason = "se esperaba el saludo";
        } else if (version != kLockstepProtocolVersion) {
            reason = std::format("protocolo {} (el anfitrión usa el {})", version, kLockstepProtocolVersion);
        } else if (hash != data_hash_) {
            reason = "los ficheros de datos no son los mismos que los del anfitrión";
        } else if (state_ != LockstepState::Lobby || welcomed + 1 >= capacity) {
            reason = "la partida está completa o ya ha empezado";
        }
        if (!reason.empty()) {
            from.conn.send(reject_frame(reason));
            from.conn.pump();
            from.conn.close("rechazado: " + reason);
            return;
        }
        from.welcomed = true;
        from.player = static_cast<sim::PlayerId>(welcomed + 1);
        ByteWriter w;
        w.u8(static_cast<std::uint8_t>(Msg::Welcome));
        w.u8(from.player);
        w.u32(static_cast<std::uint32_t>(turn_ticks_));
        w.u32(static_cast<std::uint32_t>(delay_turns_));
        w.u32(static_cast<std::uint32_t>(hash_every_));
        w.u32(end_tick_);
        from.conn.send(w.out());
        send_roster();
        if (auto_start_ != 0 && connected_ == auto_start_) {
            start({}, 0);
        }
        return;
    }
    bool ok = false;
    if (type == Msg::Turn && frame.size() > 1 + sizeof(std::uint32_t)) {
        // El jugador va tras el número de turno: solo puede hablar por sí mismo.
        ok = frame[1 + sizeof(std::uint32_t)] == from.player && handle_turn(frame);
        if (ok) {
            ByteReader r(std::span<const std::uint8_t>(frame).subspan(1));
            from.last_turn = r.u32();
        }
    } else if (type == Msg::Done && frame.size() > 1) {
        ok = frame[1] == from.player && handle_done(frame);
    } else if (type == Msg::Chat && frame.size() > 1) {
        // En la sala los puestos se renumeran al empezar: el remitente lo pone el anfitrión.
        frame[1] = from.player;
        ok = handle_chat(frame);
    }
    if (!ok) {
        fail(std::format("mensaje no válido del jugador {}", from.player));
        return;
    }
    relay(&from, frame);
}

void LockstepSession::handle_client_frame(std::vector<std::uint8_t> frame) {
    if (frame.empty()) {
        fail("mensaje vacío del anfitrión");
        return;
    }
    const auto body = std::span<const std::uint8_t>(frame).subspan(1);
    bool ok = true;
    switch (static_cast<Msg>(frame[0])) {
        case Msg::Welcome:
            apply_welcome(body);
            return;
        case Msg::Reject: {
            ByteReader r(body);
            fail("el anfitrión no admite la conexión: " + r.str());
            return;
        }
        case Msg::Roster: {
            ByteReader r(body);
            connected_ = r.u8();
            ok = r.ok();
            break;
        }
        case Msg::Start:
            apply_start(body);
            return;
        case Msg::Turn:
            ok = handle_turn(frame);
            break;
        case Msg::Done:
            ok = handle_done(frame);
            break;
        case Msg::Chat:
            ok = handle_chat(frame);
            break;
        case Msg::Drop:
            ok = handle_drop(frame);
            break;
        case Msg::Hello:
            ok = false;
            break;
    }
    if (!ok) {
        fail(std::format("mensaje no válido del anfitrión (tipo {})", frame[0]));
    }
}

void LockstepSession::apply_welcome(std::span<const std::uint8_t> body) {
    ByteReader r(body);
    local_ = r.u8();
    turn_ticks_ = static_cast<std::int32_t>(r.u32());
    delay_turns_ = static_cast<std::int32_t>(r.u32());
    hash_every_ = static_cast<std::int32_t>(r.u32());
    end_tick_ = r.u32();
    if (!r.ok() || local_ == 0 || turn_ticks_ <= 0 || delay_turns_ <= 0 || hash_every_ <= 0) {
        fail("bienvenida no válida");
        return;
    }
    next_send_turn_ = static_cast<std::uint32_t>(delay_turns_);
}

void LockstepSession::apply_start(std::span<const std::uint8_t> body) {
    ByteReader r(body);
    const sim::PlayerId me = r.u8();
    const std::uint8_t players = r.u8();
    const std::uint8_t profile = r.u8();
    std::string settings = r.str();
    if (!r.ok() || turn_ticks_ == 0 || players < 2 || me == 0 || me >= players || state_ != LockstepState::Lobby) {
        fail("el anfitrión empezó sin una bienvenida válida");
        return;
    }
    local_ = me;
    players_ = players;
    connected_ = players;
    takeover_profile_ = profile;
    settings_ = std::move(settings);
    finals_.assign(players_, std::nullopt);
    dropped_.assign(players_, std::nullopt);
    state_ = LockstepState::Running;
}

void LockstepSession::start(std::string settings, std::uint8_t takeover_profile) {
    if (!is_host_ || state_ != LockstepState::Lobby) {
        return;
    }
    // Los que siguen conectados, numerados de nuevo sin huecos.
    std::erase_if(peers_, [](const Peer& p) { return !p.conn.open() || !p.welcomed; });
    players_ = static_cast<std::uint8_t>(peers_.size() + 1);
    connected_ = players_;
    settings_ = std::move(settings);
    takeover_profile_ = takeover_profile;
    for (std::size_t i = 0; i < peers_.size(); ++i) {
        Peer& p = peers_[i];
        p.player = static_cast<sim::PlayerId>(i + 1);
        ByteWriter w;
        w.u8(static_cast<std::uint8_t>(Msg::Start));
        w.u8(p.player);
        w.u8(players_);
        w.u8(takeover_profile_);
        w.str(settings_);
        p.conn.send(w.out());
    }
    finals_.assign(players_, std::nullopt);
    dropped_.assign(players_, std::nullopt);
    state_ = LockstepState::Running;
}

void LockstepSession::relay(const Peer* except, std::span<const std::uint8_t> frame) {
    for (Peer& p : peers_) {
        if (&p != except && p.welcomed) {
            p.conn.send(frame);
        }
    }
}

void LockstepSession::send_to_all(std::span<const std::uint8_t> frame) {
    if (is_host_) {
        relay(nullptr, frame);
    } else if (host_) {
        host_->send(frame);
    }
}

LockstepSession::TurnInputs& LockstepSession::inputs(std::uint32_t turn) {
    TurnInputs& in = turns_[turn];
    in.by_player.resize(players_);
    return in;
}

bool LockstepSession::has_input(const TurnInputs* in, sim::PlayerId player, std::uint32_t turn) const {
    if (dropped_[player] && turn >= *dropped_[player]) {
        return true;  // abandonó: sus turnos desde entonces van vacíos
    }
    return in != nullptr && in->by_player[player].has_value();
}

bool LockstepSession::handle_turn(std::span<const std::uint8_t> frame) {
    if (state_ != LockstepState::Running) {
        return false;
    }
    ByteReader r(frame.subspan(1));
    const std::uint32_t turn = r.u32();
    const sim::PlayerId player = r.u8();
    const std::uint32_t hash_tick = r.u32();
    const std::uint64_t hash = r.u64();
    std::vector<sim::Command> commands(r.count(1));
    for (sim::Command& c : commands) {
        // El relevo de la IA solo lo decide la sesión.
        if (!read_command(r, c) || c.type == sim::CommandType::AiTakeover) {
            return false;
        }
        // Nadie da órdenes por otro ni para otro momento.
        c.player = player;
        c.tick = turn * static_cast<std::uint32_t>(turn_ticks_);
    }
    // Un jugador va como mucho input_delay_turns turnos por delante: más es un error.
    const auto delay = static_cast<std::uint32_t>(delay_turns_);
    if (!r.ok() || r.remaining() != 0 || player >= players_ || player == local_ || turn < delay ||
        turn < next_run_turn_ || turn > next_run_turn_ + 2 * delay + 1 ||
        (dropped_[player] && turn >= *dropped_[player])) {
        return false;
    }
    auto& slot = inputs(turn).by_player[player];
    if (slot) {
        return false;
    }
    slot = std::move(commands);
    if (hash_tick != kNoHashTick) {
        record_remote_hash({hash_tick, player, hash});
    }
    return true;
}

bool LockstepSession::handle_done(std::span<const std::uint8_t> frame) {
    ByteReader r(frame.subspan(1));
    const sim::PlayerId player = r.u8();
    const sim::Tick tick = r.u32();
    const std::uint64_t hash = r.u64();
    if (!r.ok() || r.remaining() != 0 || player >= players_ || player == local_ || finals_[player]) {
        return false;
    }
    finals_[player] = std::pair{tick, hash};
    check_done();
    return true;
}

bool LockstepSession::handle_chat(std::span<const std::uint8_t> frame) {
    ByteReader r(frame.subspan(1));
    const sim::PlayerId player = r.u8();
    std::string text = r.str();
    if (!r.ok() || r.remaining() != 0 || text.size() > static_cast<std::size_t>(config_.chat_max_chars)) {
        return false;
    }
    chat_.push_back({player, std::move(text)});
    if (chat_.size() > static_cast<std::size_t>(config_.chat_history)) {
        chat_.erase(chat_.begin());
    }
    return true;
}

void LockstepSession::send_chat(std::string_view text) {
    if (text.empty() || state_ == LockstepState::Failed) {
        return;
    }
    // Se corta sin partir un carácter UTF-8 (los bytes de continuación son 10xxxxxx).
    constexpr unsigned kContinuationMask = 0xC0U;
    constexpr unsigned kContinuation = 0x80U;
    std::size_t n = std::min(text.size(), static_cast<std::size_t>(config_.chat_max_chars));
    while (n > 0 && n < text.size() &&
           (static_cast<unsigned char>(text[n]) & kContinuationMask) == kContinuation) {
        --n;
    }
    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(Msg::Chat));
    w.u8(local_);
    w.str(text.substr(0, n));
    handle_chat(w.out());  // también en el registro propio
    send_to_all(w.out());
}

bool LockstepSession::handle_drop(std::span<const std::uint8_t> frame) {
    ByteReader r(frame.subspan(1));
    const sim::PlayerId player = r.u8();
    const std::uint32_t from = r.u32();
    if (!r.ok() || r.remaining() != 0 || state_ != LockstepState::Running || player == 0 || player >= players_ ||
        player == local_ || dropped_[player] || from < next_run_turn_) {
        return false;
    }
    drop(player, from);
    return true;
}

void LockstepSession::drop(sim::PlayerId player, std::uint32_t from_turn) {
    dropped_[player] = from_turn;
    for (auto& [turn, in] : turns_) {
        if (turn >= from_turn) {
            in.by_player[player].reset();
        }
    }
    finals_[player].reset();
    chat_.push_back({player, "(ha abandonado; la IA toma su bando)"});
    // Sus hashes ya no llegarán: lo que solo esperaba los suyos queda confirmado.
    check_hashes();
    check_done();
}

void LockstepSession::submit(sim::Command command) {
    if (command.type == sim::CommandType::AiTakeover) {
        return;
    }
    command.player = local_;
    local_commands_.push_back(std::move(command));
}

bool LockstepSession::begin_tick(const sim::World& world, const Issue& issue) {
    if (state_ != LockstepState::Running) {
        return false;
    }
    const sim::Tick tick = world.tick();
    const auto tt = static_cast<std::uint32_t>(turn_ticks_);
    if (tick % tt != 0) {
        return true;
    }
    const std::uint32_t turn = tick / tt;
    const auto delay = static_cast<std::uint32_t>(delay_turns_);
    if (next_send_turn_ == turn + delay) {
        std::uint32_t hash_tick = kNoHashTick;
        std::uint64_t hash = 0;
        if (turn % static_cast<std::uint32_t>(hash_every_) == 0) {
            hash_tick = tick;
            hash = world.state_hash();
        }
        const std::uint32_t target = turn + delay;
        for (sim::Command& c : local_commands_) {
            c.tick = target * tt;
        }
        ByteWriter w;
        w.u8(static_cast<std::uint8_t>(Msg::Turn));
        w.u32(target);
        w.u8(local_);
        w.u32(hash_tick);
        w.u64(hash);
        w.count(local_commands_.size());
        for (const sim::Command& c : local_commands_) {
            write_command(w, c);
        }
        send_to_all(w.out());
        inputs(target).by_player[local_] = std::move(local_commands_);
        local_commands_.clear();
        ++next_send_turn_;
        if (hash_tick != kNoHashTick) {
            record_own_hash(hash_tick, hash);
            if (state_ == LockstepState::Failed) {
                return false;
            }
        }
    }
    if (turn < next_run_turn_) {
        return true;
    }
    const auto it = turns_.find(turn);
    TurnInputs* in = it != turns_.end() ? &it->second : nullptr;
    if (turn >= delay) {
        for (std::uint8_t p = 0; p < players_; ++p) {
            if (!has_input(in, p, turn)) {
                stall_check();
                return false;
            }
        }
    }
    for (std::uint8_t p = 0; p < players_; ++p) {
        if (dropped_[p] && *dropped_[p] == turn) {
            sim::Command c;
            c.tick = tick;
            c.player = p;
            c.type = sim::CommandType::AiTakeover;
            c.kind = takeover_profile_;
            issue(std::move(c));
        } else if (in != nullptr && in->by_player[p]) {
            for (sim::Command& c : *in->by_player[p]) {
                issue(std::move(c));
            }
        }
    }
    if (in != nullptr) {
        turns_.erase(it);
    }
    next_run_turn_ = turn + 1;
    waiting_since_.reset();
    return true;
}

void LockstepSession::stall_check() {
    if (!waiting_since_) {
        waiting_since_ = Clock::now();
        return;
    }
    if (ms_since(*waiting_since_) <= config_.stall_timeout_ms) {
        return;
    }
    std::string missing;
    const auto it = turns_.find(next_run_turn_);
    const TurnInputs* in = it != turns_.end() ? &it->second : nullptr;
    for (std::uint8_t p = 0; p < players_; ++p) {
        if (!has_input(in, p, next_run_turn_)) {
            missing += std::format("{}{}", missing.empty() ? "" : ", ", p);
        }
    }
    fail(std::format("{} ms sin las órdenes del turno {} de los jugadores {}", config_.stall_timeout_ms,
                     next_run_turn_, missing));
}

void LockstepSession::record_own_hash(sim::Tick tick, std::uint64_t hash) {
    own_hashes_[tick] = {hash, 0};
    check_hashes();
}

void LockstepSession::record_remote_hash(RemoteHash remote) {
    remote_hashes_.push_back(remote);
    check_hashes();
}

void LockstepSession::check_hashes() {
    std::erase_if(remote_hashes_, [this](const RemoteHash& r) {
        const auto it = own_hashes_.find(r.tick);
        if (it == own_hashes_.end() || desync_) {
            return false;
        }
        if (it->second.hash != r.hash) {
            desync_ = Desync{r.tick, r.player, it->second.hash, r.hash};
            return true;
        }
        ++hashes_compared_;
        ++it->second.confirmations;
        return true;
    });
    if (desync_) {
        fail(std::format("desincronización en el tick {}: el hash del jugador {} es {:016x} y el propio {:016x}",
                         desync_->tick, desync_->player, desync_->remote, desync_->local));
        return;
    }
    const std::size_t needed = active_remotes();
    std::erase_if(own_hashes_, [needed](const auto& kv) { return kv.second.confirmations >= needed; });
}

void LockstepSession::finish(const sim::World& world) {
    if (state_ != LockstepState::Running || own_final_) {
        return;
    }
    own_final_ = std::pair{world.tick(), world.state_hash()};
    finals_[local_] = own_final_;
    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(Msg::Done));
    w.u8(local_);
    w.u32(own_final_->first);
    w.u64(own_final_->second);
    send_to_all(w.out());
    check_done();
}

void LockstepSession::check_done() {
    if (!own_final_ || state_ != LockstepState::Running) {
        return;
    }
    for (std::uint8_t p = 0; p < players_; ++p) {
        if (!finals_[p] && !dropped_[p]) {
            return;
        }
    }
    for (std::uint8_t p = 0; p < players_; ++p) {
        if (!finals_[p]) {
            continue;
        }
        const auto& [tick, hash] = *finals_[p];
        if (tick != own_final_->first) {
            fail(std::format("el jugador {} terminó en el tick {} y este en el {}", p, tick, own_final_->first));
            return;
        }
        if (hash != own_final_->second) {
            desync_ = Desync{tick, p, own_final_->second, hash};
            fail(std::format("desincronización al final (tick {}): el hash del jugador {} es {:016x} y el propio "
                             "{:016x}",
                             tick, p, hash, own_final_->second));
            return;
        }
    }
    state_ = LockstepState::Finished;
}

}  // namespace rts::game
