// Pruebas del mercado (C3): comprar encarece y vender abarata, para todos; vender deja
// comisión; sin con qué pagar no hay trato; los precios vuelven a su base; las
// caravanas entre dos mercados propios dan oro en cada llegada según la distancia.

#include <cstdint>
#include <vector>

#include <doctest/doctest.h>

#include "sim/world.hpp"
#include "test_helpers.hpp"

using rts::sim::Command;
using rts::sim::CommandType;
using rts::sim::World;
using rts::sim::WorldParams;
using namespace rts::test;

namespace {

constexpr std::size_t kFood = 0;
constexpr std::size_t kGold = 3;

struct MarketSetup {
    WorldParams params = test_world_params(0, 16);
    rts::sim::BuildingTypeId market = 0;
    rts::sim::UnitTypeId mule = 0;

    MarketSetup() {
        params.map.bands = {{rts::sim::kElevationRange, 1}};
        params.demo.player = 1;
        rts::sim::BuildingType m = params.building_types[kHouse];
        m.population = 0;
        m.market = true;
        market = static_cast<rts::sim::BuildingTypeId>(params.building_types.size());
        params.building_types.push_back(m);
        rts::sim::UnitType u = params.unit_types[kVillager];
        u.worker = false;
        u.carry_capacity = 0;
        u.convoy_capacity = 20;
        mule = static_cast<rts::sim::UnitTypeId>(params.unit_types.size());
        params.unit_types.push_back(u);
        auto& mk = params.market;
        mk.enabled = true;
        mk.lot = 100;
        mk.base_price = {50, 50, 70, 0, 90};
        mk.price_step = 3;
        mk.min_price = 10;
        mk.max_price = 400;
        mk.sell_fee_percent = 30;
        mk.recover_interval_ticks = 10;
        mk.caravan_gold_milli_per_tile = 1000;  // 1 de oro por casilla
        mk.caravan_reach = rts::sim::Fixed::from_ratio(4, 5);
    }
};

Command trade(std::uint32_t market, std::size_t r, bool sell, rts::sim::PlayerId player = 0) {
    Command c;
    c.type = CommandType::Trade;
    c.player = player;
    c.object = market;
    c.kind = static_cast<std::uint8_t>(r | (sell ? rts::sim::kSellBit : 0U));
    return c;
}

}  // namespace

TEST_CASE("Mercado: comprar encarece y vender abarata, con comisión, y para todos") {
    MarketSetup s;
    World w(s.params);
    const auto m0 = *w.spawn_building(0, s.market, {40, 40}, true);
    const auto m1 = *w.spawn_building(1, s.market, {80, 40}, true);
    w.set_stock(0, stock(0, 0, 0, 200));
    w.set_stock(1, stock(300, 0, 0, 0));
    w.step();  // tick 0: el de la vuelta a la base; que no se mezcle
    w.issue(trade(m0, kFood, false));
    w.step();
    CHECK(w.player_state(0).stock[kFood] == 100);
    CHECK(w.player_state(0).stock[kGold] == 150);
    CHECK(w.market().prices()[kFood] == 53);
    // El rival vende comida: el precio baja también para el jugador 0.
    w.issue(trade(m1, kFood, true, 1));
    w.step();
    CHECK(w.player_state(1).stock[kFood] == 200);
    CHECK(w.player_state(1).stock[kGold] == 53 * 70 / 100);
    CHECK(w.market().prices()[kFood] == 50);
    // Sin oro no se compra; sin el lote no se vende; el oro no se comercia.
    w.set_stock(0, stock(50, 0, 0, 10));
    w.issue(trade(m0, kFood, false));
    w.issue(trade(m0, kFood, true));
    w.issue(trade(m0, kGold, true));
    w.step();
    CHECK(w.player_state(0).stock[kFood] == 50);
    CHECK(w.player_state(0).stock[kGold] == 10);
}

TEST_CASE("Mercado: los precios vuelven poco a poco a su base") {
    MarketSetup s;
    World w(s.params);
    const auto m0 = *w.spawn_building(0, s.market, {40, 40}, true);
    w.set_stock(0, stock(0, 0, 0, 1000));
    w.step();
    for (int i = 0; i < 5; ++i) {
        w.issue(trade(m0, kFood, false));
    }
    w.step();
    CHECK(w.market().prices()[kFood] == 65);
    for (int t = 0; t < 10 * 15 + 10; ++t) {
        w.step();
    }
    CHECK(w.market().prices()[kFood] == 50);
}

TEST_CASE("Mercado: la caravana entre dos mercados propios da oro en cada llegada, según la distancia") {
    MarketSetup s;
    World w(s.params);
    const auto near = *w.spawn_building(0, s.market, {40, 40}, true);
    const auto far = *w.spawn_building(0, s.market, {70, 40}, true);
    (void)near;
    const auto mule = w.spawn_unit(0, s.mule, {38, 41});
    w.set_stock(0, stock(0, 0, 0, 0));
    Command route;
    route.type = CommandType::TradeRoute;
    route.player = 0;
    route.units = {mule};
    route.object = far;
    w.issue(route);
    std::int32_t first = 0;
    for (int t = 0; t < 4000 && first == 0; ++t) {
        w.step();
        first = w.player_state(0).stock[kGold];
    }
    CHECK(first == 30);  // 30 casillas entre los centros, a 1 de oro cada una
    for (int t = 0; t < 4000 && w.player_state(0).stock[kGold] == first; ++t) {
        w.step();
    }
    CHECK(w.player_state(0).stock[kGold] == 60);  // y otra vez al volver
    CHECK(w.market().caravan_gold(0) == 60);
}

TEST_CASE("Mercado: desactivado, no hay tratos") {
    MarketSetup s;
    s.params.market.enabled = false;
    World w(s.params);
    const auto m0 = *w.spawn_building(0, s.market, {40, 40}, true);
    w.set_stock(0, stock(0, 0, 0, 200));
    w.issue(trade(m0, kFood, false));
    w.step();
    CHECK(w.player_state(0).stock[kGold] == 200);
}
