#include "render/renderer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <format>
#include <span>
#include <utility>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <spdlog/spdlog.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

#include "platform/profile.hpp"
#include "render/embedded_shaders.hpp"

namespace rts::render {

namespace {

// Capacidad inicial del búfer de instancias: 16 384 sprites = 448 KiB. Crece al doble
// cuando un fotograma la supera (1080p con el mapa entero visible: ~2 200 casillas).
constexpr std::uint32_t kInitialInstanceCapacity = 16'384;
constexpr float kColorByteMax = 255.0f;

// Uniforms del vertex shader (cbuffer ViewUniforms): float2 + relleno a 16 bytes.
struct ViewUniforms {
    float screen_w;
    float screen_h;
    float unused0;
    float unused1;
};

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

// Formatos de shader que podemos entregar: SPIR-V siempre; DXIL si el build lo generó
// (Windows). DXBC se anuncia con DXIL porque el backend de ImGui usa DXBC en D3D12.
SDL_GPUShaderFormat available_shader_formats() {
    SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV;
    if (!shaders::sprite_vert_dxil().empty()) {
        formats |= SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_DXBC;
    }
    return formats;
}

SDL_GPUShader* create_shader(SDL_GPUDevice* device, SDL_GPUShaderStage stage, std::span<const std::uint8_t> spirv,
                             std::span<const std::uint8_t> dxil, std::uint32_t samplers, std::uint32_t uniforms) {
    const SDL_GPUShaderFormat supported = SDL_GetGPUShaderFormats(device);
    SDL_GPUShaderCreateInfo info{};
    info.entrypoint = "main";
    info.stage = stage;
    info.num_samplers = samplers;
    info.num_uniform_buffers = uniforms;
    if ((supported & SDL_GPU_SHADERFORMAT_SPIRV) != 0 && !spirv.empty()) {
        info.format = SDL_GPU_SHADERFORMAT_SPIRV;
        info.code = spirv.data();
        info.code_size = spirv.size();
    } else if ((supported & SDL_GPU_SHADERFORMAT_DXIL) != 0 && !dxil.empty()) {
        info.format = SDL_GPU_SHADERFORMAT_DXIL;
        info.code = dxil.data();
        info.code_size = dxil.size();
    } else {
        SDL_SetError("el backend no acepta ninguno de los formatos de shader compilados");
        return nullptr;
    }
    return SDL_CreateGPUShader(device, &info);
}

}  // namespace

std::expected<std::unique_ptr<Renderer>, std::string> Renderer::create(const RendererDesc& desc) {
#if defined(NDEBUG)
    constexpr bool debug_mode = false;
#else
    constexpr bool debug_mode = true;
#endif
    // Primero el backend que elija SDL (Direct3D 12 en Windows); si no arranca, Vulkan,
    // que también existe en casi todas las GPU de Windows. Así un fallo de un backend
    // en una máquina concreta no deja el juego sin abrir.
    std::string first_error;
    for (const char* driver : {static_cast<const char*>(nullptr), "vulkan"}) {
        SDL_GPUDevice* device = SDL_CreateGPUDevice(available_shader_formats(), debug_mode, driver);
        if (device == nullptr) {
            const std::string error = std::format("SDL_CreateGPUDevice({}): {}", driver != nullptr ? driver : "auto",
                                                  SDL_GetError());
            first_error = first_error.empty() ? error : first_error;
            continue;
        }
        const std::string name = SDL_GetGPUDeviceDriver(device);
        // Desde aquí el destructor libera lo que se haya creado.
        std::unique_ptr<Renderer> renderer(new Renderer(desc.window, device, desc, build_atlas(desc.view, desc.art != nullptr ? *desc.art : ArtSpec{})));
        if (auto ok = renderer->init_gpu_resources(desc.vsync); !ok) {
            const std::string error = std::format("{} ({})", ok.error(), name);
            first_error = first_error.empty() ? error : first_error;
            spdlog::warn("Render con {} no disponible: {}", name, error);
            if (driver != nullptr || name == "vulkan") {
                break;  // ya era Vulkan: no hay más que probar
            }
            continue;
        }
        return renderer;
    }
    return std::unexpected(first_error);
}

Renderer::Renderer(SDL_Window* window, SDL_GPUDevice* device, const RendererDesc& desc, Atlas atlas)
    : window_(window),
      device_(device),
      view_(desc.view),
      atlas_(std::move(atlas)),
      scene_builder_(desc.view, atlas_, desc.terrain_colors) {}

std::expected<void, std::string> Renderer::init_gpu_resources(bool vsync) {
    if (!SDL_ClaimWindowForGPUDevice(device_, window_)) {
        return std::unexpected(std::format("SDL_ClaimWindowForGPUDevice: {}", SDL_GetError()));
    }
    const SDL_GPUPresentMode present_mode = choose_present_mode(device_, window_, vsync);
    if (!SDL_SetGPUSwapchainParameters(device_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, present_mode)) {
        return std::unexpected(std::format("SDL_SetGPUSwapchainParameters: {}", SDL_GetError()));
    }
    const SDL_GPUTextureFormat swapchain_format = SDL_GetGPUSwapchainTextureFormat(device_, window_);

    // --- Pipeline de sprites ---------------------------------------------------
    SDL_GPUShader* vs = create_shader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::sprite_vert_spirv(),
                                      shaders::sprite_vert_dxil(), 0, 1);
    SDL_GPUShader* fs = create_shader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::sprite_frag_spirv(),
                                      shaders::sprite_frag_dxil(), 1, 0);
    if (vs == nullptr || fs == nullptr) {
        std::string error = std::format("SDL_CreateGPUShader: {}", SDL_GetError());
        SDL_ReleaseGPUShader(device_, vs);
        SDL_ReleaseGPUShader(device_, fs);
        return std::unexpected(std::move(error));
    }

    SDL_GPUVertexBufferDescription buffer_desc{};
    buffer_desc.slot = 0;
    buffer_desc.pitch = sizeof(SpriteInstance);
    buffer_desc.input_rate = SDL_GPU_VERTEXINPUTRATE_INSTANCE;
    buffer_desc.instance_step_rate = 0;

    const std::array<SDL_GPUVertexAttribute, 4> attributes{{
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(SpriteInstance, x)},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(SpriteInstance, w)},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_USHORT4_NORM, offsetof(SpriteInstance, u0)},
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(SpriteInstance, color)},
    }};

    SDL_GPUColorTargetDescription color_target{};
    color_target.format = swapchain_format;
    color_target.blend_state.enable_blend = true;
    color_target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    color_target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color_target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    color_target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    color_target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color_target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

    SDL_GPUGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.vertex_shader = vs;
    pipeline_info.fragment_shader = fs;
    pipeline_info.vertex_input_state.vertex_buffer_descriptions = &buffer_desc;
    pipeline_info.vertex_input_state.num_vertex_buffers = 1;
    pipeline_info.vertex_input_state.vertex_attributes = attributes.data();
    pipeline_info.vertex_input_state.num_vertex_attributes = static_cast<std::uint32_t>(attributes.size());
    pipeline_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipeline_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pipeline_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pipeline_info.target_info.color_target_descriptions = &color_target;
    pipeline_info.target_info.num_color_targets = 1;
    pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline_info);
    SDL_ReleaseGPUShader(device_, vs);
    SDL_ReleaseGPUShader(device_, fs);
    if (pipeline_ == nullptr) {
        return std::unexpected(std::format("SDL_CreateGPUGraphicsPipeline: {}", SDL_GetError()));
    }

    // --- Atlas y sampler -------------------------------------------------------
    if (auto ok = upload_atlas(); !ok) {
        return ok;
    }
    SDL_GPUSamplerCreateInfo sampler_info{};
    // Muestreo al texel más cercano: los sprites se dibujan a escala 1:1.
    sampler_info.min_filter = SDL_GPU_FILTER_NEAREST;
    sampler_info.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = SDL_CreateGPUSampler(device_, &sampler_info);
    if (sampler_ == nullptr) {
        return std::unexpected(std::format("SDL_CreateGPUSampler: {}", SDL_GetError()));
    }
    if (!ensure_instance_capacity(kInitialInstanceCapacity)) {
        return std::unexpected(std::format("búfer de instancias: {}", SDL_GetError()));
    }

    // --- ImGui -----------------------------------------------------------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    imgui_ready_ = true;
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // sin imgui.ini junto al ejecutable
    ImGui::StyleColorsDark();
    const float scale = SDL_GetWindowDisplayScale(window_);
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;

    ImGui_ImplSDL3_InitForSDLGPU(window_);
    ImGui_ImplSDLGPU3_InitInfo init_info{};
    init_info.Device = device_;
    init_info.ColorTargetFormat = swapchain_format;
    init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    init_info.SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    init_info.PresentMode = present_mode;
    ImGui_ImplSDLGPU3_Init(&init_info);
    return {};
}

std::expected<void, std::string> Renderer::upload_atlas() {
    SDL_GPUTextureCreateInfo tex_info{};
    tex_info.type = SDL_GPU_TEXTURETYPE_2D;
    tex_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tex_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tex_info.width = static_cast<std::uint32_t>(atlas_.width);
    tex_info.height = static_cast<std::uint32_t>(atlas_.height);
    tex_info.layer_count_or_depth = 1;
    tex_info.num_levels = 1;
    atlas_texture_ = SDL_CreateGPUTexture(device_, &tex_info);
    if (atlas_texture_ == nullptr) {
        return std::unexpected(std::format("SDL_CreateGPUTexture: {}", SDL_GetError()));
    }

    const auto bytes = static_cast<std::uint32_t>(atlas_.rgba.size());
    SDL_GPUTransferBufferCreateInfo transfer_info{};
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = bytes;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &transfer_info);
    if (transfer == nullptr) {
        return std::unexpected(std::format("SDL_CreateGPUTransferBuffer: {}", SDL_GetError()));
    }
    void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
    if (mapped == nullptr) {
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        return std::unexpected(std::format("SDL_MapGPUTransferBuffer: {}", SDL_GetError()));
    }
    std::memcpy(mapped, atlas_.rgba.data(), bytes);
    SDL_UnmapGPUTransferBuffer(device_, transfer);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (cmd == nullptr) {
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        return std::unexpected(std::format("SDL_AcquireGPUCommandBuffer: {}", SDL_GetError()));
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = transfer;
    src.pixels_per_row = static_cast<std::uint32_t>(atlas_.width);
    src.rows_per_layer = static_cast<std::uint32_t>(atlas_.height);
    SDL_GPUTextureRegion dst{};
    dst.texture = atlas_texture_;
    dst.w = static_cast<std::uint32_t>(atlas_.width);
    dst.h = static_cast<std::uint32_t>(atlas_.height);
    dst.d = 1;
    SDL_UploadToGPUTexture(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    const bool submitted = SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
    if (!submitted) {
        return std::unexpected(std::format("SDL_SubmitGPUCommandBuffer: {}", SDL_GetError()));
    }
    return {};
}

bool Renderer::ensure_instance_capacity(std::uint32_t count) {
    if (count <= instance_capacity_) {
        return true;
    }
    std::uint32_t capacity = std::max(instance_capacity_, kInitialInstanceCapacity);
    while (capacity < count) {
        capacity *= 2;
    }
    SDL_ReleaseGPUBuffer(device_, instance_buffer_);
    SDL_ReleaseGPUTransferBuffer(device_, instance_transfer_);
    instance_buffer_ = nullptr;
    instance_transfer_ = nullptr;
    instance_capacity_ = 0;

    const std::uint32_t bytes = capacity * static_cast<std::uint32_t>(sizeof(SpriteInstance));
    SDL_GPUBufferCreateInfo buffer_info{};
    buffer_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    buffer_info.size = bytes;
    instance_buffer_ = SDL_CreateGPUBuffer(device_, &buffer_info);
    SDL_GPUTransferBufferCreateInfo transfer_info{};
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = bytes;
    instance_transfer_ = SDL_CreateGPUTransferBuffer(device_, &transfer_info);
    if (instance_buffer_ == nullptr || instance_transfer_ == nullptr) {
        return false;
    }
    instance_capacity_ = capacity;
    return true;
}

Renderer::~Renderer() {
    SDL_WaitForGPUIdle(device_);
    if (imgui_ready_) {
        ImGui_ImplSDL3_Shutdown();
        ImGui_ImplSDLGPU3_Shutdown();
        ImGui::DestroyContext();
    }
    SDL_ReleaseGPUBuffer(device_, instance_buffer_);
    SDL_ReleaseGPUTransferBuffer(device_, instance_transfer_);
    SDL_ReleaseGPUSampler(device_, sampler_);
    SDL_ReleaseGPUTexture(device_, atlas_texture_);
    SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
    SDL_ReleaseWindowFromGPUDevice(device_, window_);
    SDL_DestroyGPUDevice(device_);
}

const char* Renderer::driver_name() const noexcept {
    return SDL_GetGPUDeviceDriver(device_);
}

void Renderer::process_event(const SDL_Event& event) {
    ImGui_ImplSDL3_ProcessEvent(&event);
}

bool Renderer::ui_wants_mouse() const noexcept {
    return ImGui::GetIO().WantCaptureMouse;
}

bool Renderer::ui_wants_keyboard() const noexcept {
    return ImGui::GetIO().WantCaptureKeyboard;
}

Vec2 Renderer::screen_size() const noexcept {
    int w = 0;
    int h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    return {static_cast<float>(w), static_cast<float>(h)};
}

void Renderer::begin_frame() {
    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    batch_.clear();
}

SceneStats Renderer::draw_scene(const Scene& scene) {
    RTS_PROFILE_ZONE();
    return scene_builder_.build(scene, batch_);
}

void Renderer::end_frame() {
    RTS_PROFILE_ZONE();
    ImGui::Render();
    ImDrawData* draw_data = ImGui::GetDrawData();
    const bool minimized = draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f;

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (cmd == nullptr) {
        return;
    }

    auto count = static_cast<std::uint32_t>(batch_.size());
    if (count > 0 && !ensure_instance_capacity(count)) {
        count = 0;  // sin memoria de GPU: se dibuja solo la interfaz
    }
    sprites_last_frame_ = count;
    if (count > 0) {
        const std::uint32_t bytes = count * static_cast<std::uint32_t>(sizeof(SpriteInstance));
        // cycle = true: si la GPU aún lee el búfer del fotograma anterior, SDL da otro.
        void* mapped = SDL_MapGPUTransferBuffer(device_, instance_transfer_, true);
        if (mapped != nullptr) {
            std::memcpy(mapped, batch_.instances().data(), bytes);
            SDL_UnmapGPUTransferBuffer(device_, instance_transfer_);
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
            const SDL_GPUTransferBufferLocation src{instance_transfer_, 0};
            const SDL_GPUBufferRegion dst{instance_buffer_, 0, bytes};
            SDL_UploadToGPUBuffer(copy, &src, &dst, true);
            SDL_EndGPUCopyPass(copy);
        } else {
            count = 0;
        }
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
        target.clear_color = {static_cast<float>(view_.clear_color[0]) / kColorByteMax,
                              static_cast<float>(view_.clear_color[1]) / kColorByteMax,
                              static_cast<float>(view_.clear_color[2]) / kColorByteMax, 1.0f};
        target.load_op = SDL_GPU_LOADOP_CLEAR;
        target.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
        if (count > 0) {
            const Vec2 screen = screen_size();
            const ViewUniforms uniforms{screen.x, screen.y, 0.0f, 0.0f};
            SDL_PushGPUVertexUniformData(cmd, 0, &uniforms, sizeof(uniforms));
            SDL_BindGPUGraphicsPipeline(pass, pipeline_);
            const SDL_GPUBufferBinding vb{instance_buffer_, 0};
            SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
            const SDL_GPUTextureSamplerBinding tex{atlas_texture_, sampler_};
            SDL_BindGPUFragmentSamplers(pass, 0, &tex, 1);
            // 6 vértices por quad; una instancia por sprite: una sola llamada de dibujo.
            SDL_DrawGPUPrimitives(pass, 6, count, 0, 0);
        }
        ImGui_ImplSDLGPU3_RenderDrawData(draw_data, cmd, pass);
        SDL_EndGPURenderPass(pass);
    }
    SDL_SubmitGPUCommandBuffer(cmd);
}

}  // namespace rts::render
