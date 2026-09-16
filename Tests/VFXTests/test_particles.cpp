// VFXTests — deterministic particle simulation.

#include <NF/Test/TestFramework.hpp>
#include <NF/Vfx/Particles.hpp>

#include <cmath>
#include <vector>

using namespace nf;
using namespace nf::vfx;

NF_TEST(particles_emit_at_rate_and_die) {
    EmitterConfig cfg;
    cfg.rate = 60.0f;
    cfg.lifetime = 1.0f;
    cfg.lifetime_spread = 0.0f; // exact lifetimes: steady state == rate * life
    cfg.gravity = Vec3{0, 0, 0};
    ParticleSystem ps(cfg);
    for (int i = 0; i < 60; ++i) ps.update(1.0f / 60.0f); // 1 second
    NF_CHECK(ps.alive() == 60);
    for (int i = 0; i < 60; ++i) ps.update(1.0f / 60.0f); // another second
    NF_CHECK(ps.alive() == 60); // births == deaths: steady state
}

NF_TEST(particles_burst_and_cap) {
    EmitterConfig cfg;
    cfg.rate = 0.0f; // burst-only
    cfg.max_particles = 10;
    ParticleSystem ps(cfg);
    ps.burst(25);
    NF_CHECK(ps.alive() == 10); // capped, never over-allocated
    ps.clear();
    NF_CHECK(ps.alive() == 0);
    ps.burst(3);
    NF_CHECK(ps.alive() == 3);
}

NF_TEST(particles_gravity_pulls_down) {
    EmitterConfig cfg;
    cfg.rate = 0.0f;
    cfg.velocity = Vec3{0, 0, 0};
    cfg.velocity_spread = Vec3{0, 0, 0};
    cfg.gravity = Vec3{0, -10, 0};
    cfg.lifetime = 5.0f;
    cfg.lifetime_spread = 0.0f;
    ParticleSystem ps(cfg);
    ps.burst(1);
    NF_CHECK(ps.alive() == 1);
    const float y0 = ps.particles()[0].position.y;
    ps.update(0.5f);
    // Semi-implicit Euler: v=-5 after 0.5s, y drops by v*dt = -2.5.
    NF_CHECK_NEAR(ps.particles()[0].velocity.y, -5.0f, 1e-4f);
    NF_CHECK_NEAR(ps.particles()[0].position.y, y0 - 2.5f, 1e-4f);
    // Drag bleeds speed.
    EmitterConfig draggy = cfg;
    draggy.drag = 2.0f;
    ParticleSystem ps2(draggy);
    ps2.burst(1);
    ps2.update(0.5f);
    NF_CHECK(std::abs(ps2.particles()[0].velocity.y) < std::abs(ps.particles()[0].velocity.y));
}

NF_TEST(particles_grade_size_and_color_over_life) {
    EmitterConfig cfg;
    cfg.rate = 0.0f;
    cfg.lifetime = 2.0f;
    cfg.lifetime_spread = 0.0f;
    cfg.gravity = Vec3{0, 0, 0};
    cfg.start_size = 1.0f;
    cfg.end_size = 0.0f;
    cfg.start_color = Vec3{1, 1, 1};
    cfg.end_color = Vec3{0, 0, 0};
    ParticleSystem ps(cfg);
    ps.burst(1);
    ps.update(1.0f); // half life
    NF_CHECK(ps.alive() == 1);
    NF_CHECK_NEAR(ps.particles()[0].size, 0.5f, 1e-5f);
    NF_CHECK_NEAR(ps.particles()[0].color.x, 0.5f, 1e-5f);
    ps.update(1.0f); // end of life: killed
    NF_CHECK(ps.alive() == 0);
}

NF_TEST(particles_are_deterministic) {
    EmitterConfig cfg;
    cfg.rate = 45.0f;
    cfg.lifetime = 2.0f;
    ParticleSystem a(cfg), b(cfg);
    for (int i = 0; i < 120; ++i) {
        a.update(1.0f / 60.0f);
        b.update(1.0f / 60.0f);
    }
    NF_CHECK(a.alive() == b.alive());
    NF_CHECK(a.alive() > 0);
    for (usize i = 0; i < a.alive(); ++i) {
        NF_CHECK_NEAR(a.particles()[i].position.x, b.particles()[i].position.x, 1e-6f);
        NF_CHECK_NEAR(a.particles()[i].position.y, b.particles()[i].position.y, 1e-6f);
        NF_CHECK_NEAR(a.particles()[i].velocity.z, b.particles()[i].velocity.z, 1e-6f);
    }
}
