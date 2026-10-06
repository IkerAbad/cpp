#pragma once

// Mezclador (F2): voces de efectos con volumen y panorama, y una pista de música que se
// repite y funde al cambiar. Puro: lo llama el hilo principal, que entrega lo mezclado
// a la salida de audio de la plataforma.

#include <cstdint>
#include <span>
#include <vector>

namespace rts::audio {

class Mixer {
public:
    explicit Mixer(std::int32_t max_voices) : max_voices_(static_cast<std::size_t>(max_voices)) {}

    // Guarda unas muestras (mono) y devuelve su número para play() o set_music().
    std::int32_t add(std::vector<float> samples);

    // Suena una vez. volume 0..1; pan -1 (izquierda) .. 1 (derecha). Si no caben más
    // voces, sustituye a la más débil si esta suena más; si no, se descarta.
    void play(std::int32_t sound, float volume, float pan);

    // Música en bucle; -1 la calla. El cambio funde en fade_frames.
    void set_music(std::int32_t sound, std::int32_t fade_frames);
    [[nodiscard]] std::int32_t music() const noexcept { return music_.sound; }

    void set_gains(float master, float effects, float music) noexcept {
        master_ = master;
        effects_ = effects;
        music_gain_ = music;
    }

    // Mezcla out.size() / 2 tramas estéreo intercaladas (izquierda, derecha).
    void mix(std::span<float> out);

    [[nodiscard]] std::size_t active_voices() const noexcept { return voices_.size(); }

private:
    struct Voice {
        std::int32_t sound = 0;
        std::size_t pos = 0;
        float left = 0.0f;
        float right = 0.0f;
    };
    struct Track {
        std::int32_t sound = -1;
        std::size_t pos = 0;
    };

    std::size_t max_voices_;
    std::vector<std::vector<float>> sounds_;
    std::vector<Voice> voices_;
    Track music_;
    Track fading_;          // la pista que se va
    std::int32_t fade_total_ = 0;
    std::int32_t fade_left_ = 0;
    float master_ = 1.0f;
    float effects_ = 1.0f;
    float music_gain_ = 1.0f;
};

}  // namespace rts::audio
