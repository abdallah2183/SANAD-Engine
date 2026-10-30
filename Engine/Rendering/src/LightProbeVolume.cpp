#include <NF/Rendering/LightProbeVolume.hpp>
#include <algorithm>
#include <cmath>

namespace nf::rendering {

void LightProbeVolume::initialize_grid(const Vec3& min_b, const Vec3& max_b,
                                      u32 nx, u32 ny, u32 nz) {
    m_dim_x = std::max(2u, nx);
    m_dim_y = std::max(2u, ny);
    m_dim_z = std::max(2u, nz);

    m_min_bounds = {
        std::min(min_b.x, max_b.x),
        std::min(min_b.y, max_b.y),
        std::min(min_b.z, max_b.z)
    };
    m_max_bounds = {
        std::max(min_b.x, max_b.x),
        std::max(min_b.y, max_b.y),
        std::max(min_b.z, max_b.z)
    };

    // Ensure non-degenerate extents
    for (int axis = 0; axis < 3; ++axis) {
        float* pmin = (&m_min_bounds.x) + axis;
        float* pmax = (&m_max_bounds.x) + axis;
        if (*pmax - *pmin < 1e-3f) {
            *pmax = *pmin + 1.0f;
        }
    }

    const u32 count = m_dim_x * m_dim_y * m_dim_z;
    m_probes.clear();
    m_probes.resize(count);

    for (u32 iz = 0; iz < m_dim_z; ++iz) {
        for (u32 iy = 0; iy < m_dim_y; ++iy) {
            for (u32 ix = 0; ix < m_dim_x; ++ix) {
                const u32 idx = probe_index(ix, iy, iz);
                m_probes[idx].position = probe_position(ix, iy, iz);
                m_probes[idx].sh = SH9Color{};
                m_probes[idx].valid = true;
            }
        }
    }
}

Vec3 LightProbeVolume::cell_size() const {
    if (!is_valid()) return Vec3{1.0f, 1.0f, 1.0f};
    return {
        (m_max_bounds.x - m_min_bounds.x) / static_cast<float>(m_dim_x - 1),
        (m_max_bounds.y - m_min_bounds.y) / static_cast<float>(m_dim_y - 1),
        (m_max_bounds.z - m_min_bounds.z) / static_cast<float>(m_dim_z - 1)
    };
}

u32 LightProbeVolume::probe_index(u32 ix, u32 iy, u32 iz) const {
    return ix + iy * m_dim_x + iz * (m_dim_x * m_dim_y);
}

Vec3 LightProbeVolume::probe_position(u32 ix, u32 iy, u32 iz) const {
    const float tx = (m_dim_x > 1) ? static_cast<float>(ix) / static_cast<float>(m_dim_x - 1) : 0.0f;
    const float ty = (m_dim_y > 1) ? static_cast<float>(iy) / static_cast<float>(m_dim_y - 1) : 0.0f;
    const float tz = (m_dim_z > 1) ? static_cast<float>(iz) / static_cast<float>(m_dim_z - 1) : 0.0f;

    return {
        nf::lerp(m_min_bounds.x, m_max_bounds.x, tx),
        nf::lerp(m_min_bounds.y, m_max_bounds.y, ty),
        nf::lerp(m_min_bounds.z, m_max_bounds.z, tz)
    };
}

const LightProbe& LightProbeVolume::get_probe(u32 ix, u32 iy, u32 iz) const {
    ix = std::min(ix, m_dim_x - 1);
    iy = std::min(iy, m_dim_y - 1);
    iz = std::min(iz, m_dim_z - 1);
    return m_probes[probe_index(ix, iy, iz)];
}

LightProbe& LightProbeVolume::get_probe(u32 ix, u32 iy, u32 iz) {
    ix = std::min(ix, m_dim_x - 1);
    iy = std::min(iy, m_dim_y - 1);
    iz = std::min(iz, m_dim_z - 1);
    return m_probes[probe_index(ix, iy, iz)];
}

SH9Color LightProbeVolume::sample_irradiance(const Vec3& world_pos) const {
    if (!is_valid()) return SH9Color{};

    // Normalized coordinate along each dimension [0, dim - 1]
    const float fx = std::clamp((world_pos.x - m_min_bounds.x) / (m_max_bounds.x - m_min_bounds.x), 0.0f, 1.0f) * (m_dim_x - 1);
    const float fy = std::clamp((world_pos.y - m_min_bounds.y) / (m_max_bounds.y - m_min_bounds.y), 0.0f, 1.0f) * (m_dim_y - 1);
    const float fz = std::clamp((world_pos.z - m_min_bounds.z) / (m_max_bounds.z - m_min_bounds.z), 0.0f, 1.0f) * (m_dim_z - 1);

    const u32 x0 = std::min(static_cast<u32>(std::floor(fx)), m_dim_x - 2);
    const u32 y0 = std::min(static_cast<u32>(std::floor(fy)), m_dim_y - 2);
    const u32 z0 = std::min(static_cast<u32>(std::floor(fz)), m_dim_z - 2);

    const u32 x1 = x0 + 1;
    const u32 y1 = y0 + 1;
    const u32 z1 = z0 + 1;

    const float u = fx - static_cast<float>(x0);
    const float v = fy - static_cast<float>(y0);
    const float w = fz - static_cast<float>(z0);

    // Fetch the 8 corner probes
    const SH9Color& c000 = get_probe(x0, y0, z0).sh;
    const SH9Color& c100 = get_probe(x1, y0, z0).sh;
    const SH9Color& c010 = get_probe(x0, y1, z0).sh;
    const SH9Color& c110 = get_probe(x1, y1, z0).sh;

    const SH9Color& c001 = get_probe(x0, y0, z1).sh;
    const SH9Color& c101 = get_probe(x1, y0, z1).sh;
    const SH9Color& c011 = get_probe(x0, y1, z1).sh;
    const SH9Color& c111 = get_probe(x1, y1, z1).sh;

    // Trilinear interpolation
    const SH9Color c00 = SH9Color::lerp(c000, c100, u);
    const SH9Color c10 = SH9Color::lerp(c010, c110, u);
    const SH9Color c01 = SH9Color::lerp(c001, c101, u);
    const SH9Color c11 = SH9Color::lerp(c011, c111, u);

    const SH9Color c0 = SH9Color::lerp(c00, c10, v);
    const SH9Color c1 = SH9Color::lerp(c01, c11, v);

    return SH9Color::lerp(c0, c1, w);
}

Vec3 LightProbeVolume::evaluate_irradiance(const Vec3& world_pos, const Vec3& normal) const {
    const SH9Color sh = sample_irradiance(world_pos);
    return sh.evaluate(normal);
}

void LightProbeVolume::bake_sky(const Vec3& zenith, const Vec3& horizon, const Vec3& ground) {
    const SH9Color sky_sh = SH9Color::from_sky_hemisphere(zenith, horizon, ground);
    for (auto& probe : m_probes) {
        probe.sh += sky_sh;
    }
}

void LightProbeVolume::bake_directional_light(const Vec3& light_dir, const Vec3& color) {
    const SH9Color dir_sh = SH9Color::from_directional_light(light_dir, color);
    for (auto& probe : m_probes) {
        probe.sh += dir_sh;
    }
}

void LightProbeVolume::bake_point_light(const Vec3& light_pos, const Vec3& color, float radius) {
    for (auto& probe : m_probes) {
        const SH9Color pt_sh = SH9Color::from_point_light(probe.position, light_pos, color, radius);
        probe.sh += pt_sh;
    }
}

void LightProbeVolume::clear() {
    for (auto& probe : m_probes) {
        probe.sh = SH9Color{};
    }
}

} // namespace nf::rendering
