#pragma once

// Generadores deterministas de la simulación. Solo enteros sin signo de 64 bits, con
// aritmética módulo 2^64 definida por el estándar: salida idéntica en toda plataforma.
//
// xoshiro256++ y splitmix64: algoritmos de Blackman y Vigna (dominio público),
// https://prng.di.unimi.it/ . splitmix64 se usa solo para expandir una semilla de
// 64 bits a los 256 bits de estado, que es lo que recomiendan los autores.

#include <array>
#include <bit>
#include <cassert>
#include <cstdint>

namespace rts::sim {

class SplitMix64 {
public:
    explicit constexpr SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

    constexpr std::uint64_t next() noexcept {
        std::uint64_t z = (state_ += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }

private:
    std::uint64_t state_;
};

class Xoshiro256pp {
public:
    using State = std::array<std::uint64_t, 4>;

    explicit constexpr Xoshiro256pp(std::uint64_t seed) noexcept {
        SplitMix64 sm(seed);
        for (auto& word : s_) {
            word = sm.next();
        }
    }

    // Estado explícito, para restaurar partidas y para los vectores de referencia.
    // El estado todo a cero es inválido: el generador devolvería ceros para siempre.
    [[nodiscard]] static constexpr Xoshiro256pp from_state(const State& state) noexcept {
        assert(state[0] != 0 || state[1] != 0 || state[2] != 0 || state[3] != 0);
        Xoshiro256pp rng(0);
        rng.s_ = state;
        return rng;
    }

    constexpr std::uint64_t next() noexcept {
        const std::uint64_t result = std::rotl(s_[0] + s_[3], 23) + s_[0];
        const std::uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = std::rotl(s_[3], 45);
        return result;
    }

    // Entero uniforme en [0, bound). Método de Lemire con multiplicación 32x32->64:
    // sin sesgo y sin enteros de 128 bits, que MSVC no tiene.
    constexpr std::uint32_t next_below(std::uint32_t bound) noexcept {
        assert(bound > 0);
        // Los 32 bits altos son los de mejor calidad en xoshiro256++.
        std::uint64_t m = (next() >> 32) * std::uint64_t{bound};
        auto low = static_cast<std::uint32_t>(m);
        if (low < bound) {
            const std::uint32_t threshold = (0u - bound) % bound;
            while (low < threshold) {
                m = (next() >> 32) * std::uint64_t{bound};
                low = static_cast<std::uint32_t>(m);
            }
        }
        return static_cast<std::uint32_t>(m >> 32);
    }

    // Entero uniforme en [lo, hi], ambos incluidos. Requiere hi - lo < 2^32 - 1.
    constexpr std::int32_t next_in_range(std::int32_t lo, std::int32_t hi) noexcept {
        assert(lo <= hi);
        const auto span = static_cast<std::uint32_t>(std::int64_t{hi} - lo + 1);
        return static_cast<std::int32_t>(std::int64_t{lo} + next_below(span));
    }

    [[nodiscard]] constexpr const State& state() const noexcept { return s_; }

private:
    State s_{};
};

}  // namespace rts::sim
