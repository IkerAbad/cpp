#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

#include "game/config.hpp"

namespace rts::game {

struct LaunchOptions {
    std::filesystem::path config_path;
    bool headless = false;
    // Solo en modo headless: número de ticks a simular antes de salir.
    std::int64_t headless_ticks = 0;
};

// Analiza argv. Devuelve nullopt y escribe la ayuda si los argumentos no son válidos.
std::optional<LaunchOptions> parse_arguments(int argc, char** argv);

// Simula sin ventana ni GPU y escribe el hash final. Lo usa la CI.
int run_headless(const EngineConfig& config, std::int64_t ticks);

// Bucle interactivo con ventana, render y paso fijo.
int run_windowed(const EngineConfig& config);

}  // namespace rts::game
