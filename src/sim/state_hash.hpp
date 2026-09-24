#pragma once

// Hash del estado de la simulación, FNV-1a de 64 bits. Cada valor se descompone en
// bytes en orden little-endian explícito, así que el resultado no depende de la
// endianidad ni del relleno de las estructuras. No es criptográfico: sirve para
// detectar divergencias entre repeticiones, plataformas y clientes lockstep.

#include <cstdint>

#include "sim/fixed.hpp"

namespace rts::sim {

class StateHasher {
public:
    constexpr void add_u64(std::uint64_t v) noexcept {
        for (int i = 0; i < 8; ++i) {
            hash_ ^= (v >> (8 * i)) & 0xffU;
            hash_ *= kPrime;
        }
    }
    constexpr void add_u32(std::uint32_t v) noexcept { add_u64(v); }
    constexpr void add_i32(std::int32_t v) noexcept { add_u64(static_cast<std::uint32_t>(v)); }
    constexpr void add_fixed(Fixed v) noexcept { add_i32(v.raw()); }

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return hash_; }

private:
    static constexpr std::uint64_t kOffsetBasis = 0xcbf29ce484222325ULL;
    static constexpr std::uint64_t kPrime = 0x100000001b3ULL;

    std::uint64_t hash_ = kOffsetBasis;
};

}  // namespace rts::sim
