#include "render/renderer.hpp"

#include <algorithm>
#include <format>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

#include "platform/profile.hpp"

namespace rts::render {

namespace {

// Aspecto provisional de la demo de M0. Son marcadores de depuración que desaparecen
// en M1 con el atlas de texturas; los parámetros de diseño de juego van en data/.
constexpr float kArenaMarginPx = 24.0f;
constexpr float kPointRadiusPx = 2.5f;
constexpr ImU32 kArenaBorderColor = IM_COL32(90, 90, 110, 255);
constexpr ImU32 kPointColor = IM_COL32(230, 200, 90, 255);
constexpr SDL_FColor kClearColor{0.08f, 0.09f, 0.11f, 1.0f};

float fixed_to_float(sim::Fixed v) noexcept {
    return static_cast<float>(v.raw()) / static_cast<float>(sim::Fixed::kOneRaw);
}

SDL_GPUPresentMode choose_present_mode(SDL_GPUDevice* device, SDL_Window* window, bool vsync) {
    if (vsync) {
        return SDL_GPU_PRESENTMODE_VSYNC;  // soportado siempre, según SDL_gpu.h
    }
    if (SDL_WindowSupportsGPUPresentMode(device, window, SDL_GPU_PRESENTMODE_IMMEDIATE)) {
        return SDL_GPU_PRESENTMODE_IMMEDIATE;
    }
    if (SDL_WindowSupportsGPUPresentMode(device, window, SDL_GPU_PRESENTMODE_MAILBOX)) {
        return SDL_GPU_PRESENTMODE_MAILBOX;
    }
    return SDL_GPU_PRESENTMODE_VSYNC;
}

}  // namespace

std::expected<std::unique_ptr<Renderer>, std::string> Renderer::create(SDL_Window* window, bool vsync) {
    // SPIR-V para Vulkan; DXBC y DXIL para D3D12. ImGui trae sus shaders en ambos
    // formatos, así que SDL elige el backend disponible en la máquina.
    constexpr SDL_GPUShaderFormat formats =
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXBC | SDL_GPU_SHADERFORMAT_DXIL;
#if defined(NDEBUG)
    constexpr bool debug_mode = false;
#else
    constexpr bool debug_mode = true;
#endif
    SDL_GPUDevice* device = SDL_CreateGPUDevice(formats, debug_mode, nullptr);
    if (device == nullptr) {
        return std::unexpected(std::format("SDL_CreateGPUDevice: {}", SDL_GetError()));
    }
    // Desde aquí el destructor libera lo que se haya creado.
    std::unique_ptr<Renderer> renderer(new Renderer(window, device));

    if (!SDL_ClaimWindowForGPUDevice(device, window)) {
        return std::unexpected(std::format("SDL_ClaimWindowForGPUDevice: {}", SDL_GetError()));
    }
    const SDL_GPUPresentMode present_mode = choose_present_mode(device, window, vsync);
    if (!SDL_SetGPUSwapchainParameters(device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, present_mode)) {
        return std::unexpected(std::format("SDL_SetGPUSwapchainParameters: {}", SDL_GetError()));
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    renderer->imgui_ready_ = true;
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // sin imgui.ini junto al ejecutable
    ImGui::StyleColorsDark();

    const float scale = SDL_GetWindowDisplayScale(window);
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;

    ImGui_ImplSDL3_InitForSDLGPU(window);
    ImGui_ImplSDLGPU3_InitInfo init_info{};
    init_info.Device = device;
    init_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(device, window);
    init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    init_info.SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    init_info.PresentMode = present_mode;
    ImGui_ImplSDLGPU3_Init(&init_info);

    return renderer;
}

Renderer::~Renderer() {
    SDL_WaitForGPUIdle(device_);
    if (imgui_ready_) {
        ImGui_ImplSDL3_Shutdown();
        ImGui_ImplSDLGPU3_Shutdown();
        ImGui::DestroyContext();
    }
    SDL_ReleaseWindowFromGPUDevice(device_, window_);
    SDL_DestroyGPUDevice(device_);
}

const char* Renderer::driver_name() const noexcept {
    return SDL_GetGPUDeviceDriver(device_);
}

void Renderer::process_event(const SDL_Event& event) {
    ImGui_ImplSDL3_ProcessEvent(&event);
}

void Renderer::render_frame(const sim::Snapshot& prev, const sim::Snapshot& curr, double alpha,
                            const FrameStats& stats) {
    RTS_PROFILE_ZONE();
    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    draw_world(prev, curr, alpha);
    draw_debug_panel(stats, curr);

    ImGui::Render();
    ImDrawData* draw_data = ImGui::GetDrawData();
    const bool minimized = draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f;

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (cmd == nullptr) {
        return;
    }
    SDL_GPUTexture* swapchain = nullptr;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window_, &swapchain, nullptr, nullptr)) {
        SDL_CancelGPUCommandBuffer(cmd);
        return;
    }
    if (swapchain != nullptr && !minimized) {
        ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, cmd);
        SDL_GPUColorTargetInfo target{};
        target.texture = swapchain;
        target.clear_color = kClearColor;
        target.load_op = SDL_GPU_LOADOP_CLEAR;
        target.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
        ImGui_ImplSDLGPU3_RenderDrawData(draw_data, cmd, pass);
        SDL_EndGPURenderPass(pass);
    }
    SDL_SubmitGPUCommandBuffer(cmd);
}

void Renderer::draw_world(const sim::Snapshot& prev, const sim::Snapshot& curr, double alpha) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float arena = fixed_to_float(curr.arena_size);
    if (arena <= 0.0f) {
        return;
    }
    const float side = std::min(display.x, display.y) - 2.0f * kArenaMarginPx;
    if (side <= 0.0f) {
        return;
    }
    const float px_per_tile = side / arena;
    const ImVec2 origin{(display.x - side) * 0.5f, (display.y - side) * 0.5f};

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddRect(origin, ImVec2{origin.x + side, origin.y + side}, kArenaBorderColor);

    // Solo se interpola si prev y curr describen las mismas entidades en el mismo orden.
    const bool can_interpolate = prev.entities.size() == curr.entities.size();
    const auto a = static_cast<float>(alpha);
    for (std::size_t i = 0; i < curr.entities.size(); ++i) {
        const sim::SnapshotEntity& c = curr.entities[i];
        float x = fixed_to_float(c.pos.x);
        float y = fixed_to_float(c.pos.y);
        if (can_interpolate && prev.entities[i].id == c.id) {
            const float px = fixed_to_float(prev.entities[i].pos.x);
            const float py = fixed_to_float(prev.entities[i].pos.y);
            x = px + (x - px) * a;
            y = py + (y - py) * a;
        }
        dl->AddCircleFilled(ImVec2{origin.x + x * px_per_tile, origin.y + y * px_per_tile}, kPointRadiusPx,
                            kPointColor);
    }
}

void Renderer::draw_debug_panel(const FrameStats& stats, const sim::Snapshot& curr) {
    ImGui::Begin("Depuración");
    ImGui::Text("Backend GPU: %s", driver_name());
    ImGui::Text("Fotograma: %.2f ms (%.0f FPS)", stats.frame_ms, stats.frame_ms > 0.0 ? 1000.0 / stats.frame_ms : 0.0);
    ImGui::Separator();
    ImGui::Text("Tick: %u", curr.tick);
    ImGui::Text("Ticks en este fotograma: %d", stats.ticks_this_frame);
    ImGui::Text("Simulación: %.3f ms/tick (media), %.3f ms este fotograma", stats.sim_ms_per_tick,
                stats.sim_ms_last_frame);
    ImGui::Text("Ticks descartados (total): %lld", static_cast<long long>(stats.dropped_ticks_total));
    ImGui::Text("alpha: %.3f", stats.alpha);
    ImGui::Text("Entidades: %zu", curr.entities.size());
    ImGui::Text("Hash de estado: %016llx", static_cast<unsigned long long>(stats.state_hash));
    ImGui::End();
}

}  // namespace rts::render
