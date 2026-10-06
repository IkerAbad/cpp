#include "audio/mixer.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace rts::audio {

std::int32_t Mixer::add(std::vector<float> samples) {
    sounds_.push_back(std::move(samples));
    return static_cast<std::int32_t>(sounds_.size() - 1);
}

void Mixer::play(std::int32_t sound, float volume, float pan) {
    if (sound < 0 || static_cast<std::size_t>(sound) >= sounds_.size() || volume <= 0.0f) {
        return;
    }
    // Panorama de igual potencia: el volumen total no cambia al moverse.
    const float angle = (std::clamp(pan, -1.0f, 1.0f) + 1.0f) * std::numbers::pi_v<float> * 0.25f;
    const Voice v{sound, 0, volume * std::cos(angle), volume * std::sin(angle)};
    if (voices_.size() < max_voices_) {
        voices_.push_back(v);
        return;
    }
    const auto weakest =
        std::ranges::min_element(voices_, {}, [](const Voice& w) { return w.left + w.right; });
    if (weakest != voices_.end() && weakest->left + weakest->right < v.left + v.right) {
        *weakest = v;
    }
}

void Mixer::set_music(std::int32_t sound, std::int32_t fade_frames) {
    if (sound == music_.sound) {
        return;
    }
    fading_ = music_;
    music_ = {sound, 0};
    fade_total_ = std::max(fade_frames, 1);
    fade_left_ = fade_total_;
}

void Mixer::mix(std::span<float> out) {
    std::ranges::fill(out, 0.0f);
    const std::size_t frames = out.size() / 2;
    for (Voice& v : voices_) {
        const std::vector<float>& s = sounds_[static_cast<std::size_t>(v.sound)];
        const std::size_t n = std::min(frames, s.size() - v.pos);
        for (std::size_t i = 0; i < n; ++i) {
            const float x = s[v.pos + i] * effects_;
            out[i * 2] += x * v.left;
            out[i * 2 + 1] += x * v.right;
        }
        v.pos += n;
    }
    std::erase_if(voices_, [this](const Voice& v) { return v.pos >= sounds_[static_cast<std::size_t>(v.sound)].size(); });

    const auto add_track = [&](Track& t, std::size_t i, float gain) {
        if (t.sound < 0) {
            return;
        }
        const std::vector<float>& s = sounds_[static_cast<std::size_t>(t.sound)];
        if (s.empty()) {
            return;
        }
        const float x = s[t.pos] * gain;
        out[i * 2] += x;
        out[i * 2 + 1] += x;
        t.pos = (t.pos + 1) % s.size();
    };
    for (std::size_t i = 0; i < frames; ++i) {
        float in = 1.0f;
        float gone = 0.0f;
        if (fade_left_ > 0) {
            gone = static_cast<float>(fade_left_) / static_cast<float>(fade_total_);
            in = 1.0f - gone;
            --fade_left_;
        }
        add_track(music_, i, music_gain_ * in);
        if (gone > 0.0f) {
            add_track(fading_, i, music_gain_ * gone);
        }
    }
    for (float& x : out) {
        x = std::clamp(x * master_, -1.0f, 1.0f);
    }
}

}  // namespace rts::audio
