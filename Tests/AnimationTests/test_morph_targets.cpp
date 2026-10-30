#include <NF/Animation/MorphTarget.hpp>
#include <NF/Test/TestFramework.hpp>

using namespace nf;
using namespace nf::animation;

NF_TEST(morph_target_set_creation_and_resizing) {
    MorphTargetSet set(4);
    NF_CHECK_EQ(set.vertex_count(), 4u);
    NF_CHECK_EQ(set.target_count(), 0u);

    const size_t smile_idx = set.add_target("smile");
    NF_CHECK_EQ(smile_idx, 0u);
    NF_CHECK_EQ(set.target_count(), 1u);

    const auto* smile = set.get_target(0);
    NF_CHECK(smile != nullptr);
    NF_CHECK_EQ(smile->position_deltas.size(), 4u);
    NF_CHECK_EQ(smile->normal_deltas.size(), 4u);

    // Resizing updates existing targets
    set.set_vertex_count(8);
    NF_CHECK_EQ(set.vertex_count(), 8u);
    NF_CHECK_EQ(smile->position_deltas.size(), 8u);
}

NF_TEST(morph_target_delta_setting) {
    MorphTargetSet set(3);
    set.add_target("brow_raise");
    auto* target = set.get_target(0);

    target->set_delta(1, Vec3{0, 0.5f, 0}, Vec3{0, 1.0f, 0});
    NF_CHECK_NEAR(target->position_deltas[1].y, 0.5f, 1e-5f);
    NF_CHECK_NEAR(target->normal_deltas[1].y, 1.0f, 1e-5f);

    // Out of bounds is safely ignored
    target->set_delta(10, Vec3{1, 1, 1});
}

NF_TEST(morph_target_lookup_by_name) {
    MorphTargetSet set(2);
    set.add_target("blink_left");
    set.add_target("blink_right");
    set.add_target("jaw_open");

    NF_CHECK_EQ(set.find_target_index("blink_left"), 0);
    NF_CHECK_EQ(set.find_target_index("blink_right"), 1);
    NF_CHECK_EQ(set.find_target_index("jaw_open"), 2);
    NF_CHECK_EQ(set.find_target_index("non_existent"), -1);

    NF_CHECK(set.find_target("jaw_open") != nullptr);
    NF_CHECK(set.find_target("unknown") == nullptr);
}

NF_TEST(morph_controller_binding_initializes_weights) {
    MorphTargetSet set(3);
    set.add_target("shape_a");
    set.add_target("shape_b");

    MorphController ctrl(&set);
    NF_CHECK_EQ(ctrl.get_weight(0), 0.0f);
    NF_CHECK_EQ(ctrl.get_weight(1), 0.0f);

    ctrl.set_weight(0, 0.75f);
    NF_CHECK_NEAR(ctrl.get_weight(0), 0.75f, 1e-5f);

    // Re-binding resets weights
    ctrl.bind_target_set(&set);
    NF_CHECK_EQ(ctrl.get_weight(0), 0.0f);
}

NF_TEST(morph_controller_weight_clamping) {
    MorphTargetSet set(2);
    set.add_target("morph");

    MorphController ctrl(&set);
    ctrl.set_weight_range(0.0f, 1.0f);

    ctrl.set_weight(0, 1.5f);
    NF_CHECK_NEAR(ctrl.get_weight(0), 1.0f, 1e-5f);

    ctrl.set_weight(0, -0.5f);
    NF_CHECK_NEAR(ctrl.get_weight(0), 0.0f, 1e-5f);
}

NF_TEST(morph_controller_set_weight_by_name) {
    MorphTargetSet set(2);
    set.add_target("smile");

    MorphController ctrl(&set);
    const bool success = ctrl.set_weight_by_name("smile", 0.6f);
    NF_CHECK(success);
    NF_CHECK_NEAR(ctrl.get_weight_by_name("smile"), 0.6f, 1e-5f);

    const bool failure = ctrl.set_weight_by_name("frown", 0.5f);
    NF_CHECK(!failure);
}

NF_TEST(morph_controller_single_shape_deformation) {
    MorphTargetSet set(2);
    const size_t idx = set.add_target("stretch");
    set.get_target(idx)->set_delta(0, Vec3{0, 2.0f, 0});

    const std::vector<Vec3> base_pos = {Vec3{0, 0, 0}, Vec3{1, 1, 1}};
    const std::vector<Vec3> base_norm = {Vec3{0, 1, 0}, Vec3{0, 1, 0}};
    std::vector<Vec3> out_pos, out_norm;

    MorphController ctrl(&set);
    ctrl.set_weight(0, 0.5f);
    ctrl.apply(base_pos, base_norm, out_pos, out_norm);

    NF_CHECK_NEAR(out_pos[0].y, 1.0f, 1e-4f); // 0 + 0.5 * 2 = 1.0
    NF_CHECK_NEAR(out_pos[1].y, 1.0f, 1e-4f); // unaffected vertex
}

NF_TEST(morph_controller_multi_shape_additive_accumulation) {
    MorphTargetSet set(1);
    const size_t t0 = set.add_target("move_x");
    set.get_target(t0)->set_delta(0, Vec3{1.0f, 0, 0});

    const size_t t1 = set.add_target("move_y");
    set.get_target(t1)->set_delta(0, Vec3{0, 2.0f, 0});

    const std::vector<Vec3> base_pos = {Vec3{0, 0, 0}};
    const std::vector<Vec3> base_norm = {Vec3{0, 0, 1}};
    std::vector<Vec3> out_pos, out_norm;

    MorphController ctrl(&set);
    ctrl.set_weight_by_name("move_x", 0.5f);
    ctrl.set_weight_by_name("move_y", 1.0f);
    ctrl.apply(base_pos, base_norm, out_pos, out_norm);

    NF_CHECK_NEAR(out_pos[0].x, 0.5f, 1e-4f);
    NF_CHECK_NEAR(out_pos[0].y, 2.0f, 1e-4f);
}

NF_TEST(morph_controller_normal_renormalization) {
    MorphTargetSet set(1);
    const size_t t0 = set.add_target("twist_normal");
    set.get_target(t0)->set_delta(0, Vec3{}, Vec3{1.0f, 0, 0}); // Add X to normal

    const std::vector<Vec3> base_pos = {Vec3{0, 0, 0}};
    const std::vector<Vec3> base_norm = {Vec3{0, 1, 0}}; // Along Y
    std::vector<Vec3> out_pos, out_norm;

    MorphController ctrl(&set);
    ctrl.set_weight(0, 1.0f);
    ctrl.apply(base_pos, base_norm, out_pos, out_norm);

    // Sum is (1, 1, 0), normalized: (1/sqrt(2), 1/sqrt(2), 0)
    NF_CHECK_NEAR(out_norm[0].length(), 1.0f, 1e-4f);
    NF_CHECK_NEAR(out_norm[0].x, 0.7071f, 1e-3f);
    NF_CHECK_NEAR(out_norm[0].y, 0.7071f, 1e-3f);
}

NF_TEST(morph_controller_reset_weights) {
    MorphTargetSet set(2);
    set.add_target("a");
    set.add_target("b");

    MorphController ctrl(&set);
    ctrl.set_weight(0, 0.8f);
    ctrl.set_weight(1, 0.4f);

    ctrl.reset_weights();
    NF_CHECK_EQ(ctrl.get_weight(0), 0.0f);
    NF_CHECK_EQ(ctrl.get_weight(1), 0.0f);
}
