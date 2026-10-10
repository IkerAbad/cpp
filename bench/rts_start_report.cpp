// Inicios de partida (mejora tras D4): qué rodea a cada inicio en cada mapa, para saber
// por qué en muchos mapas gana el mismo lado juegue quien juegue. Mismas semillas que
// rts_ai_match (la del mapa + n), así que se puede cruzar con sus resultados.
//
// Uso: rts_start_report --data <carpeta> [--maps N] [--radius R]
// Una línea por mapa y jugador: comida, madera, oro, piedra y hierro a R casillas o
// menos (cantidad), casillas transitables, de colina y de bosque a R, altura media del
// entorno y de su edificio inicial.

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#include "game/config.hpp"
#include "sim/world.hpp"

namespace {

std::int64_t number(std::string_view s) {
    std::int64_t v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path data_dir = "data";
    std::int64_t maps = 10;
    std::int64_t radius = 20;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string_view a = argv[i];
        if (a == "--data") {
            data_dir = argv[i + 1];
        } else if (a == "--maps") {
            maps = number(argv[i + 1]);
        } else if (a == "--radius") {
            radius = number(argv[i + 1]);
        } else {
            std::fprintf(stderr, "Uso: rts_start_report --data <carpeta> [--maps N] [--radius R]\n");
            return 2;
        }
    }
    const auto data = rts::game::load_game_data(data_dir);
    if (!data) {
        std::fprintf(stderr, "%s\n", data.error().c_str());
        return 2;
    }
    const auto hills = data->terrain.find("colinas");
    const auto forest = data->terrain.find("bosque");
    const auto r = static_cast<std::int32_t>(radius);
    std::printf("mapa jugador comida madera oro piedra hierro transitables colinas bosque altura_media altura_inicio\n");
    for (std::int64_t m = 0; m < maps; ++m) {
        rts::sim::WorldParams params = data->engine.world;
        params.map.seed += static_cast<std::uint64_t>(m);
        params.setup.seed += static_cast<std::uint64_t>(m);
        const rts::sim::World world(params);
        rts::sim::Snapshot s;
        world.write_snapshot(s);
        const rts::sim::TileMap& map = world.map();
        for (const rts::sim::SnapshotObject& start : s.objects) {
            if (start.kind != rts::sim::ObjectKind::Building) {
                continue;
            }
            const rts::sim::TileCoord c{start.origin.x + start.size / 2, start.origin.y + start.size / 2};
            std::array<std::int64_t, rts::sim::kResourceCount> res{};
            for (const rts::sim::SnapshotObject& o : s.objects) {
                if (o.kind != rts::sim::ObjectKind::Resource) {
                    continue;
                }
                const std::int32_t dx = o.origin.x - c.x;
                const std::int32_t dy = o.origin.y - c.y;
                if (dx * dx + dy * dy <= r * r) {
                    res[rts::sim::resource_index(data->nodes.types[o.type].type.kind)] += o.amount;
                }
            }
            std::int64_t passable = 0;
            std::int64_t hill = 0;
            std::int64_t wood = 0;
            std::int64_t elev = 0;
            std::int64_t tiles = 0;
            for (std::int32_t y = c.y - r; y <= c.y + r; ++y) {
                for (std::int32_t x = c.x - r; x <= c.x + r; ++x) {
                    if (x < 0 || y < 0 || x >= map.width() || y >= map.height() ||
                        (x - c.x) * (x - c.x) + (y - c.y) * (y - c.y) > r * r) {
                        continue;
                    }
                    const auto t = map.terrain({x, y});
                    ++tiles;
                    elev += map.elevation({x, y});
                    passable += data->terrain.types[t].passable ? 1 : 0;
                    hill += hills && t == *hills ? 1 : 0;
                    wood += forest && t == *forest ? 1 : 0;
                }
            }
            std::printf("%lld %u %lld %lld %lld %lld %lld %lld %lld %lld %.2f %u\n", static_cast<long long>(m),
                        static_cast<unsigned>(start.owner), static_cast<long long>(res[0]),
                        static_cast<long long>(res[1]), static_cast<long long>(res[3]), static_cast<long long>(res[2]),
                        static_cast<long long>(res[4]), static_cast<long long>(passable), static_cast<long long>(hill),
                        static_cast<long long>(wood), tiles > 0 ? static_cast<double>(elev) / static_cast<double>(tiles) : 0.0,
                        static_cast<unsigned>(map.elevation(c)));
        }
    }
    return 0;
}
