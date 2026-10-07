#include "game/crash_report.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <format>
#include <fstream>

#include "game/replay.hpp"
#include "sim/world.hpp"

namespace rts::game {

namespace {

struct Current {
    const ReplayRecorder* recorder = nullptr;
    const sim::World* world = nullptr;
};

Current g_current;
std::filesystem::path g_root;
std::filesystem::path g_log;
CrashNotify g_notify = nullptr;
std::atomic<bool> g_writing{false};  // un informe a la vez (una caída dentro de otra)

#if defined(_MSC_VER)
constexpr std::string_view kCompiler = "MSVC/clang-cl";
#elif defined(__clang__)
constexpr std::string_view kCompiler = "Clang";
#elif defined(__GNUC__)
constexpr std::string_view kCompiler = "GCC";
#else
constexpr std::string_view kCompiler = "desconocido";
#endif

#ifdef NDEBUG
constexpr std::string_view kBuild = "optimizada";
#else
constexpr std::string_view kBuild = "depuración";
#endif

std::string signal_name(int sig) {
    switch (sig) {
        case SIGSEGV:
            return "SIGSEGV (acceso a memoria no válida)";
        case SIGABRT:
            return "SIGABRT (abort, a menudo una aserción)";
        case SIGFPE:
            return "SIGFPE (error aritmético)";
        case SIGILL:
            return "SIGILL (instrucción no válida)";
        default:
            return std::format("señal {}", sig);
    }
}

void on_signal(int sig) {
    write_crash_report(signal_name(sig));
    std::signal(sig, SIG_DFL);  // y termina como habría terminado
    std::raise(sig);
}

[[noreturn]] void on_terminate() {
    std::string reason = "std::terminate";
    if (const std::exception_ptr e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& ex) {
            reason = std::format("excepción no capturada: {}", ex.what());
        } catch (...) {
            reason = "excepción no capturada de tipo desconocido";
        }
    }
    write_crash_report(reason);
    std::abort();
}

std::string time_stamp() {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%Y-%m-%d_%H-%M-%S}", now);
}

}  // namespace

CrashGuard::CrashGuard(const ReplayRecorder& recorder, const sim::World& world) noexcept {
    g_current = {&recorder, &world};
}

CrashGuard::~CrashGuard() {
    g_current = {};
}

void install_crash_handlers(const std::filesystem::path& report_root, const std::filesystem::path& log,
                            CrashNotify notify) {
    g_root = report_root;
    g_log = log;
    g_notify = notify;
    for (const int sig : {SIGSEGV, SIGABRT, SIGFPE, SIGILL}) {
        std::signal(sig, on_signal);
    }
    std::set_terminate(on_terminate);
}

std::optional<std::filesystem::path> write_crash_report(std::string_view reason) {
    if (g_writing.exchange(true)) {
        return std::nullopt;
    }
    std::optional<std::filesystem::path> out;
    try {
        if (g_notify != nullptr) {
            g_notify(reason);
        }
        std::filesystem::path dir = g_root / std::format("informe-{}", time_stamp());
        // Dos informes en el mismo segundo: otra carpeta.
        for (int n = 2; std::filesystem::exists(dir); ++n) {
            dir = g_root / std::format("informe-{}-{}", time_stamp(), n);
        }
        std::filesystem::create_directories(dir);
        std::ofstream txt(dir / "informe.txt", std::ios::binary);
        txt << std::format("Qué pasó: {}\nCompilación: {}, {}\n", reason, kCompiler, kBuild);
        if (g_current.recorder != nullptr && g_current.world != nullptr) {
            const sim::World& w = *g_current.world;
            txt << std::format("Tick: {}\n", w.tick());
            const Replay replay = g_current.recorder->finish(w);
            if (save_replay(dir / "partida.rtsrep", replay)) {
                txt << "Repetición: partida.rtsrep (rts --verify-replay partida.rtsrep la reproduce)\n";
            }
        } else {
            txt << "Sin partida en curso.\n";
        }
        txt.close();
        std::error_code ec;
        if (!g_log.empty() && std::filesystem::exists(g_log)) {
            std::filesystem::copy_file(g_log, dir / "rts.log", std::filesystem::copy_options::overwrite_existing, ec);
        }
        out = dir;
    } catch (...) {
        // Un informe que falla no debe tapar el fallo original.
    }
    g_writing = false;
    return out;
}

}  // namespace rts::game
