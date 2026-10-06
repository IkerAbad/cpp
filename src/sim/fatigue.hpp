#pragma once

// Fatiga (B3). Marchar cansa, y más en terreno difícil y a paso forzado; golpear,
// también. Parado se descansa. El cansancio resta velocidad y ataque, a lo sumo hasta
// exhausted_*_percent con la unidad agotada. El paso forzado va más deprisa a costa de
// cansar mucho más: Vegetius da 20 millas en cinco horas al paso militar y 24 al «full
// step», y «If they exceed this pace, they no longer march but run» («De re militari»,
// libro I).
//
// Determinismo: recorrido en el orden de la vista; los golpes, en el orden en que los
// deja el combate.

#include <cstdint>
#include <span>
#include <vector>

#include <entt/entity/registry.hpp>

#include "sim/morale.hpp"
#include "sim/movement.hpp"
#include "sim/tick.hpp"
#include "sim/units.hpp"

namespace rts::sim {

class StateHasher;

// data/config/engine.toml, sección [fatigue]. Ritmos en cienmilésimas de cansancio
// (kFullFatigue = agotada), escalados por 100 / stamina del tipo.
struct FatigueParams {
    bool enabled = false;
    std::int32_t march_per_tick = 0;     // marchando en llano, al paso
    std::int32_t strike = 0;             // por golpe dado
    std::int32_t rest_per_tick = 0;      // parada (no escalado: descansar no depende del aguante)
    std::int32_t forced_speed_percent = 100;
    std::int32_t forced_fatigue_percent = 100;
    std::int32_t exhausted_speed_percent = 100;
    std::int32_t exhausted_attack_percent = 100;
};

class FatigueSystem {
public:
    FatigueSystem(const FatigueParams& params, std::vector<UnitType> units);

    // SetStance con kind kPaceNormal o kPaceForced: el paso de marcha.
    void apply(entt::registry& registry, const Command& command, std::span<const entt::entity> units) const;
    // Después del movimiento del tick: quien se movió se cansa (según el terreno que
    // pisa) y quien no, descansa; hits son los golpes dados este tick.
    void update(entt::registry& registry, const MovementSystem& movement, std::span<const MoraleHit> hits) const;

    [[nodiscard]] bool enabled() const noexcept { return params_.enabled; }
    [[nodiscard]] const FatigueParams& params() const noexcept { return params_; }
    void hash_into(StateHasher& h, const entt::registry& registry) const;

private:
    void refresh(Fatigue& f) const noexcept;
    [[nodiscard]] std::int32_t scaled(UnitTypeId type, std::int64_t amount) const noexcept;

    FatigueParams params_;
    std::vector<UnitType> units_;
};

}  // namespace rts::sim
