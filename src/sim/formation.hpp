#pragma once

// Formaciones (B5): línea, columna y cuadro. Una formación solo vale con al menos
// min_members unidades en ella a cohesion_radius o menos (contándose a sí misma).
//   - Columna: marcha más rápido, pero desplegada pega peor.
//   - Línea: todos tienen tiro libre: los tiradores hieren más; marcha algo más lenta.
//   - Cuadro: el erizo de picas, «far too dense for the cavalry to penetrate»
//     (Wikipedia, «Schiltron»): anula la carga y la caballería le hace menos daño;
//     pero es lento y blanco fácil para los arqueros, como en Falkirk (1298).
// Al moverse en formación, cada unidad va a su puesto alrededor del destino.
//
// Determinismo: rejilla espacial ordenada por conteo; puestos repartidos por orden de
// entidad.

#include <cstdint>
#include <span>
#include <vector>

#include <entt/entity/registry.hpp>

#include "sim/fixed.hpp"
#include "sim/movement.hpp"
#include "sim/tick.hpp"
#include "sim/units.hpp"

namespace rts::sim {

class StateHasher;

enum class FormationKind : std::uint8_t { None, Line, Column, Square };

// SetStance con kind = kFormationBase + FormationKind cambia la formación.
inline constexpr std::uint8_t kFormationBase = 32;

struct Formation {
    FormationKind kind = FormationKind::None;
    // Derivados (los pone FormationSystem cada tick; no entran en el hash):
    bool active = false;                  // hay bastantes en formación alrededor
    std::int32_t speed_percent = 100;
    std::int32_t attack_percent = 100;    // tiradores en línea, desplegados en columna
    std::int32_t cavalry_taken_percent = 100;
    std::int32_t ranged_taken_percent = 100;
    bool stops_charge = false;
};

// Efectos de una formación (engine.toml [formation.*]).
struct FormationEffect {
    std::int32_t speed_percent = 100;
    std::int32_t melee_attack_percent = 100;
    std::int32_t ranged_attack_percent = 100;
    std::int32_t cavalry_taken_percent = 100;
    std::int32_t ranged_taken_percent = 100;
    bool stops_charge = false;
};

// data/config/engine.toml, sección [formation].
struct FormationParams {
    bool enabled = false;
    std::int32_t min_members = 1;
    Fixed cohesion_radius;
    Fixed spacing;                 // separación entre puestos, en casillas
    ArmorClassId cavalry_class = 0;
    FormationEffect line;
    FormationEffect column;
    FormationEffect square;
};

class FormationSystem {
public:
    FormationSystem(std::int32_t width, std::int32_t height, const FormationParams& params,
                    std::vector<UnitType> units);

    // SetStance con kind de formación: la adopta. Move de unidades en formación: cada
    // una, a su puesto. Devuelve las unidades que ya ha mandado (el resto, como siempre).
    std::vector<entt::entity> apply(entt::registry& registry, MovementSystem& movement, const Command& command,
                                    std::span<const entt::entity> units, std::uint32_t& next_order_id, Tick tick);
    void update(entt::registry& registry);

    [[nodiscard]] bool enabled() const noexcept { return params_.enabled; }
    [[nodiscard]] const FormationParams& params() const noexcept { return params_; }
    // Puestos de n unidades de la formación kind alrededor de target, mirando en la
    // dirección dir (de dónde vienen a dónde van).
    [[nodiscard]] std::vector<TileCoord> slots(FormationKind kind, std::size_t n, TileCoord target,
                                               FVec2 dir) const;
    void hash_into(StateHasher& h, const entt::registry& registry) const;

private:
    [[nodiscard]] const FormationEffect& effect(FormationKind k) const noexcept;

    std::int32_t width_;
    std::int32_t height_;
    FormationParams params_;
    std::vector<UnitType> units_;
};

}  // namespace rts::sim
