#include "platform/window.hpp"

#include <format>

#include <SDL3/SDL.h>

namespace rts::platform {

std::expected<std::unique_ptr<Window>, std::string> Window::create(const WindowDesc& desc) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return std::unexpected(std::format("SDL_Init: {}", SDL_GetError()));
    }
    const SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    SDL_Window* window = SDL_CreateWindow(desc.title.c_str(), desc.width, desc.height, flags);
    if (window == nullptr) {
        std::string error = std::format("SDL_CreateWindow: {}", SDL_GetError());
        SDL_Quit();
        return std::unexpected(std::move(error));
    }
    return std::unique_ptr<Window>(new Window(window));
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

std::uint64_t now_ns() noexcept {
    return SDL_GetTicksNS();
}

std::string executable_dir() {
    const char* base = SDL_GetBasePath();
    return base != nullptr ? std::string(base) : std::string();
}

}  // namespace rts::platform
