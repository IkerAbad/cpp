#pragma once

// Salida de audio (F2) con SDL3. Sin hilos propios: el bucle principal llama a pump()
// cada fotograma y se mezcla lo justo para mantener latency de sonido en cola.

#include <cstdint>
#include <optional>
#include <vector>

#include "audio/mixer.hpp"

struct SDL_AudioStream;

namespace rts::platform {

class AudioOut {
public:
    // Abre el dispositivo por defecto (estéreo, coma flotante). nullopt si no hay audio
    // (máquinas sin tarjeta, CI): el juego sigue en silencio.
    static std::optional<AudioOut> open(std::int32_t sample_rate, std::int32_t latency_ms);

    AudioOut(AudioOut&& o) noexcept;
    AudioOut& operator=(AudioOut&&) = delete;
    AudioOut(const AudioOut&) = delete;
    AudioOut& operator=(const AudioOut&) = delete;
    ~AudioOut();

    void pump(audio::Mixer& mixer);

private:
    AudioOut(SDL_AudioStream* stream, std::int32_t target_frames) : stream_(stream), target_frames_(target_frames) {}

    SDL_AudioStream* stream_ = nullptr;
    std::int32_t target_frames_ = 0;
    std::vector<float> buffer_;
};

}  // namespace rts::platform
