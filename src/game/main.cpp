#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/callback_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "game/app.hpp"
#include "game/config.hpp"
#include "platform/window.hpp"

namespace {

// Registro en consola y en rts.log junto al ejecutable (en Windows, al abrirlo con
// doble clic, la consola se cierra con el programa y su mensaje se pierde). Guarda el
// último error para mostrarlo en una ventana si el arranque falla.
std::string g_last_error;

void setup_logging() {
    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    try {
        const auto path = std::filesystem::path(rts::platform::executable_dir()) / "rts.log";
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true));
    } catch (const spdlog::spdlog_ex&) {
        // Sin permiso de escritura junto al ejecutable: solo consola.
    }
    sinks.push_back(std::make_shared<spdlog::sinks::callback_sink_mt>([](const spdlog::details::log_msg& msg) {
        if (msg.level >= spdlog::level::err) {
            g_last_error.assign(msg.payload.data(), msg.payload.size());
        }
    }));
    auto logger = std::make_shared<spdlog::logger>("rts", sinks.begin(), sinks.end());
    logger->flush_on(spdlog::level::info);
    spdlog::set_default_logger(logger);
}

// Error de arranque con ventana: un cuadro de diálogo que no se cierra solo.
void show_error(const std::string& message) {
    const std::string text = message + "\n\nDetalles en rts.log, junto a rts.exe.";
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "rts: no se pudo arrancar", text.c_str(), nullptr);
}

int run(int argc, char** argv, bool& windowed) {
    const auto options = rts::game::parse_arguments(argc, argv);
    if (!options) {
        return 2;
    }
    windowed = !options->headless && options->verify_replay.empty();
    // Las repeticiones llevan sus propios datos: no se lee data/.
    if (!options->verify_replay.empty()) {
        return rts::game::run_verify_replay(options->verify_replay);
    }
    if (!options->replay.empty()) {
        return rts::game::run_replay(options->replay, options->max_frames);
    }
    if (!options->load.empty()) {
        return rts::game::run_load(options->load, options->max_frames);
    }
    const auto data = rts::game::load_game_data(options->data_dir);
    if (!data) {
        spdlog::error("Datos: {}", data.error());
        return 1;
    }
    if (options->headless && (options->host_port || !options->join_host.empty())) {
        return rts::game::run_net_headless(*data, *options);
    }
    if (options->headless) {
        return rts::game::run_headless(*data, options->headless_ticks, options->record);
    }
    // Sin límite de fotogramas (lo normal al abrirlo), el menú; con él (pruebas de humo),
    // directamente una partida.
    if (options->max_frames == 0) {
        return rts::game::run_interactive(*data);
    }
    return rts::game::run_windowed(*data, options->max_frames);
}

}  // namespace

// SDL3 no redefine main salvo que se incluya SDL_main.h, que aquí no hace falta:
// el ejecutable es de consola en Windows y la ventana la crea SDL_CreateWindow.
int main(int argc, char** argv) {
    setup_logging();
    bool windowed = true;
    int code = 0;
    try {
        code = run(argc, argv, windowed);
    } catch (const std::exception& e) {
        spdlog::error("Excepción no controlada: {}", e.what());
        code = 3;
    }
    if (code != 0 && windowed) {
        show_error(g_last_error.empty() ? std::string("Error desconocido (código ") + std::to_string(code) + ")"
                                        : g_last_error);
    }
    return code;
}
