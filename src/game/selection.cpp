#include "game/selection.hpp"

#include <algorithm>
#include <cmath>

namespace rts::game {

ScreenRect ScreenRect::from_corners(render::Vec2 a, render::Vec2 b) noexcept {
    return {{std::min(a.x, b.x), std::min(a.y, b.y)}, {std::max(a.x, b.x), std::max(a.y, b.y)}};
}

void Selection::begin_drag(render::Vec2 at) noexcept {
    dragging_ = true;
    start_ = at;
    current_ = at;
}

void Selection::update_drag(render::Vec2 at) noexcept {
    if (dragging_) {
        current_ = at;
    }
}

bool Selection::is_click() const noexcept {
    const auto threshold = static_cast<float>(cfg_.drag_threshold_px);
    return std::abs(current_.x - start_.x) <= threshold && std::abs(current_.y - start_.y) <= threshold;
}

bool Selection::has_visible_rect() const noexcept {
    return dragging_ && !is_click();
}

void Selection::end_drag(render::Vec2 at, bool additive, std::span<const ScreenEntity> entities) {
    if (!dragging_) {
        return;
    }
    current_ = at;
    dragging_ = false;

    std::vector<std::uint32_t> picked;
    if (is_click()) {
        const auto radius = static_cast<float>(cfg_.click_radius_px);
        float best = radius * radius;
        std::uint32_t best_id = 0;
        bool found = false;
        for (const ScreenEntity& e : entities) {
            const float dx = e.pos.x - at.x;
            const float dy = e.pos.y - at.y;
            const float d2 = dx * dx + dy * dy;
            // <= para que un empate lo gane la última dibujada, que es la que está encima.
            if (d2 <= best) {
                best = d2;
                best_id = e.id;
                found = true;
            }
        }
        if (found) {
            picked.push_back(best_id);
        }
    } else {
        const ScreenRect rect = drag_rect();
        for (const ScreenEntity& e : entities) {
            if (rect.contains(e.pos)) {
                picked.push_back(e.id);
            }
        }
    }

    if (!additive) {
        selected_.clear();
    }
    selected_.insert(selected_.end(), picked.begin(), picked.end());
    std::ranges::sort(selected_);
    const auto dup = std::ranges::unique(selected_);
    selected_.erase(dup.begin(), dup.end());
}

bool Selection::is_selected(std::uint32_t id) const noexcept {
    return std::ranges::binary_search(selected_, id);
}

void Selection::retain(std::span<const ScreenEntity> alive) {
    std::erase_if(selected_, [&](std::uint32_t id) {
        return std::ranges::none_of(alive, [id](const ScreenEntity& e) { return e.id == id; });
    });
}

}  // namespace rts::game
