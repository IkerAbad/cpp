#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

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
    // Partida en red sin ventana (E1): anfitrión en este puerto o invitado de esta dirección.
    std::optional<std::uint16_t> host_port;
    std::uint8_t net_players = 2;  // anfitrión: jugadores en total, él incluido
    std::string join_host;
    std::uint16_t join_port = 0;
    // Informes de errores (G3): dónde se escriben (vacío: informes/ junto al ejecutable).
    std::filesystem::path report_dir;
    // Solo para probar los informes: sin ventana, provocar una caída en este tick.
    std::int64_t crash_at = -1;
};

// Analiza argv. Devuelve nullopt y escribe la ayuda si los argumentos no son válidos.
std::optional<LaunchOptions> parse_arguments(int argc, char** argv);

struct Replay;

// Simula sin ventana ni GPU y escribe el hash final. Lo usa la CI. Con record no vacío,
// graba la partida en ese fichero.
// crash_at >= 0: provoca una caída en ese tick (prueba de los informes de errores, G3).
int run_headless(const GameData& data, std::int64_t ticks, const std::filesystem::path& record,
                 std::int64_t crash_at = -1);

// Partida en red sin ventana: el anfitrión espera a los invitados y fija la duración
// (ticks); cada uno da órdenes de prueba a su jugador ([net.probe]) y la IA juega dentro
// de la simulación. Escribe el hash final; 0 si todos acaban con el mismo.
int run_net_headless(const GameData& data, const LaunchOptions& options);

// Partida en red con ventana sin pasar por el menú (pruebas de humo): el anfitrión
// empieza en cuanto se reúnen options.net_players jugadores, todos humanos.
int run_net_windowed(const GameData& base, const LaunchOptions& options);

// Bucle interactivo con ventana, render y paso fijo. Sin replay, partida nueva que se
// graba sola en data.engine.replay.directory; con ella, reproductor sin órdenes.
// Con resume, sigue la partida guardada que es (se rehace a toda velocidad al empezar).
int run_windowed(const GameData& data, std::int64_t max_frames, const Replay* replay = nullptr,
                 const Replay* resume = nullptr);

// Menú inicial (nueva partida con semilla, rival y niebla; cargar; repeticiones) y,
// al terminar cada partida, vuelta a él. Es lo que se abre sin argumentos.
// data_dir: de donde salen también las campañas (F4).
int run_interactive(const GameData& base, const std::filesystem::path& data_dir);

// Carga una partida guardada con los datos que lleva dentro y la sigue con ventana.
int run_load(const std::filesystem::path& path, std::int64_t max_frames);

// Carga la repetición con los datos que lleva dentro y la reproduce con ventana.
int run_replay(const std::filesystem::path& path, std::int64_t max_frames);

// Reproduce la repetición sin ventana y compara todos sus hashes. 0 si coinciden.
int run_verify_replay(const std::filesystem::path& path);

}  // namespace rts::game
