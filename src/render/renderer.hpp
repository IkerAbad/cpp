#pragma once

// Presentación. Lee snapshots de la simulación y nunca escribe en ella.
// M0: dispositivo SDL_GPU, ImGui y dibujo de la demo mediante la lista de dibujo de
// fondo de ImGui. El sprite batcher con shaders propios llega en M1.

#include <cstdint>
#include <expected>
#include <memory>
#include <string>

#include "sim/world.hpp"

struct SDL_Window;
struct SDL_GPUDevice;
union SDL_Event;

namespace rts::render {

// Métricas que el pegamento de juego entrega al panel de depuración.
struct FrameStats {
    double frame_ms = 0.0;
    double sim_ms_last_frame = 0.0;  // tiempo total de simulación en este fotograma
    double sim_ms_per_tick = 0.0;    // media móvil del coste de un tick
    std::int32_t ticks_this_frame = 0;
    std::int64_t dropped_ticks_total = 0;
    double alpha = 0.0;
    std::uint64_t state_hash = 0;
};

class Renderer {
public:
    static std::expected<std::unique_ptr<Renderer>, std::string> create(SDL_Window* window, bool vsync);

    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    void process_event(const SDL_Event& event);

    // Interpola cada entidad entre prev y curr con alpha en [0, 1).
    void render_frame(const sim::Snapshot& prev, const sim::Snapshot& curr, double alpha,
                      const FrameStats& stats);

    [[nodiscard]] const char* driver_name() const noexcept;

private:
    Renderer(SDL_Window* window, SDL_GPUDevice* device) noexcept : window_(window), device_(device) {}

    void draw_world(const sim::Snapshot& prev, const sim::Snapshot& curr, double alpha);
    void draw_debug_panel(const FrameStats& stats, const sim::Snapshot& curr);

    SDL_Window* window_ = nullptr;
    SDL_GPUDevice* device_ = nullptr;
    bool imgui_ready_ = false;
};

}  // namespace rts::render
