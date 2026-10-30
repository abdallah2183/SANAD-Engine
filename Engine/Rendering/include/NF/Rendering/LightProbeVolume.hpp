#pragma once

#include <NF/Rendering/SphericalHarmonics.hpp>
#include <vector>

namespace nf::rendering {

/// An individual spatial irradiance probe.
struct LightProbe {
    Vec3 position{};
    SH9Color sh{};
    bool valid = true;
};

/// 3D Light Probe Volume: a regular grid of Spherical Harmonics probes providing
/// smooth indirect diffuse irradiance (GI) for dynamic entities and characters.
class LightProbeVolume {
public:
    LightProbeVolume() = default;

    /// Configures and allocates a 3D grid of probes spanning [min_b, max_b].
    /// Minimum dimension is 2 in each axis (making at least 2x2x2 = 8 probes).
    void initialize_grid(const Vec3& min_b, const Vec3& max_b, u32 nx, u32 ny, u32 nz);

    [[nodiscard]] bool is_valid() const {
        return m_dim_x >= 2 && m_dim_y >= 2 && m_dim_z >= 2 && !m_probes.empty();
    }

    [[nodiscard]] u32 total_probes() const {
        return static_cast<u32>(m_probes.size());
    }

    [[nodiscard]] u32 dim_x() const { return m_dim_x; }
    [[nodiscard]] u32 dim_y() const { return m_dim_y; }
    [[nodiscard]] u32 dim_z() const { return m_dim_z; }

    [[nodiscard]] const Vec3& min_bounds() const { return m_min_bounds; }
    [[nodiscard]] const Vec3& max_bounds() const { return m_max_bounds; }
    [[nodiscard]] Vec3 cell_size() const;

    [[nodiscard]] u32 probe_index(u32 ix, u32 iy, u32 iz) const;
    [[nodiscard]] Vec3 probe_position(u32 ix, u32 iy, u32 iz) const;

    [[nodiscard]] const LightProbe& get_probe(u32 ix, u32 iy, u32 iz) const;
    LightProbe& get_probe(u32 ix, u32 iy, u32 iz);

    /// Samples interpolated SH9 irradiance at any arbitrary world position using trilinear filtering.
    [[nodiscard]] SH9Color sample_irradiance(const Vec3& world_pos) const;

    /// Samples and evaluates diffuse irradiance in a given surface normal direction.
    [[nodiscard]] Vec3 evaluate_irradiance(const Vec3& world_pos, const Vec3& normal) const;

    /// Bakes sky atmospheric ambient into all probes in the volume.
    void bake_sky(const Vec3& zenith, const Vec3& horizon, const Vec3& ground);

    /// Bakes directional light irradiance into all probes.
    void bake_directional_light(const Vec3& light_dir, const Vec3& color);

    /// Bakes a point light with spherical radius falloff into probes within its influence.
    void bake_point_light(const Vec3& light_pos, const Vec3& color, float radius);

    /// Clears all probes to black.
    void clear();

private:
    Vec3 m_min_bounds{0.0f};
    Vec3 m_max_bounds{0.0f};
    u32 m_dim_x = 0;
    u32 m_dim_y = 0;
    u32 m_dim_z = 0;
    std::vector<LightProbe> m_probes;
};

} // namespace nf::rendering
