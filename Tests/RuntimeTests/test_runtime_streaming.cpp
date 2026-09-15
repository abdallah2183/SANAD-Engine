// Tests/RuntimeTests/test_runtime_streaming.cpp — Runtime wiring for world streaming.
//
// The policy is stream-tested with fakes and the merge is unit-tested with
// real files (StreamingTests). These tests drive the wired path end to end:
// enable_streaming -> update() loads the chunk into the live world (meshes
// kicked, physics rebuilt) -> moving the volume unloads it again.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/RHITestCommon.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/StreamingVolume.hpp>

#include <filesystem>

using namespace nf;
using namespace nf::test;

namespace {

std::filesystem::path temp_dir_for(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::create_directories(p);
    return p;
}

scene::StreamingVolume volume_at(float x, float y, float z) {
    scene::StreamingVolume v;
    v.center = {x, y, z};
    v.load_radius = 10.0f;
    v.unload_radius = 16.0f;
    v.chunk_size = 32.0f;
    return v;
}

} // namespace

NF_TEST(runtime_streaming_loads_and_unloads_a_chunk) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;
    rhi::reset_validation_error_count();

    assets::VirtualFileSystem vfs;
    const auto tmp = temp_dir_for("nf_rt_streaming");
    std::filesystem::create_directories(tmp / "Content" / "Scenes");
    std::filesystem::create_directories(tmp / "Content" / "Chunks");
    std::filesystem::create_directories(tmp / "Cache");
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");

    // Base scene: one inert entity so live counts stay distinguishable.
    scene::Scene base("StreamBase");
    {
        ecs::Entity e = base.world().create_entity();
        base.world().add<scene::Transform>(e, scene::Transform{});
        base.world().add<scene::NameComponent>(e, scene::NameComponent{"Base"});
    }
    std::string err;
    NF_CHECK(runtime::save_scene_to_vfs(vfs, "content://Scenes/Base.nfscene", base, err));

    // Chunk scene: a root with physics plus one child.
    scene::Scene chunk("Chunk");
    {
        ecs::Entity root = chunk.world().create_entity();
        chunk.world().add<scene::Transform>(root, scene::Transform{});
        chunk.world().get<scene::Transform>(root)->local_x = 4.0f;
        chunk.world().add<scene::NameComponent>(root, scene::NameComponent{"Streamed"});
        physics::RigidBodyComponent rb;
        rb.type = physics::BodyType::Dynamic;
        chunk.world().add<physics::RigidBodyComponent>(root, rb);
        physics::ColliderComponent col;
        chunk.world().add<physics::ColliderComponent>(root, col);
        ecs::Entity kid = chunk.world().create_entity();
        chunk.world().add<scene::Transform>(kid, scene::Transform{});
        scene::set_parent(chunk.world(), kid, root);
    }
    NF_CHECK(runtime::save_scene_to_vfs(vfs, "content://Chunks/chunk_0_0_0.nfscene", chunk, err));

    assets::AssetRegistry reg;
    assets::AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    NF_CHECK(runtime.load_scene("content://Scenes/Base.nfscene", err));
    NF_CHECK_EQ(runtime.scene()->world().alive_entity_count(), 1u);

    runtime.enable_streaming("content://Chunks", volume_at(0, 0, 0));
    NF_CHECK(runtime.streaming_enabled());
    runtime.update(0.016f);

    // The chunk landed: two entities, physics rebuilt around the merged body.
    NF_CHECK_EQ(runtime.streamed_chunk_count(), 1u);
    NF_CHECK_EQ(runtime.streamed_entity_count(), 2u);
    NF_CHECK_EQ(runtime.scene()->world().alive_entity_count(), 3u);
    NF_CHECK(runtime.physics_world() != nullptr);
    NF_CHECK_EQ(runtime.physics_world()->body_count(), 1u);

    // Move the volume far away: the chunk unloads, the base scene is intact,
    // and physics no longer carries the streamed body.
    runtime.set_streaming_volume(volume_at(1000, 0, 0));
    runtime.update(0.016f);
    NF_CHECK_EQ(runtime.streamed_chunk_count(), 0u);
    NF_CHECK_EQ(runtime.streamed_entity_count(), 0u);
    NF_CHECK_EQ(runtime.scene()->world().alive_entity_count(), 1u);
    NF_CHECK_EQ(runtime.physics_world()->body_count(), 0u);

    runtime.disable_streaming();
    NF_CHECK(!runtime.streaming_enabled());
    NF_CHECK(rhi::validation_error_count() == 0);
    device.wait_idle();
    runtime.shutdown();
    manager.clear();
    device.wait_idle();
    std::filesystem::remove_all(tmp);
    rhi::reset_validation_error_count();
}
