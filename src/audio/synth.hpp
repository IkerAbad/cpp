#pragma once

// Sonido propio (F2): todo se sintetiza por código a partir de recetas de data/sound.toml.
// Nada se carga de fuera. Puro: sin SDL ni E/S; produce muestras PCM en coma flotante
// (mono, -1..1) que el mezclador reproduce.

#include <cstdint>
#include <string>
#include <vector>

namespace rts::audio {

enum class Wave : std::uint8_t { Sine, Triangle, Square, Saw, Noise };

// Una capa de un sonido: un oscilador con su envolvente y su filtro.
struct Layer {
    Wave wave = Wave::Sine;
    std::int32_t freq_start_hz = 0;  // el tono se desliza de start a end durante la capa
    std::int32_t freq_end_hz = 0;
    std::int32_t delay_ms = 0;       // empieza tras este retraso
    std::int32_t attack_ms = 0;      // subida lineal
    std::int32_t decay_ms = 0;       // caída exponencial hasta el silencio (-60 dB)
    std::int32_t volume_percent = 0;
    std::int32_t lowpass_hz = 0;     // 0 = sin filtro
    std::int32_t highpass_hz = 0;
    std::int32_t vibrato_hz = 0;
    std::int32_t vibrato_cents = 0;
    std::int32_t repeats = 1;        // golpes repetidos (crepitar, pasos)
    std::int32_t repeat_ms = 0;      // separación media entre golpes
};

struct SoundRecipe {
    std::string name;
    std::vector<Layer> layers;
    std::int32_t volume_percent = 100;
    std::int32_t variants = 1;          // versiones con el tono algo cambiado (no suena clonado)
    std::int32_t variant_cents = 0;     // separación de tono entre versiones
    std::int32_t cooldown_ms = 0;       // el mismo sonido no se repite antes
};

// Muestras de la receta, con el tono desplazado cents (100 = un semitono). seed varía el
// ruido y los tiempos de las repeticiones; mismo seed, mismas muestras.
[[nodiscard]] std::vector<float> render(const SoundRecipe& recipe, std::int32_t sample_rate, std::int32_t cents,
                                        std::uint32_t seed);

// Música propia: piezas modales (como la música medieval que se toca hoy) con bordón,
// melodía de flauta y tamboril, compuestas por un generador con semilla.
struct MusicPiece {
    std::string name;
    std::uint32_t seed = 0;
    std::int32_t tempo_bpm = 0;
    std::int32_t root_hz = 0;
    std::vector<std::int32_t> mode;  // semitonos de la escala desde la tónica (p. ej. dórico)
    std::int32_t bars = 0;
    std::int32_t beats_per_bar = 0;
    std::int32_t melody_percent = 0;
    std::int32_t drone_percent = 0;
    std::int32_t drum_percent = 0;
    std::int32_t drum_density_percent = 0;  // golpes de tamboril fuera del primer tiempo
};

// Una vuelta entera de la pieza (pensada para repetirse sin corte).
[[nodiscard]] std::vector<float> compose(const MusicPiece& piece, std::int32_t sample_rate);

}  // namespace rts::audio
