#pragma once

// Combate (M4): blancos, persecución, golpes cuerpo a cuerpo, proyectiles que vuelan
// hacia donde estaba el blanco al disparar (una unidad en marcha puede esquivarlos),
// experiencia por unidad con niveles y héroes con aura.
//
// Determinismo: todo el daño de un tick se calcula sobre el estado del principio del
// tick, se acumula y se aplica de una vez (Jacobi); las muertes, al final. Dos unidades
// iguales que se golpean a la vez mueren a la vez, sin depender del orden de iteración.

#include <cstdint>
#include <span>
#include <vector>

#include <entt/entity/registry.hpp>

#include "sim/economy.hpp"
#include "sim/fixed.hpp"
#include "sim/fmath.hpp"
#include "sim/movement.hpp"
#include "sim/tick.hpp"
#include "sim/units.hpp"

namespace rts::sim {

class StateHasher;

// data/config/engine.toml, sección [combat].
struct CombatParams {
    // Cada unidad busca blanco cada tantos ticks (repartidas por id): coste por tick
    // proporcional a unidades / intervalo, no a todas.
    std::int32_t acquire_interval_ticks = 1;
    // Se vuelve a calcular la persecución si el blanco se alejó al menos esto.
    std::int32_t repath_tiles = 1;
    // Persecuciones que pueden acabar sin alcanzar un blanco quieto antes de rendirse.
    std::int32_t chase_attempts = 1;
    // Holgura extra de alcance contra edificios: quien llega a una casilla vecina de la
    // huella se para cerca de su centro, a hasta ~0,75 casillas del borde.
    Fixed building_reach;
    Fixed projectile_hit_radius;  // holgura sobre el radio del blanco al caer el proyectil
    std::int32_t xp_kill_bonus = 0;
    std::vector<std::int32_t> level_thresholds;  // experiencia para el nivel i + 1; el último = héroe
    std::int32_t hp_percent_per_level = 0;
    std::int32_t attack_percent_per_level = 0;
    std::int32_t armor_every_levels = 0;  // +1 de armadura cada tantos niveles (0 = nunca)
    Fixed hero_aura_radius;               // casillas
    std::int32_t hero_aura_attack_percent = 0;
    std::int32_t hero_name_count = 0;     // nombres disponibles (los textos, en la presentación)
};

// Proyectil en vuelo: una entidad propia, sin Unit (el movimiento no lo ve).
struct Projectile {
    FVec2 pos;
    FVec2 dest;
    Fixed speed;
    entt::entity attacker = entt::null;
    entt::entity intended = entt::null;
    PlayerId owner = 0;
    UnitTypeId attacker_type = 0;
    std::int32_t attack_percent = 100;  // nivel y aura del tirador al disparar
};

struct CombatTickStats {
    std::int32_t melee_hits = 0;
    std::int32_t projectiles_fired = 0;
    std::int32_t projectiles_hit = 0;
    std::int32_t projectiles_missed = 0;
    std::int32_t kills = 0;
    std::int32_t acquisitions = 0;
};

class CombatSystem {
public:
    CombatSystem(std::int32_t width, std::int32_t height, const CombatParams& params, std::vector<UnitType> units,
                 std::vector<BuildingType> buildings);

    // Attack, AttackMove y SetStance; cualquier otra orden sobre unidades cancela su blanco.
    void apply(entt::registry& registry, MovementSystem& movement, const Command& command,
               std::span<const entt::entity> units, std::uint32_t& next_order_id, Tick tick);
    void update(entt::registry& registry, MovementSystem& movement, EconomySystem& economy,
                std::uint32_t& next_order_id, Tick tick);

    // Daño de un golpe de una unidad (tipo y porcentaje de ataque) sobre un blanco.
    [[nodiscard]] std::int32_t damage(UnitTypeId attacker_type, std::int32_t attack_percent,
                                      const entt::registry& registry, entt::entity target) const;
    [[nodiscard]] std::int32_t attack_percent(const Combatant& c, bool aura) const noexcept;
    [[nodiscard]] const CombatParams& params() const noexcept { return params_; }
    [[nodiscard]] const CombatTickStats& last_stats() const noexcept { return stats_; }

    void hash_into(StateHasher& h, const entt::registry& registry) const;

private:
    struct Hit {
        entt::entity target;
        entt::entity attacker;
        std::int32_t amount;
    };

    // Rejilla espacial de unidades del tick (ordenación por conteo), índice denso.
    struct Scratch {
        std::vector<entt::entity> entity;
        std::vector<FVec2> pos;
        std::vector<Fixed> radius;
        std::vector<PlayerId> owner;
        std::vector<std::uint8_t> aura;
        std::vector<std::uint32_t> cell_start;
        std::vector<std::uint32_t> cell_units;
    };

    void gather(const entt::registry& registry);
    [[nodiscard]] std::size_t cell_of(FVec2 p) const noexcept;
    template <typename Fn>
    void for_each_near(FVec2 center, Fixed radius, Fn&& fn) const;
    void mark_auras(const entt::registry& registry);
    [[nodiscard]] bool is_enemy_target(const entt::registry& registry, entt::entity target, PlayerId me) const;
    [[nodiscard]] entt::entity acquire(std::size_t i, Fixed sight) const;
    [[nodiscard]] bool in_range(const entt::registry& registry, FVec2 pos, Fixed radius, Fixed range,
                                entt::entity target) const;
    [[nodiscard]] FVec2 aim_point(const entt::registry& registry, entt::entity target) const;
    void clear_target(entt::registry& registry, MovementSystem& movement, entt::entity e, Combatant& c,
                      std::uint32_t& next_order_id, Tick tick) const;
    void update_projectiles(entt::registry& registry, const EconomySystem& economy);
    void apply_hits(entt::registry& registry, MovementSystem& movement, EconomySystem& economy);
    void level_up(entt::registry& registry, entt::entity e, Combatant& c);

    std::int32_t width_;
    std::int32_t height_;
    CombatParams params_;
    std::vector<UnitType> units_;
    std::vector<BuildingType> buildings_;
    std::int32_t heroes_made_ = 0;
    Scratch s_;
    std::vector<Hit> hits_;
    std::vector<Projectile> new_projectiles_;
    std::vector<entt::entity> scratch_;
    CombatTickStats stats_;
};

}  // namespace rts::sim
