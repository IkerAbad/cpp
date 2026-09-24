#pragma once

// Binarios de shader generados en tiempo de build (cmake/Shaders.cmake) a partir de
// src/render/shaders/*.hlsl. Un span vacío indica que el formato no se compiló en
// esta plataforma (DXIL solo existe en Windows).

#include <cstdint>
#include <span>

namespace rts::render::shaders {

std::span<const std::uint8_t> sprite_vert_spirv();
std::span<const std::uint8_t> sprite_vert_dxil();
std::span<const std::uint8_t> sprite_frag_spirv();
std::span<const std::uint8_t> sprite_frag_dxil();

}  // namespace rts::render::shaders
