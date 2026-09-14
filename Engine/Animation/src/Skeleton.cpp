#include <NF/Animation/Skeleton.hpp>

namespace nf::animation {

int32_t Skeleton::find_bone(const std::string& bone_name) const {
    for (usize i = 0; i < bones.size(); ++i) {
        if (bones[i].name == bone_name) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

void compute_world_transforms(const Skeleton& skel,
                              const std::vector<LocalPose>& local,
                              std::vector<WorldPose>& world) {
    const usize n = skel.bones.size();
    world.resize(n);

    for (usize i = 0; i < n; ++i) {
        const Bone& bone = skel.bones[i];
        const LocalPose& lp = (i < local.size()) ? local[i] : LocalPose{};

        // Build the local TRS. The engine's Mat4 uses row-vector convention
        // (v * M, confirmed by Vec4 operator*: v.x*m[0][0] + v.y*m[1][0]...),
        // so the correct TRS order is S * R * T — first scale, then rotate,
        // then translate — and the parent composition is local * parent_world.
        Mat4 s = Mat4::scale(lp.scale);
        Mat4 r = lp.rotation.to_matrix();
        Mat4 t = Mat4::translate(lp.translation);
        Mat4 m = s * r * t;

        if (bone.parent >= 0 && static_cast<usize>(bone.parent) < n) {
            world[i].transform = m * world[static_cast<usize>(bone.parent)].transform;
        } else {
            world[i].transform = m;
        }
    }
}

Skeleton make_default_skeleton() {
    Skeleton skel;
    Bone root;
    root.name = "root";
    root.parent = -1;
    root.rest_translation = {0.0f, 0.0f, 0.0f};
    root.rest_rotation = Quat::identity();
    root.rest_scale = {1.0f, 1.0f, 1.0f};
    skel.bones.push_back(std::move(root));
    return skel;
}

} // namespace nf::animation
