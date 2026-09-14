// Tests/GameplayTests/test_gameplay_serialization.cpp — Phase 10, W5
//
// The scene format is a line-oriented text file where a component is a
// space-separated run of `key=value` tokens. Module state is *user data*, so it
// can contain the separators the format itself uses. These tests exist because
// the obvious implementation (write the value, read up to the next space) passes
// for every well-behaved value and silently truncates the first one that is not
// — a bug that only shows up in someone's save file.

#include <NF/Test/TestFramework.hpp>

#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

#include <NF/Gameplay/Components.hpp>
#include <NF/Gameplay/GameplayModule.hpp>
#include <NF/Gameplay/GameplayState.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace nf;
using namespace nf::gameplay;
using namespace nf::runtime;
using namespace nf::assets;

namespace {

struct SerializationSettings {
    f32         radius = 5.0f;
    i32         steps  = 0;
    std::string label  = "plain";

    NF_CLASS(SerializationSettings)
    NF_PROPERTY(SerializationSettings, radius, Float,  Prop_SerializeField, "Orbit")
    NF_PROPERTY(SerializationSettings, steps,  Int,    Prop_SerializeField, "Orbit")
    NF_PROPERTY(SerializationSettings, label,  String, Prop_SerializeField, "Orbit")
    NF_CLASS_END(SerializationSettings)
};

class SerializationProbeModule final : public GameplayModule {
public:
    [[nodiscard]] const char* name() const override { return "SerializationProbe"; }
    GameplayStateBinding state() override {
        return {&settings, SerializationSettings::nf_class_meta()};
    }
    SerializationSettings settings;
};

/// Temp dir + content:// mount. No device, so these run everywhere.
class ContentHarness {
public:
    explicit ContentHarness(const std::string& tag) {
        m_tmp = std::filesystem::temp_directory_path() / ("nf_gameplay_ser_" + tag);
        std::filesystem::remove_all(m_tmp);
        std::filesystem::create_directories(m_tmp);
        m_vfs.mount("content://", m_tmp);
    }

    ~ContentHarness() { std::filesystem::remove_all(m_tmp); }

    ContentHarness(const ContentHarness&) = delete;
    ContentHarness& operator=(const ContentHarness&) = delete;

    [[nodiscard]] VirtualFileSystem& vfs() { return m_vfs; }
    [[nodiscard]] const std::filesystem::path& tmp() const { return m_tmp; }

private:
    std::filesystem::path m_tmp;
    VirtualFileSystem     m_vfs;
};

/// The raw text of a scene file, for the assertions that need to see the
/// on-disk form rather than the reloaded objects.
std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

const GameplayModuleComponent* find_module(const scene::Scene& s, const std::string& name) {
    for (ecs::Entity e : s.world().query<GameplayModuleComponent>()) {
        const auto* comp = s.world().get<GameplayModuleComponent>(e);
        if (comp != nullptr && comp->module_name == name) return comp;
    }
    return nullptr;
}

} // namespace

NF_TEST(module_component_round_trips_through_a_scene_file) {
    ContentHarness harness("basic");

    {
        scene::Scene out("ModuleScene");
        auto& w = out.world();
        ecs::Entity e = w.create_entity();
        w.add<scene::Transform>(e, scene::Transform{});

        GameplayModuleComponent comp;
        comp.module_name = "SerializationProbe";
        comp.enabled     = false;
        comp.properties.emplace("radius", "12.5");
        comp.properties.emplace("steps", "9");
        comp.properties.emplace("label", "plain");
        w.add<GameplayModuleComponent>(e, std::move(comp));

        std::string err;
        NF_CHECK(save_scene_to_vfs(harness.vfs(), "content://Scenes/Modules.nfscene", out, err));
    }

    const SceneLoadResult result = load_scene_from_vfs(harness.vfs(), "content://Scenes/Modules.nfscene");
    NF_CHECK(result.success);
    NF_CHECK(result.scene != nullptr);

    const auto* comp = find_module(*result.scene, "SerializationProbe");
    NF_CHECK(comp != nullptr);
    if (comp != nullptr) {
        NF_CHECK(!comp->enabled);
        NF_CHECK_EQ(comp->properties.size(), static_cast<size_t>(3));
        NF_CHECK_EQ(comp->properties.at("radius"), std::string("12.5"));
        NF_CHECK_EQ(comp->properties.at("steps"), std::string("9"));
        NF_CHECK_EQ(comp->properties.at("label"), std::string("plain"));
    }
}

NF_TEST(module_property_values_survive_spaces_and_format_separators) {
    ContentHarness harness("escaping");

    // Every value here contains a character the scene format uses as a
    // separator. A naive writer truncates the first two at the space and splits
    // the third into two bogus properties.
    const std::vector<std::string> hostile = {
        "hello world",
        "a|b|c",
        "key=value=more",
        "back\\slash",
        "  leading and trailing  ",
        "tab\there",
    };

    {
        scene::Scene out("EscapeScene");
        auto& w = out.world();
        ecs::Entity e = w.create_entity();
        w.add<scene::Transform>(e, scene::Transform{});

        GameplayModuleComponent comp;
        comp.module_name = "SerializationProbe";
        for (usize i = 0; i < hostile.size(); ++i) {
            comp.properties.emplace("p" + std::to_string(i), hostile[i]);
        }
        w.add<GameplayModuleComponent>(e, std::move(comp));

        std::string err;
        NF_CHECK(save_scene_to_vfs(harness.vfs(), "content://Scenes/Escape.nfscene", out, err));
    }

    const SceneLoadResult result = load_scene_from_vfs(harness.vfs(), "content://Scenes/Escape.nfscene");
    NF_CHECK(result.success);
    NF_CHECK(result.scene != nullptr);

    const auto* comp = find_module(*result.scene, "SerializationProbe");
    NF_CHECK(comp != nullptr);
    if (comp != nullptr) {
        // No property may be invented or lost by the escaping.
        NF_CHECK_EQ(comp->properties.size(), hostile.size());
        for (usize i = 0; i < hostile.size(); ++i) {
            const auto it = comp->properties.find("p" + std::to_string(i));
            NF_CHECK(it != comp->properties.end());
            if (it != comp->properties.end()) {
                NF_CHECK_EQ(it->second, hostile[i]);
            }
        }
    }
}

NF_TEST(module_component_coexists_with_hand_written_components) {
    ContentHarness harness("mixed");

    {
        scene::Scene out("MixedScene");
        auto& w = out.world();

        ecs::Entity e = w.create_entity();
        scene::Transform t;
        t.local_x = 3.0f;
        t.world_x = 3.0f;
        w.add<scene::Transform>(e, t);
        w.add<scene::NameComponent>(e, scene::NameComponent{"Camera rig"});

        GameplayModuleComponent comp;
        comp.module_name = "SerializationProbe";
        comp.properties.emplace("radius", "7.5");
        comp.properties.emplace("steps", "3");
        comp.properties.emplace("label", "orbit rig");
        w.add<GameplayModuleComponent>(e, std::move(comp));

        std::string err;
        NF_CHECK(save_scene_to_vfs(harness.vfs(), "content://Scenes/Mixed.nfscene", out, err));
    }

    const SceneLoadResult result = load_scene_from_vfs(harness.vfs(), "content://Scenes/Mixed.nfscene");
    NF_CHECK(result.success);
    NF_CHECK(result.scene != nullptr);
    if (!result.scene) return;

    // The hand-written components are untouched by the new line's presence.
    bool saw_transform = false;
    bool saw_name = false;
    for (ecs::Entity e : result.scene->world().all_entities()) {
        if (const auto* t = result.scene->world().get<scene::Transform>(e)) {
            saw_transform = true;
            NF_CHECK_NEAR(t->local_x, 3.0f, 1e-4f);
        }
        if (const auto* n = result.scene->world().get<scene::NameComponent>(e)) {
            saw_name = true;
            NF_CHECK_EQ(n->name, std::string("Camera rig"));
        }
    }
    NF_CHECK(saw_transform);
    NF_CHECK(saw_name);

    const auto* comp = find_module(*result.scene, "SerializationProbe");
    NF_CHECK(comp != nullptr);
    if (comp != nullptr) {
        NF_CHECK_EQ(comp->properties.at("label"), std::string("orbit rig"));
    }

    // The module line must not have produced a warning: an unknown-component
    // warning here would mean the loader did not recognise its own output.
    NF_CHECK(result.warnings.empty());
}

NF_TEST(module_line_without_a_name_is_reported_and_not_stored) {
    ContentHarness harness("nameless");

    const std::string text =
        "# NOVAForge Scene v1\n"
        "version: 1\n"
        "name: Nameless\n"
        "entity_count: 1\n"
        "---\n"
        "entity: 0:0\n"
        "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) parent(4294967295:0)\n"
        "  Module: enabled=true props=radius=1\n";

    {
        std::ofstream out(harness.tmp() / "Nameless.nfscene", std::ios::binary | std::ios::trunc);
        out << text;
    }

    const SceneLoadResult result = load_scene_from_vfs(harness.vfs(), "content://Nameless.nfscene");
    NF_CHECK(result.success);
    NF_CHECK(result.scene != nullptr);
    if (!result.scene) return;

    // A module with no name cannot be matched to a factory, so storing it would
    // create a component that can never do anything. It is reported instead.
    NF_CHECK(result.scene->world().query<GameplayModuleComponent>().empty());
    NF_CHECK(!result.warnings.empty());
}

NF_TEST(module_state_reaches_the_live_module_after_a_scene_load) {
    ContentHarness harness("live");

    // The scene carries the state; the live module has none yet.
    {
        scene::Scene out("LiveScene");
        auto& w = out.world();
        ecs::Entity e = w.create_entity();
        w.add<scene::Transform>(e, scene::Transform{});

        GameplayModuleComponent comp;
        comp.module_name = "SerializationProbe";
        comp.properties.emplace("radius", "3.5");
        comp.properties.emplace("steps", "11");
        comp.properties.emplace("label", "from the file");
        w.add<GameplayModuleComponent>(e, std::move(comp));

        std::string err;
        NF_CHECK(save_scene_to_vfs(harness.vfs(), "content://Scenes/Live.nfscene", out, err));
    }

    auto device = rhi::create_device();
    if (!device) NF_SKIP("no Vulkan device available");
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    if (!device->init(desc)) NF_SKIP("headless Vulkan device init failed");

    {
        AssetRegistry reg;
        AssetManager manager(harness.vfs(), reg);
        Runtime rt(harness.vfs(), reg, manager, *device, nullptr);

        auto probe = std::make_unique<SerializationProbeModule>();
        SerializationProbeModule* raw = probe.get();
        rt.add_gameplay_module(std::move(probe));

        NF_CHECK_NEAR(raw->settings.radius, 5.0f, 1e-5f);
        NF_CHECK_EQ(raw->settings.steps, 0);
        NF_CHECK_EQ(raw->settings.label, std::string("plain"));

        std::string err;
        NF_CHECK(rt.load_scene("content://Scenes/Live.nfscene", err));

        // load_scene -> init_gameplay -> apply_gameplay_state, so the file's
        // values are in the live module by the time it returns.
        NF_CHECK_NEAR(raw->settings.radius, 3.5f, 1e-5f);
        NF_CHECK_EQ(raw->settings.steps, 11);
        NF_CHECK_EQ(raw->settings.label, std::string("from the file"));

        // And back out again: capture writes the live state into the component,
        // so a second save carries whatever the session changed.
        raw->settings.radius = 8.0f;
        rt.capture_gameplay_state();

        const auto* comp = find_module(*rt.scene(), "SerializationProbe");
        NF_CHECK(comp != nullptr);
        if (comp != nullptr) {
            NF_CHECK_EQ(comp->properties.at("radius"), std::string("8"));
        }
    }

    device->wait_idle();
    device->shutdown();
}

NF_TEST(module_line_is_written_in_the_documented_shape) {
    ContentHarness harness("shape");

    {
        scene::Scene out("ShapeScene");
        auto& w = out.world();
        ecs::Entity e = w.create_entity();
        w.add<scene::Transform>(e, scene::Transform{});

        GameplayModuleComponent comp;
        comp.module_name = "SerializationProbe";
        comp.enabled = true;
        comp.properties.emplace("label", "two words");
        w.add<GameplayModuleComponent>(e, std::move(comp));

        std::string err;
        NF_CHECK(save_scene_to_vfs(harness.vfs(), "content://Scenes/Shape.nfscene", out, err));
    }

    // The physical path mirrors the logical one: content://Scenes/Shape.nfscene
    // is <mount>/Scenes/Shape.nfscene.
    const std::string text = read_text_file(harness.tmp() / "Scenes" / "Shape.nfscene");
    NF_CHECK(!text.empty());

    // The exact on-disk form, so a future refactor that changes the format has
    // to change this test deliberately rather than by accident.
    NF_CHECK(text.find("  Module: name=SerializationProbe enabled=true props=label=two\\swords") !=
             std::string::npos);
}
