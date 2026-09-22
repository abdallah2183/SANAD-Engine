// Tests/PerfTests/test_perf_scene.cpp — the G8 performance contract.
//
// A perf scene (Content/Scenes/Perf.nfscene, ~2k objects + shadowed sun +
// destructible crates) is loaded, rendered headless and measured, and the
// measurements are gated against Tests/PerfTests/perf_baseline.csv: any metric
// more than 10% above its baseline row FAILS the suite loudly, so a perf
// regression cannot ship unnoticed.
//
// Modes (all via env, so the same binary serves tests, local gates and CI):
//   default            measure and print `PERF_METRIC <name> <value>` lines.
//   NF_PERF_GATE=1     additionally enforce the baseline CSV. A metric with no
//                      baseline row also fails — an unmeasured contract is a
//                      hole, not a pass.
//   NF_PERF_RECORD=f   append `metric,machine,value,tolerance_pct` rows to f
//                      (used by Tests/PerfTests/perf_gate.sh --record).
//   NF_PERF_MACHINE=m  the machine tag of THIS run. A baseline row tagged
//                      `any` is enforced everywhere (draw calls, object
//                      counts, scene-attributable memory — deterministic). A
//                      row tagged with a machine name (frame times, total
//                      working set) is enforced only on that machine, because
//                      an absolute time measured on one box says nothing
//                      about another.
//
// The comparison itself — `evaluate_gate()` below — is a pure function with no
// GPU and no environment behind it, and the tests at the bottom of this file
// drive it directly: a >10% regression, an unbaselined metric, and another
// machine's timing are all asserted, so the contract is proven on every run,
// not only on a machine whose frame time happens to be measured.
//
// GPU-dependent metrics (frame time, draw calls) run through the real Runtime
// + Renderer3D path and NF_SKIP — never pass silently — when no Vulkan device
// exists. CI wires Lavapipe so the gate executes there for real.

// This suite reads its knobs from the environment and is built /WX, so MSVC's
// "unsafe" getenv deprecation (C4996) has to be silenced before any CRT header
// is pulled in — hence the very top of the file, not the platform block below.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <NF/Test/TestFramework.hpp>

#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/MeshAsset.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/Core/Time.hpp>
#include <NF/Destruction/DestructionWorld.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/Rendering/MeshUpload.hpp>
#include <NF/Rendering/StaticMesh.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
// Both are already on the command line for this target; redefining them here
// would be a C4005 warning, which /WX turns into an error.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

#ifndef NF_PERF_SCENE_PATH
#define NF_PERF_SCENE_PATH "Content/Scenes/Perf.nfscene"
#endif
#ifndef NF_PERF_BASELINE_PATH
#define NF_PERF_BASELINE_PATH "Tests/PerfTests/perf_baseline.csv"
#endif

using namespace nf;
using namespace nf::runtime;
using namespace nf::assets;
using namespace nf::ecs;

namespace {

// The cube asset id every Perf.nfscene Mesh line references (same cube the
// editor and the other scenes cook).
constexpr const char* kCubeAssetId = "f04e488b-4dc8-414b-baee-e3c50e8829ad";

const char* env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : fallback;
}

// --- Baseline table ---------------------------------------------------------

struct BaselineRow {
    std::string metric;
    std::string machine;
    double value = 0.0;
    double tolerance_pct = 10.0;
};

std::vector<BaselineRow> load_baseline(const std::string& path) {
    std::vector<BaselineRow> rows;
    std::ifstream in(path);
    if (!in) return rows;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        BaselineRow row;
        std::string tol;
        if (!std::getline(ss, row.metric, ',')) continue;
        if (!std::getline(ss, row.machine, ',')) continue;
        if (!std::getline(ss, tol, ',')) continue;
        row.value = std::strtod(tol.c_str(), nullptr);
        // value column done; tolerance is the 4th field
        std::string rest;
        if (std::getline(ss, rest, ',')) row.tolerance_pct = std::strtod(rest.c_str(), nullptr);
        rows.push_back(row);
    }
    return rows;
}

void emit_metric(const std::string& name, double value) {
    std::cout << "PERF_METRIC " << name << " " << value << "\n";
}

/// The machine tag of THIS run. Machine-dependent rows are recorded with it and
/// enforced only against it (see the file header).
std::string this_machine_tag() { return env_or("NF_PERF_MACHINE", "dev"); }

enum class GateVerdict {
    Ok,              ///< within tolerance
    ReportOnly,      ///< baseline row belongs to a different machine
    MissingBaseline, ///< no row — a hole, not a pass
    Regressed,       ///< above the tolerance limit
};

struct GateResult {
    GateVerdict verdict = GateVerdict::Ok;
    double baseline = 0.0;
    double limit = 0.0;
    double over_pct = 0.0;
    std::string machine;
};

/// The performance contract, isolated from the GPU and from the environment so
/// it can be tested directly: given the baseline rows, a metric and this run's
/// machine tag, say whether the value passes.
///
/// `any` rows are enforced on every machine; a row tagged with a machine name is
/// enforced only on that machine and reported (never silently ignored) anywhere
/// else. A metric with no row is MissingBaseline — deliberately not Ok: a gate
/// metric nobody baselined is a hole, and holes do not pass.
GateResult evaluate_gate(const std::vector<BaselineRow>& rows, const std::string& name,
                         double value, const std::string& machine_of_run) {
    GateResult r;
    const BaselineRow* row = nullptr;
    for (const auto& candidate : rows) {
        if (candidate.metric == name) { row = &candidate; break; }
    }
    if (row == nullptr) {
        r.verdict = GateVerdict::MissingBaseline;
        return r;
    }

    r.baseline = row->value;
    r.machine = row->machine;

    if (row->machine != "any" && row->machine != machine_of_run) {
        r.verdict = GateVerdict::ReportOnly;
        return r;
    }

    r.limit = row->value * (1.0 + row->tolerance_pct / 100.0);
    if (value > r.limit) {
        r.verdict = GateVerdict::Regressed;
        // A zero baseline has no meaningful percentage; the limit (0) still
        // rejects any positive value above.
        r.over_pct = (row->value != 0.0) ? (value - row->value) / row->value * 100.0 : 0.0;
        return r;
    }
    r.verdict = GateVerdict::Ok;
    return r;
}

/// Enforce one metric against the baseline. Split out of `perf_metric` so the
/// tests can drive the exact code path CI takes without also emitting a
/// `PERF_METRIC` line or appending a bogus row to a `--record` file.
///
/// The baseline is re-read on every call on purpose: caching it in a function-
/// static made the result depend on which test happened to call this first.
void enforce_metric(const std::string& name, double value) {
    const std::vector<BaselineRow> rows =
        load_baseline(env_or("NF_PERF_BASELINE", NF_PERF_BASELINE_PATH));
    const GateResult r = evaluate_gate(rows, name, value, this_machine_tag());

    switch (r.verdict) {
        case GateVerdict::MissingBaseline:
            throw std::runtime_error("PERF GATE FAILED: metric '" + name +
                                     "' has no baseline row — a gate metric nobody baselined is a hole. "
                                     "Run Tests/PerfTests/perf_gate.sh --record to add one.");
        case GateVerdict::ReportOnly:
            std::cout << "PERF_GATE report-only " << name << "=" << value
                      << " (baseline " << r.baseline << " is machine '" << r.machine
                      << "', this run is '" << this_machine_tag() << "')\n";
            return;
        case GateVerdict::Regressed:
            throw std::runtime_error("PERF GATE FAILED: " + name + "=" + std::to_string(value) +
                                     " exceeds baseline " + std::to_string(r.baseline) + " by " +
                                     std::to_string(r.over_pct) + "% (limit " + std::to_string(r.limit) +
                                     "). A perf regression >10% must not ship.");
        case GateVerdict::Ok:
            std::cout << "PERF_GATE ok " << name << "=" << value << " (limit " << r.limit << ")\n";
            return;
    }
}

/// Print, optionally record, and (in gate mode) enforce one metric.
/// `machine_tag` is "any" for machine-independent counters, or this run's tag
/// for machine-dependent timings (see file header).
void perf_metric(const std::string& name, double value, const char* machine_tag) {
    emit_metric(name, value);

    if (const char* rec = std::getenv("NF_PERF_RECORD")) {
        std::ofstream out(rec, std::ios::app);
        if (out) out << name << ',' << machine_tag << ',' << value << ",10\n";
    }

    if (std::string(env_or("NF_PERF_GATE", "0")) != "1") return;
    enforce_metric(name, value);
}

double median(std::vector<double>& v) {
    NF_CHECK(!v.empty());
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

#ifdef _WIN32
u64 working_set_bytes() {
    PROCESS_MEMORY_COUNTERS pmc{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0;
    return static_cast<u64>(pmc.WorkingSetSize);
}
#else
u64 working_set_bytes() { return 0; }
#endif

// --- Shared GPU harness -----------------------------------------------------

struct PerfHarness {
    std::filesystem::path tmp;
    VirtualFileSystem vfs;
    AssetRegistry registry;
    AssetManager manager;
    std::unique_ptr<rhi::IGraphicsDevice> device;
    std::unique_ptr<Runtime> runtime;

    PerfHarness() : manager(vfs, registry) {
        tmp = std::filesystem::temp_directory_path() / "nf_perf_gate";
        std::filesystem::remove_all(tmp);
        std::filesystem::create_directories(tmp / "Content" / "Scenes");
        std::filesystem::create_directories(tmp / "Content" / "Materials");
        std::filesystem::create_directories(tmp / "Cache" / "Meshes");
        vfs.mount("content://", tmp / "Content");
        vfs.mount("cache://", tmp / "Cache");

        // The scene's material, copied for the same reason as the scene itself:
        // without it the runtime logs "material missing, using gray fallback"
        // and the perf scene stops being the scene that ships. A perf number
        // measured on a fallback material is a perf number for a different
        // scene. Derived from the scene path so it is absolute like the rest.
        {
            const std::filesystem::path scene_path(NF_PERF_SCENE_PATH);
            const std::filesystem::path mat_path =
                scene_path.parent_path().parent_path() / "Materials" / "Default.nfmat";
            std::ifstream mat_in(mat_path, std::ios::binary);
            NF_CHECK(static_cast<bool>(mat_in));
            std::string mat_text((std::istreambuf_iterator<char>(mat_in)), std::istreambuf_iterator<char>());
            NF_CHECK(!mat_text.empty());
            std::ofstream mat_out(tmp / "Content" / "Materials" / "Default.nfmat", std::ios::binary);
            mat_out.write(mat_text.data(), static_cast<std::streamsize>(mat_text.size()));
            NF_CHECK(static_cast<bool>(mat_out));
        }

        // The perf scene fixture, copied into the scratch content tree so the
        // cooked mesh the harness generates below is the only registry entry —
        // no dependency on whatever state the repo's Cache/ is in.
        std::ifstream scene_in(NF_PERF_SCENE_PATH, std::ios::binary);
        NF_CHECK(static_cast<bool>(scene_in));
        std::string text((std::istreambuf_iterator<char>(scene_in)), std::istreambuf_iterator<char>());
        NF_CHECK(!text.empty());
        std::ofstream scene_out(tmp / "Content" / "Scenes" / "Perf.nfscene", std::ios::binary);
        scene_out.write(text.data(), static_cast<std::streamsize>(text.size()));
        NF_CHECK(static_cast<bool>(scene_out));

        // Cook the cube deterministically and register it under the fixture's id.
        AssetId mesh_id = AssetId::from_string(kCubeAssetId);
        auto cube = rendering::StaticMesh::create_cube(1.0f);
        auto cooked = rendering::make_mesh_asset(*cube, mesh_id, "content://Meshes/cube.nfmesh");
        std::vector<uint8_t> bytes;
        cooked->save_to_bytes(bytes);
        NF_CHECK(vfs.write_bytes("cache://Meshes/cube.nfmesh", std::span<const uint8_t>(bytes)).ok);

        AssetMetadata meta{};
        meta.id = mesh_id;
        meta.type = AssetType::Mesh;
        meta.logical_path = "content://Meshes/cube.nfmesh";
        meta.cooked_path = "cache://Meshes/cube.nfmesh";
        meta.fingerprint = "perf-gate";
        meta.format = "nfmesh-v1";
        std::string reg_err;
        NF_CHECK(registry.add(meta, reg_err));
    }

    bool init_gpu() {
        device = rhi::create_device();
        if (!device) return false;
        rhi::DeviceDesc desc{};
        desc.window_handle = nullptr;
        desc.enable_validation = false;
        if (!device->init(desc)) {
            device.reset();
            return false;
        }
        return true;
    }

    void load_scene() {
        runtime = std::make_unique<Runtime>(vfs, registry, manager, *device, nullptr);
        std::string err;
        NF_CHECK(runtime->load_scene("content://Scenes/Perf.nfscene", err));

        auto handle = manager.load_mesh_sync(AssetId::from_string(kCubeAssetId));
        NF_CHECK(handle && handle->state == AssetState::Ready);
        manager.update();
        runtime->update(1.0f / 60.0f);
    }

    /// One full frame: update + offscreen render, submitted and waited on —
    /// the honest per-frame CPU cost a headless run pays.
    double frame_once(rhi::Texture& target, f32 dt) {
        nf::Clock c;
        runtime->update(dt);
        const double update_us = c.elapsed_us();

        auto cmd = device->create_command_buffer();
        auto fence = device->create_fence(false);
        NF_CHECK(cmd && fence);
        c.reset();
        cmd->begin();
        runtime->render_offscreen(target, *cmd);
        cmd->end();
        device->submit(*cmd, rhi::SubmitInfo{.signal_fence = fence.get()});
        NF_CHECK(fence->wait(5'000'000'000ull));
        device->wait_idle();
        return (c.elapsed_us() + update_us) / 1000.0; // ms, update + render
    }

    void teardown() {
        if (runtime) {
            device->wait_idle();
            runtime->shutdown();
            runtime.reset();
        }
        if (device) {
            device->wait_idle();
            device->shutdown();
            device.reset();
        }
        std::filesystem::remove_all(tmp);
    }
};

constexpr u32 kRenderW = 320;
constexpr u32 kRenderH = 180;
constexpr u32 kWarmupFrames = 3;
constexpr u32 kMeasuredFrames = 10;
// Working-set samples taken a frame apart; the minimum is the gated value (see
// the memory block in perf_gpu_frame_baseline for why).
constexpr u32 kMemSamples = 3;

} // namespace

// --- CPU-only: the fixture itself meets the perf-scene contract --------------

NF_TEST(perf_scene_fixture_meets_contract) {
    auto result = load_scene_from_physical(NF_PERF_SCENE_PATH);
    NF_CHECK(result.success);
    NF_CHECK(result.scene != nullptr);
    if (!result.success) return;

    auto& w = result.scene->world();
    u32 meshes = 0;
    for (auto e : w.query<MeshComponent>()) { (void)e; ++meshes; }
    u32 destructibles = 0;
    for (auto e : w.query<DestructibleComponent>()) { (void)e; ++destructibles; }

    u32 shadowed_lights = 0;
    for (auto e : w.query<DirectionalLight>()) {
        const auto* l = w.get<DirectionalLight>(e);
        if (l && l->cast_shadows) ++shadowed_lights;
    }
    u32 active_cameras = 0;
    for (auto e : w.query<CameraComponent>()) {
        const auto* c = w.get<CameraComponent>(e);
        if (c && c->is_active) ++active_cameras;
    }

    NF_LOG_INFO(LogCategory::Core, "[perf-scene] meshes={} destructibles={} shadowed_lights={} active_cameras={}",
                meshes, destructibles, shadowed_lights, active_cameras);

    // The contract: ~2k objects, real shadows, live destruction, a view.
    NF_CHECK(meshes >= 1900 && meshes <= 2200);
    NF_CHECK(destructibles >= 20);
    NF_CHECK(shadowed_lights >= 1);
    NF_CHECK(active_cameras >= 1);

    perf_metric("fixture_mesh_entities", static_cast<double>(meshes), "any");
    perf_metric("fixture_destructibles", static_cast<double>(destructibles), "any");
}

// --- GPU: frame time, draw calls, memory through the real Runtime path -------

NF_TEST(perf_gpu_frame_baseline) {
    PerfHarness h;
    if (!h.init_gpu()) {
        h.teardown();
        NF_SKIP("no Vulkan device available — GPU frame/draw/memory metrics cannot be measured");
    }

    const u64 ws_before = working_set_bytes();
    h.load_scene();

    rhi::TextureDesc td{};
    td.width = kRenderW;
    td.height = kRenderH;
    td.format = rhi::Format::R8G8B8A8_UNorm;
    td.usage = rhi::ImageUsage::ColorAtt;
    auto target = h.device->create_texture(td);
    NF_CHECK(target);

    // Warmup (shader pipelines, mesh upload, allocator growth).
    for (u32 i = 0; i < kWarmupFrames; ++i) (void)h.frame_once(*target, 1.0f / 60.0f);

    // Memory baseline point: scene loaded + first frames served.
    //
    // The working set is sampled kMemSamples times, a frame apart, and the
    // MINIMUM is gated. A single sample carries one-shot page-in — a cold
    // process measured ~147MB where a settled one sits at ~92MB — and gating
    // that spike would fail for reasons that have nothing to do with this
    // scene. The minimum is the settled footprint; a real regression raises
    // every sample, so it still trips.
    u64 ws_after = ~static_cast<u64>(0);
    for (u32 i = 0; i < kMemSamples; ++i) {
        (void)h.frame_once(*target, 1.0f / 60.0f);
        ws_after = std::min(ws_after, working_set_bytes());
    }
    const double ws_mb = static_cast<double>(ws_after) / (1024.0 * 1024.0);
    // Signed: the working set can legitimately shrink below the pre-load
    // sample, and an unsigned difference would wrap into a nonsense number.
    const double ws_delta_mb =
        (static_cast<double>(ws_after) - static_cast<double>(ws_before)) / (1024.0 * 1024.0);
    NF_LOG_INFO(LogCategory::Core, "[perf] working set: min-total={:.1f}MB delta={:.1f}MB", ws_mb, ws_delta_mb);
    // The total is dominated by the Vulkan driver's own footprint, which differs
    // per machine — gated only where it was recorded. The DELTA is what this
    // scene allocates on top of a live device, so it is machine-independent
    // enough to gate everywhere, including CI on Lavapipe.
    perf_metric("working_set_mb", ws_mb, this_machine_tag().c_str());
    perf_metric("working_set_delta_mb", ws_delta_mb, "any");

    std::vector<double> frame_ms;
    std::vector<double> update_ms;
    frame_ms.reserve(kMeasuredFrames);
    for (u32 i = 0; i < kMeasuredFrames; ++i) {
        nf::Clock c;
        h.runtime->update(1.0f / 60.0f);
        update_ms.push_back(c.elapsed_ms());
        frame_ms.push_back(h.frame_once(*target, 1.0f / 60.0f));
    }

    const auto* stats = h.runtime->renderer() ? &h.runtime->renderer()->last_stats() : nullptr;
    NF_CHECK(stats != nullptr);
    NF_LOG_INFO(LogCategory::Core, "[perf] stats: extracted={} visible={} draw_calls={}",
                stats->extracted, stats->visible, stats->draw_calls);
    NF_CHECK(stats->extracted >= 1900);
    NF_CHECK(stats->visible > 0);
    NF_CHECK(stats->draw_calls > 0);

    perf_metric("extracted_objects", static_cast<double>(stats->extracted), "any");
    perf_metric("visible_objects", static_cast<double>(stats->visible), "any");
    perf_metric("draw_calls", static_cast<double>(stats->draw_calls), "any");
    perf_metric("frame_time_ms", median(frame_ms), this_machine_tag().c_str());
    perf_metric("update_ms", median(update_ms), this_machine_tag().c_str());

    // Drop the offscreen target while the device is still alive. teardown()
    // shuts the device down first, and a texture outliving it is a leaked
    // VkImage — a loud validation error in an otherwise clean run, which is
    // exactly the kind of teardown noise that hides a real leak.
    target.reset();
    h.teardown();
}

// --- GPU: the destruction in the perf scene is live, not decoration ----------

NF_TEST(perf_scene_destruction_blast_is_live) {
    PerfHarness h;
    if (!h.init_gpu()) {
        h.teardown();
        NF_SKIP("no Vulkan device available — destruction liveness needs the runtime render path");
    }
    h.load_scene();

    auto* scene = h.runtime->edit_scene();
    NF_CHECK(scene != nullptr);
    auto& w = scene->world();
    auto destructibles = w.query<DestructibleComponent>();
    NF_CHECK(destructibles.size() >= 20);

    // Blast one crate with an impulse far beyond any threshold.
    ecs::Entity victim = *destructibles.begin();
    const auto* t = w.get<scene::Transform>(victim);
    NF_CHECK(t != nullptr);
    destruction::DamageEvent event;
    event.world_point = Vec3{t->world_x, t->world_y, t->world_z};
    event.radius = 3.0f;
    event.impulse = 1000.0f;
    const u32 shards = h.runtime->apply_damage(victim, event);
    NF_LOG_INFO(LogCategory::Core, "[perf] blast shards spawned: {}", shards);
    NF_CHECK(shards > 0);
    NF_CHECK(h.runtime->bonds_shattered_total() > 0);

    // The debris world steps inside update() without breaking the frame.
    for (u32 i = 0; i < 30; ++i) h.runtime->update(1.0f / 60.0f);

    h.teardown();
}

// --- The gate contract itself -------------------------------------------------
//
// The GPU tests above can only gate frame time on the machine that recorded it,
// so on any other machine (a CI runner, a colleague's laptop) the timing rows
// are reported rather than enforced. That is the honest behaviour, but it means
// "a >10% regression fails loudly" must be provable WITHOUT a GPU and WITHOUT
// being the recording machine. These tests do exactly that: they drive the same
// comparison CI drives, with values that are deliberately over the line.
//
// A gate nobody has ever seen reject anything is a decoration.

NF_TEST(perf_gate_rejects_a_ten_percent_regression) {
    const std::vector<BaselineRow> rows{
        BaselineRow{"draw_calls", "any", 100.0, 10.0},
    };

    // At the baseline: fine.
    NF_CHECK(evaluate_gate(rows, "draw_calls", 100.0, "some-machine").verdict == GateVerdict::Ok);
    // Exactly on the limit is still inside the contract — the rule is ">10%
    // fails", and 110 is not more than 10% above 100.
    NF_CHECK(evaluate_gate(rows, "draw_calls", 110.0, "some-machine").verdict == GateVerdict::Ok);
    // 11% over: rejected.
    const GateResult r = evaluate_gate(rows, "draw_calls", 111.0, "some-machine");
    NF_CHECK(r.verdict == GateVerdict::Regressed);
    NF_CHECK_NEAR(r.limit, 110.0, 1e-9);
    NF_CHECK(r.over_pct > 10.0);
    // The shape of a real regression — twice the draw calls — is rejected too.
    NF_CHECK(evaluate_gate(rows, "draw_calls", 200.0, "some-machine").verdict == GateVerdict::Regressed);
}

NF_TEST(perf_gate_rejects_an_unbaselined_metric) {
    const std::vector<BaselineRow> rows{
        BaselineRow{"draw_calls", "any", 100.0, 10.0},
    };
    // A metric the table does not mention is a hole, and a hole is not a pass.
    NF_CHECK(evaluate_gate(rows, "frame_time_ms", 1.0, "any-machine").verdict ==
             GateVerdict::MissingBaseline);
    // An empty table is every metric at once being a hole.
    const std::vector<BaselineRow> empty;
    NF_CHECK(evaluate_gate(empty, "draw_calls", 100.0, "any-machine").verdict ==
             GateVerdict::MissingBaseline);
}

NF_TEST(perf_gate_enforces_any_rows_on_every_machine) {
    const std::vector<BaselineRow> rows{
        BaselineRow{"working_set_delta_mb", "any", 50.0, 10.0},
    };
    // `any` means any: a CI runner gets the same verdict as the recording box.
    NF_CHECK(evaluate_gate(rows, "working_set_delta_mb", 50.0, "ci-lavapipe").verdict == GateVerdict::Ok);
    NF_CHECK(evaluate_gate(rows, "working_set_delta_mb", 55.1, "ci-lavapipe").verdict ==
             GateVerdict::Regressed);
    NF_CHECK(evaluate_gate(rows, "working_set_delta_mb", 55.1, "win11-x64-dev").verdict ==
             GateVerdict::Regressed);
}

NF_TEST(perf_gate_reports_but_does_not_fail_on_another_machines_timing) {
    const std::vector<BaselineRow> rows{
        BaselineRow{"frame_time_ms", "win11-x64-dev", 85.0, 10.0},
    };
    // A 5x slower run on a different machine must NOT fail the build: an
    // absolute time measured on one box says nothing about another. It is
    // reported, never silently ignored.
    NF_CHECK(evaluate_gate(rows, "frame_time_ms", 425.0, "ci-lavapipe").verdict == GateVerdict::ReportOnly);
    // On the machine that recorded it, the same 5x regression is rejected.
    NF_CHECK(evaluate_gate(rows, "frame_time_ms", 425.0, "win11-x64-dev").verdict == GateVerdict::Regressed);
}

// The wired path, not just the comparison: a baseline file on disk, the gate
// switch on, and a metric deliberately over the line. This is the end-to-end
// proof that a regression fails loudly — it runs on every machine, GPU or not.
NF_TEST(perf_gate_fails_loudly_on_a_deliberate_regression) {
    const std::filesystem::path csv =
        std::filesystem::temp_directory_path() / "nf_perf_gate_selftest.csv";
    {
        std::ofstream out(csv);
        NF_CHECK(static_cast<bool>(out));
        out << "# deliberate regression fixture for perf_gate_fails_loudly_on_a_deliberate_regression\n";
        out << "draw_calls,any,100,10\n";
    }

    const std::string prev_gate = env_or("NF_PERF_GATE", "");
    const std::string prev_baseline = env_or("NF_PERF_BASELINE", "");
    const std::string prev_machine = env_or("NF_PERF_MACHINE", "");

    auto set_env = [](const char* key, const std::string& v) {
#ifdef _WIN32
        _putenv_s(key, v.c_str());
#else
        setenv(key, v.c_str(), 1);
#endif
    };

    set_env("NF_PERF_GATE", "1");
    set_env("NF_PERF_BASELINE", csv.string());
    set_env("NF_PERF_MACHINE", "selftest");

    bool threw = false;
    std::string message;
    try {
        enforce_metric("draw_calls", 111.0); // 11% over the 100 baseline
    } catch (const std::exception& e) {
        threw = true;
        message = e.what();
    }
    NF_CHECK(threw);
    NF_CHECK(message.find("PERF GATE FAILED") != std::string::npos);
    NF_CHECK(message.find("draw_calls") != std::string::npos);

    // The same wired path accepts a value inside tolerance — so the failure
    // above is the comparison biting, not the harness throwing at everything.
    bool threw_when_ok = false;
    try {
        enforce_metric("draw_calls", 109.0);
    } catch (const std::exception&) {
        threw_when_ok = true;
    }
    NF_CHECK(!threw_when_ok);

    // Restore, so no later test inherits a gate or a fixture path.
    set_env("NF_PERF_GATE", prev_gate);
    set_env("NF_PERF_BASELINE", prev_baseline);
    set_env("NF_PERF_MACHINE", prev_machine);
    std::filesystem::remove(csv);
}
