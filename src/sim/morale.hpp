#pragma once

// Moral y desbandada (B1). Las batallas antiguas y medievales se decidían más por la
// huida que por la muerte de todos: el vencido sufría casi todas sus bajas en la
// persecución, y lo que provocaba la huida era el miedo, sobre todo a un ataque por el
// flanco o la retaguardia (Ardant du Picq, «Battle Studies»).
//
// Cada unidad combatiente con firmeza (UnitType::morale_resolve > 0) tiene una moral de
// 0 a kFullMorale. Baja con las bajas propias cercanas, los golpes recibidos (mucho más
// si vienen del flanco o la retaguardia, es decir, de quien no tiene delante), los
// compañeros que huyen cerca (el pánico se contagia), verse superada en número, el
// hambre y la muerte de un héroe; de noche, todo pesa más. Sube con la calma, con los
// compañeros alrededor, con un héroe cerca y con las bajas del enemigo. La firmeza del
// tipo escala las pérdidas: la leva cede antes que el caballero.
//
// Por debajo de rout_below la unidad se desbanda (Routing): deja de pelear, huye lejos
// del enemigo y no obedece órdenes. Se rehace cuando su moral vuelve a rally_above y no
// tiene enemigos a rally_safe_tiles; entonces se reorganiza un rato (Reorganizing).
//
// Determinismo: los sucesos del tick (golpes y bajas) se aplican en el orden en que los
// deja el combate (ordenados); la revisión periódica, de todas las unidades a la vez
// cada interval_ticks, recorre una rejilla espacial ordenada por conteo. Los ticks sin
// sucesos ni revisión no cuestan nada.

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

inline constexpr std::int32_t kFullMorale = 1000;

// data/config/engine.toml, sección [morale]. Las pérdidas y ganancias son milésimas de
// moral; las pérdidas se escalan por 100 / morale_resolve del tipo.
struct MoraleParams {
    bool enabled = false;
    std::int32_t interval_ticks = 1;  // revisión de todas las unidades cada tantos ticks
    std::int32_t rout_below = 0;
    std::int32_t rally_above = 0;
    Fixed awareness_radius;           // casillas: bajas, compañeros y enemigos que cuentan
    Fixed rally_safe_radius;          // sin enemigos a esta distancia se puede rehacer
    // Sucesos.
    std::int32_t casualty_loss = 0;       // por cada baja propia cercana
    std::int32_t enemy_casualty_gain = 0; // por cada baja enemiga cercana
    std::int32_t hit_loss = 0;            // por golpe recibido de frente
    std::int32_t flank_hit_loss = 0;      // por golpe recibido de flanco o retaguardia
    std::int32_t hero_death_loss = 0;     // muere un héroe propio
    Fixed hero_death_radius;
    // Revisión periódica.
    std::int32_t contagion_loss = 0;      // por cada compañero cercano en desbandada
    std::int32_t outnumbered_loss = 0;    // más enemigos que compañeros cerca
    std::int32_t hungry_loss = 0;
    std::int32_t calm_ticks = 0;          // sin recibir daño: en calma
    std::int32_t calm_gain = 0;
    std::int32_t comrade_gain = 0;        // por compañero cercano (hasta comrade_cap), en calma
    std::int32_t comrade_cap = 0;
    std::int32_t hero_gain = 0;           // héroe propio cerca (aura), en calma
    std::int32_t hero_loss_percent = 100; // con un héroe cerca, las pérdidas a este %
    std::int32_t night_loss_percent = 100;
    std::int32_t routing_gain = 0;        // huyendo, sin enemigos cerca
    // Huida.
    std::int32_t flee_tiles = 0;
    std::int32_t flee_repath_ticks = 1;
    std::int32_t rally_reorganize_ticks = 0;
};

// Suceso de combate que pesa en la moral.
struct MoraleHit {
    entt::entity target = entt::null;
    entt::entity attacker = entt::null;
};

struct MoraleDeath {
    FVec2 pos;
    PlayerId owner = 0;
    bool hero = false;
};

struct MoraleTickStats {
    std::int32_t routed = 0;   // desbandadas este tick
    std::int32_t rallied = 0;  // se rehacen este tick
    std::int32_t routing = 0;  // en desbandada ahora
};

class MoraleSystem {
public:
    MoraleSystem(std::int32_t width, std::int32_t height, const MoraleParams& params, std::vector<UnitType> units,
                 Fixed hero_aura_radius);

    // hits y deaths: los del combate de este tick. daylight: % de luz (100 de día).
    void update(entt::registry& registry, MovementSystem& movement, std::span<const MoraleHit> hits,
                std::span<const MoraleDeath> deaths, std::int32_t daylight, std::uint32_t& next_order_id,
                Tick tick);

    [[nodiscard]] bool enabled() const noexcept { return params_.enabled; }
    [[nodiscard]] const MoraleParams& params() const noexcept { return params_; }
    [[nodiscard]] const MoraleTickStats& last_stats() const noexcept { return stats_; }
    // Desbandadas acumuladas por jugador (estadística, no forma parte del estado).
    [[nodiscard]] std::int32_t routs_of(PlayerId p) const noexcept {
        return p < routs_.size() ? routs_[p] : 0;
    }
    void hash_into(StateHasher& h, const entt::registry& registry) const;

private:
    struct Scratch {
        std::vector<entt::entity> entity;
        std::vector<FVec2> pos;
        std::vector<PlayerId> owner;
        std::vector<std::uint8_t> armed;    // combatiente que puede herir a la tropa
        std::vector<std::uint8_t> routing;
        std::vector<std::uint8_t> hero;
        std::vector<std::uint32_t> cell_start;
        std::vector<std::uint32_t> cell_units;
    };

    [[nodiscard]] bool has_morale(const entt::registry& registry, entt::entity e) const;
    void gather(const entt::registry& registry);
    [[nodiscard]] std::size_t cell_of(FVec2 p) const noexcept;
    template <typename Fn>
    void for_each_near(FVec2 center, Fixed radius, Fn&& fn) const;
    // Pérdida escalada por la firmeza del tipo, la noche y el héroe cercano.
    [[nodiscard]] std::int32_t scaled_loss(const entt::registry& registry, entt::entity e, std::int32_t loss,
                                           bool hero_near) const;
    void lose(entt::registry& registry, entt::entity e, std::int32_t loss, bool hero_near);
    void apply_events(entt::registry& registry, std::span<const MoraleHit> hits, std::span<const MoraleDeath> deaths);
    void review(entt::registry& registry, MovementSystem& movement, std::uint32_t& next_order_id, Tick tick);
    void rout(entt::registry& registry, MovementSystem& movement, entt::entity e, std::uint32_t& next_order_id,
              Tick tick);
    void flee(entt::registry& registry, MovementSystem& movement, entt::entity e, std::size_t i,
              std::uint32_t& next_order_id, Tick tick);

    std::int32_t width_;
    std::int32_t height_;
    MoraleParams params_;
    std::vector<UnitType> units_;
    Fixed hero_aura_radius_;
    std::int32_t daylight_ = 100;
    Scratch s_;
    std::vector<std::int32_t> index_of_;  // entidad -> índice en s_ (por id, -1 = no está)
    std::vector<std::int32_t> routs_;
    MoraleTickStats stats_;
};

}  // namespace rts::sim
