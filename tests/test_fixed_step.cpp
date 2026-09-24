#include <doctest/doctest.h>

#include "game/fixed_step.hpp"

using rts::game::FixedStepClock;
using rts::game::FixedStepConfig;

namespace {
constexpr std::int64_t kMs = 1'000'000;
constexpr FixedStepConfig kConfig{50 * kMs, 5};
}  // namespace

TEST_CASE("Paso fijo: 200 ms de tiempo real son exactamente 4 ticks") {
    SUBCASE("en un solo fotograma") {
        FixedStepClock clock(kConfig);
        const auto plan = clock.advance(200 * kMs);
        CHECK(plan.ticks == 4);
        CHECK(plan.dropped_ticks == 0);
        CHECK(clock.accumulated_ns() == 0);
    }
    SUBCASE("en fotogramas de 16 ms (60 Hz)") {
        FixedStepClock clock(kConfig);
        std::int32_t ticks = 0;
        std::int64_t elapsed = 0;
        while (elapsed + 16 * kMs <= 200 * kMs) {
            ticks += clock.advance(16 * kMs).ticks;
            elapsed += 16 * kMs;
        }
        ticks += clock.advance(200 * kMs - elapsed).ticks;  // resto: 8 ms
        CHECK(ticks == 4);
        CHECK(clock.accumulated_ns() == 0);
    }
}

TEST_CASE("Paso fijo: el resto parcial se conserva entre fotogramas") {
    FixedStepClock clock(kConfig);
    CHECK(clock.advance(49 * kMs).ticks == 0);
    CHECK(clock.advance(1 * kMs).ticks == 1);
    CHECK(clock.accumulated_ns() == 0);
}

TEST_CASE("Paso fijo: alpha es la fracción del siguiente tick") {
    FixedStepClock clock(kConfig);
    const auto plan = clock.advance(75 * kMs);
    CHECK(plan.ticks == 1);
    CHECK(plan.alpha == doctest::Approx(0.5));
}

TEST_CASE("Paso fijo: un parón largo no provoca la espiral de la muerte") {
    FixedStepClock clock(kConfig);
    const auto plan = clock.advance(1'010 * kMs);  // 20 ticks debidos + 10 ms
    CHECK(plan.ticks == 5);
    CHECK(plan.dropped_ticks == 15);
    CHECK(clock.accumulated_ns() == 10 * kMs);
    CHECK(clock.advance(0).ticks == 0);
}

TEST_CASE("Paso fijo: un reloj que retrocede no resta tiempo") {
    FixedStepClock clock(kConfig);
    CHECK(clock.advance(30 * kMs).ticks == 0);
    CHECK(clock.advance(-100 * kMs).ticks == 0);
    CHECK(clock.accumulated_ns() == 30 * kMs);
}
