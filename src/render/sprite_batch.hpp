#pragma once

// Lote de sprites de un fotograma. Cada sprite es una instancia de 28 bytes que el
// vertex shader expande a un quad; todo el lote se dibuja en una sola llamada.

#include <cstdint>
#include <span>
#include <vector>

#include "render/atlas.hpp"
#include "render/projection.hpp"
#include "render/view_params.hpp"

namespace rts::render {

// Disposición idéntica a la entrada del vertex shader (sprite.vert.hlsl).
struct SpriteInstance {
    float x;  // esquina superior izquierda, píxeles de pantalla
    float y;
    float w;
    float h;
    std::uint16_t u0;  // UV normalizadas a 16 bits
    std::uint16_t v0;
    std::uint16_t u1;
    std::uint16_t v1;
    Rgba color;  // multiplica al texel
};
static_assert(sizeof(SpriteInstance) == 28, "debe coincidir con los atributos del pipeline");

class SpriteBatch {
public:
    void clear() noexcept { instances_.clear(); }

    void add(Vec2 top_left, Vec2 size, const AtlasRegion& region, Rgba color) {
        instances_.push_back({top_left.x, top_left.y, size.x, size.y, region.u0, region.v0, region.u1, region.v1,
                              color});
    }

    [[nodiscard]] std::span<const SpriteInstance> instances() const noexcept { return instances_; }
    [[nodiscard]] std::size_t size() const noexcept { return instances_.size(); }

private:
    std::vector<SpriteInstance> instances_;
};

}  // namespace rts::render
