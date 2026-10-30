#include <NF/Core/Noise.hpp>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace nf::core {

namespace {

inline i32 fast_floor(f32 x) {
    const i32 xi = static_cast<i32>(x);
    return x < static_cast<f32>(xi) ? xi - 1 : xi;
}

inline f32 fade(f32 t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

inline f32 lerp_f(f32 a, f32 b, f32 t) {
    return a + t * (b - a);
}

inline f32 grad_2d(u8 hash, f32 x, f32 y) {
    const u8 h = hash & 7;
    f32 u = h < 4 ? x : y;
    f32 v = h < 4 ? y : x;
    return ((h & 1) ? -u : u) + ((h & 2) ? -2.0f * v : 2.0f * v);
}

inline f32 grad_3d(u8 hash, f32 x, f32 y, f32 z) {
    const u8 h = hash & 15;
    const f32 u = h < 8 ? x : y;
    const f32 v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
    return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
}

} // namespace

NoiseGenerator::NoiseGenerator(u32 seed) {
    set_seed(seed);
}

void NoiseGenerator::set_seed(u32 seed) noexcept {
    m_seed = seed;
    std::array<u8, 256> p{};
    std::iota(p.begin(), p.end(), static_cast<u8>(0));

    u32 state = m_seed != 0 ? m_seed : 1337u;
    for (size_t i = 255; i > 0; --i) {
        state = state * 1664525u + 1013904223u;
        const size_t j = static_cast<size_t>(state % (i + 1));
        std::swap(p[i], p[j]);
    }

    for (size_t i = 0; i < 256; ++i) {
        m_perm[i] = p[i];
        m_perm[256 + i] = p[i];
    }
}

u8 NoiseGenerator::hash_coord(i32 x, i32 y) const {
    const u8 xi = static_cast<u8>(x & 255);
    const u8 yi = static_cast<u8>(y & 255);
    return m_perm[xi + m_perm[yi]];
}

u8 NoiseGenerator::hash_coord(i32 x, i32 y, i32 z) const {
    const u8 xi = static_cast<u8>(x & 255);
    const u8 yi = static_cast<u8>(y & 255);
    const u8 zi = static_cast<u8>(z & 255);
    return m_perm[xi + m_perm[yi + m_perm[zi]]];
}

f32 NoiseGenerator::perlin_2d(f32 x, f32 y) const {
    const i32 x0 = fast_floor(x);
    const i32 y0 = fast_floor(y);
    const i32 x1 = x0 + 1;
    const i32 y1 = y0 + 1;

    const f32 dx0 = x - static_cast<f32>(x0);
    const f32 dy0 = y - static_cast<f32>(y0);
    const f32 dx1 = dx0 - 1.0f;
    const f32 dy1 = dy0 - 1.0f;

    const f32 sx = fade(dx0);
    const f32 sy = fade(dy0);

    const f32 n00 = grad_2d(hash_coord(x0, y0), dx0, dy0);
    const f32 n10 = grad_2d(hash_coord(x1, y0), dx1, dy0);
    const f32 n01 = grad_2d(hash_coord(x0, y1), dx0, dy1);
    const f32 n11 = grad_2d(hash_coord(x1, y1), dx1, dy1);

    const f32 ix0 = lerp_f(n00, n10, sx);
    const f32 ix1 = lerp_f(n01, n11, sx);

    return std::clamp(lerp_f(ix0, ix1, sy) * 0.5f, -1.0f, 1.0f);
}

f32 NoiseGenerator::perlin_3d(f32 x, f32 y, f32 z) const {
    const i32 x0 = fast_floor(x);
    const i32 y0 = fast_floor(y);
    const i32 z0 = fast_floor(z);
    const i32 x1 = x0 + 1;
    const i32 y1 = y0 + 1;
    const i32 z1 = z0 + 1;

    const f32 dx0 = x - static_cast<f32>(x0);
    const f32 dy0 = y - static_cast<f32>(y0);
    const f32 dz0 = z - static_cast<f32>(z0);
    const f32 dx1 = dx0 - 1.0f;
    const f32 dy1 = dy0 - 1.0f;
    const f32 dz1 = dz0 - 1.0f;

    const f32 sx = fade(dx0);
    const f32 sy = fade(dy0);
    const f32 sz = fade(dz0);

    const f32 n000 = grad_3d(hash_coord(x0, y0, z0), dx0, dy0, dz0);
    const f32 n100 = grad_3d(hash_coord(x1, y0, z0), dx1, dy0, dz0);
    const f32 n010 = grad_3d(hash_coord(x0, y1, z0), dx0, dy1, dz0);
    const f32 n110 = grad_3d(hash_coord(x1, y1, z0), dx1, dy1, dz0);

    const f32 n001 = grad_3d(hash_coord(x0, y0, z1), dx0, dy0, dz1);
    const f32 n101 = grad_3d(hash_coord(x1, y0, z1), dx1, dy0, dz1);
    const f32 n011 = grad_3d(hash_coord(x0, y1, z1), dx0, dy1, dz1);
    const f32 n111 = grad_3d(hash_coord(x1, y1, z1), dx1, dy1, dz1);

    const f32 ix00 = lerp_f(n000, n100, sx);
    const f32 ix10 = lerp_f(n010, n110, sx);
    const f32 ix01 = lerp_f(n001, n101, sx);
    const f32 ix11 = lerp_f(n011, n111, sx);

    const f32 iy0 = lerp_f(ix00, ix10, sy);
    const f32 iy1 = lerp_f(ix01, ix11, sy);

    return std::clamp(lerp_f(iy0, iy1, sz) * 0.96f, -1.0f, 1.0f);
}

f32 NoiseGenerator::simplex_2d(f32 x, f32 y) const {
    constexpr f32 F2 = 0.36602540378f; // 0.5 * (sqrt(3) - 1)
    constexpr f32 G2 = 0.21132486540f; // (3 - sqrt(3)) / 6

    const f32 s = (x + y) * F2;
    const i32 i = fast_floor(x + s);
    const i32 j = fast_floor(y + s);

    const f32 t = static_cast<f32>(i + j) * G2;
    const f32 x0 = x - (static_cast<f32>(i) - t);
    const f32 y0 = y - (static_cast<f32>(j) - t);

    i32 i1 = 0;
    i32 j1 = 0;
    if (x0 > y0) {
        i1 = 1; j1 = 0;
    } else {
        i1 = 0; j1 = 1;
    }

    const f32 x1 = x0 - static_cast<f32>(i1) + G2;
    const f32 y1 = y0 - static_cast<f32>(j1) + G2;
    const f32 x2 = x0 - 1.0f + 2.0f * G2;
    const f32 y2 = y0 - 1.0f + 2.0f * G2;

    f32 n0 = 0.0f;
    f32 n1 = 0.0f;
    f32 n2 = 0.0f;

    f32 t0 = 0.5f - x0 * x0 - y0 * y0;
    if (t0 > 0.0f) {
        t0 *= t0;
        n0 = t0 * t0 * grad_2d(hash_coord(i, j), x0, y0);
    }

    f32 t1 = 0.5f - x1 * x1 - y1 * y1;
    if (t1 > 0.0f) {
        t1 *= t1;
        n1 = t1 * t1 * grad_2d(hash_coord(i + i1, j + j1), x1, y1);
    }

    f32 t2 = 0.5f - x2 * x2 - y2 * y2;
    if (t2 > 0.0f) {
        t2 *= t2;
        n2 = t2 * t2 * grad_2d(hash_coord(i + 1, j + 1), x2, y2);
    }

    return std::clamp(45.0f * (n0 + n1 + n2), -1.0f, 1.0f);
}

f32 NoiseGenerator::simplex_3d(f32 x, f32 y, f32 z) const {
    constexpr f32 F3 = 1.0f / 3.0f;
    constexpr f32 G3 = 1.0f / 6.0f;

    const f32 s = (x + y + z) * F3;
    const i32 i = fast_floor(x + s);
    const i32 j = fast_floor(y + s);
    const i32 k = fast_floor(z + s);

    const f32 t = static_cast<f32>(i + j + k) * G3;
    const f32 x0 = x - (static_cast<f32>(i) - t);
    const f32 y0 = y - (static_cast<f32>(j) - t);
    const f32 z0 = z - (static_cast<f32>(k) - t);

    i32 i1 = 0, j1 = 0, k1 = 0;
    i32 i2 = 0, j2 = 0, k2 = 0;

    if (x0 >= y0) {
        if (y0 >= z0) {
            i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 1; k2 = 0;
        } else if (x0 >= z0) {
            i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 0; k2 = 1;
        } else {
            i1 = 0; j1 = 0; k1 = 1; i2 = 1; j2 = 0; k2 = 1;
        }
    } else {
        if (y0 < z0) {
            i1 = 0; j1 = 0; k1 = 1; i2 = 0; j2 = 1; k2 = 1;
        } else if (x0 < z0) {
            i1 = 0; j1 = 1; k1 = 0; i2 = 0; j2 = 1; k2 = 1;
        } else {
            i1 = 0; j1 = 1; k1 = 0; i2 = 1; j2 = 1; k2 = 0;
        }
    }

    const f32 x1 = x0 - static_cast<f32>(i1) + G3;
    const f32 y1 = y0 - static_cast<f32>(j1) + G3;
    const f32 z1 = z0 - static_cast<f32>(k1) + G3;

    const f32 x2 = x0 - static_cast<f32>(i2) + 2.0f * G3;
    const f32 y2 = y0 - static_cast<f32>(j2) + 2.0f * G3;
    const f32 z2 = z0 - static_cast<f32>(k2) + 2.0f * G3;

    const f32 x3 = x0 - 1.0f + 3.0f * G3;
    const f32 y3 = y0 - 1.0f + 3.0f * G3;
    const f32 z3 = z0 - 1.0f + 3.0f * G3;

    f32 n0 = 0.0f, n1 = 0.0f, n2 = 0.0f, n3 = 0.0f;

    f32 t0 = 0.6f - x0 * x0 - y0 * y0 - z0 * z0;
    if (t0 > 0.0f) {
        t0 *= t0;
        n0 = t0 * t0 * grad_3d(hash_coord(i, j, k), x0, y0, z0);
    }

    f32 t1 = 0.6f - x1 * x1 - y1 * y1 - z1 * z1;
    if (t1 > 0.0f) {
        t1 *= t1;
        n1 = t1 * t1 * grad_3d(hash_coord(i + i1, j + j1, k + k1), x1, y1, z1);
    }

    f32 t2 = 0.6f - x2 * x2 - y2 * y2 - z2 * z2;
    if (t2 > 0.0f) {
        t2 *= t2;
        n2 = t2 * t2 * grad_3d(hash_coord(i + i2, j + j2, k + k2), x2, y2, z2);
    }

    f32 t3 = 0.6f - x3 * x3 - y3 * y3 - z3 * z3;
    if (t3 > 0.0f) {
        t3 *= t3;
        n3 = t3 * t3 * grad_3d(hash_coord(i + 1, j + 1, k + 1), x3, y3, z3);
    }

    return std::clamp(32.0f * (n0 + n1 + n2 + n3), -1.0f, 1.0f);
}

f32 NoiseGenerator::cellular_2d(f32 x, f32 y) const {
    const i32 xr = fast_floor(x);
    const i32 yr = fast_floor(y);

    f32 min_dist1 = 1e30f;
    f32 min_dist2 = 1e30f;
    u8 closest_hash = 0;

    for (i32 dy = -1; dy <= 1; ++dy) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            const i32 xi = xr + dx;
            const i32 yi = yr + dy;
            const u8 h = hash_coord(xi, yi);

            const f32 fx = static_cast<f32>(xi) + (static_cast<f32>(h & 15) / 15.0f - 0.5f) * m_cellular_jitter;
            const f32 fy = static_cast<f32>(yi) + (static_cast<f32>((h >> 4) & 15) / 15.0f - 0.5f) * m_cellular_jitter;

            const f32 diff_x = std::fabs(x - fx);
            const f32 diff_y = std::fabs(y - fy);

            f32 dist = 0.0f;
            if (m_cellular_metric == CellularDistanceMetric::Euclidean) {
                dist = std::sqrt(diff_x * diff_x + diff_y * diff_y);
            } else if (m_cellular_metric == CellularDistanceMetric::Manhattan) {
                dist = diff_x + diff_y;
            } else {
                dist = std::max(diff_x, diff_y);
            }

            if (dist < min_dist1) {
                min_dist2 = min_dist1;
                min_dist1 = dist;
                closest_hash = h;
            } else if (dist < min_dist2) {
                min_dist2 = dist;
            }
        }
    }

    switch (m_cellular_return) {
        case CellularReturnType::Distance:
            return std::clamp(min_dist1 * 2.0f - 1.0f, -1.0f, 1.0f);
        case CellularReturnType::Distance2:
            return std::clamp(min_dist2 * 2.0f - 1.0f, -1.0f, 1.0f);
        case CellularReturnType::Distance2MinusDistance1:
            return std::clamp((min_dist2 - min_dist1) * 2.0f - 1.0f, -1.0f, 1.0f);
        case CellularReturnType::CellValue:
            return static_cast<f32>(closest_hash) / 127.5f - 1.0f;
    }
    return 0.0f;
}

f32 NoiseGenerator::cellular_3d(f32 x, f32 y, f32 z) const {
    const i32 xr = fast_floor(x);
    const i32 yr = fast_floor(y);
    const i32 zr = fast_floor(z);

    f32 min_dist1 = 1e30f;
    f32 min_dist2 = 1e30f;
    u8 closest_hash = 0;

    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dy = -1; dy <= 1; ++dy) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                const i32 xi = xr + dx;
                const i32 yi = yr + dy;
                const i32 zi = zr + dz;
                const u8 h = hash_coord(xi, yi, zi);

                const f32 fx = static_cast<f32>(xi) + (static_cast<f32>(h & 7) / 7.0f - 0.5f) * m_cellular_jitter;
                const f32 fy = static_cast<f32>(yi) + (static_cast<f32>((h >> 3) & 7) / 7.0f - 0.5f) * m_cellular_jitter;
                const f32 fz = static_cast<f32>(zi) + (static_cast<f32>((h >> 6) & 3) / 3.0f - 0.5f) * m_cellular_jitter;

                const f32 diff_x = std::fabs(x - fx);
                const f32 diff_y = std::fabs(y - fy);
                const f32 diff_z = std::fabs(z - fz);

                f32 dist = 0.0f;
                if (m_cellular_metric == CellularDistanceMetric::Euclidean) {
                    dist = std::sqrt(diff_x * diff_x + diff_y * diff_y + diff_z * diff_z);
                } else if (m_cellular_metric == CellularDistanceMetric::Manhattan) {
                    dist = diff_x + diff_y + diff_z;
                } else {
                    dist = std::max({diff_x, diff_y, diff_z});
                }

                if (dist < min_dist1) {
                    min_dist2 = min_dist1;
                    min_dist1 = dist;
                    closest_hash = h;
                } else if (dist < min_dist2) {
                    min_dist2 = dist;
                }
            }
        }
    }

    switch (m_cellular_return) {
        case CellularReturnType::Distance:
            return std::clamp(min_dist1 * 2.0f - 1.0f, -1.0f, 1.0f);
        case CellularReturnType::Distance2:
            return std::clamp(min_dist2 * 2.0f - 1.0f, -1.0f, 1.0f);
        case CellularReturnType::Distance2MinusDistance1:
            return std::clamp((min_dist2 - min_dist1) * 2.0f - 1.0f, -1.0f, 1.0f);
        case CellularReturnType::CellValue:
            return static_cast<f32>(closest_hash) / 127.5f - 1.0f;
    }
    return 0.0f;
}

f32 NoiseGenerator::value_2d(f32 x, f32 y) const {
    const i32 x0 = fast_floor(x);
    const i32 y0 = fast_floor(y);
    const i32 x1 = x0 + 1;
    const i32 y1 = y0 + 1;

    const f32 dx = x - static_cast<f32>(x0);
    const f32 dy = y - static_cast<f32>(y0);

    const f32 sx = fade(dx);
    const f32 sy = fade(dy);

    const f32 v00 = static_cast<f32>(hash_coord(x0, y0)) / 127.5f - 1.0f;
    const f32 v10 = static_cast<f32>(hash_coord(x1, y0)) / 127.5f - 1.0f;
    const f32 v01 = static_cast<f32>(hash_coord(x0, y1)) / 127.5f - 1.0f;
    const f32 v11 = static_cast<f32>(hash_coord(x1, y1)) / 127.5f - 1.0f;

    const f32 ix0 = lerp_f(v00, v10, sx);
    const f32 ix1 = lerp_f(v01, v11, sx);

    return lerp_f(ix0, ix1, sy);
}

f32 NoiseGenerator::value_3d(f32 x, f32 y, f32 z) const {
    const i32 x0 = fast_floor(x);
    const i32 y0 = fast_floor(y);
    const i32 z0 = fast_floor(z);
    const i32 x1 = x0 + 1;
    const i32 y1 = y0 + 1;
    const i32 z1 = z0 + 1;

    const f32 dx = x - static_cast<f32>(x0);
    const f32 dy = y - static_cast<f32>(y0);
    const f32 dz = z - static_cast<f32>(z0);

    const f32 sx = fade(dx);
    const f32 sy = fade(dy);
    const f32 sz = fade(dz);

    const f32 v000 = static_cast<f32>(hash_coord(x0, y0, z0)) / 127.5f - 1.0f;
    const f32 v100 = static_cast<f32>(hash_coord(x1, y0, z0)) / 127.5f - 1.0f;
    const f32 v010 = static_cast<f32>(hash_coord(x0, y1, z0)) / 127.5f - 1.0f;
    const f32 v110 = static_cast<f32>(hash_coord(x1, y1, z0)) / 127.5f - 1.0f;
    const f32 v001 = static_cast<f32>(hash_coord(x0, y0, z1)) / 127.5f - 1.0f;
    const f32 v101 = static_cast<f32>(hash_coord(x1, y0, z1)) / 127.5f - 1.0f;
    const f32 v011 = static_cast<f32>(hash_coord(x0, y1, z1)) / 127.5f - 1.0f;
    const f32 v111 = static_cast<f32>(hash_coord(x1, y1, z1)) / 127.5f - 1.0f;

    const f32 ix00 = lerp_f(v000, v100, sx);
    const f32 ix10 = lerp_f(v010, v110, sx);
    const f32 ix01 = lerp_f(v001, v101, sx);
    const f32 ix11 = lerp_f(v011, v111, sx);

    const f32 iy0 = lerp_f(ix00, ix10, sy);
    const f32 iy1 = lerp_f(ix01, ix11, sy);

    return lerp_f(iy0, iy1, sz);
}

f32 NoiseGenerator::evaluate_single_2d(f32 x, f32 y) const {
    switch (m_noise_type) {
        case NoiseType::Perlin:   return perlin_2d(x, y);
        case NoiseType::Simplex:  return simplex_2d(x, y);
        case NoiseType::Cellular: return cellular_2d(x, y);
        case NoiseType::Value:    return value_2d(x, y);
    }
    return 0.0f;
}

f32 NoiseGenerator::evaluate_single_3d(f32 x, f32 y, f32 z) const {
    switch (m_noise_type) {
        case NoiseType::Perlin:   return perlin_3d(x, y, z);
        case NoiseType::Simplex:  return simplex_3d(x, y, z);
        case NoiseType::Cellular: return cellular_3d(x, y, z);
        case NoiseType::Value:    return value_3d(x, y, z);
    }
    return 0.0f;
}

f32 NoiseGenerator::sample_2d(f32 x, f32 y) const {
    f32 sum = 0.0f;
    f32 amp = 1.0f;
    f32 freq = m_frequency;
    f32 max_amp = 0.0f;

    for (u32 o = 0; o < m_octaves; ++o) {
        f32 val = evaluate_single_2d(x * freq, y * freq);

        if (m_fractal_type == FractalType::Billow) {
            val = std::fabs(val) * 2.0f - 1.0f;
            sum += val * amp;
        } else if (m_fractal_type == FractalType::RigidMulti) {
            val = 1.0f - std::fabs(val);
            val = val * val;
            sum += val * amp;
            amp *= std::clamp(val, 0.0f, 1.0f);
        } else {
            sum += val * amp;
        }

        max_amp += amp;
        freq *= m_lacunarity;
        amp *= m_gain;
    }

    return max_amp > 1e-5f ? std::clamp(sum / max_amp, -1.0f, 1.0f) : 0.0f;
}

f32 NoiseGenerator::sample_3d(f32 x, f32 y, f32 z) const {
    f32 sum = 0.0f;
    f32 amp = 1.0f;
    f32 freq = m_frequency;
    f32 max_amp = 0.0f;

    for (u32 o = 0; o < m_octaves; ++o) {
        f32 val = evaluate_single_3d(x * freq, y * freq, z * freq);

        if (m_fractal_type == FractalType::Billow) {
            val = std::fabs(val) * 2.0f - 1.0f;
            sum += val * amp;
        } else if (m_fractal_type == FractalType::RigidMulti) {
            val = 1.0f - std::fabs(val);
            val = val * val;
            sum += val * amp;
            amp *= std::clamp(val, 0.0f, 1.0f);
        } else {
            sum += val * amp;
        }

        max_amp += amp;
        freq *= m_lacunarity;
        amp *= m_gain;
    }

    return max_amp > 1e-5f ? std::clamp(sum / max_amp, -1.0f, 1.0f) : 0.0f;
}

void NoiseGenerator::generate_heightmap_2d(
    std::span<f32> output,
    u32 width,
    u32 height,
    f32 scale_x,
    f32 scale_y,
    f32 offset_x,
    f32 offset_y) const
{
    const size_t total = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (output.size() < total) {
        return;
    }

    for (u32 y = 0; y < height; ++y) {
        const f32 sample_y = static_cast<f32>(y) * scale_y + offset_y;
        for (u32 x = 0; x < width; ++x) {
            const f32 sample_x = static_cast<f32>(x) * scale_x + offset_x;
            const size_t idx = static_cast<size_t>(y) * width + x;
            output[idx] = sample_2d_normalized(sample_x, sample_y);
        }
    }
}

} // namespace nf::core
