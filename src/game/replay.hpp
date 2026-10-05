#pragma once

// Repeticiones (M5). Una repetición guarda lo mínimo para reconstruir la partida: una
// copia de los ficheros de datos y las órdenes de los jugadores humanos, cada una con
// el tick en que se emitió. La IA vive dentro de la simulación y es determinista: sus
// órdenes no se graban, se vuelven a generar. Cada checkpoint_interval_ticks se graba
// el hash de estado; al reproducir, el primer hash distinto localiza la divergencia.
//
// Formato binario .rtsrep, todo en little-endian explícito:
//   "RTSREP\0\0", u32 versión, u64 hash de los datos,
//   u32 ficheros { str ruta, str texto },
//   u32 órdenes { u32 tick de emisión, orden },
//   u32 checkpoints { u32 tick, u64 hash },
//   u32 tick final, u64 hash final,
//   u64 FNV-1a de todos los bytes anteriores.
// Una orden: u32 tick, u8 jugador, u8 tipo, u32 n + n x u32 unidades, i32 x, i32 y,
// u32 objeto, u8 kind. Un str: u32 longitud + bytes.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "game/config.hpp"
#include "sim/tick.hpp"
#include "sim/units.hpp"
#include "sim/world.hpp"

namespace rts::game {

inline constexpr std::uint32_t kReplayFormatVersion = 1;

struct ReplayCommand {
    sim::Tick issued_at = 0;  // world.tick() cuando se emitió (antes del step de ese tick)
    sim::Command command;
};

struct ReplayCheckpoint {
    sim::Tick tick = 0;  // world.tick() después del step
    std::uint64_t hash = 0;
};

struct Replay {
    std::vector<DataFile> data;
    std::vector<ReplayCommand> commands;
    std::vector<ReplayCheckpoint> checkpoints;
    sim::Tick end_tick = 0;
    std::uint64_t end_hash = 0;
};

// Hash de los ficheros de datos: identifica con qué datos se jugó.
[[nodiscard]] std::uint64_t data_hash(std::span<const DataFile> files);

[[nodiscard]] std::vector<std::uint8_t> encode_replay(const Replay& replay);
// Rechaza ficheros truncados, corruptos (suma de comprobación o hash de datos) o de
// otra versión del formato, con un mensaje que dice cuál es el caso.
[[nodiscard]] std::expected<Replay, std::string> decode_replay(std::span<const std::uint8_t> bytes);

std::expected<void, std::string> save_replay(const std::filesystem::path& path, const Replay& replay);
std::expected<Replay, std::string> load_replay(const std::filesystem::path& path);

// Graba una partida en curso. Toda orden humana pasa por issue(); after_step() se
// llama después de cada world.step().
class ReplayRecorder {
public:
    ReplayRecorder(std::vector<DataFile> data, std::int32_t checkpoint_interval_ticks);

    void issue(sim::World& world, sim::Command command);
    void after_step(const sim::World& world);
    [[nodiscard]] Replay finish(const sim::World& world) const;

private:
    Replay replay_;
    std::int32_t interval_;
};

// Reproduce una repetición sobre un mundo creado con sus datos: before_step() emite
// las órdenes grabadas para el tick actual; after_step() compara el hash con el
// checkpoint de ese tick, si lo hay.
class ReplayPlayer {
public:
    explicit ReplayPlayer(const Replay& replay) : replay_(&replay) {}

    void before_step(sim::World& world);
    void after_step(const sim::World& world);

    [[nodiscard]] bool finished(const sim::World& world) const noexcept { return world.tick() >= replay_->end_tick; }
    [[nodiscard]] bool diverged() const noexcept { return diverged_at_ != kNone; }
    [[nodiscard]] sim::Tick diverged_at() const noexcept { return diverged_at_; }
    [[nodiscard]] std::uint64_t expected_hash() const noexcept { return expected_; }
    [[nodiscard]] std::uint64_t actual_hash() const noexcept { return actual_; }
    [[nodiscard]] std::size_t checkpoints_checked() const noexcept { return next_checkpoint_; }

private:
    static constexpr sim::Tick kNone = ~sim::Tick{0};

    void compare(sim::Tick tick, std::uint64_t expected, std::uint64_t actual);

    const Replay* replay_;
    std::size_t next_command_ = 0;
    std::size_t next_checkpoint_ = 0;
    sim::Tick diverged_at_ = kNone;
    std::uint64_t expected_ = 0;
    std::uint64_t actual_ = 0;
};

struct VerifyResult {
    bool ok = false;
    sim::Tick ticks = 0;                 // ticks simulados
    std::size_t checkpoints = 0;         // checkpoints comparados
    std::optional<sim::Tick> diverged_at;  // primer tick con hash distinto
    std::uint64_t expected_hash = 0;
    std::uint64_t actual_hash = 0;
};

// Reproduce la repetición entera sin ventana. Se detiene en la primera divergencia.
[[nodiscard]] VerifyResult verify_replay(const Replay& replay, const sim::WorldParams& params);

// Partidas guardadas (.rtssav): son la repetición hasta el momento de guardar. Cargar
// es rehacerla a toda velocidad sobre world (recién creado con sus datos) volviendo a
// grabar sus órdenes en recorder, que sigue grabando desde ahí. Al ser determinista,
// se llega exactamente al mismo estado: se comprueba con el hash del final.
std::expected<void, std::string> resume_saved_game(const Replay& save, sim::World& world, ReplayRecorder& recorder);

}  // namespace rts::game
