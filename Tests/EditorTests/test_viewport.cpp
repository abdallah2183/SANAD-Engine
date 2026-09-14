// Editor viewport: offscreen target lifecycle (create/resize/dispose) and a
// real Runtime scene render with pixel proof — no swapchain, no present.

#include <NF/Test/TestFramework.hpp>
#include <NF/Test/RHITestCommon.hpp>
#include <NF/Assets/AssetManager.hpp>
#include <NF/Assets/AssetRegistry.hpp>
#include <NF/Assets/MeshAsset.hpp>
#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Editor/Console.hpp>
#include <NF/Editor/EditorApp.hpp>
#include <NF/Editor/Viewport.hpp>
#include <NF/Rendering/StaticMesh.hpp>
#include <NF/Rendering/MeshUpload.hpp>
#include <NF/Runtime/Runtime.hpp>
#include <NF/Runtime/RuntimeSceneLoader.hpp>

#include <filesystem>

using namespace nf;
using namespace nf::test;
using namespace nf::assets;

static std::filesystem::path temp_dir_for(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::create_directories(p);
    return p;
}

static uint32_t render_and_count_lit(rhi::IGraphicsDevice& device, runtime::Runtime& runtime,
                                     editor::ViewportResources& res) {
    auto cmd = device.create_command_buffer();
    auto fence = device.create_fence(false);
    if (!cmd || !fence || !res.target) {
        return 0;
    }
    rhi::BufferDesc rb_desc{};
    rb_desc.size = static_cast<usize>(res.width) * res.height * 4;
    rb_desc.usage = rhi::BufferUsage::TransferDst;
    rb_desc.memory = rhi::MemoryUsage::GPUToCPU;
    auto rb = device.create_buffer(rb_desc);
    if (!rb) {
        return 0;
    }
    cmd->begin();
    runtime.render_offscreen(*res.target, *cmd);
    cmd->copy_texture_to_buffer(*res.target, *rb, 0, 0, res.width, res.height, 0);
    cmd->end();
    device.submit(*cmd, rhi::SubmitInfo{.signal_fence = fence.get()});
    if (!fence->wait(5000000000ULL)) {
        return 0;
    }
    const auto* px = static_cast<const uint8_t*>(rb->map());
    if (px == nullptr) {
        return 0;
    }
    uint32_t lit = 0;
    const size_t n = static_cast<size_t>(res.width) * res.height;
    for (size_t i = 0; i < n; ++i) {
        if (px[i * 4] > 10 || px[i * 4 + 1] > 10 || px[i * 4 + 2] > 10) {
            ++lit;
        }
    }
    rb->unmap();
    device.wait_idle();
    return lit;
}

NF_TEST(editor_viewport_resize_lifecycle) {
    const GpuFixture& f = require_gpu();
    auto& device = *f.device;
    rhi::reset_validation_error_count();

    VirtualFileSystem vfs;
    const auto tmp = temp_dir_for("nf_ed_viewport");
    std::filesystem::create_directories(tmp / "Content" / "Scenes");
    std::filesystem::create_directories(tmp / "Cache" / "Meshes");
    vfs.mount("content://", tmp / "Content");
    vfs.mount("cache://", tmp / "Cache");

    auto cube = rendering::StaticMesh::create_cube(2.0f);
    const AssetId mesh_id = AssetId::generate();
    auto asset = rendering::make_mesh_asset(*cube, mesh_id, "content://Meshes/cube.nfmesh");
    std::vector<uint8_t> bytes;
    asset->save_to_bytes(bytes);
    NF_CHECK(vfs.write_bytes("cache://Meshes/cube.nfmesh", std::span<const uint8_t>(bytes)).ok);
    AssetRegistry reg;
    AssetMetadata meta;
    meta.id = mesh_id;
    meta.type = AssetType::Mesh;
    meta.logical_path = "content://Meshes/cube.nfmesh";
    meta.cooked_path = "cache://Meshes/cube.nfmesh";
    std::string err;
    NF_CHECK(reg.add(meta, err));

    scene::Scene scene("Viewport");
    auto& w = scene.world();
    ecs::Entity cam_e = w.create_entity();
    w.add<scene::Transform>(cam_e, scene::Transform{});
    w.get<scene::Transform>(cam_e)->local_z = 5.0f;
    runtime::CameraComponent cam;
    cam.is_active = true;
    w.add<runtime::CameraComponent>(cam_e, cam);
    ecs::Entity light_e = w.create_entity();
    w.add<runtime::DirectionalLight>(light_e, runtime::DirectionalLight{});
    ecs::Entity mesh_e = w.create_entity();
    w.add<scene::Transform>(mesh_e, scene::Transform{});
    runtime::MeshComponent mc;
    mc.mesh_id = mesh_id;
    w.add<runtime::MeshComponent>(mesh_e, mc);
    NF_CHECK(runtime::save_scene_to_vfs(vfs, "content://Scenes/View.nfscene", scene, err));

    AssetManager manager(vfs, reg);
    runtime::Runtime runtime(vfs, reg, manager, device, nullptr);
    NF_CHECK(runtime.load_scene("content://Scenes/View.nfscene", err));
    auto handle = manager.load_mesh_sync(mesh_id);
    NF_CHECK(handle && handle->state == AssetState::Ready);
    manager.update();
    runtime.update(0.016f);

    // Create at 64x64 and prove real pixels (no swapchain involved).
    editor::ViewportState state{64, 64};
    editor::ViewportResources res;
    NF_CHECK(editor::ensure_viewport_target(device, state, res));
    NF_CHECK(res.valid() && res.width == 64 && res.height == 64);
    // Compare identity, not address. The allocator recycles heap addresses
    // across create/destroy, so a pointer comparison cannot distinguish "kept
    // the same texture" from "destroyed it and got the same address back" —
    // that ambiguity is what made this test fail. creation_serial() is
    // monotonic and never reused.
    const u64 first_serial = res.target->creation_serial();
    NF_CHECK(first_serial != 0);
    // Same size reuses the texture (no churn).
    NF_CHECK(editor::ensure_viewport_target(device, state, res));
    NF_CHECK(res.target->creation_serial() == first_serial);
    NF_CHECK(render_and_count_lit(device, runtime, res) > 100);

    // Resize recreates safely and still renders.
    device.wait_idle();
    state.width = 48;
    state.height = 32;
    NF_CHECK(editor::ensure_viewport_target(device, state, res));
    NF_CHECK(res.valid() && res.width == 48 && res.height == 32);
    NF_CHECK(res.target->creation_serial() != first_serial);
    NF_CHECK(render_and_count_lit(device, runtime, res) > 50);

    // Zero size is rejected without destroying the live target.
    editor::ViewportState bad{0, 0};
    NF_CHECK(!editor::ensure_viewport_target(device, bad, res));
    NF_CHECK(res.valid());

    NF_CHECK(rhi::validation_error_count() == 0);
    device.wait_idle();
    res.reset();
    runtime.shutdown();
    manager.clear();
    device.wait_idle();
    NF_CHECK(rhi::validation_error_count() == 0);
    std::filesystem::remove_all(tmp);
    rhi::reset_validation_error_count();
}
