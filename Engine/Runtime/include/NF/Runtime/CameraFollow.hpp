#pragma once

// NF/Runtime/CameraFollow.hpp — the follow camera, as maths.
//
// A third-person camera is three decisions: WHERE it looks (the pivot on
// the target), HOW FAR it sits and from which angle (the orbit), and HOW
// IT MOVES when the target moves (smoothing). The first two are geometry.
// The third is a decision about feel: a camera that snaps is a camera the
// player cannot look away from, and a camera that lags is a camera the
// player cannot aim.
//
// The maths lives here, free of the ECS, the physics world and the frame
// loop, so it can be tested on its own: a camera bug is a maths bug, and a
// maths bug is the easiest kind to pin down.

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include <string>

namespace nf::runtime {

/// How the camera follows. Mirrors the fields of the scene's CameraComponent
/// so the runtime can copy one into the other without a translation layer.
struct FollowCameraSettings {
    /// The name of the entity the camera follows. Empty = no follow: the
    /// camera is wherever its own Transform says, looking at the origin.
    ///
    /// A NAME and not a handle, because the scene file has no way to point
    /// at another entity yet, and a handle that resolves to nothing at load
    /// time is a silent no-op. A name that matches nothing is handled the
    /// same way -- the camera holds its last position instead of snapping.
    std::string target_name;

    /// The pivot, in the TARGET'S local space: the point the camera orbits
    /// and looks at. (0, 1.6, 0) is roughly a character's head.
    float pivot_x = 0.0f;
    float pivot_y = 1.6f;
    float pivot_z = 0.0f;

    /// How far the camera sits from the pivot.
    float distance = 5.0f;

    /// The orbit angles, in degrees. yaw is the horizontal rotation (0 = the
    /// camera is behind the target, looking along the target's forward
    /// axis); pitch is the vertical one (negative = looking down).
    float yaw = 0.0f;
    float pitch = -15.0f;

    /// How fast the camera catches up, in units of the remaining distance per
    /// second. 0 means "no smoothing": the camera is exactly where the
    /// geometry says, every frame.
    float smoothing = 8.0f;

    /// The radius of the camera's collision sphere, in metres. When
    /// something solid is between the pivot and the camera, the camera is
    /// pulled in front of it by this much, so it never ends up inside a
    /// tree.
    float collision_radius = 0.35f;

    /// Whether the camera pulls in when something is in the way.
    bool avoid_collision = true;
};

/// The camera's state between frames: where it is, where it looks, and
/// whether it has ever been placed.
struct FollowCameraState {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 target{0.0f, 0.0f, 0.0f};
    bool initialised = false;
};

/// The direction the camera looks from, for a given orbit.
///
/// yaw is measured around the world Y axis and pitch around the camera's
/// right axis. The result is a UNIT vector: the caller scales it by the
/// distance.
Vec3 follow_camera_direction(float yaw_deg, float pitch_deg);

/// Where the camera sits for a given pivot and orbit.
///
/// The pivot is the point the camera looks at; the camera sits `distance`
/// away, in the direction the orbit says. A distance of zero (or less)
/// puts the camera ON the pivot, which is degenerate but not an error: the
/// caller decides what to do with it.
Vec3 follow_camera_position(const Vec3& pivot, float yaw_deg, float pitch_deg, float distance);

/// The fraction of the remaining distance to cover in `dt` seconds.
///
/// Exponential, and therefore frame-rate independent: at 30 fps and at 240
/// fps the camera covers the same fraction of the gap per second, so a
/// scene does not change feel with the frame rate. A linear blend would
/// make the camera twice as fast on a fast machine.
float follow_camera_blend(float smoothing, float dt);

/// The camera's position when something solid is in the way.
///
/// `hit_distance` is how far the cast travelled before it hit something, or
/// a negative number when nothing was hit. The camera is placed just in
/// front of the hit, and never closer than `min_distance` to the pivot: a
/// camera inside the character's head is worse than one that clips a wall.
Vec3 follow_camera_avoid(const Vec3& pivot, const Vec3& desired, float hit_distance,
                         float radius, float min_distance);

} // namespace nf::runtime
