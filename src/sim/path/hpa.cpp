#include "sim/path/hpa.hpp"

#include <algorithm>
#include <cassert>

namespace rts::sim {

namespace {

struct AbsHeapNode {
    std::int32_t f;
    std::int32_t h;
    std::uint32_t index;
};

// Orden total (f, h, índice): mismo orden de expansión en cualquier compilador.
bool heap_greater(const AbsHeapNode& a, const AbsHeapNode& b) noexcept {
    if (a.f != b.f) {
        return a.f > b.f;
    }
    if (a.h != b.h) {
        return a.h > b.h;
    }
    return a.index > b.index;
}

}  // namespace

HpaGraph::HpaGraph(const PassGrid& grid, const HpaParams& params, GridSearch& search) : params_(params) {
    assert(params.sector_size >= 2);
    assert(params.max_portal_width >= 1);
    const std::int32_t s = params.sector_size;
    sectors_w_ = (grid.width() + s - 1) / s;
    sectors_h_ = (grid.height() + s - 1) / s;
    tile_to_node_.assign(static_cast<std::size_t>(grid.width()) * static_cast<std::size_t>(grid.height()), -1);
    sector_nodes_.resize(static_cast<std::size_t>(sectors_w_) * static_cast<std::size_t>(sectors_h_));

    // 1. Portales en los bordes entre sectores (aristas entre sectores, coste 10).
    std::vector<std::vector<TempEdge>> adj;
    for (std::int32_t sy = 0; sy < sectors_h_; ++sy) {
        for (std::int32_t sx = 0; sx < sectors_w_; ++sx) {
            const std::int32_t y0 = sy * s;
            const std::int32_t y1 = std::min(grid.height(), y0 + s);
            const std::int32_t x0 = sx * s;
            const std::int32_t x1 = std::min(grid.width(), x0 + s);
            if (sx + 1 < sectors_w_) {
                adj.resize(nodes_.size());
                add_portals_on_border(grid, adj, true, x1 - 1, y0, y1);
            }
            if (sy + 1 < sectors_h_) {
                adj.resize(nodes_.size());
                add_portals_on_border(grid, adj, false, y1 - 1, x0, x1);
            }
        }
    }
    adj.resize(nodes_.size());

    // 2. Aristas internas: Dijkstra acotado al sector desde cada nodo.
    for (std::size_t sector = 0; sector < sector_nodes_.size(); ++sector) {
        const SearchRect rect = sector_rect(static_cast<std::int32_t>(sector));
        const auto& members = sector_nodes_[sector];
        for (const std::uint32_t a : members) {
            search.dijkstra(grid, nodes_[a].tile, rect);
            for (const std::uint32_t b : members) {
                if (a == b) {
                    continue;
                }
                const std::int32_t cost = search.cost_at(grid, nodes_[b].tile);
                if (cost != kUnreached) {
                    adj[a].push_back({b, cost});
                }
            }
        }
    }

    // 3. Compactar a CSR.
    edge_begin_.resize(nodes_.size() + 1, 0);
    for (std::size_t n = 0; n < nodes_.size(); ++n) {
        edge_begin_[n + 1] = edge_begin_[n] + static_cast<std::uint32_t>(adj[n].size());
        for (const TempEdge& e : adj[n]) {
            edge_to_.push_back(e.node);
            edge_cost_.push_back(e.cost);
        }
    }
    const std::size_t abstract_nodes = nodes_.size() + 2;  // + origen y destino temporales
    stamp_.assign(abstract_nodes, 0);
    g_.assign(abstract_nodes, kUnreached);
    parent_.assign(abstract_nodes, 0);
    closed_.assign(abstract_nodes, 0);
}

SearchRect HpaGraph::sector_rect(std::int32_t sector) const noexcept {
    const std::int32_t s = params_.sector_size;
    const std::int32_t sx = sector % sectors_w_;
    const std::int32_t sy = sector / sectors_w_;
    // El último sector de cada fila/columna puede ser menor si el mapa no es múltiplo;
    // el rectángulo se recorta de todos modos al comprobar transitabilidad.
    return {sx * s, sy * s, sx * s + s, sy * s + s};
}

std::uint32_t HpaGraph::add_node(const PassGrid& grid, TileCoord tile) {
    const std::size_t ti = grid.index(tile);
    if (tile_to_node_[ti] >= 0) {
        return static_cast<std::uint32_t>(tile_to_node_[ti]);
    }
    const auto id = static_cast<std::uint32_t>(nodes_.size());
    const std::int32_t sector = sector_of(tile);
    nodes_.push_back({tile, sector});
    tile_to_node_[ti] = static_cast<std::int32_t>(id);
    sector_nodes_[static_cast<std::size_t>(sector)].push_back(id);
    return id;
}

void HpaGraph::add_portals_on_border(const PassGrid& grid, std::vector<std::vector<TempEdge>>& adj,
                                     bool vertical_border, std::int32_t fixed, std::int32_t from, std::int32_t to) {
    // vertical_border: la frontera separa las columnas fixed y fixed+1; se recorre y.
    // si no: separa las filas fixed y fixed+1; se recorre x.
    auto side_a = [&](std::int32_t t) { return vertical_border ? TileCoord{fixed, t} : TileCoord{t, fixed}; };
    auto side_b = [&](std::int32_t t) { return vertical_border ? TileCoord{fixed + 1, t} : TileCoord{t, fixed + 1}; };
    auto open = [&](std::int32_t t) { return grid.passable(side_a(t)) && grid.passable(side_b(t)); };

    std::int32_t t = from;
    while (t < to) {
        if (!open(t)) {
            ++t;
            continue;
        }
        std::int32_t end = t;
        while (end < to && open(end)) {
            ++end;
        }
        // Tramo [t, end): se parte en trozos de como mucho max_portal_width; un portal
        // en el centro de cada trozo.
        for (std::int32_t chunk = t; chunk < end; chunk += params_.max_portal_width) {
            const std::int32_t chunk_end = std::min(end, chunk + params_.max_portal_width);
            const std::int32_t mid = (chunk + chunk_end - 1) / 2;
            const std::uint32_t a = add_node(grid, side_a(mid));
            const std::uint32_t b = add_node(grid, side_b(mid));
            adj.resize(nodes_.size());
            adj[a].push_back({b, kStraightCost});
            adj[b].push_back({a, kStraightCost});
        }
        t = end;
    }
}

void HpaGraph::connect_temp(const PassGrid& grid, TileCoord tile, GridSearch& search,
                            std::vector<TempEdge>& out) const {
    out.clear();
    const std::int32_t sector = sector_of(tile);
    search.dijkstra(grid, tile, sector_rect(sector));
    for (const std::uint32_t n : sector_nodes_[static_cast<std::size_t>(sector)]) {
        const std::int32_t cost = search.cost_at(grid, nodes_[n].tile);
        if (cost != kUnreached) {
            out.push_back({n, cost});
        }
    }
}

std::optional<std::vector<TileCoord>> HpaGraph::find_waypoints(const PassGrid& grid, TileCoord start, TileCoord goal,
                                                               GridSearch& search) {
    if (!grid.passable(start) || !grid.passable(goal) || grid.component(start) != grid.component(goal)) {
        return std::nullopt;
    }
    if (start == goal) {
        return std::vector<TileCoord>{goal};
    }
    // Mismo sector: si hay camino sin salir de él, no hace falta el grafo abstracto.
    if (sector_of(start) == sector_of(goal) &&
        search.astar(grid, start, goal, sector_rect(sector_of(start)), nullptr).has_value()) {
        return std::vector<TileCoord>{goal};
    }

    std::vector<TempEdge> start_edges;
    std::vector<TempEdge> goal_edges;
    connect_temp(grid, start, search, start_edges);
    connect_temp(grid, goal, search, goal_edges);

    const auto start_id = static_cast<std::uint32_t>(nodes_.size());
    const std::uint32_t goal_id = start_id + 1;
    ++generation_;
    if (generation_ == 0) {
        std::ranges::fill(stamp_, 0);
        generation_ = 1;
    }
    std::vector<AbsHeapNode> heap;
    auto node_tile = [&](std::uint32_t n) { return n == start_id ? start : (n == goal_id ? goal : nodes_[n].tile); };
    auto relax = [&](std::uint32_t from, std::uint32_t to, std::int32_t cost) {
        if (stamp_[to] != generation_) {
            stamp_[to] = generation_;
            g_[to] = kUnreached;
            closed_[to] = 0;
        }
        const std::int32_t g = g_[from] + cost;
        if (closed_[to] == 0 && g < g_[to]) {
            g_[to] = g;
            parent_[to] = from;
            const std::int32_t h = octile_distance(node_tile(to), goal);
            heap.push_back({g + h, h, to});
            std::ranges::push_heap(heap, heap_greater);
        }
    };

    stamp_[start_id] = generation_;
    g_[start_id] = 0;
    closed_[start_id] = 0;
    heap.push_back({octile_distance(start, goal), octile_distance(start, goal), start_id});
    while (!heap.empty()) {
        std::ranges::pop_heap(heap, heap_greater);
        const AbsHeapNode cur = heap.back();
        heap.pop_back();
        if (closed_[cur.index] != 0) {
            continue;
        }
        closed_[cur.index] = 1;
        if (cur.index == goal_id) {
            std::vector<TileCoord> waypoints;
            for (std::uint32_t n = goal_id; n != start_id; n = parent_[n]) {
                waypoints.push_back(node_tile(n));
            }
            std::ranges::reverse(waypoints);
            // Un portal que coincide con el origen no aporta nada.
            if (!waypoints.empty() && waypoints.front() == start) {
                waypoints.erase(waypoints.begin());
            }
            return waypoints;
        }
        if (cur.index == start_id) {
            for (const TempEdge& e : start_edges) {
                relax(start_id, e.node, e.cost);
            }
            continue;
        }
        for (std::uint32_t e = edge_begin_[cur.index]; e < edge_begin_[cur.index + 1]; ++e) {
            relax(cur.index, edge_to_[e], edge_cost_[e]);
        }
        for (const TempEdge& e : goal_edges) {
            if (e.node == cur.index) {
                relax(cur.index, goal_id, e.cost);
            }
        }
    }
    return std::nullopt;
}

std::optional<std::int32_t> HpaGraph::refine(const PassGrid& grid, TileCoord from, TileCoord to, GridSearch& search,
                                             std::vector<TileCoord>& out) const {
    const SearchRect a = sector_rect(sector_of(from));
    const SearchRect b = sector_rect(sector_of(to));
    const SearchRect box{std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
    return search.astar(grid, from, to, box, &out);
}

}  // namespace rts::sim
