#include <NF/Test/TestFramework.hpp>
#include <NF/Core/Noise.hpp>
#include <cmath>
#include <vector>

using namespace nf;
using namespace nf::core;

NF_TEST(noise_perlin_deterministic_and_bounds) {
    NoiseGenerator gen1(1337);
    NoiseGenerator gen2(1337);

    // Identical seeds must produce identical results
    for (float x = -5.0f; x <= 5.0f; x += 0.75f) {
        for (float y = -5.0f; y <= 5.0f; y += 0.75f) {
            float val1 = gen1.perlin_2d(x, y);
            float val2 = gen2.perlin_2d(x, y);
            NF_CHECK_NEAR(val1, val2, 1e-6f);
            NF_CHECK(val1 >= -1.0f && val1 <= 1.0f);
        }
    }

    // 3D Perlin noise
    for (float z = 0.0f; z <= 2.0f; z += 0.5f) {
        float val3d = gen1.perlin_3d(1.23f, 4.56f, z);
        NF_CHECK(val3d >= -1.0f && val3d <= 1.0f);
        NF_CHECK(!std::isnan(val3d));
    }
}

NF_TEST(noise_simplex_2d_and_3d) {
    NoiseGenerator gen(42);
    gen.set_noise_type(NoiseType::Simplex);

    // Continuous smoothness check: small step in space -> small delta in noise
    float p0 = gen.simplex_2d(10.0f, 20.0f);
    float p1 = gen.simplex_2d(10.01f, 20.01f);
    NF_CHECK(std::fabs(p1 - p0) < 0.1f);
    NF_CHECK(p0 >= -1.0f && p0 <= 1.0f);

    float s3d = gen.simplex_3d(1.0f, 2.0f, 3.0f);
    NF_CHECK(s3d >= -1.0f && s3d <= 1.0f);
    NF_CHECK(!std::isnan(s3d));
}

NF_TEST(noise_cellular_voronoi_metrics) {
    NoiseGenerator gen(999);
    gen.set_noise_type(NoiseType::Cellular);

    // Euclidean distance
    gen.set_cellular_metric(CellularDistanceMetric::Euclidean);
    gen.set_cellular_return(CellularReturnType::Distance);
    float c_euc = gen.cellular_2d(5.5f, 5.5f);
    NF_CHECK(c_euc >= -1.0f && c_euc <= 1.0f);

    // Manhattan metric
    gen.set_cellular_metric(CellularDistanceMetric::Manhattan);
    float c_man = gen.cellular_2d(5.5f, 5.5f);
    NF_CHECK(c_man >= -1.0f && c_man <= 1.0f);

    // Cell borders: Distance2 - Distance1
    gen.set_cellular_return(CellularReturnType::Distance2MinusDistance1);
    float c_border = gen.cellular_2d(5.5f, 5.5f);
    NF_CHECK(c_border >= -1.0f && c_border <= 1.0f);

    // 3D Cellular
    float c_3d = gen.cellular_3d(2.0f, 3.0f, 4.0f);
    NF_CHECK(c_3d >= -1.0f && c_3d <= 1.0f);
}

NF_TEST(noise_fractal_fbm_and_variants) {
    NoiseGenerator gen(777);
    gen.set_frequency(0.05f);
    gen.set_octaves(4);
    gen.set_lacunarity(2.0f);
    gen.set_gain(0.5f);

    // FBM
    gen.set_fractal_type(FractalType::FBM);
    float fbm_val = gen.sample_2d(100.0f, 200.0f);
    NF_CHECK(fbm_val >= -1.0f && fbm_val <= 1.0f);

    // Billow (puffy clouds)
    gen.set_fractal_type(FractalType::Billow);
    float billow_val = gen.sample_2d(100.0f, 200.0f);
    NF_CHECK(billow_val >= -1.0f && billow_val <= 1.0f);

    // RigidMulti (sharp mountain ridges)
    gen.set_fractal_type(FractalType::RigidMulti);
    float rigid_val = gen.sample_2d(100.0f, 200.0f);
    NF_CHECK(rigid_val >= -1.0f && rigid_val <= 1.0f);

    // 3D FBM sample
    float fbm_3d = gen.sample_3d(10.0f, 20.0f, 30.0f);
    NF_CHECK(fbm_3d >= -1.0f && fbm_3d <= 1.0f);
}

NF_TEST(noise_heightmap_generation) {
    NoiseGenerator gen(2026);
    gen.set_frequency(0.02f);
    gen.set_octaves(4);

    constexpr u32 w = 32;
    constexpr u32 h = 32;
    std::vector<f32> heightmap(w * h, 0.0f);

    gen.generate_heightmap_2d(heightmap, w, h, 1.0f, 1.0f, 0.0f, 0.0f);

    f32 min_h = 1000.0f;
    f32 max_h = -1000.0f;

    for (f32 height : heightmap) {
        NF_CHECK(height >= 0.0f && height <= 1.0f);
        min_h = std::min(min_h, height);
        max_h = std::max(max_h, height);
    }

    // Must exhibit natural elevation variation
    NF_CHECK(max_h > min_h);
    NF_CHECK(max_h - min_h > 0.1f);
}
