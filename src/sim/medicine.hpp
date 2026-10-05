#pragma once

// Sanidad: puestos médicos, enfermeros y heridas. Un herido no vuelve solo al combate:
// hay que llevarlo a un puesto médico (orden Treat). Ingresado queda inoperativo y sin
// bienes (pierde víveres y munición), se le puede atacar y, si el puesto cae, muere.
// Le curan el tiempo (reposo) y los enfermeros: aldeanos que atienden el puesto (orden
// Tend), cada uno a unos pocos pacientes. La calidad del puesto (care_percent) escala
// el ritmo, y su alcance (heal_to_percent) dice hasta dónde cura: un puesto de socorro
// solo estabiliza. Los pacientes comen como un aldeano, del almacén del jugador; sin
// comida no mejoran. Al alta, sin bienes, tardan en reorganizarse y armarse.
//
// Fuera de los puestos, solo las heridas leves sanan solas, y en calma: unidad
// curable, comida, con la vida por encima de light_wound_percent y sin daño reciente.
//
// Determinismo: recorrido en el orden de las vistas; los cuidados de cada puesto se
// reparten por orden de entidad.

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

// data/config/engine.toml, sección [medicine].
struct MedicineParams {
    std::int32_t bed_heal_milli_per_tick = 0;    // reposo: milésimas de vida por tick
    std::int32_t nurse_heal_milli_per_tick = 0;  // lo que añade estar atendido
    std::int32_t patients_per_nurse = 1;
    std::int32_t patient_ration_ticks = 1;       // un paciente come como un aldeano
    std::int32_t reorganize_ticks = 0;           // tras el alta, sin poder atacar
    Fixed care_reach;                            // holgura entre la unidad y la huella del puesto
    // Heridas leves: con la vida en este % o más sanan solas, 1 punto cada
    // natural_heal_interval_ticks (0 = nunca), si no ha recibido daño en calm_ticks.
    std::int32_t light_wound_percent = 0;
    std::int32_t natural_heal_interval_ticks = 0;
    std::int32_t calm_ticks = 0;
};

struct MedicineTickStats {
    std::int32_t patients = 0;     // ingresados
    std::int32_t admitted = 0;     // ingresos este tick
    std::int32_t discharged = 0;   // altas este tick
    std::int32_t died = 0;         // muertos al caer su puesto
};

class MedicineSystem {
public:
    MedicineSystem(const MedicineParams& params, const Stock& ration_cost, std::vector<UnitType> units,
                   std::vector<BuildingType> buildings);

    // Treat: los heridos curables van al puesto. Cualquier otra orden (salvo la
    // postura) saca de él a quien estaba ingresado, que tendrá que reorganizarse.
    void apply(entt::registry& registry, MovementSystem& movement, const Command& command,
               std::span<const entt::entity> units, std::uint32_t& next_order_id, Tick tick);
    void update(entt::registry& registry, MovementSystem& movement, EconomySystem& economy,
                std::uint32_t& next_order_id, Tick tick);

    // Vida hasta la que cura un puesto a una unidad con esa vida máxima.
    [[nodiscard]] std::int32_t heal_target(const BuildingType& post, std::int32_t max_hp) const noexcept;
    [[nodiscard]] bool is_post(const entt::registry& registry, entt::entity b, PlayerId player) const;
    [[nodiscard]] const MedicineParams& params() const noexcept { return params_; }
    [[nodiscard]] const MedicineTickStats& last_stats() const noexcept { return stats_; }
    void hash_into(StateHasher& h, const entt::registry& registry) const;

private:
    void discharge(entt::registry& registry, entt::entity e) const;
    void update_patients(entt::registry& registry, MovementSystem& movement, EconomySystem& economy,
                         std::uint32_t& next_order_id, Tick tick);
    void natural_healing(entt::registry& registry, Tick tick) const;

    MedicineParams params_;
    Stock ration_cost_;
    std::vector<UnitType> units_;
    std::vector<BuildingType> buildings_;
    std::vector<entt::entity> scratch_;
    std::vector<entt::entity> dead_;
    MedicineTickStats stats_;
};

}  // namespace rts::sim
