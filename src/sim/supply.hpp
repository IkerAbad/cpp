#pragma once

// Suministro (M6): víveres y munición. Cada unidad gasta raciones con el tiempo (la
// caballería, también forraje) y los tiradores, munición al disparar. Se reponen poco
// a poco junto a un edificio propio que abastece (centro urbano, almacén, campamento),
// pagando del almacén del jugador: un ejército lejos de casa vive de lo que lleva.
//
// Sin raciones, la unidad está hambrienta: su ataque baja (CombatParams) y los
// aldeanos trabajan más despacio (EconomyParams). Si el hambre dura, las tropas
// pierden vida hasta morir; los aldeanos, no.
//
// Determinismo: recorrido en el orden de la vista; cada unidad mira si puede
// reabastecerse una vez cada resupply_interval_ticks, repartidas por id.

#include <cstdint>
#include <vector>

#include <entt/entity/registry.hpp>

#include "sim/economy.hpp"
#include "sim/fixed.hpp"
#include "sim/tick.hpp"
#include "sim/units.hpp"

namespace rts::sim {

class StateHasher;

// data/config/engine.toml, sección [supply].
struct SupplyParams {
    std::int32_t resupply_radius_tiles = 0;    // distancia máxima a la huella del edificio que abastece
    std::int32_t resupply_interval_ticks = 1;  // cada unidad se reabastece como mucho una vez por intervalo
    Stock ration_cost{};                       // coste de cada ración repuesta
    std::int32_t starve_after_ticks = 0;       // hambre que se aguanta sin perder vida
    std::int32_t starve_hp_interval_ticks = 1; // después, 1 de vida cada tantos ticks
};

struct SupplyTickStats {
    std::int32_t rations_issued = 0;
    std::int32_t ammo_issued = 0;
    std::int32_t hungry = 0;
    std::int32_t starved = 0;  // muertas de hambre este tick
};

class SupplySystem {
public:
    SupplySystem(const SupplyParams& params, std::vector<UnitType> units, std::vector<BuildingType> buildings);

    void update(entt::registry& registry, EconomySystem& economy, Tick tick);

    // Edificio propio que abastece al alcance de la unidad, o null. Los aldeanos comen
    // también donde descargan: les vale cualquier almacén propio en uso.
    [[nodiscard]] entt::entity source_near(const entt::registry& registry, const EconomySystem& economy,
                                           PlayerId player, FVec2 pos, bool worker) const;

    [[nodiscard]] const SupplyParams& params() const noexcept { return params_; }
    [[nodiscard]] const SupplyTickStats& last_stats() const noexcept { return stats_; }
    void hash_into(StateHasher& h, const entt::registry& registry) const;

private:
    SupplyParams params_;
    std::vector<UnitType> units_;
    std::vector<BuildingType> buildings_;
    std::vector<entt::entity> dead_;
    SupplyTickStats stats_;
};

}  // namespace rts::sim
