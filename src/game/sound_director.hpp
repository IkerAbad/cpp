#pragma once

// Qué suena en cada momento (F2). Traduce lo que pasa en la partida (golpes, disparos,
// bajas, derrumbes, obras, fuego, avisos, órdenes) en sonidos con volumen y panorama
// según dónde ocurre en pantalla, y elige la música: de paz o de batalla si hay combate
// a la vista. Puro: no toca SDL; lo que decide se recoge con take().

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

class SoundDirector {
public:
    // Posición en pantalla de un punto del mapa, o nada si el jugador no lo ve.
    using Locate = std::function<std::optional<render::Vec2>(sim::Position)>;

    SoundDirector(const audio::SoundSpec& spec, const GameData& data) : spec_(&spec), data_(&data) {
        last_played_.assign(spec.recipes.size(), -1.0e9);
    }

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

private:
    void cue(std::int32_t recipe, float volume, float pan, double now_ms);
    void cue_at(std::int32_t recipe, render::Vec2 at, render::Vec2 screen, double now_ms);
    [[nodiscard]] std::int32_t hit_sound(const sim::SnapshotHit& h) const;

    const audio::SoundSpec* spec_;
    const GameData* data_;
    std::vector<double> last_played_;  // por receta: última vez (ms)
    std::vector<audio::SoundCue> cues_;
    double last_combat_ms_ = -1.0e9;
};

}  // namespace rts::game
