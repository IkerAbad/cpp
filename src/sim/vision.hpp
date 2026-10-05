#pragma once

// Niebla de guerra. Solo se sabe lo que alguien ha visto. Para cada jugador y casilla:
//   sin explorar -> explorada (se ha visto alguna vez) -> visible ahora.
// La vista sale de sus unidades y edificios (sight_tiles), y es realista:
//   - los árboles (nodos que tapan la vista) dejan ver cover_depth_tiles casillas de
//     bosque; más allá, nada. Una unidad entre árboles solo se ve de cerca
//     (spot_in_cover_tiles); un edificio, por su tamaño, en cuanto se ve una casilla
//     de su huella. Talar abre la vista;
//   - la altura: desde arriba se ve más lejos cuesta abajo (elevation_sight_per_level
//     casillas por nivel de diferencia) y una loma más alta que ambos extremos tapa lo
//     que hay detrás;
//   - el día: de noche la vista baja a night_sight_percent, con amanecer y anochecer
//     graduales.
// De los edificios enemigos se recuerda el último estado visto: si caen sin que nadie
// lo vea, siguen figurando hasta que se vuelva a mirar.
//
// Lo calcula la simulación (la IA lo usa, así que entra en el hash) cada
// interval_ticks. Desactivada (enabled = false), todo es visible: es el mundo de las
// pruebas de movimiento, combate y economía.

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <entt/entity/registry.hpp>

#include "sim/economy.hpp"
#include "sim/fog.hpp"
#include "sim/tick.hpp"
#include "sim/tile_map.hpp"
#include "sim/units.hpp"

namespace rts::sim {

class StateHasher;

// data/config/engine.toml, sección [vision].
struct VisionParams {
    bool enabled = false;
    std::int32_t interval_ticks = 1;
    std::int32_t elevation_sight_per_level = 0;
    std::int32_t cover_depth_tiles = 0;
    std::int32_t spot_in_cover_tiles = 0;
    // Día: day_ticks de luz (los primeros y los últimos twilight_ticks, amanecer y
    // anochecer) y night_ticks de noche; la partida empieza en el tick start_tick del ciclo.
    std::int32_t day_ticks = 1;
    std::int32_t night_ticks = 0;
    std::int32_t twilight_ticks = 0;
    std::int32_t night_sight_percent = 100;
    std::int32_t start_tick = 0;
};

// Edificio enemigo tal como se vio por última vez.
struct RememberedBuilding {
    entt::entity entity = entt::null;
    BuildingTypeId type = 0;
    PlayerId owner = 0;
    Footprint footprint;
    std::int32_t hp = 0;
    bool complete = false;
    bool burned = false;
    bool burning = false;
};

class VisionSystem {
public:
    VisionSystem(const VisionParams& params, const TileMap& map, std::vector<UnitType> units,
                 std::vector<BuildingType> buildings, std::vector<ResourceNodeType> nodes, std::int32_t players);

    // Recalcula la visibilidad si toca (cada interval_ticks) o si se pide (force).
    void update(const entt::registry& registry, Tick tick, bool force = false);

    [[nodiscard]] bool enabled() const noexcept { return params_.enabled; }
    // % de la vista diurna en este tick (100 de día, night_sight_percent de noche).
    [[nodiscard]] std::int32_t daylight_percent(Tick tick) const noexcept;
    [[nodiscard]] bool visible(PlayerId p, TileCoord c) const noexcept;
    [[nodiscard]] bool explored(PlayerId p, TileCoord c) const noexcept;
    // ¿Ve el jugador p esta unidad o este edificio? (los suyos, siempre)
    [[nodiscard]] bool sees_unit(const entt::registry& registry, PlayerId p, entt::entity e) const;
    [[nodiscard]] bool sees_footprint(PlayerId p, const Footprint& f) const noexcept;
    [[nodiscard]] std::span<const RememberedBuilding> memory(PlayerId p) const noexcept;
    // Capa para la presentación (Fog por casilla); null si está desactivada.
    [[nodiscard]] std::shared_ptr<const std::vector<std::uint8_t>> fog_layer(PlayerId p) const;
    void hash_into(StateHasher& h) const;

private:
    static constexpr std::uint8_t kVisible = 1;
    static constexpr std::uint8_t kNear = 2;
    static constexpr std::uint8_t kExplored = 4;

    struct Observer {
        TileCoord tile;
        std::int32_t sight;
        bool operator==(const Observer&) const = default;
    };

    [[nodiscard]] std::size_t index(TileCoord c) const noexcept {
        return static_cast<std::size_t>(c.y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(c.x);
    }
    [[nodiscard]] bool line_clear(TileCoord from, TileCoord to, std::int32_t h_from, std::int32_t h_to) const noexcept;
    void look(std::vector<std::uint8_t>& flags, const Observer& o, std::int32_t daylight) const;
    void remember(const entt::registry& registry, PlayerId p);

    VisionParams params_;
    const TileMap& map_;
    std::int32_t width_;
    std::int32_t height_;
    std::vector<UnitType> units_;
    std::vector<BuildingType> buildings_;
    std::vector<ResourceNodeType> nodes_;
    std::vector<std::uint8_t> cover_;               // casilla con árboles que tapan la vista
    std::vector<std::vector<std::uint8_t>> flags_;  // por jugador y casilla: kVisible | kNear | kExplored
    std::vector<std::vector<RememberedBuilding>> memory_;
    std::vector<std::shared_ptr<const std::vector<std::uint8_t>>> fog_;
    std::vector<Observer> observers_;
    std::uint64_t digest_ = 0;  // resumen del estado, rehecho en cada cálculo
};

}  // namespace rts::sim
