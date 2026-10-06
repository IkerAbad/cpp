#include "game/wire.hpp"

namespace rts::game {

namespace {

constexpr std::uint8_t kLastCommandType = static_cast<std::uint8_t>(sim::CommandType::Count) - 1;
constexpr unsigned kByteBits = 8;
constexpr unsigned kByteMask = 0xffU;

}  // namespace

void ByteWriter::le(std::uint64_t v, unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
        out_.push_back(static_cast<std::uint8_t>((v >> (kByteBits * i)) & kByteMask));
    }
}

std::size_t ByteReader::count(std::size_t min_bytes) {
    const std::size_t n = u32();
    if (n > remaining() / min_bytes) {
        ok_ = false;
        return 0;
    }
    return n;
}

std::string ByteReader::str() {
    const std::size_t n = count(1);
    std::string s(reinterpret_cast<const char*>(in_.data() + pos_), n);
    pos_ += n;
    return s;
}

std::uint64_t ByteReader::le(unsigned n) {
    if (!ok_ || remaining() < n) {
        ok_ = false;
        return 0;
    }
    std::uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i) {
        v |= static_cast<std::uint64_t>(in_[pos_ + i]) << (kByteBits * i);
    }
    pos_ += n;
    return v;
}

void write_command(ByteWriter& w, const sim::Command& c) {
    w.u32(c.tick);
    w.u8(c.player);
    w.u8(static_cast<std::uint8_t>(c.type));
    w.count(c.units.size());
    for (const std::uint32_t id : c.units) {
        w.u32(id);
    }
    w.i32(c.target.x);
    w.i32(c.target.y);
    w.u32(c.object);
    w.u8(c.kind);
}

bool read_command(ByteReader& r, sim::Command& c) {
    c.tick = r.u32();
    c.player = r.u8();
    const std::uint8_t type = r.u8();
    if (type > kLastCommandType) {
        return false;
    }
    c.type = static_cast<sim::CommandType>(type);
    c.units.resize(r.count(sizeof(std::uint32_t)));
    for (std::uint32_t& id : c.units) {
        id = r.u32();
    }
    c.target.x = r.i32();
    c.target.y = r.i32();
    c.object = r.u32();
    c.kind = r.u8();
    return r.ok();
}

}  // namespace rts::game
