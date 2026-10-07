#pragma once

// Objetivos de escenario (F4): lo que gana o pierde una partida de campaña además de la
// conquista. Son reglas de la simulación (deterministas, en el hash); sin objetivos no
// hacen nada y no tocan el hash, así que las partidas normales no cambian.
//
// Un jugador gana cuando ha cumplido todos sus objetivos (los de conservar cuentan
// mientras no fallen; hace falta al menos uno de otro tipo). Pierde si falla uno de
// conservar o si otro gana antes. El primero que gana decide la partida.

#include <cstdint>
#include <optional>
#include <vector>

#include <entt/entity/fwd.hpp>

#include "sim/economy.hpp"
#include "sim/tick.hpp"
#include "sim/units.hpp"

namespace rts::sim {

class StateHasher;

enum class ObjectiveKind : std::uint8_t {
    Destroy,  // que no quede nada del tipo (unidades o edificios de target, en la zona)
    Keep,     // que quede algo del tipo (de target); si no queda, falla
    Survive,  // llegar al tick `ticks`
    Reach,    // tener `count` unidades propias del tipo (o tropa) en la zona
    Gather,   // tener `count` de `resource` en el almacén
    Defeat,   // que target quede derrotado
};

enum class ObjectiveStatus : std::uint8_t { Pending, Done, Failed };

inline constexpr std::uint8_t kAnyObjectType = 0xFF;

struct Objective {
    ObjectiveKind kind = ObjectiveKind::Survive;
    PlayerId player = 0;  // de quién es el objetivo
    PlayerId target = 0;  // dueño de lo que se destruye, conserva o derrota
    bool units = false;   // Destroy, Keep, Reach: `type` es un tipo de unidad (si no, de edificio)
    std::uint8_t type = kAnyObjectType;  // kAnyObjectType: cualquiera (Reach: cualquier tropa)
    std::int32_t count = 1;              // Reach: unidades; Gather: cantidad
    Resource resource = Resource::Food;  // Gather
    bool zoned = false;                  // con zona; si no, todo el mapa
    TileCoord zone_min;                  // esquinas de la zona, incluidas
    TileCoord zone_max;
    Tick ticks = 0;                      // Survive

    [[nodiscard]] bool in_zone(TileCoord c) const noexcept {
        return !zoned || (c.x >= zone_min.x && c.y >= zone_min.y && c.x <= zone_max.x && c.y <= zone_max.y);
    }
};

// data/config/engine.toml, sección [objectives].
struct ObjectiveParams {
    std::int32_t check_every_ticks = 1;  // cada cuánto se comprueban
};

class ObjectiveSystem {
public:
    ObjectiveSystem(const ObjectiveParams& params, std::vector<Objective> objectives, std::vector<UnitType> unit_types,
                    std::size_t player_count);

    // Al final del tick (después del combate). No hace nada sin objetivos o ya decidida.
    void update(const entt::registry& registry, const EconomySystem& economy, Tick tick);

    [[nodiscard]] bool active() const noexcept { return !objectives_.empty(); }
    [[nodiscard]] const std::vector<Objective>& objectives() const noexcept { return objectives_; }
    [[nodiscard]] const std::vector<ObjectiveStatus>& status() const noexcept { return status_; }
    [[nodiscard]] std::optional<PlayerId> winner() const noexcept { return winner_; }
    // Ha fallado un objetivo de conservar, o ha ganado otro.
    [[nodiscard]] bool lost(PlayerId p) const noexcept;

    void hash_into(StateHasher& h) const;

private:
    [[nodiscard]] std::int32_t count_matching(const entt::registry& registry, const Objective& o, PlayerId owner) const;

    ObjectiveParams params_;
    std::vector<Objective> objectives_;
    std::vector<ObjectiveStatus> status_;
    std::vector<UnitType> unit_types_;
    std::size_t player_count_ = 0;
    std::optional<PlayerId> winner_;
};

}  // namespace rts::sim
