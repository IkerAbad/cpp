#pragma once

// Inicialización de SDL, ventana principal y lectura del estado de entrada. Es la
// única pieza que llama a SDL_Init.

#include <cstdint>
#include <expected>
#include <memory>
#include <string>

struct SDL_Window;
union SDL_Event;

namespace rts::platform {

struct WindowDesc {
    std::string title;
    std::int32_t width = 0;
    std::int32_t height = 0;
};

// Teclas de desplazamiento: flechas o WASD.
struct ScrollKeys {
    bool left = false;
    bool right = false;
    bool up = false;
    bool down = false;
};

struct MouseState {
    float x = 0.0f;  // píxeles lógicos de ventana
    float y = 0.0f;
    bool in_window = false;
};

class Window {
public:
    static std::expected<std::unique_ptr<Window>, std::string> create(const WindowDesc& desc);

    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&) = delete;
    Window& operator=(Window&&) = delete;

    [[nodiscard]] SDL_Window* handle() const noexcept { return window_; }
    [[nodiscard]] bool minimized() const noexcept;

    // Extrae un evento de la cola; false si no quedan.
    bool poll_event(SDL_Event& event) const noexcept;
    [[nodiscard]] bool is_close_request(const SDL_Event& event) const noexcept;

    [[nodiscard]] ScrollKeys scroll_keys() const noexcept;
    [[nodiscard]] bool shift_held() const noexcept;
    [[nodiscard]] MouseState mouse() const noexcept;

private:
    explicit Window(SDL_Window* window) noexcept : window_(window) {}

    SDL_Window* window_ = nullptr;
};

// Reloj monotónico en nanosegundos desde el arranque de SDL.
[[nodiscard]] std::uint64_t now_ns() noexcept;

// Carpeta del ejecutable, con separador final. Vacía si la plataforma no la sabe.
[[nodiscard]] std::string executable_dir();

}  // namespace rts::platform
