// EditorTests — gizmo model (P2): snap, Local/World, multi-select transaction.
//
// Everything here is CPU-only: no GPU, no window, no font atlas. The tests
// drive the pure model directly — apply_gizmo_delta for the math, GizmoDrag
// for the gesture, and CommandStack for the "one undo step per gesture"
// contract the viewport panel relies on.
//
// Rotation composes through quaternions (euler angles do not commute), so the
// Local-vs-World tests compare the QUATERNION the euler output reconstructs
// against the expected product, never the euler angles themselves: the
// decomposition in NF/Scene/Transform.hpp can pick a different but equivalent
// euler triple, and asserting raw degrees would pin an implementation detail
// of the decomposer rather than the physics.

#include <NF/Core/Math.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/Editor/Commands.hpp>
#include <NF/Editor/Gizmo.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/Test/TestFramework.hpp>

#include <cmath>
#include <string>
#include <vector>

using namespace nf;
using namespace nf::editor;
using namespace nf::scene;

namespace {

// A rotate goes euler -> quat -> composed -> euler, and that round trip loses
// ~4e-6 degrees at 45 degrees in single precision (verified numerically), so a
// rotate assertion cannot hold at 1e-6. Translate/scale never touch trig and
// stay exact, which is why their tolerances are tighter.
constexpr float kRotateTol = 1e-4f;

GizmoDelta translate_delta(float x, float y, float z) {
    GizmoDelta d;
    d.dx = x;
    d.dy = y;
    d.dz = z;
    return d;
}

GizmoDelta rotate_delta(float yaw_deg, float pitch_deg) {
    GizmoDelta d;
    d.yaw_deg = yaw_deg;
    d.pitch_deg = pitch_deg;
    return d;
}

GizmoDelta scale_delta(float dscale) {
    GizmoDelta d;
    d.dscale = dscale;
    return d;
}

ecs::Entity add_box(ecs::World& w, float x, float y, float z) {
    Transform t{};
    t.local_x = x;
    t.local_y = y;
    t.local_z = z;
    const ecs::Entity e = w.create_entity();
    w.add<Transform>(e, t);
    return e;
}

bool transforms_equal(const Transform& a, const Transform& b) {
    return std::abs(a.local_x - b.local_x) < 1e-6f && std::abs(a.local_y - b.local_y) < 1e-6f &&
           std::abs(a.local_z - b.local_z) < 1e-6f && std::abs(a.rot_x - b.rot_x) < 1e-6f &&
           std::abs(a.rot_y - b.rot_y) < 1e-6f && std::abs(a.rot_z - b.rot_z) < 1e-6f &&
           std::abs(a.scale_x - b.scale_x) < 1e-6f && std::abs(a.scale_y - b.scale_y) < 1e-6f &&
           std::abs(a.scale_z - b.scale_z) < 1e-6f;
}

} // namespace

NF_TEST(gizmo_snap_rounds_translate_onto_the_grid) {
    // Accumulated 0.3 / 0.62 / -0.33 on a 0.25 grid -> 0.25 / 0.50 / -0.25.
    // Negatives must round symmetrically about zero, so a drag back through
    // the origin lands ON the origin, not on -step.
    Transform base{};
    const GizmoSnap snap{0.25f, 0.0f, 0.0f};
    const Transform out =
        apply_gizmo_delta(base, translate_delta(0.30f, 0.62f, -0.33f), GizmoMode::Translate,
                          GizmoSpace::World, snap);
    NF_CHECK_NEAR(out.local_x, 0.25f, 1e-6f);
    NF_CHECK_NEAR(out.local_y, 0.50f, 1e-6f);
    NF_CHECK_NEAR(out.local_z, -0.25f, 1e-6f);
}

NF_TEST(gizmo_unsnapped_translate_is_exact) {
    // The default GizmoSnap snaps nothing: a step of 0 means "off", and the
    // delta must survive at full precision (the viewport's live feedback and
    // the committed undo entry use the same path).
    Transform base{};
    base.local_x = 1.0f;
    base.local_z = -2.0f;
    const Transform out = apply_gizmo_delta(base, translate_delta(0.123f, 0.0f, 0.456f),
                                            GizmoMode::Translate, GizmoSpace::World);
    NF_CHECK_NEAR(out.local_x, 1.123f, 1e-6f);
    NF_CHECK_NEAR(out.local_y, 0.0f, 1e-6f);
    NF_CHECK_NEAR(out.local_z, -1.544f, 1e-6f);
}

NF_TEST(gizmo_snap_rounds_rotate_to_the_angle_step) {
    // 15-degree grid: +7 degrees snaps back to zero (the object holds still
    // between grid lines — this is the behaviour that makes snapping usable
    // with a fast pointer), +20 goes to +15, -40 goes to -45.
    Transform base{};
    const GizmoSnap snap{0.0f, 15.0f, 0.0f};
    const Transform held = apply_gizmo_delta(base, rotate_delta(7.0f, 0.0f), GizmoMode::Rotate,
                                             GizmoSpace::World, snap);
    NF_CHECK_NEAR(held.rot_y, 0.0f, kRotateTol);
    const Transform fwd = apply_gizmo_delta(base, rotate_delta(20.0f, 0.0f), GizmoMode::Rotate,
                                            GizmoSpace::World, snap);
    NF_CHECK_NEAR(fwd.rot_y, 15.0f, kRotateTol);
    const Transform back = apply_gizmo_delta(base, rotate_delta(0.0f, -40.0f), GizmoMode::Rotate,
                                             GizmoSpace::World, snap);
    NF_CHECK_NEAR(back.rot_x, -45.0f, kRotateTol);
}

NF_TEST(gizmo_snap_rounds_scale_on_a_factor_grid) {
    // Scale snaps the FACTOR (1 + dscale), never the delta: snapping the delta
    // would move the grid depending on where the gesture started.
    Transform base{};
    const GizmoSnap snap{0.0f, 0.0f, 0.25f};
    const Transform out = apply_gizmo_delta(base, scale_delta(0.30f), GizmoMode::Scale,
                                            GizmoSpace::World, snap);
    NF_CHECK_NEAR(out.scale_x, 1.25f, 1e-6f); // 1.30 -> 1.25
    NF_CHECK_NEAR(out.scale_y, 1.25f, 1e-6f);
    NF_CHECK_NEAR(out.scale_z, 1.25f, 1e-6f);
    // The factor grid is relative, so a scaled-up object lands on its own
    // multiples: 2.0 * 1.25 = 2.5.
    Transform big{};
    big.scale_x = big.scale_y = big.scale_z = 2.0f;
    const Transform big_out =
        apply_gizmo_delta(big, scale_delta(0.30f), GizmoMode::Scale, GizmoSpace::World, snap);
    NF_CHECK_NEAR(big_out.scale_x, 2.5f, 1e-6f);
}

NF_TEST(gizmo_scale_never_collapses_through_zero) {
    // A violent downward drag clamps at 1% rather than going negative or zero:
    // a negative scale is not a valid transform.
    Transform base{};
    base.scale_x = 2.0f;
    const Transform out = apply_gizmo_delta(base, scale_delta(-100.0f), GizmoMode::Scale,
                                            GizmoSpace::World);
    NF_CHECK_NEAR(out.scale_x, 0.02f, 1e-6f);
    NF_CHECK(out.scale_y > 0.0f);
    NF_CHECK(out.scale_z > 0.0f);
}

NF_TEST(gizmo_zero_rotate_delta_returns_the_base_untouched) {
    // The euler -> quat -> euler round trip is exact at zero, but only because
    // apply_gizmo_delta short-circuits: without it, -0.0 vs 0.0 or a 360 wrap
    // could slip in and a click-without-drag would push a phantom undo entry.
    Transform base{};
    base.rot_x = 30.0f;
    base.rot_y = 45.0f;
    base.rot_z = 10.0f;
    base.local_x = 3.0f;
    base.scale_x = 1.5f;
    const Transform out =
        apply_gizmo_delta(base, rotate_delta(0.0f, 0.0f), GizmoMode::Rotate, GizmoSpace::Local);
    NF_CHECK(transforms_equal(base, out));
}

NF_TEST(gizmo_world_translate_moves_along_camera_plane_axes) {
    // Unrotated object: a world delta lands on the same world axes.
    Transform base{};
    const Transform out = apply_gizmo_delta(base, translate_delta(1.0f, 2.0f, 3.0f),
                                            GizmoMode::Translate, GizmoSpace::World);
    NF_CHECK_NEAR(out.local_x, 1.0f, 1e-6f);
    NF_CHECK_NEAR(out.local_y, 2.0f, 1e-6f);
    NF_CHECK_NEAR(out.local_z, 3.0f, 1e-6f);
}

NF_TEST(gizmo_local_translate_moves_along_the_entitys_own_axes) {
    // Yawed +90 degrees: the object's local +Z points at world +X (verified
    // through the same quat_from_euler_xyz_degrees the gizmo uses), so a world
    // +X drag is a local +Z move. World space ignores the rotation entirely.
    Transform base{};
    base.rot_y = 90.0f;
    const Transform local_out = apply_gizmo_delta(base, translate_delta(1.0f, 0.0f, 0.0f),
                                                  GizmoMode::Translate, GizmoSpace::Local);
    NF_CHECK_NEAR(local_out.local_x, 0.0f, 1e-5f);
    NF_CHECK_NEAR(local_out.local_y, 0.0f, 1e-5f);
    NF_CHECK_NEAR(local_out.local_z, 1.0f, 1e-5f);
    const Transform world_out = apply_gizmo_delta(base, translate_delta(1.0f, 0.0f, 0.0f),
                                                  GizmoMode::Translate, GizmoSpace::World);
    NF_CHECK_NEAR(world_out.local_x, 1.0f, 1e-5f);
    NF_CHECK_NEAR(world_out.local_z, 0.0f, 1e-5f);
    // Round trip: dragging back the other way returns to the start.
    const Transform back =
        apply_gizmo_delta(local_out, translate_delta(-1.0f, 0.0f, 0.0f), GizmoMode::Translate,
                          GizmoSpace::Local);
    NF_CHECK_NEAR(back.local_x, 0.0f, 1e-5f);
    NF_CHECK_NEAR(back.local_z, 0.0f, 1e-5f);
}

NF_TEST(gizmo_world_and_local_rotate_compose_in_the_right_order) {
    // Pitched 30 degrees, then yaw +90. World space pre-multiplies the yaw
    // (it applies about the world Y axis); Local post-multiplies (about the
    // object's own yawed axis). The two must differ, and each must equal the
    // quaternion product it claims to be — compared as quaternions, since the
    // euler decomposition may report an equivalent triple.
    Transform base{};
    base.rot_x = 30.0f;
    const GizmoDelta d = rotate_delta(90.0f, 0.0f);
    const Quat q_old = quat_from_euler_xyz_degrees(base.rot_x, base.rot_y, base.rot_z);
    const Quat q_delta = quat_from_euler_xyz_degrees(d.pitch_deg, d.yaw_deg, 0.0f);

    const Transform world = apply_gizmo_delta(base, d, GizmoMode::Rotate, GizmoSpace::World);
    const Transform local = apply_gizmo_delta(base, d, GizmoMode::Rotate, GizmoSpace::Local);

    const Quat q_world = quat_from_euler_xyz_degrees(world.rot_x, world.rot_y, world.rot_z);
    const Quat q_local = quat_from_euler_xyz_degrees(local.rot_x, local.rot_y, local.rot_z);
    const Quat want_world = q_delta * q_old;
    const Quat want_local = q_old * q_delta;

    NF_CHECK(std::abs(q_world.dot(want_world) - 1.0f) < 1e-5f);
    NF_CHECK(std::abs(q_local.dot(want_local) - 1.0f) < 1e-5f);
    // Same axis-only case aside, the two spaces genuinely disagree...
    NF_CHECK(std::abs(q_world.dot(q_local) - 1.0f) > 1e-3f);
    // ...and a pure single-axis yaw composes identically in both (the sanity
    // check that the divergence above is composition order, not a bug).
    Transform yaw_base{};
    const Transform yaw_world =
        apply_gizmo_delta(yaw_base, d, GizmoMode::Rotate, GizmoSpace::World);
    const Transform yaw_local =
        apply_gizmo_delta(yaw_base, d, GizmoMode::Rotate, GizmoSpace::Local);
    NF_CHECK_NEAR(yaw_world.rot_y, 90.0f, 1e-5f);
    NF_CHECK_NEAR(yaw_local.rot_y, 90.0f, 1e-5f);
}

NF_TEST(gizmo_drag_commits_one_undo_step_for_one_entity) {
    // The single-entity path is the multi-select path with one entry: one
    // gesture still folds into exactly one undo step.
    Scene scene("GizmoOne");
    ecs::World& w = scene.world();
    const ecs::Entity e = add_box(w, 0.0f, 0.0f, 0.0f);
    CommandStack stack;

    GizmoDrag drag;
    std::string err;
    NF_CHECK(drag.begin(w, e, GizmoMode::Translate, GizmoSpace::World, err));
    drag.accumulate(translate_delta(0.5f, 0.0f, 0.0f));
    NF_CHECK(drag.live_apply(w));
    NF_CHECK_NEAR(w.get<Transform>(e)->local_x, 0.5f, 1e-6f);
    stack.push(drag.commit(w), w);
    NF_CHECK_EQ(stack.undo_size(), 1u);
    NF_CHECK(stack.undo_label().find("Move") != std::string::npos);
    NF_CHECK(w.get<Transform>(e) != nullptr);
    NF_CHECK_NEAR(w.get<Transform>(e)->local_x, 0.5f, 1e-6f);

    NF_CHECK(stack.undo(w));
    NF_CHECK_NEAR(w.get<Transform>(e)->local_x, 0.0f, 1e-6f);
    NF_CHECK(stack.redo(w));
    NF_CHECK_NEAR(w.get<Transform>(e)->local_x, 0.5f, 1e-6f);
}

NF_TEST(gizmo_multi_drag_folds_every_entity_into_one_undo_step) {
    // The P2 contract: shift-click three objects, drag, release — ONE undo
    // entry restores all three, not three entries.
    Scene scene("GizmoMulti");
    ecs::World& w = scene.world();
    const ecs::Entity a = add_box(w, 0.0f, 0.0f, 0.0f);
    const ecs::Entity b = add_box(w, 1.0f, 0.0f, 0.0f);
    const ecs::Entity c = add_box(w, 2.0f, 0.0f, 0.0f);
    CommandStack stack;

    GizmoDrag drag;
    std::string err;
    const std::vector<ecs::Entity> group{a, b, c};
    NF_CHECK(drag.begin(w, group, GizmoMode::Translate, GizmoSpace::World, GizmoSnap{}, err));
    drag.accumulate(translate_delta(0.0f, 1.0f, 0.0f));
    NF_CHECK(drag.live_apply(w));
    NF_CHECK_NEAR(w.get<Transform>(a)->local_y, 1.0f, 1e-6f);
    NF_CHECK_NEAR(w.get<Transform>(b)->local_y, 1.0f, 1e-6f);
    NF_CHECK_NEAR(w.get<Transform>(c)->local_y, 1.0f, 1e-6f);

    stack.push(drag.commit(w), w);
    NF_CHECK_EQ(stack.undo_size(), 1u);
    NF_CHECK_EQ(drag.entities().size(), 0u); // commit ends the gesture

    // Undo puts every entity back where it started.
    NF_CHECK(stack.undo(w));
    NF_CHECK_NEAR(w.get<Transform>(a)->local_y, 0.0f, 1e-6f);
    NF_CHECK_NEAR(w.get<Transform>(b)->local_y, 0.0f, 1e-6f);
    NF_CHECK_NEAR(w.get<Transform>(c)->local_y, 0.0f, 1e-6f);
    // Redo moves all three again.
    NF_CHECK(stack.redo(w));
    NF_CHECK_NEAR(w.get<Transform>(a)->local_y, 1.0f, 1e-6f);
    NF_CHECK_NEAR(w.get<Transform>(b)->local_y, 1.0f, 1e-6f);
    NF_CHECK_NEAR(w.get<Transform>(c)->local_y, 1.0f, 1e-6f);
}

NF_TEST(gizmo_multi_drag_survives_a_member_deleted_mid_gesture) {
    // The user deletes one of five selected objects while dragging. Undo must
    // still restore the survivors; the dead entry is skipped, never fatal.
    Scene scene("GizmoPartial");
    ecs::World& w = scene.world();
    const ecs::Entity a = add_box(w, 0.0f, 0.0f, 0.0f);
    const ecs::Entity b = add_box(w, 1.0f, 0.0f, 0.0f);
    const ecs::Entity c = add_box(w, 2.0f, 0.0f, 0.0f);
    CommandStack stack;

    GizmoDrag drag;
    std::string err;
    NF_CHECK(drag.begin(w, std::vector<ecs::Entity>{a, b, c}, GizmoMode::Translate,
                        GizmoSpace::World, GizmoSnap{}, err));
    drag.accumulate(translate_delta(0.0f, 0.0f, 4.0f));
    w.destroy_entity(b);
    stack.push(drag.commit(w), w);
    NF_CHECK_EQ(stack.undo_size(), 1u);

    NF_CHECK(stack.undo(w));
    NF_CHECK_NEAR(w.get<Transform>(a)->local_z, 0.0f, 1e-6f);
    NF_CHECK_NEAR(w.get<Transform>(c)->local_z, 0.0f, 1e-6f);
    NF_CHECK(stack.redo(w));
    NF_CHECK_NEAR(w.get<Transform>(a)->local_z, 4.0f, 1e-6f);
    NF_CHECK_NEAR(w.get<Transform>(c)->local_z, 4.0f, 1e-6f);
}

NF_TEST(gizmo_drag_skipping_transformless_entities_still_drags_the_rest) {
    // A group selection may include an entity without a Transform (a holder
    // the user ctrl-clicked by accident). begin() reports it and drags the
    // rest; only nothing-left-to-drag is fatal.
    Scene scene("GizmoSkip");
    ecs::World& w = scene.world();
    const ecs::Entity a = add_box(w, 0.0f, 0.0f, 0.0f);
    const ecs::Entity bare = w.create_entity(); // no Transform
    CommandStack stack;

    GizmoDrag drag;
    std::string err;
    NF_CHECK(drag.begin(w, std::vector<ecs::Entity>{a, bare}, GizmoMode::Translate,
                        GizmoSpace::World, GizmoSnap{}, err));
    NF_CHECK(!err.empty()); // names what it skipped
    drag.accumulate(translate_delta(2.0f, 0.0f, 0.0f));
    stack.push(drag.commit(w), w);
    NF_CHECK_EQ(stack.undo_size(), 1u);
    NF_CHECK_NEAR(w.get<Transform>(a)->local_x, 2.0f, 1e-6f);

    // An all-transformless selection cannot drag at all.
    std::string err2;
    NF_CHECK(!drag.begin(w, std::vector<ecs::Entity>{bare}, GizmoMode::Translate,
                         GizmoSpace::World, GizmoSnap{}, err2));
    NF_CHECK(!err2.empty());
}

NF_TEST(gizmo_snapped_drag_below_one_step_commits_nothing) {
    // A tiny gesture on a coarse grid never moves anything, so the undo stack
    // stays clean — same contract as a plain click.
    Scene scene("GizmoNoop");
    ecs::World& w = scene.world();
    const ecs::Entity e = add_box(w, 0.0f, 0.0f, 0.0f);
    CommandStack stack;

    GizmoDrag drag;
    std::string err;
    const GizmoSnap snap{0.5f, 0.0f, 0.0f};
    NF_CHECK(drag.begin(w, std::vector<ecs::Entity>{e}, GizmoMode::Translate, GizmoSpace::World,
                        snap, err));
    drag.accumulate(translate_delta(0.10f, 0.10f, 0.0f)); // rounds to zero
    NF_CHECK(drag.live_apply(w));
    NF_CHECK_NEAR(w.get<Transform>(e)->local_x, 0.0f, 1e-6f);
    NF_CHECK(drag.commit(w) == nullptr);
    NF_CHECK_EQ(stack.undo_size(), 0u);
}

NF_TEST(gizmo_abort_restores_every_dragged_entity) {
    // Escape (or a scene switch mid-drag) must leave no moved-without-undo
    // state behind, for the whole group.
    Scene scene("GizmoAbort");
    ecs::World& w = scene.world();
    const ecs::Entity a = add_box(w, 0.0f, 0.0f, 0.0f);
    const ecs::Entity b = add_box(w, 5.0f, 0.0f, 0.0f);
    CommandStack stack;

    GizmoDrag drag;
    std::string err;
    NF_CHECK(drag.begin(w, std::vector<ecs::Entity>{a, b}, GizmoMode::Rotate, GizmoSpace::World,
                        GizmoSnap{}, err));
    drag.accumulate(rotate_delta(45.0f, 0.0f));
    NF_CHECK(drag.live_apply(w));
    NF_CHECK(w.get<Transform>(a)->rot_y > 1.0f);
    NF_CHECK(drag.abort(w));
    NF_CHECK_NEAR(w.get<Transform>(a)->rot_y, 0.0f, 1e-6f);
    NF_CHECK_NEAR(w.get<Transform>(b)->rot_y, 0.0f, 1e-6f);
    NF_CHECK_EQ(stack.undo_size(), 0u);
}

NF_TEST(gizmo_command_label_names_the_mode_and_the_count) {
    // The undo menu reads this: a lone selection must not read like a batch.
    Scene scene("GizmoLabel");
    ecs::World& w = scene.world();
    const ecs::Entity a = add_box(w, 0.0f, 0.0f, 0.0f);
    const ecs::Entity b = add_box(w, 1.0f, 0.0f, 0.0f);
    const Transform before_a = *w.get<Transform>(a);
    const Transform before_b = *w.get<Transform>(b);
    Transform after_a = before_a;
    after_a.local_x = 3.0f;
    Transform after_b = before_b;
    after_b.local_x = 4.0f;

    std::vector<SetTransformsCommand::Entry> one{{a, before_a, after_a}};
    SetTransformsCommand single(std::move(one), "Move");
    NF_CHECK_EQ(single.label(), "Move entity");

    std::vector<SetTransformsCommand::Entry> two{{a, before_a, after_a}, {b, before_b, after_b}};
    SetTransformsCommand batch(std::move(two), "Rotate");
    NF_CHECK_EQ(batch.label(), "Rotate 2 entities");

    // apply/undo round-trip through the stack, same as the drag.
    CommandStack stack;
    stack.push(std::make_unique<SetTransformsCommand>(
                   std::vector<SetTransformsCommand::Entry>{{a, before_a, after_a},
                                                             {b, before_b, after_b}},
                   "Scale"),
               w);
    NF_CHECK_NEAR(w.get<Transform>(a)->local_x, 3.0f, 1e-6f);
    NF_CHECK_NEAR(w.get<Transform>(b)->local_x, 4.0f, 1e-6f);
    stack.undo(w);
    NF_CHECK_NEAR(w.get<Transform>(a)->local_x, 0.0f, 1e-6f);
    NF_CHECK_NEAR(w.get<Transform>(b)->local_x, 1.0f, 1e-6f);
}
