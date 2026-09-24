#pragma once

// Configuración de arranque, leída de data/. Ningún parámetro ajustable vive en el código.

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "render/view_params.hpp"
#include "sim/world.hpp"

namespace rts::game {

// --- data/terrain.toml ---------------------------------------------------------

struct TerrainInfo {
    std::string name;
    bool passable = false;
    std::array<std::uint8_t, 3> color{};  // RGB provisional
};

struct TerrainCatalog {
    // El índice en el vector es el TerrainId.
    std::vector<TerrainInfo> types;

    [[nodiscard]] std::optional<sim::TerrainId> find(std::string_view name) const;
};

// --- data/config/engine.toml ---------------------------------------------------

struct WindowConfig {
    std::string title;
    std::int32_t width = 0;
    std::int32_t height = 0;
    bool vsync = true;
};

struct LoopConfig {
    std::int32_t max_ticks_per_frame = 0;
};

struct CameraConfig {
    std::int32_t scroll_keys_px_per_s = 0;
    std::int32_t scroll_edge_px_per_s = 0;
    std::int32_t edge_margin_px = 0;
};

struct SelectionConfig {
    std::int32_t drag_threshold_px = 0;
    std::int32_t click_radius_px = 0;
};

struct EngineConfig {
    WindowConfig window;
    LoopConfig loop;
    sim::WorldParams world;
    render::ViewParams view;
    CameraConfig camera;
    SelectionConfig selection;
};

// Los errores son textos legibles que nombran el fichero y la clave que falla.
std::expected<TerrainCatalog, std::string> parse_terrain_catalog(std::string_view toml_text,
                                                                 std::string_view source_name = "<memoria>");

// Las bandas del mapa nombran terrenos: se resuelven contra el catálogo.
std::expected<EngineConfig, std::string> parse_engine_config(std::string_view toml_text,
                                                             const TerrainCatalog& terrain,
                                                             std::string_view source_name = "<memoria>");

struct GameData {
    TerrainCatalog terrain;
    EngineConfig engine;
};

// Lee data/terrain.toml y data/config/engine.toml bajo data_dir.
std::expected<GameData, std::string> load_game_data(const std::filesystem::path& data_dir);

}  // namespace rts::game
