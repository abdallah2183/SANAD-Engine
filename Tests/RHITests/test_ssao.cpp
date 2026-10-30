#include <NF/Rendering/SSAO.hpp>
#include <NF/Test/TestFramework.hpp>
#include <cmath>
#include <vector>

using namespace nf;
using namespace nf::rendering;

NF_TEST(ssao_kernel_generation) {
    SSAOKernel kernel(16, 42);
    NF_CHECK(kernel.sample_count() == 16);
    NF_CHECK(kernel.samples().size() == 16);

    for (const auto& s : kernel.samples()) {
        float len2 = s.x * s.x + s.y * s.y + s.z * s.z;
        NF_CHECK(len2 > 0.001f);
        NF_CHECK(len2 <= 1.001f);
        // Z must be positive (hemisphere oriented along normal)
        NF_CHECK(s.z > 0.0f);
    }
}

NF_TEST(ssao_kernel_noise) {
    SSAOKernel kernel(16, 42);
    NF_CHECK(kernel.noise().size() == 16);

    for (const auto& n : kernel.noise()) {
        float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        NF_CHECK_NEAR(len, 1.0f, 1e-4f);
        // Noise vectors lie in tangent plane (z = 0)
        NF_CHECK_NEAR(n.z, 0.0f, 1e-6f);
    }
}

NF_TEST(ssao_view_reconstruction) {
    const u32 w = 64, h = 64;
    const float depth = 10.0f;
    const float fovY = 1.0472f; // ~60 deg
    const float aspect = 1.0f;

    // Center pixel (32, 32)
    Vec3 center = SSAOProcessor::reconstruct_view_pos(32, 32, w, h, depth, fovY, aspect);
    NF_CHECK_NEAR(center.x, 0.0f, 0.2f);
    NF_CHECK_NEAR(center.y, 0.0f, 0.2f);
    NF_CHECK_NEAR(center.z, depth, 1e-4f);
}

NF_TEST(ssao_view_projection_roundtrip) {
    const u32 w = 128, h = 128;
    const float depth = 5.0f;
    const float fovY = 1.0472f;
    const float aspect = 1.0f;

    const u32 testX = 40, testY = 80;
    Vec3 viewPos = SSAOProcessor::reconstruct_view_pos(testX, testY, w, h, depth, fovY, aspect);

    float u = 0.0f, v = 0.0f;
    bool ok = SSAOProcessor::project_view_pos(viewPos, fovY, aspect, u, v);
    NF_CHECK(ok);

    u32 projX = static_cast<u32>(u * static_cast<float>(w));
    u32 projY = static_cast<u32>(v * static_cast<float>(h));

    NF_CHECK(std::abs(static_cast<int>(projX) - static_cast<int>(testX)) <= 1);
    NF_CHECK(std::abs(static_cast<int>(projY) - static_cast<int>(testY)) <= 1);
}

NF_TEST(ssao_disabled_returns_full_white) {
    const u32 w = 16, h = 16;
    std::vector<float> depth(w * h, 5.0f);
    std::vector<Vec3> normals(w * h, Vec3(0, 0, -1));
    std::vector<float> occlusion(w * h, 0.0f);

    SSAOKernel kernel(16);
    SSAOSettings settings;
    settings.enabled = false;

    SSAOProcessor::compute_ssao(w, h, depth.data(), normals.data(), 1.0f, 1.0f, kernel, settings, occlusion.data());

    for (float val : occlusion) {
        NF_CHECK_NEAR(val, 1.0f, 1e-6f);
    }
}

NF_TEST(ssao_background_depth_unoccluded) {
    const u32 w = 8, h = 8;
    std::vector<float> depth(w * h, 0.0f); // 0 = sky/background
    std::vector<Vec3> normals(w * h, Vec3(0, 0, -1));
    std::vector<float> occlusion(w * h, 0.5f);

    SSAOKernel kernel(16);
    SSAOSettings settings;

    SSAOProcessor::compute_ssao(w, h, depth.data(), normals.data(), 1.0f, 1.0f, kernel, settings, occlusion.data());

    for (float val : occlusion) {
        NF_CHECK_NEAR(val, 1.0f, 1e-6f);
    }
}

NF_TEST(ssao_flat_plane_mostly_unoccluded) {
    const u32 w = 32, h = 32;
    std::vector<float> depth(w * h, 5.0f); // Constant flat wall at z = 5
    std::vector<Vec3> normals(w * h, Vec3(0, 0, -1)); // Facing camera
    std::vector<float> occlusion(w * h, 0.0f);

    SSAOKernel kernel(16, 123);
    SSAOSettings settings;
    settings.radius = 0.5f;
    settings.bias = 0.02f;

    SSAOProcessor::compute_ssao(w, h, depth.data(), normals.data(), 1.0f, 1.0f, kernel, settings, occlusion.data());

    // On a flat wall, hemisphere samples in front (+Z relative to normal pointing towards -Z)
    // should not be heavily occluded
    float avg = 0.0f;
    for (float v : occlusion) avg += v;
    avg /= static_cast<float>(occlusion.size());

    NF_CHECK(avg > 0.80f);
}

NF_TEST(ssao_crevice_corner_occlusion) {
    const u32 w = 32, h = 32;
    std::vector<float> depth(w * h, 5.0f);
    std::vector<Vec3> normals(w * h, Vec3(0, 0, -1));
    std::vector<float> occlusion(w * h, 1.0f);

    // Create an occluding wall occupying the left half (x < 16) protruding closer (z = 4.7f)
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < 16; ++x) {
            depth[y * w + x] = 4.7f;
            normals[y * w + x] = Vec3(1, 0, 0); // Side of protruding step facing right
        }
    }

    SSAOKernel kernel(16, 777);
    SSAOSettings settings;
    settings.radius = 0.8f;
    settings.bias = 0.01f;

    SSAOProcessor::compute_ssao(w, h, depth.data(), normals.data(), 1.0f, 1.0f, kernel, settings, occlusion.data());

    // In the seam / corner at x = 16, 17, occlusion should be lower than far away at x = 30
    float seamOcc = occlusion[16 * w + 16];
    float openOcc = occlusion[16 * w + 30];

    NF_CHECK(seamOcc < openOcc);
}

NF_TEST(ssao_bilateral_blur_preserves_depth_edge) {
    const u32 w = 16, h = 16;
    std::vector<float> depth(w * h);
    std::vector<float> inOcc(w * h);
    std::vector<float> outOcc(w * h, 0.0f);

    // Left half: foreground at depth 2.0 with occlusion 0.2
    // Right half: background at depth 10.0 with occlusion 1.0
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            u32 idx = y * w + x;
            if (x < 8) {
                depth[idx] = 2.0f;
                inOcc[idx] = 0.2f;
            } else {
                depth[idx] = 10.0f;
                inOcc[idx] = 1.0f;
            }
        }
    }

    SSAOProcessor::bilateral_blur(w, h, inOcc.data(), depth.data(), 2, 0.1f, outOcc.data());

    // Pixel at (7, 8) right next to edge: should NOT blur with background (remain close to 0.2)
    NF_CHECK_NEAR(outOcc[8 * w + 7], 0.2f, 0.05f);
    // Pixel at (8, 8) right across edge: should NOT blur with foreground (remain close to 1.0)
    NF_CHECK_NEAR(outOcc[8 * w + 8], 1.0f, 0.05f);
}

NF_TEST(ssao_bilateral_blur_smooths_flat_region) {
    const u32 w = 16, h = 16;
    std::vector<float> depth(w * h, 5.0f); // Uniform flat depth
    std::vector<float> inOcc(w * h, 0.5f);
    std::vector<float> outOcc(w * h, 0.0f);

    // Add checkerboard noise (+0.2 / -0.2)
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            inOcc[y * w + x] = ((x + y) % 2 == 0) ? 0.7f : 0.3f;
        }
    }

    SSAOProcessor::bilateral_blur(w, h, inOcc.data(), depth.data(), 2, 0.5f, outOcc.data());

    // Interior blurred pixels should be close to 0.5 average
    float centerVal = outOcc[8 * w + 8];
    NF_CHECK_NEAR(centerVal, 0.5f, 0.08f);
}
