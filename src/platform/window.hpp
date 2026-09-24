#pragma once

// Inicialización de SDL y ventana principal. Es la única pieza que llama a SDL_Init.

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

private:
    explicit Window(SDL_Window* window) noexcept : window_(window) {}

    SDL_Window* window_ = nullptr;
};

// Reloj monotónico en nanosegundos desde el arranque de SDL.
[[nodiscard]] std::uint64_t now_ns() noexcept;

// Carpeta del ejecutable, con separador final. Vacía si la plataforma no la sabe.
[[nodiscard]] std::string executable_dir();

}  // namespace rts::platform
