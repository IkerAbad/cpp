// Avisos al jugador: se generan comparando dos estados y no se repiten en la misma
// zona antes de la espera.

#include <doctest/doctest.h>

#include "game/alerts.hpp"

using rts::game::Alert;
using rts::game::AlertKind;
using rts::game::AlertParams;
using rts::game::AlertTracker;
using rts::sim::Snapshot;

namespace {

AlertParams params() {
    AlertParams p;
    p.cooldown_ticks = 100;
    p.zone_tiles = 16;
    p.show_ticks = 50;
    p.max_shown = 4;
    return p;
}

rts::sim::SnapshotEntity unit(std::uint32_t id, std::int32_t hp, rts::sim::PlayerId owner = 0) {
    rts::sim::SnapshotEntity e;
    e.id = id;
    e.hp = hp;
    e.owner = owner;
    e.pos = {rts::sim::Fixed::from_int(10), rts::sim::Fixed::from_int(10)};
    return e;
}

bool has(const std::vector<Alert>& v, AlertKind k) {
    return std::ranges::any_of(v, [k](const Alert& a) { return a.kind == k; });
}

}  // namespace

TEST_CASE("Avisos: te atacan, una vez por zona hasta que pasa la espera") {
    AlertTracker t(params());
    Snapshot a;
    a.tick = 1;
    a.entities = {unit(1, 40), unit(2, 40, 1)};
    Snapshot b = a;
    b.tick = 2;
    b.entities[0].hp = 30;
    b.entities[1].hp = 10;  // el enemigo herido no es aviso
    const auto out = t.update(a, b, 0);
    REQUIRE(out.size() == 1);
    CHECK(out[0].kind == AlertKind::UnderAttack);
    Snapshot c = b;
    c.tick = 3;
    c.entities[0].hp = 20;
    CHECK(t.update(b, c, 0).empty());  // misma zona, dentro de la espera
    Snapshot d = c;
    d.tick = 200;
    d.entities[0].hp = 10;
    CHECK(has(t.update(c, d, 0), AlertKind::UnderAttack));
}

TEST_CASE("Avisos: edificio en llamas, hambre, sin comida y unidad lista; nada en el primer estado") {
    AlertTracker t(params());
    Snapshot empty;
    Snapshot a;
    a.tick = 1;
    rts::sim::SnapshotObject house;
    house.id = 9;
    house.kind = rts::sim::ObjectKind::Building;
    house.owner = 0;
    house.hp = 500;
    house.complete = true;
    a.objects = {house};
    a.entities = {unit(1, 40)};
    a.players.resize(1);
    a.players[0].stock[0] = 5;
    CHECK(t.update(empty, a, 0).empty());
    Snapshot b = a;
    b.tick = 2;
    b.objects[0].fire = 100;
    b.entities[0].hungry = true;
    b.entities.push_back(unit(3, 40));
    b.players[0].stock[0] = 0;
    const auto out = t.update(a, b, 0);
    CHECK(has(out, AlertKind::Fire));
    CHECK(has(out, AlertKind::Hunger));
    CHECK(has(out, AlertKind::NoFood));
    CHECK(has(out, AlertKind::UnitReady));
    CHECK(t.shown(2).size() == 4);
    CHECK(t.shown(100).empty());  // ya pasó su tiempo en pantalla
}

TEST_CASE("Avisos: tropas propias que empiezan a huir; las del rival no avisan") {
    AlertTracker t(params());
    Snapshot a;
    a.tick = 10;
    a.entities = {unit(1, 40), unit(2, 40, 1)};
    Snapshot b = a;
    b.tick = 11;
    b.entities[0].routing = true;
    b.entities[1].routing = true;
    const auto out = t.update(a, b, 0);
    CHECK(out.size() == 1);
    CHECK(has(out, AlertKind::Rout));
    Snapshot c = b;
    c.tick = 12;
    CHECK_FALSE(has(t.update(b, c, 0), AlertKind::Rout));  // sigue huyendo: no se repite
}
