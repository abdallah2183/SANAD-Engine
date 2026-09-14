#pragma once

#include <NF/Animation/AnimationPlayer.hpp>
#include <NF/Animation/AnimationStateMachine.hpp>
#include <NF/Animation/Skeleton.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace nf::animation {

/// ECS component for skeletal animation. Stores the skeleton, a clip table
/// (by name), the player, and the optional state machine. The runtime steps
/// the player and/or state machine each frame and writes the world-space pose
/// to the entity's Transform hierarchy.
struct AnimationComponent {
    Skeleton skeleton;
    std::unordered_map<std::string, AnimationClip> clips;
    AnimationPlayer player;
    AnimationStateMachine state_machine;
    bool use_state_machine = false; // false = direct player, true = state machine
    f32 speed = 1.0f;
    bool paused = false;

    /// Last computed local pose (for debugging / debug draw).
    std::vector<LocalPose> last_local_pose;
    /// Last computed world pose (for the render path to consume).
    std::vector<WorldPose> last_world_pose;

    /// The procedural spec `procedural_clip_name` was generated from, if any.
    ///
    /// The clip in `clips` is built at load time rather than read from disk, so
    /// this spec is the only record of what it was. Without it a save/load
    /// round trip would reload the scene with an empty clip and the entity
    /// would quietly stop moving — which is exactly the failure this phase is
    /// fixing, so it must not be reintroduced through the save path.
    bool has_procedural = false;
    ProceduralClipSpec procedural;
    std::string procedural_clip_name;

    /// The entity placement captured the first time the animation ran.
    ///
    /// The pose is applied as an *offset* from this rather than replacing the
    /// transform, because the root bone sits at the rig's origin: overwriting
    /// would snap the entity to the origin on frame one and lose wherever the
    /// scene placed it. `Runtime::rebase_animation` refreshes it after a
    /// hand-edit, since the animation otherwise owns the transform.
    bool has_base_transform = false;
    Vec3 base_translation;
    Quat base_rotation = Quat::identity();
    Vec3 base_scale = {1.0f, 1.0f, 1.0f};
};

} // namespace nf::animation
