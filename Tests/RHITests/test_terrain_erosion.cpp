#include <NF/Rendering/TerrainErosion.hpp>
#include <NF/Test/TestFramework.hpp>
#include <cmath>
#include <numeric>
#include <vector>

using namespace nf;
using namespace nf::rendering;

NF_TEST(erosion_gradient_flat_surface) {
    HeightField hf;
    hf.width = 16;
    hf.height = 16;
    hf.heights.assign(hf.width * hf.height, 5.0f); // Uniform height 5.0

    float gx = 0.0f, gy = 0.0f, h = 0.0f;
    TerrainErosion::compute_gradient(hf, 8.5f, 8.5f, gx, gy, h);

    NF_CHECK_NEAR(gx, 0.0f, 1e-6f);
    NF_CHECK_NEAR(gy, 0.0f, 1e-6f);
    NF_CHECK_NEAR(h, 5.0f, 1e-6f);
}

NF_TEST(erosion_gradient_sloped_surface) {
    HeightField hf;
    hf.width = 10;
    hf.height = 10;
    hf.heights.resize(hf.width * hf.height);

    // Height ramps up with X: h(x, y) = 0.1 * x
    for (u32 y = 0; y < hf.height; ++y) {
        for (u32 x = 0; x < hf.width; ++x) {
            hf.heights[y * hf.width + x] = 0.1f * static_cast<float>(x);
        }
    }

    float gx = 0.0f, gy = 0.0f, h = 0.0f;
    TerrainErosion::compute_gradient(hf, 4.5f, 4.5f, gx, gy, h);

    NF_CHECK_NEAR(gx, 0.1f, 1e-4f);
    NF_CHECK_NEAR(gy, 0.0f, 1e-4f);
    NF_CHECK_NEAR(h, 0.45f, 1e-4f);
}

NF_TEST(erosion_hydraulic_empty_no_crash) {
    HeightField hf;
    TerrainErosionSettings settings;
    TerrainErosion::simulate_hydraulic(hf, settings);
    NF_CHECK(!hf.ok());
}

NF_TEST(erosion_hydraulic_carves_peak) {
    HeightField hf;
    hf.width = 32;
    hf.height = 32;
    hf.heights.assign(hf.width * hf.height, 0.0f);

    // Create a central conical mountain
    const float cx = 16.0f, cy = 16.0f;
    for (u32 y = 0; y < hf.height; ++y) {
        for (u32 x = 0; x < hf.width; ++x) {
            float dist = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
            hf.heights[y * hf.width + x] = std::max(0.0f, 10.0f - dist);
        }
    }

    float initialPeak = hf.heights[16 * hf.width + 16];

    TerrainErosionSettings settings;
    settings.dropletCount = 3000;
    settings.erodeSpeed = 0.5f;
    settings.depositSpeed = 0.5f;
    settings.randomSeed = 42;

    TerrainErosion::simulate_hydraulic(hf, settings);

    float postPeak = hf.heights[16 * hf.width + 16];

    // Droplets running down the mountain must carve/lower the peak
    NF_CHECK(postPeak < initialPeak);
}

NF_TEST(erosion_hydraulic_deposits_at_base) {
    HeightField hf;
    hf.width = 32;
    hf.height = 32;
    hf.heights.assign(hf.width * hf.height, 0.0f);

    // Steep ramp on left half (x < 16), flat basin on right half (x >= 16)
    for (u32 y = 0; y < hf.height; ++y) {
        for (u32 x = 0; x < hf.width; ++x) {
            hf.heights[y * hf.width + x] = (x < 16) ? static_cast<float>(16 - x) : 0.0f;
        }
    }

    TerrainErosionSettings settings;
    settings.dropletCount = 4000;
    settings.randomSeed = 100;

    TerrainErosion::simulate_hydraulic(hf, settings);

    // At the foot of the hill (e.g. x = 16, 17), sediment should be deposited above 0
    float basinHeight = hf.heights[16 * hf.width + 17];
    NF_CHECK(basinHeight > 0.0f);
}

NF_TEST(erosion_thermal_relaxes_cliff) {
    HeightField hf;
    hf.width = 8;
    hf.height = 8;
    hf.heights.assign(hf.width * hf.height, 0.0f);

    // Sudden cliff: top half is height 10.0, bottom half is 0.0
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            hf.heights[y * 8 + x] = 10.0f;
        }
    }

    TerrainErosionSettings settings;
    settings.thermalIterations = 10;
    settings.talusAngleThreshold = 0.5f; // Step of 10 exceeds threshold
    settings.thermalSlippageRate = 0.5f;

    TerrainErosion::simulate_thermal(hf, settings);

    // Upper row at edge of cliff (y = 3) should have slipped down
    float topEdge = hf.heights[3 * 8 + 4];
    NF_CHECK(topEdge < 10.0f);

    // Lower row below cliff (y = 4) should have received material
    float botEdge = hf.heights[4 * 8 + 4];
    NF_CHECK(botEdge > 0.0f);
}

NF_TEST(erosion_thermal_gentle_slope_untouched) {
    HeightField hf;
    hf.width = 8;
    hf.height = 8;
    hf.heights.resize(64);

    // Slope of 0.01 per cell
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            hf.heights[y * 8 + x] = 0.01f * static_cast<float>(x);
        }
    }

    auto before = hf.heights;

    TerrainErosionSettings settings;
    settings.thermalIterations = 5;
    settings.talusAngleThreshold = 0.1f; // Threshold higher than slope

    TerrainErosion::simulate_thermal(hf, settings);

    // Gentle slope below talus threshold must remain identical
    for (size_t i = 0; i < before.size(); ++i) {
        NF_CHECK_NEAR(hf.heights[i], before[i], 1e-6f);
    }
}

NF_TEST(erosion_sediment_map_computation) {
    HeightField orig;
    orig.width = 4;
    orig.height = 4;
    orig.heights.assign(16, 5.0f);

    HeightField eroded = orig;
    eroded.heights[0] = 3.0f; // -2 erosion
    eroded.heights[5] = 6.5f; // +1.5 deposition

    auto diff = TerrainErosion::compute_sediment_map(orig, eroded);
    NF_CHECK(diff.size() == 16);
    NF_CHECK_NEAR(diff[0], -2.0f, 1e-6f);
    NF_CHECK_NEAR(diff[5], 1.5f, 1e-6f);
    NF_CHECK_NEAR(diff[1], 0.0f, 1e-6f);
}

NF_TEST(erosion_combined_pipeline) {
    HeightField hf;
    hf.width = 16;
    hf.height = 16;
    hf.heights.assign(256, 1.0f);
    hf.heights[8 * 16 + 8] = 10.0f; // Sharp spire

    TerrainErosionSettings settings;
    settings.dropletCount = 500;
    settings.thermalIterations = 3;

    TerrainErosion::simulate(hf, settings);

    // Spire should have reduced in height after combined erosion
    NF_CHECK(hf.heights[8 * 16 + 8] < 10.0f);
}

NF_TEST(erosion_deterministic_with_same_seed) {
    HeightField hf1;
    hf1.width = 16;
    hf1.height = 16;
    hf1.heights.assign(256, 5.0f);

    HeightField hf2 = hf1;

    TerrainErosionSettings settings;
    settings.dropletCount = 1000;
    settings.randomSeed = 9999;

    TerrainErosion::simulate_hydraulic(hf1, settings);
    TerrainErosion::simulate_hydraulic(hf2, settings);

    for (size_t i = 0; i < hf1.heights.size(); ++i) {
        NF_CHECK_NEAR(hf1.heights[i], hf2.heights[i], 1e-6f);
    }
}
