// Tests/FoliageTests/test_foliage_interaction.cpp — foliage interaction & trampling dynamics.

#include <NF/Foliage/FoliageInteraction.hpp>
#include <NF/Test/TestFramework.hpp>

#include <cmath>
#include <vector>

namespace {

using nf::f32;
using nf::u32;
using nf::Vec3;
using nf::foliage::FoliageBendField;
using nf::foliage::FoliageBendResult;
using nf::foliage::FoliageInstance;
using nf::foliage::FoliageInteractor;
using nf::foliage::FoliageType;
using nf::foliage::InteractorShape;
using nf::foliage::WindState;

NF_TEST(empty_field_returns_zero_interaction) {
    FoliageBendField field;
    Vec3 point{10.0f, 0.0f, 5.0f};
    FoliageBendResult res = field.evaluate(point, 1.0f, 1.0f);

    NF_CHECK(res.displacement.length_sq() < 1e-6f);
    NF_CHECK_NEAR(res.bend_angle, 0.0f, 1e-4f);
    NF_CHECK_NEAR(res.depression, 0.0f, 1e-4f);
    NF_CHECK_NEAR(res.influence, 0.0f, 1e-4f);
}

NF_TEST(interactor_registration_and_update) {
    FoliageBendField field;
    FoliageInteractor it;
    it.position = Vec3{0.0f, 0.0f, 0.0f};
    it.radius = 2.0f;
    it.strength = 1.5f;

    u32 id = field.register_interactor(it);
    NF_CHECK(id > 0);
    NF_CHECK_EQ(field.active_interactor_count(), 1u);

    // Update position
    bool updated = field.update_interactor(id, Vec3{5.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f});
    NF_CHECK(updated);

    // Point at origin is now far from (5,0,0) (radius is 2.0, dist is 5.0)
    FoliageBendResult res_old = field.evaluate(Vec3{0.0f, 0.0f, 0.0f});
    NF_CHECK(res_old.displacement.length_sq() < 1e-6f);

    // Point at (5.5, 0, 0) is inside
    FoliageBendResult res_new = field.evaluate(Vec3{5.5f, 0.0f, 0.0f});
    NF_CHECK(res_new.displacement.length() > 0.05f);

    // Unregister
    bool removed = field.unregister_interactor(id);
    NF_CHECK(removed);
    NF_CHECK_EQ(field.active_interactor_count(), 0u);
}

NF_TEST(radial_displacement_falloff) {
    FoliageBendField field;
    FoliageInteractor it;
    it.position = Vec3{0.0f, 0.0f, 0.0f};
    it.radius = 4.0f;
    it.strength = 1.0f;
    it.push_down_ratio = 0.5f;
    field.register_interactor(it);

    // Sample at near, middle, edge, outside
    FoliageBendResult r_near = field.evaluate(Vec3{0.5f, 0.0f, 0.0f});
    FoliageBendResult r_mid = field.evaluate(Vec3{2.0f, 0.0f, 0.0f});
    FoliageBendResult r_far = field.evaluate(Vec3{3.5f, 0.0f, 0.0f});
    FoliageBendResult r_out = field.evaluate(Vec3{4.5f, 0.0f, 0.0f});

    NF_CHECK(r_near.displacement.length() > r_mid.displacement.length());
    NF_CHECK(r_mid.displacement.length() > r_far.displacement.length());
    NF_CHECK(r_far.displacement.length() > 0.001f);
    NF_CHECK_NEAR(r_out.displacement.length(), 0.0f, 1e-5f);

    // Direction should be pushing along +X (away from origin) and downwards (-Y)
    NF_CHECK(r_near.displacement.x > 0.0f);
    NF_CHECK(r_near.displacement.y < 0.0f);
}

NF_TEST(capsule_interactor_closest_point) {
    FoliageBendField field;
    FoliageInteractor it;
    it.shape = InteractorShape::Capsule;
    it.position = Vec3{0.0f, 0.0f, 0.0f};
    it.end_position = Vec3{10.0f, 0.0f, 0.0f};
    it.radius = 2.0f;
    it.strength = 1.0f;
    field.register_interactor(it);

    // Point parallel to capsule midpoint at (5.0, 0, 1.0)
    FoliageBendResult res_mid = field.evaluate(Vec3{5.0f, 0.0f, 1.0f});
    NF_CHECK(res_mid.displacement.length() > 0.1f);
    // Push direction should be primarily along +Z (perpendicular to capsule segment)
    NF_CHECK(res_mid.displacement.z > 0.05f);

    // Point far past the end (15.0, 0, 0) is outside radius
    FoliageBendResult res_out = field.evaluate(Vec3{15.0f, 0.0f, 0.0f});
    NF_CHECK_NEAR(res_out.displacement.length(), 0.0f, 1e-5f);
}

NF_TEST(interactor_velocity_adds_forward_bias) {
    FoliageBendField field_still;
    FoliageInteractor it_still;
    it_still.position = Vec3{0.0f, 0.0f, 0.0f};
    it_still.radius = 3.0f;
    it_still.strength = 1.0f;
    field_still.register_interactor(it_still);

    FoliageBendField field_moving;
    FoliageInteractor it_moving = it_still;
    it_moving.velocity = Vec3{5.0f, 0.0f, 0.0f}; // moving fast along +X
    field_moving.register_interactor(it_moving);

    // Plant is situated at (0, 0, 1.0)
    FoliageBendResult r_still = field_still.evaluate(Vec3{0.0f, 0.0f, 1.0f});
    FoliageBendResult r_moving = field_moving.evaluate(Vec3{0.0f, 0.0f, 1.0f});

    // Moving interactor adds +X bias to displacement
    NF_CHECK(r_moving.displacement.x > r_still.displacement.x);
}

NF_TEST(plant_stiffness_reduces_displacement) {
    FoliageBendField field;
    FoliageInteractor it;
    it.position = Vec3{0.0f, 0.0f, 0.0f};
    it.radius = 3.0f;
    it.strength = 1.0f;
    field.register_interactor(it);

    Vec3 pt{1.0f, 0.0f, 0.0f};
    FoliageBendResult soft = field.evaluate(pt, 1.0f, 0.5f);  // soft grass
    FoliageBendResult stiff = field.evaluate(pt, 1.0f, 3.0f); // tough shrub

    NF_CHECK(soft.displacement.length() > stiff.displacement.length() * 2.0f);
}

NF_TEST(trample_stamp_decay_over_time) {
    FoliageBendField field;
    Vec3 stamp_pos{2.0f, 0.0f, 2.0f};
    field.add_trample_stamp(stamp_pos, 1.5f, 1.0f, 2.0f); // 2 second lifetime

    NF_CHECK_EQ(field.trample_stamp_count(), 1u);

    // Immediate sample
    FoliageBendResult r0 = field.evaluate(stamp_pos);
    NF_CHECK(r0.depression > 0.8f);

    // Advance 1.0s (half life)
    field.update(1.0f);
    NF_CHECK_EQ(field.trample_stamp_count(), 1u);
    FoliageBendResult r1 = field.evaluate(stamp_pos);
    NF_CHECK(r1.depression < r0.depression);
    NF_CHECK(r1.depression > 0.2f);

    // Advance another 1.1s (past lifetime of 2.0s)
    field.update(1.1f);
    NF_CHECK_EQ(field.trample_stamp_count(), 0u);
    FoliageBendResult r2 = field.evaluate(stamp_pos);
    NF_CHECK_NEAR(r2.depression, 0.0f, 1e-4f);
}

NF_TEST(multiple_interactors_blend) {
    FoliageBendField field;
    FoliageInteractor it1;
    it1.position = Vec3{-1.0f, 0.0f, 0.0f};
    it1.radius = 2.0f;
    field.register_interactor(it1);

    FoliageInteractor it2;
    it2.position = Vec3{1.0f, 0.0f, 0.0f};
    it2.radius = 2.0f;
    field.register_interactor(it2);

    // Midway between both at origin (0, 0, 0)
    // Lateral pushes cancel each other (it1 pushes +X, it2 pushes -X)
    FoliageBendResult r_mid = field.evaluate(Vec3{0.0f, 0.0f, 0.0f});
    NF_CHECK_NEAR(r_mid.displacement.x, 0.0f, 1e-3f);
    // But downward push accumulates
    NF_CHECK(r_mid.displacement.y < -0.05f);
}

NF_TEST(combined_wind_and_interaction) {
    FoliageBendField field;
    FoliageInteractor it;
    it.position = Vec3{0.0f, 0.0f, 0.0f};
    it.radius = 3.0f;
    it.strength = 1.0f;
    field.register_interactor(it);

    FoliageType type;
    type.wind_amount = 0.5f;
    type.wind_speed = 1.0f;

    WindState wind;
    wind.time = 1.0f;
    wind.gust = 1.0f;
    wind.direction = Vec3{1.0f, 0.0f, 0.0f};

    FoliageInstance inst;
    inst.position[0] = 0.5f;
    inst.position[1] = 0.0f;
    inst.position[2] = 0.0f;
    inst.scale = 1.0f;
    inst.wind_phase = 0.0f;

    Vec3 combined = field.evaluate_combined(inst, type, wind);
    // Combined displacement must be non-zero and bounded
    NF_CHECK(combined.length() > 0.01f);
    NF_CHECK(combined.length() <= field.max_displacement());
}

NF_TEST(batch_evaluation_matches_scalar) {
    FoliageBendField field;
    FoliageInteractor it;
    it.position = Vec3{5.0f, 0.0f, 5.0f};
    it.radius = 4.0f;
    it.strength = 1.5f;
    field.register_interactor(it);

    FoliageType type;
    type.wind_amount = 0.5f;

    WindState wind;
    wind.time = 2.5f;
    wind.direction = Vec3{0.0f, 0.0f, 1.0f};

    std::vector<FoliageInstance> instances(5);
    for (size_t i = 0; i < instances.size(); ++i) {
        instances[i].position[0] = static_cast<f32>(i) * 2.0f;
        instances[i].position[1] = 0.0f;
        instances[i].position[2] = 5.0f;
        instances[i].scale = 1.2f;
        instances[i].wind_phase = static_cast<f32>(i) * 0.5f;
    }

    std::vector<Vec3> batch_results(instances.size());
    field.evaluate_batch(instances, type, wind, batch_results);

    for (size_t i = 0; i < instances.size(); ++i) {
        Vec3 single = field.evaluate_combined(instances[i], type, wind);
        NF_CHECK_NEAR(batch_results[i].x, single.x, 1e-4f);
        NF_CHECK_NEAR(batch_results[i].y, single.y, 1e-4f);
        NF_CHECK_NEAR(batch_results[i].z, single.z, 1e-4f);
    }
}

} // namespace
