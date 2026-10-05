// Pruebas de la moral y la desbandada (B1): las bajas cercanas desmoralizan; un golpe
// por el flanco o la espalda pesa más que de frente; rodeada, la tropa se desbanda,
// deja de pelear, huye y no obedece; lejos del enemigo se rehace y se reorganiza antes
// de volver a obedecer; la leva cede antes que el caballero; sin moral activada, nada
// cambia.

#include <cstdint>
#include <optional>
#include <vector>

#include <doctest/doctest.h>

#include "sim/world.hpp"
#include "test_helpers.hpp"

using rts::sim::Command;
using rts::sim::CommandType;
using rts::sim::Fixed;
using rts::sim::Morale;
using rts::sim::Routing;
using rts::sim::Stance;
using rts::sim::TileCoord;
using rts::sim::World;
using rts::sim::WorldParams;
using namespace rts::test;

namespace {

constexpr std::int32_t kRallyReorganize = 40;

WorldParams morale_params(std::int32_t resolve = 100) {
    WorldParams p = test_world_params(0, 16);
    p.map.bands = {{rts::sim::kElevationRange, 1}};
    p.demo.player = 1;
    p.unit_types[kSoldier].morale_resolve = resolve;
    p.unit_types[kSoldier].combat.hp = 400;  // aguanta lo bastante para desbandarse antes de morir
    auto& m = p.morale;
    m.enabled = true;
    m.interval_ticks = 5;
    m.rout_below = 250;
    m.rally_above = 600;
    m.awareness_radius = Fixed::from_int(6);
    m.rally_safe_radius = Fixed::from_int(8);
    m.casualty_loss = 60;
    m.enemy_casualty_gain = 20;
    m.hit_loss = 4;
    m.flank_hit_loss = 25;
    m.hero_death_loss = 300;
    m.hero_death_radius = Fixed::from_int(12);
    m.contagion_loss = 10;
    m.outnumbered_loss = 8;
    m.hungry_loss = 1;
    m.calm_ticks = 40;
    m.calm_gain = 10;
    m.comrade_gain = 2;
    m.comrade_cap = 8;
    m.hero_gain = 10;
    m.hero_loss_percent = 70;
    m.night_loss_percent = 130;
    m.routing_gain = 40;
    m.flee_tiles = 10;
    m.flee_repath_ticks = 20;
    m.rally_reorganize_ticks = kRallyReorganize;
    return p;
}

entt::entity ent(std::uint32_t id) {
    return static_cast<entt::entity>(id);
}

Command order(CommandType type, rts::sim::PlayerId player, std::vector<std::uint32_t> units) {
    Command c;
    c.type = type;
    c.player = player;
    c.units = std::move(units);
    return c;
}

void hold_ground(World& w, rts::sim::PlayerId player, std::vector<std::uint32_t> units) {
    Command c = order(CommandType::SetStance, player, std::move(units));
    c.kind = static_cast<std::uint8_t>(Stance::HoldGround);
    w.issue(c);
}

std::int32_t morale(const World& w, std::uint32_t id) {
    const Morale* m = w.registry().try_get<Morale>(ent(id));
    return m != nullptr ? m->value : -1;
}

bool routing(const World& w, std::uint32_t id) {
    return w.registry().valid(ent(id)) && w.registry().all_of<Routing>(ent(id));
}

Fixed distance(const World& w, std::uint32_t a, std::uint32_t b) {
    const auto& pa = w.registry().get<rts::sim::Position>(ent(a));
    const auto& pb = w.registry().get<rts::sim::Position>(ent(b));
    return rts::sim::length(rts::sim::FVec2{pa.x, pa.y} - rts::sim::FVec2{pb.x, pb.y});
}

// Un soldado del jugador 0 rodeado por cinco del jugador 1 que no le persiguen.
struct Surrounded {
    World world;
    std::uint32_t lone = 0;
    std::vector<std::uint32_t> foes;

    explicit Surrounded(WorldParams p) : world(p) {
        lone = world.spawn_unit(0, kSoldier, {30, 30});
        for (const TileCoord t : {TileCoord{31, 30}, TileCoord{29, 30}, TileCoord{30, 31}, TileCoord{30, 29},
                                  TileCoord{31, 31}}) {
            foes.push_back(world.spawn_unit(1, kSoldier, t));
        }
        hold_ground(world, 1, foes);
    }

    // Ticks hasta que se desbanda (nullopt si no lo hace en max_ticks).
    std::optional<std::int32_t> run_until_rout(std::int32_t max_ticks) {
        for (std::int32_t t = 0; t < max_ticks; ++t) {
            world.step();
            if (routing(world, lone)) {
                return t;
            }
        }
        return std::nullopt;
    }
};

}  // namespace

TEST_CASE("Moral: las unidades combatientes empiezan con la moral entera; los aldeanos no tienen") {
    World w(morale_params());
    const auto soldier = w.spawn_unit(0, kSoldier, {10, 10});
    const auto villager = w.spawn_unit(0, kVillager, {12, 10});
    w.step();
    CHECK(morale(w, soldier) == rts::sim::kFullMorale);
    CHECK(morale(w, villager) == -1);
}

TEST_CASE("Moral: rodeada, la tropa se desbanda, deja de pelear y huye lejos del enemigo") {
    Surrounded s(morale_params());
    const auto routed_at = s.run_until_rout(2000);
    REQUIRE(routed_at.has_value());
    CHECK(s.world.morale().routs_of(0) == 1);
    CHECK((s.world.registry().get<rts::sim::Combatant>(ent(s.lone)).target == entt::null));
    const Fixed before = distance(s.world, s.lone, s.foes[0]);
    for (int t = 0; t < 100 && routing(s.world, s.lone); ++t) {
        s.world.step();
        CHECK((s.world.registry().get<rts::sim::Combatant>(ent(s.lone)).target == entt::null));
    }
    CHECK(distance(s.world, s.lone, s.foes[0]) > before + Fixed::from_int(3));

    rts::sim::Snapshot snap;
    s.world.write_snapshot(snap);
    bool found = false;
    for (const auto& e : snap.entities) {
        if (e.id == s.lone) {
            found = true;
            CHECK(e.morale >= 0);
        }
    }
    CHECK(found);
}

TEST_CASE("Moral: en desbandada no obedece; se rehace lejos del enemigo, se reorganiza y vuelve a obedecer") {
    Surrounded s(morale_params());
    REQUIRE(s.run_until_rout(2000).has_value());
    // Ordenarle volver al centro del cerco: no obedece.
    s.world.issue(move_order(s.world.tick(), {s.lone}, {30, 30}));
    s.world.step();
    s.world.step();
    const Fixed d0 = distance(s.world, s.lone, s.foes[0]);
    for (int t = 0; t < 40 && routing(s.world, s.lone); ++t) {
        s.world.step();
    }
    CHECK(distance(s.world, s.lone, s.foes[0]) >= d0);

    // Lejos y sin perseguidores, se rehace.
    bool rallied = false;
    for (int t = 0; t < 3000 && !rallied; ++t) {
        s.world.step();
        rallied = !routing(s.world, s.lone);
    }
    REQUIRE(rallied);
    CHECK(s.world.registry().all_of<rts::sim::Reorganizing>(ent(s.lone)));
    CHECK(morale(s.world, s.lone) >= s.world.morale().params().rally_above);
    CHECK(distance(s.world, s.lone, s.foes[0]) > Fixed::from_int(8));

    // Rehecho, obedece.
    const TileCoord dest{4, 4};
    s.world.issue(move_order(s.world.tick(), {s.lone}, dest));
    for (int t = 0; t < 1500; ++t) {
        s.world.step();
    }
    const auto& p = s.world.registry().get<rts::sim::Position>(ent(s.lone));
    CHECK(rts::sim::tile_of(p) == dest);
}

TEST_CASE("Moral: la leva cede antes que el caballero") {
    Surrounded levy(morale_params(70));
    Surrounded knight(morale_params(150));
    const auto levy_at = levy.run_until_rout(2000);
    const auto knight_at = knight.run_until_rout(2000);
    REQUIRE(levy_at.has_value());
    CHECK((!knight_at.has_value() || *knight_at > *levy_at));
}

TEST_CASE("Moral: un golpe por la espalda pesa más que uno de frente") {
    WorldParams p = morale_params();
    p.morale.outnumbered_loss = 0;  // solo cuentan los golpes
    p.morale.calm_gain = 0;
    p.morale.comrade_gain = 0;
    p.morale.rout_below = 0;
    p.morale.rally_above = 1;
    p.unit_types[kVillager].combat.hp = 400;
    // De frente: A ataca a B y solo B le golpea.
    World front(p);
    const auto a1 = front.spawn_unit(0, kSoldier, {30, 30});
    const auto b1 = front.spawn_unit(1, kSoldier, {31, 30});
    Command atk = order(CommandType::Attack, 0, {a1});
    atk.object = b1;
    front.issue(atk);
    // Por la espalda: A ataca a B, un aldeano que no se defiende, y quien le golpea es C,
    // a su espalda.
    World rear(p);
    const auto a2 = rear.spawn_unit(0, kSoldier, {30, 30});
    const auto b2 = rear.spawn_unit(1, kVillager, {31, 30});
    const auto c2 = rear.spawn_unit(1, kSoldier, {29, 30});
    Command atk2 = order(CommandType::Attack, 0, {a2});
    atk2.object = b2;
    rear.issue(atk2);
    Command hit = order(CommandType::Attack, 1, {c2});
    hit.object = a2;
    rear.issue(hit);
    for (int t = 0; t < 200; ++t) {
        front.step();
        rear.step();
    }
    const auto& h1 = front.registry().get<rts::sim::Health>(ent(a1));
    const auto& h2 = rear.registry().get<rts::sim::Health>(ent(a2));
    const std::int32_t lost1 = rts::sim::kFullMorale - morale(front, a1);
    const std::int32_t lost2 = rts::sim::kFullMorale - morale(rear, a2);
    REQUIRE(h1.hp < h1.max_hp);
    REQUIRE(h2.hp < h2.max_hp);
    // Por golpe recibido (mismo daño por golpe), la espalda pesa más.
    const std::int32_t hits1 = (h1.max_hp - h1.hp);
    const std::int32_t hits2 = (h2.max_hp - h2.hp);
    CHECK(std::int64_t{lost2} * hits1 > std::int64_t{lost1} * hits2 * 3);
}

TEST_CASE("Moral: una baja propia cercana desmoraliza a quien la ve") {
    WorldParams p = morale_params();
    p.morale.calm_gain = 0;
    p.morale.comrade_gain = 0;
    p.morale.hero_gain = 0;
    p.unit_types[kVillager].morale_resolve = 0;
    World w(p);
    const auto victim = w.spawn_unit(0, kVillager, {30, 30});
    const auto witness = w.spawn_unit(0, kSoldier, {33, 30});
    const auto far = w.spawn_unit(0, kSoldier, {50, 30});
    const auto killer = w.spawn_unit(1, kSoldier, {31, 30});
    hold_ground(w, 0, {witness, far});
    hold_ground(w, 1, {killer});
    Command atk = order(CommandType::Attack, 1, {killer});
    atk.object = victim;
    w.issue(atk);
    for (int t = 0; t < 600 && w.registry().valid(ent(victim)); ++t) {
        w.step();
    }
    REQUIRE_FALSE(w.registry().valid(ent(victim)));
    w.step();
    CHECK(morale(w, witness) == rts::sim::kFullMorale - p.morale.casualty_loss);
    CHECK(morale(w, far) == rts::sim::kFullMorale);
}

TEST_CASE("Moral: desactivada, ninguna unidad la tiene ni se desbanda") {
    WorldParams p = morale_params();
    p.morale.enabled = false;
    Surrounded s(p);
    CHECK_FALSE(s.run_until_rout(400).has_value());
    CHECK(s.world.registry().view<const Morale>().empty());
}
