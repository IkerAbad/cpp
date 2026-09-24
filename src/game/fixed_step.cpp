#include "game/fixed_step.hpp"

#include <algorithm>
#include <cassert>

namespace rts::game {

FixedStepClock::FixedStepClock(const FixedStepConfig& config) : config_(config) {
    assert(config_.tick_ns > 0);
    assert(config_.max_ticks_per_frame > 0);
}

StepPlan FixedStepClock::advance(std::int64_t elapsed_ns) {
    // Un reloj que retrocede (cambio de hora, fallo del SO) no debe restar tiempo.
    accumulator_ns_ += std::max<std::int64_t>(elapsed_ns, 0);

    StepPlan plan;
    const std::int64_t due = accumulator_ns_ / config_.tick_ns;
    const std::int64_t run = std::min<std::int64_t>(due, config_.max_ticks_per_frame);
    plan.ticks = static_cast<std::int32_t>(run);
    plan.dropped_ticks = due - run;
    // Se conserva el resto parcial y se descartan los ticks completos que no se ejecutan.
    accumulator_ns_ -= due * config_.tick_ns;
    plan.alpha = static_cast<double>(accumulator_ns_) / static_cast<double>(config_.tick_ns);
    return plan;
}

}  // namespace rts::game
