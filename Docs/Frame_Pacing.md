# Frame pacing telemetry

`nf::rendering::FramePacing` is a rolling window over recent frames. It answers
three questions that a single frame cannot:

- What was the sustained frame rate?
- How bad were the slow frames (the ones a player remembers)?
- Was the frame limited by the GPU, or by everything on the CPU side of the
  presentation boundary?

It is **measurement only**. It does not sleep, wait on a fence, or change render
scale. Dynamic resolution still owns its own governor and its own smoothed,
clamped sample; this window is a diagnostic view and never feeds the governor.

## Where the samples come from

The host loop pushes one `push_frame(cpu_seconds, gpu_seconds, gpu_valid)` per
frame:

- `cpu_seconds` is **raw wall time**. It is not the simulation's clamped `dt`,
  because a 2-second debugger pause or scene load is exactly the kind of frame
  this window exists to expose. The player takes it from its frame clock before
  `runtime.update(dt)` clamps the value; the editor takes it from the editor
  frame clock.
- `gpu_seconds` is optional. It is the last completed timestamp sample from
  `Renderer3D`, which is already one in-flight slot old. The GPU duration is only
  stored when a new `gpu_sample_serial` is observed, so two reads of the same
  sample do not count twice.
- `gpu_valid` is false when timestamps are disabled, unsupported, or no new
  sample has arrived. The CPU frame is still recorded.

GPU samples live in the same ring as the CPU samples. A device that stops
reporting timestamps therefore ages out of the window; the bound becomes
`Unknown` instead of keeping an old verdict forever.

## The window

The default window is 120 frames (about two seconds at 60 fps); callers can
configure it from 8 to 1200. Every number describes only the frames currently in
that window. A hitch is visible for the next window, then drops out.

| Value | Definition |
|:--|:--|
| Average frame | Arithmetic mean of the window, in milliseconds. |
| Average FPS | `1000 / average frame ms`. |
| 1% low | Mean of the slowest `max(1, ceil(N / 100))` frames. For 120 frames that is the two slowest; for 100 frames it is the single slowest. |
| 1% low FPS | `1000 / 1% low frame ms`. |
| Jitter | Population standard deviation of the frame times in the window. Variance is also exposed. |
| Min / max | Fastest and slowest frame in the window. |
| GPU average | Mean of the paired GPU samples only. |
| Bound | `GPU avg / CPU wall avg`: at least 0.90 is **GPU**, at most 0.75 is **CPU**, otherwise **Balanced**. Needs at least 8 paired GPU samples; before that it is **Unknown**. |

The 1% low is deliberately not the single worst frame. One breakpoint should
not define a session forever, and it is not an interpolated percentile.

## CPU includes presentation waiting

A frame that waits for vsync is not doing CPU work, but the wall clock still
contains that wait. The bound verdict is operational: when GPU time is close to
the whole frame, the GPU is the constraint; when it is much smaller, the frame is
limited by the CPU side, which includes presentation/vsync waiting. The editor
hint says this out loud. A future busy-time counter could separate real CPU work
from the wait, but it would be a different measurement, not this ratio.

## Editor

The **Performance** section shows the instantaneous FPS/frame row first, then the
rolling window:

- Average FPS
- 1% low FPS
- Frame jitter (standard deviation, ms)
- Bottleneck (`CPU`, `GPU`, `Balanced`, or `Unknown`)

The window resets when a scene is opened or created, and when play mode changes.
That keeps an authored-still scene from describing the first two seconds of
gameplay, and vice versa. The host sees every scene door (Ctrl+N, the menu, the
toolbar) through the monotonic `EditorApp::scene_serial()`; ordinary edits and
saves do not advance it. The editor enables frame timestamps by default, so the
bound normally has enough samples immediately.

## Player

`NFPlayer --frame-stats` (or `NF_PLAYER_FRAME_STATS=1`) prints the window at exit:

```bat
build\Release\bin\NFPlayer.exe --project Games\TreeWorld\TreeWorld.nfproj ^
    --frames 300 --frame-stats --gpu-timing
```

Add `--gpu-timing` for the GPU average and the bottleneck verdict. Without it the
player reports only the CPU wall-clock metrics and leaves the bound `Unknown`;
it does not silently start GPU measurements just because frame stats were
requested. The two switches stay independent:

- `--frame-stats`: CPU pacing, average, 1% low, jitter.
- `--gpu-timing`: real GPU frame and per-pass durations.

A scene that uses GPU-aware dynamic resolution may enable frame timestamps
internally, but the player still prints GPU timing only when explicitly asked.

## Headless and other limits

- Headless runs do not render; they update and sleep about 16 ms per frame. Their
  CPU numbers describe that harness, not GPU performance, and the bound is
  `Unknown`.
- A frame dropped by a window resize or a paused debugger is measured honestly.
  Reset the window after a known non-frame cost if it would distort a benchmark.
- The GPU duration is sampled one in-flight slot late and may be paired with the
  wall time of the frame that just completed. Average and bound are not sensitive
  to that one-frame phase; it is not intended as per-frame causality.
- Per-pass timestamping inserts `BottomOfPipe` drains. Use it for attribution,
  not as a free measurement of an untimed frame. See
  [GPU profiling](GPU_Profiling.md).

## Tests

`Tests/RHITests/test_frame_pacing.cpp` pins the pure arithmetic without a GPU:
empty window, average/FPS, the exact 1% slice at 100 and 120 frames, population
variance/stddev, ring wrap, invalid CPU/GPU samples, GPU-sample ageing, bound
thresholds and the minimum sample count, window clamping, and reset.
`Tests/EditorTests/test_save_load.cpp` pins the host-side reset signal: opening
or creating a scene advances `EditorApp::scene_serial()`, while editing and
`save_copy` do not.

The player's windowed acceptance run proves the wiring: `--frame-stats` prints a
window, and adding `--gpu-timing` adds a GPU average and a non-`Unknown` bound
when the device supports timestamps.

In the Release validation for this phase, `RHITests` passed 338/338 (including
the 12 tests above), `RuntimeTests` 215/215, `EditorTests` 283/283, and
`UITests` 45/45. Two windowed TreeWorld runs (300 frames, `--validation`) exited
0 with `Validation errors: 0`: the CPU-only run printed the window with bound
`unknown`, and the run with `--gpu-timing` printed `GPU frame time: 0.31 ms
average across 120 samples` with bound `cpu`. A windowed Arabic editor run (90
frames, `--validation`) exited 0 with `Validation errors: 0` and displayed the new
Performance rows. Those numbers are machine-specific and vary between runs (the
1% low is designed to expose that); the independent switches and the log shape
are the contract.
