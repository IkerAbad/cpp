#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

#include "game/config.hpp"

namespace rts::game {

struct LaunchOptions {
    std::filesystem::path data_dir;
    bool headless = false;
    // Solo en modo headless: número de ticks a simular antes de salir.
    std::int64_t headless_ticks = 0;
    // Con ventana: salir tras este número de fotogramas (0 = sin límite). Prueba de humo.
    std::int64_t max_frames = 0;
    std::filesystem::path record;         // headless: grabar la repetición aquí
    std::filesystem::path replay;         // ver esta repetición con ventana
    std::filesystem::path verify_replay;  // reproducirla sin ventana y comprobar sus hashes
    std::filesystem::path load;           // cargar esta partida guardada (.rtssav) y seguir jugando
};

// Analiza argv. Devuelve nullopt y escribe la ayuda si los argumentos no son válidos.
std::optional<LaunchOptions> parse_arguments(int argc, char** argv);

struct Replay;

// Simula sin ventana ni GPU y escribe el hash final. Lo usa la CI. Con record no vacío,
// graba la partida en ese fichero.
int run_headless(const GameData& data, std::int64_t ticks, const std::filesystem::path& record);

// Bucle interactivo con ventana, render y paso fijo. Sin replay, partida nueva que se
// graba sola en data.engine.replay.directory; con ella, reproductor sin órdenes.
// Con resume, sigue la partida guardada que es (se rehace a toda velocidad al empezar).
int run_windowed(const GameData& data, std::int64_t max_frames, const Replay* replay = nullptr,
                 const Replay* resume = nullptr);

// Menú inicial (nueva partida con semilla, rival y niebla; cargar; repeticiones) y,
// al terminar cada partida, vuelta a él. Es lo que se abre sin argumentos.
int run_interactive(const GameData& base);

// Carga una partida guardada con los datos que lleva dentro y la sigue con ventana.
int run_load(const std::filesystem::path& path, std::int64_t max_frames);

// Carga la repetición con los datos que lleva dentro y la reproduce con ventana.
int run_replay(const std::filesystem::path& path, std::int64_t max_frames);

// Reproduce la repetición sin ventana y compara todos sus hashes. 0 si coinciden.
int run_verify_replay(const std::filesystem::path& path);

}  // namespace rts::game
