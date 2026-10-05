#pragma once

// Avisos al jugador: comparan el estado anterior con el actual y dicen lo que merece
// su atención (le atacan, algo arde, sus tropas pasan hambre, no queda comida para
// las raciones, una unidad nueva está lista). Cada clase de aviso tiene una espera
// mínima por zona para no inundar la pantalla. Es presentación: no toca la simulación.

#include <cstdint>
#include <string>
#include <vector>

#include "game/config.hpp"
#include "sim/world.hpp"

namespace rts::game {

enum class AlertKind : std::uint8_t { UnderAttack, Fire, Hunger, NoFood, UnitReady, Rout, Count };

struct Alert {
    AlertKind kind = AlertKind::UnderAttack;
    sim::TileCoord where;  // para llevar la cámara
    sim::Tick tick = 0;
};

class AlertTracker {
public:
    explicit AlertTracker(const AlertParams& params) : params_(params) {}

    // Avisos nuevos para el jugador me entre prev y curr (ticks consecutivos o no).
    std::vector<Alert> update(const sim::Snapshot& prev, const sim::Snapshot& curr, sim::PlayerId me);

    // Los avisos aún en pantalla en el tick dado, el más reciente primero.
    [[nodiscard]] std::vector<Alert> shown(sim::Tick now) const;

private:
    bool allow(AlertKind kind, sim::TileCoord where, sim::Tick tick);

    struct Last {
        AlertKind kind;
        sim::TileCoord zone;
        sim::Tick tick;
    };
    AlertParams params_;
    std::vector<Last> last_;
    std::vector<Alert> recent_;
};

}  // namespace rts::game
