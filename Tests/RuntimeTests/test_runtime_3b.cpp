// Tests/RuntimeTests/test_runtime_3b.cpp — Phase 3B Runtime tests (real rendering)

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/RHITestCommon.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/MeshAsset.hpp>
#include <NF/Rendering/StaticMesh.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/RHI/RHI.hpp>

#include <filesystem>

using namespace nf;
using namespace nf::test;
using namespace nf::assets;
using namespace nf::runtime;

static std::filesystem::path temp_dir_for(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::create_directories(p);
    return p;
}

NF_TEST(runtime_scene_loads_mesh_asset) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;
    rhi::reset_validation_error_count();

    VirtualFileSystem vfs;
    auto tmp = temp_dir_for("nf_rt_load_mesh");
    std::filesystem::create_directories(tmp / "Content" / "Scenes");
    std::filesystem::create_directories(tmp / "Cache" / "Meshes");
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");

    // Create a cube mesh asset
    auto cube = rendering::StaticMesh::create_cube(1.0f);
    AssetId mesh_id = AssetId::generate();
    std::string logical = "content://Meshes/cube_test.nfmesh";
    std::string cooked = "cache://Meshes/cube_test.nfmesh";
    auto asset = MeshAsset::from_static_mesh(*cube, mesh_id, logical);
    std::vector<uint8_t> bytes; asset->save_to_bytes(bytes);
    NF_CHECK(vfs.write_bytes(cooked, std::span<const uint8_t>(bytes)).ok);

    AssetRegistry reg;
    AssetMetadata meta; meta.id=mesh_id; meta.type=AssetType::Mesh; meta.logical_path=logical; meta.cooked_path=cooked; meta.fingerprint="test123"; meta.format="nfmesh-v1";
    std::string err; NF_CHECK(reg.add(meta, err));
    NF_CHECK(reg.save(vfs, "content://AssetRegistry.nfreg", err));

    // Create a scene that references this mesh
    scene::Scene scene("TestMeshLoad");
    auto& world = scene.world();
    ecs::Entity e = world.create_entity();
    world.add<scene::Transform>(e, scene::Transform{});
    world.add<MeshComponent>(e, MeshComponent{mesh_id, "default"});
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/TestMesh.nfscene", scene, err));

    AssetManager manager(vfs, reg, &device);
    Runtime runtime(vfs, reg, manager, device, nullptr);
    bool ok = runtime.load_scene("content://Scenes/TestMesh.nfscene", err);
    NF_CHECK(ok);
    // Pump the manager to load the mesh
    auto handle = manager.find(mesh_id);
    if (!handle || handle->state != AssetState::Ready) {
        // Try sync load
        handle = manager.load_mesh_sync(mesh_id);
    }
    NF_CHECK(handle && handle->state == AssetState::Ready);
    NF_CHECK(handle->mesh && handle->mesh->is_uploaded());
    NF_CHECK(handle->mesh->vertex_buffer(0) != nullptr);

    // Ensure no validation errors
    NF_CHECK(rhi::validation_error_count() == 0);

    device.wait_idle();
    runtime.shutdown();
    manager.clear();
    device.wait_idle();
    std::filesystem::remove_all(tmp);
    rhi::reset_validation_error_count();
}

NF_TEST(runtime_offscreen_scene_produces_pixels) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;
    rhi::reset_validation_error_count();

    VirtualFileSystem vfs;
    auto tmp = temp_dir_for("nf_rt_offscreen");
    std::filesystem::create_directories(tmp / "Content" / "Scenes");
    std::filesystem::create_directories(tmp / "Cache" / "Meshes");
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");

    // Create mesh asset (2.0 cube at distance 5 covers ~400px at 64x64, robustly >100)
    auto cube = rendering::StaticMesh::create_cube(2.0f);
    AssetId mesh_id = AssetId::generate();
    std::string logical = "content://Meshes/offscreen.nfmesh";
    std::string cooked = "cache://Meshes/offscreen.nfmesh";
    auto asset = MeshAsset::from_static_mesh(*cube, mesh_id, logical);
    std::vector<uint8_t> bytes; asset->save_to_bytes(bytes);
    vfs.write_bytes(cooked, std::span<const uint8_t>(bytes));
    AssetRegistry reg;
    AssetMetadata meta; meta.id=mesh_id; meta.type=AssetType::Mesh; meta.logical_path=logical; meta.cooked_path=cooked; meta.fingerprint="off123"; meta.format="nfmesh-v1";
    std::string err; reg.add(meta, err);
    reg.save(vfs, "content://AssetRegistry.nfreg", err);

    // Create scene with camera, light, and mesh
    scene::Scene scene("OffscreenTest");
    auto& world = scene.world();
    // Camera
    ecs::Entity cam_e = world.create_entity();
    world.add<scene::Transform>(cam_e, scene::Transform{});
    world.get<scene::Transform>(cam_e)->local_z = 5;
    CameraComponent cam; cam.fov_y=60; cam.aspect=1.0f; cam.is_active=true;
    world.add<CameraComponent>(cam_e, cam);
    // Light
    ecs::Entity light_e = world.create_entity();
    world.add<DirectionalLight>(light_e, DirectionalLight{});
    // Mesh
    ecs::Entity mesh_e = world.create_entity();
    world.add<scene::Transform>(mesh_e, scene::Transform{});
    world.add<MeshComponent>(mesh_e, MeshComponent{mesh_id, "default"});
    save_scene_to_vfs(vfs, "content://Scenes/Offscreen.nfscene", scene, err);

    AssetManager manager(vfs, reg, &device);
    Runtime runtime(vfs, reg, manager, device, nullptr);
    NF_CHECK(runtime.load_scene("content://Scenes/Offscreen.nfscene", err));
    // Ensure mesh is loaded
    auto handle = manager.load_mesh_sync(mesh_id);
    NF_CHECK(handle->state == AssetState::Ready);
    manager.update();
    runtime.update(0.016f);

    // Offscreen target 64x64
    rhi::TextureDesc td{}; td.width=64; td.height=64; td.format=rhi::Format::R8G8B8A8_UNorm; td.usage=rhi::ImageUsage::ColorAtt | rhi::ImageUsage::TransferSrc;
    auto target = device.create_texture(td);
    NF_CHECK(target);
    auto cmd = device.create_command_buffer();
    auto fence = device.create_fence(false);
    cmd->begin();
    runtime.render_offscreen(*target, *cmd);
    cmd->end();
    device.submit(*cmd, rhi::SubmitInfo{.signal_fence=fence.get()});
    NF_CHECK(fence->wait(kGpuTimeoutNs));
    // Readback
    rhi::BufferDesc rb_desc{}; rb_desc.size=64*64*4; rb_desc.usage=rhi::BufferUsage::TransferDst; rb_desc.memory=rhi::MemoryUsage::GPUToCPU;
    auto rb = device.create_buffer(rb_desc);
    auto cmd2 = device.create_command_buffer(); auto fence2=device.create_fence(false);
    cmd2->begin();
    cmd2->copy_texture_to_buffer(*target, *rb, 0,0,64,64,0);
    cmd2->end();
    device.submit(*cmd2, rhi::SubmitInfo{.signal_fence=fence2.get()});
    NF_CHECK(fence2->wait(kGpuTimeoutNs));
    auto* px = static_cast<Pixel*>(rb->map());
    NF_CHECK(px);
    uint32_t lit=0;
    for(int i=0;i<64*64;++i) if (px[i].r>10 || px[i].g>10 || px[i].b>10) ++lit;
    rb->unmap();
    NF_LOG_INFO(LogCategory::Core, "Offscreen lit pixels: {}", lit);
    NF_CHECK(lit > 100); // Should have visible pixels, not all clear
    NF_CHECK(rhi::validation_error_count()==0);

    device.wait_idle();
    runtime.shutdown();
    manager.clear();
    device.wait_idle();
    std::filesystem::remove_all(tmp);
    rhi::reset_validation_error_count();
}

NF_TEST(runtime_camera_changes_output) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;
    rhi::reset_validation_error_count();

    VirtualFileSystem vfs;
    auto tmp = temp_dir_for("nf_rt_camera");
    std::filesystem::create_directories(tmp / "Content" / "Scenes");
    std::filesystem::create_directories(tmp / "Cache" / "Meshes");
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");

    auto cube = rendering::StaticMesh::create_cube(2.0f);
    AssetId mesh_id = AssetId::generate();
    std::string logical="content://Meshes/cam.nfmesh", cooked="cache://Meshes/cam.nfmesh";
    auto asset = MeshAsset::from_static_mesh(*cube, mesh_id, logical);
    std::vector<uint8_t> bytes; asset->save_to_bytes(bytes);
    vfs.write_bytes(cooked, std::span<const uint8_t>(bytes));
    AssetRegistry reg; AssetMetadata meta; meta.id=mesh_id; meta.type=AssetType::Mesh; meta.logical_path=logical; meta.cooked_path=cooked; meta.fingerprint="cam123"; meta.format="nfmesh-v1";
    std::string err; reg.add(meta, err); reg.save(vfs, "content://AssetRegistry.nfreg", err);

    // Scene with camera at (0,0,5) looking at origin
    scene::Scene scene1("CamTest1");
    auto& w1 = scene1.world();
    ecs::Entity cam_e1 = w1.create_entity();
    w1.add<scene::Transform>(cam_e1, scene::Transform{});
    w1.get<scene::Transform>(cam_e1)->local_z=5;
    CameraComponent cam1; cam1.fov_y=60; cam1.is_active=true;
    w1.add<CameraComponent>(cam_e1, cam1);
    ecs::Entity light_e1 = w1.create_entity(); w1.add<DirectionalLight>(light_e1, DirectionalLight{});
    ecs::Entity mesh_e1 = w1.create_entity(); w1.add<scene::Transform>(mesh_e1, scene::Transform{}); w1.add<MeshComponent>(mesh_e1, MeshComponent{mesh_id, ""});
    save_scene_to_vfs(vfs, "content://Scenes/Cam1.nfscene", scene1, err);

    // Scene with camera at (5,0,0) looking at origin (90 degrees different)
    scene::Scene scene2("CamTest2");
    auto& w2 = scene2.world();
    ecs::Entity cam_e2 = w2.create_entity();
    w2.add<scene::Transform>(cam_e2, scene::Transform{});
    w2.get<scene::Transform>(cam_e2)->local_x=5;
    w2.add<CameraComponent>(cam_e2, CameraComponent{60,1.0f,0.1f,100.0f,true});
    ecs::Entity light_e2 = w2.create_entity(); w2.add<DirectionalLight>(light_e2, DirectionalLight{});
    ecs::Entity mesh_e2 = w2.create_entity(); w2.add<scene::Transform>(mesh_e2, scene::Transform{}); w2.add<MeshComponent>(mesh_e2, MeshComponent{mesh_id, ""});
    save_scene_to_vfs(vfs, "content://Scenes/Cam2.nfscene", scene2, err);

    AssetManager manager(vfs, reg, &device);
    auto h = manager.load_mesh_sync(mesh_id); NF_CHECK(h->state==AssetState::Ready);

    auto render_to_pixels = [&](const std::string& scene_path) -> std::vector<Pixel> {
        Runtime rt(vfs, reg, manager, device, nullptr);
        std::string load_err;
        NF_CHECK(rt.load_scene(scene_path, load_err));
        rt.update(0.016f);
        rhi::TextureDesc td{}; td.width=64; td.height=64; td.format=rhi::Format::R8G8B8A8_UNorm; td.usage=rhi::ImageUsage::ColorAtt | rhi::ImageUsage::TransferSrc;
        auto target = device.create_texture(td);
        auto cmd = device.create_command_buffer(); auto fence=device.create_fence(false);
        cmd->begin(); rt.render_offscreen(*target, *cmd); cmd->end();
        device.submit(*cmd, rhi::SubmitInfo{.signal_fence=fence.get()});
        NF_CHECK(fence->wait(kGpuTimeoutNs));
        rhi::BufferDesc rbd{}; rbd.size=64*64*4; rbd.usage=rhi::BufferUsage::TransferDst; rbd.memory=rhi::MemoryUsage::GPUToCPU;
        auto rb = device.create_buffer(rbd);
        auto cmd2=device.create_command_buffer(); auto f2=device.create_fence(false);
        cmd2->begin(); cmd2->copy_texture_to_buffer(*target, *rb, 0,0,64,64,0); cmd2->end();
        device.submit(*cmd2, rhi::SubmitInfo{.signal_fence=f2.get()});
        NF_CHECK(f2->wait(kGpuTimeoutNs));
        auto* px = static_cast<Pixel*>(rb->map());
        std::vector<Pixel> out(px, px+64*64);
        rb->unmap();
        device.wait_idle();
        rt.shutdown();
        return out;
    };

    auto pixels1 = render_to_pixels("content://Scenes/Cam1.nfscene");
    auto pixels2 = render_to_pixels("content://Scenes/Cam2.nfscene");
    // The two renders should be different (camera moved)
    size_t diff=0;
    for(size_t i=0;i<pixels1.size();++i) if (pixels1[i].r!=pixels2[i].r || pixels1[i].g!=pixels2[i].g || pixels1[i].b!=pixels2[i].b) ++diff;
    NF_LOG_INFO(LogCategory::Core, "Camera change diff pixels: {}", diff);
    NF_CHECK(diff > 100); // Should be significantly different
    NF_CHECK(rhi::validation_error_count()==0);

    device.wait_idle();
    manager.clear();
    device.wait_idle();
    std::filesystem::remove_all(tmp);
    rhi::reset_validation_error_count();
}

NF_TEST(runtime_missing_mesh_fails_safely) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;
    rhi::reset_validation_error_count();

    VirtualFileSystem vfs;
    auto tmp = temp_dir_for("nf_rt_missing");
    std::filesystem::create_directories(tmp / "Content" / "Scenes");
    vfs.mount("content://", tmp / "Content");
    AssetRegistry reg;
    AssetManager manager(vfs, reg, &device);

    scene::Scene scene("MissingTest");
    auto& w = scene.world();
    ecs::Entity e = w.create_entity();
    w.add<scene::Transform>(e, scene::Transform{});
    AssetId missing = AssetId::generate(); // not in registry
    w.add<MeshComponent>(e, MeshComponent{missing, ""});
    ecs::Entity cam_e = w.create_entity();
    w.add<scene::Transform>(cam_e, scene::Transform{});
    w.get<scene::Transform>(cam_e)->local_z=5;
    w.add<CameraComponent>(cam_e, CameraComponent{60,1,0.1f,100,true});
    w.add<DirectionalLight>(w.create_entity(), DirectionalLight{});
    std::string err;
    save_scene_to_vfs(vfs, "content://Scenes/Missing.nfscene", scene, err);

    Runtime runtime(vfs, reg, manager, device, nullptr);
    bool ok = runtime.load_scene("content://Scenes/Missing.nfscene", err);
    NF_CHECK(ok); // Should still succeed (partial), with missing_assets warning
    // Try to render offscreen — should not crash, should produce a fallback (clear color)
    rhi::TextureDesc td{}; td.width=32; td.height=32; td.format=rhi::Format::R8G8B8A8_UNorm; td.usage=rhi::ImageUsage::ColorAtt | rhi::ImageUsage::TransferSrc;
    auto target = device.create_texture(td);
    auto cmd = device.create_command_buffer(); auto fence=device.create_fence(false);
    cmd->begin();
    // This should not crash even though the mesh is missing
    runtime.render_offscreen(*target, *cmd);
    cmd->end();
    device.submit(*cmd, rhi::SubmitInfo{.signal_fence=fence.get()});
    NF_CHECK(fence->wait(kGpuTimeoutNs));
    NF_CHECK(rhi::validation_error_count()==0); // No validation error even with missing mesh

    device.wait_idle();
    runtime.shutdown();
    manager.clear();
    device.wait_idle();
    std::filesystem::remove_all(tmp);
    rhi::reset_validation_error_count();
}

NF_TEST(runtime_clean_shutdown_no_vk_leaks) {
    require_gpu();
    rhi::reset_validation_error_count();
    {
        VirtualFileSystem vfs;
        auto tmp = temp_dir_for("nf_rt_clean");
        std::filesystem::create_directories(tmp / "Content" / "Scenes");
        vfs.mount("content://", tmp / "Content");
        scene::Scene scene("CleanTest");
        auto& w = scene.world();
        ecs::Entity e = w.create_entity();
        w.add<scene::Transform>(e, scene::Transform{});
        std::string err;
        save_scene_to_vfs(vfs, "content://Scenes/Clean.nfscene", scene, err);

        auto device = rhi::create_device();
        rhi::DeviceDesc desc{}; desc.window_handle=nullptr; desc.enable_validation=true;
        if (!device->init(desc)) { std::filesystem::remove_all(tmp); return; }
        {
            AssetRegistry reg;
            AssetManager mgr(vfs, reg, device.get());
            Runtime rt(vfs, reg, mgr, *device, nullptr);
            std::string load_err;
            NF_CHECK(rt.load_scene("content://Scenes/Clean.nfscene", load_err));
            rt.update(0.016f);
            // Create a small offscreen target and render one frame.
            // All RHI objects are scoped so they die before shutdown.
            {
                rhi::TextureDesc td{}; td.width=16; td.height=16; td.format=rhi::Format::R8G8B8A8_UNorm; td.usage=rhi::ImageUsage::ColorAtt | rhi::ImageUsage::TransferSrc;
                auto target = device->create_texture(td);
                auto cmd = device->create_command_buffer();
                auto fence = device->create_fence(false);
                NF_CHECK(target && cmd && fence);
                cmd->begin();
                rt.render_offscreen(*target, *cmd);
                cmd->end();
                device->submit(*cmd, rhi::SubmitInfo{.signal_fence=fence.get()});
                NF_CHECK(fence->wait(kGpuTimeoutNs));
                device->wait_idle();
            }
            // Explicit GPU cleanup while the device is alive.
            device->wait_idle();
            rt.shutdown();
            mgr.clear();
            device->wait_idle();
            NF_CHECK(device->alive_objects() == 0);
        }
        device->wait_idle();
        // Check for validation errors and leaked objects
        NF_CHECK(rhi::validation_error_count()==0);
        NF_CHECK(device->alive_objects() == 0);
        device->shutdown();
        NF_CHECK(rhi::validation_error_count()==0);
        std::filesystem::remove_all(tmp);
    }
    rhi::reset_validation_error_count();
}
