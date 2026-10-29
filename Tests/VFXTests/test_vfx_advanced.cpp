#include <NF/Test/TestFramework.hpp>
#include <NF/VFX/ParticleEmitterShape.hpp>
#include <NF/VFX/ForceField.hpp>
#include <NF/VFX/RibbonTrail.hpp>
#include <NF/VFX/ColorGradient.hpp>
#include <cmath>

using namespace nf;
using namespace nf::vfx;

NF_TEST(emitter_shapes_volume_and_surface) {
    // 1. Sphere
    EmitterShapeConfig sphere_cfg;
    sphere_cfg.type = EmitterShapeType::Sphere;
    sphere_cfg.radius = 5.0f;
    sphere_cfg.center = Vec3(0, 0, 0);

    for (int i = 0; i < 20; ++i) {
        float r1 = static_cast<float>(i) / 20.0f;
        float r2 = static_cast<float>((i * 7) % 20) / 20.0f;
        float r3 = static_cast<float>((i * 13) % 20) / 20.0f;
        auto sp = generate_spawn_point(sphere_cfg, r1, r2, r3);
        float dist = sp.position.length();
        NF_CHECK(dist <= 5.001f);
        NF_CHECK(sp.direction.length() > 0.99f);
    }

    // 2. Box
    EmitterShapeConfig box_cfg;
    box_cfg.type = EmitterShapeType::Box;
    box_cfg.center = Vec3(10, 0, 0);
    box_cfg.box_extents = Vec3(2, 3, 4);

    auto sp_box = generate_spawn_point(box_cfg, 0.5f, 0.5f, 0.5f);
    NF_CHECK_NEAR(sp_box.position.x, 10.0f, 0.001f);
    NF_CHECK_NEAR(sp_box.position.y, 0.0f, 0.001f);
    NF_CHECK_NEAR(sp_box.position.z, 0.0f, 0.001f);

    // 3. Ring
    EmitterShapeConfig ring_cfg;
    ring_cfg.type = EmitterShapeType::Ring;
    ring_cfg.inner_radius = 2.0f;
    ring_cfg.radius = 4.0f;

    auto sp_ring = generate_spawn_point(ring_cfg, 0.25f, 0.5f, 0.0f);
    float r_dist = std::sqrt(sp_ring.position.x * sp_ring.position.x + sp_ring.position.z * sp_ring.position.z);
    NF_CHECK(r_dist >= 1.99f && r_dist <= 4.01f);
    NF_CHECK_NEAR(sp_ring.position.y, 0.0f, 0.001f);
}

NF_TEST(force_field_wind_and_attractor) {
    // Directional Wind
    ForceField wind;
    wind.type = ForceType::DirectionalWind;
    wind.direction = Vec3(1.0f, 0.0f, 0.0f);
    wind.strength = 10.0f;
    wind.radius = 0.0f; // unbounded

    Vec3 f_wind = wind.evaluate(Vec3(5, 5, 5), Vec3(0, 0, 0));
    NF_CHECK_NEAR(f_wind.x, 10.0f, 0.001f);
    NF_CHECK_NEAR(f_wind.y, 0.0f, 0.001f);
    NF_CHECK_NEAR(f_wind.z, 0.0f, 0.001f);

    // Point Attractor with radius falloff
    ForceField attractor;
    attractor.type = ForceType::PointAttractor;
    attractor.position = Vec3(0, 0, 0);
    attractor.strength = 20.0f;
    attractor.radius = 10.0f;
    attractor.falloff_exp = 1.0f;

    // Particle at (5, 0, 0) -> distance 5 -> falloff = 1 - 5/10 = 0.5 -> effective strength = 10
    // Direction towards origin is (-1, 0, 0)
    Vec3 f_attr = attractor.evaluate(Vec3(5, 0, 0), Vec3(0, 0, 0));
    NF_CHECK_NEAR(f_attr.x, -10.0f, 0.001f);
    NF_CHECK_NEAR(f_attr.y, 0.0f, 0.001f);
    NF_CHECK_NEAR(f_attr.z, 0.0f, 0.001f);

    // Particle outside radius (12, 0, 0) -> force should be 0
    Vec3 f_out = attractor.evaluate(Vec3(12, 0, 0), Vec3(0, 0, 0));
    NF_CHECK_NEAR(f_out.length(), 0.0f, 0.001f);
}

NF_TEST(force_field_vortex) {
    ForceField vortex;
    vortex.type = ForceType::Vortex;
    vortex.position = Vec3(0, 0, 0);
    vortex.direction = Vec3(0, 1, 0); // Y-axis rotation
    vortex.strength = 15.0f;
    vortex.radius = 0.0f;

    // Particle at (2, 0, 0) -> radial vector is (2, 0, 0).
    // Tangent = Axis (0,1,0) x Radial (2,0,0) = (0, 0, -2) -> normalized (0, 0, -1)
    Vec3 f_vortex = vortex.evaluate(Vec3(2, 0, 0), Vec3(0, 0, 0));
    NF_CHECK_NEAR(f_vortex.x, 0.0f, 0.001f);
    NF_CHECK_NEAR(f_vortex.y, 0.0f, 0.001f);
    NF_CHECK_NEAR(f_vortex.z, -15.0f, 0.001f);
}

NF_TEST(ribbon_trail_mesh_generation) {
    RibbonTrail trail(1.0f, 0.1f);
    trail.add_point(Vec3(0, 0, 0), 0.4f, Vec4(1, 0, 0, 1));
    trail.add_point(Vec3(0, 1, 0), 0.4f, Vec4(1, 0, 0, 1));
    trail.add_point(Vec3(0, 2, 0), 0.4f, Vec4(1, 0, 0, 1));
    trail.add_point(Vec3(0, 3, 0), 0.4f, Vec4(1, 0, 0, 1));

    NF_CHECK_EQ(trail.point_count(), 4u);

    std::vector<RibbonVertex> vertices;
    std::vector<uint32_t> indices;
    trail.generate_mesh(vertices, indices, Vec3(0, 1.5f, -5.0f));

    // 4 points -> 8 vertices (2 per point: left and right)
    NF_CHECK_EQ(vertices.size(), 8u);
    // 3 quad segments -> 3 * 6 = 18 indices
    NF_CHECK_EQ(indices.size(), 18u);

    // Verify UV v coordinate increases from 0.0 to 1.0
    NF_CHECK_NEAR(vertices[0].uv.y, 0.0f, 0.001f);
    NF_CHECK_NEAR(vertices[7].uv.y, 1.0f, 0.001f);

    // Update trail by 0.5s
    trail.update(0.5f);
    NF_CHECK_EQ(trail.point_count(), 4u);

    // Regenerate mesh: width and alpha should decay by 50%
    trail.generate_mesh(vertices, indices, Vec3(0, 1.5f, -5.0f));
    NF_CHECK_NEAR(vertices[0].color.w, 0.5f, 0.01f);

    // Update past lifetime (1.1s total) -> trail empties
    trail.update(0.6f);
    NF_CHECK_EQ(trail.point_count(), 0u);
}

NF_TEST(color_gradient_stops_and_presets) {
    ColorGradient cg;
    cg.add_stop(0.0f, Vec4(1, 0, 0, 1)); // Red
    cg.add_stop(1.0f, Vec4(0, 0, 1, 1)); // Blue

    Vec4 c_start = cg.evaluate(0.0f);
    NF_CHECK_NEAR(c_start.x, 1.0f, 0.001f);
    NF_CHECK_NEAR(c_start.z, 0.0f, 0.001f);

    Vec4 c_mid = cg.evaluate(0.5f);
    NF_CHECK_NEAR(c_mid.x, 0.5f, 0.001f);
    NF_CHECK_NEAR(c_mid.z, 0.5f, 0.001f);

    Vec4 c_end = cg.evaluate(1.0f);
    NF_CHECK_NEAR(c_end.x, 0.0f, 0.001f);
    NF_CHECK_NEAR(c_end.z, 1.0f, 0.001f);

    // Test fire preset
    ColorGradient fire = ColorGradient::fire();
    NF_CHECK_EQ(fire.stop_count(), 4u);
    Vec4 fire_mid = fire.evaluate(0.3f);
    NF_CHECK(fire_mid.x > 0.8f);
}
