// Tests/VFXTests/test_beam_system.cpp — procedural electric arcs, chain lightning, and laser beams.

#include <NF/Test/TestFramework.hpp>
#include <NF/Vfx/BeamSystem.hpp>

#include <cmath>
#include <vector>

namespace {

using nf::f32;
using nf::u32;
using nf::Vec3;
using nf::Vec4;
using nf::vfx::BeamSegment;
using nf::vfx::BeamVertex;
using nf::vfx::ChainJump;
using nf::vfx::ChainLightningSolver;
using nf::vfx::ChainTarget;
using nf::vfx::LaserBeam;
using nf::vfx::LightningArc;
using nf::vfx::LightningArcDescriptor;

NF_TEST(lightning_endpoints_match) {
    LightningArc arc;
    LightningArcDescriptor desc;
    desc.subdivisions = 8;
    desc.roughness = 0.5f;
    desc.branch_chance = 0.0f; // trunk only

    Vec3 start{0.0f, 0.0f, 0.0f};
    Vec3 end{10.0f, 0.0f, 0.0f};
    arc.generate(start, end, desc);

    NF_CHECK(arc.segment_count() > 0);
    NF_CHECK_NEAR(arc.start_point().x, start.x, 1e-4f);
    NF_CHECK_NEAR(arc.end_point().x, end.x, 1e-4f);

    const auto& segs = arc.segments();
    NF_CHECK_NEAR(segs.front().start.x, start.x, 1e-4f);
    NF_CHECK_NEAR(segs.back().end.x, end.x, 1e-4f);
}

NF_TEST(straight_beam_when_zero_roughness) {
    LightningArc arc;
    LightningArcDescriptor desc;
    desc.subdivisions = 16;
    desc.roughness = 0.0f;     // no deviation
    desc.branch_chance = 0.0f;

    Vec3 start{0.0f, 0.0f, 0.0f};
    Vec3 end{10.0f, 0.0f, 0.0f};
    arc.generate(start, end, desc);

    // Every intermediate point should have y == 0 and z == 0
    for (const auto& seg : arc.segments()) {
        NF_CHECK_NEAR(seg.start.y, 0.0f, 1e-4f);
        NF_CHECK_NEAR(seg.start.z, 0.0f, 1e-4f);
        NF_CHECK_NEAR(seg.end.y, 0.0f, 1e-4f);
        NF_CHECK_NEAR(seg.end.z, 0.0f, 1e-4f);
    }
}

NF_TEST(jaggedness_with_roughness) {
    LightningArc arc;
    LightningArcDescriptor desc;
    desc.subdivisions = 16;
    desc.roughness = 0.6f;
    desc.branch_chance = 0.0f;

    Vec3 start{0.0f, 0.0f, 0.0f};
    Vec3 end{10.0f, 0.0f, 0.0f};
    arc.generate(start, end, desc);

    // At least one segment node must deviate off the X-axis
    bool deviated = false;
    for (const auto& seg : arc.segments()) {
        if (std::abs(seg.start.y) > 0.01f || std::abs(seg.start.z) > 0.01f) {
            deviated = true;
            break;
        }
    }
    NF_CHECK(deviated);
}

NF_TEST(deterministic_arc_generation) {
    LightningArcDescriptor desc1;
    desc1.subdivisions = 8;
    desc1.roughness = 0.5f;
    desc1.jitter_seed = 42ull;

    LightningArcDescriptor desc2 = desc1;
    desc2.jitter_seed = 42ull;

    LightningArcDescriptor desc3 = desc1;
    desc3.jitter_seed = 99999ull;

    LightningArc arc1, arc2, arc3;
    Vec3 start{0.0f, 0.0f, 0.0f};
    Vec3 end{5.0f, 5.0f, 0.0f};

    arc1.generate(start, end, desc1);
    arc2.generate(start, end, desc2);
    arc3.generate(start, end, desc3);

    // Arc1 and Arc2 must be bit-identical
    NF_CHECK_EQ(arc1.segment_count(), arc2.segment_count());
    for (size_t i = 0; i < arc1.segment_count(); ++i) {
        NF_CHECK_NEAR(arc1.segments()[i].start.x, arc2.segments()[i].start.x, 1e-5f);
        NF_CHECK_NEAR(arc1.segments()[i].start.y, arc2.segments()[i].start.y, 1e-5f);
    }

    // Arc3 with different seed should have differences
    bool different = false;
    for (size_t i = 0; i < std::min(arc1.segment_count(), arc3.segment_count()); ++i) {
        if (std::abs(arc1.segments()[i].start.x - arc3.segments()[i].start.x) > 1e-4f) {
            different = true;
            break;
        }
    }
    NF_CHECK(different);
}

NF_TEST(secondary_branching) {
    LightningArc arc;
    LightningArcDescriptor desc;
    desc.subdivisions = 16;
    desc.roughness = 0.4f;
    desc.branch_chance = 0.8f; // high branch probability
    desc.max_branch_depth = 2;

    arc.generate(Vec3{0.0f, 0.0f, 0.0f}, Vec3{20.0f, 0.0f, 0.0f}, desc);

    // Total segment count must exceed trunk count due to branches
    NF_CHECK(arc.segment_count() > arc.trunk_segment_count());

    bool found_branch = false;
    for (const auto& seg : arc.segments()) {
        if (seg.branch_level > 0) {
            found_branch = true;
            break;
        }
    }
    NF_CHECK(found_branch);
}

NF_TEST(mesh_generation_quads_and_indices) {
    LightningArc arc;
    LightningArcDescriptor desc;
    desc.subdivisions = 8;
    desc.roughness = 0.3f;
    desc.branch_chance = 0.0f; // trunk only

    arc.generate(Vec3{0.0f, 0.0f, 0.0f}, Vec3{10.0f, 0.0f, 0.0f}, desc);

    std::vector<BeamVertex> vertices;
    std::vector<uint32_t> indices;
    arc.generate_mesh(vertices, indices, Vec3{5.0f, 0.0f, -5.0f}, false); // core only

    // Core only: 4 vertices and 6 indices per segment
    size_t segs = arc.segment_count();
    NF_CHECK_EQ(vertices.size(), segs * 4);
    NF_CHECK_EQ(indices.size(), segs * 6);

    // With glow enabled: 2 quads per segment
    arc.generate_mesh(vertices, indices, Vec3{5.0f, 0.0f, -5.0f}, true);
    NF_CHECK_EQ(vertices.size(), segs * 8);
    NF_CHECK_EQ(indices.size(), segs * 12);
}

NF_TEST(chain_lightning_closest_targets) {
    Vec3 source{0.0f, 0.0f, 0.0f};

    std::vector<ChainTarget> candidates = {
        {1, Vec3{2.0f, 0.0f, 0.0f}, 1.0f},   // nearest (2m)
        {2, Vec3{5.0f, 0.0f, 0.0f}, 1.0f},   // 2nd (3m from #1)
        {3, Vec3{8.0f, 0.0f, 0.0f}, 1.0f},   // 3rd (3m from #2)
        {4, Vec3{100.0f, 0.0f, 0.0f}, 1.0f}, // out of range (100m)
    };

    auto jumps = ChainLightningSolver::solve(source, candidates, 3, 10.0f);

    NF_CHECK_EQ(jumps.size(), 3u);
    NF_CHECK_EQ(jumps[0].target_id, 1u);
    NF_CHECK_EQ(jumps[1].target_id, 2u);
    NF_CHECK_EQ(jumps[2].target_id, 3u);
}

NF_TEST(chain_lightning_decay_and_delay) {
    Vec3 source{0.0f, 0.0f, 0.0f};
    std::vector<ChainTarget> targets = {
        {1, Vec3{1.0f, 0.0f, 0.0f}, 1.0f},
        {2, Vec3{2.0f, 0.0f, 0.0f}, 1.0f},
        {3, Vec3{3.0f, 0.0f, 0.0f}, 1.0f},
    };

    f32 decay = 0.8f;
    f32 delay = 0.05f;
    auto jumps = ChainLightningSolver::solve(source, targets, 3, 10.0f, decay, delay);

    NF_CHECK_EQ(jumps.size(), 3u);
    NF_CHECK_NEAR(jumps[0].damage_multiplier, 1.0f, 1e-4f);
    NF_CHECK_NEAR(jumps[0].delay, 0.0f, 1e-4f);

    NF_CHECK_NEAR(jumps[1].damage_multiplier, 0.8f, 1e-4f);
    NF_CHECK_NEAR(jumps[1].delay, 0.05f, 1e-4f);

    NF_CHECK_NEAR(jumps[2].damage_multiplier, 0.64f, 1e-4f);
    NF_CHECK_NEAR(jumps[2].delay, 0.10f, 1e-4f);
}

NF_TEST(laser_beam_endpoints_and_hit) {
    LaserBeam laser;
    laser.set_direction(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 50.0f);

    NF_CHECK_NEAR(laser.length(), 50.0f, 1e-4f);

    // Set hit obstruction at 12.5 meters
    laser.set_hit_distance(12.5f);
    NF_CHECK_NEAR(laser.length(), 12.5f, 1e-4f);

    Vec3 hit_pt = laser.current_hit_point();
    NF_CHECK_NEAR(hit_pt.x, 0.0f, 1e-4f);
    NF_CHECK_NEAR(hit_pt.y, 12.5f, 1e-4f);
    NF_CHECK_NEAR(hit_pt.z, 0.0f, 1e-4f);

    // Clamping past max length
    laser.set_hit_distance(100.0f);
    NF_CHECK_NEAR(laser.length(), 50.0f, 1e-4f);
}

NF_TEST(laser_pulse_modulation) {
    LaserBeam laser;
    laser.set_direction(Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, 10.0f);
    laser.set_colors(Vec4{1.0f, 1.0f, 1.0f, 1.0f}, Vec4{0.5f, 0.5f, 0.5f, 0.5f});
    laser.set_pulse(2.0f, 0.5f, 1.0f); // 2 Hz frequency, range [0.5, 1.0]

    std::vector<BeamVertex> v0;
    std::vector<uint32_t> idx0;
    laser.generate_mesh(v0, idx0);

    // Advance quarter period (1 / (4 * 2) = 0.125s) -> sin peaks at 1.0
    laser.update(0.125f);
    std::vector<BeamVertex> v1;
    std::vector<uint32_t> idx1;
    laser.generate_mesh(v1, idx1);

    NF_CHECK(v1[0].color.x > v0[0].color.x);
}

} // namespace
