#include "game/options.hpp"

#include <format>

#include <toml++/toml.hpp>

namespace rts::game {

namespace {

constexpr std::int64_t kMaxPercent = 100;
constexpr std::int64_t kMinSide = 320;
constexpr std::int64_t kMaxSide = 16384;

}  // namespace

std::expected<Options, std::string> parse_options(std::string_view text, const Options* base,
                                                  std::string_view source) {
    toml::table root;
    try {
        root = toml::parse(text, source);
    } catch (const toml::parse_error& e) {
        return std::unexpected(std::format("{}: {}", source, e.description()));
    }
    Options o = base != nullptr ? *base : Options{};
    const auto fail = [&](std::string msg) { return std::unexpected(std::format("{}: {}", source, msg)); };
    const auto need = [&](std::string_view key) { return base == nullptr && !root.at_path(key); };

    if (need("language") || need("fullscreen") || need("width") || need("height") || need("resolutions")) {
        return fail("faltan 'language', 'fullscreen', 'width', 'height' o 'resolutions'");
    }
    o.language = root["language"].value_or(o.language);
    o.fullscreen = root["fullscreen"].value_or(o.fullscreen);
    const auto width = root["width"].value_or(std::int64_t{o.width});
    const auto height = root["height"].value_or(std::int64_t{o.height});
    if (width < kMinSide || height < kMinSide || width > kMaxSide || height > kMaxSide) {
        return fail(std::format("'width' y 'height' deben estar entre {} y {}", kMinSide, kMaxSide));
    }
    o.width = static_cast<std::int32_t>(width);
    o.height = static_cast<std::int32_t>(height);
    if (const toml::array* list = root["resolutions"].as_array()) {
        o.resolutions.clear();
        for (const auto& r : *list) {
            const toml::array* wh = r.as_array();
            if (wh == nullptr || wh->size() != 2) {
                return fail("'resolutions' debe ser una lista de [ancho, alto]");
            }
            o.resolutions.emplace_back(static_cast<std::int32_t>((*wh)[0].value_or(std::int64_t{0})),
                                       static_cast<std::int32_t>((*wh)[1].value_or(std::int64_t{0})));
        }
    }
    for (const auto& [key, dest] : {std::pair{"volume.master", &o.master_percent},
                                    std::pair{"volume.effects", &o.effects_percent},
                                    std::pair{"volume.music", &o.music_percent}}) {
        if (need(key)) {
            return fail(std::format("falta '{}'", key));
        }
        const auto v = root.at_path(key).value_or(std::int64_t{*dest});
        if (v < 0 || v > kMaxPercent) {
            return fail(std::format("'{}' debe estar entre 0 y {}", key, kMaxPercent));
        }
        *dest = static_cast<std::int32_t>(v);
    }
    for (std::size_t i = 0; i < kKeyActionCount; ++i) {
        const std::string path = std::format("keys.{}", kKeyActionIds[i]);
        if (need(path)) {
            return fail(std::format("falta '{}'", path));
        }
        o.keys[i] = root.at_path(path).value_or(o.keys[i]);
        if (o.keys[i].empty()) {
            return fail(std::format("'{}' no puede estar vacía", path));
        }
    }
    return o;
}

std::string options_toml(const Options& o) {
    std::string out = std::format(
        "# Opciones del jugador (F5), escritas por el juego. Lo que falte se toma de\n"
        "# data/opciones.toml.\nlanguage = \"{}\"\nfullscreen = {}\nwidth = {}\nheight = {}\n\n[volume]\n"
        "master = {}\neffects = {}\nmusic = {}\n\n[keys]\n",
        o.language, o.fullscreen ? "true" : "false", o.width, o.height, o.master_percent, o.effects_percent,
        o.music_percent);
    for (std::size_t i = 0; i < kKeyActionCount; ++i) {
        out += std::format("{} = \"{}\"\n", kKeyActionIds[i], o.keys[i]);
    }
    return out;
}

}  // namespace rts::game
