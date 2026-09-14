// Tests/HeadlessProbe.cpp — Renderer3D stage-by-stage diagnostic.
// Renders one frame through the full pipeline and dumps pixel statistics from
// every intermediate target (depth / gbuffer / HDR / tonemapped output) to
// localize where the image goes black.

#include <NF/Core/Logger.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/Renderer3D.hpp>
#include <NF/Rendering/MeshLibrary.hpp>
#include <NF/Runtime/SceneExtraction.hpp>
#include <NF/Rendering/Components.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/Scene/Transform.hpp>

#include <cstdio>
#include <filesystem>
#include <vector>

using namespace nf;
namespace fs = std::filesystem;

#ifndef NF_BASIC3D_SHADER_DIR
    #define NF_BASIC3D_SHADER_DIR ""
#endif

#define STEP(msg) do { std::printf("[probe] %s\n", msg); std::fflush(stdout); } while (0)

int main() {
    Logger::instance().add_sink(Logger::make_console_sink());
    Logger::instance().set_min_level(LogLevel::Warn);

    auto device = rhi::create_device();
    rhi::DeviceDesc desc{};
    desc.window_handle = nullptr;
    desc.enable_validation = true;
    if (!device->init(desc)) { STEP("INIT FAILED"); return 1; }
    STEP("device ok");

    const u32 W = 64, H = 64;
    rendering::Renderer3D renderer;
    if (!renderer.init(*device, fs::path(NF_BASIC3D_SHADER_DIR), W, H)) { STEP("renderer init FAILED"); return 1; }
    STEP("renderer ok");

    rendering::MeshLibrary meshes;
    renderer.set_mesh_library(&meshes);
    auto cube = rendering::StaticMesh::create_cube(1.5f);
    auto h = meshes.add(std::move(cube));
    if (!meshes.upload_all(*device)) { STEP("mesh upload FAILED"); return 1; }
    STEP("mesh ok");

    rendering::PBRMaterialParams params{};
    params.base_color[0] = 0.8f; params.base_color[1] = 0.2f; params.base_color[2] = 0.2f;
    auto mat = renderer.materials().create_instance(*renderer.gbuffer_material(), params, "red");

    ecs::World world;
    auto e = world.create_entity();
    world.add<scene::Transform>(e, scene::Transform{});
    world.add<rendering::MeshComponent>(e, rendering::MeshComponent{h, mat, true});
    scene::propagate_transforms(world);

    rendering::RenderWorld rw;
    nf::runtime::extract_render_objects(world, meshes, rw);
    std::printf("[probe] extracted=%zu\n", rw.size()); std::fflush(stdout);

    rendering::Camera cam{};
    cam.position = {0, 0, 3};
    cam.target = {0, 0, 0};
    cam.fov_y_rad = 60.0f * 3.14159265f / 180.0f;
    cam.aspect = 1.0f;
    update_camera(cam);
    renderer.set_directional_light(rendering::DirectionalLight{rendering::Vec3{0, -1, -0.6f}, rendering::Vec3{1, 1, 1}, 2.0f, true});

    rhi::TextureDesc td{};
    td.width = W; td.height = H;
    td.format = rhi::Format::R8G8B8A8_UNorm;
    td.usage = rhi::ImageUsage::ColorAtt | rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferSrc;
    auto target = device->create_texture(td);
    STEP("target ok");

    auto cmd = device->create_command_buffer();
    auto fence = device->create_fence(false);
    cmd->begin();
    if (!renderer.render(*cmd, rw, cam, *target)) { STEP("render FAILED"); return 1; }
    STEP("render recorded");

    // Dump every intermediate target in the same submission.
    struct Dump { rhi::Texture* tex; const char* label; usize bpp; };
    std::vector<Dump> dumps;
    dumps.push_back({renderer.depth_target(), "depth", 4});
    dumps.push_back({renderer.gbuffer_target(0), "gb0-base", 4});
    dumps.push_back({renderer.gbuffer_target(2), "gb2-surface", 4});
    dumps.push_back({renderer.hdr_target(), "hdr", 8});
    dumps.push_back({target.get(), "output", 4});

    std::vector<std::unique_ptr<rhi::Buffer>> rbs;
    for (auto& d : dumps) {
        rhi::BufferDesc bd{};
        bd.size = usize(W) * H * d.bpp;
        bd.usage = rhi::BufferUsage::TransferDst;
        bd.memory = rhi::MemoryUsage::GPUToCPU;
        auto rb = device->create_buffer(bd);
        if (!rb) { STEP("readback buffer FAILED"); return 1; }
        if (d.bpp == 8) {
            // HDR is 16/16/16/16 float — copy region is W*H*8 bytes.
        }
        cmd->copy_texture_to_buffer(*d.tex, *rb, 0, 0, W, H, 0);
        rbs.push_back(std::move(rb));
    }
    cmd->end();
    device->submit(*cmd, rhi::SubmitInfo{.signal_fence = fence.get()});
    if (!fence->wait()) { STEP("fence FAILED"); return 1; }
    device->wait_idle();
    STEP("submit done");

    for (usize i = 0; i < dumps.size(); ++i) {
        auto* data = static_cast<const u8*>(rbs[i]->map());
        if (!data) { std::printf("[diag] %s: map failed\n", dumps[i].label); std::fflush(stdout); continue; }
        usize center = (usize(32) * W + 32) * dumps[i].bpp;
        usize corner = (usize(2) * W + 2) * dumps[i].bpp;
        u32 lit = 0;
        for (usize p = 0; p < usize(W) * H; ++p) {
            const u8* px = data + p * dumps[i].bpp;
            if (px[0] > 30 || px[1] > 30 || px[2] > 30) ++lit;
        }
        std::printf("[diag] %-12s center=(%u,%u,%u,%u) corner=(%u,%u,%u) lit>30=%u\n",
                    dumps[i].label,
                    data[center], data[center+1], data[center+2], data[center+3],
                    data[corner], data[corner+1], data[corner+2], lit);
        std::fflush(stdout);
        rbs[i]->unmap();
    }

    renderer.shutdown();
    STEP("probe done");
    return 0;
}
