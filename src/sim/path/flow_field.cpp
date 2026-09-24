#include "sim/path/flow_field.hpp"

namespace rts::sim {

FlowField::FlowField(const PassGrid& grid, TileCoord goal, const SectorMask& corridor, GridSearch& search)
    : goal_(goal) {
    cost_.assign(static_cast<std::size_t>(grid.width()) * static_cast<std::size_t>(grid.height()), kUnreached);
    const SearchRect whole{0, 0, grid.width(), grid.height()};
    search.dijkstra(grid, goal, whole, corridor);
    // Se copian solo las casillas de los sectores del pasillo.
    const std::int32_t s = corridor.sector_size;
    for (std::size_t sector = 0; sector < corridor.allowed.size(); ++sector) {
        if (corridor.allowed[sector] == 0) {
            continue;
        }
        const std::int32_t sx = static_cast<std::int32_t>(sector) % corridor.sectors_w;
        const std::int32_t sy = static_cast<std::int32_t>(sector) / corridor.sectors_w;
        for (std::int32_t y = sy * s; y < sy * s + s && y < grid.height(); ++y) {
            for (std::int32_t x = sx * s; x < sx * s + s && x < grid.width(); ++x) {
                const std::int32_t c = search.cost_at(grid, {x, y});
                if (c != kUnreached) {
                    cost_[grid.index({x, y})] = c;
                    ++reached_;
                }
            }
        }
    }
}

std::int32_t FlowField::cost(const PassGrid& grid, TileCoord c) const noexcept {
    return grid.contains(c) ? cost_[grid.index(c)] : kUnreached;
}

std::optional<TileCoord> FlowField::next_step(const PassGrid& grid, TileCoord from) const noexcept {
    const std::int32_t here = cost(grid, from);
    if (here == kUnreached || here == 0) {
        return std::nullopt;
    }
    std::optional<TileCoord> best;
    std::int32_t best_cost = here;
    for (const Direction& d : kDirections) {
        if (!grid.can_step(from, d)) {
            continue;
        }
        const TileCoord n{from.x + d.dx, from.y + d.dy};
        const std::int32_t c = cost(grid, n);
        if (c < best_cost) {
            best_cost = c;
            best = n;
        }
    }
    return best;
}

}  // namespace rts::sim
