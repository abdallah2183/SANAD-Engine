#pragma once

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

namespace nf::rendering {

/// Exponential height fog and atmospheric inscattering parameters.
struct FogSettings {
    // Basic fog density and height parameters
    float density = 0.02f;              // Overall fog density multiplier
    float heightFalloff = 0.1f;         // Vertical density falloff rate (exponent per meter)
    float fogHeight = 0.0f;             // World height baseline for maximum fog density
    float maxOpacity = 1.0f;            // Maximum fog opacity cap [0, 1]
    float startDistance = 0.0f;         // Distance before fog starts accumulating
    float cutoffDistance = 5000.0f;     // Distance beyond which fog is clamped

    // Ambient fog coloration
    Vec3 fogColor = Vec3(0.5f, 0.6f, 0.7f); // Base fog albedo (sky-tinted slate)

    // Sun directional inscattering (atmospheric halo)
    bool enableSunInscattering = true;
    Vec3 sunDirection = Vec3(0.577f, -0.577f, 0.577f); // Normalized direction toward sun
    Vec3 sunColor = Vec3(1.0f, 0.9f, 0.7f);            // Solar inscattering color
    float sunInscatteringExponent = 16.0f;             // Phase function sharpness
    float sunInscatteringIntensity = 1.0f;             // Sun inscattering blend strength

    bool enabled = true;
};

/// Analytical evaluator for exponential height fog and atmospheric inscattering.
class AtmosphericFog {
public:
    /// Evaluates the analytical optical thickness (tau) along a ray segment from camera to world target.
    [[nodiscard]] static float compute_optical_depth(
        const Vec3& cameraPos,
        const Vec3& worldTargetPos,
        const FogSettings& settings
    );

    /// Evaluates fog extinction factor [0, 1], where 1 = no fog (100% transmission), 0 = completely opaque fog.
    [[nodiscard]] static float compute_extinction(
        const Vec3& cameraPos,
        const Vec3& worldTargetPos,
        const FogSettings& settings
    );

    /// Evaluates directional inscattering color taking sun direction and phase into account.
    [[nodiscard]] static Vec3 compute_inscattering_color(
        const Vec3& rayDir,
        const FogSettings& settings
    );

    /// Blends incoming scene radiance with atmospheric fog along the view ray.
    /// Returns: sceneColor * extinction + inscatteringColor * inscatteringAmount
    [[nodiscard]] static Vec3 apply_fog(
        const Vec3& sceneRadiance,
        const Vec3& cameraPos,
        const Vec3& worldTargetPos,
        const FogSettings& settings
    );

    /// Evaluates fog on an entire RGB image buffer against a corresponding world position buffer.
    static void apply_fog_to_buffer(
        u32 width, u32 height,
        const Vec3* scenePixels,
        const Vec3* worldPositions,
        const Vec3& cameraPos,
        const FogSettings& settings,
        Vec3* outLitPixels
    );
};

} // namespace nf::rendering
