#include "sim/path/grid.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>

namespace rts::sim {

namespace {

// Orden total del montículo: f, luego h, luego índice de casilla. Sin empates posibles
// entre casillas distintas, así que el orden de expansión es idéntico en todo compilador.
struct HeapGreater {
    template <typename N>
    bool operator()(const N& a, const N& b) const noexcept {
        if (a.f != b.f) {
            return a.f > b.f;
        }
        if (a.h != b.h) {
            return a.h > b.h;
        }
        return a.index > b.index;
    }
};

}  // namespace

PassGrid::PassGrid(const TileMap& map, std::span<const std::uint8_t> passable_by_terrain)
    : width_(map.width()), height_(map.height()) {
    terrain_pass_.resize(static_cast<std::size_t>(map.tile_count()));
    const auto terrain = map.terrain_layer();
    for (std::size_t i = 0; i < terrain_pass_.size(); ++i) {
        assert(terrain[i] < passable_by_terrain.size());
        terrain_pass_[i] = passable_by_terrain[terrain[i]] != 0 ? 1 : 0;
    }
    blocked_.assign(terrain_pass_.size(), 0);
    pass_ = terrain_pass_;
    label_components();
}

void PassGrid::set_blocked(TileCoord c, bool blocked) noexcept {
    assert(contains(c));
    const std::size_t i = index(c);
    blocked_[i] = blocked ? 1 : 0;
    pass_[i] = terrain_pass_[i] != 0 && !blocked ? 1 : 0;
}

bool PassGrid::can_step(TileCoord c, const Direction& d) const noexcept {
    const TileCoord n{c.x + d.dx, c.y + d.dy};
    if (!passable(n)) {
        return false;
    }
    if (d.dx != 0 && d.dy != 0) {
        return passable({c.x + d.dx, c.y}) && passable({c.x, c.y + d.dy});
    }
    return true;
}

void PassGrid::label_components() {
    comp_.assign(pass_.size(), 0);
    comp_size_.assign(1, 0);
    std::uint32_t next = 1;
    std::vector<std::size_t> stack;
    for (std::size_t start = 0; start < pass_.size(); ++start) {
        if (pass_[start] == 0 || comp_[start] != 0) {
            continue;
        }
        comp_[start] = next;
        comp_size_.push_back(0);
        stack.push_back(start);
        while (!stack.empty()) {
            const std::size_t i = stack.back();
            stack.pop_back();
            ++comp_size_[next];
            const TileCoord c = coord(i);
            for (const Direction& d : kDirections) {
                if (!can_step(c, d)) {
                    continue;
                }
                const std::size_t n = index({c.x + d.dx, c.y + d.dy});
                if (comp_[n] == 0) {
                    comp_[n] = next;
                    stack.push_back(n);
                }
            }
        }
        ++next;
    }
}

std::optional<TileCoord> PassGrid::nearest_in_component(TileCoord target, std::uint32_t component,
                                                        std::int32_t max_radius) const {
    if (component == 0) {
        return std::nullopt;
    }
    if (this->component(target) == component) {
        return target;
    }
    // Anillos de Chebyshev crecientes; dentro de cada anillo, la de menor distancia
    // octil y, a igualdad, la de menor índice (determinista).
    for (std::int32_t r = 1; r <= max_radius; ++r) {
        std::optional<TileCoord> best;
        std::int32_t best_d = kUnreached;
        for (std::int32_t y = target.y - r; y <= target.y + r; ++y) {
            for (std::int32_t x = target.x - r; x <= target.x + r; ++x) {
                if (std::max(std::abs(x - target.x), std::abs(y - target.y)) != r) {
                    continue;
                }
                const TileCoord c{x, y};
                if (this->component(c) != component) {
                    continue;
                }
                const std::int32_t d = octile_distance(c, target);
                if (d < best_d) {
                    best_d = d;
                    best = c;
                }
            }
        }
        if (best) {
            return best;
        }
    }
    return std::nullopt;
}

GridSearch::GridSearch(std::int32_t width, std::int32_t height) {
    const auto n = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    stamp_.assign(n, 0);
    g_.assign(n, kUnreached);
    parent_.assign(n, 0);
    closed_.assign(n, 0);
}

void GridSearch::begin(std::size_t tile_count) {
    assert(tile_count == stamp_.size());
    (void)tile_count;
    ++generation_;
    if (generation_ == 0) {  // desbordamiento tras 2^32 búsquedas: se reinicia de verdad
        std::ranges::fill(stamp_, 0);
        generation_ = 1;
    }
    heap_.clear();
}

void GridSearch::push(HeapNode n) {
    heap_.push_back(n);
    std::ranges::push_heap(heap_, HeapGreater{});
}

GridSearch::HeapNode GridSearch::pop() {
    std::ranges::pop_heap(heap_, HeapGreater{});
    const HeapNode n = heap_.back();
    heap_.pop_back();
    return n;
}

std::optional<std::int32_t> GridSearch::astar(const PassGrid& grid, TileCoord start, TileCoord goal,
                                              const SearchRect& rect, std::vector<TileCoord>* path) {
    if (!grid.passable(start) || !grid.passable(goal) || !rect.contains(start) || !rect.contains(goal)) {
        return std::nullopt;
    }
    begin(static_cast<std::size_t>(grid.width()) * static_cast<std::size_t>(grid.height()));
    const std::size_t s = grid.index(start);
    const std::size_t goal_i = grid.index(goal);
    stamp_[s] = generation_;
    g_[s] = 0;
    closed_[s] = 0;
    push({octile_distance(start, goal), octile_distance(start, goal), static_cast<std::uint32_t>(s)});

    while (!heap_.empty()) {
        const HeapNode cur = pop();
        const std::size_t ci = cur.index;
        if (closed_[ci] != 0) {
            continue;  // entrada obsoleta del montículo (se reinsertó con menor g)
        }
        closed_[ci] = 1;
        ++expanded_total_;
        if (ci == goal_i) {
            if (path != nullptr) {
                path->clear();
                for (std::size_t i = goal_i; i != s; i = parent_[i]) {
                    path->push_back(grid.coord(i));
                }
                std::ranges::reverse(*path);
            }
            return g_[ci];
        }
        const TileCoord c = grid.coord(ci);
        for (const Direction& d : kDirections) {
            const TileCoord n{c.x + d.dx, c.y + d.dy};
            if (!rect.contains(n) || !grid.can_step(c, d)) {
                continue;
            }
            const std::size_t ni = grid.index(n);
            const std::int32_t g = g_[ci] + d.cost;
            if (!visited(ni)) {
                stamp_[ni] = generation_;
                closed_[ni] = 0;
                g_[ni] = kUnreached;
            }
            if (closed_[ni] == 0 && g < g_[ni]) {
                g_[ni] = g;
                parent_[ni] = static_cast<std::uint32_t>(ci);
                const std::int32_t h = octile_distance(n, goal);
                push({g + h, h, static_cast<std::uint32_t>(ni)});
            }
        }
    }
    return std::nullopt;
}

void GridSearch::dijkstra(const PassGrid& grid, TileCoord source, const SearchRect& rect, const SectorMask& mask) {
    begin(static_cast<std::size_t>(grid.width()) * static_cast<std::size_t>(grid.height()));
    if (!grid.passable(source) || !rect.contains(source) || !mask.contains(source)) {
        return;
    }
    const std::size_t s = grid.index(source);
    stamp_[s] = generation_;
    g_[s] = 0;
    closed_[s] = 0;
    push({0, 0, static_cast<std::uint32_t>(s)});
    while (!heap_.empty()) {
        const HeapNode cur = pop();
        const std::size_t ci = cur.index;
        if (closed_[ci] != 0) {
            continue;
        }
        closed_[ci] = 1;
        ++expanded_total_;
        const TileCoord c = grid.coord(ci);
        for (const Direction& d : kDirections) {
            const TileCoord n{c.x + d.dx, c.y + d.dy};
            if (!rect.contains(n) || !mask.contains(n) || !grid.can_step(c, d)) {
                continue;
            }
            const std::size_t ni = grid.index(n);
            const std::int32_t g = g_[ci] + d.cost;
            if (!visited(ni)) {
                stamp_[ni] = generation_;
                closed_[ni] = 0;
                g_[ni] = kUnreached;
            }
            if (closed_[ni] == 0 && g < g_[ni]) {
                g_[ni] = g;
                push({g, 0, static_cast<std::uint32_t>(ni)});
            }
        }
    }
}

std::int32_t GridSearch::cost_at(const PassGrid& grid, TileCoord c) const noexcept {
    if (!grid.contains(c)) {
        return kUnreached;
    }
    const std::size_t i = grid.index(c);
    return visited(i) ? g_[i] : kUnreached;
}

}  // namespace rts::sim
