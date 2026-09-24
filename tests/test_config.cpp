#include <string>

#include <doctest/doctest.h>

#include "game/config.hpp"

using rts::game::load_engine_config;
using rts::game::parse_engine_config;
using rts::sim::Fixed;

namespace {

constexpr const char* kValid = R"(
[window]
title = "prueba"
width = 800
height = 600
vsync = false

[loop]
max_ticks_per_frame = 5

[demo]
seed = 1
point_count = 10
arena_tiles = 32
max_speed_milli_tiles_per_tick = 250
)";

}  // namespace

TEST_CASE("Configuración: un fichero válido se lee entero") {
    const auto cfg = parse_engine_config(kValid);
    REQUIRE(cfg.has_value());
    CHECK(cfg->window.title == "prueba");
    CHECK(cfg->window.width == 800);
    CHECK_FALSE(cfg->window.vsync);
    CHECK(cfg->loop.max_ticks_per_frame == 5);
    CHECK(cfg->demo.point_count == 10);
    CHECK(cfg->demo.max_speed == Fixed::from_ratio(1, 4));
}

TEST_CASE("Configuración: una clave ausente da un error que la nombra") {
    std::string text = kValid;
    text.replace(text.find("point_count"), 11, "otra_clave");
    const auto cfg = parse_engine_config(text);
    REQUIRE_FALSE(cfg.has_value());
    CHECK(cfg.error().find("demo.point_count") != std::string::npos);
}

TEST_CASE("Configuración: un valor fuera de rango se rechaza") {
    std::string text = kValid;
    text.replace(text.find("max_ticks_per_frame = 5"), 23, "max_ticks_per_frame = 0");
    const auto cfg = parse_engine_config(text);
    REQUIRE_FALSE(cfg.has_value());
    CHECK(cfg.error().find("loop.max_ticks_per_frame") != std::string::npos);
}

TEST_CASE("Configuración: un decimal donde se espera un entero se rechaza") {
    std::string text = kValid;
    text.replace(text.find("= 250"), 5, "= 0.25");
    CHECK_FALSE(parse_engine_config(text).has_value());
}

TEST_CASE("Configuración: TOML mal formado da error, no excepción") {
    CHECK_FALSE(parse_engine_config("[window\ntitle = ").has_value());
}

TEST_CASE("Configuración: el engine.toml del repositorio es válido") {
    const auto cfg = load_engine_config(RTS_DATA_DIR "/config/engine.toml");
    CHECK_MESSAGE(cfg.has_value(), (cfg ? std::string() : cfg.error()));
}
