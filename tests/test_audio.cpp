// Sonido propio (F2): síntesis, música, mezclador, director de sonido y datos.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "audio/bank.hpp"
#include "audio/mixer.hpp"
#include "audio/synth.hpp"
#include "game/config.hpp"
#include "game/sound_director.hpp"

using rts::audio::Layer;
using rts::audio::Mixer;
using rts::audio::SoundRecipe;
using rts::audio::Wave;

namespace {

constexpr std::int32_t kRate = 48000;

const rts::game::GameData& game_data() {
    static const rts::game::GameData data = [] {
        auto d = rts::game::load_game_data(RTS_DATA_DIR);
        REQUIRE_MESSAGE(d.has_value(), (d ? std::string() : d.error()));
        return std::move(*d);
    }();
    return data;
}

SoundRecipe tone(Wave wave, std::int32_t hz, std::int32_t decay_ms) {
    SoundRecipe r;
    r.name = "prueba";
    Layer l;
    l.wave = wave;
    l.freq_start_hz = hz;
    l.freq_end_hz = hz;
    l.attack_ms = 1;
    l.decay_ms = decay_ms;
    l.volume_percent = 50;
    r.layers.push_back(l);
    return r;
}

// Cruces por cero hacia arriba: dos veces la frecuencia por segundo de un tono puro... una.
std::int32_t rising_crossings(const std::vector<float>& s) {
    std::int32_t n = 0;
    for (std::size_t i = 1; i < s.size(); ++i) {
        n += s[i - 1] < 0.0f && s[i] >= 0.0f ? 1 : 0;
    }
    return n;
}

}  // namespace

TEST_CASE("Síntesis: el tono que se pide, sin pasar de ±1, y lo mismo cada vez") {
    const auto r = tone(Wave::Sine, 440, 1000);
    const auto a = rts::audio::render(r, kRate, 0, 7);
    const auto b = rts::audio::render(r, kRate, 0, 7);
    CHECK(a == b);
    CHECK(a.size() >= static_cast<std::size_t>(kRate));  // 1 ms de subida + 1 s de caída
    CHECK(std::ranges::all_of(a, [](float x) { return x >= -1.0f && x <= 1.0f; }));
    // Un segundo de 440 Hz: unos 440 cruces por cero hacia arriba.
    const std::vector<float> first_second(a.begin(), a.begin() + kRate);
    CHECK(std::abs(rising_crossings(first_second) - 440) <= 2);
    // Una octava arriba (1200 cents), el doble.
    const auto up = rts::audio::render(r, kRate, 1200, 7);
    const std::vector<float> up_second(up.begin(), up.begin() + kRate);
    CHECK(std::abs(rising_crossings(up_second) - 880) <= 3);
    // La envolvente cae: el final es casi silencio.
    CHECK(std::abs(a[a.size() - 10]) < 0.01f);
}

TEST_CASE("Síntesis: el ruido cambia con la semilla; el filtro paso bajo lo apaga") {
    const auto r = tone(Wave::Noise, 0, 200);
    CHECK(rts::audio::render(r, kRate, 0, 1) != rts::audio::render(r, kRate, 0, 2));
    auto low = r;
    low.layers[0].lowpass_hz = 200;
    const auto raw = rts::audio::render(r, kRate, 0, 1);
    const auto filtered = rts::audio::render(low, kRate, 0, 1);
    const auto energy = [](const std::vector<float>& s) {
        double e = 0.0;
        for (const float x : s) {
            e += static_cast<double>(x) * static_cast<double>(x);
        }
        return e;
    };
    CHECK(energy(filtered) < energy(raw) * 0.5);
}

TEST_CASE("Música: dura lo que dicen sus compases, es determinista y no satura") {
    const auto& d = game_data();
    REQUIRE(d.sound.music.size() >= 2);
    for (const auto& m : d.sound.music) {
        CAPTURE(m.name);
        const auto a = rts::audio::compose(m, kRate);
        const double seconds = 60.0 / m.tempo_bpm * m.beats_per_bar * m.bars;
        CHECK(std::abs(static_cast<double>(a.size()) / kRate - seconds) < 0.01);
        CHECK(std::ranges::all_of(a, [](float x) { return std::isfinite(x) && x >= -1.0f && x <= 1.0f; }));
        // Suena (no es silencio) y no vive pegada al techo.
        const auto loud = std::ranges::count_if(a, [](float x) { return std::abs(x) > 0.99f; });
        CHECK(loud < static_cast<std::ptrdiff_t>(a.size() / 100));
        CHECK(std::ranges::any_of(a, [](float x) { return std::abs(x) > 0.1f; }));
    }
    CHECK(rts::audio::compose(d.sound.music[0], kRate) == rts::audio::compose(d.sound.music[0], kRate));
    // Dos piezas distintas no se parecen.
    CHECK(rts::audio::compose(d.sound.music[0], kRate) != rts::audio::compose(d.sound.music[1], kRate));
}

TEST_CASE("Mezclador: panorama, robo de la voz más débil y música que se repite y funde") {
    Mixer mixer(2);
    const auto one = mixer.add(std::vector<float>(100, 0.5f));
    mixer.play(one, 1.0f, -1.0f);  // todo a la izquierda
    std::vector<float> out(20);
    mixer.mix(out);
    CHECK(out[0] > 0.4f);
    CHECK(std::abs(out[1]) < 1e-6f);
    // Con dos voces llenas, una tercera más fuerte sustituye a la más débil; una más
    // débil se descarta.
    Mixer m2(2);
    const auto s = m2.add(std::vector<float>(1000, 0.1f));
    m2.play(s, 0.2f, 0.0f);
    m2.play(s, 0.3f, 0.0f);
    m2.play(s, 0.1f, 0.0f);  // más débil que todas: fuera
    CHECK(m2.active_voices() == 2);
    std::vector<float> before(2);
    m2.mix(before);
    m2.play(s, 0.9f, 0.0f);  // sustituye a la de 0.2
    std::vector<float> after(2);
    m2.mix(after);
    CHECK(after[0] > before[0]);
    // Una voz se acaba sola.
    std::vector<float> rest(4000);
    m2.mix(rest);
    CHECK(m2.active_voices() == 0);
    // Música: se repite al acabar y el cambio de pieza funde sin saltos de volumen.
    Mixer m3(1);
    const auto a = m3.add(std::vector<float>(10, 0.5f));
    const auto b = m3.add(std::vector<float>(10, -0.5f));
    m3.set_music(a, 1);
    std::vector<float> loop(60);
    m3.mix(loop);
    CHECK(loop[58] > 0.3f);  // sigue sonando tras varias vueltas
    m3.set_music(b, 20);
    std::vector<float> fade(60);
    m3.mix(fade);
    CHECK(fade[0] > 0.0f);           // al empezar, casi toda la vieja
    CHECK(std::abs(fade[20]) < 0.1f);  // a mitad, se compensan
    CHECK(fade[58] < -0.3f);         // al final, la nueva
}

TEST_CASE("Banco: cada receta con sus versiones, que van rotando") {
    const auto& d = game_data();
    Mixer mixer(8);
    rts::audio::Bank bank(d.sound, mixer, false);
    CHECK(bank.piece(0) == -1);  // la música aún no está
    bank.add_music(mixer, {std::vector<float>(10, 0.1f), std::vector<float>(10, 0.2f)});
    CHECK(bank.piece(0) >= 0);
    CHECK(bank.piece(1) >= 0);
    bank.play(mixer, {d.sound.events.melee_hit, 1.0f, 0.0f});
    bank.play(mixer, {d.sound.events.melee_hit, 1.0f, 0.0f});
    CHECK(mixer.active_voices() == 2);
}

TEST_CASE("Director de sonido: lo que se ve suena, con panorama; lo lejano no; esperas; música de batalla") {
    const auto& d = game_data();
    rts::game::SoundDirector dir(d.sound, d);
    const rts::render::Vec2 screen{1000.0f, 600.0f};
    // Posición de mapa = posición de pantalla en x (para la prueba).
    const rts::game::SoundDirector::Locate locate = [](rts::sim::Position p) -> std::optional<rts::render::Vec2> {
        if (p.y.raw() < 0) {
            return std::nullopt;  // bajo la niebla
        }
        return rts::render::Vec2{static_cast<float>(p.x.raw()) / static_cast<float>(rts::sim::Fixed::from_int(1).raw()),
                                 300.0f};
    };
    const auto pos = [](std::int32_t x, std::int32_t y) {
        return rts::sim::Position{rts::sim::Fixed::from_int(x), rts::sim::Fixed::from_int(y)};
    };
    rts::sim::Snapshot prev;
    rts::sim::Snapshot curr;
    // Un aldeano (martillo: golpe sordo) a la derecha; un golpe fuera de la pantalla; uno bajo la niebla.
    curr.hits.push_back({pos(900, 0), 0, false});
    curr.hits.push_back({pos(5000, 0), 0, false});
    curr.hits.push_back({pos(500, -1), 0, false});
    double now = 100000.0;
    CHECK(dir.music(now) == d.sound.peace_music);
    dir.on_tick(prev, curr, 0, locate, screen, now);
    auto cues = dir.take();
    REQUIRE(cues.size() == 1);
    CHECK(cues[0].recipe == d.sound.events.blunt_hit);
    CHECK(cues[0].pan > 0.5f);
    CHECK(static_cast<double>(cues[0].volume) == doctest::Approx(1.0));
    CHECK(dir.music(now) == d.sound.battle_music);  // combate a la vista
    // Dentro de la espera, el mismo golpe no se repite.
    dir.on_tick(prev, curr, 0, locate, screen, now + 1.0);
    CHECK(dir.take().empty());
    // Pasada la espera de la batalla, vuelve la música de paz.
    CHECK(dir.music(now + d.sound.config.battle_hold_ms + 1.0) == d.sound.peace_music);
    // Un poco fuera de la pantalla aún se oye, más bajo.
    rts::sim::Snapshot near;
    near.hits.push_back({pos(1000 + d.sound.config.hearing_margin_px / 2, 0), 0, false});
    now += 10000.0;
    dir.on_tick(prev, near, 0, locate, screen, now);
    cues = dir.take();
    REQUIRE(cues.size() == 1);
    CHECK(static_cast<double>(cues[0].volume) == doctest::Approx(0.5).epsilon(0.05));

    // Bajas, derrumbes y obras terminadas.
    rts::sim::Snapshot a;
    rts::sim::Snapshot b;
    rts::sim::SnapshotEntity e;
    e.id = 7;
    e.pos = pos(300, 0);
    a.entities.push_back(e);  // en b ya no está: baja
    rts::sim::SnapshotObject wall;
    wall.id = 1;
    wall.kind = rts::sim::ObjectKind::Building;
    wall.origin = {300, 0};
    a.objects.push_back(wall);  // en b ya no está: derrumbe
    rts::sim::SnapshotObject house;
    house.id = 2;
    house.kind = rts::sim::ObjectKind::Building;
    house.complete = false;
    house.owner = 0;
    a.objects.push_back(house);
    house.complete = true;
    b.objects.push_back(house);
    now += 10000.0;
    dir.on_tick(a, b, 0, locate, screen, now);
    cues = dir.take();
    const auto has = [&](std::int32_t recipe) {
        return std::ranges::any_of(cues, [&](const rts::audio::SoundCue& c) { return c.recipe == recipe; });
    };
    CHECK(has(d.sound.events.death));
    CHECK(has(d.sound.events.collapse));
    CHECK(has(d.sound.events.built));

    // Avisos, órdenes, trabajo y fuego.
    now += 10000.0;
    dir.on_alert(rts::game::AlertKind::UnderAttack, now);
    dir.on_alert(rts::game::AlertKind::UnitReady, now);
    dir.on_order(now);
    dir.on_work(rts::game::WorkSound::Chop, {500.0f, 300.0f}, screen, now);
    dir.on_fires({{100.0f, 100.0f}, {500.0f, 300.0f}}, screen, now);
    cues = dir.take();
    CHECK(has(d.sound.events.alarm));
    CHECK(has(d.sound.events.ready));
    CHECK(has(d.sound.events.order));
    CHECK(has(d.sound.events.chop));
    CHECK(has(d.sound.events.fire));
}

TEST_CASE("Datos de sonido: errores claros en sound.toml") {
    const auto& d = game_data();
    const auto file = std::ranges::find(d.files, std::string("sound.toml"), &rts::game::DataFile::path);
    REQUIRE(file != d.files.end());
    REQUIRE(rts::game::parse_sound_spec(file->text).has_value());
    const auto replaced = [&](std::string_view from, std::string_view to) {
        std::string t = file->text;
        const auto at = t.find(from);
        REQUIRE(at != std::string::npos);
        t.replace(at, from.size(), to);
        return rts::game::parse_sound_spec(t);
    };
    const auto bad_wave = replaced("wave = \"seno\"", "wave = \"flauta\"");
    REQUIRE_FALSE(bad_wave.has_value());
    CHECK(bad_wave.error().find("flauta") != std::string::npos);
    const auto bad_event = replaced("melee_hit = \"espada\"", "melee_hit = \"laud\"");
    REQUIRE_FALSE(bad_event.has_value());
    CHECK(bad_event.error().find("laud") != std::string::npos);
    const auto bad_mode = replaced("mode = [0, 2, 3, 5, 7, 9, 10]", "mode = [2, 3, 5]");
    REQUIRE_FALSE(bad_mode.has_value());
    CHECK(bad_mode.error().find("mode") != std::string::npos);
}
