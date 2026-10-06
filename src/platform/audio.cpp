#include "platform/audio.hpp"

#include <SDL3/SDL.h>

namespace rts::platform {

namespace {
constexpr int kChannels = 2;
constexpr std::int32_t kMsPerSecond = 1000;
}  // namespace

std::optional<AudioOut> AudioOut::open(std::int32_t sample_rate, std::int32_t latency_ms) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        return std::nullopt;
    }
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = kChannels;
    spec.freq = sample_rate;
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (stream == nullptr) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return std::nullopt;
    }
    SDL_ResumeAudioStreamDevice(stream);
    return AudioOut(stream, sample_rate * latency_ms / kMsPerSecond);
}

AudioOut::AudioOut(AudioOut&& o) noexcept
    : stream_(o.stream_), target_frames_(o.target_frames_), buffer_(std::move(o.buffer_)) {
    o.stream_ = nullptr;
}

AudioOut::~AudioOut() {
    if (stream_ != nullptr) {
        SDL_DestroyAudioStream(stream_);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
}

void AudioOut::pump(audio::Mixer& mixer) {
    if (stream_ == nullptr) {
        return;
    }
    constexpr int kFrameBytes = static_cast<int>(sizeof(float)) * kChannels;
    const int queued = SDL_GetAudioStreamQueued(stream_) / kFrameBytes;
    const int missing = target_frames_ - queued;
    if (missing <= 0) {
        return;
    }
    buffer_.resize(static_cast<std::size_t>(missing) * kChannels);
    mixer.mix(buffer_);
    SDL_PutAudioStreamData(stream_, buffer_.data(), missing * kFrameBytes);
}

}  // namespace rts::platform
