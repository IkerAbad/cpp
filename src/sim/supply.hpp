#pragma once

// Suministro (M6): víveres, munición y convoyes. Cada unidad gasta raciones con el
// tiempo (la caballería, también forraje) y los tiradores, munición al disparar. Se
// reponen poco a poco junto a una fuente propia, que paga lo que da:
//   - un edificio que abastece (centro urbano, molino, cuartel): del almacén del jugador;
//   - un campamento de campaña: de su propio almacén, que llenan los convoyes;
//   - una acémila o carreta cargada: de su carga (depósito móvil; sus animales
//     también comen de ella).
// Un ejército lejos de casa vive de lo que lleva y de lo que le llega.
//
// Sin raciones, la unidad está hambrienta: su ataque baja (CombatParams) y los
// aldeanos trabajan más despacio (EconomyParams). Si el hambre dura, las tropas
// pierden vida hasta morir; los aldeanos, no.
//
// Convoyes: la orden Convoy a un campamento hace que el bagaje cargue en el edificio
// de casa que abastece más cercano, descargue en el campamento y repita; a otro
// edificio que abastece, que cargue allí y se quede.
//
// Determinismo: recorrido en el orden de las vistas; cada unidad mira si puede
// reabastecerse una vez cada resupply_interval_ticks, repartidas por id.

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

// data/config/engine.toml, sección [supply].
struct SupplyParams {
    std::int32_t resupply_radius_tiles = 0;    // distancia máxima a la fuente (huella o bagaje)
    std::int32_t resupply_interval_ticks = 1;  // cada unidad se reabastece como mucho una vez por intervalo
    Stock ration_cost{};                       // coste de cada ración repuesta
    std::int32_t starve_after_ticks = 0;       // hambre que se aguanta sin perder vida
    std::int32_t starve_hp_interval_ticks = 1; // después, 1 de vida cada tantos ticks
    // Convoyes: reparto de la carga por recurso (en %, suma 100), ticks que se tarda en
    // cargar o descargar y holgura entre el bagaje y la huella del edificio.
    Stock convoy_mix{};
    std::int32_t load_ticks = 0;
    Fixed convoy_reach;
    // Forrajeo y saqueo (C2): la tropa parada, sin pelear y con raciones por llenar se
    // sirve sola a forage_reach_tiles: del grano de las granjas enemigas (que se vacían),
    // de la comida del rival junto a sus almacenes de comida (la saquea) y, si no, del
    // bosque, gratis pero solo una ración cada forest_forage_ticks.
    bool forage = false;
    std::int32_t forage_reach_tiles = 0;
    std::int32_t forest_forage_ticks = 1;
};

// Almacén propio de un campamento de campaña.
struct SupplyStore {
    Stock stock{};
};

enum class ConvoyTask : std::uint8_t { Idle, Load, Unload };

// Bagaje: lo que lleva y su ruta.
struct Carrier {
    Stock load{};
    ConvoyTask task = ConvoyTask::Idle;
    entt::entity home = entt::null;  // edificio donde carga (null: el más cercano que abastece)
    entt::entity camp = entt::null;  // campamento al que lleva (null: se queda donde cargó)
    std::int32_t timer = 0;          // ticks cargando o descargando
    std::uint32_t move_order = 0;    // desplazamiento propio en curso (0 = ninguno)
};

struct SupplyTickStats {
    std::int32_t rations_issued = 0;
    std::int32_t ammo_issued = 0;
    std::int32_t hungry = 0;
    std::int32_t starved = 0;   // muertas de hambre este tick
    std::int32_t loaded = 0;    // cargas de bagaje completadas
    std::int32_t unloaded = 0;  // descargas en campamentos
    std::int32_t pillaged = 0;  // raciones sacadas al enemigo (granjas y almacenes)
    std::int32_t foraged = 0;   // raciones sacadas del bosque
};

class SupplySystem {
public:
    SupplySystem(const SupplyParams& params, std::vector<UnitType> units, std::vector<BuildingType> buildings);

    // Convoy asigna la ruta al bagaje; cualquier otra orden la cancela (conserva la carga).
    void apply(entt::registry& registry, MovementSystem& movement, const Command& command,
               std::span<const entt::entity> units, std::uint32_t& next_order_id, Tick tick);
    void update(entt::registry& registry, MovementSystem& movement, EconomySystem& economy,
                std::uint32_t& next_order_id, Tick tick);
    // Raciones saqueadas al jugador p (estadística; no forma parte del estado).
    [[nodiscard]] std::int32_t pillaged_from(PlayerId p) const noexcept {
        return p < pillaged_from_.size() ? pillaged_from_[p] : 0;
    }

    // Edificio propio que abastece al alcance de la unidad, o null. Los aldeanos comen
    // también donde descargan: les vale cualquier almacén propio en uso.
    [[nodiscard]] entt::entity source_near(const entt::registry& registry, const EconomySystem& economy,
                                           PlayerId player, FVec2 pos, bool worker) const;

    [[nodiscard]] const SupplyParams& params() const noexcept { return params_; }
    [[nodiscard]] const SupplyTickStats& last_stats() const noexcept { return stats_; }
    void hash_into(StateHasher& h, const entt::registry& registry) const;

private:
    struct CarrierSeen {
        entt::entity entity;
        PlayerId owner;
        FVec2 pos;
    };

    void update_carriers(entt::registry& registry, MovementSystem& movement, EconomySystem& economy,
                         std::uint32_t& next_order_id, Tick tick);
    // Al alcance de la huella; si no, se acerca (una orden propia cada vez que la pierde).
    bool approach(entt::registry& registry, MovementSystem& movement, entt::entity e, Carrier& c,
                  const Footprint& f, std::uint32_t& next_order_id, Tick tick) const;
    [[nodiscard]] bool is_home(const entt::registry& registry, entt::entity b, PlayerId player) const;
    [[nodiscard]] bool is_camp(const entt::registry& registry, entt::entity b, PlayerId player) const;
    [[nodiscard]] entt::entity nearest_home(const entt::registry& registry, PlayerId player, FVec2 pos) const;
    // Forrajeo y saqueo (C2): una ración de lo que haya cerca. true si la consiguió.
    bool forage(entt::registry& registry, MovementSystem& movement, EconomySystem& economy, entt::entity e,
                PlayerId player, FVec2 pos, Tick tick);
    void resupply(entt::registry& registry, EconomySystem& economy, entt::entity e, PlayerId player, FVec2 pos,
                  Supply& s, const SupplyStats& st);

    SupplyParams params_;
    std::vector<UnitType> units_;
    std::vector<BuildingType> buildings_;
    std::vector<entt::entity> dead_;
    std::vector<entt::entity> scratch_;
    std::vector<CarrierSeen> carriers_;  // bagaje cargado de este tick (fuentes móviles)
    std::vector<std::int32_t> pillaged_from_;  // por jugador saqueado
    SupplyTickStats stats_;
};

}  // namespace rts::sim
