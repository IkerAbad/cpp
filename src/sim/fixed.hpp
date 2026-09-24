#pragma once

// Punto fijo 16.16 con signo sobre int32_t. Rango [-32768, 32768), resolución 1/65536.
//
// Política de desbordamiento: assert en Debug. En Release, la conversión int64 -> int32
// es módulo 2^32 (definido desde C++20), así que un desbordamiento da un resultado
// erróneo pero idéntico en todas las plataformas: el determinismo no se pierde.
//
// Redondeo:
//   mul: hacia -infinito (desplazamiento aritmético, definido desde C++20)
//   div: hacia cero (división entera de C++)
//
// Las magnitudes cuadráticas (distancia al cuadrado en un mapa de 256 casillas llega
// a 131072) no caben en 16.16: usar mul_wide(), que devuelve el producto en 32.32.

#include <cassert>
#include <compare>
#include <cstdint>
#include <limits>

namespace rts::sim {

class Fixed {
public:
    static constexpr int kFracBits = 16;
    static constexpr std::int32_t kOneRaw = std::int32_t{1} << kFracBits;

    constexpr Fixed() noexcept = default;

    [[nodiscard]] static constexpr Fixed from_raw(std::int32_t raw) noexcept {
        Fixed f;
        f.raw_ = raw;
        return f;
    }

    [[nodiscard]] static constexpr Fixed from_int(std::int32_t value) noexcept {
        return from_raw(narrow(std::int64_t{value} * kOneRaw));
    }

    // num/den redondeado hacia cero. Sustituye a los literales decimales: 1.5 -> from_ratio(3, 2).
    [[nodiscard]] static constexpr Fixed from_ratio(std::int32_t num, std::int32_t den) noexcept {
        assert(den != 0);
        return from_raw(narrow((std::int64_t{num} * kOneRaw) / den));
    }

    [[nodiscard]] static constexpr Fixed max() noexcept {
        return from_raw(std::numeric_limits<std::int32_t>::max());
    }
    [[nodiscard]] static constexpr Fixed lowest() noexcept {
        return from_raw(std::numeric_limits<std::int32_t>::min());
    }

    [[nodiscard]] constexpr std::int32_t raw() const noexcept { return raw_; }

    // Parte entera redondeada hacia -infinito: -0.5 -> -1.
    [[nodiscard]] constexpr std::int32_t floor_to_int() const noexcept { return raw_ >> kFracBits; }

    constexpr Fixed operator-() const noexcept { return from_raw(narrow(-std::int64_t{raw_})); }

    friend constexpr Fixed operator+(Fixed a, Fixed b) noexcept {
        return from_raw(narrow(std::int64_t{a.raw_} + b.raw_));
    }
    friend constexpr Fixed operator-(Fixed a, Fixed b) noexcept {
        return from_raw(narrow(std::int64_t{a.raw_} - b.raw_));
    }
    friend constexpr Fixed operator*(Fixed a, Fixed b) noexcept {
        return from_raw(narrow((std::int64_t{a.raw_} * b.raw_) >> kFracBits));
    }
    friend constexpr Fixed operator/(Fixed a, Fixed b) noexcept {
        assert(b.raw_ != 0);
        return from_raw(narrow((std::int64_t{a.raw_} * kOneRaw) / b.raw_));
    }
    friend constexpr Fixed operator*(Fixed a, std::int32_t k) noexcept {
        return from_raw(narrow(std::int64_t{a.raw_} * k));
    }

    constexpr Fixed& operator+=(Fixed o) noexcept { return *this = *this + o; }
    constexpr Fixed& operator-=(Fixed o) noexcept { return *this = *this - o; }
    constexpr Fixed& operator*=(Fixed o) noexcept { return *this = *this * o; }
    constexpr Fixed& operator/=(Fixed o) noexcept { return *this = *this / o; }

    friend constexpr auto operator<=>(Fixed, Fixed) noexcept = default;
    friend constexpr bool operator==(Fixed, Fixed) noexcept = default;

private:
    [[nodiscard]] static constexpr std::int32_t narrow(std::int64_t v) noexcept {
        assert(v >= std::numeric_limits<std::int32_t>::min() && v <= std::numeric_limits<std::int32_t>::max());
        return static_cast<std::int32_t>(v);
    }

    std::int32_t raw_ = 0;
};

// Producto exacto en 32.32 (int64). Para distancias al cuadrado y comparaciones de alcance.
[[nodiscard]] constexpr std::int64_t mul_wide(Fixed a, Fixed b) noexcept {
    return std::int64_t{a.raw()} * b.raw();
}

}  // namespace rts::sim
