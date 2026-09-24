#include "game/config.hpp"

#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>

#include <toml++/toml.hpp>

namespace rts::game {

namespace {

// Convención de data/: sin números en coma flotante. Las magnitudes fraccionarias
// se escriben como enteros en una unidad menor, indicada en el nombre de la clave
// (_milli = milésimas). Así la carga no depende de cómo cada plataforma redondea
// un decimal a binario.
constexpr std::int32_t kMilli = 1000;

class Reader {
public:
    Reader(const toml::table& root, std::string_view source) : root_(root), source_(source) {}

    std::int32_t get_i32(std::string_view path, std::int64_t min, std::int64_t max) {
        const auto value = root_.at_path(path).value<std::int64_t>();
        if (!value) {
            fail(std::format("{}: falta la clave entera '{}'", source_, path));
            return 0;
        }
        if (*value < min || *value > max) {
            fail(std::format("{}: '{}' = {} fuera del rango [{}, {}]", source_, path, *value, min, max));
            return 0;
        }
        return static_cast<std::int32_t>(*value);
    }

    std::uint64_t get_u64(std::string_view path) {
        const auto value = root_.at_path(path).value<std::int64_t>();
        if (!value || *value < 0) {
            fail(std::format("{}: falta la clave entera no negativa '{}'", source_, path));
            return 0;
        }
        return static_cast<std::uint64_t>(*value);
    }

    bool get_bool(std::string_view path) {
        const auto value = root_.at_path(path).value<bool>();
        if (!value) {
            fail(std::format("{}: falta la clave booleana '{}'", source_, path));
            return false;
        }
        return *value;
    }

    std::string get_string(std::string_view path) {
        const auto value = root_.at_path(path).value<std::string>();
        if (!value) {
            fail(std::format("{}: falta la clave de texto '{}'", source_, path));
            return {};
        }
        return *value;
    }

    [[nodiscard]] const std::optional<std::string>& error() const noexcept { return error_; }

private:
    void fail(std::string message) {
        if (!error_) {
            error_ = std::move(message);
        }
    }

    const toml::table& root_;
    std::string_view source_;
    std::optional<std::string> error_;
};

}  // namespace

std::expected<EngineConfig, std::string> parse_engine_config(std::string_view toml_text,
                                                             std::string_view source_name) {
    toml::table root;
    try {
        root = toml::parse(toml_text, source_name);
    } catch (const toml::parse_error& e) {
        return std::unexpected(std::format("{}: {}", source_name, e.description()));
    }

    Reader r(root, source_name);
    EngineConfig cfg;
    cfg.window.title = r.get_string("window.title");
    cfg.window.width = r.get_i32("window.width", 320, 16384);
    cfg.window.height = r.get_i32("window.height", 240, 16384);
    cfg.window.vsync = r.get_bool("window.vsync");
    cfg.loop.max_ticks_per_frame = r.get_i32("loop.max_ticks_per_frame", 1, 100);
    cfg.demo.seed = r.get_u64("demo.seed");
    cfg.demo.point_count = r.get_i32("demo.point_count", 0, 100'000);
    cfg.demo.arena_tiles = r.get_i32("demo.arena_tiles", 1, 1024);
    const std::int32_t speed_milli = r.get_i32("demo.max_speed_milli_tiles_per_tick", 0, 10 * kMilli);
    cfg.demo.max_speed = sim::Fixed::from_ratio(speed_milli, kMilli);

    if (r.error()) {
        return std::unexpected(*r.error());
    }
    if (cfg.demo.max_speed * 2 >= sim::Fixed::from_int(cfg.demo.arena_tiles)) {
        return std::unexpected(std::format("{}: demo.max_speed_milli_tiles_per_tick debe ser menor que "
                                           "la mitad de demo.arena_tiles",
                                           source_name));
    }
    return cfg;
}

std::expected<EngineConfig, std::string> load_engine_config(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected(std::format("{}: no se puede abrir", path.string()));
    }
    const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    return parse_engine_config(text, path.string());
}

}  // namespace rts::game
