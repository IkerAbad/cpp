#include "game/app.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include "game/camera_control.hpp"
#include "game/i18n.hpp"
#include "game/options.hpp"
#include "game/campaign.hpp"
#include "game/crash_report.hpp"
#include "game/alerts.hpp"
#include "game/fixed_step.hpp"
#include "game/lockstep.hpp"
#include "game/scenario.hpp"
#include "game/sound_director.hpp"
#include "game/minimap.hpp"
#include "game/replay.hpp"
#include "game/selection.hpp"
#include "platform/audio.hpp"
#include "platform/profile.hpp"
#include "platform/window.hpp"
#include "render/projection.hpp"
#include "render/renderer.hpp"
#include "sim/world.hpp"

namespace rts::game {

namespace {

constexpr std::int64_t kNsPerSecond = 1'000'000'000;
constexpr std::int64_t kTickNs = kNsPerSecond / sim::kTicksPerSecond;
static_assert(kTickNs * sim::kTicksPerSecond == kNsPerSecond, "el tick debe durar un número entero de ns");

constexpr double kNsPerMs = 1'000'000.0;
constexpr float kNsPerSecondF = 1'000'000'000.0f;
// Peso de la media móvil exponencial de los costes por fotograma: ~20 muestras efectivas.
constexpr double kSmoothing = 0.05;
// Un fotograma de más de 100 ms (depurador, arrastre de ventana) no debe teletransportar la cámara.
constexpr float kMaxCameraDtS = 0.1f;
// Separación de los paneles de depuración respecto al borde de la ventana.
constexpr float kPanelMarginPx = 10.0f;
constexpr ImVec4 kWarnColor{1.0f, 0.45f, 0.35f, 1.0f};  // avisos: hambre, sin munición
constexpr std::uint8_t kOpaque = 255;
// Jugador humano de esta máquina. En LAN (M8) lo asignará la sala de espera.
constexpr sim::PlayerId kLocalPlayer = 0;
constexpr std::int32_t kPercent = 100;
constexpr std::int32_t kPermille = 1000;
// Editor de escenarios (F3): pincel máximo y carpeta de los escenarios (junto al ejecutable).
constexpr int kMaxBrush = 8;
constexpr std::string_view kScenarioDir = "escenarios";

// Teclas en uso (F5), como códigos de SDL, en el orden de KeyAction. Hasta que se leen
// las opciones del jugador, las de siempre (las mismas que data/opciones.toml).
std::array<std::int32_t, kKeyActionCount>& key_codes() {
    static std::array<std::int32_t, kKeyActionCount> codes{
        SDL_SCANCODE_LEFT,  SDL_SCANCODE_RIGHT, SDL_SCANCODE_UP,  SDL_SCANCODE_DOWN, SDL_SCANCODE_A,
        SDL_SCANCODE_D,     SDL_SCANCODE_W,     SDL_SCANCODE_S,   SDL_SCANCODE_SPACE, SDL_SCANCODE_F5,
        SDL_SCANCODE_F10,   SDL_SCANCODE_F1,    SDL_SCANCODE_F2,
    };
    return codes;
}

bool is_key(SDL_Scancode code, KeyAction a) {
    return static_cast<std::int32_t>(code) == key_codes()[static_cast<std::size_t>(a)];
}

// Nombre a la vista de la tecla de una acción ("F10").
std::string key_label(KeyAction a) {
    return platform::scancode_name(key_codes()[static_cast<std::size_t>(a)]);
}

render::Rgba opaque(const std::array<std::uint8_t, 3>& rgb) noexcept {
    return {rgb[0], rgb[1], rgb[2], kOpaque};
}

render::Rgba shaded(render::Rgba c, std::int32_t percent) noexcept {
    for (std::size_t i = 0; i < 3; ++i) {
        c[i] = static_cast<std::uint8_t>(c[i] * percent / kPercent);
    }
    return c;
}

// "madera 30, piedra 100"
std::string cost_text(const sim::Stock& cost) {
    std::string out;
    for (std::size_t r = 0; r < sim::kResourceCount; ++r) {
        if (cost[r] > 0) {
            out += std::format("{}{} {}", out.empty() ? "" : ", ", T(resource_key(static_cast<sim::Resource>(r)).data()), cost[r]);
        }
    }
    return out.empty() ? T("gratis") : out;
}

bool affordable(const sim::Stock& stock, const sim::Stock& cost) noexcept {
    for (std::size_t r = 0; r < sim::kResourceCount; ++r) {
        if (stock[r] < cost[r]) {
            return false;
        }
    }
    return true;
}

const char* task_name(sim::WorkerTask t) noexcept {
    switch (t) {
        case sim::WorkerTask::Idle:
            return T("ocioso");
        case sim::WorkerTask::Gather:
            return T("recogiendo");
        case sim::WorkerTask::Deliver:
            return T("llevando al almacén");
        case sim::WorkerTask::Build:
            return T("construyendo");
        case sim::WorkerTask::Demolish:
            return T("desmontando");
        case sim::WorkerTask::Nurse:
            return T("de enfermero");
    }
    return "?";
}

using SteadyClock = std::chrono::steady_clock;

double elapsed_ms(SteadyClock::time_point since) {
    return std::chrono::duration<double, std::milli>(SteadyClock::now() - since).count();
}

float fixed_to_float(sim::Fixed v) noexcept {
    return static_cast<float>(v.raw()) / static_cast<float>(sim::Fixed::kOneRaw);
}

void print_usage() {
    spdlog::info("Uso: rts [--data <carpeta>] [--frames <N>] | [--headless --ticks <N> [--record <fichero>]]");
    spdlog::info("     rts --replay <fichero> [--frames <N>] | --verify-replay <fichero>");
    spdlog::info("     rts --load <partida.rtssav> [--frames <N>]  (F5 guarda durante la partida)");
    spdlog::info("     rts --headless --ticks <N> --host <puerto> [--players <N>] [--record <fichero>]");
    spdlog::info("     rts --headless --join <dirección>:<puerto> [--record <fichero>]");
    spdlog::info("     [--report-dir <carpeta>]  informes de errores (por omisión, informes/ junto al ejecutable)");
}


bool parse_count(std::string_view value, std::int64_t& out) {
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), out);
    return ec == std::errc{} && end == value.data() + value.size() && out >= 0;
}

// "anfitrión:puerto" (el puerto tras el último ':').
bool parse_address(std::string_view value, std::string& host, std::uint16_t& port) {
    const auto colon = value.rfind(':');
    if (colon == std::string_view::npos || colon == 0) {
        return false;
    }
    std::int64_t p = 0;
    if (!parse_count(value.substr(colon + 1), p) || p == 0 || p > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }
    host = value.substr(0, colon);
    port = static_cast<std::uint16_t>(p);
    return true;
}

// Unidades de un jugador en orden de creación (el del snapshot).
std::vector<std::uint32_t> player_units(const sim::Snapshot& snap, sim::PlayerId player) {
    std::vector<std::uint32_t> ids;
    for (const sim::SnapshotEntity& e : snap.entities) {
        if (e.owner == player) {
            ids.push_back(e.id);
        }
    }
    return ids;
}

const sim::SnapshotObject* first_building(const sim::Snapshot& snap, sim::PlayerId player,
                                          std::optional<sim::BuildingTypeId> type) {
    for (const sim::SnapshotObject& o : snap.objects) {
        if (o.kind == sim::ObjectKind::Building && o.owner == player && (!type || o.type == *type)) {
            return &o;
        }
    }
    return nullptr;
}

sim::TileCoord clamp_to(const sim::SnapshotObject& o, sim::TileCoord c) {
    return {std::clamp(c.x, o.origin.x, o.origin.x + o.size - 1), std::clamp(c.y, o.origin.y, o.origin.y + o.size - 1)};
}

// Resolución del guion en enteros (sin coma flotante): el hash de la CI depende de
// qué nodo y qué sitio se eligen, y debe coincidir entre compiladores.
std::optional<std::uint32_t> nearest_node(const GameData& data, const sim::Snapshot& snap,
                                          std::span<const std::uint32_t> units, sim::Resource kind) {
    std::int64_t sx = 0;
    std::int64_t sy = 0;
    std::int64_t n = 0;
    for (const sim::SnapshotEntity& e : snap.entities) {
        if (std::ranges::find(units, e.id) != units.end()) {
            sx += e.pos.x.floor_to_int();
            sy += e.pos.y.floor_to_int();
            ++n;
        }
    }
    if (n == 0) {
        return std::nullopt;
    }
    const sim::TileCoord from{static_cast<std::int32_t>(sx / n), static_cast<std::int32_t>(sy / n)};
    std::optional<std::uint32_t> best;
    std::int32_t best_d = std::numeric_limits<std::int32_t>::max();
    for (const sim::SnapshotObject& o : snap.objects) {
        if (o.kind != sim::ObjectKind::Resource || data.nodes.types[o.type].type.kind != kind) {
            continue;
        }
        const std::int32_t d = sim::octile_distance(from, clamp_to(o, from));
        if (d < best_d) {
            best_d = d;
            best = o.id;
        }
    }
    return best;
}

std::optional<sim::TileCoord> build_site(const GameData& data, const sim::World& world, const sim::Snapshot& snap,
                                         sim::PlayerId player, sim::BuildingTypeId type) {
    const sim::SnapshotObject* home = first_building(snap, player, std::nullopt);
    if (home == nullptr) {
        return std::nullopt;
    }
    const std::int32_t size = data.buildings.types[type].type.size;
    const Scenario& sc = data.headless_scenario;
    // Anillos de orígenes alrededor de la huella de referencia, con gap casillas libres.
    for (std::int32_t r = sc.build_gap_tiles + size; r <= sc.build_search_radius_tiles; ++r) {
        const std::int32_t x0 = home->origin.x - r;
        const std::int32_t y0 = home->origin.y - r;
        const std::int32_t x1 = home->origin.x + home->size - 1 + r;
        const std::int32_t y1 = home->origin.y + home->size - 1 + r;
        for (std::int32_t y = y0; y <= y1; ++y) {
            for (std::int32_t x = x0; x <= x1; ++x) {
                const bool on_ring = x == x0 || x == x1 || y == y0 || y == y1;
                if (on_ring && world.can_place(type, {x, y})) {
                    return sim::TileCoord{x, y};
                }
            }
        }
    }
    return std::nullopt;
}

// Convierte las órdenes del guion en órdenes de la simulación, resueltas contra el
// estado inicial del mundo, y las entrega a issue.
template <typename Issue>
void issue_scenario(const GameData& data, sim::World& world, Issue&& issue) {
    sim::Snapshot snap;
    world.write_snapshot(snap);
    for (const ScenarioOrder& o : data.headless_scenario.orders) {
        sim::Command c;
        c.tick = o.tick;
        c.player = o.player;
        const auto ids = player_units(snap, o.player);
        const auto first = std::min(ids.size(), static_cast<std::size_t>(o.first));
        const auto last = std::min(ids.size(), static_cast<std::size_t>(o.first) + static_cast<std::size_t>(o.count));
        c.units.assign(ids.begin() + static_cast<std::ptrdiff_t>(first), ids.begin() + static_cast<std::ptrdiff_t>(last));
        switch (o.action) {
            case ScenarioAction::Move:
                c.type = sim::CommandType::Move;
                c.target = o.target;
                break;
            case ScenarioAction::Gather: {
                const auto node = nearest_node(data, snap, c.units, o.resource);
                if (!node) {
                    spdlog::warn("guion: no hay nodos de {} para el jugador {}", resource_key(o.resource), o.player);
                    continue;
                }
                c.type = sim::CommandType::Gather;
                c.object = *node;
                break;
            }
            case ScenarioAction::Build: {
                const auto site = build_site(data, world, snap, o.player, o.building);
                if (!site) {
                    spdlog::warn("guion: sin sitio para {} del jugador {}", data.buildings.types[o.building].name,
                                 o.player);
                    continue;
                }
                c.type = sim::CommandType::Place;
                c.kind = o.building;
                c.target = *site;
                break;
            }
            case ScenarioAction::Train: {
                const sim::SnapshotObject* b = first_building(snap, o.player, o.building);
                if (b == nullptr) {
                    spdlog::warn("guion: el jugador {} no tiene {}", o.player, data.buildings.types[o.building].name);
                    continue;
                }
                c.type = sim::CommandType::Train;
                c.object = b->id;
                c.kind = o.unit;
                c.units.clear();
                for (std::int32_t i = 0; i < o.count; ++i) {
                    issue(c);
                }
                continue;
            }
        }
        issue(std::move(c));
    }
}

// replays/partida-2026-09-25_18-30-05.rtsrep, con la hora local de inicio (UTC si el
// sistema no tiene zona horaria).
std::string time_stamp() {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    try {
        return std::format("{:%Y-%m-%d_%H-%M-%S}", std::chrono::zoned_time{std::chrono::current_zone(), now});
    } catch (const std::exception&) {
        return std::format("{:%Y-%m-%d_%H-%M-%S}Z", now);
    }
}

std::filesystem::path auto_replay_path(const GameData& data) {
    return std::filesystem::path(platform::executable_dir()) / data.engine.replay.directory /
           std::format("partida-{}.rtsrep", time_stamp());
}

// Partida guardada con F5: guardada-<fecha>.rtssav junto a las repeticiones.
std::filesystem::path save_game_path(const GameData& data) {
    return std::filesystem::path(platform::executable_dir()) / data.engine.replay.directory /
           std::format("guardada-{}.rtssav", time_stamp());
}

// Métricas del panel de depuración.
struct FrameStats {
    double frame_ms = 0.0;
    double sim_ms_per_tick = 0.0;  // media móvil
    double scene_ms = 0.0;         // media móvil: traducir la escena a sprites (CPU pura)
    double submit_ms = 0.0;        // media móvil: subir, grabar y presentar (incluye la espera de vsync)
    std::int32_t ticks_this_frame = 0;
    std::int64_t dropped_ticks_total = 0;
    double alpha = 0.0;
    std::uint64_t state_hash = 0;
    render::SceneStats scene;
};

// Sonido de la ventana (F2): el mezclador con todo lo sintetizado y la salida de la
// plataforma. Solo existe si hay dispositivo de audio.
struct AudioSystem {
    audio::Mixer mixer;
    audio::Bank bank;
    platform::AudioOut out;
    const audio::SoundSpec* spec;
    // La música se compone en otro hilo (unos 300 ms): los efectos ya suenan mientras.
    std::future<std::vector<std::vector<float>>> music_ready;
    std::int32_t wanted_piece = -1;

    AudioSystem(const audio::SoundSpec& s, platform::AudioOut o)
        : mixer(s.config.max_voices), bank(s, mixer, false), out(std::move(o)), spec(&s),
          music_ready(std::async(std::launch::async, [&s] { return audio::Bank::compose_music(s); })) {
        constexpr float kPercentF = 100.0f;
        mixer.set_gains(static_cast<float>(s.config.master_percent) / kPercentF,
                        static_cast<float>(s.config.effects_percent) / kPercentF,
                        static_cast<float>(s.config.music_percent) / kPercentF);
    }
    void pump() {
        if (music_ready.valid() &&
            music_ready.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            bank.add_music(mixer, music_ready.get());
            const std::int32_t piece = wanted_piece;
            wanted_piece = -2;  // forzar el cambio ahora que hay pistas
            music(piece);
        }
        out.pump(mixer);
    }
    void music(std::int32_t piece) {
        if (piece == wanted_piece) {
            return;
        }
        wanted_piece = piece;
        constexpr std::int32_t kMsPerS = 1000;
        mixer.set_music(bank.piece(piece), spec->config.sample_rate / kMsPerS * spec->config.fade_ms);
    }
};

// Registro de la charla y línea para escribir (sala y partida en red).
void draw_chat(LockstepSession& net, const GameData& data, std::string& input) {
    const float input_h = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("lineas", {0.0f, -input_h});
    for (const ChatLine& line : net.chat()) {
        const auto& c = line.player < data.engine.seat_colors.size() ? data.engine.seat_colors[line.player]
                                                                       : std::array<std::uint8_t, 3>{kOpaque, kOpaque,
                                                                                                     kOpaque};
        ImGui::TextColored({static_cast<float>(c[0]) / kOpaque, static_cast<float>(c[1]) / kOpaque,
                            static_cast<float>(c[2]) / kOpaque, 1.0f},
                           T("Jugador %u%s:"), static_cast<unsigned>(line.player),
                           line.player == net.local_player() ? T(" (tú)") : "");
        ImGui::SameLine();
        ImGui::TextWrapped("%s", line.text.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);  // pegado al final mientras no se suba a leer
    }
    ImGui::EndChild();
    input.resize(static_cast<std::size_t>(data.engine.net.lockstep.chat_max_chars) + 1, '\0');
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##charla", input.data(), input.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
        net.send_chat(input.c_str());
        std::fill(input.begin(), input.end(), '\0');
        ImGui::SetKeyboardFocusHere(-1);
    }
}

class WindowedGame {
public:
    // Con replay, reproduce esa repetición (sin órdenes); sin ella, partida nueva que
    // se graba sola.
    WindowedGame(const GameData& data, platform::Window& window, render::Renderer& renderer, const Replay* replay,
                 const Replay* resume, LockstepSession* net = nullptr, AudioSystem* audio = nullptr,
                 ScenarioDoc* editor = nullptr)
        : data_(data),
          editor_(editor),
          net_(net),
          audio_(audio),
          local_(net != nullptr ? net->local_player() : kLocalPlayer),
          window_(window),
          renderer_(renderer),
          world_(std::make_unique<sim::World>(data.engine.world)),
          proj_(data.engine.view),
          // A velocidad xN caben N veces más ticks por fotograma.
          clock_({kTickNs, data.engine.loop.max_ticks_per_frame *
                               (replay != nullptr ? data.engine.replay.speeds.back() : 1)}),
          selection_(data.engine.selection) {
        view_player_ = local_;
        if (editor_ != nullptr) {
            // Editor de escenarios (F3): todo a la vista, sin simulación ni ayuda de partida.
            view_player_.reset();
            show_help_ = false;
            edit_params_ = data_.engine.world;
        }
        if (audio_ != nullptr && !data_.sound.recipes.empty()) {
            sounds_.emplace(data_.sound, data_);
        }
        // Partida nueva de un escenario con informe: se lee antes de empezar.
        briefing_open_ = editor_ == nullptr && replay == nullptr && resume == nullptr && net_ == nullptr &&
                         !data_.scenario_briefing.empty();
        if (!data_.engine.world.scenario.objectives.empty()) {
            show_help_ = false;  // con objetivos a la vista; F2 la abre
        }
        if (replay != nullptr) {
            player_.emplace(*replay);
            replay_end_ = replay->end_tick;
            view_player_.reset();  // en una repetición se ve todo (se puede elegir la vista de un jugador)
        } else {
            recorder_.emplace(data.files, data.engine.replay.checkpoint_interval_ticks);
            replay_path_ = auto_replay_path(data);
            if (editor_ == nullptr) {
                crash_guard_.emplace(*recorder_, *world_);  // G3: un fallo guarda esta partida
            }
            if (resume != nullptr) {
                // Partida guardada: se rehace hasta donde se guardó y se sigue desde ahí.
                if (auto ok = resume_saved_game(*resume, *world_, *recorder_); !ok) {
                    throw std::runtime_error(ok.error());
                }
                spdlog::info("Partida cargada en el tick {}", world_->tick());
            }
        }
        world_->write_snapshot(curr_);
        prev_ = curr_;
        stats_.state_hash = world_->state_hash();
        // La cámara arranca sobre el primer edificio del jugador local o, sin él, en el
        // centro del mapa (donde aparecen las unidades de prueba).
        const sim::TileMap& map = world_->map();
        render::Vec2 center{static_cast<float>(map.width()) * 0.5f, static_cast<float>(map.height()) * 0.5f};
        for (const sim::SnapshotObject& o : curr_.objects) {
            if (o.kind == sim::ObjectKind::Building && o.owner == local_) {
                const float half = static_cast<float>(o.size) * 0.5f;
                center = {static_cast<float>(o.origin.x) + half, static_cast<float>(o.origin.y) + half};
                break;
            }
        }
        camera_.center_on(proj_.tile_to_world(center), renderer_.screen_size());
    }

    [[nodiscard]] sim::Tick tick() const noexcept { return world_->tick(); }
    // Editor: se pidió probar el escenario.
    [[nodiscard]] bool play_requested() const noexcept { return play_requested_; }

    // Al salir de run(): ¿se pidió volver al menú (F10 o el botón del final)?
    [[nodiscard]] bool back_to_menu() const noexcept { return back_to_menu_; }
    // El jugador local ha ganado (campaña: el capítulo cuenta como superado).
    [[nodiscard]] bool won() const noexcept { return won_; }

    int run(std::int64_t max_frames) {
        std::uint64_t last_ns = platform::now_ns();
        std::int64_t frame = 0;
        for (; max_frames == 0 || frame < max_frames; ++frame) {
            RTS_PROFILE_FRAME();
            if (!handle_events() || back_to_menu_) {
                break;
            }
            const std::uint64_t now = platform::now_ns();
            const auto frame_ns = static_cast<std::int64_t>(now - last_ns);
            last_ns = now;
            anim_time_s_ += static_cast<double>(frame_ns) / static_cast<double>(kNsPerSecond);
            if (window_.minimized()) {
                // Minimizada la simulación sigue su curso; solo se evita presentar.
                SDL_WaitEventTimeout(nullptr, static_cast<std::int32_t>(kTickNs / 1'000'000));
            }
            simulate(frame_ns);
            update_camera(frame_ns);
            if (audio_ != nullptr) {
                audio_->pump();
            }
            if (!window_.minimized()) {
                present();
            }
        }
        spdlog::info("{} fotogramas; medias móviles: escena {:.3f} ms, envío {:.3f} ms (con espera de vsync), "
                     "simulación {:.3f} ms/tick; {} sprites en el último",
                     frame, stats_.scene_ms, stats_.submit_ms, stats_.sim_ms_per_tick, renderer_.sprites_last_frame());
        if (recorder_ && world_->tick() > 0) {
            if (const auto saved = save_replay(replay_path_, recorder_->finish(*world_)); saved) {
                spdlog::info("Repetición guardada en {}", replay_path_.string());
            } else {
                spdlog::error("Repetición: {}", saved.error());
            }
        }
        return 0;
    }

private:
    bool handle_events() {
        SDL_Event event;
        while (window_.poll_event(event)) {
            renderer_.process_event(event);
            if (window_.is_close_request(event)) {
                return false;
            }
            const bool ui_mouse = renderer_.ui_wants_mouse();
            if (editor_ != nullptr && edit_event(event, ui_mouse)) {
                continue;
            }
            switch (event.type) {
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    if (event.button.button == SDL_BUTTON_LEFT && !ui_mouse) {
                        if (placing_) {
                            place_at({event.button.x, event.button.y});
                        } else {
                            selection_.begin_drag({event.button.x, event.button.y});
                        }
                    } else if (event.button.button == SDL_BUTTON_RIGHT && !ui_mouse) {
                        if (placing_) {
                            placing_.reset();
                        } else {
                            issue_context_order({event.button.x, event.button.y});
                        }
                    }
                    break;
                case SDL_EVENT_MOUSE_MOTION:
                    selection_.update_drag({event.motion.x, event.motion.y});
                    break;
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    if (event.button.button == SDL_BUTTON_LEFT && selection_.dragging()) {
                        finish_selection({event.button.x, event.button.y}, event.button.clicks);
                    }
                    break;
                case SDL_EVENT_KEY_DOWN:
                    if (player_ && !renderer_.ui_wants_keyboard()) {
                        replay_key(event.key.scancode);
                    }
                    if (!player_ && !renderer_.ui_wants_keyboard()) {
                        control_group_key(event.key.scancode);
                        if (is_key(event.key.scancode, KeyAction::LastAlert)) {
                            jump_to_alert();
                        }
                        if (is_key(event.key.scancode, KeyAction::QuickSave)) {
                            save_game();
                        }
                    }
                    if (is_key(event.key.scancode, KeyAction::Menu)) {
                        back_to_menu_ = true;  // la partida se graba al salir
                    }
                    if (is_key(event.key.scancode, KeyAction::Debug)) {
                        show_debug_ = !show_debug_;
                    }
                    if (is_key(event.key.scancode, KeyAction::Help)) {
                        show_help_ = !show_help_;
                    }
                    if (event.key.scancode == SDL_SCANCODE_ESCAPE && !renderer_.ui_wants_keyboard()) {
                        if (placing_) {
                            placing_.reset();
                        } else {
                            selection_.clear();
                            selected_building_.reset();
                        }
                    }
                    break;
                default:
                    break;
            }
        }
        return true;
    }

    // Grupos de control: Ctrl + 1…9 guarda la selección en ese grupo; 1…9 la recupera
    // (solo lo que siga vivo).
    void control_group_key(SDL_Scancode key) {
        if (key < SDL_SCANCODE_1 || key > SDL_SCANCODE_9) {
            return;
        }
        const auto index = static_cast<std::size_t>(key) - static_cast<std::size_t>(SDL_SCANCODE_1);
        if (window_.ctrl_held()) {
            groups_[index] = selection_.selected();
            return;
        }
        std::vector<std::uint32_t> alive;
        for (const std::uint32_t id : groups_[index]) {
            const auto it = std::ranges::find(curr_.entities, id, &sim::SnapshotEntity::id);
            if (it != curr_.entities.end() && it->owner == local_) {
                alive.push_back(id);
            }
        }
        groups_[index] = alive;
        if (!alive.empty()) {
            selected_building_.reset();
            selection_.set(std::move(alive));
        }
    }

    // Reproductor: espacio pausa; 1, 2, 3... eligen la velocidad de la lista de datos.
    void replay_key(SDL_Scancode key) {
        if (key == SDL_SCANCODE_SPACE) {
            paused_ = !paused_;
            return;
        }
        const auto index = static_cast<std::size_t>(key) - static_cast<std::size_t>(SDL_SCANCODE_1);
        if (key >= SDL_SCANCODE_1 && key <= SDL_SCANCODE_9 && index < data_.engine.replay.speeds.size()) {
            speed_index_ = index;
        }
    }

    // Toda orden del jugador pasa por aquí: se graba. En una repetición no se aceptan.
    // Reloj de la presentación en ms (animaciones y sonido).
    [[nodiscard]] double now_ms() const noexcept {
        constexpr double kMsPerS = 1000.0;
        return anim_time_s_ * kMsPerS;
    }

    // Dónde se ve en pantalla un punto del mapa; nada si la niebla lo tapa.
    [[nodiscard]] std::optional<render::Vec2> locate(sim::Position p) const {
        const render::Vec2 tile{fixed_to_float(p.x), fixed_to_float(p.y)};
        if (const std::vector<std::uint8_t>* fog = view_fog(); fog != nullptr && curr_.map) {
            const sim::TileCoord t{static_cast<std::int32_t>(std::floor(tile.x)), static_cast<std::int32_t>(std::floor(tile.y))};
            if (!curr_.map->contains(t)) {
                return std::nullopt;
            }
            const auto idx = static_cast<std::size_t>(t.y) * static_cast<std::size_t>(curr_.map->width()) +
                             static_cast<std::size_t>(t.x);
            if (idx >= fog->size() || static_cast<sim::Fog>((*fog)[idx]) != sim::Fog::Visible) {
                return std::nullopt;
            }
        }
        return camera_.world_to_screen(proj_.tile_to_world(tile));
    }

    // Golpes de herramienta: cuando una figura que trabaja llega a la pose de golpe.
    void work_sound(const sim::SnapshotEntity& se, render::Pose pose, render::Vec2 at) {
        const auto prev = last_pose_.find(se.id);
        const bool struck = pose == render::Pose::Strike && (prev == last_pose_.end() || prev->second != pose);
        last_pose_[se.id] = pose;
        if (!struck || !sounds_ || se.has_target) {
            return;
        }
        if (se.task == sim::WorkerTask::Build || se.task == sim::WorkerTask::Demolish) {
            sounds_->on_work(WorkSound::Build, at, renderer_.screen_size(), now_ms());
        } else if (se.task == sim::WorkerTask::Gather && se.carry_kind == sim::Resource::Wood) {
            sounds_->on_work(WorkSound::Chop, at, renderer_.screen_size(), now_ms());
        } else if (se.task == sim::WorkerTask::Gather && se.carry_kind != sim::Resource::Food) {
            sounds_->on_work(WorkSound::Mine, at, renderer_.screen_size(), now_ms());
        }
    }

    // Lo que el director de sonido decidió en este fotograma, al mezclador.
    void flush_sounds() {
        if (!sounds_ || audio_ == nullptr) {
            return;
        }
        std::vector<render::Vec2> burning;
        const render::Vec2 screen = renderer_.screen_size();
        for (const render::SceneObject& o : objects_) {
            if (o.fire_permille > 0) {
                const auto n = static_cast<float>(o.size);
                const render::Vec2 at = camera_.world_to_screen(proj_.tile_to_world(
                    {static_cast<float>(o.origin.x) + n * 0.5f, static_cast<float>(o.origin.y) + n * 0.5f}));
                if (at.x >= 0.0f && at.y >= 0.0f && at.x <= screen.x && at.y <= screen.y) {
                    burning.push_back(at);
                }
            }
        }
        sounds_->on_fires(burning, screen, now_ms());
        for (const audio::SoundCue& c : sounds_->take()) {
            audio_->bank.play(audio_->mixer, c);
        }
        audio_->music(sounds_->music(now_ms()));
    }

    void issue(sim::Command c) {
        if (sounds_ && !player_) {
            sounds_->on_order(now_ms());
        }
        if (net_ != nullptr) {
            net_->submit(std::move(c));  // sale por la red y se graba al ejecutarse
        } else if (recorder_) {
            recorder_->issue(*world_, std::move(c));
        }
    }

    [[nodiscard]] sim::TileCoord tile_at(render::Vec2 screen_pos) const {
        const render::Vec2 tile = proj_.world_to_tile(camera_.screen_to_world(screen_pos));
        return {static_cast<std::int32_t>(std::floor(tile.x)), static_cast<std::int32_t>(std::floor(tile.y))};
    }

    [[nodiscard]] const sim::SnapshotObject* find_object(std::uint32_t id) const {
        const auto it = std::ranges::find(curr_.objects, id, &sim::SnapshotObject::id);
        return it == curr_.objects.end() ? nullptr : &*it;
    }

    // Lo que el jugador que mira sabe que hay en la casilla: el objeto si lo ve (o, si
    // es un recurso, si la casilla está explorada) o, si no, el edificio recordado.
    [[nodiscard]] const sim::SnapshotObject* object_under(sim::TileCoord tile) const {
        const auto id = world_->object_at(tile);
        const sim::SnapshotObject* o = id ? find_object(*id) : nullptr;
        if (o != nullptr && shows_object(*o)) {
            return o;
        }
        for (const sim::SnapshotObject& r : remembered_objects_) {
            if (tile.x >= r.origin.x && tile.y >= r.origin.y && tile.x < r.origin.x + r.size &&
                tile.y < r.origin.y + r.size) {
                return &r;
            }
        }
        return nullptr;
    }

    // --- Niebla de guerra: lo que ve el jugador que mira (en las repeticiones, se elige) ---

    [[nodiscard]] const std::vector<std::uint8_t>* view_fog() const {
        if (!view_player_ || *view_player_ >= curr_.fog.size() || !curr_.fog[*view_player_]) {
            return nullptr;
        }
        return curr_.fog[*view_player_].get();
    }

    [[nodiscard]] bool tile_explored(sim::TileCoord t) const {
        const std::vector<std::uint8_t>* fog = view_fog();
        if (fog == nullptr || !curr_.map || !curr_.map->contains(t)) {
            return true;
        }
        const auto idx = static_cast<std::size_t>(t.y) * static_cast<std::size_t>(curr_.map->width()) +
                         static_cast<std::size_t>(t.x);
        return static_cast<sim::Fog>((*fog)[idx]) != sim::Fog::Unexplored;
    }

    [[nodiscard]] bool sees_entity(const sim::SnapshotEntity& e) const {
        if (e.garrisoned) {
            return false;  // dentro de una torre: se cuenta en ella, no se dibuja
        }
        return !view_player_ || e.owner == *view_player_ || (e.seen_by & (1U << *view_player_)) != 0;
    }

    [[nodiscard]] bool shows_object(const sim::SnapshotObject& o) const {
        if (!view_player_) {
            return true;
        }
        if (o.kind == sim::ObjectKind::Resource) {
            for (std::int32_t y = o.origin.y; y < o.origin.y + o.size; ++y) {
                for (std::int32_t x = o.origin.x; x < o.origin.x + o.size; ++x) {
                    if (tile_explored({x, y})) {
                        return true;
                    }
                }
            }
            return false;
        }
        return o.owner == *view_player_ || (o.seen_by & (1U << *view_player_)) != 0;
    }

    // Edificios enemigos recordados por el jugador que mira y que ahora no ve.
    void refresh_remembered() {
        remembered_objects_.clear();
        if (!view_player_ || *view_player_ >= curr_.memory.size()) {
            return;
        }
        for (const sim::RememberedBuilding& m : curr_.memory[*view_player_]) {
            const sim::SnapshotObject* live = find_object(entt::to_integral(m.entity));
            if (live != nullptr && shows_object(*live)) {
                continue;  // se ve ahora: se dibuja tal cual es
            }
            sim::SnapshotObject r;
            r.id = entt::to_integral(m.entity);
            r.kind = sim::ObjectKind::Building;
            r.type = m.type;
            r.owner = m.owner;
            r.origin = m.footprint.origin;
            r.size = m.footprint.size;
            r.hp = m.hp;
            r.complete = m.complete;
            r.burned = m.burned;
            r.fire = m.burning ? 1 : 0;
            remembered_objects_.push_back(r);
        }
    }

    // Clic (sin rectángulo) sin unidades debajo: un edificio propio queda seleccionado.
    // Solo se seleccionan unidades propias; se usan las posiciones del último fotograma,
    // que es lo que el jugador veía.
    void finish_selection(render::Vec2 at, std::uint8_t clicks) {
        const bool click = !selection_.has_visible_rect();
        selection_.end_drag(at, window_.shift_held(), own_screen_entities_);
        selected_building_.reset();
        // Doble clic sobre una unidad propia: todas las suyas de ese tipo en pantalla.
        if (click && clicks >= 2 && selection_.selected().size() == 1) {
            const auto it = std::ranges::find(curr_.entities, selection_.selected().front(), &sim::SnapshotEntity::id);
            if (it != curr_.entities.end()) {
                const render::Vec2 screen = renderer_.screen_size();
                std::vector<std::uint32_t> same;
                for (const ScreenEntity& s : own_screen_entities_) {
                    const auto e = std::ranges::find(curr_.entities, s.id, &sim::SnapshotEntity::id);
                    if (e != curr_.entities.end() && e->type == it->type && s.pos.x >= 0.0f && s.pos.y >= 0.0f &&
                        s.pos.x <= screen.x && s.pos.y <= screen.y) {
                        same.push_back(s.id);
                    }
                }
                selection_.set(std::move(same));
            }
        }
        if (click && selection_.selected().empty()) {
            const sim::SnapshotObject* o = object_under(tile_at(at));
            if (o != nullptr && o->kind == sim::ObjectKind::Building && o->owner == local_) {
                selected_building_ = o->id;
            }
        }
    }

    [[nodiscard]] sim::Command local_command(sim::CommandType type) const {
        // Se aplica al inicio del siguiente tick; en lockstep (M8) se programará unos
        // ticks más tarde para absorber la latencia.
        sim::Command c;
        c.tick = world_->tick();
        c.player = local_;
        c.type = type;
        c.units = selection_.selected();
        return c;
    }

    // Unidad enemiga más cercana al cursor dentro del radio de clic (posiciones del
    // último fotograma, lo que el jugador veía).
    [[nodiscard]] std::optional<std::uint32_t> enemy_unit_at(render::Vec2 screen_pos) const {
        const auto radius = static_cast<float>(data_.engine.selection.click_radius_px);
        std::optional<std::uint32_t> best;
        float best_d = radius * radius;
        for (std::size_t i = 0; i < screen_entities_.size(); ++i) {
            const sim::SnapshotEntity& se = curr_.entities[screen_index_[i]];
            if (se.owner == local_ || !sees_entity(se)) {
                continue;
            }
            const render::Vec2 d = screen_entities_[i].pos - screen_pos;
            const float dist = d.x * d.x + d.y * d.y;
            if (dist <= best_d) {
                best_d = dist;
                best = se.id;
            }
        }
        return best;
    }

    // Clic derecho: atacar a un enemigo (prender fuego a un edificio, o dañarlo con
    // asedio); apagar un edificio propio en llamas; recoger si hay un recurso o una
    // granja propia terminada; construir, reparar o descargar si es otro edificio
    // propio; mover en cualquier otro caso. Con Ctrl, ataque-movimiento: ir al destino peleando con lo que se
    // encuentre.
    void issue_context_order(render::Vec2 screen_pos) {
        if (selection_.selected().empty()) {
            // Edificio que produce seleccionado: clic derecho fija su punto de reunión.
            if (selected_building_) {
                const sim::SnapshotObject* b = find_object(*selected_building_);
                if (b != nullptr && !data_.buildings.types[b->type].type.trains.empty()) {
                    sim::Command c = local_command(sim::CommandType::SetRally);
                    c.object = *selected_building_;
                    c.target = tile_at(screen_pos);
                    issue(std::move(c));
                }
            }
            return;
        }
        const sim::TileCoord tile = tile_at(screen_pos);
        const sim::SnapshotObject* o = object_under(tile);
        sim::Command c = local_command(sim::CommandType::Move);
        if (window_.ctrl_held()) {
            c.type = sim::CommandType::AttackMove;
            c.target = tile;
        } else if (const auto enemy = enemy_unit_at(screen_pos)) {
            c.type = sim::CommandType::Attack;
            c.object = *enemy;
        } else if (o != nullptr && o->kind == sim::ObjectKind::Building && o->owner != local_ &&
                   window_.ctrl_held() && data_.buildings.types[o->type].type.climbable) {
            c.type = sim::CommandType::Climb;  // Ctrl: tomar el muro con escalas
            c.object = o->id;
        } else if (o != nullptr && o->kind == sim::ObjectKind::Building && o->owner != local_) {
            c.type = sim::CommandType::Attack;
            c.object = o->id;
        } else if (o != nullptr && o->kind == sim::ObjectKind::Resource) {
            c.type = sim::CommandType::Gather;
            c.object = o->id;
        } else if (o != nullptr && o->kind == sim::ObjectKind::Building && o->owner == local_ &&
                   window_.shift_held()) {
            c.type = sim::CommandType::Demolish;  // Mayús: desmontarlo (deja escombros recuperables)
            c.object = o->id;
        } else if (o != nullptr && o->kind == sim::ObjectKind::Building && o->owner == local_ && o->fire > 0) {
            c.type = sim::CommandType::Extinguish;  // edificio propio en llamas: apagarlo
            c.object = o->id;
        } else if (o != nullptr && o->kind == sim::ObjectKind::Building && o->owner == local_ && o->complete &&
                   data_.buildings.types[o->type].type.farm_food > 0 && !o->burned &&
                   o->hp >= data_.buildings.types[o->type].type.hp) {
            c.type = sim::CommandType::Gather;  // granja propia terminada e intacta: cultivarla
            c.object = o->id;
        } else if (o != nullptr && o->kind == sim::ObjectKind::Building && o->owner == local_) {
            c.type = sim::CommandType::Build;
            c.object = o->id;
            // Puesto médico terminado y en uso: los heridos ingresan; los aldeanos, si no
            // hay que repararlo, se quedan de enfermeros; el resto, la orden de siempre.
            const BuildingInfo& binfo = data_.buildings.types[o->type];
            if (binfo.type.beds > 0 && o->complete && !o->burned && o->hp >= binfo.type.hp) {
                sim::Command treat = local_command(sim::CommandType::Treat);
                treat.object = o->id;
                treat.units.clear();
                sim::Command tend = local_command(sim::CommandType::Tend);
                tend.object = o->id;
                tend.units.clear();
                std::erase_if(c.units, [&](std::uint32_t id) {
                    const auto it = std::ranges::find(curr_.entities, id, &sim::SnapshotEntity::id);
                    if (it == curr_.entities.end()) {
                        return false;
                    }
                    const sim::UnitType& ut = data_.units.types[it->type].type;
                    if (ut.worker || ut.care_skill > 0) {
                        tend.units.push_back(id);  // aldeanos (enfermeros) y cirujanos
                        return true;
                    }
                    if (ut.treatable && it->hp < it->max_hp) {
                        treat.units.push_back(id);
                        return true;
                    }
                    return false;
                });
                for (sim::Command* sub : {&treat, &tend}) {
                    if (!sub->units.empty()) {
                        issue(std::move(*sub));
                    }
                }
                if (c.units.empty()) {
                    return;
                }
            }
            // Torre terminada: las tropas (no los aldeanos ni el bagaje) la guarnecen.
            if (binfo.type.garrison > 0 && o->complete) {
                sim::Command garrison = local_command(sim::CommandType::Garrison);
                garrison.object = o->id;
                garrison.units.clear();
                std::erase_if(c.units, [&](std::uint32_t id) {
                    const auto it = std::ranges::find(curr_.entities, id, &sim::SnapshotEntity::id);
                    if (it == curr_.entities.end()) {
                        return false;
                    }
                    const sim::UnitType& ut = data_.units.types[it->type].type;
                    const bool troop = !ut.worker && ut.convoy_capacity == 0 && !ut.combat.buildings_only &&
                                       ut.combat.auto_attack;
                    if (troop) {
                        garrison.units.push_back(id);
                    }
                    return troop;
                });
                if (!garrison.units.empty()) {
                    issue(std::move(garrison));
                }
                if (c.units.empty()) {
                    return;
                }
            }
            // El bagaje, a un mercado propio: ruta de caravana.
            if (binfo.type.market && o->complete) {
                sim::Command route = local_command(sim::CommandType::TradeRoute);
                route.object = o->id;
                route.units.clear();
                std::erase_if(c.units, [&](std::uint32_t uid) {
                    const auto it = std::ranges::find(curr_.entities, uid, &sim::SnapshotEntity::id);
                    const bool carrier =
                        it != curr_.entities.end() && data_.units.types[it->type].type.convoy_capacity > 0;
                    if (carrier) {
                        route.units.push_back(uid);
                    }
                    return carrier;
                });
                if (!route.units.empty()) {
                    issue(std::move(route));
                }
                if (c.units.empty()) {
                    return;
                }
            }
                        // El bagaje, a un edificio que abastece: ruta de convoy (campamento) o cargar
            // y quedarse; el resto de la selección, la orden de siempre.
            if (data_.buildings.types[o->type].type.supplies) {
                sim::Command convoy = local_command(sim::CommandType::Convoy);
                convoy.object = o->id;
                convoy.units.clear();
                std::erase_if(c.units, [&](std::uint32_t id) {
                    const auto it = std::ranges::find(curr_.entities, id, &sim::SnapshotEntity::id);
                    const bool carrier =
                        it != curr_.entities.end() && data_.units.types[it->type].type.convoy_capacity > 0;
                    if (carrier) {
                        convoy.units.push_back(id);
                    }
                    return carrier;
                });
                if (!convoy.units.empty()) {
                    issue(std::move(convoy));
                }
                if (c.units.empty()) {
                    return;
                }
            }
        } else {
            c.target = tile;
            if (window_.shift_held()) {
                c.kind = sim::kQueueMove;  // Mayús: se encola tras el movimiento en curso
            }
        }
        issue(std::move(c));
    }

    [[nodiscard]] sim::TileCoord ghost_origin(sim::TileCoord hover, sim::BuildingTypeId type) const {
        const std::int32_t size = data_.buildings.types[type].type.size;
        return {hover.x - size / 2, hover.y - size / 2};
    }

    // Coloca el edificio en curso bajo el cursor con los aldeanos seleccionados. Con
    // Mayús se sigue colocando más del mismo tipo.
    void place_at(render::Vec2 screen_pos) {
        const sim::BuildingTypeId type = *placing_;
        sim::Command c = local_command(sim::CommandType::Place);
        c.kind = type;
        c.target = ghost_origin(tile_at(screen_pos), type);
        issue(std::move(c));
        if (!window_.shift_held()) {
            placing_.reset();
        }
    }

    // Superposiciones de depuración. Leen el registro en solo lectura: son herramientas,
    // no presentación del juego, y no pasan por el snapshot.
    void build_overlays() {
        tints_.clear();
        if (selected_building_) {
            if (const sim::SnapshotObject* b = find_object(*selected_building_); b != nullptr && b->rally.x >= 0) {
                tints_.push_back({b->rally, data_.engine.view.marker_selected_color});  // punto de reunión
            }
        }
        path_points_.clear();
        const sim::MovementSystem& mv = world_->movement();
        const auto& reg = world_->registry();
        if (show_portals_) {
            for (std::size_t n = 0; n < mv.hpa().node_count(); ++n) {
                tints_.push_back({mv.hpa().node_tile(n), data_.engine.view.debug_overlay_color});
            }
        }
        const sim::FlowField* field = nullptr;
        for (const std::uint32_t id : selection_.selected()) {
            const auto e = static_cast<entt::entity>(id);
            if (!reg.valid(e)) {
                continue;
            }
            const sim::MoveGoal* goal = reg.try_get<sim::MoveGoal>(e);
            if (goal != nullptr && field == nullptr) {
                field = mv.flow_field_of(*goal);
            }
            const sim::PathFollow* follow = reg.try_get<sim::PathFollow>(e);
            if (!show_paths_ || follow == nullptr) {
                continue;
            }
            auto add_point = [&](sim::TileCoord t) {
                const render::Vec2 center{static_cast<float>(t.x) + 0.5f, static_cast<float>(t.y) + 0.5f};
                path_points_.push_back(camera_.world_to_screen(proj_.tile_to_world(center)));
            };
            for (std::size_t i = follow->next_tile; i < follow->segment.size(); ++i) {
                add_point(follow->segment[i]);
            }
            for (std::size_t i = follow->next_waypoint; i < follow->waypoints.size(); ++i) {
                add_point(follow->waypoints[i]);
            }
        }
        if (show_flow_ && field != nullptr) {
            // Tinte proporcional a la cercanía al destino, solo en las casillas visibles.
            const render::Vec2 screen = renderer_.screen_size();
            const sim::TileMap& map = world_->map();
            std::int32_t max_cost = 1;
            render::for_each_visible_tile(proj_, camera_, screen, map.width(), map.height(), [&](std::int32_t i, std::int32_t j) {
                const std::int32_t c = field->cost(mv.grid(), {i, j});
                if (c != sim::kUnreached) {
                    max_cost = std::max(max_cost, c);
                }
            });
            render::for_each_visible_tile(proj_, camera_, screen, map.width(), map.height(), [&](std::int32_t i, std::int32_t j) {
                const std::int32_t c = field->cost(mv.grid(), {i, j});
                if (c == sim::kUnreached) {
                    return;
                }
                render::Rgba color = data_.engine.view.debug_overlay_color;
                color[3] = static_cast<std::uint8_t>(color[3] * (max_cost - c) / max_cost);
                tints_.push_back({{i, j}, color});
            });
        }
    }

    void simulate(std::int64_t frame_ns) {
        if (editor_ != nullptr || briefing_open_) {
            return;  // el editor no simula; con el informe abierto, la partida aún no empieza
        }
        std::int64_t sim_ns = frame_ns;
        if (player_) {
            const bool stopped = paused_ || player_->finished(*world_);
            sim_ns = stopped ? 0 : frame_ns * data_.engine.replay.speeds[speed_index_];
        }
        if (net_ != nullptr) {
            net_->poll(0);
            net_waiting_ = false;
            if (net_->desync() && !desync_reported_) {
                desync_reported_ = true;
                write_crash_report(std::format("desincronización en red: {}", net_->error()));
            }
        }
        const StepPlan plan = clock_.advance(sim_ns);
        const auto start = SteadyClock::now();
        for (std::int32_t i = 0; i < plan.ticks; ++i) {
            RTS_PROFILE_ZONE_NAMED("sim_tick");
            if (player_ && player_->finished(*world_)) {
                break;
            }
            // En red, un turno solo empieza con las órdenes de todos: si faltan, este
            // fotograma no avanza más (la partida va al paso del más lento).
            if (net_ != nullptr &&
                !net_->begin_tick(*world_, [this](sim::Command c) { recorder_->issue(*world_, std::move(c)); })) {
                net_waiting_ = net_->state() == LockstepState::Running;
                break;
            }
            std::swap(prev_, curr_);
            if (player_) {
                player_->before_step(*world_);
            }
            world_->step();
            if (player_) {
                player_->after_step(*world_);
            } else {
                recorder_->after_step(*world_);
            }
            world_->write_snapshot(curr_);
            if (sounds_) {
                sounds_->on_tick(prev_, curr_, local_, [this](sim::Position p) { return locate(p); },
                                 renderer_.screen_size(), now_ms());
            }
            if (!player_) {
                for (const Alert& a : alerts_.update(prev_, curr_, local_)) {
                    if (sounds_) {
                        sounds_->on_alert(a.kind, now_ms());
                    }
                }
            }
        }
        if (plan.ticks > 0) {
            const double per_tick = elapsed_ms(start) / plan.ticks;
            stats_.sim_ms_per_tick += kSmoothing * (per_tick - stats_.sim_ms_per_tick);
            stats_.state_hash = world_->state_hash();
        }
        if (plan.dropped_ticks > 0) {
            spdlog::warn("Fotograma lento: {} ticks descartados", plan.dropped_ticks);
        }
        stats_.frame_ms = static_cast<double>(frame_ns) / kNsPerMs;
        stats_.ticks_this_frame = plan.ticks;
        stats_.dropped_ticks_total += plan.dropped_ticks;
        stats_.alpha = plan.alpha;
    }

    void update_camera(std::int64_t frame_ns) {
        const render::Vec2 screen = renderer_.screen_size();
        CameraInput input;
        if (!renderer_.ui_wants_keyboard()) {
            const platform::ScrollKeys keys = window_.scroll_keys();
            input.left = keys.left;
            input.right = keys.right;
            input.up = keys.up;
            input.down = keys.down;
        }
        const platform::MouseState mouse = window_.mouse();
        input.mouse = {mouse.x, mouse.y};
        // Durante un arrastre de selección el borde no desplaza: el rectángulo se movería bajo el ratón.
        input.mouse_in_window = mouse.in_window && !selection_.dragging() && !renderer_.ui_wants_mouse();

        const float dt = std::min(static_cast<float>(frame_ns) / kNsPerSecondF, kMaxCameraDtS);
        camera_.origin = camera_.origin + camera_scroll(data_.engine.camera, input, screen, dt);
        const sim::TileMap& map = world_->map();
        camera_.clamp_to_map(proj_, map.width(), map.height(), screen);
    }

    // Interpola cada marcador entre los dos últimos ticks y lo proyecta a pantalla.
    void build_screen_entities() {
        screen_entities_.clear();
        screen_index_.clear();
        const bool can_interpolate = prev_.entities.size() == curr_.entities.size();
        const auto a = static_cast<float>(stats_.alpha);
        moving_.assign(curr_.entities.size(), false);
        // Las figuras se pulsan por el cuerpo, no por los pies.
        const render::Vec2 lift{0.0f, art_ ? -static_cast<float>(data_.engine.view.unit_pick_lift_px) : 0.0f};
        for (std::size_t i = 0; i < curr_.entities.size(); ++i) {
            const sim::SnapshotEntity& c = curr_.entities[i];
            render::Vec2 tile{fixed_to_float(c.pos.x), fixed_to_float(c.pos.y)};
            if (can_interpolate && prev_.entities[i].id == c.id) {
                const render::Vec2 p{fixed_to_float(prev_.entities[i].pos.x), fixed_to_float(prev_.entities[i].pos.y)};
                const render::Vec2 step = tile - p;
                if (step.x != 0.0f || step.y != 0.0f) {
                    moving_[i] = true;
                    // En pantalla, x crece con (dx - dy): de ahí hacia dónde mira.
                    const float dx = step.x - step.y;
                    if (dx != 0.0f) {
                        facing_left_[c.id] = dx < 0.0f;
                    }
                }
                tile = p + step * a;
            }
            if (c.has_target) {
                const float dx = (fixed_to_float(c.target_pos.x) - fixed_to_float(c.target_pos.y)) - (tile.x - tile.y);
                if (dx != 0.0f && !moving_[i]) {
                    facing_left_[c.id] = dx < 0.0f;
                }
            }
            screen_entities_.push_back({c.id, camera_.world_to_screen(proj_.tile_to_world(tile)) + lift});
        }
        own_screen_entities_.clear();
        for (std::size_t i = 0; i < curr_.entities.size(); ++i) {
            if (curr_.entities[i].owner == local_) {
                own_screen_entities_.push_back(screen_entities_[i]);
            }
        }
        // Orden de pintado: de atrás (y pequeña) hacia delante. Se ordena una permutación
        // para llevar el tipo de cada unidad con su posición.
        order_.resize(screen_entities_.size());
        for (std::size_t i = 0; i < order_.size(); ++i) {
            order_[i] = i;
        }
        std::ranges::stable_sort(order_, {}, [this](std::size_t i) { return screen_entities_[i].pos.y; });
        sorted_.clear();
        for (const std::size_t i : order_) {
            if (!sees_entity(curr_.entities[i])) {
                continue;  // fuera de la vista del jugador que mira
            }
            sorted_.push_back(screen_entities_[i]);
            screen_index_.push_back(i);
        }
        screen_entities_.swap(sorted_);
    }

    [[nodiscard]] std::optional<sim::TileCoord> hovered_tile() const {
        const platform::MouseState mouse = window_.mouse();
        if (!mouse.in_window || renderer_.ui_wants_mouse()) {
            return std::nullopt;
        }
        const render::Vec2 tile = proj_.world_to_tile(camera_.screen_to_world({mouse.x, mouse.y}));
        const sim::TileCoord c{static_cast<std::int32_t>(std::floor(tile.x)), static_cast<std::int32_t>(std::floor(tile.y))};
        if (!curr_.map || !curr_.map->contains(c)) {
            return std::nullopt;
        }
        return c;
    }

    void draw_ui(const std::optional<sim::TileCoord>& hover) {
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        if (editor_ != nullptr) {
            draw_editor(display, hover);
        }
        draw_resource_bar(display);
        draw_selection_panel(display);
        draw_outcome(display);
        draw_objectives(display);
        draw_briefing(display);
        draw_replay_panel(display);
        draw_help(display);
        draw_alerts(display);
        draw_notice(display);
        if (net_ != nullptr) {
            draw_net(display);
        }
        draw_minimap();
        draw_hover_tooltip(hover);
        draw_night();
        draw_unit_labels();
        if (show_debug_) {
            draw_debug(display, hover);
        }
    }

    // Paneles de depuración (F1): rendimiento, movimiento, combate y la casilla bajo el ratón.
    void draw_debug(const ImVec2& display, const std::optional<sim::TileCoord>& hover) {
        ImGui::SetNextWindowPos({kPanelMarginPx, kPanelMarginPx}, ImGuiCond_FirstUseEver);
        ImGui::Begin(T("Depuración"), nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Text(T("Backend GPU: %s"), renderer_.driver_name());
        ImGui::Text(T("Fotograma: %.2f ms (%.0f FPS)"), stats_.frame_ms,
                    stats_.frame_ms > 0.0 ? 1000.0 / stats_.frame_ms : 0.0);
        ImGui::Text(T("Escena: %.3f ms · envío: %.3f ms (incluye vsync)"), stats_.scene_ms, stats_.submit_ms);
        ImGui::Text(T("Sprites: %zu (%d casillas, %d objetos, %d marcadores), 1 llamada de dibujo"),
                    renderer_.sprites_last_frame(), stats_.scene.tiles_drawn, stats_.scene.objects_drawn,
                    stats_.scene.markers_drawn);
        ImGui::Separator();
        ImGui::Text(T("Tick: %u"), curr_.tick);
        ImGui::Text(T("Ticks en este fotograma: %d"), stats_.ticks_this_frame);
        ImGui::Text(T("Simulación: %.3f ms/tick (media)"), stats_.sim_ms_per_tick);
        ImGui::Text(T("Ticks descartados (total): %lld"), static_cast<long long>(stats_.dropped_ticks_total));
        ImGui::Text(T("alpha: %.3f"), stats_.alpha);
        ImGui::Text(T("Hash de estado: %016llx"), static_cast<unsigned long long>(stats_.state_hash));
        ImGui::Separator();
        const sim::MovementTickStats& mv = world_->movement().last_stats();
        ImGui::Text(T("Movimiento: %d en marcha · %d caminos resueltos, %d pendientes"), mv.moving_units, mv.paths_solved,
                    mv.paths_pending);
        ImGui::Text(T("Nodos expandidos en el último tick: %lld · campos de flujo: %d"),
                    static_cast<long long>(mv.nodes_expanded), mv.flow_fields_built);
        ImGui::Text(T("HPA*: %zu nodos, %zu aristas · sectores rehechos en el último tick: %d"),
                    world_->movement().hpa().node_count(), world_->movement().hpa().edge_count(), mv.sectors_rebuilt);
        ImGui::Checkbox(T("Portales HPA*"), &show_portals_);
        ImGui::SameLine();
        ImGui::Checkbox(T("Ruta"), &show_paths_);
        ImGui::SameLine();
        ImGui::Checkbox(T("Campo de flujo"), &show_flow_);
        const sim::CombatTickStats& cb = world_->combat().last_stats();
        ImGui::Text(T("Combate: %d golpes, %d proyectiles (%d aciertos, %d fallos), %d bajas en el último tick"),
                    cb.melee_hits, cb.projectiles_fired, cb.projectiles_hit, cb.projectiles_missed, cb.kills);
        ImGui::End();

        // Arriba a la derecha, anclado por su esquina superior derecha.
        ImGui::SetNextWindowPos({display.x - kPanelMarginPx, kPanelMarginPx}, ImGuiCond_FirstUseEver, {1.0f, 0.0f});
        ImGui::Begin(T("Casilla"), nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        if (hover && curr_.map) {
            const sim::TileMap& map = *curr_.map;
            const sim::TerrainId id = map.terrain(*hover);
            const TerrainInfo& info = data_.terrain.types[id];
            ImGui::Text("(%d, %d)", hover->x, hover->y);
            ImGui::Text(T("Terreno: %s (id %u)"), N(info.name), static_cast<unsigned>(id));
            ImGui::Text(T("Transitable: %s"), info.passable ? T("sí") : T("no"));
            ImGui::Text(T("Altura: %u"), static_cast<unsigned>(map.elevation(*hover)));
            if (const sim::SnapshotObject* o = object_under(*hover)) {
                if (o->kind == sim::ObjectKind::Resource) {
                    const NodeInfo& n = data_.nodes.types[o->type];
                    ImGui::Text(T("%s: quedan %d de %s"), N(n.name), o->amount,
                                T(resource_key(n.type.kind).data()));
                } else {
                    ImGui::Text(T("%s del jugador %u"), N(data_.buildings.types[o->type].name),
                                static_cast<unsigned>(o->owner));
                }
            }
        } else {
            ImGui::TextDisabled("%s", T("Pasa el ratón sobre el mapa"));
        }
        ImGui::End();
    }

    // Recursos no nulos de un almacén o una carga, p. ej. "comida 21 · madera 6".
    [[nodiscard]] static std::string stock_text(const sim::Stock& s) {
        std::string out;
        for (std::size_t r = 0; r < sim::kResourceCount; ++r) {
            if (s[r] != 0) {
                out += std::format("{}{} {}", out.empty() ? "" : " · ", T(resource_key(static_cast<sim::Resource>(r)).data()), s[r]);
            }
        }
        return out.empty() ? std::string(T("nada")) : out;
    }

    [[nodiscard]] static const char* convoy_text(sim::ConvoyTask t) {
        switch (t) {
            case sim::ConvoyTask::Load:
                return T("va a cargar");
            case sim::ConvoyTask::Unload:
                return T("lleva la carga al campamento");
            case sim::ConvoyTask::Idle:
                break;
        }
        return T("parada: abastece a las tropas de alrededor");
    }

    [[nodiscard]] bool out_of_ammo(const sim::SnapshotEntity& e) const {
        return data_.units.types[e.type].type.supply.ammo > 0 && e.ammo <= 0;
    }

    // Víveres y munición de una unidad propia (lo que el jugador sabe de las suyas).
    void draw_supply_text(const sim::SnapshotEntity& e) const {
        const sim::SupplyStats& st = data_.units.types[e.type].type.supply;
        if (st.rations > 0) {
            ImGui::Text(T("víveres %d/%d"), e.rations, st.rations);
            if (st.ammo > 0) {
                ImGui::SameLine();
            }
        }
        if (st.ammo > 0) {
            ImGui::Text(T("munición %d/%d"), e.ammo, st.ammo);
        }
        if (e.hungry) {
            ImGui::TextColored(kWarnColor, T("con hambre: ataca y trabaja peor%s"),
                               st.starves ? T("; acabará perdiendo vida") : "");
        }
        if (out_of_ammo(e)) {
            ImGui::TextColored(kWarnColor, "%s", T("sin munición: no puede disparar"));
        }
    }

    // Mercado (C3): comprar y vender lotes por oro a los precios de ahora.
    void draw_market(const sim::SnapshotObject& o, std::uint32_t id) {
        if (!data_.buildings.types[o.type].type.market || !world_->market().enabled()) {
            return;
        }
        const sim::MarketParams& mp = world_->market().params();
        ImGui::SeparatorText(TF("Mercado (lotes de {})", mp.lot).c_str());
        ImGui::TextDisabled("%s", T("Clic derecho con bagaje: caravana desde el mercado propio más cercano"));
        static constexpr std::array<const char*, sim::kResourceCount> kNames{TK("comida"), TK("madera"), TK("piedra"), TK("oro"), TK("hierro")};
        for (std::size_t r = 0; r < sim::kResourceCount; ++r) {
            if (mp.base_price[r] <= 0) {
                continue;
            }
            ImGui::Text(T("%s: compra %d, venta %d"), T(kNames[r]), curr_.market_prices[r], world_->market().sell_price(r));
            ImGui::SameLine();
            ImGui::PushID(static_cast<int>(r));
            if (ImGui::SmallButton(T("Comprar"))) {
                sim::Command c = local_command(sim::CommandType::Trade);
                c.units.clear();
                c.object = id;
                c.kind = static_cast<std::uint8_t>(r);
                issue(std::move(c));
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(T("Vender"))) {
                sim::Command c = local_command(sim::CommandType::Trade);
                c.units.clear();
                c.object = id;
                c.kind = static_cast<std::uint8_t>(r | sim::kSellBit);
                issue(std::move(c));
            }
            ImGui::PopID();
        }
    }

    // Herrería (C1): mejoras que se investigan aquí, en curso, hechas o por hacer.
    void draw_research(const sim::SnapshotObject& o, std::uint32_t id) {
        const auto& ups = data_.buildings.upgrades;
        if (std::ranges::none_of(ups, [&](const UpgradeInfo& u) { return u.type.at == o.type; })) {
            return;
        }
        ImGui::SeparatorText(T("Mejoras"));
        const sim::PlayerState* me = local_ < curr_.players.size() ? &curr_.players[local_] : nullptr;
        const auto done = [&](std::size_t u) {
            return me != nullptr && u < me->researched.size() && me->researched[u] != 0;
        };
        if (o.research >= 0) {
            const UpgradeInfo& u = ups[static_cast<std::size_t>(o.research)];
            ImGui::ProgressBar(static_cast<float>(o.research_progress) / static_cast<float>(u.type.research_ticks),
                               {-1.0f, 0.0f}, N(u.name));
            if (ImGui::SmallButton(T("Anular (se devuelve el coste)"))) {
                sim::Command c = local_command(sim::CommandType::CancelTrain);
                c.units.clear();
                c.object = id;
                issue(std::move(c));
            }
        }
        for (std::size_t i = 0; i < ups.size(); ++i) {
            const UpgradeInfo& u = ups[i];
            if (u.type.at != o.type) {
                continue;
            }
            if (done(i)) {
                ImGui::TextDisabled(T("%s: hecha"), N(u.name));
                continue;
            }
            const bool ready = !u.type.requires_upgrade || done(*u.type.requires_upgrade);
            ImGui::BeginDisabled(!ready || o.research >= 0 || me == nullptr || !affordable(me->stock, u.type.cost));
            if (ImGui::Button(std::format("{} ({})", N(u.name), cost_text(u.type.cost)).c_str())) {
                sim::Command c = local_command(sim::CommandType::Research);
                c.units.clear();
                c.object = id;
                c.kind = static_cast<std::uint8_t>(i);
                issue(std::move(c));
            }
            ImGui::EndDisabled();
        }
    }

    // Estado sanitario de una unidad.
    void draw_care_text(const sim::SnapshotEntity& e) const {
        if (e.admitted) {
            ImGui::TextColored({0.6f, 0.85f, 1.0f, 1.0f}, "%s", T("ingresado en un puesto médico: no combate"));
        } else if (e.care_post != sim::kNoObject) {
            ImGui::TextDisabled("%s", T("herido, camino del puesto médico"));
        }
        if (e.reorganizing) {
            ImGui::TextColored(kWarnColor, "%s", T("reorganizándose: aún no ataca"));
        }
        if (e.morale >= 0) {
            ImGui::Text(T("moral %d %%"), e.morale * kPercent / sim::kFullMorale);
        }
        if (e.routing) {
            ImGui::TextColored(kWarnColor, "%s", T("en desbandada: huye y no obedece hasta rehacerse"));
        }
        if (e.caravan) {
            ImGui::TextDisabled("%s", T("caravana entre mercados: cada llegada da oro"));
        }
        if (e.climbing) {
            ImGui::TextColored(kWarnColor, "%s", T("subiendo por una escala: no pelea y está expuesto"));
        }
        if (e.formation != sim::FormationKind::None) {
            static constexpr std::array<const char*, 4> kNames{"", TK("línea"), TK("columna"), TK("cuadro")};
            ImGui::Text(T("en %s%s"), T(kNames[static_cast<std::size_t>(e.formation)]),
                        e.formation_active ? "" : T(" (pocos para formar: sin efecto)"));
        }
        if (e.fatigue >= 0) {
            ImGui::Text(T("cansancio %d %%%s"), e.fatigue * kPercent / sim::kFullFatigue,
                        e.forced_march ? T(" · a paso forzado") : "");
        }
    }

    // Ayuda (F2): controles y leyenda de los marcadores.
    void draw_help(const ImVec2& display) {
        if (!show_help_) {
            ImGui::SetNextWindowPos({display.x - kPanelMarginPx, display.y - kPanelMarginPx}, ImGuiCond_Always,
                                    {1.0f, 1.0f});
            ImGui::Begin("AyudaOculta", nullptr,
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoInputs);
            ImGui::TextDisabled("%s", TF("{}: ayuda · {}: depuración", key_label(KeyAction::Help), key_label(KeyAction::Debug)).c_str());
            ImGui::End();
            return;
        }
        ImGui::SetNextWindowPos({display.x - kPanelMarginPx, display.y - kPanelMarginPx}, ImGuiCond_FirstUseEver,
                                {1.0f, 1.0f});
        ImGui::Begin(T("Ayuda (F2)"), &show_help_, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::SeparatorText(T("Ratón"));
        ImGui::BulletText("%s", T("Clic o arrastre: seleccionar tus unidades (Mayús: añadir)"));
        ImGui::BulletText("%s", T("Clic en un edificio tuyo: ver su panel y entrenar unidades"));
        ImGui::BulletText("%s", T("Clic derecho en el suelo: mover"));
        ImGui::BulletText("%s", T("Clic derecho en un enemigo: atacar (sin asedio, a un edificio se le prende fuego)"));
        ImGui::BulletText("%s", T("Clic derecho en un recurso o granja: recoger (aldeanos)"));
        ImGui::BulletText("%s", T("Clic derecho en un edificio tuyo: construir, reparar o descargar"));
        ImGui::BulletText("%s", T("Clic derecho en un edificio tuyo en llamas: apagarlo"));
        ImGui::BulletText("%s", T("Ctrl + clic derecho: avanzar atacando lo que salga"));
        ImGui::BulletText("%s", T("Mayús + clic derecho en un edificio tuyo: desmontarlo"));
        ImGui::BulletText("%s", T("Mayús + clic derecho en el suelo: añadir un punto de paso"));
        ImGui::BulletText("%s", T("Edificio seleccionado + clic derecho: punto de reunión"));
        ImGui::BulletText("%s", T("Doble clic en una unidad: todas las de su tipo en pantalla"));
        ImGui::SeparatorText(T("Teclado"));
        ImGui::BulletText("%s", TF("{}, {}, {}, {} o {}, {}, {}, {}, o borde de la ventana: mover la cámara",
                                   key_label(KeyAction::CameraLeft), key_label(KeyAction::CameraRight),
                                   key_label(KeyAction::CameraUp), key_label(KeyAction::CameraDown),
                                   key_label(KeyAction::CameraLeftAlt), key_label(KeyAction::CameraRightAlt),
                                   key_label(KeyAction::CameraUpAlt), key_label(KeyAction::CameraDownAlt))
                                    .c_str());
        ImGui::BulletText("%s", T("Esc: cancelar colocación o soltar la selección"));
        ImGui::BulletText("%s", T("Ctrl + 1-9: guardar grupo; 1-9: seleccionarlo"));
        ImGui::BulletText("%s", TF("{}: ir al último aviso · {}: guardar la partida", key_label(KeyAction::LastAlert),
                                   key_label(KeyAction::QuickSave))
                                    .c_str());
        ImGui::BulletText("%s", TF("{}: volver al menú (la partida queda grabada)", key_label(KeyAction::Menu)).c_str());
        ImGui::BulletText("%s", TF("{}: datos de depuración · {}: esta ayuda", key_label(KeyAction::Debug),
                                   key_label(KeyAction::Help))
                                    .c_str());
        ImGui::SeparatorText(T("Leyenda"));
        ImGui::BulletText("%s", T("Cada figura viste el color de su jugador; la inicial sale solo como aviso"));
        ImGui::BulletText("%s", T("Punto de color: lo que lleva un aldeano"));
        ImGui::BulletText("%s", T("Inicial en rojo: con hambre o sin munición"));
        ImGui::BulletText("%s", T("Inicial en amarillo: en desbandada (huye y no obedece)"));
        ImGui::BulletText("%s", T("Ctrl + clic derecho en un muro enemigo: escalarlo (leva y hombres de armas)"));
        ImGui::BulletText("%s", T("Clic derecho en una torre propia con tropas: guarnecerla"));
        ImGui::SeparatorText(T("Logística"));
        ImGui::BulletText("%s", T("Las tropas gastan víveres; los tiradores, munición"));
        ImGui::BulletText("%s", T("Se reponen junto al centro urbano, el molino, el cuartel o un campamento"));
        ImGui::BulletText("%s", T("Cada ración cuesta comida; la munición, madera (y hierro)"));
        ImGui::BulletText("%s", T("Campamento: almacén avanzado que llenan acémilas y carretas"));
        ImGui::BulletText("%s", T("Bagaje + clic derecho en un campamento: ruta de convoy"));
        ImGui::BulletText("%s", T("Bagaje cargado y parado: abastece a las tropas de alrededor"));
        ImGui::BulletText("%s", T("Trabuquete: se monta en un campamento con lo traído en convoy"));
        ImGui::SeparatorText(T("Sanidad"));
        ImGui::BulletText("%s", T("Heridos + clic derecho en un puesto médico: ingresan"));
        ImGui::BulletText("%s", T("Aldeanos o cirujanos + clic derecho en él: atienden (el cirujano cura mejor)"));
        ImGui::BulletText("%s", T("Socorro estabiliza; hospital de campaña y hospital curan del todo"));
        ImGui::BulletText(T("Solo lo leve (%d %% de vida o más) sana solo; si cae el puesto, mueren"),
                          data_.engine.world.medicine.light_wound_percent);
        ImGui::SeparatorText(T("Niebla de guerra"));
        ImGui::BulletText("%s", T("Negro: sin explorar; oscuro: explorado, sin vista ahora"));
        ImGui::BulletText("%s", T("Los árboles tapan la vista; desde una loma se ve más lejos"));
        ImGui::BulletText("%s", T("De noche se ve la mitad; los edificios enemigos se recuerdan"));
        ImGui::BulletText("%s", T("Pasa el ratón sobre algo para ver qué es"));
        ImGui::End();
    }

    // Texto del dueño visto por el jugador local.
    [[nodiscard]] std::string owner_text(sim::PlayerId owner) const {
        return owner == local_ ? std::string(T("tuyo")) : TF("enemigo (jugador {})", owner);
    }

    // Nombre de lo que hay bajo el ratón: la unidad más cercana dentro del radio de clic
    // o, si no hay, el edificio o recurso de la casilla.
    void draw_hover_tooltip(const std::optional<sim::TileCoord>& hover) {
        if (!hover || selection_.dragging()) {
            return;
        }
        const platform::MouseState mouse = window_.mouse();
        const render::Vec2 at{mouse.x, mouse.y};
        const auto radius = static_cast<float>(data_.engine.selection.click_radius_px);
        const sim::SnapshotEntity* best = nullptr;
        float best_d = radius * radius;
        for (std::size_t i = 0; i < screen_entities_.size(); ++i) {
            const render::Vec2 d = screen_entities_[i].pos - at;
            const float dist = d.x * d.x + d.y * d.y;
            if (dist <= best_d) {
                best_d = dist;
                best = &curr_.entities[screen_index_[i]];
            }
        }
        if (best != nullptr) {
            const UnitInfo& u = data_.units.types[best->type];
            ImGui::BeginTooltip();
            ImGui::Text("%s · %s", N(u.name), owner_text(best->owner).c_str());
            ImGui::Text(T("vida %d/%d · nivel %d"), best->hp, best->max_hp, best->level);
            if (best->owner == local_) {
                draw_supply_text(*best);
            }
            draw_care_text(*best);
            if (best->carried > 0) {
                ImGui::Text(T("lleva %d de %s"), best->carried, T(resource_key(best->carry_kind).data()));
            }
            ImGui::EndTooltip();
            return;
        }
        const sim::SnapshotObject* o = object_under(*hover);
        if (o == nullptr) {
            return;
        }
        ImGui::BeginTooltip();
        if (o->kind == sim::ObjectKind::Resource) {
            const NodeInfo& n = data_.nodes.types[o->type];
            ImGui::Text(T("%s: quedan %d de %s"), N(n.name), o->amount,
                        T(resource_key(n.type.kind).data()));
        } else {
            const BuildingInfo& b = data_.buildings.types[o->type];
            ImGui::Text("%s · %s", N(b.name), owner_text(o->owner).c_str());
            ImGui::Text(T("vida %d/%d%s"), o->hp, b.type.hp, o->complete ? "" : " · en obra");
            if (o->fire > 0) {
                ImGui::TextColored({1.0f, 0.5f, 0.1f, 1.0f}, "%s", T("en llamas"));
            } else if (o->burned) {
                ImGui::TextColored({0.8f, 0.6f, 0.4f, 1.0f}, "%s", T("quemado: no funciona"));
            }
        }
        ImGui::EndTooltip();
    }

    // Inicial del tipo junto a cada unidad (a la derecha del marcador, para no tapar su
    // color ni la barra de vida), sobre un recuadro oscuro que se lee en cualquier terreno.
    // Va en la capa de fondo de ImGui: encima de la escena y debajo de los paneles.
    // Minimapa (arriba a la izquierda): terreno con la niebla del jugador que mira, lo
    // que ve y recuerda, y el recuadro de la cámara. Clic o arrastre: mover la cámara.
    void draw_minimap() {
        if (!curr_.map) {
            return;
        }
        const sim::TileMap& map = *curr_.map;
        const render::ViewParams& view = data_.engine.view;
        const auto width = static_cast<float>(view.minimap_width_px);
        const float k = width / static_cast<float>(map.width() + map.height());  // píxeles por casilla
        ImGui::SetNextWindowPos({kPanelMarginPx, kPanelMarginPx * 5.0f}, ImGuiCond_FirstUseEver);
        ImGui::Begin(T("Mapa"), nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse);
        const ImVec2 o = ImGui::GetCursorScreenPos();
        const auto to_mini = [&](float tx, float ty) {
            return ImVec2{o.x + (tx - ty) * k + width * 0.5f, o.y + (tx + ty) * k * 0.5f};
        };
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const std::int32_t cells = view.minimap_cells;
        const std::int32_t bw = std::max(1, (map.width() + cells - 1) / cells);
        const std::int32_t bh = std::max(1, (map.height() + cells - 1) / cells);
        const std::vector<std::uint8_t>* fog = view_fog();
        for (std::int32_t by = 0; by < map.height(); by += bh) {
            for (std::int32_t bx = 0; bx < map.width(); bx += bw) {
                const sim::TileCoord t{bx + bw / 2, by + bh / 2};
                const sim::TileCoord c{std::min(t.x, map.width() - 1), std::min(t.y, map.height() - 1)};
                const auto& rgb = data_.terrain.types[map.terrain(c)].color;
                render::Rgba col{rgb[0], rgb[1], rgb[2], 255};
                if (fog != nullptr) {
                    const auto f = static_cast<sim::Fog>(
                        (*fog)[static_cast<std::size_t>(c.y) * static_cast<std::size_t>(map.width()) +
                               static_cast<std::size_t>(c.x)]);
                    if (f == sim::Fog::Unexplored) {
                        col = view.fog_unexplored_color;
                    } else if (f == sim::Fog::Explored) {
                        col = shaded(col, view.fog_explored_shade_percent);
                    }
                }
                const auto x0 = static_cast<float>(bx);
                const auto y0 = static_cast<float>(by);
                const auto x1 = static_cast<float>(bx + bw);
                const auto y1 = static_cast<float>(by + bh);
                dl->AddQuadFilled(to_mini(x0, y0), to_mini(x1, y0), to_mini(x1, y1), to_mini(x0, y1),
                                  IM_COL32(col[0], col[1], col[2], 255));
            }
        }
        std::vector<render::Rgba> players;
        for (const auto& c : data_.engine.player_colors) {
            players.push_back(opaque(c));
        }
        std::vector<render::Rgba> nodes;
        for (const auto& n : data_.nodes.types) {
            nodes.push_back(opaque(n.color));
        }
        for (const MinimapDot& d : minimap_dots(curr_, view_player_, players, nodes)) {
            const auto x = static_cast<float>(d.origin.x);
            const auto y = static_cast<float>(d.origin.y);
            const auto s = static_cast<float>(std::max(d.size, 2));  // al menos 2 casillas: que se vea
            dl->AddQuadFilled(to_mini(x, y), to_mini(x + s, y), to_mini(x + s, y + s), to_mini(x, y + s),
                              IM_COL32(d.color[0], d.color[1], d.color[2], 255));
        }
        // Contorno del mapa (lo no explorado es casi negro).
        const auto mw = static_cast<float>(map.width());
        const auto mh = static_cast<float>(map.height());
        dl->AddQuad(to_mini(0.0f, 0.0f), to_mini(mw, 0.0f), to_mini(mw, mh), to_mini(0.0f, mh),
                    IM_COL32(120, 120, 130, 255));
        // Recuadro de la cámara.
        const render::Vec2 screen = renderer_.screen_size();
        const auto corner = [&](float sx, float sy) {
            const render::Vec2 t = proj_.world_to_tile(camera_.screen_to_world({sx, sy}));
            return to_mini(t.x, t.y);
        };
        dl->AddQuad(corner(0.0f, 0.0f), corner(screen.x, 0.0f), corner(screen.x, screen.y), corner(0.0f, screen.y),
                    IM_COL32(255, 255, 255, 200));
        ImGui::InvisibleButton("minimapa", {width, width * 0.5f});
        if (ImGui::IsItemActive()) {
            const ImVec2 m = ImGui::GetIO().MousePos;
            const float a = (m.x - o.x - width * 0.5f) / k;  // tx - ty
            const float b = (m.y - o.y) * 2.0f / k;          // tx + ty
            const render::Vec2 world = proj_.tile_to_world({(a + b) * 0.5f, (b - a) * 0.5f});
            camera_.origin = world - screen * 0.5f;
            camera_.clamp_to_map(proj_, map.width(), map.height(), screen);
        }
        ImGui::End();
    }

    [[nodiscard]] static const char* alert_text(AlertKind k) {
        switch (k) {
            case AlertKind::UnderAttack:
                return T("¡Te atacan!");
            case AlertKind::Fire:
                return T("¡Un edificio arde!");
            case AlertKind::Hunger:
                return T("Tus tropas pasan hambre");
            case AlertKind::NoFood:
                return T("Sin comida para las raciones");
            case AlertKind::UnitReady:
                return T("Unidad lista");
            case AlertKind::Rout:
                return T("¡Tus tropas huyen!");
            case AlertKind::Count:
                break;
        }
        return "";
    }

    // Avisos bajo la barra de recursos, el más reciente arriba.
    void draw_alerts(const ImVec2& display) {
        const auto shown = alerts_.shown(curr_.tick);
        if (shown.empty()) {
            return;
        }
        constexpr float kBelowBarPx = 48.0f;
        ImGui::SetNextWindowPos({display.x * 0.5f, kBelowBarPx}, ImGuiCond_Always, {0.5f, 0.0f});
        ImGui::Begin(T("Avisos"), nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground);
        for (const Alert& a : shown) {
            const bool urgent = a.kind != AlertKind::UnitReady;
            ImGui::TextColored(urgent ? kWarnColor : ImVec4{0.7f, 0.9f, 0.7f, 1.0f}, "%s", alert_text(a.kind));
        }
        ImGui::TextDisabled("%s", TF("{}: ir allí", key_label(KeyAction::LastAlert)).c_str());
        ImGui::End();
    }

    // Mensaje breve (partida guardada).
    void draw_notice(const ImVec2& display) {
        if (notice_.empty() || curr_.tick - notice_tick_ > static_cast<sim::Tick>(data_.engine.alerts.show_ticks)) {
            return;
        }
        ImGui::SetNextWindowPos({display.x * 0.5f, display.y * 0.25f}, ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::Begin(T("Aviso"), nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoInputs);
        ImGui::TextUnformatted(notice_.c_str());
        ImGui::End();
    }

    // Partida en red: charla, espera y fallo de la conexión.
    void draw_net(const ImVec2& display) {
        ImGui::SetNextWindowPos({kPanelMarginPx, display.y * 0.5f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({display.x * 0.3f, display.y * 0.25f}, ImGuiCond_FirstUseEver);
        ImGui::Begin(T("Charla"));
        draw_chat(*net_, data_, chat_input_);
        ImGui::End();
        if (net_waiting_) {
            ImGui::SetNextWindowPos({display.x * 0.5f, display.y * 0.35f}, ImGuiCond_Always, {0.5f, 0.5f});
            ImGui::Begin(T("Red"), nullptr,
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoInputs);
            ImGui::TextUnformatted(T("Esperando a los demás jugadores..."));
            ImGui::End();
        }
        if (net_->state() == LockstepState::Failed) {
            ImGui::SetNextWindowPos({display.x * 0.5f, display.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
            ImGui::Begin(T("Partida en red interrumpida"), nullptr,
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
            ImGui::TextUnformatted(net_->error().c_str());
            ImGui::TextUnformatted(T("La repetición se guarda hasta aquí."));
            if (ImGui::Button(T("Volver al menú"))) {
                back_to_menu_ = true;
            }
            ImGui::End();
        }
    }

    // F5: guarda la partida (su repetición hasta ahora) y lo dice en pantalla.
    void save_game() {
        const auto path = save_game_path(data_);
        if (auto ok = save_replay(path, recorder_->finish(*world_)); !ok) {
            spdlog::error("No se pudo guardar: {}", ok.error());
            notice_ = T("No se pudo guardar (detalles en rts.log)");
        } else {
            spdlog::info("Partida guardada en {}", path.string());
            notice_ = TF("Partida guardada: {}", path.filename().string());
        }
        notice_tick_ = curr_.tick;
    }

    // Espacio: la cámara al último aviso que tiene lugar.
    void jump_to_alert() {
        for (const Alert& a : alerts_.shown(curr_.tick)) {
            if (a.where.x >= 0) {
                const render::Vec2 world = proj_.tile_to_world(
                    {static_cast<float>(a.where.x) + 0.5f, static_cast<float>(a.where.y) + 0.5f});
                const render::Vec2 screen = renderer_.screen_size();
                camera_.origin = world - screen * 0.5f;
                camera_.clamp_to_map(proj_, world_->map().width(), world_->map().height(), screen);
                return;
            }
        }
    }

    // Velo de la noche: más opaco cuanto menos luz (sobre la escena, bajo los paneles).
    void draw_night() const {
        const std::int32_t night = data_.engine.world.vision.night_sight_percent;
        if (curr_.daylight_percent >= kPercent || night >= kPercent) {
            return;
        }
        const render::Rgba& c = data_.engine.view.night_color;
        const std::int32_t alpha = c[3] * (kPercent - curr_.daylight_percent) / (kPercent - night);
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        ImGui::GetBackgroundDrawList()->AddRectFilled({0.0f, 0.0f}, display,
                                                      IM_COL32(c[0], c[1], c[2], std::clamp(alpha, 0, 255)));
    }

    void draw_unit_labels() {
        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        constexpr ImU32 kBack = IM_COL32(0, 0, 0, 170);
        constexpr ImU32 kBackWarn = IM_COL32(170, 20, 20, 220);  // con hambre o sin munición
        constexpr ImU32 kBackRout = IM_COL32(200, 160, 0, 230);  // en desbandada
        constexpr ImU32 kText = IM_COL32(255, 255, 255, 255);
        constexpr float kPadPx = 1.0f;
        const auto radius = static_cast<float>(data_.engine.view.marker_radius_px);
        for (std::size_t i = 0; i < screen_entities_.size(); ++i) {
            const sim::SnapshotEntity& se = curr_.entities[screen_index_[i]];
            const std::string& label = data_.units.types[se.type].label;
            const bool warn = se.owner == local_ && (se.hungry || out_of_ammo(se));
            if (art_ && !warn && !se.routing) {
                continue;  // con arte, la figura ya dice qué es: la inicial solo avisa
            }
            const ImVec2 size = ImGui::CalcTextSize(label.c_str());
            const render::Vec2 p = screen_entities_[i].pos;
            const ImVec2 at{std::round(p.x + radius + kPadPx * 2.0f), std::round(p.y - size.y * 0.5f)};
            const ImU32 back = se.routing ? kBackRout : (warn ? kBackWarn : kBack);
            draw->AddRectFilled({at.x - kPadPx, at.y}, {at.x + size.x + kPadPx, at.y + size.y}, back);
            draw->AddText(at, kText, label.c_str());
        }
    }

    // Reproductor: tiempo, pausa, velocidad y estado de la verificación en curso.
    void draw_replay_panel(const ImVec2& display) {
        if (!player_) {
            return;
        }
        ImGui::SetNextWindowPos({display.x * 0.5f, display.y - kPanelMarginPx}, ImGuiCond_FirstUseEver, {0.5f, 1.0f});
        ImGui::Begin(T("Repetición"), nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        const auto clock_text = [](sim::Tick t) {
            const auto seconds = t / static_cast<sim::Tick>(sim::kTicksPerSecond);
            constexpr sim::Tick kSecondsPerMinute = 60;
            return std::format("{}:{:02}", seconds / kSecondsPerMinute, seconds % kSecondsPerMinute);
        };
        ImGui::Text("%s / %s", clock_text(world_->tick()).c_str(), clock_text(replay_end_).c_str());
        ImGui::SameLine();
        if (ImGui::Button(paused_ ? T("Reanudar") : T("Pausa"))) {
            paused_ = !paused_;
        }
        const auto& speeds = data_.engine.replay.speeds;
        for (std::size_t i = 0; i < speeds.size(); ++i) {
            ImGui::SameLine();
            const std::string label = std::format("x{}", speeds[i]);
            if (ImGui::RadioButton(label.c_str(), speed_index_ == i)) {
                speed_index_ = i;
            }
        }
        // Vista: todo, o lo que veía cada jugador (niebla de guerra).
        ImGui::TextUnformatted(T("Vista:"));
        ImGui::SameLine();
        if (ImGui::RadioButton(T("todo"), !view_player_)) {
            view_player_.reset();
        }
        for (std::size_t p = 0; p < curr_.players.size(); ++p) {
            ImGui::SameLine();
            const auto id = static_cast<sim::PlayerId>(p);
            if (ImGui::RadioButton(TF("jugador {}", p).c_str(), view_player_ == id)) {
                view_player_ = id;
            }
        }
        if (player_->diverged()) {
            ImGui::TextColored({1.0f, 0.35f, 0.3f, 1.0f}, T("Divergencia en el tick %u: la partida ya no es la grabada"),
                               player_->diverged_at());
        } else {
            ImGui::TextDisabled(T("%zu comprobaciones de hash correctas%s"), player_->checkpoints_checked(),
                                player_->finished(*world_) ? T(" · fin") : "");
        }
        ImGui::TextDisabled(T("Espacio: pausa · 1-%zu: velocidad · sin órdenes"), speeds.size());
        ImGui::End();
    }

    // Objetivos del escenario (F4): los propios, con su estado, y los plazos de los demás.
    void draw_objectives(const ImVec2& display) {
        const auto& objectives = data_.engine.world.scenario.objectives;
        if (objectives.empty() || curr_.objectives.size() != objectives.size()) {
            return;
        }
        constexpr float kMargin = 10.0f;
        constexpr float kTop = 40.0f;  // debajo de la barra de recursos
        ImGui::SetNextWindowPos({display.x - kMargin, kTop}, ImGuiCond_Always, {1.0f, 0.0f});
        ImGui::Begin(T("Objetivos"), nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing);
        for (std::size_t i = 0; i < objectives.size(); ++i) {
            const sim::Objective& o = objectives[i];
            const std::string text = i < data_.objective_texts.size() ? data_.objective_texts[i] : std::string();
            if (o.player == local_) {
                const sim::ObjectiveStatus s = curr_.objectives[i];
                const ImVec4 color = s == sim::ObjectiveStatus::Done     ? ImVec4{0.4f, 1.0f, 0.4f, 1.0f}
                                     : s == sim::ObjectiveStatus::Failed ? ImVec4{1.0f, 0.35f, 0.3f, 1.0f}
                                                                        : ImVec4{1.0f, 1.0f, 1.0f, 1.0f};
                const char* mark = s == sim::ObjectiveStatus::Done ? "[x]" : s == sim::ObjectiveStatus::Failed ? "[!]" : "[ ]";
                ImGui::TextColored(color, "%s %s", mark, text.c_str());
            } else if (o.kind == sim::ObjectiveKind::Survive && curr_.objectives[i] == sim::ObjectiveStatus::Pending) {
                const sim::Tick left = o.ticks > curr_.tick ? o.ticks - curr_.tick : 0;
                const sim::Tick secs = left / static_cast<sim::Tick>(sim::kTicksPerSecond);
                constexpr sim::Tick kSecondsPerMinute = 60;
                ImGui::TextColored({1.0f, 0.8f, 0.3f, 1.0f}, T("%s · quedan %u:%02u"), text.c_str(), secs / kSecondsPerMinute,
                                   secs % kSecondsPerMinute);
            }
        }
        ImGui::End();
    }

    // Informe del escenario (F4) antes de empezar.
    void draw_briefing(const ImVec2& display) {
        if (!briefing_open_) {
            return;
        }
        constexpr float kWidthShare = 0.5f;
        ImGui::SetNextWindowPos({display.x * 0.5f, display.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::SetNextWindowSize({display.x * kWidthShare, 0.0f}, ImGuiCond_Always);
        ImGui::SetNextWindowFocus();
        ImGui::Begin(data_.scenario_name.empty() ? T("Informe") : data_.scenario_name.c_str(), nullptr,
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);
        ImGui::TextWrapped("%s", data_.scenario_briefing.c_str());
        if (ImGui::Button(T("Empezar"))) {
            briefing_open_ = false;
        }
        ImGui::End();
    }

    // Victoria o derrota: el jugador local sin unidades ni edificios pierde; si todos los
    // demás que llegaron a tener algo lo han perdido todo, gana.
    // Con objetivos (F4), además: gana quien los cumple todos; pierde quien falla uno de
    // conservar o ve ganar a otro.
    void draw_outcome(const ImVec2& display) {
        if (curr_.players.size() <= local_) {
            return;
        }
        bool won = curr_.winner >= 0 && std::cmp_equal(curr_.winner, local_);
        bool lost = curr_.players[local_].defeated || (curr_.winner >= 0 && !won);
        const auto& objectives = data_.engine.world.scenario.objectives;
        for (std::size_t i = 0; i < objectives.size() && i < curr_.objectives.size(); ++i) {
            lost = lost || (objectives[i].player == local_ && curr_.objectives[i] == sim::ObjectiveStatus::Failed);
        }
        if (!lost && !won) {
            bool any_rival = false;
            bool all_defeated = true;
            for (std::size_t p = 0; p < curr_.players.size(); ++p) {
                if (p == local_ || !curr_.players[p].started) {
                    continue;
                }
                any_rival = true;
                all_defeated = all_defeated && curr_.players[p].defeated;
            }
            won = any_rival && all_defeated;
        }
        if (!won && !lost) {
            return;
        }
        won_ = won && !lost;
        ImGui::SetNextWindowPos({display.x * 0.5f, display.y * 0.35f}, ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::Begin(T("Resultado"), nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove);
        constexpr float kOutcomeScale = 3.0f;  // letra del cartel: tres veces la normal
        ImGui::SetWindowFontScale(kOutcomeScale);
        ImGui::TextColored(won ? ImVec4{0.4f, 1.0f, 0.4f, 1.0f} : ImVec4{1.0f, 0.35f, 0.3f, 1.0f}, "%s",
                           won ? T("¡Victoria!") : T("Derrota"));
        ImGui::SetWindowFontScale(1.0f);
        draw_stats_table();
        if (ImGui::Button(T("Volver al menú"))) {
            back_to_menu_ = true;
        }
        ImGui::End();
    }

    // Estadísticas de la partida, por jugador.
    void draw_stats_table() const {
        const auto mins = curr_.tick / static_cast<sim::Tick>(sim::kTicksPerSecond * 60);
        ImGui::Text(T("Duración: %u min"), mins);
        if (!ImGui::BeginTable("estadisticas", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
            return;
        }
        for (const char* h : {TK("Jugador"), TK("Recogido"), TK("Entrenadas"), TK("Perdidas"), TK("Abatidos"),
                              TK("Edificios perdidos"), TK("Población máx.")}) {
            ImGui::TableSetupColumn(T(h));
        }
        ImGui::TableHeadersRow();
        for (std::size_t p = 0; p < curr_.players.size(); ++p) {
            const sim::PlayerStats& s = curr_.players[p].stats;
            std::int32_t gathered = 0;
            for (const std::int32_t v : s.gathered) {
                gathered += v;
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%zu%s", p, p == local_ ? T(" (tú)") : "");
            for (const std::int32_t v : {gathered, s.units_trained, s.units_lost, s.enemies_killed, s.buildings_lost,
                                         s.peak_population}) {
                ImGui::TableNextColumn();
                ImGui::Text("%d", v);
            }
        }
        ImGui::EndTable();
    }

    // Barra superior: recursos y población del jugador local.
    void draw_resource_bar(const ImVec2& display) {
        if (curr_.players.size() <= local_) {
            return;
        }
        const sim::PlayerState& ps = curr_.players[local_];
        ImGui::SetNextWindowPos({display.x * 0.5f, kPanelMarginPx}, ImGuiCond_Always, {0.5f, 0.0f});
        ImGui::Begin(T("Recursos"), nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove);
        for (std::size_t r = 0; r < sim::kResourceCount; ++r) {
            const render::Rgba& c = data_.engine.view.resource_colors[r];
            constexpr float kChannelMax = 255.0f;
            ImGui::TextColored({static_cast<float>(c[0]) / kChannelMax, static_cast<float>(c[1]) / kChannelMax,
                                static_cast<float>(c[2]) / kChannelMax, 1.0f},
                               "%s %d",
                               T(resource_key(static_cast<sim::Resource>(r)).data()), ps.stock[r]);
            ImGui::SameLine();
        }
        ImGui::Text(T("· población %d/%d"), ps.population, ps.population_cap);
        ImGui::End();
    }

    // Panel inferior: menú de construcción con aldeanos seleccionados, o la cola del
    // edificio seleccionado.
    void draw_selection_panel(const ImVec2& display) {
        // Anclado siempre por la esquina inferior izquierda: crece hacia arriba.
        ImGui::SetNextWindowPos({kPanelMarginPx, display.y - kPanelMarginPx}, ImGuiCond_Always, {0.0f, 1.0f});
        ImGui::Begin(T("Selección"), nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        const sim::Stock stock =
            curr_.players.size() > local_ ? curr_.players[local_].stock : sim::Stock{};
        if (selected_building_) {
            draw_building_panel(*selected_building_, stock);
        } else if (!selection_.selected().empty()) {
            std::int32_t workers = 0;
            for (const std::uint32_t id : selection_.selected()) {
                const auto it = std::ranges::find(curr_.entities, id, &sim::SnapshotEntity::id);
                if (it != curr_.entities.end() && data_.units.types[it->type].type.worker) {
                    ++workers;
                    if (selection_.selected().size() == 1) {
                        ImGui::Text(T("Aldeano: %s, lleva %d de %s"), task_name(it->task), it->carried,
                                    T(resource_key(it->carry_kind).data()));
                    }
                }
            }
            ImGui::Text(T("%zu unidades seleccionadas (%d aldeanos)"), selection_.selected().size(), workers);
            std::int32_t hungry = 0;
            std::int32_t no_ammo = 0;
            for (const std::uint32_t id : selection_.selected()) {
                const auto it = std::ranges::find(curr_.entities, id, &sim::SnapshotEntity::id);
                if (it != curr_.entities.end()) {
                    hungry += it->hungry ? 1 : 0;
                    no_ammo += out_of_ammo(*it) ? 1 : 0;
                }
            }
            if (hungry > 0 || no_ammo > 0) {
                ImGui::TextColored(kWarnColor, T("%d con hambre · %d sin munición: llévalas junto a un edificio que abastezca"),
                                   hungry, no_ammo);
            }
            if (selection_.selected().size() == 1) {
                const auto it = std::ranges::find(curr_.entities, selection_.selected().front(), &sim::SnapshotEntity::id);
                if (it != curr_.entities.end()) {
                    const UnitInfo& u = data_.units.types[it->type];
                    ImGui::Text(T("%s · vida %d/%d · nivel %d (%d de experiencia)"), N(u.name), it->hp, it->max_hp,
                                it->level, it->xp);
                    draw_supply_text(*it);
                    draw_care_text(*it);
                    if (u.type.convoy_capacity > 0) {
                        ImGui::Text(T("carga %s (de %d)"), stock_text(it->load).c_str(), u.type.convoy_capacity);
                        ImGui::TextDisabled("%s", convoy_text(it->convoy));
                        ImGui::TextDisabled("%s", T("Clic derecho en un campamento: ruta de convoy; en otro almacén: cargar"));
                    }
                    if (it->hero_name >= 0 && !data_.engine.hero_names.empty()) {
                        const auto n = static_cast<std::size_t>(it->hero_name) % data_.engine.hero_names.size();
                        ImGui::TextColored({1.0f, 0.8f, 0.2f, 1.0f}, T("Héroe: %s"), data_.engine.hero_names[n].c_str());
                    }
                }
            }
            if (ImGui::Button(T("Agresiva"))) {
                sim::Command c = local_command(sim::CommandType::SetStance);
                c.kind = static_cast<std::uint8_t>(sim::Stance::Aggressive);
                issue(std::move(c));
            }
            ImGui::SameLine();
            if (ImGui::Button(T("Mantener posición"))) {
                sim::Command c = local_command(sim::CommandType::SetStance);
                c.kind = static_cast<std::uint8_t>(sim::Stance::HoldGround);
                issue(std::move(c));
            }
            if (world_->fatigue().enabled()) {
                if (ImGui::Button(T("Paso normal"))) {
                    sim::Command c = local_command(sim::CommandType::SetStance);
                    c.kind = sim::kPaceNormal;
                    issue(std::move(c));
                }
                ImGui::SameLine();
                if (ImGui::Button(T("Paso forzado"))) {
                    sim::Command c = local_command(sim::CommandType::SetStance);
                    c.kind = sim::kPaceForced;
                    issue(std::move(c));
                }
            }
            if (world_->formation().enabled()) {
                constexpr std::array<std::pair<const char*, sim::FormationKind>, 4> kForms{{
                    {TK("Sin formación"), sim::FormationKind::None},
                    {TK("Línea"), sim::FormationKind::Line},
                    {TK("Columna"), sim::FormationKind::Column},
                    {TK("Cuadro"), sim::FormationKind::Square},
                }};
                for (std::size_t i = 0; i < kForms.size(); ++i) {
                    if (i > 0) {
                        ImGui::SameLine();
                    }
                    if (ImGui::Button(T(kForms[i].first))) {
                        sim::Command c = local_command(sim::CommandType::SetStance);
                        c.kind = static_cast<std::uint8_t>(sim::kFormationBase + static_cast<std::uint8_t>(kForms[i].second));
                        issue(std::move(c));
                    }
                }
            }
            if (workers > 0) {
                ImGui::SeparatorText(T("Construir"));
                for (std::size_t b = 0; b < data_.buildings.types.size(); ++b) {
                    const BuildingInfo& info = data_.buildings.types[b];
                    const auto type = static_cast<sim::BuildingTypeId>(b);
                    const bool allowed = world_->meets_requirements(local_, type);
                    ImGui::BeginDisabled(!allowed || !affordable(stock, info.type.cost));
                    if (ImGui::Button(std::format("{} ({})", info.name, cost_text(info.type.cost)).c_str())) {
                        placing_ = type;
                    }
                    ImGui::EndDisabled();
                    if (!allowed) {
                        std::string needs;
                        for (const sim::BuildingTypeId r : info.type.required) {
                            needs += (needs.empty() ? "" : ", ") + data_.buildings.types[r].name;
                        }
                        ImGui::SameLine();
                        ImGui::TextDisabled(T("requiere %s"), needs.c_str());
                    }
                }
                if (placing_) {
                    ImGui::TextDisabled("%s", T("Clic: colocar · Mayús+clic: varios · clic derecho o Esc: cancelar"));
                }
            }
        } else {
            ImGui::TextDisabled("%s", T("Nada seleccionado"));
        }
        ImGui::End();
    }

    void draw_building_panel(std::uint32_t id, const sim::Stock& stock) {
        const sim::SnapshotObject* o = find_object(id);
        if (o == nullptr) {
            selected_building_.reset();  // destruido
            return;
        }
        const BuildingInfo& info = data_.buildings.types[o->type];
        ImGui::Text(T("%s · vida %d/%d"), N(info.name), o->hp, info.type.hp);
        if (info.type.beds > 0) {
            std::int32_t patients = 0;
            std::int32_t nurses = 0;
            for (const sim::SnapshotEntity& e : curr_.entities) {
                patients += e.admitted && e.care_post == id ? 1 : 0;
            }
            // Personal asignado (enfermeros y cirujanos; cuidan los que están a su lado).
            for (const sim::SnapshotEntity& e : curr_.entities) {
                nurses += e.owner == local_ && e.tending && e.work_building == id ? 1 : 0;
            }
            ImGui::Text(T("camas %d/%d · personal %d/%d · cura hasta el %d %%"), patients, info.type.beds, nurses,
                        info.type.nurses, info.type.heal_to_percent);
            ImGui::TextDisabled("%s", T("Clic derecho con heridos: ingresan; con aldeanos o cirujanos: atienden"));
        }
        if (info.type.store_capacity > 0) {
            ImGui::Text(T("suministros %s (de %d)"), stock_text(o->store).c_str(), info.type.store_capacity);
            ImGui::TextDisabled("%s", T("Se llena con acémilas o carretas: clic derecho sobre él con ellas"));
        } else if (info.type.supplies) {
            ImGui::TextDisabled("%s", T("Abastece a las tropas cercanas con víveres y munición"));
        }
        if (o->fire > 0) {
            ImGui::TextColored({1.0f, 0.5f, 0.1f, 1.0f}, T("En llamas (%d %%): clic derecho con unidades para apagarlo"),
                               o->fire * kPercent / data_.engine.world.fire.max_intensity);
        } else if (o->burned) {
            ImGui::TextColored({0.8f, 0.6f, 0.4f, 1.0f}, "%s", T("Quemado: no funciona hasta que lo reparen aldeanos"));
        } else if (o->complete && o->hp < info.type.hp) {
            ImGui::TextDisabled("%s", T("Dañado: clic derecho con aldeanos para repararlo (cuesta madera)"));
        }
        ImGui::TextDisabled("%s", T("Mayús + clic derecho con aldeanos: desmontarlo (deja escombros)"));
        if (!o->complete) {
            ImGui::ProgressBar(static_cast<float>(o->progress) / static_cast<float>(info.type.build_ticks), {-1.0f, 0.0f},
                               T("en obra"));
            return;
        }
        draw_research(*o, id);
        draw_market(*o, id);
        if (info.type.garrison > 0) {
            ImGui::Text(T("Guarnición: %d de %d"), o->garrison, info.type.garrison);
            ImGui::TextDisabled("%s", T("Clic derecho con tropas: guarnecerla (dentro no se las puede atacar)"));
            if (o->garrison > 0 && ImGui::SmallButton(T("Vaciar la torre"))) {
                sim::Command c = local_command(sim::CommandType::Garrison);
                c.units.clear();
                c.object = id;
                c.kind = sim::kUngarrison;
                issue(std::move(c));
            }
        }
        if (info.type.trains.empty()) {
            return;
        }
        if (o->rally.x >= 0) {
            ImGui::Text(T("Punto de reunión: (%d, %d)"), o->rally.x, o->rally.y);
            ImGui::SameLine();
            if (ImGui::SmallButton(T("Quitar"))) {
                sim::Command c = local_command(sim::CommandType::SetRally);
                c.units.clear();
                c.object = id;
                c.kind = sim::kClearRally;
                issue(std::move(c));
            }
        } else {
            ImGui::TextDisabled("%s", T("Clic derecho en el mapa: punto de reunión"));
        }
        ImGui::SeparatorText(T("Producción"));
        for (std::size_t i = 0; i < o->queue.size(); ++i) {
            const UnitInfo& u = data_.units.types[o->queue[i]];
            if (i == 0) {
                ImGui::ProgressBar(static_cast<float>(o->queue_progress) / static_cast<float>(u.type.train_ticks),
                                   {-1.0f, 0.0f}, N(u.name));
            } else {
                ImGui::BulletText("%s", N(u.name));
            }
        }
        const bool full = std::cmp_greater_equal(o->queue.size(), data_.engine.world.economy.queue_capacity);
        // Un campamento paga con su propio almacén (lo traído en convoy).
        const sim::Stock& pays = info.type.store_capacity > 0 ? o->store : stock;
        if (info.type.store_capacity > 0) {
            ImGui::TextDisabled("%s", T("Se paga con el almacén del campamento"));
        }
        for (const sim::UnitTypeId t : info.type.trains) {
            const UnitInfo& u = data_.units.types[t];
            ImGui::BeginDisabled(full || !affordable(pays, u.type.cost));
            if (ImGui::Button(std::format("{} ({})", u.name, cost_text(u.type.cost)).c_str())) {
                sim::Command c = local_command(sim::CommandType::Train);
                c.units.clear();
                c.object = id;
                c.kind = t;
                issue(std::move(c));
            }
            ImGui::EndDisabled();
        }
        ImGui::BeginDisabled(o->queue.empty());
        if (ImGui::Button(T("Cancelar la última (reembolso íntegro)"))) {
            sim::Command c = local_command(sim::CommandType::CancelTrain);
            c.units.clear();
            c.object = id;
            issue(std::move(c));
        }
        ImGui::EndDisabled();
    }

    // Edificios y recursos del snapshot, más el fantasma de colocación. Los rombos de
    // huellas distintas nunca se solapan, así que no hace falta ordenarlos.
    void build_objects(const std::optional<sim::TileCoord>& hover) {
        const render::ViewParams& view = data_.engine.view;
        objects_.clear();
        refresh_remembered();
        const auto add_object = [&](const sim::SnapshotObject& o, bool remembered) {
            render::SceneObject so = scene_object(o);
            if (remembered) {
                so.base = shaded(so.base, view.fog_explored_shade_percent);
                so.body = shaded(so.body, view.fog_explored_shade_percent);
                so.tint = shaded(so.tint, view.fog_explored_shade_percent);
                so.fire_permille = 0;  // lo recordado no arde a la vista
            }
            objects_.push_back(so);
        };
        for (const sim::SnapshotObject& o : curr_.objects) {
            if (shows_object(o)) {
                add_object(o, false);
            }
        }
        for (const sim::SnapshotObject& o : remembered_objects_) {
            add_object(o, true);
        }
        if (placing_ && hover) {
            const BuildingInfo& info = data_.buildings.types[*placing_];
            const sim::TileCoord origin = ghost_origin(*hover, *placing_);
            const bool can_pay = curr_.players.size() > local_ &&
                                 affordable(curr_.players[local_].stock, info.type.cost);
            render::SceneObject ghost;
            ghost.origin = origin;
            ghost.size = info.type.size;
            ghost.body = can_pay && world_->can_place(*placing_, origin) ? view.ghost_valid_color : view.ghost_invalid_color;
            objects_.push_back(ghost);
            if (art_) {
                // El edificio mismo, translúcido y teñido de verde o rojo, sobre la huella.
                render::SceneObject shape = ghost;
                shape.art = render::SceneObject::Art::Building;
                shape.art_type = *placing_;
                shape.tint = ghost.body;
                objects_.push_back(shape);
            }
        }
    }

    [[nodiscard]] render::SceneObject scene_object(const sim::SnapshotObject& o) const {
        const render::ViewParams& view = data_.engine.view;
        {
            render::SceneObject so;
            so.origin = o.origin;
            so.size = o.size;
            if (o.kind == sim::ObjectKind::Resource) {
                so.body = opaque(data_.nodes.types[o.type].color);
                so.body_percent = view.node_body_percent;
                if (art_) {
                    so.art = render::SceneObject::Art::Node;
                    so.art_type = o.type;
                    // Variante fija por casilla: el bosque no cambia al moverse la cámara.
                    constexpr std::uint32_t kVariantMask = 0x7FFF;
                    so.variant = static_cast<std::int32_t>(
                        (static_cast<std::uint32_t>(o.origin.x) * 73856093U ^ static_cast<std::uint32_t>(o.origin.y) * 19349663U) &
                        kVariantMask);
                }
            } else {
                if (art_) {
                    const sim::BuildingType& type = data_.buildings.types[o.type].type;
                    so.art = render::SceneObject::Art::Building;
                    so.art_type = o.type;
                    if (o.owner < data_.engine.player_colors.size()) {
                        so.team = opaque(data_.engine.player_colors[o.owner]);
                    }
                    if (!o.complete && type.build_ticks > 0) {
                        so.build_permille = std::clamp(o.progress * kPermille / type.build_ticks, 0, kPermille);
                    }
                    if (o.burned) {
                        so.tint = shaded(so.tint, view.burned_shade_percent);
                    }
                    if (o.fire > 0 && data_.engine.world.fire.max_intensity > 0) {
                        so.fire_permille = o.fire * kPermille / data_.engine.world.fire.max_intensity;
                    }
                }
                so.base = o.owner < data_.engine.player_colors.size() ? opaque(data_.engine.player_colors[o.owner])
                                                                      : render::Rgba{};
                so.body = opaque(data_.buildings.types[o.type].color);
                if (!o.complete) {
                    so.body = shaded(so.body, view.construction_shade_percent);
                }
                if (o.burned) {
                    so.body = shaded(so.body, view.burned_shade_percent);
                }
                if (o.fire > 0) {
                    // Hacia el color del fuego en proporción a su intensidad.
                    const std::int32_t t = o.fire * kPercent / data_.engine.world.fire.max_intensity;
                    for (std::size_t i = 0; i < 3; ++i) {
                        so.body[i] = static_cast<std::uint8_t>((so.body[i] * (kPercent - t) + view.fire_color[i] * t) /
                                                               kPercent);
                    }
                }
                so.body_percent = view.building_body_percent;
                so.highlighted = selected_building_ == o.id;
                const std::int32_t max_hp = data_.buildings.types[o.type].type.hp;
                if (o.complete && max_hp > 0 && o.hp < max_hp) {
                    so.health_permille = o.hp * kPermille / max_hp;
                }
            }
            return so;
        }
    }

    // --- Editor de escenarios (F3) ---------------------------------------------------

    void rebuild_world() {
        edit_params_.scenario = editor_->params;
        world_ = std::make_unique<sim::World>(edit_params_);
        world_->write_snapshot(curr_);
        prev_ = curr_;
        remembered_objects_.clear();
    }

    [[nodiscard]] std::optional<sim::TileCoord> mouse_tile(float x, float y) const {
        const sim::TileCoord t = tile_at({x, y});
        return world_->map().contains(t) ? std::optional(t) : std::nullopt;
    }

    // Pincel (terreno y altura) en una casilla nueva del trazo.
    void edit_stroke(sim::TileCoord t) {
        if (edit_last_ == t) {
            return;
        }
        edit_last_ = t;
        sim::ScenarioParams& sp = editor_->params;
        if (edit_tool_ == EditTool::Terrain) {
            paint_terrain(sp, t, edit_radius_ - 1, static_cast<sim::TerrainId>(edit_type_));
        } else {
            raise(sp, t, edit_radius_ - 1, edit_lowering_ ? -1 : 1, data_.engine.world.map.elevation_levels - 1);
        }
        edit_stroke_.push_back(t);
    }

    // Colocar en la casilla t lo que dice la herramienta, si cabe.
    void edit_place(sim::TileCoord t) {
        sim::ScenarioParams& sp = editor_->params;
        sim::ScenarioPlacement p;
        p.type = static_cast<std::uint8_t>(edit_type_);
        p.owner = static_cast<sim::PlayerId>(std::min(edit_player_, sp.players - 1));
        bool fits = false;
        switch (edit_tool_) {
            case EditTool::Building: {
                p.kind = sim::ScenarioPlacement::Kind::Building;
                p.at = ghost_origin(t, p.type);
                fits = world_->can_place(p.type, p.at);
                break;
            }
            case EditTool::Node: {
                p.kind = sim::ScenarioPlacement::Kind::Node;
                const std::int32_t size = data_.nodes.types[p.type].type.size;
                p.at = {t.x - size / 2, t.y - size / 2};
                fits = world_->can_place_size(size, p.at);
                break;
            }
            case EditTool::Unit:
                p.kind = sim::ScenarioPlacement::Kind::Unit;
                p.at = t;
                fits = world_->movement().grid().passable(t);
                break;
            case EditTool::Terrain:
            case EditTool::Height:
            case EditTool::Erase:
                return;
        }
        if (!fits) {
            edit_status_ = T("No cabe ahí");
            return;
        }
        sp.placements.push_back(p);
        edit_status_.clear();
        rebuild_world();
    }

    // Eventos del ratón en el editor; true si los ha consumido.
    bool edit_event(const SDL_Event& event, bool ui_mouse) {
        switch (event.type) {
            case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                if (ui_mouse) {
                    return false;
                }
                const auto t = mouse_tile(event.button.x, event.button.y);
                if (!t) {
                    return true;
                }
                if (event.button.button == SDL_BUTTON_RIGHT && edit_tool_ != EditTool::Height) {
                    if (erase_at(editor_->params, *t, data_)) {
                        rebuild_world();
                    }
                    return true;
                }
                if (edit_tool_ == EditTool::Terrain || edit_tool_ == EditTool::Height) {
                    edit_painting_ = true;
                    edit_lowering_ = event.button.button == SDL_BUTTON_RIGHT;
                    edit_last_.reset();
                    edit_stroke(*t);
                } else if (edit_tool_ == EditTool::Erase) {
                    if (erase_at(editor_->params, *t, data_)) {
                        rebuild_world();
                    }
                } else if (event.button.button == SDL_BUTTON_LEFT) {
                    edit_place(*t);
                }
                return true;
            }
            case SDL_EVENT_MOUSE_MOTION:
                if (edit_painting_) {
                    if (const auto t = mouse_tile(event.motion.x, event.motion.y)) {
                        edit_stroke(*t);
                    }
                    return true;
                }
                return false;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (edit_painting_) {
                    edit_painting_ = false;
                    edit_stroke_.clear();
                    rebuild_world();  // al soltar: el mundo se rehace una vez por trazo
                    return true;
                }
                return false;
            default:
                return false;
        }
    }

    void draw_editor(const ImVec2& display, const std::optional<sim::TileCoord>& hover) {
        sim::ScenarioParams& sp = editor_->params;
        if (edit_name_[0] == '\0') {
            std::ranges::copy(editor_->name.substr(0, edit_name_.size() - 1), edit_name_.begin());
        }
        ImGui::SetNextWindowPos({display.x - kPanelMarginPx, kPanelMarginPx * 5.0f}, ImGuiCond_FirstUseEver, {1.0f, 0.0f});
        ImGui::Begin(T("Editor de escenarios"), nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::InputText(T("Nombre"), edit_name_.data(), edit_name_.size());
        int players = sp.players;
        const auto max_players = static_cast<int>(data_.engine.seat_starts.size());
        if (ImGui::SliderInt(T("Jugadores"), &players, 1, max_players)) {
            sp.players = players;
            // Lo de jugadores que ya no están, fuera.
            std::erase_if(sp.placements, [&](const sim::ScenarioPlacement& p) {
                return p.kind != sim::ScenarioPlacement::Kind::Node && p.owner >= players;
            });
            rebuild_world();
        }
        ImGui::SeparatorText(T("Herramienta"));
        constexpr std::array<const char*, 6> kTools{TK("Terreno"), TK("Altura"), TK("Recurso"), TK("Edificio"), TK("Unidad"), TK("Borrar")};
        for (std::size_t i = 0; i < kTools.size(); ++i) {
            if (ImGui::RadioButton(T(kTools[i]), edit_tool_ == static_cast<EditTool>(i))) {
                edit_tool_ = static_cast<EditTool>(i);
                edit_type_ = 0;
            }
            if (i % 3 != 2) {
                ImGui::SameLine();
            }
        }
        const auto type_list = [&](const auto& types) {
            const std::size_t current = std::min<std::size_t>(static_cast<std::size_t>(edit_type_), types.size() - 1);
            if (ImGui::BeginCombo(T("Tipo"), N(types[current].name))) {
                for (std::size_t i = 0; i < types.size(); ++i) {
                    if (ImGui::Selectable(N(types[i].name), i == current)) {
                        edit_type_ = static_cast<int>(i);
                    }
                }
                ImGui::EndCombo();
            }
        };
        switch (edit_tool_) {
            case EditTool::Terrain:
                type_list(data_.terrain.types);
                ImGui::SliderInt(T("Pincel"), &edit_radius_, 1, kMaxBrush);
                ImGui::TextDisabled("%s", T("Arrastrar: pintar"));
                break;
            case EditTool::Height:
                ImGui::SliderInt(T("Pincel"), &edit_radius_, 1, kMaxBrush);
                ImGui::TextDisabled("%s", T("Izquierdo: subir; derecho: bajar"));
                break;
            case EditTool::Node:
                type_list(data_.nodes.types);
                break;
            case EditTool::Building:
            case EditTool::Unit:
                if (edit_tool_ == EditTool::Building) {
                    type_list(data_.buildings.types);
                } else {
                    type_list(data_.units.types);
                }
                edit_player_ = std::min(edit_player_, sp.players - 1);
                ImGui::SliderInt(T("Jugador"), &edit_player_, 0, sp.players - 1);
                break;
            case EditTool::Erase:
                ImGui::TextDisabled("%s", T("Clic: quitar lo que hay"));
                break;
        }
        ImGui::TextDisabled("%s", T("Derecho (salvo altura): quitar"));
        // Siempre la misma línea: que los botones no se muevan al pasar sobre el panel.
        if (hover) {
            ImGui::Text(T("Casilla %d, %d"), hover->x, hover->y);
        } else {
            ImGui::TextDisabled("%s", T("Casilla -"));
        }
        ImGui::Separator();
        if (ImGui::Button(T("Guardar"))) {
            editor_->name = edit_name_.data();
            edit_status_ = save_scenario();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Probar"))) {
            editor_->name = edit_name_.data();
            play_requested_ = true;
            back_to_menu_ = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Salir"))) {
            back_to_menu_ = true;
        }
        if (!edit_status_.empty()) {
            ImGui::TextUnformatted(edit_status_.c_str());
        }
        ImGui::End();
        // El trazo en curso, teñido (el mundo se rehace al soltar).
        for (const sim::TileCoord& t : edit_stroke_) {
            for (std::int32_t y = t.y - edit_radius_ + 1; y <= t.y + edit_radius_ - 1; ++y) {
                for (std::int32_t x = t.x - edit_radius_ + 1; x <= t.x + edit_radius_ - 1; ++x) {
                    if (std::abs(x - t.x) + std::abs(y - t.y) <= edit_radius_ - 1) {
                        tints_.push_back({{x, y}, data_.engine.view.ghost_valid_color});
                    }
                }
            }
        }
        // Fantasma del edificio a colocar.
        placing_.reset();
        if (edit_tool_ == EditTool::Building && hover) {
            placing_ = static_cast<sim::BuildingTypeId>(edit_type_);
        }
    }

    // Guarda en escenarios/<nombre>.toml, junto al ejecutable. Devuelve el mensaje.
    std::string save_scenario() {
        std::string file;
        for (const char c : editor_->name) {
            file += std::isalnum(static_cast<unsigned char>(c)) != 0 ? c : '_';
        }
        if (file.empty()) {
            file = "escenario";
        }
        const auto dir = std::filesystem::path(platform::executable_dir()) / kScenarioDir;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const auto path = dir / (file + ".toml");
        std::ofstream out(path, std::ios::binary);
        out << scenario_doc_toml(*editor_, data_);
        if (!out) {
            return T("No se pudo guardar (detalles en rts.log)");
        }
        spdlog::info("Escenario guardado en {}", path.string());
        return TF("Guardado: {}", path.filename().string());
    }

    // Pose de la figura: andando, el ciclo de pasos; combatiendo, según cuánto falta para
    // el golpe; trabajando, golpes de herramienta. Cada unidad, desfasada de las demás.
    [[nodiscard]] render::Pose pose_of(const sim::SnapshotEntity& se, bool moving) const {
        const render::ViewParams& view = data_.engine.view;
        constexpr double kMsPerS = 1000.0;
        constexpr std::uint32_t kPhaseSpread = 997;
        const double t_ms = anim_time_s_ * kMsPerS + static_cast<double>(se.id % kPhaseSpread) * view.walk_frame_ms / 3.0;
        if (moving) {
            constexpr std::array<render::Pose, 4> kWalk{render::Pose::StepA, render::Pose::Idle, render::Pose::StepB,
                                                        render::Pose::Idle};
            return kWalk[static_cast<std::size_t>(t_ms / view.walk_frame_ms) % kWalk.size()];
        }
        if (se.has_target && se.type < data_.units.types.size()) {
            const std::int32_t reload = data_.units.types[se.type].type.combat.reload_ticks;
            if (se.cooldown > reload - view.attack_strike_ticks) {
                return render::Pose::Strike;
            }
            if (se.cooldown <= view.attack_windup_ticks) {
                return render::Pose::Act;
            }
            return render::Pose::Idle;
        }
        if (se.task == sim::WorkerTask::Gather || se.task == sim::WorkerTask::Build ||
            se.task == sim::WorkerTask::Demolish) {
            return static_cast<std::int64_t>(t_ms / view.work_frame_ms) % 2 == 0 ? render::Pose::Act
                                                                                  : render::Pose::Strike;
        }
        return render::Pose::Idle;
    }

    void present() {
        build_screen_entities();
        selection_.retain(own_screen_entities_);
        renderer_.begin_frame();

        markers_.clear();
        const render::Vec2 feet{0.0f, art_ ? static_cast<float>(data_.engine.view.unit_pick_lift_px) : 0.0f};
        for (std::size_t i = 0; i < screen_entities_.size(); ++i) {
            const ScreenEntity& e = screen_entities_[i];
            const sim::SnapshotEntity& se = curr_.entities[screen_index_[i]];
            render::Marker m;
            m.screen_pos = e.pos + feet;
            if (art_) {
                m.unit_type = se.type;
                m.pose = pose_of(se, moving_[screen_index_[i]]);
                work_sound(se, m.pose, m.screen_pos);
                const auto f = facing_left_.find(se.id);
                m.mirror = f != facing_left_.end() && f->second;
            }
            m.selected = selection_.is_selected(e.id);
            m.color = opaque(data_.units.types[se.type].color);
            if (se.owner < data_.engine.player_colors.size()) {
                m.owner = opaque(data_.engine.player_colors[se.owner]);
            }
            if (se.carried > 0) {
                m.badge = data_.engine.view.resource_colors[sim::resource_index(se.carry_kind)];
            }
            if (se.max_hp > 0 && (se.hp < se.max_hp || m.selected)) {
                m.health_permille = se.hp * kPermille / se.max_hp;
            }
            m.hero = se.hero_name >= 0;
            markers_.push_back(m);
        }
        build_overlays();
        const std::optional<sim::TileCoord> hover = hovered_tile();
        draw_ui(hover);
        build_objects(hover);
        flush_sounds();

        render::Scene scene;
        scene.map = curr_.map.get();
        scene.camera = camera_;
        scene.screen = renderer_.screen_size();
        scene.objects = objects_;
        scene.markers = markers_;
        scene.hovered_tile = hover;
        scene.time_s = static_cast<float>(anim_time_s_);
        scene.tile_tints = tints_;
        scene.path_points = path_points_;
        if (const std::vector<std::uint8_t>* fog = view_fog()) {
            scene.fog = *fog;
        }
        projectile_points_.clear();
        for (const sim::Position& p : curr_.projectiles) {
            const render::Vec2 t{fixed_to_float(p.x), fixed_to_float(p.y)};
            projectile_points_.push_back(camera_.world_to_screen(proj_.tile_to_world(t)));
        }
        scene.projectiles = projectile_points_;
        if (selection_.has_visible_rect()) {
            const ScreenRect r = selection_.drag_rect();
            scene.drag_rect = render::Rect{r.min, r.max};
        }
        const auto scene_start = SteadyClock::now();
        stats_.scene = renderer_.draw_scene(scene);
        stats_.scene_ms += kSmoothing * (elapsed_ms(scene_start) - stats_.scene_ms);
        const auto submit_start = SteadyClock::now();
        renderer_.end_frame();
        stats_.submit_ms += kSmoothing * (elapsed_ms(submit_start) - stats_.submit_ms);
    }

    const GameData& data_;
    ScenarioDoc* editor_;  // editor de escenarios (F3); null: partida
    bool art_ = !data_.art.units.empty();  // arte propio (F1); sin él, formas planas
    // Estado del editor.
    enum class EditTool : std::uint8_t { Terrain, Height, Node, Building, Unit, Erase };
    EditTool edit_tool_ = EditTool::Terrain;
    int edit_type_ = 0;
    int edit_player_ = 0;
    int edit_radius_ = 1;
    bool edit_painting_ = false;
    bool edit_lowering_ = false;
    std::optional<sim::TileCoord> edit_last_;
    std::vector<sim::TileCoord> edit_stroke_;
    sim::WorldParams edit_params_;
    std::string edit_status_;
    std::array<char, 64> edit_name_{};
    bool play_requested_ = false;
    std::optional<SoundDirector> sounds_;  // F2; solo con audio y datos de sonido
    std::unordered_map<std::uint32_t, render::Pose> last_pose_;  // para oír cada golpe de herramienta
    double anim_time_s_ = 0.0;             // reloj de la presentación (animaciones)
    std::vector<bool> moving_;             // por índice de curr_.entities
    std::unordered_map<std::uint32_t, bool> facing_left_;
    LockstepSession* net_;  // partida en red (E2); null: partida local
    AudioSystem* audio_;    // null: sin sonido
    sim::PlayerId local_;   // jugador de esta máquina
    bool net_waiting_ = false;  // este fotograma faltaron órdenes de otros
    std::string chat_input_;
    platform::Window& window_;
    render::Renderer& renderer_;
    std::unique_ptr<sim::World> world_;  // se rehace al editar un escenario (F3)
    sim::Snapshot prev_;
    sim::Snapshot curr_;
    render::IsoProjection proj_;
    render::Camera camera_;
    FixedStepClock clock_;
    Selection selection_;
    std::vector<ScreenEntity> screen_entities_;
    std::vector<ScreenEntity> sorted_;
    std::vector<std::size_t> order_;
    std::vector<std::size_t> screen_index_;  // índice en curr_.entities de cada entidad ordenada
    std::vector<ScreenEntity> own_screen_entities_;
    std::vector<render::Marker> markers_;
    std::vector<render::SceneObject> objects_;
    std::optional<std::uint32_t> selected_building_;
    std::optional<sim::BuildingTypeId> placing_;
    std::vector<render::TileTint> tints_;
    std::vector<render::Vec2> path_points_;
    std::vector<render::Vec2> projectile_points_;
    std::optional<ReplayRecorder> recorder_;
    std::optional<CrashGuard> crash_guard_;  // después de recorder_ y world_: se va antes
    bool desync_reported_ = false;
    std::filesystem::path replay_path_;
    std::optional<ReplayPlayer> player_;
    sim::Tick replay_end_ = 0;
    bool paused_ = false;
    std::size_t speed_index_ = 0;
    // Jugador cuya vista se muestra (niebla de guerra); sin valor, todo.
    std::optional<sim::PlayerId> view_player_;
    std::vector<sim::SnapshotObject> remembered_objects_;  // edificios recordados, no vistos ahora
    std::array<std::vector<std::uint32_t>, 9> groups_;  // grupos de control 1…9
    AlertTracker alerts_{data_.engine.alerts};
    bool won_ = false;
    bool briefing_open_ = false;  // F4: informe del escenario al empezar (la partida espera)
    bool back_to_menu_ = false;   // F10 o el botón del final: volver al menú
    std::string notice_;          // mensaje breve en pantalla
    sim::Tick notice_tick_ = 0;
    bool show_debug_ = false;  // F1
    bool show_help_ = true;    // F2
    bool show_portals_ = false;
    bool show_paths_ = true;
    bool show_flow_ = false;
    FrameStats stats_;
};

}  // namespace

std::optional<LaunchOptions> parse_arguments(int argc, char** argv) {
    LaunchOptions options;
    options.data_dir = std::filesystem::path(platform::executable_dir()) / "data";

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--headless") {
            options.headless = true;
        } else if (arg == "--data" && has_value) {
            options.data_dir = argv[++i];
        } else if (arg == "--record" && has_value) {
            options.record = argv[++i];
        } else if (arg == "--replay" && has_value) {
            options.replay = argv[++i];
        } else if (arg == "--verify-replay" && has_value) {
            options.verify_replay = argv[++i];
        } else if ((arg == "--ticks" || arg == "--frames") && has_value) {
            const std::string_view value = argv[++i];
            std::int64_t& target = arg == "--ticks" ? options.headless_ticks : options.max_frames;
            if (!parse_count(value, target)) {
                spdlog::error("{} necesita un entero no negativo, recibido '{}'", arg, value);
                return std::nullopt;
            }
        } else if (arg == "--host" && has_value) {
            std::int64_t port = 0;
            if (!parse_count(argv[++i], port) || port > std::numeric_limits<std::uint16_t>::max()) {
                spdlog::error("--host necesita un puerto, recibido '{}'", argv[i]);
                return std::nullopt;
            }
            options.host_port = static_cast<std::uint16_t>(port);
        } else if (arg == "--players" && has_value) {
            std::int64_t n = 0;
            if (!parse_count(argv[++i], n) || n < 2 || n > std::numeric_limits<std::uint8_t>::max()) {
                spdlog::error("--players necesita un número de jugadores, recibido '{}'", argv[i]);
                return std::nullopt;
            }
            options.net_players = static_cast<std::uint8_t>(n);
        } else if (arg == "--join" && has_value) {
            if (!parse_address(argv[++i], options.join_host, options.join_port)) {
                spdlog::error("--join necesita <dirección>:<puerto>, recibido '{}'", argv[i]);
                return std::nullopt;
            }
        } else if (arg == "--report-dir" && has_value) {
            options.report_dir = argv[++i];
        } else if (arg == "--crash-at" && has_value) {
            if (!parse_count(argv[++i], options.crash_at)) {
                spdlog::error("--crash-at necesita un tick, recibido '{}'", argv[i]);
                return std::nullopt;
            }
        } else if (arg == "--load" && has_value) {
            options.load = argv[++i];
        } else if (arg.ends_with(".rtsrep")) {
            options.replay = argv[i];  // arrastrar una repetición sobre el ejecutable
        } else if (arg.ends_with(".rtssav")) {
            options.load = argv[i];  // arrastrar una partida guardada sobre el ejecutable
        } else {
            spdlog::error("Argumento no reconocido: '{}'", arg);
            print_usage();
            return std::nullopt;
        }
    }
    return options;
}

int run_headless(const GameData& data, std::int64_t ticks, const std::filesystem::path& record,
                 std::int64_t crash_at) {
    const auto gen_start = SteadyClock::now();
    sim::World world(data.engine.world);
    const double gen_ms = elapsed_ms(gen_start);
    std::optional<ReplayRecorder> recorder;
    if (!record.empty()) {
        recorder.emplace(data.files, data.engine.replay.checkpoint_interval_ticks);
    }
    std::optional<CrashGuard> guard;
    if (recorder) {
        guard.emplace(*recorder, world);
    }
    issue_scenario(data, world, [&](sim::Command c) {
        if (recorder) {
            recorder->issue(world, std::move(c));
        } else {
            world.issue(std::move(c));
        }
    });

    const auto start = SteadyClock::now();
    for (std::int64_t i = 0; i < ticks; ++i) {
        if (i == crash_at) {
            spdlog::warn("--crash-at: caída provocada en el tick {}", world.tick());
            std::raise(SIGSEGV);
        }
        world.step();
        if (recorder) {
            recorder->after_step(world);
        }
    }
    const double total_ms = elapsed_ms(start);
    const double per_tick = ticks > 0 ? total_ms / static_cast<double>(ticks) : 0.0;
    spdlog::info("headless: mapa {}x{} generado en {:.2f} ms", world.map().width(), world.map().height(), gen_ms);
    sim::Snapshot snap;
    world.write_snapshot(snap);
    spdlog::info("headless: {} ticks, {} unidades, {} objetos, {} órdenes de guion, {:.3f} ms total, {:.4f} ms/tick",
                 ticks, snap.entities.size(), snap.objects.size(), data.headless_scenario.orders.size(), total_ms,
                 per_tick);
    for (std::size_t p = 0; p < snap.players.size(); ++p) {
        const sim::PlayerState& ps = snap.players[p];
        spdlog::info("headless: jugador {}: comida {} madera {} piedra {} oro {} hierro {}, población {}/{}", p,
                     ps.stock[0], ps.stock[1], ps.stock[2], ps.stock[3], ps.stock[4], ps.population,
                     ps.population_cap);
    }
    // Formato estable: la CI lo compara entre plataformas.
    spdlog::info("state_hash={:016x}", world.state_hash());
    if (recorder) {
        if (const auto saved = save_replay(record, recorder->finish(world)); !saved) {
            spdlog::error("Repetición: {}", saved.error());
            return 1;
        }
        spdlog::info("Repetición guardada en {}", record.string());
    }
    return 0;
}

int run_net_headless(const GameData& data, const LaunchOptions& options) {
    const NetConfig& net = data.engine.net;
    if (options.host_port && options.headless_ticks <= 0) {
        spdlog::error("red: el anfitrión necesita --ticks (la duración de la partida)");
        return 2;
    }
    const std::uint64_t hash = data_hash(data.files);
    auto session = options.host_port
                       ? LockstepSession::host(*options.host_port, options.net_players, net.lockstep, hash,
                                               static_cast<sim::Tick>(options.headless_ticks))
                       : LockstepSession::join(options.join_host, options.join_port, net.lockstep, hash);
    if (!session) {
        spdlog::error("red: {}", session.error());
        return 1;
    }
    if (options.host_port) {
        spdlog::info("red: anfitrión en el puerto {}, esperando a {} jugadores más", session->port(),
                     options.net_players - 1);
    }
    while (session->state() == LockstepState::Lobby) {
        session->poll(net.poll_wait_ms);
    }
    if (session->state() == LockstepState::Failed) {
        spdlog::error("red: {}", session->error());
        return 1;
    }
    const sim::PlayerId me = session->local_player();
    spdlog::info("red: partida de {} jugadores, soy el {}, {} ticks", session->players(), me, session->end_tick());

    sim::World world(data.engine.world);
    std::optional<ReplayRecorder> recorder;
    if (!options.record.empty()) {
        recorder.emplace(data.files, data.engine.replay.checkpoint_interval_ticks);
    }
    std::optional<CrashGuard> guard;
    if (recorder) {
        guard.emplace(*recorder, world);
    }
    const auto issue = [&](sim::Command c) {
        if (recorder) {
            recorder->issue(world, std::move(c));
        } else {
            world.issue(std::move(c));
        }
    };
    // Cada jugador su propia serie: la del otro solo se conoce por la red.
    sim::Xoshiro256pp rng(data.engine.world.setup.seed + me);
    const sim::TileCoord map_size{world.map().width(), world.map().height()};
    std::optional<sim::Tick> probed;
    std::int64_t waits = 0;
    const auto start = SteadyClock::now();
    while (world.tick() < session->end_tick() && session->state() == LockstepState::Running) {
        const sim::Tick tick = world.tick();
        if (tick % static_cast<sim::Tick>(net.probe.every_ticks) == 0 && probed != tick) {
            probed = tick;
            sim::Snapshot snap;
            world.write_snapshot(snap);
            for (sim::Command& c : probe_orders(snap, me, net.probe, map_size, rng)) {
                session->submit(std::move(c));
            }
        }
        session->poll(0);
        if (session->begin_tick(world, issue)) {
            world.step();
            if (recorder) {
                recorder->after_step(world);
            }
        } else {
            ++waits;
            session->poll(net.poll_wait_ms);
        }
    }
    session->finish(world);
    while (session->state() == LockstepState::Running) {
        session->poll(net.poll_wait_ms);
    }
    // Lo último (el fin propio, lo que reenvía el anfitrión) tiene que salir antes de cerrar.
    const auto flush_start = SteadyClock::now();
    while (!session->flushed() && elapsed_ms(flush_start) < net.lockstep.stall_timeout_ms) {
        session->poll(net.poll_wait_ms);
    }
    spdlog::info("red: {} ticks en {:.0f} ms, {} esperas a la red, {} hashes comparados", world.tick(),
                 elapsed_ms(start), waits, session->hashes_compared());
    spdlog::info("state_hash={:016x}", world.state_hash());
    if (recorder) {
        if (const auto saved = save_replay(options.record, recorder->finish(world)); !saved) {
            spdlog::error("Repetición: {}", saved.error());
            return 1;
        }
        spdlog::info("Repetición guardada en {}", options.record.string());
    }
    if (session->state() != LockstepState::Finished) {
        spdlog::error("red: {}", session->error());
        if (session->desync()) {
            write_crash_report(std::format("desincronización en red: {}", session->error()));
        }
        return 1;
    }
    spdlog::info("red: todos los jugadores acaban con el mismo hash");
    return 0;
}

int run_verify_replay(const std::filesystem::path& path) {
    const auto replay = load_replay(path);
    if (!replay) {
        spdlog::error("{}", replay.error());
        return 1;
    }
    const auto data = parse_game_data(replay->data);
    if (!data) {
        spdlog::error("Datos de la repetición: {}", data.error());
        return 1;
    }
    const auto start = SteadyClock::now();
    const VerifyResult r = verify_replay(*replay, data->engine.world);
    const double ms = elapsed_ms(start);
    spdlog::info("verify: {} órdenes, {} ticks de {}, {} checkpoints comparados en {:.1f} ms", replay->commands.size(),
                 r.ticks, replay->end_tick, r.checkpoints, ms);
    if (!r.ok) {
        spdlog::error("verify: DIVERGENCIA en el tick {}: esperado {:016x}, obtenido {:016x}", *r.diverged_at,
                      r.expected_hash, r.actual_hash);
        return 1;
    }
    // Formato estable, como state_hash= en headless.
    spdlog::info("verify: OK state_hash={:016x}", r.actual_hash);
    return 0;
}

namespace {

struct Display {
    std::unique_ptr<platform::Window> window;
    std::unique_ptr<render::Renderer> renderer;
    std::unique_ptr<AudioSystem> audio;  // null: sin sonido (sin dispositivo o sin datos)

    void pump_audio(std::int32_t piece) {
        if (audio) {
            audio->music(piece);
            audio->pump();
        }
    }
};

// Con opciones del jugador (F5), la ventana sale de su tamaño (o a pantalla completa).
std::optional<Display> open_display(const GameData& data, const Options* options = nullptr) {
    const WindowConfig& wc = data.engine.window;
    auto window = platform::Window::create({wc.title, options != nullptr ? options->width : wc.width,
                                            options != nullptr ? options->height : wc.height,
                                            options != nullptr && options->fullscreen});
    if (!window) {
        spdlog::error("{}", window.error());
        return std::nullopt;
    }
    render::RendererDesc desc;
    desc.window = (*window)->handle();
    desc.vsync = wc.vsync;
    desc.view = data.engine.view;
    for (const TerrainInfo& t : data.terrain.types) {
        desc.terrain_colors.push_back(t.color);
    }
    desc.art = &data.art;
    auto renderer = render::Renderer::create(desc);
    if (!renderer) {
        spdlog::error("{}", renderer.error());
        return std::nullopt;
    }
    spdlog::info("Backend GPU: {}", (*renderer)->driver_name());
    Display display{std::move(*window), std::move(*renderer), nullptr};
    // Sonido (F2): todo se sintetiza aquí, una vez. Sin dispositivo, en silencio.
    if (!data.sound.recipes.empty()) {
        if (auto out = platform::AudioOut::open(data.sound.config.sample_rate, data.sound.config.latency_ms)) {
            const auto start = SteadyClock::now();
            display.audio = std::make_unique<AudioSystem>(data.sound, std::move(*out));
            spdlog::info("Sonido: {} recetas y {} piezas sintetizadas en {:.0f} ms", data.sound.recipes.size(),
                         data.sound.music.size(), elapsed_ms(start));
        } else {
            spdlog::info("Sonido: sin dispositivo de audio, en silencio");
        }
    }
    return display;
}

struct MenuChoice {
    enum class Kind : std::uint8_t { New, Load, Replay, Host, Join, EditNew, Edit, PlayScenario, Campaign, Options, Quit };
    Kind kind = Kind::Quit;
    std::filesystem::path path;
    std::size_t campaign = 0;  // Campaign: cuál
};

// Lo que se escribe en el menú para jugar en red (E2).
struct NetMenu {
    int port = 0;
    std::array<char, 256> address{};  // nombre o IP del anfitrión
};

// Ficheros con esa extensión en la carpeta de repeticiones, los más recientes primero.
std::vector<std::filesystem::path> saved_files(const GameData& data, std::string_view ext, std::size_t max) {
    std::vector<std::filesystem::path> out;
    const auto dir = std::filesystem::path(platform::executable_dir()) / data.engine.replay.directory;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ext) {
            out.push_back(entry.path());
        }
    }
    std::ranges::sort(out, std::greater<>{});  // el nombre lleva la fecha: orden inverso = recientes primero
    if (out.size() > max) {
        out.resize(max);
    }
    return out;
}

// Escenarios guardados por el editor (F3), por nombre.
std::vector<std::filesystem::path> scenario_files() {
    std::vector<std::filesystem::path> out;
    const auto dir = std::filesystem::path(platform::executable_dir()) / kScenarioDir;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".toml") {
            out.push_back(entry.path());
        }
    }
    std::ranges::sort(out);
    return out;
}

std::expected<ScenarioDoc, std::string> read_scenario(const std::filesystem::path& path, const GameData& base) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::unexpected(std::format("no se puede leer {}", path.string()));
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return parse_scenario_doc(text, base, path.filename().string());
}

// Menú inicial: nueva partida (semilla, rival, niebla), cargar, repeticiones, salir.
MenuChoice run_menu(Display& d, const GameData& base, MatchSettings& settings, NetMenu& net,
                    const std::vector<Campaign>& campaigns) {
    constexpr std::size_t kListed = 8;
    const auto saves = saved_files(base, ".rtssav", kListed);
    const auto replays = saved_files(base, ".rtsrep", kListed);
    const auto& profiles = base.engine.ai_profile_names;
    while (true) {
        d.pump_audio(base.sound.peace_music);
        SDL_Event event;
        while (d.window->poll_event(event)) {
            d.renderer->process_event(event);
            if (d.window->is_close_request(event)) {
                return {};
            }
        }
        d.renderer->begin_frame();
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos({display.x * 0.5f, display.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::Begin(T("Menú"), nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
        constexpr float kTitleScale = 2.0f;
        ImGui::SetWindowFontScale(kTitleScale);
        ImGui::TextUnformatted(base.engine.window.title.c_str());
        ImGui::SetWindowFontScale(1.0f);
        MenuChoice choice;
        bool chosen = false;
        ImGui::SeparatorText(T("Nueva partida"));
        int seed = static_cast<int>(settings.seed);
        if (ImGui::InputInt(T("Semilla del mapa"), &seed)) {
            settings.seed = static_cast<std::uint64_t>(std::max(seed, 0));
        }
        if (ImGui::BeginCombo(T("Rival (IA)"), N(settings.rival))) {
            for (const std::string& p : profiles) {
                if (ImGui::Selectable(N(p), p == settings.rival)) {
                    settings.rival = p;
                }
            }
            ImGui::EndCombo();
        }
        // Tipo de mapa (F3): ríos con vados, etc.
        const auto& presets = base.engine.map_presets;
        if (!presets.empty()) {
            const std::string shown = settings.map.empty() ? presets.front().name : settings.map;
            if (ImGui::BeginCombo(T("Mapa"), N(shown))) {
                for (const MapPreset& p : presets) {
                    if (ImGui::Selectable(N(p.name), p.name == shown)) {
                        settings.map = p.name;
                    }
                }
                ImGui::EndCombo();
            }
        }
        ImGui::Checkbox(T("Niebla de guerra"), &settings.fog);
        if (ImGui::Button(T("Empezar"))) {
            choice.kind = MenuChoice::Kind::New;
            chosen = true;
        }
        if (!campaigns.empty()) {
            ImGui::SeparatorText(T("Campaña"));
            for (std::size_t i = 0; i < campaigns.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Button(campaigns[i].name.c_str())) {
                    choice.kind = MenuChoice::Kind::Campaign;
                    choice.campaign = i;
                    chosen = true;
                }
                ImGui::PopID();
            }
        }
        ImGui::SeparatorText(T("En red (de 2 a 4 jugadores; los puestos libres, para la IA)"));
        ImGui::InputInt(T("Puerto"), &net.port);
        net.port = std::clamp(net.port, 1, static_cast<int>(std::numeric_limits<std::uint16_t>::max()));
        if (ImGui::Button(T("Crear sala"))) {
            choice.kind = MenuChoice::Kind::Host;
            chosen = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
        ImGui::InputText("##direccion", net.address.data(), net.address.size());
        ImGui::SameLine();
        if (ImGui::Button(T("Unirse"))) {
            choice.kind = MenuChoice::Kind::Join;
            chosen = true;
        }
        ImGui::SeparatorText(T("Escenarios (F3)"));
        if (ImGui::Button(T("Nuevo escenario con este mapa"))) {
            choice.kind = MenuChoice::Kind::EditNew;
            chosen = true;
        }
        for (const auto& p : scenario_files()) {
            ImGui::PushID(p.string().c_str());
            if (ImGui::SmallButton(T("Jugar"))) {
                choice = {MenuChoice::Kind::PlayScenario, p};
                chosen = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(T("Editar"))) {
                choice = {MenuChoice::Kind::Edit, p};
                chosen = true;
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(p.stem().string().c_str());
            ImGui::PopID();
        }
        ImGui::SeparatorText(TF("Cargar partida guardada ({} durante la partida)", key_label(KeyAction::QuickSave)).c_str());
        if (saves.empty()) {
            ImGui::TextDisabled("%s", T("Ninguna todavía"));
        }
        for (const auto& p : saves) {
            if (ImGui::Selectable(p.filename().string().c_str())) {
                choice = {MenuChoice::Kind::Load, p};
                chosen = true;
            }
        }
        ImGui::SeparatorText(T("Ver una repetición"));
        if (replays.empty()) {
            ImGui::TextDisabled("%s", T("Ninguna todavía"));
        }
        for (const auto& p : replays) {
            if (ImGui::Selectable(p.filename().string().c_str())) {
                choice = {MenuChoice::Kind::Replay, p};
                chosen = true;
            }
        }
        ImGui::Separator();
        if (ImGui::Button(T("Opciones"))) {
            choice.kind = MenuChoice::Kind::Options;
            chosen = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Salir"))) {
            choice.kind = MenuChoice::Kind::Quit;
            chosen = true;
        }
        ImGui::End();
        d.renderer->end_frame();
        if (chosen) {
            return choice;
        }
    }
}

// Sala de espera de una partida en red. El anfitrión ve quién hay, elige cuántos puestos
// tiene la partida (los que no ocupa nadie, para la IA) y empieza; el invitado espera.
// Devuelve los datos de la partida al empezar, o nada si se sale o falla.
std::optional<GameData> run_lobby(Display& d, const GameData& base, MatchSettings settings, LockstepSession& net) {
    const auto& profiles = base.engine.ai_profile_names;
    const auto max_seats = static_cast<int>(
        std::min<std::size_t>(base.engine.seat_starts.size(), static_cast<std::size_t>(base.engine.net.lockstep.max_players)));
    int seats = 2;
    std::string chat_input;
    while (true) {
        d.pump_audio(base.sound.peace_music);
        SDL_Event event;
        while (d.window->poll_event(event)) {
            d.renderer->process_event(event);
            if (d.window->is_close_request(event)) {
                return std::nullopt;
            }
        }
        net.poll(0);
        if (net.state() == LockstepState::Running && !net.is_host()) {
            auto data = with_match_settings_text(base, net.settings());
            if (!data) {
                spdlog::error("Ajustes del anfitrión: {}", data.error());
                return std::nullopt;
            }
            return std::move(*data);
        }
        d.renderer->begin_frame();
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos({display.x * 0.5f, display.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::SetNextWindowSize({display.x * 0.5f, display.y * 0.6f}, ImGuiCond_Always);
        ImGui::Begin(T("Sala"), nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);
        bool leave = false;
        std::optional<GameData> started;
        if (net.state() == LockstepState::Failed) {
            ImGui::TextColored(kWarnColor, "%s", net.error().c_str());
        } else if (net.is_host()) {
            ImGui::Text(T("Sala abierta en el puerto %u. Los demás se unen con la IP de este equipo y ese puerto."),
                        static_cast<unsigned>(net.port()));
            ImGui::Text(T("Jugadores conectados: %u (tú incluido)"), static_cast<unsigned>(net.connected()));
            seats = std::clamp(seats, std::max<int>(2, net.connected()), max_seats);
            ImGui::SliderInt(T("Puestos"), &seats, std::max<int>(2, net.connected()), max_seats);
            for (int i = 0; i < seats; ++i) {
                ImGui::BulletText(T("Puesto %d: %s"), i,
                                  i < net.connected() ? T("humano") : TF("IA {}", N(settings.rival)).c_str());
            }
            ImGui::Text(T("Semilla %llu, mapa %s, niebla %s"), static_cast<unsigned long long>(settings.seed),
                        settings.map.empty() ? T("por defecto") : N(settings.map), settings.fog ? T("sí") : T("no"));
            ImGui::BeginDisabled(net.connected() < 2);
            if (ImGui::Button(T("Empezar"))) {
                settings.seats.assign(static_cast<std::size_t>(seats), "ia");
                for (std::size_t i = 0; i < net.connected(); ++i) {
                    settings.seats[i] = "humano";
                }
                auto data = with_match_settings(base, settings);
                if (!data) {
                    spdlog::error("Ajustes de partida: {}", data.error());
                } else {
                    const auto it = std::ranges::find(profiles, settings.rival);
                    net.start(match_settings_toml(settings), static_cast<std::uint8_t>(it - profiles.begin()));
                    started = std::move(*data);
                }
            }
            ImGui::EndDisabled();
        } else {
            if (net.joining()) {
                ImGui::TextUnformatted("Conectando con el anfitrión...");
            } else {
                ImGui::Text(T("Conectado. Jugadores en la sala: %u. Empieza el anfitrión."),
                            static_cast<unsigned>(net.connected()));
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Salir de la sala"))) {
            leave = true;
        }
        ImGui::SeparatorText(T("Charla"));
        ImGui::BeginChild("charla");
        draw_chat(net, base, chat_input);
        ImGui::EndChild();
        ImGui::End();
        d.renderer->end_frame();
        if (started) {
            return started;
        }
        if (leave) {
            return std::nullopt;
        }
    }
}

}  // namespace

int run_windowed(const GameData& data, std::int64_t max_frames, const Replay* replay, const Replay* resume) {
    auto display = open_display(data);
    if (!display) {
        return 1;
    }
    WindowedGame game(data, *display->window, *display->renderer, replay, resume, nullptr, display->audio.get());
    return game.run(max_frames);
}

int run_net_windowed(const GameData& base, const LaunchOptions& options) {
    const std::uint64_t hash = data_hash(base.files);
    const auto& cfg = base.engine.net.lockstep;
    auto session = options.host_port ? LockstepSession::host(*options.host_port, 0, cfg, hash, 0)
                                     : LockstepSession::join(options.join_host, options.join_port, cfg, hash);
    if (!session) {
        spdlog::error("red: {}", session.error());
        return 1;
    }
    // Sin sala con ventana: el anfitrión empieza en cuanto están todos, todos humanos.
    MatchSettings settings;
    settings.seed = base.engine.world.map.seed;
    settings.fog = base.engine.world.vision.enabled;
    settings.rival = base.engine.ai_profile_names.front();
    settings.seats.assign(options.net_players, "humano");
    const auto start = SteadyClock::now();
    while (session->state() == LockstepState::Lobby && elapsed_ms(start) < cfg.lobby_timeout_ms) {
        session->poll(base.engine.net.poll_wait_ms);
        if (session->is_host() && session->connected() == options.net_players) {
            session->start(match_settings_toml(settings), 0);
        }
    }
    if (session->state() != LockstepState::Running) {
        spdlog::error("red: {}", session->error().empty() ? "no se reunieron los jugadores" : session->error());
        return 1;
    }
    auto data = with_match_settings_text(base, session->settings());
    if (!data) {
        spdlog::error("red: ajustes del anfitrión: {}", data.error());
        return 1;
    }
    auto display = open_display(*data);
    if (!display) {
        return 1;
    }
    WindowedGame game(*data, *display->window, *display->renderer, nullptr, nullptr, &*session, display->audio.get());
    const int code = game.run(options.max_frames);
    spdlog::info("red: jugador {}, tick {}, estado {}", session->local_player(), game.tick(),
                 session->state() == LockstepState::Running ? "en partida" : session->error());
    // Que el otro cierre antes es lo normal al acabar una prueba de humo; una
    // desincronización, no.
    return session->desync() ? 1 : code;
}

// --- Opciones (F5) -------------------------------------------------------------------

std::string read_text(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::filesystem::path options_path() {
    return std::filesystem::path(platform::executable_dir()) / kOptionsFile;
}

// Las de data/ y, encima, las del jugador. Si las del jugador no se leen, se avisa y
// se juega con las de data/.
std::optional<Options> load_options(const std::filesystem::path& data_dir) {
    const auto defaults_path = data_dir / kOptionsFile;
    auto defaults = parse_options(read_text(defaults_path), nullptr, defaults_path.string());
    if (!defaults) {
        spdlog::error("Opciones: {}", defaults.error());
        return std::nullopt;
    }
    if (!std::filesystem::exists(options_path())) {
        return std::move(*defaults);
    }
    auto mine = parse_options(read_text(options_path()), &*defaults, options_path().string());
    if (!mine) {
        spdlog::warn("Opciones del jugador: {}; se usan las de partida", mine.error());
        return std::move(*defaults);
    }
    return std::move(*mine);
}

void save_options(const Options& o) {
    std::ofstream out(options_path(), std::ios::binary);
    out << options_toml(o);
    if (!out) {
        spdlog::warn("No se pudieron guardar las opciones en {}", options_path().string());
    }
}

// Aplica idioma, teclas, volumen y pantalla.
void apply_options(const Options& o, Display& d, const std::filesystem::path& data_dir) {
    if (auto r = set_language(data_dir, o.language); !r) {
        spdlog::warn("Idioma: {}", r.error());
    }
    for (std::size_t i = 0; i < kKeyActionCount; ++i) {
        const std::int32_t code = platform::scancode_from_name(o.keys[i]);
        if (code < 0) {
            spdlog::warn("Opciones: la tecla \"{}\" de {} no existe; se queda la anterior", o.keys[i], kKeyActionIds[i]);
            continue;
        }
        key_codes()[i] = code;
    }
    std::array<std::int32_t, 8> scroll{};
    std::copy_n(key_codes().begin(), scroll.size(), scroll.begin());
    d.window->set_scroll_keys(scroll);
    if (d.audio) {
        constexpr float kPercentF = 100.0f;
        d.audio->mixer.set_gains(static_cast<float>(o.master_percent) / kPercentF,
                                 static_cast<float>(o.effects_percent) / kPercentF,
                                 static_cast<float>(o.music_percent) / kPercentF);
    }
    d.window->set_fullscreen(o.fullscreen);
    if (!o.fullscreen) {
        d.window->set_size(o.width, o.height);
    }
}

// Pantalla de opciones. Devuelve las nuevas si se guardan; nada si se vuelve sin guardar.
std::optional<Options> run_options_menu(Display& d, const GameData& base, Options o,
                                        const std::filesystem::path& data_dir) {
    const auto languages = available_languages(data_dir);
    static constexpr std::array<const char*, kKeyActionCount> kLabels{
        TK("Cámara: izquierda"), TK("Cámara: derecha"), TK("Cámara: arriba"), TK("Cámara: abajo"),
        TK("Cámara: izquierda (otra)"), TK("Cámara: derecha (otra)"), TK("Cámara: arriba (otra)"),
        TK("Cámara: abajo (otra)"), TK("Ir al último aviso"), TK("Guardar la partida"), TK("Volver al menú"),
        TK("Datos de depuración"), TK("Ayuda"),
    };
    std::optional<std::size_t> capturing;  // acción que espera su tecla nueva
    while (true) {
        d.pump_audio(base.sound.peace_music);
        SDL_Event event;
        while (d.window->poll_event(event)) {
            d.renderer->process_event(event);
            if (d.window->is_close_request(event)) {
                return std::nullopt;
            }
            if (capturing && event.type == SDL_EVENT_KEY_DOWN) {
                if (event.key.scancode != SDL_SCANCODE_ESCAPE) {
                    o.keys[*capturing] = platform::scancode_name(static_cast<std::int32_t>(event.key.scancode));
                }
                capturing.reset();
            }
        }
        d.renderer->begin_frame();
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos({display.x * 0.5f, display.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::Begin(T("Opciones"), nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
        std::optional<Options> result;
        bool back = false;
        ImGui::SeparatorText(T("Idioma"));
        const auto current = std::ranges::find(languages, o.language, &LanguageInfo::code);
        if (ImGui::BeginCombo(T("Idioma"), current != languages.end() ? current->name.c_str() : o.language.c_str())) {
            for (const LanguageInfo& l : languages) {
                if (ImGui::Selectable(l.name.c_str(), l.code == o.language)) {
                    o.language = l.code;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SeparatorText(T("Pantalla"));
        ImGui::Checkbox(T("Pantalla completa"), &o.fullscreen);
        const std::string size = std::format("{} x {}", o.width, o.height);
        ImGui::BeginDisabled(o.fullscreen);
        if (ImGui::BeginCombo(T("Tamaño de la ventana"), size.c_str())) {
            for (const auto& [w, h] : o.resolutions) {
                if (ImGui::Selectable(std::format("{} x {}", w, h).c_str(), w == o.width && h == o.height)) {
                    o.width = w;
                    o.height = h;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::SeparatorText(T("Volumen"));
        constexpr int kMaxPercent = 100;
        ImGui::SliderInt(T("General"), &o.master_percent, 0, kMaxPercent, "%d %%");
        ImGui::SliderInt(T("Efectos"), &o.effects_percent, 0, kMaxPercent, "%d %%");
        ImGui::SliderInt(T("Música"), &o.music_percent, 0, kMaxPercent, "%d %%");
        ImGui::SeparatorText(T("Teclas"));
        if (ImGui::BeginTable("teclas", 2, ImGuiTableFlags_SizingFixedFit)) {
            for (std::size_t i = 0; i < kKeyActionCount; ++i) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(T(kLabels[i]));
                ImGui::TableNextColumn();
                ImGui::PushID(static_cast<int>(i));
                const std::string label = capturing == i ? std::string(T("Pulsa una tecla (Esc: dejarla)")) : o.keys[i];
                if (ImGui::Button(label.c_str())) {
                    capturing = i;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::Separator();
        if (ImGui::Button(T("Guardar"))) {
            result = o;
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Volver sin guardar"))) {
            back = true;
        }
        ImGui::End();
        d.renderer->end_frame();
        if (result || back) {
            return result;
        }
    }
}

// Progreso de las campañas: junto al ejecutable, como las repeticiones.
std::filesystem::path progress_path() {
    return std::filesystem::path(platform::executable_dir()) / kCampaignFile;
}

CampaignProgress load_progress() {
    std::ifstream in(progress_path(), std::ios::binary);
    return parse_progress(std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()});
}

void save_progress(const CampaignProgress& p) {
    std::ofstream out(progress_path(), std::ios::binary);
    out << progress_toml(p);
    if (!out) {
        spdlog::warn("No se pudo guardar el progreso de la campaña en {}", progress_path().string());
    }
}

// Capítulo elegido en la pantalla de una campaña, con su escenario y su rival.
struct ChapterPick {
    std::size_t chapter = 0;
    ScenarioDoc doc;
    std::string rival;
};

// Pantalla de una campaña (F4): capítulos (los bloqueados, en gris), y del elegido su
// fecha, informe, nota histórica y fuentes; rival y «Jugar». Nada si se vuelve al menú.
std::optional<ChapterPick> run_campaign_menu(Display& d, const GameData& base, const Campaign& c,
                                             const CampaignProgress& progress, std::size_t& selected) {
    // Escenarios leídos una vez: el informe sale de ellos.
    std::vector<std::expected<ScenarioDoc, std::string>> docs;
    for (const CampaignChapter& ch : c.chapters) {
        docs.push_back(read_scenario(c.dir / ch.scenario, base));
    }
    std::vector<std::string> rivals;
    for (const CampaignChapter& ch : c.chapters) {
        rivals.push_back(ch.rival.empty() ? base.engine.ai_profile_names.front() : ch.rival);
    }
    const auto& profiles = base.engine.ai_profile_names;
    while (true) {
        d.pump_audio(base.sound.peace_music);
        SDL_Event event;
        while (d.window->poll_event(event)) {
            d.renderer->process_event(event);
            if (d.window->is_close_request(event)) {
                return std::nullopt;
            }
        }
        d.renderer->begin_frame();
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        constexpr float kShare = 0.85f;
        ImGui::SetNextWindowPos({display.x * 0.5f, display.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::SetNextWindowSize({display.x * kShare, display.y * kShare}, ImGuiCond_Always);
        ImGui::Begin(c.name.c_str(), nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);
        std::optional<ChapterPick> pick;
        bool back = false;
        constexpr float kListShare = 0.28f;
        ImGui::BeginChild("capitulos", {ImGui::GetContentRegionAvail().x * kListShare, 0.0f}, ImGuiChildFlags_Borders);
        ImGui::TextWrapped("%s", c.intro.c_str());
        ImGui::Separator();
        for (std::size_t i = 0; i < c.chapters.size(); ++i) {
            const bool open = progress.unlocked(c, i);
            const bool done = progress.has_won(c.id, c.chapters[i].id);
            const std::string label = std::format("{}. {}{}", i + 1, c.chapters[i].title, done ? " (superado)" : "");
            ImGui::BeginDisabled(!open);
            if (ImGui::Selectable(label.c_str(), selected == i)) {
                selected = i;
            }
            ImGui::EndDisabled();
        }
        ImGui::Separator();
        if (ImGui::Button(T("Volver al menú"))) {
            back = true;
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("detalle", {0.0f, 0.0f}, ImGuiChildFlags_Borders);
        const CampaignChapter& ch = c.chapters[selected];
        constexpr float kTitleScale = 1.5f;
        ImGui::SetWindowFontScale(kTitleScale);
        ImGui::TextUnformatted(ch.title.c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::TextDisabled("%s", ch.date.c_str());
        const auto& doc = docs[selected];
        if (!doc) {
            ImGui::TextColored({1.0f, 0.35f, 0.3f, 1.0f}, "%s", doc.error().c_str());
        } else {
            ImGui::SeparatorText(T("Informe"));
            ImGui::TextWrapped("%s", doc->briefing.c_str());
        }
        ImGui::SeparatorText(T("Nota histórica"));
        ImGui::TextWrapped("%s", ch.history.c_str());
        ImGui::SeparatorText(T("Fuentes (citas literales)"));
        for (const CampaignSource& s : ch.sources) {
            ImGui::TextWrapped("«%s»", s.quote.c_str());
            ImGui::TextDisabled("%s · %s", s.work.c_str(), s.url.c_str());
            ImGui::Spacing();
        }
        ImGui::Separator();
        if (ImGui::BeginCombo(T("Rival (IA)"), N(rivals[selected]))) {
            for (const std::string& p : profiles) {
                if (ImGui::Selectable(N(p), p == rivals[selected])) {
                    rivals[selected] = p;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::BeginDisabled(!doc || !progress.unlocked(c, selected));
        if (ImGui::Button(T("Jugar este capítulo"))) {
            pick = ChapterPick{selected, *doc, rivals[selected]};
        }
        ImGui::EndDisabled();
        ImGui::EndChild();
        ImGui::End();
        d.renderer->end_frame();
        if (back) {
            return std::nullopt;
        }
        if (pick) {
            return pick;
        }
    }
}

int run_interactive(const GameData& base, const std::filesystem::path& data_dir) {
    std::optional<Options> options = load_options(data_dir);
    auto display = open_display(base, options ? &*options : nullptr);
    if (!display) {
        return 1;
    }
    if (options) {
        apply_options(*options, *display, data_dir);
    }
    MatchSettings settings;
    settings.seed = base.engine.world.map.seed;
    settings.fog = base.engine.world.vision.enabled;
    if (!base.engine.world.ai_players.empty()) {
        settings.rival = base.engine.ai_profile_names[base.engine.world.ai_players.front().profile];
    }
    NetMenu net_menu;
    net_menu.port = base.engine.net.default_port;
    std::ranges::copy(base.engine.net.default_address.substr(0, net_menu.address.size() - 1),
                      net_menu.address.begin());
    std::vector<std::string> campaign_errors;
    const std::vector<Campaign> campaigns = load_campaigns(data_dir, campaign_errors);
    for (const std::string& e : campaign_errors) {
        spdlog::error("Campaña: {}", e);
    }
    CampaignProgress progress = load_progress();
    std::size_t chapter_selected = 0;
    while (true) {
        const MenuChoice choice = run_menu(*display, base, settings, net_menu, campaigns);
        std::optional<GameData> data;
        std::optional<Replay> recorded;
        std::optional<LockstepSession> net;
        std::optional<ScenarioDoc> editing;
        switch (choice.kind) {
            case MenuChoice::Kind::Quit:
                return 0;
            case MenuChoice::Kind::Options:
                if (!options) {
                    continue;  // sin data/opciones.toml no hay de dónde partir (ya se avisó)
                }
                if (auto changed = run_options_menu(*display, base, *options, data_dir)) {
                    options = std::move(changed);
                    save_options(*options);
                    apply_options(*options, *display, data_dir);
                }
                continue;
            case MenuChoice::Kind::Campaign: {
                // Capítulo a capítulo hasta volver al menú; ganar uno desbloquea el siguiente.
                const Campaign& c = campaigns[choice.campaign];
                chapter_selected = std::min(chapter_selected, c.chapters.size() - 1);
                while (auto pick = run_campaign_menu(*display, base, c, progress, chapter_selected)) {
                    const CampaignChapter& ch = c.chapters[pick->chapter];
                    MatchSettings chapter_settings = settings;
                    chapter_settings.rival = pick->rival;
                    chapter_settings.fog = ch.fog;
                    auto d = with_scenario(base, pick->doc, chapter_settings);
                    if (!d) {
                        spdlog::error("Capítulo {}: {}", ch.id, d.error());
                        continue;
                    }
                    WindowedGame game(*d, *display->window, *display->renderer, nullptr, nullptr, nullptr,
                                      display->audio.get());
                    game.run(0);
                    if (game.won()) {
                        progress.mark_won(c.id, ch.id);
                        save_progress(progress);
                        chapter_selected = std::min(pick->chapter + 1, c.chapters.size() - 1);
                    }
                    if (!game.back_to_menu()) {
                        return 0;  // ventana cerrada
                    }
                }
                continue;
            }
            case MenuChoice::Kind::EditNew:
            case MenuChoice::Kind::Edit:
            case MenuChoice::Kind::PlayScenario: {
                if (choice.kind == MenuChoice::Kind::EditNew) {
                    // Punto de partida: el mapa generado con los ajustes del menú.
                    auto gen = with_match_settings(base, settings);
                    if (!gen) {
                        spdlog::error("Ajustes de partida: {}", gen.error());
                        continue;
                    }
                    const sim::World world(gen->engine.world);
                    editing = scenario_from_world(world, "Nuevo escenario",
                                                  static_cast<std::int32_t>(gen->engine.world.setup.starts.size()));
                } else {
                    auto doc = read_scenario(choice.path, base);
                    if (!doc) {
                        spdlog::error("{}", doc.error());
                        continue;
                    }
                    editing = std::move(*doc);
                }
                if (choice.kind == MenuChoice::Kind::PlayScenario) {
                    auto d = with_scenario(base, *editing, settings);
                    if (!d) {
                        spdlog::error("Escenario: {}", d.error());
                        continue;
                    }
                    data = std::move(*d);
                    editing.reset();
                    break;
                }
                // Editor: los datos con todos los puestos (colores de hasta cuatro jugadores).
                ScenarioDoc all = *editing;
                all.params.players = static_cast<std::int32_t>(base.engine.seat_starts.size());
                auto d = with_scenario(base, all, settings);
                if (!d) {
                    spdlog::error("Escenario: {}", d.error());
                    continue;
                }
                data = std::move(*d);
                break;
            }
            case MenuChoice::Kind::Host:
            case MenuChoice::Kind::Join: {
                const std::uint64_t hash = data_hash(base.files);
                const auto port = static_cast<std::uint16_t>(net_menu.port);
                auto session = choice.kind == MenuChoice::Kind::Host
                                   ? LockstepSession::host(port, 0, base.engine.net.lockstep, hash, 0)
                                   : LockstepSession::join(net_menu.address.data(), port, base.engine.net.lockstep, hash);
                if (!session) {
                    spdlog::error("Red: {}", session.error());
                    continue;
                }
                net.emplace(std::move(*session));
                auto d = run_lobby(*display, base, settings, *net);
                if (!d) {
                    continue;
                }
                data = std::move(*d);
                break;
            }
            case MenuChoice::Kind::New: {
                auto d = with_match_settings(base, settings);
                if (!d) {
                    spdlog::error("Ajustes de partida: {}", d.error());
                    continue;
                }
                data = std::move(*d);
                break;
            }
            case MenuChoice::Kind::Load:
            case MenuChoice::Kind::Replay: {
                auto r = load_replay(choice.path);
                if (!r) {
                    spdlog::error("{}", r.error());
                    continue;
                }
                auto d = parse_game_data(r->data);
                if (!d) {
                    spdlog::error("Datos de {}: {}", choice.path.string(), d.error());
                    continue;
                }
                recorded = std::move(*r);
                data = std::move(*d);
                break;
            }
        }
        const bool is_replay = choice.kind == MenuChoice::Kind::Replay;
        const bool is_load = choice.kind == MenuChoice::Kind::Load;
        try {
            WindowedGame game(*data, *display->window, *display->renderer, is_replay ? &*recorded : nullptr,
                              is_load ? &*recorded : nullptr, net ? &*net : nullptr, display->audio.get(),
                              editing ? &*editing : nullptr);
            game.run(0);
            if (!game.back_to_menu()) {
                return 0;  // ventana cerrada
            }
            // Editor: «Probar» juega el escenario tal como está.
            if (editing && game.play_requested()) {
                auto d = with_scenario(base, *editing, settings);
                if (!d) {
                    spdlog::error("Escenario: {}", d.error());
                    continue;
                }
                WindowedGame trial(*d, *display->window, *display->renderer, nullptr, nullptr, nullptr,
                                   display->audio.get());
                trial.run(0);
                if (!trial.back_to_menu()) {
                    return 0;
                }
            }
        } catch (const std::exception& e) {
            spdlog::error("{}", e.what());  // p. ej. una partida guardada que no se reproduce
        }
    }
}

int run_load(const std::filesystem::path& path, std::int64_t max_frames) {
    const auto save = load_replay(path);
    if (!save) {
        spdlog::error("{}", save.error());
        return 1;
    }
    const auto data = parse_game_data(save->data);
    if (!data) {
        spdlog::error("Datos de la partida guardada: {}", data.error());
        return 1;
    }
    spdlog::info("Cargando {}: {} ticks", path.string(), save->end_tick);
    return run_windowed(*data, max_frames, nullptr, &*save);
}

int run_replay(const std::filesystem::path& path, std::int64_t max_frames) {
    const auto replay = load_replay(path);
    if (!replay) {
        spdlog::error("{}", replay.error());
        return 1;
    }
    const auto data = parse_game_data(replay->data);
    if (!data) {
        spdlog::error("Datos de la repetición: {}", data.error());
        return 1;
    }
    spdlog::info("Repetición {}: {} órdenes, {} ticks", path.string(), replay->commands.size(), replay->end_tick);
    return run_windowed(*data, max_frames, &*replay);
}

}  // namespace rts::game
