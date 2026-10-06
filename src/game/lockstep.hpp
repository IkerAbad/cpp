#pragma once

// Partida en red por lockstep (E1). Ninguna máquina manda el estado: todas simulan lo
// mismo con las mismas órdenes, y la simulación es determinista.
//
// - El tiempo se parte en turnos de turn_ticks ticks. Las órdenes que da un jugador
//   mientras su mundo está en el turno t se ejecutan al empezar el turno
//   t + input_delay_turns, el mismo tick en todas las máquinas.
// - Al empezar el turno t, cada jugador envía su mensaje del turno t + retraso (sus
//   órdenes, quizá ninguna). Un turno solo empieza cuando han llegado los de todos; los
//   primeros input_delay_turns turnos no llevan órdenes de nadie.
// - Estrella: los invitados hablan solo con el anfitrión, que reenvía a todos lo que
//   recibe. TCP conserva el orden de cada conexión.
// - Cada hash_every_turns turnos el mensaje lleva el hash de estado del tick en que se
//   envía; al compararlo con el propio se detecta la desincronización y su tick.
// - Al acabar, cada uno envía su tick y hash finales; se comparan igual.
//
// Mensajes (tramas de net::Connection, primer byte = tipo):
//   Hola       invitado -> anfitrión  u32 versión del protocolo, u64 hash de los datos
//   Bienvenida anfitrión -> invitado  u8 puesto en la sala, u32 turn_ticks,
//                                     u32 input_delay_turns, u32 hash_every_turns, u32 tick final
//   Rechazo    anfitrión -> invitado  str motivo
//   Sala       anfitrión -> todos     u8 jugadores conectados (anfitrión incluido)
//   Empezar    anfitrión -> cada uno  u8 su jugador, u8 jugadores, u8 perfil de relevo,
//                                     str ajustes de la partida (TOML de config/partida.toml)
//   Turno      todos                  u32 turno, u8 jugador, u32 tick del hash (o ~0), u64 hash,
//                                     u32 n + n órdenes (game/wire)
//   Fin        todos                  u8 jugador, u32 tick, u64 hash
//   Charla     todos                  u8 jugador, str texto
//   Abandono   anfitrión -> todos     u8 jugador, u32 primer turno sin él
//
// Abandono (E2): si un invitado se desconecta en partida, el anfitrión fija el primer
// turno del que no recibió nada suyo (nadie puede haberlo ejecutado aún, porque le
// faltaba). Desde ese turno sus órdenes son vacías y, al empezarlo, todos emiten la
// misma orden AiTakeover: la IA toma su bando. Si cae el anfitrión, la partida acaba.

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "net/socket.hpp"
#include "sim/rng.hpp"
#include "sim/tick.hpp"
#include "sim/units.hpp"
#include "sim/world.hpp"

namespace rts::game {

inline constexpr std::uint32_t kLockstepProtocolVersion = 2;

// data/config/engine.toml, sección [net].
struct LockstepConfig {
    std::int32_t turn_ticks = 0;
    std::int32_t input_delay_turns = 0;
    std::int32_t hash_every_turns = 0;
    std::int32_t stall_timeout_ms = 0;    // sin noticias de otro jugador durante esto: error
    std::int32_t lobby_timeout_ms = 0;    // espera a que se unan todos
    std::int32_t connect_timeout_ms = 0;  // espera a que el anfitrión acepte
    std::int32_t connect_retry_ms = 0;
    std::int32_t max_frame_bytes = 0;
    std::int32_t max_players = 0;
    std::int32_t chat_max_chars = 0;  // un mensaje más largo se corta
    std::int32_t chat_history = 0;    // líneas que se guardan
};

struct ChatLine {
    sim::PlayerId player = 0;
    std::string text;
};

// Órdenes de prueba para las partidas en red sin ventana (CI): cada every_ticks, hasta
// units unidades propias al azar se mueven a una casilla a como mucho radius_tiles. Solo
// su jugador las conoce; los demás las reciben por la red, así que si los hashes
// coinciden es que el intercambio funciona. data/config/engine.toml, [net.probe].
struct ProbeConfig {
    std::int32_t every_ticks = 0;
    std::int32_t units = 0;
    std::int32_t radius_tiles = 0;
};

[[nodiscard]] std::vector<sim::Command> probe_orders(const sim::Snapshot& snap, sim::PlayerId player,
                                                     const ProbeConfig& config, sim::TileCoord map_size,
                                                     sim::Xoshiro256pp& rng);

enum class LockstepState : std::uint8_t {
    Lobby,     // esperando a los demás
    Running,   // en partida
    Finished,  // todos han terminado y sus hashes finales coinciden
    Failed,    // conexión perdida, rechazo, plazo agotado o desincronización
};

struct Desync {
    sim::Tick tick = 0;
    sim::PlayerId player = 0;  // el jugador cuyo hash no coincide con el propio
    std::uint64_t local = 0;
    std::uint64_t remote = 0;
};

class LockstepSession {
public:
    using Issue = std::function<void(sim::Command)>;

    // El anfitrión es el jugador 0; los invitados reciben 1, 2... por orden de llegada.
    // Con auto_start_players > 0, la partida empieza sola al reunirse esos jugadores
    // (sin ajustes); con 0, es una sala y empieza cuando el anfitrión llama a start().
    // end_tick 0: sin final fijado.
    static std::expected<LockstepSession, std::string> host(std::uint16_t port, std::uint8_t auto_start_players,
                                                            const LockstepConfig& config, std::uint64_t data_hash,
                                                            sim::Tick end_tick);
    static std::expected<LockstepSession, std::string> join(std::string_view address, std::uint16_t port,
                                                            const LockstepConfig& config, std::uint64_t data_hash);

    // Red: acepta, envía, recibe y procesa. Espera como mucho wait_ms a que llegue algo.
    void poll(std::int32_t wait_ms);

    // Anfitrión en la sala: empieza con los que haya. settings viaja a todos tal cual;
    // takeover_profile es el perfil de IA que releva a quien abandone.
    void start(std::string settings, std::uint8_t takeover_profile);

    // Una orden del jugador local: sale en su próximo mensaje de turno.
    void submit(sim::Command command);
    void send_chat(std::string_view text);

    // Antes de cada world.step(). Al empezar un turno envía el mensaje propio y, si ya
    // están las órdenes de todos para él, las emite por issue (por jugador, en orden) y
    // devuelve true. Falso: hay que esperar (poll) y volver a intentarlo.
    bool begin_tick(const sim::World& world, const Issue& issue);

    // Fin de la partida: envía el tick y el hash finales. Después, poll() hasta que
    // state() sea Finished o Failed.
    void finish(const sim::World& world);

    [[nodiscard]] LockstepState state() const noexcept { return state_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] const std::optional<Desync>& desync() const noexcept { return desync_; }
    [[nodiscard]] sim::PlayerId local_player() const noexcept { return local_; }
    [[nodiscard]] std::uint8_t players() const noexcept { return players_; }
    [[nodiscard]] std::uint16_t port() const noexcept { return listener_ ? listener_->port() : 0; }
    [[nodiscard]] sim::Tick end_tick() const noexcept { return end_tick_; }
    [[nodiscard]] std::int32_t turn_ticks() const noexcept { return turn_ticks_; }
    [[nodiscard]] std::size_t hashes_compared() const noexcept { return hashes_compared_; }
    [[nodiscard]] bool is_host() const noexcept { return is_host_; }
    // En la sala: jugadores conectados, anfitrión incluido.
    [[nodiscard]] std::uint8_t connected() const noexcept { return connected_; }
    [[nodiscard]] const std::string& settings() const noexcept { return settings_; }
    [[nodiscard]] std::uint8_t takeover_profile() const noexcept { return takeover_profile_; }
    [[nodiscard]] const std::vector<ChatLine>& chat() const noexcept { return chat_; }
    // Jugadores que abandonaron (y relevó la IA), por orden.
    [[nodiscard]] std::vector<sim::PlayerId> dropped_players() const;
    // Ya no queda nada por enviar (para salir sin cortar los últimos mensajes).
    [[nodiscard]] bool flushed() const;

private:
    struct Peer {
        net::Connection conn;
        sim::PlayerId player = 0;
        bool welcomed = false;
        std::optional<std::uint32_t> last_turn;  // último turno recibido de él
    };
    struct TurnInputs {
        std::vector<std::optional<std::vector<sim::Command>>> by_player;
    };
    struct OwnHash {
        std::uint64_t hash = 0;
        std::size_t confirmations = 0;
    };
    struct RemoteHash {
        sim::Tick tick = 0;
        sim::PlayerId player = 0;
        std::uint64_t hash = 0;
    };

    LockstepSession(const LockstepConfig& config, std::uint64_t data_hash);

    void fail(std::string reason);
    void accept_peers();
    void handle_host_frame(Peer& from, std::vector<std::uint8_t> frame);
    void handle_client_frame(std::vector<std::uint8_t> frame);
    bool handle_turn(std::span<const std::uint8_t> frame);
    bool handle_done(std::span<const std::uint8_t> frame);
    void relay(const Peer* except, std::span<const std::uint8_t> frame);
    void send_to_all(std::span<const std::uint8_t> frame);
    void apply_welcome(std::span<const std::uint8_t> frame);
    void apply_start(std::span<const std::uint8_t> frame);
    bool handle_chat(std::span<const std::uint8_t> frame);
    bool handle_drop(std::span<const std::uint8_t> frame);
    void drop(sim::PlayerId player, std::uint32_t from_turn);
    void send_roster();
    [[nodiscard]] bool has_input(const TurnInputs* in, sim::PlayerId player, std::uint32_t turn) const;
    [[nodiscard]] std::size_t active_remotes() const;
    void record_own_hash(sim::Tick tick, std::uint64_t hash);
    void record_remote_hash(RemoteHash remote);
    void check_hashes();
    void check_done();
    TurnInputs& inputs(std::uint32_t turn);
    void stall_check();

    LockstepConfig config_;
    std::uint64_t data_hash_ = 0;
    bool is_host_ = false;
    LockstepState state_ = LockstepState::Lobby;
    std::string error_;
    std::optional<Desync> desync_;

    std::optional<net::Listener> listener_;
    std::vector<Peer> peers_;               // anfitrión: un invitado por conexión
    std::optional<net::Connection> host_;  // invitado: la conexión con el anfitrión

    sim::PlayerId local_ = 0;
    std::uint8_t players_ = 0;
    std::uint8_t auto_start_ = 0;
    std::uint8_t connected_ = 0;
    std::string settings_;
    std::uint8_t takeover_profile_ = 0;
    std::vector<ChatLine> chat_;
    std::vector<std::optional<std::uint32_t>> dropped_;  // por jugador: primer turno sin él
    std::int32_t turn_ticks_ = 0;
    std::int32_t delay_turns_ = 0;
    std::int32_t hash_every_ = 0;
    sim::Tick end_tick_ = 0;

    std::vector<sim::Command> local_commands_;
    std::map<std::uint32_t, TurnInputs> turns_;
    std::uint32_t next_send_turn_ = 0;  // próximo turno para el que se envía mensaje
    std::uint32_t next_run_turn_ = 0;   // próximo turno cuyas órdenes se emiten
    std::map<sim::Tick, OwnHash> own_hashes_;
    std::vector<RemoteHash> remote_hashes_;
    std::size_t hashes_compared_ = 0;

    std::optional<std::pair<sim::Tick, std::uint64_t>> own_final_;
    std::vector<std::optional<std::pair<sim::Tick, std::uint64_t>>> finals_;

    std::chrono::steady_clock::time_point created_;
    std::optional<std::chrono::steady_clock::time_point> waiting_since_;
};

}  // namespace rts::game
