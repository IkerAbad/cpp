// Pruebas aleatorias de órdenes largas (G2), para la CI en Release: varias semillas en
// partidas de 2 y 4 jugadores (con y sin niebla) y en cada capítulo de campaña. Ante el
// primer fallo dice cuál y sale con 1.
//
// Uso: rts_fuzz --data <carpeta> [--seeds N] [--first-seed S] [--ticks T] [--orders K] [--only texto]

#include <charconv>
#include <expected>
#include <format>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "game/campaign.hpp"
#include "game/config.hpp"
#include "game/fuzz.hpp"
#include "game/scenario.hpp"

namespace {

std::int64_t number(std::string_view s) {
    std::int64_t v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path data_dir = "data";
    std::int64_t seeds = 4;
    std::int64_t ticks = 6000;
    std::int64_t orders = 2;
    std::int64_t first_seed = 1;
    std::string only;  // solo las partidas cuyo nombre contiene esto
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string_view a = argv[i];
        if (a == "--data") {
            data_dir = argv[i + 1];
        } else if (a == "--seeds") {
            seeds = number(argv[i + 1]);
        } else if (a == "--ticks") {
            ticks = number(argv[i + 1]);
        } else if (a == "--first-seed") {
            first_seed = number(argv[i + 1]);
        } else if (a == "--only") {
            only = argv[i + 1];
        } else if (a == "--orders") {
            orders = number(argv[i + 1]);
        } else {
            std::fprintf(stderr, "Uso: rts_fuzz --data <carpeta> [--seeds N] [--first-seed S] [--ticks T] [--orders K] [--only texto]\n");
            return 2;
        }
    }
    const auto base = rts::game::load_game_data(data_dir);
    if (!base) {
        std::fprintf(stderr, "%s\n", base.error().c_str());
        return 2;
    }
    // Las partidas: 2 y 4 jugadores, con niebla o sin ella; y cada capítulo.
    std::vector<std::pair<std::string, rts::game::GameData>> games;
    for (const bool fog : {false, true}) {
        for (const std::size_t seats : {2U, 4U}) {
            rts::game::MatchSettings s;
            s.fog = fog;
            s.rival = base->engine.ai_profile_names.back();
            s.seats.assign(seats, "ia");
            s.seats[0] = "humano";
            auto d = rts::game::with_match_settings(*base, s);
            if (!d) {
                std::fprintf(stderr, "%s\n", d.error().c_str());
                return 2;
            }
            games.emplace_back(std::format("{} jugadores{}", seats, fog ? " con niebla" : ""), std::move(*d));
        }
    }
    std::vector<std::string> errors;
    for (const auto& c : rts::game::load_campaigns(data_dir, errors)) {
        for (const auto& ch : c.chapters) {
            std::ifstream in(c.dir / ch.scenario, std::ios::binary);
            const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
            auto doc = rts::game::parse_scenario_doc(text, *base, ch.scenario);
            rts::game::MatchSettings s;
            s.rival = ch.rival;
            s.fog = ch.fog;
            auto d = doc ? rts::game::with_scenario(*base, *doc, s) : std::unexpected(doc.error());
            if (!d) {
                std::fprintf(stderr, "%s\n", d.error().c_str());
                return 2;
            }
            games.emplace_back(std::format("capítulo {}", ch.id), std::move(*d));
        }
    }
    rts::game::FuzzParams params;
    params.orders_per_tick = static_cast<std::int32_t>(orders);
    constexpr std::int32_t kCheckEvery = 20;
    std::int64_t total = 0;
    for (const auto& [name, data] : games) {
        if (!only.empty() && name.find(only) == std::string::npos) {
            continue;
        }
        for (std::int64_t seed = first_seed; seed < first_seed + seeds; ++seed) {
            const auto r = rts::game::run_fuzz(data, static_cast<std::uint64_t>(seed), static_cast<std::int32_t>(ticks),
                                               params, kCheckEvery);
            total += r.orders;
            if (r.failure) {
                std::printf("FALLO en %s, semilla %lld: %s\n", name.c_str(), static_cast<long long>(seed),
                            r.failure->c_str());
                return 1;
            }
            std::printf("%s, semilla %lld: bien (hash %016llx)\n", name.c_str(), static_cast<long long>(seed),
                        static_cast<unsigned long long>(r.hash));
            std::fflush(stdout);
        }
    }
    std::printf("OK: %lld órdenes al azar en %zu partidas x %lld semillas x %lld ticks\n", static_cast<long long>(total),
                games.size(), static_cast<long long>(seeds), static_cast<long long>(ticks));
    return 0;
}
