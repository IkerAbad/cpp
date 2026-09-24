#pragma once

// Aritmética vectorial determinista en punto fijo. La raíz cuadrada es entera
// (isqrt): mismo resultado bit a bit en cualquier compilador y CPU.

#include <bit>
#include <cassert>
#include <cstdint>

#include "sim/fixed.hpp"

namespace rts::sim {

// Raíz cuadrada entera por defecto: el mayor r con r*r <= n. Newton sobre enteros con
// estimación inicial por el número de bits: converge en <= 6 iteraciones para 64 bits.
[[nodiscard]] constexpr std::uint64_t isqrt(std::uint64_t n) noexcept {
    if (n < 2) {
        return n;
    }
    // 2^ceil(bits/2) >= sqrt(n): punto de partida por encima, Newton baja monótono.
    std::uint64_t x = std::uint64_t{1} << ((static_cast<unsigned>(std::bit_width(n)) + 1) / 2);
    while (true) {
        const std::uint64_t y = (x + n / x) / 2;
        if (y >= x) {
            return x;
        }
        x = y;
    }
}

struct FVec2 {
    Fixed x;
    Fixed y;

    friend constexpr FVec2 operator+(FVec2 a, FVec2 b) noexcept { return {a.x + b.x, a.y + b.y}; }
    friend constexpr FVec2 operator-(FVec2 a, FVec2 b) noexcept { return {a.x - b.x, a.y - b.y}; }
    friend constexpr FVec2 operator*(FVec2 a, Fixed k) noexcept { return {a.x * k, a.y * k}; }
    friend constexpr FVec2 operator*(FVec2 a, std::int32_t k) noexcept { return {a.x * k, a.y * k}; }
    friend constexpr bool operator==(FVec2, FVec2) noexcept = default;
};

// Producto escalar y longitud al cuadrado en 32.32 (int64): no desbordan 16.16.
[[nodiscard]] constexpr std::int64_t dot_wide(FVec2 a, FVec2 b) noexcept {
    return mul_wide(a.x, b.x) + mul_wide(a.y, b.y);
}

[[nodiscard]] constexpr std::int64_t length_sq_wide(FVec2 v) noexcept {
    return dot_wide(v, v);
}

// Longitud en 16.16: sqrt de un valor 32.32 da directamente la raíz en 16.16.
[[nodiscard]] constexpr Fixed length(FVec2 v) noexcept {
    const std::int64_t sq = length_sq_wide(v);
    assert(sq >= 0);
    return Fixed::from_raw(static_cast<std::int32_t>(isqrt(static_cast<std::uint64_t>(sq))));
}

// Reescala v a la longitud pedida. Un vector nulo se queda nulo.
[[nodiscard]] constexpr FVec2 with_length(FVec2 v, Fixed target) noexcept {
    const Fixed len = length(v);
    if (len.raw() == 0) {
        return {};
    }
    return {Fixed::from_raw(static_cast<std::int32_t>(std::int64_t{v.x.raw()} * target.raw() / len.raw())),
            Fixed::from_raw(static_cast<std::int32_t>(std::int64_t{v.y.raw()} * target.raw() / len.raw()))};
}

// Limita la longitud de v a max_len sin cambiar su dirección.
[[nodiscard]] constexpr FVec2 clamp_length(FVec2 v, Fixed max_len) noexcept {
    const std::int64_t max_sq = mul_wide(max_len, max_len);
    return length_sq_wide(v) > max_sq ? with_length(v, max_len) : v;
}

}  // namespace rts::sim
