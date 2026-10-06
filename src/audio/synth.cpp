#include "audio/synth.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace rts::audio {

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr double kMsPerS = 1000.0;
constexpr double kCentsPerOctave = 1200.0;
constexpr double kSemitonesPerOctave = 12.0;
constexpr double kPercent = 100.0;
// La caída exponencial llega a -60 dB (una milésima) al final de decay_ms.
constexpr double kDecayToSilence = 6.907755;  // ln(1000)

// Seno por tabla con interpolación lineal: unas veinte veces más barato que std::sin y
// con un error por debajo de 1e-6 con 4096 puntos (inaudible).
class SineTable {
public:
    SineTable() {
        for (std::size_t i = 0; i <= kSize; ++i) {
            table_[i] = std::sin(kTwoPi * static_cast<double>(i) / static_cast<double>(kSize));
        }
    }
    // phase en ciclos (cualquier valor; se toma la parte fraccionaria).
    [[nodiscard]] double at(double phase) const noexcept {
        const double f = (phase - std::floor(phase)) * static_cast<double>(kSize);
        const auto i = static_cast<std::size_t>(f);
        const double t = f - static_cast<double>(i);
        return table_[i] + (table_[i + 1] - table_[i]) * t;
    }

private:
    static constexpr std::size_t kSize = 4096;
    std::array<double, kSize + 1> table_{};
};

const SineTable& sine() {
    static const SineTable table;
    return table;
}

// Generador determinista (xorshift32) para ruido y tiempos.
class Noise {
public:
    explicit Noise(std::uint32_t seed) : s_(seed == 0 ? 0x9E3779B9U : seed) {}
    std::uint32_t next() {
        s_ ^= s_ << 13U;
        s_ ^= s_ >> 17U;
        s_ ^= s_ << 5U;
        return s_;
    }
    double unit() { return static_cast<double>(next()) / 4294967296.0; }  // [0, 1)
    double signed_unit() { return unit() * 2.0 - 1.0; }                  // [-1, 1)

private:
    std::uint32_t s_;
};

double wave_at(Wave w, double phase, Noise& noise) {
    const double f = phase - std::floor(phase);
    switch (w) {
        case Wave::Sine:
            return sine().at(f);
        case Wave::Triangle:
            return f < 0.5 ? 4.0 * f - 1.0 : 3.0 - 4.0 * f;
        case Wave::Square:
            return f < 0.5 ? 1.0 : -1.0;
        case Wave::Saw:
            return 2.0 * f - 1.0;
        case Wave::Noise:
            return noise.signed_unit();
    }
    return 0.0;
}

// Coeficiente de un filtro de un polo para la frecuencia de corte dada.
double one_pole(double cutoff_hz, double sample_rate) {
    return 1.0 - std::exp(-kTwoPi * cutoff_hz / sample_rate);
}

double cents_ratio(double cents) { return std::pow(2.0, cents / kCentsPerOctave); }

// 2^(c/1200) para desviaciones pequeñas (vibrato de unos pocos cents): 1 + c·ln2/1200.
// Con 15 cents el error es de 0,01 cents, inaudible, y no hay una potencia por muestra.
double vibrato_ratio(double cents) {
    constexpr double kLn2 = 0.6931471805599453;
    return 1.0 + cents * kLn2 / kCentsPerOctave;
}

// Recorte suave: nunca pasa de ±1 y casi no toca lo que está por debajo. Cúbica
// (x - 4x³/27 hasta |x| = 1,5, luego ±1): continua y derivable, sin tanh por muestra.
float soft_clip(double v) {
    constexpr double kKnee = 1.5;
    constexpr double kCubic = 4.0 / 27.0;
    if (v >= kKnee) {
        return 1.0f;
    }
    if (v <= -kKnee) {
        return -1.0f;
    }
    return static_cast<float>(v - kCubic * v * v * v);
}

// Una nota de una capa en out a partir de start (muestras).
void render_burst(const Layer& l, double sample_rate, double pitch, std::size_t start, std::vector<double>& out,
                  Noise& noise) {
    const double attack = std::max(1.0, l.attack_ms * sample_rate / kMsPerS);
    const double decay = std::max(1.0, l.decay_ms * sample_rate / kMsPerS);
    const auto length = static_cast<std::size_t>(attack + decay);
    const double f0 = std::max(1, l.freq_start_hz) * pitch;
    const double f1 = std::max(1, l.freq_end_hz) * pitch;
    const double lp = l.lowpass_hz > 0 ? one_pole(l.lowpass_hz, sample_rate) : 0.0;
    const double hp = l.highpass_hz > 0 ? one_pole(l.highpass_hz, sample_rate) : 0.0;
    const double volume = l.volume_percent / kPercent;
    double phase = 0.0;
    double low = 0.0;
    double high_state = 0.0;
    // Deslizamiento exponencial del tono (como lo oye el oído): un factor por muestra.
    const double glide = std::pow(f1 / f0, 1.0 / static_cast<double>(std::max<std::size_t>(length, 1)));
    double base = f0;
    // Envolvente: la caída exponencial también es un factor por muestra.
    const double fall = std::exp(-kDecayToSilence / decay);
    double env_decay = 1.0;
    for (std::size_t i = 0; i < length && start + i < out.size(); ++i) {
        double f = base;
        base *= glide;
        if (l.vibrato_hz > 0) {
            f *= vibrato_ratio(l.vibrato_cents * sine().at(l.vibrato_hz * static_cast<double>(i) / sample_rate));
        }
        phase += f / sample_rate;
        double x = wave_at(l.wave, phase, noise);
        if (lp > 0.0) {
            low += lp * (x - low);
            x = low;
        }
        if (hp > 0.0) {
            high_state += hp * (x - high_state);
            x -= high_state;
        }
        const double fi = static_cast<double>(i);
        double env = 0.0;
        if (fi < attack) {
            env = fi / attack;
        } else {
            env = env_decay;
            env_decay *= fall;
        }
        out[start + i] += x * env * volume;
    }
}

}  // namespace

std::vector<float> render(const SoundRecipe& recipe, std::int32_t sample_rate, std::int32_t cents, std::uint32_t seed) {
    const auto sr = static_cast<double>(sample_rate);
    double length_ms = 0.0;
    for (const Layer& l : recipe.layers) {
        const double span = l.delay_ms + l.attack_ms + l.decay_ms + std::max(l.repeats - 1, 0) * l.repeat_ms * 1.4;
        length_ms = std::max(length_ms, span);
    }
    std::vector<double> mix(static_cast<std::size_t>(length_ms * sr / kMsPerS) + 1, 0.0);
    Noise noise(seed);
    const double pitch = cents_ratio(cents);
    for (const Layer& l : recipe.layers) {
        double at_ms = l.delay_ms;
        for (std::int32_t r = 0; r < std::max(l.repeats, 1); ++r) {
            render_burst(l, sr, pitch, static_cast<std::size_t>(at_ms * sr / kMsPerS), mix, noise);
            // Separación irregular entre golpes: entre el 60 % y el 140 % de la media.
            at_ms += l.repeat_ms * (0.6 + 0.8 * noise.unit());
        }
    }
    const double volume = recipe.volume_percent / kPercent;
    std::vector<float> out(mix.size());
    for (std::size_t i = 0; i < mix.size(); ++i) {
        out[i] = soft_clip(mix[i] * volume);
    }
    return out;
}

// ---------------------------------------------------------------------------------
// Música.

namespace {

// Figuras de un compás en corcheas: se elige una por compás.
const std::vector<std::vector<std::int32_t>>& rhythms(std::int32_t eighths) {
    static const std::vector<std::vector<std::int32_t>> kFour{{2, 2, 2, 2}, {1, 1, 2, 2, 2}, {3, 1, 2, 2},
                                                              {2, 2, 4},    {4, 2, 2},       {1, 1, 1, 1, 2, 2},
                                                              {2, 1, 1, 4}};
    static const std::vector<std::vector<std::int32_t>> kThree{{2, 2, 2}, {3, 1, 2}, {2, 4}, {1, 1, 2, 2}, {4, 2}};
    static const std::vector<std::vector<std::int32_t>> kAny{{2}};
    if (eighths == 8) {
        return kFour;
    }
    if (eighths == 6) {
        return kThree;
    }
    return kAny;
}

struct Note {
    std::int32_t degree = 0;  // grado de la escala (0 = tónica), puede pasar de la octava
    std::int32_t eighths = 0;
};

// Una frase de cuatro compases: paseo por la escala que vuelve a la tónica al final.
std::vector<std::vector<Note>> phrase(const MusicPiece& piece, Noise& rnd, bool closing) {
    const std::int32_t eighths = piece.beats_per_bar * 2;
    constexpr std::int32_t kBars = 4;
    constexpr std::int32_t kTop = 9;  // grado más alto: algo más de una octava
    std::vector<std::vector<Note>> bars;
    std::int32_t degree = static_cast<std::int32_t>(rnd.next() % 5U);
    for (std::int32_t b = 0; b < kBars; ++b) {
        const auto& options = rhythms(eighths);
        std::vector<std::int32_t> pattern = options[rnd.next() % options.size()];
        if (b == kBars - 1) {
            pattern = {eighths};  // nota larga al cerrar la frase
        }
        std::vector<Note> bar;
        for (std::size_t i = 0; i < pattern.size(); ++i) {
            if (b == kBars - 1) {
                degree = closing ? 0 : 4;  // cadencia: tónica o quinta (semicadencia)
            } else {
                // Pasos cortos y algún salto, sin salirse del ámbito.
                static constexpr std::array<std::int32_t, 9> kSteps{-2, -1, -1, 0, 1, 1, 2, 3, -3};
                degree = std::clamp(degree + kSteps[rnd.next() % kSteps.size()], 0, kTop);
            }
            bar.push_back({degree, pattern[i]});
        }
        bars.push_back(std::move(bar));
    }
    return bars;
}

}  // namespace

std::vector<float> compose(const MusicPiece& piece, std::int32_t sample_rate) {
    const auto sr = static_cast<double>(sample_rate);
    const double eighth_s = 60.0 / std::max(piece.tempo_bpm, 1) / 2.0;
    const std::int32_t eighths_per_bar = piece.beats_per_bar * 2;
    const double length_s = eighth_s * eighths_per_bar * piece.bars;
    const auto total = static_cast<std::size_t>(length_s * sr);
    std::vector<double> mix(total, 0.0);
    Noise rnd(piece.seed);
    const auto mode_size = static_cast<std::int32_t>(piece.mode.size());
    const auto degree_hz = [&](std::int32_t degree, double octave_up) {
        const std::int32_t step = piece.mode[static_cast<std::size_t>(degree % mode_size)];
        const double semis = step + kSemitonesPerOctave * (degree / mode_size) + kSemitonesPerOctave * octave_up;
        return piece.root_hz * std::pow(2.0, semis / kSemitonesPerOctave);
    };

    // Forma A A B A con frases de cuatro compases; la última A cierra en la tónica.
    const auto a = phrase(piece, rnd, true);
    const auto a_open = phrase(piece, rnd, false);
    const auto b = phrase(piece, rnd, false);
    std::vector<std::vector<Note>> score;
    const std::array<const std::vector<std::vector<Note>>*, 4> form{&a_open, &a, &b, &a};
    for (std::size_t f = 0; static_cast<std::int32_t>(score.size()) < piece.bars; ++f) {
        for (const auto& bar : *form[f % form.size()]) {
            if (static_cast<std::int32_t>(score.size()) < piece.bars) {
                score.push_back(bar);
            }
        }
    }

    // Melodía: flauta (seno con algo de segundo y tercer armónico, vibrato que entra tarde).
    constexpr double kAttackS = 0.03;
    constexpr double kReleaseS = 0.06;
    constexpr double kVibratoHz = 5.0;
    constexpr double kVibratoCents = 14.0;
    constexpr double kVibratoDelayS = 0.12;
    constexpr double kBreath = 0.04;
    const double melody = piece.melody_percent / kPercent;
    double t0 = 0.0;
    for (const auto& bar : score) {
        for (const Note& n : bar) {
            const double dur = n.eighths * eighth_s;
            const double f = degree_hz(n.degree, 1.0);
            const auto start = static_cast<std::size_t>(t0 * sr);
            const auto len = static_cast<std::size_t>(dur * sr);
            double phase = 0.0;
            double breath = 0.0;
            for (std::size_t i = 0; i < len && start + i < total; ++i) {
                const double t = static_cast<double>(i) / sr;
                const double vib = t > kVibratoDelayS ? kVibratoCents * sine().at(kVibratoHz * t) : 0.0;
                phase += f * vibrato_ratio(vib) / sr;
                const double env = std::min({1.0, t / kAttackS, (dur - t) / kReleaseS});
                breath += 0.2 * (rnd.signed_unit() - breath);
                const double tone = sine().at(phase) + 0.3 * sine().at(2.0 * phase) + 0.1 * sine().at(3.0 * phase) +
                                    kBreath * breath;
                mix[start + i] += tone * std::max(env, 0.0) * melody;
            }
            t0 += dur;
        }
    }

    // Bordón de tónica y quinta, con frecuencias ajustadas para dar ciclos enteros en la
    // vuelta: al repetir, no hay salto.
    const double drone = piece.drone_percent / kPercent;
    const double lp = one_pole(800.0, sr);
    for (const double ratio : {1.0, 1.5}) {
        const double f = std::round(piece.root_hz * ratio / 2.0 * length_s) / length_s;
        double low = 0.0;
        for (std::size_t i = 0; i < total; ++i) {
            const double t = static_cast<double>(i) / sr;
            const double saw = 2.0 * (f * t - std::floor(f * t)) - 1.0;
            low += lp * (saw - low);
            mix[i] += low * drone * (ratio == 1.0 ? 1.0 : 0.7);
        }
    }

    // Tamboril: golpe grave (seno que cae) con piel (ruido filtrado). Siempre en el
    // primer tiempo; en los demás, según la densidad.
    const double drum = piece.drum_percent / kPercent;
    const double skin_lp = one_pole(300.0, sr);
    constexpr double kDrumS = 0.18;
    constexpr double kThumpHz = 90.0;
    for (std::int32_t bar = 0; bar < piece.bars; ++bar) {
        for (std::int32_t beat = 0; beat < piece.beats_per_bar; ++beat) {
            const bool strong = beat == 0;
            if (!strong && rnd.unit() * kPercent >= piece.drum_density_percent) {
                continue;
            }
            const double at = (static_cast<double>(bar) * eighths_per_bar + beat * 2.0) * eighth_s;
            const auto start = static_cast<std::size_t>(at * sr);
            const double accent = strong ? 1.0 : 0.6;
            double phase = 0.0;
            double skin = 0.0;
            for (std::size_t i = 0; i < static_cast<std::size_t>(kDrumS * sr) && start + i < total; ++i) {
                const double t = static_cast<double>(i) / sr;
                phase += kThumpHz * (1.0 - t * 2.0) / sr;
                skin += skin_lp * (rnd.signed_unit() - skin);
                const double env = std::exp(-kDecayToSilence * t / kDrumS);
                mix[start + i] += (sine().at(phase) + 2.0 * skin) * env * drum * accent;
            }
        }
    }

    std::vector<float> out(total);
    for (std::size_t i = 0; i < total; ++i) {
        out[i] = soft_clip(mix[i]);
    }
    return out;
}

}  // namespace rts::audio
