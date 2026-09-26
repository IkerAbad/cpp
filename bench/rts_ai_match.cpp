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
//                   [--min-win-percent P]
// Con --min-win-percent, termina con código 1 si A gana menos del P % de las partidas.

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "game/config.hpp"
#include "sim/world.hpp"

namespace {

bool parse_arg(std::string_view value, std::int64_t& out) {
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), out);
    return ec == std::errc{} && end == value.data() + value.size() && out >= 0;
}

int usage() {
    std::fprintf(stderr,
                 "Uso: rts_ai_match --data <carpeta> --a <perfil> --b <perfil> [--games N] [--ticks T] "
                 "[--min-win-percent P]\n");
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

}  // namespace

int main(int argc, char** argv) {
    std::string data_dir;
    std::string name_a;
    std::string name_b;
    std::int64_t games = 20;
    std::int64_t ticks = 36'000;  // 30 minutos
    std::int64_t min_win = -1;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string_view key = argv[i];
        const std::string_view value = argv[i + 1];
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
        } else if (key == "--min-win-percent") {
            ok = parse_arg(value, min_win) && min_win <= kPercent;
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
    for (std::int64_t g = 0; g < games; ++g) {
        rts::sim::WorldParams params = data->engine.world;
        const auto pair = static_cast<std::uint64_t>(g / 2);
        params.map.seed += pair;
        params.setup.seed += pair;
        const bool a_first = g % 2 == 0;
        params.ai_players = {{0, a_first ? *a : *b}, {1, a_first ? *b : *a}};
        rts::sim::World world(params);
        std::int64_t t = 0;
        for (; t < ticks && !world.player_state(0).defeated && !world.player_state(1).defeated; ++t) {
            world.step();
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
        const char* result = "empate";
        if (winner < 0) {
            ++draws;
        } else {
            const bool a_won = (winner == 0) == a_first;
            (a_won ? wins_a : wins_b) += 1;
            result = a_won ? "gana A" : "gana B";
        }
        std::printf("partida %2lld (semilla +%llu, A es el jugador %d): %s %s en %lld:%02lld (valor %lld contra %lld)\n",
                    static_cast<long long>(g), static_cast<unsigned long long>(pair), a_first ? 0 : 1, result,
                    by_points ? "a los puntos" : "por derrota", static_cast<long long>(t / rts::sim::kTicksPerSecond / kSecondsPerMinute),
                    static_cast<long long>(t / rts::sim::kTicksPerSecond % kSecondsPerMinute), static_cast<long long>(a_first ? v0 : v1),
                    static_cast<long long>(a_first ? v1 : v0));
        std::fflush(stdout);
    }
    const std::int64_t win_pct = wins_a * kPercent / games;
    std::printf("A = %s, B = %s: A gana %lld, B gana %lld, empates %lld de %lld (A: %lld %%); %lld por derrota\n",
                name_a.c_str(), name_b.c_str(), static_cast<long long>(wins_a), static_cast<long long>(wins_b),
                static_cast<long long>(draws), static_cast<long long>(games), static_cast<long long>(win_pct),
                static_cast<long long>(decisive));
    return min_win >= 0 && win_pct < min_win ? 1 : 0;
}
