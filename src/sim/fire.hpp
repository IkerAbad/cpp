#pragma once

// Fuego en edificios. Sin armas de asedio no se hace daño a un edificio: las demás
// unidades le prenden fuego (cada golpe, antorcha o flecha incendiaria, aviva su
// intensidad). Un fuego por debajo de la intensidad de sostén se apaga solo; por
// encima crece por sí mismo, quema vida en proporción a su intensidad y, muy vivo,
// prende los edificios de madera vecinos. Cualquier unidad puede apagarlo
// (Extinguish), cada tipo con su eficiencia.
//
// La madera arde hasta caer. En la piedra el fuego consume tejado e interior: al
// bajar la vida hasta stone_floor_percent el edificio queda quemado (inutilizado) y
// en pie, y el fuego se extingue por falta de combustible.
//
// Determinismo: los aportes de un tick (golpes, apagado, propagación) se acumulan y
// se aplican de una vez; los edificios caídos se retiran al final.

#include <cstdint>
#include <span>
#include <vector>

#include <entt/entity/registry.hpp>

#include "sim/economy.hpp"
#include "sim/fixed.hpp"
#include "sim/movement.hpp"
#include "sim/tick.hpp"
#include "sim/units.hpp"

namespace rts::sim {

class StateHasher;

// data/config/engine.toml, sección [fire]. Intensidades en una escala 0..max_intensity.
struct FireParams {
    std::int32_t max_intensity = 1;
    std::int32_t sustain_intensity = 0;  // por debajo, el fuego mengua y se apaga solo
    std::int32_t decay_per_tick = 0;     // lo que mengua por tick bajo el sostén (o sin combustible)
    std::int32_t growth_wood_per_tick = 0;
    std::int32_t growth_stone_per_tick = 0;
    // Vida quemada por tick a la intensidad máxima, en milésimas de punto de vida.
    std::int32_t burn_wood_milli_per_tick = 0;
    std::int32_t burn_stone_milli_per_tick = 0;
    std::int32_t stone_floor_percent = 0;  // el fuego no baja la piedra de este % de su vida
    std::int32_t spread_intensity = 0;     // desde esta intensidad prende a los vecinos de madera
    std::int32_t spread_per_tick = 0;      // fuego que pasa a cada vecino por tick
    std::int32_t spread_gap_tiles = 0;     // vecinos: a esta distancia o menos de la huella
    Fixed extinguish_reach;                // holgura entre la unidad y la huella para apagar
};

// Fuego activo en un edificio.
struct Fire {
    std::int32_t intensity = 0;
    std::int32_t burn_acc = 0;  // milésimas de vida quemadas aún sin descontar
};

// Unidad apagando un edificio propio.
struct Extinguisher {
    entt::entity building = entt::null;
    std::uint32_t move_order = 0;  // desplazamiento propio hasta el edificio (0 = ninguno)
};

struct FireTickStats {
    std::int32_t burning = 0;
    std::int32_t ignited = 0;   // fuegos nuevos este tick (golpes o propagación)
    std::int32_t burned_down = 0;
    std::int32_t gutted = 0;    // edificios de piedra quemados este tick
};

class FireSystem {
public:
    FireSystem(const FireParams& params, std::vector<UnitType> units, std::vector<BuildingType> buildings);

    // Extinguish asigna la tarea; cualquier otra orden sobre unidades la cancela.
    void apply(entt::registry& registry, MovementSystem& movement, const Command& command,
               std::span<const entt::entity> units, std::uint32_t& next_order_id, Tick tick);
    // Aporte de fuego a un edificio (golpe incendiario). Se aplica en el siguiente update().
    void add_heat(entt::entity building, std::int32_t amount);
    void update(entt::registry& registry, MovementSystem& movement, EconomySystem& economy,
                std::uint32_t& next_order_id, Tick tick);

    [[nodiscard]] const FireParams& params() const noexcept { return params_; }
    [[nodiscard]] const FireTickStats& last_stats() const noexcept { return stats_; }
    void hash_into(StateHasher& h, const entt::registry& registry) const;

private:
    struct Heat {
        entt::entity building;
        std::int32_t amount;  // positivo: aviva; negativo: apaga
    };

    void update_extinguishers(entt::registry& registry, MovementSystem& movement, std::uint32_t& next_order_id,
                              Tick tick);
    void spread_from(const entt::registry& registry, const EconomySystem& economy, entt::entity building);

    FireParams params_;
    std::vector<UnitType> units_;
    std::vector<BuildingType> buildings_;
    std::vector<Heat> heat_;
    std::vector<entt::entity> scratch_;
    std::vector<entt::entity> spread_seen_;
    FireTickStats stats_;
};

}  // namespace rts::sim
