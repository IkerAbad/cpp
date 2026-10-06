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
#include "sim/fire.hpp"
#include "sim/fixed.hpp"
#include "sim/fmath.hpp"
#include "sim/morale.hpp"
#include "sim/movement.hpp"
#include "sim/tick.hpp"
#include "sim/tile_map.hpp"
#include "sim/units.hpp"
#include "sim/vision.hpp"

namespace rts::sim {

class StateHasher;

// Daño de un golpe: max(1, max(0, cuerpo - armadura) + max(0, proyectil - armadura) +
// bonus contra la clase del blanco). percent escala el ataque (nivel y aura).
[[nodiscard]] std::int32_t hit_damage(const CombatStats& attacker, std::int32_t percent, std::int32_t armor_melee,
                                      std::int32_t armor_pierce, ArmorClassId armor_class) noexcept;

// Clases de blanco para la adquisición automática (engine.toml, combat.target_priority).
enum class TargetClass : std::uint8_t {
    AttackingMe,  // unidad armada que tiene por blanco a quien busca (autopreservación)
    Armed,        // puede herir a la tropa
    Siege,        // ingenio de asedio (solo ataca edificios)
    Carrier,      // bagaje
    Worker,       // aldeano
    Other,
};

// Terreno en combate (B2): engine.toml [terrain] y, por tipo, terrain.toml.
//   - Altura: el tirador más alto alcanza más lejos y hiere más; cuesta arriba, menos.
//     Cuerpo a cuerpo, pelear cuesta arriba cansa y resta; cuesta abajo suma.
//   - Bosque: cubre de las flechas (arrow_cover_percent del terreno del blanco).
//   - Carga: tras charge_run_ticks en marcha, el primer golpe cuerpo a cuerpo de un
//     tipo con charge_percent > 100 vale ese %, si ambos pisan terreno que lo permite.
struct TerrainCombatParams {
    bool enabled = false;
    Fixed range_per_level;                 // alcance por nivel de altura de ventaja
    std::int32_t max_levels = 0;           // la diferencia de altura que cuenta, como mucho
    std::int32_t ranged_percent_per_level = 0;
    std::int32_t melee_percent_per_level = 0;
    std::int32_t charge_run_ticks = 1;
    std::vector<std::int32_t> arrow_cover_percent_by_terrain;  // por TerrainId
    std::vector<std::uint8_t> charge_by_terrain;               // por TerrainId
};

// Alcance de un tirador con levels niveles de altura de ventaja (negativo: cuesta
// arriba). No baja de la mitad del alcance base.
[[nodiscard]] Fixed slope_range(Fixed range, Fixed per_level, std::int32_t levels) noexcept;
// Ataque en % con levels niveles de ventaja, a per_level % por nivel.
[[nodiscard]] std::int32_t slope_percent(std::int32_t percent, std::int32_t per_level, std::int32_t levels) noexcept;

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
    // Orden de preferencia al elegir blanco solo: primero la clase que va antes en la
    // lista y, dentro de ella, el más cercano. Las no listadas, al final. Vacía: solo
    // la distancia.
    std::vector<TargetClass> target_priority;
    // Una unidad hambrienta (sin víveres) ataca a este % (data: [supply]).
    std::int32_t hungry_attack_percent = 100;
    TerrainCombatParams terrain;
    // Escalando un muro (B4: [climb]): el daño que recibe, a este %.
    std::int32_t climb_exposed_percent = 100;
    // Clase de armadura de la caballería (B5: el cuadro la resiste).
    ArmorClassId cavalry_class = 0;
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
    // map: alturas y terrenos para el terreno en combate (B2); debe vivir más que el sistema.
    CombatSystem(const TileMap& map, const CombatParams& params, std::vector<UnitType> units,
                 std::vector<BuildingType> buildings);

    // Attack, AttackMove y SetStance; cualquier otra orden sobre unidades cancela su blanco.
    void apply(entt::registry& registry, MovementSystem& movement, const Command& command,
               std::span<const entt::entity> units, std::uint32_t& next_order_id, Tick tick);
    // Los golpes de unidades que no son de asedio contra edificios no les quitan vida:
    // avivan un fuego en ellos (fire.add_heat).
    // vision: nadie elige por sí mismo un blanco que su jugador no ve (null = lo ve todo).
    void update(entt::registry& registry, MovementSystem& movement, EconomySystem& economy, FireSystem& fire,
                std::uint32_t& next_order_id, Tick tick, const VisionSystem* vision = nullptr);

    // Daño de un golpe de una unidad (tipo y porcentaje de ataque) sobre un blanco.
    // Con las mejoras de la herrería (C1) del atacante (attacker_owner) y del blanco.
    [[nodiscard]] std::int32_t damage(UnitTypeId attacker_type, PlayerId attacker_owner, std::int32_t attack_percent,
                                      const entt::registry& registry, entt::entity target) const;
    [[nodiscard]] std::int32_t attack_percent(const Combatant& c, bool aura) const noexcept;
    // Con el hambre: el ataque efectivo de la unidad e en este tick.
    [[nodiscard]] std::int32_t attack_percent(const entt::registry& registry, entt::entity e, const Combatant& c,
                                              bool aura) const noexcept;
    // Tirador que gasta munición y no le queda.
    [[nodiscard]] bool out_of_ammo(const entt::registry& registry, entt::entity e) const noexcept;
    [[nodiscard]] const CombatParams& params() const noexcept { return params_; }
    [[nodiscard]] const CombatTickStats& last_stats() const noexcept { return stats_; }
    // Golpes a unidades y bajas de este tick, para la moral.
    [[nodiscard]] std::span<const MoraleHit> morale_hits() const noexcept { return morale_hits_; }
    [[nodiscard]] std::span<const MoraleDeath> morale_deaths() const noexcept { return morale_deaths_; }
    // Para la presentación (sonido, F2): dónde cayó cada golpe de este tick y qué se
    // disparó. No entran en el hash: se rehacen cada tick.
    struct Struck {
        FVec2 pos;
        UnitTypeId attacker_type = 0;
        bool building = false;
    };
    [[nodiscard]] std::span<const Struck> struck() const noexcept { return struck_; }
    [[nodiscard]] std::span<const Projectile> new_projectiles() const noexcept { return new_projectiles_; }

    void hash_into(StateHasher& h, const entt::registry& registry) const;

private:
    struct Hit {
        entt::entity target;
        entt::entity attacker;
        std::int32_t amount;
        UnitTypeId attacker_type = 0;  // para las formaciones: caballería, proyectil
    };

    // Rejilla espacial de unidades del tick (ordenación por conteo), índice denso.
    struct Scratch {
        std::vector<entt::entity> entity;
        std::vector<FVec2> pos;
        std::vector<Fixed> radius;
        std::vector<PlayerId> owner;
        std::vector<std::uint8_t> aura;
        std::vector<TargetClass> klass;   // clase como blanco (sin contar a quién ataca)
        std::vector<entt::entity> aiming; // a quién tiene por blanco (null: a nadie)
        std::vector<std::uint8_t> shielded;  // dentro de una torre: no se le puede atacar
        std::vector<std::uint32_t> cell_start;
        std::vector<std::uint32_t> cell_units;
    };

    void gather(const entt::registry& registry);
    [[nodiscard]] std::size_t cell_of(FVec2 p) const noexcept;
    template <typename Fn>
    void for_each_near(FVec2 center, Fixed radius, Fn&& fn) const;
    void mark_auras(const entt::registry& registry);
    [[nodiscard]] bool is_enemy_target(const entt::registry& registry, entt::entity target, PlayerId me) const;
    [[nodiscard]] entt::entity acquire(const entt::registry& registry, std::size_t i, Fixed sight) const;
    // Posición de la clase en combat.target_priority (menor = antes).
    [[nodiscard]] std::size_t priority_of(TargetClass k) const noexcept;
    // Sin unidades enemigas a la vista: el edificio enemigo más cercano dentro de ella.
    [[nodiscard]] entt::entity acquire_building(const entt::registry& registry, const EconomySystem& economy,
                                                std::size_t i, std::int32_t sight_tiles) const;
    [[nodiscard]] bool in_range(const entt::registry& registry, FVec2 pos, Fixed radius, Fixed range,
                                entt::entity target) const;
    [[nodiscard]] FVec2 aim_point(const entt::registry& registry, entt::entity target) const;
    void clear_target(entt::registry& registry, MovementSystem& movement, entt::entity e, Combatant& c,
                      std::uint32_t& next_order_id, Tick tick) const;
    void update_projectiles(entt::registry& registry, const EconomySystem& economy, FireSystem& fire);
    // Golpe de una unidad de tipo attacker_type: daño (unidades, o edificios si es de
    // asedio) o fuego (edificios, el resto).
    void strike(const entt::registry& registry, FireSystem& fire, entt::entity target, entt::entity attacker,
                UnitTypeId attacker_type, PlayerId attacker_owner, std::int32_t percent);
    // Puede hacer algo contra este edificio: dañarlo (asedio) o prenderle fuego.
    [[nodiscard]] bool can_harm_building(const entt::registry& registry, UnitTypeId attacker_type,
                                         entt::entity building) const;
    void apply_hits(entt::registry& registry, MovementSystem& movement, EconomySystem& economy, Tick tick);
    void level_up(entt::registry& registry, entt::entity e, Combatant& c);
    // B2: niveles de altura de ventaja (con signo, acotados) del punto a sobre el blanco.
    [[nodiscard]] std::int32_t slope_levels(const entt::registry& registry, FVec2 a, entt::entity target) const;
    [[nodiscard]] TerrainId terrain_at(FVec2 p) const noexcept;
    [[nodiscard]] bool charge_ground(TerrainId t) const noexcept;  // llano firme: se puede cargar
    [[nodiscard]] bool stops_charge(const entt::registry& registry, entt::entity target) const;
    [[nodiscard]] Fixed effective_range(const CombatStats& st, std::int32_t levels) const noexcept;

    const TileMap* map_;
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
    std::vector<MoraleHit> morale_hits_;
    std::vector<MoraleDeath> morale_deaths_;
    std::vector<Struck> struck_;
    const VisionSystem* vision_ = nullptr;  // del tick en curso
    const EconomySystem* economy_ = nullptr;  // del tick en curso (mejoras de la herrería)
    CombatTickStats stats_;
};

}  // namespace rts::sim
