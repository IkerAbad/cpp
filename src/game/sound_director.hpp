#pragma once

// Qué suena en cada momento (F2). Traduce lo que pasa en la partida (golpes, disparos,
// bajas, derrumbes, obras, fuego, avisos, órdenes) en sonidos con volumen y panorama
// según dónde ocurre en pantalla, y elige la música: de paz o de batalla si hay combate
// a la vista. Puro: no toca SDL; lo que decide se recoge con take().

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "audio/bank.hpp"
#include "audio/sound_spec.hpp"
#include "game/alerts.hpp"
#include "game/config.hpp"
#include "render/projection.hpp"
#include "sim/world.hpp"

namespace rts::game {

enum class WorkSound : std::uint8_t { Chop, Mine, Build };

// Lo que se ve, para el fondo: casillas exploradas en pantalla y cuántas son agua o
// bosque (terreno o árbol), y la luz del día.
struct AmbientView {
    std::int32_t tiles = 0;
    std::int32_t water = 0;
    std::int32_t forest = 0;
    std::int32_t daylight_percent = 100;
};

class SoundDirector {
public:
    // Posición en pantalla de un punto del mapa, o nada si el jugador no lo ve.
    using Locate = std::function<std::optional<render::Vec2>(sim::Position)>;

    SoundDirector(const audio::SoundSpec& spec, const GameData& data);

    // Tras cada tick: golpes, disparos, bajas, derrumbes y obras terminadas del jugador me.
    void on_tick(const sim::Snapshot& prev, const sim::Snapshot& curr, sim::PlayerId me, const Locate& locate,
                 render::Vec2 screen, double now_ms);
    void on_alert(AlertKind kind, double now_ms);
    void on_order(double now_ms);
    // Golpe de herramienta de una figura que trabaja.
    void on_work(WorkSound kind, render::Vec2 at, render::Vec2 screen, double now_ms);
    // Crepitar de los edificios en llamas a la vista (posiciones en pantalla).
    void on_fires(const std::vector<render::Vec2>& burning, render::Vec2 screen, double now_ms);

    [[nodiscard]] std::vector<audio::SoundCue> take();
    // Pieza que toca ahora (índice en SoundSpec::music).
    [[nodiscard]] std::int32_t music(double now_ms) const noexcept;

    // Fondo: qué terrenos cuentan como agua o bosque, y el volumen 0..1 de cada bucle
    // (por AmbientSlot), que sigue a lo que se ve en fade_ms.
    [[nodiscard]] bool water_terrain(sim::TerrainId t) const noexcept { return t < water_.size() && water_[t] != 0; }
    [[nodiscard]] bool forest_terrain(sim::TerrainId t) const noexcept { return t < forest_.size() && forest_[t] != 0; }
    void on_view(const AmbientView& view, double now_ms);
    [[nodiscard]] const std::array<float, audio::kAmbientCount>& ambient() const noexcept { return ambient_; }

private:
    void cue(std::int32_t recipe, float volume, float pan, double now_ms);
    void cue_at(std::int32_t recipe, render::Vec2 at, render::Vec2 screen, double now_ms);
    [[nodiscard]] std::int32_t hit_sound(const sim::SnapshotHit& h) const;

    const audio::SoundSpec* spec_;
    const GameData* data_;
    std::vector<double> last_played_;  // por receta: última vez (ms)
    std::vector<audio::SoundCue> cues_;
    double last_combat_ms_ = -1.0e9;
    std::vector<std::uint8_t> water_;   // por terreno
    std::vector<std::uint8_t> forest_;
    std::array<float, audio::kAmbientCount> ambient_{};
    double last_view_ms_ = -1.0;
};

}  // namespace rts::game
