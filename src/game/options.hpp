#pragma once

// Opciones del jugador (F5): idioma, pantalla, volumen y teclas. No son datos de la
// partida (no viajan en repeticiones ni por la red): los valores de partida están en
// data/opciones.toml y lo que cambia el jugador, en opciones.toml junto al ejecutable,
// con el mismo formato (lo que falte ahí se toma de data/).
//
//   language = "es"                   # data/lang/<código>.toml
//   fullscreen = false
//   width = 1600                      # ventana
//   height = 900
//   resolutions = [[1280, 720], ...]  # solo en data/: las que se ofrecen
//   [volume] master = 80, effects = 80, music = 40     # por ciento
//   [keys] camera_left = "Left", ...  # nombres de tecla de SDL ("A", "Space", "F5")

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rts::game {

inline constexpr std::string_view kOptionsFile = "opciones.toml";

enum class KeyAction : std::uint8_t {
    CameraLeft,
    CameraRight,
    CameraUp,
    CameraDown,
    CameraLeftAlt,
    CameraRightAlt,
    CameraUpAlt,
    CameraDownAlt,
    LastAlert,
    QuickSave,
    Menu,
    Debug,
    Help,
    Count,
};

inline constexpr std::size_t kKeyActionCount = static_cast<std::size_t>(KeyAction::Count);

// Nombres de las acciones en los ficheros, en el orden de KeyAction.
inline constexpr std::array<std::string_view, kKeyActionCount> kKeyActionIds{
    "camera_left",     "camera_right",     "camera_up",  "camera_down", "camera_left_alt",
    "camera_right_alt", "camera_up_alt",   "camera_down_alt", "last_alert", "quick_save",
    "menu",            "debug",            "help",
};

struct Options {
    std::string language;
    bool fullscreen = false;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::vector<std::pair<std::int32_t, std::int32_t>> resolutions;
    std::int32_t master_percent = 0;
    std::int32_t effects_percent = 0;
    std::int32_t music_percent = 0;
    std::array<std::string, kKeyActionCount> keys;

    [[nodiscard]] const std::string& key(KeyAction a) const { return keys[static_cast<std::size_t>(a)]; }
};

// Con base = nullptr, el fichero debe traerlo todo (data/opciones.toml); con base, lo
// que falte se toma de ella (el del jugador).
[[nodiscard]] std::expected<Options, std::string> parse_options(std::string_view text, const Options* base,
                                                              std::string_view source = "<memoria>");
// Lo que se guarda para el jugador (sin la lista de resoluciones).
[[nodiscard]] std::string options_toml(const Options& o);

}  // namespace rts::game
