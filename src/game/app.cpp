#include "game/app.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
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
#include "game/fixed_step.hpp"
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
constexpr std::uint8_t kOpaque = 255;
// Jugador humano de esta máquina. En LAN (M8) lo asignará la sala de espera.
constexpr sim::PlayerId kLocalPlayer = 0;
constexpr std::int32_t kPercent = 100;

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
    spdlog::info("Uso: rts [--data <carpeta>] [--frames <N>] | [--headless --ticks <N>]");
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
// estado inicial del mundo.
void issue_scenario(const GameData& data, sim::World& world) {
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
                    world.issue(c);
                }
                continue;
            }
        }
        world.issue(std::move(c));
    }
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
    WindowedGame(const GameData& data, platform::Window& window, render::Renderer& renderer)
        : data_(data),
          window_(window),
          renderer_(renderer),
          world_(data.engine.world),
          proj_(data.engine.view),
          clock_({kTickNs, data.engine.loop.max_ticks_per_frame}),
          selection_(data.engine.selection) {
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
                        finish_selection({event.button.x, event.button.y});
                    }
                    break;
                case SDL_EVENT_KEY_DOWN:
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

    [[nodiscard]] sim::TileCoord tile_at(render::Vec2 screen_pos) const {
        const render::Vec2 tile = proj_.world_to_tile(camera_.screen_to_world(screen_pos));
        return {static_cast<std::int32_t>(std::floor(tile.x)), static_cast<std::int32_t>(std::floor(tile.y))};
    }

    [[nodiscard]] const sim::SnapshotObject* find_object(std::uint32_t id) const {
        const auto it = std::ranges::find(curr_.objects, id, &sim::SnapshotObject::id);
        return it == curr_.objects.end() ? nullptr : &*it;
    }

    [[nodiscard]] const sim::SnapshotObject* object_under(sim::TileCoord tile) const {
        const auto id = world_.object_at(tile);
        return id ? find_object(*id) : nullptr;
    }

    // Clic (sin rectángulo) sin unidades debajo: un edificio propio queda seleccionado.
    // Solo se seleccionan unidades propias; se usan las posiciones del último fotograma,
    // que es lo que el jugador veía.
    void finish_selection(render::Vec2 at) {
        const bool click = !selection_.has_visible_rect();
        selection_.end_drag(at, window_.shift_held(), own_screen_entities_);
        selected_building_.reset();
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

    // Clic derecho: recoger si hay un recurso, construir o descargar si es un edificio
    // propio, mover en cualquier otro caso.
    void issue_context_order(render::Vec2 screen_pos) {
        if (selection_.selected().empty()) {
            return;
        }
        const sim::TileCoord tile = tile_at(screen_pos);
        const sim::SnapshotObject* o = object_under(tile);
        sim::Command c = local_command(sim::CommandType::Move);
        if (o != nullptr && o->kind == sim::ObjectKind::Resource) {
            c.type = sim::CommandType::Gather;
            c.object = o->id;
        } else if (o != nullptr && o->kind == sim::ObjectKind::Building && o->owner == kLocalPlayer) {
            c.type = sim::CommandType::Build;
            c.object = o->id;
        } else {
            c.target = tile;
        }
        world_.issue(std::move(c));
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
        world_.issue(std::move(c));
        if (!window_.shift_held()) {
            placing_.reset();
        }
    }

    // Superposiciones de depuración. Leen el registro en solo lectura: son herramientas,
    // no presentación del juego, y no pasan por el snapshot.
    void build_overlays() {
        tints_.clear();
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
        const StepPlan plan = clock_.advance(frame_ns);
        const auto start = SteadyClock::now();
        for (std::int32_t i = 0; i < plan.ticks; ++i) {
            RTS_PROFILE_ZONE_NAMED("sim_tick");
            std::swap(prev_, curr_);
            world_.step();
            world_.write_snapshot(curr_);
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
        ImGui::Separator();
        ImGui::TextDisabled("Arrastre: rectángulo · Mayús: añadir · Esc: limpiar");
        ImGui::TextDisabled("Clic derecho: mover, recoger (recurso), construir o descargar (edificio propio)");
        ImGui::TextDisabled("Flechas/WASD o borde de ventana: desplazar");
        ImGui::End();

        draw_resource_bar(display);
        draw_selection_panel(display);

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
            if (workers > 0) {
                ImGui::SeparatorText("Construir");
                for (std::size_t b = 0; b < data_.buildings.types.size(); ++b) {
                    const BuildingInfo& info = data_.buildings.types[b];
                    ImGui::BeginDisabled(!affordable(stock, info.type.cost));
                    if (ImGui::Button(std::format("{} ({})", info.name, cost_text(info.type.cost)).c_str())) {
                        placing_ = static_cast<sim::BuildingTypeId>(b);
                    }
                    ImGui::EndDisabled();
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
        if (!o->complete) {
            ImGui::ProgressBar(static_cast<float>(o->progress) / static_cast<float>(info.type.build_ticks), {-1.0f, 0.0f},
                               "en obra");
            return;
        }
        if (info.type.trains.empty()) {
            return;
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
        for (const sim::UnitTypeId t : info.type.trains) {
            const UnitInfo& u = data_.units.types[t];
            ImGui::BeginDisabled(full || !affordable(stock, u.type.cost));
            if (ImGui::Button(std::format("{} ({})", u.name, cost_text(u.type.cost)).c_str())) {
                sim::Command c = local_command(sim::CommandType::Train);
                c.units.clear();
                c.object = id;
                c.kind = t;
                world_.issue(std::move(c));
            }
            ImGui::EndDisabled();
        }
        ImGui::BeginDisabled(o->queue.empty());
        if (ImGui::Button("Cancelar la última (reembolso íntegro)")) {
            sim::Command c = local_command(sim::CommandType::CancelTrain);
            c.units.clear();
            c.object = id;
            world_.issue(std::move(c));
        }
        ImGui::EndDisabled();
    }

    // Edificios y recursos del snapshot, más el fantasma de colocación. Los rombos de
    // huellas distintas nunca se solapan, así que no hace falta ordenarlos.
    void build_objects(const std::optional<sim::TileCoord>& hover) {
        const render::ViewParams& view = data_.engine.view;
        objects_.clear();
        for (const sim::SnapshotObject& o : curr_.objects) {
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
                so.body_percent = view.building_body_percent;
                so.highlighted = selected_building_ == o.id;
            }
            objects_.push_back(so);
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

    void present() {
        build_screen_entities();
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
        } else if ((arg == "--ticks" || arg == "--frames") && has_value) {
            const std::string_view value = argv[++i];
            std::int64_t& target = arg == "--ticks" ? options.headless_ticks : options.max_frames;
            if (!parse_count(value, target)) {
                spdlog::error("{} necesita un entero no negativo, recibido '{}'", arg, value);
                return std::nullopt;
            }
        } else {
            spdlog::error("Argumento no reconocido: '{}'", arg);
            print_usage();
            return std::nullopt;
        }
    }
    return options;
}

int run_headless(const GameData& data, std::int64_t ticks) {
    const auto gen_start = SteadyClock::now();
    sim::World world(data.engine.world);
    const double gen_ms = elapsed_ms(gen_start);
    issue_scenario(data, world);

    const auto start = SteadyClock::now();
    for (std::int64_t i = 0; i < ticks; ++i) {
        world.step();
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
        spdlog::info("headless: jugador {}: comida {} madera {} piedra {} oro {}, población {}/{}", p, ps.stock[0],
                     ps.stock[1], ps.stock[2], ps.stock[3], ps.population, ps.population_cap);
    }
    // Formato estable: la CI lo compara entre plataformas.
    spdlog::info("state_hash={:016x}", world.state_hash());
    return 0;
}

int run_windowed(const GameData& data, std::int64_t max_frames) {
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

    WindowedGame game(data, **window, **renderer);
    return game.run(max_frames);
}

}  // namespace rts::game
