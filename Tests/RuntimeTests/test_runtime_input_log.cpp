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
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

#include <NF/Gameplay/Components.hpp>
#include <NF/Gameplay/GameplayModule.hpp>

#include <filesystem>
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
// The log itself
// ---------------------------------------------------------------------------

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
