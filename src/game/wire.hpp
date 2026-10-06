#pragma once

// Codificación binaria compartida por las repeticiones (M5) y la red (E1): enteros en
// little-endian explícito, sin depender del orden de bytes de la máquina.
// Una orden: u32 tick, u8 jugador, u8 tipo, u32 n + n x u32 unidades, i32 x, i32 y,
// u32 objeto, u8 kind. Un str: u32 longitud + bytes.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sim/units.hpp"

namespace rts::game {

class ByteWriter {
public:
    void u8(std::uint8_t v) { out_.push_back(v); }
    void u32(std::uint32_t v) { le(v, sizeof(std::uint32_t)); }
    void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
    void u64(std::uint64_t v) { le(v, sizeof(std::uint64_t)); }
    void count(std::size_t n) { u32(static_cast<std::uint32_t>(n)); }
    void str(std::string_view s) {
        count(s.size());
        out_.insert(out_.end(), s.begin(), s.end());
    }
    void bytes(std::span<const std::uint8_t> b) { out_.insert(out_.end(), b.begin(), b.end()); }
    [[nodiscard]] std::vector<std::uint8_t>& out() noexcept { return out_; }

private:
    void le(std::uint64_t v, unsigned n);

    std::vector<std::uint8_t> out_;
};

// Lector con límites: cualquier lectura fuera del búfer marca el error y devuelve 0.
class ByteReader {
public:
    explicit ByteReader(std::span<const std::uint8_t> in) : in_(in) {}

    std::uint8_t u8() { return static_cast<std::uint8_t>(le(1)); }
    std::uint32_t u32() { return static_cast<std::uint32_t>(le(sizeof(std::uint32_t))); }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    std::uint64_t u64() { return le(sizeof(std::uint64_t)); }
    // Número de elementos de al menos min_bytes cada uno: no puede superar lo que queda.
    std::size_t count(std::size_t min_bytes);
    std::string str();
    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return in_.size() - pos_; }

private:
    std::uint64_t le(unsigned n);

    std::span<const std::uint8_t> in_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

void write_command(ByteWriter& w, const sim::Command& c);
// Falso si los bytes no forman una orden válida (truncada o de un tipo desconocido).
bool read_command(ByteReader& r, sim::Command& c);

}  // namespace rts::game
