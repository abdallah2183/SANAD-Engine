#include <NF/Water/WaterRipple.hpp>

namespace nf::water {

WaterRippleGrid::WaterRippleGrid(const WaterRippleOptions& options) {
    init(options);
}

void WaterRippleGrid::init(const WaterRippleOptions& options) {
    m_options = options;
    m_options.resolution = std::clamp(m_options.resolution, 16u, 512u);
    m_options.world_size = std::max(1.0f, m_options.world_size);
    m_options.wave_speed = std::max(0.1f, m_options.wave_speed);
    m_options.damping = std::clamp(m_options.damping, 0.8f, 0.999f);
    m_options.max_height = std::max(0.1f, m_options.max_height);

    m_cell_size = m_options.world_size / static_cast<f32>(m_options.resolution - 1);

    const size_t total_cells = static_cast<size_t>(m_options.resolution) * m_options.resolution;
    m_height_current.assign(total_cells, 0.0f);
    m_height_previous.assign(total_cells, 0.0f);
    m_height_next.assign(total_cells, 0.0f);
}

void WaterRippleGrid::clear() {
    std::fill(m_height_current.begin(), m_height_current.end(), 0.0f);
    std::fill(m_height_previous.begin(), m_height_previous.end(), 0.0f);
    std::fill(m_height_next.begin(), m_height_next.end(), 0.0f);
}

bool WaterRippleGrid::world_to_grid(f32 world_x, f32 world_z, f32& out_gx, f32& out_gz) const {
    const f32 half_size = m_options.world_size * 0.5f;
    const f32 local_x = (world_x - m_options.world_center.x) + half_size;
    const f32 local_z = (world_z - m_options.world_center.y) + half_size;

    if (local_x < 0.0f || local_x > m_options.world_size || local_z < 0.0f || local_z > m_options.world_size) {
        return false;
    }

    out_gx = local_x / m_cell_size;
    out_gz = local_z / m_cell_size;
    return true;
}

void WaterRippleGrid::step(f32 dt) {
    if (dt <= 0.0f) {
        return;
    }

    // CFL condition: wave_speed * dt / cell_size <= 0.5 for stable 2D explicit scheme
    const f32 max_substep_dt = 0.5f * (m_cell_size / m_options.wave_speed);
    const u32 num_substeps = std::max(1u, static_cast<u32>(std::ceil(dt / max_substep_dt)));
    const f32 sub_dt = dt / static_cast<f32>(num_substeps);

    const f32 c_sq_dt_sq_over_dx_sq = (m_options.wave_speed * sub_dt / m_cell_size) * (m_options.wave_speed * sub_dt / m_cell_size);
    const f32 sub_damping = std::pow(m_options.damping, sub_dt);
    const u32 res = m_options.resolution;

    for (u32 sub = 0; sub < num_substeps; ++sub) {
        // Interior update via discrete Laplacian
        for (u32 z = 1; z < res - 1; ++z) {
            for (u32 x = 1; x < res - 1; ++x) {
                const size_t idx = index(x, z);
                const f32 cur = m_height_current[idx];
                const f32 prev = m_height_previous[idx];

                const f32 laplacian = m_height_current[index(x + 1, z)] +
                                      m_height_current[index(x - 1, z)] +
                                      m_height_current[index(x, z + 1)] +
                                      m_height_current[index(x, z - 1)] -
                                      (4.0f * cur);

                const f32 next_h = (2.0f * cur - prev + (c_sq_dt_sq_over_dx_sq * laplacian)) * sub_damping;
                m_height_next[idx] = std::clamp(next_h, -m_options.max_height, m_options.max_height);
            }
        }

        // Absorbing boundary dampening
        for (u32 x = 0; x < res; ++x) {
            m_height_next[index(x, 0)] = m_height_next[index(x, 1)] * 0.5f;
            m_height_next[index(x, res - 1)] = m_height_next[index(x, res - 2)] * 0.5f;
        }
        for (u32 z = 0; z < res; ++z) {
            m_height_next[index(0, z)] = m_height_next[index(1, z)] * 0.5f;
            m_height_next[index(res - 1, z)] = m_height_next[index(res - 2, z)] * 0.5f;
        }

        m_height_previous = m_height_current;
        m_height_current = m_height_next;
    }
}

void WaterRippleGrid::add_ripple(const Vec2& world_pos, f32 radius, f32 strength) {
    if (radius <= 0.0f) {
        return;
    }

    f32 center_gx = 0.0f;
    f32 center_gz = 0.0f;
    if (!world_to_grid(world_pos.x, world_pos.y, center_gx, center_gz)) {
        return;
    }

    const f32 grid_radius = radius / m_cell_size;
    const i32 min_x = std::max(1, static_cast<i32>(std::floor(center_gx - grid_radius)));
    const i32 max_x = std::min(static_cast<i32>(m_options.resolution - 2), static_cast<i32>(std::ceil(center_gx + grid_radius)));
    const i32 min_z = std::max(1, static_cast<i32>(std::floor(center_gz - grid_radius)));
    const i32 max_z = std::min(static_cast<i32>(m_options.resolution - 2), static_cast<i32>(std::ceil(center_gz + grid_radius)));

    for (i32 z = min_z; z <= max_z; ++z) {
        for (i32 x = min_x; x <= max_x; ++x) {
            const f32 dx = (static_cast<f32>(x) - center_gx) * m_cell_size;
            const f32 dz = (static_cast<f32>(z) - center_gz) * m_cell_size;
            const f32 dist = std::sqrt(dx * dx + dz * dz);

            if (dist < radius) {
                // Smooth cosine drop falloff
                const f32 weight = 0.5f * (1.0f + std::cos(PI * (dist / radius)));
                const size_t idx = index(static_cast<u32>(x), static_cast<u32>(z));
                const f32 delta = strength * weight;
                m_height_current[idx] = std::clamp(m_height_current[idx] + delta, -m_options.max_height, m_options.max_height);
                m_height_previous[idx] = m_height_current[idx];
            }
        }
    }
}

void WaterRippleGrid::add_wake(const Vec3& world_pos, const Vec3& velocity, f32 radius, f32 strength, f32 dt) {
    const f32 speed = velocity.length();
    if (speed < 0.05f || radius <= 0.0f || dt <= 0.0f) {
        return;
    }

    const f32 impulse = strength * std::min(1.0f, speed / 5.0f) * std::clamp(dt, 0.001f, 0.1f);
    add_ripple(Vec2{world_pos.x, world_pos.z}, radius, impulse);
}

f32 WaterRippleGrid::sample_height(f32 world_x, f32 world_z) const {
    f32 gx = 0.0f;
    f32 gz = 0.0f;
    if (!world_to_grid(world_x, world_z, gx, gz)) {
        return 0.0f;
    }

    const u32 x0 = std::clamp(static_cast<u32>(std::floor(gx)), 0u, m_options.resolution - 2);
    const u32 z0 = std::clamp(static_cast<u32>(std::floor(gz)), 0u, m_options.resolution - 2);
    const u32 x1 = x0 + 1;
    const u32 z1 = z0 + 1;

    const f32 tx = gx - static_cast<f32>(x0);
    const f32 tz = gz - static_cast<f32>(z0);

    const f32 h00 = m_height_current[index(x0, z0)];
    const f32 h10 = m_height_current[index(x1, z0)];
    const f32 h01 = m_height_current[index(x0, z1)];
    const f32 h11 = m_height_current[index(x1, z1)];

    return (1.0f - tx) * ((1.0f - tz) * h00 + tz * h01) +
           tx * ((1.0f - tz) * h10 + tz * h11);
}

Vec3 WaterRippleGrid::sample_normal(f32 world_x, f32 world_z) const {
    const f32 eps = m_cell_size;
    const f32 h_left = sample_height(world_x - eps, world_z);
    const f32 h_right = sample_height(world_x + eps, world_z);
    const f32 h_down = sample_height(world_x, world_z - eps);
    const f32 h_up = sample_height(world_x, world_z + eps);

    const f32 dh_dx = (h_right - h_left) / (2.0f * eps);
    const f32 dh_dz = (h_up - h_down) / (2.0f * eps);

    Vec3 normal{-dh_dx, 1.0f, -dh_dz};
    return normal.normalized();
}

f32 WaterRippleGrid::sample_velocity(f32 world_x, f32 world_z) const {
    f32 gx = 0.0f;
    f32 gz = 0.0f;
    if (!world_to_grid(world_x, world_z, gx, gz)) {
        return 0.0f;
    }

    const u32 x0 = std::clamp(static_cast<u32>(std::floor(gx)), 0u, m_options.resolution - 1);
    const u32 z0 = std::clamp(static_cast<u32>(std::floor(gz)), 0u, m_options.resolution - 1);
    const size_t idx = index(x0, z0);

    return m_height_current[idx] - m_height_previous[idx];
}

f32 WaterRippleGrid::total_energy() const noexcept {
    f32 sum = 0.0f;
    for (f32 h : m_height_current) {
        sum += h * h;
    }
    return sum;
}

CombinedWaterSample WaterRippleGrid::evaluate_combined(
    f32 world_x,
    f32 world_z,
    const std::vector<Wave>& waves,
    const WaterOptions& water_opts) const
{
    CombinedWaterSample sample;
    sample.height = water_height(world_x, world_z, waves, water_opts);
    sample.normal = water_normal(world_x, world_z, waves, water_opts);

    const f32 ripple_h = sample_height(world_x, world_z);
    sample.height += ripple_h;

    const Vec3 ripple_norm = sample_normal(world_x, world_z);
    sample.normal.x += ripple_norm.x;
    sample.normal.z += ripple_norm.z;
    sample.normal = sample.normal.normalized();

    return sample;
}

} // namespace nf::water
