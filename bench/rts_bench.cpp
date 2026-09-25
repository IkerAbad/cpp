// Banco de rendimiento de la simulación: N unidades con un guion de órdenes fijo que
// ejercita campos de flujo (grupo grande), evitación local (dos mitades que se cruzan)
// y el planificador HPA* con presupuesto (cientos de grupos pequeños a la vez).
//
// Modo combate (--combat 1): N unidades en dos ejércitos de N/2 (2/3 soldados, 1/3
// arqueros) que avanzan uno contra otro con ataque-movimiento: adquisición de
// blancos, persecución, proyectiles, muertes y niveles a la vez.
//
// Uso: rts_bench [--units N] [--ticks T] [--max-ms X] [--combat 0|1]
// Mide el tiempo de cada tick y termina con código 1 si el máximo supera X ms
// (criterio de M2: 50 ms, el presupuesto de un tick a 20 Hz).

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <vector>

#include "sim/world.hpp"

namespace {

using rts::sim::Command;
using rts::sim::CommandType;
using rts::sim::Fixed;
using rts::sim::TileCoord;
using rts::sim::World;
using rts::sim::WorldParams;

// Mismos parámetros que las pruebas (independientes de data/, que puede cambiar).
WorldParams bench_params(std::int32_t units) {
    WorldParams p;
    p.map.seed = 0x5EED'2026'0924ULL;
    p.map.width = 256;
    p.map.height = 256;
    p.map.noise_cell_tiles = 32;
    p.map.elevation_levels = 8;
    p.map.bands = {{20'000, 0}, {45'000, 1}, {rts::sim::kElevationRange, 2}};
    p.passable_by_terrain = {0, 1, 1};
    rts::sim::UnitType soldier;
    soldier.radius = Fixed::from_ratio(3, 10);
    soldier.speed = Fixed::from_ratio(6, 100);
    soldier.combat.hp = 45;
    soldier.combat.attack_melee = 6;
    soldier.combat.armor_melee = 1;
    soldier.combat.armor_pierce = 1;
    soldier.combat.range = Fixed::from_ratio(15, 100);
    soldier.combat.reload_ticks = 40;
    soldier.combat.sight_tiles = 6;
    rts::sim::UnitType archer;
    archer.radius = Fixed::from_ratio(1, 4);
    archer.speed = Fixed::from_ratio(55, 1000);
    archer.combat.hp = 30;
    archer.combat.attack_pierce = 4;
    archer.combat.range = Fixed::from_int(4);
    archer.combat.reload_ticks = 40;
    archer.combat.sight_tiles = 7;
    archer.combat.projectile_speed = Fixed::from_ratio(35, 100);
    p.unit_types = {soldier, archer};
    auto& c = p.combat;
    c.acquire_interval_ticks = 10;
    c.repath_tiles = 2;
    c.chase_attempts = 3;
    c.building_reach = Fixed::from_ratio(3, 5);
    c.projectile_hit_radius = Fixed::from_ratio(15, 100);
    c.xp_kill_bonus = 20;
    c.level_thresholds = {30, 70, 120, 180, 260, 360, 480, 620, 780, 960, 1160, 1400};
    c.hp_percent_per_level = 8;
    c.attack_percent_per_level = 6;
    c.armor_every_levels = 3;
    c.hero_aura_radius = Fixed::from_int(4);
    c.hero_aura_attack_percent = 15;
    c.hero_name_count = 8;
    auto& m = p.movement;
    m.hpa = {16, 8};
    m.path_node_budget_per_tick = 60'000;
    m.flow_field_min_group = 8;
    m.flow_field_cache_size = 4;
    m.retarget_radius_tiles = 16;
    m.neighbor_radius = Fixed::from_int(2);
    m.max_neighbors = 8;
    m.time_horizon_ticks = 20;
    m.preference_weight = 100;
    m.collision_weight = 60;
    m.stuck_arrive_ticks = 40;
    m.arrive_radius = Fixed::from_ratio(1, 4);
    m.waypoint_radius = Fixed::from_ratio(45, 100);
    p.demo.seed = 0x5EED'2026'0924ULL;
    p.demo.unit_type = 0;
    p.demo.count = units;
    // Área de aparición con holgura: ~1 unidad por cada 3 casillas de tierra.
    p.demo.area_tiles = std::clamp(static_cast<std::int32_t>(std::sqrt(units * 5.0)), 64, 200);
    return p;
}

Command move(rts::sim::Tick tick, std::vector<std::uint32_t> units, TileCoord target) {
    Command c;
    c.tick = tick;
    c.type = CommandType::Move;
    c.units = std::move(units);
    c.target = target;
    return c;
}

void issue_script(World& world) {
    rts::sim::Snapshot snap;
    world.write_snapshot(snap);
    std::vector<std::uint32_t> all;
    for (const auto& e : snap.entities) {
        all.push_back(e.id);
    }
    const auto half = static_cast<std::ptrdiff_t>(all.size() / 2);
    // 1. Todas juntas: un campo de flujo.
    world.issue(move(5, all, {200, 60}));
    // 2. Dos mitades que se cruzan: evitación local densa.
    world.issue(move(400, {all.begin(), all.begin() + half}, {60, 200}));
    world.issue(move(400, {all.begin() + half, all.end()}, {200, 200}));
    // 3. Grupos de 5 (por debajo del mínimo de campo de flujo) a destinos repartidos:
    //    cientos de consultas HPA* compitiendo por el presupuesto de nodos.
    rts::sim::Xoshiro256pp rng(7);
    for (std::size_t i = 0; i < all.size(); i += 5) {
        const auto end = std::min(all.size(), i + 5);
        const std::int32_t tx = 16 + static_cast<std::int32_t>(rng.next_below(224));
        const std::int32_t ty = 16 + static_cast<std::int32_t>(rng.next_below(224));
        world.issue(move(800, {all.begin() + static_cast<std::ptrdiff_t>(i), all.begin() + static_cast<std::ptrdiff_t>(end)},
                         {tx, ty}));
    }
}

// Dos ejércitos en campo abierto, con los frentes a 40 casillas, en ataque-movimiento:
// se encuentran hacia el tick 300 y el resto del banco es batalla.
World make_battle(std::int64_t units) {
    WorldParams p = bench_params(0);
    p.map.bands = {{rts::sim::kElevationRange, 1}};
    p.demo.player = 1;  // dos jugadores
    World world(p);
    const auto per_side = static_cast<std::int32_t>(units / 2);
    const std::int32_t cols = std::max<std::int32_t>(1, static_cast<std::int32_t>(std::sqrt(per_side * 2.0)));
    for (rts::sim::PlayerId side = 0; side < 2; ++side) {
        Command c;
        c.type = CommandType::AttackMove;
        c.player = side;
        c.target = {side == 0 ? 190 : 66, 128};
        for (std::int32_t i = 0; i < per_side; ++i) {
            // Filas hacia el enemigo: columnas en y, profundidad en x.
            const std::int32_t row = i / cols;
            const std::int32_t col = i % cols;
            const TileCoord t{side == 0 ? 108 - row : 148 + row, 128 - cols / 2 + col};
            c.units.push_back(world.spawn_unit(side, static_cast<rts::sim::UnitTypeId>(i % 3 == 0 ? 1 : 0), t));
        }
        world.issue(std::move(c));
    }
    return world;
}

bool parse_arg(std::string_view value, std::int64_t& out) {
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), out);
    return ec == std::errc{} && end == value.data() + value.size() && out > 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::int64_t units = 1000;
    std::int64_t ticks = 1200;
    std::int64_t max_ms = 50;
    std::int64_t combat = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string_view key = argv[i];
        std::int64_t* target = key == "--units"    ? &units
                               : key == "--ticks"  ? &ticks
                               : key == "--max-ms" ? &max_ms
                               : key == "--combat" ? &combat
                                                   : nullptr;
        const bool ok = target == &combat ? (argv[i + 1] == std::string_view("0") || argv[i + 1] == std::string_view("1"))
                                          : target != nullptr && parse_arg(argv[i + 1], *target);
        if (!ok) {
            std::fprintf(stderr, "Uso: rts_bench [--units N] [--ticks T] [--max-ms X] [--combat 0|1]\n");
            return 2;
        }
        if (target == &combat) {
            combat = argv[i + 1][0] == '1' ? 1 : 0;
        }
    }

    using Clock = std::chrono::steady_clock;
    const auto setup_start = Clock::now();
    World world = combat != 0 ? make_battle(units) : World(bench_params(static_cast<std::int32_t>(units)));
    if (combat == 0) {
        issue_script(world);
    }
    const double setup_ms = std::chrono::duration<double, std::milli>(Clock::now() - setup_start).count();

    std::vector<double> tick_ms;
    tick_ms.reserve(static_cast<std::size_t>(ticks));
    std::int64_t max_nodes = 0;
    std::int32_t max_pending = 0;
    std::int64_t kills = 0;
    std::int64_t shots = 0;
    for (std::int64_t t = 0; t < ticks; ++t) {
        const auto start = Clock::now();
        world.step();
        tick_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        max_nodes = std::max(max_nodes, world.movement().last_stats().nodes_expanded);
        max_pending = std::max(max_pending, world.movement().last_stats().paths_pending);
        kills += world.combat().last_stats().kills;
        shots += world.combat().last_stats().projectiles_fired;
    }

    std::vector<double> sorted = tick_ms;
    std::ranges::sort(sorted);
    double sum = 0.0;
    for (const double v : tick_ms) {
        sum += v;
    }
    const auto pct = [&](double p) {
        return sorted[std::min(sorted.size() - 1, static_cast<std::size_t>(p * static_cast<double>(sorted.size())))];
    };
    const double worst = sorted.back();
    const auto worst_tick = std::ranges::max_element(tick_ms) - tick_ms.begin();

    rts::sim::Snapshot snap;
    world.write_snapshot(snap);
    std::printf("unidades=%zu ticks=%lld preparación=%.1f ms (mapa, HPA*, aparición)\n", snap.entities.size(),
                static_cast<long long>(ticks), setup_ms);
    std::printf("ms/tick: media=%.3f p50=%.3f p99=%.3f max=%.3f (tick %lld)\n", sum / static_cast<double>(ticks),
                pct(0.50), pct(0.99), worst, static_cast<long long>(worst_tick));
    std::printf("planificador: máx. %lld nodos/tick, máx. %d caminos pendientes\n", static_cast<long long>(max_nodes),
                max_pending);
    if (combat != 0) {
        std::printf("combate: %lld bajas, %lld proyectiles, %zu unidades vivas al final\n",
                    static_cast<long long>(kills), static_cast<long long>(shots), snap.entities.size());
    }
    std::printf("state_hash=%016llx\n", static_cast<unsigned long long>(world.state_hash()));
    if (worst > static_cast<double>(max_ms)) {
        std::printf("FALLO: el tick más lento (%.3f ms) supera el límite de %lld ms\n", worst,
                    static_cast<long long>(max_ms));
        return 1;
    }
    std::printf("OK: todos los ticks por debajo de %lld ms\n", static_cast<long long>(max_ms));
    return 0;
}
