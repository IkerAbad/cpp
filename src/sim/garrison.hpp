#pragma once

// Guarnición de torres (B4). Las unidades de combate van a una torre propia (orden
// Garrison) y entran si hay plaza. Dentro no se mueven ni se las puede atacar; los
// tiradores disparan desde lo alto, con los niveles de altura de la torre sumados a los
// del terreno (B2). Cualquier otra orden las saca, igual que vaciar la torre (Garrison
// con kUngarrison); si la torre cae, la guarnición sale a su alrededor.
//
// Determinismo: recorrido en el orden de las vistas; las plazas se cuentan antes de
// dejar entrar a nadie y se reparten por orden de entidad.

#include <cstdint>
#include <span>
#include <vector>

#include <entt/entity/registry.hpp>

#include "sim/economy.hpp"
#include "sim/movement.hpp"
#include "sim/tick.hpp"
#include "sim/units.hpp"

namespace rts::sim {

class StateHasher;

// Unidad de la guarnición de una torre: camino de ella (inside = false) o dentro (sin
// Velocity: el movimiento no la ve).
struct Garrisoned {
    entt::entity tower = entt::null;
    Footprint tower_footprint;     // para salir aunque la torre ya no exista
    bool inside = false;
    std::uint32_t move_order = 0;  // camino de la torre
    std::int32_t approaches = 0;   // llegadas sin alcanzarla (a la segunda, desiste)
};

// data/config/engine.toml, sección [garrison].
struct GarrisonParams {
    Fixed enter_reach;  // holgura entre la unidad y la huella de la torre para entrar
};

class GarrisonSystem {
public:
    GarrisonSystem(const GarrisonParams& params, std::vector<UnitType> units, std::vector<BuildingType> buildings);

    // Antes que los demás sistemas: así quien estaba dentro sale y puede cumplir la orden.
    void apply(entt::registry& registry, MovementSystem& movement, const EconomySystem& economy,
               const Command& command, std::span<const entt::entity> units, std::uint32_t& next_order_id,
               Tick tick);
    void update(entt::registry& registry, MovementSystem& movement, const EconomySystem& economy,
                std::uint32_t& next_order_id, Tick tick);

    // Puede guarnecer (unidad de combate que no es aldeano, bagaje ni ingenio).
    [[nodiscard]] bool can_garrison(const entt::registry& registry, entt::entity e) const;
    [[nodiscard]] bool is_tower(const entt::registry& registry, entt::entity b, PlayerId player) const;
    // Niveles de altura que da la torre en la que está e (0 si no está dentro de ninguna).
    [[nodiscard]] std::int32_t tower_levels(const entt::registry& registry, entt::entity e) const;
    void hash_into(StateHasher& h, const entt::registry& registry) const;

private:
    void enter(entt::registry& registry, entt::entity e, Garrisoned& g) const;
    void leave(entt::registry& registry, const MovementSystem& movement, const EconomySystem& economy,
               entt::entity e) const;
    [[nodiscard]] std::int32_t inside_count(const entt::registry& registry, entt::entity tower) const;

    GarrisonParams params_;
    std::vector<UnitType> units_;
    std::vector<BuildingType> buildings_;
};

}  // namespace rts::sim
