#pragma once

// Qué suena y cómo (data/sound.toml, F2): recetas de efectos, piezas de música, qué
// suceso del juego usa cada receta y los ajustes de la mezcla.

#include <cstdint>
#include <string>
#include <vector>

#include "audio/synth.hpp"

namespace rts::audio {

struct AudioConfig {
    std::int32_t sample_rate = 0;
    std::int32_t latency_ms = 0;        // lo que se adelanta a la tarjeta
    std::int32_t max_voices = 0;
    std::int32_t master_percent = 0;
    std::int32_t effects_percent = 0;
    std::int32_t music_percent = 0;
    std::int32_t fade_ms = 0;           // cambio de pieza
    std::int32_t battle_hold_ms = 0;    // sin combate a la vista durante esto, vuelve la paz
    std::int32_t hearing_margin_px = 0; // fuera de la pantalla se oye hasta esta distancia
    std::int32_t pan_percent = 0;       // cuánto se abre el panorama a los lados
    std::int32_t fire_interval_ms = 0;  // crepitar de los edificios en llamas
};

// Receta (índice en recipes) de cada suceso; -1 = sin sonido.
struct EventSounds {
    std::int32_t melee_hit = -1;   // espada, hacha
    std::int32_t blunt_hit = -1;   // lanza, maza, herramienta
    std::int32_t shot = -1;        // arco o ballesta que dispara
    std::int32_t ranged_hit = -1;  // flecha o virote que acierta
    std::int32_t siege_hit = -1;   // ariete o trabuquete
    std::int32_t death = -1;
    std::int32_t collapse = -1;    // edificio que cae
    std::int32_t fire = -1;
    std::int32_t chop = -1;        // talar
    std::int32_t mine = -1;        // picar piedra o mineral
    std::int32_t build = -1;       // obra
    std::int32_t alarm = -1;       // le atacan, algo arde
    std::int32_t ready = -1;       // unidad lista
    std::int32_t built = -1;       // edificio terminado
    std::int32_t order = -1;       // orden dada
};

// Fondo (F2): bucles que suenan según lo que se ve. Recetas con loop_ms (-1 = ninguno).
enum class AmbientSlot : std::uint8_t { Wind, Water, Birds, Night };
inline constexpr std::size_t kAmbientCount = 4;

struct AmbientSounds {
    std::int32_t wind = -1;   // siempre
    std::int32_t water = -1;  // con agua a la vista
    std::int32_t birds = -1;  // con bosque a la vista, de día
    std::int32_t night = -1;  // de noche (grillos)
    std::int32_t wind_percent = 0;
    std::int32_t water_percent = 0;
    std::int32_t birds_percent = 0;
    std::int32_t night_percent = 0;
    // Con este % de la vista de agua (o de bosque) ya suena al máximo.
    std::int32_t water_full_percent = 1;
    std::int32_t forest_full_percent = 1;
    std::int32_t fade_ms = 0;  // lo que tarda en seguir a la cámara
    std::vector<std::string> water_terrains;   // nombres de terreno que cuentan como agua
    std::vector<std::string> forest_terrains;  // ... y como bosque (además de los árboles)
};

struct SoundSpec {
    AudioConfig config;
    std::vector<SoundRecipe> recipes;
    std::vector<MusicPiece> music;
    std::int32_t peace_music = -1;   // índice en music
    std::int32_t battle_music = -1;
    EventSounds events;
    AmbientSounds ambient;
};

}  // namespace rts::audio
