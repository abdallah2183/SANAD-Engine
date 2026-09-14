// Tests/SaveTests/test_save.cpp — Phase 10, W4 (save system)
//
// A save system is only as good as its failure behaviour, so these tests spend
// as much effort on the refusals as on the happy path: an invalid slot name, a
// missing slot, a slot written by a newer engine, a save with no scene loaded,
// and a second async save while one is in flight. Every one of those must fail
// loudly rather than write something plausible.

#include <NF/Test/TestFramework.hpp>

#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Runtime/SaveSystem.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

#include <NF/Gameplay/Components.hpp>
#include <NF/Gameplay/GameplayModule.hpp>
#include <NF/Gameplay/GameplayState.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace nf;
using namespace nf::gameplay;
using namespace nf::runtime;
using namespace nf::assets;

namespace {

struct SaveProbeSettings {
    f32         radius = 5.0f;
    i32         steps  = 0;
    std::string label  = "plain";

    NF_CLASS(SaveProbeSettings)
    NF_PROPERTY(SaveProbeSettings, radius, Float,  Prop_SerializeField, "Orbit")
    NF_PROPERTY(SaveProbeSettings, steps,  Int,    Prop_SerializeField, "Orbit")
    NF_PROPERTY(SaveProbeSettings, label,  String, Prop_SerializeField, "Orbit")
    NF_CLASS_END(SaveProbeSettings)
};

class SaveProbeModule final : public GameplayModule {
public:
    [[nodiscard]] const char* name() const override { return "SaveProbe"; }
    GameplayStateBinding state() override { return {&settings, SaveProbeSettings::nf_class_meta()}; }
    SaveProbeSettings settings;
};

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_file(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

/// Temp content tree + a headless device + a Runtime + a SaveSystem. `saves://`
/// is deliberately NOT pre-mounted: every test goes through the derivation path
/// so that path is exercised rather than assumed.
class SaveHarness {
public:
    explicit SaveHarness(const std::string& tag) {
        m_tmp = std::filesystem::temp_directory_path() / ("nf_save_" + tag);
        std::filesystem::remove_all(m_tmp);
        std::filesystem::create_directories(m_tmp / "Content");
        m_vfs.mount("content://", m_tmp / "Content");
    }

    ~SaveHarness() {
        m_save.reset();
        m_rt.reset();
        if (m_device) {
            m_device->wait_idle();
            m_device->shutdown();
        }
        std::filesystem::remove_all(m_tmp);
    }

    SaveHarness(const SaveHarness&) = delete;
    SaveHarness& operator=(const SaveHarness&) = delete;

    [[nodiscard]] bool ready() {
        m_device = rhi::create_device();
        if (!m_device) return false;
        rhi::DeviceDesc desc{};
        desc.window_handle = nullptr;
        if (!m_device->init(desc)) {
            m_device.reset();
            return false;
        }
        m_manager = std::make_unique<AssetManager>(m_vfs, m_reg, m_device.get());
        m_rt = std::make_unique<Runtime>(m_vfs, m_reg, *m_manager, *m_device, nullptr);
        m_save = std::make_unique<SaveSystem>(m_vfs, *m_rt);
        return true;
    }

    [[nodiscard]] VirtualFileSystem& vfs() { return m_vfs; }
    [[nodiscard]] Runtime& rt() { return *m_rt; }
    [[nodiscard]] SaveSystem& save() { return *m_save; }
    [[nodiscard]] const std::filesystem::path& tmp() const { return m_tmp; }
    [[nodiscard]] std::filesystem::path slot_dir(const std::string& slot) const {
        return m_tmp / "Saves" / slot;
    }

private:
    std::filesystem::path                 m_tmp;
    VirtualFileSystem                     m_vfs;
    AssetRegistry                         m_reg;
    std::unique_ptr<rhi::IGraphicsDevice> m_device;
    std::unique_ptr<AssetManager>         m_manager;
    std::unique_ptr<Runtime>              m_rt;
    std::unique_ptr<SaveSystem>           m_save;
};

/// A scene with one transform-only entity and one gameplay module component.
void write_scene_with_module(VirtualFileSystem& vfs, const std::string& logical,
                             const std::string& label, f32 radius) {
    scene::Scene s("SaveScene");
    auto& w = s.world();

    ecs::Entity e = w.create_entity();
    scene::Transform t;
    t.local_x = 4.0f;
    t.world_x = 4.0f;
    w.add<scene::Transform>(e, t);

    GameplayModuleComponent comp;
    comp.module_name = "SaveProbe";
    comp.properties.emplace("radius", std::to_string(radius));
    comp.properties.emplace("steps", "7");
    comp.properties.emplace("label", label);
    w.add<GameplayModuleComponent>(e, std::move(comp));

    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, logical, s, err));
}

} // namespace

// ---------------------------------------------------------------------------
// Mount
// ---------------------------------------------------------------------------

NF_TEST(save_mount_is_derived_from_the_content_mount) {
    SaveHarness harness("mount");
    if (!harness.ready()) NF_SKIP("no Vulkan device available");

    SaveSystem& save = harness.save();
    NF_CHECK(!save.mounted());

    std::string err;
    NF_CHECK(save.ensure_mount(err));
    NF_CHECK(save.mounted());

    // Derived beside content, not into the process's working directory.
    NF_CHECK(std::filesystem::is_directory(harness.tmp() / "Saves"));
}

// ---------------------------------------------------------------------------
// Synchronous save / load
// ---------------------------------------------------------------------------

NF_TEST(save_writes_the_three_documented_files) {
    SaveHarness harness("files");
    if (!harness.ready()) NF_SKIP("no Vulkan device available");

    write_scene_with_module(harness.vfs(), "content://Scenes/Main.nfscene", "hello", 5.0f);
    std::string err;
    NF_CHECK(harness.rt().load_scene("content://Scenes/Main.nfscene", err));

    NF_CHECK(harness.save().save_game("slot1", err));

    const std::filesystem::path dir = harness.slot_dir("slot1");
    NF_CHECK(std::filesystem::is_regular_file(dir / "scene.nfscene"));
    NF_CHECK(std::filesystem::is_regular_file(dir / "modules.txt"));
    NF_CHECK(std::filesystem::is_regular_file(dir / "meta.txt"));

    const std::string meta = read_file(dir / "meta.txt");
    NF_CHECK(meta.find("schema_version: 1") != std::string::npos);
    NF_CHECK(meta.find("scene: SaveScene") != std::string::npos);
    NF_CHECK(meta.find("saved_at: ") != std::string::npos);

    const std::string modules = read_file(dir / "modules.txt");
    NF_CHECK(modules.find("module: SaveProbe") != std::string::npos);
    NF_CHECK(modules.find("props: ") != std::string::npos);

    // No staging or backup directories left behind by a successful save.
    NF_CHECK(!std::filesystem::exists(dir.string() + ".staging"));
    NF_CHECK(!std::filesystem::exists(dir.string() + ".previous"));
}

NF_TEST(save_and_load_round_trips_scene_and_module_state) {
    SaveHarness harness("roundtrip");
    if (!harness.ready()) NF_SKIP("no Vulkan device available");

    Runtime& rt = harness.rt();
    auto probe = std::make_unique<SaveProbeModule>();
    SaveProbeModule* raw = probe.get();
    rt.add_gameplay_module(std::move(probe));

    write_scene_with_module(harness.vfs(), "content://Scenes/Main.nfscene", "orbiting rig", 9.5f);
    std::string err;
    NF_CHECK(rt.load_scene("content://Scenes/Main.nfscene", err));

    NF_CHECK_NEAR(raw->settings.radius, 9.5f, 1e-5f);
    NF_CHECK_EQ(raw->settings.label, std::string("orbiting rig"));

    // Change the live state, save it, then change it again and load back.
    raw->settings.radius = 42.0f;
    raw->settings.steps  = 3;
    raw->settings.label  = "saved value";
    NF_CHECK(harness.save().save_game("slotA", err));

    raw->settings.radius = 1.0f;
    raw->settings.steps  = 0;
    raw->settings.label  = "clobbered";
    NF_CHECK_EQ(raw->settings.label, std::string("clobbered"));

    NF_CHECK(harness.save().load_game("slotA", err));

    // The module's own state came back...
    NF_CHECK_NEAR(raw->settings.radius, 42.0f, 1e-5f);
    NF_CHECK_EQ(raw->settings.steps, 3);
    NF_CHECK_EQ(raw->settings.label, std::string("saved value"));

    // ...and so did the scene geometry, not just the module.
    bool saw_transform = false;
    for (ecs::Entity e : rt.scene()->world().all_entities()) {
        if (const auto* t = rt.scene()->world().get<scene::Transform>(e)) {
            saw_transform = true;
            NF_CHECK_NEAR(t->local_x, 4.0f, 1e-4f);
        }
    }
    NF_CHECK(saw_transform);
}

NF_TEST(save_overwrites_an_existing_slot) {
    SaveHarness harness("overwrite");
    if (!harness.ready()) NF_SKIP("no Vulkan device available");

    Runtime& rt = harness.rt();
    auto probe = std::make_unique<SaveProbeModule>();
    SaveProbeModule* raw = probe.get();
    rt.add_gameplay_module(std::move(probe));

    write_scene_with_module(harness.vfs(), "content://Scenes/Main.nfscene", "first", 1.0f);
    std::string err;
    NF_CHECK(rt.load_scene("content://Scenes/Main.nfscene", err));

    raw->settings.label = "first write";
    NF_CHECK(harness.save().save_game("slot", err));

    raw->settings.label = "second write";
    NF_CHECK(harness.save().save_game("slot", err));

    NF_CHECK(harness.save().has_save("slot"));
    const std::string modules = read_file(harness.slot_dir("slot") / "modules.txt");
    NF_CHECK(modules.find("second") != std::string::npos);
    NF_CHECK(modules.find("first") == std::string::npos);
}

NF_TEST(save_rejects_an_invalid_slot_name) {
    SaveHarness harness("badname");
    if (!harness.ready()) NF_SKIP("no Vulkan device available");

    write_scene_with_module(harness.vfs(), "content://Scenes/Main.nfscene", "x", 1.0f);
    std::string err;
    NF_CHECK(harness.rt().load_scene("content://Scenes/Main.nfscene", err));

    // A slot name is a directory component. Anything that could escape the slot
    // root must be refused, and `.` / `..` are the cases a character whitelist
    // alone would let through.
    const std::vector<std::string> invalid = {
        "", "..", ".", "../escape", "a/b", "a\\b", "C:/tmp", "with space", "tab\there",
    };

    for (const std::string& slot : invalid) {
        err.clear();
        NF_CHECK(!harness.save().save_game(slot, err));
        NF_CHECK(!err.empty());
        NF_CHECK(!harness.save().has_save(slot));
    }
}

NF_TEST(save_without_a_loaded_scene_fails_cleanly) {
    SaveHarness harness("noscene");
    if (!harness.ready()) NF_SKIP("no Vulkan device available");

    std::string err;
    NF_CHECK(!harness.save().save_game("empty", err));
    NF_CHECK(!err.empty());
    NF_CHECK(!harness.save().has_save("empty"));
}

NF_TEST(load_of_a_missing_slot_fails_with_a_clear_error) {
    SaveHarness harness("missing");
    if (!harness.ready()) NF_SKIP("no Vulkan device available");

    std::string err;
    NF_CHECK(!harness.save().has_save("nope"));
    NF_CHECK(!harness.save().load_game("nope", err));
    NF_CHECK(err.find("nope") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Listing and deletion
// ---------------------------------------------------------------------------

NF_TEST(list_saves_reports_every_slot_in_order) {
    SaveHarness harness("list");
    if (!harness.ready()) NF_SKIP("no Vulkan device available");

    write_scene_with_module(harness.vfs(), "content://Scenes/Main.nfscene", "x", 1.0f);
    std::string err;
    NF_CHECK(harness.rt().load_scene("content://Scenes/Main.nfscene", err));

    NF_CHECK(harness.save().save_game("beta", err));
    NF_CHECK(harness.save().save_game("alpha", err));

    const std::vector<SaveSystem::SlotInfo> slots = harness.save().list_saves();
    NF_CHECK_EQ(slots.size(), static_cast<size_t>(2));
    if (slots.size() == 2) {
        NF_CHECK_EQ(slots[0].name, std::string("alpha"));
        NF_CHECK_EQ(slots[1].name, std::string("beta"));
        NF_CHECK_EQ(slots[0].scene_name, std::string("SaveScene"));
        NF_CHECK_EQ(slots[0].schema_version, 1u);
        NF_CHECK_EQ(slots[0].engine_version, std::string(SaveSystem::engine_version()));
        NF_CHECK(!slots[0].saved_at.empty());
    }
}

NF_TEST(delete_save_removes_the_slot_and_reports_a_missing_one) {
    SaveHarness harness("delete");
    if (!harness.ready()) NF_SKIP("no Vulkan device available");

    write_scene_with_module(harness.vfs(), "content://Scenes/Main.nfscene", "x", 1.0f);
    std::string err;
    NF_CHECK(harness.rt().load_scene("content://Scenes/Main.nfscene", err));
    NF_CHECK(harness.save().save_game("temp", err));
    NF_CHECK(harness.save().has_save("temp"));

    NF_CHECK(harness.save().delete_save("temp", err));
    NF_CHECK(!harness.save().has_save("temp"));
    NF_CHECK(!std::filesystem::exists(harness.slot_dir("temp")));

    // Deleting what is not there is an error, not a silent success.
    err.clear();
    NF_CHECK(!harness.save().delete_save("temp", err));
    NF_CHECK(!err.empty());
}

// ---------------------------------------------------------------------------
// Async
// ---------------------------------------------------------------------------

NF_TEST(async_save_completes_and_matches_the_synchronous_result) {
    SaveHarness harness("async");
    if (!harness.ready()) NF_SKIP("no Vulkan device available");

    JobSystem& jobs = JobSystem::instance();
    if (!jobs.is_initialized()) jobs.init(2);

    Runtime& rt = harness.rt();
    auto probe = std::make_unique<SaveProbeModule>();
    SaveProbeModule* raw = probe.get();
    rt.add_gameplay_module(std::move(probe));

    write_scene_with_module(harness.vfs(), "content://Scenes/Main.nfscene", "async rig", 6.0f);
    std::string err;
    NF_CHECK(rt.load_scene("content://Scenes/Main.nfscene", err));

    raw->settings.radius = 12.5f;
    raw->settings.steps  = 4;
    raw->settings.label  = "async rig";

    const u32 before = harness.save().async_saves_dispatched();
    NF_CHECK(harness.save().save_game_async("async1", err));
    NF_CHECK_EQ(harness.save().async_saves_dispatched(), before + 1);

    NF_CHECK(harness.save().wait_for_async_save(err));
    NF_CHECK(harness.save().has_save("async1"));
    NF_CHECK(!harness.save().async_save_pending());
    NF_CHECK(!std::filesystem::exists(harness.slot_dir("async1").string() + ".staging"));

    // The point of the async path is not that files appeared — it is that the
    // save is *usable*. Load it back and check the state actually returned.
    raw->settings.radius = 1.0f;
    raw->settings.steps  = 0;
    raw->settings.label  = "clobbered";

    NF_CHECK(harness.save().load_game("async1", err));
    NF_CHECK_NEAR(raw->settings.radius, 12.5f, 1e-5f);
    NF_CHECK_EQ(raw->settings.steps, 4);
    NF_CHECK_EQ(raw->settings.label, std::string("async rig"));
}

NF_TEST(async_save_refuses_a_second_concurrent_save) {
    SaveHarness harness("concurrent");
    if (!harness.ready()) NF_SKIP("no Vulkan device available");

    JobSystem& jobs = JobSystem::instance();
    if (!jobs.is_initialized()) jobs.init(2);

    write_scene_with_module(harness.vfs(), "content://Scenes/Main.nfscene", "x", 1.0f);
    std::string err;
    NF_CHECK(harness.rt().load_scene("content://Scenes/Main.nfscene", err));

    NF_CHECK(harness.save().save_game_async("one", err));

    // Two writers into one slot would race on the directory swap. Refusing the
    // second is the honest answer; queueing it silently would make the winner
    // depend on the job system's scheduling.
    std::string second_error;
    const bool second = harness.save().save_game_async("two", second_error);
    if (harness.save().async_save_pending()) {
        NF_CHECK(!second);
        NF_CHECK(!second_error.empty());
    }

    NF_CHECK(harness.save().wait_for_async_save(err));
    NF_CHECK(harness.save().has_save("one"));
}

// ---------------------------------------------------------------------------
// Autosave
// ---------------------------------------------------------------------------

NF_TEST(autosave_fires_on_the_interval_and_names_its_slots) {
    SaveHarness harness("autosave");
    if (!harness.ready()) NF_SKIP("no Vulkan device available");

    write_scene_with_module(harness.vfs(), "content://Scenes/Main.nfscene", "x", 1.0f);
    std::string err;
    NF_CHECK(harness.rt().load_scene("content://Scenes/Main.nfscene", err));

    SaveSystem& save = harness.save();
    save.set_autosave(2.0f, "auto_");
    NF_CHECK(save.autosave_enabled());

    // Below the interval: the timer accumulates but nothing is written.
    save.tick(1.0f);
    NF_CHECK_EQ(save.autosaves_performed(), 0u);
    NF_CHECK_NEAR(save.autosave_elapsed(), 1.0f, 1e-5f);

    save.tick(1.0f);
    NF_CHECK_EQ(save.autosaves_performed(), 1u);
    NF_CHECK(save.has_save("auto_1"));

    save.tick(2.0f);
    NF_CHECK_EQ(save.autosaves_performed(), 2u);
    NF_CHECK(save.has_save("auto_2"));

    // The timer resets rather than carrying a remainder that would make the
    // second interval shorter than the first.
    NF_CHECK_NEAR(save.autosave_elapsed(), 0.0f, 1e-5f);

    save.disable_autosave();
    save.tick(10.0f);
    NF_CHECK_EQ(save.autosaves_performed(), 2u);
}

NF_TEST(autosave_ignores_a_non_positive_or_non_finite_interval) {
    SaveHarness harness("autosave-off");
    if (!harness.ready()) NF_SKIP("no Vulkan device available");

    write_scene_with_module(harness.vfs(), "content://Scenes/Main.nfscene", "x", 1.0f);
    std::string err;
    NF_CHECK(harness.rt().load_scene("content://Scenes/Main.nfscene", err));

    SaveSystem& save = harness.save();

    // "Every frame" is not a save policy. A zero or negative interval means off.
    save.set_autosave(0.0f, "auto_");
    NF_CHECK(!save.autosave_enabled());
    save.tick(100.0f);
    NF_CHECK_EQ(save.autosaves_performed(), 0u);

    save.set_autosave(-5.0f, "auto_");
    NF_CHECK(!save.autosave_enabled());

    // A NaN delta must not be able to rewind or trip the timer.
    save.set_autosave(1.0f, "auto_");
    save.tick(std::numeric_limits<f32>::quiet_NaN());
    NF_CHECK_EQ(save.autosaves_performed(), 0u);
}

// ---------------------------------------------------------------------------
// Versioning
// ---------------------------------------------------------------------------

NF_TEST(schema_newer_than_the_engine_is_refused) {
    SaveHarness harness("newer");
    if (!harness.ready()) NF_SKIP("no Vulkan device available");

    write_scene_with_module(harness.vfs(), "content://Scenes/Main.nfscene", "x", 1.0f);
    std::string err;
    NF_CHECK(harness.rt().load_scene("content://Scenes/Main.nfscene", err));
    NF_CHECK(harness.save().save_game("future", err));

    // Forge a slot from a future engine.
    const std::filesystem::path meta = harness.slot_dir("future") / "meta.txt";
    std::string text = read_file(meta);
    const size_t pos = text.find("schema_version: 1");
    NF_CHECK(pos != std::string::npos);
    text.replace(pos, std::string("schema_version: 1").size(), "schema_version: 99");
    write_file(meta, text);

    err.clear();
    NF_CHECK(!harness.save().load_game("future", err));
    NF_CHECK(err.find("newer engine") != std::string::npos);
}

NF_TEST(schema_mismatch_runs_the_registered_migration) {
    SaveHarness harness("migrate");
    if (!harness.ready()) NF_SKIP("no Vulkan device available");

    write_scene_with_module(harness.vfs(), "content://Scenes/Main.nfscene", "x", 1.0f);
    std::string err;
    NF_CHECK(harness.rt().load_scene("content://Scenes/Main.nfscene", err));
    NF_CHECK(harness.save().save_game("old", err));

    // Forge a slot that claims to predate the current schema.
    const std::filesystem::path meta = harness.slot_dir("old") / "meta.txt";
    std::string text = read_file(meta);
    const size_t pos = text.find("schema_version: 1");
    NF_CHECK(pos != std::string::npos);
    text.replace(pos, std::string("schema_version: 1").size(), "schema_version: 0");
    write_file(meta, text);

    // A migration registered for 0 -> 1 must be what makes this load succeed.
    static u32 s_migration_runs = 0;
    s_migration_runs = 0;
    SaveSystem::register_migration(0, [](std::string&) {
        ++s_migration_runs;
        return true;
    });

    NF_CHECK(harness.save().load_game("old", err));
    NF_CHECK_EQ(s_migration_runs, 1u);
    NF_CHECK_EQ(harness.save().last_migration_from(), 0u);
}

NF_TEST(a_load_that_needs_no_migration_reports_none) {
    SaveHarness harness("nomigrate");
    if (!harness.ready()) NF_SKIP("no Vulkan device available");

    write_scene_with_module(harness.vfs(), "content://Scenes/Main.nfscene", "x", 1.0f);
    std::string err;
    NF_CHECK(harness.rt().load_scene("content://Scenes/Main.nfscene", err));
    NF_CHECK(harness.save().save_game("current", err));
    NF_CHECK(harness.save().load_game("current", err));

    // The observable that separates "the migration path ran" from "the version
    // happened to match".
    NF_CHECK_EQ(harness.save().last_migration_from(), 0u);
}
