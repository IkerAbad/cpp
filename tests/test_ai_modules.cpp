// Pruebas de los módulos de la IA que piensan mejor (perfil "normal"), cada uno por
// separado sobre un mapa llano: se llama a AiSystem::think y se miran sus órdenes.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include <doctest/doctest.h>

#include "sim/ai.hpp"
#include "sim/world.hpp"
#include "test_helpers.hpp"

using rts::sim::AiBehavior;
using rts::sim::AiParams;
using rts::sim::AiSystem;
using rts::sim::Command;
using rts::sim::CommandType;
using rts::sim::World;
using rts::sim::WorldParams;
using namespace rts::test;

namespace {

constexpr rts::sim::PlayerId kAi = 1;
constexpr rts::sim::PlayerId kRival = 0;
// El jugador 1 decide en los ticks con tick % 20 == 1.
constexpr rts::sim::Tick kThinkTick = 1;

WorldParams flat_two_players() {
    WorldParams p = test_world_params(0, 16);
    p.map.bands = {{rts::sim::kElevationRange, 1}};
    p.demo.player = 1;  // dos jugadores
    return p;
}

// Órdenes de un solo módulo del perfil "normal" (índice 1 de test_ai_params).
std::vector<Command> think_with(const World& world, std::vector<AiBehavior> behaviors) {
    AiParams params = test_ai_params();
    params.profiles[1].behaviors = std::move(behaviors);
    AiSystem ai(params, rts::sim::SupplyParams{}, {{kAi, 1}});
    std::vector<Command> out;
    ai.think(world.registry(), world.economy(), world.movement().grid(), kThinkTick, out);
    return out;
}

// Base de la IA: centro urbano (da plazas) y cuartel terminados, y dinero de sobra.
void ai_base(World& world) {
    world.spawn_building(kAi, kCenter, {150, 150}, true);
    world.spawn_building(kAi, kBarracks, {160, 150}, true);
    world.set_stock(kAi, stock(1000, 1000, 1000, 1000));
}

}  // namespace

TEST_CASE("IA normal: el ejército se elige contra lo que tiene el rival") {
    WorldParams p = flat_two_players();
    // Arqueros muy eficaces contra infantería: contra soldados conviene el arquero.
    p.unit_types[kArcher].combat.bonus[kClassInfantry] = 20;

    SUBCASE("contra un ejército de soldados, arqueros") {
        World world(p);
        ai_base(world);
        for (std::int32_t i = 0; i < 8; ++i) {
            world.spawn_unit(kRival, kSoldier, {60 + i, 60});
        }
        world.step();
        const auto out = think_with(world, {AiBehavior::ArmyCounter});
        REQUIRE(out.size() == 1);
        CHECK(out[0].type == CommandType::Train);
        CHECK(out[0].kind == kArcher);
    }
    SUBCASE("sin unidades armadas enemigas, lo que antes derriba edificios") {
        World world(p);
        ai_base(world);
        world.spawn_building(kRival, kHouse, {60, 60}, true);
        world.spawn_unit(kRival, kVillager, {70, 60});  // desarmado: no cuenta como ejército
        world.step();
        // Soldado contra casa: 5 - 3 = 2 por golpe; arquero: 4 - 8 < 0, el mínimo, 1.
        const auto out = think_with(world, {AiBehavior::ArmyCounter});
        REQUIRE(out.size() == 1);
        CHECK(out[0].kind == kSoldier);
    }
}

TEST_CASE("IA normal: fuego concentrado en el enemigo armado que antes cae") {
    World world(flat_two_players());
    ai_base(world);
    const auto a = world.spawn_unit(kAi, kSoldier, {100, 100});
    const auto b = world.spawn_unit(kAi, kSoldier, {100, 101});
    // El soldado enemigo está más cerca, pero el arquero cae en 6 golpes (30 / 5) y el
    // soldado en 10 (40 / (5 - 1)).
    const auto soldier = world.spawn_unit(kRival, kSoldier, {102, 100});
    const auto archer = world.spawn_unit(kRival, kArcher, {104, 101});
    const auto villager = world.spawn_unit(kRival, kVillager, {101, 102});  // desarmado: no es prioridad
    Command c;
    c.player = kAi;
    c.type = CommandType::AttackMove;
    c.units = {a, b};
    c.target = {110, 100};
    world.issue(c);
    world.step();

    const auto out = think_with(world, {AiBehavior::FocusFire});
    REQUIRE_FALSE(out.empty());
    for (const Command& o : out) {
        CHECK(o.type == CommandType::Attack);
        CHECK(o.object == archer);
        CHECK(o.object != soldier);
        CHECK(o.object != villager);
    }
    std::vector<std::uint32_t> attackers;
    for (const Command& o : out) {
        attackers.insert(attackers.end(), o.units.begin(), o.units.end());
    }
    std::ranges::sort(attackers);
    CHECK(attackers == std::vector<std::uint32_t>{std::min(a, b), std::max(a, b)});
}

TEST_CASE("IA normal: ataca cuando su fuerza supera a la enemiga, y no antes") {
    SUBCASE("ocho soldados contra dos: ataca el edificio enemigo") {
        World world(flat_two_players());
        ai_base(world);
        for (std::int32_t i = 0; i < 8; ++i) {
            world.spawn_unit(kAi, kSoldier, {145, 140 + i});
        }
        world.spawn_unit(kRival, kSoldier, {60, 60});
        world.spawn_unit(kRival, kSoldier, {61, 60});
        world.spawn_building(kRival, kHouse, {50, 50}, true);
        world.step();
        const auto out = think_with(world, {AiBehavior::AttackStrength});
        REQUIRE(out.size() == 1);
        CHECK(out[0].type == CommandType::AttackMove);
        CHECK(out[0].units.size() == 8);
        CHECK(out[0].target == rts::sim::TileCoord{51, 51});  // centro de la casa
    }
    SUBCASE("ocho soldados contra veinte: espera") {
        World world(flat_two_players());
        ai_base(world);
        for (std::int32_t i = 0; i < 8; ++i) {
            world.spawn_unit(kAi, kSoldier, {145, 140 + i});
        }
        for (std::int32_t i = 0; i < 20; ++i) {
            world.spawn_unit(kRival, kSoldier, {60 + i % 10, 60 + i / 10});
        }
        world.step();
        CHECK(think_with(world, {AiBehavior::AttackStrength}).empty());
    }
}

TEST_CASE("IA normal: las unidades que quedan sueltas lejos de casa no se quedan quietas") {
    WorldParams p = flat_two_players();
    SUBCASE("sin ejército en campaña, siguen hacia el centro urbano enemigo") {
        World world(p);
        ai_base(world);
        std::vector<std::uint32_t> stranded;
        for (std::int32_t i = 0; i < 3; ++i) {
            stranded.push_back(world.spawn_unit(kAi, kSoldier, {70, 60 + i}));
        }
        world.spawn_building(kRival, kCenter, {50, 50}, true);
        world.step();
        const auto out = think_with(world, {AiBehavior::AttackStrength});
        REQUIRE(out.size() == 1);
        CHECK(out[0].type == CommandType::AttackMove);
        CHECK(out[0].target == rts::sim::TileCoord{51, 51});  // centro del centro urbano enemigo
        std::vector<std::uint32_t> units = out[0].units;
        std::ranges::sort(units);
        CHECK(units == stranded);
    }
    SUBCASE("con ejército en campaña, se unen a él") {
        World world(p);
        ai_base(world);
        const auto loose = world.spawn_unit(kAi, kSoldier, {100, 100});
        const auto a = world.spawn_unit(kAi, kSoldier, {60, 70});
        const auto b = world.spawn_unit(kAi, kSoldier, {62, 70});
        world.spawn_building(kRival, kCenter, {50, 50}, true);
        Command c;
        c.player = kAi;
        c.type = CommandType::AttackMove;
        c.units = {a, b};
        c.target = {51, 51};
        world.issue(c);
        world.step();
        const auto out = think_with(world, {AiBehavior::AttackStrength});
        REQUIRE(out.size() == 1);
        CHECK(out[0].type == CommandType::AttackMove);
        CHECK(out[0].units == std::vector<std::uint32_t>{loose});
        // Hacia el centro del ejército en campaña, no al objetivo.
        CHECK(out[0].target != rts::sim::TileCoord{51, 51});
        CHECK(std::abs(out[0].target.x - 61) <= 1);
        CHECK(std::abs(out[0].target.y - 70) <= 1);
    }
}

TEST_CASE("IA normal: no reordena a las que tienen enemigos a la vista") {
    // Paradas junto a la base enemiga eligen blanco solas: reordenarlas les cortaría el ataque.
    World world(flat_two_players());
    ai_base(world);
    for (std::int32_t i = 0; i < 3; ++i) {
        world.spawn_unit(kAi, kSoldier, {55, 50 + i});
    }
    world.spawn_building(kRival, kCenter, {50, 50}, true);
    world.step();
    CHECK(think_with(world, {AiBehavior::AttackStrength}).empty());
}

TEST_CASE("IA normal: se retira de una batalla perdida") {
    World world(flat_two_players());
    ai_base(world);
    const auto a = world.spawn_unit(kAi, kSoldier, {60, 64});
    const auto b = world.spawn_unit(kAi, kSoldier, {61, 64});
    for (std::int32_t i = 0; i < 10; ++i) {
        world.spawn_unit(kRival, kSoldier, {56 + i, 58});
    }
    Command c;
    c.player = kAi;
    c.type = CommandType::AttackMove;
    c.units = {a, b};
    c.target = {60, 50};
    world.issue(c);
    world.step();

    const auto out = think_with(world, {AiBehavior::AttackStrength});
    REQUIRE(out.size() == 1);
    CHECK(out[0].type == CommandType::Move);
    CHECK(out[0].target == rts::sim::TileCoord{151, 151});  // centro del centro urbano
    std::vector<std::uint32_t> units = out[0].units;
    std::ranges::sort(units);
    CHECK(units == std::vector<std::uint32_t>{std::min(a, b), std::max(a, b)});
}

TEST_CASE("IA normal: manda aldeanos a apagar sus edificios en llamas") {
    World world(flat_two_players());
    const auto house = *world.spawn_building(kAi, kHouse, {120, 120}, true);
    std::vector<std::uint32_t> villagers;
    for (std::int32_t i = 0; i < 5; ++i) {
        villagers.push_back(world.spawn_unit(kAi, kVillager, {110 + i, 110}));
    }
    // Prenderle fuego: soldados enemigos atacan la casa.
    std::vector<std::uint32_t> raiders;
    for (std::int32_t i = 0; i < 3; ++i) {
        raiders.push_back(world.spawn_unit(kRival, kSoldier, {119, 119 + i}));
    }
    world.step();
    Command attack;
    attack.player = kRival;
    attack.type = CommandType::Attack;
    attack.units = raiders;
    attack.object = house;
    world.issue(attack);
    // Hasta que el fuego se sostiene solo (por encima de 200).
    const auto& reg = world.registry();
    for (int t = 0; t < 600 && (!reg.all_of<rts::sim::Fire>(static_cast<entt::entity>(house)) ||
                                reg.get<rts::sim::Fire>(static_cast<entt::entity>(house)).intensity < 300);
         ++t) {
        world.step();
    }
    REQUIRE(world.registry().all_of<rts::sim::Fire>(static_cast<entt::entity>(house)));
    // Con los incendiarios aún junto a la casa no manda a nadie a su lado.
    CHECK(think_with(world, {AiBehavior::Extinguish}).empty());
    // Se retiran: ahora sí.
    Command away;
    away.player = kRival;
    away.type = CommandType::Move;
    away.units = raiders;
    away.target = {60, 60};
    world.issue(away);
    for (int t = 0; t < 100; ++t) {
        world.step();
    }
    REQUIRE(world.registry().all_of<rts::sim::Fire>(static_cast<entt::entity>(house)));
    const auto out = think_with(world, {AiBehavior::Extinguish});
    REQUIRE(out.size() == 1);
    CHECK(out[0].type == CommandType::Extinguish);
    CHECK(out[0].object == house);
    CHECK(out[0].units.size() == 3);  // extinguishers_per_fire del perfil de prueba
}

TEST_CASE("IA normal: incursión contra un edificio de madera sin defensa, no contra uno defendido") {
    World world(flat_two_players());
    ai_base(world);
    std::vector<std::uint32_t> riders;
    for (std::int32_t i = 0; i < 3; ++i) {
        riders.push_back(world.spawn_unit(kAi, kSoldier, {145, 140 + i}));
    }
    const auto guarded = *world.spawn_building(kRival, kHouse, {120, 120}, true);  // más cerca, defendida
    for (std::int32_t i = 0; i < 4; ++i) {
        world.spawn_unit(kRival, kSoldier, {118, 118 + i});
    }
    const auto lonely = *world.spawn_building(kRival, kHouse, {60, 60}, true);
    world.step();
    const auto out = think_with(world, {AiBehavior::Raid});
    REQUIRE(out.size() == 1);
    CHECK(out[0].type == CommandType::Attack);
    CHECK(out[0].object == lonely);
    CHECK(out[0].object != guarded);
    std::vector<std::uint32_t> units = out[0].units;
    std::ranges::sort(units);
    CHECK(units == riders);
}

TEST_CASE("IA normal: construye el taller de asedio cuando tiene cuartel") {
    World world(flat_two_players());
    ai_base(world);  // centro urbano y cuartel terminados, dinero de sobra
    world.spawn_unit(kAi, kVillager, {150, 145});
    world.step();
    const auto out = think_with(world, {AiBehavior::Workshop});
    REQUIRE(out.size() == 1);
    CHECK(out[0].type == CommandType::Place);
    CHECK(out[0].kind == kWorkshop);
}

TEST_CASE("IA: la tropa corta de víveres vuelve a abastecerse; la que ya está junto al almacén espera") {
    WorldParams p = flat_two_players();
    p.unit_types[kSoldier].supply.rations = 10;
    p.unit_types[kSoldier].supply.ration_ticks = 1;  // se gastan enseguida
    p.building_types[kCenter].supplies = true;
    p.supply.resupply_radius_tiles = 3;
    p.supply.resupply_interval_ticks = 1000;  // que no repongan durante la prueba
    p.supply.ration_cost = stock(1, 0, 0, 0);
    World world(p);
    ai_base(world);
    const auto far = world.spawn_unit(kAi, kSoldier, {100, 100});
    const auto near = world.spawn_unit(kAi, kSoldier, {148, 151});
    for (std::int32_t t = 0; t < 8; ++t) {
        world.step();  // quedan 2 de 10 raciones: 20 %
    }
    AiParams params = test_ai_params();
    params.profiles[1].behaviors = {AiBehavior::Resupply};
    params.profiles[1].resupply_percent = 30;
    AiSystem ai(params, p.supply, {{kAi, 1}});
    std::vector<Command> out;
    ai.think(world.registry(), world.economy(), world.movement().grid(), kThinkTick, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].type == CommandType::Move);
    CHECK(out[0].units == std::vector<std::uint32_t>{far});
    CHECK(out[0].target == rts::sim::TileCoord{151, 151});  // centro del centro urbano
    CHECK(std::ranges::find(out[0].units, near) == out[0].units.end());
}

namespace {

// Mundo con bagaje (acémila, tipo 4) y una IA que lo usa.
struct BaggageSetup {
    WorldParams params = flat_two_players();
    AiParams ai = test_ai_params();
    rts::sim::UnitTypeId mule = 0;

    BaggageSetup() {
        rts::sim::UnitType m = params.unit_types[kVillager];
        m.worker = false;
        m.carry_capacity = 0;
        m.convoy_capacity = 20;
        mule = static_cast<rts::sim::UnitTypeId>(params.unit_types.size());
        params.unit_types.push_back(m);
        params.building_types[kCenter].trains.push_back(mule);
        params.building_types[kCenter].supplies = true;
        params.unit_types[kSoldier].supply.rations = 10;
        params.unit_types[kSoldier].supply.ration_ticks = 1;
        params.supply.resupply_radius_tiles = 3;
        params.supply.resupply_interval_ticks = 1000;
        params.supply.ration_cost = stock(1, 0, 0, 0);
        params.supply.convoy_mix = stock(100, 0, 0, 0);
        params.supply.convoy_reach = rts::sim::Fixed::from_ratio(4, 5);
        ai.carrier = mule;
        auto& p = ai.profiles[1];
        p.behaviors = {AiBehavior::Resupply, AiBehavior::Logistics};
        p.resupply_percent = 30;
        p.convoy_carriers = 2;
        p.baggage_offset_tiles = 4;
        p.min_attack_army = 2;
    }

    std::vector<Command> think(const World& world) const {
        AiSystem system(ai, params.supply, {{kAi, 1}});
        std::vector<Command> out;
        system.think(world.registry(), world.economy(), world.movement().grid(), kThinkTick, out);
        return out;
    }
};

const Command* find_order(const std::vector<Command>& out, CommandType type) {
    const auto it = std::ranges::find(out, type, &Command::type);
    return it != out.end() ? &*it : nullptr;
}

}  // namespace

TEST_CASE("IA: con ejército para atacar entrena bagaje; el vacío va a cargar a casa") {
    BaggageSetup s;
    World world(s.params);
    ai_base(world);
    world.spawn_unit(kAi, kSoldier, {140, 150});
    world.spawn_unit(kAi, kSoldier, {141, 150});
    const auto mule = world.spawn_unit(kAi, s.mule, {145, 155});
    world.step();
    const auto out = s.think(world);
    const Command* train = find_order(out, CommandType::Train);
    REQUIRE(train != nullptr);
    CHECK(train->kind == s.mule);
    const Command* convoy = find_order(out, CommandType::Convoy);
    REQUIRE(convoy != nullptr);
    CHECK(convoy->units == std::vector<std::uint32_t>{mule});
}

TEST_CASE("IA: el bagaje cargado sigue al ejército en campaña por detrás; las tropas se abastecen en él") {
    BaggageSetup s;
    World world(s.params);
    ai_base(world);
    world.set_stock(kAi, stock(1000, 0, 0, 0));
    const auto mule = world.spawn_unit(kAi, s.mule, {150, 160});
    world.step();
    world.issue([&] {
        Command c;
        c.type = CommandType::Convoy;
        c.player = kAi;
        c.units = {mule};
        c.object = *world.object_at({150, 150});
        return c;
    }());
    for (std::int32_t t = 0; t < 300; ++t) {
        world.step();
    }
    REQUIRE(world.registry().get<rts::sim::Carrier>(static_cast<entt::entity>(mule)).load[0] == 20);
    // Ejército en campaña lejos de la base, avanzando hacia el enemigo.
    std::vector<std::uint32_t> army;
    for (std::int32_t i = 0; i < 3; ++i) {
        army.push_back(world.spawn_unit(kAi, kSoldier, {100 + i, 100}));
    }
    Command go;
    go.type = CommandType::AttackMove;
    go.player = kAi;
    go.units = army;
    go.target = {60, 60};
    world.issue(go);
    world.step();
    const auto out = s.think(world);
    const auto follows = std::ranges::find_if(out, [&](const Command& c) {
        return c.type == CommandType::Move && c.units == std::vector<std::uint32_t>{mule};
    });
    REQUIRE(follows != out.end());
    // Detrás del ejército, hacia la base: más cerca de la base que el ejército.
    CHECK(follows->target.x > 101);
    CHECK(follows->target.y > 100);
}

TEST_CASE("IA: la incursión va antes a por el bagaje enemigo sin escolta que a quemar") {
    BaggageSetup s;
    s.ai.profiles[1].behaviors = {AiBehavior::Raid};
    World world(s.params);
    ai_base(world);
    for (std::int32_t i = 0; i < 3; ++i) {
        world.spawn_unit(kAi, kSoldier, {145, 140 + i});
    }
    world.spawn_building(kRival, kHouse, {120, 120}, true);   // más cerca, sin defensa
    const auto mule = world.spawn_unit(kRival, s.mule, {60, 60});  // lejos, sin escolta
    world.step();
    const auto out = s.think(world);
    REQUIRE(out.size() == 1);
    CHECK(out[0].type == CommandType::Attack);
    CHECK(out[0].object == mule);
}

TEST_CASE("IA: con campamento en uso y almacén suficiente, monta allí el ingenio de asedio") {
    BaggageSetup s;
    // Campamento de prueba (copia de la casa) que monta arietes con su almacén.
    rts::sim::BuildingType camp = s.params.building_types[kHouse];
    camp.supplies = true;
    camp.store_capacity = 500;
    camp.store_target = stock(0, 200, 0, 0, 50);
    camp.trains = {kRam};
    camp.population = 0;
    const auto camp_type = static_cast<rts::sim::BuildingTypeId>(s.params.building_types.size());
    s.params.building_types.push_back(camp);
    s.ai.camp = camp_type;
    s.ai.siege_engine = kRam;
    s.ai.profiles[1].behaviors = {AiBehavior::Logistics};
    s.ai.profiles[1].siege_engines = 1;
    World world(s.params);
    ai_base(world);
    REQUIRE(world.spawn_building(kAi, camp_type, {100, 100}, true));
    world.step();  // el campamento terminado recibe su almacén (vacío)
    CHECK(std::ranges::none_of(s.think(world), [](const Command& o) { return o.type == CommandType::Train && o.kind == kRam; }));
    // Con lo que cuesta el ariete en el almacén del campamento, lo monta allí.
    s.params.unit_types[kRam].cost = stock(0, 0, 0, 0, 0);
    World world2(s.params);
    ai_base(world2);
    const auto c2 = *world2.spawn_building(kAi, camp_type, {100, 100}, true);
    world2.step();
    const auto out = s.think(world2);
    const auto it = std::ranges::find_if(out, [](const Command& o) { return o.type == CommandType::Train; });
    REQUIRE(it != out.end());
    CHECK(it->object == c2);
    CHECK(it->kind == kRam);
}

TEST_CASE("IA: con niebla y sin conocer al enemigo, manda un explorador a lo no explorado") {
    WorldParams p = flat_two_players();
    p.vision.enabled = true;
    p.vision.interval_ticks = 1;
    p.vision.day_ticks = 1000;
    p.vision.night_sight_percent = 100;
    p.building_types[kCenter].sight_tiles = 6;
    World world(p);
    ai_base(world);
    REQUIRE(world.spawn_building(kRival, kCenter, {40, 40}, true));  // lejos: no lo ve
    const auto soldier = world.spawn_unit(kAi, kSoldier, {150, 156});
    world.step();
    AiParams params = test_ai_params();
    params.profiles[1].behaviors = {AiBehavior::Explore};
    params.profiles[1].scouts = 1;
    params.profiles[1].explore_step_tiles = 12;
    AiSystem ai(params, p.supply, {{kAi, 1}});
    std::vector<Command> out;
    ai.think(world.registry(), world.economy(), world.movement().grid(), kThinkTick, out, &world.vision());
    REQUIRE(out.size() == 1);
    CHECK(out[0].type == CommandType::Move);
    CHECK(out[0].units == std::vector<std::uint32_t>{soldier});
    CHECK_FALSE(world.vision().explored(kAi, out[0].target));
    // Sin niebla sabe dónde está el enemigo: nadie explora.
    std::vector<Command> none;
    ai.think(world.registry(), world.economy(), world.movement().grid(), kThinkTick, none);
    CHECK(none.empty());
}

TEST_CASE("IA: ante un recinto cerrado abre brecha en el muro más cercano; si hay paso, no") {
    WorldParams p = flat_two_players();
    p.unit_types[kSoldier].climbs = true;
    rts::sim::BuildingType wall = p.building_types[kHouse];
    wall.size = 1;
    wall.population = 0;
    wall.material = rts::sim::Material::Stone;
    wall.climbable = true;
    const auto wall_type = static_cast<rts::sim::BuildingTypeId>(p.building_types.size());
    p.building_types.push_back(wall);

    const auto ring = [&](World& world, bool gap) {
        // Centro urbano del rival en (60, 60)-(62, 62), rodeado a 2 casillas.
        world.spawn_building(kRival, kCenter, {60, 60}, true);
        for (std::int32_t i = 57; i <= 65; ++i) {
            for (const rts::sim::TileCoord t : {rts::sim::TileCoord{i, 57}, rts::sim::TileCoord{i, 65},
                                                rts::sim::TileCoord{57, i}, rts::sim::TileCoord{65, i}}) {
                if (gap && t == rts::sim::TileCoord{61, 65}) {
                    continue;
                }
                world.spawn_building(kRival, wall_type, t, true);
            }
        }
    };
    const auto army = [&](World& world) {
        std::vector<std::uint32_t> ids;
        for (std::int32_t i = 0; i < 3; ++i) {
            ids.push_back(world.spawn_unit(kAi, kSoldier, {61 + i, 72}));
        }
        ids.push_back(world.spawn_unit(kAi, kRam, {60, 73}));
        return ids;
    };

    SUBCASE("cerrado: el ariete golpea el muro y la infantería lo escala") {
        World world(p);
        ai_base(world);
        ring(world, false);
        army(world);
        world.step();
        AiParams params = test_ai_params();
        params.profiles[1].behaviors = {AiBehavior::Assault};
        params.ladder_cost = stock(0, 15, 0, 0);
        AiSystem ai(params, rts::sim::SupplyParams{}, {{kAi, 1}});
        std::vector<Command> out;
        ai.think(world.registry(), world.economy(), world.movement().grid(), kThinkTick, out);
        const auto attack = std::ranges::find(out, CommandType::Attack, &Command::type);
        const auto climb = std::ranges::find(out, CommandType::Climb, &Command::type);
        REQUIRE(attack != out.end());
        REQUIRE(climb != out.end());
        CHECK(attack->units.size() == 1);  // el ariete
        CHECK(climb->units.size() == 3);   // los soldados
        CHECK(attack->object == climb->object);
        // Un tramo del lado sur, el más cercano al ejército.
        const auto& f = world.registry().get<rts::sim::Footprint>(static_cast<entt::entity>(climb->object));
        CHECK(f.origin.y == 65);
    }
    SUBCASE("con un hueco: se llega, no hace falta brecha") {
        World world(p);
        ai_base(world);
        ring(world, true);
        army(world);
        world.step();
        CHECK(think_with(world, {AiBehavior::Assault}).empty());
    }
}
