#pragma once

// NF/Runtime/PlayerLook.hpp — aiming the camera, as maths.
//
// A follow camera that only obeys its authored yaw is a camera the player
// watches. A follow camera the player turns is a game. The difference is
// not a device: it is four small decisions, and all four are geometry.
//
//   1. WHAT A MOUSE PIXEL MEANS. A mouse reports displacement, not an angle,
//      so something has to say how many degrees a pixel is worth. That
//      number is a taste, and it belongs in the scene, not in the source.
//   2. WHAT A STICK MEANS. A stick reports a deflection held over time, so
//      the same taste has to be expressed per SECOND. Applying the mouse
//      number to a stick makes the camera crawl; applying the stick number
//      to a mouse makes it spin.
//   3. HOW FAR IT MAY LOOK UP OR DOWN. A camera that can pitch past
//      vertical rolls over and shows the world upside down. The clamp is
//      not a limitation; it is what makes the orbit an orbit.
//   4. WHICH WAY "FORWARD" IS. On a keyboard, W is a direction on the
//      screen, not a direction in the world. Once the player can turn the
//      camera, W has to mean "away from the camera" — so the movement
//      vector has to be rotated by the camera's yaw before it reaches the
//      character.
//
// Everything here is free of the ECS, the window and the frame loop, for
// the same reason CameraFollow.hpp is: a camera bug is a maths bug, and a
// maths bug is the easiest kind to pin down. The runtime calls these
// functions; the tests call the same ones.

#include <NF/Core/Types.hpp>

namespace nf::runtime {

/// How the player's hands turn into camera angles.
///
/// Mirrors the fields of the scene's CameraComponent, so the runtime copies
/// one into the other without a translation layer — the same arrangement
/// FollowCameraSettings has.
struct LookSettings {
    /// Degrees of yaw per pixel of mouse travel. 0.12 is the usual
    /// third-person default: a full 1920-pixel sweep is ~230 degrees, so a
    /// flick of the wrist turns the camera about a quarter turn.
    f32 mouse_sensitivity = 0.12f;

    /// Degrees per SECOND at full stick deflection. Slower than the mouse
    /// because it is held, not flicked: 180 deg/s is a half turn in a
    /// second, which is fast enough to chase something and slow enough to
    /// aim with.
    f32 stick_sensitivity = 180.0f;

    /// The vertical clamp, in degrees. NEGATIVE looks down (the camera rises
    /// above the target), positive looks up. The defaults stop just short of
    /// straight down and well short of straight up: a camera that reaches
    /// the pole gimbals, and a camera that looks at the sky from below the
    /// ground sees the inside of the terrain.
    f32 pitch_min = -80.0f;
    f32 pitch_max = 60.0f;

    /// Flip the vertical axis. Some players grew up with it; the scene says
    /// which kind of player it is for.
    bool invert_y = false;
};

/// The two angles that aim the camera, in degrees.
///
/// yaw is measured around world +Y and 0 puts the camera on the +Z side of
/// the target, looking towards -Z. pitch is negative when the camera sits
/// above the target and looks down — the convention CameraFollow.hpp
/// already uses, and the one every third-person game uses.
struct LookState {
    f32 yaw = 0.0f;
    f32 pitch = -15.0f;
};

/// Wrap an angle into (-180, 180].
///
/// Unbounded yaw accumulates: after ten turns it is 3600, and every
/// comparison against a scene-authored angle (auto-align, a script asking
/// which way the camera faces) has to unwrap it first. Wrapping at the
/// source means there is one representation instead of two.
f32 look_wrap_deg(f32 degrees);

/// The shortest signed rotation that takes `from` to `to`, in (-180, 180].
///
/// Positive means "turn left" under the yaw convention above (increasing
/// yaw). Used by auto-align, where turning 350 degrees the wrong way is the
/// difference between a camera that settles and one that spins.
f32 look_angle_delta(f32 from, f32 to);

/// Apply a mouse displacement, in pixels, to the orbit.
///
/// The mouse reports "how far did the pointer move since the last frame".
/// Moving it right turns the camera right, which — because yaw increases
/// counter-clockwise — DECREASES yaw. Moving it down looks down, which
/// makes pitch more negative. `invert_y` flips the second one and only the
/// second one: inverting the horizontal axis is not a preference, it is a
/// bug.
LookState look_apply_mouse(LookState state, f32 dx_pixels, f32 dy_pixels,
                           const LookSettings& settings);

/// Apply a stick deflection held for `dt` seconds to the orbit.
///
/// `sx` and `sy` are in [-1, 1] with +x to the right and +y DOWN, which is
/// the direction a right stick reads on every pad. The rotation is
/// `deflection * sensitivity * dt`, so the camera turns at a constant rate
/// while the stick is held, whatever the frame rate.
LookState look_apply_stick(LookState state, f32 sx, f32 sy, f32 dt,
                           const LookSettings& settings);

/// Clamp pitch into the settings' range and wrap yaw.
///
/// Called after every input so the invariant "the orbit is always legal"
/// holds for the code that reads it, rather than being re-derived there.
LookState look_clamp(LookState state, const LookSettings& settings);

/// The yaw that puts the camera BEHIND something moving along (x, z).
///
/// The camera looks along `-direction(yaw)`, so the camera is behind a
/// movement direction when its view direction equals it:
/// `(-sin yaw, -cos yaw) == normalize(x, z)`, hence `yaw = atan2(-x, -z)`.
/// A zero-length direction has no answer and returns `fallback`.
f32 look_yaw_for_direction(f32 dir_x, f32 dir_z, f32 fallback);

/// Rotate `yaw` towards `desired` at `rate` degrees per second.
///
/// This is the auto-align used when the player runs without touching the
/// mouse: the camera drifts back behind the character instead of staying
/// where it was left. `rate <= 0` disables it and returns `yaw` unchanged,
/// which is the behaviour of every scene authored before this existed.
f32 look_align(f32 yaw, f32 desired, f32 rate, f32 dt);

/// The follow distance after a wheel notch.
///
/// `wheel` is positive when the wheel rolls forward (the conventional
/// "zoom in"). One notch moves the camera by `step` metres, and the result
/// is clamped: a distance of zero puts the camera inside the character's
/// head, and an unbounded one puts it outside the far plane.
f32 follow_zoom(f32 distance, f32 wheel, f32 step, f32 min_distance, f32 max_distance);

/// Rotate a movement input by the camera's yaw, so W goes where the player
/// is looking.
///
/// `move_x` is +1 for right and `move_z` is +1 for BACK, which is the
/// convention `nf.axis("move_z")` already reports (W gives -1). The output
/// is a world-space XZ direction of the same length: the rotation is
/// length-preserving, so a normalised input stays normalised and a
/// diagonal does not become faster than a straight line.
void move_relative(f32 move_x, f32 move_z, f32 camera_yaw_deg, f32& out_x, f32& out_z);

/// The stick deadzone, applied to a PAIR of axes.
///
/// A stick that rests at 0.02 must not move the player, and a circular
/// deadzone per axis would still let a resting stick creep diagonally. The
/// magnitude is what is tested, and the surviving magnitude is rescaled so
/// the first millimetre past the deadzone is slow rather than a jump from
/// nothing to `deadzone` — the rescale is the difference between a stick
/// that feels analogue and one that feels like four buttons.
///
/// The deadzone is clamped to [0, 0.95]: at 1.0 nothing could ever pass it.
void stick_deadzone(f32& x, f32& y, f32 deadzone);

} // namespace nf::runtime
