# GPU profiling

SANAD can read **real GPU time** with Vulkan timestamp queries: one duration
for the whole frame and, optionally, one duration per RenderGraph pass. There
are two consumers:

- the developer profiler, which is opt-in and reports numbers;
- GPU-aware dynamic resolution (Phase 40), where a scene explicitly asks the
  resolution governor to use the frame duration as its load signal.

With neither path active, no timestamps are recorded and no extra pipeline
drains are drawn. The scene path turns on **frame timing only**; the per-pass
breakdown and the report stay developer tools.

## Quick start

The editor enables both toggles before loading a scene and shows the result in
**Performance**:

- `GPU (timestamp)` — the last completed timed frame (one in-flight slot old)
- `GPU (avg)` — a decaying average in the editor UI
- `GPU passes` — the pass count, followed by one row per pass with its name and
  measured time

On a device without timestamp support, the editor keeps showing the previous
submit-to-fence estimate (`gpu_us` / `gpu_avg_us`) and says that timestamps are
unavailable.

For a packaged game, opt in explicitly:

```bat
NFPlayer.exe --project MyGame\MyGame.nfproj --gpu-timing
```

or:

```bat
set NF_PLAYER_GPU_TIMING=1
NFPlayer.exe --project MyGame\MyGame.nfproj
```

When the run ends, NFPlayer prints the last completed frame and its pass
breakdown. The flag is also the **display** switch: a scene may enable frame
timestamps for dynamic resolution without `--gpu-timing`, and the player still
will not print a report the user did not request.

A scene can opt into GPU-aware dynamic resolution on its PostProcess line:

```text
PostProcess: dynamic_resolution=true target_fps=60 dynamic_resolution_timing=gpu
```

That enables frame timestamps automatically for the governor, but leaves
per-pass timing off. Without the scene key the governor keeps using CPU frame
time, exactly as in Phase 38.

`NFPlayer --headless` does not render a frame, so it has no GPU sample to
consume; a GPU-timed governor falls back to CPU there. Run the player with its
window for a capture. The editor's headless mode still renders its offscreen
viewport and does report timestamps.

## Two toggles, deliberately separate

`Renderer3D` exposes:

```cpp
void set_gpu_timing_enabled(bool enabled);       // frame begin/end
void set_gpu_pass_timing_enabled(bool enabled);  // per-pass bracket
bool gpu_timing_supported() const;
bool gpu_timing_enabled() const;
bool gpu_pass_timing_enabled() const;
```

Frame timing costs two queries per frame. Per-pass timing costs two queries per
pass **and** ends each timed pass at `PipelineStage::BottomOfPipe`, which drains
the pipeline and serialises adjacent passes. That is useful for attribution and
unacceptable as a default in gameplay, which is why the toggles are separate.

At the runtime level there is one developer switch:

```cpp
runtime.set_gpu_profiling_enabled(true);
bool requested = runtime.gpu_profiling_enabled();
bool supported = runtime.gpu_profiling_supported();
```

It enables both renderer toggles and remembers the request. The request is
re-applied after every `Renderer3D::init()`, so loading a scene (which shuts
down and recreates renderer resources) cannot silently turn profiling off.
Scene-requested frame timing is a second, independent reason for the frame
toggle to be on; it never turns on the pass toggle.

## GPU-aware dynamic resolution (Phase 40)

The governor's configuration has a timing source:

```cpp
DynamicResolutionConfig config;
config.enabled = true;
config.timing = DynamicResolutionTiming::Gpu; // preference, not requirement
governor.configure(config);
governor.update(cpu_frame_s, gpu_frame_s, gpu_sample_valid);
```

The rules are:

- **Load** uses the GPU sample only when the source is GPU, the sample is
  marked valid, and its duration is finite and positive. Every other case uses
  the CPU frame time. A zero or NaN timestamp is "no sample", not "infinitely
  fast".
- **Cooldown** always advances on CPU wall time (falling back to the selected
  sample only when no wall time exists). Half a second of GPU work can span
  several seconds of real time; the cooldown must not inherit that mismatch.
- **Source changes reset** the average, the cooldown, and the sample flag, so
  CPU and GPU measurements are never mixed into one decision.
- **A sample is consumed once.** `Renderer3D::Stats::gpu_sample_serial` increases
  on every completed readback. `Runtime` only feeds the governor when the id
  changes, so an editor that updates more often than it renders cannot count the
  same completed frame repeatedly. With no new sample, the governor falls back
  to CPU.
- **Fallback is quiet and automatic.** A device without timestamps, a profiler
  that is off, or a frame that produced no result all keep the game running on
  the CPU clock. The log gets one warning per renderer lifetime when timestamps
  were explicitly required but unsupported.

The automatic activation is deliberately narrow: `dynamic_resolution=true` plus
`dynamic_resolution_timing=gpu` enables frame begin/end timestamps before the
renderer records the frame. It does **not** enable the per-pass bracket, does
**not** print a report, and does **not** replace `--gpu-timing`. The cost of the
frame path is two queries plus the frame-end `BottomOfPipe`; the per-pass path
is the expensive one because it drains at every pass boundary.

The scene key is a closed set. `cpu` is the default and is omitted by the writer;
`gpu` is written explicitly. Any other value is rejected by name with a warning,
so a typo cannot silently change the controller.

## Engine design

### RHI

- `IGraphicsDevice::create_timestamp_query_pool(count)` creates a timestamp-only
  pool. It returns null when the device has no support.
- `CommandBuffer::reset_query_pool(pool, first, count)` resets a range.
- `CommandBuffer::write_timestamp(pool, query, stage)` writes the GPU clock.
- `QueryPool::get_results(span, first)` uses
  `VK_QUERY_RESULT_WITH_AVAILABILITY_BIT` and **never blocks**. It returns false
  while any requested query is unavailable and leaves the output untouched.
- `timestamp_valid_bits()` and `timestamp_period_ns()` describe the device
  clock. Deltas are masked when fewer than 64 bits are valid.

### Renderer3D

Each of the two in-flight slots owns one 66-query pool:

```text
0                      frame begin
1, 2                   pass 0 begin, end
3, 4                   pass 1 begin, end
...
63, 64                 pass 31 begin, end
65                     frame end
```

At the start of a slot's next `render()` (after the caller has waited on that
slot's fence), the renderer polls the previous frame's results. This gives a
**one-frame-old sample** instead of stalling the current frame. The first frame
after enabling profiling has no result yet.

Only written queries are polled: frame begin, frame end, and the contiguous
range of pass pairs that were actually recorded. Polling the whole pool would
fail forever because unused pass queries stay reset and unavailable.

### RenderGraph

A timed pass writes its start timestamp immediately before `pass.execute` and
its end timestamp immediately after. The graph's own barriers are outside that
bracket, so a pass is charged for its draws, not for the transitions between
passes. `RenderGraph::reset()` clears the timing source, and the renderer
re-arms it after rebuilding the pass list.

## What the numbers mean

- **Sample latency:** the value belongs to the previous use of the same
  in-flight slot, not the frame currently being recorded.
- **Profiling overhead is included:** per-pass values include the
  `BottomOfPipe` drain. Compare a timed run with another timed run, not with an
  untimed frame.
- **Frame vs. sum of passes:** graph barriers, presentation, and work outside
  the pass brackets are not assigned to any pass, so the pass sum need not equal
  the frame duration exactly.
- **Unsupported devices:** `gpu_timing_supported()` is false, stats stay
  invalid, and callers keep their coarse fallback (for example the editor's
  submit-to-fence measurement).
- **Dynamic resolution:** the governor uses the frame duration, not a pass
  duration, and only a fresh serial. It is a GPU-load signal: a CPU-bound frame
  will not lower the scale just because the GPU finished early.

## Tests

`RHITests` covers:

- `timestamp_queries_measure_a_gpu_transfer` — a real timestamped transfer
- `rendergraph_brackets_passes_with_timestamps` — per-pass brackets
- `renderer_reports_real_gpu_frame_and_pass_timings` — three-frame integration,
  validity timing, pass count, pass-sum sanity, and a non-zero
  `gpu_sample_serial`
- `dynamic_resolution_uses_gpu_load_and_cpu_cooldown` — GPU load with wall-time
  cooldown (a GPU-based cooldown would step too early)
- `dynamic_resolution_falls_back_to_cpu_when_gpu_sample_is_missing` — no sample,
  zero, and NaN all use CPU
- `dynamic_resolution_ignores_gpu_time_when_configured_for_cpu` — the GPU clock
  must be asked for, not merely supplied

`RuntimeTests` adds `runtime_feeds_gpu_frame_time_to_dynamic_resolution`: a
scene key enables frame timestamps without profiling, a cheap GPU frame holds
scale 1.0 against a deliberately slow CPU `dt`, and updates without renders
eventually fall back to CPU because the serial did not change. `EditorTests`
and `UITests` cover the timing field, command validation, and localized labels.

Timestamp tests skip when the machine has no timestamp-capable GPU/queue. The
headless player does not render, so it cannot produce a sample; use a windowed
run for GPU evidence.
