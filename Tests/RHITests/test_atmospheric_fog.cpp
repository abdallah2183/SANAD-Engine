#include <NF/Rendering/AtmosphericFog.hpp>
#include <NF/Test/TestFramework.hpp>
#include <cmath>
#include <vector>

using namespace nf;
using namespace nf::rendering;

NF_TEST(fog_zero_density_no_extinction) {
    FogSettings settings;
    settings.density = 0.0f;

    Vec3 cam(0, 0, 0);
    Vec3 target(0, 0, 100);

    float tau = AtmosphericFog::compute_optical_depth(cam, target, settings);
    NF_CHECK_NEAR(tau, 0.0f, 1e-6f);

    float ext = AtmosphericFog::compute_extinction(cam, target, settings);
    NF_CHECK_NEAR(ext, 1.0f, 1e-6f);
}

NF_TEST(fog_disabled_returns_scene_radiance) {
    FogSettings settings;
    settings.enabled = false;
    settings.density = 0.5f;

    Vec3 sceneCol(0.2f, 0.4f, 0.8f);
    Vec3 cam(0, 0, 0);
    Vec3 target(0, 0, 50);

    Vec3 result = AtmosphericFog::apply_fog(sceneCol, cam, target, settings);
    NF_CHECK_NEAR(result.x, sceneCol.x, 1e-6f);
    NF_CHECK_NEAR(result.y, sceneCol.y, 1e-6f);
    NF_CHECK_NEAR(result.z, sceneCol.z, 1e-6f);
}

NF_TEST(fog_distance_growth) {
    FogSettings settings;
    settings.density = 0.01f;
    settings.heightFalloff = 0.0f; // Uniform horizontal fog

    Vec3 cam(0, 0, 0);
    Vec3 nearTarg(0, 0, 10);
    Vec3 farTarg(0, 0, 50);

    float tauNear = AtmosphericFog::compute_optical_depth(cam, nearTarg, settings);
    float tauFar = AtmosphericFog::compute_optical_depth(cam, farTarg, settings);

    NF_CHECK(tauFar > tauNear);
    NF_CHECK_NEAR(tauFar / tauNear, 5.0f, 0.01f);

    float extNear = AtmosphericFog::compute_extinction(cam, nearTarg, settings);
    float extFar = AtmosphericFog::compute_extinction(cam, farTarg, settings);

    NF_CHECK(extFar < extNear);
}

NF_TEST(fog_height_falloff) {
    FogSettings settings;
    settings.density = 0.02f;
    settings.heightFalloff = 0.1f;
    settings.fogHeight = 0.0f;

    Vec3 lowCam(0, 0, 0);
    Vec3 lowTarget(100, 0, 0); // At ground height y = 0

    Vec3 highCam(0, 50, 0);
    Vec3 highTarget(100, 50, 0); // At mountain altitude y = 50

    float tauLow = AtmosphericFog::compute_optical_depth(lowCam, lowTarget, settings);
    float tauHigh = AtmosphericFog::compute_optical_depth(highCam, highTarget, settings);

    // High altitude must have drastically lower fog optical thickness
    NF_CHECK(tauLow > tauHigh * 10.0f);
}

NF_TEST(fog_start_distance_delay) {
    FogSettings settings;
    settings.density = 0.05f;
    settings.startDistance = 20.0f;

    Vec3 cam(0, 0, 0);
    Vec3 targetWithin(0, 0, 15); // Within start distance
    Vec3 targetBeyond(0, 0, 30); // Beyond start distance

    float tauWithin = AtmosphericFog::compute_optical_depth(cam, targetWithin, settings);
    NF_CHECK_NEAR(tauWithin, 0.0f, 1e-6f);

    float tauBeyond = AtmosphericFog::compute_optical_depth(cam, targetBeyond, settings);
    NF_CHECK(tauBeyond > 0.0f);
}

NF_TEST(fog_cutoff_distance_clamp) {
    FogSettings settings;
    settings.density = 0.01f;
    settings.cutoffDistance = 100.0f;
    settings.heightFalloff = 0.0f;

    Vec3 cam(0, 0, 0);
    Vec3 targetCutoff(0, 0, 100);
    Vec3 targetFarBeyond(0, 0, 1000);

    float tauCutoff = AtmosphericFog::compute_optical_depth(cam, targetCutoff, settings);
    float tauBeyond = AtmosphericFog::compute_optical_depth(cam, targetFarBeyond, settings);

    // Beyond cutoff, optical thickness must be clamped to the cutoff distance
    NF_CHECK_NEAR(tauCutoff, tauBeyond, 1e-4f);
}

NF_TEST(fog_directional_sun_halo_peak) {
    FogSettings settings;
    settings.enableSunInscattering = true;
    settings.sunDirection = Vec3(0, 0, 1); // Sun in +Z
    settings.sunColor = Vec3(1.0f, 0.8f, 0.2f);
    settings.fogColor = Vec3(0.5f, 0.5f, 0.5f);
    settings.sunInscatteringIntensity = 1.0f;

    // Looking straight at the sun
    Vec3 lookSun(0, 0, 1);
    Vec3 col = AtmosphericFog::compute_inscattering_color(lookSun, settings);

    NF_CHECK_NEAR(col.x, settings.sunColor.x, 1e-3f);
    NF_CHECK_NEAR(col.y, settings.sunColor.y, 1e-3f);
    NF_CHECK_NEAR(col.z, settings.sunColor.z, 1e-3f);
}

NF_TEST(fog_directional_sun_halo_opposite) {
    FogSettings settings;
    settings.enableSunInscattering = true;
    settings.sunDirection = Vec3(0, 0, 1); // Sun in +Z
    settings.sunColor = Vec3(1.0f, 0.8f, 0.2f);
    settings.fogColor = Vec3(0.4f, 0.5f, 0.6f);

    // Looking in opposite direction (-Z)
    Vec3 lookAway(0, 0, -1);
    Vec3 col = AtmosphericFog::compute_inscattering_color(lookAway, settings);

    // Should receive pure ambient fog color
    NF_CHECK_NEAR(col.x, settings.fogColor.x, 1e-4f);
    NF_CHECK_NEAR(col.y, settings.fogColor.y, 1e-4f);
    NF_CHECK_NEAR(col.z, settings.fogColor.z, 1e-4f);
}

NF_TEST(fog_opaque_blend_limit) {
    FogSettings settings;
    settings.density = 10.0f; // Very thick soup
    settings.fogColor = Vec3(0.7f, 0.7f, 0.7f);
    settings.enableSunInscattering = false;

    Vec3 redWall(1.0f, 0.0f, 0.0f);
    Vec3 cam(0, 0, 0);
    Vec3 target(0, 0, 100);

    Vec3 lit = AtmosphericFog::apply_fog(redWall, cam, target, settings);

    // In dense fog, the red wall is obscured and replaced by fog color
    NF_CHECK_NEAR(lit.x, settings.fogColor.x, 0.01f);
    NF_CHECK_NEAR(lit.y, settings.fogColor.y, 0.01f);
    NF_CHECK_NEAR(lit.z, settings.fogColor.z, 0.01f);
}

NF_TEST(fog_buffer_application) {
    const u32 count = 4;
    std::vector<Vec3> scenePixels(count, Vec3(0.5f, 0.5f, 0.5f));
    std::vector<Vec3> worldPositions = {
        Vec3(0, 0, 0),
        Vec3(0, 0, 20),
        Vec3(0, 0, 50),
        Vec3(0, 0, 100)
    };
    std::vector<Vec3> outPixels(count);

    FogSettings settings;
    settings.density = 0.02f;
    settings.fogColor = Vec3(1.0f, 1.0f, 1.0f);

    AtmosphericFog::apply_fog_to_buffer(2, 2, scenePixels.data(), worldPositions.data(), Vec3(0, 0, 0), settings, outPixels.data());

    // First pixel (at distance 0): should match scene pixel exactly
    NF_CHECK_NEAR(outPixels[0].x, 0.5f, 1e-4f);

    // Later pixels: luminance should increase as they blend with white fog
    NF_CHECK(outPixels[1].x > outPixels[0].x);
    NF_CHECK(outPixels[3].x > outPixels[1].x);
}
