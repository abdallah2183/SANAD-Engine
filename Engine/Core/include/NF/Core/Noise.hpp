#pragma once

#include <NF/Core/Types.hpp>
#include <NF/Core/Math.hpp>

#include <algorithm>
#include <span>
#include <vector>
#include <array>
#include <cmath>

namespace nf::core {

enum class NoiseType : u8 {
    Perlin = 0,
    Simplex,
    Cellular,   // Worley / Voronoi
    Value
};

enum class FractalType : u8 {
    FBM = 0,    // Fractional Brownian Motion
    Billow,     // Puffy, cloudy |noise| * 2 - 1
    RigidMulti  // Sharp ridges (1 - |noise|)^2
};

enum class CellularDistanceMetric : u8 {
    Euclidean = 0,
    Manhattan,
    Chebyshev
};

enum class CellularReturnType : u8 {
    Distance = 0,
    Distance2,
    Distance2MinusDistance1,
    CellValue
};

/// High-performance coherent procedural noise and fractal generator
class NoiseGenerator {
public:
    explicit NoiseGenerator(u32 seed = 1337);

    void set_seed(u32 seed) noexcept;
    [[nodiscard]] u32 seed() const noexcept { return m_seed; }

    void set_noise_type(NoiseType type) noexcept { m_noise_type = type; }
    [[nodiscard]] NoiseType noise_type() const noexcept { return m_noise_type; }

    void set_fractal_type(FractalType type) noexcept { m_fractal_type = type; }
    [[nodiscard]] FractalType fractal_type() const noexcept { return m_fractal_type; }

    void set_frequency(f32 freq) noexcept { m_frequency = freq; }
    [[nodiscard]] f32 frequency() const noexcept { return m_frequency; }

    void set_octaves(u32 octaves) noexcept { m_octaves = std::clamp(octaves, 1u, 16u); }
    [[nodiscard]] u32 octaves() const noexcept { return m_octaves; }

    void set_lacunarity(f32 lacunarity) noexcept { m_lacunarity = lacunarity; }
    [[nodiscard]] f32 lacunarity() const noexcept { return m_lacunarity; }

    void set_gain(f32 gain) noexcept { m_gain = gain; }
    [[nodiscard]] f32 gain() const noexcept { return m_gain; }

    // Cellular configurations
    void set_cellular_metric(CellularDistanceMetric metric) noexcept { m_cellular_metric = metric; }
    [[nodiscard]] CellularDistanceMetric cellular_metric() const noexcept { return m_cellular_metric; }

    void set_cellular_return(CellularReturnType ret) noexcept { m_cellular_return = ret; }
    [[nodiscard]] CellularReturnType cellular_return() const noexcept { return m_cellular_return; }

    void set_cellular_jitter(f32 jitter) noexcept { m_cellular_jitter = std::clamp(jitter, 0.0f, 1.0f); }
    [[nodiscard]] f32 cellular_jitter() const noexcept { return m_cellular_jitter; }

    // Single-octave raw noise in [-1.0, 1.0]
    [[nodiscard]] f32 perlin_2d(f32 x, f32 y) const;
    [[nodiscard]] f32 perlin_3d(f32 x, f32 y, f32 z) const;
    [[nodiscard]] f32 simplex_2d(f32 x, f32 y) const;
    [[nodiscard]] f32 simplex_3d(f32 x, f32 y, f32 z) const;
    [[nodiscard]] f32 cellular_2d(f32 x, f32 y) const;
    [[nodiscard]] f32 cellular_3d(f32 x, f32 y, f32 z) const;
    [[nodiscard]] f32 value_2d(f32 x, f32 y) const;
    [[nodiscard]] f32 value_3d(f32 x, f32 y, f32 z) const;

    // Evaluates multi-octave fractal noise using the configured settings in [-1.0, 1.0]
    [[nodiscard]] f32 sample_2d(f32 x, f32 y) const;
    [[nodiscard]] f32 sample_3d(f32 x, f32 y, f32 z) const;

    // Evaluates normalized noise in [0.0, 1.0]
    [[nodiscard]] f32 sample_2d_normalized(f32 x, f32 y) const {
        return std::clamp(sample_2d(x, y) * 0.5f + 0.5f, 0.0f, 1.0f);
    }
    [[nodiscard]] f32 sample_3d_normalized(f32 x, f32 y, f32 z) const {
        return std::clamp(sample_3d(x, y, z) * 0.5f + 0.5f, 0.0f, 1.0f);
    }

    // Fills a 2D heightmap buffer
    void generate_heightmap_2d(std::span<f32> output, u32 width, u32 height,
                               f32 scale_x, f32 scale_y, f32 offset_x = 0.0f, f32 offset_y = 0.0f) const;

private:
    [[nodiscard]] f32 evaluate_single_2d(f32 x, f32 y) const;
    [[nodiscard]] f32 evaluate_single_3d(f32 x, f32 y, f32 z) const;

    [[nodiscard]] u8 hash_coord(i32 x, i32 y) const;
    [[nodiscard]] u8 hash_coord(i32 x, i32 y, i32 z) const;

    u32 m_seed = 1337;
    NoiseType m_noise_type = NoiseType::Perlin;
    FractalType m_fractal_type = FractalType::FBM;
    f32 m_frequency = 0.01f;
    u32 m_octaves = 4;
    f32 m_lacunarity = 2.0f;
    f32 m_gain = 0.5f;

    CellularDistanceMetric m_cellular_metric = CellularDistanceMetric::Euclidean;
    CellularReturnType m_cellular_return = CellularReturnType::Distance;
    f32 m_cellular_jitter = 0.45f;

    std::array<u8, 512> m_perm{};
};

} // namespace nf::core
