#include "game/replay.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <iterator>
#include <utility>

#include "sim/state_hash.hpp"

namespace rts::game {

namespace {

constexpr std::array<std::uint8_t, 8> kMagic{'R', 'T', 'S', 'R', 'E', 'P', 0, 0};
constexpr std::uint8_t kLastCommandType = static_cast<std::uint8_t>(sim::CommandType::Count) - 1;
constexpr std::size_t kChecksumBytes = 8;
constexpr unsigned kByteBits = 8;
constexpr unsigned kByteMask = 0xffU;

std::uint64_t fnv1a(std::span<const std::uint8_t> bytes) {
    sim::StateHasher h;
    h.add_bytes(bytes);
    return h.value();
}

class Writer {
public:
    void u8(std::uint8_t v) { out_.push_back(v); }
    void u32(std::uint32_t v) { le(v, 4); }
    void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
    void u64(std::uint64_t v) { le(v, 8); }
    void count(std::size_t n) { u32(static_cast<std::uint32_t>(n)); }
    void str(std::string_view s) {
        count(s.size());
        out_.insert(out_.end(), s.begin(), s.end());
    }
    void bytes(std::span<const std::uint8_t> b) { out_.insert(out_.end(), b.begin(), b.end()); }
    [[nodiscard]] std::vector<std::uint8_t>& out() noexcept { return out_; }

private:
    void le(std::uint64_t v, unsigned n) {
        for (unsigned i = 0; i < n; ++i) {
            out_.push_back(static_cast<std::uint8_t>((v >> (kByteBits * i)) & kByteMask));
        }
    }

    std::vector<std::uint8_t> out_;
};

// Lector con límites: cualquier lectura fuera del búfer marca el error y devuelve 0.
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> in) : in_(in) {}

    std::uint8_t u8() { return static_cast<std::uint8_t>(le(1)); }
    std::uint32_t u32() { return static_cast<std::uint32_t>(le(4)); }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    std::uint64_t u64() { return le(8); }
    // Número de elementos de al menos min_bytes cada uno: no puede superar lo que queda.
    std::size_t count(std::size_t min_bytes) {
        const std::size_t n = u32();
        if (n > remaining() / min_bytes) {
            ok_ = false;
            return 0;
        }
        return n;
    }
    std::string str() {
        const std::size_t n = count(1);
        std::string s(reinterpret_cast<const char*>(in_.data() + pos_), n);
        pos_ += n;
        return s;
    }
    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return in_.size() - pos_; }

private:
    std::uint64_t le(unsigned n) {
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

    std::span<const std::uint8_t> in_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

void write_command(Writer& w, const sim::Command& c) {
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

bool read_command(Reader& r, sim::Command& c) {
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

}  // namespace

std::uint64_t data_hash(std::span<const DataFile> files) {
    sim::StateHasher h;
    h.add_u64(files.size());
    for (const DataFile& f : files) {
        h.add_bytes({reinterpret_cast<const std::uint8_t*>(f.path.data()), f.path.size()});
        h.add_bytes({reinterpret_cast<const std::uint8_t*>(f.text.data()), f.text.size()});
    }
    return h.value();
}

std::vector<std::uint8_t> encode_replay(const Replay& replay) {
    Writer w;
    w.bytes(kMagic);
    w.u32(kReplayFormatVersion);
    w.u64(data_hash(replay.data));
    w.count(replay.data.size());
    for (const DataFile& f : replay.data) {
        w.str(f.path);
        w.str(f.text);
    }
    w.count(replay.commands.size());
    for (const ReplayCommand& rc : replay.commands) {
        w.u32(rc.issued_at);
        write_command(w, rc.command);
    }
    w.count(replay.checkpoints.size());
    for (const ReplayCheckpoint& cp : replay.checkpoints) {
        w.u32(cp.tick);
        w.u64(cp.hash);
    }
    w.u32(replay.end_tick);
    w.u64(replay.end_hash);
    w.u64(fnv1a(w.out()));
    return std::move(w.out());
}

std::expected<Replay, std::string> decode_replay(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kMagic.size() || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        return std::unexpected("no es una repetición (.rtsrep)");
    }
    Reader r(bytes.subspan(kMagic.size()));
    const std::uint32_t version = r.u32();
    if (!r.ok()) {
        return std::unexpected("repetición truncada");
    }
    if (version != kReplayFormatVersion) {
        return std::unexpected(
            std::format("versión de formato {} no admitida (esta versión del juego lee la {})", version,
                        kReplayFormatVersion));
    }
    if (bytes.size() < kMagic.size() + kChecksumBytes) {
        return std::unexpected("repetición truncada");
    }
    const auto body = bytes.first(bytes.size() - kChecksumBytes);
    Reader tail(bytes.last(kChecksumBytes));
    if (tail.u64() != fnv1a(body)) {
        return std::unexpected("repetición corrupta: la suma de comprobación no coincide");
    }
    // A partir de aquí el contenido está íntegro; aun así, cada lectura se comprueba.
    Replay replay;
    const std::uint64_t expected_data_hash = r.u64();
    replay.data.resize(r.count(2 * sizeof(std::uint32_t)));
    for (DataFile& f : replay.data) {
        f.path = r.str();
        f.text = r.str();
    }
    replay.commands.resize(r.count(sizeof(std::uint32_t)));
    for (ReplayCommand& rc : replay.commands) {
        rc.issued_at = r.u32();
        if (!read_command(r, rc.command)) {
            return std::unexpected("repetición corrupta: orden no válida");
        }
    }
    replay.checkpoints.resize(r.count(sizeof(std::uint32_t) + sizeof(std::uint64_t)));
    for (ReplayCheckpoint& cp : replay.checkpoints) {
        cp.tick = r.u32();
        cp.hash = r.u64();
    }
    replay.end_tick = r.u32();
    replay.end_hash = r.u64();
    if (!r.ok() || r.remaining() != kChecksumBytes) {
        return std::unexpected("repetición corrupta: estructura no válida");
    }
    if (data_hash(replay.data) != expected_data_hash) {
        return std::unexpected("repetición corrupta: los datos no coinciden con su hash");
    }
    return replay;
}

std::expected<void, std::string> save_replay(const std::filesystem::path& path, const Replay& replay) {
    const std::vector<std::uint8_t> bytes = encode_replay(replay);
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        return std::unexpected(std::format("{}: no se puede escribir", path.string()));
    }
    return {};
}

std::expected<Replay, std::string> load_replay(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected(std::format("{}: no se puede abrir", path.string()));
    }
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    auto replay = decode_replay(bytes);
    if (!replay) {
        return std::unexpected(std::format("{}: {}", path.string(), replay.error()));
    }
    return replay;
}

// --- Grabación -----------------------------------------------------------------

ReplayRecorder::ReplayRecorder(std::vector<DataFile> data, std::int32_t checkpoint_interval_ticks)
    : interval_(checkpoint_interval_ticks) {
    replay_.data = std::move(data);
}

void ReplayRecorder::issue(sim::World& world, sim::Command command) {
    replay_.commands.push_back({world.tick(), command});
    world.issue(std::move(command));
}

void ReplayRecorder::after_step(const sim::World& world) {
    if (world.tick() % static_cast<sim::Tick>(interval_) == 0) {
        replay_.checkpoints.push_back({world.tick(), world.state_hash()});
    }
}

Replay ReplayRecorder::finish(const sim::World& world) const {
    Replay out = replay_;
    out.end_tick = world.tick();
    out.end_hash = world.state_hash();
    return out;
}

// --- Reproducción --------------------------------------------------------------

void ReplayPlayer::before_step(sim::World& world) {
    const auto& commands = replay_->commands;
    while (next_command_ < commands.size() && commands[next_command_].issued_at <= world.tick()) {
        world.issue(commands[next_command_].command);
        ++next_command_;
    }
}

void ReplayPlayer::after_step(const sim::World& world) {
    const auto& checkpoints = replay_->checkpoints;
    while (next_checkpoint_ < checkpoints.size() && checkpoints[next_checkpoint_].tick <= world.tick()) {
        const ReplayCheckpoint& cp = checkpoints[next_checkpoint_];
        if (cp.tick == world.tick()) {
            compare(cp.tick, cp.hash, world.state_hash());
        }
        ++next_checkpoint_;
    }
    if (world.tick() == replay_->end_tick) {
        compare(world.tick(), replay_->end_hash, world.state_hash());
    }
}

void ReplayPlayer::compare(sim::Tick tick, std::uint64_t expected, std::uint64_t actual) {
    if (expected != actual && !diverged()) {
        diverged_at_ = tick;
        expected_ = expected;
        actual_ = actual;
    }
}

VerifyResult verify_replay(const Replay& replay, const sim::WorldParams& params) {
    sim::World world(params);
    ReplayPlayer player(replay);
    while (!player.finished(world) && !player.diverged()) {
        player.before_step(world);
        world.step();
        player.after_step(world);
    }
    VerifyResult result;
    result.ticks = world.tick();
    result.checkpoints = player.checkpoints_checked();
    if (player.diverged()) {
        result.diverged_at = player.diverged_at();
        result.expected_hash = player.expected_hash();
        result.actual_hash = player.actual_hash();
    } else {
        result.ok = true;
        result.expected_hash = replay.end_hash;
        result.actual_hash = world.state_hash();
    }
    return result;
}

}  // namespace rts::game
