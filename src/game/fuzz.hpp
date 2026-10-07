#pragma once

// Pruebas aleatorias de órdenes (G2). Órdenes al azar sobre una partida real: muchas
// válidas (unidades propias, objetos que existen, casillas del mapa) y otras a
// propósito mal formadas (unidades ajenas o que no existen, casillas fuera del mapa,
// tipos fuera de rango, jugadores que no existen). La simulación debe rechazar lo que
// no vale sin romperse, sin salirse de sus invariantes y sin dejar de ser determinista.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "game/config.hpp"
#include "sim/rng.hpp"
#include "sim/world.hpp"

namespace rts::game {

struct FuzzParams {
    std::int32_t orders_per_tick = 1;   // órdenes que se generan en cada tick
    std::int32_t max_units = 20;        // unidades por orden, como mucho
    std::int32_t garbage_percent = 20;  // % de campos que se rellenan con basura
};

// Órdenes al azar para el tick actual del mundo.
[[nodiscard]] std::vector<sim::Command> random_orders(const sim::World& world, sim::Xoshiro256pp& rng,
                                                      const FuzzParams& params);

// Una partida con órdenes al azar: dos mundos a la par con las mismas órdenes (deben
// dar el mismo hash), invariantes cada check_every ticks, y al final la repetición
// grabada debe reproducirse igual. failure dice lo primero que salió mal.
struct FuzzResult {
    std::optional<std::string> failure;
    std::uint64_t hash = 0;
    std::int64_t orders = 0;
};
[[nodiscard]] FuzzResult run_fuzz(const GameData& data, std::uint64_t seed, std::int32_t ticks,
                                  const FuzzParams& params, std::int32_t check_every);

// Lo que nunca debe pasar (unidades fuera del mapa, vida fuera de rango, almacén en
// negativo...). Devuelve la primera violación, o nada.
[[nodiscard]] std::optional<std::string> check_invariants(const sim::World& world);

}  // namespace rts::game
