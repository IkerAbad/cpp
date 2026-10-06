#include "audio/bank.hpp"

namespace rts::audio {

Bank::Bank(const SoundSpec& spec, Mixer& mixer, bool with_music) {
    for (std::size_t r = 0; r < spec.recipes.size(); ++r) {
        const SoundRecipe& rec = spec.recipes[r];
        std::vector<std::int32_t> ids;
        for (std::int32_t v = 0; v < rec.variants; ++v) {
            // Versiones repartidas alrededor del tono original: -k, ..., 0, ..., +k.
            const std::int32_t cents = (v - (rec.variants - 1) / 2) * rec.variant_cents;
            const auto seed = static_cast<std::uint32_t>(r * 7919 + static_cast<std::size_t>(v) * 104729 + 1);
            ids.push_back(mixer.add(render(rec, spec.config.sample_rate, cents, seed)));
        }
        variants_.push_back(std::move(ids));
    }
    next_.assign(variants_.size(), 0);
    if (with_music) {
        add_music(mixer, compose_music(spec));
    }
}

std::vector<std::vector<float>> Bank::compose_music(const SoundSpec& spec) {
    std::vector<std::vector<float>> out;
    for (const MusicPiece& m : spec.music) {
        out.push_back(compose(m, spec.config.sample_rate));
    }
    return out;
}

void Bank::add_music(Mixer& mixer, std::vector<std::vector<float>> pieces) {
    pieces_.clear();
    for (auto& p : pieces) {
        pieces_.push_back(mixer.add(std::move(p)));
    }
}

void Bank::play(Mixer& mixer, const SoundCue& cue) {
    if (cue.recipe < 0 || static_cast<std::size_t>(cue.recipe) >= variants_.size()) {
        return;
    }
    const auto r = static_cast<std::size_t>(cue.recipe);
    const auto& ids = variants_[r];
    if (ids.empty()) {
        return;
    }
    mixer.play(ids[next_[r] % ids.size()], cue.volume, cue.pan);
    ++next_[r];
}

std::int32_t Bank::piece(std::int32_t index) const noexcept {
    return index >= 0 && static_cast<std::size_t>(index) < pieces_.size() ? pieces_[static_cast<std::size_t>(index)]
                                                                           : -1;
}

}  // namespace rts::audio
