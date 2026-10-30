#include <NF/Rendering/SSAO.hpp>
#include <algorithm>
#include <cmath>

namespace nf::rendering {

namespace {

inline float smoothstep(float edge0, float edge1, float x) {
    float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

inline Vec3 cross_product(const Vec3& a, const Vec3& b) {
    return Vec3(
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    );
}

inline float dot_product(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline float length_sq(const Vec3& v) {
    return dot_product(v, v);
}

inline Vec3 normalize_safe(const Vec3& v, const Vec3& fallback = Vec3(0, 0, 1)) {
    float len2 = length_sq(v);
    if (len2 > 1e-8f) {
        float inv = 1.0f / std::sqrt(len2);
        return Vec3(v.x * inv, v.y * inv, v.z * inv);
    }
    return fallback;
}

} // namespace

SSAOKernel::SSAOKernel(u32 sampleCount, u32 seed) {
    generate(sampleCount, seed);
}

void SSAOKernel::generate(u32 sampleCount, u32 seed) {
    m_samples.clear();
    m_samples.reserve(sampleCount);

    u32 state = seed == 0 ? 1337u : seed;
    auto lcg_float = [&state]() -> float {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state) / 4294967296.0f; // [0, 1)
    };

    for (u32 i = 0; i < sampleCount; ++i) {
        // Random point on hemisphere: x, y in [-1, 1], z in [0.05, 1.0]
        float x = lcg_float() * 2.0f - 1.0f;
        float y = lcg_float() * 2.0f - 1.0f;
        float z = lcg_float() * 0.95f + 0.05f;

        Vec3 sample = normalize_safe(Vec3(x, y, z), Vec3(0, 0, 1));
        
        // Scale with quadratic falloff so samples cluster closer to the center
        float scale = static_cast<float>(i + 1) / static_cast<float>(sampleCount);
        scale = std::lerp(0.1f, 1.0f, scale * scale);
        
        m_samples.push_back(Vec3(sample.x * scale, sample.y * scale, sample.z * scale));
    }

    // 16 random rotation vectors for a 4x4 tiled noise pattern (z = 0, lying in tangent plane)
    m_noise.clear();
    m_noise.reserve(16);
    for (u32 i = 0; i < 16; ++i) {
        float nx = lcg_float() * 2.0f - 1.0f;
        float ny = lcg_float() * 2.0f - 1.0f;
        m_noise.push_back(normalize_safe(Vec3(nx, ny, 0.0f), Vec3(1, 0, 0)));
    }
}

Vec3 SSAOProcessor::reconstruct_view_pos(
    u32 x, u32 y,
    u32 width, u32 height,
    float linearDepth,
    float fovYRad,
    float aspectRatio
) {
    if (width == 0 || height == 0 || linearDepth <= 0.0f) {
        return Vec3(0, 0, linearDepth);
    }

    float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(width);
    float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(height);

    float ndcX = u * 2.0f - 1.0f;
    float ndcY = 1.0f - v * 2.0f; // Y up in view space

    float tanHalfFov = std::tan(fovYRad * 0.5f);
    float vx = ndcX * aspectRatio * tanHalfFov * linearDepth;
    float vy = ndcY * tanHalfFov * linearDepth;
    float vz = linearDepth;

    return Vec3(vx, vy, vz);
}

bool SSAOProcessor::project_view_pos(
    const Vec3& viewPos,
    float fovYRad,
    float aspectRatio,
    float& outU, float& outV
) {
    if (viewPos.z <= 1e-4f) {
        return false;
    }

    float tanHalfFov = std::tan(fovYRad * 0.5f);
    float denomX = viewPos.z * aspectRatio * tanHalfFov;
    float denomY = viewPos.z * tanHalfFov;

    if (std::abs(denomX) < 1e-6f || std::abs(denomY) < 1e-6f) {
        return false;
    }

    float ndcX = viewPos.x / denomX;
    float ndcY = viewPos.y / denomY;

    outU = (ndcX + 1.0f) * 0.5f;
    outV = (1.0f - ndcY) * 0.5f;

    return true;
}

float SSAOProcessor::compute_sample_occlusion(
    const Vec3& surfacePos,
    const Vec3& sampleOffset,
    float geometryDepth,
    float radius,
    float bias
) {
    float sampleZ = surfacePos.z + sampleOffset.z;
    float depthDiff = sampleZ - geometryDepth;

    // Occlusion occurs when geometry is in front of the sample (geometryDepth < sampleZ)
    // but within the influence radius
    if (depthDiff > bias && depthDiff < radius) {
        // Range check to prevent haloing/bleeding on far geometries
        float rangeCheck = smoothstep(0.0f, 1.0f, radius / (depthDiff + 1e-4f));
        return std::clamp(rangeCheck, 0.0f, 1.0f);
    }

    return 0.0f;
}

void SSAOProcessor::compute_ssao(
    u32 width, u32 height,
    const float* linearDepth,
    const Vec3* viewNormals,
    float fovYRad,
    float aspectRatio,
    const SSAOKernel& kernel,
    const SSAOSettings& settings,
    float* outOcclusion
) {
    if (!outOcclusion || width == 0 || height == 0) return;

    if (!settings.enabled || kernel.samples().empty()) {
        std::fill(outOcclusion, outOcclusion + (static_cast<size_t>(width) * height), 1.0f);
        return;
    }

    const auto& samples = kernel.samples();
    const auto& noise = kernel.noise();
    const u32 sampleCount = static_cast<u32>(samples.size());

    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const u32 pixelIdx = y * width + x;
            float depth = linearDepth ? linearDepth[pixelIdx] : 0.0f;

            // Background or invalid depth is unoccluded
            if (depth <= 1e-3f || depth >= 1e6f) {
                outOcclusion[pixelIdx] = 1.0f;
                continue;
            }

            Vec3 viewPos = reconstruct_view_pos(x, y, width, height, depth, fovYRad, aspectRatio);
            Vec3 normal = viewNormals ? viewNormals[pixelIdx] : Vec3(0, 0, -1);
            normal = normalize_safe(normal, Vec3(0, 0, -1));

            // Random rotation vector from 4x4 tiled noise
            Vec3 rvec = Vec3(1, 0, 0);
            if (!noise.empty()) {
                u32 noiseIdx = ((y % 4) * 4 + (x % 4)) % static_cast<u32>(noise.size());
                rvec = noise[noiseIdx];
            }

            // Gram-Schmidt process to create orthonormal basis (T, B, N)
            Vec3 tangent = normalize_safe(Vec3(
                rvec.x - normal.x * dot_product(rvec, normal),
                rvec.y - normal.y * dot_product(rvec, normal),
                rvec.z - normal.z * dot_product(rvec, normal)
            ), Vec3(1, 0, 0));

            Vec3 bitangent = cross_product(normal, tangent);

            float totalOcclusion = 0.0f;

            for (u32 i = 0; i < sampleCount; ++i) {
                const Vec3& s = samples[i];
                // Transform sample from tangent space to view space
                Vec3 offset = Vec3(
                    tangent.x * s.x + bitangent.x * s.y + normal.x * s.z,
                    tangent.y * s.x + bitangent.y * s.y + normal.y * s.z,
                    tangent.z * s.x + bitangent.z * s.y + normal.z * s.z
                );

                offset = Vec3(offset.x * settings.radius, offset.y * settings.radius, offset.z * settings.radius);
                Vec3 samplePos = Vec3(viewPos.x + offset.x, viewPos.y + offset.y, viewPos.z + offset.z);

                float u = 0.0f, v = 0.0f;
                if (!project_view_pos(samplePos, fovYRad, aspectRatio, u, v)) {
                    continue;
                }

                if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) {
                    continue;
                }

                u32 sx = std::clamp(static_cast<u32>(u * static_cast<float>(width)), 0u, width - 1);
                u32 sy = std::clamp(static_cast<u32>(v * static_cast<float>(height)), 0u, height - 1);

                float geoDepth = linearDepth[sy * width + sx];
                if (geoDepth > 1e-3f) {
                    totalOcclusion += compute_sample_occlusion(viewPos, offset, geoDepth, settings.radius, settings.bias);
                }
            }

            float normalizedOcc = totalOcclusion / static_cast<float>(sampleCount);
            float factor = 1.0f - (normalizedOcc * settings.intensity);
            factor = std::clamp(factor, 0.0f, 1.0f);
            if (settings.power != 1.0f) {
                factor = std::pow(factor, settings.power);
            }

            outOcclusion[pixelIdx] = factor;
        }
    }
}

void SSAOProcessor::bilateral_blur(
    u32 width, u32 height,
    const float* inOcclusion,
    const float* linearDepth,
    u32 blurRadius,
    float depthThreshold,
    float* outBlurred
) {
    if (!inOcclusion || !outBlurred || width == 0 || height == 0) return;

    if (blurRadius == 0 || !linearDepth || depthThreshold <= 0.0f) {
        std::copy(inOcclusion, inOcclusion + (static_cast<size_t>(width) * height), outBlurred);
        return;
    }

    const int r = static_cast<int>(blurRadius);
    const float invThresh = 1.0f / depthThreshold;

    for (int y = 0; y < static_cast<int>(height); ++y) {
        for (int x = 0; x < static_cast<int>(width); ++x) {
            const int centerIdx = y * static_cast<int>(width) + x;
            const float centerZ = linearDepth[centerIdx];

            if (centerZ <= 1e-3f) {
                outBlurred[centerIdx] = inOcclusion[centerIdx];
                continue;
            }

            float weightSum = 0.0f;
            float valueSum = 0.0f;

            for (int dy = -r; dy <= r; ++dy) {
                int ny = std::clamp(y + dy, 0, static_cast<int>(height) - 1);
                for (int dx = -r; dx <= r; ++dx) {
                    int nx = std::clamp(x + dx, 0, static_cast<int>(width) - 1);
                    int sampleIdx = ny * static_cast<int>(width) + nx;

                    float sampleZ = linearDepth[sampleIdx];
                    float depthDiff = std::abs(sampleZ - centerZ);

                    // Spatial Gaussian-like falloff
                    float spatialDist2 = static_cast<float>(dx * dx + dy * dy);
                    float spatialWeight = std::exp(-spatialDist2 * 0.2f);

                    // Range weight: suppresses contribution if depth step is across a geometric edge
                    float rangeWeight = std::exp(-depthDiff * invThresh);

                    float weight = spatialWeight * rangeWeight;
                    weightSum += weight;
                    valueSum += inOcclusion[sampleIdx] * weight;
                }
            }

            outBlurred[centerIdx] = weightSum > 1e-5f ? (valueSum / weightSum) : inOcclusion[centerIdx];
        }
    }
}

} // namespace nf::rendering
