#include <NF/AI/FlowField.hpp>

#include <cmath>
#include <queue>

namespace nf::ai {

namespace {

struct QNode {
    u32 cost = 0;
    FlowCellCoord coord;

    bool operator>(const QNode& o) const noexcept {
        return cost > o.cost;
    }
};

struct NeighborStep {
    i32 dx;
    i32 dy;
    u32 distance_weight; // 10 for cardinal, 14 for diagonal (~1.414 * 10)
    bool is_diagonal;
};

constexpr NeighborStep k_neighbors[8] = {
    { 0, -1, 10, false}, // N
    { 1,  0, 10, false}, // E
    { 0,  1, 10, false}, // S
    {-1,  0, 10, false}, // W
    { 1, -1, 14, true }, // NE
    { 1,  1, 14, true }, // SE
    {-1,  1, 14, true }, // SW
    {-1, -1, 14, true }  // NW
};

} // namespace

FlowFieldGrid::FlowFieldGrid(u32 width, u32 height, f32 cell_size, Vec2 origin) {
    resize(width, height, cell_size, origin);
}

void FlowFieldGrid::resize(u32 width, u32 height, f32 cell_size, Vec2 origin) {
    m_width = std::max(2u, width);
    m_height = std::max(2u, height);
    m_cell_size = std::max(0.01f, cell_size);
    m_origin = origin;

    usize total = static_cast<usize>(m_width) * m_height;
    m_cost_field.assign(total, 1); // Default cost = 1 (normal ground)
    m_integration_field.assign(total, kIntegrationMax);
    m_vector_field.assign(total, Vec2{0.0f, 0.0f});
}

void FlowFieldGrid::set_cost(u32 x, u32 y, u8 cost) {
    if (x < m_width && y < m_height) {
        m_cost_field[static_cast<usize>(y) * m_width + x] = cost;
    }
}

u8 FlowFieldGrid::get_cost(u32 x, u32 y) const {
    if (x < m_width && y < m_height) {
        return m_cost_field[static_cast<usize>(y) * m_width + x];
    }
    return kCostImpassable;
}

void FlowFieldGrid::fill_cost(u8 cost) {
    std::fill(m_cost_field.begin(), m_cost_field.end(), cost);
}

void FlowFieldGrid::set_blocked(u32 x, u32 y, bool blocked) {
    set_cost(x, y, blocked ? kCostImpassable : 1);
}

u16 FlowFieldGrid::get_integration(u32 x, u32 y) const {
    if (x < m_width && y < m_height) {
        return m_integration_field[static_cast<usize>(y) * m_width + x];
    }
    return kIntegrationMax;
}

Vec2 FlowFieldGrid::get_vector(u32 x, u32 y) const {
    if (x < m_width && y < m_height) {
        return m_vector_field[static_cast<usize>(y) * m_width + x];
    }
    return Vec2{0.0f, 0.0f};
}

void FlowFieldGrid::compute_field(u32 target_x, u32 target_y) {
    std::vector<FlowCellCoord> goals;
    if (target_x < m_width && target_y < m_height) {
        goals.push_back(FlowCellCoord{static_cast<i32>(target_x), static_cast<i32>(target_y)});
    }
    compute_field_multi(goals);
}

void FlowFieldGrid::compute_field_multi(const std::vector<FlowCellCoord>& goals) {
    generate_integration_field(goals);
    generate_vector_field();
}

void FlowFieldGrid::generate_integration_field(const std::vector<FlowCellCoord>& goals) {
    std::fill(m_integration_field.begin(), m_integration_field.end(), kIntegrationMax);

    std::priority_queue<QNode, std::vector<QNode>, std::greater<QNode>> pq;

    for (const auto& g : goals) {
        if (in_bounds(g.x, g.y)) {
            usize idx = static_cast<usize>(g.y) * m_width + g.x;
            if (m_cost_field[idx] < kCostImpassable) {
                m_integration_field[idx] = 0;
                pq.push(QNode{0, g});
            }
        }
    }

    while (!pq.empty()) {
        QNode current = pq.top();
        pq.pop();

        usize cur_idx = static_cast<usize>(current.coord.y) * m_width + current.coord.x;
        if (current.cost > m_integration_field[cur_idx]) {
            continue;
        }

        for (const auto& n : k_neighbors) {
            i32 nx = current.coord.x + n.dx;
            i32 ny = current.coord.y + n.dy;

            if (!in_bounds(nx, ny)) {
                continue;
            }

            // Prevent cutting diagonally through adjacent impassable blocks
            if (n.is_diagonal) {
                if (get_cost(static_cast<u32>(current.coord.x + n.dx), static_cast<u32>(current.coord.y)) == kCostImpassable ||
                    get_cost(static_cast<u32>(current.coord.x), static_cast<u32>(current.coord.y + n.dy)) == kCostImpassable) {
                    continue;
                }
            }

            usize n_idx = static_cast<usize>(ny) * m_width + nx;
            u8 cost = m_cost_field[n_idx];
            if (cost == kCostImpassable) {
                continue;
            }

            u32 step_cost = static_cast<u32>(cost) * n.distance_weight;
            u32 new_cost = current.cost + step_cost;

            if (new_cost < m_integration_field[n_idx]) {
                m_integration_field[n_idx] = static_cast<u16>(std::min(static_cast<u32>(kIntegrationMax), new_cost));
                pq.push(QNode{new_cost, FlowCellCoord{nx, ny}});
            }
        }
    }
}

void FlowFieldGrid::generate_vector_field() {
    for (i32 y = 0; y < static_cast<i32>(m_height); ++y) {
        for (i32 x = 0; x < static_cast<i32>(m_width); ++x) {
            usize idx = static_cast<usize>(y) * m_width + x;

            if (m_cost_field[idx] == kCostImpassable || m_integration_field[idx] == 0 || m_integration_field[idx] == kIntegrationMax) {
                m_vector_field[idx] = Vec2{0.0f, 0.0f};
                continue;
            }

            u16 lowest_cost = m_integration_field[idx];
            i32 best_dx = 0;
            i32 best_dy = 0;

            for (const auto& n : k_neighbors) {
                i32 nx = x + n.dx;
                i32 ny = y + n.dy;

                if (!in_bounds(nx, ny)) {
                    continue;
                }

                if (n.is_diagonal) {
                    if (get_cost(static_cast<u32>(x + n.dx), static_cast<u32>(y)) == kCostImpassable ||
                        get_cost(static_cast<u32>(x), static_cast<u32>(y + n.dy)) == kCostImpassable) {
                        continue;
                    }
                }

                usize n_idx = static_cast<usize>(ny) * m_width + nx;
                u16 n_cost = m_integration_field[n_idx];

                if (n_cost < lowest_cost) {
                    lowest_cost = n_cost;
                    best_dx = n.dx;
                    best_dy = n.dy;
                }
            }

            if (best_dx != 0 || best_dy != 0) {
                Vec2 dir{static_cast<f32>(best_dx), static_cast<f32>(best_dy)};
                m_vector_field[idx] = dir.normalized();
            } else {
                m_vector_field[idx] = Vec2{0.0f, 0.0f};
            }
        }
    }
}

bool FlowFieldGrid::world_to_grid(const Vec2& world_pos, i32& out_x, i32& out_y) const {
    f32 gx = (world_pos.x - m_origin.x) / m_cell_size;
    f32 gy = (world_pos.y - m_origin.y) / m_cell_size;

    out_x = static_cast<i32>(std::floor(gx));
    out_y = static_cast<i32>(std::floor(gy));

    return in_bounds(out_x, out_y);
}

Vec2 FlowFieldGrid::grid_to_world(i32 x, i32 y) const {
    return Vec2{
        m_origin.x + (static_cast<f32>(x) + 0.5f) * m_cell_size,
        m_origin.y + (static_cast<f32>(y) + 0.5f) * m_cell_size
    };
}

Vec2 FlowFieldGrid::sample_flow_world(const Vec2& world_pos) const {
    i32 gx = 0;
    i32 gy = 0;
    if (!world_to_grid(world_pos, gx, gy)) {
        return Vec2{0.0f, 0.0f};
    }

    // Bilinear interpolation between cell centers
    f32 norm_x = (world_pos.x - m_origin.x) / m_cell_size - 0.5f;
    f32 norm_y = (world_pos.y - m_origin.y) / m_cell_size - 0.5f;

    i32 x0 = static_cast<i32>(std::floor(norm_x));
    i32 y0 = static_cast<i32>(std::floor(norm_y));
    i32 x1 = x0 + 1;
    i32 y1 = y0 + 1;

    f32 tx = norm_x - static_cast<f32>(x0);
    f32 ty = norm_y - static_cast<f32>(y0);

    Vec2 v00 = in_bounds(x0, y0) ? get_vector(static_cast<u32>(x0), static_cast<u32>(y0)) : get_vector(static_cast<u32>(gx), static_cast<u32>(gy));
    Vec2 v10 = in_bounds(x1, y0) ? get_vector(static_cast<u32>(x1), static_cast<u32>(y0)) : v00;
    Vec2 v01 = in_bounds(x0, y1) ? get_vector(static_cast<u32>(x0), static_cast<u32>(y1)) : v00;
    Vec2 v11 = in_bounds(x1, y1) ? get_vector(static_cast<u32>(x1), static_cast<u32>(y1)) : v10;

    Vec2 top = v00 + (v10 - v00) * tx;
    Vec2 bot = v01 + (v11 - v01) * tx;
    Vec2 blended = top + (bot - top) * ty;

    f32 len_sq = blended.x * blended.x + blended.y * blended.y;
    if (len_sq > 1e-4f) {
        return blended * (1.0f / std::sqrt(len_sq));
    }

    return get_vector(static_cast<u32>(gx), static_cast<u32>(gy));
}

} // namespace nf::ai
