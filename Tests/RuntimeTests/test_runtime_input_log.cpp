// Tests/RuntimeTests/test_runtime_input_log.cpp — Phase 10, W4 (deterministic input)
//
// A save restores a scene's *state*. An input log restores its *behaviour*: the
// exact stream of reads a session made, replayed into a freshly loaded scene so
// the same simulation runs to the same place. These tests prove the round trip
// through Runtime::update() — Rule 0 — not the log's internals, because a log
// that serializes perfectly and never drives the runtime is not a replay at all.
//
// The shape of every test here is the same: run a session with a scripted input
// source, capture it, then reload the scene and replay the log into a source
// that answers nothing. If the entity ends up in the same place, the log carried
// everything the simulation consumed.

#include <NF/Test/TestFramework.hpp>

#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Runtime/InputLog.hpp>
#include <NF/Runtime/Application.hpp>
#include <NF/Runtime/RunConfigResolver.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

#include <NF/Gameplay/Components.hpp>
#include <NF/Gameplay/GameplayModule.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace nf;
using namespace nf::gameplay;
using namespace nf::runtime;
using namespace nf::assets;

namespace {

/// An input source scripted ahead of time. Recording over this is the point: a
/// real device cannot be replayed, so the log is the only thing that carries the
/// session's inputs from one run of the scene to the next.
class ScriptedInput final : public IInputSource {
public:
    void press(const std::string& action) { m_pressed.insert(action); }
    void axis(const std::string& action, f32 v) { m_axes[action] = v; }

    bool action_pressed(std::string_view action) const override {
        return m_pressed.count(std::string(action)) > 0;
    }
    f32 action_axis(std::string_view action) const override {
        const auto it = m_axes.find(std::string(action));
        return it == m_axes.end() ? 0.0f : it->second;
    }

private:
    std::unordered_set<std::string>      m_pressed;
    std::unordered_map<std::string, f32> m_axes;
};

/// Reads "move_x" every update and slides an entity by it. The axis is read
/// *every* frame whether or not it is being used, because a log that only
/// recorded non-zero reads could not distinguish "held still" from "not polled".
class MoverModule final : public GameplayModule {
public:
    [[nodiscard]] const char* name() const override { return "Mover"; }

    void on_scene_load(GameplayContext& ctx) override {
        // The entity this module owns is created per scene, not once in
        // on_init: a reload destroys the world it lived in, and an id that
        // survived across that would point at a different entity or nothing.
        if (ctx.scene == nullptr) return;
        m_entity = ctx.scene->world().create_entity();
        scene::Transform t{};
        t.local_x = 0.0f;
        t.world_x = 0.0f;
        ctx.scene->world().add<scene::Transform>(m_entity, t);
    }

    void on_scene_unload(GameplayContext& /*ctx*/) override { m_entity = ecs::Entity{}; }

    void on_update(GameplayContext& ctx) override {
        if (ctx.scene == nullptr || !m_entity.valid()) return;
        // The one read the simulation makes. Everything the log has to carry is
        // this single query.
        const f32 x = (ctx.input != nullptr) ? ctx.input->action_axis("move_x") : 0.0f;
        scene::Transform* t = ctx.scene->world().get<scene::Transform>(m_entity);
        if (t == nullptr) return;
        t->local_x += x * ctx.dt;
        t->world_x = t->local_x;
        m_last_read = x;
    }

    ecs::Entity entity() const { return m_entity; }
    f32 last_read() const { return m_last_read; }

private:
    ecs::Entity m_entity;
    f32 m_last_read = 0.0f;
};

/// Headless device + runtime + a scene that has nothing in it but the module
/// component. Kept deliberately bare: the only thing under test is input flow.
class InputHarness {
public:
    explicit InputHarness(const std::string& tag) {
        m_tmp = std::filesystem::temp_directory_path() / ("nf_input_" + tag);
        std::filesystem::remove_all(m_tmp);
        std::filesystem::create_directories(m_tmp / "Content" / "Scenes");
        m_vfs.mount("content://", m_tmp / "Content");
    }

    ~InputHarness() {
        m_rt.reset();
        if (m_device) {
            m_device->wait_idle();
            m_device->shutdown();
        }
        std::filesystem::remove_all(m_tmp);
    }

    InputHarness(const InputHarness&) = delete;
    InputHarness& operator=(const InputHarness&) = delete;

    [[nodiscard]] bool ready() {
        m_device = rhi::create_device();
        if (!m_device) return false;
        rhi::DeviceDesc desc{};
        desc.window_handle = nullptr;
        if (!m_device->init(desc)) {
            m_device.reset();
            return false;
        }
        m_manager = std::make_unique<AssetManager>(m_vfs, m_reg);
        m_rt = std::make_unique<Runtime>(m_vfs, m_reg, *m_manager, *m_device, nullptr);
        return true;
    }

    void write_scene(const std::string& logical) {
        scene::Scene s("InputScene");
        auto& w = s.world();
        ecs::Entity e = w.create_entity();
        GameplayModuleComponent comp;
        comp.module_name = "Mover";
        comp.enabled = true;
        w.add<GameplayModuleComponent>(e, std::move(comp));
        std::string err;
        NF_CHECK(save_scene_to_vfs(m_vfs, logical, s, err));
    }

    [[nodiscard]] VirtualFileSystem& vfs() { return m_vfs; }
    [[nodiscard]] Runtime& rt() { return *m_rt; }

private:
    std::filesystem::path                 m_tmp;
    VirtualFileSystem                     m_vfs;
    AssetRegistry                         m_reg;
    std::unique_ptr<rhi::IGraphicsDevice> m_device;
    std::unique_ptr<AssetManager>         m_manager;
    std::unique_ptr<Runtime>              m_rt;
};

} // namespace

// ---------------------------------------------------------------------------
// The CLI route: `--input-log` / `--replay` (G5 gap #2)
//
// Everything below drives the recorder and the replay through the C++ API.
// That was the whole of the reachability problem: `InputLog`, `InputRecorder`,
// `InputReplay`, `begin_input_capture()` and `play_input_log()` were all built,
// documented and tested — and a *shipped game* could not touch any of them,
// because no command-line flag led there. `PlayerController` polls an action and,
// with no input source installed (which is every headless run), polls nothing and
// does nothing. So "the player moved" could be asserted only from C++.
//
// These tests exercise the layer a game actually uses: a log file on disk, and
// the validation that decides whether the run may start at all.
// ---------------------------------------------------------------------------

namespace {

/// Write a log file holding `frames` frames that drive `move_x` at `value`, in
/// the engine's own text form. Written by hand rather than by a recorder so the
/// test does not depend on the very capture path it feeds.
void write_move_log(const std::filesystem::path& file, int frames, float value) {
    InputLog log;
    for (int i = 0; i < frames; ++i) {
        log.write("move_x", value);
        log.seal(static_cast<u64>(i));
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << log.serialize();
    out.close();
}

} // namespace

NF_TEST(replay_flag_loads_a_log_file_from_disk) {
    auto dir = std::filesystem::temp_directory_path() / "nf_replay_flag_load";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto log_file = dir / "session.nfinput";
    write_move_log(log_file, 5, 1.0f);

    ApplicationConfig cfg;
    cfg.input_replay_path = log_file.string();

    InputLog loaded;
    ReplayProbe probe = ReplayProbe::NotConfigured;
    std::string err;
    NF_CHECK(prepare_input_replay(cfg, loaded, probe, err));
    NF_CHECK(err.empty());
    NF_CHECK(probe == ReplayProbe::Loaded);
    NF_CHECK_EQ(loaded.frame_count(), 5u);
    NF_CHECK_NEAR(loaded.value_at(0, "move_x"), 1.0f, 1e-6f);

    std::filesystem::remove_all(dir);
}

NF_TEST(replay_flag_rejects_a_missing_file) {
    auto dir = std::filesystem::temp_directory_path() / "nf_replay_flag_missing";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    ApplicationConfig cfg;
    cfg.input_replay_path = (dir / "nope.nfinput").string();

    InputLog loaded;
    ReplayProbe probe = ReplayProbe::NotConfigured;
    std::string err;
    // Reported, not accepted: a run that silently replayed nothing would exit 0
    // having demonstrated nothing at all.
    NF_CHECK(!prepare_input_replay(cfg, loaded, probe, err));
    NF_CHECK(probe == ReplayProbe::Unreadable);
    NF_CHECK(err.find("--replay") != std::string::npos);

    std::filesystem::remove_all(dir);
}

NF_TEST(replay_flag_distinguishes_an_empty_log_from_a_corrupt_one) {
    auto dir = std::filesystem::temp_directory_path() / "nf_replay_flag_corrupt";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    // Both files are unusable and both must be refused, but they are different
    // mistakes: one is "you pointed me at nothing", the other is "this is not a
    // log". `deserialize` answers an empty log for both, so the distinction has
    // to be made from the file's own bytes.
    {
        ApplicationConfig cfg;
        cfg.input_replay_path = (dir / "empty.nfinput").string();
        { std::ofstream(dir / "empty.nfinput", std::ios::binary); }

        InputLog loaded;
        ReplayProbe probe = ReplayProbe::NotConfigured;
        std::string err;
        NF_CHECK(!prepare_input_replay(cfg, loaded, probe, err));
        NF_CHECK(probe == ReplayProbe::EmptyLog);
        NF_CHECK(err.find("empty") != std::string::npos);
    }
    {
        ApplicationConfig cfg;
        cfg.input_replay_path = (dir / "corrupt.nfinput").string();
        {
            std::ofstream f(dir / "corrupt.nfinput", std::ios::binary);
            // Frames out of order: deserialize refuses the whole log rather than
            // replaying it at the wrong time.
            f << "# NOVAForge InputLog v1\nframe_count: 2\n@0 a=1\n@5 b=2\n";
        }

        InputLog loaded;
        ReplayProbe probe = ReplayProbe::NotConfigured;
        std::string err;
        NF_CHECK(!prepare_input_replay(cfg, loaded, probe, err));
        NF_CHECK(probe == ReplayProbe::RejectedLog);
        NF_CHECK(err.find("not a valid input log") != std::string::npos);
    }

    std::filesystem::remove_all(dir);
}

NF_TEST(recording_and_replaying_at_once_is_refused) {
    // Recording a replay would log the replay back into itself. The recorder and
    // the replay source are mutually exclusive by construction, so the
    // combination is refused by name instead of one flag quietly winning.
    ApplicationConfig cfg;
    cfg.input_log_path = "out.nfinput";
    cfg.input_replay_path = "in.nfinput";

    InputLog loaded;
    ReplayProbe probe = ReplayProbe::NotConfigured;
    std::string err;
    NF_CHECK(!prepare_input_replay(cfg, loaded, probe, err));
    NF_CHECK(err.find("--input-log") != std::string::npos);
    NF_CHECK(err.find("--replay") != std::string::npos);
}

NF_TEST(no_replay_flags_is_a_valid_configuration) {
    // The overwhelmingly common case must keep working, and must not be treated
    // as "a replay was requested and found nothing".
    ApplicationConfig cfg;

    InputLog loaded;
    ReplayProbe probe = ReplayProbe::Loaded;  // wrong on purpose; must be overwritten
    std::string err = "untouched";
    NF_CHECK(prepare_input_replay(cfg, loaded, probe, err));
    NF_CHECK(probe == ReplayProbe::NotConfigured);
    NF_CHECK(err == "untouched");
    NF_CHECK_EQ(loaded.frame_count(), 0u);
}

NF_TEST(a_log_from_disk_drives_the_entity_through_a_fresh_runtime) {
    // The end-to-end claim: a file on disk, with no input source installed at
    // all, moves an entity. This is the assertion the gap prevented — before the
    // CLI route only a C++ host could produce it, and a packaged game could not
    // demonstrate its own gameplay at all.
    InputHarness harness("replay_from_file");
    if (!harness.ready()) {
        NF_SKIP("no Vulkan device available");
    }
    harness.write_scene("content://Scenes/Input.nfscene");

    auto dir = std::filesystem::temp_directory_path() / "nf_replay_from_file";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto log_file = dir / "drive.nfinput";
    // 60 frames at axis 1.0 with dt 1/60 = one full unit of travel.
    write_move_log(log_file, 60, 1.0f);

    ApplicationConfig cfg;
    cfg.input_replay_path = log_file.string();

    InputLog loaded;
    ReplayProbe probe = ReplayProbe::NotConfigured;
    std::string err;
    NF_CHECK(prepare_input_replay(cfg, loaded, probe, err));
    NF_CHECK(probe == ReplayProbe::Loaded);

    Runtime& rt = harness.rt();
    auto module = std::make_unique<MoverModule>();
    MoverModule* mover = module.get();
    rt.add_gameplay_module(std::move(module));
    rt.set_playing(true);

    std::string load_err;
    NF_CHECK(rt.load_scene("content://Scenes/Input.nfscene", load_err));

    // No set_input_source() call anywhere: the replay IS the input source, which
    // is the whole point on a headless run where no keyboard exists.
    NF_CHECK(rt.input_source() == nullptr);
    rt.play_input_log(loaded);
    NF_CHECK(rt.input_replay_active());
    NF_CHECK(rt.input_source() != nullptr);

    for (int i = 0; i < 60; ++i) {
        rt.update(1.0f / 60.0f);
    }

    // The module polled "move_x" and got the recorded value, so the entity moved.
    // A log that parsed but never reached the simulation would leave this at 0.
    NF_CHECK_NEAR(mover->last_read(), 1.0f, 1e-6f);
    const ecs::World& w = rt.edit_scene()->world();
    const scene::Transform* t = w.get<scene::Transform>(mover->entity());
    NF_CHECK(t != nullptr);
    if (t != nullptr) {
        NF_CHECK_NEAR(t->local_x, 1.0f, 1e-3f);
    }

    rt.stop_input_replay();
    NF_CHECK(!rt.input_replay_active());
    // Unwinding must restore the source that was live before — here, none.
    NF_CHECK(rt.input_source() == nullptr);

    std::filesystem::remove_all(dir);
}

NF_TEST(a_replay_advances_the_module_against_the_frame_index) {
    // The log is a timeline, not a soup of inputs: frame 5 of the run must be
    // answered by frame 5 of the log. Serving the last frame forever, or always
    // serving frame 0, would both "work" in the sense of producing movement while
    // being a completely wrong replay.
    InputHarness harness("replay_timeline");
    if (!harness.ready()) {
        NF_SKIP("no Vulkan device available");
    }
    harness.write_scene("content://Scenes/Input.nfscene");

    InputLog log;
    log.write("move_x", 1.0f);
    log.seal(0);
    log.write("move_x", 0.0f);
    log.seal(1);
    log.write("move_x", -1.0f);
    log.seal(2);

    Runtime& rt = harness.rt();
    auto module = std::make_unique<MoverModule>();
    MoverModule* mover = module.get();
    rt.add_gameplay_module(std::move(module));
    rt.set_playing(true);

    std::string load_err;
    NF_CHECK(rt.load_scene("content://Scenes/Input.nfscene", load_err));
    rt.play_input_log(log);

    rt.update(1.0f / 60.0f);
    NF_CHECK_NEAR(mover->last_read(), 1.0f, 1e-6f);
    rt.update(1.0f / 60.0f);
    NF_CHECK_NEAR(mover->last_read(), 0.0f, 1e-6f);
    rt.update(1.0f / 60.0f);
    NF_CHECK_NEAR(mover->last_read(), -1.0f, 1e-6f);
    // Past the end the last recorded frame is held, never invented input.
    rt.update(1.0f / 60.0f);
    NF_CHECK_NEAR(mover->last_read(), -1.0f, 1e-6f);

    rt.stop_input_replay();
}

NF_TEST(a_recorded_log_replays_to_the_same_place_it_was_recorded) {
    // The property the whole subsystem exists for: record a session, then prove a
    // second run of a freshly loaded scene ends in the same place. If the file
    // route lost anything, this is where it would show — and unlike the tests
    // above it covers the round trip through disk, which is the shipped path.
    InputHarness harness("replay_determinism");
    if (!harness.ready()) {
        NF_SKIP("no Vulkan device available");
    }
    harness.write_scene("content://Scenes/Input.nfscene");

    auto dir = std::filesystem::temp_directory_path() / "nf_replay_determinism";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto log_file = dir / "session.nfinput";

    float recorded_x = 0.0f;

    // --- Session one: a scripted source, recorded to a file on disk. ---------
    {
        ScriptedInput scripted;
        scripted.axis("move_x", 0.75f);

        Runtime& rt = harness.rt();
        auto module = std::make_unique<MoverModule>();
        MoverModule* mover = module.get();
        rt.add_gameplay_module(std::move(module));
        rt.set_playing(true);

        std::string load_err;
        NF_CHECK(rt.load_scene("content://Scenes/Input.nfscene", load_err));
        rt.set_input_source(&scripted);
        rt.begin_input_capture();
        NF_CHECK(rt.input_capture_active());

        for (int i = 0; i < 30; ++i) {
            rt.update(1.0f / 60.0f);
        }
        const InputLog recorded = rt.end_input_capture();
        NF_CHECK(!rt.input_capture_active());
        // "Nothing was polled" is itself a recordable frame, so a module that
        // reads nothing still produces a 30-frame log rather than an empty one.
        NF_CHECK_EQ(recorded.frame_count(), 30u);

        const ecs::World& w = rt.edit_scene()->world();
        const scene::Transform* t = w.get<scene::Transform>(mover->entity());
        NF_CHECK(t != nullptr);
        recorded_x = t != nullptr ? t->local_x : 0.0f;

        { std::ofstream f(log_file, std::ios::binary | std::ios::trunc); f << recorded.serialize(); }
    }

    // --- Session two: fresh scene, replay from that file. --------------------
    {
        ApplicationConfig cfg;
        cfg.input_replay_path = log_file.string();
        InputLog loaded;
        ReplayProbe probe = ReplayProbe::NotConfigured;
        std::string err;
        NF_CHECK(prepare_input_replay(cfg, loaded, probe, err));
        NF_CHECK(probe == ReplayProbe::Loaded);

        Runtime& rt2 = harness.rt();
        auto module2 = std::make_unique<MoverModule>();
        MoverModule* mover2 = module2.get();
        rt2.add_gameplay_module(std::move(module2));
        rt2.set_playing(true);
        std::string load_err;
        NF_CHECK(rt2.load_scene("content://Scenes/Input.nfscene", load_err));
        rt2.play_input_log(loaded);
        for (int i = 0; i < 30; ++i) {
            rt2.update(1.0f / 60.0f);
        }

        const ecs::World& w2 = rt2.edit_scene()->world();
        const scene::Transform* t2 = w2.get<scene::Transform>(mover2->entity());
        NF_CHECK(t2 != nullptr);
        if (t2 != nullptr) {
            // 0.75 * 30 frames * 1/60 s = 0.375, and the same twice: the replay
            // landed exactly where the recording did.
            NF_CHECK_NEAR(t2->local_x, recorded_x, 1e-6f);
            NF_CHECK_NEAR(t2->local_x, 0.375f, 1e-4f);
        }
        rt2.stop_input_replay();
    }

    std::filesystem::remove_all(dir);
}

NF_TEST(an_input_log_round_trips_through_text_unchanged) {
    InputLog log;
    log.write("move_x", 0.5f);
    log.write("jump", 1.0f);
    log.seal(0);
    // A frame in which nothing was polled is still a frame, and still sealed.
    log.seal(1);
    log.write("move_x", -0.25f);
    log.seal(2);

    NF_CHECK_EQ(log.frame_count(), 3u);
    NF_CHECK_NEAR(log.value_at(0, "move_x"), 0.5f, 1e-6f);
    NF_CHECK(log.value_at(0, "jump") != 0.0f);
    // An action not read in a frame is zero, which is what a replay needs to
    // answer for an action the scene never polled.
    NF_CHECK_NEAR(log.value_at(1, "move_x"), 0.0f, 1e-6f);
    NF_CHECK_NEAR(log.value_at(2, "move_x"), -0.25f, 1e-6f);

    const std::string text = log.serialize();
    const InputLog back = InputLog::deserialize(text);

    NF_CHECK_EQ(back.frame_count(), 3u);
    NF_CHECK_NEAR(back.value_at(0, "move_x"), 0.5f, 1e-6f);
    NF_CHECK(back.value_at(0, "jump") != 0.0f);
    NF_CHECK_NEAR(back.value_at(2, "move_x"), -0.25f, 1e-6f);

    // The serialized form is the log's contract: two runs of the same session
    // must produce identical bytes, or a save containing a log is not
    // reproducible.
    NF_CHECK_EQ(back.serialize(), text);
}

NF_TEST(an_input_log_holds_values_at_nine_significant_digits) {
    InputLog log;
    // A value the old six-digit writer would have flattened. The log carries
    // floats, and a replayed 0.0999999 moves an entity a different distance
    // than the 0.1 that was recorded — which defeats the log's only purpose.
    log.write("move_x", 0.1234567f);
    log.seal(0);

    const std::string text = log.serialize();
    NF_CHECK(text.find("0.1234567") != std::string::npos);

    const InputLog back = InputLog::deserialize(text);
    NF_CHECK_EQ(back.value_at(0, "move_x"), 0.1234567f);
}

NF_TEST(a_reordered_input_log_is_refused_not_mis_timed) {
    InputLog log;
    log.write("move_x", 1.0f);
    log.seal(0);
    log.write("move_x", 2.0f);
    log.seal(1);

    // Two frames in the wrong order. The frame index and the entry count must
    // agree, otherwise a log whose lines were reordered would silently replay
    // at the wrong time — a desync that looks exactly like a correct replay.
    // Built by hand rather than by mangling the good text, so the wrongness
    // under test is the ordering itself and not an accidental malformed line.
    std::string bad;
    bad += "# NOVAForge InputLog v1\n";
    bad += "frame_count: 2\n";
    bad += "@1 move_x=2\n";
    bad += "@0 move_x=1\n";
    NF_CHECK_EQ(InputLog::deserialize(bad).frame_count(), 0u);

    // The same two frames in the right order load, so it is the ordering that
    // is being refused and not the contents.
    std::string good;
    good += "# NOVAForge InputLog v1\n";
    good += "frame_count: 2\n";
    good += "@0 move_x=1\n";
    good += "@1 move_x=2\n";
    NF_CHECK_EQ(InputLog::deserialize(good).frame_count(), 2u);
}

NF_TEST(a_replay_clamps_at_the_end_of_the_log_rather_than_wrapping) {
    InputLog log;
    log.write("move_x", 1.0f);
    log.seal(0);
    // A frame in which nothing was polled is still sealed and still counts, so
    // it is what the end of the log looks like — not the last input that existed.
    log.seal(1);

    InputReplay replay(log);
    replay.advance(0);
    NF_CHECK(replay.action_pressed("move_x"));
    NF_CHECK_EQ(replay.current_frame(), 0u);

    replay.advance(5);
    // Past the end, the last frame is held as it was recorded. Two wrong
    // alternatives look the same as this one from outside: wrapping the log back
    // to frame 0, and reaching back for the last frame that had input. The first
    // would report frame 0; the second would report a held button the session
    // had stopped reading.
    NF_CHECK_EQ(replay.current_frame(), 1u);
    NF_CHECK(!replay.action_pressed("move_x"));
    NF_CHECK_NEAR(replay.action_axis("move_x"), 0.0f, 1e-6f);

    // A log whose final frame *does* hold input replays it past the end — this
    // is the case the runtime's partial-log test depends on, and the reason the
    // clamp targets the last frame rather than stopping the replay outright.
    InputLog held;
    held.write("move_x", 2.0f);
    held.seal(0);
    InputReplay held_replay(held);
    held_replay.advance(9);
    NF_CHECK_EQ(held_replay.current_frame(), 0u);
    NF_CHECK_NEAR(held_replay.action_axis("move_x"), 2.0f, 1e-6f);
}

// ---------------------------------------------------------------------------
// Through Runtime::update()
// ---------------------------------------------------------------------------

NF_TEST(capture_records_what_the_modules_actually_polled) {
    InputHarness harness("capture");
    if (!harness.ready()) NF_SKIP("no Vulkan device available");

    harness.write_scene("content://Scenes/Main.nfscene");

    auto module = std::make_unique<MoverModule>();
    MoverModule* mover = module.get();
    harness.rt().add_gameplay_module(std::move(module));

    ScriptedInput scripted;
    scripted.axis("move_x", 3.0f);
    harness.rt().set_input_source(&scripted);

    std::string err;
    NF_CHECK(harness.rt().load_scene("content://Scenes/Main.nfscene", err));

    harness.rt().begin_input_capture();
    // The recorder wraps the scripted source, so gameplay still sees live input
    // while the log fills.
    NF_CHECK(harness.rt().input_capture_active());
    for (u32 i = 0; i < 10u; ++i) harness.rt().update(0.1f);

    const InputLog log = harness.rt().end_input_capture();
    NF_CHECK(!harness.rt().input_capture_active());

    // Ten steps, ten sealed frames — and the one action that was polled, at the
    // value it held, in every one of them.
    NF_CHECK_EQ(log.frame_count(), 10u);
    for (u64 f = 0; f < 10u; ++f) {
        NF_CHECK_NEAR(log.value_at(f, "move_x"), 3.0f, 1e-6f);
    }
    NF_CHECK_NEAR(mover->last_read(), 3.0f, 1e-6f);
    // 3.0 * 0.1 * 10 = 3.0, which is what a replay has to reproduce.
    NF_CHECK_NEAR(harness.rt().scene()->world().get<scene::Transform>(mover->entity())->local_x,
                  3.0f, 1e-4f);
}

NF_TEST(a_replayed_log_reproduces_the_original_session) {
    InputHarness harness("replay");
    if (!harness.ready()) NF_SKIP("no Vulkan device available");

    harness.write_scene("content://Scenes/Main.nfscene");

    auto module = std::make_unique<MoverModule>();
    MoverModule* mover = module.get();
    harness.rt().add_gameplay_module(std::move(module));

    ScriptedInput scripted;
    scripted.axis("move_x", 3.0f);
    harness.rt().set_input_source(&scripted);

    std::string err;
    NF_CHECK(harness.rt().load_scene("content://Scenes/Main.nfscene", err));

    // --- the original session ------------------------------------------------
    harness.rt().begin_input_capture();
    for (u32 i = 0; i < 10u; ++i) harness.rt().update(0.1f);
    const InputLog log = harness.rt().end_input_capture();
    const std::string text = log.serialize();

    // --- the same scene, freshly loaded, no live input at all ----------------
    NF_CHECK(harness.rt().load_scene("content://Scenes/Main.nfscene", err));

    const InputLog replay_log = InputLog::deserialize(text);
    harness.rt().play_input_log(replay_log);
    NF_CHECK(harness.rt().input_replay_active());
    // Note the deliberate absence of any ScriptedInput here: the log is the only
    // source. If the entity still arrives at the same place, the log carried
    // everything the simulation consumed.
    for (u32 i = 0; i < 10u; ++i) harness.rt().update(0.1f);

    const scene::Transform* t2 =
        harness.rt().scene()->world().get<scene::Transform>(mover->entity());
    NF_CHECK(t2 != nullptr);
    if (t2 == nullptr) return;
    NF_CHECK_NEAR(t2->local_x, 3.0f, 1e-4f);
    NF_CHECK_NEAR(mover->last_read(), 3.0f, 1e-6f);
}

NF_TEST(a_replay_of_a_partial_log_holds_the_last_frame) {
    InputHarness harness("partial");
    if (!harness.ready()) NF_SKIP("no Vulkan device available");

    harness.write_scene("content://Scenes/Main.nfscene");

    auto module = std::make_unique<MoverModule>();
    MoverModule* mover = module.get();
    harness.rt().add_gameplay_module(std::move(module));

    ScriptedInput scripted;
    scripted.axis("move_x", 2.0f);
    harness.rt().set_input_source(&scripted);

    std::string err;
    NF_CHECK(harness.rt().load_scene("content://Scenes/Main.nfscene", err));

    harness.rt().begin_input_capture();
    for (u32 i = 0; i < 5u; ++i) harness.rt().update(0.1f);
    const InputLog log = harness.rt().end_input_capture();

    // Reload and run twice as many steps as the log has frames. The last frame
    // is held, so the mover keeps sliding at the recorded rate — the observable
    // difference between "clamped" and "wrapped" is whether it stops or starts
    // the log over again.
    NF_CHECK(harness.rt().load_scene("content://Scenes/Main.nfscene", err));
    harness.rt().play_input_log(log);
    for (u32 i = 0; i < 10u; ++i) harness.rt().update(0.1f);

    // 5 frames at 2.0 * 0.1 = 1.0, then 5 more held at the same rate = 2.0.
    const scene::Transform* t2 =
        harness.rt().scene()->world().get<scene::Transform>(mover->entity());
    NF_CHECK(t2 != nullptr);
    if (t2 == nullptr) return;
    NF_CHECK_NEAR(t2->local_x, 2.0f, 1e-4f);
    NF_CHECK_NEAR(mover->last_read(), 2.0f, 1e-6f);
}

NF_TEST(stopping_a_replay_restores_the_prior_input_source) {
    InputHarness harness("stop");
    if (!harness.ready()) NF_SKIP("no Vulkan device available");

    harness.write_scene("content://Scenes/Main.nfscene");
    harness.rt().add_gameplay_module(std::make_unique<MoverModule>());

    std::string err;
    NF_CHECK(harness.rt().load_scene("content://Scenes/Main.nfscene", err));

    ScriptedInput scripted;
    scripted.axis("move_x", 1.0f);
    harness.rt().set_input_source(&scripted);
    NF_CHECK_EQ(harness.rt().input_source(), &scripted);

    InputLog log;
    log.write("move_x", 5.0f);
    log.seal(0);
    harness.rt().play_input_log(log);
    NF_CHECK(harness.rt().input_source() != &scripted);

    harness.rt().stop_input_replay();
    NF_CHECK(!harness.rt().input_replay_active());
    // The device that was live before the replay is the one live after it.
    NF_CHECK_EQ(harness.rt().input_source(), &scripted);
}

NF_TEST(capture_over_a_capture_seals_one_log_not_two) {
    InputHarness harness("nested");
    if (!harness.ready()) NF_SKIP("no Vulkan device available");

    harness.write_scene("content://Scenes/Main.nfscene");
    harness.rt().add_gameplay_module(std::make_unique<MoverModule>());

    std::string err;
    NF_CHECK(harness.rt().load_scene("content://Scenes/Main.nfscene", err));

    ScriptedInput scripted;
    scripted.axis("move_x", 1.0f);
    harness.rt().set_input_source(&scripted);

    harness.rt().begin_input_capture();
    harness.rt().update(0.1f);
    // A second begin over an active capture wraps the device, not the old
    // recorder — otherwise the outer log would record the inner one's answers
    // and the same input would land in the log twice.
    harness.rt().begin_input_capture();
    harness.rt().update(0.1f);

    const InputLog log = harness.rt().end_input_capture();
    NF_CHECK_EQ(log.frame_count(), 1u);
    NF_CHECK_NEAR(log.value_at(0, "move_x"), 1.0f, 1e-6f);
    NF_CHECK(harness.rt().input_source() == &scripted);
}
