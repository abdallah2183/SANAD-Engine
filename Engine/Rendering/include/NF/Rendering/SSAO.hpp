#pragma once

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <vector>

namespace nf::rendering {

/// Configuration parameters for Screen-Space Ambient Occlusion (SSAO).
struct SSAOSettings {
    float radius = 0.5f;          // Sampling hemisphere radius in view-space units
    float bias = 0.025f;          // Depth bias to eliminate acne and self-occlusion
    float intensity = 1.5f;       // Occlusion contrast multiplier
    float power = 2.0f;           // Power curve exponent applied to occlusion
    u32 sampleCount = 16;         // Hemisphere sample count (typically 16 or 32)
    u32 blurRadius = 2;           // Bilateral filter radius (in pixels)
    float depthThreshold = 0.05f; // Depth delta threshold for edge preservation
    bool enabled = true;
};

/// Hemisphere sample kernel and random rotation noise generator for SSAO.
class SSAOKernel {
public:
    explicit SSAOKernel(u32 sampleCount = 16, u32 seed = 1337);

    [[nodiscard]] const std::vector<Vec3>& samples() const { return m_samples; }
    [[nodiscard]] const std::vector<Vec3>& noise() const { return m_noise; }

    [[nodiscard]] u32 sample_count() const { return static_cast<u32>(m_samples.size()); }

    void generate(u32 sampleCount, u32 seed = 1337);

private:
    std::vector<Vec3> m_samples;
    std::vector<Vec3> m_noise; // 16 samples for a 4x4 tiled noise grid
};

/// CPU reference and algorithmic processor for SSAO and depth-aware bilateral blur.
class SSAOProcessor {
public:
    /// Reconstructs view-space 3D position from pixel coordinates (x, y), linear depth, and camera FOV.
    [[nodiscard]] static Vec3 reconstruct_view_pos(
        u32 x, u32 y,
        u32 width, u32 height,
        float linearDepth,
        float fovYRad,
        float aspectRatio
    );

    /// Projects view-space point to normalized screen coordinates [0, 1] x [0, 1].
    [[nodiscard]] static bool project_view_pos(
        const Vec3& viewPos,
        float fovYRad,
        float aspectRatio,
        float& outU, float& outV
    );

    /// Evaluates sample occlusion against geometry depth.
    /// Returns occlusion value [0, 1] where 1.0 means sample is occluded.
    [[nodiscard]] static float compute_sample_occlusion(
        const Vec3& surfacePos,
        const Vec3& sampleOffset,
        float geometryDepth,
        float radius,
        float bias
    );

    /// Computes full SSAO buffer from depth and normals.
    /// linearDepth: positive depth in view space (z > 0)
    /// viewNormals: normalized view-space normals
    /// outOcclusion: array of size (width * height), values in [0, 1] where 1.0 = unoccluded, 0.0 = fully occluded
    static void compute_ssao(
        u32 width, u32 height,
        const float* linearDepth,
        const Vec3* viewNormals,
        float fovYRad,
        float aspectRatio,
        const SSAOKernel& kernel,
        const SSAOSettings& settings,
        float* outOcclusion
    );

    /// Applies depth-aware bilateral blur to smooth occlusion while preserving sharp geometry edges.
    static void bilateral_blur(
        u32 width, u32 height,
        const float* inOcclusion,
        const float* linearDepth,
        u32 blurRadius,
        float depthThreshold,
        float* outBlurred
    );
};

} // namespace nf::rendering
