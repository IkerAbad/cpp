#include "game/app.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
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

// Convierte las órdenes del guion en órdenes de la simulación: [first, first + count)
// indexa las unidades en orden de creación, que es el orden del snapshot inicial.
void issue_scenario(sim::World& world, const std::vector<ScenarioOrder>& orders) {
    sim::Snapshot snap;
    world.write_snapshot(snap);
    for (const ScenarioOrder& o : orders) {
        sim::Command c;
        c.tick = o.tick;
        c.type = sim::CommandType::Move;
        c.target = o.target;
        const auto end = std::min<std::size_t>(snap.entities.size(), static_cast<std::size_t>(o.first + o.count));
        for (auto i = static_cast<std::size_t>(o.first); i < end; ++i) {
            c.units.push_back(snap.entities[i].id);
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
        // La cámara arranca centrada en el mapa, donde están los marcadores de prueba.
        const sim::TileMap& map = world_.map();
        const render::Vec2 center{static_cast<float>(map.width()) * 0.5f, static_cast<float>(map.height()) * 0.5f};
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
                        selection_.begin_drag({event.button.x, event.button.y});
                    } else if (event.button.button == SDL_BUTTON_RIGHT && !ui_mouse) {
                        issue_move_to({event.button.x, event.button.y});
                    }
                    break;
                case SDL_EVENT_MOUSE_MOTION:
                    selection_.update_drag({event.motion.x, event.motion.y});
                    break;
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        // Se selecciona sobre las posiciones del último fotograma: lo que el jugador veía.
                        selection_.end_drag({event.button.x, event.button.y}, window_.shift_held(), screen_entities_);
                    }
                    break;
                case SDL_EVENT_KEY_DOWN:
                    if (event.key.scancode == SDL_SCANCODE_ESCAPE && !renderer_.ui_wants_keyboard()) {
                        selection_.clear();
                    }
                    break;
                default:
                    break;
            }
        }
        return true;
    }

    // Orden de mover a la casilla bajo el cursor. Se aplica al inicio del siguiente tick;
    // en lockstep (M8) se programará unos ticks más tarde para absorber la latencia.
    void issue_move_to(render::Vec2 screen_pos) {
        if (selection_.selected().empty()) {
            return;
        }
        const render::Vec2 tile = proj_.world_to_tile(camera_.screen_to_world(screen_pos));
        sim::Command c;
        c.tick = world_.tick();
        c.type = sim::CommandType::Move;
        c.units = selection_.selected();
        c.target = {static_cast<std::int32_t>(std::floor(tile.x)), static_cast<std::int32_t>(std::floor(tile.y))};
        world_.issue(std::move(c));
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
        screen_types_.clear();
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
            screen_types_.push_back(curr_.entities[i].type);
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
        ImGui::Text("Sprites: %zu (%d casillas, %d marcadores), 1 llamada de dibujo", renderer_.sprites_last_frame(),
                    stats_.scene.tiles_drawn, stats_.scene.markers_drawn);
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
        ImGui::Text("HPA*: %zu nodos, %zu aristas", world_.movement().hpa().node_count(),
                    world_.movement().hpa().edge_count());
        ImGui::Checkbox("Portales HPA*", &show_portals_);
        ImGui::SameLine();
        ImGui::Checkbox("Ruta", &show_paths_);
        ImGui::SameLine();
        ImGui::Checkbox("Campo de flujo", &show_flow_);
        ImGui::Separator();
        ImGui::Text("Seleccionados: %zu de %zu", selection_.selected().size(), curr_.entities.size());
        ImGui::TextDisabled("Arrastre: rectángulo · Mayús: añadir · Esc: limpiar · clic derecho: mover");
        ImGui::TextDisabled("Flechas/WASD o borde de ventana: desplazar");
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
        } else {
            ImGui::TextDisabled("Pasa el ratón sobre el mapa");
        }
        ImGui::End();
    }

    void present() {
        build_screen_entities();
        renderer_.begin_frame();

        markers_.clear();
        for (std::size_t i = 0; i < screen_entities_.size(); ++i) {
            const ScreenEntity& e = screen_entities_[i];
            const auto& rgb = data_.units.types[screen_types_[i]].color;
            markers_.push_back({e.pos, selection_.is_selected(e.id), {rgb[0], rgb[1], rgb[2], kOpaque}});
        }
        build_overlays();
        const std::optional<sim::TileCoord> hover = hovered_tile();
        draw_ui(hover);

        render::Scene scene;
        scene.map = curr_.map.get();
        scene.camera = camera_;
        scene.screen = renderer_.screen_size();
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
    std::vector<sim::UnitTypeId> screen_types_;
    std::vector<render::Marker> markers_;
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
    issue_scenario(world, data.headless_scenario);

    const auto start = SteadyClock::now();
    for (std::int64_t i = 0; i < ticks; ++i) {
        world.step();
    }
    const double total_ms = elapsed_ms(start);
    const double per_tick = ticks > 0 ? total_ms / static_cast<double>(ticks) : 0.0;
    spdlog::info("headless: mapa {}x{} generado en {:.2f} ms", world.map().width(), world.map().height(), gen_ms);
    spdlog::info("headless: {} ticks, {} unidades, {} órdenes de guion, {:.3f} ms total, {:.4f} ms/tick", ticks,
                 data.engine.world.demo.count, data.headless_scenario.size(), total_ms, per_tick);
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
