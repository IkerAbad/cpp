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
    // Las repeticiones llevan sus propios datos: no se lee data/.
    if (!options->verify_replay.empty()) {
        return rts::game::run_verify_replay(options->verify_replay);
    }
    if (!options->replay.empty()) {
        return rts::game::run_replay(options->replay, options->max_frames);
    }
    const auto data = rts::game::load_game_data(options->data_dir);
    if (!data) {
        spdlog::error("Datos: {}", data.error());
        return 1;
    }
    if (options->headless) {
        return rts::game::run_headless(*data, options->headless_ticks, options->record);
    }
    return rts::game::run_windowed(*data, options->max_frames);
}
