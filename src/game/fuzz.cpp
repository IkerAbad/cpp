#include "game/fuzz.hpp"

#include <format>

#include "game/replay.hpp"

namespace rts::game {

namespace {

constexpr std::uint32_t kPercent = 100;
constexpr std::uint32_t kByteValues = 256;
constexpr std::int32_t kOutside = 1000;  // cuánto puede salirse del mapa una casilla basura

bool chance(sim::Xoshiro256pp& rng, std::int32_t percent) {
    return rng.next_below(kPercent) < static_cast<std::uint32_t>(percent);
}

}  // namespace

std::vector<sim::Command> random_orders(const sim::World& world, sim::Xoshiro256pp& rng, const FuzzParams& params) {
    sim::Snapshot s;
    world.write_snapshot(s);
    const auto players = static_cast<std::uint32_t>(std::max<std::size_t>(s.players.size(), 1));
    const std::int32_t w = world.map().width();
    const std::int32_t h = world.map().height();
    std::vector<sim::Command> out;
    for (std::int32_t n = 0; n < params.orders_per_tick; ++n) {
        sim::Command c;
        c.tick = world.tick();
        c.player = chance(rng, params.garbage_percent / 4) ? static_cast<sim::PlayerId>(rng.next_below(kByteValues))
                                                            : static_cast<sim::PlayerId>(rng.next_below(players));
        c.type = static_cast<sim::CommandType>(rng.next_below(static_cast<std::uint32_t>(sim::CommandType::Count)));
        // Unidades: casi siempre propias; a veces de cualquiera o inexistentes.
        std::vector<std::uint32_t> mine;
        for (const sim::SnapshotEntity& e : s.entities) {
            if (e.owner == c.player) {
                mine.push_back(e.id);
            }
        }
        const auto count = rng.next_in_range(0, params.max_units);
        for (std::int32_t i = 0; i < count; ++i) {
            if (!mine.empty() && !chance(rng, params.garbage_percent)) {
                c.units.push_back(mine[rng.next_below(static_cast<std::uint32_t>(mine.size()))]);
            } else if (!s.entities.empty() && chance(rng, static_cast<std::int32_t>(kPercent / 2))) {
                c.units.push_back(s.entities[rng.next_below(static_cast<std::uint32_t>(s.entities.size()))].id);
            } else {
                c.units.push_back(static_cast<std::uint32_t>(rng.next()));
            }
        }
        // Objeto: uno que existe (edificio, recurso o unidad), ninguno o basura.
        const std::uint32_t pick = rng.next_below(kPercent);
        if (pick < kPercent / 2 && !s.objects.empty()) {
            c.object = s.objects[rng.next_below(static_cast<std::uint32_t>(s.objects.size()))].id;
        } else if (pick < kPercent * 3 / 4 && !s.entities.empty()) {
            c.object = s.entities[rng.next_below(static_cast<std::uint32_t>(s.entities.size()))].id;
        } else if (chance(rng, static_cast<std::int32_t>(kPercent / 2))) {
            c.object = sim::kNoObject;
        } else {
            c.object = static_cast<std::uint32_t>(rng.next());
        }
        c.target = chance(rng, params.garbage_percent)
                       ? sim::TileCoord{rng.next_in_range(-kOutside, w + kOutside), rng.next_in_range(-kOutside, h + kOutside)}
                       : sim::TileCoord{rng.next_in_range(0, w - 1), rng.next_in_range(0, h - 1)};
        c.kind = static_cast<std::uint8_t>(rng.next_below(kByteValues));
        // A menudo un tipo que existe, para llegar más hondo (colocar, entrenar, formar...).
        if (!chance(rng, params.garbage_percent)) {
            constexpr std::uint8_t kSmallKinds = 16;
            c.kind = static_cast<std::uint8_t>(rng.next_below(kSmallKinds));
        }
        out.push_back(std::move(c));
    }
    return out;
}

std::optional<std::string> check_invariants(const sim::World& world) {
    sim::Snapshot s;
    world.write_snapshot(s);
    const std::int32_t w = world.map().width();
    const std::int32_t h = world.map().height();
    for (const sim::SnapshotEntity& e : s.entities) {
        const sim::TileCoord t = sim::tile_of(e.pos);
        if (t.x < 0 || t.y < 0 || t.x >= w || t.y >= h) {
            return std::format("unidad {} fuera del mapa en ({}, {})", e.id, t.x, t.y);
        }
        if (e.hp < 0 || e.hp > e.max_hp) {
            return std::format("unidad {} con vida {} de {}", e.id, e.hp, e.max_hp);
        }
        if (e.owner >= s.players.size()) {
            return std::format("unidad {} de un jugador que no existe ({})", e.id, e.owner);
        }
        if (e.carried < 0 || e.rations < 0 || e.ammo < 0) {
            return std::format("unidad {} con carga, víveres o munición en negativo", e.id);
        }
    }
    for (const sim::SnapshotObject& o : s.objects) {
        if (o.origin.x < 0 || o.origin.y < 0 || o.origin.x + o.size > w || o.origin.y + o.size > h) {
            return std::format("objeto {} fuera del mapa", o.id);
        }
        if (o.kind == sim::ObjectKind::Building && o.hp < 0) {
            return std::format("edificio {} con vida {}", o.id, o.hp);
        }
        if (o.kind == sim::ObjectKind::Resource && o.amount < 0) {
            return std::format("recurso {} con cantidad {}", o.id, o.amount);
        }
        for (const std::int32_t v : o.store) {
            if (v < 0) {
                return std::format("almacén del campamento {} en negativo", o.id);
            }
        }
    }
    for (std::size_t p = 0; p < s.players.size(); ++p) {
        for (const std::int32_t v : s.players[p].stock) {
            if (v < 0) {
                return std::format("almacén del jugador {} en negativo ({})", p, v);
            }
        }
        if (s.players[p].population < 0) {
            return std::format("población del jugador {} en negativo", p);
        }
    }
    return std::nullopt;
}

FuzzResult run_fuzz(const GameData& data, std::uint64_t seed, std::int32_t ticks, const FuzzParams& params,
                    std::int32_t check_every) {
    FuzzResult r;
    sim::World a(data.engine.world);
    sim::World b(data.engine.world);
    ReplayRecorder rec(data.files, data.engine.replay.checkpoint_interval_ticks);
    sim::Xoshiro256pp rng(seed);
    for (std::int32_t t = 0; t < ticks; ++t) {
        for (sim::Command& c : random_orders(a, rng, params)) {
            b.issue(c);
            rec.issue(a, std::move(c));
            ++r.orders;
        }
        a.step();
        b.step();
        rec.after_step(a);
        if (check_every > 0 && (t + 1) % check_every == 0) {
            if (auto bad = check_invariants(a)) {
                r.failure = std::format("tick {}: {}", a.tick(), *bad);
                return r;
            }
            if (a.state_hash() != b.state_hash()) {
                r.failure = std::format("tick {}: dos mundos con las mismas órdenes divergen", a.tick());
                return r;
            }
        }
    }
    r.hash = a.state_hash();
    const Replay replay = rec.finish(a);
    const auto replayed = parse_game_data(replay.data);
    if (!replayed) {
        r.failure = std::format("los datos de la repetición no se leen: {}", replayed.error());
        return r;
    }
    if (const VerifyResult v = verify_replay(replay, replayed->engine.world); !v.ok) {
        r.failure = std::format("la repetición no se reproduce igual (tick {}, {} comprobaciones)",
                                v.diverged_at.value_or(v.ticks), v.checkpoints);
    }
    return r;
}

}  // namespace rts::game
