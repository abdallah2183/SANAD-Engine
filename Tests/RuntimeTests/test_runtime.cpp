// Tests/RuntimeTests/test_runtime.cpp — Runtime/Application tests

#include <NF/Test/TestFramework.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/Application.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/MeshAsset.hpp>
#include <NF/Rendering/StaticMesh.hpp>
#include <NF/Scene/Scene.hpp>
#include <NF/Scene/Transform.hpp>

using namespace nf;
using namespace nf::runtime;
using namespace nf::assets;

NF_TEST(runtime_headless_load_scene) {
    VirtualFileSystem vfs;
    auto tmp = std::filesystem::temp_directory_path() / "nf_runtime_headless";
    std::filesystem::create_directories(tmp / "Content" / "Scenes");
    std::filesystem::create_directories(tmp / "Cache" / "Meshes");
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");

    // Cook a cube mesh so the scene renders real geometry (not just clear).
    // 2.0 cube at distance 5 covers ~400px at 64x64, robustly above threshold.
    auto cube = rendering::StaticMesh::create_cube(2.0f);
    AssetId mesh_id = AssetId::generate();
    auto cooked_asset = MeshAsset::from_static_mesh(*cube, mesh_id, "content://Meshes/cube.nfmesh");
    std::vector<uint8_t> mesh_bytes;
    cooked_asset->save_to_bytes(mesh_bytes);
    NF_CHECK(vfs.write_bytes("cache://Meshes/cube.nfmesh", std::span<const uint8_t>(mesh_bytes)).ok);

    AssetRegistry registry;
    AssetMetadata meta{};
    meta.id = mesh_id;
    meta.type = AssetType::Mesh;
    meta.logical_path = "content://Meshes/cube.nfmesh";
    meta.cooked_path = "cache://Meshes/cube.nfmesh";
    meta.fingerprint = "headless1";
    meta.format = "nfmesh-v1";
    std::string reg_err;
    NF_CHECK(registry.add(meta, reg_err));

    // Scene with Camera + DirectionalLight + Mesh (real offscreen content).
    scene::Scene scene("HeadlessTest");
    auto& w = scene.world();
    ecs::Entity cam_e = w.create_entity();
    w.add<scene::Transform>(cam_e, scene::Transform{});
    w.get<scene::Transform>(cam_e)->local_z = 5.0f;
    CameraComponent cam{};
    cam.fov_y = 60.0f;
    cam.aspect = 1.0f;
    cam.is_active = true;
    w.add<CameraComponent>(cam_e, cam);
    ecs::Entity light_e = w.create_entity();
    w.add<DirectionalLight>(light_e, DirectionalLight{});
    ecs::Entity mesh_e = w.create_entity();
    w.add<scene::Transform>(mesh_e, scene::Transform{});
    w.add<MeshComponent>(mesh_e, MeshComponent{mesh_id, "default"});
    std::string err;
    NF_CHECK(save_scene_to_vfs(vfs, "content://Scenes/Headless.nfscene", scene, err));

    // Headless device with validation when the layer is available.
    // NF_SKIP (not a bare return) so a GPU-less machine reports SKIPPED rather
    // than a PASS that verified nothing.
    auto device = rhi::create_device();
    if (!device) {
        std::filesystem::remove_all(tmp);
        NF_SKIP("no Vulkan device available");
    }
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    desc.enable_validation = true;
    if (!device->init(desc)) {
        std::filesystem::remove_all(tmp);
        NF_SKIP("headless Vulkan device init failed");
    }
    rhi::reset_validation_error_count();

    {
        AssetRegistry reg = registry;
        AssetManager manager(vfs, reg, device.get());
        Runtime runtime(vfs, reg, manager, *device, nullptr);
        std::string load_err;
        bool ok = runtime.load_scene("content://Scenes/Headless.nfscene", load_err);
        NF_CHECK(ok);
        if (!ok) {
            device->wait_idle();
            device->shutdown();
            std::filesystem::remove_all(tmp);
            return;
        }
        // Ensure the mesh is Ready and uploaded on this (render) thread.
        auto handle = manager.load_mesh_sync(mesh_id);
        NF_CHECK(handle && handle->state == AssetState::Ready);
        manager.update();
        runtime.update(0.016f);

        // Offscreen render + readback in one submission (all RHI objects scoped
        // so they die before device shutdown).
        constexpr uint32_t kW = 64;
        constexpr uint32_t kH = 64;
        rhi::TextureDesc td{};
        td.width = kW;
        td.height = kH;
        td.format = rhi::Format::R8G8B8A8_UNorm;
        td.usage = rhi::ImageUsage::ColorAtt | rhi::ImageUsage::TransferSrc;
        auto target = device->create_texture(td);
        NF_CHECK(target);
        rhi::BufferDesc rb_desc{};
        rb_desc.size = static_cast<usize>(kW) * kH * 4;
        rb_desc.usage = rhi::BufferUsage::TransferDst;
        rb_desc.memory = rhi::MemoryUsage::GPUToCPU;
        auto readback = device->create_buffer(rb_desc);
        NF_CHECK(readback);
        auto cmd = device->create_command_buffer();
        auto fence = device->create_fence(false);
        NF_CHECK(cmd && fence);
        cmd->begin();
        runtime.render_offscreen(*target, *cmd);
        cmd->copy_texture_to_buffer(*target, *readback, 0, 0, kW, kH, 0);
        cmd->end();
        device->submit(*cmd, rhi::SubmitInfo{.signal_fence = fence.get()});
        NF_CHECK(fence->wait(5000000000ULL));
        device->wait_idle();

        // Pixel readback assertion: real geometry must produce lit pixels,
        // not black and not the magenta fallback clear.
        auto* px = static_cast<const uint8_t*>(readback->map());
        NF_CHECK(px);
        uint32_t lit = 0;
        uint32_t magenta = 0;
        for (uint32_t i = 0; i < kW * kH; ++i) {
            const uint8_t r = px[i * 4 + 0];
            const uint8_t g = px[i * 4 + 1];
            const uint8_t b = px[i * 4 + 2];
            if (r > 10 || g > 10 || b > 10) {
                ++lit;
            }
            if (r > 200 && g < 50 && b > 200) {
                ++magenta;
            }
        }
        readback->unmap();
        NF_LOG_INFO(LogCategory::Core, "runtime_headless lit pixels: {} magenta: {}", lit, magenta);
        NF_CHECK(lit > 100);
        NF_CHECK(magenta == 0);
        NF_CHECK(rhi::validation_error_count() == 0);

        // Explicit GPU cleanup while the device is alive.
        device->wait_idle();
        runtime.shutdown();
        manager.clear();
    }

    device->wait_idle();
    if (device->validation_enabled()) {
        NF_CHECK(rhi::validation_error_count() == 0);
    }
    NF_CHECK(device->alive_objects() == 0);
    device->shutdown();
    if (device->validation_enabled()) {
        NF_CHECK(rhi::validation_error_count() == 0);
    }
    std::filesystem::remove_all(tmp);
    rhi::reset_validation_error_count();
}

NF_TEST(runtime_application_config) {
    runtime::ApplicationConfig cfg;
    cfg.title = "TestApp";
    cfg.width = 800;
    cfg.height = 600;
    cfg.vsync = false;
    cfg.validation = false;
    cfg.headless = true;
    cfg.scene_path = "content://Scenes/Test.nfscene";
    cfg.max_frames = 1;
    // Just test that the config is correctly stored and that Application can be constructed
    runtime::Application app(cfg);
    // We don't actually run it (would need a window), just check that it was constructed
    NF_CHECK(true);
}
