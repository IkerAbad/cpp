// Pruebas de la economía (M3): tiempos exactos de recogida, conservación de recursos,
// agotamiento, colas con coste y reembolso, pausa por población, construcción, y la
// actualización incremental del pathfinding cuando los objetos bloquean o liberan casillas.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <vector>

#include <doctest/doctest.h>

#include "sim/map_gen.hpp"
#include "sim/path/grid.hpp"
#include "sim/path/hpa.hpp"
#include "sim/rng.hpp"
#include "sim/world.hpp"
#include "test_helpers.hpp"

using rts::sim::Building;
using rts::sim::Command;
using rts::sim::CommandType;
using rts::sim::GridSearch;
using rts::sim::HpaGraph;
using rts::sim::MoveGoal;
using rts::sim::PassGrid;
using rts::sim::Position;
using rts::sim::ProductionQueue;
using rts::sim::Resource;
using rts::sim::ResourceNode;
using rts::sim::TileCoord;
using rts::sim::TileMap;
using rts::sim::Unit;
using rts::sim::Worker;
using rts::sim::WorkerTask;
using rts::sim::World;
using rts::sim::WorldParams;
using rts::sim::Xoshiro256pp;
using rts::test::kBerries;
using rts::test::kCenter;
using rts::test::kHouse;
using rts::test::kSoldier;
using rts::test::kTree;
using rts::test::kVillager;
using rts::test::stock;

namespace {

constexpr auto kWood = rts::sim::resource_index(Resource::Wood);
constexpr auto kFood = rts::sim::resource_index(Resource::Food);

// Campo abierto (todo transitable), sin unidades de demostración: la geometría de
// cada prueba la pone la propia prueba.
WorldParams open_field() {
    WorldParams p = rts::test::test_world_params(0, 16);
    p.map.bands = {{rts::sim::kElevationRange, 1}};
    return p;
}

entt::entity ent(std::uint32_t id) {
    return static_cast<entt::entity>(id);
}

Command command(CommandType type, std::vector<std::uint32_t> units = {}, std::uint32_t object = rts::sim::kNoObject) {
    Command c;
    c.type = type;
    c.units = std::move(units);
    c.object = object;
    return c;
}

void run(World& world, std::int32_t ticks) {
    for (std::int32_t i = 0; i < ticks; ++i) {
        world.step();
    }
}

std::int32_t units_of(const World& world, rts::sim::UnitTypeId type) {
    std::int32_t n = 0;
    const auto units = world.registry().view<const Unit>();
    for (const entt::entity e : units) {
        n += units.get<const Unit>(e).type == type ? 1 : 0;
    }
    return n;
}

bool all_on_passable_tiles(const World& world) {
    const auto positions = world.registry().view<const Position>();
    for (const entt::entity e : positions) {
        const Position& p = positions.get<const Position>(e);
        if (!world.movement().grid().passable({p.x.floor_to_int(), p.y.floor_to_int()})) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("HPA*: la reconstrucción incremental es idéntica a construir desde cero") {
    const TileMap map = rts::sim::generate_map(rts::test::test_map_params());
    const std::vector<std::uint8_t> passable{0, 1, 1};
    PassGrid grid(map, passable);
    GridSearch search(map.width(), map.height());
    const rts::sim::HpaParams params{16, 8};
    HpaGraph incremental(grid, params, search);
    Xoshiro256pp rng(99);
    std::int32_t recomputed_total = 0;
    for (std::int32_t round = 0; round < 12; ++round) {
        // Bloquea (o, cada tres rondas, libera) unos rectángulos al azar, como haría una
        // tanda de edificios o de árboles talados.
        std::vector<std::uint8_t> dirty(incremental.sector_count(), 0);
        for (std::int32_t k = 0; k < 4; ++k) {
            const auto x0 = static_cast<std::int32_t>(rng.next_below(250));
            const auto y0 = static_cast<std::int32_t>(rng.next_below(250));
            const auto size = 1 + static_cast<std::int32_t>(rng.next_below(4));
            for (std::int32_t y = y0; y < y0 + size; ++y) {
                for (std::int32_t x = x0; x < x0 + size; ++x) {
                    grid.set_blocked({x, y}, round % 3 != 2);
                    dirty[static_cast<std::size_t>(incremental.sector_of({x, y}))] = 1;
                }
            }
        }
        grid.relabel_components();
        recomputed_total += incremental.rebuild(grid, dirty, search);
        const HpaGraph fresh(grid, params, search);
        REQUIRE(incremental.same_graph(fresh));
    }
    MESSAGE("sectores recalculados en 12 rondas de 4 cambios: " << recomputed_total << " de "
                                                                 << 12 * static_cast<std::int32_t>(incremental.sector_count()));
    // Cada cambio toca 1-4 sectores y a lo sumo sus vecinos cambian de portales.
    CHECK(recomputed_total < 12 * 4 * 9);
}

TEST_CASE("Rejilla: bloquear y liberar casillas cambia transitabilidad y componentes") {
    TileMap map(8, 1);
    const std::vector<std::uint8_t> passable{1};
    PassGrid grid(map, passable);
    CHECK(grid.component({0, 0}) == grid.component({7, 0}));
    grid.set_blocked({4, 0}, true);
    CHECK_FALSE(grid.passable({4, 0}));
    CHECK(grid.terrain_passable({4, 0}));
    grid.relabel_components();
    CHECK(grid.component({0, 0}) != grid.component({7, 0}));
    CHECK(grid.component_size(grid.component({0, 0})) == 4);
    grid.set_blocked({4, 0}, false);
    grid.relabel_components();
    CHECK(grid.component({0, 0}) == grid.component({7, 0}));
}

TEST_CASE("Economía: un aldeano junto al árbol y al almacén recoge con el tiempo exacto") {
    World world(open_field());
    world.spawn_building(0, kCenter, {100, 100}, true);  // ocupa 100..102
    const auto tree = world.spawn_node(kTree, {104, 101});
    REQUIRE(tree);
    // En la casilla (103, 101): a 0,5 casillas del centro urbano y del árbol.
    const std::uint32_t v = world.spawn_unit(0, kVillager, {103, 101});
    const std::int32_t start_wood = world.player_state(0).stock[kWood];
    world.issue(command(CommandType::Gather, {v}, *tree));

    // Madera: 12 ticks por unidad, 10 por viaje. Una unidad cada 12 ticks desde el
    // primero; con 10 encima, al tick siguiente descarga. Ciclo: 12 * 10 + 1 = 121.
    constexpr std::int32_t kCycle = 12 * 10 + 1;
    run(world, kCycle - 1);
    CHECK(world.player_state(0).stock[kWood] == start_wood);
    CHECK(world.registry().get<Worker>(ent(v)).carried == 10);
    CHECK(world.registry().get<Worker>(ent(v)).task == WorkerTask::Deliver);
    run(world, 1);
    CHECK(world.player_state(0).stock[kWood] == start_wood + 10);
    run(world, kCycle);
    CHECK(world.player_state(0).stock[kWood] == start_wood + 20);
    CHECK(world.registry().get<ResourceNode>(ent(*tree)).amount == 80);
    // No se movió: ninguna orden de desplazamiento interna.
    CHECK_FALSE(world.registry().all_of<MoveGoal>(ent(v)));
}

TEST_CASE("Economía: los recursos se conservan (recogido = entregado + en mano)") {
    World world(open_field());
    world.spawn_building(0, kCenter, {100, 100}, true);
    std::vector<std::uint32_t> trees;
    for (std::int32_t i = 0; i < 4; ++i) {
        trees.push_back(*world.spawn_node(kTree, {110 + 2 * i, 108}));
    }
    std::vector<std::uint32_t> villagers;
    for (std::int32_t i = 0; i < 6; ++i) {
        villagers.push_back(world.spawn_unit(0, kVillager, {98 + i, 98}));
    }
    const std::int32_t start_wood = world.player_state(0).stock[kWood];
    world.issue(command(CommandType::Gather, villagers, trees[0]));
    const std::int32_t total = 4 * 100;
    for (std::int32_t t = 0; t < 3000; ++t) {
        world.step();
        std::int32_t remaining = 0;
        const auto nodes = world.registry().view<const ResourceNode>();
        for (const entt::entity e : nodes) {
            remaining += nodes.get<const ResourceNode>(e).amount;
        }
        std::int32_t carried = 0;
        const auto workers = world.registry().view<const Worker>();
        for (const entt::entity e : workers) {
            carried += workers.get<const Worker>(e).carried;
        }
        REQUIRE(world.player_state(0).stock[kWood] - start_wood + carried + remaining == total);
    }
    // En 150 s, 6 aldeanos a 12 ticks por unidad y con paseos cortos agotan los árboles.
    MESSAGE("madera entregada: " << world.player_state(0).stock[kWood] - start_wood);
    CHECK(world.player_state(0).stock[kWood] - start_wood > 200);
    CHECK(all_on_passable_tiles(world));
}

TEST_CASE("Economía: un nodo agotado libera su casilla y el aldeano busca otro cercano") {
    World world(open_field());
    world.spawn_building(0, kCenter, {100, 100}, true);
    const auto bush = world.spawn_node(kBerries, {104, 101});  // 20 de comida
    const auto other = world.spawn_node(kBerries, {106, 104});
    const std::uint32_t v = world.spawn_unit(0, kVillager, {103, 101});
    world.issue(command(CommandType::Gather, {v}, *bush));
    world.step();
    REQUIRE_FALSE(world.movement().grid().passable({104, 101}));
    // Comida: 10 ticks por unidad; 20 unidades, dos viajes.
    run(world, 2 * (10 * 10 + 1) + 5);
    CHECK_FALSE(world.registry().valid(ent(*bush)));
    CHECK(world.movement().grid().passable({104, 101}));
    CHECK(world.object_at({104, 101}) == std::nullopt);
    const Worker& w = world.registry().get<Worker>(ent(v));
    CHECK(w.task == WorkerTask::Gather);
    CHECK(w.node == ent(*other));
    run(world, 600);
    // Sin almacén de partida: 20 del primer arbusto y 20 del segundo, ya agotado.
    CHECK(world.player_state(0).stock[kFood] == 40);
}

TEST_CASE("Economía: la cola cobra al encolar, reembolsa al cancelar y respeta la capacidad") {
    World world(open_field());
    const auto center = *world.spawn_building(0, kCenter, {100, 100}, true);
    world.set_stock(0, stock(1000, 0, 0, 0));
    Command train = command(CommandType::Train, {}, center);
    train.kind = kVillager;
    for (std::int32_t i = 0; i < 7; ++i) {
        world.issue(train);
    }
    world.step();
    const auto& q = world.registry().get<ProductionQueue>(ent(center));
    CHECK(q.items.size() == 5);  // capacidad 5: dos rechazadas sin cobrar
    CHECK(world.player_state(0).stock[kFood] == 1000 - 5 * 50);

    world.issue(command(CommandType::CancelTrain, {}, center));
    world.issue(command(CommandType::CancelTrain, {}, center));
    world.step();
    CHECK(q.items.size() == 3);
    CHECK(world.player_state(0).stock[kFood] == 1000 - 3 * 50);

    // Sin recursos no se encola.
    world.set_stock(0, stock(10, 0, 0, 0));
    world.issue(train);
    world.step();
    CHECK(q.items.size() == 3);
    CHECK(world.player_state(0).stock[kFood] == 10);

    // Otro jugador no puede usar el edificio.
    Command foreign = train;
    foreign.player = 1;
    world.issue(foreign);
    world.step();
    CHECK(q.items.size() == 3);
}

TEST_CASE("Economía: sin plazas de población la cola se detiene y sigue al construir una casa") {
    World world(open_field());
    const auto center = *world.spawn_building(0, kCenter, {100, 100}, true);  // 5 plazas
    for (std::int32_t i = 0; i < 5; ++i) {
        world.spawn_unit(0, kVillager, {90 + i, 90});
    }
    world.set_stock(0, stock(500, 0, 0, 0));
    Command train = command(CommandType::Train, {}, center);
    train.kind = kVillager;
    world.issue(train);
    run(world, 200 + 50);  // más que el tiempo de entrenamiento
    const auto& q = world.registry().get<ProductionQueue>(ent(center));
    CHECK(units_of(world, kVillager) == 5);
    CHECK(q.items.size() == 1);
    CHECK(q.progress == 0);
    CHECK(world.player_state(0).population == 5);
    CHECK(world.player_state(0).population_cap == 5);

    world.spawn_building(0, kHouse, {110, 90}, true);
    run(world, 200 + 1);
    CHECK(units_of(world, kVillager) == 6);
    CHECK(q.items.empty());
    CHECK(world.player_state(0).population == 6);
    CHECK(world.player_state(0).population_cap == 10);
}

TEST_CASE("Economía: la unidad producida aparece junto al edificio, en casilla libre") {
    World world(open_field());
    const auto center = *world.spawn_building(0, kCenter, {100, 100}, true);
    world.set_stock(0, stock(1000, 0, 0, 1000));
    Command train = command(CommandType::Train, {}, center);
    train.kind = kSoldier;
    world.issue(train);
    world.issue(train);
    run(world, 2 * 100 + 1);
    CHECK(units_of(world, kSoldier) == 2);
    const auto units = world.registry().view<const Position, const Unit>();
    for (const entt::entity e : units) {
        const Position& p = units.get<const Position>(e);
        const TileCoord t{p.x.floor_to_int(), p.y.floor_to_int()};
        CHECK(world.movement().grid().passable(t));
        CHECK(std::max(std::abs(t.x - 101), std::abs(t.y - 101)) <= 3);
    }
}

TEST_CASE("Economía: tres constructores terminan en un tercio del tiempo que uno") {
    auto build_ticks = [](std::int32_t builders) {
        World world(open_field());
        world.set_stock(0, stock(0, 100, 0, 0));
        std::vector<std::uint32_t> ids;
        // Junto a la huella de la casa (110..111, 100..101), sin pisarla.
        const TileCoord spots[] = {{109, 100}, {112, 101}, {110, 102}};
        for (std::int32_t i = 0; i < builders; ++i) {
            ids.push_back(world.spawn_unit(0, kVillager, spots[i]));
        }
        Command place = command(CommandType::Place, ids);
        place.kind = kHouse;
        place.target = {110, 100};
        world.issue(place);
        world.step();
        const auto house = world.object_at({110, 100});
        REQUIRE(house);
        CHECK(world.player_state(0).stock[kWood] == 70);  // cobrada al colocar
        std::int32_t ticks = 1;
        while (!world.registry().get<Building>(ent(*house)).complete && ticks < 2000) {
            world.step();
            ++ticks;
        }
        CHECK(world.registry().get<rts::sim::Health>(ent(*house)).hp == 500);
        world.step();  // la población se recuenta al empezar cada tick
        CHECK(world.player_state(0).population_cap == 5);
        return ticks;
    };
    const std::int32_t one = build_ticks(1);
    const std::int32_t three = build_ticks(3);
    MESSAGE("casa de 300 ticks de trabajo: 1 constructor " << one << " ticks, 3 constructores " << three);
    CHECK(one == 300);
    CHECK(three == 100);
}

TEST_CASE("Economía: una colocación inválida no cobra ni bloquea") {
    World world(open_field());
    world.set_stock(0, stock(0, 100, 0, 0));
    const std::uint32_t v = world.spawn_unit(0, kVillager, {120, 120});
    world.spawn_node(kTree, {131, 130});
    Command place = command(CommandType::Place, {v});
    place.kind = kHouse;
    SUBCASE("encima de una unidad") {
        place.target = {119, 119};
    }
    SUBCASE("encima de un árbol") {
        place.target = {130, 130};
    }
    SUBCASE("fuera del mapa") {
        place.target = {255, 10};
    }
    world.issue(place);
    world.step();
    CHECK(world.player_state(0).stock[kWood] == 100);
    CHECK(world.registry().view<const Building>().size() == 0);
}

TEST_CASE("Economía: un edificio que corta el camino obliga a replanificar sin pisarlo") {
    WorldParams p = open_field();
    World world(p);
    std::vector<std::uint32_t> single{world.spawn_unit(0, kSoldier, {60, 100})};
    std::vector<std::uint32_t> group;
    for (std::int32_t i = 0; i < 12; ++i) {
        group.push_back(world.spawn_unit(0, kSoldier, {60 + i % 4, 110 + i / 4}));
    }
    world.issue(rts::test::move_order(0, single, {100, 100}));
    world.issue(rts::test::move_order(0, group, {100, 111}));
    run(world, 40);
    // Muros de 3x3 atravesados en ambas rutas cuando ya van de camino.
    for (std::int32_t y = 95; y <= 116; y += 3) {
        REQUIRE(world.spawn_building(0, kCenter, {80, y}, true));
    }
    std::int32_t invalidated = 0;
    for (std::int32_t t = 0; t < 2000; ++t) {
        world.step();
        invalidated += world.movement().last_stats().paths_invalidated;
        REQUIRE(all_on_passable_tiles(world));
    }
    CHECK(invalidated >= 1);
    std::int32_t arrived = 0;
    const auto goals = world.registry().view<const MoveGoal>();
    for (const entt::entity e : goals) {
        arrived += goals.get<const MoveGoal>(e).arrived ? 1 : 0;
    }
    MESSAGE("caminos invalidados: " << invalidated << "; llegadas: " << arrived << " de 13");
    CHECK(arrived == 13);
    // El grafo tras los cambios es el mismo que se construiría desde cero.
    GridSearch search(256, 256);
    CHECK(world.movement().hpa().same_graph(HpaGraph(world.movement().grid(), p.movement.hpa, search)));
}

TEST_CASE("Economía: Move cancela la tarea y Build sobre un almacén descarga") {
    World world(open_field());
    const auto center = *world.spawn_building(0, kCenter, {100, 100}, true);
    const auto tree = *world.spawn_node(kTree, {104, 101});
    const std::uint32_t v = world.spawn_unit(0, kVillager, {103, 101});
    world.issue(command(CommandType::Gather, {v}, tree));
    run(world, 12 * 5);
    CHECK(world.registry().get<Worker>(ent(v)).carried == 5);
    world.issue(rts::test::move_order(world.tick(), {v}, {110, 110}));
    world.step();
    CHECK(world.registry().get<Worker>(ent(v)).task == WorkerTask::Idle);
    CHECK(world.registry().get<Worker>(ent(v)).carried == 5);  // la carga se conserva
    run(world, 400);
    const std::int32_t before = world.player_state(0).stock[kWood];
    world.issue(command(CommandType::Build, {v}, center));
    for (std::int32_t t = 0; t < 600 && world.registry().get<Worker>(ent(v)).carried > 0; ++t) {
        world.step();
    }
    CHECK(world.player_state(0).stock[kWood] == before + 5);
    // Tras descargar vuelve a su árbol.
    CHECK(world.registry().get<Worker>(ent(v)).task == WorkerTask::Gather);
    CHECK(world.registry().get<Worker>(ent(v)).node == ent(tree));
}

TEST_CASE("Preparación: edificio inicial, aldeanos y recursos cerca de cada jugador") {
    WorldParams p = rts::test::test_world_params(0, 16);
    p.setup.seed = 11;
    p.setup.starts = {{60, 60}, {196, 196}};
    p.setup.start_search_radius = 60;
    p.setup.min_start_region_tiles = 2000;
    p.setup.start_building = kCenter;
    p.setup.start_unit = kVillager;
    p.setup.start_units = 4;
    p.setup.start_stock = stock(200, 200, 100, 100);
    p.setup.near_start = {{rts::test::kGoldMine, 1, 6, 10}, {kBerries, 5, 4, 7}};
    p.setup.forest_terrain = 2;
    p.setup.tree_type = kTree;
    p.setup.tree_density_permille = 300;
    p.setup.clear_radius = 7;
    const World world(p);
    rts::sim::Snapshot snap;
    world.write_snapshot(snap);
    REQUIRE(snap.players.size() == 2);
    std::int32_t centers = 0;
    std::int32_t trees = 0;
    for (const auto& o : snap.objects) {
        if (o.kind == rts::sim::ObjectKind::Building) {
            ++centers;
            CHECK(o.complete);
            // Claro: ningún árbol a menos de clear_radius del centro del edificio.
            const TileCoord c{o.origin.x + 1, o.origin.y + 1};
            for (const auto& n : snap.objects) {
                if (n.kind == rts::sim::ObjectKind::Resource && n.type == kTree) {
                    CHECK(std::max(std::abs(n.origin.x - c.x), std::abs(n.origin.y - c.y)) > 7);
                }
            }
        } else if (o.type == kTree) {
            ++trees;
        }
    }
    CHECK(centers == 2);
    CHECK(trees > 100);
    CHECK(snap.entities.size() == 8);
    CHECK(snap.players[0].population == 4);
    CHECK(snap.players[0].population_cap == 5);
    CHECK(snap.players[1].stock[kWood] == 200);
    CHECK(all_on_passable_tiles(world));
}
