#include <spdlog/spdlog.h>

#include "game/app.hpp"
#include "game/config.hpp"

// SDL3 no redefine main salvo que se incluya SDL_main.h, que aquí no hace falta:
// el ejecutable es de consola en Windows y la ventana la crea SDL_CreateWindow.
int main(int argc, char** argv) {
    const auto options = rts::game::parse_arguments(argc, argv);
    if (!options) {
        return 2;
    }
    const auto config = rts::game::load_engine_config(options->config_path);
    if (!config) {
        spdlog::error("Configuración: {}", config.error());
        return 1;
    }
    if (options->headless) {
        return rts::game::run_headless(*config, options->headless_ticks);
    }
    return rts::game::run_windowed(*config);
}
