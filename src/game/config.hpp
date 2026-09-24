#pragma once

// Configuración de arranque, leída de data/config/engine.toml. Ningún parámetro
// ajustable vive en el código.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

#include "sim/world.hpp"

namespace rts::game {

struct WindowConfig {
    std::string title;
    std::int32_t width = 0;
    std::int32_t height = 0;
    bool vsync = true;
};

struct LoopConfig {
    std::int32_t max_ticks_per_frame = 0;
};

struct EngineConfig {
    WindowConfig window;
    LoopConfig loop;
    sim::DemoParams demo;
};

// El error es un texto legible con la clave que falla.
std::expected<EngineConfig, std::string> parse_engine_config(std::string_view toml_text,
                                                             std::string_view source_name = "<memoria>");
std::expected<EngineConfig, std::string> load_engine_config(const std::filesystem::path& path);

}  // namespace rts::game
