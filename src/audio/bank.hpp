#pragma once

// Banco de sonidos (F2): todas las recetas y piezas de un SoundSpec sintetizadas una vez
// y guardadas en el mezclador. play() elige la siguiente versión de cada receta.

#include <cstdint>
#include <vector>

#include "audio/mixer.hpp"
#include "audio/sound_spec.hpp"

namespace rts::audio {

// Un sonido a reproducir: receta (índice en SoundSpec::recipes), volumen 0..1 y panorama.
struct SoundCue {
    std::int32_t recipe = -1;
    float volume = 0.0f;
    float pan = 0.0f;
};

class Bank {
public:
    // Con with_music = false, las piezas no se componen aquí: compose_music() puede
    // hacerlo en otro hilo y add_music() las entrega después.
    Bank(const SoundSpec& spec, Mixer& mixer, bool with_music = true);

    [[nodiscard]] static std::vector<std::vector<float>> compose_music(const SoundSpec& spec);
    void add_music(Mixer& mixer, std::vector<std::vector<float>> pieces);

    void play(Mixer& mixer, const SoundCue& cue);
    // Número en el mezclador de la pieza (índice en SoundSpec::music); -1 si no existe.
    [[nodiscard]] std::int32_t piece(std::int32_t index) const noexcept;

private:
    std::vector<std::vector<std::int32_t>> variants_;  // por receta: números en el mezclador
    std::vector<std::size_t> next_;
    std::vector<std::int32_t> pieces_;
};

}  // namespace rts::audio
