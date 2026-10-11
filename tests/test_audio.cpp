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
    // Fondo: solo recetas en bucle, y sostener u ondular solo en ellas.
    const auto not_loop = replaced("wind = \"viento\"", "wind = \"espada\"");
    REQUIRE_FALSE(not_loop.has_value());
    CHECK(not_loop.error().find("loop_ms") != std::string::npos);
    const auto sustain_once = replaced("name = \"espada\"", "name = \"espada\"\nloop_ms = 0");
    REQUIRE(sustain_once.has_value());  // loop_ms = 0 es un sonido normal
    const auto bad_sustain = replaced("{ wave = \"seno\", freq_start_hz = 2400", "{ wave = \"seno\", sustain = true, freq_start_hz = 2400");
    REQUIRE_FALSE(bad_sustain.has_value());
    CHECK(bad_sustain.error().find("bucle") != std::string::npos);
}

namespace {

double rms(const std::vector<float>& s, std::size_t from = 0, std::size_t to = 0) {
    to = to == 0 ? s.size() : to;
    double e = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        e += static_cast<double>(s[i]) * static_cast<double>(s[i]);
    }
    return to > from ? std::sqrt(e / static_cast<double>(to - from)) : 0.0;
}

// Mayor salto entre dos muestras seguidas dentro del bucle (sin contar la costura).
float max_step(const std::vector<float>& s) {
    float m = 0.0f;
    for (std::size_t i = 1; i < s.size(); ++i) {
        m = std::max(m, std::abs(s[i] - s[i - 1]));
    }
    return m;
}

}  // namespace

TEST_CASE("Fondo: un bucle dura lo que dice y se cierra sin salto") {
    // Un seno sostenido de 437,5 Hz no cabe un número entero de veces en 1 s: sin el
    // fundido, la costura daría un chasquido.
    SoundRecipe r;
    r.name = "zumbido";
    r.loop_ms = 1000;
    Layer l;
    l.wave = Wave::Sine;
    l.freq_start_hz = 437;
    l.freq_end_hz = 437;
    l.sustain = true;
    l.volume_percent = 50;
    r.layers.push_back(l);
    const auto a = rts::audio::render(r, kRate, 50, 3);
    REQUIRE(a.size() == static_cast<std::size_t>(kRate));
    const float seam = std::abs(a.front() - a.back());
    CHECK(seam <= max_step(a) * 1.5f);
    CHECK(rms(a) > 0.2);  // suena todo el bucle, no solo al principio
    CHECK(rms(a, a.size() - kRate / 10) > 0.2);
    CHECK(a == rts::audio::render(r, kRate, 50, 3));
}

TEST_CASE("Fondo: los golpes que pasan del final suenan al principio; la ondulación llega al silencio") {
    SoundRecipe r;
    r.name = "gota";
    r.loop_ms = 1000;
    Layer l;
    l.wave = Wave::Sine;
    l.freq_start_hz = 900;
    l.freq_end_hz = 900;
    l.delay_ms = 950;  // empieza a 50 ms del final y dura 201 ms
    l.attack_ms = 1;
    l.decay_ms = 200;
    l.volume_percent = 60;
    r.layers.push_back(l);
    const auto a = rts::audio::render(r, kRate, 0, 1);
    REQUIRE(a.size() == static_cast<std::size_t>(kRate));
    CHECK(rms(a, 0, kRate / 20) > 0.01);           // la cola, al principio
    CHECK(rms(a, kRate / 2, kRate / 2 + kRate / 10) < 1e-6);  // en medio, nada

    SoundRecipe w;
    w.name = "rafagas";
    w.loop_ms = 2000;
    Layer n;
    n.wave = Wave::Noise;
    n.sustain = true;
    n.volume_percent = 50;
    n.tremolo_ms = 1000;
    n.tremolo_percent = 100;
    w.layers.push_back(n);
    const auto b = rts::audio::render(w, kRate, 0, 1);
    // Dos vueltas por bucle (un seno): crestas en 1/8 y 5/8, valles en 3/8 y 7/8.
    const std::size_t e = b.size() / 8;
    const std::size_t win = b.size() / 200;
    CHECK(rms(b, e - win, e + win) > 10.0 * rms(b, 3 * e - win, 3 * e + win));
    CHECK(rms(b, 5 * e - win, 5 * e + win) > 10.0 * rms(b, 7 * e - win, 7 * e + win));
}

TEST_CASE("Fondo: cada golpe con su tono si lo pide (jitter), sin tocar lo que no lo pide") {
    auto plain = tone(Wave::Sine, 1000, 30);
    plain.layers[0].repeats = 8;
    plain.layers[0].repeat_ms = 50;
    auto jittered = plain;
    jittered.layers[0].jitter_cents = 600;
    CHECK(rts::audio::render(plain, kRate, 0, 5) != rts::audio::render(jittered, kRate, 0, 5));
    // Sin jitter, los tiempos de los golpes no cambian: el mismo sonido que antes.
    CHECK(rts::audio::render(plain, kRate, 0, 5) == rts::audio::render(plain, kRate, 0, 5));
}

TEST_CASE("Fondo: los bucles de los datos duran lo suyo, se cierran sin salto y no saturan") {
    const auto& d = game_data();
    const auto& a = d.sound.ambient;
    for (const std::int32_t i : {a.wind, a.water, a.birds, a.night}) {
        REQUIRE(i >= 0);
        const auto& rec = d.sound.recipes[static_cast<std::size_t>(i)];
        CAPTURE(rec.name);
        REQUIRE(rec.loop_ms > 0);
        const auto s = rts::audio::render(rec, kRate, 0, 11);
        CHECK(s.size() == static_cast<std::size_t>(static_cast<std::int64_t>(rec.loop_ms) * kRate / 1000));
        CHECK(std::ranges::all_of(s, [](float x) { return std::isfinite(x) && x >= -1.0f && x <= 1.0f; }));
        CHECK(std::abs(s.front() - s.back()) <= max_step(s));
        // Un fondo: se oye, pero por debajo de los efectos (nunca pegado al techo).
        const double level = rms(s);
        MESSAGE(rec.name, ": nivel ", level);
        CHECK(level > 0.005);
        CHECK(level < 0.3);
        CHECK(std::ranges::count_if(s, [](float x) { return std::abs(x) > 0.99f; }) == 0);
    }
}

TEST_CASE("Mezclador: los fondos entran sin salto, suenan con los efectos y se callan") {
    Mixer mixer(4);
    mixer.set_gains(1.0f, 0.5f, 1.0f);
    std::vector<float> loop(1000, 0.0f);
    for (std::size_t i = 0; i < loop.size(); ++i) {
        loop[i] = i < 500 ? 0.4f : 0.2f;  // la derecha lee medio bucle más adelante
    }
    const auto id = mixer.add(loop);
    mixer.set_ambient(0, id, 1.0f);
    std::vector<float> out(200);  // 100 tramas
    mixer.mix(out);
    // Rampa desde el silencio hasta el volumen pedido, por los efectos (0,5).
    CHECK(out[0] < 0.01f);
    CHECK(out[198] == doctest::Approx(0.4 * 0.5).epsilon(0.02));
    CHECK(out[199] == doctest::Approx(0.2 * 0.5).epsilon(0.02));
    mixer.mix(out);
    CHECK(out[0] == doctest::Approx(0.2).epsilon(0.02));  // ya a su volumen
    mixer.set_ambient(0, id, 0.0f);
    mixer.mix(out);
    CHECK(std::abs(out[198]) < 0.01f);
    mixer.mix(out);
    CHECK(std::ranges::all_of(out, [](float x) { return x == 0.0f; }));
    // Un hueco fuera de rango o un sonido que no existe no hacen nada.
    mixer.set_ambient(99, id, 1.0f);
    mixer.set_ambient(1, 12345, 1.0f);
    mixer.mix(out);
    CHECK(std::ranges::all_of(out, [](float x) { return x == 0.0f; }));
}

TEST_CASE("Director de sonido: el fondo sigue a lo que se ve y a la luz, sin saltos") {
    const auto& d = game_data();
    rts::game::SoundDirector dir(d.sound, d);
    const auto& a = d.sound.ambient;
    const auto slot = [&](rts::audio::AmbientSlot s) { return dir.ambient()[static_cast<std::size_t>(s)]; };
    using rts::audio::AmbientSlot;
    // Agua y bosque, de día: viento, agua y pájaros; nada de grillos.
    rts::game::AmbientView day{100, 40, 50, 100};
    dir.on_view(day, 0.0);  // la primera vista se toma tal cual
    CHECK(slot(AmbientSlot::Wind) == doctest::Approx(a.wind_percent / 100.0));
    CHECK(slot(AmbientSlot::Water) == doctest::Approx(a.water_percent / 100.0));
    CHECK(slot(AmbientSlot::Birds) == doctest::Approx(a.birds_percent / 100.0));
    CHECK(slot(AmbientSlot::Night) == doctest::Approx(0.0));
    // La cámara se va a un campo sin agua ni bosque: a mitad de fade_ms, a medio camino.
    rts::game::AmbientView field{100, 0, 0, 100};
    dir.on_view(field, a.fade_ms / 2.0);
    CHECK(slot(AmbientSlot::Water) == doctest::Approx(a.water_percent / 200.0).epsilon(0.01));
    dir.on_view(field, a.fade_ms * 2.0);
    CHECK(slot(AmbientSlot::Water) == doctest::Approx(0.0));
    CHECK(slot(AmbientSlot::Birds) == doctest::Approx(0.0));
    // Noche cerrada sobre el bosque: grillos y no pájaros.
    rts::game::AmbientView night{100, 0, 50, d.engine.world.vision.night_sight_percent};
    dir.on_view(night, a.fade_ms * 10.0);
    CHECK(slot(AmbientSlot::Birds) == doctest::Approx(0.0));
    CHECK(slot(AmbientSlot::Night) == doctest::Approx(a.night_percent / 100.0));
    // Sin nada explorado a la vista, solo el viento.
    dir.on_view({0, 0, 0, 100}, a.fade_ms * 20.0);
    CHECK(slot(AmbientSlot::Water) == doctest::Approx(0.0));
    CHECK(slot(AmbientSlot::Wind) == doctest::Approx(a.wind_percent / 100.0));
    // Terrenos del fondo, por nombre en los datos.
    for (std::size_t t = 0; t < d.terrain.types.size(); ++t) {
        const auto& name = d.terrain.types[t].name;
        CHECK(dir.water_terrain(static_cast<rts::sim::TerrainId>(t)) ==
              (name == "agua" || name == "agua_profunda" || name == "vado"));
        CHECK(dir.forest_terrain(static_cast<rts::sim::TerrainId>(t)) == (name == "bosque"));
    }
}
