#pragma once

// La estación de campaña (H2). Las guerras de antes se hacían en buen tiempo: «In
// premodern times, campaigns were usually interrupted during the winter season», y en
// Europa la campaña iba «between the planting (late spring) and harvest times (late
// autumn)» (Wikipedia, «Military campaign»). Cada estación cambia, en tanto por ciento:
//   - farm_percent: el ritmo de la cosecha en las granjas (0: no hay nada que cosechar);
//   - forage_percent: lo que da el bosque a la tropa que forrajea (0: nada);
//   - march_percent: la velocidad de marcha (lluvia y barro);
//   - ration_percent: lo que come la tropa (125: un 25 % más, por el frío).
// Las cifras son de diseño (data/config/engine.toml, [[season.kind]]). El verano es la
// estación de campaña y va al 100 % en todo: con él, el juego es el de siempre.
//
// Un día de juego dura 18 minutos, así que una partida cubre dos o tres días: por
// omisión la estación no cambia (season_ticks = 0) y la fija el escenario o el capítulo.
// Con season_ticks > 0, cambia cada tantos ticks: primavera, verano, otoño, invierno.

#include <array>
#include <cstddef>
#include <cstdint>

#include "sim/tick.hpp"

namespace reinos::sim {

inline constexpr std::size_t kSeasonCount = 4;  // primavera, verano, otoño, invierno, en ese orden
inline constexpr std::uint8_t kSummer = 1;

struct SeasonEffects {
    std::int32_t farm_percent = 100;
    std::int32_t forage_percent = 100;
    std::int32_t march_percent = 100;
    std::int32_t ration_percent = 100;
};

struct SeasonParams {
    std::array<SeasonEffects, kSeasonCount> seasons{};  // todas al 100 %: sin efecto
    std::uint8_t start = kSummer;  // con qué estación empieza la partida
    Tick season_ticks = 0;         // 0: no cambia
};

// Estación en el tick dado (índice en seasons).
[[nodiscard]] inline std::uint8_t season_at(const SeasonParams& p, Tick tick) noexcept {
    const Tick passed = p.season_ticks > 0 ? tick / p.season_ticks : 0;
    return static_cast<std::uint8_t>((p.start + passed) % kSeasonCount);
}

[[nodiscard]] inline const SeasonEffects& season_effects(const SeasonParams& p, Tick tick) noexcept {
    return p.seasons[season_at(p, tick)];
}

}  // namespace reinos::sim
