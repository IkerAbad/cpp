#pragma once

// Inicialización de SDL, ventana principal y lectura del estado de entrada. Es la
// única pieza que llama a SDL_Init.

#include <array>
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
    bool fullscreen = false;  // F5
};

// Teclas de desplazamiento: las configuradas (por omisión, flechas o WASD).
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
    // Teclas de cámara (F5): izquierda, derecha, arriba y abajo, y otras cuatro
    // alternativas, como códigos de tecla (scancode_from_name).
    void set_scroll_keys(const std::array<std::int32_t, 8>& codes) noexcept { scroll_codes_ = codes; }
    // Pantalla (F5).
    void set_size(std::int32_t width, std::int32_t height) noexcept;
    void set_fullscreen(bool fullscreen) noexcept;
    [[nodiscard]] bool shift_held() const noexcept;
    [[nodiscard]] bool ctrl_held() const noexcept;
    [[nodiscard]] MouseState mouse() const noexcept;

private:
    explicit Window(SDL_Window* window) noexcept : window_(window) {}

    SDL_Window* window_ = nullptr;
    std::array<std::int32_t, 8> scroll_codes_{-1, -1, -1, -1, -1, -1, -1, -1};
};

// Código de tecla de un nombre de SDL ("A", "Left", "F5"); -1 si no existe. Y al revés.
[[nodiscard]] std::int32_t scancode_from_name(const std::string& name) noexcept;
[[nodiscard]] std::string scancode_name(std::int32_t code);

// Reloj monotónico en nanosegundos desde el arranque de SDL.
[[nodiscard]] std::uint64_t now_ns() noexcept;

// Carpeta del ejecutable, con separador final. Vacía si la plataforma no la sabe.
[[nodiscard]] std::string executable_dir();

}  // namespace rts::platform
