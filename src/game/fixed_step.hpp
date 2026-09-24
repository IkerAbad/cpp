#pragma once

// Reloj de paso fijo (patrón "Fix Your Timestep"). Decide cuántos ticks de simulación
// ejecutar en cada fotograma a partir del tiempo real transcurrido, medido en
// nanosegundos enteros. No conoce SDL: el llamador le pasa el tiempo.

#include <cstdint>

namespace rts::game {

struct FixedStepConfig {
    std::int64_t tick_ns = 0;
    // Tope de ticks por fotograma. Evita la espiral de la muerte: si un fotograma
    // tarda más que los ticks que genera, el retraso se descarta en lugar de acumularse.
    std::int32_t max_ticks_per_frame = 0;
};

struct StepPlan {
    std::int32_t ticks = 0;          // ticks a ejecutar en este fotograma
    std::int64_t dropped_ticks = 0;  // ticks descartados por el tope
    double alpha = 0.0;              // fracción [0, 1) del siguiente tick, para interpolar
};

class FixedStepClock {
public:
    explicit FixedStepClock(const FixedStepConfig& config);

    StepPlan advance(std::int64_t elapsed_ns);

    [[nodiscard]] std::int64_t accumulated_ns() const noexcept { return accumulator_ns_; }

private:
    FixedStepConfig config_;
    std::int64_t accumulator_ns_ = 0;
};

}  // namespace rts::game
