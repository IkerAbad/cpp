#include "game/app.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include "game/camera_control.hpp"
#include "game/alerts.hpp"
#include "game/fixed_step.hpp"
#include "game/minimap.hpp"
#include "game/replay.hpp"
#include "game/selection.hpp"
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
            out += std::format("{}{} {}", out.empty() ? "" : ", ", resource_key(static_cast<sim::Resource>(r)), cost[r]);
        }
    }
    return out.empty() ? "gratis" : out;
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
            return "ocioso";
        case sim::WorkerTask::Gather:
            return "recogiendo";
        case sim::WorkerTask::Deliver:
            return "llevando al almacén";
        case sim::WorkerTask::Build:
            return "construyendo";
        case sim::WorkerTask::Demolish:
            return "desmontando";
        case sim::WorkerTask::Nurse:
            return "de enfermero";
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
}

bool parse_count(std::string_view value, std::int64_t& out) {
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), out);
    return ec == std::errc{} && end == value.data() + value.size() && out >= 0;
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
std::filesystem::path auto_replay_path(const GameData& data) {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    std::string stamp;
    try {
        stamp = std::format("{:%Y-%m-%d_%H-%M-%S}", std::chrono::zoned_time{std::chrono::current_zone(), now});
    } catch (const std::exception&) {
        stamp = std::format("{:%Y-%m-%d_%H-%M-%S}Z", now);
    }
    return std::filesystem::path(platform::executable_dir()) / data.engine.replay.directory /
           std::format("partida-{}.rtsrep", stamp);
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

class WindowedGame {
public:
    // Con replay, reproduce esa repetición (sin órdenes); sin ella, partida nueva que
    // se graba sola.
    WindowedGame(const GameData& data, platform::Window& window, render::Renderer& renderer, const Replay* replay)
        : data_(data),
          window_(window),
          renderer_(renderer),
          world_(data.engine.world),
          proj_(data.engine.view),
          // A velocidad xN caben N veces más ticks por fotograma.
          clock_({kTickNs, data.engine.loop.max_ticks_per_frame *
                               (replay != nullptr ? data.engine.replay.speeds.back() : 1)}),
          selection_(data.engine.selection) {
        if (replay != nullptr) {
            player_.emplace(*replay);
            replay_end_ = replay->end_tick;
            view_player_.reset();  // en una repetición se ve todo (se puede elegir la vista de un jugador)
        } else {
            recorder_.emplace(data.files, data.engine.replay.checkpoint_interval_ticks);
            replay_path_ = auto_replay_path(data);
        }
        world_.write_snapshot(curr_);
        prev_ = curr_;
        stats_.state_hash = world_.state_hash();
        // La cámara arranca sobre el primer edificio del jugador local o, sin él, en el
        // centro del mapa (donde aparecen las unidades de prueba).
        const sim::TileMap& map = world_.map();
        render::Vec2 center{static_cast<float>(map.width()) * 0.5f, static_cast<float>(map.height()) * 0.5f};
        for (const sim::SnapshotObject& o : curr_.objects) {
            if (o.kind == sim::ObjectKind::Building && o.owner == kLocalPlayer) {
                const float half = static_cast<float>(o.size) * 0.5f;
                center = {static_cast<float>(o.origin.x) + half, static_cast<float>(o.origin.y) + half};
                break;
            }
        }
        camera_.center_on(proj_.tile_to_world(center), renderer_.screen_size());
    }

    int run(std::int64_t max_frames) {
        std::uint64_t last_ns = platform::now_ns();
        std::int64_t frame = 0;
        for (; max_frames == 0 || frame < max_frames; ++frame) {
            RTS_PROFILE_FRAME();
            if (!handle_events()) {
                break;
            }
            const std::uint64_t now = platform::now_ns();
            const auto frame_ns = static_cast<std::int64_t>(now - last_ns);
            last_ns = now;
            if (window_.minimized()) {
                // Minimizada la simulación sigue su curso; solo se evita presentar.
                SDL_WaitEventTimeout(nullptr, static_cast<std::int32_t>(kTickNs / 1'000'000));
            }
            simulate(frame_ns);
            update_camera(frame_ns);
            if (!window_.minimized()) {
                present();
            }
        }
        spdlog::info("{} fotogramas; medias móviles: escena {:.3f} ms, envío {:.3f} ms (con espera de vsync), "
                     "simulación {:.3f} ms/tick; {} sprites en el último",
                     frame, stats_.scene_ms, stats_.submit_ms, stats_.sim_ms_per_tick, renderer_.sprites_last_frame());
        if (recorder_ && world_.tick() > 0) {
            if (const auto saved = save_replay(replay_path_, recorder_->finish(world_)); saved) {
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
                        if (event.key.scancode == SDL_SCANCODE_SPACE) {
                            jump_to_alert();
                        }
                    }
                    if (event.key.scancode == SDL_SCANCODE_F1) {
                        show_debug_ = !show_debug_;
                    }
                    if (event.key.scancode == SDL_SCANCODE_F2) {
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
            if (it != curr_.entities.end() && it->owner == kLocalPlayer) {
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
    void issue(sim::Command c) {
        if (recorder_) {
            recorder_->issue(world_, std::move(c));
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
        const auto id = world_.object_at(tile);
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
            if (o != nullptr && o->kind == sim::ObjectKind::Building && o->owner == kLocalPlayer) {
                selected_building_ = o->id;
            }
        }
    }

    [[nodiscard]] sim::Command local_command(sim::CommandType type) const {
        // Se aplica al inicio del siguiente tick; en lockstep (M8) se programará unos
        // ticks más tarde para absorber la latencia.
        sim::Command c;
        c.tick = world_.tick();
        c.player = kLocalPlayer;
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
            if (se.owner == kLocalPlayer || !sees_entity(se)) {
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
        } else if (o != nullptr && o->kind == sim::ObjectKind::Building && o->owner != kLocalPlayer) {
            c.type = sim::CommandType::Attack;
            c.object = o->id;
        } else if (o != nullptr && o->kind == sim::ObjectKind::Resource) {
            c.type = sim::CommandType::Gather;
            c.object = o->id;
        } else if (o != nullptr && o->kind == sim::ObjectKind::Building && o->owner == kLocalPlayer &&
                   window_.shift_held()) {
            c.type = sim::CommandType::Demolish;  // Mayús: desmontarlo (deja escombros recuperables)
            c.object = o->id;
        } else if (o != nullptr && o->kind == sim::ObjectKind::Building && o->owner == kLocalPlayer && o->fire > 0) {
            c.type = sim::CommandType::Extinguish;  // edificio propio en llamas: apagarlo
            c.object = o->id;
        } else if (o != nullptr && o->kind == sim::ObjectKind::Building && o->owner == kLocalPlayer && o->complete &&
                   data_.buildings.types[o->type].type.farm_food > 0 && !o->burned &&
                   o->hp >= data_.buildings.types[o->type].type.hp) {
            c.type = sim::CommandType::Gather;  // granja propia terminada e intacta: cultivarla
            c.object = o->id;
        } else if (o != nullptr && o->kind == sim::ObjectKind::Building && o->owner == kLocalPlayer) {
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
        const sim::MovementSystem& mv = world_.movement();
        const auto& reg = world_.registry();
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
            const sim::TileMap& map = world_.map();
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
        std::int64_t sim_ns = frame_ns;
        if (player_) {
            const bool stopped = paused_ || player_->finished(world_);
            sim_ns = stopped ? 0 : frame_ns * data_.engine.replay.speeds[speed_index_];
        }
        const StepPlan plan = clock_.advance(sim_ns);
        const auto start = SteadyClock::now();
        for (std::int32_t i = 0; i < plan.ticks; ++i) {
            RTS_PROFILE_ZONE_NAMED("sim_tick");
            if (player_ && player_->finished(world_)) {
                break;
            }
            std::swap(prev_, curr_);
            if (player_) {
                player_->before_step(world_);
            }
            world_.step();
            if (player_) {
                player_->after_step(world_);
            } else {
                recorder_->after_step(world_);
            }
            world_.write_snapshot(curr_);
            if (!player_) {
                alerts_.update(prev_, curr_, kLocalPlayer);
            }
        }
        if (plan.ticks > 0) {
            const double per_tick = elapsed_ms(start) / plan.ticks;
            stats_.sim_ms_per_tick += kSmoothing * (per_tick - stats_.sim_ms_per_tick);
            stats_.state_hash = world_.state_hash();
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
        const sim::TileMap& map = world_.map();
        camera_.clamp_to_map(proj_, map.width(), map.height(), screen);
    }

    // Interpola cada marcador entre los dos últimos ticks y lo proyecta a pantalla.
    void build_screen_entities() {
        screen_entities_.clear();
        screen_index_.clear();
        const bool can_interpolate = prev_.entities.size() == curr_.entities.size();
        const auto a = static_cast<float>(stats_.alpha);
        for (std::size_t i = 0; i < curr_.entities.size(); ++i) {
            const sim::SnapshotEntity& c = curr_.entities[i];
            render::Vec2 tile{fixed_to_float(c.pos.x), fixed_to_float(c.pos.y)};
            if (can_interpolate && prev_.entities[i].id == c.id) {
                const render::Vec2 p{fixed_to_float(prev_.entities[i].pos.x), fixed_to_float(prev_.entities[i].pos.y)};
                tile = p + (tile - p) * a;
            }
            screen_entities_.push_back({c.id, camera_.world_to_screen(proj_.tile_to_world(tile))});
        }
        own_screen_entities_.clear();
        for (std::size_t i = 0; i < curr_.entities.size(); ++i) {
            if (curr_.entities[i].owner == kLocalPlayer) {
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
        draw_resource_bar(display);
        draw_selection_panel(display);
        draw_outcome(display);
        draw_replay_panel(display);
        draw_help(display);
        draw_alerts(display);
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
        ImGui::Begin("Depuración", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Text("Backend GPU: %s", renderer_.driver_name());
        ImGui::Text("Fotograma: %.2f ms (%.0f FPS)", stats_.frame_ms,
                    stats_.frame_ms > 0.0 ? 1000.0 / stats_.frame_ms : 0.0);
        ImGui::Text("Escena: %.3f ms · envío: %.3f ms (incluye vsync)", stats_.scene_ms, stats_.submit_ms);
        ImGui::Text("Sprites: %zu (%d casillas, %d objetos, %d marcadores), 1 llamada de dibujo",
                    renderer_.sprites_last_frame(), stats_.scene.tiles_drawn, stats_.scene.objects_drawn,
                    stats_.scene.markers_drawn);
        ImGui::Separator();
        ImGui::Text("Tick: %u", curr_.tick);
        ImGui::Text("Ticks en este fotograma: %d", stats_.ticks_this_frame);
        ImGui::Text("Simulación: %.3f ms/tick (media)", stats_.sim_ms_per_tick);
        ImGui::Text("Ticks descartados (total): %lld", static_cast<long long>(stats_.dropped_ticks_total));
        ImGui::Text("alpha: %.3f", stats_.alpha);
        ImGui::Text("Hash de estado: %016llx", static_cast<unsigned long long>(stats_.state_hash));
        ImGui::Separator();
        const sim::MovementTickStats& mv = world_.movement().last_stats();
        ImGui::Text("Movimiento: %d en marcha · %d caminos resueltos, %d pendientes", mv.moving_units, mv.paths_solved,
                    mv.paths_pending);
        ImGui::Text("Nodos expandidos en el último tick: %lld · campos de flujo: %d",
                    static_cast<long long>(mv.nodes_expanded), mv.flow_fields_built);
        ImGui::Text("HPA*: %zu nodos, %zu aristas · sectores rehechos en el último tick: %d",
                    world_.movement().hpa().node_count(), world_.movement().hpa().edge_count(), mv.sectors_rebuilt);
        ImGui::Checkbox("Portales HPA*", &show_portals_);
        ImGui::SameLine();
        ImGui::Checkbox("Ruta", &show_paths_);
        ImGui::SameLine();
        ImGui::Checkbox("Campo de flujo", &show_flow_);
        const sim::CombatTickStats& cb = world_.combat().last_stats();
        ImGui::Text("Combate: %d golpes, %d proyectiles (%d aciertos, %d fallos), %d bajas en el último tick",
                    cb.melee_hits, cb.projectiles_fired, cb.projectiles_hit, cb.projectiles_missed, cb.kills);
        ImGui::End();

        // Arriba a la derecha, anclado por su esquina superior derecha.
        ImGui::SetNextWindowPos({display.x - kPanelMarginPx, kPanelMarginPx}, ImGuiCond_FirstUseEver, {1.0f, 0.0f});
        ImGui::Begin("Casilla", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        if (hover && curr_.map) {
            const sim::TileMap& map = *curr_.map;
            const sim::TerrainId id = map.terrain(*hover);
            const TerrainInfo& info = data_.terrain.types[id];
            ImGui::Text("(%d, %d)", hover->x, hover->y);
            ImGui::Text("Terreno: %s (id %u)", info.name.c_str(), static_cast<unsigned>(id));
            ImGui::Text("Transitable: %s", info.passable ? "sí" : "no");
            ImGui::Text("Altura: %u", static_cast<unsigned>(map.elevation(*hover)));
            if (const sim::SnapshotObject* o = object_under(*hover)) {
                if (o->kind == sim::ObjectKind::Resource) {
                    const NodeInfo& n = data_.nodes.types[o->type];
                    ImGui::Text("%s: quedan %d de %s", n.name.c_str(), o->amount,
                                std::string(resource_key(n.type.kind)).c_str());
                } else {
                    ImGui::Text("%s del jugador %u", data_.buildings.types[o->type].name.c_str(),
                                static_cast<unsigned>(o->owner));
                }
            }
        } else {
            ImGui::TextDisabled("Pasa el ratón sobre el mapa");
        }
        ImGui::End();
    }

    // Recursos no nulos de un almacén o una carga, p. ej. "comida 21 · madera 6".
    [[nodiscard]] static std::string stock_text(const sim::Stock& s) {
        std::string out;
        for (std::size_t r = 0; r < sim::kResourceCount; ++r) {
            if (s[r] != 0) {
                out += std::format("{}{} {}", out.empty() ? "" : " · ", resource_key(static_cast<sim::Resource>(r)), s[r]);
            }
        }
        return out.empty() ? std::string("nada") : out;
    }

    [[nodiscard]] static const char* convoy_text(sim::ConvoyTask t) {
        switch (t) {
            case sim::ConvoyTask::Load:
                return "va a cargar";
            case sim::ConvoyTask::Unload:
                return "lleva la carga al campamento";
            case sim::ConvoyTask::Idle:
                break;
        }
        return "parada: abastece a las tropas de alrededor";
    }

    [[nodiscard]] bool out_of_ammo(const sim::SnapshotEntity& e) const {
        return data_.units.types[e.type].type.supply.ammo > 0 && e.ammo <= 0;
    }

    // Víveres y munición de una unidad propia (lo que el jugador sabe de las suyas).
    void draw_supply_text(const sim::SnapshotEntity& e) const {
        const sim::SupplyStats& st = data_.units.types[e.type].type.supply;
        if (st.rations > 0) {
            ImGui::Text("víveres %d/%d", e.rations, st.rations);
            if (st.ammo > 0) {
                ImGui::SameLine();
            }
        }
        if (st.ammo > 0) {
            ImGui::Text("munición %d/%d", e.ammo, st.ammo);
        }
        if (e.hungry) {
            ImGui::TextColored(kWarnColor, "con hambre: ataca y trabaja peor%s",
                               st.starves ? "; acabará perdiendo vida" : "");
        }
        if (out_of_ammo(e)) {
            ImGui::TextColored(kWarnColor, "sin munición: no puede disparar");
        }
    }

    // Estado sanitario de una unidad.
    void draw_care_text(const sim::SnapshotEntity& e) const {
        if (e.admitted) {
            ImGui::TextColored({0.6f, 0.85f, 1.0f, 1.0f}, "ingresado en un puesto médico: no combate");
        } else if (e.care_post != sim::kNoObject) {
            ImGui::TextDisabled("herido, camino del puesto médico");
        }
        if (e.reorganizing) {
            ImGui::TextColored(kWarnColor, "reorganizándose tras el alta: aún no ataca");
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
            ImGui::TextDisabled("F2: ayuda · F1: depuración");
            ImGui::End();
            return;
        }
        ImGui::SetNextWindowPos({display.x - kPanelMarginPx, display.y - kPanelMarginPx}, ImGuiCond_FirstUseEver,
                                {1.0f, 1.0f});
        ImGui::Begin("Ayuda (F2)", &show_help_, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::SeparatorText("Ratón");
        ImGui::BulletText("Clic o arrastre: seleccionar tus unidades (Mayús: añadir)");
        ImGui::BulletText("Clic en un edificio tuyo: ver su panel y entrenar unidades");
        ImGui::BulletText("Clic derecho en el suelo: mover");
        ImGui::BulletText("Clic derecho en un enemigo: atacar (sin asedio, a un edificio se le prende fuego)");
        ImGui::BulletText("Clic derecho en un recurso o granja: recoger (aldeanos)");
        ImGui::BulletText("Clic derecho en un edificio tuyo: construir, reparar o descargar");
        ImGui::BulletText("Clic derecho en un edificio tuyo en llamas: apagarlo");
        ImGui::BulletText("Ctrl + clic derecho: avanzar atacando lo que salga");
        ImGui::BulletText("Mayús + clic derecho en un edificio tuyo: desmontarlo");
        ImGui::BulletText("Mayús + clic derecho en el suelo: añadir un punto de paso");
        ImGui::BulletText("Edificio seleccionado + clic derecho: punto de reunión");
        ImGui::BulletText("Doble clic en una unidad: todas las de su tipo en pantalla");
        ImGui::SeparatorText("Teclado");
        ImGui::BulletText("Flechas, WASD o borde de la ventana: mover la cámara");
        ImGui::BulletText("Esc: cancelar colocación o soltar la selección");
        ImGui::BulletText("Ctrl + 1-9: guardar grupo; 1-9: seleccionarlo");
        ImGui::BulletText("Espacio: ir al último aviso");
        ImGui::BulletText("F1: datos de depuración · F2: esta ayuda");
        ImGui::SeparatorText("Leyenda");
        ImGui::BulletText("Cada unidad lleva su inicial; el borde es el color del jugador");
        ImGui::BulletText("Punto de color: lo que lleva un aldeano");
        ImGui::BulletText("Inicial en rojo: con hambre o sin munición");
        ImGui::SeparatorText("Logística");
        ImGui::BulletText("Las tropas gastan víveres; los tiradores, munición");
        ImGui::BulletText("Se reponen junto al centro urbano, el molino, el cuartel o un campamento");
        ImGui::BulletText("Cada ración cuesta comida; la munición, madera (y hierro)");
        ImGui::BulletText("Campamento: almacén avanzado que llenan acémilas y carretas");
        ImGui::BulletText("Bagaje + clic derecho en un campamento: ruta de convoy");
        ImGui::BulletText("Bagaje cargado y parado: abastece a las tropas de alrededor");
        ImGui::BulletText("Trabuquete: se monta en un campamento con lo traído en convoy");
        ImGui::SeparatorText("Sanidad");
        ImGui::BulletText("Heridos + clic derecho en un puesto médico: ingresan");
        ImGui::BulletText("Aldeanos o cirujanos + clic derecho en él: atienden (el cirujano cura mejor)");
        ImGui::BulletText("Socorro estabiliza; hospital de campaña y hospital curan del todo");
        ImGui::BulletText("Solo lo leve (%d %% de vida o más) sana solo; si cae el puesto, mueren",
                          data_.engine.world.medicine.light_wound_percent);
        ImGui::SeparatorText("Niebla de guerra");
        ImGui::BulletText("Negro: sin explorar; oscuro: explorado, sin vista ahora");
        ImGui::BulletText("Los árboles tapan la vista; desde una loma se ve más lejos");
        ImGui::BulletText("De noche se ve la mitad; los edificios enemigos se recuerdan");
        ImGui::BulletText("Pasa el ratón sobre algo para ver qué es");
        ImGui::End();
    }

    // Texto del dueño visto por el jugador local.
    [[nodiscard]] static std::string owner_text(sim::PlayerId owner) {
        return owner == kLocalPlayer ? std::string("tuyo") : std::format("enemigo (jugador {})", owner);
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
            ImGui::Text("%s · %s", u.name.c_str(), owner_text(best->owner).c_str());
            ImGui::Text("vida %d/%d · nivel %d", best->hp, best->max_hp, best->level);
            if (best->owner == kLocalPlayer) {
                draw_supply_text(*best);
            }
            draw_care_text(*best);
            if (best->carried > 0) {
                ImGui::Text("lleva %d de %s", best->carried, std::string(resource_key(best->carry_kind)).c_str());
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
            ImGui::Text("%s: quedan %d de %s", n.name.c_str(), o->amount,
                        std::string(resource_key(n.type.kind)).c_str());
        } else {
            const BuildingInfo& b = data_.buildings.types[o->type];
            ImGui::Text("%s · %s", b.name.c_str(), owner_text(o->owner).c_str());
            ImGui::Text("vida %d/%d%s", o->hp, b.type.hp, o->complete ? "" : " · en obra");
            if (o->fire > 0) {
                ImGui::TextColored({1.0f, 0.5f, 0.1f, 1.0f}, "en llamas");
            } else if (o->burned) {
                ImGui::TextColored({0.8f, 0.6f, 0.4f, 1.0f}, "quemado: no funciona");
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
        ImGui::Begin("Mapa", nullptr,
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
                return "¡Te atacan!";
            case AlertKind::Fire:
                return "¡Un edificio arde!";
            case AlertKind::Hunger:
                return "Tus tropas pasan hambre";
            case AlertKind::NoFood:
                return "Sin comida para las raciones";
            case AlertKind::UnitReady:
                return "Unidad lista";
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
        ImGui::Begin("Avisos", nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground);
        for (const Alert& a : shown) {
            const bool urgent = a.kind != AlertKind::UnitReady;
            ImGui::TextColored(urgent ? kWarnColor : ImVec4{0.7f, 0.9f, 0.7f, 1.0f}, "%s", alert_text(a.kind));
        }
        ImGui::TextDisabled("Espacio: ir allí");
        ImGui::End();
    }

    // Espacio: la cámara al último aviso que tiene lugar.
    void jump_to_alert() {
        for (const Alert& a : alerts_.shown(curr_.tick)) {
            if (a.where.x >= 0) {
                const render::Vec2 world = proj_.tile_to_world(
                    {static_cast<float>(a.where.x) + 0.5f, static_cast<float>(a.where.y) + 0.5f});
                const render::Vec2 screen = renderer_.screen_size();
                camera_.origin = world - screen * 0.5f;
                camera_.clamp_to_map(proj_, world_.map().width(), world_.map().height(), screen);
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
        constexpr ImU32 kText = IM_COL32(255, 255, 255, 255);
        constexpr float kPadPx = 1.0f;
        const auto radius = static_cast<float>(data_.engine.view.marker_radius_px);
        for (std::size_t i = 0; i < screen_entities_.size(); ++i) {
            const sim::SnapshotEntity& se = curr_.entities[screen_index_[i]];
            const std::string& label = data_.units.types[se.type].label;
            const bool warn = se.owner == kLocalPlayer && (se.hungry || out_of_ammo(se));
            const ImVec2 size = ImGui::CalcTextSize(label.c_str());
            const render::Vec2 p = screen_entities_[i].pos;
            const ImVec2 at{std::round(p.x + radius + kPadPx * 2.0f), std::round(p.y - size.y * 0.5f)};
            draw->AddRectFilled({at.x - kPadPx, at.y}, {at.x + size.x + kPadPx, at.y + size.y}, warn ? kBackWarn : kBack);
            draw->AddText(at, kText, label.c_str());
        }
    }

    // Reproductor: tiempo, pausa, velocidad y estado de la verificación en curso.
    void draw_replay_panel(const ImVec2& display) {
        if (!player_) {
            return;
        }
        ImGui::SetNextWindowPos({display.x * 0.5f, display.y - kPanelMarginPx}, ImGuiCond_FirstUseEver, {0.5f, 1.0f});
        ImGui::Begin("Repetición", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        const auto clock_text = [](sim::Tick t) {
            const auto seconds = t / static_cast<sim::Tick>(sim::kTicksPerSecond);
            constexpr sim::Tick kSecondsPerMinute = 60;
            return std::format("{}:{:02}", seconds / kSecondsPerMinute, seconds % kSecondsPerMinute);
        };
        ImGui::Text("%s / %s", clock_text(world_.tick()).c_str(), clock_text(replay_end_).c_str());
        ImGui::SameLine();
        if (ImGui::Button(paused_ ? "Reanudar" : "Pausa")) {
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
        ImGui::TextUnformatted("Vista:");
        ImGui::SameLine();
        if (ImGui::RadioButton("todo", !view_player_)) {
            view_player_.reset();
        }
        for (std::size_t p = 0; p < curr_.players.size(); ++p) {
            ImGui::SameLine();
            const auto id = static_cast<sim::PlayerId>(p);
            if (ImGui::RadioButton(std::format("jugador {}", p).c_str(), view_player_ == id)) {
                view_player_ = id;
            }
        }
        if (player_->diverged()) {
            ImGui::TextColored({1.0f, 0.35f, 0.3f, 1.0f}, "Divergencia en el tick %u: la partida ya no es la grabada",
                               player_->diverged_at());
        } else {
            ImGui::TextDisabled("%zu comprobaciones de hash correctas%s", player_->checkpoints_checked(),
                                player_->finished(world_) ? " · fin" : "");
        }
        ImGui::TextDisabled("Espacio: pausa · 1-%zu: velocidad · sin órdenes", speeds.size());
        ImGui::End();
    }

    // Victoria o derrota: el jugador local sin unidades ni edificios pierde; si todos los
    // demás que llegaron a tener algo lo han perdido todo, gana.
    void draw_outcome(const ImVec2& display) {
        if (curr_.players.size() <= kLocalPlayer) {
            return;
        }
        bool won = false;
        const bool lost = curr_.players[kLocalPlayer].defeated;
        if (!lost) {
            bool any_rival = false;
            bool all_defeated = true;
            for (std::size_t p = 0; p < curr_.players.size(); ++p) {
                if (p == kLocalPlayer || !curr_.players[p].started) {
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
        ImGui::SetNextWindowPos({display.x * 0.5f, display.y * 0.35f}, ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::Begin("Resultado", nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove);
        constexpr float kOutcomeScale = 3.0f;  // letra del cartel: tres veces la normal
        ImGui::SetWindowFontScale(kOutcomeScale);
        ImGui::TextColored(won ? ImVec4{0.4f, 1.0f, 0.4f, 1.0f} : ImVec4{1.0f, 0.35f, 0.3f, 1.0f}, "%s",
                           won ? "¡Victoria!" : "Derrota");
        ImGui::End();
    }

    // Barra superior: recursos y población del jugador local.
    void draw_resource_bar(const ImVec2& display) {
        if (curr_.players.size() <= kLocalPlayer) {
            return;
        }
        const sim::PlayerState& ps = curr_.players[kLocalPlayer];
        ImGui::SetNextWindowPos({display.x * 0.5f, kPanelMarginPx}, ImGuiCond_Always, {0.5f, 0.0f});
        ImGui::Begin("Recursos", nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove);
        for (std::size_t r = 0; r < sim::kResourceCount; ++r) {
            const render::Rgba& c = data_.engine.view.resource_colors[r];
            constexpr float kChannelMax = 255.0f;
            ImGui::TextColored({static_cast<float>(c[0]) / kChannelMax, static_cast<float>(c[1]) / kChannelMax,
                                static_cast<float>(c[2]) / kChannelMax, 1.0f},
                               "%s %d",
                               std::string(resource_key(static_cast<sim::Resource>(r))).c_str(), ps.stock[r]);
            ImGui::SameLine();
        }
        ImGui::Text("· población %d/%d", ps.population, ps.population_cap);
        ImGui::End();
    }

    // Panel inferior: menú de construcción con aldeanos seleccionados, o la cola del
    // edificio seleccionado.
    void draw_selection_panel(const ImVec2& display) {
        // Anclado siempre por la esquina inferior izquierda: crece hacia arriba.
        ImGui::SetNextWindowPos({kPanelMarginPx, display.y - kPanelMarginPx}, ImGuiCond_Always, {0.0f, 1.0f});
        ImGui::Begin("Selección", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        const sim::Stock stock =
            curr_.players.size() > kLocalPlayer ? curr_.players[kLocalPlayer].stock : sim::Stock{};
        if (selected_building_) {
            draw_building_panel(*selected_building_, stock);
        } else if (!selection_.selected().empty()) {
            std::int32_t workers = 0;
            for (const std::uint32_t id : selection_.selected()) {
                const auto it = std::ranges::find(curr_.entities, id, &sim::SnapshotEntity::id);
                if (it != curr_.entities.end() && data_.units.types[it->type].type.worker) {
                    ++workers;
                    if (selection_.selected().size() == 1) {
                        ImGui::Text("Aldeano: %s, lleva %d de %s", task_name(it->task), it->carried,
                                    std::string(resource_key(it->carry_kind)).c_str());
                    }
                }
            }
            ImGui::Text("%zu unidades seleccionadas (%d aldeanos)", selection_.selected().size(), workers);
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
                ImGui::TextColored(kWarnColor, "%d con hambre · %d sin munición: llévalas junto a un edificio que abastezca",
                                   hungry, no_ammo);
            }
            if (selection_.selected().size() == 1) {
                const auto it = std::ranges::find(curr_.entities, selection_.selected().front(), &sim::SnapshotEntity::id);
                if (it != curr_.entities.end()) {
                    const UnitInfo& u = data_.units.types[it->type];
                    ImGui::Text("%s · vida %d/%d · nivel %d (%d de experiencia)", u.name.c_str(), it->hp, it->max_hp,
                                it->level, it->xp);
                    draw_supply_text(*it);
                    draw_care_text(*it);
                    if (u.type.convoy_capacity > 0) {
                        ImGui::Text("carga %s (de %d)", stock_text(it->load).c_str(), u.type.convoy_capacity);
                        ImGui::TextDisabled("%s", convoy_text(it->convoy));
                        ImGui::TextDisabled("Clic derecho en un campamento: ruta de convoy; en otro almacén: cargar");
                    }
                    if (it->hero_name >= 0 && !data_.engine.hero_names.empty()) {
                        const auto n = static_cast<std::size_t>(it->hero_name) % data_.engine.hero_names.size();
                        ImGui::TextColored({1.0f, 0.8f, 0.2f, 1.0f}, "Héroe: %s", data_.engine.hero_names[n].c_str());
                    }
                }
            }
            if (ImGui::Button("Agresiva")) {
                sim::Command c = local_command(sim::CommandType::SetStance);
                c.kind = static_cast<std::uint8_t>(sim::Stance::Aggressive);
                issue(std::move(c));
            }
            ImGui::SameLine();
            if (ImGui::Button("Mantener posición")) {
                sim::Command c = local_command(sim::CommandType::SetStance);
                c.kind = static_cast<std::uint8_t>(sim::Stance::HoldGround);
                issue(std::move(c));
            }
            if (workers > 0) {
                ImGui::SeparatorText("Construir");
                for (std::size_t b = 0; b < data_.buildings.types.size(); ++b) {
                    const BuildingInfo& info = data_.buildings.types[b];
                    const auto type = static_cast<sim::BuildingTypeId>(b);
                    const bool allowed = world_.meets_requirements(kLocalPlayer, type);
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
                        ImGui::TextDisabled("requiere %s", needs.c_str());
                    }
                }
                if (placing_) {
                    ImGui::TextDisabled("Clic: colocar · Mayús+clic: varios · clic derecho o Esc: cancelar");
                }
            }
        } else {
            ImGui::TextDisabled("Nada seleccionado");
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
        ImGui::Text("%s · vida %d/%d", info.name.c_str(), o->hp, info.type.hp);
        if (info.type.beds > 0) {
            std::int32_t patients = 0;
            std::int32_t nurses = 0;
            for (const sim::SnapshotEntity& e : curr_.entities) {
                patients += e.admitted && e.care_post == id ? 1 : 0;
            }
            // Personal asignado (enfermeros y cirujanos; cuidan los que están a su lado).
            for (const sim::SnapshotEntity& e : curr_.entities) {
                nurses += e.owner == kLocalPlayer && e.tending && e.work_building == id ? 1 : 0;
            }
            ImGui::Text("camas %d/%d · personal %d/%d · cura hasta el %d %%", patients, info.type.beds, nurses,
                        info.type.nurses, info.type.heal_to_percent);
            ImGui::TextDisabled("Clic derecho con heridos: ingresan; con aldeanos o cirujanos: atienden");
        }
        if (info.type.store_capacity > 0) {
            ImGui::Text("suministros %s (de %d)", stock_text(o->store).c_str(), info.type.store_capacity);
            ImGui::TextDisabled("Se llena con acémilas o carretas: clic derecho sobre él con ellas");
        } else if (info.type.supplies) {
            ImGui::TextDisabled("Abastece a las tropas cercanas con víveres y munición");
        }
        if (o->fire > 0) {
            ImGui::TextColored({1.0f, 0.5f, 0.1f, 1.0f}, "En llamas (%d %%): clic derecho con unidades para apagarlo",
                               o->fire * kPercent / data_.engine.world.fire.max_intensity);
        } else if (o->burned) {
            ImGui::TextColored({0.8f, 0.6f, 0.4f, 1.0f}, "Quemado: no funciona hasta que lo reparen aldeanos");
        } else if (o->complete && o->hp < info.type.hp) {
            ImGui::TextDisabled("Dañado: clic derecho con aldeanos para repararlo (cuesta madera)");
        }
        ImGui::TextDisabled("Mayús + clic derecho con aldeanos: desmontarlo (deja escombros)");
        if (!o->complete) {
            ImGui::ProgressBar(static_cast<float>(o->progress) / static_cast<float>(info.type.build_ticks), {-1.0f, 0.0f},
                               "en obra");
            return;
        }
        if (info.type.trains.empty()) {
            return;
        }
        if (o->rally.x >= 0) {
            ImGui::Text("Punto de reunión: (%d, %d)", o->rally.x, o->rally.y);
            ImGui::SameLine();
            if (ImGui::SmallButton("Quitar")) {
                sim::Command c = local_command(sim::CommandType::SetRally);
                c.units.clear();
                c.object = id;
                c.kind = sim::kClearRally;
                issue(std::move(c));
            }
        } else {
            ImGui::TextDisabled("Clic derecho en el mapa: punto de reunión");
        }
        ImGui::SeparatorText("Producción");
        for (std::size_t i = 0; i < o->queue.size(); ++i) {
            const UnitInfo& u = data_.units.types[o->queue[i]];
            if (i == 0) {
                ImGui::ProgressBar(static_cast<float>(o->queue_progress) / static_cast<float>(u.type.train_ticks),
                                   {-1.0f, 0.0f}, u.name.c_str());
            } else {
                ImGui::BulletText("%s", u.name.c_str());
            }
        }
        const bool full = std::cmp_greater_equal(o->queue.size(), data_.engine.world.economy.queue_capacity);
        // Un campamento paga con su propio almacén (lo traído en convoy).
        const sim::Stock& pays = info.type.store_capacity > 0 ? o->store : stock;
        if (info.type.store_capacity > 0) {
            ImGui::TextDisabled("Se paga con el almacén del campamento");
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
        if (ImGui::Button("Cancelar la última (reembolso íntegro)")) {
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
            const bool can_pay = curr_.players.size() > kLocalPlayer &&
                                 affordable(curr_.players[kLocalPlayer].stock, info.type.cost);
            render::SceneObject ghost;
            ghost.origin = origin;
            ghost.size = info.type.size;
            ghost.body = can_pay && world_.can_place(*placing_, origin) ? view.ghost_valid_color : view.ghost_invalid_color;
            objects_.push_back(ghost);
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
            } else {
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

    void present() {
        build_screen_entities();
        selection_.retain(own_screen_entities_);
        renderer_.begin_frame();

        markers_.clear();
        for (std::size_t i = 0; i < screen_entities_.size(); ++i) {
            const ScreenEntity& e = screen_entities_[i];
            const sim::SnapshotEntity& se = curr_.entities[screen_index_[i]];
            render::Marker m;
            m.screen_pos = e.pos;
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

        render::Scene scene;
        scene.map = curr_.map.get();
        scene.camera = camera_;
        scene.screen = renderer_.screen_size();
        scene.objects = objects_;
        scene.markers = markers_;
        scene.hovered_tile = hover;
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
    platform::Window& window_;
    render::Renderer& renderer_;
    sim::World world_;
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
    std::filesystem::path replay_path_;
    std::optional<ReplayPlayer> player_;
    sim::Tick replay_end_ = 0;
    bool paused_ = false;
    std::size_t speed_index_ = 0;
    // Jugador cuya vista se muestra (niebla de guerra); sin valor, todo.
    std::optional<sim::PlayerId> view_player_ = kLocalPlayer;
    std::vector<sim::SnapshotObject> remembered_objects_;  // edificios recordados, no vistos ahora
    std::array<std::vector<std::uint32_t>, 9> groups_;  // grupos de control 1…9
    AlertTracker alerts_{data_.engine.alerts};
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
        } else if (arg.ends_with(".rtsrep")) {
            options.replay = argv[i];  // arrastrar una repetición sobre el ejecutable
        } else {
            spdlog::error("Argumento no reconocido: '{}'", arg);
            print_usage();
            return std::nullopt;
        }
    }
    return options;
}

int run_headless(const GameData& data, std::int64_t ticks, const std::filesystem::path& record) {
    const auto gen_start = SteadyClock::now();
    sim::World world(data.engine.world);
    const double gen_ms = elapsed_ms(gen_start);
    std::optional<ReplayRecorder> recorder;
    if (!record.empty()) {
        recorder.emplace(data.files, data.engine.replay.checkpoint_interval_ticks);
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

int run_windowed(const GameData& data, std::int64_t max_frames, const Replay* replay) {
    const WindowConfig& wc = data.engine.window;
    auto window = platform::Window::create({wc.title, wc.width, wc.height});
    if (!window) {
        spdlog::error("{}", window.error());
        return 1;
    }
    render::RendererDesc desc;
    desc.window = (*window)->handle();
    desc.vsync = wc.vsync;
    desc.view = data.engine.view;
    for (const TerrainInfo& t : data.terrain.types) {
        desc.terrain_colors.push_back(t.color);
    }
    auto renderer = render::Renderer::create(desc);
    if (!renderer) {
        spdlog::error("{}", renderer.error());
        return 1;
    }
    spdlog::info("Backend GPU: {}", (*renderer)->driver_name());

    WindowedGame game(data, **window, **renderer, replay);
    return game.run(max_frames);
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
