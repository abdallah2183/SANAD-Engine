#pragma once

#include <NF/Core/Math.hpp>
#include <array>
#include <algorithm>

namespace nf::rendering {

/// Second-order Spherical Harmonics (9 coefficients per RGB channel).
/// Encodes low-frequency directional irradiance for diffuse indirect lighting (GI).
struct SH9Color {
    std::array<Vec3, 9> coeffs{};

    SH9Color() = default;

    constexpr explicit SH9Color(const Vec3& constant_ambient) {
        // L00 encodes constant isotropic ambient: L00 * Y00 * A0 = ambient
        // where Y00 = 0.282095, A0 = pi => c4 = 0.886227
        coeffs[0] = constant_ambient * (1.0f / 0.886227f);
    }

    SH9Color& operator+=(const SH9Color& o) {
        for (size_t i = 0; i < 9; ++i) coeffs[i] += o.coeffs[i];
        return *this;
    }

    SH9Color& operator*=(float scalar) {
        for (size_t i = 0; i < 9; ++i) coeffs[i] = coeffs[i] * scalar;
        return *this;
    }

    SH9Color operator+(const SH9Color& o) const {
        SH9Color res = *this;
        res += o;
        return res;
    }

    SH9Color operator*(float scalar) const {
        SH9Color res = *this;
        res *= scalar;
        return res;
    }

    /// Evaluates diffuse irradiance in the given world normal direction.
    /// Ramamoorthi & Hanrahan (2001) analytical formulation.
    [[nodiscard]] Vec3 evaluate(const Vec3& normal) const {
        const float c1 = 0.429043f;
        const float c2 = 0.511664f;
        const float c3 = 0.743125f;
        const float c4 = 0.886227f;

        const Vec3 n = normal.normalized();
        Vec3 res = coeffs[0] * c4
                 + (coeffs[1] * n.y + coeffs[2] * n.z + coeffs[3] * n.x) * (2.0f * c2)
                 + (coeffs[4] * (n.x * n.y) + coeffs[5] * (n.y * n.z) + coeffs[7] * (n.x * n.z)) * (2.0f * c1)
                 + coeffs[6] * (c3 * (3.0f * n.y * n.y - 1.0f))
                 + coeffs[8] * (c1 * (n.x * n.x - n.z * n.z));

        return {std::max(0.0f, res.x), std::max(0.0f, res.y), std::max(0.0f, res.z)};
    }

    /// Linear interpolation between two SH9 color sets.
    static SH9Color lerp(const SH9Color& a, const SH9Color& b, float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        SH9Color res;
        for (size_t i = 0; i < 9; ++i) {
            res.coeffs[i] = {
                nf::lerp(a.coeffs[i].x, b.coeffs[i].x, t),
                nf::lerp(a.coeffs[i].y, b.coeffs[i].y, t),
                nf::lerp(a.coeffs[i].z, b.coeffs[i].z, t)
            };
        }
        return res;
    }

    /// Project a distant directional light (pointing along light_dir, arriving from -light_dir) into SH9.
    static SH9Color from_directional_light(const Vec3& light_dir, const Vec3& color_intensity) {
        SH9Color sh;
        // Direction towards the light source:
        const Vec3 l = (-light_dir).normalized();

        sh.coeffs[0] = color_intensity * 0.282095f;
        sh.coeffs[1] = color_intensity * (0.488603f * l.y);
        sh.coeffs[2] = color_intensity * (0.488603f * l.z);
        sh.coeffs[3] = color_intensity * (0.488603f * l.x);
        sh.coeffs[4] = color_intensity * (1.092548f * l.x * l.y);
        sh.coeffs[5] = color_intensity * (1.092548f * l.y * l.z);
        sh.coeffs[6] = color_intensity * (0.315392f * (3.0f * l.y * l.y - 1.0f));
        sh.coeffs[7] = color_intensity * (1.092548f * l.x * l.z);
        sh.coeffs[8] = color_intensity * (0.546274f * (l.x * l.x - l.z * l.z));
        return sh;
    }

    /// Project an atmospheric three-band sky gradient (Zenith, Horizon, Ground) into SH9.
    static SH9Color from_sky_hemisphere(const Vec3& zenith, const Vec3& horizon, const Vec3& ground) {
        SH9Color sh;
        // Average ambient baseline (monopole L00)
        const Vec3 avg = (zenith + horizon * 2.0f + ground) * 0.25f;
        sh.coeffs[0] = avg * (1.0f / 0.886227f);

        // Vertical sky gradient (dipole L1-1 along world-up Y)
        const Vec3 vert_gradient = (zenith - ground) * 0.5f;
        sh.coeffs[1] = vert_gradient * (1.0f / (2.0f * 0.511664f));

        // Subtle horizon curvature along band 2 (quadrupole L20)
        const Vec3 curvature = (horizon * 2.0f - zenith - ground) * 0.25f;
        sh.coeffs[6] = curvature * (1.0f / 0.743125f);
        return sh;
    }

    /// Project an attenuated point light into a probe at probe_pos.
    static SH9Color from_point_light(const Vec3& probe_pos, const Vec3& light_pos,
                                     const Vec3& color_intensity, float radius) {
        const Vec3 to_light = light_pos - probe_pos;
        const float dist_sq = to_light.length_sq();
        if (dist_sq >= radius * radius || dist_sq < 1e-4f) {
            return SH9Color{};
        }
        const float dist = std::sqrt(dist_sq);
        const float atten = std::max(0.0f, 1.0f - (dist / radius));
        const float falloff = (atten * atten) / std::max(1.0f, dist_sq * 0.25f);
        const Vec3 effective_color = color_intensity * falloff;
        return from_directional_light(-to_light.normalized(), effective_color);
    }
};

} // namespace nf::rendering
