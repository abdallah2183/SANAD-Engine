// Tests/AssetTests/test_asset_manager.cpp — AssetManager tests

#include <NF/Test/TestFramework.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Rendering/StaticMesh.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Jobs/JobSystem.hpp>
#include <NF/Test/RHITestCommon.hpp>

using namespace nf;
using namespace nf::assets;
using namespace nf::test;

NF_TEST(asset_loading_cooked_nfmesh) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_asset_mgr_test";
    std::filesystem::create_directories(tmp / "Content" / "Meshes");
    std::filesystem::create_directories(tmp / "Cache" / "Meshes");
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");

    // Create a cube mesh and cook it to a .nfmesh file
    auto cube = rendering::StaticMesh::create_cube(1.0f);
    AssetId id = AssetId::generate();
    std::string logical = "content://Meshes/cube.nfmesh";
    std::string cooked = "cache://Meshes/cube.nfmesh";
    auto asset = MeshAsset::from_static_mesh(*cube, id, logical);
    std::string err;
    // Save via VFS
    auto data_res = std::vector<uint8_t>{};
    asset->save_to_bytes(data_res);
    NF_CHECK(vfs.write_bytes(cooked, std::span<const uint8_t>(data_res)).ok);

    // Create registry entry
    AssetRegistry reg;
    AssetMetadata meta; meta.id=id; meta.type=AssetType::Mesh; meta.logical_path=logical; meta.cooked_path=cooked; meta.fingerprint="test123"; meta.format="nfmesh-v1";
    NF_CHECK(reg.add(meta, err));
    NF_CHECK(reg.save(vfs, "content://AssetRegistry.nfreg", err));

    // Load via AssetManager
    AssetManager mgr(vfs, reg, &device);
    auto handle = mgr.load_mesh_sync(id);
    NF_CHECK(handle && handle->state == AssetState::Ready);
    NF_CHECK(handle->mesh && handle->mesh->vertex_buffer(0) != nullptr);
    NF_CHECK(handle->asset && handle->asset->vertices.size()==24);

    std::filesystem::remove_all(tmp);
}

NF_TEST(asset_cache_identity) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_asset_cache_test";
    std::filesystem::create_directories(tmp / "Content");
    std::filesystem::create_directories(tmp / "Cache");
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");

    auto cube = rendering::StaticMesh::create_cube(1.0f);
    AssetId id = AssetId::generate();
    std::string logical = "content://Meshes/cube2.nfmesh";
    std::string cooked = "cache://Meshes/cube2.nfmesh";
    auto asset = MeshAsset::from_static_mesh(*cube, id, logical);
    std::vector<uint8_t> bytes; asset->save_to_bytes(bytes);
    vfs.write_bytes(cooked, std::span<const uint8_t>(bytes));
    AssetRegistry reg;
    AssetMetadata meta; meta.id=id; meta.type=AssetType::Mesh; meta.logical_path=logical; meta.cooked_path=cooked; meta.fingerprint="abc"; meta.format="nfmesh-v1";
    std::string err; reg.add(meta, err);
    AssetManager mgr(vfs, reg, &device);
    auto h1 = mgr.load_mesh_sync(id);
    auto h2 = mgr.load_mesh_sync(id);
    NF_CHECK(h1 == h2); // same handle (cache identity)
    NF_CHECK(h1->mesh == h2->mesh);

    std::filesystem::remove_all(tmp);
}

NF_TEST(asset_missing_fails_safely) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_asset_missing_test";
    std::filesystem::create_directories(tmp / "Content");
    vfs.mount("content://", tmp / "Content");
    AssetRegistry reg;
    AssetManager mgr(vfs, reg, nullptr); // headless
    AssetId missing = AssetId::generate();
    auto handle = mgr.load_mesh_sync(missing);
    NF_CHECK(handle && handle->state == AssetState::Failed);
    NF_CHECK(!handle->error.empty());
    std::filesystem::remove_all(tmp);
}

NF_TEST(asset_async_cpu_then_gpu_upload) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;
    if (!nf::JobSystem::instance().is_initialized()) nf::JobSystem::instance().init(2);

    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_asset_async_test";
    std::filesystem::create_directories(tmp / "Content" / "Meshes");
    std::filesystem::create_directories(tmp / "Cache" / "Meshes");
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");

    auto cube = rendering::StaticMesh::create_cube(1.0f);
    AssetId id = AssetId::generate();
    std::string logical = "content://Meshes/async.nfmesh";
    std::string cooked = "cache://Meshes/async.nfmesh";
    auto asset = MeshAsset::from_static_mesh(*cube, id, logical);
    std::vector<uint8_t> bytes; asset->save_to_bytes(bytes);
    vfs.write_bytes(cooked, std::span<const uint8_t>(bytes));
    AssetRegistry reg;
    AssetMetadata meta; meta.id=id; meta.type=AssetType::Mesh; meta.logical_path=logical; meta.cooked_path=cooked; meta.fingerprint="async123"; meta.format="nfmesh-v1";
    std::string err; reg.add(meta, err);
    AssetManager mgr(vfs, reg, &device);
    auto handle = mgr.load_mesh(id);
    NF_CHECK(handle && handle->state == AssetState::Loading);
    // Pump until ready (async CPU + GPU)
    for (int i=0;i<50;++i){
        mgr.update();
        if (handle->state == AssetState::Ready || handle->state == AssetState::Failed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    NF_CHECK(handle->state == AssetState::Ready);
    NF_CHECK(handle->mesh && handle->mesh->is_uploaded());

    std::filesystem::remove_all(tmp);
}

NF_TEST(asset_unloading_and_reload_no_leaks) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;

    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_asset_unload_test";
    std::filesystem::create_directories(tmp / "Content");
    std::filesystem::create_directories(tmp / "Cache");
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");

    auto cube = rendering::StaticMesh::create_cube(1.0f);
    AssetId id = AssetId::generate();
    std::string logical = "content://Meshes/unload.nfmesh";
    std::string cooked = "cache://Meshes/unload.nfmesh";
    auto asset = MeshAsset::from_static_mesh(*cube, id, logical);
    std::vector<uint8_t> bytes; asset->save_to_bytes(bytes);
    vfs.write_bytes(cooked, std::span<const uint8_t>(bytes));
    AssetRegistry reg;
    AssetMetadata meta; meta.id=id; meta.type=AssetType::Mesh; meta.logical_path=logical; meta.cooked_path=cooked; meta.fingerprint="unload123"; meta.format="nfmesh-v1";
    std::string err; reg.add(meta, err);
    AssetManager mgr(vfs, reg, &device);
    auto h1 = mgr.load_mesh_sync(id);
    NF_CHECK(h1->state==AssetState::Ready);
    size_t before = mgr.cached_count();
    mgr.unload(id);
    NF_CHECK(mgr.cached_count()==before-1);
    auto h2 = mgr.load_mesh_sync(id);
    NF_CHECK(h2->state==AssetState::Ready);
    NF_CHECK(h1 != h2); // new handle after unload
    NF_CHECK(h2->mesh != nullptr);

    std::filesystem::remove_all(tmp);
}
