// Tests/StreamingTests/test_stream_merge.cpp — scene merge for world streaming.
//
// The WorldStreamer policy (test_streaming.cpp) drives load/unload callbacks
// with fakes. These tests drive the REAL loader half: a chunk .nfscene file
// merged into a live world via merge_scene_into_world, which is what the
// Runtime wires as its load handler. Parsing goes through load_scene_from_vfs,
// so the merge understands exactly what the loader understands.

#include <NF/Test/TestFramework.hpp>
#include <NF/Assets/AssetId.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scene/NameComponent.hpp>
#include <NF/Scene/Transform.hpp>

#include <filesystem>

using namespace nf;

namespace {

std::filesystem::path temp_dir_for(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::create_directories(p);
    return p;
}

// A chunk with a root (name, transform, rigid body, mesh) and one child.
// Entity ids are 0-based in file order, matching what a fresh world assigns,
// so parent(0:0) resolves; parent(4294967295:0) is the invalid sentinel the
// loader itself writes.
const char* kChunkText =
    "# NOVAForge Scene v1\n"
    "version: 1\n"
    "name: Chunk\n"
    "entity_count: 2\n"
    "---\n"
    "entity: 0:0\n"
    "  Name: ChunkRoot\n"
    "  Transform: local(10,0,0) world(10,0,0) rot(0,0,0) scale(1,1,1) parent(4294967295:0)\n"
    "  RigidBody: type=Dynamic mass=2\n"
    "  Collider: shape=Sphere radius=1\n"
    "  Mesh: asset_id=12345678-1234-1234-1234-123456789abc material=content://Materials/Default\n"
    "---\n"
    "entity: 1:0\n"
    "  Name: ChunkChild\n"
    "  Transform: local(1,0,0) world(11,0,0) rot(0,0,0) scale(1,1,1) parent(0:0)\n";

} // namespace

NF_TEST(stream_merge_loads_a_chunk_into_the_live_world) {
    assets::VirtualFileSystem vfs;
    const auto tmp = temp_dir_for("nf_stream_merge");
    std::filesystem::create_directories(tmp / "Content" / "Chunks");
    vfs.mount("content://", tmp / "Content");
    NF_CHECK(vfs.write_text("content://Chunks/chunk_0_0_0.nfscene", kChunkText).ok);

    ecs::World live;
    runtime::SceneMergeResult merged =
        runtime::merge_scene_into_world(vfs, "content://Chunks/chunk_0_0_0.nfscene", live);
    NF_CHECK(merged.success);
    NF_CHECK(merged.error.empty());
    NF_CHECK_EQ(merged.created.size(), 1u);

    // The root arrived with its data; the child was re-homed under the NEW
    // root (not the file's entity 0, which lives in the thrown-away temp).
    NF_CHECK_EQ(live.alive_entity_count(), 2u);
    const ecs::Entity root = merged.created[0];
    NF_CHECK(live.is_alive(root));
    const auto* name = live.get<scene::NameComponent>(root);
    NF_CHECK(name != nullptr && name->name == "ChunkRoot");
    const auto* tr = live.get<scene::Transform>(root);
    NF_CHECK(tr != nullptr);
    NF_CHECK_NEAR(tr->world_x, 10.0f, 1e-5f);
    NF_CHECK(!tr->parent.valid());

    // Physics arrives as data, never as a live session handle.
    const auto* rb = live.get<physics::RigidBodyComponent>(root);
    NF_CHECK(rb != nullptr);
    NF_CHECK(rb->type == physics::BodyType::Dynamic);
    NF_CHECK_NEAR(rb->mass, 2.0f, 1e-5f);
    NF_CHECK(!rb->body.valid());
    const auto* col = live.get<physics::ColliderComponent>(root);
    NF_CHECK(col != nullptr);

    // The mesh reference survives by stable id; resolving it is the asset
    // system's job, not the merge's.
    const auto* mesh = live.get<runtime::MeshComponent>(root);
    NF_CHECK(mesh != nullptr);
    NF_CHECK(mesh->mesh_id == assets::AssetId::from_string("12345678-1234-1234-1234-123456789abc"));
    NF_CHECK(mesh->material == "content://Materials/Default");

    // Exactly one other entity, parented under the new root.
    size_t others = 0;
    for (ecs::Entity e : live.all_entities()) {
        if (e == root) continue;
        ++others;
        const auto* ct = live.get<scene::Transform>(e);
        NF_CHECK(ct != nullptr);
        NF_CHECK(ct->parent == root);
        const auto* cn = live.get<scene::NameComponent>(e);
        NF_CHECK(cn != nullptr && cn->name == "ChunkChild");
    }
    NF_CHECK_EQ(others, 1u);

    std::filesystem::remove_all(tmp);
}

NF_TEST(stream_merge_missing_file_fails_cleanly) {
    assets::VirtualFileSystem vfs;
    const auto tmp = temp_dir_for("nf_stream_merge_missing");
    std::filesystem::create_directories(tmp / "Content" / "Chunks");
    vfs.mount("content://", tmp / "Content");

    ecs::World live;
    runtime::SceneMergeResult merged =
        runtime::merge_scene_into_world(vfs, "content://Chunks/chunk_9_9_9.nfscene", live);
    NF_CHECK(!merged.success);
    NF_CHECK(!merged.error.empty());
    NF_CHECK(merged.created.empty());
    NF_CHECK_EQ(live.alive_entity_count(), 0u);

    std::filesystem::remove_all(tmp);
}

NF_TEST(stream_merge_keeps_multiple_roots_parentless) {
    assets::VirtualFileSystem vfs;
    const auto tmp = temp_dir_for("nf_stream_merge_roots");
    std::filesystem::create_directories(tmp / "Content" / "Chunks");
    vfs.mount("content://", tmp / "Content");
    const char* text =
        "# NOVAForge Scene v1\n"
        "version: 1\n"
        "name: TwoRoots\n"
        "entity_count: 2\n"
        "---\n"
        "entity: 0:0\n"
        "  Name: A\n"
        "  Transform: local(0,0,0) world(0,0,0) rot(0,0,0) scale(1,1,1) parent(4294967295:0)\n"
        "---\n"
        "entity: 1:0\n"
        "  Name: B\n"
        "  Transform: local(5,0,0) world(5,0,0) rot(0,0,0) scale(1,1,1) parent(4294967295:0)\n";
    NF_CHECK(vfs.write_text("content://Chunks/chunk_1_0_0.nfscene", text).ok);

    ecs::World live;
    runtime::SceneMergeResult merged =
        runtime::merge_scene_into_world(vfs, "content://Chunks/chunk_1_0_0.nfscene", live);
    NF_CHECK(merged.success);
    NF_CHECK_EQ(merged.created.size(), 2u);
    for (ecs::Entity r : merged.created) {
        NF_CHECK(live.is_alive(r));
        const auto* t = live.get<scene::Transform>(r);
        NF_CHECK(t != nullptr && !t->parent.valid());
    }

    std::filesystem::remove_all(tmp);
}
