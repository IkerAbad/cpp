#pragma once

// Escalas de asedio (B4). La infantería que sabe escalar (UnitType::climbs) puede tomar
// un tramo de muro enemigo (BuildingType::climbable) con la orden Climb: paga la escala
// (ladder_cost, del almacén del jugador), va al pie del muro, sube durante climb_ticks
// —sin pelear y más expuesta: el daño que recibe sube a exposed_percent— y baja por el
// otro lado, a la casilla opuesta a la que subió. Si esa casilla está ocupada o cerrada,
// la escalada fracasa y se queda al pie del muro.
//
// Determinismo: recorrido en el orden de la vista, con las llegadas ordenadas por entidad.

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

// Unidad que escala un tramo de muro: camino de él (timer < 0) o subiendo.
struct Climbing {
    entt::entity wall = entt::null;
    TileCoord wall_tile;
    TileCoord from;                // casilla desde la que sube
    std::int32_t timer = -1;       // ticks subiendo (-1: aún no ha llegado)
    std::uint32_t move_order = 0;
    std::int32_t approaches = 0;   // llegadas sin alcanzarlo (a la segunda, desiste)
};

// data/config/engine.toml, sección [climb].
struct ClimbParams {
    Stock ladder_cost{};
    std::int32_t climb_ticks = 1;
    Fixed reach;                   // holgura entre la unidad y el muro para empezar a subir
    std::int32_t exposed_percent = 100;
};

struct ClimbTickStats {
    std::int32_t climbed = 0;  // escaladas logradas este tick
    std::int32_t failed = 0;   // escaladas fallidas (no había dónde bajar)
};

class ClimbSystem {
public:
    ClimbSystem(const ClimbParams& params, std::vector<UnitType> units, std::vector<BuildingType> buildings);

    // Climb: quienes saben escalar van al muro, si el jugador paga sus escalas. Cualquier
    // otra orden (salvo la postura) anula la escalada en curso.
    void apply(entt::registry& registry, MovementSystem& movement, EconomySystem& economy, const Command& command,
               std::span<const entt::entity> units, std::uint32_t& next_order_id, Tick tick);
    void update(entt::registry& registry, MovementSystem& movement, const EconomySystem& economy,
                std::uint32_t& next_order_id, Tick tick);

    [[nodiscard]] bool is_climbable(const entt::registry& registry, entt::entity b, PlayerId attacker) const;
    [[nodiscard]] const ClimbParams& params() const noexcept { return params_; }
    [[nodiscard]] const ClimbTickStats& last_stats() const noexcept { return stats_; }
    void hash_into(StateHasher& h, const entt::registry& registry) const;

private:
    ClimbParams params_;
    std::vector<UnitType> units_;
    std::vector<BuildingType> buildings_;
    ClimbTickStats stats_;
};

}  // namespace rts::sim
