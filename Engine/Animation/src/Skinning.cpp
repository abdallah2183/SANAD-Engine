// NF/Animation/Skinning.cpp — CPU skin-matrix preparation.

#include <NF/Animation/Skinning.hpp>

namespace nf::animation {

std::vector<Mat4> compute_inverse_bind_pose(const Skeleton& skel) {
    // The bind pose is the rest pose: build rest locals, walk the hierarchy,
    // invert each world matrix.
    std::vector<LocalPose> rest(skel.bones.size());
    for (usize i = 0; i < skel.bones.size(); ++i) {
        rest[i].translation = skel.bones[i].rest_translation;
        rest[i].rotation = skel.bones[i].rest_rotation;
        rest[i].scale = skel.bones[i].rest_scale;
    }

    std::vector<WorldPose> bind_world;
    compute_world_transforms(skel, rest, bind_world);

    std::vector<Mat4> inverse_bind(skel.bones.size());
    for (usize i = 0; i < skel.bones.size(); ++i) {
        inverse_bind[i] = bind_world[i].transform.inverse();
    }
    return inverse_bind;
}

void compute_skin_palette(const Skeleton& skel,
                          const std::vector<WorldPose>& world,
                          const std::vector<Mat4>& inverse_bind,
                          std::vector<Mat4>& out_palette) {
    const usize n = skel.bones.size();
    out_palette.resize(n);
    for (usize i = 0; i < n; ++i) {
        // Row-vector convention (v' = v * M): a bind-space vertex is first
        // un-bound into bone-local space, then carried to world, so the
        // palette composes as inverse_bind * world (NOT world * inverse_bind,
        // which is the column-vector order).
        const Mat4& w = (i < world.size()) ? world[i].transform : Mat4::identity();
        const Mat4& ib = (i < inverse_bind.size()) ? inverse_bind[i] : Mat4::identity();
        out_palette[i] = ib * w;
    }
}

} // namespace nf::animation
