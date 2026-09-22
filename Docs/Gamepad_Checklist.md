# Gamepad Manual Checklist (G1)

Xbox-compatible XInput pad on Windows x64. Automated coverage lives in
`Tests/InputTests` (edge/hold/release, XInput mask, sticks, disconnect);
this file is the live hardware path only.

## Prerequisites

- Windows PC with Vulkan-capable GPU
- Built `NFSampleVehicleDemo.exe` (`bash .workbuddy-ai/nfb.sh --target NFSampleVehicleDemo`)
- Xbox pad (or XInput-compatible) connected before or during the run
- `xinput1_4.dll` / `xinput1_3.dll` / `xinput9_1_0.dll` present (default on Win10+)

## Pass criteria

| # | Step | Expected |
|---|------|----------|
| 1 | Connect pad, launch demo | Title shows pad hints after first poll; no crash if pad absent |
| 2 | RT / W | Throttle accelerates; km/h rises |
| 3 | LT / S | Brake / reverse slows then backs up |
| 4 | Left stick / A-D | Steer left/right; stick past deadzone (~0.15) feels responsive |
| 5 | A / Space | Handbrake: rear wheels lock, car slides |
| 6 | Y or Start / R | Reset to spawn; Resets counter increments |
| 7 | Back / Escape | Clean exit (score logged) |
| 8 | Unplug mid-drive | Inputs release; car coasts; no stuck throttle |
| 9 | Replug | Pad drives again without relaunch |
| 10 | Keyboard alone (no pad) | WASD path unchanged |

## Notes

- Guide button has no XInput bit; not bound.
- Deadzones owned by `InputMapper` (`GamepadSettings`), not the raw poll.
- A stick pushed to the physical limit must command the **full** action value
  (steer/throttle = 1.0). The radial deadzone rescales `[deadzone, 1]` onto
  `[0, 1]`, so topping out below full travel is a fault, not a tuning choice.
- XInput polling happens in `Window::poll_events` (after the message pump), so
  pad state lands in the same frame as key events. `poll_gamepad()` is a no-op
  when no XInput DLL/pad is present — the demo must still run on keyboard.
- Sample edits are read+drive only for G1; routing issues → COORDINATION Requests.

## Result log

| Date | Pad | Build | Tester | Result |
|------|-----|-------|--------|--------|
| | | | | |
