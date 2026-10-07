#include "platform/window.hpp"

#include <format>
#include <utility>

#include <SDL3/SDL.h>

namespace rts::platform {

std::expected<std::unique_ptr<Window>, std::string> Window::create(const WindowDesc& desc) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return std::unexpected(std::format("SDL_Init: {}", SDL_GetError()));
    }
    const SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                  (desc.fullscreen ? SDL_WINDOW_FULLSCREEN : SDL_WindowFlags{0});
    SDL_Window* window = SDL_CreateWindow(desc.title.c_str(), desc.width, desc.height, flags);
    if (window == nullptr) {
        std::string error = std::format("SDL_CreateWindow: {}", SDL_GetError());
        SDL_Quit();
        return std::unexpected(std::move(error));
    }
    auto w = std::unique_ptr<Window>(new Window(window));
    // Flechas y WASD hasta que las opciones digan otra cosa.
    w->set_scroll_keys({SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_A,
                        SDL_SCANCODE_D, SDL_SCANCODE_W, SDL_SCANCODE_S});
    return w;
}

Window::~Window() {
    SDL_DestroyWindow(window_);
    SDL_Quit();
}

bool Window::minimized() const noexcept {
    return (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0;
}

bool Window::poll_event(SDL_Event& event) const noexcept {
    return SDL_PollEvent(&event);
}

bool Window::is_close_request(const SDL_Event& event) const noexcept {
    return event.type == SDL_EVENT_QUIT ||
           (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window_));
}

ScrollKeys Window::scroll_keys() const noexcept {
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    const auto held = [&](std::size_t i) {
        const std::int32_t c = scroll_codes_[i];
        return c >= 0 && c < count && keys[c];
    };
    constexpr std::size_t kAlt = 4;  // las cuatro alternativas van después
    return {held(0) || held(kAlt), held(1) || held(kAlt + 1), held(2) || held(kAlt + 2), held(3) || held(kAlt + 3)};
}

void Window::set_size(std::int32_t width, std::int32_t height) noexcept {
    SDL_SetWindowSize(window_, width, height);
}

void Window::set_fullscreen(bool fullscreen) noexcept {
    SDL_SetWindowFullscreen(window_, fullscreen);
}

std::int32_t scancode_from_name(const std::string& name) noexcept {
    const SDL_Scancode c = SDL_GetScancodeFromName(name.c_str());
    return c == SDL_SCANCODE_UNKNOWN ? -1 : static_cast<std::int32_t>(c);
}

std::string scancode_name(std::int32_t code) {
    return SDL_GetScancodeName(static_cast<SDL_Scancode>(code));
}

bool Window::shift_held() const noexcept {
    return (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
}

bool Window::ctrl_held() const noexcept {
    return (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
}

MouseState Window::mouse() const noexcept {
    MouseState m;
    SDL_GetMouseState(&m.x, &m.y);
    m.in_window = SDL_GetMouseFocus() == window_;
    return m;
}

std::uint64_t now_ns() noexcept {
    return SDL_GetTicksNS();
}

std::string executable_dir() {
    const char* base = SDL_GetBasePath();
    return base != nullptr ? std::string(base) : std::string();
}

}  // namespace rts::platform
