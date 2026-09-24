#include "game/app.hpp"

#include <charconv>
#include <chrono>
#include <string_view>
#include <utility>

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include "game/fixed_step.hpp"
#include "platform/profile.hpp"
#include "platform/window.hpp"
#include "render/renderer.hpp"
#include "sim/world.hpp"

namespace rts::game {

namespace {

constexpr std::int64_t kNsPerSecond = 1'000'000'000;
constexpr std::int64_t kTickNs = kNsPerSecond / sim::kTicksPerSecond;
static_assert(kTickNs * sim::kTicksPerSecond == kNsPerSecond, "el tick debe durar un número entero de ns");

constexpr double kNsPerMs = 1'000'000.0;
// Peso de la media móvil exponencial del coste por tick: ~20 muestras efectivas.
constexpr double kTickCostSmoothing = 0.05;

using SteadyClock = std::chrono::steady_clock;

double elapsed_ms(SteadyClock::time_point since) {
    return std::chrono::duration<double, std::milli>(SteadyClock::now() - since).count();
}

void print_usage() {
    spdlog::info("Uso: rts [--config <fichero.toml>] [--headless --ticks <N>]");
}

}  // namespace

std::optional<LaunchOptions> parse_arguments(int argc, char** argv) {
    LaunchOptions options;
    options.config_path = std::filesystem::path(platform::executable_dir()) / "data" / "config" / "engine.toml";

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--headless") {
            options.headless = true;
        } else if (arg == "--config" && has_value) {
            options.config_path = argv[++i];
        } else if (arg == "--ticks" && has_value) {
            const std::string_view value = argv[++i];
            const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), options.headless_ticks);
            if (ec != std::errc{} || end != value.data() + value.size() || options.headless_ticks < 0) {
                spdlog::error("--ticks necesita un entero no negativo, recibido '{}'", value);
                return std::nullopt;
            }
        } else {
            spdlog::error("Argumento no reconocido: '{}'", arg);
            print_usage();
            return std::nullopt;
        }
    }
    return options;
}

int run_headless(const EngineConfig& config, std::int64_t ticks) {
    sim::World world(config.demo);
    const auto start = SteadyClock::now();
    for (std::int64_t i = 0; i < ticks; ++i) {
        world.step();
    }
    const double total_ms = elapsed_ms(start);
    const double per_tick = ticks > 0 ? total_ms / static_cast<double>(ticks) : 0.0;
    spdlog::info("headless: {} ticks, {} entidades, {:.3f} ms total, {:.4f} ms/tick", ticks,
                 config.demo.point_count, total_ms, per_tick);
    // Formato estable: la CI lo compara entre plataformas.
    spdlog::info("state_hash={:016x}", world.state_hash());
    return 0;
}

int run_windowed(const EngineConfig& config) {
    auto window = platform::Window::create({config.window.title, config.window.width, config.window.height});
    if (!window) {
        spdlog::error("{}", window.error());
        return 1;
    }
    auto renderer = render::Renderer::create((*window)->handle(), config.window.vsync);
    if (!renderer) {
        spdlog::error("{}", renderer.error());
        return 1;
    }
    spdlog::info("Backend GPU: {}", (*renderer)->driver_name());

    sim::World world(config.demo);
    sim::Snapshot prev;
    sim::Snapshot curr;
    world.write_snapshot(curr);
    prev = curr;

    FixedStepClock clock({kTickNs, config.loop.max_ticks_per_frame});
    render::FrameStats stats;
    stats.state_hash = world.state_hash();
    std::uint64_t last_ns = platform::now_ns();

    bool running = true;
    while (running) {
        RTS_PROFILE_FRAME();
        SDL_Event event;
        while ((*window)->poll_event(event)) {
            (*renderer)->process_event(event);
            if ((*window)->is_close_request(event)) {
                running = false;
            }
        }

        const std::uint64_t now = platform::now_ns();
        const auto frame_ns = static_cast<std::int64_t>(now - last_ns);
        last_ns = now;

        if ((*window)->minimized()) {
            // Minimizada la simulación sigue su curso; solo se evita presentar.
            // SDL_WaitEventTimeout duerme sin quemar CPU y despierta con cualquier evento.
            SDL_WaitEventTimeout(nullptr, static_cast<std::int32_t>(kTickNs / 1'000'000));
        }

        const StepPlan plan = clock.advance(frame_ns);
        const auto sim_start = SteadyClock::now();
        for (std::int32_t i = 0; i < plan.ticks; ++i) {
            RTS_PROFILE_ZONE_NAMED("sim_tick");
            std::swap(prev, curr);
            world.step();
            world.write_snapshot(curr);
        }
        stats.sim_ms_last_frame = elapsed_ms(sim_start);
        if (plan.ticks > 0) {
            const double per_tick = stats.sim_ms_last_frame / plan.ticks;
            stats.sim_ms_per_tick += kTickCostSmoothing * (per_tick - stats.sim_ms_per_tick);
            stats.state_hash = world.state_hash();
        }
        if (plan.dropped_ticks > 0) {
            spdlog::warn("Fotograma lento: {} ticks descartados", plan.dropped_ticks);
        }
        stats.frame_ms = static_cast<double>(frame_ns) / kNsPerMs;
        stats.ticks_this_frame = plan.ticks;
        stats.dropped_ticks_total += plan.dropped_ticks;
        stats.alpha = plan.alpha;

        if (!(*window)->minimized()) {
            (*renderer)->render_frame(prev, curr, plan.alpha, stats);
        }
    }
    return 0;
}

}  // namespace rts::game
