#pragma once

// Mercado y comercio (C3). En un mercado propio se compran o venden lotes de un recurso
// por oro. Los precios son los mismos para todos: comprar encarece y vender abarata, y
// con el tiempo vuelven a su precio base. Vender deja una comisión al mercader.
//
// Caravanas: el bagaje (acémila, carreta) con la orden TradeRoute va y viene entre el
// mercado propio más cercano y el mercado propio que se le indica, y en cada llegada da
// oro en proporción a la distancia entre ambos: el comercio de larga distancia de las
// ferias de Champaña, que «linked the cloth-producing cities of the Low Countries with
// the Italian dyeing and exporting centers» (Wikipedia, «Champagne fairs»).
//
// Determinismo: precios enteros; caravanas recorridas en orden de entidad.

#include <array>
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

// Trade: kind = índice del recurso, más kSellBit para vender.
inline constexpr std::uint8_t kSellBit = 0x80;

// Caravana entre dos mercados propios.
struct Caravan {
    entt::entity from = entt::null;
    entt::entity to = entt::null;
    bool outbound = true;          // hacia to (si no, de vuelta a from)
    std::uint32_t move_order = 0;
    std::int32_t approaches = 0;   // llegadas sin alcanzar el mercado (a la segunda, desiste)
};

// data/config/engine.toml, sección [market].
struct MarketParams {
    bool enabled = false;
    std::int32_t lot = 1;                                // unidades por compra o venta
    std::array<std::int32_t, kResourceCount> base_price{};  // oro por lote (0: no se comercia)
    std::int32_t price_step = 0;                         // lo que sube o baja cada operación
    std::int32_t min_price = 1;
    std::int32_t max_price = 1;
    std::int32_t sell_fee_percent = 0;
    std::int32_t recover_interval_ticks = 1;             // cada tantos ticks, 1 hacia la base
    std::int32_t caravan_gold_milli_per_tile = 0;        // milésimas de oro por casilla de ruta
    Fixed caravan_reach;
};

class MarketSystem {
public:
    MarketSystem(const MarketParams& params, std::vector<UnitType> units, std::vector<BuildingType> buildings);

    void apply(entt::registry& registry, MovementSystem& movement, EconomySystem& economy, const Command& command,
               std::span<const entt::entity> units, std::uint32_t& next_order_id, Tick tick);
    void update(entt::registry& registry, MovementSystem& movement, EconomySystem& economy,
                std::uint32_t& next_order_id, Tick tick);

    [[nodiscard]] bool enabled() const noexcept { return params_.enabled; }
    [[nodiscard]] const MarketParams& params() const noexcept { return params_; }
    [[nodiscard]] const std::array<std::int32_t, kResourceCount>& prices() const noexcept { return price_; }
    // Oro que da vender un lote de r ahora (con la comisión).
    [[nodiscard]] std::int32_t sell_price(std::size_t r) const noexcept;
    [[nodiscard]] bool is_market(const entt::registry& registry, entt::entity b, PlayerId player) const;
    // Oro ganado por las caravanas de p (estadística; no forma parte del estado).
    [[nodiscard]] std::int32_t caravan_gold(PlayerId p) const noexcept {
        return p < caravan_gold_.size() ? caravan_gold_[p] : 0;
    }
    void hash_into(StateHasher& h, const entt::registry& registry) const;

private:
    [[nodiscard]] entt::entity nearest_market(const entt::registry& registry, PlayerId player, FVec2 pos,
                                              entt::entity exclude) const;
    void head_to(entt::registry& registry, MovementSystem& movement, entt::entity e, Caravan& c,
                 std::uint32_t& next_order_id, Tick tick) const;

    MarketParams params_;
    std::vector<UnitType> units_;
    std::vector<BuildingType> buildings_;
    std::array<std::int32_t, kResourceCount> price_{};
    std::vector<std::int32_t> caravan_gold_;
    std::vector<std::int32_t> gold_milli_;  // por jugador: milésimas de oro aún sin sumar
    bool used_ = false;  // alguien ha comerciado: desde entonces, los precios entran en el hash
};

}  // namespace rts::sim
