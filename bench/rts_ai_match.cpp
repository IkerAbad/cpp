// Torneo entre perfiles de IA sin ventana: mide si un perfil juega mejor que otro.
//
// Cada par de partidas usa la misma semilla (mapa y preparación) con los lados
// cambiados, para que la ventaja de un inicio no cuente. Gana quien derrota al
// rival antes del límite de ticks; si nadie lo consigue, gana a los puntos quien
// tenga más valor vivo (coste de sus unidades y de sus edificios terminados; lo
// ahorrado no cuenta) y, con el mismo valor, es empate. Todo es
// determinista: el mismo torneo da el mismo resultado en cualquier máquina.
//
// Uso: rts_ai_match --data <carpeta> --a <perfil> --b <perfil> [--games N] [--ticks T]
//                   [--min-win-percent P] [--min-both-sides N] [--trace SEGUNDOS]
// Con --min-win-percent, termina con código 1 si A gana menos del P % de las partidas.
// Cada mapa se juega dos veces, una desde cada lado. En muchos mapas gana el mismo
// lado juegue quien juegue (el mapa decide, no la IA: D4). Por eso también se cuentan
// los mapas que una IA gana desde los dos lados; con --min-both-sides N, termina con
// código 1 si A no gana al menos N mapas así o si B gana alguno.
// Con --symmetric, cada mapa generado se juega reflejado por el centro (escenario con
// las dos mitades iguales): separa lo que decide el mapa de lo que decide la IA.
// Con --trace, cada tantos segundos de partida imprime el estado de cada jugador
// (aldeanos, ejército, bagaje, campamentos, hambre, almacén): para entender por qué
// gana o pierde un perfil.

#include <charconv>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "game/config.hpp"
#include "game/scenario.hpp"
#include "sim/world.hpp"

namespace {

bool parse_arg(std::string_view value, std::int64_t& out) {
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), out);
    return ec == std::errc{} && end == value.data() + value.size() && out >= 0;
}

int usage() {
    std::fprintf(stderr,
                 "Uso: rts_ai_match --data <carpeta> --a <perfil> --b <perfil> [--games N] [--ticks T] "
                 "[--min-win-percent P] [--min-both-sides N] [--symmetric] [--trace SEGUNDOS]\n");
    return 2;
}

constexpr std::int64_t kPercent = 100;
constexpr std::int64_t kSecondsPerMinute = 60;

std::int64_t cost_sum(const rts::sim::Stock& cost) {
    std::int64_t sum = 0;
    for (const std::int32_t c : cost) {
        sum += c;
    }
    return sum;
}

// Valor vivo de un jugador: coste de sus unidades y de sus edificios terminados.
std::int64_t live_value(const rts::sim::World& world, const rts::sim::WorldParams& params, rts::sim::PlayerId p) {
    rts::sim::Snapshot s;
    world.write_snapshot(s);
    std::int64_t value = 0;
    for (const auto& e : s.entities) {
        value += e.owner == p ? cost_sum(params.unit_types[e.type].cost) : 0;
    }
    for (const auto& o : s.objects) {
        if (o.kind == rts::sim::ObjectKind::Building && o.owner == p && o.complete) {
            value += cost_sum(params.building_types[o.type].cost);
        }
    }
    return value;
}

// Una línea por jugador con lo que importa para entender la partida.
void trace(const rts::sim::World& world, const rts::sim::WorldParams& params, char a_or_b0, std::int64_t t) {
    rts::sim::Snapshot s;
    world.write_snapshot(s);
    for (rts::sim::PlayerId p = 0; p < 2; ++p) {
        std::int64_t villagers = 0;
        std::int64_t army = 0;
        std::int64_t carriers = 0;
        std::int64_t hungry = 0;
        std::int64_t no_ammo = 0;
        for (const auto& e : s.entities) {
            if (e.owner != p) {
                continue;
            }
            const rts::sim::UnitType& u = params.unit_types[e.type];
            villagers += u.worker ? 1 : 0;
            carriers += u.convoy_capacity > 0 ? 1 : 0;
            army += !u.worker && u.convoy_capacity == 0 ? 1 : 0;
            hungry += e.hungry ? 1 : 0;
            no_ammo += u.supply.ammo > 0 && e.ammo <= 0 ? 1 : 0;
        }
        std::int64_t camps = 0;
        std::int64_t camp_store = 0;
        std::int64_t buildings = 0;
        for (const auto& o : s.objects) {
            if (o.kind != rts::sim::ObjectKind::Building || o.owner != p) {
                continue;
            }
            ++buildings;
            if (params.building_types[o.type].store_capacity > 0) {
                ++camps;
                camp_store += cost_sum(o.store);
            }
        }
        // Lo más cerca que está su ejército de un edificio vital enemigo (presión).
        std::int64_t pressure = -1;
        for (const auto& o : s.objects) {
            if (o.kind != rts::sim::ObjectKind::Building || o.owner == p || !params.building_types[o.type].vital) {
                continue;
            }
            for (const auto& e : s.entities) {
                const rts::sim::UnitType& u = params.unit_types[e.type];
                if (e.owner != p || u.worker || u.convoy_capacity > 0) {
                    continue;
                }
                const std::int64_t dx = std::abs(e.pos.x.floor_to_int() - (o.origin.x + o.size / 2));
                const std::int64_t dy = std::abs(e.pos.y.floor_to_int() - (o.origin.y + o.size / 2));
                const std::int64_t d = std::max(dx, dy);
                pressure = pressure < 0 ? d : std::min(pressure, d);
            }
        }
        const auto& st = world.player_state(p).stock;
        const char who = p == 0 ? a_or_b0 : (a_or_b0 == 'A' ? 'B' : 'A');
        std::printf("  %lld:%02lld %c: aldeanos %lld, ejército %lld (hambre %lld, sin munición %lld), bagaje %lld, "
                    "campamentos %lld (%lld), edificios %lld, almacén %d/%d/%d/%d/%d, a %lld del centro enemigo\n",
                    static_cast<long long>(t / rts::sim::kTicksPerSecond / kSecondsPerMinute),
                    static_cast<long long>(t / rts::sim::kTicksPerSecond % kSecondsPerMinute), who,
                    static_cast<long long>(villagers), static_cast<long long>(army), static_cast<long long>(hungry),
                    static_cast<long long>(no_ammo), static_cast<long long>(carriers), static_cast<long long>(camps),
                    static_cast<long long>(camp_store), static_cast<long long>(buildings), st[0], st[1], st[2], st[3],
                    st[4], static_cast<long long>(pressure));
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string data_dir;
    std::string name_a;
    std::string name_b;
    std::int64_t games = 20;
    std::int64_t ticks = 36'000;  // 30 minutos
    std::int64_t min_win = -1;
    bool symmetric = false;
    std::int64_t min_both = -1;
    std::int64_t trace_seconds = 0;
    // --symmetric no lleva valor; el resto va por parejas.
    std::vector<std::string_view> args;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--symmetric") {
            symmetric = true;
        } else {
            args.emplace_back(argv[i]);
        }
    }
    if (args.size() % 2 != 0) {
        return usage();
    }
    for (std::size_t i = 0; i + 1 < args.size(); i += 2) {
        const std::string_view key = args[i];
        const std::string_view value = args[i + 1];
        bool ok = true;
        if (key == "--data") {
            data_dir = value;
        } else if (key == "--a") {
            name_a = value;
        } else if (key == "--b") {
            name_b = value;
        } else if (key == "--games") {
            ok = parse_arg(value, games) && games > 0;
        } else if (key == "--ticks") {
            ok = parse_arg(value, ticks) && ticks > 0;
        } else if (key == "--min-both-sides") {
            ok = parse_arg(value, min_both);
        } else if (key == "--min-win-percent") {
            ok = parse_arg(value, min_win) && min_win <= kPercent;
        } else if (key == "--trace") {
            ok = parse_arg(value, trace_seconds) && trace_seconds > 0;
        } else {
            ok = false;
        }
        if (!ok) {
            return usage();
        }
    }
    if (data_dir.empty() || name_a.empty() || name_b.empty()) {
        return usage();
    }
    const auto data = rts::game::load_game_data(std::filesystem::path(data_dir));
    if (!data) {
        std::fprintf(stderr, "%s\n", data.error().c_str());
        return 2;
    }
    const auto& names = data->engine.ai_profile_names;
    const auto find = [&](const std::string& n) -> std::optional<std::uint8_t> {
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (names[i] == n) {
                return static_cast<std::uint8_t>(i);
            }
        }
        return std::nullopt;
    };
    const auto a = find(name_a);
    const auto b = find(name_b);
    if (!a || !b) {
        std::fprintf(stderr, "Perfil desconocido: '%s'\n", (!a ? name_a : name_b).c_str());
        return 2;
    }
    if (data->engine.world.setup.starts.size() != 2) {
        std::fprintf(stderr, "El torneo necesita exactamente dos jugadores en engine.toml\n");
        return 2;
    }

    std::int64_t wins_a = 0;
    std::int64_t wins_b = 0;
    std::int64_t draws = 0;
    std::int64_t decisive = 0;  // victorias por derrota del rival (no a los puntos)
    std::int64_t routed_wins = 0;  // victorias en las que el vencido sufrió desbandadas
    // Por mapa: quién ganó la partida en la que A era el jugador 0 (+1 A, -1 B, 0 empate).
    std::int64_t both_a = 0;  // mapas que A gana desde los dos lados
    std::int64_t both_b = 0;
    int first_of_pair = 0;
    for (std::int64_t g = 0; g < games; ++g) {
        rts::sim::WorldParams params = data->engine.world;
        const auto pair = static_cast<std::uint64_t>(g / 2);
        params.map.seed += pair;
        params.setup.seed += pair;
        const bool a_first = g % 2 == 0;
        if (symmetric) {
            const rts::sim::World generated(params);
            params.scenario =
                rts::game::mirrored_scenario(rts::game::scenario_from_world(generated, "espejo", 2), *data).params;
        }
        params.ai_players = {{0, a_first ? *a : *b}, {1, a_first ? *b : *a}};
        rts::sim::World world(params);
        std::int64_t t = 0;
        for (; t < ticks && !world.player_state(0).defeated && !world.player_state(1).defeated; ++t) {
            world.step();
            if (trace_seconds > 0 && (t + 1) % (trace_seconds * rts::sim::kTicksPerSecond) == 0) {
                trace(world, params, a_first ? 'A' : 'B', t + 1);
            }
        }
        const bool p0_lost = world.player_state(0).defeated;
        const bool p1_lost = world.player_state(1).defeated;
        const std::int64_t v0 = live_value(world, params, 0);
        const std::int64_t v1 = live_value(world, params, 1);
        // Ganador: 0, 1 o -1 (empate).
        int winner = -1;
        const bool by_points = p0_lost == p1_lost;
        if (!by_points) {
            winner = p1_lost ? 0 : 1;
            ++decisive;
        } else if (v0 != v1) {
            winner = v0 > v1 ? 0 : 1;
        }
        const std::int64_t routs0 = world.morale().routs_of(0);
        const std::int64_t routs1 = world.morale().routs_of(1);
        if (winner >= 0 && (winner == 0 ? routs1 : routs0) > 0) {
            ++routed_wins;
        }
        const char* result = "empate";
        if (winner < 0) {
            ++draws;
        } else {
            const bool a_won = (winner == 0) == a_first;
            (a_won ? wins_a : wins_b) += 1;
            result = a_won ? "gana A" : "gana B";
        }
        const int outcome = winner < 0 ? 0 : (((winner == 0) == a_first) ? 1 : -1);
        if (a_first) {
            first_of_pair = outcome;
        } else if (outcome != 0 && outcome == first_of_pair) {
            (outcome > 0 ? both_a : both_b) += 1;
        }
        std::printf("partida %2lld (semilla +%llu, A es el jugador %d): %s %s en %lld:%02lld (valor %lld contra %lld; "
                    "desbandadas %lld contra %lld)\n",
                    static_cast<long long>(g), static_cast<unsigned long long>(pair), a_first ? 0 : 1, result,
                    by_points ? "a los puntos" : "por derrota", static_cast<long long>(t / rts::sim::kTicksPerSecond / kSecondsPerMinute),
                    static_cast<long long>(t / rts::sim::kTicksPerSecond % kSecondsPerMinute), static_cast<long long>(a_first ? v0 : v1),
                    static_cast<long long>(a_first ? v1 : v0), static_cast<long long>(a_first ? routs0 : routs1),
                    static_cast<long long>(a_first ? routs1 : routs0));
        std::fflush(stdout);
    }
    const std::int64_t win_pct = wins_a * kPercent / games;
    std::printf("A = %s, B = %s: A gana %lld, B gana %lld, empates %lld de %lld (A: %lld %%); %lld por derrota; "
                "%lld con desbandada del vencido\n",
                name_a.c_str(), name_b.c_str(), static_cast<long long>(wins_a), static_cast<long long>(wins_b),
                static_cast<long long>(draws), static_cast<long long>(games), static_cast<long long>(win_pct),
                static_cast<long long>(decisive), static_cast<long long>(routed_wins));
    std::printf("Mapas ganados desde los dos lados: A %lld, B %lld (el resto los decide el mapa)\n",
                static_cast<long long>(both_a), static_cast<long long>(both_b));
    if (min_both >= 0 && (both_a < min_both || both_b > 0)) {
        return 1;
    }
    return min_win >= 0 && win_pct < min_win ? 1 : 0;
}
