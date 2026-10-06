#pragma once

// Presentación con SDL_GPU. Lee snapshots de la simulación y nunca escribe en ella.
//
// Por fotograma:
//   begin_frame()     nuevo fotograma de ImGui (el juego añade sus paneles después)
//   draw_scene()      traduce la escena a instancias de sprite (CPU)
//   end_frame()       sube las instancias, dibuja sprites + ImGui y presenta

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include "render/atlas.hpp"
#include "render/scene.hpp"
#include "render/sprite_batch.hpp"
#include "render/view_params.hpp"

struct SDL_Window;
struct SDL_GPUDevice;
struct SDL_GPUGraphicsPipeline;
struct SDL_GPUTexture;
struct SDL_GPUSampler;
struct SDL_GPUBuffer;
struct SDL_GPUTransferBuffer;
union SDL_Event;

namespace rts::render {

struct RendererDesc {
    SDL_Window* window = nullptr;
    bool vsync = true;
    ViewParams view;
    std::vector<std::array<std::uint8_t, 3>> terrain_colors;
    const ArtSpec* art = nullptr;  // arte propio (F1); null: formas planas
};

class Renderer {
public:
    static std::expected<std::unique_ptr<Renderer>, std::string> create(const RendererDesc& desc);

    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    void process_event(const SDL_Event& event);
    [[nodiscard]] bool ui_wants_mouse() const noexcept;
    [[nodiscard]] bool ui_wants_keyboard() const noexcept;

    // Tamaño de la ventana en píxeles lógicos: el mismo espacio que el ratón y la escena.
    [[nodiscard]] Vec2 screen_size() const noexcept;

    void begin_frame();
    SceneStats draw_scene(const Scene& scene);
    void end_frame();

    [[nodiscard]] const char* driver_name() const noexcept;
    [[nodiscard]] std::size_t sprites_last_frame() const noexcept { return sprites_last_frame_; }

private:
    Renderer(SDL_Window* window, SDL_GPUDevice* device, const RendererDesc& desc, Atlas atlas);

    std::expected<void, std::string> init_gpu_resources(bool vsync);
    std::expected<void, std::string> upload_atlas();
    bool ensure_instance_capacity(std::uint32_t count);

    SDL_Window* window_ = nullptr;
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUGraphicsPipeline* pipeline_ = nullptr;
    SDL_GPUTexture* atlas_texture_ = nullptr;
    SDL_GPUSampler* sampler_ = nullptr;
    SDL_GPUBuffer* instance_buffer_ = nullptr;
    SDL_GPUTransferBuffer* instance_transfer_ = nullptr;
    std::uint32_t instance_capacity_ = 0;
    bool imgui_ready_ = false;

    ViewParams view_;
    Atlas atlas_;
    SceneBuilder scene_builder_;
    SpriteBatch batch_;
    std::size_t sprites_last_frame_ = 0;
};

}  // namespace rts::render
