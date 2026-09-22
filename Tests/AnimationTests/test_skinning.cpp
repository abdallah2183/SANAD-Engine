// AnimationTests — CPU skin matrix preparation: inverse bind, palette at
// rest = identity, animated palette maps bind-space points to world.
//
// These are the exact matrices a GPU skinning vertex shader consumes; the
// skinned render path itself is render-core (see COORDINATION.md, G4
// request). The CPU functions are the reference implementation.

#include <NF/Test/TestFramework.hpp>
#include <NF/Animation/Skeleton.hpp>
#include <NF/Animation/Skinning.hpp>

#include <cmath>

using namespace nf;
using namespace nf::animation;

namespace {

/// root -> hips -> head, hips offset (0,1,0) per level.
Skeleton make_three_bone() {
    Skeleton skel;
    skel.bones.resize(3);
    for (int i = 0; i < 3; ++i) {
        skel.bones[i].parent = (i == 0) ? -1 : i - 1;
        skel.bones[i].rest_translation = (i == 0) ? Vec3{0, 0, 0} : Vec3{0, 1, 0};
        skel.bones[i].rest_scale = {1, 1, 1};
    }
    skel.bones[0].name = "root";
    skel.bones[1].name = "hips";
    skel.bones[2].name = "head";
    return skel;
}

} // namespace

NF_TEST(skin_inverse_bind_of_rest_pose) {
    const Skeleton skel = make_three_bone();
    const std::vector<Mat4> inv = compute_inverse_bind_pose(skel);

    NF_CHECK_EQ(inv.size(), skel.bone_count());
    // Inverse bind of an identity rest pose is identity everywhere.
    for (usize i = 0; i < inv.size(); ++i) {
        NF_CHECK_NEAR(inv[i].m[0][0], 1.0f, 1e-6f);
        NF_CHECK_NEAR(inv[i].m[1][1], 1.0f, 1e-6f);
        NF_CHECK_NEAR(inv[i].m[2][2], 1.0f, 1e-6f);
        NF_CHECK_NEAR(inv[i].m[3][3], 1.0f, 1e-6f);
        NF_CHECK_NEAR(inv[i].m[3][0], 0.0f, 1e-6f);
    }
}

NF_TEST(skin_inverse_bind_of_offset_rest_pose) {
    Skeleton skel = make_three_bone();
    // Put the whole rig 5 m up: bind matrices must undo that offset.
    skel.bones[0].rest_translation = Vec3{0, 5, 0};

    const std::vector<Mat4> inv = compute_inverse_bind_pose(skel);
    // Inverse of the root's bind: translates (0,-5,0).
    NF_CHECK_NEAR(inv[0].m[3][1], -5.0f, 1e-5f);
    // Round trip: bind * inverse_bind == identity.
    std::vector<LocalPose> rest(skel.bone_count());
    for (usize i = 0; i < skel.bone_count(); ++i) {
        rest[i].translation = skel.bones[i].rest_translation;
        rest[i].scale = skel.bones[i].rest_scale;
    }
    std::vector<WorldPose> bind_world;
    compute_world_transforms(skel, rest, bind_world);
    for (usize i = 0; i < skel.bone_count(); ++i) {
        const Mat4 round_trip = bind_world[i].transform * inv[i];
        NF_CHECK_NEAR(round_trip.m[0][0], 1.0f, 1e-5f);
        NF_CHECK_NEAR(round_trip.m[3][3], 1.0f, 1e-5f);
    }
}

NF_TEST(skin_palette_at_rest_pose_is_identity) {
    const Skeleton skel = make_three_bone();

    std::vector<LocalPose> rest(skel.bone_count());
    for (usize i = 0; i < skel.bone_count(); ++i) {
        rest[i].translation = skel.bones[i].rest_translation;
        rest[i].scale = skel.bones[i].rest_scale;
    }
    std::vector<WorldPose> world;
    compute_world_transforms(skel, rest, world);

    const std::vector<Mat4> inv = compute_inverse_bind_pose(skel);
    std::vector<Mat4> palette;
    compute_skin_palette(skel, world, inv, palette);

    for (usize i = 0; i < palette.size(); ++i) {
        NF_CHECK_NEAR(palette[i].m[0][0], 1.0f, 1e-5f);
        NF_CHECK_NEAR(palette[i].m[1][1], 1.0f, 1e-5f);
        NF_CHECK_NEAR(palette[i].m[2][2], 1.0f, 1e-5f);
        NF_CHECK_NEAR(palette[i].m[3][0], 0.0f, 1e-5f);
        NF_CHECK_NEAR(palette[i].m[3][1], 0.0f, 1e-5f);
        NF_CHECK_NEAR(palette[i].m[3][2], 0.0f, 1e-5f);
    }
}

NF_TEST(skin_palette_carries_bind_space_points_to_world) {
    const Skeleton skel = make_three_bone();
    // Bind world: root (0,0,0), hips (0,1,0), head (0,2,0).

    // Animated pose: root lifted to y = 2, head rotated 90 deg about Z.
    // A bone's own local rotation rotates its local space (its children and
    // its skin), NOT its own offset: compute_world_transforms builds
    // local = S * R * T and composes local * parent, so the head's origin is
    // its unrotated (0,1,0) offset from hips at (0,3,0) -> (0,4,0). The
    // rotation instead carries the head's local +X onto +Y.
    std::vector<LocalPose> pose(skel.bone_count());
    for (usize i = 0; i < skel.bone_count(); ++i) {
        pose[i].translation = skel.bones[i].rest_translation;
        pose[i].scale = skel.bones[i].rest_scale;
    }
    pose[0].translation = Vec3{0, 2, 0};
    pose[2].rotation = Quat::from_axis_angle(Vec3{0, 0, 1}, HALF_PI);

    std::vector<WorldPose> world;
    compute_world_transforms(skel, pose, world);

    const std::vector<Mat4> inv = compute_inverse_bind_pose(skel);
    std::vector<Mat4> palette;
    compute_skin_palette(skel, world, inv, palette);

    // A vertex bound at the head's rest position IS the head origin; it must
    // follow the bone to its animated world position (0,4,0).
    const Vec3 p = palette[2].transform_point(Vec3{0, 2, 0});
    NF_CHECK_NEAR(p.x, 0.0f, 1e-4f);
    NF_CHECK_NEAR(p.y, 4.0f, 1e-4f);
    NF_CHECK_NEAR(p.z, 0.0f, 1e-4f);

    // A vertex bound 1 m along +X of the head's bind origin carries the
    // head's animated rotation: head-local (1,0,0) maps to (0,1,0), so the
    // point lands 1 m above the head origin at (0,5,0). This is the check
    // that catches a transposed palette (world * inverse_bind would land it
    // at (-2,2,0)).
    const Vec3 q = palette[2].transform_point(Vec3{1, 2, 0});
    NF_CHECK_NEAR(q.x, 0.0f, 1e-4f);
    NF_CHECK_NEAR(q.y, 5.0f, 1e-4f);
}
