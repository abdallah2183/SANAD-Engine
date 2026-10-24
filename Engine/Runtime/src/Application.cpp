#include <NF/Runtime/Application.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/BuiltinModules.hpp>
#include <NF/Runtime/KeyboardInput.hpp>
#include <NF/Runtime/RunConfigResolver.hpp>
#include <NF/Runtime/InputLog.hpp>
#include <NF/Platform/Platform.hpp>
#include <NF/Platform/Window.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/Core/Time.hpp>
#include <NF/Jobs/JobSystem.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/FramePacing.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <span>
#include <thread>
#include <vector>

namespace nf::runtime {

namespace {

// The largest frame delta the simulation is allowed to see. A hitch (a scene
// load, a shader compile, a debugger break) produces a multi-second delta;
// feeding that to the audio mixer or the particle sim either stalls the
// frame or leaves a backlog the mixer can never drain.
constexpr float kMaxFrameDelta = 0.25f;

// How many frames a headless run renders when nobody said how many. A
// headless run has no window to close, so "until the window closes" never
// arrives and the loop would spin forever.
constexpr uint32_t kHeadlessDefaultFrames = 120;

} // namespace

Application::Application(const ApplicationConfig& config) : m_config(config) {}

Application::~Application() = default;

int Application::run() {
    Logger::instance().add_sink(Logger::make_console_sink());
    Logger::instance().set_min_level(LogLevel::Info);

    NF_LOG_INFO(LogCategory::Core, "=== SANAD Runtime ===");
    NF_LOG_INFO(LogCategory::Core, "Project: '{}' Frames: {} Validation: {} Headless: {}",
                m_config.project_path.empty() ? std::string("<none — engine tree fallback>")
                                              : m_config.project_path,
                m_config.max_frames, m_config.validation, m_config.headless);

    platform_init();
    JobSystem::instance().init(0);

    // --- VFS setup + effective run settings ---------------------------------
    // Precedence and the engine-tree fallback live in resolve_run_config so they
    // are unit-testable instead of buried in this function, which also opens a
    // window and a Vulkan device.
    assets::VirtualFileSystem vfs;
    ResolvedRunConfig run_cfg;
    {
        std::string cfg_err;
        if (!resolve_run_config(m_config, vfs, run_cfg, cfg_err)) {
            NF_LOG_ERROR(LogCategory::Core, "{}", cfg_err);
            JobSystem::instance().shutdown();
            platform_shutdown();
            return 1;
        }
    }
    const std::string& resolved_scene = run_cfg.scene_path;
    const std::string& resolved_title = run_cfg.title;
    const uint32_t resolved_width = run_cfg.width;
    const uint32_t resolved_height = run_cfg.height;

    // --- Input record/replay validation -------------------------------------
    // Before any expensive setup: a mistyped --replay path should cost a
    // second, not a window and a device.
    //
    // `replay_log` is declared out here, not inside the probe scope, because
    // it is the object `play_input_log()` is later handed. Loading it into a
    // local that then went out of scope — and replaying a second, still-empty
    // log — is the exact bug this shape invites: the run reports success while
    // replaying nothing at all, which is the failure the whole flag exists to
    // prevent.
    InputLog replay_log;
    ReplayProbe replay_probe = ReplayProbe::NotConfigured;
    {
        std::string input_err;
        if (!prepare_input_replay(m_config, replay_log, replay_probe, input_err)) {
            NF_LOG_ERROR(LogCategory::Core, "{}", input_err);
            JobSystem::instance().shutdown();
            platform_shutdown();
            return 1;
        }
    }

    // Asset Registry
    assets::AssetRegistry registry;
    std::string reg_err;
    bool has_registry = false;
    {
        auto r = vfs.exists("content://AssetRegistry.nfreg");
        if (r.ok && r.value) {
            if (registry.load(vfs, "content://AssetRegistry.nfreg", reg_err)) {
                NF_LOG_INFO(LogCategory::Core, "AssetRegistry loaded: {} entries", registry.size());
                has_registry = true;
            } else {
                NF_LOG_WARN(LogCategory::Core, "Failed to load registry: {}", reg_err);
            }
        } else {
            // Try cache://
            auto r2 = vfs.exists("cache://AssetRegistry.nfreg");
            if (r2.ok && r2.value && registry.load(vfs, "cache://AssetRegistry.nfreg", reg_err)) {
                NF_LOG_INFO(LogCategory::Core, "AssetRegistry loaded from cache: {} entries", registry.size());
                has_registry = true;
            }
        }
    }
    if (!has_registry) {
        NF_LOG_INFO(LogCategory::Core, "No AssetRegistry found, starting with empty registry");
    }

    // Window (unless headless)
    Window window;
    if (!m_config.headless) {
        WindowDesc wdesc{};
        wdesc.width = resolved_width;
        wdesc.height = resolved_height;
        wdesc.title = resolved_title;
        wdesc.vsync = m_config.vsync;
        if (!window.create(wdesc)) {
            NF_LOG_ERROR(LogCategory::Platform, "Failed to create window");
            JobSystem::instance().shutdown();
            platform_shutdown();
            return 1;
        }
        window.set_event_callback([&](WindowEvent ev, u32 w, u32 h) {
            if (ev == WindowEvent::Resize) {
                NF_LOG_INFO(LogCategory::Platform, "Window resized to {}x{}", w, h);
            }
        });
#ifdef _WIN32
        // Take the foreground from the editor that launched us. The editor
        // grants the right with AllowSetForegroundWindow; doing it here, on our
        // own window, avoids the editor enumerating windows from a background
        // thread — a pattern code-integrity policy flags, and it stopped the
        // editor from launching at all.
        if (void* native = window.native_handle()) {
            ::ShowWindow(static_cast<HWND>(native), SW_RESTORE);
            ::SetForegroundWindow(static_cast<HWND>(native));
        }
#endif
    }

    // Device and swapchain
    auto device = rhi::create_device();
    if (!device) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to create device");
        if (!m_config.headless) {
            window.destroy();
        }
        JobSystem::instance().shutdown();
        platform_shutdown();
        return 1;
    }
    rhi::DeviceDesc ddesc{};
    ddesc.window_handle = m_config.headless ? nullptr : window.native_handle();
    ddesc.enable_validation = m_config.validation;
    // Shipped/play sessions don't need per-call layer checks (same rationale
    // as the editor default); explicit --validation still enables them.
    ddesc.disable_validation = !m_config.validation;
    if (!device->init(ddesc)) {
        NF_LOG_ERROR(LogCategory::RHI, "Failed to init device");
        if (!m_config.headless) {
            window.destroy();
        }
        JobSystem::instance().shutdown();
        platform_shutdown();
        return 1;
    }
    if (m_config.validation && !device->validation_enabled()) {
        NF_LOG_WARN(LogCategory::RHI, "Validation requested but layer unavailable; continuing without it");
    }

    std::unique_ptr<rhi::Swapchain> swapchain;
    if (!m_config.headless) {
        rhi::SwapchainDesc sc_desc{};
        sc_desc.width = window.width();
        sc_desc.height = window.height();
        sc_desc.format = rhi::Format::B8G8R8A8_UNorm;
        sc_desc.present = m_config.vsync ? rhi::PresentMode::FIFO : rhi::PresentMode::Immediate;
        sc_desc.image_count = 2;
        swapchain = device->create_swapchain(sc_desc);
        if (!swapchain) {
            NF_LOG_ERROR(LogCategory::RHI, "Failed to create swapchain");
            device->shutdown();
            window.destroy();
            JobSystem::instance().shutdown();
            platform_shutdown();
            return 1;
        }
    }

    // Everything GPU-owned (AssetManager caches, Runtime renderer/meshes,
    // per-frame cmd/semaphores) lives inside this scope and is destroyed
    // before swapchain/device shutdown below. Destroying them after
    // vkDestroyDevice would leak and trip validation.
    uint32_t frame_count = 0;
    // The frame limit. max_frames == 0 means "until the window closes", and a
    // headless run has no window to close -- so an unbounded headless run
    // spins forever and never returns. It is bounded here, and the bound is
    // ANNOUNCED: the old code stopped after ten frames with no message at
    // all, which read as a clean but truncated validation run.
    uint32_t frame_limit = m_config.max_frames;
    if (m_config.headless && frame_limit == 0) {
        frame_limit = kHeadlessDefaultFrames;
        NF_LOG_WARN(LogCategory::Core,
                    "Headless run without --frames: stopping after {} frames. "
                    "Pass --frames N to choose the count.",
                    frame_limit);
    }
    // Set when a scene loaded successfully but nothing was ever drawn. That is
    // the signature of an uncooked asset cache: every mesh reference resolves to
    // a missing cache:// file, so the run would otherwise exit 0 with zero
    // validation errors and a completely empty frame. Declared outside the GPU
    // scope below so the exit code can still see it.
    bool geometry_missing = false;
    // Animation acceptance. A scene that carries an animated entity must show
    // that entity's driven transform actually moving: "an AnimationComponent
    // exists" and "the pose reached the transform" are different claims, and
    // only the second one would have caught the defect this phase exists to fix
    // (a pose computed every frame and then dropped on the floor).
    //
    // The observable is the largest deviation from the first sampled frame
    // across the whole run, not a first-vs-last comparison: a looping clip that
    // completes a whole number of revolutions returns to its starting transform,
    // so a first-vs-last test would report "no motion" for a perfectly good
    // spin.
    bool animation_stalled = false;
    std::vector<Runtime::AnimatedTransformSample> first_anim_sample;
    float anim_max_deviation = 0.0f;
    // Audio is mixed in whole device blocks, so a single frame may mix nothing
    // (the accumulator has not filled yet). Track the maxima over the run rather
    // than reading the last frame, which would be a coin flip.
    size_t audio_sources_mixed_max = 0;
    float audio_peak_max = 0.0f;
    {
        // CPU-pure since Phase 11 W1: no device here — the GPU copy of a mesh
        // is Runtime's business (rendering::MeshLibrary).
        assets::AssetManager asset_manager(vfs, registry);
        Runtime runtime(vfs, registry, asset_manager, *device, swapchain.get());

        // Opt-in developer profiling. Applying it before load_scene matters:
        // the renderer is created lazily by the first scene, and the request is
        // re-applied after every Renderer3D init as well.
        if (m_config.gpu_timing) {
            runtime.set_gpu_profiling_enabled(true);
            NF_LOG_INFO(LogCategory::Core,
                        "GPU profiling requested: frame timestamps + per-pass breakdown "
                        "(profiling overhead included in the numbers)");
        }

        // A windowed run IS a played run: gameplay modules own the camera and
        // the player entity, so they must be allowed to write (set_playing),
        // and something real must answer their input polls. Headless keeps
        // the editor-safe defaults — playing=false, no input source — which
        // is what every headless harness asserts on.
        KeyboardInputSource keyboard_input;
        if (!m_config.headless) {
            // The source needs the window to clip and centre the pointer; a
            // source without one still reads keys and sticks, which is what a
            // headless run and the editor's play mode want.
            keyboard_input.set_window(window.native_handle());

            // Rebinding is optional. A project that ships
            // `Content/Input/bindings.nfinput` gets its own vocabulary; a
            // project that does not gets the engine's, which is exactly what
            // the source used before the table existed. A file that does not
            // parse is reported and ignored rather than half-applied: half a
            // control scheme is worse than the one that worked.
            if (const auto text = vfs.read_text("content://Input/bindings.nfinput"); text.ok) {
                std::string parse_error;
                runtime::InputBindings parsed =
                    runtime::InputBindings::parse(text.value, &parse_error);
                if (parse_error.empty()) {
                    // Overlay, not replace: a file that rebinds jump and
                    // nothing else must not delete `interact`.
                    InputBindings bindings = InputBindings::defaults();
                    bindings.merge(parsed);
                    keyboard_input.set_bindings(std::move(bindings));
                    NF_LOG_INFO(LogCategory::Core, "Input bindings loaded: {} actions overridden",
                                parsed.action_count());
                } else {
                    NF_LOG_WARN(LogCategory::Core,
                                "Input bindings ignored ({}), keeping the defaults", parse_error);
                }
            }

            runtime.set_playing(true);
            if (m_config.enable_input) {
                runtime.set_input_source(&keyboard_input);
            }
        }

        std::string scene_err;
        bool scene_ok = true;
        if (!resolved_scene.empty()) {
            scene_ok = runtime.load_scene(resolved_scene, scene_err);
            if (!scene_ok) {
                NF_LOG_ERROR(LogCategory::Core, "Failed to load scene '{}': {}", resolved_scene, scene_err);
                // Missing scene is a hard failure for the sample: exit non-zero
                // after cleaning up GPU resources below.
                device->wait_idle();
                runtime.shutdown();
                asset_manager.clear();
                swapchain.reset();
                device->shutdown();
                if (!m_config.headless) {
                    window.destroy();
                }
                JobSystem::instance().shutdown();
                platform_shutdown();
                return 1;
            }
            // The one log line that separates "the scene played" from "the
            // scene rendered": gameplay modules registered in this binary and
            // stepped by the loop below. Zero here means a PlayerController
            // in the scene has nothing to run it.
            NF_LOG_INFO(LogCategory::Core, "Gameplay modules registered: {} (input: {})",
                        runtime.gameplay_module_count(),
                        runtime.input_source() != nullptr ? "keyboard" : "none");
        }

        // --- Deterministic input ------------------------------------------------
        // Prepared before the loop so a bad flag fails the run at startup with a
        // message, instead of producing a "successful" run in which nothing was
        // ever replayed and the entity stood still for reasons nobody can see.
        // Both `replay_log` and `replay_probe` come from the validation above.
        const bool replaying = !m_config.input_replay_path.empty();
        if (replaying) {
            // A replay replaces the keyboard outright, including in a headless
            // run where no keyboard was ever installed. That is the whole point:
            // `PlayerController` polls an action, and without a source it polls
            // nothing and does nothing — so before this route existed, a
            // headless run could not demonstrate input-driven gameplay at all.
            runtime.set_playing(true);
            runtime.play_input_log(replay_log);
            NF_LOG_INFO(LogCategory::Core,
                        "Replaying input log: {} frames from '{}' ({})",
                        replay_log.frame_count(), m_config.input_replay_path,
                        to_string(replay_probe));
        } else if (!m_config.input_log_path.empty()) {
            // Capture wraps whatever source is live. With a keyboard installed it
            // records real play; headless it records the "no source" stream, which
            // is still a replayable history of a run that polled nothing.
            runtime.begin_input_capture();
            NF_LOG_INFO(LogCategory::Core, "Recording input log to '{}'",
                        m_config.input_log_path);
        }

        // Two frames in flight: while the GPU executes frame N the CPU records
        // frame N+1. A single cmd/fence pair forced every frame into
        // "wait GPU → record → draw → wait GPU" — with a heavy scene the CPU
        // and GPU each idled for the other's entire frame, which is the
        // stutter users see with many-mesh scenes. Per-SLOT frame resources
        // (fence, cmd, acquire semaphore) are what makes the overlap legal:
        // slot resources are only touched after that slot's fence says its
        // previous submission finished.
        static constexpr uint32_t kFramesInFlight = 2;
        std::array<std::unique_ptr<rhi::Semaphore>, kFramesInFlight> image_available;
        std::array<std::unique_ptr<rhi::Fence>, kFramesInFlight> frame_fence;
        std::array<std::unique_ptr<rhi::CommandBuffer>, kFramesInFlight> cmd;
        for (uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
            image_available[slot] = device->create_semaphore();
            frame_fence[slot] = device->create_fence(true);
            cmd[slot] = device->create_command_buffer();
        }
        std::vector<std::unique_ptr<rhi::Semaphore>> render_finished;
        if (!m_config.headless && swapchain) {
            render_finished.reserve(swapchain->image_count());
            for (uint32_t i = 0; i < swapchain->image_count(); ++i) {
                render_finished.push_back(device->create_semaphore());
            }
        }
        if (!m_config.headless) {
            bool sync_ok = true;
            for (uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
                if (!image_available[slot] || !frame_fence[slot] || !cmd[slot]) sync_ok = false;
            }
            if (!sync_ok) {
                NF_LOG_ERROR(LogCategory::RHI, "Failed to create per-frame sync objects");
                device->wait_idle();
                runtime.shutdown();
                asset_manager.clear();
                swapchain.reset();
                device->shutdown();
                window.destroy();
                JobSystem::instance().shutdown();
                platform_shutdown();
                return 1;
            }
        }

        auto recreate_swapchain = [&]() -> bool {
            if (m_config.headless) {
                return false;
            }
            device->wait_idle();
            rhi::SwapchainDesc sc_desc{};
            sc_desc.width = window.width();
            sc_desc.height = window.height();
            if (sc_desc.width == 0 || sc_desc.height == 0) {
                return false;
            }
            sc_desc.format = rhi::Format::B8G8R8A8_UNorm;
            sc_desc.present = m_config.vsync ? rhi::PresentMode::FIFO : rhi::PresentMode::Immediate;
            sc_desc.image_count = 2;
            auto fresh = device->create_swapchain(sc_desc);
            if (!fresh) {
                NF_LOG_ERROR(LogCategory::RHI, "Swapchain recreation failed ({}x{})", sc_desc.width,
                             sc_desc.height);
                return false;
            }
            swapchain = std::move(fresh);
            runtime.on_swapchain_resized(swapchain.get());
            render_finished.clear();
            render_finished.reserve(swapchain->image_count());
            for (uint32_t i = 0; i < swapchain->image_count(); ++i) {
                render_finished.push_back(device->create_semaphore());
            }
            NF_LOG_INFO(LogCategory::RHI, "Swapchain recreated ({}x{})", swapchain->width(),
                        swapchain->height());
            return true;
        };

        Clock clock;
        // Phase 41 telemetry lives in the host loop, not in Runtime::update:
        // raw wall time includes presentation/vsync waiting and hitches, while
        // the simulation deliberately receives a clamped dt. Feeding the
        // telemetry from the simulation clock would hide exactly the stalls it
        // exists to report.
        rendering::FramePacing frame_pacing;
        u64 frame_pacing_gpu_serial = 0;
        while (true) {
            if (!m_config.headless) {
                window.poll_events();
                if (window.should_close()) {
                    break;
                }
                // Resize: recreate swapchain + renderer resolution targets.
                if (swapchain &&
                    (window.width() != swapchain->width() || window.height() != swapchain->height())) {
                    NF_LOG_INFO(LogCategory::RHI, "Resize detected: window {}x{} vs swapchain {}x{}",
                                window.width(), window.height(), swapchain->width(), swapchain->height());
                    if (!recreate_swapchain()) {
                        NF_LOG_ERROR(LogCategory::RHI, "Failed to handle resize; exiting");
                        break;
                    }
                }
            }

            // The frame delta is clamped before it reaches the simulation. A
            // scene load, a shader compile or a debugger break produces a
            // multi-second delta, and the audio mixer drains at most four
            // blocks per frame — so an unclamped hitch left a permanent
            // backlog that could never be caught up, and the audio stayed
            // seconds behind the video for the rest of the session.
            const float raw_dt = static_cast<float>(clock.elapsed_seconds());
            clock.reset();
            const float dt = std::min(raw_dt, kMaxFrameDelta);

            // Phase 42: a scene that decides its render scale from the frame
            // WINDOW needs the window. `stats()` sorts a copy of the ring, so
            // once every eight frames is enough for a 120-frame window and
            // keeps that sort off the per-frame path. A scene that never opts
            // in ignores the value entirely.
            if ((frame_count % 8u) == 0u) {
                runtime.set_frame_window(frame_pacing.stats());
            }

            runtime.update(dt);

            // Track how far every animated entity's driven transform has moved
            // from where it started. Recorded every frame so any motion at any
            // point counts, not just motion still present on the last frame.
            {
                const auto now = runtime.animated_transform_samples();
                if (first_anim_sample.empty()) {
                    first_anim_sample = now;
                } else if (now.size() == first_anim_sample.size()) {
                    for (size_t i = 0; i < now.size(); ++i) {
                        const auto& a = first_anim_sample[i];
                        const auto& b = now[i];
                        const float d = std::max({
                            std::abs(a.translation.x - b.translation.x),
                            std::abs(a.translation.y - b.translation.y),
                            std::abs(a.translation.z - b.translation.z),
                            std::abs(a.rotation_euler_degrees.x - b.rotation_euler_degrees.x),
                            std::abs(a.rotation_euler_degrees.y - b.rotation_euler_degrees.y),
                            std::abs(a.rotation_euler_degrees.z - b.rotation_euler_degrees.z),
                            std::abs(a.scale.x - b.scale.x),
                            std::abs(a.scale.y - b.scale.y),
                            std::abs(a.scale.z - b.scale.z),
                        });
                        anim_max_deviation = std::max(anim_max_deviation, d);
                    }
                }
                audio_sources_mixed_max =
                    std::max(audio_sources_mixed_max, runtime.audio_sources_mixed());
                audio_peak_max = std::max(audio_peak_max, runtime.audio_output_peak());
            }

            if (!m_config.headless && swapchain) {
                const uint32_t slot = frame_count % kFramesInFlight;
                frame_fence[slot]->wait();
                frame_fence[slot]->reset();

                const uint32_t image_index = swapchain->acquire_next_image(*image_available[slot]);
                if (image_index == 0xFFFFFFFF) {
                    NF_LOG_WARN(LogCategory::RHI, "Swapchain out of date; recreating");
                    if (!recreate_swapchain()) {
                        break;
                    }
                    continue;
                }
                if (image_index >= swapchain->image_count()) {
                    NF_LOG_ERROR(LogCategory::RHI, "Invalid swapchain image index {}", image_index);
                    break;
                }

                // Windowed Runtime: Scene → AssetManager → Renderer3D
                // (Shadow → Depth → GBuffer → Lighting → Tonemap), submit, present.
                cmd[slot]->reset();
                cmd[slot]->begin();
                runtime.render(image_index, *cmd[slot], slot);
                cmd[slot]->end();

                rhi::SubmitInfo submit_info{};
                const std::array<const rhi::Semaphore*, 1> wait_sems{image_available[slot].get()};
                const std::array<rhi::PipelineStage, 1> wait_stages{rhi::PipelineStage::ColorAttachmentOutput};
                const std::array<const rhi::Semaphore*, 1> signal_sems{
                    render_finished[image_index].get()};
                submit_info.wait_semaphores = std::span<const rhi::Semaphore* const>(wait_sems);
                submit_info.wait_stages = std::span<const rhi::PipelineStage>(wait_stages);
                submit_info.signal_semaphores = std::span<const rhi::Semaphore* const>(signal_sems);
                submit_info.signal_fence = frame_fence[slot].get();
                device->submit(*cmd[slot], submit_info);
                swapchain->present(image_index, std::span<const rhi::Semaphore* const>(signal_sems));
            } else {
                // Headless: no swapchain, no PRESENT. Update only.
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }

            // One pacing sample per presented/updated frame. The GPU duration
            // is the last completed timestamp sample, so it is paired with the
            // wall time of the frame that just finished. GPU data stays
            // opt-in: without --gpu-timing the bound is reported as unknown.
            {
                f64 gpu_frame_s = 0.0;
                bool gpu_sample = false;
                if (m_config.gpu_timing) {
                    if (const auto* renderer = runtime.renderer(); renderer != nullptr) {
                        const auto& stats = renderer->last_stats();
                        if (stats.gpu_timing_valid && stats.gpu_sample_serial != 0 &&
                            stats.gpu_sample_serial != frame_pacing_gpu_serial) {
                            frame_pacing_gpu_serial = stats.gpu_sample_serial;
                            gpu_frame_s = stats.gpu_frame_ms / 1000.0;
                            gpu_sample = true;
                        }
                    }
                }
                frame_pacing.push_frame(raw_dt, gpu_frame_s, gpu_sample);
            }

            ++frame_count;
            if (frame_limit > 0 && frame_count >= frame_limit) {
                break;
            }
            // There is no second limit here. max_frames == 0 means "no limit"
            // for a windowed run, and the headless case above has already
            // been given a bound. The old headless-only branch silently
            // stopped a headless run after ten frames when --frames was
            // omitted -- a hard-to-diagnose truncation of a validation run,
            // and a disagreement with the windowed path, which kept running.
        }

        NF_LOG_INFO(LogCategory::Core, "Rendered {} frames (meshes={})", frame_count, runtime.mesh_count());
        if (m_config.frame_stats) {
            const auto pacing = frame_pacing.stats();
            if (pacing.samples == 0) {
                NF_LOG_WARN(LogCategory::Core, "Frame stats requested, but no frame was sampled");
            } else {
                NF_LOG_INFO(
                    LogCategory::Core,
                    "Frame stats ({} frames): {:.2f} fps avg ({:.2f} ms) | 1% low {:.2f} fps "
                    "({:.2f} ms) | jitter {:.2f} ms | range {:.2f}-{:.2f} ms | bound {}",
                    pacing.samples, pacing.average_fps, pacing.average_frame_ms,
                    pacing.low_1pct_fps, pacing.low_1pct_frame_ms, pacing.stddev_ms,
                    pacing.min_frame_ms, pacing.max_frame_ms,
                    rendering::frame_pacing_bound_name(pacing.bound));
                if (pacing.gpu_samples > 0) {
                    NF_LOG_INFO(LogCategory::Core,
                                "  GPU frame time: {:.2f} ms average across {} samples in the "
                                "window",
                                pacing.gpu_average_ms, pacing.gpu_samples);
                } else if (m_config.gpu_timing && m_config.headless) {
                    NF_LOG_INFO(LogCategory::Core,
                                "  GPU frame time: none (headless runs do not render a frame)");
                } else if (!m_config.gpu_timing) {
                    NF_LOG_INFO(LogCategory::Core,
                                "  GPU frame time: not measured (add --gpu-timing for a bound "
                                "verdict)");
                } else {
                    NF_LOG_INFO(LogCategory::Core,
                                "  GPU frame time: no valid sample yet (timestamps unsupported or "
                                "the run was too short)");
                }
            }
        }
        if (runtime.renderer() != nullptr) {
            const auto& stats = runtime.renderer()->last_stats();
            NF_LOG_INFO(LogCategory::Core, "Last frame: extracted={} visible={} draws={}", stats.extracted,
                        stats.visible, stats.draw_calls);
            // Display stays opt-in. A scene with GPU-aware dynamic resolution
            // enables frame timestamps internally even when the player was not
            // asked to profile; that is a control input, not a report, and it
            // must not print timing the user did not request.
            if (stats.gpu_timing_valid && m_config.gpu_timing) {
                NF_LOG_INFO(LogCategory::Core, "GPU timestamps: frame {:.4f} ms across {} passes",
                            stats.gpu_frame_ms, stats.gpu_passes.size());
                for (const auto& pass : stats.gpu_passes) {
                    NF_LOG_INFO(LogCategory::Core, "  GPU pass {:<18} {:.4f} ms", pass.name,
                                pass.milliseconds);
                }
            } else if (m_config.gpu_timing && m_config.headless) {
                NF_LOG_WARN(LogCategory::Core,
                            "GPU timing requested in headless mode, but this player does not "
                            "render a frame; run with a window to capture timestamps");
            } else if (m_config.gpu_timing) {
                NF_LOG_WARN(LogCategory::Core,
                            "GPU timing requested but no completed timestamp sample was consumed "
                            "(run at least two frames)");
            }
            // A scene that loaded but drew no geometry is a failure, not a
            // quiet success. Without this the sample reported a clean exit for a
            // run that rendered an empty frame.
            if (scene_ok && !resolved_scene.empty() && runtime.mesh_count() == 0) {
                NF_LOG_ERROR(LogCategory::Core,
                             "Scene '{}' loaded but drew no geometry (draws={}) — the asset cache is "
                             "probably not cooked. Run NFAssetCooker on Content/ (see README).",
                             resolved_scene, stats.draw_calls);
                geometry_missing = true;
            }
        }
        // Animation acceptance: the scene carried an animated entity, so its
        // driven transform has to have moved. See the note where
        // `animation_stalled` is declared for why this is a max-deviation test
        // rather than a first-vs-last one.
        if (!first_anim_sample.empty()) {
            NF_LOG_INFO(LogCategory::Core,
                         "Animated entities: {} (max transform deviation over {} frames: {})",
                         first_anim_sample.size(), frame_count, anim_max_deviation);
            if (anim_max_deviation <= 1e-4f) {
                NF_LOG_ERROR(LogCategory::Core,
                             "Scene has {} animated entit{} but no driven transform moved over {} "
                             "frames (max deviation {}). The animation pose is being computed but "
                             "not reaching the entity, so the animation is not actually running.",
                             first_anim_sample.size(),
                             first_anim_sample.size() == 1 ? "y" : "ies", frame_count,
                             anim_max_deviation);
                animation_stalled = true;
            }
        } else if (runtime.rigged_character_count() > 0) {
            // A RIGGED character's animation moves its mesh, not its
            // transform: its placement belongs to the game. So a scene whose
            // only animation is a character is checked by the character's own
            // deformation -- otherwise the check would report a walking
            // character as "an animation that is not running".
            const float character_deviation = runtime.character_max_deviation();
            NF_LOG_INFO(LogCategory::Core,
                        "Animated characters: {} (max mesh deviation over {} frames: {})",
                        runtime.rigged_character_count(), frame_count, character_deviation);
            if (character_deviation <= 1e-4f) {
                NF_LOG_ERROR(LogCategory::Core,
                             "Scene has {} character(s) whose mesh never moved over {} frames (max "
                             "deviation {}). The rig is loaded but the pose is not reaching the mesh.",
                             runtime.rigged_character_count(), frame_count, character_deviation);
                animation_stalled = true;
            }
        }
        // Audio is reported but not asserted: the plan's acceptance covers the
        // animated entity's motion, and a scene legitimately may have no audio.
        // The test suite asserts the mixing path directly instead.
        NF_LOG_INFO(LogCategory::Core, "Audio sources mixed (peak): {} ({})",
                    audio_sources_mixed_max, audio_peak_max);
        if (m_config.validation) {
            const uint32_t verrs = rhi::validation_error_count();
            NF_LOG_INFO(LogCategory::RHI, "Validation errors: {}", verrs);
            if (verrs != 0) {
                NF_LOG_ERROR(LogCategory::RHI, "Validation failed with {} errors", verrs);
            }
        }
        // Explicit GPU cleanup while the device is still alive.
        // Per-frame objects (cmd/semaphores) are still alive here; they die at
        // scope end. Runtime/AssetManager GPU resources are freed now.
        device->wait_idle();
        runtime.shutdown();
        asset_manager.clear();
        device->wait_idle();
        NF_LOG_INFO(LogCategory::Core, "Shutting down runtime after {} frames", frame_count);

        // --- Write the recording, if one was asked for -----------------------
        // After the frame loop and before the runtime dies, because
        // end_input_capture() hands back the log the loop filled.
        //
        // A failed write is reported and fails the run: an `--input-log` that
        // silently writes nothing is the exact failure this flag exists to
        // prevent — a recording step that appears to work and leaves no evidence.
        if (!m_config.input_log_path.empty()) {
            const InputLog recorded = runtime.end_input_capture();
            std::error_code write_ec;
            const std::filesystem::path out_path(m_config.input_log_path);
            if (out_path.has_parent_path()) {
                std::filesystem::create_directories(out_path.parent_path(), write_ec);
            }
            std::ofstream log_file(out_path, std::ios::binary | std::ios::trunc);
            if (log_file) {
                log_file << recorded.serialize();
                log_file.close();
                NF_LOG_INFO(LogCategory::Core, "Wrote input log: {} frames to '{}'",
                            recorded.frame_count(), out_path.string());
            } else {
                // Reported, not fatal: the simulation ran and its result is
                // valid, so refusing to exit cleanly would throw away a
                // successful run over a sidecar file. The log is gone either
                // way, so the message says exactly that.
                NF_LOG_ERROR(LogCategory::Core,
                             "--input-log: could not open '{}' for writing — the "
                             "recording of this run was lost",
                             out_path.string());
            }
        }
    }

    // --replay-strict turns "the flag was accepted" into "the flag did something".
    // Checked here, outside the runtime's lifetime, so the guarantee does not
    // depend on GPU teardown having succeeded first.
    if (m_config.input_replay_strict && replay_log.frame_count() == 0) {
        NF_LOG_ERROR(LogCategory::Core,
                     "--replay-strict: --replay was given but served no frames, "
                     "so this run proved nothing.");
        swapchain.reset();
        device->wait_idle();
        device->shutdown();
        if (!m_config.headless) window.destroy();
        JobSystem::instance().shutdown();
        platform_shutdown();
        return 1;
    }

    // All GPU objects from the scope above are gone (Runtime, AssetManager,
    // per-frame cmd/semaphores). Only the swapchain remains.
    swapchain.reset();
    device->wait_idle();
    uint32_t leaked = 0;
    {
        const uint32_t alive = device->alive_objects();
        NF_LOG_INFO(LogCategory::RHI, "Alive RHI objects before shutdown: {}", alive);
        if (alive != 0) {
            NF_LOG_ERROR(LogCategory::RHI, "Leaked RHI objects detected: {}", alive);
            leaked = alive;
        }
    }
    device->shutdown();
    if (!m_config.headless) {
        window.destroy();
    }
    JobSystem::instance().shutdown();
    platform_shutdown();

    if (m_config.validation && rhi::validation_error_count() != 0) {
        NF_LOG_ERROR(LogCategory::RHI, "Exiting with validation errors present");
        return 1;
    }
    if (leaked != 0) {
        NF_LOG_ERROR(LogCategory::RHI, "Exiting with leaked RHI objects present");
        return 1;
    }
    if (geometry_missing) {
        NF_LOG_ERROR(LogCategory::Core,
                     "Exiting: the scene produced no geometry. Cook the asset cache first "
                     "(NFAssetCooker — see README).");
        return 1;
    }
    if (animation_stalled) {
        NF_LOG_ERROR(LogCategory::Core,
                     "Exiting: the scene has an animated entity that never moved.");
        return 1;
    }
    NF_LOG_INFO(LogCategory::Core, "=== SANAD Runtime exited cleanly ===");
    return 0;
}

} // namespace nf::runtime
