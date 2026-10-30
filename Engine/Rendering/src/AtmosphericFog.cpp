#include <NF/Rendering/AtmosphericFog.hpp>
#include <algorithm>
#include <cmath>

namespace nf::rendering {

namespace {

inline float dot_product(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline float length_sq(const Vec3& v) {
    return dot_product(v, v);
}

inline Vec3 normalize_safe(const Vec3& v, const Vec3& fallback = Vec3(0, 1, 0)) {
    float len2 = length_sq(v);
    if (len2 > 1e-8f) {
        float inv = 1.0f / std::sqrt(len2);
        return Vec3(v.x * inv, v.y * inv, v.z * inv);
    }
    return fallback;
}

inline Vec3 lerp_vec3(const Vec3& a, const Vec3& b, float t) {
    return Vec3(
        std::lerp(a.x, b.x, t),
        std::lerp(a.y, b.y, t),
        std::lerp(a.z, b.z, t)
    );
}

} // namespace

float AtmosphericFog::compute_optical_depth(
    const Vec3& cameraPos,
    const Vec3& worldTargetPos,
    const FogSettings& settings
) {
    if (!settings.enabled || settings.density <= 0.0f) {
        return 0.0f;
    }

    Vec3 delta(
        worldTargetPos.x - cameraPos.x,
        worldTargetPos.y - cameraPos.y,
        worldTargetPos.z - cameraPos.z
    );

    float totalDist = std::sqrt(length_sq(delta));
    if (totalDist < 1e-4f) {
        return 0.0f;
    }

    Vec3 rayDir(delta.x / totalDist, delta.y / totalDist, delta.z / totalDist);

    float effectiveStart = std::clamp(settings.startDistance, 0.0f, totalDist);
    float effectiveDist = totalDist - effectiveStart;

    if (effectiveDist <= 0.0f) {
        return 0.0f;
    }

    float maxAllowedDist = std::max(0.0f, settings.cutoffDistance - effectiveStart);
    float clampedDist = std::min(effectiveDist, maxAllowedDist);

    Vec3 startPos(
        cameraPos.x + rayDir.x * effectiveStart,
        cameraPos.y + rayDir.y * effectiveStart,
        cameraPos.z + rayDir.z * effectiveStart
    );

    // Height Y is upward
    float heightDiff = startPos.y - settings.fogHeight;
    float baseDensity = settings.density * std::exp(-settings.heightFalloff * heightDiff);

    float k = settings.heightFalloff * rayDir.y;
    float opticalDepth = 0.0f;

    if (std::abs(k) > 1e-5f) {
        opticalDepth = baseDensity * (1.0f - std::exp(-k * clampedDist)) / k;
    } else {
        // Nearly horizontal ray (k ~ 0)
        opticalDepth = baseDensity * clampedDist;
    }

    return std::max(0.0f, opticalDepth);
}

float AtmosphericFog::compute_extinction(
    const Vec3& cameraPos,
    const Vec3& worldTargetPos,
    const FogSettings& settings
) {
    float tau = compute_optical_depth(cameraPos, worldTargetPos, settings);
    return std::clamp(std::exp(-tau), 0.0f, 1.0f);
}

Vec3 AtmosphericFog::compute_inscattering_color(
    const Vec3& rayDir,
    const FogSettings& settings
) {
    if (!settings.enableSunInscattering || settings.sunInscatteringIntensity <= 0.0f) {
        return settings.fogColor;
    }

    Vec3 normSun = normalize_safe(settings.sunDirection, Vec3(0, 1, 0));
    Vec3 normRay = normalize_safe(rayDir, Vec3(0, 0, 1));

    float sunDot = std::max(0.0f, dot_product(normRay, normSun));
    float sunPhase = std::pow(sunDot, std::max(1.0f, settings.sunInscatteringExponent));
    float sunFactor = std::clamp(sunPhase * settings.sunInscatteringIntensity, 0.0f, 1.0f);

    return lerp_vec3(settings.fogColor, settings.sunColor, sunFactor);
}

Vec3 AtmosphericFog::apply_fog(
    const Vec3& sceneRadiance,
    const Vec3& cameraPos,
    const Vec3& worldTargetPos,
    const FogSettings& settings
) {
    if (!settings.enabled || settings.density <= 0.0f) {
        return sceneRadiance;
    }

    float extinction = compute_extinction(cameraPos, worldTargetPos, settings);
    float inscatterAmount = (1.0f - extinction) * std::clamp(settings.maxOpacity, 0.0f, 1.0f);

    Vec3 delta(
        worldTargetPos.x - cameraPos.x,
        worldTargetPos.y - cameraPos.y,
        worldTargetPos.z - cameraPos.z
    );
    Vec3 inscatterColor = compute_inscattering_color(delta, settings);

    return Vec3(
        sceneRadiance.x * extinction + inscatterColor.x * inscatterAmount,
        sceneRadiance.y * extinction + inscatterColor.y * inscatterAmount,
        sceneRadiance.z * extinction + inscatterColor.z * inscatterAmount
    );
}

void AtmosphericFog::apply_fog_to_buffer(
    u32 width, u32 height,
    const Vec3* scenePixels,
    const Vec3* worldPositions,
    const Vec3& cameraPos,
    const FogSettings& settings,
    Vec3* outLitPixels
) {
    if (!scenePixels || !worldPositions || !outLitPixels || width == 0 || height == 0) return;

    const u32 totalPixels = width * height;
    for (u32 i = 0; i < totalPixels; ++i) {
        outLitPixels[i] = apply_fog(scenePixels[i], cameraPos, worldPositions[i], settings);
    }
}

} // namespace nf::rendering
