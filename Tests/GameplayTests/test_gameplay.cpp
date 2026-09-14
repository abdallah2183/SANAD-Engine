// Tests/GameplayTests/test_gameplay.cpp — Phase 10, W2 (gameplay modules)
//
// Two layers are covered, and they fail for different reasons:
//
//   * The registry and the state bridge are pure data — no GPU, no Runtime.
//   * The Runtime integration is the layer that decides whether a module runs at
//     all. A registry full of modules that nothing instantiates is the exact
//     defect Phase 9 shipped (a green module suite next to a subsystem that was
//     never stepped), so the assertions below are about *call counts on a probe
//     module*, not about the registry being populated.

#include <NF/Test/TestFramework.hpp>

#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

#include <NF/Gameplay/Components.hpp>
#include <NF/Gameplay/GameplayModule.hpp>
#include <NF/Gameplay/GameplayModuleRegistry.hpp>
#include <NF/Gameplay/GameplayState.hpp>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace nf;
using namespace nf::gameplay;
using namespace nf::runtime;
using namespace nf::assets;

namespace {

// ---------------------------------------------------------------------------
// Probes
// ---------------------------------------------------------------------------

/// Reflected settings for the probe. Every property is `SerializeField` so the
/// state bridge has something to move, and `gain` is also `EditAnywhere` so the
/// capture test can prove the two flags are independent.
struct ProbeSettings {
    f32  gain   = 1.0f;
    i32  ticks  = 0;
    bool active = true;

    NF_CLASS(ProbeSettings)
    NF_PROPERTY(ProbeSettings, gain,   Float, Prop_EditAnywhere | Prop_SerializeField, "Probe")
    NF_PROPERTY(ProbeSettings, ticks,  Int,   Prop_SerializeField,                     "Probe")
    NF_PROPERTY(ProbeSettings, active, Bool,  Prop_SerializeField,                     "Probe")
    NF_CLASS_END(ProbeSettings)
};

/// Settings where nothing is serializable, to pin the "capture writes only what
/// opted in" contract.
struct UnsavedSettings {
    f32 scratch = 5.0f;

    NF_CLASS(UnsavedSettings)
    NF_PROPERTY(UnsavedSettings, scratch, Float, Prop_EditAnywhere, "Scratch")
    NF_CLASS_END(UnsavedSettings)
};

/// Records every hook it receives. `update_order` is shared so a test can assert
/// the relative order of two probes rather than just that both ran.
std::vector<std::string> g_update_order;

/// Shutdown is counted outside the module on purpose: `shutdown_gameplay()`
/// destroys the modules, so a test that held a pointer across it would be
/// reading freed memory and would pass or fail depending on what the allocator
/// left behind.
u32 g_probe_shutdown_calls = 0;

class ProbeModule final : public GameplayModule {
public:
    ProbeModule(std::string probe_name = "Probe", f32 priority = 0.0f)
        : m_name(std::move(probe_name)), m_priority(priority) {}

    [[nodiscard]] const char* name() const override { return m_name.c_str(); }
    [[nodiscard]] f32 update_priority() const override { return m_priority; }

    void on_init(GameplayContext&) override { ++init_calls; }
    void on_update(GameplayContext&) override {
        ++update_calls;
        g_update_order.push_back(m_name);
    }
    void on_shutdown(GameplayContext&) override {
        ++shutdown_calls;
        ++g_probe_shutdown_calls;
    }
    void on_scene_load(GameplayContext&) override { ++scene_load_calls; }
    void on_scene_unload(GameplayContext&) override { ++scene_unload_calls; }

    GameplayStateBinding state() override { return {&settings, ProbeSettings::nf_class_meta()}; }

    ProbeSettings settings;
    u32 init_calls        = 0;
    u32 update_calls      = 0;
    u32 shutdown_calls    = 0;
    u32 scene_load_calls  = 0;
    u32 scene_unload_calls = 0;

private:
    std::string m_name;
    f32 m_priority = 0.0f;
};

/// A probe whose settings opt out of serialization entirely.
class UnsavedProbeModule final : public GameplayModule {
public:
    [[nodiscard]] const char* name() const override { return "UnsavedProbe"; }
    GameplayStateBinding state() override { return {&settings, UnsavedSettings::nf_class_meta()}; }
    UnsavedSettings settings;
};

/// Registered through the macro, so this one exercises the registration path the
/// engine itself uses rather than a hand-written factory.
class MacroProbeModule final : public GameplayModule {
public:
    [[nodiscard]] const char* name() const override { return "MacroProbe"; }
};

/// Counts how many times it was constructed, to prove a factory was actually
/// invoked rather than merely stored.
class CountingProbeModule final : public GameplayModule {
public:
    [[nodiscard]] const char* name() const override { return "CountingProbe"; }
    [[nodiscard]] static u32& constructions() {
        static u32 count = 0;
        return count;
    }
};

std::unique_ptr<GameplayModule> make_counting_probe() {
    ++CountingProbeModule::constructions();
    return std::make_unique<CountingProbeModule>();
}

/// Registers a probe under a name no other test uses. Names are per-test so the
/// suite never depends on which tests ran first.
GameplayModule* add_probe(Runtime& rt, const std::string& probe_name, f32 priority = 0.0f) {
    return rt.add_gameplay_module(std::make_unique<ProbeModule>(probe_name, priority));
}

/// Minimal scene: one entity with a transform. Gameplay modules do not need
/// geometry, and a scene with no meshes keeps these tests off the asset cooker.
void write_minimal_scene(VirtualFileSystem& vfs, const std::string& logical) {
    scene::Scene s("GameplayScene");
    auto& w = s.world();
    ecs::Entity e = w.create_entity();
    w.add<scene::Transform>(e, scene::Transform{});

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, logical, s, err));
}

/// Device + Runtime + temp content mount. Torn down in the destructor so a
/// failing NF_CHECK unwinds without leaking a Vulkan device.
class RuntimeHarness {
public:
    explicit RuntimeHarness(const std::string& tag) {
        m_tmp = std::filesystem::temp_directory_path() / ("nf_gameplay_" + tag);
        std::filesystem::remove_all(m_tmp);
        std::filesystem::create_directories(m_tmp);
        m_vfs.mount("content://", m_tmp);
    }

    ~RuntimeHarness() {
        m_rt.reset();
        if (m_device) {
            m_device->wait_idle();
            m_device->shutdown();
        }
        std::filesystem::remove_all(m_tmp);
    }

    RuntimeHarness(const RuntimeHarness&) = delete;
    RuntimeHarness& operator=(const RuntimeHarness&) = delete;

    /// False when there is no usable device, so the caller can NF_SKIP.
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

NF_GAMEPLAY_MODULE(MacroProbeModule, "MacroProbe")

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

NF_TEST(gameplay_registry_creates_a_registered_module) {
    GameplayModuleRegistry& registry = GameplayModuleRegistry::instance();
    registry.register_module("CountingProbe", &make_counting_probe);

    const u32 before = CountingProbeModule::constructions();
    auto module = registry.create("CountingProbe");

    NF_CHECK(module != nullptr);
    NF_CHECK_EQ(std::string(module->name()), std::string("CountingProbe"));
    // The factory ran, rather than the registry handing back a cached instance.
    NF_CHECK_EQ(CountingProbeModule::constructions(), before + 1);
}

NF_TEST(gameplay_registry_returns_null_for_an_unknown_name) {
    NF_CHECK(GameplayModuleRegistry::instance().create("NoSuchGameplayModule") == nullptr);
    NF_CHECK(!GameplayModuleRegistry::instance().contains("NoSuchGameplayModule"));
}

NF_TEST(gameplay_registry_keeps_the_first_registration_of_a_name) {
    GameplayModuleRegistry& registry = GameplayModuleRegistry::instance();
    registry.register_module("DuplicateProbe", &make_counting_probe);

    // A second registration under the same name must not replace the first:
    // otherwise the winner would depend on link order, which is not specified.
    registry.register_module("DuplicateProbe", nullptr);
    registry.register_module("DuplicateProbe", &make_counting_probe);

    u32 seen = 0;
    for (const std::string& name : registry.names()) {
        if (name == "DuplicateProbe") ++seen;
    }
    NF_CHECK_EQ(seen, 1u);
}

NF_TEST(gameplay_registry_rejects_empty_names_and_null_factories) {
    GameplayModuleRegistry& registry = GameplayModuleRegistry::instance();
    const usize before = registry.size();

    registry.register_module("", &make_counting_probe);
    registry.register_module("NullFactoryProbe", nullptr);

    NF_CHECK_EQ(registry.size(), before);
    NF_CHECK(!registry.contains(""));
    NF_CHECK(!registry.contains("NullFactoryProbe"));
}

NF_TEST(gameplay_registry_names_are_sorted) {
    const std::vector<std::string> names = GameplayModuleRegistry::instance().names();
    NF_CHECK(std::is_sorted(names.begin(), names.end()));
}

NF_TEST(gameplay_macro_registration_is_discoverable) {
    // Registration happened during static initialisation, with no engine code
    // naming this type — which is the whole point of the macro.
    GameplayModuleRegistry& registry = GameplayModuleRegistry::instance();
    NF_CHECK(registry.contains("MacroProbe"));

    auto module = registry.create("MacroProbe");
    NF_CHECK(module != nullptr);
    NF_CHECK_EQ(std::string(module->name()), std::string("MacroProbe"));
}

// ---------------------------------------------------------------------------
// State bridge
// ---------------------------------------------------------------------------

NF_TEST(gameplay_capture_writes_only_serializable_properties) {
    UnsavedProbeModule module;
    module.settings.scratch = 9.0f;

    std::unordered_map<std::string, std::string> properties;
    NF_CHECK(capture_state(module.state(), properties));

    // `scratch` is EditAnywhere but not SerializeField. The two flags mean
    // different things and this is the test that keeps them from collapsing.
    NF_CHECK_EQ(properties.size(), static_cast<size_t>(0));
}

NF_TEST(gameplay_state_round_trips_through_the_component_map) {
    ProbeModule source;
    source.settings.gain   = 2.5f;
    source.settings.ticks  = 7;
    source.settings.active = false;

    std::unordered_map<std::string, std::string> properties;
    NF_CHECK(capture_state(source.state(), properties));
    NF_CHECK_EQ(properties.size(), static_cast<size_t>(3));

    ProbeModule restored;
    NF_CHECK_EQ(apply_state(restored.state(), properties), 3u);
    NF_CHECK_NEAR(restored.settings.gain, 2.5f, 1e-5f);
    NF_CHECK_EQ(restored.settings.ticks, 7);
    NF_CHECK(!restored.settings.active);
}

NF_TEST(gameplay_apply_ignores_unknown_keys) {
    ProbeModule module;
    module.settings.gain = 1.0f;

    std::unordered_map<std::string, std::string> properties;
    properties.emplace("gain", "4");
    properties.emplace("not_a_property", "12");
    properties.emplace("also_missing", "hello");

    // Only the declared property is written; the count reports that, rather than
    // reporting the size of the input map.
    NF_CHECK_EQ(apply_state(module.state(), properties), 1u);
    NF_CHECK_NEAR(module.settings.gain, 4.0f, 1e-5f);
}

NF_TEST(gameplay_apply_skips_malformed_values_without_half_applying) {
    ProbeModule module;
    module.settings.gain   = 3.0f;
    module.settings.ticks  = 5;
    module.settings.active = true;

    std::unordered_map<std::string, std::string> properties;
    properties.emplace("gain", "not a number");
    properties.emplace("ticks", "11");
    properties.emplace("active", "perhaps");

    // One property lands, two are refused. The refused ones keep their old
    // values — a corrupt save must not leave a half-applied object.
    NF_CHECK_EQ(apply_state(module.state(), properties), 1u);
    NF_CHECK_NEAR(module.settings.gain, 3.0f, 1e-5f);
    NF_CHECK_EQ(module.settings.ticks, 11);
    NF_CHECK(module.settings.active);
}

NF_TEST(gameplay_capture_of_an_invalid_binding_is_refused) {
    GameplayStateBinding invalid{};
    NF_CHECK(!invalid.valid());

    std::unordered_map<std::string, std::string> properties;
    properties.emplace("leftover", "1");
    NF_CHECK(!capture_state(invalid, properties));
    // The map is cleared even on refusal, so a caller cannot mistake the
    // previous module's state for this one's.
    NF_CHECK(properties.empty());

    NF_CHECK_EQ(apply_state(invalid, properties), 0u);
}

// ---------------------------------------------------------------------------
// Runtime integration
// ---------------------------------------------------------------------------

NF_TEST(runtime_instantiates_and_steps_registered_modules) {
    RuntimeHarness harness("step");
    if (!harness.ready()) NF_SKIP("no Vulkan device available");

    Runtime& rt = harness.rt();
    ProbeModule* probe = dynamic_cast<ProbeModule*>(add_probe(rt, "RuntimeStepProbe"));
    NF_CHECK(probe != nullptr);

    write_minimal_scene(harness.vfs(), "content://Scenes/Gameplay.nfscene");
    std::string err;
    NF_CHECK(rt.load_scene("content://Scenes/Gameplay.nfscene", err));

    // on_init fires during load_scene, and the scene hook follows it.
    NF_CHECK_EQ(probe->init_calls, 1u);
    NF_CHECK_EQ(probe->scene_load_calls, 1u);

    NF_CHECK_EQ(probe->update_calls, 0u);
    NF_CHECK_EQ(rt.gameplay_updates(), static_cast<size_t>(0));

    for (int i = 0; i < 5; ++i) {
        rt.update(1.0f / 60.0f);
    }

    // The observable that separates "a module exists" from "a module runs".
    NF_CHECK_EQ(probe->update_calls, 5u);
    NF_CHECK(rt.gameplay_updates() >= 5u);
}

NF_TEST(runtime_steps_modules_in_priority_order_then_by_name) {
    RuntimeHarness harness("order");
    if (!harness.ready()) NF_SKIP("no Vulkan device available");

    Runtime& rt = harness.rt();
    // Deliberately added in the opposite order to the one they must run in.
    add_probe(rt, "OrderLate", 10.0f);
    add_probe(rt, "OrderEarly", -10.0f);

    write_minimal_scene(harness.vfs(), "content://Scenes/Gameplay.nfscene");
    std::string err;
    NF_CHECK(rt.load_scene("content://Scenes/Gameplay.nfscene", err));

    g_update_order.clear();
    rt.step_gameplay(1.0f / 60.0f);

    // Filter to this test's probes: the macro-registered module also runs.
    std::vector<std::string> mine;
    for (const std::string& entry : g_update_order) {
        if (entry == "OrderEarly" || entry == "OrderLate") mine.push_back(entry);
    }
    NF_CHECK_EQ(mine.size(), static_cast<size_t>(2));
    if (mine.size() == 2) {
        NF_CHECK_EQ(mine[0], std::string("OrderEarly"));
        NF_CHECK_EQ(mine[1], std::string("OrderLate"));
    }
}

NF_TEST(runtime_skips_a_module_whose_component_is_disabled) {
    RuntimeHarness harness("disabled");
    if (!harness.ready()) NF_SKIP("no Vulkan device available");

    Runtime& rt = harness.rt();
    ProbeModule* probe = dynamic_cast<ProbeModule*>(add_probe(rt, "RuntimeDisabledProbe"));
    NF_CHECK(probe != nullptr);

    write_minimal_scene(harness.vfs(), "content://Scenes/Gameplay.nfscene");
    std::string err;
    NF_CHECK(rt.load_scene("content://Scenes/Gameplay.nfscene", err));

    rt.step_gameplay(1.0f / 60.0f);
    NF_CHECK_EQ(probe->update_calls, 1u);

    // Disable it through the scene, the way the inspector would.
    auto* world = &rt.edit_scene()->world();
    ecs::Entity owner = world->create_entity();
    GameplayModuleComponent comp;
    comp.module_name = "RuntimeDisabledProbe";
    comp.enabled = false;
    world->add<GameplayModuleComponent>(owner, std::move(comp));

    rt.step_gameplay(1.0f / 60.0f);
    rt.step_gameplay(1.0f / 60.0f);
    NF_CHECK_EQ(probe->update_calls, 1u);
}

NF_TEST(runtime_gameplay_state_round_trips_through_the_scene) {
    RuntimeHarness harness("state");
    if (!harness.ready()) NF_SKIP("no Vulkan device available");

    Runtime& rt = harness.rt();
    ProbeModule* probe = dynamic_cast<ProbeModule*>(add_probe(rt, "RuntimeStateProbe"));
    NF_CHECK(probe != nullptr);
    probe->settings.gain   = 6.25f;
    probe->settings.ticks  = 42;
    probe->settings.active = false;

    write_minimal_scene(harness.vfs(), "content://Scenes/Gameplay.nfscene");
    std::string err;
    NF_CHECK(rt.load_scene("content://Scenes/Gameplay.nfscene", err));

    // Module -> scene: the component is created because the module has none.
    rt.capture_gameplay_state();

    auto* world = &rt.edit_scene()->world();
    bool found = false;
    for (ecs::Entity e : world->query<GameplayModuleComponent>()) {
        const auto* comp = world->get<GameplayModuleComponent>(e);
        if (comp == nullptr || comp->module_name != "RuntimeStateProbe") continue;
        found = true;
        NF_CHECK_EQ(comp->properties.size(), static_cast<size_t>(3));
        NF_CHECK_EQ(comp->properties.at("gain"), std::string("6.25"));
        NF_CHECK_EQ(comp->properties.at("ticks"), std::string("42"));
        NF_CHECK_EQ(comp->properties.at("active"), std::string("false"));
    }
    NF_CHECK(found);

    // Scene -> module: clobber the live state, then restore from the snapshot.
    probe->settings.gain   = 1.0f;
    probe->settings.ticks  = 0;
    probe->settings.active = true;
    rt.apply_gameplay_state();

    NF_CHECK_NEAR(probe->settings.gain, 6.25f, 1e-5f);
    NF_CHECK_EQ(probe->settings.ticks, 42);
    NF_CHECK(!probe->settings.active);
}

NF_TEST(runtime_scene_unload_fires_before_the_next_scene_loads) {
    RuntimeHarness harness("unload");
    if (!harness.ready()) NF_SKIP("no Vulkan device available");

    Runtime& rt = harness.rt();
    ProbeModule* probe = dynamic_cast<ProbeModule*>(add_probe(rt, "RuntimeUnloadProbe"));
    NF_CHECK(probe != nullptr);

    write_minimal_scene(harness.vfs(), "content://Scenes/First.nfscene");
    write_minimal_scene(harness.vfs(), "content://Scenes/Second.nfscene");

    std::string err;
    NF_CHECK(rt.load_scene("content://Scenes/First.nfscene", err));
    NF_CHECK_EQ(probe->scene_load_calls, 1u);
    NF_CHECK_EQ(probe->scene_unload_calls, 0u);

    // The first scene's modules must hear about the teardown before the world
    // they were pointing at is replaced.
    NF_CHECK(rt.load_scene("content://Scenes/Second.nfscene", err));
    NF_CHECK_EQ(probe->scene_unload_calls, 1u);
    NF_CHECK_EQ(probe->scene_load_calls, 2u);
}

NF_TEST(runtime_shutdown_calls_module_on_shutdown) {
    RuntimeHarness harness("shutdown");
    if (!harness.ready()) NF_SKIP("no Vulkan device available");

    Runtime& rt = harness.rt();
    ProbeModule* probe = dynamic_cast<ProbeModule*>(add_probe(rt, "RuntimeShutdownProbe"));
    NF_CHECK(probe != nullptr);

    write_minimal_scene(harness.vfs(), "content://Scenes/Gameplay.nfscene");
    std::string err;
    NF_CHECK(rt.load_scene("content://Scenes/Gameplay.nfscene", err));

    g_probe_shutdown_calls = 0;
    NF_CHECK_EQ(probe->shutdown_calls, 0u);

    rt.shutdown_gameplay();

    // Asserted through the out-of-band counter, not `probe`: the call above
    // destroyed it.
    NF_CHECK_EQ(g_probe_shutdown_calls, 1u);
    NF_CHECK_EQ(rt.gameplay_module_count(), static_cast<size_t>(0));

    // Idempotent: a second shutdown must not re-enter a destroyed module.
    rt.shutdown_gameplay();
    NF_CHECK_EQ(g_probe_shutdown_calls, 1u);
}

NF_TEST(runtime_gameplay_context_carries_the_scene_world) {
    RuntimeHarness harness("context");
    if (!harness.ready()) NF_SKIP("no Vulkan device available");

    Runtime& rt = harness.rt();

    // A module that records what the context pointed at, so a null world in a
    // loaded scene is caught rather than silently producing a no-op module.
    class ContextProbe final : public GameplayModule {
    public:
        [[nodiscard]] const char* name() const override { return "RuntimeContextProbe"; }
        void on_update(GameplayContext& ctx) override {
            ++updates;
            saw_world   = ctx.world != nullptr;
            saw_scene   = ctx.scene != nullptr;
            last_frame  = ctx.frame;
            last_dt     = ctx.dt;
        }
        u32 updates = 0;
        bool saw_world = false;
        bool saw_scene = false;
        u64 last_frame = 0;
        f32 last_dt = 0.0f;
    };

    auto owned = std::make_unique<ContextProbe>();
    ContextProbe* probe = owned.get();
    rt.add_gameplay_module(std::move(owned));

    write_minimal_scene(harness.vfs(), "content://Scenes/Gameplay.nfscene");
    std::string err;
    NF_CHECK(rt.load_scene("content://Scenes/Gameplay.nfscene", err));

    rt.update(0.5f);
    NF_CHECK_EQ(probe->updates, 1u);
    NF_CHECK(probe->saw_world);
    NF_CHECK(probe->saw_scene);
    NF_CHECK_NEAR(probe->last_dt, 0.5f, 1e-6f);
}
